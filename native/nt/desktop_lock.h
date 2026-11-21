#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <x86intrin.h>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "cpu/call_cycles.h"
#include "desktop/desktop_hold.h"

namespace raven {

inline thread_local const char *desktop_serving = nullptr;

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
		if (desktop_shared_depth)
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
		let_go();
	}
	// Takes it shared; a thread that holds it whole already has what it asks for. It does not count as an entry (see entries): what a
	// shared holder changes, it publishes itself.
	void enter_shared()
	{
		if (owner.load(std::memory_order_relaxed) == std::this_thread::get_id()) {
			++shared_within;
			return;
		}
		if (desktop_shared_depth++)
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
		if (desktop_shared_depth && !--desktop_shared_depth)
			pthread_rwlock_unlock(&lock);
	}
	// Whether the calling thread holds it shared and not whole.
	static bool held_shared() { return desktop_shared_depth != 0; }
	// Gives the lock up however deeply the calling thread holds it, and says how deeply, for resume.
	unsigned suspend()
	{
		if (owner.load(std::memory_order_relaxed) != std::this_thread::get_id())
			return 0;
		const unsigned held = depth;
		depth = 0;
		let_go();
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
		if (pthread_rwlock_trywrlock(&lock)) {
			const uint64_t started = __rdtsc();
			pthread_rwlock_wrlock(&lock);
			CallCycles::instance().waited_lock(__rdtsc() - started);
		}
		taken_at = __rdtsc();
	}
	// Gives it up whole; how long it was held goes to the call being served.
	void let_go()
	{
		owner.store(std::thread::id(), std::memory_order_relaxed);
		const uint64_t since = taken_at;
		pthread_rwlock_unlock(&lock);
		const uint64_t held = __rdtsc() - since;
		CallCycles::instance().held_lock(held);
		static const bool trace = std::getenv("RAVEN_TRACE_WHOLE");
		static std::atomic<int> lines{0};
		if (trace && held > 5000 && lines++ < 400)
			std::fprintf(stderr, "whole: %s %llu\n", desktop_serving ? desktop_serving : "(none)", (unsigned long long)held);
	}
	pthread_rwlock_t lock;
	std::atomic<std::thread::id> owner{};
	std::atomic<uint64_t> taken{0};
	unsigned depth = 0;
	// When its holder took it whole: only the holder reads or writes it.
	uint64_t taken_at = 0;
	// How many shared scopes the calling thread opened while it held it whole; how deeply it holds it shared is desktop_shared_depth.
	static inline thread_local unsigned shared_within = 0;
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

// The objects a call under the shared desktop lock works on, each behind one of a fixed set of locks chosen by its handle: two calls
// on the same context, bitmap or window take turns, calls on different ones mostly do not. A scope takes its stripes in their order, so
// two scopes never wait for each other in a circle; a stripe is held only inside one call, never over a callback.
class DesktopStripes {
public:
	explicit DesktopStripes(const std::vector<uint64_t> &handles)
	{
		for (const uint64_t handle : handles)
			if (handle)
				taken.push_back(stripe(handle));
		std::sort(taken.begin(), taken.end());
		taken.erase(std::unique(taken.begin(), taken.end()), taken.end());
		for (const size_t index : taken)
			if (!locks()[index].try_lock()) {
				const uint64_t started = __rdtsc();
				locks()[index].lock();
				CallCycles::instance().waited_lock(__rdtsc() - started);
			}
	}
	~DesktopStripes()
	{
		for (auto index = taken.rbegin(); index != taken.rend(); ++index)
			locks()[*index].unlock();
	}
	DesktopStripes(const DesktopStripes &) = delete;
	DesktopStripes &operator=(const DesktopStripes &) = delete;

private:
	// Enough that the few objects each of a handful of threads works on seldom share one.
	static constexpr size_t count = 1024;
	static size_t stripe(uint64_t handle) { return size_t((handle * 0x9e3779b97f4a7c15ULL) >> 54); }
	static std::array<std::mutex, count> &locks()
	{
		static std::array<std::mutex, count> made;
		return made;
	}
	std::vector<size_t> taken;
};

}
