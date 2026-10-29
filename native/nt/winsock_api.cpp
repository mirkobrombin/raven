#include "child_process.h"
#include "completion_port.h"
#include "kernel_context.h"

#include <algorithm>
#include "text.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace raven {

namespace {

// A socket is a host descriptor that stays non-blocking inside Raven: a blocking call waits on the host without stopping other guest
// threads, and whether the guest sees blocking behavior is a property of the object.
class SocketObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"File"; }
	explicit SocketObject(int descriptor) : descriptor(descriptor) {}
	~SocketObject() override { ::close(descriptor); }
	int signaled(uint32_t) const override { return 1; }
	int descriptor;
	bool guest_nonblocking = false;
	// Set when the program closed the handle while transfers on it were still pending: they end as aborted.
	bool closed = false;
	// A non-blocking connect is under way: select reports its success as writable and its failure as an exception.
	bool connecting = false;
	int type = SOCK_STREAM;
	std::shared_ptr<CompletionPort> port;
	uint64_t key = 0;
	// SetFileCompletionNotificationModes: no packet for a transfer that finishes at once, and no event set for it.
	bool skip_port_on_success = false, skip_event_on_success = false;
	// WSAEventSelect: the event to set, the network events asked for, the ones that may be reported again, and the ones recorded
	// since the program last enumerated them, with their error codes (WSANETWORKEVENTS order).
	uint64_t select_event = 0;
	uint32_t select_mask = 0, select_enabled = 0, select_recorded = 0;
	std::array<int32_t, 10> select_errors{};
};

constexpr uint32_t wsaeintr = 10004, wsaebadf = 10009, wsaeacces = 10013, wsaefault = 10014, wsaeinval = 10022, wsaemfile = 10024,
	wsaewouldblock = 10035, wsaeinprogress = 10036, wsaealready = 10037, wsaenotsock = 10038, wsaedestaddrreq = 10039, wsaemsgsize = 10040,
	wsaeprototype = 10041, wsaenoprotoopt = 10042, wsaeprotonosupport = 10043, wsaesocktnosupport = 10044, wsaeopnotsupp = 10045,
	wsaeafnosupport = 10047, wsaeaddrinuse = 10048, wsaeaddrnotavail = 10049, wsaenetdown = 10050, wsaenetunreach = 10051,
	wsaeconnaborted = 10053, wsaeconnreset = 10054, wsaenobufs = 10055, wsaeisconn = 10056, wsaenotconn = 10057, wsaeshutdown = 10058,
	wsaetimedout = 10060, wsaeconnrefused = 10061, wsaehostunreach = 10065, wsanotinitialised = 10093, wsahost_not_found = 11001;
constexpr uint64_t status_pending = 0x103, overlapped_error = 0xe0000000;
constexpr uint32_t wsa_io_incomplete = 996;
constexpr uint32_t fd_read = 0x1, fd_write = 0x2, fd_oob = 0x4, fd_accept = 0x8, fd_connect = 0x10, fd_close = 0x20;

uint32_t wsa_error(int host)
{
	switch (host) {
	case EINTR: return wsaeintr;
	case EBADF: return wsaenotsock;
	case EACCES: case EPERM: return wsaeacces;
	case EFAULT: return wsaefault;
	case EINVAL: return wsaeinval;
	case EMFILE: case ENFILE: return wsaemfile;
	case EAGAIN: return wsaewouldblock;
	case EINPROGRESS: return wsaeinprogress;
	case EALREADY: return wsaealready;
	case ENOTSOCK: return wsaenotsock;
	case EDESTADDRREQ: return wsaedestaddrreq;
	case EMSGSIZE: return wsaemsgsize;
	case EPROTOTYPE: return wsaeprototype;
	case ENOPROTOOPT: return wsaenoprotoopt;
	case EPROTONOSUPPORT: return wsaeprotonosupport;
	case ESOCKTNOSUPPORT: return wsaesocktnosupport;
	case EOPNOTSUPP: return wsaeopnotsupp;
	case EAFNOSUPPORT: return wsaeafnosupport;
	case EADDRINUSE: return wsaeaddrinuse;
	case EADDRNOTAVAIL: return wsaeaddrnotavail;
	case ENETDOWN: return wsaenetdown;
	case ENETUNREACH: return wsaenetunreach;
	case ECONNABORTED: case EPIPE: return wsaeconnaborted;
	case ECONNRESET: return wsaeconnreset;
	case ENOBUFS: case ENOMEM: return wsaenobufs;
	case EISCONN: return wsaeisconn;
	case ENOTCONN: return wsaenotconn;
	case ESHUTDOWN: return wsaeshutdown;
	case ETIMEDOUT: return wsaetimedout;
	case ECONNREFUSED: return wsaeconnrefused;
	case EHOSTUNREACH: return wsaehostunreach;
	default: return wsaeinval;
	}
}

// RAVEN_TRACE_SOCKETS: how many bytes a socket received, from whom and when, without the content.
void trace_received(int descriptor, ssize_t count, const char *path)
{
	static const bool traced = std::getenv("RAVEN_TRACE_SOCKETS");
	if (!traced || count <= 0)
		return;
	sockaddr_storage peer{};
	socklen_t size = sizeof(peer);
	char text[INET6_ADDRSTRLEN] = "?";
	uint16_t port = 0;
	if (!::getpeername(descriptor, reinterpret_cast<sockaddr *>(&peer), &size)) {
		if (peer.ss_family == AF_INET) {
			const auto &address = reinterpret_cast<const sockaddr_in &>(peer);
			::inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
			port = ntohs(address.sin_port);
		} else if (peer.ss_family == AF_INET6) {
			const auto &address = reinterpret_cast<const sockaddr_in6 &>(peer);
			::inet_ntop(AF_INET6, &address.sin6_addr, text, sizeof(text));
			port = ntohs(address.sin6_port);
		}
	}
	std::fprintf(stderr, "raven: socket %d received %zd bytes from %s:%u at %lld ms (%s)\n", descriptor, count, text, port,
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(), path);
}

constexpr uint16_t windows_inet6 = 23;

int host_family(int family) { return family == windows_inet6 ? AF_INET6 : family; }
int windows_family(int family) { return family == AF_INET6 ? windows_inet6 : family; }

// A sockaddr as the host wants it: the family number is the only difference from the Windows layout.
std::vector<uint8_t> host_address(ApiCall &call, uint64_t address, size_t length)
{
	auto bytes = call.read(address, length);
	if (bytes.size() >= 2) {
		uint16_t family;
		std::memcpy(&family, bytes.data(), 2);
		family = uint16_t(host_family(family));
		std::memcpy(bytes.data(), &family, 2);
	}
	return bytes;
}

void put_address(ApiCall &call, uint64_t address, uint64_t length_address, const sockaddr_storage &storage, socklen_t length)
{
	if (!address || !length_address)
		return;
	const uint32_t capacity = call.load<uint32_t>(length_address);
	std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t *>(&storage), reinterpret_cast<const uint8_t *>(&storage) + length);
	if (bytes.size() >= 2) {
		uint16_t family;
		std::memcpy(&family, bytes.data(), 2);
		family = uint16_t(windows_family(family));
		std::memcpy(bytes.data(), &family, 2);
	}
	call.write(address, std::span<const uint8_t>(bytes.data(), std::min<size_t>(bytes.size(), capacity)));
	call.store<uint32_t>(length_address, uint32_t(length));
}

constexpr uint32_t sol_socket_windows = 0xffff;

std::vector<uint8_t> message_control(ApiCall &call, int descriptor, uint64_t address, uint32_t length)
{
	if (length > 65536) throw wsaenobufs;
	if (length && !address) throw wsaefault;
	const auto raw = call.read(address, length);
	const size_t width = call.pointer_width(), header = width + 8;
	std::vector<uint8_t> result;
	for (size_t at = 0; at < raw.size();) {
		if (raw.size() - at < header) throw wsaeinval;
		uint64_t size = 0;
		uint32_t level = 0, type = 0;
		std::memcpy(&size, raw.data() + at, width);
		std::memcpy(&level, raw.data() + at + width, 4);
		std::memcpy(&type, raw.data() + at + width + 4, 4);
		if (size < header || size > raw.size() - at) throw wsaeinval;
		const auto *data = raw.data() + at + header;
		const size_t count = size_t(size) - header;
		int host_type = 0;
		std::vector<uint8_t> payload;
		if (level == IPPROTO_IP && type == 19) {
			if (count != 8) throw wsaeinval;
			in_pktinfo info{};
			std::memcpy(&info.ipi_spec_dst, data, 4);
			std::memcpy(&info.ipi_ifindex, data + 4, 4);
			payload.resize(sizeof(info));
			std::memcpy(payload.data(), &info, sizeof(info));
			host_type = IP_PKTINFO;
		} else if (level == IPPROTO_IPV6 && type == 19) {
			if (count != sizeof(in6_pktinfo)) throw wsaeinval;
			payload.assign(data, data + count);
			host_type = IPV6_PKTINFO;
		} else if ((level == IPPROTO_IP || level == IPPROTO_IPV6) && type == 50) {
			if (count != 4) throw wsaeinval;
			uint32_t ecn = 0;
			std::memcpy(&ecn, data, 4);
			if (ecn > 3) throw wsaeinval;
			int setting = 0;
			socklen_t setting_size = sizeof(setting);
			host_type = level == IPPROTO_IP ? IP_TOS : IPV6_TCLASS;
			if (::getsockopt(descriptor, int(level), host_type, &setting, &setting_size)) throw wsa_error(errno);
			setting = (setting & ~3) | int(ecn);
			payload.resize(sizeof(setting));
			std::memcpy(payload.data(), &setting, sizeof(setting));
		} else if (level == IPPROTO_IPV6 && type == 21) {
			if (count != 4) throw wsaeinval;
			payload.assign(data, data + count);
			host_type = IPV6_HOPLIMIT;
		} else {
			throw wsaenoprotoopt;
		}
		const size_t offset = result.size();
		result.resize(offset + CMSG_SPACE(payload.size()), 0);
		auto *control = reinterpret_cast<cmsghdr *>(result.data() + offset);
		control->cmsg_level = int(level);
		control->cmsg_type = host_type;
		control->cmsg_len = CMSG_LEN(payload.size());
		std::memcpy(CMSG_DATA(control), payload.data(), payload.size());
		const size_t next = (at + size_t(size) + width - 1) & ~(width - 1);
		if (next > raw.size()) {
			if (at + size == raw.size()) break;
			throw wsaeinval;
		}
		at = next;
	}
	return result;
}

}

// One TCP endpoint as the host lists it, with the process that holds the socket when it is a process of this runtime.
struct TcpEntry {
	bool v6 = false;
	std::array<uint8_t, 16> local{}, remote{};
	uint32_t local_port = 0, remote_port = 0, state = 0, pid = 0;
};

// Linux socket states in /proc/net/tcp order and the MIB_TCP_STATE values Windows reports for them.
bool set_socket_completion_modes(NativeObjects &objects, uint64_t handle, uint32_t modes)
{
	const auto socket = objects.object_as<SocketObject>(handle);
	if (!socket)
		return false;
	socket->skip_port_on_success = modes & 1;
	socket->skip_event_on_success = modes & 2;
	return true;
}

static uint32_t windows_tcp_state(unsigned host)
{
	static constexpr uint32_t states[] = {0, 5, 3, 4, 6, 7, 11, 1, 8, 10, 2, 9};
	return host < std::size(states) ? states[host] : 1;
}

// The sockets of the processes this user runs, by inode: the owner a TCP table names.
static std::map<uint64_t, uint32_t> socket_owners()
{
	std::map<uint64_t, uint32_t> owners;
	std::error_code error;
	for (const auto &process : std::filesystem::directory_iterator("/proc", error)) {
		const auto name = process.path().filename().string();
		if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
			continue;
		std::error_code inner;
		for (const auto &descriptor : std::filesystem::directory_iterator(process.path() / "fd", inner)) {
			char target[64];
			const auto length = ::readlink(descriptor.path().c_str(), target, sizeof(target) - 1);
			if (length <= 8)
				continue;
			target[length] = 0;
			uint64_t inode = 0;
			if (std::sscanf(target, "socket:[%lu]", &inode) == 1)
				owners.emplace(inode, uint32_t(std::stoul(name)));
		}
	}
	return owners;
}

