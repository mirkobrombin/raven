#pragma once

#include "host_names.h"
#include "shared_file.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>

namespace raven {

// The host socket a name shared by the processes of a prefix stands for: a file in the prefix's objects folder, or in a folder of the
// person's under /tmp when that path is too long for a socket. Abstract socket names would reach across the confinement, which closes them
// where the kernel can.
inline socklen_t local_socket_address(const std::string &name, sockaddr_un &storage)
{
	const auto file = hex_text(name_hash(name));
	auto folder = shared_objects_directory();
	if (folder.string().size() + file.size() + 2 >= sizeof(storage.sun_path))
		folder = std::filesystem::path("/tmp") / ("raven-" + std::to_string(::getuid())) / prefix_tag();
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	::chmod(folder.c_str(), 0700);
	const auto path = (folder / file).string();
	std::memset(&storage, 0, sizeof(storage));
	storage.sun_family = AF_UNIX;
	std::memcpy(storage.sun_path, path.data(), std::min(path.size(), sizeof(storage.sun_path) - 1));
	return socklen_t(offsetof(sockaddr_un, sun_path) + path.size() + 1);
}

// The host socket files live sockets are bound to: the kernel lists every bound socket, with its path last, in /proc/net/unix.
inline std::unordered_set<std::string> bound_local_sockets()
{
	std::unordered_set<std::string> paths;
	std::ifstream list("/proc/net/unix");
	std::string line;
	while (std::getline(list, line))
		if (const auto space = line.rfind(' '); space != std::string::npos)
			paths.insert(line.substr(space + 1));
	return paths;
}

// Whether a live socket is bound to a host socket file.
inline bool local_socket_bound(const std::string &path)
{
	return bound_local_sockets().contains(path);
}

// Binds a socket to a shared name. A socket file no live socket is bound to any more, left by a process that ended, is replaced.
inline int bind_local_socket(int descriptor, const std::string &name)
{
	sockaddr_un storage;
	const auto length = local_socket_address(name, storage);
	if (!::bind(descriptor, reinterpret_cast<const sockaddr *>(&storage), length))
		return 0;
	if (errno != EADDRINUSE)
		return -1;
	if (local_socket_bound(storage.sun_path)) {
		errno = EADDRINUSE;
		return -1;
	}
	::unlink(storage.sun_path);
	return ::bind(descriptor, reinterpret_cast<const sockaddr *>(&storage), length);
}

}
