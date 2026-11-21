#pragma once

#include <atomic>
#include <x86intrin.h>
#include <mutex>
#include <thread>

#include "cpu/call_cycles.h"

namespace raven {

// The lock of the desktop's state, USER's and GDI's alike: the windows and their queues, and the objects they share with drawing. Windows
// keeps USER under one lock as well. The host thread that holds the runtime's turn takes it for each call it serves, and a host thread
// that answers a call of its own guest thread beside the turn takes it alone. Whoever holds it lets it go entirely over a guest callback
// and whenever it gives the turn up, and takes it back after, as Windows leaves USER over a callback: the turn is never waited for while
// the lock is held, and the lock is only ever taken after the turn.
class DesktopLock {
public:
	void enter()
	{
		const auto me = std::this_thread::get_id();
		if (owner.load(std::memory_order_relaxed) == me) {
			++depth;
			return;
		}
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
		mutex.unlock();
	}
	// Gives the lock up however deeply the calling thread holds it, and says how deeply, for resume.
	unsigned suspend()
	{
		if (owner.load(std::memory_order_relaxed) != std::this_thread::get_id())
			return 0;
		const unsigned held = depth;
		depth = 0;
		owner.store(std::thread::id(), std::memory_order_relaxed);
		mutex.unlock();
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
	// Takes the mutex; what it waited while another thread held it goes to the call being served (see CallCycles).
	void take()
	{
		if (mutex.try_lock())
			return;
		const uint64_t started = __rdtsc();
		mutex.lock();
		CallCycles::instance().waited_lock(__rdtsc() - started);
	}
	std::mutex mutex;
	std::atomic<std::thread::id> owner{};
	std::atomic<uint64_t> taken{0};
	unsigned depth = 0;
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

// Lets it go over a scope, a guest callback, and takes it back as deeply as before.
struct DesktopRelease {
	unsigned held = desktop_lock().suspend();
	DesktopRelease() = default;
	~DesktopRelease() { desktop_lock().resume(held); }
	DesktopRelease(const DesktopRelease &) = delete;
	DesktopRelease &operator=(const DesktopRelease &) = delete;
};

}
