#include "event.h"
#include "guest_events.h"
#include "kernel_waits.h"
#include "shared_file.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <sys/stat.h>
#include <utility>

namespace raven {

std::optional<uint32_t> event_access(uint32_t requested)
{
	if (requested & ~0xf21f0003U)
		return std::nullopt;
	uint32_t access = requested & 0x1f0003;
	if (requested & 0x80000000U) access |= 0x120001;
	if (requested & 0x40000000U) access |= 0x120002;
	if (requested & 0x20000000U) access |= 0x120000;
	if (requested & (0x10000000U | 0x02000000U)) access |= 0x1f0003;
	return access;
}

struct EventObject::Shared {
	std::atomic<uint32_t> signaled;
	uint32_t type;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free);

// A private event's state is the process's table's, where its waits are handed the event as it is set.
EventObject::EventObject(uint32_t type, bool signaled) : type(type), local_signaled(signaled),
	waits_object(raven_waits_create(kernel_waits(), type == 0 ? raven_manual_event : raven_auto_event, signaled, 0))
{
}

EventObject::EventObject(int descriptor) : type(0), local_signaled(false), descriptor(descriptor)
{
	try {
		struct stat information{};
		if (descriptor < 0 || ::fstat(descriptor, &information) || information.st_size != sizeof(Shared))
			throw std::runtime_error("invalid event storage");
		void *mapped = ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
		if (mapped == MAP_FAILED)
			throw std::runtime_error("cannot map event storage");
		shared = static_cast<Shared *>(mapped);
		if (shared->type > 1)
			throw std::runtime_error("invalid event type");
		type = shared->type;
		listen_shared_changes();
		++shared_object_count;
	} catch (...) {
		if (shared) ::munmap(shared, sizeof(Shared));
		if (descriptor >= 0) ::close(descriptor);
		throw;
	}
}

EventObject::~EventObject()
{
	if (waits_object)
		raven_waits_close(kernel_waits(), waits_object);
	if (shared) {
		::munmap(shared, sizeof(Shared));
		--shared_object_count;
	}
	if (descriptor >= 0) ::close(descriptor);
}

bool EventObject::describe(InheritedHandle &record) const
{
	std::lock_guard lock(mutex);
	if (!shared) {
		const int created = ::memfd_create("raven-event", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (created < 0)
			return false;
		if (::ftruncate(created, sizeof(Shared)) ||
			::fcntl(created, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) {
			::close(created);
			return false;
		}
		void *mapped = ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, created, 0);
		if (mapped == MAP_FAILED) {
			::close(created);
			return false;
		}
		// The event leaves the table with its state; the waits there for it end, to wait again on the shared one. The guest gives it up
		// first: a set it made on the object afterwards would be lost.
		guest_gone.store(true, std::memory_order_seq_cst);
		if (auto *const events = GuestEvents::active())
			events->withdraw_event(this);
		if (waits_object) {
			uint32_t state = 0;
			raven_waits_detach(kernel_waits(), std::exchange(waits_object, 0), &state, nullptr, nullptr);
			local_signaled = state != 0;
		}
		shared = new (mapped) Shared{{uint32_t(local_signaled)}, type};
		descriptor = created;
		listen_shared_changes();
		++shared_object_count;
		changed.notify_all();
	}
	record.kind = "event";
	record.descriptor = descriptor;
	return true;
}

bool EventObject::is_set() const
{
	uint32_t state = 0;
	if (waits_object)
		return raven_waits_query(kernel_waits(), waits_object, &state) == 0 && state;
	return shared ? shared->signaled.load() != 0 : local_signaled;
}

bool EventObject::take(uint32_t thread)
{
	if (type == 0)
		return is_set();
	if (waits_object)
		return raven_waits_wait(kernel_waits(), thread, &waits_object, 1, 0, 0, 0) == 0;
	if (shared)
		return shared->signaled.exchange(0) != 0;
	const bool previous = local_signaled;
	local_signaled = false;
	return previous;
}

int EventObject::signaled(uint32_t thread) const
{
	std::lock_guard lock(mutex);
	return is_set() || std::find(pulses.begin(), pulses.end(), thread) != pulses.end();
}

void EventObject::consume(uint32_t thread)
{
	try_consume(thread);
}

bool EventObject::try_consume(uint32_t thread)
{
	std::lock_guard lock(mutex);
	if (take(thread))
		return true;
	return std::erase(pulses, thread) != 0;
}

void EventObject::grant_pulse(uint32_t thread)
{
	std::lock_guard lock(mutex);
	if (std::find(pulses.begin(), pulses.end(), thread) == pulses.end())
		pulses.push_back(thread);
}

void EventObject::revoke_pulse(uint32_t thread)
{
	std::lock_guard lock(mutex);
	std::erase(pulses, thread);
}

bool EventObject::pulse_host_waiters()
{
	std::lock_guard lock(mutex);
	if (waiters.empty())
		return false;
	if (type == 0) {
		for (const auto &waiter : waiters) waiter->ready = true;
		waiters.clear();
	} else {
		waiters.front()->ready = true;
		waiters.pop_front();
	}
	changed.notify_all();
	return type != 0;
}

bool EventObject::pulse_table_waiters()
{
	std::lock_guard lock(mutex);
	uint32_t granted = 0;
	return waits_object && raven_waits_pulse(kernel_waits(), waits_object, nullptr, &granted) == 0 && granted && type != 0;
}

std::atomic<uint32_t> *EventObject::wait_word() const
{
	std::lock_guard lock(mutex);
	return shared ? &shared->signaled : nullptr;
}

uint32_t EventObject::table_object() const
{
	std::lock_guard lock(mutex);
	return waits_object;
}

bool EventObject::guest_may_work() const
{
	return !guest_gone.load(std::memory_order_acquire) && !guest_holds.load(std::memory_order_acquire);
}

void EventObject::hold_from_guest()
{
	guest_holds.fetch_add(1, std::memory_order_seq_cst);
	if (auto *const events = GuestEvents::active())
		events->pause_event(this);
}

void EventObject::give_back_to_guest()
{
	if (guest_holds.fetch_sub(1, std::memory_order_acq_rel) != 1)
		return;
	offer_tried.store(false, std::memory_order_release);
	if (auto *const events = GuestEvents::active())
		events->resume_event(this, [this] { return guest_may_work(); });
}

void EventObject::pulse_other_processes()
{
	std::lock_guard lock(mutex);
	if (!shared)
		return;
	shared->signaled.store(1);
	wake_word(&shared->signaled);
	announce_shared_change();
}

int EventObject::set_state(bool signaled)
{
	std::lock_guard lock(mutex);
	bool previous = shared ? shared->signaled.exchange(uint32_t(signaled)) != 0 : local_signaled;
	local_signaled = signaled;
	if (waits_object) {
		// The host's own waiters (see waiters) look first, as they did before the event was set in the table.
		uint32_t before = 0;
		if (signaled && type != 0 && !waiters.empty()) {
			previous = is_set();
			waiters.front()->ready = true;
			waiters.pop_front();
			changed.notify_all();
			return previous;
		}
		signaled ? raven_waits_set(kernel_waits(), waits_object, &before) : raven_waits_reset(kernel_waits(), waits_object, &before);
		previous = before != 0;
	}
	if (signaled) {
		if (type == 0) {
			for (const auto &waiter : waiters) waiter->ready = true;
			waiters.clear();
		} else if (!waiters.empty() && take(0)) {
			waiters.front()->ready = true;
			waiters.pop_front();
		}
		changed.notify_all();
	}
	if (shared) {
		if (signaled)
			wake_word(&shared->signaled);
		announce_shared_change();
	}
	return previous;
}

}
