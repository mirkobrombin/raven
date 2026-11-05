#pragma once

#include "child_process.h"
#include "objects.h"

#include <deque>
#include <functional>
#include <memory>
#include <poll.h>
#include <vector>

namespace raven {

struct CompletionPacket {
	uint64_t bytes = 0, key = 0, overlapped = 0;
	// The Windows error of the operation, zero when it succeeded.
	uint32_t status = 0;
};

// An I/O completion port: a queue of finished operations that threads take from.
class CompletionPort : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"IoCompletion"; }
	void post(const CompletionPacket &packet) { packets.push_back(packet); }
	bool take(CompletionPacket &packet)
	{
		if (packets.empty())
			return false;
		packet = packets.front();
		packets.pop_front();
		return true;
	}
	bool empty() const { return packets.empty(); }

private:
	std::deque<CompletionPacket> packets;
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
	}
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
};

}
