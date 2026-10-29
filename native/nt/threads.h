#pragma once

#include "environment.h"
#include "memory.h"
#include "objects.h"
#include "tls.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ucontext.h>
#include <poll.h>
#include <vector>

namespace raven {

class AccessToken;

// A Windows thread: guest registers and stack, plus the host coroutine that runs its API handlers.
struct GuestThread {
	enum class State { ready, running, waiting, done };
	uint32_t id = 0;
	uint64_t teb = 0, stack_low = 0, stack_high = 0;
	State state = State::ready;
	NativeThreadState snapshot;
	ucontext_t context{};
	std::unique_ptr<char[]> host_stack;
	// Wait condition: pure check, evaluated by the scheduler; the thread resumes when it holds or the wake time passes.
	std::function<bool()> ready;
	std::optional<std::chrono::steady_clock::time_point> wake;
	uint32_t exit_code = 0x103;
	int suspend_count = 0;
	uint64_t start = 0, parameter = 0;
	// Extra start arguments for entry points that take more than one (thread pool callbacks); empty means just the parameter.
	std::vector<uint64_t> arguments;
	bool main = false;
	// Set when the process is ending: the thread leaves through its host frames the next time it runs, so nothing they hold outlives it.
	bool unwinding = false;
	struct Apc {
		uint64_t routine, parameter;
		// A completion routine takes its three arguments from here instead of the single parameter.
		std::vector<uint64_t> arguments;
	};
	std::deque<Apc> apcs;
	// Alerts: NtAlertThread's ends the thread's alertable native wait; NtAlertThreadByThreadId's ends its NtWaitForAlertByThreadId, or
	// the next one. Each is taken by the wait it ends.
	bool alerted = false, alerted_by_id = false;
	int priority = 0;
	std::shared_ptr<AccessToken> impersonation;
	// Pipes to the processes that hold a handle to this thread: each gets the exit code when the thread ends.
	std::vector<int> exit_reports;
};

class ThreadObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"Thread"; }
	uint32_t default_access() const override { return 0x1fffff; }
	explicit ThreadObject(std::shared_ptr<GuestThread> thread) : thread(std::move(thread)) {}
	int signaled(uint32_t) const override { return thread->state == GuestThread::State::done; }
	// A handle that goes to another process carries a pipe the thread's end is reported on.
	bool describe(InheritedHandle &record) const override;
	std::shared_ptr<GuestThread> thread;
};

// A thread of another process: it has ended when its process wrote the exit code to the pipe, or closed the pipe by ending itself.
class RemoteThreadObject : public KernelObject {
public:
	explicit RemoteThreadObject(int descriptor) : descriptor(descriptor) {}
	~RemoteThreadObject() override;
	std::u16string_view type_name() const override { return u"Thread"; }
	int signaled(uint32_t) const override { return code().has_value(); }
	// The exit code once the thread has ended.
	std::optional<uint32_t> code() const;

private:
	int descriptor;
	mutable std::optional<uint32_t> ended;
};

struct ThreadExit {
	uint32_t code;
};

