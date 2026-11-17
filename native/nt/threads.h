#pragma once

#include "environment.h"
#include "memory.h"
#include "objects.h"
#include "tls.h"

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ucontext.h>
#include <poll.h>
#include <pthread.h>
#include <thread>
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
	// The API the thread waits in and since when, for the list RAVEN_DUMP_THREADS_ON asks for.
	const char *waiting_in = nullptr;
	std::chrono::steady_clock::time_point waiting_since;
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
	// Whether the thread last gave the processor up only because its time slice ended, and when: such a thread waits behind those
	// that wait and wake, which on Windows find a free processor at once, but never longer than a few slices.
	bool spent_slice = false;
	std::chrono::steady_clock::time_point set_aside;
	// Until when the thread goes before those that only compute, because it has just read the user's input (see took_input).
	std::chrono::steady_clock::time_point input_until;
	// Background processing mode, which only the thread itself enters and leaves; meanwhile it reports idle priority.
	bool background = false;
	std::shared_ptr<AccessToken> impersonation;
	// Pipes to the processes that hold a handle to this thread: each gets the exit code when the thread ends.
	std::vector<int> exit_reports;
	// With a host thread for each guest thread (see NativeThreads): the host thread, its Linux id, and its place in the line for the
	// runtime. `parked` while its guest runs; `paused` while another thread holds that guest stopped; `held` a stop that came before the
	// pause took hold. `job` is work another thread has it do on its own task, which only it may trace.
	pthread_t host{};
	bool hosted = false;
	pid_t host_tid = 0;
	// The count of signals at which the wait's condition was last found not to hold, and whether it is looked at whatever the count.
	uint64_t seen_signals = 0;
	bool polled = false;
	// Whether the thread stands in line only because its wait's condition held when it was last looked at.
	bool conditional = false;
	std::condition_variable turn;
	bool queued = false, parked = false, paused = false;
	int held = -2;
	std::function<void()> job;
	bool job_done = false;
	// Whether the host thread waits outside the runtime, in the process's table of waits (see NativeThreads::wait_outside): nobody
	// puts it in line meanwhile, and it is called away from there for a job, a termination or the end of the process.
	bool outside = false;
	// Whether that wait has no time-out of its own, so that only another thread can end it (see the check for a deadlock).
	bool outside_endless = false;
	// A wait outside on shared objects sleeps on their words and on `calls`, which grows each time the host calls the thread away (see
	// NativeThreads::call_away); `calls_seen` is how many of those the thread has answered.
	std::atomic<uint32_t> calls{0};
	uint32_t calls_seen = 0;
	// The objects of the process's table that wait outside is for, which the thread that sets one of them looks at (see hand_over_to),
	// and how many turns the thread has taken in the runtime.
	std::vector<uint32_t> waits_on;
	std::atomic<uint64_t> turns{0};
	// How many waits the thread has begun, in the runtime or outside: one that grew since a set woke it waits again.
	std::atomic<uint64_t> waits{0};
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
	// ExitThread ends a thread as returning from it does, with the libraries told; TerminateThread and the end of the process do not.
	bool detach = false;
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
	// Called in a thread that is ending, before the libraries are told: the destructors of its fiber-local values.
	void set_exit_hook(std::function<void()> hook) { exit_hook = std::move(hook); }
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
	void serve();
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
	// The id of the guest thread the calling host thread serves, whether it holds the turn or not; zero on a thread that serves none.
	static uint32_t own_id();
	// Blocks the current thread until ready() holds (true) or the wake time passes (false).
	// `polled` tells that the condition reads what the guest changes by itself, as the word of a lock it releases without a call: it is
	// looked at whenever a thread gives the runtime up, where another is looked at again only after a call that may have ended a wait.
	bool block(std::function<bool()> ready, std::optional<std::chrono::steady_clock::time_point> wake, bool turn = false, bool polled = false);
	// Whether waits on the objects of the process's table (kernel_waits.h) block their host thread there, as they do with a host thread
	// for each guest thread. RAVEN_POLLED_WAITS=1 has them looked at by the scheduler instead, as the waits on other objects are, to
	// compare the two.
	bool waits_outside() const
	{
		static const bool polled = std::getenv("RAVEN_POLLED_WAITS") != nullptr;
		return threaded && !polled;
	}
	// Waits outside the runtime: the running thread gives the runtime up, `wait` blocks its host thread in the table of waits until
	// whoever signals what it waits for wakes it, and the thread takes its turn again. `wait` gets the time to wait until (the run's
	// limit shortens it for the main thread), and is called again after the thread was called away for a job.
	uint32_t wait_outside(const std::function<uint32_t(std::optional<std::chrono::steady_clock::time_point>)> &wait,
		std::optional<std::chrono::steady_clock::time_point> wake);
	// Queues an APC to a thread, which ends its alertable wait.
	void queue_apc(GuestThread &thread, GuestThread::Apc apc);
	// Ends the thread's wait outside the runtime, or the next one it begins, so that it takes its turn and looks at why.
	static void call_away(GuestThread &thread);
	// Tells that something may have ended a wait: an object signaled, a message posted, a thread ended.
	void signaled() { signals.fetch_add(1, std::memory_order_relaxed); }
	// Whether no thread waits outside the runtime for `object` of the process's table (or for one that shares its slot): a call answered
	// outside the runtime's turn (see NativeProcess::serve_alone) may then change the object without handing the turn to whom it wakes.
	bool quiet_for(uint32_t object) const { return table_waited[object % table_waited.size()].load(std::memory_order_acquire) == 0; }
	// Whether a thread waits in the runtime: one a change outside the turn may concern is looked at again after a nudge.
	bool waiting_in_runtime() const { return waiting_threads.load(std::memory_order_acquire) != 0; }
	// Every stop and call of a guest counts as such unless the runtime says, while it serves it, that it ends no wait.
	void quiet_call() { told = false; }
	// Lets another runnable thread run; false when nothing else is runnable.
	bool yield();
	// Runs a thread whose wait the running one has just ended, as setting an event does; false when no waiting thread became ready.
	bool hand_over();
	// The same with a host thread for each guest thread: the threads waiting outside for `object` of the process's table (0 for none),
	// which setting it may have woken, and those waiting in the runtime whose wait now holds, take their turn before the setter goes on, until each calls again, waits again or ends (a few milliseconds at
	// most). False when no thread was waiting for it.
	bool hand_over_to(uint32_t object);
	// How many threads wait outside for objects of the process's table, so that a set nobody waits for looks no further.
	std::atomic<int> table_waiters{0};
	// Moves on each time a thread takes its turn, goes to wait outside or ends; a setter handing over sleeps on it instead of keeping
	// the processor the threads it woke need (see moved).
	std::atomic<uint32_t> moves{0};
	std::atomic<int> handing{0};
	void moved();
	// The threads in block, counted on entry before they look at what they wait for (see waiting_in_runtime), and those that wait
	// outside for each object of the table, by slot (see quiet_for).
	std::atomic<int> waiting_threads{0};
	std::array<std::atomic<uint32_t>, 1024> table_waited{};
	// The running thread read a message of the mouse or the keyboard. Windows raises the priority of a thread whose window takes
	// input, and its threads have a processor each: a thread that follows the pointer is not slowed by a compiler or a renderer at
	// work in the background. Here, where the program's threads take turns, such a thread is for a moment not set aside when its
	// slice ends; the others run while it waits, and once in a few slices besides.
	void took_input();
	// Whether a thread other than the running one waits.
	bool others_waiting() const;
	[[noreturn]] void exit_current(uint32_t code);
	// Runs the end of the calling thread: its fiber-local destructors, then the libraries' notifications.
	void detach_current();
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

	// The process's time limit, which a start forked ahead learns only when its request comes.
	void set_deadline(std::chrono::steady_clock::time_point end) { deadline = end; }

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	NativeMemory &memory;
	NativeTLS *tls;
	std::chrono::steady_clock::time_point deadline;
	std::function<NativeResult(uint64_t, std::span<const uint64_t>)> run;
	std::function<void(uint32_t)> notifier;
	std::function<void()> exit_hook;
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
	// `only_due` leaves out the threads that only compute and have not been kept waiting: what is left is due the processor at once.
	std::shared_ptr<GuestThread> pick(bool include_current, bool only_due = false);
	void schedule();
	void switch_to(GuestThread &next);
	void reap();
	void finish(GuestThread &thread, uint32_t code);
	static void trampoline(unsigned low, unsigned high);
	void thread_main(GuestThread &thread);
	void release_resources(GuestThread &thread);
	void check_pending();
	// The host thread for each guest thread: one thread holds the runtime at a time (`holder`), the others wait in line or for their guest.
	bool threaded = false;
	std::mutex gate;
	std::condition_variable jobs_done;
	std::atomic<GuestThread *> holder{nullptr};
	std::deque<GuestThread *> line;
	// How many threads stand in the line, for a look without the gate.
	std::atomic<unsigned> lined{0};
	// Counts what may have ended a wait, and when every waiting thread's condition was last looked at whatever the count.
	std::atomic<uint64_t> signals{1};
	// Whether what the thread that holds the runtime is serving still has to be counted.
	bool told = true;
	// When a thread that left the runtime last looked for the waits that ended (see leave).
	std::chrono::steady_clock::time_point leave_scanned;
	std::chrono::steady_clock::time_point scanned{};
	// Where the next look at the waiting threads starts, when the last could not take them all.
	size_t scan_from = 0;
	std::function<void()> host_end;
	// The administrative thread: it traces the task the runtime uses for memory work while every guest runs, and does the host's own work
	// (services, pumps, timeouts) when no guest thread asks for it.
	GuestThread admin;
	pthread_t admin_host{};
	bool admin_hosted = false;
	bool stopping = false;
	std::chrono::steady_clock::time_point admin_due = std::chrono::steady_clock::time_point::max();
	void enter(GuestThread &me);
	void leave(GuestThread &me, bool save = true);
	void wait_turn(GuestThread &me, std::optional<std::chrono::steady_clock::time_point> wake);
	void install(GuestThread &me);
	void enqueue(GuestThread &thread);
	void wake_ready(GuestThread *except, bool all = false);
	void run_job(GuestThread &me, std::unique_lock<std::mutex> &lock);
	void send_job(GuestThread &thread, std::function<void()> work);
	int park(pid_t task, std::chrono::steady_clock::time_point deadline, const uint64_t *block);
	void host_main(GuestThread &thread);
	void admin_main();
	void end_host(GuestThread &thread);
	bool threaded_block(std::function<bool()> &ready, std::optional<std::chrono::steady_clock::time_point> wake);
	void threaded_unwind();
	void pause_task_of(GuestThread &thread);
	void resume_task_of(GuestThread &thread);
public:
	// Sets the registers a paused thread continues with, in its own task.
	void set_registers(GuestThread &thread, const user_regs_struct &registers);
	// Writes the thread's saved floating-point and extended state into its task, where the task holds its state (see set_registers).
	void set_xstate(GuestThread &thread);
	bool host_threads() const { return threaded; }
	// Has the host thread with this id do `work` on itself; nothing is done when it serves no guest thread any more.
	void on_host(pid_t host, std::function<void()> work);
	// What each host thread does last, on itself, before it ends.
	void set_host_end(std::function<void()> end);
};

}
