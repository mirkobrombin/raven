#include "kernel_context.h"

#include <array>
#include <bit>
#include <cstring>
#include <deque>
#include <map>

namespace raven {

namespace {

class AlpcPort;
struct AlpcConnection;
struct AlpcReply;

struct AlpcMessage {
	std::vector<uint8_t> data;
	uint64_t process = 0, thread = 0;
	uint32_t id = 0;
	uint16_t type = 0;
	std::shared_ptr<AlpcConnection> request;
	std::shared_ptr<AlpcReply> reply;
};

struct AlpcConnection {
	enum class State { waiting, accepted, rejected };
	State state = State::waiting;
	bool delivered = false;
	std::weak_ptr<AlpcPort> listener, client, server;
	AlpcMessage response;
};

struct AlpcReply {
	std::weak_ptr<AlpcConnection> connection;
	size_t maximum = UINT16_MAX;
	bool delivered = false;
	std::optional<AlpcMessage> result;
};

class AlpcPort : public KernelObject {
public:
	explicit AlpcPort(std::u16string name = {}) : name(std::move(name)) {}
	std::u16string_view type_name() const override { return u"ALPC Port"; }
	std::u16string_view object_name() const override { return name; }
	uint32_t default_access() const override { return 0x1f0001; }
	bool watches_last_handle() const override { return true; }
	void last_handle_closed() override { disconnect(); name_closed = true; }
	void disconnect() { closed = true; inbox.clear(); requests.clear(); replies.clear(); }

	bool closed = false, name_closed = false;
	bool server = false;
	size_t message_limit = UINT16_MAX;
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
	registry.add("NtAlpcCreatePort", 3, [&objects = context.objects, names](ApiCall &call) {
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
			if (!key.empty())
				names->emplace(std::move(key), port);
			const auto made = objects.add_object(port);
			if (!made.status) {
				objects.set_inherit(made.value, (flags & 2) != 0);
				try {
					call.store_pointer(call[0], made.value);
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
	auto next_id = std::make_shared<uint64_t>(1);
	const auto disconnected = [](const AlpcPort &port) {
		if (port.closed)
			return true;
		if (!port.connection)
			return false;
		const auto listener = port.connection->listener.lock();
		const auto peer = (port.server ? port.connection->client : port.connection->server).lock();
		return !listener || listener->closed || !peer || peer->closed;
	};
	registry.add("NtAlpcConnectPort", 11, alpc_status([&context, names, next_id](ApiCall &call) -> uint32_t {
		const auto name = alpc_name(call, call[1]);
		if (!name)
			return 0xc0000033u;
		const auto named = names->find(alpc_name_key(*name));
		const auto listener = named == names->end() ? nullptr : named->second.lock();
		if (!listener || listener->closed)
			return 0xc0000034u;
		if (call[2] || call[5] || call[8] || call[9] || (call.u32(4) & ~0x20000u))
			return not_supported;
		if ((call[6] != 0) != (call[7] != 0))
			return invalid_parameter;
		call.process.validate_write_memory(call[0], call.pointer_width());
		const auto wake = deadline(call, call[10]);
		auto request = call[6] ? read_message(call, call[6], call.load_pointer(call[7])) : std::optional<AlpcMessage>(AlpcMessage{});
		const auto limit = port_message_limit(call, call[3]);
		if (!request || !limit || header_size(call) + request->data.size() > listener->message_limit)
			return invalid_parameter;
		if (listener->requests.size() >= 256 || listener->inbox.size() >= 256 || *next_id > UINT32_MAX)
			return 0xc0000044u;
		const auto client = std::make_shared<AlpcPort>();
		client->message_limit = *limit;
		const auto connection = std::make_shared<AlpcConnection>();
		client->connection = connection;
		connection->client = client;
		connection->listener = listener;
		const uint32_t id = uint32_t((*next_id)++);
		WithdrawAlpcRequest withdraw{*listener, id};
		request->process = call.load_pointer(context.environment.teb() + 8 * call.pointer_width());
		request->thread = context.threads.current_id();
		request->id = id;
		request->type = 10;
		request->request = connection;
		listener->requests.emplace(id, connection);
		listener->inbox.push_back(std::move(*request));
		if (!context.threads.block([&] { return listener->closed || connection->state != AlpcConnection::State::waiting; }, wake))
			return timeout;
		if (listener->closed)
			return port_disconnected;
		if (connection->state != AlpcConnection::State::accepted)
			return 0xc0000041u;
		if (call[6])
			if (const auto status = write_message(call, call[6], call[7], connection->response))
				return status;
		const auto made = context.objects.add_object(client);
		if (made.status)
			return made.status;
		try { call.store_pointer(call[0], made.value); }
		catch (...) { context.objects.close(made.value); throw; }
		return 0;
	}));
	registry.add("NtAlpcAcceptConnectPort", 9, alpc_status([&context](ApiCall &call) -> uint32_t {
		const auto listener = context.objects.object_as<AlpcPort>(call[1]);
		if (!listener)
			return invalid_handle;
		if (listener->closed)
			return port_disconnected;
		if (call.u32(2) || call[3] || call[5] || call[7])
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
		if (!call.u32(8)) {
			connection->state = AlpcConnection::State::rejected;
			return 0;
		}
		const auto limit = port_message_limit(call, call[4]);
		if (!limit)
			return invalid_parameter;
		call.process.validate_write_memory(call[0], call.pointer_width());
		const auto server = std::make_shared<AlpcPort>();
		server->message_limit = *limit;
		server->server = true;
		server->connection = connection;
		const auto made = context.objects.add_object(server);
		if (made.status)
			return made.status;
		try { call.store_pointer(call[0], made.value); }
		catch (...) { context.objects.close(made.value); throw; }
		connection->server = server;
		connection->response = std::move(*response);
		connection->state = AlpcConnection::State::accepted;
		return 0;
	}));
	registry.add("NtAlpcSendWaitReceivePort", 8, alpc_status([&context, next_id, disconnected](ApiCall &call) -> uint32_t {
		const auto port = context.objects.object_as<AlpcPort>(call[0]);
		if (!port)
			return invalid_handle;
		constexpr uint32_t reply_flag = 1, sync_flag = 0x20000;
		const uint32_t flags = call.u32(1);
		if (flags & ~(reply_flag | sync_flag) || call[3] || call[6])
			return not_supported;
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
				message->type = 2;
				pending->second->result = std::move(*message);
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
					message->reply = reply;
					message->type = 1;
					WithdrawAlpcRequest withdraw{*destination, message->id};
					destination->replies.emplace(message->id, reply);
					destination->inbox.push_back(std::move(*message));
					if (!context.threads.block([&] { return reply->result || disconnected(*port); }, wake))
						return timeout;
					return reply->result ? write_message(call, call[4], call[5], *reply->result) : port_disconnected;
				}
				message->type = 3;
				destination->inbox.push_back(std::move(*message));
			}
		}
		if (!call[4])
			return 0;
		if (!context.threads.block([&] { return !port->inbox.empty() || disconnected(*port); }, wake))
			return timeout;
		if (port->inbox.empty())
			return port_disconnected;
		const auto &message = port->inbox.front();
		if (const auto status = write_message(call, call[4], call[5], message))
			return status;
		if (message.request)
			message.request->delivered = true;
		if (message.reply)
			message.reply->delivered = true;
		port->inbox.pop_front();
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
