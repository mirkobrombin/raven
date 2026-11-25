#include "kernel_context.h"

#include "handle_channel.h"
#include "local_socket.h"

#include <array>
#include <bit>
#include <cstring>
#include <deque>
#include <map>
#include <fcntl.h>

namespace raven {

namespace {

class AlpcPort;
struct AlpcConnection;
struct AlpcReply;

struct AlpcMessage {
	std::vector<uint8_t> data;
	uint64_t process = 0, thread = 0;
	uint32_t id = 0, capacity = 0, security = 0;
	uint16_t type = 0;
	// The context the server gave the sender's connection when it accepted it, told to the connection port's receiver.
	uint64_t port_context = 0;
	std::shared_ptr<AccessToken> token;
	std::shared_ptr<AlpcConnection> request;
	std::shared_ptr<AlpcReply> reply;
};

enum class AlpcPacket : uint32_t { connect = 1, accept, reject, datagram, request, reply, cancel };

class AlpcPeer {
public:
	explicit AlpcPeer(int descriptor) : descriptor(descriptor) {}
	~AlpcPeer() { close(); }
	AlpcPeer(AlpcPeer &&other) noexcept : descriptor(std::exchange(other.descriptor, -1)), process_id(other.process_id) {}
	AlpcPeer(const AlpcPeer &) = delete;
	AlpcPeer &operator=(const AlpcPeer &) = delete;

	bool authenticate()
	{
		ucred peer{};
		socklen_t size = sizeof(peer);
		if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &size) || size != sizeof(peer) || peer.uid != ::geteuid() ||
			peer.pid <= 0 || !process_request_channel(peer.pid))
			return false;
		process_id = uint32_t(peer.pid);
		return true;
	}

	uint32_t send(AlpcPacket kind, const AlpcMessage &message) const
	{
		if (descriptor < 0)
			return 0xc0000037u;
		if (message.data.size() > maximum_payload)
			return 0xc000000du;
		std::array<uint32_t, 7> fields{
			wire_version, uint32_t(kind), message.id, uint32_t(message.thread), uint32_t(message.data.size()), message.capacity, message.security};
		iovec vectors[] = {{fields.data(), sizeof(fields)}, {const_cast<uint8_t *>(message.data.data()), message.data.size()}};
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
		msghdr header{};
		header.msg_iov = vectors;
		header.msg_iovlen = 2;
		if (message.token) {
			InheritedHandle description;
			message.token->describe(description);
			header.msg_control = control;
			header.msg_controllen = sizeof(control);
			auto *descriptor = CMSG_FIRSTHDR(&header);
			descriptor->cmsg_level = SOL_SOCKET;
			descriptor->cmsg_type = SCM_RIGHTS;
			descriptor->cmsg_len = CMSG_LEN(sizeof(int));
			std::memcpy(CMSG_DATA(descriptor), &description.descriptor, sizeof(int));
		}
		ssize_t sent;
		do sent = ::sendmsg(descriptor, &header, MSG_NOSIGNAL); while (sent < 0 && errno == EINTR);
		if (sent < 0 && (errno == EAGAIN || errno == ENOBUFS))
			return 0xc0000044u;
		return sent == ssize_t(sizeof(fields) + message.data.size()) ? 0 : 0xc0000037u;
	}

	bool receive(AlpcPacket &kind, AlpcMessage &message)
	{
		std::array<uint32_t, 7> fields{};
		ssize_t count;
		do count = ::recv(descriptor, fields.data(), sizeof(fields), MSG_PEEK); while (count < 0 && errno == EINTR);
		if (count < 0 && errno == EAGAIN)
			return false;
		if (count != ssize_t(sizeof(fields)) || fields[0] != wire_version || fields[1] < uint32_t(AlpcPacket::connect) ||
			fields[1] > uint32_t(AlpcPacket::cancel) || fields[4] > maximum_payload || fields[5] > maximum_payload) {
			close();
			return false;
		}
		message.data.resize(fields[4]);
		iovec vectors[] = {{fields.data(), sizeof(fields)}, {message.data.data(), message.data.size()}};
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
		msghdr header{};
		header.msg_iov = vectors;
		header.msg_iovlen = 2;
		header.msg_control = control;
		header.msg_controllen = sizeof(control);
		do count = ::recvmsg(descriptor, &header, MSG_CMSG_CLOEXEC); while (count < 0 && errno == EINTR);
		struct PendingToken {
			int fd = -1;
			~PendingToken() { if (fd >= 0) ::close(fd); }
		} token;
		bool invalid_control = false;
		if (count >= 0)
			for (auto *item = CMSG_FIRSTHDR(&header); item; item = CMSG_NXTHDR(&header, item)) {
				if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS || item->cmsg_len < CMSG_LEN(sizeof(int))) {
					invalid_control = true;
					continue;
				}
				const size_t descriptors = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
				for (size_t index = 0; index < descriptors; ++index) {
					int fd;
					std::memcpy(&fd, CMSG_DATA(item) + index * sizeof(int), sizeof(fd));
					if (token.fd < 0)
						token.fd = fd;
					else {
						::close(fd);
						invalid_control = true;
					}
				}
			}
		if (count != ssize_t(sizeof(fields) + message.data.size()) || (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || invalid_control) {
			close();
			return false;
		}
		kind = AlpcPacket(fields[1]);
		if (token.fd >= 0) {
			constexpr int required_seals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
			const int seals = ::fcntl(token.fd, F_GET_SEALS);
			if ((kind != AlpcPacket::connect && kind != AlpcPacket::request) || seals < 0 || (seals & required_seals) != required_seals) {
				close();
				return false;
			}
			try {
				message.token.reset(new AccessToken(std::exchange(token.fd, -1)));
			} catch (const std::runtime_error &) {
				close();
				return false;
			}
		}
		message.process = process_id;
		message.thread = fields[3];
		message.id = fields[2];
		message.capacity = fields[5];
		message.security = fields[6];
		return true;
	}

