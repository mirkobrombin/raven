#include "child_process.h"
#include "kernel_context.h"
#include "mutex.h"
#include "event.h"
#include "security_attributes.h"
#include "shared_file.h"
#include "text.h"

#include <algorithm>
#include <chrono>
#include <map>

namespace raven {

namespace {

constexpr uint64_t current_thread_pseudo = UINT64_MAX - 1;
constexpr uint32_t wait_timeout = 0x102, wait_failed = UINT32_MAX, wait_io_completion = 0xc0, wait_abandoned = 0x80;

uint32_t thread_token_error(uint32_t status)
{
	switch (status) {
	case 0xc0000005U: return 998;
	case 0xc0000022U: return 5;
	case 0xc0000008U: case 0xc0000024U: return 6;
	case 0xc000007cU: return 1008;
	case 0xc00000a5U: return 1346;
	case 0xc00000a6U: return 1347;
	case 0xc00000a8U: return 1349;
	case 0xc00000bbU: return 50;
	default: return 87;
	}
}

uint32_t thread_access(uint32_t requested)
{
	uint32_t access = requested & 0x1fffff;
	if (requested & (0x10000000U | 0x02000000U)) access |= 0x1fffff;
	if (requested & 0x80000000U) access |= 0x20048;
	if (requested & 0x40000000U) access |= 0x20037;
	if (requested & 0x20000000U) access |= 0x120000;
	if (access & 0x40) access |= 0x800;
	if (access & 0x20) access |= 0x400;
	return access;
}

struct SemaphoreState {
	std::atomic<int32_t> count{0};
	int32_t maximum = 0;
};

class SemaphoreObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"Semaphore"; }
	int signaled(uint32_t) const override { return state->count.load() > 0; }
	bool try_consume(uint32_t) override
	{
		auto current = state->count.load();
		while (current > 0)
			if (state->count.compare_exchange_weak(current, current - 1))
				return true;
		return false;
	}
	void consume(uint32_t thread) override { try_consume(thread); }
	SemaphoreState local;
	SemaphoreState *state = &local;
	std::shared_ptr<SharedFile> shared;
};

// A named event: its state is shared memory, so a program in another process of the prefix sees it set.
struct EventState {
	std::atomic<uint32_t> signaled{0};
	uint32_t manual = 0;
};

// A waitable timer: signaled from its due time on, a notification timer until it is set again, a synchronization timer until one
// wait takes it. A periodic timer moves its due time on by its period each time it fires.
class WaitableTimerObject : public KernelObject {
public:
	explicit WaitableTimerObject(bool manual) : manual(manual) {}
	std::u16string_view type_name() const override { return u"Timer"; }
	int signaled(uint32_t) const override
	{
		const auto now = std::chrono::steady_clock::now();
		if (due && now >= *due) {
			state = true;
			if (period.count() > 0) {
				while (*due <= now)
					*due += period;
			} else {
				due.reset();
			}
		}
		return state ? 1 : 0;
	}
	void consume(uint32_t) override
	{
		if (!manual)
			state = false;
	}
	std::optional<std::chrono::steady_clock::time_point> wakes_at() const override { return due; }
	void set(std::chrono::steady_clock::time_point when, std::chrono::milliseconds every)
	{
		state = false;
		due = when;
		period = every;
	}
	void cancel() { due.reset(); }

private:
	bool manual;
	mutable bool state = false;
	mutable std::optional<std::chrono::steady_clock::time_point> due;
	std::chrono::milliseconds period{0};
};

class SharedEventObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"Event"; }
	int signaled(uint32_t) const override { return state->signaled.load() ? 1 : 0; }
	bool try_consume(uint32_t) override { return state->manual || state->signaled.exchange(0) == 1; }
	void consume(uint32_t thread) override { try_consume(thread); }
	int set_state(bool value) override
	{
		const int previous = int(state->signaled.exchange(value ? 1 : 0));
		if (value)
			announce_shared_change();
		return previous;
	}
	EventState *state = nullptr;
	std::shared_ptr<SharedFile> shared;
};

// A registered wait: the callback runs on a guest thread once the object is signaled or the time runs out.
class WaitRegistration : public KernelObject {
public:
	uint64_t handle = 0, callback = 0, context = 0;
	uint32_t timeout = 0, flags = 0;
	std::chrono::steady_clock::time_point deadline;
	bool active = true;
	std::shared_ptr<GuestThread> running;
};

// A timer of a timer queue: its callback runs on a guest thread when it is due, and again every period until it is deleted.
class QueueTimer : public KernelObject {
public:
	uint64_t handle = 0, callback = 0, parameter = 0;
	uint32_t period = 0, flags = 0;
	std::chrono::steady_clock::time_point deadline;
	bool active = true;
	std::shared_ptr<GuestThread> running;
};

class TimerQueue : public KernelObject {
public:
	std::vector<std::shared_ptr<QueueTimer>> timers;
};

class WorkObject : public KernelObject {
public:
	uint64_t callback = 0, parameter = 0;
	std::vector<std::shared_ptr<GuestThread>> running;
};

// A private pool only bounds its threads; work items run on guest threads whichever pool they name.
class PoolObject : public KernelObject {
public:
	uint32_t minimum = 0, maximum = 500;
};

// The handles of the work items made with a cleanup group in their environment, closed together with it.
class CleanupGroupObject : public KernelObject {
public:
	std::vector<uint64_t> members;
};

}

