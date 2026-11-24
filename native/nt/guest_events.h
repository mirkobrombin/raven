#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace raven {

// The events a guest sets and resets with its own ioctls on their ntsync objects (see the event preludes of preludes.S), through a table
// it shares with the runtime. The runtime stays the kernel half: it makes the objects and owns the handles, and puts in the table only a
// handle it lets the guest work, with a descriptor of the event's object in the guest's own descriptor table. Anything else the guest
// asks the runtime for, as before.
//
// The table is a header and a slot for each value of a handle's bits 2 to 13. The header's first word is set while the guest may work no
// event itself, its second counts what it did, its third is the address of the agent's kernel gate the guest makes its ioctls through. A slot is {handle, descriptor, users}: the guest counts itself among its users, looks again
// at the header and the handle, makes its ioctl and leaves. The runtime takes a slot back by clearing it and waiting until it has no
// users, so no ioctl of the guest reaches an object after the runtime has let it go.
class GuestEvents {
public:
	static constexpr size_t slots = 4096, slot_size = 16, header = 64, size = header + slots * slot_size;
	static constexpr size_t barred = 0, made = 8, gate = 16, slot_handle = 0, slot_descriptor = 8, slot_users = 12;
	static constexpr uint64_t paused = uint64_t(1) << 63;
	// `give` puts a host descriptor into the guest's table and gives its number there; `drop` closes one there.
	GuestEvents(uint8_t *table, std::function<int(int)> give, std::function<void(int)> drop);
	// The process's table once the runtime made it, or null: a guest that cannot work events itself has none.
	static GuestEvents *active();
	static void activate(std::unique_ptr<GuestEvents> events);
	static size_t slot_of(uint64_t handle) { return size_t(handle >> 2) % slots; }
	// What an offer for the handle is checked against: nothing when its slot is taken, else the count of times the slot was withdrawn,
	// which closing the handle in the meantime changes.
	std::optional<uint64_t> ticket(uint64_t handle) const;
	// Lets the guest work the event (a descriptor of its object that stays the caller's) under the handle, unless the slot was taken or
	// withdrawn since the ticket or `allowed`, asked under the table's lock once the guest holds the descriptor, says no.
	bool offer(uint64_t handle, const void *event, int descriptor, uint64_t ticket, const std::function<bool()> &allowed);
	// Takes back the slot of a handle, or every slot of an event. Their descriptors in the guest are closed by the next flush, which the
	// caller makes once it holds no lock of the object table: closing one there is a call of the guest's.
	void withdraw(uint64_t handle);
	void withdraw_event(const void *event);
	void flush();
	// Keeps the guest from working an event while the runtime looks at a wait for it by itself, which would not see a set made in the
	// guest: its slots stay, with their handles marked so the guest's comparison fails. Resuming clears the mark when `free` says, under
	// the table's lock, that nothing holds the event back any more.
	void pause_event(const void *event);
	void resume_event(const void *event, const std::function<bool()> &free);
	// While held, the guest works no event itself: a wait the runtime looks at by itself would not see what the guest set.
	void hold();
	void release();
	uint64_t made_by_guest() const;
	// The process is going and ends its threads wherever they are: no set of the guest counts any more, and nothing waits for a slot's
	// users, among whom may be a thread that was ended there.
	void shut();
	bool shut_down() const { return closing_down.load(std::memory_order_acquire); }

private:
	uint8_t *table;
	std::function<int(int)> give;
	std::function<void(int)> drop;
	mutable std::mutex lock;
	std::vector<const void *> owners = std::vector<const void *>(slots);
	std::vector<uint64_t> tickets = std::vector<uint64_t>(slots);
	std::vector<size_t> used;
	std::vector<int> closing;
	size_t holders = 0;
	std::atomic<bool> closing_down{false};
	uint64_t &word(size_t offset) const { return *reinterpret_cast<uint64_t *>(table + offset); }
	uint8_t *slot(size_t index) const { return table + header + index * slot_size; }
	void empty(size_t index);
	void wait_unused(size_t index) const;
};

}
