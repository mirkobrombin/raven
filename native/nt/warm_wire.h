#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

// What the raven launcher and a warm host share: the identity of a start, the socket it is asked on, and the messages between them.
namespace raven::warm {

// What the host tells a client: the process its signals go to, the start's wait status, or that it must run the start itself.
enum : uint32_t { reply_process = 'P', reply_status = 'S', reply_refused = 'R' };

// The descriptors a request hands over: the standard input, output and error, and the terminal the client was started from (its console),
// or /dev/null when it has none.
constexpr int handed_count = 4;

struct Reply {
	uint32_t type;
	int32_t value;
};

// What a file or folder was: a start finds a host stale once one it read is no longer what it was.
struct Stamp {
	std::filesystem::path path;
	bool exists = false;
	dev_t device = 0;
	ino_t inode = 0;
	off_t size = 0;
	int64_t modified = 0, changed = 0;
};

Stamp stamp_of(const std::filesystem::path &path);
bool same(const Stamp &left, const Stamp &right);
// Only a start with nothing open but its three standard streams can be served elsewhere: they are all the client passes.
bool eligible();
// The identity of a start: the runtime binary, the user, the directory, the arguments with the files they name, the environment
// and which standard streams are terminals. Two starts with the same identity prepare the same host.
std::string identity_of(const std::filesystem::path &host, int argc, char **argv);
// The abstract socket of an identity: abstract names leave no file behind, and the host's ends with it.
sockaddr_un address_of(const std::string &identity, socklen_t &length);
bool write_all(int descriptor, const void *data, size_t size);
bool read_all(int descriptor, void *data, size_t size);

}