	void close()
	{
		if (descriptor >= 0)
			::close(std::exchange(descriptor, -1));
	}

	uint32_t process() const { return process_id; }
	int descriptor;

private:
	static constexpr uint32_t wire_version = 2;
	static constexpr size_t maximum_payload = UINT16_MAX - 24;
	uint32_t process_id = 0;
};

constexpr uint32_t security_granted = 1, security_dynamic = 2, security_effective = 4, security_level_shift = 3;
constexpr uint32_t security_mask = security_granted | security_dynamic | security_effective | (3 << security_level_shift);
// A request whose sender waits for the reply is delivered with LPC_CONTINUATION_REQUIRED beside its type: the receiver answers it.
constexpr uint16_t awaited_request = 1 | 0x2000;

struct AlpcConnection {
	enum class State { waiting, accepted, rejected };
	State state = State::waiting;
	bool delivered = false;
	std::unique_ptr<AlpcPeer> peer;
	uint32_t local_request_id = 0, remote_request_id = 0, remote_capacity = 0;
	uint32_t security = 0;
	uint64_t context = 0;
	std::shared_ptr<AccessToken> static_token;
	std::weak_ptr<AlpcPort> listener, client, server;
	AlpcMessage response;
	std::map<uint32_t, std::shared_ptr<AlpcReply>> outbound;
};

struct AlpcReply {
	std::weak_ptr<AlpcConnection> connection;
	size_t maximum = UINT16_MAX;
	uint32_t remote_id = 0;
	uint64_t sender_process = 0, sender_thread = 0;
	std::shared_ptr<AccessToken> token;
	bool delivered = false;
	std::optional<AlpcMessage> result;
};

std::string alpc_socket_name(std::u16string_view name)
{
	return "alpc:" + std::string(reinterpret_cast<const char *>(name.data()), name.size() * sizeof(char16_t));
}

class AlpcListenerSocket {
public:
	~AlpcListenerSocket()
	{
		stop();
		if (!path.empty())
			::unlink(path.c_str());
		if (reservation >= 0)
			::close(reservation);
	}

	uint32_t open(std::u16string_view name)
	{
		const auto address = alpc_socket_name(name);
		sockaddr_un storage;
		const auto length = local_socket_address(address, storage);
		if (length > sizeof(storage))
			return 0xc0000033u;
		const std::string socket_path = storage.sun_path;
		reservation = ::open((socket_path + ".lock").c_str(), O_CREAT | O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (reservation < 0)
			return 0xc000009au;
		// Keep the name reserved through disconnect until the last handle closes.
		if (::flock(reservation, LOCK_EX | LOCK_NB))
			return errno == EWOULDBLOCK ? 0xc0000035u : 0xc000009au;
		descriptor = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (descriptor < 0)
			return 0xc000009au;
		if (bind_local_socket(descriptor, address))
			return errno == EADDRINUSE ? 0xc0000035u : 0xc000009au;
		path = socket_path;
		return ::listen(descriptor, 64) ? 0xc000009au : 0;
	}

	void stop()
	{
		if (descriptor >= 0)
			::close(std::exchange(descriptor, -1));
	}

	int descriptor = -1;

private:
	int reservation = -1;
	std::string path;
};

class AlpcPort : public KernelObject {
public:
	explicit AlpcPort(std::u16string name = {}) : name(std::move(name)) {}
	std::u16string_view type_name() const override { return u"ALPC Port"; }
	std::u16string_view object_name() const override { return name; }
	uint32_t default_access() const override { return 0x1f0001; }
	// A port can be waited on: it is signaled while a message or a connection request waits to be received.
	int signaled(uint32_t thread) const override { (void)thread; return inbox.empty() ? 0 : 1; }
	void deliver(AlpcMessage message)
	{
		inbox.push_back(std::move(message));
		signaled_now();
	}
	bool watches_last_handle() const override { return true; }
	void last_handle_closed() override { disconnect(); listener_socket.reset(); name_closed = true; }
	void disconnect()
	{
		closed = true;
		if (listener_socket)
			listener_socket->stop();
		if (connection && connection->peer)
			connection->peer->close();
		for (const auto &remote : remote_connections)
			if (const auto link = remote.lock())
				link->peer->close();
		inbox.clear();
		requests.clear();
		replies.clear();
	}

