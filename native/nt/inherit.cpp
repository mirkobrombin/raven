#include "inherit.h"
#include "access_token.h"
#include "child_process.h"
#include "event.h"
#include "mutex.h"

#include "file_objects.h"
#include "kernel_context.h"
#include "pipe.h"
#include "section.h"
#include "threads.h"

#include <cstring>
#include <stdexcept>

namespace raven {

namespace {

template<typename T> void put(std::vector<uint8_t> &bytes, T value)
{
	const auto raw = reinterpret_cast<const uint8_t *>(&value);
	bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

void put_text(std::vector<uint8_t> &bytes, const void *data, size_t size)
{
	put<uint32_t>(bytes, uint32_t(size));
	const auto raw = static_cast<const uint8_t *>(data);
	bytes.insert(bytes.end(), raw, raw + size);
}

class Reader {
public:
	explicit Reader(std::span<const uint8_t> bytes) : bytes(bytes) {}
	template<typename T> T get()
	{
		if (at + sizeof(T) > bytes.size())
			throw std::runtime_error("truncated inherited handle block");
		T value;
		std::memcpy(&value, bytes.data() + at, sizeof(T));
		at += sizeof(T);
		return value;
	}
	std::string text()
	{
		const auto size = get<uint32_t>();
		if (at + size > bytes.size())
			throw std::runtime_error("truncated inherited handle block");
		std::string value(reinterpret_cast<const char *>(bytes.data() + at), size);
		at += size;
		return value;
	}
	bool done() const { return at >= bytes.size(); }

private:
	std::span<const uint8_t> bytes;
	size_t at = 0;
};

}

std::vector<uint8_t> serialize_inherited(const std::vector<InheritedHandle> &handles)
{
	std::vector<uint8_t> bytes;
	for (const auto &item : handles) {
		put<uint64_t>(bytes, item.handle);
		put_text(bytes, item.kind.data(), item.kind.size());
		put<int32_t>(bytes, item.descriptor);
		put<uint32_t>(bytes, item.access);
		put<uint32_t>(bytes, item.flags);
		put<uint32_t>(bytes, item.bits);
		put<uint32_t>(bytes, item.handle_access);
		put<uint64_t>(bytes, item.size);
		put_text(bytes, item.text.data(), item.text.size() * 2);
		put_text(bytes, item.path.data(), item.path.size());
		put<uint32_t>(bytes, item.standard);
	}
	return bytes;
}

std::vector<InheritedHandle> parse_inherited(std::span<const uint8_t> bytes)
{
	std::vector<InheritedHandle> handles;
	Reader reader(bytes);
	while (!reader.done()) {
		InheritedHandle item;
		item.handle = reader.get<uint64_t>();
		item.kind = reader.text();
		item.descriptor = reader.get<int32_t>();
		item.access = reader.get<uint32_t>();
		item.flags = reader.get<uint32_t>();
		item.bits = reader.get<uint32_t>();
		item.handle_access = reader.get<uint32_t>();
		item.size = reader.get<uint64_t>();
		const auto text = reader.text();
		item.text.assign(reinterpret_cast<const char16_t *>(text.data()), text.size() / 2);
		item.path = reader.text();
		item.standard = reader.get<uint32_t>();
		handles.push_back(std::move(item));
	}
	return handles;
}

// The object a description stands for, rebuilt around the descriptor that came with it; null for a kind that cannot travel.
std::shared_ptr<KernelObject> object_from_record(const InheritedHandle &item)
{
	if (item.kind == "mutex")
		return std::make_shared<MutexObject>(item.descriptor);
	if (item.kind == "event")
		return std::make_shared<EventObject>(item.descriptor);
	if (item.kind == "token")
		return std::make_shared<AccessToken>(item.descriptor);
	if (item.kind == "file") {
		// The creator keeps the duty to delete a file that closes with its last handle.
		return std::make_shared<FileObject>(item.descriptor, item.access, item.bits & 1, std::filesystem::path(item.path), item.text, item.flags & ~0x04000000U);
	}
	if (item.kind == "pipe" && (item.bits & 8)) {
		// A server end that waits for its client: the listening socket came instead of a connection.
		auto pipe = std::make_shared<PipeObject>(-1, item.access, item.flags);
		pipe->listener = adopt_pipe_listener(item.descriptor, item.path, int(item.size >> 32), uint32_t(item.size));
		pipe->server = true;
		pipe->message_mode = item.bits & 1;
		pipe->read_messages = item.bits & 4;
		pipe->inherit = true;
		pipe->name = item.text;
		return pipe;
	}
	if (item.kind == "pipe") {
		auto pipe = std::make_shared<PipeObject>(item.descriptor, item.access, item.flags);
		pipe->connected = true;
		pipe->message_mode = item.bits & 1;
		pipe->read_messages = item.bits & 4;
		pipe->server = item.bits & 2;
		pipe->inherit = true;
		pipe->name = item.text;
		return pipe;
	}
	if (item.kind == "thread")
		return std::make_shared<RemoteThreadObject>(item.descriptor);
	if (item.kind == "process") {
		// The process is named by its id; the pidfd only kept it from being mistaken for another while the handle travelled.
		::close(item.descriptor);
		return std::make_shared<ProcessObject>(pid_t(item.size), true);
	}
	if (item.kind == "section") {
		auto section = std::make_shared<SectionObject>();
		section->descriptor = item.descriptor;
		section->size = item.size;
		section->protection = item.access;
		section->name = item.text;
		return section;
	}
	return nullptr;
}

// Rebuilds the handles a creator passed on, each under the value it had there.
void adopt_inherited(NativeObjects &objects, const std::vector<InheritedHandle> &handles)
{
	for (const auto &item : handles) {
		const auto object = object_from_record(item);
		if (!object) {
			::close(item.descriptor);
			continue;
		}
		if (!objects.adopt(item.handle, object, true))
			throw std::runtime_error("inherited handle value is taken");
		if (item.handle_access || item.kind == "token" || item.kind == "event" || item.kind == "mutex")
			objects.set_access(item.handle, item.handle_access);
	}
}

}
