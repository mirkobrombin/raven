#pragma once

#include "completion_port.h"
#include "file_objects.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace raven {

struct KernelContext;
class ApiCall;
class ApiRegistry;

// What every instance of one named pipe shares: the listening socket that clients connect to.
struct PipeListener {
	~PipeListener()
	{
		if (descriptor >= 0)
			::close(descriptor);
	}
	int descriptor = -1;
	int type = SOCK_STREAM;
	uint32_t maximum = 255, instances = 0;
	std::string address;
};

// One end of a pipe. A server end is created listening and becomes connected when a client opens the pipe; a client end, and both ends
// of an anonymous pipe, are connected from the start. The data moves through a host socket that stays blocking for the program that
// shares it, so every transfer asks for non-blocking behavior call by call.
class PipeObject : public FileObject {
public:
	PipeObject(int descriptor, uint32_t access, uint32_t flags) : FileObject(descriptor, access, false, std::filesystem::path(), std::u16string(), flags) {}
	~PipeObject() override;
	bool describe(InheritedHandle &record) const override
	{
		// A server end that no client has opened yet hands over the socket its clients connect to, and waits for them there.
		if (descriptor < 0) {
			if (!server || !listener || listener->descriptor < 0)
				return false;
			record.kind = "pipe";
			record.descriptor = listener->descriptor;
			record.access = access;
			record.flags = flags;
			record.bits = (message_mode ? 1U : 0U) | 2U | (read_messages ? 4U : 0U) | 8U;
			record.size = uint64_t(uint32_t(listener->type)) << 32 | listener->maximum;
			record.path = listener->address;
			record.text = name;
			return true;
		}
		record.kind = "pipe";
		record.descriptor = descriptor;
		record.access = access;
		record.flags = flags;
		record.bits = (message_mode ? 1U : 0U) | (server ? 2U : 0U) | (read_messages ? 4U : 0U);
		record.text = name;
		return true;
	}
	bool overlapped() const { return flags & 0x40000000U; }
	std::shared_ptr<PipeListener> listener;
	// message_mode is the type of the pipe; read_messages how this handle reads it.
	bool server = false, connected = false, message_mode = false, read_messages = false, inherit = false;
	std::u16string name;
	// The unread rest of a message that did not fit the buffer of the read that took it.
	std::vector<uint8_t> carry;
};

// An overlapped operation that has not finished: the pipe it runs on, so closing or cancelling can end it.
struct PendingIo {
	std::weak_ptr<PipeObject> pipe;
	uint64_t overlapped = 0;
	bool cancelled = false;
};

struct PipeState {
	std::map<std::string, std::weak_ptr<PipeListener>> listeners;
	std::vector<std::shared_ptr<PendingIo>> pending;
};

// The listening socket of a pipe that a server end received from another process carries, which later instances of the pipe made
// in this process share.
std::shared_ptr<PipeListener> adopt_pipe_listener(int descriptor, const std::string &address, int type, uint32_t maximum);
// A server end whose client has opened the pipe takes that connection: true when the end is connected.
bool ensure_pipe_connected(PipeObject &pipe);
void register_pipe_api(ApiRegistry &registry, KernelContext &context);
// The Windows form of a pipe path, "\\.\pipe\name": the name without the prefix, or nothing for any other path.
std::optional<std::u16string> pipe_name_of(std::u16string_view path);
// CreateFile on a pipe path; false when the path names no pipe.
bool open_pipe_client(KernelContext &context, ApiCall &call, std::u16string_view path, uint32_t access, uint32_t flags);
// ReadFile and WriteFile on a pipe handle; false when the handle is no pipe.
bool transfer_pipe(KernelContext &context, ApiCall &call, bool reading);
// NtReadFile and NtWriteFile on a pipe handle, with their event and IO_STATUS_BLOCK; false when the handle is no pipe.
bool native_pipe_transfer(KernelContext &context, ApiCall &call, bool reading);
// Reports an overlapped transfer on a file handle that finished inside the call: the OVERLAPPED gets the status and count, its event is
// set, and the file's completion port receives a packet unless the handle skips packets for transfers that succeed at once.
void complete_file_io(KernelContext &context, FileObject &file, uint64_t overlapped, size_t width, uint32_t error, uint64_t bytes);

}