	bool closed = false, name_closed = false;
	bool server = false;
	size_t message_limit = UINT16_MAX;
	std::vector<uint8_t> owner_sid;
	std::unique_ptr<AlpcListenerSocket> listener_socket;
	std::vector<std::weak_ptr<AlpcConnection>> remote_connections;
	std::shared_ptr<AlpcConnection> connection;
	std::deque<AlpcMessage> inbox;
	std::map<uint32_t, std::shared_ptr<AlpcConnection>> requests;
	std::map<uint32_t, std::shared_ptr<AlpcReply>> replies;

private:
	std::u16string name;
};

// An abandoned request must cease to be visible to the receiving endpoint.
struct WithdrawAlpcRequest {
	AlpcPort &port;
	uint32_t id;
	~WithdrawAlpcRequest()
	{
		port.requests.erase(id);
		port.replies.erase(id);
		std::erase_if(port.inbox, [id = id](const auto &message) { return message.id == id; });
	}
};

bool route_alpc_packet(AlpcPort &port, const std::shared_ptr<AlpcConnection> &connection, AlpcPacket kind,
	AlpcMessage message, uint64_t &next_id)
{
	if (kind == AlpcPacket::reply) {
		const auto pending = connection->outbound.find(message.id);
		if (pending != connection->outbound.end() && !pending->second->result) {
			message.type = 2;
			pending->second->result = std::move(message);
		}
		return true;
	}
	if (kind == AlpcPacket::cancel) {
		if (!message.data.empty())
			return false;
		const auto pending = std::find_if(port.replies.begin(), port.replies.end(), [&](const auto &entry) {
			return entry.second->remote_id == message.id && entry.second->connection.lock() == connection;
		});
		if (pending != port.replies.end()) {
			const WithdrawAlpcRequest withdraw{port, pending->first};
		}
		return true;
	}
	if ((kind != AlpcPacket::datagram && kind != AlpcPacket::request) || port.inbox.size() >= 256 || next_id > UINT32_MAX)
		return false;
	const auto remote_id = message.id;
	message.id = uint32_t(next_id++);
	message.type = kind == AlpcPacket::request ? awaited_request : 3;
	if (kind == AlpcPacket::request) {
		if (port.replies.size() >= 256 || std::any_of(port.replies.begin(), port.replies.end(), [&](const auto &entry) {
			return entry.second->remote_id == remote_id && entry.second->connection.lock() == connection;
		}))
			return false;
		const auto reply = std::make_shared<AlpcReply>();
		reply->remote_id = remote_id;
		reply->connection = connection;
		reply->maximum = port.message_limit;
		reply->sender_process = message.process;
		reply->sender_thread = message.thread;
		reply->token = std::move(message.token);
		message.reply = reply;
		port.replies.emplace(message.id, reply);
	}
	if (connection->listener.lock().get() == &port)
		message.port_context = connection->context;
	port.deliver(std::move(message));
	return true;
}

void watch_alpc_peer(Reactor &reactor, const std::shared_ptr<AlpcConnection> &connection,
	const std::shared_ptr<AlpcPort> &port, const std::shared_ptr<uint64_t> &next_id, size_t header, bool listener)
{
	reactor.add([connection, endpoint = std::weak_ptr(port), next_id, header, listener] {
		const auto port = endpoint.lock();
		auto &peer = *connection->peer;
		if (port && !port->closed && peer.descriptor >= 0) {
			try {
				AlpcPacket kind;
				AlpcMessage message;
				if (peer.receive(kind, message)) {
					const auto grant = message.security;
					const bool invalid_grant = (grant & ~security_mask) || (grant && !(grant & security_granted)) ||
						(kind == AlpcPacket::connect && bool(message.token) != bool(grant & security_granted));
					if (!message.id || invalid_grant || header + message.data.size() > port->message_limit) {
						peer.close();
					} else if (connection->state == AlpcConnection::State::accepted) {
						const bool needs_token = listener && kind == AlpcPacket::request && (connection->security & security_dynamic);
						if (message.capacity || message.security || bool(message.token) != needs_token ||
							!route_alpc_packet(*port, connection, kind, std::move(message), *next_id))
							peer.close();
					} else if (connection->state != AlpcConnection::State::waiting) {
						peer.close();
					} else if (listener && kind == AlpcPacket::connect && !connection->local_request_id &&
						port->requests.size() < 256 && port->inbox.size() < 256 && *next_id <= UINT32_MAX) {
						connection->security = grant;
						connection->static_token = std::move(message.token);
						connection->local_request_id = uint32_t((*next_id)++);
						connection->remote_request_id = message.id;
						connection->remote_capacity = message.capacity;
						message.id = connection->local_request_id;
						message.type = 10;
						message.request = connection;
						port->requests.emplace(message.id, connection);
						port->deliver(std::move(message));
					} else if (!listener && !message.security && message.id == connection->local_request_id &&
						(kind == AlpcPacket::accept || kind == AlpcPacket::reject)) {
						connection->state = kind == AlpcPacket::accept ? AlpcConnection::State::accepted : AlpcConnection::State::rejected;
						connection->remote_capacity = message.capacity;
						connection->response = std::move(message);
					} else {
						peer.close();
					}
				}
			} catch (const std::bad_alloc &) {
				peer.close();
			}
			if (peer.descriptor >= 0)
				return false;
		}
		peer.close();
		if (port) {
			if (listener)
				port->requests.erase(connection->local_request_id);
			std::erase_if(port->inbox, [&](const auto &message) {
				return message.request == connection || (message.reply && message.reply->connection.lock() == connection);
			});
			std::erase_if(port->replies, [&](const auto &entry) { return entry.second->connection.lock() == connection; });
		}
		return true;
	}, connection->peer->descriptor, POLLIN);
}

void watch_alpc_listener(Reactor &reactor, const std::shared_ptr<AlpcPort> &port,
	const std::shared_ptr<uint64_t> &next_id, size_t header)
{
	reactor.add([&reactor, weak = std::weak_ptr(port), next_id, header] {
		const auto listener = weak.lock();
		if (!listener || listener->closed)
			return true;
		for (unsigned count = 0; count < 16; ++count) {
			AlpcPeer incoming(::accept4(listener->listener_socket->descriptor, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
			if (incoming.descriptor < 0)
				break;
			std::erase_if(listener->remote_connections, [](const auto &item) { return item.expired(); });
			if (listener->remote_connections.size() >= 256)
				continue;
			try {
				if (!incoming.authenticate())
					continue;
				const auto connection = std::make_shared<AlpcConnection>();
				connection->peer = std::make_unique<AlpcPeer>(std::move(incoming));
				connection->listener = listener;
				listener->remote_connections.push_back(connection);
				watch_alpc_peer(reactor, connection, listener, next_id, header, true);
			} catch (const std::bad_alloc &) {
				break;
			}
		}
		return false;
	}, port->listener_socket->descriptor, POLLIN);
}

constexpr uint32_t invalid_parameter = 0xc000000du, invalid_handle = 0xc0000008u, not_supported = 0xc00000bbu,
	port_disconnected = 0xc0000037u, buffer_too_small = 0xc0000023u, timeout = 0x102;

std::u16string alpc_name_key(std::u16string name)
{
	for (auto &character : name)
		if (character >= u'A' && character <= u'Z')
			character += u'a' - u'A';
	return name;
}

std::optional<std::u16string> alpc_name(ApiCall &call, uint64_t counted)
{
	if (!counted)
		return std::nullopt;
	const auto size = call.load<uint16_t>(counted);
	if (!size || size % 2 || size > 65532 || size > call.load<uint16_t>(counted + 2))
		return std::nullopt;
	const auto bytes = call.read(call.load_pointer(counted + call.pointer_width()), size);
	std::u16string name(size / 2, 0);
	std::memcpy(name.data(), bytes.data(), size);
	if (!name.starts_with(u'\\') || name.find(u'\0') != name.npos || name.ends_with(u'\\'))
		return std::nullopt;
	return name;
}

size_t header_size(const ApiCall &call)
{
	return call.pointer_width() == 8 ? 40 : 24;
}

std::optional<size_t> port_message_limit(ApiCall &call, uint64_t attributes)
{
	if (!attributes)
		return UINT16_MAX;
	if (call.load<uint32_t>(attributes) & 0x00100000u)
		return std::nullopt;
	const auto maximum = call.load_pointer(attributes + 16);
	if (maximum && (maximum < header_size(call) || maximum > UINT16_MAX))
		return std::nullopt;
	return maximum ? size_t(maximum) : UINT16_MAX;
}

std::optional<AlpcMessage> read_message(ApiCall &call, uint64_t address, size_t capacity = UINT16_MAX)
{
	const size_t header = header_size(call);
	const auto data = call.load<uint16_t>(address), total = call.load<uint16_t>(address + 2);
	if (total < header + data || total > capacity || call.load<uint16_t>(address + 6))
		return std::nullopt;
	AlpcMessage message;
	message.id = call.load<uint32_t>(address + 8 + 2 * call.pointer_width());
	message.data = call.read(address + header, data);
	return message;
}

uint32_t write_message(ApiCall &call, uint64_t address, uint64_t length, const AlpcMessage &message)
{
	const size_t header = header_size(call), width = call.pointer_width(), needed = header + message.data.size();
	const uint64_t capacity = call.load_pointer(length);
	call.process.validate_write_memory(length, width);
	if (capacity < needed) {
		call.store_pointer(length, needed);
		return buffer_too_small;
	}
	std::vector<uint8_t> bytes(needed, 0);
	const auto put = [&](size_t offset, uint64_t value, size_t count) { std::memcpy(bytes.data() + offset, &value, count); };
	put(0, message.data.size(), 2);
	put(2, needed, 2);
	put(4, message.type, 2);
	put(8, message.process, width);
	put(8 + width, message.thread, width);
	put(8 + 2 * width, message.id, 4);
	if (!message.data.empty())
		std::memcpy(bytes.data() + header, message.data.data(), message.data.size());
	call.write(address, bytes);
	call.store_pointer(length, needed);
	return 0;
}

std::optional<std::chrono::steady_clock::time_point> deadline(ApiCall &call, uint64_t address)
{
	if (!address)
		return std::nullopt;
	const int64_t interval = call.load<int64_t>(address);
	const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch());
	const uint64_t ticks = interval < 0 ? uint64_t(-(interval + 1)) + 1 : uint64_t(std::max<int64_t>(0, interval - (now.count() / 100 + int64_t(filetime_epoch_offset))));
	if (ticks > 100ULL * 365 * 24 * 3600 * 10000000ULL)
		return std::nullopt;
	return std::chrono::steady_clock::now() + std::chrono::nanoseconds(ticks * 100);
}

template<typename Handler> auto alpc_status(Handler handler)
{
	return [handler = std::move(handler)](ApiCall &call) {
		try { call.ret(handler(call)); }
		catch (const std::bad_alloc &) { call.ret(0xc000009au); }
		catch (const ApiFault &) { call.ret(0xc0000005u); }
		catch (const std::runtime_error &) { call.ret(0xc0000005u); }
	};
}

constexpr uint32_t attribute_mask = 0xfe000000u;

size_t attribute_size(uint32_t allocated, size_t width, uint32_t first_excluded = 0)
{
	// The slots follow the descending flag order: security, view, context, handle, token, direct, work on behalf.
	const std::array<size_t, 7> sizes{3 * width, 4 * width, width == 8 ? 32u : 20u, width == 8 ? 24u : 16u, 24, width, 8};
	size_t size = 8;
	for (size_t index = 0; index < sizes.size(); ++index) {
		const uint32_t bit = 0x80000000u >> index;
		if (bit <= first_excluded)
			break;
		if (allocated & bit)
			size += sizes[index];
	}
	return size;
}

// A received message brings none of the attributes a sender can attach here, only its context: the port context of the sender's
// connection and the message's id.
void write_received_attributes(ApiCall &call, uint64_t address, const AlpcMessage &message)
{
	constexpr uint32_t context_flag = 0x20000000u;
	const size_t width = call.pointer_width();
	const uint32_t valid = call.load<uint32_t>(address) & context_flag;
	call.store<uint32_t>(address + 4, valid);
	if (!valid)
		return;
	const uint64_t slot = address + attribute_size(call.load<uint32_t>(address), width, context_flag);
	call.store_pointer(slot, message.port_context);
	call.store_pointer(slot + width, 0);
	call.store(slot + 2 * width, std::array<uint32_t, 3>{0, message.id, 0});
}

}

void register_alpc_api(ApiRegistry &registry, KernelContext &context)
{
	registry.add("AlpcGetHeaderSize", 1, [](ApiCall &call) {
		call.ret(attribute_size(call.u32(0), call.pointer_width()));
	});
	registry.add("AlpcInitializeMessageAttribute", 4, [](ApiCall &call) {
		const auto flags = call.u32(0);
		const auto needed = attribute_size(flags, call.pointer_width());
		call.store_pointer(call[3], needed);
		if (call[2] < needed) {
			call.ret(0xc0000023u);
			return;
		}
		if (call[1])
			call.store(call[1], std::array<uint32_t, 2>{flags, 0});
		call.ret(0);
	});
	registry.add("AlpcGetMessageAttribute", 2, [](ApiCall &call) {
		const auto flag = call.u32(1);
		if (!call[0] || !(flag & attribute_mask) || !std::has_single_bit(flag))
			return;
		const auto allocated = call.load<uint32_t>(call[0]);
		if (allocated & flag)
			call.ret(call[0] + attribute_size(allocated, call.pointer_width(), flag));
	});
	const auto names = std::make_shared<std::map<std::u16string, std::weak_ptr<AlpcPort>>>();
	const auto next_id = std::make_shared<uint64_t>(1);
	registry.add("NtAlpcCreatePort", 3, [&context, &objects = context.objects, names, next_id](ApiCall &call) {
		try {
			const size_t width = call.pointer_width();
			call.process.validate_write_memory(call[0], width);
			const auto limit = port_message_limit(call, call[2]);
			if (!limit) {
				call.ret(invalid_parameter);
				return;
			}
			uint32_t flags = 0;
			std::u16string name;
			if (const auto attributes = call[1]) {
				if (call.load<uint32_t>(attributes) != 6 * width) {
					call.ret(0xc000000du);
					return;
				}
				if (call.load_pointer(attributes + width) || call.load_pointer(attributes + 4 * width)) {
					call.ret(0xc00000bbu);
					return;
				}
				flags = call.load<uint32_t>(attributes + 3 * width);
				if (const auto counted = call.load_pointer(attributes + 2 * width)) {
					auto parsed = alpc_name(call, counted);
					if (!parsed) {
						call.ret(0xc0000033u);
						return;
					}
					name = std::move(*parsed);
				}
			}
			auto key = alpc_name_key(name);
			std::erase_if(*names, [](const auto &entry) {
				const auto port = entry.second.lock();
				return !port || port->name_closed;
			});
			if (!key.empty() && names->contains(key)) {
				call.ret(0xc0000035u);
				return;
			}
			const auto port = std::make_shared<AlpcPort>(std::move(name));
			port->message_limit = *limit;
			if (!key.empty()) {
				port->listener_socket = std::make_unique<AlpcListenerSocket>();
				if (const auto status = port->listener_socket->open(key)) {
					call.ret(status);
					return;
				}
				port->owner_sid = user_sid();
				names->emplace(std::move(key), port);
			}
			const auto made = objects.add_object(port);
			if (!made.status) {
				objects.set_inherit(made.value, (flags & 2) != 0);
				try {
					call.store_pointer(call[0], made.value);
					if (port->listener_socket)
						watch_alpc_listener(*context.reactor, port, next_id, header_size(call));
				} catch (...) {
					objects.close(made.value);
					throw;
				}
			}
			call.ret(made.status);
		} catch (const std::bad_alloc &) {
			call.ret(0xc000009au);
		} catch (const ApiFault &) {
			call.ret(0xc0000005u);
		} catch (const std::runtime_error &) {
			call.ret(0xc0000005u);
		}
	});
	const auto disconnected = [](const AlpcPort &port) {
		if (port.closed)
			return true;
		if (!port.connection)
			return false;
		if (port.connection->peer)
			return port.connection->peer->descriptor < 0;
		const auto listener = port.connection->listener.lock();
		const auto peer = (port.server ? port.connection->client : port.connection->server).lock();
		return !listener || listener->closed || !peer || peer->closed;
	};
	registry.add("NtAlpcConnectPort", 11, alpc_status([&context, names, next_id](ApiCall &call) -> uint32_t {
		const auto name = alpc_name(call, call[1]);
		if (!name)
			return 0xc0000033u;
		const auto key = alpc_name_key(*name);
		const auto named = names->find(key);
		const auto listener = named == names->end() ? nullptr : named->second.lock();
		if (listener && listener->closed)
			return 0xc0000034u;
		if (call.u32(4) & ~0x20000u)
			return not_supported;
		const size_t width = call.pointer_width();
		// The client port is nameless: its object attributes may only carry flags.
		if (const auto attributes = call[2]) {
			if (call.load<uint32_t>(attributes) != 6 * width)
				return invalid_parameter;
			if (call.load_pointer(attributes + width) || call.load_pointer(attributes + 2 * width) || call.load_pointer(attributes + 4 * width))
				return not_supported;
		}
		// Of what a client can send with its connection message, the security attribute is taken: its quality of service grants the
		// server what the port attributes' would. One without a quality of service (CoreMessaging sends it so, with a context handle of
		// -2) leaves the port attributes' in force; there are no security contexts here for its handle to name.
		constexpr uint32_t security_flag = 0x80000000u;
		uint64_t sent_quality = 0;
		if (const auto sent = call[8]) {
			const uint32_t allocated = call.load<uint32_t>(sent), valid = call.load<uint32_t>(sent + 4);
			if ((valid & ~security_flag) || (valid & ~allocated))
				return not_supported;
			if (valid & security_flag)
				sent_quality = call.load_pointer(sent + attribute_size(allocated, width, security_flag) + width);
		}
		if (call[9])
			call.process.validate_write_memory(call[9], 8);
		std::vector<uint8_t> required_sid;
		if (const auto required = call[5]) {
			const auto sid_header = call.read(required, 2);
			if (sid_header[0] != 1 || sid_header[1] > 15)
				return 0xc0000078u;
			const size_t length = 8 + 4 * sid_header[1];
			required_sid = call.read(required, length);
			if (listener && required_sid != listener->owner_sid)
				return 0xc00002a0u;
		}
		if ((call[6] != 0) != (call[7] != 0))
			return invalid_parameter;
		call.process.validate_write_memory(call[0], call.pointer_width());
		const auto wake = deadline(call, call[10]);
		auto request = call[6] ? read_message(call, call[6], call.load_pointer(call[7])) : std::optional<AlpcMessage>(AlpcMessage{});
		const auto limit = port_message_limit(call, call[3]);
		if (!request || !limit)
			return invalid_parameter;
		if (*next_id > UINT32_MAX)
			return 0xc0000044u;
		const auto client = std::make_shared<AlpcPort>();
		client->message_limit = *limit;
		const auto connection = std::make_shared<AlpcConnection>();
		client->connection = connection;
		uint64_t quality = sent_quality;
		if (const auto attrs = call[3]; !quality && attrs && (call.load<uint32_t>(attrs) & 0x10000u))
			quality = attrs + 4;
		if (quality) {
			const uint32_t level = call.load<uint32_t>(quality + 4);
			const auto tracking = call.load<uint8_t>(quality + 8), effective = call.load<uint8_t>(quality + 9);
			if (call.load<uint32_t>(quality) != 12 || level > 3 || tracking > 1 || effective > 1)
				return invalid_parameter;
			connection->security = security_granted | (level << security_level_shift);
			if (effective)
				connection->security |= security_effective;
			if (tracking)
				connection->security |= security_dynamic;
			// The token the client has as it asks: what a static grant impersonates, and what the server's receiver impersonates from
			// the connection request itself.
			const auto &current = context.threads.current().impersonation;
			connection->static_token = std::make_shared<AccessToken>(*(current ? current : context.process_token));
		}
		struct UncommittedConnection {
			AlpcConnection &connection;
			bool committed = false;
			~UncommittedConnection() { if (!committed && connection.peer) connection.peer->close(); }
		} pending{*connection};
		connection->client = client;
		request->process = call.load_pointer(context.environment.teb() + 8 * call.pointer_width());
		request->thread = context.threads.current_id();
		request->id = uint32_t((*next_id)++);
		if (listener) {
			if (header_size(call) + request->data.size() > listener->message_limit)
				return invalid_parameter;
			if (listener->requests.size() >= 256 || listener->inbox.size() >= 256)
				return 0xc0000044u;
			connection->listener = listener;
			WithdrawAlpcRequest withdraw{*listener, request->id};
			request->type = 10;
			request->request = connection;
			listener->requests.emplace(request->id, connection);
			listener->deliver(std::move(*request));
			if (!context.threads.block([&] { return listener->closed || connection->state != AlpcConnection::State::waiting; }, wake))
				return timeout;
			if (listener->closed)
				return port_disconnected;
		} else {
			sockaddr_un address;
			const auto length = local_socket_address(alpc_socket_name(key), address);
			if (length > sizeof(address))
				return 0xc0000033u;
			AlpcPeer peer(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
			if (peer.descriptor < 0)
				return 0xc000009au;
			if (::connect(peer.descriptor, reinterpret_cast<const sockaddr *>(&address), length))
				return errno == EAGAIN ? 0xc0000044u : 0xc0000034u;
			if (!peer.authenticate())
				return 0xc0000022u;
			// The verified runtime has the same account SID as other processes of this prefix.
			if (!required_sid.empty() && required_sid != user_sid())
				return 0xc00002a0u;
			connection->peer = std::make_unique<AlpcPeer>(std::move(peer));
			connection->local_request_id = request->id;
			request->capacity = uint32_t(client->message_limit - header_size(call));
			request->security = connection->security;
			request->token = connection->static_token;
			if (const auto status = connection->peer->send(AlpcPacket::connect, *request))
				return status;
			watch_alpc_peer(*context.reactor, connection, client, next_id, header_size(call), false);
			if (!context.threads.block([&] {
				return connection->peer->descriptor < 0 || connection->state != AlpcConnection::State::waiting;
			}, wake))
				return timeout;
			if (connection->peer->descriptor < 0 && connection->state == AlpcConnection::State::waiting)
				return port_disconnected;
		}
		if (connection->state != AlpcConnection::State::accepted)
			return 0xc0000041u;
		if (call[6])
			if (const auto status = write_message(call, call[6], call[7], connection->response))
				return status;
		if (call[9])
			write_received_attributes(call, call[9], connection->response);
		const auto made = context.objects.add_object(client);
		if (made.status)
			return made.status;
		try { call.store_pointer(call[0], made.value); }
		catch (...) { context.objects.close(made.value); throw; }
		pending.committed = true;
		return 0;
	}));
	registry.add("NtAlpcAcceptConnectPort", 9, alpc_status([&context](ApiCall &call) -> uint32_t {
		const auto listener = context.objects.object_as<AlpcPort>(call[1]);
		if (!listener)
			return invalid_handle;
		if (listener->closed)
			return port_disconnected;
		if (call.u32(2) || call[3] || call[7])
			return not_supported;
		if (!(context.objects.access_of(call[1]) & 1))
			return 0xc0000022u;
		if (!call[6])
			return invalid_parameter;
		auto response = read_message(call, call[6]);
		if (!response)
			return invalid_parameter;
		const auto pending = listener->requests.find(response->id);
		if (pending == listener->requests.end() || !pending->second->delivered || pending->second->state != AlpcConnection::State::waiting)
			return 0xc000021fu;
		const auto connection = pending->second;
		if (connection->peer && connection->peer->descriptor < 0)
			return port_disconnected;
		if (!call.u32(8)) {
			if (connection->peer) {
				response->id = connection->remote_request_id;
				response->data.clear();
				if (const auto status = connection->peer->send(AlpcPacket::reject, *response))
					return status;
				connection->peer->close();
			}
			connection->state = AlpcConnection::State::rejected;
			return 0;
		}
		const auto limit = port_message_limit(call, call[4]);
		if (!limit || (connection->peer && response->data.size() > connection->remote_capacity))
			return invalid_parameter;
		call.process.validate_write_memory(call[0], call.pointer_width());
		const auto server = std::make_shared<AlpcPort>();
		server->message_limit = *limit;
		server->server = true;
		const auto made = context.objects.add_object(server);
		if (made.status)
			return made.status;
		if (connection->peer) {
			response->id = connection->remote_request_id;
			response->thread = context.threads.current_id();
			response->capacity = uint32_t(std::min(listener->message_limit, server->message_limit) - header_size(call));
			if (const auto status = connection->peer->send(AlpcPacket::accept, *response)) {
				context.objects.close(made.value);
				return status;
			}
			listener->requests.erase(connection->local_request_id);
		}
		server->connection = connection;
		connection->server = server;
		connection->context = call[5];
		connection->response = std::move(*response);
		connection->state = AlpcConnection::State::accepted;
		try { call.store_pointer(call[0], made.value); }
		catch (...) { context.objects.close(made.value); throw; }
		return 0;
	}));
	registry.add("NtAlpcSendWaitReceivePort", 8, alpc_status([&context, next_id, disconnected](ApiCall &call) -> uint32_t {
		const auto port = context.objects.object_as<AlpcPort>(call[0]);
		if (!port)
			return invalid_handle;
		constexpr uint32_t reply_flag = 1, release_flag = 0x10000, sync_flag = 0x20000;
		const uint32_t flags = call.u32(1);
		// Releasing a message frees what the kernel keeps for it once received (views, handles, attributes), with the attributes the
		// receiver was given; none is kept here, so a release has nothing to free and a reply still due stays due. CoreMessaging
		// releases a message it built itself, of id 0, and one it received with its attributes.
		if (flags == release_flag) {
			if (!call[2] || call[4] || call[6])
				return invalid_parameter;
			if (const auto given = call[3]; given && (call.load<uint32_t>(given + 4) & ~call.load<uint32_t>(given)))
				return invalid_parameter;
			if (!(context.objects.access_of(call[0]) & 1))
				return 0xc0000022u;
			return read_message(call, call[2]) ? 0 : invalid_parameter;
		}
		if (flags & ~(reply_flag | sync_flag))
			return not_supported;
		// Of the attributes a sender names as valid, a context and a work-on-behalf ticket are taken and go no further (CoreMessaging
		// sends both with each request): the receiver's context comes from its connection, and the ticket's contents are not public.
		if (const auto sent = call[3]) {
			constexpr uint32_t context_flag = 0x20000000u, work_on_behalf_flag = 0x02000000u;
			const uint32_t allocated = call.load<uint32_t>(sent), valid = call.load<uint32_t>(sent + 4);
			if (valid & ~allocated)
				return invalid_parameter;
			if (valid & ~(context_flag | work_on_behalf_flag))
				return not_supported;
		}
		if (flags && (!call[2] || ((flags & sync_flag) && (!call[4] || (flags & reply_flag)))))
			return invalid_parameter;
		if (!(context.objects.access_of(call[0]) & 1))
			return 0xc0000022u;
		if ((!call[2] && !call[4]) || ((call[4] != 0) != (call[5] != 0)))
			return invalid_parameter;
		if (disconnected(*port))
			return port_disconnected;
		const auto wake = deadline(call, call[7]);
		if (call[2]) {
			auto message = read_message(call, call[2]);
			if (!message)
				return invalid_parameter;
			message->process = call.load_pointer(context.environment.teb() + 8 * call.pointer_width());
			message->thread = context.threads.current_id();
			if (flags & reply_flag) {
				const auto receiver = port->server ? port->connection->listener.lock() : port;
				if (!receiver)
					return port_disconnected;
				const auto pending = receiver->replies.find(message->id);
				if (pending == receiver->replies.end() || !pending->second->delivered || pending->second->result ||
					(port->connection && pending->second->connection.lock() != port->connection))
					return 0xc000021fu;
				if (header_size(call) + message->data.size() > pending->second->maximum)
					return invalid_parameter;
				if (pending->second->remote_id) {
					const auto link = pending->second->connection.lock();
					if (!link || !link->peer)
						return port_disconnected;
					if (message->data.size() > link->remote_capacity)
						return invalid_parameter;
					message->id = pending->second->remote_id;
					if (const auto status = link->peer->send(AlpcPacket::reply, *message))
						return status;
					receiver->replies.erase(pending);
				} else {
					message->type = 2;
					pending->second->result = std::move(*message);
				}
			} else if (port->connection && port->connection->peer) {
				const auto link = port->connection;
				if (header_size(call) + message->data.size() > port->message_limit || message->data.size() > link->remote_capacity)
					return invalid_parameter;
				if (*next_id > UINT32_MAX)
					return 0xc0000044u;
				message->id = uint32_t((*next_id)++);
				if (flags & sync_flag) {
					call.process.validate_write_memory(call[5], call.pointer_width());
					if (link->outbound.size() >= 256)
						return 0xc0000044u;
					const auto reply = std::make_shared<AlpcReply>();
					if (!port->server && (link->security & security_dynamic)) {
						const auto &current = context.threads.current().impersonation;
						message->token = current ? current : context.process_token;
					}
					link->outbound.emplace(message->id, reply);
					struct Pending {
						AlpcConnection &connection;
						uint32_t id;
						~Pending() { connection.outbound.erase(id); }
					} pending{*link, message->id};
					if (const auto status = link->peer->send(AlpcPacket::request, *message))
						return status;
					if (!context.threads.block([&] { return reply->result || disconnected(*port); }, wake)) {
						message->data.clear();
						message->token.reset();
						if (link->peer->send(AlpcPacket::cancel, *message))
							link->peer->close();
						return timeout;
					}
					return reply->result ? write_message(call, call[4], call[5], *reply->result) : port_disconnected;
				}
				if (const auto status = link->peer->send(AlpcPacket::datagram, *message))
					return status;
			} else {
				if (!port->connection)
					return port_disconnected;
				const auto destination = (port->server ? port->connection->client : port->connection->listener).lock();
				if (!destination)
					return port_disconnected;
				if (header_size(call) + message->data.size() > std::min(port->message_limit, destination->message_limit))
					return invalid_parameter;
				if (destination->inbox.size() >= 256 || *next_id > UINT32_MAX)
					return 0xc0000044u;
				message->id = uint32_t((*next_id)++);
				if (call[4]) {
					call.process.validate_write_memory(call[5], call.pointer_width());
					if (destination->replies.size() >= 256)
						return 0xc0000044u;
					const auto reply = std::make_shared<AlpcReply>();
					reply->connection = port->connection;
					reply->maximum = std::min(port->message_limit, destination->message_limit);
					reply->sender_process = message->process;
					reply->sender_thread = message->thread;
					if (!port->server && (port->connection->security & security_dynamic)) {
						const auto &current = context.threads.current().impersonation;
						reply->token = current ? current : context.process_token;
					}
					message->reply = reply;
					message->type = awaited_request;
					if (!port->server)
						message->port_context = port->connection->context;
					WithdrawAlpcRequest withdraw{*destination, message->id};
					destination->replies.emplace(message->id, reply);
					destination->deliver(std::move(*message));
					if (!context.threads.block([&] { return reply->result || disconnected(*port); }, wake))
						return timeout;
					return reply->result ? write_message(call, call[4], call[5], *reply->result) : port_disconnected;
				}
				message->type = 3;
				if (!port->server)
					message->port_context = port->connection->context;
				destination->deliver(std::move(*message));
			}
		}
		if (!call[4])
			return 0;
		if (!context.threads.block([&] { return !port->inbox.empty() || disconnected(*port); }, wake))
			return timeout;
		if (port->inbox.empty())
			return port_disconnected;
		const auto &message = port->inbox.front();
		if (call[6])
			call.process.validate_write_memory(call[6], 8);
		if (const auto status = write_message(call, call[4], call[5], message))
			return status;
		if (call[6])
			write_received_attributes(call, call[6], message);
		if (message.request)
			message.request->delivered = true;
		if (message.reply)
			message.reply->delivered = true;
		port->inbox.pop_front();
		return 0;
	}));
	registry.add("NtAlpcImpersonateClientOfPort", 3, alpc_status([&context, disconnected](ApiCall &call) -> uint32_t {
		const auto port = context.objects.object_as<AlpcPort>(call[0]);
		if (!port)
			return invalid_handle;
		if (!(context.objects.access_of(call[0]) & 1))
			return 0xc0000022u;
		const uint32_t flags = call.u32(2);
		const auto width = call.pointer_width();
		uint32_t grant = 0;
		std::shared_ptr<AccessToken> source;
		if (!port->connection && !port->server) {
			// On the connection port, the receiver impersonates a client that asks to connect, named by its request, before it
			// accepts it: servers decide so by who asks.
			if (port->closed)
				return port_disconnected;
			if (!call[1] || (flags & ~0xfu))
				return invalid_parameter;
			if (flags & 1)
				return not_supported;
			const auto pending = port->requests.find(call.load<uint32_t>(call[1] + 8 + 2 * width));
			if (pending == port->requests.end() || !pending->second->delivered || pending->second->state != AlpcConnection::State::waiting)
				return 0xc000021fu;
			grant = pending->second->security;
			source = pending->second->static_token;
		} else {
			if (!port->server || !port->connection || disconnected(*port))
				return port_disconnected;
			if (!call[1] || (flags & ~0xfu))
				return invalid_parameter;
			if (flags & 1)
				return not_supported;
			const auto listener = port->connection->listener.lock();
			if (!listener || listener->closed)
				return port_disconnected;
			const auto id = call.load<uint32_t>(call[1] + 8 + 2 * width);
			const auto pending = listener->replies.find(id);
			if (pending == listener->replies.end() || !pending->second->delivered || pending->second->result ||
				pending->second->connection.lock() != port->connection ||
				call.load_pointer(call[1] + 8) != pending->second->sender_process ||
				call.load_pointer(call[1] + 8 + width) != pending->second->sender_thread)
				return 0xc000021fu;
			grant = port->connection->security;
			source = grant & security_dynamic ? pending->second->token : port->connection->static_token;
		}
		if (!(grant & security_granted) || !source)
			return 0xc0000022u;
		auto token = std::make_shared<AccessToken>(*source);
		const auto identity = token->data();
		uint32_t ceiling = grant >> security_level_shift;
		if (identity.type == 2)
			ceiling = std::min(ceiling, identity.impersonation_level);
		const uint32_t level = flags >> 2 ? flags >> 2 : ceiling;
		if (!level)
			return not_supported;
		if (level > ceiling || ((flags & 2) && level < 2))
			return 0xc00000a5u;
		if (grant & security_effective) {
			std::vector<uint64_t> disabled;
			for (const auto &privilege : identity.privileges)
				if (!(privilege.attributes & 2))
					disabled.push_back(privilege.luid);
			token->restrict_privileges(false, disabled);
		}
		token->set_type(2);
		token->set_impersonation_level(level);
		context.threads.current().impersonation = std::move(token);
		return 0;
	}));
	registry.add("NtAlpcQueryInformation", 5, alpc_status([&objects = context.objects, disconnected](ApiCall &call) -> uint32_t {
		const auto port = objects.object_as<AlpcPort>(call[0]);
		if (!port)
			return invalid_handle;
		constexpr uint32_t server_session_information = 12;
		if (call.u32(1) != server_session_information)
			return 0xc0000003u;
		if (!(objects.access_of(call[0]) & 1))
			return 0xc0000022u;
		if (!port->connection || port->server || disconnected(*port))
			return invalid_parameter;
		const auto &peer = port->connection->peer;
		const std::array<uint32_t, 2> info{native_session_id, peer ? peer->process() : uint32_t(::getpid())};
		if (call[4])
			call.process.validate_write_memory(call[4], sizeof(uint32_t));
		if (call.u32(3) < sizeof(info)) {
			if (call[4])
				call.store<uint32_t>(call[4], sizeof(info));
			return 0xc0000004u;
		}
		call.store(call[2], info);
		if (call[4])
			call.store<uint32_t>(call[4], sizeof(info));
		return 0;
	}));
	registry.add("NtAlpcDisconnectPort", 2, [&objects = context.objects](ApiCall &call) {
		const auto port = objects.object_as<AlpcPort>(call[0]);
		if (!port)
			call.ret(invalid_handle);
		else if (call.u32(1))
			call.ret(invalid_parameter);
		else if (!(objects.access_of(call[0]) & 1))
			call.ret(0xc0000022u);
		else {
			port->disconnect();
			call.ret(0);
		}
	});
}

}
