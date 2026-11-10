#include "confine.h"
#include "sandbox.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/landlock.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace raven {

namespace {

int broker_socket = -1;
bool is_confined = false;

std::mutex &broker_lock()
{
	static std::mutex lock;
	return lock;
}

// Landlock's ruleset attribute as the newest kernels take it; older headers lack the scopes.
struct RulesetAttributes {
	uint64_t handled_access_fs = 0, handled_access_net = 0, scoped = 0;
};
constexpr uint64_t scope_signal = 1ull << 1;
constexpr uint64_t fs_refer = 1ull << 13, fs_truncate = 1ull << 14, fs_ioctl_dev = 1ull << 15;
// The rights that apply to a file, rather than to what a folder holds.
constexpr uint64_t file_rights = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | fs_truncate | fs_ioctl_dev;
constexpr uint64_t read_rights = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
constexpr uint64_t run_rights = read_rights | LANDLOCK_ACCESS_FS_EXECUTE;

enum : char { request_open = 'o', request_spawn = 's', request_address = 'a', request_host = 'h' };

bool send_message(int socket, const std::string &bytes, const std::vector<int> &descriptors)
{
	iovec data{const_cast<char *>(bytes.data()), bytes.size()};
	msghdr message{};
	message.msg_iov = &data;
	message.msg_iovlen = 1;
	std::vector<char> control(CMSG_SPACE(sizeof(int) * descriptors.size()));
	if (!descriptors.empty()) {
		message.msg_control = control.data();
		message.msg_controllen = control.size();
		auto *header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(int) * descriptors.size());
		std::memcpy(CMSG_DATA(header), descriptors.data(), sizeof(int) * descriptors.size());
	}
	ssize_t sent;
	do sent = ::sendmsg(socket, &message, MSG_NOSIGNAL); while (sent < 0 && errno == EINTR);
	return sent == ssize_t(bytes.size());
}

bool receive_message(int socket, std::string &bytes, std::vector<int> &descriptors)
{
	bytes.assign(1 << 16, '\0');
	iovec data{bytes.data(), bytes.size()};
	std::array<char, CMSG_SPACE(sizeof(int) * 64)> control{};
	msghdr message{};
	message.msg_iov = &data;
	message.msg_iovlen = 1;
	message.msg_control = control.data();
	message.msg_controllen = control.size();
	ssize_t received;
	do received = ::recvmsg(socket, &message, MSG_CMSG_CLOEXEC); while (received < 0 && errno == EINTR);
	if (received <= 0)
		return false;
	bytes.resize(size_t(received));
	descriptors.clear();
	for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header))
		if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
			const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			const auto *first = reinterpret_cast<const int *>(CMSG_DATA(header));
			descriptors.insert(descriptors.end(), first, first + count);
		}
	return true;
}

// An answer: a 64-bit value and an errno.
std::string answer(int64_t value, int error)
{
	std::string bytes(12, '\0');
	std::memcpy(bytes.data(), &value, 8);
	std::memcpy(bytes.data() + 8, &error, 4);
	return bytes;
}

// Asks the broker and waits for its answer, with the descriptors it sends back.
int64_t ask(const std::string &request, const std::vector<int> &descriptors, std::vector<int> &returned)
{
	std::lock_guard guard(broker_lock());
	std::string bytes;
	if (broker_socket < 0 || !send_message(broker_socket, request, descriptors) || !receive_message(broker_socket, bytes, returned) || bytes.size() < 12) {
		errno = EACCES;
		return -1;
	}
	int64_t value;
	int error;
	std::memcpy(&value, bytes.data(), 8);
	std::memcpy(&error, bytes.data() + 8, 4);
	errno = error;
	return value;
}

// The broker's checks: the grant the path needs, both before it is opened and for the file it turned out to be.
bool permitted(const std::filesystem::path &host, int flags)
{
	const auto access = sandbox().access(host);
	const bool write = (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC));
	if (write)
		return access == Sandbox::Access::write;
	return access == Sandbox::Access::read || access == Sandbox::Access::write || (access == Sandbox::Access::traverse && (flags & O_DIRECTORY));
}

