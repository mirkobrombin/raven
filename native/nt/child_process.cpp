#include "child_process.h"
#include "console.h"

#include "elevation.h"
#include "text.h"

#include <atomic>
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <vector>
#include <cstdlib>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;

namespace raven {

namespace {

std::atomic<int> live{0};
std::mutex known_mutex;

std::map<pid_t, std::shared_ptr<ProcessObject>> &known_children()
{
	// Child IPC and exit status outlive the last guest handle. Construct after the channel registry so cleanup runs before it.
	static std::map<pid_t, std::shared_ptr<ProcessObject>> children;
	return children;
}

}

ProcessObject::~ProcessObject()
{
	if (channel)
		remove_child_channel(channel.get());
	if (exit_file >= 0)
		::close(exit_file);
	if (process_descriptor >= 0)
		::close(process_descriptor);
}

bool describe_process(pid_t pid, int &descriptor, InheritedHandle &record)
{
	if (descriptor < 0)
		descriptor = int(::syscall(SYS_pidfd_open, pid, 0));
	if (descriptor < 0)
		return false;
	::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	record.kind = "process";
	record.descriptor = descriptor;
	record.size = uint64_t(pid);
	return true;
}

bool ProcessObject::describe(InheritedHandle &record) const
{
	return describe_process(pid, process_descriptor, record);
}

SelfProcessObject::~SelfProcessObject()
{
	if (process_descriptor >= 0)
		::close(process_descriptor);
}

bool SelfProcessObject::describe(InheritedHandle &record) const
{
	return describe_process(::getpid(), process_descriptor, record);
}

bool ProcessObject::exited() const
{
	reap();
	return done;
}

uint32_t ProcessObject::exit_code() const
{
	reap();
	return code;
}

bool runtime_process_alive(pid_t pid)
{
	if (pid <= 0)
		return false;
	std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
	std::string text;
	std::getline(stat, text);
	const auto close = text.rfind(')');
	if (close == std::string::npos || close + 2 >= text.size() || text[close + 2] == 'Z' || text[close + 2] == 'X')
		return false;
	char mine[4096], theirs[4096];
	const auto mine_length = ::readlink("/proc/self/exe", mine, sizeof(mine));
	const auto theirs_length = ::readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), theirs, sizeof(theirs));
	return mine_length > 0 && mine_length == theirs_length && std::equal(mine, mine + mine_length, theirs);
}

void ProcessObject::reap() const
{
	if (done)
		return;
	if (foreign) {
		if (!runtime_process_alive(pid)) {
			done = true;
			code = 0;
		}
		return;
	}
	int status = 0;
	pid_t result;
	do result = ::waitpid(pid, &status, WNOHANG); while (result < 0 && errno == EINTR);
	if (result == 0)
		return;
	done = true;
	--live;
	if (result < 0)
		code = 0;
	else if (killed)
		code = requested;
	else if (WIFEXITED(status)) {
		code = uint32_t(WEXITSTATUS(status));
		uint32_t whole = 0;
		if (exit_file >= 0 && ::pread(exit_file, &whole, sizeof whole, 0) == ssize_t(sizeof whole))
			code = whole;
	}
	else {
		code = 0xc0000005U;
		if (std::getenv("RAVEN_TRACE_FAIL"))
			std::fprintf(stderr, "raven: process %d ended with host signal %d\n", int(pid), WTERMSIG(status));
	}
}

void ProcessObject::terminate(uint32_t exit)
{
	if (exited())
		return;
	requested = exit;
	killed = true;
	::kill(pid, SIGKILL);
}

