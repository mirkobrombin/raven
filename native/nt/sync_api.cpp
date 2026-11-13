#include "kernel_context.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <tuple>

namespace raven {

namespace {

// CRITICAL_SECTION (x64): debug info, lock count, recursion count, owning thread, semaphore, spin count.
constexpr uint64_t lock_count = 8, recursion_count = 12, owning_thread = 16, spin_count = 32;

struct Waiter {
	bool woken = false;
};

// A keyed event a program created for itself; its keys are apart from those of the process-wide one and of any other.
class KeyedEventObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"KeyedEvent"; }
};

}

void register_sync_api(ApiRegistry &registry, KernelContext &context)
{
	auto &threads = context.threads;
	const auto me = [&threads] { return uint64_t(threads.current_id()); };
	const auto initialize = [](ApiCall &call) {
		std::vector<uint8_t> bytes(40);
		const int32_t unlocked = -1;
		std::memcpy(bytes.data() + lock_count, &unlocked, 4);
		call.write(call[0], bytes);
		call.ret(1);
	};
	registry.add("InitializeCriticalSection", 1, initialize);
	registry.add("InitializeCriticalSectionAndSpinCount", 2, [initialize](ApiCall &call) {
		initialize(call);
		call.store<uint64_t>(call[0] + spin_count, call[1] & 0x00ffffff);
	});
	registry.add("InitializeCriticalSectionEx", 3, [initialize](ApiCall &call) {
		if ((call.u32(2) & 0xff000000U) & ~0x1f000000U) {
			call.fail(87);
			return;
		}
		initialize(call);
		if (call.u32(2) & 0x01000000U)
			call.store_pointer(call[0], UINT64_MAX);
		call.store<uint64_t>(call[0] + spin_count, call[1] & 0x00ffffff);
	});
	registry.add("DeleteCriticalSection", 1, [](ApiCall &call) {
		call.store<uint64_t>(call[0] + owning_thread, 0);
		call.store<int32_t>(call[0] + lock_count, -1);
		call.store<int32_t>(call[0] + recursion_count, 0);
	});
	// A section is free when nothing owns it; an owner nests, anyone else waits for the owner to leave.
	const auto take = [me](ApiCall &call) {
		const auto owner = call.load<uint64_t>(call[0] + owning_thread);
		const auto recursion = call.load<int32_t>(call[0] + recursion_count);
		if (recursion > 0 && owner == me()) {
			call.store<int32_t>(call[0] + recursion_count, recursion + 1);
			call.store<int32_t>(call[0] + lock_count, call.load<int32_t>(call[0] + lock_count) + 1);
			return true;
		}
		if (recursion > 0 && owner)
			return false;
		call.store<uint64_t>(call[0] + owning_thread, me());
		call.store<int32_t>(call[0] + recursion_count, 1);
		call.store<int32_t>(call[0] + lock_count, 0);
		return true;
	};
	registry.add("EnterCriticalSection", 1, [take, &context, &threads](ApiCall &call) {
		const uint64_t section = call[0];
		while (!take(call)) {
			threads.block([&context, section] {
				return context.process.read_memory(section + recursion_count, 4) == std::vector<uint8_t>(4, 0) ||
					context.process.read_memory(section + owning_thread, 8) == std::vector<uint8_t>(8, 0);
			}, std::nullopt);
		}
	});
	registry.add("TryEnterCriticalSection", 1, [take](ApiCall &call) { call.ret(take(call)); });
	registry.add("LeaveCriticalSection", 1, [](ApiCall &call) {
		const auto recursion = call.load<int32_t>(call[0] + recursion_count);
		if (recursion <= 0)
			return;
		call.store<int32_t>(call[0] + recursion_count, recursion - 1);
		if (recursion == 1) {
			call.store<uint64_t>(call[0] + owning_thread, 0);
			call.store<int32_t>(call[0] + lock_count, -1);
		} else {
			call.store<int32_t>(call[0] + lock_count, call.load<int32_t>(call[0] + lock_count) - 1);
		}
	});

	// SRW locks are one pointer-sized word: 1 is exclusive, shared holders count in steps of 2.
	const auto acquire_exclusive = [&context, &threads](ApiCall &call) {
		const uint64_t lock = call[0];
		const size_t width = call.pointer_width();
		while (call.process.compare_exchange(lock, 0, 1, width))
			threads.block([&context, lock, width] { return context.process.read_memory(lock, width) == std::vector<uint8_t>(width, 0); }, std::nullopt);
	};
	const auto acquire_shared = [&context, &threads](ApiCall &call) {
		const uint64_t lock = call[0];
		for (;;) {
			const auto value = call.load_pointer(lock);
			if (value & 1)
				threads.block([&context, lock] { return !(context.process.read_memory(lock, 1)[0] & 1); }, std::nullopt);
			else if (call.process.compare_exchange(lock, value, value + 2, call.pointer_width()) == value)
				return;
		}
	};
	const auto release_exclusive = [](ApiCall &call) { call.process.compare_exchange(call[0], 1, 0, call.pointer_width()); };
	const auto release_shared = [](ApiCall &call) {
		for (;;) {
			const auto value = call.load_pointer(call[0]);
			if (value < 2 || call.process.compare_exchange(call[0], value, value - 2, call.pointer_width()) == value)
				return;
		}
	};
	registry.add("InitializeSRWLock", 1, [](ApiCall &call) { call.store_pointer(call[0], 0); });
	registry.add("AcquireSRWLockExclusive", 1, acquire_exclusive);
	// The guest component takes a free lock itself and asks here, under the ntdll names, only for one that is held.
	registry.add("RtlAcquireSRWLockExclusive", 1, acquire_exclusive);
	registry.add("TryAcquireSRWLockExclusive", 1, [](ApiCall &call) {
		call.ret(call.process.compare_exchange(call[0], 0, 1, call.pointer_width()) == 0);
	});
	registry.add("ReleaseSRWLockExclusive", 1, release_exclusive);
	registry.add("AcquireSRWLockShared", 1, acquire_shared);
	registry.add("RtlAcquireSRWLockShared", 1, acquire_shared);
	registry.add("TryAcquireSRWLockShared", 1, [](ApiCall &call) {
		for (;;) {
			const auto value = call.load_pointer(call[0]);
			if (value & 1) {
				call.ret(0);
				return;
			}
			if (call.process.compare_exchange(call[0], value, value + 2, call.pointer_width()) == value) {
				call.ret(1);
				return;
			}
		}
	});
	registry.add("ReleaseSRWLockShared", 1, release_shared);

	// Condition variables keep their waiters host-side, keyed by the variable's address. The variable's word holds how many sleep on it,
	// so a guest can see that a wake has nobody to wake without asking.
	auto variables = std::make_shared<std::map<uint64_t, std::vector<std::shared_ptr<Waiter>>>>();
	const auto sleep_on = [&threads, variables](ApiCall &call, std::function<void()> release, std::function<void()> reacquire, uint32_t milliseconds) {
		auto waiter = std::make_shared<Waiter>();
		(*variables)[call[0]].push_back(waiter);
		call.store_pointer(call[0], (*variables)[call[0]].size());
		release();
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (milliseconds != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		threads.block([waiter] { return waiter->woken; }, wake);
		auto &list = (*variables)[call[0]];
		list.erase(std::remove(list.begin(), list.end(), waiter), list.end());
		call.store_pointer(call[0], list.size());
		reacquire();
		if (!waiter->woken) {
			call.fail(1460);
			return;
		}
		call.ret(1);
	};
	registry.add("InitializeConditionVariable", 1, [](ApiCall &call) { call.store_pointer(call[0], 0); });
	registry.add("SleepConditionVariableSRW", 4, [sleep_on, acquire_exclusive, acquire_shared, release_exclusive, release_shared](ApiCall &call) {
		const bool shared = call.u32(3) & 1;
		const uint64_t lock = call[1];
		auto sub = ApiCall(call.process, call.environment, {lock});
		sleep_on(call, [&] { if (shared) release_shared(sub); else release_exclusive(sub); },
			[&] { if (shared) acquire_shared(sub); else acquire_exclusive(sub); }, call.u32(2));
	});
	registry.add("SleepConditionVariableCS", 3, [sleep_on, take, &threads, &context](ApiCall &call) {
		const uint64_t section = call[1];
		if (call.pointer_width() == 4 || context.modules.all().contains("raven-rtl.dll")) {
			// A 32-bit section is not laid out in guest memory, and a 64-bit one belongs to the guest RTL when it is loaded: leaving and
			// entering it goes through the program's own calls.
			const auto leave = context.modules.resolve({"kernel32.dll", "LeaveCriticalSection", 0, false, 0});
			const auto enter = context.modules.resolve({"kernel32.dll", "EnterCriticalSection", 0, false, 0});
			const uint64_t arguments[] = {section};
			sleep_on(call, [&] { context.call(leave, arguments); }, [&] { context.call(enter, arguments); }, call.u32(2));
			return;
		}
		auto sub = ApiCall(call.process, call.environment, {section});
		int32_t saved = 0;
		sleep_on(call, [&] {
			saved = sub.load<int32_t>(section + recursion_count);
			sub.store<uint64_t>(section + owning_thread, 0);
			sub.store<int32_t>(section + recursion_count, 0);
			sub.store<int32_t>(section + lock_count, -1);
		}, [&] {
			while (!take(sub))
				threads.block([&context, section] { return context.process.read_memory(section + recursion_count, 4) == std::vector<uint8_t>(4, 0); }, std::nullopt);
			sub.store<int32_t>(section + recursion_count, saved);
		}, call.u32(2));
	});
	const auto wake = [variables, &threads](ApiCall &call, bool all) {
		auto &list = (*variables)[call[0]];
		bool woke = false;
		for (auto &waiter : list)
			if (!waiter->woken) {
				waiter->woken = woke = true;
				if (!all)
					break;
			}
		// The thread woken runs before the waker goes on, as it would on another processor: a renderer's worker answers the thread
		// that waits for its frame without first waiting for every other thread's turn.
		if (woke && !call.process.serving())
			threads.hand_over();
	};
	registry.add("WakeConditionVariable", 1, [wake](ApiCall &call) { wake(call, false); });
	registry.add("WakeAllConditionVariable", 1, [wake](ApiCall &call) { wake(call, true); });
	registry.add("RtlWakeConditionVariable", 1, [wake](ApiCall &call) { wake(call, false); });
	registry.add("RtlWakeAllConditionVariable", 1, [wake](ApiCall &call) { wake(call, true); });

	// INIT_ONCE is one word: 0 untouched, 1 running, otherwise the context pointer with low bits 2.
	registry.add("InitOnceInitialize", 1, [](ApiCall &call) { call.store_pointer(call[0], 0); });
	registry.add("InitOnceBeginInitialize", 4, [&context, &threads](ApiCall &call) {
		const uint32_t flags = call.u32(1);
		if (flags & ~3U) {
			call.fail(87);
			return;
		}
		const uint64_t once = call[0];
		auto state = call.load_pointer(once);
		if ((state & 3) == 1 && !(flags & 1)) {
			threads.block([&context, once] { return (context.process.read_memory(once, 1)[0] & 3) != 1; }, std::nullopt);
			state = call.load_pointer(once);
		}
		if ((state & 3) == 2) {
			call.store<int32_t>(call[2], 0);
			if (call[3]) call.store_pointer(call[3], state & ~3ULL);
			call.ret(1);
			return;
		}
		if (flags & 1) {
			call.fail(1627);
			return;
		}
		call.store_pointer(once, 1);
		call.store<int32_t>(call[2], 1);
		call.ret(1);
	});
	registry.add("InitOnceComplete", 3, [](ApiCall &call) {
		const uint32_t flags = call.u32(1);
		if (call.load_pointer(call[0]) != 1 || (flags & 4 && call[2])) {
			call.fail(87);
			return;
		}
		call.store_pointer(call[0], flags & 4 ? 0 : (call[2] & ~3ULL) | 2);
		call.ret(1);
	});
	registry.add("InitOnceExecuteOnce", 4, [&](ApiCall &call) {
		const uint64_t once = call[0];
		auto state = call.load_pointer(once);
		if ((state & 3) == 1) {
			threads.block([&context, once] { return (context.process.read_memory(once, 1)[0] & 3) != 1; }, std::nullopt);
			state = call.load_pointer(once);
		}
		if ((state & 3) == 2) {
			if (call[3]) call.store_pointer(call[3], state & ~3ULL);
			call.ret(1);
			return;
		}
		call.store_pointer(once, 1);
		const auto scratch = context.allocator.allocate(16);
		if (!scratch) {
			call.fail(8);
			return;
		}
		const uint64_t arguments[] = {once, call[2], scratch};
		const auto result = context.call(call[1], arguments);
		if (result.stop != NativeStop::returned)
			throw result;
		const auto produced = call.load_pointer(scratch);
		context.allocator.release(scratch);
		if (!uint32_t(result.value)) {
			call.store_pointer(once, 0);
			call.fail(1627);
			return;
		}
		call.store_pointer(once, (produced & ~3ULL) | 2);
		if (call[3]) call.store_pointer(call[3], produced);
		call.ret(1);
	});

	// WaitOnAddress watches memory directly; wakeups are implicit because waiting threads re-check the value.
	registry.add("WaitOnAddress", 4, [&context, &threads](ApiCall &call) {
		const size_t size = call[2];
		if (size != 1 && size != 2 && size != 4 && size != 8) {
			call.fail(87);
			return;
		}
		const uint64_t address = call[0];
		const auto expected = call.read(call[1], size);
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (call.u32(3) != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(call.u32(3));
		const bool changed = threads.block([&context, address, size, expected] { return context.process.read_memory(address, size) != expected; }, wake);
		if (!changed) {
			call.fail(1460);
			return;
		}
		call.ret(1);
	});
	// A thread whose address changed meanwhile runs before the waker goes on, as for a condition variable.
	const auto wake_address = [&threads](ApiCall &call) {
		if (!call.process.serving())
			threads.hand_over();
	};
	registry.add("WakeByAddressSingle", 1, wake_address);
	registry.add("WakeByAddressAll", 1, wake_address);

	// An NT timeout: none waits for ever, a negative one is relative and a positive one absolute, both in 100 ns units.
	const auto nt_timeout = [](ApiCall &call, uint64_t address) -> std::optional<std::chrono::steady_clock::time_point> {
		if (!address)
			return std::nullopt;
		const int64_t value = call.load<int64_t>(address);
		auto delay = std::chrono::nanoseconds(0);
		if (value < 0) {
			delay = std::chrono::nanoseconds(-value * 100);
		} else if (value > 0) {
			const auto since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch());
			const int64_t now = since_epoch.count() / 100 + int64_t(filetime_epoch_offset);
			delay = std::chrono::nanoseconds(std::max<int64_t>(0, value - now) * 100);
		}
		return std::chrono::steady_clock::now() + delay;
	};
	// RtlWaitOnAddress: the ntdll form of WaitOnAddress, answering with an NTSTATUS and taking an NT timeout.
	registry.add("RtlWaitOnAddress", 4, [&context, &threads, nt_timeout](ApiCall &call) {
		const size_t size = call[2];
		if (size != 1 && size != 2 && size != 4 && size != 8) {
			call.ret(0xc000000d);
			return;
		}
		const uint64_t address = call[0];
		const auto expected = call.read(call[1], size);
		const bool changed = threads.block([&context, address, size, expected] { return context.process.read_memory(address, size) != expected; },
			nt_timeout(call, call[3]));
		call.ret(changed ? 0 : 0x102);
	});
	registry.add("RtlWakeAddressSingle", 1, wake_address);
	registry.add("RtlWakeAddressAll", 1, wake_address);

	// Keyed events: a release meets one wait on the same key, whichever comes first waiting for the other. A null handle names the
	// process-wide keyed event that critical sections and slim locks use; a key is an address within the event it is used on.
	struct KeyedParty {
		bool met = false;
	};
	using KeyedSlot = std::tuple<const KernelObject *, bool, uint64_t>;
	auto keyed = std::make_shared<std::map<KeyedSlot, std::deque<std::shared_ptr<KeyedParty>>>>();
	const auto rendezvous = [&context, &threads, keyed, nt_timeout](ApiCall &call, bool releasing) {
		constexpr uint32_t success = 0, timeout = 0x102, invalid_handle = 0xc0000008;
		std::shared_ptr<KeyedEventObject> event;
		if (call[0] && !(event = context.objects.object_as<KeyedEventObject>(call[0]))) {
			call.ret(invalid_handle);
			return;
		}
		const KernelObject *space = event.get();
		const uint64_t key = call[1];
		// One who arrived on the other side first is met at once.
		auto &others = (*keyed)[{space, !releasing, key}];
		if (!others.empty()) {
			others.front()->met = true;
			others.pop_front();
			call.ret(success);
			return;
		}
		const auto wake = nt_timeout(call, call[3]);
		auto party = std::make_shared<KeyedParty>();
		auto &mine = (*keyed)[{space, releasing, key}];
		mine.push_back(party);
		threads.block([party] { return party->met; }, wake);
		if (!party->met) {
			auto &list = (*keyed)[{space, releasing, key}];
			list.erase(std::remove(list.begin(), list.end(), party), list.end());
			call.ret(timeout);
			return;
		}
		call.ret(success);
	};
	registry.add("NtWaitForKeyedEvent", 4, [rendezvous](ApiCall &call) { rendezvous(call, false); });
	registry.add("NtReleaseKeyedEvent", 4, [rendezvous](ApiCall &call) { rendezvous(call, true); });
	// NtCreateKeyedEvent(handle out, access, attributes, flags) and NtOpenKeyedEvent(handle out, access, attributes): named keyed events
	// belong to the object directory, where the only one is the process-wide event the null handle already names.
	const auto make_keyed = [&context](ApiCall &call) {
		const auto made = context.objects.add_object(std::make_shared<KeyedEventObject>());
		if (made.status) {
			call.ret(0xc000009a);
			return;
		}
		call.store_pointer(call[0], made.value);
		call.ret(0);
	};
	registry.add("NtCreateKeyedEvent", 4, make_keyed);
	registry.add("ZwCreateKeyedEvent", 4, make_keyed);
	registry.add("NtOpenKeyedEvent", 3, make_keyed);
	registry.add("ZwOpenKeyedEvent", 3, make_keyed);
}

}