static std::vector<TcpEntry> tcp_entries(bool v6)
{
	std::vector<TcpEntry> entries;
	std::ifstream table(v6 ? "/proc/net/tcp6" : "/proc/net/tcp");
	std::string line;
	std::getline(table, line);
	std::map<uint64_t, uint32_t> owners;
	bool owners_read = false;
	while (std::getline(table, line)) {
		std::istringstream fields(line);
		std::string slot, local, remote, state, queues, timer, retransmits, uid, timeout, inode;
		if (!(fields >> slot >> local >> remote >> state >> queues >> timer >> retransmits >> uid >> timeout >> inode))
			continue;
		TcpEntry entry;
		entry.v6 = v6;
		const auto address = [&](const std::string &text, std::array<uint8_t, 16> &out, uint32_t &port) {
			const auto colon = text.find(':');
			const auto hex = text.substr(0, colon);
			// The kernel prints each 32-bit word of the address in host order.
			for (size_t word = 0; word * 8 < hex.size() && word < 4; ++word) {
				const uint32_t value = uint32_t(std::stoul(hex.substr(word * 8, 8), nullptr, 16));
				std::memcpy(out.data() + word * 4, &value, 4);
			}
			port = uint32_t(std::stoul(text.substr(colon + 1), nullptr, 16));
		};
		address(local, entry.local, entry.local_port);
		address(remote, entry.remote, entry.remote_port);
		entry.state = windows_tcp_state(unsigned(std::stoul(state, nullptr, 16)));
		const uint64_t node = std::stoull(inode);
		if (node) {
			if (!owners_read) {
				owners = socket_owners();
				owners_read = true;
			}
			if (const auto found = owners.find(node); found != owners.end())
				entry.pid = found->second;
		}
		entries.push_back(entry);
	}
	return entries;
}