std::shared_ptr<ProcessObject> spawn_child(const ChildLaunch &launch, uint32_t &error)
{
	// The child may write to the terminal too, moving the cursor where this process cannot follow it.
	TerminalConsole::instance().mark_stale();
	char self[4096];
	const auto length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (length <= 0) {
		error = 5;
		return nullptr;
	}
	self[length] = 0;
	// What the parent was started with, such as the window system and the time budget, applies to its children.
	std::vector<std::string> arguments{self, "--run-process", "--execution=native"};
	if (const char *options = std::getenv("RAVEN_CHILD_OPTIONS")) {
		std::istringstream words(options);
		for (std::string word; words >> word;)
			arguments.push_back(word);
	}
	arguments.push_back("--stdio=inherit");
	// A child of an elevated process carries its elevated token.
	if (process_elevated() || child_elevation_requested_now())
		arguments.push_back("--elevated");
	// So does a child of a process in the services' session.
	if (process_noninteractive())
		arguments.push_back("--noninteractive");

	// Everything the child receives is a descriptor of the parent that must appear at a number the child expects: the standard handles
	// at 0 to 2, the rest from 20 up. The parent's own copies are closed once the child exists.
	struct Pass {
		int source, target;
	};
	std::vector<Pass> passes;
	std::vector<int> owned;
	int next_target = 20;
	for (int index = 0; index < 3; ++index)
		passes.push_back({launch.standard[index], index});
	const auto pass = [&](int descriptor) {
		const int target = next_target++;
		passes.push_back({descriptor, target});
		return target;
	};
	const auto release = [&] {
		for (const int descriptor : owned)
			::close(descriptor);
	};
	if (!launch.environment.empty()) {
		// The block travels in an anonymous file the child reads once and closes.
		const int file = ::memfd_create("raven-environment", MFD_CLOEXEC);
		if (file < 0) {
			error = 8;
			return nullptr;
		}
		owned.push_back(file);
		const auto bytes = reinterpret_cast<const char *>(launch.environment.data());
		const size_t size = launch.environment.size() * 2;
		for (size_t done = 0; done < size;) {
			const auto count = ::write(file, bytes + done, size - done);
			if (count <= 0) {
				release();
				error = 8;
				return nullptr;
			}
			done += size_t(count);
		}
		::lseek(file, 0, SEEK_SET);
		arguments.push_back("--environment-fd=" + std::to_string(pass(file)));
	}
	int gate_pair[2] = {-1, -1};
	if (launch.suspended) {
		if (::pipe2(gate_pair, O_CLOEXEC)) {
			release();
			error = 8;
			return nullptr;
		}
		// Only the reading end goes to the child; the writing end stays with the parent.
		owned.push_back(gate_pair[0]);
		arguments.push_back("--suspended-fd=" + std::to_string(pass(gate_pair[0])));
	}
	std::vector<InheritedHandle> records;
	for (auto record : launch.inherited) {
		if (record.descriptor < 0)
			continue;
		record.descriptor = pass(record.descriptor);
		records.push_back(std::move(record));
	}
	if (!records.empty()) {
		const int file = ::memfd_create("raven-handles", MFD_CLOEXEC);
		const auto bytes = serialize_inherited(records);
		if (file < 0 || ::write(file, bytes.data(), bytes.size()) != ssize_t(bytes.size())) {
			if (file >= 0)
				::close(file);
			release();
			if (gate_pair[1] >= 0)
				::close(gate_pair[1]);
			error = 8;
			return nullptr;
		}
		::lseek(file, 0, SEEK_SET);
		owned.push_back(file);
		arguments.push_back("--inherit-fd=" + std::to_string(pass(file)));
	}
	// The runtime's messages in the child go where this runtime writes its own, apart from the program's standard error.
	arguments.push_back("--diagnostics-fd=" + std::to_string(pass(2)));
	// The child writes its exit code into a file both share.
	const int exit_file = ::memfd_create("raven-exit", MFD_CLOEXEC);
	const int exit_kept = exit_file < 0 ? -1 : ::fcntl(exit_file, F_DUPFD_CLOEXEC, 0);
	if (exit_kept >= 0) {
		owned.push_back(exit_file);
		arguments.push_back("--exit-code-fd=" + std::to_string(pass(exit_file)));
	} else if (exit_file >= 0)
		::close(exit_file);
	// Handles that are duplicated into the child later travel over a socket pair; the child keeps one end.
	int channel_pair[2] = {-1, -1};
	if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel_pair) == 0) {
		owned.push_back(channel_pair[1]);
		arguments.push_back("--handle-channel-fd=" + std::to_string(pass(channel_pair[1])));
	}
	arguments.push_back("--command-line=" + to_utf8(launch.command_line));
	arguments.push_back("--directory=" + to_utf8(launch.directory));
	arguments.push_back(launch.image.string());
	std::vector<char *> argv;
	for (auto &argument : arguments)
		argv.push_back(argument.data());
	argv.push_back(nullptr);

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	// Duplicating onto a descriptor that still has to be read as a source would lose it: move every source above all the targets first.
	std::vector<int> moved;
	const auto fail_moves = [&] {
		for (const int descriptor : moved)
			::close(descriptor);
		release();
		if (gate_pair[1] >= 0)
			::close(gate_pair[1]);
		if (channel_pair[0] >= 0)
			::close(channel_pair[0]);
		if (exit_kept >= 0)
			::close(exit_kept);
		posix_spawn_file_actions_destroy(&actions);
		error = 6;
	};
	for (const auto &item : passes) {
		const int copy = ::fcntl(item.source, F_DUPFD_CLOEXEC, next_target + 16);
		if (copy < 0) {
			fail_moves();
			return nullptr;
		}
		moved.push_back(copy);
		posix_spawn_file_actions_adddup2(&actions, copy, item.target);
	}
	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
	sigset_t none, all;
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_setsigmask(&attributes, &none);
	posix_spawnattr_setsigdefault(&attributes, &all);
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	pid_t child = 0;
	const int status = ::posix_spawn(&child, self, &actions, &attributes, argv.data(), environ);
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
	for (const int descriptor : moved)
		::close(descriptor);
	release();
	if (status) {
		if (gate_pair[1] >= 0)
			::close(gate_pair[1]);
		if (channel_pair[0] >= 0)
			::close(channel_pair[0]);
		if (exit_kept >= 0)
			::close(exit_kept);
		error = status == ENOENT ? 2 : status == ENOMEM ? 8 : 5;
		return nullptr;
	}
	if (launch.gate)
		*launch.gate = gate_pair[1];
	++live;
	auto object = std::make_shared<ProcessObject>(child);
	object->exit_file = exit_kept;
	if (channel_pair[0] >= 0) {
		object->channel = std::make_unique<HandleChannel>(channel_pair[0]);
		add_child_channel(object->channel.get());
	}
	object->name = from_utf8(launch.image.filename().string());
	{
		std::lock_guard lock(known_mutex);
		known_children()[child] = object;
	}
	return object;
}

