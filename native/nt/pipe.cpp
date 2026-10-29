#include "pipe.h"

#include "child_process.h"
#include "host_names.h"
#include "kernel_context.h"
#include "security_attributes.h"
#include "text.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace raven {

namespace {

constexpr uint32_t error_access_denied = 5, error_invalid_parameter = 87, error_file_not_found = 2, error_broken_pipe = 109, error_no_data = 232,
	error_more_data = 234, error_pipe_busy = 231, error_pipe_connected = 535, error_pipe_listening = 536, error_not_connected = 233, error_io_pending = 997,
	error_aborted = 995, error_io_incomplete = 996, error_not_found = 1168, error_invalid_function = 1, error_not_enough_memory = 8;
constexpr uint32_t status_pending = 0x103;
constexpr uint32_t file_flag_first_pipe_instance = 0x00080000U, pipe_type_message = 4;
constexpr uint64_t invalid_handle = UINT64_MAX;

// The pipes of one prefix share a namespace of abstract socket names, so processes that run in the same prefix find each other's pipes.
std::string pipe_address(const std::u16string &name)
{
	auto lower = to_utf8(name);
	for (auto &character : lower)
		character = char(std::tolower(static_cast<unsigned char>(character)));
	return "raven.pipe." + prefix_tag() + "." + hex_text(name_hash(lower));
}

socklen_t abstract_address(const std::string &address, sockaddr_un &storage)
{
	std::memset(&storage, 0, sizeof(storage));
	storage.sun_family = AF_UNIX;
	std::memcpy(storage.sun_path + 1, address.data(), address.size());
	return socklen_t(offsetof(sockaddr_un, sun_path) + 1 + address.size());
}

// OVERLAPPED holds NTSTATUS values; the errors the pipe calls report map to the statuses Windows uses for them.
uint32_t status_of(uint32_t error)
{
	switch (error) {
	case 0: return 0;
	case error_broken_pipe: return 0xc000014bU;
	case error_aborted: return 0xc0000120U;
	case error_not_connected: return 0xc00000b0U;
	case error_no_data: return 0xc00000b1U;
	case error_pipe_listening: return 0xc00000b3U;
	case error_more_data: return 0x80000005U;
	default: return 0xc0070000U | (error & 0xffff);
	}
}

uint32_t error_of_status(uint64_t status)
{
	switch (uint32_t(status)) {
	case 0: return 0;
	case 0xc000014bU: return error_broken_pipe;
	case 0xc0000120U: return error_aborted;
	case 0xc00000b0U: return error_not_connected;
	case 0xc00000b1U: return error_no_data;
	case 0xc00000b3U: return error_pipe_listening;
	case 0x80000005U: return error_more_data;
	default:
		if ((uint32_t(status) & 0xffff0000U) == 0xc0070000U)
			return uint32_t(status) & 0xffff;
		// A word that is no status holds a Windows error already, as the socket calls leave it.
		return uint32_t(status);
	}
}

void put_word(NativeProcess &process, uint64_t at, uint64_t value, size_t width)
{
	process.write_memory(at, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&value), width));
}

uint64_t get_word(NativeProcess &process, uint64_t at, size_t width)
{
	const auto raw = process.read_memory(at, width);
	uint64_t value = 0;
	std::memcpy(&value, raw.data(), width);
	return value;
}

// OVERLAPPED: the status and the byte count come first, the event handle after the two offset words.
// A native call names its event apart from the IO_STATUS_BLOCK it passes as the overlapped words: that event stands in for the one an
// OVERLAPPED would hold.
void finish_io(KernelContext &context, const std::shared_ptr<CompletionPort> &port, uint64_t key, uint64_t overlapped, size_t width, uint32_t error, uint64_t bytes,
	bool report = true, uint64_t event = UINT64_MAX)
{
	put_word(context.process, overlapped, status_of(error), width);
	put_word(context.process, overlapped + width, bytes, width);
	const uint64_t handle = event != UINT64_MAX ? event : get_word(context.process, overlapped + 2 * width + 8, width);
	if (handle & ~uint64_t(1))
		context.objects.set_event(handle & ~uint64_t(1), true);
	// A handle with its low bit set asks for no completion packet.
	if (report && port && !(handle & 1))
		port->post({bytes, key, overlapped, error});
}

// One try at a transfer. 0: finished, with the error and the byte count; 1: it has to wait.
struct Transfer {
	bool reading = false;
	uint64_t buffer = 0;
	uint32_t size = 0;
	std::vector<uint8_t> outgoing;
	size_t sent = 0;
};

