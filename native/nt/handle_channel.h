#pragma once

#include "inherit.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <sys/types.h>

namespace raven {

class KernelObject;
class NativeObjects;
class NativeThreads;
struct NativeObjectResult;

// A queued transfer from an older sender is outside the local handle range and must be received before lookup.
constexpr uint64_t handle_range_to_child = 0x40000000;

// A socket between a process and one of its children that carries handles after the child started (DuplicateHandle into or out of
// the other process). A transfer carries the descriptor and the record that rebuilds the object. The receiver assigns its value;
// requests are answered when the other process next schedules its threads. A peer channel joins two processes of the prefix that did
// not start each other: it carries process requests and handles asked for or sent with DuplicateHandle, never handles left to inherit.
class HandleChannel {
public:
	explicit HandleChannel(int descriptor, bool peer = false) : descriptor(descriptor), peer(peer) {}
	~HandleChannel();
	HandleChannel(const HandleChannel &) = delete;
	HandleChannel &operator=(const HandleChannel &) = delete;
	// The receiver allocates the handle and acknowledges it before the sender returns its value.
	NativeObjectResult send(NativeObjects &objects, NativeThreads &threads, InheritedHandle record, uint32_t inherit);
	// Asks the other process for the object behind one of its handles, closing it there when `close_source` is set; null when the
	// other process has no such handle or it cannot travel. `access` receives the rights the handle carried.
	std::shared_ptr<KernelObject> fetch(NativeObjects &objects, NativeThreads &threads, uint64_t value, bool close_source, uint32_t &access);
	std::optional<InheritedHandle> request_process(NativeObjects &objects, NativeThreads &threads, InheritedHandle request);
	// Takes in the handles that have arrived, answers the requests and keeps the answers to requests of this side.
	void receive(NativeObjects &objects);
	bool connected() const;
	int host_descriptor() const { return descriptor; }

private:
	bool send_frame(const InheritedHandle &record, int shared);
	std::optional<InheritedHandle> exchange(NativeObjects &objects, NativeThreads &threads, InheritedHandle request, int shared = -1);
	int descriptor;
	bool peer;
	uint64_t next_request = 1;
	std::map<uint64_t, std::optional<InheritedHandle>> answers;
};

// The channel a child keeps to its parent: it comes from the descriptor the parent passed.
void set_parent_channel(int descriptor);
HandleChannel *parent_channel();
// Makes a child's channel known so lookups of unknown handle values drain it.
void add_child_channel(HandleChannel *channel);
void remove_child_channel(HandleChannel *channel);
// Takes in whatever the other processes have sent and answers what they asked.
void receive_handles(NativeObjects &objects);
// The descriptors receive_handles reads; only where receive_handles may run.
std::vector<int> handle_descriptors();
// Process queries can reach a runtime of this prefix even when it is neither the caller's parent nor its child.
void enable_process_requests();
std::shared_ptr<HandleChannel> process_request_channel(pid_t pid);
// Whether a process keeps bound the socket the other Raven processes of this prefix send it requests on.
bool process_requests_bound(pid_t pid);

}
