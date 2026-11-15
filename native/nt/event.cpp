#include "event.h"
#include "shared_file.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <sys/stat.h>

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
	return shared ? shared->signaled.load() != 0 : local_signaled;
}

bool EventObject::take()
{
	if (type == 0)
		return is_set();
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
	if (take())
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

void EventObject::pulse_other_processes()
{
	std::lock_guard lock(mutex);
	if (!shared)
		return;
	shared->signaled.store(1);
	announce_shared_change();
}

int EventObject::set_state(bool signaled)
{
	std::lock_guard lock(mutex);
	const bool previous = shared ? shared->signaled.exchange(uint32_t(signaled)) != 0 : local_signaled;
	local_signaled = signaled;
	if (signaled) {
		if (type == 0) {
			for (const auto &waiter : waiters) waiter->ready = true;
			waiters.clear();
		} else if (!waiters.empty() && take()) {
			waiters.front()->ready = true;
			waiters.pop_front();
		}
		changed.notify_all();
	}
	if (shared)
		announce_shared_change();
	return previous;
}

}