void open_for(int socket, const std::string &bytes)
{
	int32_t flags;
	uint32_t mode;
	std::memcpy(&flags, bytes.data() + 1, 4);
	std::memcpy(&mode, bytes.data() + 5, 4);
	const std::filesystem::path host(bytes.substr(9));
	flags &= ~O_PATH;
	if (!host.is_absolute() || !permitted(host, flags)) {
		send_message(socket, answer(-1, EACCES), {});
		return;
	}
	const int descriptor = ::open(host.c_str(), flags | O_CLOEXEC | O_NOCTTY, mode_t(mode));
	if (descriptor < 0) {
		send_message(socket, answer(-1, errno), {});
		return;
	}
	// The file opened must be the granted one, whatever a link swapped meanwhile.
	char real[4096];
	const auto length = ::readlink(("/proc/self/fd/" + std::to_string(descriptor)).c_str(), real, sizeof(real) - 1);
	if (length <= 0 || !permitted(std::string(real, size_t(length)), flags)) {
		::close(descriptor);
		send_message(socket, answer(-1, EACCES), {});
		return;
	}
	send_message(socket, answer(0, 0), {descriptor});
	::close(descriptor);
}

void spawn_for(int socket, const std::string &bytes, std::vector<int> &descriptors)
{
	// The arguments, each ended by a zero, then a zero; then the number each descriptor takes in the child.
	std::vector<std::string> arguments;
	size_t at = 1;
	while (at < bytes.size() && bytes[at]) {
		const auto end = bytes.find('\0', at);
		if (end == bytes.npos)
			break;
		arguments.push_back(bytes.substr(at, end - at));
		at = end + 1;
	}
	++at;
	std::vector<int> targets;
	for (; at + 4 <= bytes.size(); at += 4) {
		int32_t target;
		std::memcpy(&target, bytes.data() + at, 4);
		targets.push_back(target);
	}
	char self[4096];
	const auto self_length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
	// Only the runtime itself, and only to run one of its trusted programs.
	if (arguments.size() < 2 || self_length <= 0 || arguments[0] != std::string(self, size_t(self_length)) || !trusted_image(arguments.back()) ||
		targets.size() != descriptors.size()) {
		send_message(socket, answer(-1, EACCES), {});
		return;
	}
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	std::vector<int> moved;
	for (size_t index = 0; index < descriptors.size(); ++index) {
		const int copy = ::fcntl(descriptors[index], F_DUPFD_CLOEXEC, 1024);
		if (copy >= 0) {
			moved.push_back(copy);
			posix_spawn_file_actions_adddup2(&actions, copy, targets[index]);
		}
	}
	posix_spawnattr_t attributes;
	posix_spawnattr_init(&attributes);
	sigset_t none, all;
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_setsigmask(&attributes, &none);
	posix_spawnattr_setsigdefault(&attributes, &all);
	posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	std::vector<char *> argv;
	for (auto &argument : arguments)
		argv.push_back(argument.data());
	argv.push_back(nullptr);
	pid_t child = 0;
	const int status = ::posix_spawn(&child, argv[0], &actions, &attributes, argv.data(), environ);
	posix_spawnattr_destroy(&attributes);
	posix_spawn_file_actions_destroy(&actions);
	for (const int descriptor : moved)
		::close(descriptor);
	if (status) {
		send_message(socket, answer(-1, status), {});
		return;
	}
	const int process = int(::syscall(SYS_pidfd_open, child, 0));
	send_message(socket, answer(child, 0), process >= 0 ? std::vector<int>{process} : std::vector<int>{});
	if (process >= 0)
		::close(process);
}