void register_winsock_api(ApiRegistry &registry, KernelContext &context)
{
	auto &objects = context.objects;
	auto &threads = context.threads;
	const auto socket_of = [&objects](uint64_t handle) { return objects.object_as<SocketObject>(handle); };
	const auto fail = [](ApiCall &call, uint32_t error, uint64_t value = UINT64_MAX) {
		static const bool traced = std::getenv("RAVEN_TRACE_SOCKETS");
		if (traced && error != 10035)
			std::fprintf(stderr, "raven: %s on socket %llx fails with %u\n", api_in_progress, (unsigned long long)call[0], error);
		call.fail(error, value);
	};

	// Waits until the descriptor is ready for the events or the time is up; true when it is ready. Other guest threads run meanwhile.
	const auto wait_ready = [&threads](int descriptor, short events, std::optional<std::chrono::steady_clock::time_point> wake) {
		const auto ready = [descriptor, events] {
			pollfd entry{descriptor, events, 0};
			return ::poll(&entry, 1, 0) > 0;
		};
		if (ready())
			return true;
		HostWait waiting;
		return threads.block(ready, wake);
	};

	const auto make_socket = [&objects](ApiCall &call, int family, int type, int protocol) -> uint64_t {
		const int descriptor = ::socket(host_family(family), type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
		if (descriptor < 0) {
			call.fail(wsa_error(errno), UINT64_MAX);
			return UINT64_MAX;
		}
		auto object = std::make_shared<SocketObject>(descriptor);
		object->type = type;
		const auto made = objects.add_object(object);
		if (made.status) {
			call.fail(wsaemfile, UINT64_MAX);
			return UINT64_MAX;
		}
		return made.value;
	};
	registry.add("socket", 3, [make_socket](ApiCall &call) { call.ret(make_socket(call, call.i32(0), call.i32(1), call.i32(2))); });
	const auto make_wsa = [make_socket](ApiCall &call) {
		int family = call.i32(0) ? call.i32(0) : AF_INET, type = call.i32(1) ? call.i32(1) : SOCK_STREAM, protocol = call.i32(2);
		// FROM_PROTOCOL_INFO (-1) takes the value from the WSAPROTOCOL_INFO given, as WSAEnumProtocols describes the provider.
		if (call[3]) {
			if (family == -1)
				family = call.load<int32_t>(call[3] + 76);
			if (type == -1)
				type = call.load<int32_t>(call[3] + 88);
			if (protocol == -1)
				protocol = call.load<int32_t>(call[3] + 92);
		}
		call.ret(make_socket(call, family, type, protocol));
	};
	registry.add("WSASocketA", 6, make_wsa);
	registry.add("WSASocketW", 6, make_wsa);
	registry.add("closesocket", 1, [&objects, &context, socket_of, fail](ApiCall &call) {
		if (!socket_of(call[0])) {
			fail(call, wsaenotsock, UINT64_MAX);
			return;
		}
		socket_of(call[0])->closed = true;
		objects.close(call[0]);
		// Transfers still pending on the socket end as aborted now: a busy process may not look at them again until their descriptor
		// turns ready, which for a quiet connection can be the server's own timeout.
		context.reactor->poll();
		call.ret(0);
	});
	registry.add("shutdown", 2, [socket_of, fail](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock);
			return;
		}
		static constexpr int how[] = {SHUT_RD, SHUT_WR, SHUT_RDWR};
		if (call.u32(1) > 2 || ::shutdown(socket->descriptor, how[call.u32(1)])) {
			fail(call, call.u32(1) > 2 ? wsaeinval : wsa_error(errno));
			return;
		}
		call.ret(0);
	});
	registry.add("bind", 3, [socket_of, fail](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || call.i32(2) < 2) {
			fail(call, socket ? wsaefault : wsaenotsock);
			return;
		}
		const auto address = host_address(call, call[1], size_t(call.i32(2)));
		if (::bind(socket->descriptor, reinterpret_cast<const sockaddr *>(address.data()), socklen_t(address.size()))) {
			fail(call, wsa_error(errno));
			return;
		}
		call.ret(0);
	});
	registry.add("listen", 2, [socket_of, fail](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || ::listen(socket->descriptor, call.i32(1))) {
			fail(call, socket ? wsa_error(errno) : wsaenotsock);
			return;
		}
		call.ret(0);
	});
	registry.add("connect", 3, [socket_of, fail, wait_ready](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || call.i32(2) < 2) {
			fail(call, socket ? wsaefault : wsaenotsock);
			return;
		}
		const auto address = host_address(call, call[1], size_t(call.i32(2)));
		if (std::getenv("RAVEN_TRACE_SOCKETS") && address.size() >= sizeof(sockaddr_in) && reinterpret_cast<const sockaddr *>(address.data())->sa_family == AF_INET) {
			const auto *target = reinterpret_cast<const sockaddr_in *>(address.data());
			char text[INET_ADDRSTRLEN] = {};
			::inet_ntop(AF_INET, &target->sin_addr, text, sizeof(text));
			std::fprintf(stderr, "raven: socket %llx connects to %s:%u\n", (unsigned long long)call[0], text, unsigned(ntohs(target->sin_port)));
		}
		if (::connect(socket->descriptor, reinterpret_cast<const sockaddr *>(address.data()), socklen_t(address.size())) == 0) {
			call.ret(0);
			return;
		}
		if (errno != EINPROGRESS) {
			fail(call, wsa_error(errno));
			return;
		}
		if (socket->guest_nonblocking) {
			socket->connecting = true;
			fail(call, wsaewouldblock);
			return;
		}
		wait_ready(socket->descriptor, POLLOUT, std::nullopt);
		int error = 0;
		socklen_t length = sizeof(error);
		::getsockopt(socket->descriptor, SOL_SOCKET, SO_ERROR, &error, &length);
		if (error) {
			fail(call, wsa_error(error));
			return;
		}
		call.ret(0);
	});
	registry.add("accept", 3, [&objects, socket_of, fail, wait_ready](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock);
			return;
		}
		for (;;) {
			sockaddr_storage storage{};
			socklen_t length = sizeof(storage);
			socket->select_enabled |= fd_accept;
			const int accepted = ::accept4(socket->descriptor, reinterpret_cast<sockaddr *>(&storage), &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (accepted >= 0) {
				const auto made = objects.add_object(std::make_shared<SocketObject>(accepted));
				if (made.status) {
					::close(accepted);
					fail(call, wsaemfile);
					return;
				}
				put_address(call, call[1], call[2], storage, length);
				call.ret(made.value);
				return;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				fail(call, wsa_error(errno));
				return;
			}
			if (socket->guest_nonblocking) {
				fail(call, wsaewouldblock);
				return;
			}
			wait_ready(socket->descriptor, POLLIN, std::nullopt);
		}
	});

	// Data transfer: every form reads and writes through one pair of helpers so blocking and flags behave the same.
	const auto host_flags = [](uint32_t flags) {
		int result = MSG_NOSIGNAL;
		if (flags & 1)
			result |= MSG_OOB;
		if (flags & 2)
			result |= MSG_PEEK;
		if (flags & 4)
			result |= MSG_DONTROUTE;
		if (flags & 8)
			result |= MSG_WAITALL;
		return result;
	};
	// Receives into the guest buffer; the byte count, or -1 with the Windows error set.
	const auto receive = [wait_ready, host_flags](ApiCall &call, const std::shared_ptr<SocketObject> &socket, uint64_t buffer, size_t length,
		uint32_t flags, sockaddr_storage *from, socklen_t *from_length) -> int64_t {
		std::vector<uint8_t> bytes(std::min<size_t>(length, 1 << 20));
		// Reading re-enables the readable notification of WSAEventSelect.
		socket->select_enabled |= fd_read;
		for (;;) {
			if (from_length)
				*from_length = sizeof(sockaddr_storage);
			const ssize_t count = ::recvfrom(socket->descriptor, bytes.data(), bytes.size(), (host_flags(flags) & ~MSG_WAITALL) | MSG_DONTWAIT,
				reinterpret_cast<sockaddr *>(from), from_length);
			if (count >= 0) {
				call.write(buffer, std::span<const uint8_t>(bytes.data(), size_t(count)));
				trace_received(socket->descriptor, count, "direct");
				return count;
			}
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				call.set_last_error(wsa_error(errno));
				return -1;
			}
			if (socket->guest_nonblocking) {
				call.set_last_error(wsaewouldblock);
				return -1;
			}
			wait_ready(socket->descriptor, POLLIN, std::nullopt);
		}
	};
	const auto transmit = [wait_ready, host_flags](ApiCall &call, const std::shared_ptr<SocketObject> &socket, uint64_t buffer, size_t length,
		uint32_t flags, const sockaddr *to, socklen_t to_length) -> int64_t {
		const auto bytes = call.read(buffer, length);
		if (std::getenv("RAVEN_TRACE_SOCKETS"))
			std::fprintf(stderr, "raven: socket sends %zu bytes: %.*s\n", bytes.size(), int(std::min<size_t>(bytes.size(), 600)), reinterpret_cast<const char *>(bytes.data()));
		size_t sent = 0;
		while (sent < bytes.size() || bytes.empty()) {
			const ssize_t count = ::sendto(socket->descriptor, bytes.data() + sent, bytes.size() - sent, host_flags(flags) | MSG_DONTWAIT, to, to_length);
			if (count >= 0) {
				sent += size_t(count);
				if (socket->type != SOCK_STREAM || bytes.empty())
					break;
				continue;
			}
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				call.set_last_error(wsa_error(errno));
				return -1;
			}
			// A non-blocking socket reports what it could take; with nothing taken it reports that it would block, and the writable
			// notification of WSAEventSelect comes again once it can take more.
			socket->select_enabled |= fd_write;
			if (socket->guest_nonblocking) {
				if (sent)
					break;
				call.set_last_error(wsaewouldblock);
				return -1;
			}
			wait_ready(socket->descriptor, POLLOUT, std::nullopt);
		}
		return int64_t(sent);
	};
	registry.add("recv", 4, [socket_of, fail, receive](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock);
			return;
		}
		const auto count = receive(call, socket, call[1], size_t(std::max(call.i32(2), 0)), call.u32(3), nullptr, nullptr);
		call.ret(count < 0 ? uint64_t(UINT64_MAX) : uint64_t(count));
	});
	registry.add("recvfrom", 6, [socket_of, fail, receive](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock);
			return;
		}
		sockaddr_storage from{};
		socklen_t from_length = sizeof(from);
		const auto count = receive(call, socket, call[1], size_t(std::max(call.i32(2), 0)), call.u32(3), call[4] ? &from : nullptr, call[4] ? &from_length : nullptr);
		if (count >= 0 && call[4])
			put_address(call, call[4], call[5], from, from_length);
		call.ret(count < 0 ? uint64_t(UINT64_MAX) : uint64_t(count));
	});
	registry.add("send", 4, [socket_of, fail, transmit](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock);
			return;
		}
		const auto count = transmit(call, socket, call[1], size_t(std::max(call.i32(2), 0)), call.u32(3), nullptr, 0);
		call.ret(count < 0 ? uint64_t(UINT64_MAX) : uint64_t(count));
	});
	registry.add("sendto", 6, [socket_of, fail, transmit](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock);
			return;
		}
		std::vector<uint8_t> to;
		if (call[4] && call.i32(5) >= 2)
			to = host_address(call, call[4], size_t(call.i32(5)));
		const auto count = transmit(call, socket, call[1], size_t(std::max(call.i32(2), 0)), call.u32(3),
			to.empty() ? nullptr : reinterpret_cast<const sockaddr *>(to.data()), socklen_t(to.size()));
		call.ret(count < 0 ? uint64_t(UINT64_MAX) : uint64_t(count));
	});
	// WSABUF is a length and a pointer: 8 bytes in a 32-bit program, 16 in a 64-bit one.
	struct Buffer {
		uint64_t address;
		uint32_t length;
	};
	const auto buffers_of = [](ApiCall &call, uint64_t array, uint32_t count) {
		std::vector<Buffer> result;
		const size_t width = call.pointer_width(), stride = width == 4 ? 8 : 16;
		for (uint32_t index = 0; index < count && index < 64; ++index)
			result.push_back({call.load_pointer(array + index * stride + (width == 4 ? 4 : 8)), call.load<uint32_t>(array + index * stride)});
		return result;
	};
	// Overlapped transfers: the work is tried at once; what cannot finish yet waits in the reactor and completes later, reporting through
	// the OVERLAPPED structure, its event and the completion port the socket belongs to.
	struct Transfer {
		std::shared_ptr<SocketObject> socket;
		bool receiving = false;
		std::vector<Buffer> buffers;
		std::vector<uint8_t> outgoing;
		std::vector<uint8_t> control;
		size_t sent = 0;
		uint32_t flags = 0;
		uint64_t from = 0, from_length = 0, flags_out = 0, overlapped = 0;
		std::vector<uint8_t> to;
		size_t width = 4;
	};
	auto &process = context.process;
	auto &reactor = *context.reactor;
	// 0 when the transfer finished (status and bytes are set), 1 when it has to wait.
	const auto attempt = [&process, host_flags](Transfer &transfer, uint32_t &status, uint64_t &bytes) -> int {
		auto &socket = *transfer.socket;
		if (socket.closed) {
			status = 995;
			bytes = 0;
			return 0;
		}
		if (transfer.receiving) {
			size_t total = 0;
			for (const auto &buffer : transfer.buffers)
				total += buffer.length;
			std::vector<uint8_t> data(std::min<size_t>(total, 1 << 20));
			sockaddr_storage from{};
			socklen_t from_length = sizeof(from);
			const ssize_t count = ::recvfrom(socket.descriptor, data.data(), data.size(), (host_flags(transfer.flags) & ~MSG_WAITALL) | MSG_DONTWAIT,
				transfer.from ? reinterpret_cast<sockaddr *>(&from) : nullptr, transfer.from ? &from_length : nullptr);
			if (count < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					return 1;
				status = wsa_error(errno);
				bytes = 0;
				return 0;
			}
			trace_received(socket.descriptor, count, "overlapped");
			size_t offset = 0;
			for (const auto &buffer : transfer.buffers) {
				const size_t part = std::min<size_t>(buffer.length, size_t(count) - offset);
				if (part)
					process.write_memory(buffer.address, std::span<const uint8_t>(data.data() + offset, part));
				offset += part;
			}
			if (transfer.from) {
				std::vector<uint8_t> raw(reinterpret_cast<const uint8_t *>(&from), reinterpret_cast<const uint8_t *>(&from) + from_length);
				if (raw.size() >= 2) {
					uint16_t family;
					std::memcpy(&family, raw.data(), 2);
					family = uint16_t(windows_family(family));
					std::memcpy(raw.data(), &family, 2);
				}
				process.write_memory(transfer.from, raw);
				if (transfer.from_length) {
					const uint32_t length = uint32_t(from_length);
					process.write_memory(transfer.from_length, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&length), 4));
				}
			}
			status = 0;
			bytes = uint64_t(count);
			return 0;
		}
		while (transfer.sent < transfer.outgoing.size() || transfer.outgoing.empty()) {
			ssize_t count;
			if (!transfer.control.empty()) {
				iovec data{transfer.outgoing.data(), transfer.outgoing.size()};
				msghdr message{};
				message.msg_name = transfer.to.empty() ? nullptr : transfer.to.data();
				message.msg_namelen = socklen_t(transfer.to.size());
				message.msg_iov = &data;
				message.msg_iovlen = 1;
				message.msg_control = transfer.control.data();
				message.msg_controllen = transfer.control.size();
				count = ::sendmsg(socket.descriptor, &message, host_flags(transfer.flags) | MSG_DONTWAIT);
			} else count = ::sendto(socket.descriptor, transfer.outgoing.empty() ? nullptr : transfer.outgoing.data() + transfer.sent, transfer.outgoing.size() - transfer.sent,
				host_flags(transfer.flags) | MSG_DONTWAIT, transfer.to.empty() ? nullptr : reinterpret_cast<const sockaddr *>(transfer.to.data()),
				socklen_t(transfer.to.size()));
			if (count < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					return 1;
				status = wsa_error(errno);
				bytes = transfer.sent;
				return 0;
			}
			transfer.sent += size_t(count);
			if (socket.type != SOCK_STREAM || transfer.outgoing.empty())
				break;
		}
		status = 0;
		bytes = transfer.sent;
		return 0;
	};
	// OVERLAPPED: the status and the byte count come first, the event handle after the two offset words.
	const auto finish = [&process, &objects](const std::shared_ptr<SocketObject> &socket, uint64_t overlapped, size_t width, uint32_t status, uint64_t bytes,
		bool at_once) {
		const auto put = [&](uint64_t at, uint64_t value) {
			std::vector<uint8_t> raw(width);
			std::memcpy(raw.data(), &value, width);
			process.write_memory(at, raw);
		};
		// The status word holds zero for success and the Windows Sockets error under a marker otherwise; WSAGetOverlappedResult reads it.
		put(overlapped, status ? (overlapped_error | status) : 0);
		put(overlapped + width, bytes);
		const auto handle = [&] {
			const auto raw = process.read_memory(overlapped + 2 * width + 8, width);
			uint64_t value = 0;
			std::memcpy(&value, raw.data(), width);
			return value;
		}();
		// A transfer that finished at once with success skips what the completion modes ask to skip.
		const bool succeeded_at_once = at_once && !status;
		if ((handle & ~uint64_t(1)) && !(succeeded_at_once && socket->skip_event_on_success))
			objects.set_event(handle & ~uint64_t(1), true);
		// A handle with its low bit set asks for no completion packet.
		if (socket->port && !(handle & 1) && !(succeeded_at_once && socket->skip_port_on_success))
			socket->port->post({bytes, socket->key, overlapped, status});
	};
	const auto complete = [finish](const Transfer &transfer, uint32_t status, uint64_t bytes, bool at_once) {
		finish(transfer.socket, transfer.overlapped, transfer.width, status, bytes, at_once);
	};
	// Starts a transfer for the WSA calls; returns the call's result (0 or SOCKET_ERROR) with the byte count stored when it finished.
	const auto start = [attempt, complete, &reactor, &objects](ApiCall &call, Transfer transfer, uint64_t bytes_out) {
		transfer.width = call.pointer_width();
		uint32_t status = 0;
		uint64_t bytes = 0;
		if (!transfer.overlapped) {
			call.fail(wsaeopnotsupp, UINT32_MAX);
			return;
		}
		// Starting an operation resets the OVERLAPPED's event, which only its completion sets: a program that reuses the structure
		// must not see the previous transfer's signal as this one's.
		if (const auto event = call.load_pointer(transfer.overlapped + 2 * transfer.width + 8) & ~uint64_t(1))
			objects.set_event(event, false);
		if (attempt(transfer, status, bytes) == 0) {
			if (status) {
				call.fail(status, UINT32_MAX);
				return;
			}
			complete(transfer, 0, bytes, true);
			if (bytes_out)
				call.store<uint32_t>(bytes_out, uint32_t(bytes));
			call.ret(0);
			return;
		}
		// Until it finishes the OVERLAPPED says the operation is pending.
		{
			std::vector<uint8_t> pending(transfer.width, 0);
			const uint64_t value = status_pending;
			std::memcpy(pending.data(), &value, transfer.width);
			call.write(transfer.overlapped, pending);
		}
		auto shared = std::make_shared<Transfer>(std::move(transfer));
		const int descriptor = shared->socket->descriptor;
		const short events = shared->receiving ? POLLIN : POLLOUT;
		reactor.add([shared, attempt, complete]() {
			uint32_t result = 0;
			uint64_t count = 0;
			// A program may release the memory of a transfer still pending, as one that frees a fiber's stack does after closing the
			// socket; the kernel then fails the copy into it and the operation ends unseen, without harm to the process.
			try {
				if (attempt(*shared, result, count) != 0)
					return false;
				complete(*shared, result, count, false);
			} catch (const std::runtime_error &failure) {
				if (std::getenv("RAVEN_TRACE_SOCKETS"))
					std::fprintf(stderr, "raven: pending transfer on socket %d dropped: %s\n", shared->socket->descriptor, failure.what());
			}
			return true;
		}, descriptor, events);
		call.fail(997, UINT32_MAX);
	};
	// Without an OVERLAPPED the call is an ordinary blocking or non-blocking transfer.
	registry.add("WSARecv", 7, [socket_of, fail, receive, buffers_of, start](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock, UINT32_MAX);
			return;
		}
		if (call[6]) {
			fail(call, wsaeopnotsupp, UINT32_MAX);
			return;
		}
		const uint32_t flags = call[4] ? call.load<uint32_t>(call[4]) : 0;
		if (call[5]) {
			Transfer transfer;
			transfer.socket = socket;
			transfer.receiving = true;
			transfer.buffers = buffers_of(call, call[1], call.u32(2));
			transfer.flags = flags;
			transfer.overlapped = call[5];
			start(call, std::move(transfer), call[3]);
			return;
		}
		int64_t total = 0;
		for (const auto &buffer : buffers_of(call, call[1], call.u32(2))) {
			if (!buffer.length)
				continue;
			const auto count = receive(call, socket, buffer.address, buffer.length, flags, nullptr, nullptr);
			if (count < 0) {
				if (total)
					break;
				call.ret(UINT32_MAX);
				return;
			}
			total += count;
			if (uint32_t(count) < buffer.length)
				break;
		}
		if (call[3])
			call.store<uint32_t>(call[3], uint32_t(total));
		if (call[4])
			call.store<uint32_t>(call[4], 0);
		call.ret(0);
	});
	registry.add("WSASend", 7, [socket_of, fail, transmit, buffers_of, start](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock, UINT32_MAX);
			return;
		}
		if (call[6]) {
			fail(call, wsaeopnotsupp, UINT32_MAX);
			return;
		}
		if (call[5]) {
			Transfer transfer;
			transfer.socket = socket;
			transfer.flags = call.u32(4);
			transfer.overlapped = call[5];
			for (const auto &buffer : buffers_of(call, call[1], call.u32(2))) {
				const auto bytes = call.read(buffer.address, buffer.length);
				transfer.outgoing.insert(transfer.outgoing.end(), bytes.begin(), bytes.end());
				if (std::getenv("RAVEN_TRACE_SOCKETS"))
					std::fprintf(stderr, "raven: socket sends %zu bytes: %.*s\n", bytes.size(), int(std::min<size_t>(bytes.size(), 600)), reinterpret_cast<const char *>(bytes.data()));
			}
			start(call, std::move(transfer), call[3]);
			return;
		}
		int64_t total = 0;
		for (const auto &buffer : buffers_of(call, call[1], call.u32(2))) {
			if (!buffer.length)
				continue;
			const auto count = transmit(call, socket, buffer.address, buffer.length, call.u32(4), nullptr, 0);
			if (count < 0) {
				if (total)
					break;
				call.ret(UINT32_MAX);
				return;
			}
			total += count;
			if (uint32_t(count) < buffer.length)
				break;
		}
		if (call[3])
			call.store<uint32_t>(call[3], uint32_t(total));
		call.ret(0);
	});
	registry.add("WSARecvFrom", 9, [socket_of, fail, receive, buffers_of, start](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock, UINT32_MAX);
			return;
		}
		if (call[8]) {
			fail(call, wsaeopnotsupp, UINT32_MAX);
			return;
		}
		const auto buffers = buffers_of(call, call[1], call.u32(2));
		if (buffers.empty()) {
			fail(call, wsaeinval, UINT32_MAX);
			return;
		}
		const uint32_t flags = call[4] ? call.load<uint32_t>(call[4]) : 0;
		if (call[7]) {
			Transfer transfer;
			transfer.socket = socket;
			transfer.receiving = true;
			transfer.buffers = buffers;
			transfer.flags = flags;
			transfer.from = call[5];
			transfer.from_length = call[6];
			transfer.overlapped = call[7];
			start(call, std::move(transfer), call[3]);
			return;
		}
		sockaddr_storage from{};
		socklen_t from_length = sizeof(from);
		const auto count = receive(call, socket, buffers[0].address, buffers[0].length, flags, call[5] ? &from : nullptr, call[5] ? &from_length : nullptr);
		if (count < 0) {
			call.ret(UINT32_MAX);
			return;
		}
		if (call[5])
			put_address(call, call[5], call[6], from, from_length);
		if (call[3])
			call.store<uint32_t>(call[3], uint32_t(count));
		if (call[4])
			call.store<uint32_t>(call[4], 0);
		call.ret(0);
	});
	registry.add("WSASendTo", 9, [socket_of, fail, transmit, buffers_of, start](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock, UINT32_MAX);
			return;
		}
		if (call[8]) {
			fail(call, wsaeopnotsupp, UINT32_MAX);
			return;
		}
		const auto buffers = buffers_of(call, call[1], call.u32(2));
		if (buffers.empty()) {
			fail(call, wsaeinval, UINT32_MAX);
			return;
		}
		std::vector<uint8_t> to;
		if (call[5] && call.i32(6) >= 2)
			to = host_address(call, call[5], size_t(call.i32(6)));
		if (call[7]) {
			Transfer transfer;
			transfer.socket = socket;
			transfer.flags = call.u32(4);
			transfer.to = to;
			transfer.overlapped = call[7];
			for (const auto &buffer : buffers) {
				const auto bytes = call.read(buffer.address, buffer.length);
				transfer.outgoing.insert(transfer.outgoing.end(), bytes.begin(), bytes.end());
			}
			start(call, std::move(transfer), call[3]);
			return;
		}
		const auto count = transmit(call, socket, buffers[0].address, buffers[0].length, call.u32(4),
			to.empty() ? nullptr : reinterpret_cast<const sockaddr *>(to.data()), socklen_t(to.size()));
		if (count < 0) {
			call.ret(UINT32_MAX);
			return;
		}
		if (call[3])
			call.store<uint32_t>(call[3], uint32_t(count));
		call.ret(0);
	});

	registry.add("WSASendMsg", 6, [socket_of, fail, buffers_of, start, attempt, wait_ready](ApiCall &call) {
		try {
			const auto socket = socket_of(call[0]);
			if (!socket) { fail(call, wsaenotsock); return; }
			if (socket->type != SOCK_DGRAM && socket->type != SOCK_RAW) { fail(call, wsaeopnotsupp); return; }
			if (call[5] || (call.u32(2) & ~5U)) { fail(call, wsaeopnotsupp); return; }
			if (!call[1] || (!call[3] && !call[4])) { fail(call, wsaefault); return; }
			const size_t width = call.pointer_width();
			call.read(call[1], width == 4 ? 28 : 56);
			if (call[3]) call.write(call[3], call.read(call[3], 4));
			if (call[4]) call.write(call[4], call.read(call[4], 3 * width + 8));
			const uint64_t name = call.load_pointer(call[1]);
			const int32_t name_size = call.load<int32_t>(call[1] + width);
			if (name_size < 0 || name_size > int32_t(sizeof(sockaddr_storage)) || (name_size && !name)) throw wsaefault;
			Transfer transfer;
			transfer.socket = socket;
			transfer.flags = call.u32(2);
			transfer.overlapped = call[4];
			if (name) {
				if (name_size < 2) throw wsaefault;
				transfer.to = host_address(call, name, size_t(name_size));
			}
			const size_t buffers_at = width == 4 ? 8 : 16, control_at = width == 4 ? 16 : 32;
			const uint64_t array = call.load_pointer(call[1] + buffers_at);
			const uint32_t count = call.load<uint32_t>(call[1] + buffers_at + width);
			if (!array || !count) throw wsaefault;
			if (count > 64) throw wsaenobufs;
			for (const auto &buffer : buffers_of(call, array, count)) {
				if (buffer.length > (1U << 20) - transfer.outgoing.size()) throw wsaemsgsize;
				const auto bytes = call.read(buffer.address, buffer.length);
				transfer.outgoing.insert(transfer.outgoing.end(), bytes.begin(), bytes.end());
			}
			transfer.control = message_control(call, socket->descriptor,
				call.load_pointer(call[1] + control_at + width), call.load<uint32_t>(call[1] + control_at));
			if (transfer.overlapped) { start(call, std::move(transfer), call[3]); return; }
			uint32_t status = 0;
			uint64_t sent = 0;
			while (attempt(transfer, status, sent)) {
				socket->select_enabled |= fd_write;
				if (socket->guest_nonblocking) { fail(call, wsaewouldblock); return; }
				wait_ready(socket->descriptor, POLLOUT, std::nullopt);
			}
			if (status) { fail(call, status); return; }
			call.store<uint32_t>(call[3], uint32_t(sent));
			call.ret(0);
		} catch (const ApiFault &) {
			fail(call, wsaefault);
		} catch (const uint32_t &error) {
			fail(call, error);
		}
	});

	// WSARecvMsg(socket, message, received, overlapped, routine): WSARecvFrom over the parts of WSAMSG (the name and its length, the
	// buffers, the control buffer and the flags). No control data is delivered, so its length comes back as zero.
	registry.add("WSARecvMsg", 5, [&registry, fail](ApiCall &call) {
		if (!call[1]) {
			fail(call, wsaefault);
			return;
		}
		const size_t width = call.pointer_width(), buffers_at = width == 4 ? 8 : 16, control_at = width == 4 ? 16 : 32,
			flags_at = width == 4 ? 24 : 48;
		const uint64_t message = call[1];
		const auto receive_from = registry.find("WSARecvFrom");
		ApiCall inner(call.process, call.environment, {call[0], call.load_pointer(message + buffers_at), call.load<uint32_t>(message + buffers_at + width),
			call[2], message + flags_at, call.load_pointer(message), call.load_pointer(message) ? message + width : 0, call[3], call[4]});
		receive_from->second(inner);
		if (inner.result != uint64_t(UINT64_MAX) || call.last_error() == 997)
			call.store<uint32_t>(message + control_at, 0);
		call.ret(inner.result);
	});

	// The extension functions of Winsock are found through WSAIoctl by GUID; each is an export the runtime answers like any other.
	registry.add("ConnectEx", 7, [socket_of, fail, finish, &reactor](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || !call[6] || call.i32(2) < 2) {
			fail(call, socket ? wsaeinval : wsaenotsock, 0);
			return;
		}
		const size_t width = call.pointer_width();
		const uint64_t overlapped = call[6];
		const auto address = host_address(call, call[1], size_t(call.i32(2)));
		auto data = call[3] && call.u32(4) ? call.read(call[3], call.u32(4)) : std::vector<uint8_t>{};
		const auto sent_out = call[5];
		const auto done = [socket, finish, overlapped, width, data = std::move(data)](uint32_t status, bool at_once) {
			uint64_t bytes = 0;
			if (!status && !data.empty()) {
				const ssize_t count = ::send(socket->descriptor, data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
				bytes = count > 0 ? uint64_t(count) : 0;
			}
			finish(socket, overlapped, width, status, bytes, at_once);
			return bytes;
		};
		if (::connect(socket->descriptor, reinterpret_cast<const sockaddr *>(address.data()), socklen_t(address.size())) == 0) {
			const auto bytes = done(0, true);
			if (sent_out)
				call.store<uint32_t>(sent_out, uint32_t(bytes));
			call.ret(1);
			return;
		}
		if (errno != EINPROGRESS) {
			fail(call, wsa_error(errno), 0);
			return;
		}
		reactor.add([socket, done]() {
			if (socket->closed) {
				done(995, false);
				return true;
			}
			pollfd entry{socket->descriptor, POLLOUT, 0};
			if (::poll(&entry, 1, 0) <= 0)
				return false;
			int error = 0;
			socklen_t length = sizeof(error);
			::getsockopt(socket->descriptor, SOL_SOCKET, SO_ERROR, &error, &length);
			done(error ? wsa_error(error) : 0, false);
			return true;
		}, socket->descriptor, short(POLLOUT));
		fail(call, 997, 0);
	});
	registry.add("AcceptEx", 8, [socket_of, fail, finish, &reactor, &process](ApiCall &call) {
		const auto listener = socket_of(call[0]);
		const auto target = socket_of(call[1]);
		if (!listener || !target || !call[7]) {
			fail(call, listener && target ? wsaeinval : wsaenotsock, 0);
			return;
		}
		if (call.u32(3)) {
			// Receiving the first data together with the connection is not done: the call says so.
			fail(call, wsaeopnotsupp, 0);
			return;
		}
		const size_t width = call.pointer_width();
		const uint64_t overlapped = call[7], buffer = call[2], local_length = call.u32(4), remote_length = call.u32(5);
		const auto attempt = [listener, target, &process, buffer, local_length, remote_length]() -> int {
			sockaddr_storage peer{};
			socklen_t peer_length = sizeof(peer);
			const int accepted = ::accept4(listener->descriptor, reinterpret_cast<sockaddr *>(&peer), &peer_length, SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (accepted < 0)
				return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : int(wsa_error(errno));
			::dup3(accepted, target->descriptor, SOCK_CLOEXEC);
			::close(accepted);
			const auto write_address = [&](uint64_t at, uint32_t capacity, const sockaddr_storage &storage, socklen_t length) {
				std::vector<uint8_t> raw(capacity);
				std::memcpy(raw.data(), &storage, std::min<size_t>(length, capacity));
				uint16_t family;
				std::memcpy(&family, raw.data(), 2);
				family = uint16_t(windows_family(family));
				std::memcpy(raw.data(), &family, 2);
				process.write_memory(at, raw);
			};
			sockaddr_storage local{};
			socklen_t local_size = sizeof(local);
			::getsockname(target->descriptor, reinterpret_cast<sockaddr *>(&local), &local_size);
			write_address(buffer, local_length, local, local_size);
			write_address(buffer + local_length, remote_length, peer, peer_length);
			return 0;
		};
		const int first = attempt();
		if (first == 0) {
			finish(listener, overlapped, width, 0, 0, true);
			call.ret(1);
			return;
		}
		if (first > 0) {
			fail(call, uint32_t(first), 0);
			return;
		}
		reactor.add([attempt, finish, listener, overlapped, width]() {
			if (listener->closed) {
				finish(listener, overlapped, width, 995, 0, false);
				return true;
			}
			const int result = attempt();
			if (result < 0)
				return false;
			finish(listener, overlapped, width, uint32_t(result), 0, false);
			return true;
		}, listener->descriptor, short(POLLIN));
		fail(call, 997, 0);
	});
	registry.add("GetAcceptExSockaddrs", 8, [](ApiCall &call) {
		const uint64_t local = call[0] + call.u32(1), remote = local + call.u32(2);
		const auto length_of = [&](uint64_t address) {
			const auto family = call.load<uint16_t>(address);
			return uint32_t(family == windows_inet6 ? 28 : 16);
		};
		if (call[4])
			call.store_pointer(call[4], local);
		if (call[5])
			call.store<uint32_t>(call[5], length_of(local));
		if (call[6])
			call.store_pointer(call[6], remote);
		if (call[7])
			call.store<uint32_t>(call[7], length_of(remote));
	});
	registry.add("DisconnectEx", 4, [socket_of, fail, finish](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock, 0);
			return;
		}
		::shutdown(socket->descriptor, SHUT_RDWR);
		if (call[1])
			finish(socket, call[1], call.pointer_width(), 0, 0, true);
		call.ret(1);
	});

	// Completion ports: one queue that overlapped operations report to and worker threads take from.
	context.threads.add_pump([&reactor] { reactor.poll(); });
	registry.add("CreateIoCompletionPort", 4, [&objects, socket_of](ApiCall &call) {
		std::shared_ptr<CompletionPort> port;
		if (call[1]) {
			port = objects.object_as<CompletionPort>(call[1]);
			if (!port) {
				call.fail(6, 0);
				return;
			}
		}
		const bool create = !port;
		uint64_t handle = call[1];
		if (create) {
			port = std::make_shared<CompletionPort>();
			const auto made = objects.add_object(port);
			if (made.status) {
				call.fail(4, 0);
				return;
			}
			handle = made.value;
		}
		if (call[0] != UINT64_MAX) {
			const auto socket = socket_of(call[0]);
			const auto file = objects.object_as<FileObject>(call[0]);
			if (!socket && !file) {
				if (create)
					objects.close(handle);
				call.fail(6, 0);
				return;
			}
			if (socket) {
				socket->port = port;
				socket->key = call[2];
			} else {
				file->port = port;
				file->key = call[2];
			}
		}
		call.ret(handle);
	});
	registry.add("PostQueuedCompletionStatus", 4, [&objects](ApiCall &call) {
		const auto port = objects.object_as<CompletionPort>(call[0]);
		if (!port) {
			call.fail(6);
			return;
		}
		port->post({call.u32(1), call[2], call[3], 0});
		call.ret(1);
	});
	// Waits for one packet; empty when the time ran out.
	const auto wait_packet = [&reactor, &threads](const std::shared_ptr<CompletionPort> &port, uint32_t milliseconds) -> std::optional<CompletionPacket> {
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (milliseconds != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		for (;;) {
			reactor.poll();
			CompletionPacket packet;
			if (port->take(packet))
				return packet;
			if (milliseconds == 0 || (wake && std::chrono::steady_clock::now() >= *wake))
				return std::nullopt;
			HostWait waiting;
			threads.block([&] { reactor.poll(); return !port->empty(); }, wake);
		}
	};
	registry.add("GetQueuedCompletionStatus", 5, [&objects, wait_packet](ApiCall &call) {
		const auto port = objects.object_as<CompletionPort>(call[0]);
		if (!port) {
			call.fail(6);
			return;
		}
		const auto packet = wait_packet(port, call.u32(4));
		if (!packet) {
			if (call[3])
				call.store_pointer(call[3], 0);
			call.fail(258);
			return;
		}
		if (call[1])
			call.store<uint32_t>(call[1], uint32_t(packet->bytes));
		if (call[2])
			call.store_pointer(call[2], packet->key);
		if (call[3])
			call.store_pointer(call[3], packet->overlapped);
		if (packet->status) {
			call.fail(packet->status);
			return;
		}
		call.ret(1);
	});
	// OVERLAPPED_ENTRY: the key, the OVERLAPPED pointer, an internal word and the byte count.
	registry.add("GetQueuedCompletionStatusEx", 6, [&objects, wait_packet](ApiCall &call) {
		const auto port = objects.object_as<CompletionPort>(call[0]);
		if (!port || !call[1] || !call.u32(2) || !call[3]) {
			call.fail(port ? 87 : 6);
			return;
		}
		const size_t width = call.pointer_width(), stride = width == 4 ? 16 : 32;
		uint32_t removed = 0;
		auto packet = wait_packet(port, call.u32(4));
		while (packet && removed < call.u32(2)) {
			const uint64_t entry = call[1] + removed * stride;
			call.store_pointer(entry, packet->key);
			call.store_pointer(entry + width, packet->overlapped);
			call.store_pointer(entry + 2 * width, packet->status);
			call.store<uint32_t>(entry + 3 * width, uint32_t(packet->bytes));
			++removed;
			// The next packet is taken only when there is room for it; one taken and not handed out would be lost.
			if (removed < call.u32(2))
				packet = wait_packet(port, 0);
		}
		if (!removed) {
			call.fail(258);
			return;
		}
		call.store<uint32_t>(call[3], removed);
		call.ret(1);
	});

	registry.add("getsockname", 3, [socket_of, fail](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || !call[2]) {
			fail(call, socket ? wsaefault : wsaenotsock);
			return;
		}
		sockaddr_storage storage{};
		socklen_t length = sizeof(storage);
		if (::getsockname(socket->descriptor, reinterpret_cast<sockaddr *>(&storage), &length)) {
			fail(call, wsa_error(errno));
			return;
		}
		put_address(call, call[1], call[2], storage, length);
		call.ret(0);
	});
	registry.add("getpeername", 3, [socket_of, fail](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || !call[2]) {
			fail(call, socket ? wsaefault : wsaenotsock);
			return;
		}
		sockaddr_storage storage{};
		socklen_t length = sizeof(storage);
		if (::getpeername(socket->descriptor, reinterpret_cast<sockaddr *>(&storage), &length)) {
			fail(call, wsa_error(errno));
			return;
		}
		put_address(call, call[1], call[2], storage, length);
		call.ret(0);
	});

	// Options: the level and the option number differ between Windows and the host, and a few values have another shape.
	struct Option {
		int level, name;
	};
	const auto host_option = [](uint32_t level, uint32_t name) -> std::optional<Option> {
		if (level == sol_socket_windows) {
			switch (name) {
			case 0x0004: return Option{SOL_SOCKET, SO_REUSEADDR};
			case 0x0008: return Option{SOL_SOCKET, SO_KEEPALIVE};
			case 0x0020: return Option{SOL_SOCKET, SO_BROADCAST};
			case 0x0080: return Option{SOL_SOCKET, SO_LINGER};
			case 0x1001: return Option{SOL_SOCKET, SO_SNDBUF};
			case 0x1002: return Option{SOL_SOCKET, SO_RCVBUF};
			case 0x1005: return Option{SOL_SOCKET, SO_SNDTIMEO};
			case 0x1006: return Option{SOL_SOCKET, SO_RCVTIMEO};
			case 0x1007: return Option{SOL_SOCKET, SO_ERROR};
			case 0x1008: return Option{SOL_SOCKET, SO_TYPE};
			default: return std::nullopt;
			}
		}
		if (level == IPPROTO_TCP && name == 1)
			return Option{IPPROTO_TCP, TCP_NODELAY};
		if (level == IPPROTO_IPV6 && name == 27)
			return Option{IPPROTO_IPV6, IPV6_V6ONLY};
		if (level == IPPROTO_IP) {
			if (name == 4)
				return Option{IPPROTO_IP, IP_TOS};
			if (name == 2)
				return Option{IPPROTO_IP, IP_TTL};
		}
		return std::nullopt;
	};
	registry.add("setsockopt", 5, [socket_of, fail, host_option](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock);
			return;
		}
		const auto option = host_option(call.u32(1), call.u32(2));
		if (!option) {
			fail(call, wsaenoprotoopt);
			return;
		}
		const auto value = call[3] ? call.read(call[3], size_t(std::max(call.i32(4), 0))) : std::vector<uint8_t>{};
		int result;
		if (option->level == SOL_SOCKET && (option->name == SO_RCVTIMEO || option->name == SO_SNDTIMEO) && value.size() >= 4) {
			uint32_t milliseconds;
			std::memcpy(&milliseconds, value.data(), 4);
			timeval timeout{time_t(milliseconds / 1000), suseconds_t(milliseconds % 1000 * 1000)};
			result = ::setsockopt(socket->descriptor, option->level, option->name, &timeout, sizeof(timeout));
		} else if (option->level == SOL_SOCKET && option->name == SO_LINGER && value.size() >= 4) {
			uint16_t on, seconds;
			std::memcpy(&on, value.data(), 2);
			std::memcpy(&seconds, value.data() + 2, 2);
			linger setting{on, seconds};
			result = ::setsockopt(socket->descriptor, option->level, option->name, &setting, sizeof(setting));
		} else if (!value.empty() && value.size() < sizeof(int)) {
			// Winsock reads a Boolean option from as little as one byte, which Linux refuses as shorter than an int.
			int widened = 0;
			for (size_t at = 0; at < value.size(); ++at)
				widened |= int(value[at]) << (8 * at);
			result = ::setsockopt(socket->descriptor, option->level, option->name, &widened, sizeof(widened));
		} else {
			result = ::setsockopt(socket->descriptor, option->level, option->name, value.data(), socklen_t(value.size()));
		}
		if (result) {
			fail(call, wsa_error(errno));
			return;
		}
		call.ret(0);
	});
	registry.add("getsockopt", 5, [socket_of, fail, host_option](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || !call[3] || !call[4]) {
			fail(call, socket ? wsaefault : wsaenotsock);
			return;
		}
		const auto option = host_option(call.u32(1), call.u32(2));
		if (!option) {
			fail(call, wsaenoprotoopt);
			return;
		}
		int value = 0;
		socklen_t length = sizeof(value);
		if (::getsockopt(socket->descriptor, option->level, option->name, &value, &length)) {
			fail(call, wsa_error(errno));
			return;
		}
		if (option->level == SOL_SOCKET && option->name == SO_ERROR)
			value = int(value ? wsa_error(value) : 0);
		const uint32_t capacity = call.load<uint32_t>(call[4]);
		if (capacity < 4) {
			fail(call, wsaefault);
			return;
		}
		call.store<int32_t>(call[3], value);
		call.store<uint32_t>(call[4], 4);
		call.ret(0);
	});

	constexpr uint32_t fionbio = 0x8004667e, fionread = 0x4004667f, sio_keepalive_vals = 0x98000004, sio_get_extension_function = 0xc8000006;
	const auto resolve_export = [&context](const char *library, const char *name) { return context.modules.resolve({library, name, 0, false, 0}); };
	// WSAEventSelect: the sockets are looked at whenever the threads are scheduled; a network event the program asked for is recorded
	// once until the call that consumes it enables it again, and recording one sets the program's event.
	auto selected = std::make_shared<std::vector<std::weak_ptr<SocketObject>>>();
	const auto select_descriptor = [](const SocketObject &socket) {
		const auto enabled = socket.select_mask & socket.select_enabled;
		if (!socket.select_mask || socket.closed || (!enabled && !socket.connecting))
			return pollfd{-1, 0, 0};
		short events = 0;
		if (enabled & (fd_read | fd_accept | fd_close)) events |= POLLIN;
		if ((enabled & fd_write) || socket.connecting) events |= POLLOUT;
		if (enabled & fd_oob) events |= POLLPRI;
		return pollfd{socket.descriptor, events, 0};
	};
	const auto record_select = [&objects](SocketObject &socket, short ready) {
		uint32_t found = 0;
		const auto record = [&](uint32_t event, size_t bit, int32_t error) {
			if (!(socket.select_mask & event) || !(socket.select_enabled & event))
				return;
			socket.select_enabled &= ~event;
			socket.select_recorded |= event;
			socket.select_errors[bit] = error;
			found |= event;
		};
		if (socket.connecting) {
			if (!(ready & (POLLOUT | POLLERR | POLLHUP)))
				return;
			socket.connecting = false;
			int error = 0;
			socklen_t length = sizeof(error);
			::getsockopt(socket.descriptor, SOL_SOCKET, SO_ERROR, &error, &length);
			record(fd_connect, 4, error ? int32_t(wsa_error(error)) : 0);
			if (error) {
				record(fd_close, 5, int32_t(wsa_error(error)));
				if (found && socket.select_event)
					objects.set_event(socket.select_event, true);
				return;
			}
		}
		int listening = 0;
		socklen_t length = sizeof(listening);
		::getsockopt(socket.descriptor, SOL_SOCKET, SO_ACCEPTCONN, &listening, &length);
		if (listening) {
			if (ready & POLLIN)
				record(fd_accept, 3, 0);
		} else {
			if (ready & POLLIN) {
				char probe;
				const ssize_t count = ::recv(socket.descriptor, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
				if (count > 0)
					record(fd_read, 0, 0);
				else if (count == 0)
					record(fd_close, 5, 0);
				else if (errno != EAGAIN && errno != EWOULDBLOCK)
					record(fd_close, 5, int32_t(wsa_error(errno)));
			} else if (ready & (POLLHUP | POLLERR)) {
				record(fd_close, 5, int32_t(wsaeconnreset));
			}
			if (ready & POLLPRI)
				record(fd_oob, 2, 0);
			if (ready & POLLOUT)
				record(fd_write, 1, 0);
		}
		if (found && socket.select_event)
			objects.set_event(socket.select_event, true);
	};
	const auto check_select = [select_descriptor, record_select](SocketObject &socket) {
		auto entry = select_descriptor(socket);
		if (entry.fd >= 0 && ::poll(&entry, 1, 0) > 0)
			record_select(socket, entry.revents);
	};
	const auto check_all = [selected, select_descriptor, record_select, entries = std::vector<pollfd>()]() mutable {
		std::erase_if(*selected, [](const auto &weak) { return weak.expired(); });
		entries.clear();
		bool active = false;
		for (const auto &weak : *selected)
			if (const auto socket = weak.lock()) {
				entries.push_back(select_descriptor(*socket));
				active |= entries.back().fd >= 0;
			} else {
				entries.push_back({-1, 0, 0});
			}
		if (!active || ::poll(entries.data(), entries.size(), 0) <= 0)
			return;
		for (size_t index = 0; index < entries.size(); ++index)
			if (entries[index].revents)
				if (const auto socket = (*selected)[index].lock())
					record_select(*socket, entries[index].revents);
	};
	context.threads.add_pump(check_all);
	context.threads.add_service(check_all);
	context.threads.add_external([selected] {
		return std::any_of(selected->begin(), selected->end(), [](const auto &weak) {
			const auto socket = weak.lock();
			return socket && socket->select_mask && !socket->closed;
		});
	});
	registry.add("WSAEventSelect", 3, [socket_of, fail, selected, check_select](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock, uint32_t(-1));
			return;
		}
		if (call.u32(2) && !call[1]) {
			fail(call, wsaeinval, uint32_t(-1));
			return;
		}
		// Selecting events makes the socket non-blocking, and every event may be reported from then on.
		socket->select_event = call[1];
		socket->select_mask = call.u32(2);
		socket->select_enabled = 0x3ff;
		socket->select_recorded = 0;
		socket->select_errors = {};
		socket->guest_nonblocking = true;
		if (std::none_of(selected->begin(), selected->end(), [&](const auto &weak) { return weak.lock() == socket; }))
			selected->push_back(socket);
		check_select(*socket);
		call.ret(0);
	});
	// WSANETWORKEVENTS: the recorded events, then ten error codes indexed by event bit.
	registry.add("WSAEnumNetworkEvents", 3, [&objects, socket_of, fail, check_select](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || !call[2]) {
			fail(call, socket ? wsaefault : wsaenotsock, uint32_t(-1));
			return;
		}
		check_select(*socket);
		if (std::getenv("RAVEN_TRACE_SOCKETS"))
			std::fprintf(stderr, "raven: socket %llx network events %x, connect error %d, close error %d\n", (unsigned long long)call[0],
				socket->select_recorded, socket->select_errors[4], socket->select_errors[5]);
		std::vector<uint8_t> bytes(44, 0);
		std::memcpy(bytes.data(), &socket->select_recorded, 4);
		std::memcpy(bytes.data() + 4, socket->select_errors.data(), 40);
		call.write(call[2], bytes);
		socket->select_recorded = 0;
		socket->select_errors = {};
		if (call[1])
			objects.set_event(call[1], false);
		call.ret(0);
	});
	// The outcome of an overlapped transfer, waited for when the caller asks to.
	registry.add("WSAGetOverlappedResult", 5, [socket_of, fail, &reactor, &threads](ApiCall &call) {
		if (!socket_of(call[0]) || !call[1] || !call[2]) {
			fail(call, socket_of(call[0]) ? wsaefault : wsaenotsock, 0);
			return;
		}
		const size_t width = call.pointer_width();
		const auto word = [&](uint64_t at) {
			const auto raw = call.read(at, width);
			uint64_t value = 0;
			std::memcpy(&value, raw.data(), width);
			return value;
		};
		if (word(call[1]) == status_pending) {
			if (!call.u32(3)) {
				fail(call, wsa_io_incomplete, 0);
				return;
			}
			HostWait waiting;
			threads.block([&] {
				reactor.poll();
				return word(call[1]) != status_pending;
			}, std::nullopt);
		}
		const auto status = word(call[1]);
		call.store<uint32_t>(call[2], uint32_t(word(call[1] + width)));
		if (call[4])
			call.store<uint32_t>(call[4], 0);
		if (status) {
			fail(call, (status & overlapped_error) == overlapped_error ? uint32_t(status & 0xffff) : uint32_t(wsaeconnreset), 0);
			return;
		}
		call.ret(1);
	});
	// The TCP endpoints of the host with the process that owns each one (iphlpapi): programs check who is on the other end of a
	// connection to themselves.
	registry.add("GetExtendedTcpTable", 6, [](ApiCall &call) {
		constexpr uint32_t af_inet = 2, af_inet6 = 23, insufficient_buffer = 122, invalid_parameter = 87, not_supported = 50;
		const uint32_t family = call.u32(3), kind = call.u32(4);
		if (!call[1] || (family != af_inet && family != af_inet6)) {
			call.ret(invalid_parameter);
			return;
		}
		// TCP_TABLE_BASIC_* (0-2) and TCP_TABLE_OWNER_PID_* (3-5); the module tables are not offered.
		if (kind > 5) {
			call.ret(not_supported);
			return;
		}
		const bool v6 = family == af_inet6, owner = kind >= 3;
		const bool listeners = kind == 0 || kind == 2 || kind == 3 || kind == 5, connections = kind == 0 || kind == 1 || kind == 4 || kind == 5;
		auto entries = tcp_entries(v6);
		std::erase_if(entries, [&](const TcpEntry &entry) { return entry.state == 2 ? !listeners : !connections; });
		if (call.u32(2))
			std::sort(entries.begin(), entries.end(), [](const TcpEntry &a, const TcpEntry &b) {
				return std::tie(a.local, a.local_port, a.remote, a.remote_port) < std::tie(b.local, b.local_port, b.remote, b.remote_port);
			});
		const size_t row = v6 ? (owner ? 56 : 52) : (owner ? 24 : 20);
		const uint32_t needed = uint32_t(4 + row * entries.size());
		const uint32_t capacity = call.load<uint32_t>(call[1]);
		if (!call[0] || capacity < needed) {
			call.store<uint32_t>(call[1], needed);
			call.ret(insufficient_buffer);
			return;
		}
		std::vector<uint8_t> bytes(needed, 0);
		const auto put = [&](size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
		put(0, uint32_t(entries.size()));
		for (size_t index = 0; index < entries.size(); ++index) {
			const auto &entry = entries[index];
			const size_t at = 4 + index * row;
			const uint32_t local_port = htons(uint16_t(entry.local_port)), remote_port = htons(uint16_t(entry.remote_port));
			if (!v6) {
				// MIB_TCPROW(_OWNER_PID): state, local address and port, remote address and port, owner.
				put(at, entry.state);
				std::memcpy(bytes.data() + at + 4, entry.local.data(), 4);
				put(at + 8, local_port);
				std::memcpy(bytes.data() + at + 12, entry.remote.data(), 4);
				put(at + 16, entry.state == 2 ? 0 : remote_port);
				if (owner)
					put(at + 20, entry.pid);
			} else {
				// MIB_TCP6ROW(_OWNER_PID): local address, scope, port, remote address, scope, port, state, owner.
				std::memcpy(bytes.data() + at, entry.local.data(), 16);
				put(at + 20, local_port);
				std::memcpy(bytes.data() + at + 24, entry.remote.data(), 16);
				put(at + 44, entry.state == 2 ? 0 : remote_port);
				put(at + 48, entry.state);
				if (owner)
					put(at + 52, entry.pid);
			}
		}
		call.write(call[0], bytes);
		call.store<uint32_t>(call[1], needed);
		call.ret(0);
	});
	registry.add("ioctlsocket", 3, [socket_of, fail](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket || !call[2]) {
			fail(call, socket ? wsaefault : wsaenotsock);
			return;
		}
		if (call.u32(1) == fionbio) {
			socket->guest_nonblocking = call.load<uint32_t>(call[2]) != 0;
		} else if (call.u32(1) == fionread) {
			int available = 0;
			::ioctl(socket->descriptor, FIONREAD, &available);
			call.store<uint32_t>(call[2], uint32_t(available));
		} else {
			fail(call, wsaeinval);
			return;
		}
		call.ret(0);
	});
	registry.add("WSAIoctl", 9, [socket_of, fail, resolve_export](ApiCall &call) {
		const auto socket = socket_of(call[0]);
		if (!socket) {
			fail(call, wsaenotsock);
			return;
		}
		// Completion by overlapped structure or routine is not carried out: say so rather than finish the call as if it were.
		if (call[7] || call[8]) {
			fail(call, wsaeopnotsupp);
			return;
		}
		const auto code = call.u32(1);
		if (code == fionbio && call[2] && call.u32(3) >= 4) {
			socket->guest_nonblocking = call.load<uint32_t>(call[2]) != 0;
		} else if (code == fionread && call[4] && call.u32(5) >= 4) {
			int available = 0;
			::ioctl(socket->descriptor, FIONREAD, &available);
			call.store<uint32_t>(call[4], uint32_t(available));
		} else if (code == sio_get_extension_function && call[2] && call.u32(3) >= 16 && call[4] && call.u32(5) >= call.pointer_width()) {
			static constexpr struct { uint32_t first; uint16_t second, third; uint8_t rest[8]; const char *name; } known[] = {
				{0x25a207b9, 0xddf3, 0x4660, {0x8e, 0xe9, 0x76, 0xe5, 0x8c, 0x74, 0x06, 0x3e}, "ConnectEx"},
				{0xb5367df1, 0xcbac, 0x11cf, {0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92}, "AcceptEx"},
				{0xb5367df2, 0xcbac, 0x11cf, {0x95, 0xca, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92}, "GetAcceptExSockaddrs"},
				{0x7fda2e11, 0x8630, 0x436f, {0xa0, 0x31, 0xf5, 0x36, 0xa6, 0xee, 0xc1, 0x57}, "DisconnectEx"},
				{0xa441e712, 0x754f, 0x43ca, {0x84, 0xa7, 0x0d, 0xee, 0x44, 0xcf, 0x60, 0x6d}, "WSASendMsg"},
				{0xf689d7c8, 0x6f1f, 0x436b, {0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22}, "WSARecvMsg"},
			};
			const auto guid = call.read(call[2], 16);
			const char *name = nullptr;
			for (const auto &entry : known) {
				uint32_t first;
				uint16_t second, third;
				std::memcpy(&first, guid.data(), 4);
				std::memcpy(&second, guid.data() + 4, 2);
				std::memcpy(&third, guid.data() + 6, 2);
				if (first == entry.first && second == entry.second && third == entry.third && std::memcmp(guid.data() + 8, entry.rest, 8) == 0)
					name = entry.name;
			}
			if (!name) {
				fail(call, wsaeinval);
				return;
			}
			try {
				call.store_pointer(call[4], resolve_export("ws2_32.dll", name));
			} catch (const std::runtime_error &) {
				fail(call, wsaeopnotsupp);
				return;
			}
		} else if (code == sio_keepalive_vals && call[2] && call.u32(3) >= 12) {
			const int enabled = call.load<uint32_t>(call[2]) != 0, idle = int(std::max<uint32_t>(call.load<uint32_t>(call[2] + 4) / 1000, 1)),
				interval = int(std::max<uint32_t>(call.load<uint32_t>(call[2] + 8) / 1000, 1));
			::setsockopt(socket->descriptor, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
			::setsockopt(socket->descriptor, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
			::setsockopt(socket->descriptor, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
		} else {
			fail(call, wsaeopnotsupp);
			return;
		}
		if (call[6])
			call.store<uint32_t>(call[6], code == fionread ? 4 : code == sio_get_extension_function ? uint32_t(call.pointer_width()) : 0);
		call.ret(0);
	});

	// select: an fd_set is a count followed by that many socket handles, each as wide as the program's pointers.
	const auto set_at = [](ApiCall &call) { return call.pointer_width() == 4 ? size_t(4) : size_t(8); };
	registry.add("select", 5, [socket_of, fail, set_at, &threads](ApiCall &call) {
		struct Entry {
			uint64_t handle;
			int descriptor;
		};
		std::vector<Entry> sets[3];
		const size_t first = set_at(call), width = call.pointer_width();
		for (int kind = 0; kind < 3; ++kind) {
			const auto address = call[1 + size_t(kind)];
			if (!address)
				continue;
			const uint32_t count = std::min<uint32_t>(call.load<uint32_t>(address), 64);
			for (uint32_t index = 0; index < count; ++index) {
				const auto handle = call.load_pointer(address + first + index * width);
				const auto socket = socket_of(handle);
				if (!socket) {
					fail(call, wsaenotsock);
					return;
				}
				sets[kind].push_back({handle, socket->descriptor});
			}
		}
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (call[4]) {
			const auto seconds = call.load<int32_t>(call[4]), micros = call.load<int32_t>(call[4] + 4);
			wake = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(seconds, 0)) + std::chrono::microseconds(std::max(micros, 0));
		}
		static constexpr short events[3] = {POLLIN, POLLOUT, POLLPRI};
		const auto poll_once = [&](std::vector<pollfd> &entries) {
			entries.clear();
			for (int kind = 0; kind < 3; ++kind)
				for (const auto &item : sets[kind])
					entries.push_back({item.descriptor, events[kind], 0});
			return entries.empty() ? 0 : ::poll(entries.data(), nfds_t(entries.size()), 0);
		};
		std::vector<pollfd> entries;
		int ready = poll_once(entries);
		if (ready <= 0 && !(call[4] && wake && *wake <= std::chrono::steady_clock::now())) {
			HostWait waiting;
			threads.block([&] {
				std::vector<pollfd> again;
				return poll_once(again) > 0;
			}, wake);
			ready = poll_once(entries);
		}
		if (ready < 0) {
			fail(call, wsa_error(errno));
			return;
		}
		size_t position = 0;
		uint32_t total = 0;
		for (int kind = 0; kind < 3; ++kind) {
			const auto address = call[1 + size_t(kind)];
			std::vector<uint64_t> kept;
			for (const auto &item : sets[kind]) {
				const auto revents = entries[position++].revents;
				const auto socket = socket_of(item.handle);
				const bool connecting = socket && socket->connecting;
				const bool failed = (revents & (POLLERR | POLLHUP)) != 0;
				bool hit;
				if (connecting && kind == 1) {
					hit = (revents & POLLOUT) != 0 && !failed;
				} else if (connecting && kind == 2) {
					hit = failed;
				} else {
					hit = kind == 0 ? (revents & (POLLIN | POLLHUP | POLLERR)) != 0 : kind == 1 ? (revents & (POLLOUT | POLLERR)) != 0 : (revents & POLLPRI) != 0;
				}
				if (hit && connecting && kind != 0)
					socket->connecting = false;
				if (hit)
					kept.push_back(item.handle);
			}
			total += uint32_t(kept.size());
			if (!address)
				continue;
			call.store<uint32_t>(address, uint32_t(kept.size()));
			for (size_t index = 0; index < kept.size(); ++index) {
				if (width == 4)
					call.store<uint32_t>(address + first + index * width, uint32_t(kept[index]));
				else
					call.store<uint64_t>(address + first + index * width, kept[index]);
			}
		}
		call.ret(total);
	});
	registry.add("__WSAFDIsSet", 2, [set_at](ApiCall &call) {
		const auto address = call[1];
		if (!address) {
			call.ret(0);
			return;
		}
		const size_t first = set_at(call), width = call.pointer_width();
		const uint32_t count = std::min<uint32_t>(call.load<uint32_t>(address), 64);
		for (uint32_t index = 0; index < count; ++index)
			if (call.load_pointer(address + first + index * width) == (width == 4 ? (call[0] & 0xffffffffULL) : call[0])) {
				call.ret(1);
				return;
			}
		call.ret(0);
	});

	// Names: the answers come from the host resolver, and the result is one block of guest memory that freeaddrinfo gives back.
	registry.add("getaddrinfo", 4, [&context](ApiCall &call) {
		const size_t width = call.pointer_width();
		// ADDRINFOA: four ints, the address length, then the canonical name, the address and the next entry.
		const size_t record = 16 + 4 * width;
		const size_t length_at = 16, canon_at = 16 + width, address_at = 16 + 2 * width, next_at = 16 + 3 * width;
		addrinfo hints{};
		const addrinfo *use = nullptr;
		if (call[2]) {
			hints.ai_flags = call.load<int32_t>(call[2]) & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
			hints.ai_family = host_family(call.load<int32_t>(call[2] + 4));
			hints.ai_socktype = call.load<int32_t>(call[2] + 8);
			hints.ai_protocol = call.load<int32_t>(call[2] + 12);
			use = &hints;
		}
		const auto node = call[0] ? call.narrow(call[0], 1025) : std::string();
		const auto service = call[1] ? call.narrow(call[1], 256) : std::string();
		addrinfo *found = nullptr;
		const int status = ::getaddrinfo(call[0] ? node.c_str() : nullptr, call[1] ? service.c_str() : nullptr, use, &found);
		if (status) {
			const uint32_t error = status == EAI_NONAME || status == EAI_NODATA ? wsahost_not_found : status == EAI_AGAIN ? 11002 :
				status == EAI_FAIL ? 11003 : status == EAI_SERVICE ? 10109 : status == EAI_FAMILY ? wsaeafnosupport : status == EAI_SOCKTYPE ? wsaesocktnosupport : wsaeinval;
			call.fail(error, error);
			return;
		}
		size_t count = 0, size = 0;
		for (const auto *entry = found; entry; entry = entry->ai_next) {
			++count;
			size += record + 32 + (entry->ai_canonname ? std::strlen(entry->ai_canonname) + 1 : 0);
		}
		const auto block = context.allocator.allocate(size, true);
		if (!block) {
			::freeaddrinfo(found);
			call.fail(wsaenobufs, wsaenobufs);
			return;
		}
		uint64_t cursor = block, previous = 0;
		for (const auto *entry = found; entry; entry = entry->ai_next) {
			const uint64_t self = cursor, address = self + record, canonical = address + 32;
			cursor = canonical + (entry->ai_canonname ? std::strlen(entry->ai_canonname) + 1 : 0);
			call.store<int32_t>(self, entry->ai_flags);
			call.store<int32_t>(self + 4, windows_family(entry->ai_family));
			call.store<int32_t>(self + 8, entry->ai_socktype);
			call.store<int32_t>(self + 12, entry->ai_protocol);
			if (width == 4)
				call.store<uint32_t>(self + length_at, entry->ai_addrlen);
			else
				call.store<uint64_t>(self + length_at, entry->ai_addrlen);
			std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t *>(entry->ai_addr), reinterpret_cast<const uint8_t *>(entry->ai_addr) + entry->ai_addrlen);
			if (bytes.size() >= 2) {
				uint16_t family;
				std::memcpy(&family, bytes.data(), 2);
				family = uint16_t(windows_family(family));
				std::memcpy(bytes.data(), &family, 2);
			}
			call.write(address, bytes);
			call.store_pointer(self + address_at, address);
			if (entry->ai_canonname) {
				const std::string name(entry->ai_canonname);
				call.write(canonical, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(name.c_str()), name.size() + 1));
				call.store_pointer(self + canon_at, canonical);
			}
			if (previous)
				call.store_pointer(previous + next_at, self);
			previous = self;
		}
		::freeaddrinfo(found);
		call.store_pointer(call[3], block);
		call.ret(0);
		(void)count;
	});
	registry.add("freeaddrinfo", 1, [&context](ApiCall &call) {
		if (call[0])
			context.allocator.release(call[0]);
	});
	// getnameinfo: the name or the number of the address a program holds, from the host resolver. Windows numbers its NI_ flags
	// differently from the host: the same five requests stand in another order.
	registry.add("getnameinfo", 7, [fail](ApiCall &call) {
		if (call.i32(1) < 2) {
			fail(call, wsaeinval);
			return;
		}
		const auto address = host_address(call, call[0], size_t(call.i32(1)));
		int flags = 0;
		if (call.u32(6) & 0x01) flags |= NI_NOFQDN;
		if (call.u32(6) & 0x02) flags |= NI_NUMERICHOST;
		if (call.u32(6) & 0x04) flags |= NI_NAMEREQD;
		if (call.u32(6) & 0x08) flags |= NI_NUMERICSERV;
		if (call.u32(6) & 0x10) flags |= NI_DGRAM;
		const uint32_t node_length = call.u32(3), service_length = call.u32(5);
		std::vector<char> node(std::max<uint32_t>(node_length, 1)), service(std::max<uint32_t>(service_length, 1));
		const int status = ::getnameinfo(reinterpret_cast<const sockaddr *>(address.data()), socklen_t(address.size()),
			node_length ? node.data() : nullptr, node_length, service_length ? service.data() : nullptr, service_length, flags);
		if (status) {
			fail(call, status == EAI_AGAIN ? 11002 : status == EAI_FAIL ? 11003 : status == EAI_OVERFLOW || status == EAI_FAMILY ? wsaeinval : wsahost_not_found);
			return;
		}
		if (node_length)
			call.put_narrow(call[2], std::string(node.data()), node_length);
		if (service_length)
			call.put_narrow(call[4], std::string(service.data()), service_length);
		call.ret(0);
	});
	registry.add("gethostname", 2, [](ApiCall &call) {
		char name[256] = {};
		::gethostname(name, sizeof(name) - 1);
		const std::string text(name);
		if (!call[0] || call.i32(1) <= int(text.size())) {
			call.fail(call[0] ? wsaefault : wsaefault, UINT64_MAX);
			return;
		}
		call.put_narrow(call[0], text, size_t(call.i32(1)));
		call.ret(0);
	});
	registry.add("inet_addr", 1, [](ApiCall &call) {
		const auto text = call.narrow(call[0], 64);
		in_addr address{};
		call.ret(::inet_aton(text.c_str(), &address) ? address.s_addr : 0xffffffffU);
	});
	auto text_block = std::make_shared<uint64_t>(0);
	registry.add("inet_ntoa", 1, [&context, text_block](ApiCall &call) {
		if (!*text_block)
			*text_block = context.allocator.allocate(32, true);
		in_addr address{};
		address.s_addr = call.u32(0);
		const std::string text = ::inet_ntoa(address);
		call.put_narrow(*text_block, text, 32);
		call.ret(*text_block);
	});
	// getservbyname and getservbyport answer from the services database into one SERVENT the process reuses, as Winsock's per-thread
	// one is reused; the 64-bit layout keeps the protocol before the port.
	auto service_block = std::make_shared<uint64_t>(0);
	const auto service = [&context, service_block](ApiCall &call, const servent *found) {
		if (!found) {
			call.fail(11004, 0);
			return;
		}
		if (!*service_block)
			*service_block = context.allocator.allocate(256, true);
		const uint64_t block = *service_block, aliases = block + 32, name = block + 48, protocol = block + 160;
		const bool narrow = call.pointer_width() == 4;
		call.put_narrow(name, std::string(found->s_name).substr(0, 111), 112);
		call.put_narrow(protocol, std::string(found->s_proto).substr(0, 31), 32);
		call.store_pointer(aliases, 0);
		call.store_pointer(block, name);
		call.store_pointer(block + (narrow ? 4 : 8), aliases);
		call.store<uint16_t>(block + (narrow ? 8 : 24), uint16_t(found->s_port));
		call.store_pointer(block + (narrow ? 12 : 16), protocol);
		call.ret(block);
	};
	registry.add("getservbyname", 2, [service](ApiCall &call) {
		const auto name = call.narrow(call[0], 256);
		const auto protocol = call[1] ? call.narrow(call[1], 32) : std::string();
		servent entry{}, *found = nullptr;
		std::array<char, 1024> buffer{};
		getservbyname_r(name.c_str(), call[1] ? protocol.c_str() : nullptr, &entry, buffer.data(), buffer.size(), &found);
		service(call, found);
	});
	registry.add("getservbyport", 2, [service](ApiCall &call) {
		const auto protocol = call[1] ? call.narrow(call[1], 32) : std::string();
		servent entry{}, *found = nullptr;
		std::array<char, 1024> buffer{};
		getservbyport_r(int(uint16_t(call.u32(0))), call[1] ? protocol.c_str() : nullptr, &entry, buffer.data(), buffer.size(), &found);
		service(call, found);
	});
	// getprotobyname and getprotobynumber answer from the protocols database into one PROTOENT the process reuses.
	auto protocol_block = std::make_shared<uint64_t>(0);
	const auto protocol = [&context, protocol_block](ApiCall &call, const protoent *found) {
		if (!found) {
			call.fail(11004, 0);
			return;
		}
		if (!*protocol_block)
			*protocol_block = context.allocator.allocate(256, true);
		const bool narrow = call.pointer_width() == 4;
		const uint64_t block = *protocol_block, aliases = block + 32, name = block + 48;
		call.put_narrow(name, std::string(found->p_name).substr(0, 95), 96);
		call.store_pointer(block, name);
		call.store_pointer(block + (narrow ? 4 : 8), aliases);
		call.store<uint16_t>(block + (narrow ? 8 : 16), uint16_t(found->p_proto));
		call.ret(block);
	};
	registry.add("getprotobyname", 1, [protocol](ApiCall &call) {
		const auto name = call.narrow(call[0], 256);
		protoent entry{}, *found = nullptr;
		std::array<char, 1024> buffer{};
		getprotobyname_r(name.c_str(), &entry, buffer.data(), buffer.size(), &found);
		protocol(call, found);
	});
	registry.add("getprotobynumber", 1, [protocol](ApiCall &call) {
		protoent entry{}, *found = nullptr;
		std::array<char, 1024> buffer{};
		getprotobynumber_r(int(call.u32(0)), &entry, buffer.data(), buffer.size(), &found);
		protocol(call, found);
	});
	registry.add("htons", 1, [](ApiCall &call) { call.ret(uint16_t(call.u32(0) << 8 | (call.u32(0) >> 8 & 0xff))); });
	registry.add("ntohs", 1, [](ApiCall &call) { call.ret(uint16_t(call.u32(0) << 8 | (call.u32(0) >> 8 & 0xff))); });
	registry.add("htonl", 1, [](ApiCall &call) { call.ret(__builtin_bswap32(call.u32(0))); });
	registry.add("ntohl", 1, [](ApiCall &call) { call.ret(__builtin_bswap32(call.u32(0))); });

	// WSADATA differs in field order: the version words come first in both, the rest follows the program's layout.
	// A numeric address with an optional port ("1.2.3.4:80", "[::1]:80" or a bare address) as a SOCKADDR of the family asked for.
	for (const bool wide : {true, false})
		registry.add(wide ? "WSAStringToAddressW" : "WSAStringToAddressA", 5, [wide](ApiCall &call) {
			const auto text = wide ? to_utf8(call.wide(call[0], 256)) : call.narrow(call[0], 256);
			const int family = call.i32(1);
			if (!call[3] || !call[4] || (family != 2 && family != 23)) {
				call.fail(family != 2 && family != 23 ? 10047U : wsaefault, UINT64_MAX);
				return;
			}
			std::string host = text;
			unsigned long port = 0;
			const auto port_of = [&](const std::string &digits) {
				char *end = nullptr;
				port = std::strtoul(digits.c_str(), &end, 10);
				return !digits.empty() && end && !*end && port <= 65535;
			};
			bool valid = true;
			if (family == 23 && !text.empty() && text[0] == '[') {
				const auto close = text.find(']');
				valid = close != text.npos && (close + 1 == text.size() || (text[close + 1] == ':' && port_of(text.substr(close + 2))));
				host = valid ? text.substr(1, close - 1) : std::string();
			} else if (family == 2 && text.find(':') != text.npos) {
				const auto colon = text.find(':');
				valid = port_of(text.substr(colon + 1));
				host = text.substr(0, colon);
			}
			std::vector<uint8_t> address(family == 2 ? 16 : 28, 0);
			const uint16_t kind = uint16_t(family), network_port = htons(uint16_t(port));
			std::memcpy(address.data(), &kind, 2);
			std::memcpy(address.data() + 2, &network_port, 2);
			if (!valid || ::inet_pton(family == 2 ? AF_INET : AF_INET6, host.c_str(), address.data() + (family == 2 ? 4 : 8)) != 1) {
				call.fail(wsaeinval, UINT64_MAX);
				return;
			}
			const auto capacity = call.load<int32_t>(call[4]);
			call.store<int32_t>(call[4], int32_t(address.size()));
			if (capacity < int32_t(address.size())) {
				call.fail(wsaefault, UINT64_MAX);
				return;
			}
			call.write(call[3], address);
			call.ret(0);
		});
	// WSAEnumProtocols(protocols, buffer, length): the base TCP and UDP providers of IPv4 and IPv6, each as a WSAPROTOCOL_INFO with
	// the service flags, provider identifiers and names the Microsoft provider publishes; a list of protocol numbers narrows them.
	const auto enumerate_protocols = [](ApiCall &call, bool wide) {
		struct Protocol {
			uint32_t service_flags, catalog;
			bool version6;
			int32_t type, protocol;
			uint32_t message_size;
			const char *name;
		};
		static constexpr Protocol protocols[] = {
			{0x20066, 1001, false, 1, 6, 0, "MSAFD Tcpip [TCP/IP]"},
			{0x20609, 1002, false, 2, 17, 0xfff7, "MSAFD Tcpip [UDP/IP]"},
			{0x20066, 1003, true, 1, 6, 0, "MSAFD Tcpip [TCP/IPv6]"},
			{0x20609, 1004, true, 2, 17, 0xfff7, "MSAFD Tcpip [UDP/IPv6]"},
		};
		static constexpr std::array<uint8_t, 16> provider4{0xa0, 0x1a, 0x0f, 0xe7, 0x8b, 0xab, 0xcf, 0x11, 0x8c, 0xa3, 0x00, 0x80, 0x5f, 0x48, 0xa1, 0x92};
		static constexpr std::array<uint8_t, 16> provider6{0xc0, 0xb0, 0xea, 0xf9, 0xd4, 0x26, 0xd0, 0x11, 0xbb, 0xbf, 0x00, 0xaa, 0x00, 0x6c, 0x34, 0xe4};
		std::vector<int32_t> wanted;
		if (call[0])
			for (uint64_t at = call[0];; at += 4) {
				const auto value = call.load<int32_t>(at);
				if (!value || wanted.size() == 64)
					break;
				wanted.push_back(value);
			}
		const size_t entry = wide ? 628 : 372;
		std::vector<uint8_t> bytes;
		size_t count = 0;
		for (const auto &protocol : protocols) {
			if (call[0] && std::find(wanted.begin(), wanted.end(), protocol.protocol) == wanted.end())
				continue;
			std::vector<uint8_t> info(entry);
			const auto put = [&info](size_t at, uint32_t value) { std::memcpy(info.data() + at, &value, 4); };
			put(0, protocol.service_flags);
			put(16, 0x8);
			std::memcpy(info.data() + 20, (protocol.version6 ? provider6 : provider4).data(), 16);
			put(36, protocol.catalog);
			put(40, 1);
			put(72, 2);
			put(76, protocol.version6 ? 23 : 2);
			put(80, protocol.version6 ? 28 : 16);
			put(84, protocol.version6 ? 28 : 16);
			put(88, uint32_t(protocol.type));
			put(92, uint32_t(protocol.protocol));
			put(108, protocol.message_size);
			for (size_t at = 0; protocol.name[at]; ++at) {
				if (wide)
					info[116 + at * 2] = uint8_t(protocol.name[at]);
				else
					info[116 + at] = uint8_t(protocol.name[at]);
			}
			bytes.insert(bytes.end(), info.begin(), info.end());
			++count;
		}
		const uint32_t capacity = call[2] ? call.load<uint32_t>(call[2]) : 0;
		if (!call[2]) {
			call.fail(wsaefault, UINT64_MAX);
			return;
		}
		if (!call[1] || capacity < bytes.size()) {
			call.store<uint32_t>(call[2], uint32_t(bytes.size()));
			call.fail(10055, UINT64_MAX);
			return;
		}
		call.write(call[1], bytes);
		call.ret(count);
	};
	registry.add("WSAEnumProtocolsW", 3, [enumerate_protocols](ApiCall &call) { enumerate_protocols(call, true); });
	registry.add("WSAEnumProtocolsA", 3, [enumerate_protocols](ApiCall &call) { enumerate_protocols(call, false); });
	registry.add("WSAStartup", 2, [](ApiCall &call) {
		if (!call[1]) {
			call.ret(10014);
			return;
		}
		const uint16_t wanted = uint16_t(call.u32(0));
		const uint8_t major = uint8_t(wanted), minor = uint8_t(wanted >> 8);
		if (major == 0 || major > 2 || (major == 2 && minor > 2)) {
			call.ret(10092);
			return;
		}
		const bool narrow = call.pointer_width() == 4;
		std::vector<uint8_t> data(narrow ? 400 : 408);
		const uint16_t version = wanted, high = 0x0202;
		std::memcpy(data.data(), &version, 2);
		std::memcpy(data.data() + 2, &high, 2);
		const char description[] = "Raven Winsock 2.2", status[] = "Running";
		const uint16_t sockets = 0x7fff, datagram = 65467;
		if (narrow) {
			std::memcpy(data.data() + 4, description, sizeof(description));
			std::memcpy(data.data() + 261, status, sizeof(status));
			std::memcpy(data.data() + 390, &sockets, 2);
			std::memcpy(data.data() + 392, &datagram, 2);
		} else {
			std::memcpy(data.data() + 4, &sockets, 2);
			std::memcpy(data.data() + 6, &datagram, 2);
			std::memcpy(data.data() + 16, description, sizeof(description));
			std::memcpy(data.data() + 273, status, sizeof(status));
		}
		call.write(call[1], data);
		call.ret(0);
	});
	registry.add("WSACleanup", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("WSAGetLastError", 0, [](ApiCall &call) { call.ret(call.last_error()); });
	registry.add("WSASetLastError", 1, [](ApiCall &call) { call.set_last_error(call.u32(0)); });
}

}