int attempt(KernelContext &context, PipeObject &pipe, Transfer &transfer, uint32_t &error, uint64_t &bytes)
{
	error = 0;
	bytes = 0;
	if (pipe.descriptor < 0) {
		error = pipe.server ? error_pipe_listening : error_not_connected;
		return 0;
	}
	if (transfer.reading) {
		if (pipe.message_mode) {
			std::vector<uint8_t> message = std::move(pipe.carry);
			pipe.carry.clear();
			if (message.empty()) {
				message.resize(1 << 16);
				const ssize_t count = ::recv(pipe.descriptor, message.data(), message.size(), MSG_DONTWAIT);
				if (count < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
						return 1;
					error = error_broken_pipe;
					return 0;
				}
				if (count == 0) {
					error = error_broken_pipe;
					return 0;
				}
				message.resize(size_t(count));
			}
			const size_t part = std::min<size_t>(message.size(), transfer.size);
			if (part)
				context.process.write_memory(transfer.buffer, std::span<const uint8_t>(message.data(), part));
			bytes = part;
			if (part < message.size()) {
				pipe.carry.assign(message.begin() + ptrdiff_t(part), message.end());
				error = error_more_data;
			}
			return 0;
		}
		std::vector<uint8_t> data(std::min<size_t>(transfer.size, 1 << 20));
		const ssize_t count = ::recv(pipe.descriptor, data.data(), data.size(), MSG_DONTWAIT);
		if (count < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return 1;
			error = error_broken_pipe;
			return 0;
		}
		if (count == 0 && !data.empty()) {
			error = error_broken_pipe;
			return 0;
		}
		if (count > 0)
			context.process.write_memory(transfer.buffer, std::span<const uint8_t>(data.data(), size_t(count)));
		bytes = uint64_t(count);
		return 0;
	}
	while (transfer.sent < transfer.outgoing.size()) {
		const ssize_t count = ::send(pipe.descriptor, transfer.outgoing.data() + transfer.sent, transfer.outgoing.size() - transfer.sent, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (count < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return 1;
			error = error_no_data;
			bytes = transfer.sent;
			return 0;
		}
		transfer.sent += size_t(count);
		if (pipe.message_mode)
			break;
	}
	bytes = transfer.sent;
	return 0;
}

// Waits until the descriptor can take or give data; other guest threads run meanwhile.
void wait_for(KernelContext &context, const PipeObject &pipe, bool reading)
{
	HostWait waiting;
	context.threads.block([&pipe, reading] {
		if (pipe.descriptor < 0)
			return true;
		pollfd entry{pipe.descriptor, short(reading ? POLLIN : POLLOUT), 0};
		return ::poll(&entry, 1, 0) != 0;
	}, std::nullopt);
}

// A transfer for the handlers that read or write on their own: it waits for the pipe and ends with the error and the bytes.
uint32_t blocking(KernelContext &context, PipeObject &pipe, Transfer &transfer, uint64_t &bytes)
{
	for (;;) {
		uint32_t error = 0;
		if (attempt(context, pipe, transfer, error, bytes) == 0)
			return error;
		wait_for(context, pipe, transfer.reading);
	}
}

// The transfer behind ReadFile and WriteFile for overlapped handles: it runs at once when it can and finishes later otherwise.
// How a transfer on an overlapped pipe started: finished at once, with an error or none and its count, or pending until the reactor
// finishes it into the overlapped words.
struct Started {
	bool pending = false;
	uint32_t error = 0;
	uint64_t bytes = 0;
};

Started start_overlapped(KernelContext &context, size_t width, const std::shared_ptr<PipeObject> &pipe, Transfer transfer, uint64_t overlapped,
	uint64_t event = UINT64_MAX)
{
	put_word(context.process, overlapped, status_pending, width);
	put_word(context.process, overlapped + width, 0, width);
	const bool native = event != UINT64_MAX;
	// Starting a transfer clears its event, so the event tells of this transfer and not of an earlier one.
	if (const uint64_t handle = native ? event : get_word(context.process, overlapped + 2 * width + 8, width); handle & ~uint64_t(1))
		context.objects.set_event(handle & ~uint64_t(1), false);
	uint32_t error = 0;
	uint64_t bytes = 0;
	if (attempt(context, *pipe, transfer, error, bytes) == 0) {
		// A failure found at the start is reported by the call; only a transfer that ran is queued as completed.
		if (error && error != error_more_data) {
			put_word(context.process, overlapped, status_of(error), width);
			return {false, error, 0};
		}
		finish_io(context, pipe->port, pipe->key, overlapped, width, error, bytes, !native && !pipe->skip_port_on_success, event);
		return {false, error, bytes};
	}
	auto io = std::make_shared<PendingIo>();
	io->pipe = pipe;
	io->overlapped = overlapped;
	context.pipes->pending.push_back(io);
	auto shared = std::make_shared<Transfer>(std::move(transfer));
	const auto port = pipe->port;
	const auto key = pipe->key;
	auto &state = *context.pipes;
	context.reactor->add([&context, &state, io, shared, port, key, width, overlapped, native, event]() {
		const auto pipe = io->pipe.lock();
		uint32_t error = 0;
		uint64_t bytes = 0;
		bool finished = false;
		if (io->cancelled || !pipe) {
			error = error_aborted;
			finished = true;
		} else {
			if (ensure_pipe_connected(*pipe) || pipe->connected)
				finished = attempt(context, *pipe, *shared, error, bytes) == 0;
		}
		if (!finished)
			return false;
		state.pending.erase(std::remove(state.pending.begin(), state.pending.end(), io), state.pending.end());
		finish_io(context, port, key, overlapped, width, error, bytes, !native, event);
		return true;
	}, pipe->descriptor, short(shared->reading ? POLLIN : POLLOUT));
	return {true, 0, 0};
}

std::shared_ptr<PipeObject> make_client(int descriptor, uint32_t access, uint32_t flags, int type, const std::u16string &name)
{
	auto pipe = std::make_shared<PipeObject>(descriptor, access, flags);
	pipe->connected = true;
	pipe->message_mode = type == SOCK_SEQPACKET;
	pipe->name = name;
	return pipe;
}

// The pipe's socket to a server, or the Windows error that Connect would have reported.
int connect_to(const std::u16string &name, int &type, uint32_t &error)
{
	sockaddr_un storage;
	const auto length = abstract_address(pipe_address(name), storage);
	for (const int candidate : {SOCK_STREAM, SOCK_SEQPACKET}) {
		const int descriptor = ::socket(AF_UNIX, candidate | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (descriptor < 0) {
			error = error_not_enough_memory;
			return -1;
		}
		if (::connect(descriptor, reinterpret_cast<sockaddr *>(&storage), length) == 0) {
			type = candidate;
			return descriptor;
		}
		const int cause = errno;
		::close(descriptor);
		if (cause == EAGAIN) {
			error = error_pipe_busy;
			return -1;
		}
		// A listener of the other socket type refuses like a name nobody serves, so the second type is tried too.
	}
	error = error_file_not_found;
	return -1;
}

// Whether any process of the prefix serves the pipe; the abstract names of the host sockets are listed in /proc/net/unix.
bool pipe_exists(KernelContext &context, const std::u16string &name)
{
	const auto address = pipe_address(name);
	if (const auto found = context.pipes->listeners.find(address); found != context.pipes->listeners.end() && !found->second.expired())
		return true;
	std::ifstream list("/proc/net/unix");
	std::string line;
	const auto wanted = "@" + address;
	while (std::getline(list, line))
		if (line.size() >= wanted.size() && line.compare(line.size() - wanted.size(), wanted.size(), wanted) == 0)
			return true;
	return false;
}

}

void complete_file_io(KernelContext &context, FileObject &file, uint64_t overlapped, size_t width, uint32_t error, uint64_t bytes)
{
	finish_io(context, file.port, file.key, overlapped, width, error, bytes, !(file.skip_port_on_success && !error));
}

PipeObject::~PipeObject()
{
	if (listener && listener->instances)
		--listener->instances;
}

std::optional<std::u16string> pipe_name_of(std::u16string_view path)
{
	for (const std::u16string_view prefix : {u"\\\\.\\pipe\\", u"\\\\?\\pipe\\", u"\\??\\pipe\\"}) {
		if (path.size() > prefix.size() && equal_fold(path.substr(0, prefix.size()), prefix))
			return std::u16string(path.substr(prefix.size()));
	}
	return std::nullopt;
}

bool ensure_pipe_connected(PipeObject &pipe)
{
	if (pipe.connected)
		return true;
	if (!pipe.server || !pipe.listener)
		return false;
	const int descriptor = ::accept4(pipe.listener->descriptor, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (descriptor < 0)
		return false;
	pipe.descriptor = descriptor;
	pipe.connected = true;
	return true;
}

bool open_pipe_client(KernelContext &context, ApiCall &call, std::u16string_view path, uint32_t access, uint32_t flags)
{
	const auto name = pipe_name_of(path);
	if (!name)
		return false;
	call.ret(invalid_handle);
	int type = SOCK_STREAM;
	uint32_t error = 0;
	const int descriptor = connect_to(*name, type, error);
	if (descriptor < 0) {
		call.set_last_error(error);
		return true;
	}
	auto pipe = make_client(descriptor, access, flags, type, *name);
	pipe->inherit = inheritable_handle(call, optional_argument(call, 3));
	const auto made = context.objects.add_object(pipe);
	if (made.status) {
		call.set_last_error(error_not_enough_memory);
		return true;
	}
	if (pipe->inherit)
		context.objects.set_inherit(made.value, true);
	call.set_last_error(0);
	call.ret(made.value);
	return true;
}

bool transfer_pipe(KernelContext &context, ApiCall &call, bool reading)
{
	const auto pipe = context.objects.object_as<PipeObject>(call[0]);
	if (!pipe)
		return false;
	if (reading ? !pipe->readable() : !pipe->writable()) {
		call.fail(error_access_denied);
		return true;
	}
	ensure_pipe_connected(*pipe);
	if (!pipe->connected) {
		call.fail(pipe->server ? error_pipe_listening : error_not_connected);
		return true;
	}
	const uint64_t counted = call[3], overlapped = call[4];
	const bool asynchronous = overlapped && pipe->overlapped();
	if (!asynchronous && !counted) {
		call.fail(error_invalid_parameter);
		return true;
	}
	Transfer transfer;
	transfer.reading = reading;
	transfer.buffer = call[1];
	transfer.size = call.u32(2);
	if (!reading)
		transfer.outgoing = call.read(call[1], transfer.size);
	if (asynchronous) {
		const auto started = start_overlapped(context, call.pointer_width(), pipe, std::move(transfer), overlapped);
		if (started.pending) {
			call.fail(error_io_pending);
			return true;
		}
		if (counted && (!started.error || started.error == error_more_data))
			call.store<uint32_t>(counted, uint32_t(started.bytes));
		if (started.error)
			call.fail(started.error);
		else
			call.ret(1);
		return true;
	}
	if (counted)
		call.store<uint32_t>(counted, 0);
	// A write of nothing ends at once; a read of nothing waits for data like any other read.
	uint64_t bytes = 0;
	const uint32_t error = blocking(context, *pipe, transfer, bytes);
	if (counted)
		call.store<uint32_t>(counted, uint32_t(bytes));
	if (error) {
		call.fail(error);
		return true;
	}
	call.ret(1);
	return true;
}

bool native_pipe_transfer(KernelContext &context, ApiCall &call, bool reading)
{
	const auto pipe = context.objects.object_as<PipeObject>(call[0]);
	if (!pipe)
		return false;
	const size_t width = call.pointer_width();
	const uint64_t event = call[1], status_block = call[4];
	if (!status_block) {
		call.ret(0xc000000dU);
		return true;
	}
	if (reading ? !pipe->readable() : !pipe->writable()) {
		call.ret(0xc0000022U);
		return true;
	}
	ensure_pipe_connected(*pipe);
	if (!pipe->connected) {
		call.ret(status_of(pipe->server ? error_pipe_listening : error_not_connected));
		return true;
	}
	Transfer transfer;
	transfer.reading = reading;
	transfer.buffer = call[5];
	transfer.size = call.u32(6);
	if (!reading)
		transfer.outgoing = call.read(call[5], transfer.size);
	if (event)
		context.objects.set_event(event, false);
	if (pipe->overlapped()) {
		const auto started = start_overlapped(context, width, pipe, std::move(transfer), status_block, event);
		call.ret(started.pending ? status_pending : status_of(started.error));
		return true;
	}
	uint64_t bytes = 0;
	const uint32_t error = blocking(context, *pipe, transfer, bytes);
	put_word(context.process, status_block, status_of(error), width);
	put_word(context.process, status_block + width, bytes, width);
	if (event)
		context.objects.set_event(event, true);
	call.ret(status_of(error));
	return true;
}

void register_pipe_api(ApiRegistry &registry, KernelContext &context)
{
	auto &objects = context.objects;
	const auto pipe_of = [&objects](uint64_t handle) { return objects.object_as<PipeObject>(handle); };

	registry.add("CreateNamedPipeW", 8, [&context, &objects](ApiCall &call) {
		const auto name = pipe_name_of(call.wide(call[0], 260));
		const uint32_t mode = call.u32(1), type = call.u32(2), limit = call.u32(3);
		call.ret(invalid_handle);
		if (!name || name->empty() || !(mode & 3) || (limit == 0 || (limit > 255 && limit != UINT32_MAX))) {
			call.set_last_error(error_invalid_parameter);
			return;
		}
		const bool message = type & pipe_type_message;
		const auto address = pipe_address(*name);
		std::shared_ptr<PipeListener> listener;
		if (const auto found = context.pipes->listeners.find(address); found != context.pipes->listeners.end())
			listener = found->second.lock();
		if (listener) {
			// More instances of a pipe the process already serves.
			if (mode & file_flag_first_pipe_instance) {
				call.set_last_error(error_access_denied);
				return;
			}
			if (listener->instances >= listener->maximum || (listener->type == SOCK_SEQPACKET) != message) {
				call.set_last_error(error_pipe_busy);
				return;
			}
		} else {
			listener = std::make_shared<PipeListener>();
			listener->type = message ? SOCK_SEQPACKET : SOCK_STREAM;
			listener->maximum = limit == UINT32_MAX ? 255 : limit;
			listener->address = address;
			listener->descriptor = ::socket(AF_UNIX, listener->type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
			sockaddr_un storage;
			const auto length = abstract_address(address, storage);
			if (listener->descriptor < 0 || ::bind(listener->descriptor, reinterpret_cast<sockaddr *>(&storage), length) || ::listen(listener->descriptor, 128)) {
				// A pipe of that name that another process serves has a first instance already.
				call.set_last_error(errno == EADDRINUSE ? ((mode & file_flag_first_pipe_instance) ? error_access_denied : error_pipe_busy) : error_not_enough_memory);
				return;
			}
			context.pipes->listeners[address] = listener;
		}
		const uint32_t access = ((mode & 1) ? 0x80000000U : 0) | ((mode & 2) ? 0x40000000U : 0);
		auto pipe = std::make_shared<PipeObject>(-1, access, mode & 0x40000000U);
		pipe->server = true;
		pipe->listener = listener;
		pipe->message_mode = message;
		pipe->name = *name;
		pipe->inherit = inheritable_handle(call, call[7]);
		++listener->instances;
		const auto made = objects.add_object(pipe);
		if (made.status) {
			call.set_last_error(error_not_enough_memory);
			return;
		}
		if (pipe->inherit)
			objects.set_inherit(made.value, true);
		call.set_last_error(0);
		call.ret(made.value);
	});
	registry.add("ConnectNamedPipe", 2, [&context, pipe_of](ApiCall &call) {
		const auto pipe = pipe_of(call[0]);
		if (!pipe || !pipe->server) {
			call.fail(pipe ? error_invalid_function : 6);
			return;
		}
		const size_t width = call.pointer_width();
		const uint64_t overlapped = call[1];
		const bool asynchronous = overlapped && pipe->overlapped();
		if (pipe->connected || ensure_pipe_connected(*pipe)) {
			// A client that came first is reported as a connection that exists; the event tells an overlapped caller all the same.
			if (asynchronous) {
				put_word(context.process, overlapped, 0, width);
				put_word(context.process, overlapped + width, 0, width);
				if (const uint64_t handle = get_word(context.process, overlapped + 2 * width + 8, width); handle & ~uint64_t(1))
					context.objects.set_event(handle & ~uint64_t(1), true);
			}
			call.fail(error_pipe_connected);
			return;
		}
		if (!asynchronous) {
			// A handle without overlapped I/O waits for a client.
			HostWait waiting;
			context.threads.block([pipe] { return ensure_pipe_connected(*pipe); }, std::nullopt);
			call.ret(1);
			return;
		}
		put_word(context.process, overlapped, status_pending, width);
		put_word(context.process, overlapped + width, 0, width);
		auto io = std::make_shared<PendingIo>();
		io->pipe = pipe;
		io->overlapped = overlapped;
		context.pipes->pending.push_back(io);
		const auto port = pipe->port;
		const auto key = pipe->key;
		auto &state = *context.pipes;
		context.reactor->add([&context, &state, io, port, key, width, overlapped]() {
			const auto pipe = io->pipe.lock();
			uint32_t error = 0;
			if (io->cancelled || !pipe)
				error = error_aborted;
			else if (!ensure_pipe_connected(*pipe))
				return false;
			state.pending.erase(std::remove(state.pending.begin(), state.pending.end(), io), state.pending.end());
			finish_io(context, port, key, overlapped, width, error, 0);
			return true;
		}, pipe->listener ? pipe->listener->descriptor : -1, short(POLLIN));
		call.fail(error_io_pending);
	});
	registry.add("DisconnectNamedPipe", 1, [pipe_of](ApiCall &call) {
		const auto pipe = pipe_of(call[0]);
		if (!pipe || !pipe->server) {
			call.fail(pipe ? error_invalid_function : 6);
			return;
		}
		if (pipe->descriptor >= 0)
			::close(pipe->descriptor);
		pipe->descriptor = -1;
		pipe->connected = false;
		pipe->carry.clear();
		call.ret(1);
	});
	registry.add("WaitNamedPipeW", 2, [&context](ApiCall &call) {
		const auto name = pipe_name_of(call.wide(call[0], 260));
		if (!name) {
			call.fail(error_invalid_parameter);
			return;
		}
		if (!pipe_exists(context, *name)) {
			call.fail(error_file_not_found);
			return;
		}
		call.ret(1);
	});
	registry.add("SetNamedPipeHandleState", 4, [pipe_of](ApiCall &call) {
		const auto pipe = pipe_of(call[0]);
		if (!pipe) {
			call.fail(6);
			return;
		}
		if (call[1])
			pipe->message_mode = pipe->message_mode && (call.load<uint32_t>(call[1]) & 2);
		call.ret(1);
	});
	registry.add("GetNamedPipeInfo", 5, [pipe_of](ApiCall &call) {
		const auto pipe = pipe_of(call[0]);
		if (!pipe) {
			call.fail(6);
			return;
		}
		if (call[1])
			call.store<uint32_t>(call[1], (pipe->server ? 1U : 0U) | (pipe->message_mode ? 4U : 0U));
		if (call[2])
			call.store<uint32_t>(call[2], 65536);
		if (call[3])
			call.store<uint32_t>(call[3], 65536);
		if (call[4])
			call.store<uint32_t>(call[4], pipe->listener ? pipe->listener->maximum : 255);
		call.ret(1);
	});
	registry.add("GetNamedPipeHandleState", 7, [pipe_of](ApiCall &call) {
		const auto pipe = pipe_of(call[0]);
		if (!pipe) {
			call.fail(6);
			return;
		}
		if (call[1])
			call.store<uint32_t>(call[1], pipe->message_mode ? 2 : 0);
		if (call[2])
			call.store<uint32_t>(call[2], pipe->listener ? pipe->listener->instances : 1);
		if (call[3])
			call.store<uint32_t>(call[3], 0);
		if (call[4])
			call.store<uint32_t>(call[4], 0);
		call.ret(1);
	});
	const auto peer_process = [pipe_of](ApiCall &call) {
		const auto pipe = pipe_of(call[0]);
		if (!pipe || !call[1]) {
			call.fail(pipe ? error_invalid_parameter : 6);
			return;
		}
		ucred credentials{};
		socklen_t length = sizeof(credentials);
		if (pipe->descriptor < 0 || ::getsockopt(pipe->descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length)) {
			call.fail(error_not_connected);
			return;
		}
		call.store<uint32_t>(call[1], uint32_t(credentials.pid));
		call.ret(1);
	};
	registry.add("GetNamedPipeClientProcessId", 2, peer_process);
	registry.add("GetNamedPipeServerProcessId", 2, peer_process);
	// Every client of a Raven pipe runs as the same user, so the server thread takes the identity of its own logon at the impersonation
	// level a client grants unless it asks otherwise.
	registry.add("ImpersonateNamedPipeClient", 1, [&context, pipe_of](ApiCall &call) {
		const auto pipe = pipe_of(call[0]);
		if (!pipe || !pipe->server) {
			call.fail(6);
			return;
		}
		ensure_pipe_connected(*pipe);
		if (!pipe->connected) {
			call.fail(1368);
			return;
		}
		auto token = std::make_shared<AccessToken>(*context.process_token);
		token->set_type(2);
		token->set_impersonation_level(2);
		context.threads.current().impersonation = std::move(token);
		call.ret(1);
	});
	// A transaction writes a request and reads the answer in one call.
	const auto transact = [&context](ApiCall &call, const std::shared_ptr<PipeObject> &pipe, uint64_t request, uint32_t request_size, uint64_t answer, uint32_t answer_size, uint64_t counted) {
		ensure_pipe_connected(*pipe);
		if (!pipe->connected) {
			call.fail(pipe->server ? error_pipe_listening : error_not_connected);
			return;
		}
		Transfer out;
		out.outgoing = call.read(request, request_size);
		uint64_t bytes = 0;
		if (const uint32_t error = blocking(context, *pipe, out, bytes)) {
			call.fail(error);
			return;
		}
		Transfer in;
		in.reading = true;
		in.buffer = answer;
		in.size = answer_size;
		const uint32_t error = blocking(context, *pipe, in, bytes);
		if (counted)
			call.store<uint32_t>(counted, uint32_t(bytes));
		if (error) {
			call.fail(error);
			return;
		}
		call.ret(1);
	};
	registry.add("TransactNamedPipe", 7, [pipe_of, transact](ApiCall &call) {
		const auto pipe = pipe_of(call[0]);
		if (!pipe || call[6]) {
			call.fail(pipe ? error_invalid_function : 6);
			return;
		}
		transact(call, pipe, call[1], call.u32(2), call[3], call.u32(4), call[5]);
	});
	registry.add("CallNamedPipeW", 7, [&context, transact](ApiCall &call) {
		const auto name = pipe_name_of(call.wide(call[0], 260));
		if (!name) {
			call.fail(error_invalid_parameter);
			return;
		}
		int type = SOCK_STREAM;
		uint32_t error = 0;
		const int descriptor = connect_to(*name, type, error);
		if (descriptor < 0) {
			call.fail(error);
			return;
		}
		const auto pipe = make_client(descriptor, 0xc0000000U, 0, type, *name);
		transact(call, pipe, call[1], call.u32(2), call[3], call.u32(4), call[5]);
	});

	// The result of an overlapped operation is in the OVERLAPPED structure the program passed to start it.
	const auto overlapped_result = [&context](ApiCall &call) {
		const uint64_t overlapped = call[1];
		if (!overlapped) {
			call.fail(error_invalid_parameter);
			return;
		}
		const size_t width = call.pointer_width();
		auto status = get_word(context.process, overlapped, width);
		if (uint32_t(status) == status_pending) {
			if (!call.u32(3)) {
				call.fail(error_io_incomplete);
				return;
			}
			HostWait waiting;
			context.threads.block([&] {
				context.reactor->poll();
				return uint32_t(get_word(context.process, overlapped, width)) != status_pending;
			}, std::nullopt);
			status = get_word(context.process, overlapped, width);
		}
		if (call[2])
			call.store<uint32_t>(call[2], uint32_t(get_word(context.process, overlapped + width, width)));
		if (const uint32_t error = error_of_status(status)) {
			call.fail(error);
			return;
		}
		call.ret(1);
	};
	registry.add("GetOverlappedResult", 4, overlapped_result);
	registry.add("GetOverlappedResultEx", 5, [&context, overlapped_result](ApiCall &call) {
		// The wait of the extended call is bounded; an operation that is still running when it ends reports a timeout.
		const uint32_t milliseconds = call.u32(3);
		const auto status = get_word(context.process, call[1], call.pointer_width());
		if (call[1] && uint32_t(status) == status_pending && milliseconds != UINT32_MAX) {
			const auto wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
			HostWait waiting;
			context.threads.block([&] {
				context.reactor->poll();
				return uint32_t(get_word(context.process, call[1], call.pointer_width())) != status_pending;
			}, wake);
			if (uint32_t(get_word(context.process, call[1], call.pointer_width())) == status_pending) {
				call.fail(258);
				return;
			}
		}
		overlapped_result(call);
	});

	// Cancelling ends the operations still waiting on the handle, as aborted; the completions are delivered at once.
	const auto cancel = [&context, pipe_of](ApiCall &call, uint64_t handle, uint64_t overlapped, bool required) {
		const auto pipe = pipe_of(handle);
		if (!pipe) {
			call.fail(6);
			return;
		}
		bool found = false;
		for (const auto &io : context.pipes->pending)
			if (io->pipe.lock() == pipe && (!overlapped || io->overlapped == overlapped)) {
				io->cancelled = true;
				found = true;
			}
		context.reactor->poll();
		if (!found && required) {
			call.fail(error_not_found);
			return;
		}
		call.ret(1);
	};
	// The flags are a UCHAR: only the low byte of the argument word is the caller's.
	registry.add("SetFileCompletionNotificationModes", 2, [&context](ApiCall &call) {
		const uint32_t modes = uint8_t(call[1]);
		if (!(modes & ~3U) && set_socket_completion_modes(context.objects, call[0], modes)) {
			call.ret(1);
			return;
		}
		const auto file = context.objects.object_as<FileObject>(call[0]);
		if (!file || (modes & ~3U)) {
			call.fail(file ? error_invalid_parameter : 6);
			return;
		}
		file->skip_port_on_success = modes & 1;
		file->skip_event_on_success = modes & 2;
		call.ret(1);
	});
	registry.add("CancelIoEx", 2, [cancel](ApiCall &call) { cancel(call, call[0], call[1], true); });
	// The native cancels: every transfer the handle has pending, or the one whose IO_STATUS_BLOCK is named; the outcome as an NTSTATUS.
	const auto native_cancel = [&context, pipe_of](ApiCall &call, uint64_t handle, uint64_t status_block, uint64_t outcome) {
		const auto pipe = pipe_of(handle);
		if (!pipe) {
			call.ret(0xc0000008U);
			return;
		}
		bool found = false;
		for (const auto &io : context.pipes->pending)
			if (io->pipe.lock() == pipe && (!status_block || io->overlapped == status_block)) {
				io->cancelled = true;
				found = true;
			}
		context.reactor->poll();
		if (outcome) {
			put_word(context.process, outcome, 0, call.pointer_width());
			put_word(context.process, outcome + call.pointer_width(), 0, call.pointer_width());
		}
		call.ret(found || !status_block ? 0 : 0xc0000225U);
	};
	registry.add("NtCancelIoFile", 2, [native_cancel](ApiCall &call) { native_cancel(call, call[0], 0, call[1]); });
	registry.add("NtCancelIoFileEx", 3, [native_cancel](ApiCall &call) { native_cancel(call, call[0], call[1], call[2]); });
	// FSCTL_PIPE_LISTEN, ConnectNamedPipe as the native call makes it: the event and the IO_STATUS_BLOCK report a client that arrives.
	registry.add("NtFsControlFile", 10, [&context, pipe_of](ApiCall &call) {
		constexpr uint32_t fsctl_pipe_listen = 0x110008, status_invalid_device_request = 0xc0000010U, status_pipe_connected = 0xc00000b2U;
		const auto pipe = pipe_of(call[0]);
		const size_t width = call.pointer_width();
		const uint64_t event = call[1], status_block = call[4];
		if (!pipe || !pipe->server || call.u32(5) != fsctl_pipe_listen || !status_block) {
			call.ret(status_invalid_device_request);
			return;
		}
		if (event)
			context.objects.set_event(event, false);
		if (pipe->connected || ensure_pipe_connected(*pipe)) {
			put_word(context.process, status_block, status_pipe_connected, width);
			put_word(context.process, status_block + width, 0, width);
			call.ret(status_pipe_connected);
			return;
		}
		if (!pipe->overlapped()) {
			HostWait waiting;
			context.threads.block([pipe] { return ensure_pipe_connected(*pipe); }, std::nullopt);
			put_word(context.process, status_block, 0, width);
			put_word(context.process, status_block + width, 0, width);
			if (event)
				context.objects.set_event(event, true);
			call.ret(0);
			return;
		}
		put_word(context.process, status_block, status_pending, width);
		put_word(context.process, status_block + width, 0, width);
		auto io = std::make_shared<PendingIo>();
		io->pipe = pipe;
		io->overlapped = status_block;
		context.pipes->pending.push_back(io);
		auto &state = *context.pipes;
		context.reactor->add([&context, &state, io, width, status_block, event]() {
			const auto pipe = io->pipe.lock();
			uint32_t error = 0;
			if (io->cancelled || !pipe)
				error = error_aborted;
			else if (!ensure_pipe_connected(*pipe))
				return false;
			state.pending.erase(std::remove(state.pending.begin(), state.pending.end(), io), state.pending.end());
			finish_io(context, nullptr, 0, status_block, width, error, 0, false, event);
			return true;
		}, pipe->listener ? pipe->listener->descriptor : -1, short(POLLIN));
		call.ret(status_pending);
	});
	registry.add("CancelIo", 1, [cancel](ApiCall &call) { cancel(call, call[0], 0, false); });
}

}