[[noreturn]] void serve(int socket)
{
	for (;;) {
		pollfd entry{socket, POLLIN, 0};
		const int ready = ::poll(&entry, 1, 1000);
		// The trusted programs it started are its children: none is left a zombie.
		while (::waitpid(-1, nullptr, WNOHANG) > 0) {}
		if (ready <= 0)
			continue;
		std::string bytes;
		std::vector<int> descriptors;
		if (!receive_message(socket, bytes, descriptors))
			::_exit(0);
		if (bytes.size() >= 9 && bytes[0] == request_open)
			open_for(socket, bytes);
		else if (bytes.size() >= 2 && bytes[0] == request_spawn)
			spawn_for(socket, bytes, descriptors);
		else if (bytes.size() >= 2 && bytes[0] == request_host) {
			// The program's name, then each target, each ended by a zero.
			std::vector<std::string> parts;
			for (size_t at = 1; at < bytes.size();) {
				const auto end = bytes.find('\0', at);
				parts.push_back(bytes.substr(at, end == bytes.npos ? bytes.npos : end - at));
				at = end == bytes.npos ? bytes.size() : end + 1;
			}
			const auto name = parts.empty() ? std::string() : parts.front();
			if (!parts.empty())
				parts.erase(parts.begin());
			send_message(socket, answer(sandbox().open_on_host(name, parts) ? 1 : 0, 0), {});
		} else if (bytes.size() >= 2 && bytes[0] == request_address)
			send_message(socket, answer(sandbox().open_address(bytes.substr(1)) ? 1 : 0, 0), {});
		else
			send_message(socket, answer(-1, EINVAL), {});
		for (const int descriptor : descriptors)
			::close(descriptor);
	}
}

void allow(int ruleset, const std::filesystem::path &path, uint64_t rights, uint64_t handled)
{
	if (path.empty())
		return;
	const int folder = ::open(path.c_str(), O_PATH | O_CLOEXEC);
	if (folder < 0)
		return;
	struct stat status{};
	if (::fstat(folder, &status) == 0 && !S_ISDIR(status.st_mode))
		rights &= file_rights;
	landlock_path_beneath_attr rule{};
	rule.allowed_access = rights & handled;
	rule.parent_fd = folder;
	::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
	::close(folder);
}

std::filesystem::path environment_path(const char *name)
{
	const char *value = std::getenv(name);
	return value && *value == '/' ? std::filesystem::path(value) : std::filesystem::path();
}

}

bool trusted_image(const std::filesystem::path &image)
{
	std::error_code error;
	const auto runtime = std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
	const auto file = std::filesystem::canonical(image, error);
	if (error)
		return false;
	const auto name = file.filename().string();
	return (file.parent_path() == runtime || file.parent_path() == runtime / "x86") && (name == "control.exe" || name == "raven-picker.exe");
}

bool confined()
{
	return is_confined;
}

bool confine(const std::filesystem::path &prefix)
{
	if (const char *off = std::getenv("RAVEN_NO_CONFINE"); off && *off == '1')
		return false;
	const long abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 1)
		return false;
	RulesetAttributes attributes;
	attributes.handled_access_fs = (1ull << 13) - 1;
	if (abi >= 2)
		attributes.handled_access_fs |= fs_refer;
	if (abi >= 3)
		attributes.handled_access_fs |= fs_truncate;
	if (abi >= 5)
		attributes.handled_access_fs |= fs_ioctl_dev;
	// A confined program cannot signal a process outside: not the host's, not another program's runtime.
	if (abi >= 6)
		attributes.scoped = scope_signal;
	const size_t size = abi >= 6 ? sizeof(RulesetAttributes) : abi >= 4 ? 16 : 8;
	const int ruleset = int(::syscall(SYS_landlock_create_ruleset, &attributes, size, 0));
	if (ruleset < 0)
		return false;
	const uint64_t all = attributes.handled_access_fs;
	std::error_code error;
	const auto runtime = std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
	// The system: its programs, libraries, settings and devices, and the runtime's own files.
	for (const char *system : {"/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32", "/etc", "/opt", "/nix", "/gnu"})
		allow(ruleset, system, run_rights, all);
	allow(ruleset, runtime, run_rights, all);
	allow(ruleset, environment_path("RAVEN_OPENGL_DIRECTORY"), run_rights, all);
	allow(ruleset, "/run", read_rights, all);
	allow(ruleset, "/var/cache", read_rights, all);
	allow(ruleset, "/sys", read_rights, all);
	allow(ruleset, "/proc", read_rights | LANDLOCK_ACCESS_FS_WRITE_FILE, all);
	allow(ruleset, "/dev", all, all);
	// The places the runtime and the libraries it uses keep their work: the prefix, the session's runtime folder, temporary files.
	allow(ruleset, prefix, all, all);
	for (const char *work : {"XDG_RUNTIME_DIR", "TMPDIR"})
		allow(ruleset, environment_path(work), all, all);
	allow(ruleset, "/tmp", all, all);
	// From the person's home, only what drawing and sound need: font and shader caches, fonts, cursors, the sound server's cookie.
	const auto home = environment_path("HOME");
	if (!home.empty()) {
		const auto cache = environment_path("XDG_CACHE_HOME").empty() ? home / ".cache" : environment_path("XDG_CACHE_HOME");
		for (const char *kept : {"mesa_shader_cache", "mesa_shader_cache_db", "fontconfig"}) {
			std::filesystem::create_directories(cache / kept, error);
			allow(ruleset, cache / kept, all, all);
		}
		for (const char *read : {".local/share/fonts", ".fonts", ".config/fontconfig", ".config/pulse", ".icons", ".local/share/icons", ".Xauthority"})
			allow(ruleset, home / read, read_rights, all);
	}
	allow(ruleset, environment_path("XAUTHORITY"), read_rights, all);
	// The host paths the program was granted when it started.
	for (const auto &grant : sandbox().grants())
		allow(ruleset, grant.host, grant.write ? all : read_rights, all);

	// The broker starts before the confinement and stays outside it.
	int pair[2];
	if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) {
		::close(ruleset);
		return false;
	}
	const pid_t broker = ::fork();
	if (broker == 0) {
		::close(pair[0]);
		::close(ruleset);
		::prctl(PR_SET_PDEATHSIG, SIGKILL);
		serve(pair[1]);
	}
	::close(pair[1]);
	if (broker < 0) {
		::close(pair[0]);
		::close(ruleset);
		return false;
	}
	broker_socket = pair[0];
	if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || ::syscall(SYS_landlock_restrict_self, ruleset, 0)) {
		::close(ruleset);
		return false;
	}
	::close(ruleset);
	is_confined = true;
	// The kernel confines the calling thread and what it starts from now on: a thread already running stays outside.
	if (std::getenv("RAVEN_TRACE_CONFINE")) {
		size_t threads = 0;
		for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task", error))
			threads += entry.is_directory(error) ? 1 : 0;
		std::fprintf(stderr, "raven: confined, %zu thread(s) running\n", threads);
	}
	return true;
}

