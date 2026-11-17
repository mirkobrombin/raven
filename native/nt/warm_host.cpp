#include "warm_host.h"
#include "warm_wire.h"
#include "console.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace raven {

namespace {

using namespace warm;

// The identity of the start this process leaves a host for.
std::string identity;
// This process prepares a start for a host, or is the host: it answers starts on this socket, and the request it serves while it prepares.
bool preparing = false;
int listener_channel = -1, request = -1;
std::vector<Stamp> stamps;
// In a start forked ahead of its request, where the request's standard streams come from.
int ready_channel = -1;
// What waits for the request's standard streams in a start forked ahead.
std::vector<std::function<void()>> when_ready;
// What waits for the process of the start itself.
std::vector<std::function<void()>> deferred;

void run_deferred()
{
	auto work = std::move(deferred);
	deferred.clear();
	for (const auto &step : work)
		step();
}

void reply(int connection, uint32_t type, int32_t value)
{
	const Reply message{type, value};
	write_all(connection, &message, sizeof message);
}

// Takes a start's request: the client is this user's and starts the same thing, and its standard streams come with it.
bool take_request(int connection, int (&streams)[handed_count])
{
	ucred peer{};
	socklen_t size = sizeof peer;
	if (::getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::getuid())
		return false;
	uint32_t length = 0;
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * handed_count)]{};
	iovec part{&length, sizeof length};
	msghdr message{};
	message.msg_iov = &part;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof control;
	ssize_t got;
	do
		got = ::recvmsg(connection, &message, MSG_CMSG_CLOEXEC);
	while (got < 0 && errno == EINTR);
	const cmsghdr *header = CMSG_FIRSTHDR(&message);
	if (got != sizeof length || !header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
		header->cmsg_len != CMSG_LEN(sizeof(int) * handed_count)) {
		if (header && header->cmsg_type == SCM_RIGHTS)
			for (size_t index = 0; index < (header->cmsg_len - CMSG_LEN(0)) / sizeof(int); ++index)
				::close(reinterpret_cast<const int *>(CMSG_DATA(header))[index]);
		return false;
	}
	std::memcpy(streams, CMSG_DATA(header), sizeof(int) * handed_count);
	std::string text(length < (1u << 20) ? length : 0, '\0');
	if (length >= (1u << 20) || !read_all(connection, text.data(), text.size()) || text != identity) {
		for (const int stream : streams)
			::close(stream);
		return false;
	}
	return true;
}

// Sends a request's standard streams to the start that takes it, or takes them there.
bool send_streams(int channel, const int (&streams)[handed_count])
{
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * handed_count)]{};
	char byte = 'S';
	iovec part{&byte, 1};
	msghdr message{};
	message.msg_iov = &part;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof control;
	cmsghdr *header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int) * handed_count);
	std::memcpy(CMSG_DATA(header), streams, sizeof streams);
	ssize_t sent;
	do
		sent = ::sendmsg(channel, &message, MSG_NOSIGNAL);
	while (sent < 0 && errno == EINTR);
	return sent == 1;
}

bool receive_streams(int channel, int (&streams)[handed_count])
{
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * handed_count)]{};
	char byte = 0;
	iovec part{&byte, 1};
	msghdr message{};
	message.msg_iov = &part;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof control;
	ssize_t got;
	do
		got = ::recvmsg(channel, &message, MSG_CMSG_CLOEXEC);
	while (got < 0 && errno == EINTR);
	const cmsghdr *header = CMSG_FIRSTHDR(&message);
	if (got != 1 || !header || header->cmsg_type != SCM_RIGHTS || header->cmsg_len != CMSG_LEN(sizeof(int) * handed_count))
		return false;
	std::memcpy(streams, CMSG_DATA(header), sizeof streams);
	return true;
}

void put_streams(const int (&streams)[handed_count])
{
	for (int stream = 0; stream <= 2; ++stream) {
		::dup2(streams[stream], stream);
		::close(streams[stream]);
	}
	set_console_terminal(streams[3]);
}

