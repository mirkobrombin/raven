#pragma once

#include "child_process.h"
#include "objects.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <vector>

namespace raven {

class CompletionReservation : public KernelObject {
public:
	bool queued = false;
};

class CompletionReserve : public CompletionReservation {
public:
	std::u16string_view type_name() const override { return u"IoCompletionReserve"; }
};

struct CompletionPacket {
	uint64_t bytes = 0, key = 0, overlapped = 0;
	// The Windows error of the operation, zero when it succeeded.
	uint32_t status = 0;
	std::optional<uint32_t> native_status = {};
	std::shared_ptr<CompletionReservation> reserve = {};
};

// An I/O completion port: a queue of finished operations that threads take from.
class CompletionPort : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"IoCompletion"; }
	// As Windows' queue object it can be waited on: signaled while it holds packets, and a wait on it leaves them queued.
	int signaled(uint32_t thread) const override { (void)thread; return packets.empty() ? 0 : 1; }
	void post(const CompletionPacket &packet)
	{
		packets.push_back(packet);
		signaled_now();
	}
	bool take(CompletionPacket &packet)
	{
		if (packets.empty())
			return false;
		packet = std::move(packets.front());
		packets.pop_front();
		if (packet.reserve) {
			packet.reserve->queued = false;
			packet.reserve.reset();
		}
		return true;
	}
	bool cancel(const CompletionReservation &reservation)
	{
		const auto found = std::find_if(packets.begin(), packets.end(), [&](const auto &packet) { return packet.reserve.get() == &reservation; });
		if (found == packets.end())
			return false;
		found->reserve->queued = false;
		packets.erase(found);
		return true;
	}
	bool empty() const { return packets.empty(); }
	// Its last handle closed: the threads that wait on it give up (ERROR_ABANDONED_WAIT_0), and the packets still queued are gone.
	bool watches_last_handle() const override { return true; }
	void last_handle_closed() override
	{
		abandoned = true;
		for (const auto &packet : packets)
			if (packet.reserve)
				packet.reserve->queued = false;
		packets.clear();
	}
	bool closed() const { return abandoned; }

private:
	std::deque<CompletionPacket> packets;
	bool abandoned = false;
};

class WaitCompletionPacket : public CompletionReservation {
public:
	std::u16string_view type_name() const override { return u"WaitCompletionPacket"; }
	uint32_t default_access() const override { return 0x000f0001; }
	std::weak_ptr<CompletionPort> port;
	std::shared_ptr<KernelObject> target;
	CompletionPacket notification;
	uint64_t generation = 0;
};

// Operations that were started and cannot finish yet. Each one is asked again whenever the process looks for completions, and drops out
// once it reports that it has finished.
class Reactor {
public:
	using Operation = std::function<bool()>;
	// A pending operation counts as work only the host can finish, so waiting threads are not taken for deadlocked. The descriptor and
	// events it waits for, when it names them, let an idle process sleep until the operation can go on instead of trying it again and
	// again.
	// An operation may add the one that continues it while it runs; that one joins the others once the round is over.
	void add(Operation operation, int descriptor = -1, short events = 0)
	{
		(polling ? added : operations).emplace_back(Pending{std::move(operation), descriptor, events}, std::make_unique<HostWait>());
		if (descriptor >= 0 && watch_added)
			watch_added(descriptor, events);
	}
	// Told the descriptor and events of each operation added that names one, so that a process that sleeps on them sleeps on it too.
	void set_watch_added(std::function<void(int, short)> told) { watch_added = std::move(told); }
	// Gives every pending operation a chance to finish; true when any did.
	bool poll()
	{
		bool finished = false;
		polling = true;
		for (size_t index = 0; index < operations.size();) {
			if (operations[index].first.run()) {
				operations.erase(operations.begin() + ptrdiff_t(index));
				finished = true;
			} else {
				++index;
			}
		}
		polling = false;
		for (auto &operation : added)
			operations.push_back(std::move(operation));
		added.clear();
		return finished;
	}
	// The same for a waiting thread's condition, which is looked at after every call another thread makes: trying every operation again,
	// each a system call, then cost more than the calls themselves. A descriptor that became ready waits a fraction of a millisecond.
	bool look()
	{
		const auto now = std::chrono::steady_clock::now();
		if (operations.empty() || now - looked < std::chrono::microseconds(200))
			return false;
		looked = now;
		return poll();
	}
	size_t pending() const { return operations.size() + added.size(); }
	// The descriptors pending operations wait on.
	void watch(std::vector<pollfd> &descriptors) const
	{
		for (const auto &[operation, wait] : operations)
			if (operation.descriptor >= 0)
				descriptors.push_back({operation.descriptor, operation.events, 0});
	}

private:
	struct Pending {
		Operation run;
		int descriptor;
		short events;
	};
	std::vector<std::pair<Pending, std::unique_ptr<HostWait>>> operations, added;
	bool polling = false;
	std::chrono::steady_clock::time_point looked;
	std::function<void(int, short)> watch_added;
};

}
