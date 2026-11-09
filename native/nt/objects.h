#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <span>
#include <cstdint>
#include <vector>

#include "inherit.h"

namespace raven {

// Base of the objects held by the kernel handle table.
class KernelObject {
public:
	virtual ~KernelObject() = default;
	// Waiting: 1 when a wait by the given thread would succeed now, 0 when it would block, -1 for objects that cannot be waited on.
	virtual int signaled(uint32_t thread) const { (void)thread; return -1; }
	// Runs when a wait succeeds: auto-reset events clear, mutexes change owner, semaphores count down.
	virtual void consume(uint32_t thread) { (void)thread; }
	// Takes what a satisfied wait takes, and says whether it still could: an object shared with other processes can lose it to one of them
	// between the check and the take.
	virtual bool try_consume(uint32_t thread) { consume(thread); return true; }
	// Sets or clears an event that lives in this object; the previous state, or -1 when the object is no event.
	virtual int set_state(bool signaled) { (void)signaled; return -1; }
	// Describes the object for a process that inherits it; false for objects that cannot be handed to another process.
	virtual bool describe(InheritedHandle &record) const { (void)record; return false; }
	// The name of the object's type in the object manager (NtQueryObject); empty for objects Windows has no type for.
	virtual std::u16string_view type_name() const { return {}; }
	// The access a new handle to the object holds when nothing narrower is asked for.
	virtual uint32_t default_access() const { return 0x1f0003; }
	// When the object turns signaled by itself, as a timer does at its due time; a waiter wakes then to look again.
	virtual std::optional<std::chrono::steady_clock::time_point> wakes_at() const { return std::nullopt; }
};

class NativeObjects;
class EventObject;
// Rebuilds the handles a creator passed on, each under the value it had there.
void adopt_inherited(NativeObjects &objects, const std::vector<InheritedHandle> &handles);
// The object a handle description stands for, built around its descriptor; null for a kind that cannot be handed over.
std::shared_ptr<KernelObject> object_from_record(const InheritedHandle &record);

// A handle value held for something another part of the runtime owns, such as an open registry key; closing the handle tells the owner.
class ReservedObject : public KernelObject {
public:
	// The runtime reserves handles for registry keys only.
	std::u16string_view type_name() const override { return u"Key"; }
	std::function<void(uint64_t)> on_close;
};

struct NativeObjectResult {
	uint64_t value = 0;
	uint32_t status = 0;
	bool runtime_timeout = false;
};

// Process handle identities refer to shared object state, rather than copying it.
class NativeObjects {
public:
	explicit NativeObjects(size_t limit = 10000) : limit(limit) {}
	NativeObjects(const NativeObjects &) = delete;
	NativeObjects &operator=(const NativeObjects &) = delete;
	NativeObjectResult create_event(uint32_t access, uint32_t type, bool signaled);
	// Events that have a name are one object however often they are created or opened: `existed` tells whether the name was taken.
	NativeObjectResult create_named_event(const std::u16string &name, uint32_t access, uint32_t type, bool signaled, bool &existed);
	// A new handle for the named event; the status is 0xc0000034 when no such event is alive.
	NativeObjectResult open_named_event(const std::u16string &name, uint32_t access);
	NativeObjectResult inherit_file(int descriptor);
	// Reads from an inherited standard handle; the value is the byte count, zero at the end of the input.
	NativeObjectResult read_file(uint64_t handle,std::span<uint8_t> into);
	NativeObjectResult file_type(uint64_t handle) const;
	// The host descriptor behind an inherited standard handle, -1 for any other handle.
	int descriptor(uint64_t handle) const;
	NativeObjectResult write_file(uint64_t handle, std::span<const uint8_t> bytes);
	uint32_t close(uint64_t handle);
	// Takes a handle value for an owner outside the table; zero when none is free. Closing it runs `on_close`.
	uint64_t reserve(std::function<void(uint64_t)> on_close);
	// Forgets a reserved handle without telling the owner (the owner closed it first).
	void release_reserved(uint64_t handle);
	// Generic objects share the handle namespace of events and inherited files.
	NativeObjectResult add_object(std::shared_ptr<KernelObject> object);
	std::shared_ptr<KernelObject> object(uint64_t handle) const;
	template<typename T> std::shared_ptr<T> object_as(uint64_t handle) const { return std::dynamic_pointer_cast<T>(object(handle)); }
	// Waiting state of any handle kind (see KernelObject::signaled); -2 when the handle does not exist.
	int signaled(uint64_t handle, uint32_t thread) const;
	void consume(uint64_t handle, uint32_t thread);
	// Takes what a satisfied wait takes; false when another process took it first.
	bool try_consume(uint64_t handle, uint32_t thread);
	// Duplicates a handle onto the same object (DuplicateHandle within the process).
	// A new handle to the same object; `access` narrows the rights the copy carries (kept for objects whose rights matter, such as sections).
	NativeObjectResult duplicate(uint64_t handle, std::optional<uint32_t> access = std::nullopt);
	uint32_t access_of(uint64_t handle) const;
	void set_access(uint64_t handle, uint32_t access);
	// Whether a child process created with handle inheritance receives the handle (HANDLE_FLAG_INHERIT).
	void set_inherit(uint64_t handle, bool inherit);
	bool inherit(uint64_t handle) const;
	// The generic object behind a handle and the handles that are inheritable, for process creation.
	std::vector<uint64_t> inheritable() const;
	// Puts an object under a handle value that another process chose; false when the value is taken.
	bool adopt(uint64_t handle, std::shared_ptr<KernelObject> object, bool inherit);
	// Process requests share the handle channel, but the process owner validates and serves them.
	std::function<InheritedHandle(const InheritedHandle &)> process_request;
	NativeObjectResult set_event(uint64_t handle, bool signaled);
	NativeObjectResult query_event(uint64_t handle);
	// The object manager type of the object behind a handle; nullopt when the handle does not exist.
	std::optional<std::u16string> type_name(uint64_t handle) const;
	NativeObjectResult wait(uint64_t handle, std::optional<int64_t> timeout,
		std::chrono::steady_clock::time_point deadline);
	size_t count() const;
	std::vector<uint64_t> values() const;
	size_t waiting(uint64_t handle) const;

private:
	struct File {
		int descriptor;
		uint32_t type;
		File(int descriptor,uint32_t type) : descriptor(descriptor),type(type) {}
		~File();
	};
	struct Handle { std::shared_ptr<EventObject> event; uint32_t access; std::shared_ptr<File> file; std::shared_ptr<KernelObject> generic = {}; bool inherit = false; };
	mutable std::mutex mutex;
	// Handles are multiples of four, and Windows leaves the two low bits to the program, which may tag a handle with them (as SNI does
	// an event's): lookups ignore those bits.
	struct UntaggedOrder {
		bool operator()(uint64_t left, uint64_t right) const { return (left & ~uint64_t(3)) < (right & ~uint64_t(3)); }
	};
	std::map<uint64_t,Handle,UntaggedOrder> handles;
	std::map<std::u16string,std::weak_ptr<EventObject>> named_events;
	uint64_t next = 0x100;
	// The next free handle value; the caller holds the mutex. Values that inherited handles occupy are skipped.
	uint64_t take();
	void arrive(uint64_t handle) const;
	size_t limit;
	std::pair<std::shared_ptr<EventObject>,uint32_t> event(uint64_t handle, uint32_t access) const;
	std::shared_ptr<File> file(uint64_t handle) const;
};

}