void register_thread_api(ApiRegistry &registry, KernelContext &context)
{
	auto &threads = context.threads;
	auto &objects = context.objects;
	// Thread handles are per object; one thread object per guest thread keeps handle identity stable.
	auto handle_of = std::make_shared<std::map<uint32_t, std::shared_ptr<ThreadObject>>>();
	const auto thread_from = [&threads, &objects](uint64_t handle) -> std::shared_ptr<GuestThread> {
		if (handle == current_thread_pseudo || handle == 0xfffffffeULL)
			return threads.find(threads.current_id());
		const auto object = objects.object_as<ThreadObject>(handle);
		return object ? object->thread : nullptr;
	};

	const auto run_apcs = [&context, &threads]() {
		auto &me = threads.current();
		bool ran = false;
		while (!me.apcs.empty()) {
			const auto apc = me.apcs.front();
			me.apcs.pop_front();
			const auto arguments = apc.arguments.empty() ? std::vector<uint64_t>{apc.parameter} : apc.arguments;
			const auto result = context.call(apc.routine, arguments);
			if (result.stop != NativeStop::returned)
				throw result;
			ran = true;
		}
		return ran;
	};
	// Waits for any or all of the handles; returns the Windows wait code.
	const auto wait = [&objects, &threads, run_apcs](ApiCall &call, const std::vector<uint64_t> &handles, bool all, uint32_t milliseconds, bool alertable) -> uint32_t {
		const uint32_t id = threads.current_id();
		const auto *me = &threads.current();
		if (handles.empty() || handles.size() > 64) {
			call.set_last_error(87);
			return wait_failed;
		}
		std::vector<uint64_t> resolved = handles;
		for (auto &handle : resolved)
			if (handle == current_thread_pseudo) {
				call.set_last_error(6);
				return wait_failed;
			}
		const auto states = [&]() {
			std::vector<int> result;
			for (const auto handle : resolved)
				result.push_back(objects.signaled(handle, id));
			return result;
		};
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (milliseconds != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		const auto ready = [&] {
			const auto now = states();
			if (alertable && !me->apcs.empty())
				return true;
			return all ? std::all_of(now.begin(), now.end(), [](int state) { return state == 1; }) :
				std::any_of(now.begin(), now.end(), [](int state) { return state == 1; });
		};
		bool looked = false;
		for (;;) {
			const auto current = states();
			for (const auto state : current)
				if (state < 0) {
					call.set_last_error(state == -3 ? 5 : 6);
					return wait_failed;
				}
			if (all) {
				if (std::all_of(current.begin(), current.end(), [](int state) { return state == 1; })) {
					bool taken = true;
					for (const auto handle : resolved)
						taken = objects.try_consume(handle, id) && taken;
					if (taken)
						return 0;
				}
			} else {
				for (size_t index = 0; index < current.size(); ++index)
					if (current[index] == 1 && objects.try_consume(resolved[index], id))
						return uint32_t(index);
			}
			if (milliseconds == 0 || (wake && std::chrono::steady_clock::now() >= *wake)) {
				if (!(alertable && !threads.current().apcs.empty())) {
					// A wait that has run out still takes in once what the host finished meanwhile: a program that only polls with a zero
					// timeout would otherwise never see a pipe or a socket transfer complete.
					if (!looked) {
						looked = true;
						if (threads.block(ready, std::chrono::steady_clock::now()))
							continue;
					}
					return wait_timeout;
				}
			}
			// Running APCs needs the thread stopped, and so does blocking in a serial process, where the guest that spins in the stub holds
			// the only task. A parallel guest spins in its own task while this host frame blocks, so a wait the other side ends soon never
			// stops the thread.
			if (call.process.serving() && (!call.process.parallel() || (alertable && !threads.current().apcs.empty()))) {
				call.process.decline_served();
				return 0;
			}
			if (alertable && !threads.current().apcs.empty() && run_apcs())
				return wait_io_completion;
			if (milliseconds == 0 || (wake && std::chrono::steady_clock::now() >= *wake))
				return wait_timeout;
			// A timer among the handles turns signaled at its due time with nobody announcing it: the wait looks again then.
			auto until = wake;
			for (const auto handle : resolved)
				if (const auto object = objects.object(handle))
					if (const auto due = object->wakes_at(); due && (!until || *due < *until))
						until = due;
			threads.block(ready, until);
		}
	};
	registry.add("WaitForSingleObject", 2, [wait](ApiCall &call) { call.ret(wait(call, {call[0]}, false, call.u32(1), false)); });
	registry.add("WaitForSingleObjectEx", 3, [wait](ApiCall &call) { call.ret(wait(call, {call[0]}, false, call.u32(1), call.u32(2))); });
	// A handle array holds handles of the program's width; the 32-bit pseudo handles widen to the values the 64-bit side knows them by.
	const auto handle_at = [](ApiCall &call, uint64_t array, uint32_t index) -> uint64_t {
		if (call.pointer_width() == 8)
			return call.load<uint64_t>(array + index * 8ULL);
		const uint32_t value = call.load<uint32_t>(array + index * 4ULL);
		return value >= 0xfffffffaU ? uint64_t(int64_t(int32_t(value))) : value;
	};
	const auto multiple = [wait, handle_at](ApiCall &call, bool alertable) {
		const uint32_t count = call.u32(0);
		if (count > 64 || !call[1]) {
			call.fail(87, wait_failed);
			return;
		}
		std::vector<uint64_t> handles(count);
		for (uint32_t index = 0; index < count; ++index)
			handles[index] = handle_at(call, call[1], index);
		call.ret(wait(call, handles, call.u32(2), call.u32(3), alertable));
	};
	registry.add("WaitForMultipleObjects", 4, [multiple](ApiCall &call) { multiple(call, false); });
	registry.add("WaitForMultipleObjectsEx", 5, [multiple](ApiCall &call) { multiple(call, call.u32(4)); });
	// The input event follows the explicit handles, so a signaled handle wins a wait-any tie.
	const auto message_wait = [&context, &objects, &threads, handle_at, run_apcs](ApiCall &call, uint32_t count, uint64_t array,
			uint32_t milliseconds, uint32_t mask, uint32_t flags) -> uint32_t {
		if (count > 63 || (count && !array) || (flags & ~7U) || (mask & ~0x1dffU)) {
			call.set_last_error(87);
			return wait_failed;
		}
		std::vector<uint64_t> handles(count);
		for (uint32_t index = 0; index < count; ++index) {
			handles[index] = handle_at(call, array, index);
			if (std::find(handles.begin(), handles.begin() + index, handles[index]) != handles.begin() + index) {
				call.set_last_error(87);
				return wait_failed;
			}
		}
		const uint32_t id = threads.current_id();
		const auto me = threads.find(id);
		const bool all = flags & 1, alertable = flags & 2, available = flags & 4;
		const auto input = [&] {
			const auto status = context.queue_status ? context.queue_status(id, mask, false) : 0;
			return (available ? status >> 16 : status & 0xffffU) != 0;
		};
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (milliseconds != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		for (;;) {
			std::vector<int> states;
			for (const auto handle : handles) {
				const int state = handle == current_thread_pseudo ? -2 : objects.signaled(handle, id);
				if (state < 0) {
					call.set_last_error(state == -3 ? 5 : 6);
					return wait_failed;
				}
				states.push_back(state);
			}
			if (all) {
				if (std::all_of(states.begin(), states.end(), [](int state) { return state == 1; }) && input()) {
					bool taken = true;
					for (const auto handle : handles) taken = objects.try_consume(handle, id) && taken;
					if (taken) return 0;
				}
			} else {
				for (size_t index = 0; index < states.size(); ++index)
					if (states[index] == 1 && objects.try_consume(handles[index], id)) return uint32_t(index);
				if (input()) return count;
			}
			if (alertable && !me->apcs.empty() && run_apcs()) return wait_io_completion;
			if (!milliseconds || (wake && std::chrono::steady_clock::now() >= *wake)) return wait_timeout;
			auto until = wake;
			for (const auto handle : handles)
				if (const auto object = objects.object(handle))
					if (const auto due = object->wakes_at(); due && (!until || *due < *until)) until = due;
			HostWait waiting;
			threads.block([&] {
				if (alertable && !me->apcs.empty()) return true;
				bool any = false, every = true;
				for (const auto handle : handles) {
					const int state = objects.signaled(handle, id);
					if (state < 0) return true;
					any |= state == 1;
					every &= state == 1;
				}
				return all ? every && input() : any || input();
			}, until);
		}
	};
	registry.add("MsgWaitForMultipleObjects", 5, [message_wait](ApiCall &call) {
		call.ret(message_wait(call, call.u32(0), call[1], call.u32(3), call.u32(4), call.u32(2) ? 1 : 0));
	});
	registry.add("MsgWaitForMultipleObjectsEx", 5, [message_wait](ApiCall &call) {
		call.ret(message_wait(call, call.u32(0), call[1], call.u32(2), call.u32(3), call.u32(4)));
	});
	registry.add("WaitMessage", 0, [message_wait](ApiCall &call) {
		call.ret(message_wait(call, 0, 0, UINT32_MAX, 0x1cff, 0) == wait_failed ? 0 : 1);
	});
	registry.add("SignalObjectAndWait", 4, [wait, &objects](ApiCall &call) {
		if (objects.signaled(call[0], 0) == -2) {
			call.fail(6, wait_failed);
			return;
		}
		call.ret(wait(call, {call[1]}, false, call.u32(2), call.u32(3)));
	});
	registry.add("Sleep", 1, [&threads](ApiCall &call) {
		const uint32_t milliseconds = call.u32(0);
		if (!milliseconds) {
			threads.yield();
			return;
		}
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (milliseconds != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		threads.block([] { return false; }, wake);
	});
	registry.add("SleepEx", 2, [&threads, run_apcs](ApiCall &call) {
		const uint32_t milliseconds = call.u32(0);
		const bool alertable = call.u32(1);
		if (alertable && run_apcs()) {
			call.ret(wait_io_completion);
			return;
		}
		if (!milliseconds) {
			threads.yield();
			return;
		}
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (milliseconds != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		const auto *me = &threads.current();
		threads.block([me, alertable] { return alertable && !me->apcs.empty(); }, wake);
		if (alertable && run_apcs())
			call.ret(wait_io_completion);
	});
	registry.add("SwitchToThread", 0, [&threads](ApiCall &call) { call.ret(threads.yield()); });

	registry.add("CreateThread", 6, [&, handle_of](ApiCall &call) {
		const uint32_t flags = call.u32(4);
		if (flags & ~(0x4U | 0x10000U)) {
			call.fail(87);
			return;
		}
		std::shared_ptr<GuestThread> thread;
		try {
			// STACK_SIZE_PARAM_IS_A_RESERVATION makes the size the reservation; otherwise it is only the commit, rounded up to a megabyte,
			// inside the program's own reservation.
			const uint64_t size = flags & 0x10000U ? call[1] : std::max(threads.default_stack_reserve, (call[1] + 0xfffff) & ~uint64_t(0xfffff));
			thread = threads.create(call[2], call[3], size ? size : 1024 * 1024, flags & 4);
		} catch (const std::runtime_error &) {
			call.fail(8);
			return;
		}
		auto object = std::make_shared<ThreadObject>(thread);
		const auto made = objects.add_object(object);
		if (made.status) {
			threads.terminate(thread, 0);
			call.fail(8);
			return;
		}
		(*handle_of)[thread->id] = object;
		if (call[5]) call.store<uint32_t>(call[5], thread->id);
		call.ret(made.value);
	});
	registry.add("ExitThread", 1, [&threads](ApiCall &call) { threads.exit_current(call.u32(0)); });
	registry.add("FreeLibraryAndExitThread", 2, [&threads](ApiCall &call) { threads.exit_current(call.u32(1)); });
	// A work item runs on a thread of its own, whose handle nobody keeps.
	registry.add("QueueUserWorkItem", 3, [&registry, &objects](ApiCall &call) {
		const auto create = registry.find("CreateThread");
		ApiCall inner(call.process, call.environment, {0, 0, call[0], call[1], 0, 0});
		create->second(inner);
		if (!inner.result) {
			call.ret(0);
			return;
		}
		objects.close(inner.result);
		call.ret(1);
	});
	registry.add("GetExitCodeThread", 2, [thread_from, &objects](ApiCall &call) {
		if (const auto remote = objects.object_as<RemoteThreadObject>(call[0])) {
			call.store<uint32_t>(call[1], remote->code().value_or(259));
			call.ret(1);
			return;
		}
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.fail(6);
			return;
		}
		call.store<uint32_t>(call[1], thread->state == GuestThread::State::done ? thread->exit_code : 259);
		call.ret(1);
	});
	registry.add("TerminateThread", 2, [thread_from, &threads](ApiCall &call) {
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.fail(6);
			return;
		}
		threads.terminate(thread, call.u32(1));
		call.ret(1);
	});
	registry.add("SuspendThread", 1, [thread_from, &threads](ApiCall &call) {
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.fail(6, UINT32_MAX);
			return;
		}
		const auto previous = threads.suspend(*thread);
		if (previous == UINT32_MAX)
			call.fail(5, UINT32_MAX);
		else
			call.ret(previous);
	});
	registry.add("ResumeThread", 1, [thread_from, &objects](ApiCall &call) {
		// The main thread of a child that was started suspended runs when this opens its gate.
		if (const auto child = objects.object_as<ChildThreadObject>(call[0])) {
			call.ret(child->resume());
			return;
		}
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.fail(6, UINT32_MAX);
			return;
		}
		const auto previous = thread->suspend_count;
		if (thread->suspend_count > 0)
			--thread->suspend_count;
		call.ret(uint32_t(previous));
	});
	// The registers of a thread as a CONTEXT (x64): the control and integer groups, from where the thread stopped.
	registry.add("GetThreadContext", 2, [thread_from, &threads](ApiCall &call) {
		if (call.pointer_width() == 4 || !call[1]) {
			call.fail(call[1] ? 50 : 87);
			return;
		}
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.fail(6);
			return;
		}
		const auto &registers = thread->id == threads.current_id() ? call.process.context().registers : thread->snapshot.context.registers;
		const uint32_t flags = call.load<uint32_t>(call[1] + 0x30);
		const auto put = [&](size_t at, uint64_t value) { call.store<uint64_t>(call[1] + at, value); };
		if (flags & 0x1) {
			call.store<uint16_t>(call[1] + 0x38, uint16_t(registers.cs));
			call.store<uint16_t>(call[1] + 0x42, uint16_t(registers.ss));
			call.store<uint32_t>(call[1] + 0x44, uint32_t(registers.eflags));
			put(0x98, registers.rsp);
			put(0xa0, registers.rbp);
			put(0xf8, registers.rip);
		}
		if (flags & 0x2) {
			put(0x78, registers.rax); put(0x80, registers.rcx); put(0x88, registers.rdx); put(0x90, registers.rbx);
			put(0xa8, registers.rsi); put(0xb0, registers.rdi);
			put(0xb8, registers.r8); put(0xc0, registers.r9); put(0xc8, registers.r10); put(0xd0, registers.r11);
			put(0xd8, registers.r12); put(0xe0, registers.r13); put(0xe8, registers.r14); put(0xf0, registers.r15);
		}
		call.ret(1);
	});
	// The registers of a stopped thread are written back the same way: a hook installed in another thread replaces the instruction it
	// runs next, which the thread finds in its own context when it is let go again.
	registry.add("SetThreadContext", 2, [thread_from, &threads](ApiCall &call) {
		if (call.pointer_width() == 4 || !call[1]) {
			call.fail(call[1] ? 50 : 87);
			return;
		}
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.fail(6);
			return;
		}
		// Only what a stopped thread will run with again can be written: the thread serving this call keeps running with the registers of
		// the call itself, which the return would overwrite.
		if (thread->id == threads.current_id() || thread->suspend_count == 0) {
			call.fail(5);
			return;
		}
		const uint32_t flags = call.load<uint32_t>(call[1] + 0x30);
		auto &registers = thread->snapshot.context.registers;
		const auto get = [&](size_t at) { return call.load<uint64_t>(call[1] + at); };
		if (flags & 0x1) {
			registers.cs = call.load<uint16_t>(call[1] + 0x38);
			registers.ss = call.load<uint16_t>(call[1] + 0x42);
			registers.eflags = call.load<uint32_t>(call[1] + 0x44);
			registers.rsp = get(0x98);
			registers.rbp = get(0xa0);
			registers.rip = get(0xf8);
		}
		if (flags & 0x2) {
			registers.rax = get(0x78); registers.rcx = get(0x80); registers.rdx = get(0x88); registers.rbx = get(0x90);
			registers.rsi = get(0xa8); registers.rdi = get(0xb0);
			registers.r8 = get(0xb8); registers.r9 = get(0xc0); registers.r10 = get(0xc8); registers.r11 = get(0xd0);
			registers.r12 = get(0xd8); registers.r13 = get(0xe0); registers.r14 = get(0xe8); registers.r15 = get(0xf0);
		}
		call.ret(1);
	});
	// A thread's description is a name the program gives it; it is kept per thread and handed back in memory the program frees with LocalFree.
	auto descriptions = std::make_shared<std::map<uint32_t, std::u16string>>();
	registry.add("SetThreadDescription", 2, [thread_from, descriptions](ApiCall &call) {
		const auto thread = thread_from(call[0]);
		if (!thread || !call[1]) {
			call.ret(thread ? 0x80070057U : 0x80070006U);
			return;
		}
		(*descriptions)[thread->id] = call.wide(call[1], 4096);
		call.ret(0);
	});
	registry.add("GetThreadDescription", 2, [thread_from, descriptions, &context](ApiCall &call) {
		const auto thread = thread_from(call[0]);
		if (!thread || !call[1]) {
			call.ret(thread ? 0x80070057U : 0x80070006U);
			return;
		}
		const auto found = descriptions->find(thread->id);
		const std::u16string text = found == descriptions->end() ? std::u16string() : found->second;
		const auto memory = context.allocator.allocate((text.size() + 1) * 2, true);
		if (!memory) {
			call.ret(0x8007000eU);
			return;
		}
		call.write(memory, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size() * 2));
		call.store_pointer(call[1], memory);
		call.ret(0);
	});
	registry.add("GetThreadPriorityBoost", 2, [](ApiCall &call) {
		if (call[1])
			call.store<uint32_t>(call[1], 0);
		call.ret(1);
	});
	registry.add("SetThreadPriorityBoost", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("SetThreadInformation", 4, [](ApiCall &call) { call.ret(1); });
	const auto set_thread_token = [thread_from, &objects, &threads](uint64_t handle, uint64_t token_handle) -> uint32_t {
		const auto thread = thread_from(handle);
		const auto child = objects.object_as<ChildThreadObject>(handle);
		if (!thread && !child)
			return 0xc0000008U;
		if (handle != current_thread_pseudo && handle != 0xfffffffeULL && !(objects.access_of(handle) & 0x80))
			return 0xc0000022U;
		const auto token = token_handle ? objects.object_as<AccessToken>(token_handle) : nullptr;
		if (token_handle && !token)
			return 0xc0000008U;
		if (token && !(objects.access_of(token_handle) & 4))
			return 0xc0000022U;
		if (token && token->data().type != 2)
			return 0xc00000a8U;
		if (thread) {
			if (thread->state == GuestThread::State::done)
				return 0xc000004bU;
			thread->impersonation = token;
			return 0;
		}
		const auto owner = child->owner();
		if (owner->exited())
			return 0xc000004bU;
		if (!owner->channel)
			return 0xc00000bbU;
		InheritedHandle request;
		request.kind = "process:thread-token-set";
		request.flags = 1;
		if (token) {
			InheritedHandle record;
			token->describe(record);
			record.handle_access = objects.access_of(token_handle);
			const auto made = owner->channel->send(objects, threads, std::move(record), 0);
			if (made.status) return made.status;
			request.handle = made.value;
		}
		const auto answer = owner->channel->request_process(objects, threads, std::move(request));
		return answer ? answer->access : 0xc0000001U;
	};
	registry.add("NtSetInformationThread", 4, [set_thread_token](ApiCall &call) {
		if (call.u32(1) != 5) { call.ret(0xc0000003U); return; }
		if (call.u32(3) != call.pointer_width()) { call.ret(0xc0000004U); return; }
		try {
			call.ret(set_thread_token(call[0], call.load_pointer(call[2])));
		} catch (const ApiFault &) { call.ret(0xc0000005U); }
	});
	registry.add("SetThreadToken", 2, [set_thread_token](ApiCall &call) {
		try {
			const auto status = set_thread_token(call[0] ? call.load_pointer(call[0]) : current_thread_pseudo, call[1]);
			if (status) call.fail(thread_token_error(status));
			else call.ret(1);
		} catch (const ApiFault &) { call.fail(998); }
	});
	registry.add("RevertToSelf", 0, [&threads](ApiCall &call) {
		threads.current().impersonation.reset();
		call.ret(1);
	});
	registry.add("ImpersonateSelf", 1, [&context, &threads](ApiCall &call) {
		if (call.u32(0) > 3) { call.fail(87); return; }
		auto token = std::make_shared<AccessToken>(*context.process_token);
		token->set_type(2);
		token->set_impersonation_level(call.u32(0));
		threads.current().impersonation = std::move(token);
		call.ret(1);
	});
	registry.add("ImpersonateLoggedOnUser", 1, [&objects, &threads](ApiCall &call) {
		auto token = objects.object_as<AccessToken>(call[0]);
		if (!token) { call.fail(6); return; }
		const auto data = token->data();
		const uint32_t needed = data.type == 1 ? 0xa : 0xc;
		if ((objects.access_of(call[0]) & needed) != needed) { call.fail(5); return; }
		if (data.type == 2 && data.impersonation_level < 2) { call.fail(1346); return; }
		if (data.type == 1) {
			token = std::make_shared<AccessToken>(*token);
			token->set_type(2);
			token->set_impersonation_level(2);
		}
		threads.current().impersonation = std::move(token);
		call.ret(1);
	});
	const auto open_thread_token = [thread_from, &objects, &threads](ApiCall &call, bool native, uint32_t attributes, size_t output) {
		const auto fail = [&](uint32_t status) {
			if (native) call.ret(status);
			else call.fail(thread_token_error(status));
		};
		try {
			call.process.validate_write_memory(call[output], call.pointer_width());
			const auto thread = thread_from(call[0]);
			const auto child = objects.object_as<ChildThreadObject>(call[0]);
			if (!thread && !child) { fail(0xc0000008U); return; }
			if (call[0] != current_thread_pseudo && call[0] != 0xfffffffeULL && !(objects.access_of(call[0]) & 0x840)) {
				fail(0xc0000022U);
				return;
			}
			const auto access = token_access(call.u32(1));
			if (!access || (attributes & ~2U)) { fail(0xc000000dU); return; }
			std::shared_ptr<AccessToken> token;
			if (thread) {
				token = thread->impersonation;
			} else {
				const auto owner = child->owner();
				if (owner->exited() || !owner->channel) { fail(0xc0000008U); return; }
				InheritedHandle request;
				request.kind = "process:thread-token-open";
				request.flags = 1;
				const auto answer = owner->channel->request_process(objects, threads, std::move(request));
				if (!answer || answer->access || answer->descriptor < 0) {
					if (answer && answer->descriptor >= 0) ::close(answer->descriptor);
					fail(answer && answer->access ? answer->access : 0xc0000001U);
					return;
				}
				token = std::make_shared<AccessToken>(answer->descriptor);
			}
			if (!token) { fail(0xc000007cU); return; }
			if (!token->data().impersonation_level) { fail(0xc00000a6U); return; }
			const auto caller = threads.current().impersonation;
			if (!call.u32(2) && caller && caller->data().impersonation_level < 2) { fail(0xc00000a5U); return; }
			const auto made = objects.add_object(token);
			if (made.status) { fail(made.status); return; }
			objects.set_access(made.value, *access);
			objects.set_inherit(made.value, (attributes & 2) != 0);
			call.store_pointer(call[output], made.value);
			call.ret(native ? 0 : 1);
		} catch (const ApiFault &) { fail(0xc0000005U); }
		catch (const std::runtime_error &) { fail(0xc0000005U); }
	};
	registry.add("OpenThreadToken", 4, [open_thread_token](ApiCall &call) { open_thread_token(call, false, 0, 3); });
	registry.add("NtOpenThreadToken", 4, [open_thread_token](ApiCall &call) { open_thread_token(call, true, 0, 3); });
	registry.add("NtOpenThreadTokenEx", 5, [open_thread_token](ApiCall &call) { open_thread_token(call, true, call.u32(3), 4); });
	// NtQueryInformationThread(thread, class, buffer, length, written): THREAD_BASIC_INFORMATION (class 0) with the thread's TEB and
	// client id, and whether I/O is pending for it (ThreadIsIoPending, 16), which it never is when asked.
	registry.add("NtQueryInformationThread", 5, [thread_from, &objects](ApiCall &call) {
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.ret(0xc0000008U);
			return;
		}
		if (call[0] != current_thread_pseudo && call[0] != 0xfffffffeULL && !(objects.access_of(call[0]) & 0x840)) {
			call.ret(0xc0000022U);
			return;
		}
		const uint32_t type = call.u32(1);
		const size_t width = call.pointer_width();
		std::vector<uint8_t> value;
		if (type == 0) {
			value.assign(width == 8 ? 48 : 28, 0);
			const auto put = [&](size_t at, uint64_t word, size_t size) { std::memcpy(value.data() + at, &word, size); };
			put(0, thread->state == GuestThread::State::done ? 0 : 0x103, 4);
			put(width == 8 ? 8 : 4, thread->teb, width);
			put(width == 8 ? 16 : 8, ::getpid(), width);
			put(width == 8 ? 24 : 12, thread->id, width);
			put(width == 8 ? 32 : 16, 1, width);
			put(width == 8 ? 40 : 20, 8, 4);
			put(width == 8 ? 44 : 24, 8, 4);
		} else if (type == 16) {
			value.assign(4, 0);
		} else {
			call.ret(0xc0000003U);
			return;
		}
		if (call.u32(3) != value.size()) {
			call.ret(0xc0000004U);
			return;
		}
		try {
			call.process.validate_write_memory(call[2], value.size());
			if (call[4])
				call.process.validate_write_memory(call[4], 4);
			call.write(call[2], value);
			if (call[4])
				call.store<uint32_t>(call[4], uint32_t(value.size()));
			call.ret(0);
		} catch (const std::runtime_error &) {
			call.ret(0xc0000005U);
		}
	});
	registry.add("GetThreadId", 1, [&objects, thread_from](ApiCall &call) {
		if (objects.object_as<ChildThreadObject>(call[0])) {
			call.ret(1);
			return;
		}
		const auto thread = thread_from(call[0]);
		call.ret(thread ? thread->id : 0);
		if (!thread) call.set_last_error(6);
	});
	// No thread here has I/O requests outstanding in the kernel's sense: overlapped operations complete on their own.
	registry.add("GetThreadIOPendingFlag", 2, [thread_from](ApiCall &call) {
		if (!thread_from(call[0])) {
			call.fail(6, 0);
			return;
		}
		call.store<uint32_t>(call[1], 0);
		call.ret(1);
	});
	registry.add("GetThreadPriority", 1, [thread_from](ApiCall &call) {
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.fail(6, 0x7fffffff);
			return;
		}
		call.ret(uint32_t(thread->priority));
	});
	registry.add("SetThreadPriority", 2, [thread_from](ApiCall &call) {
		const auto thread = thread_from(call[0]);
		if (!thread) {
			call.fail(6);
			return;
		}
		thread->priority = call.i32(1);
		call.ret(1);
	});
	registry.add("SetThreadAffinityMask", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("SetThreadStackGuarantee", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("OpenThread", 3, [&threads, &objects, handle_of](ApiCall &call) {
		const auto thread = threads.find(call.u32(2));
		if (!thread) {
			call.fail(87);
			return;
		}
		auto &object = (*handle_of)[thread->id];
		if (!object)
			object = std::make_shared<ThreadObject>(thread);
		const auto made = objects.add_object(object);
		if (!made.status) {
			objects.set_access(made.value, thread_access(call.u32(0)));
			objects.set_inherit(made.value, call.u32(1) != 0);
		}
		call.ret(made.status ? 0 : made.value);
	});
	registry.add("NtOpenThread", 4, [&threads, &objects, handle_of](ApiCall &call) {
		try {
			call.process.validate_write_memory(call[0], call.pointer_width());
			if (!call[3]) {
				call.ret(0xc000000dU);
				return;
			}
			uint32_t attributes = 0;
			if (call[2]) {
				const size_t width = call.pointer_width(), root_at = width == 8 ? 8 : 4;
				if (call.load<uint32_t>(call[2]) != (width == 8 ? 48 : 24) ||
					call.load_pointer(call[2] + root_at) || call.load_pointer(call[2] + root_at + width)) {
					call.ret(0xc000000dU);
					return;
				}
				attributes = call.load<uint32_t>(call[2] + root_at + 2 * width);
				if (attributes & ~0x42U) {
					call.ret(0xc00000bbU);
					return;
				}
			}
			const auto process = call.load_pointer(call[3]), id = call.load_pointer(call[3] + call.pointer_width());
			const auto thread = id <= UINT32_MAX ? threads.find(uint32_t(id)) : nullptr;
			if ((process && process != uint32_t(::getpid())) || !thread) {
				call.ret(0xc000000bU);
				return;
			}
			if (call.u32(1) & ~0xf21fffffU) {
				call.ret(0xc0000022U);
				return;
			}
			auto &object = (*handle_of)[thread->id];
			if (!object)
				object = std::make_shared<ThreadObject>(thread);
			const auto made = objects.add_object(object);
			if (!made.status) {
				objects.set_access(made.value, thread_access(call.u32(1)));
				objects.set_inherit(made.value, (attributes & 2) != 0);
				call.store_pointer(call[0], made.value);
			}
			call.ret(made.status);
		} catch (const ApiFault &) {
			call.ret(0xc0000005U);
		} catch (const std::runtime_error &) {
			call.ret(0xc0000005U);
		}
	});
	registry.add("QueueUserAPC", 3, [thread_from](ApiCall &call) {
		const auto thread = thread_from(call[1]);
		if (!thread) {
			call.fail(6);
			return;
		}
		thread->apcs.push_back({call[0], call[2]});
		call.ret(1);
	});

	// Waitable timers live in the process; a named one is found again by its name while a handle to it is open. A completion routine
	// is accepted and not run: the timer still signals its waiters.
	auto named_timers = std::make_shared<std::map<std::u16string, std::weak_ptr<WaitableTimerObject>>>();
	const auto create_timer = [&objects, named_timers](ApiCall &call, std::u16string name, bool manual, uint32_t access) {
		if (!name.empty())
			if (const auto found = named_timers->find(name); found != named_timers->end())
				if (const auto timer = found->second.lock()) {
					const auto made = objects.add_object(timer);
					if (made.status) {
						call.fail(8, 0);
						return;
					}
					call.set_last_error(183);
					call.ret(made.value);
					return;
				}
		auto timer = std::make_shared<WaitableTimerObject>(manual);
		const auto made = objects.add_object(timer);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		if (access)
			objects.set_access(made.value, access);
		if (!name.empty())
			(*named_timers)[name] = timer;
		call.set_last_error(0);
		call.ret(made.value);
	};
	const auto text = [](ApiCall &call, uint64_t address, bool wide) {
		return !address ? std::u16string() : wide ? call.wide(address, 259) : from_utf8(call.narrow(address, 259));
	};
	registry.add("CreateWaitableTimerW", 3, [create_timer, text](ApiCall &call) { create_timer(call, text(call, call[2], true), call.u32(1) != 0, 0); });
	registry.add("CreateWaitableTimerA", 3, [create_timer, text](ApiCall &call) { create_timer(call, text(call, call[2], false), call.u32(1) != 0, 0); });
	// CreateWaitableTimerEx(attributes, name, flags, access): CREATE_WAITABLE_TIMER_MANUAL_RESET is 1; the high resolution flag needs
	// nothing more here.
	registry.add("CreateWaitableTimerExW", 4, [create_timer, text](ApiCall &call) {
		create_timer(call, text(call, call[1], true), call.u32(2) & 1, call.u32(3));
	});
	registry.add("CreateWaitableTimerExA", 4, [create_timer, text](ApiCall &call) {
		create_timer(call, text(call, call[1], false), call.u32(2) & 1, call.u32(3));
	});
	const auto open_timer = [&objects, named_timers](ApiCall &call, std::u16string name) {
		const auto found = named_timers->find(name);
		const auto timer = found == named_timers->end() ? nullptr : found->second.lock();
		if (!timer) {
			call.fail(2, 0);
			return;
		}
		const auto made = objects.add_object(timer);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	};
	registry.add("OpenWaitableTimerW", 3, [open_timer, text](ApiCall &call) { open_timer(call, text(call, call[2], true)); });
	registry.add("OpenWaitableTimerA", 3, [open_timer, text](ApiCall &call) { open_timer(call, text(call, call[2], false)); });
	// SetWaitableTimer(timer, due, period, routine, argument, resume): a negative due time is relative, in units of 100 ns; a positive one
	// is an absolute FILETIME in UTC.
	const auto set_timer = [&objects](ApiCall &call) {
		const auto timer = objects.object_as<WaitableTimerObject>(call[0]);
		if (!timer || !call[1]) {
			call.fail(timer ? 87 : 6);
			return;
		}
		const auto due = call.load<int64_t>(call[1]);
		auto when = std::chrono::steady_clock::now();
		if (due < 0) {
			when += std::chrono::microseconds(-due / 10);
		} else {
			using namespace std::chrono;
			const auto now_1601 = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() + 11644473600000000LL;
			const auto ahead = due / 10 - now_1601;
			if (ahead > 0)
				when += microseconds(ahead);
		}
		const auto period = call.i32(2);
		timer->set(when, std::chrono::milliseconds(period > 0 ? period : 0));
		call.set_last_error(0);
		call.ret(1);
	};
	registry.add("SetWaitableTimer", 6, set_timer);
	registry.add("SetWaitableTimerEx", 7, set_timer);
	registry.add("CancelWaitableTimer", 1, [&objects](ApiCall &call) {
		const auto timer = objects.object_as<WaitableTimerObject>(call[0]);
		if (!timer) {
			call.fail(6);
			return;
		}
		timer->cancel();
		call.ret(1);
	});

	// Named kernel objects are shared with every process of the prefix, through a block of shared memory; unnamed ones stay in the process.
	const auto fail_shared = [](ApiCall &call, uint32_t error) { call.fail(error == 8 ? 8 : error, 0); };
	registry.add("CreateEventW", 4, [&objects, fail_shared](ApiCall &call) {
		if (!call[3]) {
			const auto made = objects.create_event(0x1f0003, call.u32(1) ? 0 : 1, call.u32(2) != 0);
			if (made.status) {
				call.fail(made.status == 0xc000009aU ? 4 : 87, 0);
				return;
			}
			call.set_last_error(0);
			call.ret(made.value);
			return;
		}
		const bool manual = call.u32(1) != 0, initial = call.u32(2) != 0;
		bool existed = false;
		uint32_t error = 0;
		const auto shared = SharedFile::open("event", call.wide(call[3], 259), sizeof(EventState), true, existed, error, [&](void *memory) {
			auto *state = new (memory) EventState();
			state->manual = manual;
			state->signaled = initial ? 1 : 0;
		});
		if (!shared) {
			fail_shared(call, error);
			return;
		}
		auto event = std::make_shared<SharedEventObject>();
		event->shared = shared;
		event->state = static_cast<EventState *>(shared->data);
		const auto made = objects.add_object(event);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.set_last_error(existed ? 183 : 0);
		call.ret(made.value);
	});
	registry.add("CreateEventExW", 4, [&objects, create = registry.find("CreateEventW")->second](ApiCall &call) {
		const auto access = event_access(call.u32(3));
		if (!access || (call.u32(2) & ~3U)) {
			call.fail(87);
			return;
		}
		const bool inherit = inheritable_handle(call, call[0]);
		ApiCall inner(call.process, call.environment, {call[0], call.u32(2) & 1U, call.u32(2) & 2U, call[1]});
		create(inner);
		if (inner.result) {
			objects.set_access(inner.result, *access);
			objects.set_inherit(inner.result, inherit);
		}
		call.ret(inner.result);
	});
	// Memory resource notifications are manual-reset events that the system sets while memory is low or high. Memory here is never short,
	// so the high-memory one is set and the low-memory one never is.
	registry.add("CreateMemoryResourceNotification", 1, [&objects](ApiCall &call) {
		if (call.u32(0) > 1) {
			call.fail(87, 0);
			return;
		}
		const auto made = objects.create_event(0x100001, 0, call.u32(0) == 1);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("QueryMemoryResourceNotification", 2, [&objects](ApiCall &call) {
		const int state = objects.signaled(call[0], 0);
		if (state < 0 || !call[1]) {
			call.fail(state == -2 ? 6 : 87);
			return;
		}
		call.store<uint32_t>(call[1], state ? 1 : 0);
		call.ret(1);
	});
	registry.add("OpenEventW", 3, [&objects, fail_shared](ApiCall &call) {
		if (!call[2]) {
			call.fail(87, 0);
			return;
		}
		bool existed = false;
		uint32_t error = 0;
		const auto shared = SharedFile::open("event", call.wide(call[2], 259), sizeof(EventState), false, existed, error);
		if (!shared) {
			fail_shared(call, error);
			return;
		}
		auto event = std::make_shared<SharedEventObject>();
		event->shared = shared;
		event->state = static_cast<EventState *>(shared->data);
		const auto made = objects.add_object(event);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	// The interlocked operations act on one 32-bit word of the guest. Guest threads take turns, so the host needs no atomics of its own.
	registry.add("InterlockedIncrement", 1, [](ApiCall &call) {
		const auto value = call.load<int32_t>(call[0]) + 1;
		call.store<int32_t>(call[0], value);
		call.ret(uint32_t(value));
	});
	registry.add("InterlockedDecrement", 1, [](ApiCall &call) {
		const auto value = call.load<int32_t>(call[0]) - 1;
		call.store<int32_t>(call[0], value);
		call.ret(uint32_t(value));
	});
	registry.add("InterlockedExchange", 2, [](ApiCall &call) {
		const auto old = call.load<uint32_t>(call[0]);
		call.store<uint32_t>(call[0], call.u32(1));
		call.ret(old);
	});
	registry.add("InterlockedExchangeAdd", 2, [](ApiCall &call) {
		const auto old = call.load<uint32_t>(call[0]);
		call.store<uint32_t>(call[0], old + call.u32(1));
		call.ret(old);
	});
	registry.add("InterlockedCompareExchange", 3, [](ApiCall &call) {
		const auto old = call.load<uint32_t>(call[0]);
		if (old == call.u32(2))
			call.store<uint32_t>(call[0], call.u32(1));
		call.ret(old);
	});
	// A 64-bit operand is two words on the stack of a 32-bit program, so the value is put together from them.
	registry.add("InterlockedCompareExchange64", 5, [](ApiCall &call) {
		const bool narrow = call.pointer_width() == 4;
		const uint64_t exchange = narrow ? (call[1] & 0xffffffffULL) | call[2] << 32 : call[1];
		const uint64_t comparand = narrow ? (call[3] & 0xffffffffULL) | call[4] << 32 : call[2];
		const auto old = call.load<uint64_t>(call[0]);
		if (old == comparand)
			call.store<uint64_t>(call[0], exchange);
		call.ret(old);
	});
	// Mutexes and semaphores: with a name they are shared memory the processes of the prefix all map.
	const auto make_mutex = [&objects, &threads](ApiCall &call, uint64_t name_address, bool owned) {
		auto mutex = std::make_shared<MutexObject>();
		bool existed = false;
		if (name_address) {
			uint32_t error = 0;
			mutex->shared = SharedFile::open("mutex", call.wide(name_address, 259), sizeof(MutexState), true, existed, error, [](void *memory) { new (memory) MutexState(); });
			if (!mutex->shared) {
				call.fail(error, 0);
				return;
			}
			mutex->state = static_cast<MutexState *>(mutex->shared->data);
		}
		if (owned && !existed)
			mutex->try_consume(threads.current_id());
		const auto made = objects.add_object(mutex);
		if (made.status) {
			call.fail(8);
			return;
		}
		call.set_last_error(existed ? 183 : 0);
		call.ret(made.value);
	};
	registry.add("CreateMutexW", 3, [make_mutex](ApiCall &call) { make_mutex(call, call[2], call.u32(1) != 0); });
	registry.add("CreateMutexExW", 4, [make_mutex](ApiCall &call) { make_mutex(call, call[1], (call.u32(2) & 1) != 0); });
	registry.add("OpenMutexW", 3, [&objects](ApiCall &call) {
		if (!call[2]) {
			call.fail(87);
			return;
		}
		auto mutex = std::make_shared<MutexObject>();
		bool existed = false;
		uint32_t error = 0;
		mutex->shared = SharedFile::open("mutex", call.wide(call[2], 259), sizeof(MutexState), false, existed, error);
		if (!mutex->shared) {
			call.fail(error);
			return;
		}
		mutex->state = static_cast<MutexState *>(mutex->shared->data);
		call.ret(objects.add_object(mutex).value);
	});
	registry.add("ReleaseMutex", 1, [&objects, &threads](ApiCall &call) {
		const auto mutex = objects.object_as<MutexObject>(call[0]);
		if (!mutex) {
			call.fail(6);
			return;
		}
		if (!mutex->release(threads.current_id())) {
			call.fail(288);
			return;
		}
		call.ret(1);
	});
	const auto make_semaphore = [&objects](ApiCall &call, int64_t initial, int64_t maximum, uint64_t name) {
		if (maximum <= 0 || initial < 0 || initial > maximum) {
			call.fail(87);
			return;
		}
		auto semaphore = std::make_shared<SemaphoreObject>();
		bool existed = false;
		if (name) {
			uint32_t error = 0;
			semaphore->shared = SharedFile::open("semaphore", call.wide(name, 259), sizeof(SemaphoreState), true, existed, error, [&](void *memory) {
				auto *state = new (memory) SemaphoreState();
				state->count = int32_t(initial);
				state->maximum = int32_t(maximum);
			});
			if (!semaphore->shared) {
				call.fail(error);
				return;
			}
			semaphore->state = static_cast<SemaphoreState *>(semaphore->shared->data);
		} else {
			semaphore->state->count = int32_t(initial);
			semaphore->state->maximum = int32_t(maximum);
		}
		const auto made = objects.add_object(semaphore);
		if (made.status) {
			call.fail(8);
			return;
		}
		call.set_last_error(existed ? 183 : 0);
		call.ret(made.value);
	};
	registry.add("CreateSemaphoreW", 4, [make_semaphore](ApiCall &call) { make_semaphore(call, call.i32(1), call.i32(2), call[3]); });
	registry.add("CreateSemaphoreExW", 6, [make_semaphore](ApiCall &call) { make_semaphore(call, call.i32(1), call.i32(2), call[3]); });
	registry.add("OpenSemaphoreW", 3, [&objects](ApiCall &call) {
		if (!call[2]) {
			call.fail(87);
			return;
		}
		auto semaphore = std::make_shared<SemaphoreObject>();
		bool existed = false;
		uint32_t error = 0;
		semaphore->shared = SharedFile::open("semaphore", call.wide(call[2], 259), sizeof(SemaphoreState), false, existed, error);
		if (!semaphore->shared) {
			call.fail(error);
			return;
		}
		semaphore->state = static_cast<SemaphoreState *>(semaphore->shared->data);
		call.ret(objects.add_object(semaphore).value);
	});
	registry.add("ReleaseSemaphore", 3, [&objects](ApiCall &call) {
		const auto semaphore = objects.object_as<SemaphoreObject>(call[0]);
		const int32_t amount = call.i32(1);
		if (!semaphore) {
			call.fail(6);
			return;
		}
		if (call[2]) {
			try {
				call.process.validate_write_memory(call[2], sizeof(int32_t));
			} catch (const std::runtime_error &) {
				call.fail(998);
				return;
			}
		}
		auto current = semaphore->state->count.load();
		for (;;) {
			if (amount <= 0 || int64_t(current) + amount > semaphore->state->maximum) {
				call.fail(298);
				return;
			}
			if (semaphore->state->count.compare_exchange_weak(current, current + amount))
				break;
		}
		if (semaphore->shared)
			announce_shared_change();
		if (call[2]) call.store<int32_t>(call[2], current);
		call.ret(1);
	});


	// Thread pool work items run on short-lived guest threads; waiting on them joins those threads.
	registry.add("CreateThreadpoolWork", 3, [&objects](ApiCall &call) {
		auto work = std::make_shared<WorkObject>();
		work->callback = call[0];
		work->parameter = call[1];
		const auto made = objects.add_object(work);
		call.ret(made.status ? 0 : made.value);
		// TP_CALLBACK_ENVIRON: the version, then the pool and the cleanup group as pointers.
		if (!made.status && call[2])
			if (const auto group = objects.object_as<CleanupGroupObject>(call.load_pointer(call[2] + 2 * call.pointer_width())))
				group->members.push_back(made.value);
	});
	registry.add("SubmitThreadpoolWork", 1, [&objects, &threads, &context](ApiCall &call) {
		const auto work = objects.object_as<WorkObject>(call[0]);
		if (!work)
			return;
		// TP_CALLBACK_INSTANCE is opaque to callbacks; the work handle itself stands in for it.
		const auto instance = context.allocator.allocate(64);
		const auto thread = threads.create(work->callback, work->parameter, 1024 * 1024, false);
		thread->arguments = {instance, work->parameter, call[0]};
		work->running.push_back(thread);
	});
	registry.add("WaitForThreadpoolWorkCallbacks", 2, [&objects, &threads](ApiCall &call) {
		const auto work = objects.object_as<WorkObject>(call[0]);
		if (!work)
			return;
		threads.block([work] {
			return std::all_of(work->running.begin(), work->running.end(), [](const auto &thread) { return thread->state == GuestThread::State::done; });
		}, std::nullopt);
	});
	registry.add("CloseThreadpoolWork", 1, [&objects](ApiCall &call) { objects.close(call[0]); });
	registry.add("CreateThreadpool", 1, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<PoolObject>());
		if (made.status)
			call.fail(8, 0);
		else
			call.ret(made.value);
	});
	registry.add("CloseThreadpool", 1, [&objects](ApiCall &call) { objects.close(call[0]); });
	registry.add("SetThreadpoolThreadMaximum", 2, [&objects](ApiCall &call) {
		if (const auto pool = objects.object_as<PoolObject>(call[0]))
			pool->maximum = call.u32(1);
	});
	registry.add("SetThreadpoolThreadMinimum", 2, [&objects](ApiCall &call) {
		const auto pool = objects.object_as<PoolObject>(call[0]);
		if (!pool) {
			call.fail(87);
			return;
		}
		pool->minimum = call.u32(1);
		call.ret(1);
	});
	registry.add("CreateThreadpoolCleanupGroup", 0, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<CleanupGroupObject>());
		if (made.status)
			call.fail(8, 0);
		else
			call.ret(made.value);
	});
	// The members' callbacks finish first (pending ones run too, since submitted work starts at once), then their handles close.
	registry.add("CloseThreadpoolCleanupGroupMembers", 3, [&objects, &threads](ApiCall &call) {
		const auto group = objects.object_as<CleanupGroupObject>(call[0]);
		if (!group)
			return;
		std::vector<std::shared_ptr<WorkObject>> works;
		for (const auto member : group->members)
			if (const auto work = objects.object_as<WorkObject>(member))
				works.push_back(work);
		threads.block([works] {
			return std::all_of(works.begin(), works.end(), [](const auto &work) {
				return std::all_of(work->running.begin(), work->running.end(), [](const auto &thread) { return thread->state == GuestThread::State::done; });
			});
		}, std::nullopt);
		for (const auto member : group->members)
			objects.close(member);
		group->members.clear();
	});
	registry.add("CloseThreadpoolCleanupGroup", 1, [&objects](ApiCall &call) { objects.close(call[0]); });
	// The pool grows as callbacks need threads, so a long callback needs no notice; another thread is always available.
	registry.add("CallbackMayRunLong", 1, [](ApiCall &call) { call.ret(1); });

	// Registered waits are checked whenever every thread waits; a satisfied one starts its callback on a new guest thread.
	auto registrations = std::make_shared<std::vector<std::shared_ptr<WaitRegistration>>>();
	threads.add_pump([registrations, &objects, &threads] {
		const auto now = std::chrono::steady_clock::now();
		for (const auto &wait : *registrations) {
			if (!wait->active || (wait->running && wait->running->state != GuestThread::State::done))
				continue;
			const bool signaled = objects.signaled(wait->handle, 0) == 1;
			const bool expired = wait->timeout != UINT32_MAX && now >= wait->deadline;
			if (!signaled && !expired)
				continue;
			if (signaled && !objects.try_consume(wait->handle, 0))
				continue;
			const auto thread = threads.create(wait->callback, wait->context, 1024 * 1024, false);
			thread->arguments = {wait->context, signaled ? 0u : 1u};
			wait->running = thread;
			if (wait->flags & 0x8)
				wait->active = false;
			else
				wait->deadline = now + std::chrono::milliseconds(wait->timeout);
		}
	});
	threads.add_external([registrations] {
		return std::any_of(registrations->begin(), registrations->end(), [](const auto &wait) { return wait->active; });
	});
	const auto register_wait = [&objects, registrations](ApiCall &call) {
		if (!call[0] || !call[2]) {
			call.fail(87, 0);
			return;
		}
		const auto copy = objects.duplicate(call[1]);
		if (copy.status) {
			call.fail(6, 0);
			return;
		}
		auto wait = std::make_shared<WaitRegistration>();
		wait->handle = copy.value;
		wait->callback = call[2];
		wait->context = call[3];
		wait->timeout = call.u32(4);
		wait->flags = call.u32(5);
		wait->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait->timeout);
		const auto made = objects.add_object(wait);
		if (made.status) {
			objects.close(copy.value);
			call.fail(8, 0);
			return;
		}
		registrations->push_back(wait);
		call.store_pointer(call[0], made.value);
		call.ret(1);
	};
	registry.add("RegisterWaitForSingleObject", 6, register_wait);
	const auto unregister_wait = [&objects, &threads, registrations](ApiCall &call, bool drain) {
		const auto wait = objects.object_as<WaitRegistration>(call[0]);
		if (!wait) {
			call.fail(6);
			return;
		}
		wait->active = false;
		// A caller that passes INVALID_HANDLE_VALUE waits for a callback that is still running.
		if (drain && call[1] == UINT64_MAX && wait->running)
			threads.block([wait] { return wait->running->state == GuestThread::State::done; }, std::nullopt);
		registrations->erase(std::remove(registrations->begin(), registrations->end(), wait), registrations->end());
		objects.close(wait->handle);
		objects.close(call[0]);
		if (drain && call[1] && call[1] != UINT64_MAX)
			objects.set_event(call[1], true);
		call.ret(1);
	};
	registry.add("UnregisterWait", 1, [unregister_wait](ApiCall &call) { unregister_wait(call, false); });
	registry.add("UnregisterWaitEx", 2, [unregister_wait](ApiCall &call) { unregister_wait(call, true); });

	// Timer queues: the same pump starts each due timer's callback on a new guest thread, with TRUE for a timer that ran out; a timer
	// whose callback still runs is not started again. Deleting timers can name an event that is set once their callbacks have returned.
	struct TimerState {
		std::vector<std::shared_ptr<QueueTimer>> timers;
		std::vector<std::pair<std::vector<std::shared_ptr<QueueTimer>>, uint64_t>> completions;
	};
	auto timer_state = std::make_shared<TimerState>();
	const auto finished = [](const std::shared_ptr<QueueTimer> &timer) { return !timer->running || timer->running->state == GuestThread::State::done; };
	threads.add_pump([timer_state, &objects, &threads, finished] {
		const auto now = std::chrono::steady_clock::now();
		for (const auto &timer : timer_state->timers) {
			if (!timer->active || now < timer->deadline || !finished(timer))
				continue;
			const auto thread = threads.create(timer->callback, timer->parameter, 1024 * 1024, false);
			thread->arguments = {timer->parameter, 1};
			timer->running = thread;
			if (!timer->period || (timer->flags & 0x8))
				timer->active = false;
			else
				timer->deadline = std::max(timer->deadline + std::chrono::milliseconds(timer->period), now);
		}
		auto &completions = timer_state->completions;
		for (auto at = completions.begin(); at != completions.end();)
			if (std::all_of(at->first.begin(), at->first.end(), finished)) {
				objects.set_event(at->second, true);
				at = completions.erase(at);
			} else {
				++at;
			}
	});
	threads.add_external([timer_state] {
		return !timer_state->completions.empty() ||
			std::any_of(timer_state->timers.begin(), timer_state->timers.end(), [](const auto &timer) { return timer->active; });
	});
	// Stops timers and reports how their running callbacks end: INVALID_HANDLE_VALUE waits for them, an event is set after them, and with
	// no event a callback still running fails the call with ERROR_IO_PENDING although the timers are gone.
	const auto stop_timers = [timer_state, &threads, finished](ApiCall &call, std::vector<std::shared_ptr<QueueTimer>> stopped, uint64_t completion) {
		for (const auto &timer : stopped) {
			timer->active = false;
			std::erase(timer_state->timers, timer);
		}
		const bool running = !std::all_of(stopped.begin(), stopped.end(), finished);
		if (completion == UINT64_MAX || completion == 0xffffffffULL) {
			threads.block([stopped, finished] { return std::all_of(stopped.begin(), stopped.end(), finished); }, std::nullopt);
		} else if (completion) {
			timer_state->completions.emplace_back(std::move(stopped), completion);
		} else if (running) {
			call.fail(997);
			return;
		}
		call.ret(1);
	};
	registry.add("CreateTimerQueue", 0, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<TimerQueue>());
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("CreateTimerQueueTimer", 7, [&objects, timer_state](ApiCall &call) {
		if (!call[0] || !call[2]) {
			call.fail(87, 0);
			return;
		}
		std::shared_ptr<TimerQueue> queue;
		if (call[1] && !(queue = objects.object_as<TimerQueue>(call[1]))) {
			call.fail(6, 0);
			return;
		}
		auto timer = std::make_shared<QueueTimer>();
		timer->callback = call[2];
		timer->parameter = call[3];
		timer->period = call.u32(5);
		timer->flags = call.u32(6);
		timer->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(call.u32(4));
		const auto made = objects.add_object(timer);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		timer->handle = made.value;
		if (queue)
			queue->timers.push_back(timer);
		timer_state->timers.push_back(timer);
		call.store_pointer(call[0], made.value);
		call.ret(1);
	});
	registry.add("ChangeTimerQueueTimer", 4, [&objects](ApiCall &call) {
		const auto timer = objects.object_as<QueueTimer>(call[1]);
		if (!timer) {
			call.fail(6);
			return;
		}
		timer->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(call.u32(2));
		timer->period = call.u32(3);
		call.ret(1);
	});
	const auto delete_timer = [&objects, stop_timers](ApiCall &call, uint64_t completion) {
		const auto timer = objects.object_as<QueueTimer>(call[1]);
		if (!timer) {
			call.fail(6);
			return;
		}
		if (const auto queue = objects.object_as<TimerQueue>(call[0]))
			std::erase(queue->timers, timer);
		objects.close(call[1]);
		stop_timers(call, {timer}, completion);
	};
	registry.add("DeleteTimerQueueTimer", 3, [delete_timer](ApiCall &call) { delete_timer(call, call[2]); });
	registry.add("CancelTimerQueueTimer", 2, [delete_timer](ApiCall &call) { delete_timer(call, 0); });
	const auto delete_queue = [&objects, stop_timers](ApiCall &call, uint64_t completion) {
		const auto queue = objects.object_as<TimerQueue>(call[0]);
		if (!queue) {
			call.fail(6);
			return;
		}
		auto timers = std::move(queue->timers);
		for (const auto &timer : timers)
			objects.close(timer->handle);
		objects.close(call[0]);
		stop_timers(call, std::move(timers), completion);
	};
	registry.add("DeleteTimerQueueEx", 2, [delete_queue](ApiCall &call) { delete_queue(call, call[1]); });
	registry.add("DeleteTimerQueue", 1, [delete_queue](ApiCall &call) { delete_queue(call, 0); });
}

}