void quiet_streams()
{
	const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
	for (int stream = 0; stream <= 2; ++stream)
		::dup2(null, stream);
	::close(null);
}

int waiting_milliseconds()
{
	const char *seconds = std::getenv("RAVEN_WARM_SECONDS");
	const int value = seconds ? std::atoi(seconds) : 10;
	return std::max(value, 1) * 1000;
}

// Accepts the next connection, or ends the process once no start came for a while.
int next_connection(int listener)
{
	pollfd waiting{listener, POLLIN, 0};
	const int ready = ::poll(&waiting, 1, waiting_milliseconds());
	if (ready == 0)
		::_exit(0);
	if (ready < 0)
		return -1;
	return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
}

size_t threads()
{
	size_t count = 0;
	if (DIR *folder = ::opendir("/proc/self/task")) {
		while (const dirent *entry = ::readdir(folder))
			count += entry->d_name[0] != '.';
		::closedir(folder);
	}
	return count;
}

void flush_streams()
{
	std::cout.flush();
	std::cerr.flush();
	std::fflush(nullptr);
}

}

void start_warm_host(int argc, char **argv)
{
	// Only the start the raven command asked a host for leaves one behind: the processes a program starts run the runtime itself.
	const bool asked = std::getenv("RAVEN_WARM_ASKED");
	::unsetenv("RAVEN_WARM_ASKED");
	if (!asked || !eligible())
		return;
	std::error_code error;
	identity = identity_of(std::filesystem::read_symlink("/proc/self/exe", error), argc, argv);
	flush_streams();
	const pid_t first = ::fork();
	if (first < 0)
		return;
	if (first > 0) {
		while (::waitpid(first, nullptr, 0) < 0 && errno == EINTR) {}
		return;
	}
	::setsid();
	if (::fork() != 0)
		::_exit(0);
	quiet_streams();
	const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	socklen_t length = 0;
	const auto address = address_of(identity, length);
	if (listener < 0 || ::bind(listener, reinterpret_cast<const sockaddr *>(&address), length) != 0 || ::listen(listener, 64) != 0)
		::_exit(0);
	for (;;) {
		const int connection = next_connection(listener);
		if (connection < 0)
			continue;
		int streams[handed_count];
		if (!take_request(connection, streams)) {
			::close(connection);
			continue;
		}
		int channel[2];
		if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) != 0) {
			reply(connection, reply_refused, 0);
			::close(connection);
			continue;
		}
		const pid_t preparer = ::fork();
		if (preparer == 0) {
			// This process prepares the start, and may become the host: it serves this request whether it gets there or not.
			::close(listener);
			::close(channel[0]);
			listener_channel = channel[1];
			request = connection;
			preparing = true;
			put_streams(streams);
			reply(connection, reply_process, ::getpid());
			return;
		}
		::close(channel[1]);
		for (const int stream : streams)
			::close(stream);
		// Starts that come while this one is prepared run on their own; a host that is prepared takes the listener and answers them.
		for (;;) {
			pollfd waiting[2]{{channel[0], POLLIN, 0}, {listener, POLLIN, 0}};
			if (::poll(waiting, 2, -1) < 0)
				continue;
			if (waiting[1].revents & POLLIN) {
				const int other = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
				if (other >= 0) {
					reply(other, reply_refused, 0);
					::close(other);
				}
			}
			if (!(waiting[0].revents & (POLLIN | POLLHUP)))
				continue;
			char ready = 0;
			const auto got = ::read(channel[0], &ready, 1);
			if (got < 0 && errno == EINTR)
				continue;
			if (got == 1) {
				alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
				char byte = 'L';
				iovec part{&byte, 1};
				msghdr message{};
				message.msg_iov = &part;
				message.msg_iovlen = 1;
				message.msg_control = control;
				message.msg_controllen = sizeof control;
				cmsghdr *header = CMSG_FIRSTHDR(&message);
				header->cmsg_level = SOL_SOCKET;
				header->cmsg_type = SCM_RIGHTS;
				header->cmsg_len = CMSG_LEN(sizeof(int));
				std::memcpy(CMSG_DATA(header), &listener, sizeof listener);
				::sendmsg(channel[0], &message, MSG_NOSIGNAL);
				::_exit(0);
			}
			// The start ended without a host prepared for it, as one that runs elsewhere than a native 64-bit process does: its status is
			// the request's, and later starts like it run on their own.
			int status = 0;
			while (::waitpid(preparer, &status, 0) < 0 && errno == EINTR) {}
			reply(connection, reply_status, status);
			::_exit(0);
		}
	}
}

