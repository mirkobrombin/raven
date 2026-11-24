#include "guest_events.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace raven {

static std::unique_ptr<GuestEvents> process_events;
static std::atomic<GuestEvents *> process_active{nullptr};

GuestEvents::GuestEvents(uint8_t *table, std::function<int(int)> give, std::function<void(int)> drop)
	: table(table), give(std::move(give)), drop(std::move(drop))
{
}

GuestEvents *GuestEvents::active()
{
	return process_active.load(std::memory_order_acquire);
}

void GuestEvents::activate(std::unique_ptr<GuestEvents> events)
{
	// How many sets and resets the guest made itself is told as its table goes, with RAVEN_TRACE_GUEST_EVENTS.
	if (process_events && std::getenv("RAVEN_TRACE_GUEST_EVENTS"))
		std::fprintf(stderr, "guest events: %llu sets and resets made by the guest\n", (unsigned long long)process_events->made_by_guest());
	process_active.store(events.get(), std::memory_order_release);
	process_events = std::move(events);
}

std::optional<uint64_t> GuestEvents::ticket(uint64_t handle) const
{
	const size_t index = slot_of(handle);
	std::lock_guard held(lock);
	if (std::atomic_ref(word(header + index * slot_size + slot_handle)).load(std::memory_order_relaxed))
		return std::nullopt;
	return tickets[index];
}

bool GuestEvents::offer(uint64_t handle, const void *event, int descriptor, uint64_t ticket, const std::function<bool()> &allowed)
{
	if (!handle)
		return false;
	const int guest = give(descriptor);
	if (guest < 0)
		return false;
	{
		const size_t index = slot_of(handle);
		std::lock_guard held(lock);
		std::atomic_ref holder(word(header + index * slot_size + slot_handle));
		if (tickets[index] == ticket && !holder.load(std::memory_order_relaxed) && allowed()) {
			std::atomic_ref(*reinterpret_cast<int32_t *>(slot(index) + slot_descriptor)).store(guest, std::memory_order_relaxed);
			holder.store(handle, std::memory_order_release);
			owners[index] = event;
			used.push_back(index);
			return true;
		}
	}
	drop(guest);
	return false;
}

void GuestEvents::withdraw(uint64_t handle)
{
	const size_t index = slot_of(handle);
	std::lock_guard held(lock);
	++tickets[index];
	if ((std::atomic_ref(word(header + index * slot_size + slot_handle)).load(std::memory_order_relaxed) & ~paused) == handle)
		empty(index);
}

void GuestEvents::withdraw_event(const void *event)
{
	std::lock_guard held(lock);
	for (const size_t index : std::vector<size_t>(used))
		if (owners[index] == event) {
			++tickets[index];
			empty(index);
		}
}

void GuestEvents::pause_event(const void *event)
{
	std::lock_guard held(lock);
	for (const size_t index : used)
		if (owners[index] == event) {
			std::atomic_ref holder(word(header + index * slot_size + slot_handle));
			holder.store(holder.load(std::memory_order_relaxed) | paused, std::memory_order_seq_cst);
			wait_unused(index);
		}
}

void GuestEvents::resume_event(const void *event, const std::function<bool()> &free)
{
	std::lock_guard held(lock);
	if (!free())
		return;
	for (const size_t index : used)
		if (owners[index] == event) {
			std::atomic_ref holder(word(header + index * slot_size + slot_handle));
			holder.store(holder.load(std::memory_order_relaxed) & ~paused, std::memory_order_release);
		}
}

void GuestEvents::flush()
{
	std::vector<int> closed;
	{
		std::lock_guard held(lock);
		closed.swap(closing);
	}
	for (const int descriptor : closed)
		drop(descriptor);
}

void GuestEvents::hold()
{
	std::lock_guard held(lock);
	if (holders++)
		return;
	// The guest counts itself among a slot's users before it looks at the header: once the header is set, a guest that did not see it
	// is among the users of its slot, and its ioctl is over when they are none.
	std::atomic_ref(word(barred)).store(1, std::memory_order_seq_cst);
	for (const size_t index : used)
		wait_unused(index);
}

void GuestEvents::release()
{
	std::lock_guard held(lock);
	if (holders && !--holders)
		std::atomic_ref(word(barred)).store(0, std::memory_order_release);
}

uint64_t GuestEvents::made_by_guest() const
{
	return std::atomic_ref(word(made)).load(std::memory_order_relaxed);
}

void GuestEvents::empty(size_t index)
{
	std::atomic_ref(word(header + index * slot_size + slot_handle)).store(0, std::memory_order_seq_cst);
	wait_unused(index);
	closing.push_back(std::atomic_ref(*reinterpret_cast<int32_t *>(slot(index) + slot_descriptor)).load(std::memory_order_relaxed));
	owners[index] = nullptr;
	used.erase(std::find(used.begin(), used.end(), index));
}

void GuestEvents::wait_unused(size_t index) const
{
	// A guest among the users is between two instructions of its prelude, or in the one ioctl: one the runtime stops there is moved out
	// of the window first (see NativeThreads), so the wait is for a thread that runs.
	std::atomic_ref users(*reinterpret_cast<uint32_t *>(slot(index) + slot_users));
	for (unsigned round = 0; users.load(std::memory_order_acquire); ++round)
		if (round < 1000)
			__builtin_ia32_pause();
		else
			std::this_thread::yield();
}

}