std::shared_ptr<ProcessObject> find_child(pid_t pid)
{
	std::lock_guard lock(known_mutex);
	auto &known = known_children();
	const auto found = known.find(pid);
	return found == known.end() ? nullptr : found->second;
}

std::vector<std::shared_ptr<ProcessObject>> retained_children()
{
	std::vector<std::shared_ptr<ProcessObject>> result;
	std::lock_guard lock(known_mutex);
	for (const auto &[pid, object] : known_children()) result.push_back(object);
	return result;
}

std::vector<std::shared_ptr<ProcessObject>> live_children()
{
	std::vector<std::shared_ptr<ProcessObject>> result;
	std::vector<std::shared_ptr<ProcessObject>> retired;
	{
		std::lock_guard lock(known_mutex);
		auto &known = known_children();
		for (auto item = known.begin(); item != known.end();) {
			if (!item->second->exited()) {
				result.push_back(item->second);
				++item;
			} else {
				retired.push_back(std::move(item->second));
				item = known.erase(item);
			}
		}
	}
	return result;
}

bool children_running()
{
	return live.load() > 0;
}

void wait_suspended_process(int descriptor, NativeObjects &objects)
{
	if (descriptor < 0)
		return;
	::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	for (;;) {
		receive_handles(objects);
		pollfd gate{descriptor, POLLIN, 0};
		const int ready = ::poll(&gate, 1, 10);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready != 0)
			break;
	}
	::close(descriptor);
}

namespace {

std::atomic<int> host_waits{0};

}

HostWait::HostWait() { ++host_waits; }
HostWait::~HostWait() { --host_waits; }

bool host_events_pending()
{
	return live.load() > 0 || host_waits.load() > 0;
}

}
