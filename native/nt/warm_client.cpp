#include "warm_host.h"
#include "warm_wire.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace raven {

namespace warm {

Stamp stamp_of(const std::filesystem::path &path)
{
	Stamp result{path};
	struct stat status{};
	if (::stat(path.c_str(), &status) != 0)
		return result;
	result.exists = true;
	result.device = status.st_dev;
	result.inode = status.st_ino;
	result.size = status.st_size;
	result.modified = int64_t(status.st_mtim.tv_sec) * 1000000000 + status.st_mtim.tv_nsec;
	result.changed = int64_t(status.st_ctim.tv_sec) * 1000000000 + status.st_ctim.tv_nsec;
	return result;
}

bool same(const Stamp &left, const Stamp &right)
{
	return left.exists == right.exists && left.device == right.device && left.inode == right.inode && left.size == right.size &&
		left.modified == right.modified && left.changed == right.changed;
}

static void append_stamp(std::string &text, const Stamp &stamp)
{
	text += std::to_string(stamp.exists) + ':' + std::to_string(stamp.device) + ':' + std::to_string(stamp.inode) + ':' +
		std::to_string(stamp.size) + ':' + std::to_string(stamp.modified) + ':' + std::to_string(stamp.changed) + '\n';
}

bool eligible()
{
	if (const char *warm = std::getenv("RAVEN_WARM"); warm && std::strcmp(warm, "0") == 0)
		return false;
	if (std::getenv("RAVEN_TRACE_FILE") || std::getenv("RAVEN_BACKTRACE"))
		return false;
	DIR *folder = ::opendir("/proc/self/fd");
	if (!folder)
		return false;
	bool only_standard = true;
	int streams = 0;
	while (const dirent *entry = ::readdir(folder)) {
		if (entry->d_name[0] == '.')
			continue;
		const int number = std::atoi(entry->d_name);
		if (number == ::dirfd(folder))
			continue;
		if (number <= 2)
			++streams;
		else
			only_standard = false;
	}
	::closedir(folder);
	return only_standard && streams == 3;
}

std::string identity_of(const std::filesystem::path &host, int argc, char **argv)
{
	std::string text = "raven-warm 1\n";
	append_stamp(text, stamp_of(host));
	text += std::to_string(::getuid()) + '\n';
	std::error_code error;
	text += std::filesystem::current_path(error).string() + '\n';
	for (int index = 0; index < argc; ++index) {
		text += std::string("argument ") + argv[index] + '\n';
		std::error_code kind;
		if (std::filesystem::is_regular_file(argv[index], kind))
			append_stamp(text, stamp_of(argv[index]));
	}
	std::vector<std::string> variables;
	for (char **variable = environ; *variable; ++variable)
		variables.emplace_back(*variable);
	std::sort(variables.begin(), variables.end());
	for (const auto &variable : variables)
		text += "environment " + variable + '\n';
	for (int stream = 0; stream <= 2; ++stream)
		text += ::isatty(stream) ? 't' : 'f';
	return text;
}

sockaddr_un address_of(const std::string &text, socklen_t &length)
{
	uint64_t hash = 0xcbf29ce484222325ULL;
	for (const unsigned char byte : text)
		hash = (hash ^ byte) * 0x100000001b3ULL;
	char name[64];
	const int used = std::snprintf(name, sizeof name, "raven-warm-%u-%016llx", unsigned(::getuid()), (unsigned long long)hash);
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path + 1, name, size_t(used));
	length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + size_t(used));
	return address;
}

bool write_all(int descriptor, const void *data, size_t size)
{
	const auto *bytes = static_cast<const char *>(data);
	while (size) {
		const auto written = ::send(descriptor, bytes, size, MSG_NOSIGNAL);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		bytes += written;
		size -= size_t(written);
	}
	return true;
}

bool read_all(int descriptor, void *data, size_t size)
{
	auto *bytes = static_cast<char *>(data);
	while (size) {
		const auto got = ::recv(descriptor, bytes, size, 0);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return false;
		bytes += got;
		size -= size_t(got);
	}
	return true;
}

}

namespace {

using namespace warm;

std::atomic<pid_t> signalled{0};

void pass_signal(int number)
{
	if (const pid_t target = signalled.load())
		::kill(target, number);
}

}

std::optional<int> run_on_warm_host(const std::string &identity)
{
	const int connection = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (connection < 0)
		return std::nullopt;
	socklen_t length = 0;
	const auto address = address_of(identity, length);
	if (::connect(connection, reinterpret_cast<const sockaddr *>(&address), length) != 0) {
		::close(connection);
		return std::nullopt;
	}
	uint32_t size = uint32_t(identity.size());
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 3)]{};
	iovec part{&size, sizeof size};
	msghdr message{};
	message.msg_iov = &part;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof control;
	cmsghdr *header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int) * 3);
	const int streams[3]{0, 1, 2};
	std::memcpy(CMSG_DATA(header), streams, sizeof streams);
	ssize_t sent;
	do
		sent = ::sendmsg(connection, &message, MSG_NOSIGNAL);
	while (sent < 0 && errno == EINTR);
	if (sent != sizeof size || !write_all(connection, identity.data(), identity.size())) {
		::close(connection);
		return std::nullopt;
	}
	// The start runs in another process; what the terminal or a parent sends this one goes on to it.
	struct sigaction forward{};
	forward.sa_handler = pass_signal;
	sigemptyset(&forward.sa_mask);
	for (const int number : {SIGINT, SIGTERM, SIGHUP, SIGQUIT})
		::sigaction(number, &forward, nullptr);
	bool started = false;
	for (Reply answer{}; read_all(connection, &answer, sizeof answer);) {
		if (answer.type == reply_process) {
			started = true;
			signalled = answer.value;
		} else if (answer.type == reply_refused && !started) {
			::close(connection);
			for (const int number : {SIGINT, SIGTERM, SIGHUP, SIGQUIT})
				std::signal(number, SIG_DFL);
			return std::nullopt;
		} else if (answer.type == reply_status) {
			::close(connection);
			const int status = answer.value;
			if (WIFSIGNALED(status)) {
				std::signal(WTERMSIG(status), SIG_DFL);
				::raise(WTERMSIG(status));
				return 128 + WTERMSIG(status);
			}
			return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
		}
	}
	::close(connection);
	if (!started)
		return std::nullopt;
	std::cerr << "raven: the warm host ended before the start did\n";
	return 1;
}

}