void warm_point(const std::function<void()> &detach)
{
	if (!preparing)
		return;
	preparing = false;
	// Only a process of one thread can be forked whole; one that started others goes on as an ordinary start, and the process that
	// waited for it answers its client when it ends.
	if (threads() != 1) {
		::close(listener_channel);
		listener_channel = -1;
		run_deferred();
		return;
	}
	// Take the listener from the process that waited for this one.
	const char ready = 'Z';
	int listener = -1;
	if (::write(listener_channel, &ready, 1) == 1) {
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
		char byte = 0;
		iovec part{&byte, 1};
		msghdr message{};
		message.msg_iov = &part;
		message.msg_iovlen = 1;
		message.msg_control = control;
		message.msg_controllen = sizeof control;
		if (::recvmsg(listener_channel, &message, MSG_CMSG_CLOEXEC) == 1)
			if (const cmsghdr *header = CMSG_FIRSTHDR(&message); header && header->cmsg_type == SCM_RIGHTS)
				std::memcpy(&listener, CMSG_DATA(header), sizeof listener);
	}
	::close(listener_channel);
	listener_channel = -1;
	sigset_t children, previous;
	sigemptyset(&children);
	sigaddset(&children, SIGCHLD);
	::sigprocmask(SIG_BLOCK, &children, &previous);
	const int ended = ::signalfd(-1, &children, SFD_CLOEXEC);
	std::map<pid_t, int> serving;
	// The next start is forked before its request comes: it makes its guest process and waits at warm_ready for the request's standard
	// streams, so a request finds a start already made. It runs while the host waits, with the host's quiet streams.
	pid_t spare = -1;
	int spare_channel = -1;
	bool stale = false;
	// The request being given out, which a start forked meanwhile must not keep.
	int streams[handed_count]{-1, -1, -1, -1}, pending = -1;
	const auto drop_spare = [&] {
		if (spare_channel >= 0)
			::close(spare_channel);
		spare_channel = -1;
		spare = -1;
	};
	// Forks the next start; true in it, which goes on with the start.
	const auto prepare_spare = [&] {
		int pair[2];
		if (stale || ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
			return false;
		flush_streams();
		const pid_t child = ::fork();
		if (child == 0) {
			if (listener >= 0)
				::close(listener);
			::close(ended);
			for (const auto &[process, other] : serving)
				::close(other);
			for (const int stream : streams)
				if (stream >= 0)
					::close(stream);
			if (pending >= 0)
				::close(pending);
			::close(pair[0]);
			ready_channel = pair[1];
			::sigprocmask(SIG_SETMASK, &previous, nullptr);
			detach();
			run_deferred();
			return true;
		}
		::close(pair[1]);
		if (child < 0) {
			::close(pair[0]);
			return false;
		}
		spare = child;
		spare_channel = pair[0];
		return false;
	};
	// Gives a request to the start forked for it, with its standard streams.
	const auto dispatch = [&](int connection) {
		const bool sent = spare >= 0 && send_streams(spare_channel, streams);
		for (int &stream : streams) {
			::close(stream);
			stream = -1;
		}
		pending = -1;
		if (!sent) {
			drop_spare();
			reply(connection, reply_refused, 0);
			::close(connection);
			return;
		}
		serving[spare] = connection;
		reply(connection, reply_process, spare);
		drop_spare();
	};
	// The request this process was prepared for goes to the first start; the host itself keeps quiet streams.
	for (int stream = 0; stream <= 2; ++stream)
		streams[stream] = ::fcntl(stream, F_DUPFD_CLOEXEC, 3);
	// The console the request was started from goes with it; the host itself keeps none.
	streams[3] = take_console_terminal();
	if (streams[3] < 0)
		streams[3] = ::open("/dev/null", O_RDWR | O_CLOEXEC);
	quiet_streams();
	pending = request;
	request = -1;
	if (prepare_spare())
		return;
	dispatch(pending);
	if (listener < 0 || ended < 0)
		::_exit(0);
	if (prepare_spare())
		return;
	for (;;) {
		pollfd waiting[2]{{ended, POLLIN, 0}, {listener, stale ? short(0) : short(POLLIN), 0}};
		const int ready_count = ::poll(waiting, 2, serving.empty() ? waiting_milliseconds() : -1);
		if (ready_count == 0 || (stale && serving.empty()))
			::_exit(0);
		if (ready_count < 0)
			continue;
		if (waiting[0].revents & POLLIN) {
			signalfd_siginfo information{};
			[[maybe_unused]] const auto got = ::read(ended, &information, sizeof information);
			int status = 0;
			for (pid_t process; (process = ::waitpid(-1, &status, WNOHANG)) > 0;) {
				if (const auto found = serving.find(process); found != serving.end()) {
					reply(found->second, reply_status, status);
					::close(found->second);
					serving.erase(found);
				} else if (process == spare)
					// A start that ended before its request: the next request has one forked for it.
					drop_spare();
			}
			if (stale && serving.empty())
				::_exit(0);
		}
		if (!(waiting[1].revents & POLLIN))
			continue;
		const int connection = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
		if (connection < 0)
			continue;
		if (!take_request(connection, streams)) {
			::close(connection);
			continue;
		}
		pending = connection;
		// A library or folder the host read changed: the start runs on its own, and the host ends once its starts did.
		if (std::any_of(stamps.begin(), stamps.end(), [](const Stamp &stamp) { return !same(stamp, stamp_of(stamp.path)); })) {
			for (int &stream : streams) {
				::close(stream);
				stream = -1;
			}
			pending = -1;
			reply(connection, reply_refused, 0);
			::close(connection);
			stale = true;
			drop_spare();
			::close(listener);
			listener = -1;
			if (serving.empty())
				::_exit(0);
			continue;
		}
		if (spare < 0 && prepare_spare())
			return;
		dispatch(connection);
		if (prepare_spare())
			return;
	}
}

bool warm_ready()
{
	if (ready_channel < 0)
		return false;
	int streams[handed_count];
	const bool taken = receive_streams(ready_channel, streams);
	::close(ready_channel);
	ready_channel = -1;
	// The host ended without a request for this start: nothing ran in it yet.
	if (!taken)
		::_exit(0);
	put_streams(streams);
	auto work = std::move(when_ready);
	when_ready.clear();
	for (const auto &step : work)
		step();
	return true;
}

bool warm_waiting()
{
	return ready_channel >= 0;
}

void warm_when_ready(std::function<void()> work)
{
	if (ready_channel >= 0)
		when_ready.push_back(std::move(work));
	else
		work();
}

void warm_defer(std::function<void()> work)
{
	if (preparing)
		deferred.push_back(std::move(work));
	else
		work();
}

void warm_settle()
{
	if (!preparing)
		return;
	preparing = false;
	::close(listener_channel);
	listener_channel = -1;
	run_deferred();
}

void warm_watch(const std::filesystem::path &path)
{
	if (preparing && std::none_of(stamps.begin(), stamps.end(), [&](const Stamp &stamp) { return stamp.path == path; }))
		stamps.push_back(stamp_of(path));
}

}
