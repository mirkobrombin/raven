#pragma once

#include <atomic>
#include <x86intrin.h>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <thread>

#include "cpu/call_cycles.h"

namespace raven {

// The lock of the desktop's state, USER's and GDI's alike: the windows and their queues, and the objects they share with drawing. Windows
// keeps USER under one lock as well. The host thread that holds the runtime's turn takes it for each call it serves, and a host thread
// that answers a call of its own guest thread beside the turn takes it alone. Whoever holds it lets it go entirely over a guest callback
// and whenever it gives the turn up, and takes it back after, as Windows leaves USER over a callback: the turn is never waited for while
// the lock is held, and the lock is only ever taken after the turn.
//
// A call beside the turn that only works on objects its thread names, and changes nothing else, takes the lock shared instead, with the
// stripes of those objects (see DesktopStripes): threads drawing into bitmaps of their own, or changing the data of windows of their own,
// go on at once. Whatever adds, removes or reaches across objects takes it whole, and waits for the shared holders to finish. A thread
// that holds it shared never asks for it whole.
class DesktopLock {
public:
	DesktopLock()
	{
		// Whoever waits to take it whole goes before new shared holders, or threads drawing without a pause would keep it from the turn.
		pthread_rwlockattr_t attributes;
		pthread_rwlockattr_init(&attributes);
		pthread_rwlockattr_setkind_np(&attributes, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
		pthread_rwlock_init(&lock, &attributes);
		pthread_rwlockattr_destroy(&attributes);
	}
	~DesktopLock() { pthread_rwlock_destroy(&lock); }
	DesktopLock(const DesktopLock &) = delete;
	DesktopLock &operator=(const DesktopLock &) = delete;

	void enter()
	{
		const auto me = std::this_thread::get_id();
		if (owner.load(std::memory_order_relaxed) == me) {
			++depth;
			return;
		}
		if (shared_depth)
			throw std::logic_error("the desktop lock is asked for whole while it is held shared");
		take();
		owner.store(me, std::memory_order_relaxed);
		depth = 1;
		taken.fetch_add(1, std::memory_order_release);
	}
	void leave()
	{
		// A scope that unwinds after its thread gave the lock up (see suspend) has nothing left to give.
		if (owner.load(std::memory_order_relaxed) != std::this_thread::get_id())
			return;
		if (--depth)
			return;
		owner.store(std::thread::id(), std::memory_order_relaxed);
		pthread_rwlock_unlock(&lock);
	}
	// Takes it shared; a thread that holds it whole already has what it asks for. It does not count as an entry (see entries): what a
	// shared holder changes, it publishes itself.
	void enter_shared()
	{
		if (owner.load(std::memory_order_relaxed) == std::this_thread::get_id()) {
			++shared_within;
			return;
		}
		if (shared_depth++)
			return;
		if (pthread_rwlock_tryrdlock(&lock)) {
			const uint64_t started = __rdtsc();
			pthread_rwlock_rdlock(&lock);
			CallCycles::instance().waited_lock(__rdtsc() - started);
		}
	}
	void leave_shared()
	{
		if (shared_within) {
			--shared_within;
			return;
		}
		if (shared_depth && !--shared_depth)
			pthread_rwlock_unlock(&lock);
	}
	// Whether the calling thread holds it shared and not whole.
	static bool held_shared() { return shared_depth != 0; }
	// Gives the lock up however deeply the calling thread holds it, and says how deeply, for resume.
	unsigned suspend()
	{
		if (owner.load(std::memory_order_relaxed) != std::this_thread::get_id())
			return 0;
		const unsigned held = depth;
		depth = 0;
		owner.store(std::thread::id(), std::memory_order_relaxed);
		pthread_rwlock_unlock(&lock);
		return held;
	}
	void resume(unsigned held)
	{
		if (!held)
			return;
		take();
		owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
		depth = held;
		taken.fetch_add(1, std::memory_order_release);
	}
	// How many times the lock was taken: the desktop cannot have changed while this stays the same.
	uint64_t entries() const { return taken.load(std::memory_order_acquire); }

private:
	// Takes it whole; what it waited while others held it goes to the call being served (see CallCycles).
	void take()
	{
		if (!pthread_rwlock_trywrlock(&lock))
			return;
		const uint64_t started = __rdtsc();
		pthread_rwlock_wrlock(&lock);
		CallCycles::instance().waited_lock(__rdtsc() - started);
	}
	pthread_rwlock_t lock;
	std::atomic<std::thread::id> owner{};
	std::atomic<uint64_t> taken{0};
	unsigned depth = 0;
	// How deeply the calling thread holds it shared, and how many shared scopes it opened while it held it whole.
	static inline thread_local unsigned shared_depth = 0, shared_within = 0;
};

inline DesktopLock &desktop_lock()
{
	static DesktopLock lock;
	return lock;
}

// Holds the desktop lock over a scope.
struct DesktopScope {
	DesktopScope() { desktop_lock().enter(); }
	~DesktopScope() { desktop_lock().leave(); }
	DesktopScope(const DesktopScope &) = delete;
	DesktopScope &operator=(const DesktopScope &) = delete;
};

// Holds it shared over a scope.
struct DesktopSharedScope {
	DesktopSharedScope() { desktop_lock().enter_shared(); }
	~DesktopSharedScope() { desktop_lock().leave_shared(); }
	DesktopSharedScope(const DesktopSharedScope &) = delete;
	DesktopSharedScope &operator=(const DesktopSharedScope &) = delete;
};

// Lets it go over a scope, a guest callback, and takes it back as deeply as before.
struct DesktopRelease {
	unsigned held = desktop_lock().suspend();
	DesktopRelease() = default;
	~DesktopRelease() { desktop_lock().resume(held); }
	DesktopRelease(const DesktopRelease &) = delete;
	DesktopRelease &operator=(const DesktopRelease &) = delete;
};

}