int brokered_open(const std::filesystem::path &host, int flags, mode_t mode)
{
	std::string request(9, '\0');
	request[0] = request_open;
	const int32_t sent_flags = flags;
	const uint32_t sent_mode = mode;
	std::memcpy(request.data() + 1, &sent_flags, 4);
	std::memcpy(request.data() + 5, &sent_mode, 4);
	request += host.string();
	std::vector<int> returned;
	const auto result = ask(request, {}, returned);
	if (result < 0 || returned.empty()) {
		for (const int descriptor : returned)
			::close(descriptor);
		if (result >= 0)
			errno = EACCES;
		return -1;
	}
	for (size_t index = 1; index < returned.size(); ++index)
		::close(returned[index]);
	return returned[0];
}

std::pair<pid_t, int> brokered_spawn(const std::vector<std::string> &arguments, const std::vector<std::pair<int, int>> &descriptors)
{
	std::string request(1, request_spawn);
	for (const auto &argument : arguments) {
		request += argument;
		request += '\0';
	}
	request += '\0';
	std::vector<int> sources;
	for (const auto &[source, target] : descriptors) {
		const int32_t number = target;
		request.append(reinterpret_cast<const char *>(&number), 4);
		sources.push_back(source);
	}
	std::vector<int> returned;
	const auto pid = ask(request, sources, returned);
	if (pid <= 0) {
		for (const int descriptor : returned)
			::close(descriptor);
		return {-1, -1};
	}
	return {pid_t(pid), returned.empty() ? -1 : returned[0]};
}

bool brokered_host(std::string_view name, const std::vector<std::string> &targets)
{
	std::string request(1, request_host);
	request += name;
	for (const auto &target : targets) {
		request += '\0';
		request += target;
	}
	std::vector<int> returned;
	const bool opened = ask(request, {}, returned) == 1;
	for (const int descriptor : returned)
		::close(descriptor);
	return opened;
}

bool brokered_address(std::string_view address)
{
	std::vector<int> returned;
	const bool opened = ask(std::string(1, request_address) + std::string(address), {}, returned) == 1;
	for (const int descriptor : returned)
		::close(descriptor);
	return opened;
}

}