// Cooperative scheduler over one traced process: guest threads switch only inside API calls.
class NativeThreads {
public:
	NativeThreads(NativeProcess &process, NativeEnvironment &environment, NativeMemory &memory, NativeTLS *tls,
		std::chrono::steady_clock::time_point deadline);
	~NativeThreads();
	void set_runner(std::function<NativeResult(uint64_t, std::span<const uint64_t>)> invoke) { run = std::move(invoke); }
	// The stack a thread reserves when its creator names only what to commit: the program's SizeOfStackReserve.
	uint64_t default_stack_reserve = 1024 * 1024;
	// Called in a new thread before it starts (reason 2) and after it ends (reason 3): TLS callbacks and DllMain notifications.
	void set_notifier(std::function<void(uint32_t)> notify) { notifier = std::move(notify); }
	// From here on a guest runs until it calls the runtime: the process is ending and nothing else should be scheduled behind its back.
	void stop_time_slicing();
	// Called with the thread block of a thread that was just made and of one that is being released: static TLS of a 32-bit process.
	void set_block_hooks(std::function<void(uint64_t)> made, std::function<void(uint64_t)> released)
	{
		block_made = std::move(made);
		block_released = std::move(released);
	}
	// Host polling used while every guest thread waits; it may make waiting predicates true (window events).
	void set_idle(std::function<void(std::chrono::milliseconds)> idle) { poll = std::move(idle); }
	// Adds the descriptors whose readiness can make a waiting thread's condition true; the idle wait sleeps on them.
	void set_watched(std::function<void(std::vector<pollfd> &)> descriptors) { watched = std::move(descriptors); }
	// True while something outside the process, such as a child program, can still make a waiting thread's condition true.
	void add_external(std::function<bool()> pending) { externals.push_back(std::move(pending)); }
	// Runs whenever every thread waits, so work the host does for the guest (finishing started operations) can make a wait succeed.
	void add_pump(std::function<void()> work) { pumps.push_back(std::move(work)); }
	// Runs at every scheduling decision, also while threads run: requests from other processes are answered without waiting for idleness.
	void add_service(std::function<void()> work) { services.push_back(std::move(work)); }
	void serve() { for (const auto &service : services) service(); }
	// Ends the scheduler's idle wait early: called from a host thread that finished work a waiting guest thread looks for.
	void nudge();
	// For a wait outside the scheduler, such as a thread waiting for window messages while the others wait too: the latest time it may
	// sleep to, no later than `limit`, and the descriptors that end it early. A waiting thread whose condition nothing announces keeps
	// it short, as the scheduler's own wait is. drain_wake() takes back what ended it.
	std::chrono::steady_clock::time_point idle_until(std::chrono::steady_clock::time_point limit) const;
	void idle_descriptors(std::vector<pollfd> &descriptors) const;
	void drain_wake();

	std::shared_ptr<GuestThread> create(uint64_t start, uint64_t parameter, uint64_t stack_size, bool suspended);
	GuestThread &current() { return *running; }
	std::shared_ptr<GuestThread> find(uint32_t id) const;
	uint32_t current_id() const { return running->id; }
	// Blocks the current thread until ready() holds (true) or the wake time passes (false).
	bool block(std::function<bool()> ready, std::optional<std::chrono::steady_clock::time_point> wake);
	// Lets another runnable thread run; false when nothing else is runnable.
	bool yield();
	[[noreturn]] void exit_current(uint32_t code);
	void terminate(const std::shared_ptr<GuestThread> &thread, uint32_t code);
	// Ends every thread but the running one where it stands, as ExitProcess does before the libraries are told the process detaches.
	void terminate_others(uint32_t code);
	void resume(GuestThread &thread);
	uint32_t suspend(GuestThread &thread);
	// Lets every thread that is still blocked or not yet started leave through its host frames; called when the process ends.
	void unwind_all();
	size_t live() const;
	std::vector<uint32_t> thread_ids() const;
	// Thread blocks of every thread that has not finished.
	std::vector<uint64_t> thread_blocks() const;

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	NativeMemory &memory;
	NativeTLS *tls;
	std::chrono::steady_clock::time_point deadline;
	std::function<NativeResult(uint64_t, std::span<const uint64_t>)> run;
	std::function<void(uint32_t)> notifier;
	std::function<void(uint64_t)> block_made, block_released;
	std::function<void(std::chrono::milliseconds)> poll;
	std::vector<std::function<bool()>> externals;
	std::vector<std::function<void()>> pumps;
	// When a wait that does not wait last let the services and pumps run.
	std::chrono::steady_clock::time_point pumped{};
	std::vector<std::function<void()>> services;
	// Written to end the idle wait: by nudge() and by the thread that sees other processes change shared objects.
	int wake = -1;
	std::function<void(std::vector<pollfd> &)> watched;
	std::vector<std::shared_ptr<GuestThread>> threads;
	GuestThread *running = nullptr;
	// Set while the process ends: threads leave through their frames and the deadline no longer interrupts them.
	bool tearing_down = false;
	uint32_t next_id = 2;
	// An exception from a non-main thread is carried to the main thread, which owns the process result.
	std::exception_ptr pending;
	std::shared_ptr<GuestThread> pick(bool include_current);
	void schedule();
	void switch_to(GuestThread &next);
	void reap();
	void finish(GuestThread &thread, uint32_t code);
	static void trampoline(unsigned low, unsigned high);
	void thread_main(GuestThread &thread);
	void release_resources(GuestThread &thread);
	void check_pending();
};

}
