#include "native_cpu.h"
#include "served_queue.h"
#include "served_stub.h"
#include "call_cycles.h"
#include "nt/warm_host.h"
#include <cstddef>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <fstream>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <elf.h>
#include <fcntl.h>
#include <functional>
#include <csignal>
#include <ctime>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <sched.h>
#include <map>
#include <set>
#include <optional>
#include <signal.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <thread>
#include <execinfo.h>
#include <unistd.h>

namespace raven {

// Parallel guests are served by one host thread per guest thread, each tracing its own task; RAVEN_COROUTINE_HOST=1 keeps the older
// host, where one thread serves every task and switches between coroutines.
bool threaded_host()
{
	static const bool threaded = !std::getenv("RAVEN_COROUTINE_HOST");
	return threaded;
}

// The API the runtime handled last, named when a call is left without its answer.
extern const char *api_in_progress;

// The registers of the stopped guest, read once per stop: serving one call read them several times, and each read is a system call.
// Anything that can change them (writing them, resuming the guest, the guest stopping again) drops the copy.
static thread_local pid_t registers_of = -1;
static thread_local user_regs_struct cached_registers;

static void forget_registers()
{
	registers_of = -1;
}

// A call the guest left in its thread's block and waits for inside its stub, without having stopped: the host thread that serves it
// answers in the block. The task is stopped only for work that needs it stopped, and then stands at the stub's own breakpoint, as if
// it had made the call without the block (see NativeProcess::settle_call). `held` tells that the task is stopped inside the stub.
struct WaitingCall {
	NativeProcess *process = nullptr;
	pid_t task = 0;
	uint64_t *block = nullptr;
	uint64_t trap = 0, stack = 0;
	std::array<uint64_t, 4> words{}, floats{};
	bool answered = false, held = false;
	// A function the guest ran inside the stub meanwhile left the argument registers changed.
	bool ran = false;
	// When the runtime saw the call (see CallCycles).
	uint64_t seen = 0;
};
static thread_local WaitingCall waiting;

// A guest that waits in a stub runs a function for the runtime there and goes back to waiting: a callback of the call it waits for, or a
// short routine of the runtime's own. The request is left in the thread's block from `request_slot` on (the stub, the function and then
// its result, twelve arguments), with the address of the routine that carries it out at `request_routine`; the stub goes there when
// it finds 3 where it finds 0 for an answer. The calls whose callbacks run are kept here, innermost last, each with the nesting of the
// callbacks it made.
static constexpr size_t request_slot = 300, request_routine = 320, request_arguments = 12;
static constexpr uint64_t stub_callback = 0x5256535455424342ULL;
struct OuterCall {
	WaitingCall call;
	size_t depth = 0;
};
static thread_local std::vector<OuterCall> outer_calls;
// Whether the next resumption gives the block up for a request rather than an answer.
static thread_local bool requesting = false;
// A task stopped inside its stub before its host thread took the call up.
static thread_local bool held_in_stub = false;

static void trace(enum __ptrace_request operation, pid_t child, void *data = nullptr)
{
	if (waiting.task && waiting.task == child)
		waiting.process->settle_call();
	if (operation == PTRACE_GETREGS && registers_of == child) {
		std::memcpy(data, &cached_registers, sizeof cached_registers);
		return;
	}
	if (operation != PTRACE_GETREGS && operation != PTRACE_GETSIGINFO && operation != PTRACE_GETREGSET && operation != PTRACE_PEEKTEXT &&
		operation != PTRACE_PEEKDATA && operation != PTRACE_PEEKUSER)
		forget_registers();
	if (ptrace(operation, child, nullptr, data) == -1) {
		const int failure = errno;
		if (std::getenv("RAVEN_BACKTRACE")) {
			void *frames[32];
			std::fprintf(stderr, "ptrace request %d on %d failed: %s\n", int(operation), int(child), std::strerror(failure));
			backtrace_symbols_fd(frames, backtrace(frames, 32), 2);
		}
		throw std::runtime_error("native ptrace: " + std::string(std::strerror(failure)));
	}
	if (operation == PTRACE_GETREGS) {
		std::memcpy(&cached_registers, data, sizeof cached_registers);
		registers_of = child;
	}
}

class NativeChild {
public:
	NativeChild(std::span<const int> image_fds, const std::string &bootstrap, std::chrono::milliseconds timeout);
	~NativeChild();
	NativeChild(const NativeChild &) = delete;
	NativeChild &operator=(const NativeChild &) = delete;
	pid_t pid = -1;
	uint64_t gate = 0, batch_gate = 0;
	// `served` is polled while the guest runs: calls the guest makes without stopping are answered there.
	int wait(const std::function<bool()> *served = nullptr);
	uint64_t syscall(uint64_t number, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
		uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0);
	// System calls made one after the other in a single stop of the guest, through the batch gate of the bootstrap: each the number and
	// six arguments. Their results, in order; the first that fails throws, and none after it is made.
	std::vector<uint64_t> syscalls(std::span<const std::array<uint64_t, 7>> calls);
	void stop();
	void send_descriptor(int descriptor);
	int descriptor_socket() const { return guest_channel; }
	void set_timeout(std::chrono::milliseconds timeout);
	void set_deadline(std::chrono::steady_clock::time_point end) { deadline = end; }
	void enable_parallel();
	pid_t clone_task(uint64_t stack);
	bool take_stop(pid_t task, int &status);
	void stop_task(pid_t task);
	void pause_task();
	int execution_descriptor() const { return execution_wake; }
	// Whether any task has a stop the runtime has not taken yet.
	bool stops_queued() const { return queued_stops.load(std::memory_order_acquire) > 0; }
	void drain_execution();
	// While a slice is set, wait() returns -2 once it has passed without the guest stopping.
	void set_slice(std::chrono::steady_clock::duration slice) { slice_end = std::chrono::steady_clock::now() + slice; }
	// Sets a slice that ends at `end` unless one is already running.
	void ensure_slice(std::chrono::steady_clock::time_point end)
	{
		if (!slice_end)
			slice_end = end;
	}
	void clear_slice() { slice_end.reset(); }

private:
	int host_channel = -1, guest_channel = -1;
	std::chrono::steady_clock::time_point deadline;
	std::optional<std::chrono::steady_clock::time_point> slice_end;
	pid_t group = -1;
	int execution_wake = -1;
	std::thread events;
	std::mutex events_mutex;
	std::condition_variable events_ready;
	std::map<pid_t, std::deque<int>> stops;
	std::atomic<uint64_t> stops_generation{0};
	std::atomic<int64_t> queued_stops{0};
	uint64_t missed_generation = UINT64_MAX;
	pid_t missed_task = -1;
	std::set<pid_t> tasks;
	bool events_ended = false;
};

NativeChild::NativeChild(std::span<const int> image_fds, const std::string &bootstrap, std::chrono::milliseconds timeout)
	: deadline(std::chrono::steady_clock::now() + timeout)
{
	const char *path = bootstrap.c_str();
	int channel[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) == -1)
		throw std::runtime_error("cannot create native descriptor channel");
	host_channel = channel[0];
	guest_channel = channel[1];
	pid = fork();
	if (pid == -1) {
		close(guest_channel);
		stop();
		throw std::runtime_error("cannot fork native guest");
	}
	if (pid == 0) {
		if (setpgid(0, 0) == -1)
			_exit(126);
		close(host_channel);
		if (fcntl(guest_channel, F_SETFD, 0) == -1)
			_exit(126);
		for (int fd : image_fds) {
			if (fcntl(fd, F_SETFD, 0) == -1)
				_exit(126);
		}
		if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1)
			_exit(126);
		execl(path, path, static_cast<char *>(nullptr));
		_exit(127);
	}
	close(guest_channel);
	group = pid;
	try {
		int status = wait();
		if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
			throw std::runtime_error("native bootstrap did not stop after exec");
		trace(PTRACE_SETOPTIONS, pid, reinterpret_cast<void *>(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD));
		trace(PTRACE_CONT, pid);
		status = wait();
		if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
			throw std::runtime_error("native bootstrap did not reach setup trap");
		user_regs_struct registers{};
		trace(PTRACE_GETREGS, pid, &registers);
		gate = registers.rip - 1;
		errno = 0;
		const long code = ptrace(PTRACE_PEEKTEXT, pid, reinterpret_cast<void *>(gate), nullptr);
		if ((code == -1 && errno != 0) || (static_cast<uint64_t>(code) & 0xffffffff) != 0xcc050fcc)
			throw std::runtime_error("native bootstrap syscall gate is invalid");
		// The batch gate follows the gate's UD2 and begins with mov %rdi, %rbx.
		errno = 0;
		const long batch_code = ptrace(PTRACE_PEEKTEXT, pid, reinterpret_cast<void *>(gate + 6), nullptr);
		if ((batch_code != -1 || errno == 0) && (static_cast<uint64_t>(batch_code) & 0xffffff) == 0xfb8948)
			batch_gate = gate + 6;
	} catch (...) {
		stop();
		throw;
	}
}

NativeChild::~NativeChild()
{
	stop();
}

void NativeChild::set_timeout(std::chrono::milliseconds timeout)
{
	if (timeout.count() <= 0)
		throw std::runtime_error("native execution requires a positive timeout");
	deadline = std::chrono::steady_clock::now() + timeout;
}

void NativeChild::enable_parallel()
{
	trace(PTRACE_SETOPTIONS, pid, reinterpret_cast<void *>(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACESECCOMP));
	execution_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (execution_wake < 0)
		throw std::runtime_error("cannot create native execution wake descriptor");
	tasks.insert(pid);
	// Each host thread waits for the stops of its own task: a thread reaping every task's stops would take them from their tracers.
	if (threaded_host())
		return;
	// Only this guest's process group is reaped here; host processes launched for other Windows processes keep their own waiters.
	events = std::thread([this] {
		for (;;) {
			int status = 0;
			const pid_t task = waitpid(-group, &status, __WALL);
			if (task == -1 && errno == EINTR)
				continue;
			{
				std::lock_guard lock(events_mutex);
				if (task > 0) {
					stops[task].push_back(status);
					queued_stops.fetch_add(1, std::memory_order_release);
					if (WIFEXITED(status) || WIFSIGNALED(status))
						tasks.erase(task);
				} else
					events_ended = true;
				stops_generation.fetch_add(1, std::memory_order_release);
			}
			events_ready.notify_all();
			const uint64_t one = 1;
			[[maybe_unused]] const auto written = write(execution_wake, &one, sizeof one);
			if (task <= 0)
				return;
		}
	});
}

bool NativeChild::take_stop(pid_t task, int &status)
{
	// Only the runtime probes the queue; a missed task stays empty until a stop or the stream's end is published.
	if (task == missed_task && stops_generation.load(std::memory_order_acquire) == missed_generation)
		return false;
	std::lock_guard lock(events_mutex);
	const auto found = stops.find(task);
	if (found == stops.end()) {
		if (events_ended)
			throw std::runtime_error("native execution event stream ended");
		missed_task = task;
		missed_generation = stops_generation.load(std::memory_order_relaxed);
		return false;
	}
	status = found->second.front();
	found->second.pop_front();
	queued_stops.fetch_sub(1, std::memory_order_release);
	if (found->second.empty())
		stops.erase(found);
	return true;
}

void NativeChild::drain_execution()
{
	uint64_t count;
	while (execution_wake >= 0 && read(execution_wake, &count, sizeof count) > 0) {}
}

pid_t NativeChild::clone_task(uint64_t stack)
{
	user_regs_struct saved{}, registers{};
	trace(PTRACE_GETREGS, pid, &saved);
	registers = saved;
	registers.rip = gate + 1;
	registers.cs = 0x33;
	registers.ss = 0x2b;
	registers.rax = SYS_clone;
	registers.orig_rax = uint64_t(-1);
	// Task termination belongs to the tracer; it must not deliver SIGCHLD into another guest thread.
	registers.rdi = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM;
	registers.rsi = stack;
	registers.rdx = registers.r10 = registers.r8 = 0;
	trace(PTRACE_SETREGS, pid, &registers);
	trace(PTRACE_CONT, pid);
	const int event = wait();
	if (!WIFSTOPPED(event) || (event >> 16) != PTRACE_EVENT_CLONE)
		throw std::runtime_error("native thread creation did not report its task");
	unsigned long task = 0;
	trace(PTRACE_GETEVENTMSG, pid, &task);
	{
		std::lock_guard lock(events_mutex);
		tasks.insert(pid_t(task));
	}
	const pid_t caller = pid;
	pid = pid_t(task);
	const int created = wait();
	pid = caller;
	// A task that was seized, as one a host thread took over, starts its clones in an event stop rather than with a SIGSTOP.
	if (!WIFSTOPPED(created) || (WSTOPSIG(created) != SIGSTOP && (created >> 16) != PTRACE_EVENT_STOP))
		throw std::runtime_error("new native task was not stopped before execution");
	trace(PTRACE_CONT, pid);
	const int finished = wait();
	trace(PTRACE_GETREGS, pid, &registers);
	if (!WIFSTOPPED(finished) || WSTOPSIG(finished) != SIGTRAP || registers.rip != gate + 4)
		throw std::runtime_error("native thread creation did not finish at its gate");
	trace(PTRACE_SETREGS, pid, &saved);
	// The task outlives the process it was cloned from, so it is no longer a descendant of the runtime: it names the runtime as the one
	// allowed to trace it, which the host thread that serves it needs to take it over.
	if (threaded_host()) {
		const pid_t caller = pid;
		pid = pid_t(task);
		try {
			syscall(SYS_prctl, PR_SET_PTRACER, uint64_t(getpid()));
		} catch (...) {
			pid = caller;
			throw;
		}
		pid = caller;
	}
	return pid_t(task);
}

void NativeChild::stop_task(pid_t task)
{
	if (task <= 0)
		return;
	// The host thread that traces the task reaps it.
	if (!events.joinable()) {
		kill(task, SIGKILL);
		return;
	}
	std::unique_lock lock(events_mutex);
	if (tasks.contains(task)) {
		kill(task, SIGKILL);
		events_ready.wait(lock, [&] { return events_ended || !tasks.contains(task); });
	}
	if (const auto found = stops.find(task); found != stops.end())
		queued_stops.fetch_sub(int64_t(found->second.size()), std::memory_order_release);
	stops.erase(task);
}

void NativeChild::pause_task()
{
	if (pid <= 0)
		throw std::runtime_error("native task has no live process");
	user_regs_struct registers{};
	if (ptrace(PTRACE_GETREGS, pid, nullptr, &registers) == 0)
		return;
	if (errno != ESRCH || kill(pid, SIGSTOP) != 0)
		throw std::runtime_error("cannot interrupt native task");
	set_timeout(std::chrono::seconds(2));
	const int status = wait();
	if (!WIFSTOPPED(status))
		throw std::runtime_error("native task ended while being suspended");
	if (WSTOPSIG(status) != SIGSTOP) {
		// Its own trap won the race. Drain our pending signal without losing the trap or its fault information.
		siginfo_t info{};
		trace(PTRACE_GETSIGINFO, pid, &info);
		trace(PTRACE_SYSEMU, pid);
		const int interrupted = wait();
		if (!WIFSTOPPED(interrupted) || WSTOPSIG(interrupted) != SIGSTOP)
			throw std::runtime_error("native task did not receive its suspend interrupt");
		trace(PTRACE_SETSIGINFO, pid, &info);
	}
	std::lock_guard lock(events_mutex);
	stops[pid].push_front(status);
	queued_stops.fetch_add(1, std::memory_order_release);
	stops_generation.fetch_add(1, std::memory_order_release);
}

void NativeChild::stop()
{
	if (events.joinable()) {
		kill(-group, SIGKILL);
		events.join();
		pid = -1;
	}
	if (execution_wake >= 0) {
		close(execution_wake);
		execution_wake = -1;
	}
	if (host_channel != -1) {
		close(host_channel);
		host_channel = -1;
	}
	if (pid == -1)
		return;
	// Each task has a host thread of its own then: they all go with the process, and each host thread reaps its own.
	if (threaded_host() && group > 0)
		kill(-group, SIGKILL);
	kill(pid, SIGKILL);
	int status = 0;
	while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
	forget_registers();
	pid = -1;
}

void NativeChild::send_descriptor(int descriptor)
{
	char byte = 1;
	iovec vector{&byte, 1};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
	msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof control;
	auto *header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	std::memcpy(CMSG_DATA(header), &descriptor, sizeof descriptor);
	ssize_t sent;
	do { sent = sendmsg(host_channel, &message, MSG_NOSIGNAL | MSG_DONTWAIT); } while (sent == -1 && errno == EINTR);
	if (sent != 1)
		throw std::runtime_error("cannot send native mapping descriptor");
}

// Whether the guest sleeps inside the kernel while it runs its own code. Its system calls all stop for the runtime, so such a sleep holds
// it within one instruction: a locked access that splits a cache line, which Linux delays by several milliseconds and drops when a signal
// arrives meanwhile, to start it over.
static bool held_in_kernel(pid_t pid)
{
	char path[32];
	std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
	const int file = ::open(path, O_RDONLY | O_CLOEXEC);
	if (file < 0)
		return false;
	char text[512];
	const ssize_t length = ::read(file, text, sizeof text - 1);
	::close(file);
	if (length <= 0)
		return false;
	text[length] = 0;
	const char *name_end = std::strrchr(text, ')');
	return name_end && name_end[1] == ' ' && (name_end[2] == 'S' || name_end[2] == 'D');
}

// The tracer thread's wake-up while it is blocked waiting for the guest: a one-shot timer that interrupts waitpid with a signal whose
// handler does nothing. The timer is disarmed as soon as the wait returns and the signal is never blocked, so it cannot stay pending and
// interrupt anything else.
static void interrupt_wait(int) {}

static timer_t wait_timer()
{
	static thread_local std::optional<timer_t> timer;
	if (timer)
		return *timer;
	static std::once_flag installed;
	std::call_once(installed, [] {
		struct sigaction action{};
		action.sa_handler = interrupt_wait;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGRTMIN + 5, &action, nullptr) == -1)
			throw std::runtime_error("cannot install the guest wait alarm");
	});
	sigevent event{};
	event.sigev_notify = SIGEV_THREAD_ID;
	event.sigev_signo = SIGRTMIN + 5;
	event._sigev_un._tid = gettid();
	timer_t created;
	if (timer_create(CLOCK_MONOTONIC, &event, &created) == -1)
		throw std::runtime_error("cannot create the guest wait alarm");
	timer = created;
	return created;
}

int NativeChild::wait(const std::function<bool()> *served)
{
	if (events.joinable()) {
		std::unique_lock lock(events_mutex);
		if (!events_ready.wait_until(lock, deadline, [&] { return stops.contains(pid) || events_ended; }))
			return -1;
		const auto found = stops.find(pid);
		if (found == stops.end())
			throw std::runtime_error("native execution event stream ended");
		const int status = found->second.front();
		found->second.pop_front();
		queued_stops.fetch_sub(1, std::memory_order_release);
		if (found->second.empty())
			stops.erase(found);
		forget_registers();
		return status;
	}
	// Guest code usually reaches its next trap within microseconds, so it is polled first; a guest that runs longer is waited for
	// asleep, which leaves the processor to it. Waiting asleep at once costs a wake-up on every stop, which a program that stops thousands
	// of times a second, as a game's graphics threads do, pays in frames. On a loaded machine a sleeping tracer is also woken late, so a
	// window that handles one input in a few hundred stops took seconds with a short poll; two thousand tries keep it within its frame.
	// A served call made meanwhile ends in a trap, which wakes the wait.
	unsigned polls = 0;
	for (;;) {
		// A guest that is being served keeps running, so it is as busy as one that just started; it never stops for its calls, so its
		// slice is looked at after each of them.
		if (served && (*served)()) {
			polls = 0;
			if (slice_end && std::chrono::steady_clock::now() >= *slice_end)
				return -2;
			continue;
		}
		int status = 0;
		pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == 0) {
			const auto now = std::chrono::steady_clock::now();
			if (slice_end && now >= *slice_end)
				return -2;
			if (now >= deadline)
				return -1;
			if (++polls < 2000)
				continue;
			const auto until = slice_end ? std::min(*slice_end, deadline) : deadline;
			const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count();
			itimerspec alarm{};
			alarm.it_value.tv_sec = left / 1000000000;
			alarm.it_value.tv_nsec = left % 1000000000;
			const timer_t timer = wait_timer();
			timer_settime(timer, 0, &alarm, nullptr);
			result = waitpid(pid, &status, 0);
			const itimerspec off{};
			timer_settime(timer, 0, &off, nullptr);
		}
		if (result == pid) {
			forget_registers();
			if (WIFEXITED(status) || WIFSIGNALED(status)) {
				if (std::getenv("RAVEN_TRACE_API"))
					std::cerr << "raven: native guest " << (WIFSIGNALED(status) ? "killed by signal " + std::to_string(WTERMSIG(status)) : "exited with " + std::to_string(WEXITSTATUS(status))) << '\n';
				pid = -1;
			}
			return status;
		}
		if (result == -1 && errno != EINTR)
			throw std::runtime_error("cannot wait for native guest");
	}
}

uint64_t NativeChild::syscall(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3,
	uint64_t a4, uint64_t a5, uint64_t a6)
{
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, pid, &registers);
	registers.rip = gate + 1;
	// The gate is a 64-bit SYSCALL; a compatibility-mode guest is switched out only for this instruction.
	registers.cs = 0x33;
	registers.ss = 0x2b;
	registers.rax = number;
	registers.orig_rax = uint64_t(-1);
	registers.rdi = a1;
	registers.rsi = a2;
	registers.rdx = a3;
	registers.r10 = a4;
	registers.r8 = a5;
	registers.r9 = a6;
	trace(PTRACE_SETREGS, pid, &registers);
	// PTRACE_CONT executes only this owned gate, never guest code. A slice left over from the last resume must not end this wait.
	clear_slice();
	trace(PTRACE_CONT, pid);
	const int status = wait();
	if (status == -1 || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
		std::ostringstream detail;
		detail << "native syscall did not reach its trap (wait status 0x" << std::hex << status << ')';
		throw std::runtime_error(detail.str());
	}
	trace(PTRACE_GETREGS, pid, &registers);
	if (registers.rip != gate + 4)
		throw std::runtime_error("native syscall stopped outside its gate");
	if (registers.rax >= uint64_t(-4095))
		throw std::runtime_error("native syscall: " + std::string(std::strerror(static_cast<int>(-registers.rax))));
	return registers.rax;
}

std::vector<uint64_t> NativeChild::syscalls(std::span<const std::array<uint64_t, 7>> calls)
{
	std::vector<uint64_t> results;
	results.reserve(calls.size());
	// A bootstrap without the batch gate, or a call or two, makes them one by one.
	if (!batch_gate || calls.size() < 3) {
		for (const auto &call : calls)
			results.push_back(syscall(call[0], call[1], call[2], call[3], call[4], call[5], call[6]));
		return results;
	}
	const size_t bytes = calls.size() * sizeof calls[0];
	const size_t length = (bytes + 4095) & ~size_t(4095);
	const auto table = syscall(SYS_mmap, 0, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, uint64_t(-1));
	std::vector<std::array<uint64_t, 7>> done(calls.begin(), calls.end());
	const auto move = [&](bool write) {
		iovec local{done.data(), bytes}, remote{reinterpret_cast<void *>(table), bytes};
		const auto moved = write ? process_vm_writev(pid, &local, 1, &remote, 1, 0) : process_vm_readv(pid, &local, 1, &remote, 1, 0);
		if (moved != ssize_t(bytes))
			throw std::runtime_error("native syscall batch could not reach its table");
	};
	move(true);
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, pid, &registers);
	registers.rip = batch_gate;
	registers.cs = 0x33;
	registers.ss = 0x2b;
	registers.orig_rax = uint64_t(-1);
	registers.rdi = table;
	registers.rsi = calls.size();
	trace(PTRACE_SETREGS, pid, &registers);
	clear_slice();
	trace(PTRACE_CONT, pid);
	const int status = wait();
	if (status == -1 || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
		throw std::runtime_error("native syscall batch did not reach its trap");
	trace(PTRACE_GETREGS, pid, &registers);
	if (registers.rip <= batch_gate || registers.rip > batch_gate + 64)
		throw std::runtime_error("native syscall batch stopped outside its gate");
	move(false);
	syscall(SYS_munmap, table, length);
	for (const auto &call : done) {
		if (call[0] >= uint64_t(-4095))
			throw std::runtime_error("native syscall: " + std::string(std::strerror(static_cast<int>(-call[0]))));
		results.push_back(call[0]);
	}
	return results;
}

struct NativeProcess::State {
	std::unique_ptr<NativeChild> child;
	pid_t memory_task = 0;
	uint64_t atomic_gate = 0;
	// The routine that carries out what a host thread asks of its guest waiting in a stub; zero when stubs cannot be asked.
	uint64_t request_gate = 0;
	struct SharedView {
		void *host = MAP_FAILED;
		size_t size = 0;
		~SharedView() { if (host != MAP_FAILED) munmap(host, size); }
	};
	std::map<uint64_t, std::unique_ptr<SharedView>> shared_views;
	// The guest's own mappings by start address; they never overlap. A program such as a browser makes thousands of them, and every
	// read, write and protection change looks one up.
	std::map<uint64_t, ImageMapping> mappings;
	// Counts every change to the mappings, so that a reader can tell whether a protection it saw may have changed since.
	uint64_t mapping_changes = 0;
	std::vector<std::pair<uint64_t, uint64_t>> allocations;
	std::vector<std::pair<uint64_t, uint64_t>> image_allocations;
	// The API breakpoints (see ApiTrap).
	std::map<uint64_t, ApiTrap> api_traps;
	// Where add_api_trap put the last one: traps come by the thousand in rising order, and the next goes right after it.
	std::map<uint64_t, ApiTrap>::iterator last_trap = api_traps.end();
	// The guest page add_api_trap last read, for the traps next to each other that follow; cleared whenever guest memory changes.
	std::vector<uint8_t> trap_page;
	uint64_t trap_page_address = 0;
	NativeMode mode = NativeMode::x64;
	uint64_t return_gate = 0;
	uint32_t fs_selector = 0;
	uint32_t ldt_entries = 1;
	// A translated call: arguments and floating point arguments supplied by an adapter, and the result it left.
	// Time slicing of guest threads: how long one may run before another gets a turn, whether another could use one, and the hand-over.
	std::chrono::milliseconds slice{0};
	std::chrono::steady_clock::time_point slice_started = std::chrono::steady_clock::now();
	std::function<bool()> slice_wanted;
	std::function<void()> slice_yield;
	// Cumulative counters can be sampled between debugger attaches without tracing the guest's syscalls.
	uint64_t slice_interrupts = 0, slice_deferrals = 0, slice_yield_requests = 0;
	uint64_t served_calls = 0, served_waits = 0, served_yields = 0;
	bool served_declined = false;
	uint64_t queued_calls = 0;
	std::optional<std::vector<uint64_t>> translated_words;
	// How deep in guest callbacks the translated call was made: a callback the handler starts makes real calls of its own.
	size_t translated_depth = 0;
	std::array<uint64_t, 4> translated_floats{};
	uint64_t translated_result = 0;
	uint64_t fs_base = 0;
	uint32_t pending_cleanup = 0;
	std::vector<NativeContext> callbacks;
	std::vector<uint32_t> callback_cleanups;
	uint64_t stack_top = 0;
	// The first thread keeps its stack for the life of the process; other threads borrow stack_top while they run.
	uint64_t initial_stack_top = 0;
	uint64_t initial_stack_size = 1024 * 1024;
	bool stopped = true;
	bool active_call = false;
	bool awaiting_api = false;
	bool awaiting_syscall = false;
	// The guest's own system calls stop it through a seccomp filter instead of system call emulation: only those of the runtime's gate
	// run, every other one stops the guest before it runs, as emulation did.
	bool seccomp = false;
	// How a guest task is let run its own code: a system call it makes then stops it for the runtime instead of reaching the host,
	// by the filter where there is one. A task resumed any other way would make the host's system calls with the guest's numbers.
	__ptrace_request guest_resume() const { return seccomp ? PTRACE_CONT : PTRACE_SYSEMU; }
	bool debug_print_fault = false;
	size_t steps_left = 0;
	std::function<void(uint64_t)> step_sink;
	// Calls served while the guest runs: the block the guest's stubs fill, the handler, and whether one is being served now.
	uint64_t *served_block = nullptr;
	uint64_t thread_teb = 0;
	uint64_t thread_served_guest = 0;
	uint64_t *thread_served_block = nullptr;
	// Where the served stubs start, in order, and how many bytes each covers before its fallback trap.
	std::vector<uint64_t> served_stubs;
	std::vector<uint64_t> queued_stubs;
	// Walked at every synchronous served call, so it is kept flat.
	std::vector<std::pair<uint64_t, uint64_t *>> queued_blocks;
	std::map<uint64_t *, uint64_t> served_owners;
	std::vector<uint64_t> served_yield_traps;
	size_t served_stub_size = 0;
	// The breakpoint a stub reaches when it has waited too long for its answer.
	size_t served_wait_offset = 0;
	// The stubs of the calls a host thread answers for its own guest thread, in order; they are laid out as the served ones.
	std::vector<uint64_t> call_stubs;
	uint64_t call_answers = 0, call_settles = 0, call_waits = 0;
	std::function<void(uint64_t)> served_handler;
	std::function<void()> resume_hook;
	std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> execution_wait;
	std::function<void(std::function<bool()>)> execution_turn;
	std::function<int(pid_t, std::chrono::steady_clock::time_point, const uint64_t *)> execution_park;
	std::function<std::optional<uint64_t>(uint64_t, const uint64_t *)> concurrent_served;
	std::function<bool()> poll_served;
	bool serving = false, serving_queued = false;
	// Diagnostics: where the guest is found running, every sample_period.
	std::chrono::milliseconds sample_period{0};
	std::function<void(uint64_t)> sampler;
	// Where the stack arguments of the call being served start; they are read only when a handler asks for them.
	uint64_t served_stack = 0;
	void require_stopped() const;
	void require_memory() const;
	bool covers(uint64_t address, size_t size, int protection) const;
	uint8_t *shared_memory(uint64_t address, size_t &size) const;
	uint64_t limit() const { return mode == NativeMode::x86 ? 0x100000000ULL : 0x800000000000ULL; }
};

uint8_t *NativeProcess::State::shared_memory(uint64_t address, size_t &size) const
{
	const auto after = shared_views.upper_bound(address);
	if (after != shared_views.begin()) {
		const auto &view = *std::prev(after);
		if (address - view.first < view.second->size) {
			const auto offset = size_t(address - view.first);
			size = std::min(size, view.second->size - offset);
			return static_cast<uint8_t *>(view.second->host) + offset;
		}
	}
	if (after != shared_views.end())
		size = std::min(size, size_t(after->first - address));
	return nullptr;
}

void NativeProcess::State::require_stopped() const
{
	if (!child || child->pid == -1) {
		if (std::getenv("RAVEN_BACKTRACE")) {
			void *frames[32];
			backtrace_symbols_fd(frames, backtrace(frames, 32), 2);
		}
		throw std::runtime_error("native process is not stopped and alive (the guest process is gone)");
	}
	if (!stopped && !serving && !waiting.task && !requesting) {
		if (std::getenv("RAVEN_BACKTRACE")) {
			void *frames[32];
			backtrace_symbols_fd(frames, backtrace(frames, 32), 2);
		}
		throw std::runtime_error("native process is not stopped and alive (it is running or a resume ended without a stop)");
	}
}

void NativeProcess::State::require_memory() const
{
	if (!child || child->pid == -1)
		throw std::runtime_error("native guest process is gone");
	if (child->execution_descriptor() < 0)
		require_stopped();
}

bool NativeProcess::State::covers(uint64_t address, size_t size, int protection) const
{
	if (size > UINT64_MAX - address)
		return false;
	const uint64_t end = address + size;
	while (address < end) {
		auto found = mappings.upper_bound(address);
		if (found == mappings.begin())
			return false;
		const auto &mapping = std::prev(found)->second;
		if (address - mapping.address >= mapping.size || (mapping.protection & protection) != protection)
			return false;
		address = std::min(end, mapping.address + mapping.size);
	}
	return true;
}

NativeProcess::NativeProcess(std::span<LoadedImage *const> images, const std::string &bootstrap,
	std::chrono::milliseconds setup_timeout, NativeMode mode, uint64_t stack_reserve) : state(std::make_unique<State>())
{
	if (setup_timeout.count() <= 0)
		throw std::runtime_error("native execution requires a positive timeout");
	// Whatever a start put off until its own process is done before the guest exists.
	warm_settle();
	state->mode = mode;
	std::vector<int> descriptors;
	for (auto *mapped_image : images) {
		if (!mapped_image)
			throw std::runtime_error("null native image mapping");
		if (mapped_image->data().size() > state->limit() || mapped_image->base() > state->limit() - mapped_image->data().size())
			throw std::runtime_error("native image does not fit the guest address space");
		descriptors.push_back(mapped_image->descriptor());
	}
	state->child = std::make_unique<NativeChild>(descriptors, bootstrap, setup_timeout);
	auto &child = *state->child;
	// Every image is mapped, and each of its sections given its protection, in one stop of the guest.
	std::vector<std::array<uint64_t, 7>> calls;
	std::vector<size_t> image_calls;
	for (auto *mapped_image : images) {
		image_calls.push_back(calls.size());
		calls.push_back({SYS_mmap, mapped_image->base(), mapped_image->data().size(), PROT_NONE, MAP_SHARED | MAP_FIXED_NOREPLACE,
			static_cast<uint64_t>(mapped_image->descriptor()), 0});
		for (const auto &mapping : mapped_image->mappings())
			calls.push_back({SYS_mprotect, mapping.address, mapping.size, static_cast<uint64_t>(mapping.protection), 0, 0, 0});
	}
	const auto results = child.syscalls(calls);
	for (size_t index = 0; index < images.size(); ++index) {
		if (results[image_calls[index]] != images[index]->base())
			throw std::runtime_error("host does not support exact non-replacing guest mappings");
		for (const auto &mapping : images[index]->mappings()) {
			state->mappings.insert_or_assign(mapping.address, mapping);
			++state->mapping_changes;
		}
	}
	// The first thread's stack is as large as the program's headers reserve, as Windows makes it, from 1 MiB to 64 MiB.
	const uint64_t stack_size = std::clamp<uint64_t>((stack_reserve + 0xffff) & ~uint64_t(0xffff), 1024 * 1024, 64 * 1024 * 1024);
	state->initial_stack_size = stack_size;
	const uint64_t low_flag = mode == NativeMode::x86 ? MAP_32BIT : 0;
	const auto stack = child.syscall(SYS_mmap, 0, stack_size + 8192, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | low_flag, uint64_t(-1));
	if (stack + stack_size + 8192 > state->limit())
		throw std::runtime_error("host placed the native stack outside the guest address space");
	child.syscall(SYS_mprotect, stack + 4096, stack_size, PROT_READ | PROT_WRITE);
	state->mappings.insert_or_assign(stack + 4096, ImageMapping{stack + 4096, stack_size, 0, PROT_READ | PROT_WRITE});
	++state->mapping_changes;
	state->stack_top = stack + 4096 + stack_size;
	state->initial_stack_top = state->stack_top;
	if (mode == NativeMode::x86) {
		// 32-bit return addresses cannot name the 64-bit bootstrap gate, so calls return through an INT3 page below 4 GiB.
		const auto page = child.syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, uint64_t(-1));
		if (page + 4096 > state->limit())
			throw std::runtime_error("host placed the native return gate outside the guest address space");
		state->mappings.insert_or_assign(page, ImageMapping{page, 4096, 0, PROT_READ | PROT_WRITE});
		++state->mapping_changes;
		write_memory(page, std::vector<uint8_t>{0xcc});
		child.syscall(SYS_mprotect, page, 4096, PROT_READ | PROT_EXEC);
		state->mappings.at(page).protection = PROT_READ | PROT_EXEC;
		state->return_gate = page;
	}
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, child.pid, &registers);
	registers.rax = registers.rbx = registers.rcx = registers.rdx = registers.rsi = registers.rdi = registers.rbp = 0;
	registers.r8 = registers.r9 = registers.r10 = registers.r11 = registers.r12 = registers.r13 = registers.r14 = registers.r15 = 0;
	registers.fs_base = registers.gs_base = 0;
	registers.orig_rax = uint64_t(-1);
	registers.eflags = 0x202;
	trace(PTRACE_SETREGS, child.pid, &registers);
	context();
	if (std::getenv("RAVEN_PARALLEL")) {
		child.enable_parallel();
		// This task only executes owned memory gates, including while every Windows thread is running.
		state->memory_task = child.clone_task(state->stack_top);
		state->atomic_gate = map_memory(4096, PROT_READ | PROT_WRITE);
		// The page of gates (gates.S): the locked exchanges, and the routine a stub runs what the runtime asks of the guest with.
		const auto gates = served_stub::gate_page();
		write_memory(state->atomic_gate, gates);
		if (mode == NativeMode::x64 && threaded_host()) {
			static_assert(request_slot * 8 == 2400 && request_routine * 8 == 2560, "the request routine's offsets are written into its code");
			state->request_gate = state->atomic_gate + served_stub::gate_offset(raven_gate_request);
		}
		protect_memory(state->atomic_gate, 4096, PROT_READ | PROT_EXEC);
	}
	// The filter goes in once the page of gates is there, since a guest waiting in its stub makes the runtime's system calls from it.
	if (mode == NativeMode::x64 && std::getenv("RAVEN_SECCOMP"))
		install_seccomp();
}

NativeProcess::~NativeProcess()
{
	if (std::getenv("RAVEN_PROFILE") && std::getenv("RAVEN_QUEUED_GRAPHICS"))
		std::cerr << "profile: queued calls=" << state->queued_calls << '\n';
}

void NativeProcess::install_seccomp()
{
	auto &child = *state->child;
	// The filter: an x86-64 system call whose instruction pointer is the one after the gate's SYSCALL runs; any other stops the guest
	// for the runtime (SECCOMP_RET_TRACE), which then skips it as it skipped emulated ones.
	// A guest waiting in its stub makes the runtime's calls from the page of gates (raven_gate_request_syscall): that instruction
	// pointer runs as well, or the gate's again when there is no such page. Both lie in pages only the runtime writes.
	const uint64_t after_gate = child.gate + 3;
	const uint64_t after_request = state->request_gate ? state->atomic_gate + served_stub::gate_offset(raven_gate_request_syscall_done) : after_gate;
	const sock_filter program[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 8),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, instruction_pointer)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, uint32_t(after_gate), 0, 2),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, uint32_t(after_gate >> 32), 5, 0),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, instruction_pointer)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, uint32_t(after_request), 0, 2),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, instruction_pointer) + 4),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, uint32_t(after_request >> 32), 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	const auto page = child.syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, uint64_t(-1));
	std::vector<uint8_t> bytes(sizeof program + 16);
	std::memcpy(bytes.data() + 16, program, sizeof program);
	const uint16_t count = sizeof program / sizeof program[0];
	const uint64_t filter = page + 16;
	std::memcpy(bytes.data(), &count, 2);
	std::memcpy(bytes.data() + 8, &filter, 8);
	const iovec local{bytes.data(), bytes.size()};
	const iovec remote{reinterpret_cast<void *>(page), bytes.size()};
	if (process_vm_writev(child.pid, &local, 1, &remote, 1, 0) != ssize_t(bytes.size()))
		throw std::runtime_error("cannot write the native seccomp filter");
	child.syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	// Every task of the guest takes the filter, the memory task as well, as tasks cloned after it would.
	child.syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, page);
	child.syscall(SYS_munmap, page, 4096);
	// A guest that runs in parallel keeps following its new threads.
	const long clones = state->memory_task > 0 ? PTRACE_O_TRACECLONE : 0;
	trace(PTRACE_SETOPTIONS, child.pid, reinterpret_cast<void *>(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACESECCOMP | clones));
	state->seccomp = true;
}

NativeContext NativeProcess::context() const
{
	state->require_stopped();
	NativeContext result{};
	trace(PTRACE_GETREGS, state->child->pid, &result.registers);
	// The size of the extended state is the processor's, the same for every read: it is learned once, and later reads take a buffer of
	// exactly that size instead of zeroing 64 KiB for each thread switch.
	static size_t xstate_size = 0;
	result.xstate.resize(xstate_size ? xstate_size : 65536);
	iovec buffer{result.xstate.data(), result.xstate.size()};
	if (ptrace(PTRACE_GETREGSET, state->child->pid, reinterpret_cast<void *>(NT_X86_XSTATE), &buffer) == -1)
		throw std::runtime_error("host cannot expose native XSTATE");
	if (buffer.iov_len < 512 || (xstate_size ? buffer.iov_len != xstate_size : buffer.iov_len >= result.xstate.size()))
		throw std::runtime_error("invalid native XSTATE size");
	xstate_size = buffer.iov_len;
	result.xstate.resize(buffer.iov_len);
	return result;
}

void NativeProcess::set_context(const NativeContext &context)
{
	state->require_stopped();
	// The general registers go first: if the extended state is then refused, they are put back, and nothing has changed.
	// A context saved for a callback carries the general registers alone.
	if (context.xstate.empty()) {
		trace(PTRACE_SETREGS, state->child->pid, const_cast<user_regs_struct *>(&context.registers));
		state->thread_teb = context.registers.gs_base;
		return;
	}
	user_regs_struct previous{};
	trace(PTRACE_GETREGS, state->child->pid, &previous);
	trace(PTRACE_SETREGS, state->child->pid, const_cast<user_regs_struct *>(&context.registers));
	iovec buffer{const_cast<uint8_t *>(context.xstate.data()), context.xstate.size()};
	if (ptrace(PTRACE_SETREGSET, state->child->pid, reinterpret_cast<void *>(NT_X86_XSTATE), &buffer) == -1 || buffer.iov_len != context.xstate.size()) {
		try {
			trace(PTRACE_SETREGS, state->child->pid, &previous);
		} catch (...) {
			state->child->stop();
		}
		throw std::runtime_error("cannot restore native XSTATE");
	}
	state->thread_teb = context.registers.gs_base;
}

std::vector<uint8_t> NativeProcess::read_memory(uint64_t address, size_t size) const
{
	state->require_memory();
	if (!state->covers(address, size, PROT_READ))
		throw std::runtime_error("native read exceeds readable guest memory" + (std::getenv("RAVEN_TRACE_API") ? " at " + std::to_string(address) + "+" + std::to_string(size) : std::string()));
	std::vector<uint8_t> result(size);
	for (size_t offset = 0; offset < size;) {
		size_t count = size - offset;
		if (const auto *shared = state->shared_memory(address + offset, count)) {
			std::memcpy(result.data() + offset, shared, count);
			offset += count;
			continue;
		}
		const iovec local{result.data() + offset, count};
		const iovec remote{reinterpret_cast<void *>(address + offset), count};
		const auto copied = process_vm_readv(state->child->pid, &local, 1, &remote, 1, 0);
		if (copied < 0 && errno == EINTR) continue;
		if (copied <= 0)
			throw std::runtime_error("cannot read native guest memory");
		offset += static_cast<size_t>(copied);
	}
	return result;
}

bool NativeProcess::read_alone(uint64_t address, void *into, size_t size) const
{
	for (size_t offset = 0; offset < size;) {
		const iovec local{static_cast<uint8_t *>(into) + offset, size - offset};
		const iovec remote{reinterpret_cast<void *>(address + offset), size - offset};
		const auto copied = process_vm_readv(state->child->pid, &local, 1, &remote, 1, 0);
		if (copied < 0 && errno == EINTR)
			continue;
		if (copied <= 0)
			return false;
		offset += size_t(copied);
	}
	return true;
}

bool NativeProcess::write_alone(uint64_t address, const void *from, size_t size)
{
	for (size_t offset = 0; offset < size;) {
		const iovec local{const_cast<uint8_t *>(static_cast<const uint8_t *>(from)) + offset, size - offset};
		const iovec remote{reinterpret_cast<void *>(address + offset), size - offset};
		const auto copied = process_vm_writev(state->child->pid, &local, 1, &remote, 1, 0);
		if (copied < 0 && errno == EINTR)
			continue;
		if (copied <= 0)
			return false;
		offset += size_t(copied);
	}
	return true;
}

void NativeProcess::write_memory(uint64_t address, std::span<const uint8_t> bytes)
{
	state->trap_page.clear();
	validate_write_memory(address,bytes.size());
	for (size_t offset = 0; offset < bytes.size();) {
		size_t count = bytes.size() - offset;
		if (auto *shared = state->shared_memory(address + offset, count)) {
			std::memcpy(shared, bytes.data() + offset, count);
			offset += count;
			continue;
		}
		const iovec local{const_cast<uint8_t *>(bytes.data() + offset), count};
		const iovec remote{reinterpret_cast<void *>(address + offset), count};
		const auto copied = process_vm_writev(state->child->pid, &local, 1, &remote, 1, 0);
		if (copied < 0 && errno == EINTR) continue;
		if (copied <= 0)
			throw std::runtime_error("cannot write native guest memory");
		offset += static_cast<size_t>(copied);
	}
}

void NativeProcess::validate_write_memory(uint64_t address,size_t size) const
{
	state->require_memory();
	if (!state->covers(address,size,PROT_WRITE)) throw std::runtime_error("native write exceeds writable guest memory");
}

uint64_t NativeProcess::compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, size_t size)
{
	if ((size != 4 && size != 8) || address % size)
		throw std::runtime_error("invalid native atomic memory operand");
	validate_write_memory(address, size);
	if (!parallel()) {
		const auto bytes = read_memory(address, size);
		uint64_t previous = 0;
		std::memcpy(&previous, bytes.data(), size);
		if (previous == (size == 4 ? uint32_t(expected) : expected))
			write_memory(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&desired), size));
		return previous;
	}
	// A guest that waits in a stub does the exchange itself.
	if (waiting.task && state->request_gate && !waiting.answered) {
		const uint64_t arguments[] = {address, expected, desired};
		const uint64_t previous = run_in_stub(state->atomic_gate + served_stub::gate_offset(size == 8 ? raven_gate_request_exchange64 : raven_gate_request_exchange32), arguments);
		return size == 4 ? uint32_t(previous) : previous;
	}
	settle_call();
	auto &child = *state->child;
	const pid_t caller = child.pid;
	// A host thread for each guest thread runs the gate in its own stopped task: only the administrative thread traces the other one.
	if (!threaded_host() || !state->stopped)
		child.pid = state->memory_task;
	user_regs_struct saved{}, registers{};
	try {
		trace(PTRACE_GETREGS, child.pid, &saved);
		registers = saved;
		registers.rip = state->atomic_gate + served_stub::gate_offset(size == 8 ? raven_gate_exchange64 : raven_gate_exchange32);
		registers.rax = expected;
		registers.rcx = address;
		registers.rdx = desired;
		trace(PTRACE_SETREGS, child.pid, &registers);
		child.set_timeout(std::chrono::seconds(2));
		trace(PTRACE_CONT, child.pid);
		const int status = child.wait();
		trace(PTRACE_GETREGS, child.pid, &registers);
		if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP || registers.rip != state->atomic_gate + served_stub::gate_offset(size == 8 ? raven_gate_exchange64_done : raven_gate_exchange32_done) + 1)
			throw std::runtime_error("native atomic operation lost its gate");
		trace(PTRACE_SETREGS, child.pid, &saved);
	} catch (...) {
		child.stop();
		throw;
	}
	child.pid = caller;
	forget_registers();
	return size == 4 ? uint32_t(registers.rax) : registers.rax;
}

uint64_t NativeProcess::memory_syscall(uint64_t number, uint64_t a1, uint64_t a2,
	uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
	state->require_memory();
	// A guest that waits in its stub makes the call itself, from the page of gates, and goes back to waiting: neither it nor the memory
	// task stops, which a traced call needs four exchanges with the kernel for. Only a filtered guest can: without the filter, a system
	// call of the guest stops it for the runtime wherever it is made.
	if (waiting.task && state->request_gate && !waiting.answered && state->seccomp) {
		const uint64_t arguments[] = {number, a1, a2, a3, a4, a5, a6};
		const uint64_t result = run_in_stub(state->atomic_gate + served_stub::gate_offset(raven_gate_request_syscall), arguments);
		if (result >= uint64_t(-4095))
			throw std::runtime_error("native syscall: " + std::string(std::strerror(static_cast<int>(-result))));
		return result;
	}
	settle_call();
	// A Linux system call leaves the extended processor state alone, so only the general registers the gate uses are kept.
	auto &child = *state->child;
	struct SelectedTask {
		NativeChild &child;
		pid_t previous;
		~SelectedTask()
		{
			if (child.pid != -1)
				child.pid = previous;
			forget_registers();
		}
	} selected{child, child.pid};
	if (!state->stopped) {
		if (state->memory_task <= 0)
			throw std::runtime_error("native process has no memory task");
		child.pid = state->memory_task;
	}
	user_regs_struct saved{};
	trace(PTRACE_GETREGS, child.pid, &saved);
	child.set_timeout(std::chrono::seconds(2));
	uint64_t result = 0;
	try {
		result = child.syscall(number, a1, a2, a3, a4, a5, a6);
	} catch (...) {
		try {
			user_regs_struct where{};
			if (child.pid != -1)
				trace(PTRACE_GETREGS, child.pid, &where);
			if (child.pid == -1 || where.rip != child.gate + 4)
				throw std::runtime_error("native memory operation lost its gate");
			trace(PTRACE_SETREGS, child.pid, &saved);
		} catch (...) {
			child.stop();
			throw;
		}
		throw;
	}
	try {
		trace(PTRACE_SETREGS, child.pid, &saved);
	} catch (...) {
		child.stop();
		throw;
	}
	return result;
}

static void memory_range(uint64_t address, size_t size, int protection, uint64_t limit)
{
	if (!size || size % 4096 || size > (uint64_t(1) << 42) || address % 4096 ||
		address >= limit || size > limit - address || (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)))
		throw std::runtime_error("invalid native memory range or protection");
}

// MAP_32BIT only reaches the 1 GiB window that also holds the images, so 32-bit allocations take the lowest free 64 KiB aligned gap
// of the child's own address space and are then mapped there without replacing anything.
static uint64_t low_gap(pid_t pid, uint64_t size, uint64_t limit)
{
	std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
	constexpr uint64_t granularity = 0x10000;
	uint64_t candidate = granularity;
	std::string line;
	while (std::getline(maps, line)) {
		const auto dash = line.find('-');
		const uint64_t start = std::stoull(line.substr(0, dash), nullptr, 16);
		const uint64_t end = std::stoull(line.substr(dash + 1), nullptr, 16);
		if (start >= limit)
			break;
		if (start >= candidate && start - candidate >= size)
			return candidate;
		candidate = std::max(candidate, (end + granularity - 1) & ~(granularity - 1));
	}
	return candidate < limit && limit - candidate >= size ? candidate : 0;
}

uint64_t NativeProcess::map_memory(size_t size, int protection, uint64_t address)
{
	state->trap_page.clear();
	state->require_memory();
	memory_range(address, size, protection, state->limit());
	state->allocations.reserve(state->allocations.size() + 1);
	const auto hint = address ? address : state->mode == NativeMode::x86 ? low_gap(state->child->pid, size, state->limit()) : 0;
	const auto mapped = memory_syscall(SYS_mmap, hint, size, static_cast<uint64_t>(protection),
		MAP_PRIVATE | MAP_ANONYMOUS | (protection == PROT_NONE ? MAP_NORESERVE : 0) | (hint ? MAP_FIXED_NOREPLACE : 0), uint64_t(-1));
	if ((address && mapped != address) || mapped >= state->limit() || size > state->limit() - mapped) {
		state->child->stop();
		throw std::runtime_error("host returned an invalid native allocation address");
	}
	state->mappings.insert_or_assign(mapped, ImageMapping{mapped, size, 0, protection});
	++state->mapping_changes;
	state->allocations.emplace_back(mapped, size);
	return mapped;
}

uint64_t NativeProcess::map_shared(int descriptor, size_t size, uint64_t offset, uint64_t address)
{
	return map_file(descriptor, size, offset, false, address);
}

uint64_t NativeProcess::map_descriptor(int descriptor, size_t size, uint64_t offset)
{
	state->require_memory();
	memory_range(0, size, PROT_READ | PROT_WRITE, state->limit());
	const auto flags = fcntl(descriptor, F_GETFL);
	if (flags == -1 || (flags & O_ACCMODE) != O_RDWR || (offset & 4095) || offset > INT64_MAX || size > INT64_MAX - offset)
		throw std::runtime_error("invalid native mapping descriptor or offset");
	auto view = std::make_unique<State::SharedView>();
	view->size = size;
	view->host = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, off_t(offset));
	if (view->host == MAP_FAILED)
		throw std::runtime_error("cannot map native descriptor in host");
	const auto address = map_file(descriptor, size, offset, true);
	try { state->shared_views.emplace(address, std::move(view)); }
	catch (...) { unmap_memory(address); throw; }
	return address;
}

uint64_t NativeProcess::receive_descriptor(int descriptor, uint64_t scratch)
{
	// The trusted syscall gate uses the Linux x64 ABI for both PE architectures.
	iovec vector{reinterpret_cast<void *>(scratch + 80), 1};
	msghdr message{};
	message.msg_iov = reinterpret_cast<iovec *>(scratch + 64);
	message.msg_iovlen = 1;
	message.msg_control = reinterpret_cast<void *>(scratch + 96);
	message.msg_controllen = CMSG_SPACE(sizeof(int));
	std::array<uint8_t, 128> bytes{};
	static_assert(sizeof message <= 64 && sizeof vector <= 16 && CMSG_SPACE(sizeof(int)) <= 32);
	std::memcpy(bytes.data(), &message, sizeof message);
	std::memcpy(bytes.data() + 64, &vector, sizeof vector);
	write_memory(scratch, bytes);
	state->child->send_descriptor(descriptor);
	struct Transfer {
		NativeChild &child;
		bool received = false;
		~Transfer() { if (!received) child.stop(); }
	} transfer{*state->child};
	uint64_t result;
	do { result = memory_syscall(SYS_recvmsg, state->child->descriptor_socket(), scratch, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); }
	while (result == uint64_t(-EINTR));
	const auto answer = read_memory(scratch, bytes.size());
	std::memcpy(&message, answer.data(), sizeof message);
	cmsghdr header{};
	std::memcpy(&header, answer.data() + 96, sizeof header);
	int received = -1;
	std::memcpy(&received, answer.data() + 96 + CMSG_LEN(0), sizeof received);
	if (result != 1 || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) || message.msg_controllen != CMSG_SPACE(sizeof(int)) ||
		header.cmsg_level != SOL_SOCKET || header.cmsg_type != SCM_RIGHTS || header.cmsg_len != CMSG_LEN(sizeof(int)) || received < 0) {
		throw std::runtime_error("invalid native mapping descriptor transfer");
	}
	transfer.received = true;
	return uint64_t(received);
}

uint64_t NativeProcess::map_file(int descriptor, size_t size, uint64_t offset, bool transfer, uint64_t address)
{
	state->trap_page.clear();
	state->require_memory();
	memory_range(0, size, PROT_READ | PROT_WRITE, state->limit());
	state->allocations.reserve(state->allocations.size() + 1);
	const auto path = "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(descriptor);
	std::vector<uint8_t> name(path.begin(), path.end());
	name.push_back(0);
	const auto scratch = map_memory(4096, PROT_READ | PROT_WRITE);
	uint64_t guest_descriptor = uint64_t(-1);
	try {
		if (transfer) {
			guest_descriptor = receive_descriptor(descriptor, scratch);
		} else {
			write_memory(scratch, name);
			// Reopen only the host-owned memfd; guest code never executes the Linux syscall.
			guest_descriptor = memory_syscall(SYS_openat, uint64_t(AT_FDCWD), scratch, O_RDWR | O_CLOEXEC);
		}
		// Windows places views on the 64 KiB allocation granularity and programs round view addresses down to it, so a view starts on
		// such a boundary: a reservation one granule larger is made, the file is mapped over its aligned part and the slack is returned.
		// A view asked for at an address goes exactly there and needs no slack.
		const uint64_t granularity = address ? 0 : 0x10000;
		const auto hint = address ? address : state->mode == NativeMode::x86 ? low_gap(state->child->pid, size + granularity, state->limit()) : 0;
		const auto reserved = memory_syscall(SYS_mmap, hint, size + granularity, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (hint ? MAP_FIXED_NOREPLACE : 0), uint64_t(-1), 0);
		if (reserved >= state->limit() || size + granularity > state->limit() - reserved || (address && reserved != address)) {
			state->child->stop();
			throw std::runtime_error("host returned an invalid shared mapping address");
		}
		const uint64_t aligned = granularity ? (reserved + granularity - 1) & ~(granularity - 1) : reserved;
		const auto mapped = memory_syscall(SYS_mmap, aligned, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, guest_descriptor, offset);
		if (mapped != aligned) {
			state->child->stop();
			throw std::runtime_error("host returned an invalid shared mapping address");
		}
		if (aligned > reserved)
			memory_syscall(SYS_munmap, reserved, aligned - reserved);
		if (reserved + size + granularity > aligned + size)
			memory_syscall(SYS_munmap, aligned + size, reserved + size + granularity - aligned - size);
		memory_syscall(SYS_close, guest_descriptor, 0);
		guest_descriptor = uint64_t(-1);
		unmap_memory(scratch);
		state->mappings.insert_or_assign(mapped, ImageMapping{mapped, size, 0, PROT_READ | PROT_WRITE});
		++state->mapping_changes;
		state->allocations.emplace_back(mapped, size);
		return mapped;
	} catch (...) {
		try {
			if (guest_descriptor != uint64_t(-1))
				memory_syscall(SYS_close, guest_descriptor, 0);
			unmap_memory(scratch);
		} catch (...) {
			state->child->stop();
		}
		throw;
	}
}

uint64_t NativeProcess::mapping_changes() const
{
	return state->mapping_changes;
}

bool NativeProcess::has_protection(uint64_t address, size_t size, int protection) const
{
	const uint64_t end = address + size;
	while (address < end) {
		const auto after = state->mappings.upper_bound(address);
		if (after == state->mappings.begin())
			return false;
		const auto &mapping = std::prev(after)->second;
		if (address - mapping.address >= mapping.size || mapping.protection != protection)
			return false;
		address = mapping.address + mapping.size;
	}
	return true;
}

void NativeProcess::protect_memory(uint64_t address, size_t size, int protection)
{
	state->trap_page.clear();
	protect_range(address, size, protection, false);
}

void NativeProcess::protect_image(uint64_t address, size_t size, int protection)
{
	protect_range(address, size, protection, true);
}

void NativeProcess::protect_range(uint64_t address, size_t size, int protection, bool image)
{
	state->require_memory();
	memory_range(address, size, protection, state->limit());
	const auto owned = std::find_if(state->allocations.begin(), state->allocations.end(), [&](const auto &allocation) {
		return address >= allocation.first && address - allocation.first < allocation.second &&
			size <= allocation.second - (address - allocation.first);
	});
	if (owned == state->allocations.end() && image) {
		// A program may also protect the pages of an image it was loaded from: they are accepted when mappings cover the whole range.
		uint64_t cursor = address;
		while (cursor < address + size) {
			const auto after = state->mappings.upper_bound(cursor);
			if (after == state->mappings.begin() || cursor - std::prev(after)->second.address >= std::prev(after)->second.size)
				throw std::runtime_error("native protection range is outside a dynamic allocation");
			cursor = std::prev(after)->second.address + std::prev(after)->second.size;
		}
	} else if (owned == state->allocations.end()) {
		throw std::runtime_error("native protection range is outside a dynamic allocation");
	}
	// Only the mappings the range touches are split; they are replaced once the guest's protection has changed.
	const uint64_t end = address + size;
	auto first_touched = state->mappings.upper_bound(address);
	if (first_touched != state->mappings.begin() && std::prev(first_touched)->second.address + std::prev(first_touched)->second.size > address)
		--first_touched;
	auto past_touched = first_touched;
	std::vector<ImageMapping> pieces;
	for (; past_touched != state->mappings.end() && past_touched->second.address < end; ++past_touched) {
		const auto &mapping = past_touched->second;
		const uint64_t last = mapping.address + mapping.size;
		if (mapping.address < address)
			pieces.push_back({mapping.address, address - mapping.address, 0, mapping.protection});
		const uint64_t first = std::max(mapping.address, address);
		pieces.push_back({first, std::min(last, end) - first, 0, protection});
		if (last > end)
			pieces.push_back({end, last - end, 0, mapping.protection});
	}
	memory_syscall(SYS_mprotect, address, size, static_cast<uint64_t>(protection));
	state->mappings.erase(first_touched, past_touched);
	++state->mapping_changes;
	for (const auto &piece : pieces)
		state->mappings.insert_or_assign(piece.address, piece);
}

void NativeProcess::unmap_memory(uint64_t address)
{
	state->trap_page.clear();
	state->require_memory();
	const auto owned = std::find_if(state->allocations.begin(), state->allocations.end(), [&](const auto &allocation) {
		return allocation.first == address;
	});
	if (owned == state->allocations.end())
		throw std::runtime_error("native release requires a dynamic allocation base");
	const uint64_t end = address + owned->second;
	memory_syscall(SYS_munmap, address, owned->second);
	std::erase_if(state->queued_blocks, [address](const auto &entry) { return entry.first == address; });
	if (const auto view = state->shared_views.find(address); view != state->shared_views.end())
		state->served_owners.erase(static_cast<uint64_t *>(view->second->host));
	state->shared_views.erase(address);
	state->mappings.erase(state->mappings.lower_bound(address), state->mappings.lower_bound(end));
	++state->mapping_changes;
	state->api_traps.erase(state->api_traps.lower_bound(address), state->api_traps.lower_bound(end));
	state->last_trap = state->api_traps.end();
	state->allocations.erase(owned);
}

bool NativeProcess::memory_available(uint64_t address, size_t size) const
{
	state->require_memory();
	memory_range(address,size,PROT_NONE,state->limit());
	const auto end = address+size;
	const auto stack = state->initial_stack_top-state->initial_stack_size-4096;
	if (address < state->initial_stack_top+4096 && stack < end)
		return false;
	// Mappings do not overlap, so only the one starting at or before the range and the first one after its start can meet it.
	auto after = state->mappings.upper_bound(address);
	if (after != state->mappings.end() && after->second.address < end)
		return false;
	if (after != state->mappings.begin() && address < std::prev(after)->second.address+std::prev(after)->second.size)
		return false;
	// The host maps a few things of its own into the guest near the top of the address space (the loader, vdso, its stack); the
	// runtime does not track those, so a range up there is checked against the guest's own map.
	if (end > 0x700000000000ULL) {
		std::ifstream maps("/proc/" + std::to_string(state->child->pid) + "/maps");
		std::string line;
		while (std::getline(maps, line)) {
			uint64_t first = 0, last = 0;
			if (std::sscanf(line.c_str(), "%" SCNx64 "-%" SCNx64, &first, &last) == 2 && address < last && first < end)
				return false;
		}
	}
	return true;
}

void NativeProcess::discard_memory(uint64_t address, size_t size)
{
	state->require_memory();
	memory_range(address,size,PROT_NONE,state->limit());
	const auto owned = std::find_if(state->allocations.begin(),state->allocations.end(),[&](const auto &allocation) {
		return address >= allocation.first && address-allocation.first < allocation.second &&
			size <= allocation.second-(address-allocation.first);
	});
	if (owned == state->allocations.end())
		throw std::runtime_error("native discard range is outside a dynamic allocation");
	memory_syscall(SYS_madvise,address,size,MADV_DONTNEED);
}

void NativeProcess::map_image(LoadedImage &image)
{
	state->require_memory();
	const auto base = image.base();
	const auto size = image.data().size();
	memory_range(base, size, PROT_NONE, state->limit());
	const auto mappings = image.mappings();
	state->image_allocations.reserve(state->image_allocations.size() + 1);
	const auto path = "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(image.descriptor());
	std::vector<uint8_t> name(path.begin(), path.end());
	name.push_back(0);
	uint64_t scratch = 0;
	uint64_t descriptor = uint64_t(-1);
	bool mapped = false;
	try {
		scratch = map_memory(4096, PROT_READ | PROT_WRITE);
		write_memory(scratch, name);
		// Reopen only the host-owned memfd; guest code never executes the Linux syscall.
		descriptor = memory_syscall(SYS_openat, uint64_t(AT_FDCWD), scratch, O_RDWR | O_CLOEXEC);
		const auto result = memory_syscall(SYS_mmap, base, size, PROT_NONE,
			MAP_SHARED | MAP_FIXED_NOREPLACE, descriptor);
		if (result != base) {
			state->child->stop();
			throw std::runtime_error("host returned an invalid native image address");
		}
		mapped = true;
		for (const auto &mapping : mappings)
			memory_syscall(SYS_mprotect, mapping.address, mapping.size, static_cast<uint64_t>(mapping.protection));
		memory_syscall(SYS_close, descriptor, 0);
		descriptor = uint64_t(-1);
		unmap_memory(scratch);
		scratch = 0;
		for (const auto &mapping : mappings)
			state->mappings.insert_or_assign(mapping.address, mapping);
			++state->mapping_changes;
		state->image_allocations.emplace_back(base, size);
	} catch (...) {
		try {
			if (mapped)
				memory_syscall(SYS_munmap, base, size);
			if (descriptor != uint64_t(-1))
				memory_syscall(SYS_close, descriptor, 0);
			if (scratch)
				unmap_memory(scratch);
		} catch (...) {
			state->child->stop();
		}
		throw;
	}
}

void NativeProcess::unmap_image(uint64_t address)
{
	state->require_memory();
	const auto owned = std::find_if(state->image_allocations.begin(), state->image_allocations.end(), [&](const auto &image) {
		return image.first == address;
	});
	if (owned == state->image_allocations.end())
		throw std::runtime_error("native image release requires a dynamically mapped image base");
	const uint64_t end = address + owned->second;
	memory_syscall(SYS_munmap, address, owned->second);
	state->mappings.erase(state->mappings.lower_bound(address), state->mappings.lower_bound(end));
	++state->mapping_changes;
	state->api_traps.erase(state->api_traps.lower_bound(address), state->api_traps.lower_bound(end));
	state->last_trap = state->api_traps.end();
	state->image_allocations.erase(owned);
}

std::pair<uint64_t, uint64_t> NativeProcess::stack_bounds() const
{
	state->require_stopped();
	return {state->initial_stack_top - state->initial_stack_size, state->initial_stack_top};
}

bool NativeProcess::active() const
{
	state->require_stopped();
	return state->active_call;
}

void NativeProcess::abandon_call()
{
	state->require_stopped();
	if (!state->active_call || !state->awaiting_api)
		throw std::runtime_error("native abandonment requires a pending API call");
	// Termination never resumes any suspended guest caller.
	state->callbacks.clear();
	state->callback_cleanups.clear();
	state->active_call=false;
	state->awaiting_api=false;
	state->awaiting_syscall=false;
}

void NativeProcess::end_call()
{
	if (!gone())
		settle_call();
	if (parallel() && !state->stopped) {
		auto &child = *state->child;
		child.pause_task();
		int discarded;
		while (child.take_stop(child.pid, discarded)) {}
		state->stopped = true;
	}
	// With the guest gone (the deadline ended it) there is only the bookkeeping of the call left to drop.
	if (state->child && state->child->pid != -1)
		state->require_stopped();
	state->callbacks.clear();
	state->callback_cleanups.clear();
	state->active_call=false;
	state->awaiting_api=false;
	state->awaiting_syscall=false;
	state->translated_words.reset();
}

uint64_t NativeProcess::return_gate() const
{
	return state->mode == NativeMode::x86 ? state->return_gate : state->child->gate;
}

bool NativeProcess::callback_boundary(uint64_t address) const
{
	return address == return_gate() || (state->request_gate && address == state->atomic_gate + served_stub::gate_offset(raven_gate_request_return));
}

bool NativeProcess::callback_in_stub(size_t index) const
{
	if (index >= state->callbacks.size())
		throw std::runtime_error("native callback origin beyond the nesting");
	return state->callbacks[index].registers.orig_rax == stub_callback;
}

// Leaves a request for the guest that waits for `call` in its stub.
static void leave_request(const WaitingCall &call, uint64_t stub, uint64_t function, std::span<const uint64_t> arguments)
{
	uint64_t *const slots = call.block + request_slot;
	slots[0] = stub;
	slots[1] = function;
	for (size_t index = 0; index < request_arguments; ++index)
		slots[2 + index] = index < arguments.size() ? arguments[index] : 0;
	call.block[11] = 3;
}

uint64_t NativeProcess::run_in_stub(uint64_t function, std::span<const uint64_t> arguments)
{
	// A routine of the runtime's own, which calls nothing: the guest runs it and waits again, and this thread waits for it meanwhile.
	const uint64_t stub = waiting.trap - state->served_stub_size;
	leave_request(waiting, stub, function, arguments);
	__atomic_store_n(&waiting.block[0], uint64_t(0), __ATOMIC_RELEASE);
	if (waiting.held) {
		if (ptrace(state->guest_resume(), waiting.task, nullptr, nullptr) == -1)
			throw std::runtime_error("cannot resume a native guest task for a request");
		forget_registers();
		waiting.held = false;
		state->stopped = false;
	}
	waiting.ran = true;
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	for (unsigned spins = 1; __atomic_load_n(&waiting.block[0], __ATOMIC_ACQUIRE) != stub + 1; ++spins) {
		__builtin_ia32_pause();
		if (spins % 256)
			continue;
		// A stub that had waited too long before the request came stands at its own breakpoint, and goes on from there.
		int status = 0;
		if (waitpid(waiting.task, &status, WNOHANG | __WALL) == waiting.task) {
			if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP || ptrace(state->guest_resume(), waiting.task, nullptr, nullptr) == -1)
				throw std::runtime_error("a native guest stopped while it carried out a request of the runtime");
		} else if (spins % 4096 == 0 && std::chrono::steady_clock::now() >= limit) {
			throw std::runtime_error("a native guest did not carry out a request of the runtime");
		}
	}
	return waiting.block[request_slot + 1];
}

uint64_t NativeProcess::thread_base() const
{
	state->require_stopped();
	if (waiting.task) {
		// The block a call waits in belongs to one thread, whose block of the environment was recorded with it.
		static thread_local const uint64_t *block = nullptr;
		static thread_local uint64_t base = 0;
		if (block != waiting.block) {
			const auto found = state->served_owners.find(waiting.block);
			if (found == state->served_owners.end())
				throw std::runtime_error("a waiting call has no thread-block owner");
			block = waiting.block;
			base = found->second;
		}
		return base;
	}
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	return registers.gs_base;
}

uint64_t NativeProcess::api_stack_pointer() const
{
	state->require_stopped();
	if (waiting.task)
		return waiting.stack;
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	return registers.rsp;
}

uint64_t NativeProcess::result_register() const
{
	state->require_stopped();
	if (waiting.task && waiting.answered)
		return waiting.block[10];
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	return registers.rax;
}

user_regs_struct NativeProcess::general_registers() const
{
	state->require_stopped();
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	return registers;
}

size_t NativeProcess::callback_depth() const
{
	return state->callbacks.size();
}

NativeContext NativeProcess::callback_origin(size_t index) const
{
	state->require_stopped();
	if (index >= state->callbacks.size())
		throw std::runtime_error("native callback origin beyond the nesting");
	NativeContext origin = state->callbacks[index];
	origin.xstate = context().xstate;
	return origin;
}

uint64_t NativeProcess::callback_stack(size_t index) const
{
	if (index >= state->callbacks.size())
		throw std::runtime_error("native callback origin beyond the nesting");
	return state->callbacks[index].registers.rsp;
}

void NativeProcess::trace_instructions(size_t count, std::function<void(uint64_t)> sink)
{
	state->require_stopped();
	state->steps_left = count;
	state->step_sink = std::move(sink);
}

void NativeProcess::watch_write(uint64_t address)
{
	state->require_stopped();
	const pid_t pid = state->child->pid;
	const auto poke = [&](size_t index, uint64_t value) {
		const long offset = long(offsetof(struct user, u_debugreg)) + long(index * sizeof(unsigned long));
		if (ptrace(PTRACE_POKEUSER, pid, reinterpret_cast<void *>(offset), reinterpret_cast<void *>(value)) == -1)
			throw std::runtime_error("cannot program the debug registers");
	};
	poke(7, 0);
	if (!address)
		return;
	poke(0, address);
	// Local enable of slot 0, write trigger (01) and eight-byte length (10 on x86-64, encoded as 0b10).
	poke(7, 1 | (1ULL << 16) | (2ULL << 18));
}

NativeThreadState NativeProcess::save_thread(bool snapshot_registers) const
{
	if (!parallel() || state->stopped)
		state->require_stopped();
	const bool in_task = parallel() && !snapshot_registers;
	NativeContext saved{};
	if (state->stopped) {
		if (in_task)
			trace(PTRACE_GETREGS, state->child->pid, &saved.registers);
		else
			saved = context();
	}
	if (!state->stopped && state->mode == NativeMode::x86) {
		saved.registers.fs = state->fs_selector;
		saved.registers.fs_base = state->fs_base;
	}
	return {std::move(saved), state->callbacks, state->callback_cleanups, state->active_call, state->awaiting_api, state->awaiting_syscall,
		state->debug_print_fault, state->pending_cleanup, state->stack_top, state->translated_words, state->translated_depth,
		state->translated_floats, state->translated_result, parallel() ? state->child->pid : 0, state->stopped,
		state->thread_served_guest, state->thread_served_block, state->serving, state->served_stack, in_task, state->serving_queued};
}

void NativeProcess::load_thread(const NativeThreadState &thread)
{
	if (parallel()) {
		if (thread.task <= 0)
			throw std::runtime_error("native thread has no execution task");
		state->child->pid = thread.task;
		state->stopped = thread.stopped;
	} else {
		state->require_stopped();
	}
	if (state->stopped && !thread.context_in_task)
		set_context(thread.context);
	state->callbacks = thread.callbacks;
	state->callback_cleanups = thread.callback_cleanups;
	state->active_call = thread.active_call;
	state->awaiting_api = thread.awaiting_api;
	state->awaiting_syscall = thread.awaiting_syscall;
	state->debug_print_fault = thread.debug_print_fault;
	state->pending_cleanup = thread.pending_cleanup;
	state->stack_top = thread.stack_top;
	state->translated_words = thread.translated_words;
	state->translated_depth = thread.translated_depth;
	state->translated_floats = thread.translated_floats;
	state->translated_result = thread.translated_result;
	state->thread_served_guest = thread.served_guest;
	state->thread_served_block = thread.served_block;
	state->serving = thread.serving;
	state->serving_queued = thread.serving_queued;
	state->served_stack = thread.served_stack;
	state->slice_started = std::chrono::steady_clock::now();
	if (state->mode == NativeMode::x86) {
		state->fs_selector = uint32_t(thread.context.registers.fs);
		state->fs_base = thread.context.registers.fs_base;
	}
}

NativeThreadState NativeProcess::new_thread(uint64_t stack_top, uint64_t teb)
{
	settle_call();
	struct SelectedTask {
		State &state;
		pid_t previous;
		bool stopped;
		~SelectedTask()
		{
			if (state.child->pid != -1)
				state.child->pid = previous;
			state.stopped = stopped;
			forget_registers();
		}
	} selected{*state, state->child->pid, state->stopped};
	// Registered waits can create a callback while every guest runs. Clone from the stopped administrative task then.
	if (parallel() && !state->stopped) {
		state->child->pid = state->memory_task;
		state->stopped = true;
	}
	state->require_stopped();
	NativeThreadState thread{};
	thread.context = context();
	if (state->mode == NativeMode::x86) {
		// Every thread owns an LDT entry, so a switch only reloads registers.
		const uint32_t entry = state->ldt_entries++;
		install_segment(entry, teb);
		thread.context.registers.fs = entry * 8 + 7;
		thread.context.registers.fs_base = teb;
	} else {
		thread.context.registers.gs_base = teb;
	}
	thread.stack_top = stack_top;
	if (parallel() && state->served_handler)
		make_served_block(teb, thread.served_guest, thread.served_block);
	if (parallel())
		thread.task = state->child->clone_task(stack_top);
	// The task goes to the host thread that will serve it, which takes over its tracing.
	if (parallel() && threaded_host())
		hand_task(thread.task);
	return thread;
}


bool NativeProcess::parallel() const
{
	return state->child->execution_descriptor() >= 0;
}

bool NativeProcess::stopped() const
{
	return state->stopped;
}

bool NativeProcess::gone() const
{
	return !state->child || state->child->pid == -1;
}

int NativeProcess::execution_descriptor() const
{
	return state->child->execution_descriptor();
}

void NativeProcess::drain_execution()
{
	state->child->drain_execution();
}

void NativeProcess::set_execution_wait(std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> wait)
{
	state->execution_wait = std::move(wait);
}

void NativeProcess::set_execution_turn(std::function<void(std::function<bool()>)> turn)
{
	state->execution_turn = std::move(turn);
}

void NativeProcess::set_concurrent_served(std::function<std::optional<uint64_t>(uint64_t, const uint64_t *)> alone)
{
	state->concurrent_served = std::move(alone);
}

bool NativeProcess::serve_alone(uint64_t *block)
{
	if (!block || !state->concurrent_served)
		return false;
	const uint64_t stub = __atomic_load_n(&block[0], __ATOMIC_ACQUIRE);
	// A completion (the low bit) goes the usual way. The guest spins in its stub, served or called, and takes the answer as from any.
	if (!stub || (stub & 1))
		return false;
	const auto result = state->concurrent_served(stub, block + 1);
	if (!result)
		return false;
	block[10] = *result;
	block[11] = 0;
	__atomic_store_n(&block[0], 0, __ATOMIC_RELEASE);
	return true;
}

bool NativeProcess::serve_stopped_alone(pid_t task, int status, uint64_t *block)
{
	// A guest whose host thread was asleep spins in its stub until it stops at the breakpoint of a long wait, and goes back to looking at
	// its block when let go on: a call that needs nothing of the runtime is answered there as well, without the turn.
	if (!block || status < 0 || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
		return false;
	const uint64_t stub = __atomic_load_n(&block[0], __ATOMIC_ACQUIRE);
	user_regs_struct registers{};
	if (!stub || (stub & 1) || ptrace(PTRACE_GETREGS, task, nullptr, &registers) == -1 || registers.rip != stub + served_stub::wait() + 1 ||
		!serve_alone(block))
		return false;
	if (ptrace(state->guest_resume(), task, nullptr, nullptr) == -1)
		throw std::runtime_error("cannot resume a native guest task after a call answered beside the turn");
	return true;
}

void NativeProcess::set_execution_park(std::function<int(pid_t, std::chrono::steady_clock::time_point, const uint64_t *)> park)
{
	state->execution_park = std::move(park);
}

int NativeProcess::wait_task(pid_t task, std::chrono::steady_clock::time_point deadline, const std::function<bool()> &interrupted,
	const uint64_t *block)
{
	// The calling host thread traces the task. A stop usually comes within microseconds, so the task is polled a little before the thread
	// sleeps; the sleep ends at the stop, at the deadline, or when another thread interrupts it to have something done to the task.
	// A call the guest leaves in its block comes sooner still and stops nothing: the block is looked at between the polls.
	// Only a few threads poll at a time, half as many as there are processors: a program with hundreds of threads would otherwise
	// keep every processor polling, and the guests they wait for would not get to run.
	static std::atomic<unsigned> polling{0};
	static const unsigned pollers = std::max(1u, std::thread::hardware_concurrency() / 2);
	struct Poller {
		const bool polls = polling.fetch_add(1, std::memory_order_relaxed) < pollers;
		~Poller() { polling.fetch_sub(1, std::memory_order_relaxed); }
	} poller;
	// A thread whose guest went on by itself the last times polls less before it sleeps, down to a few looks, and one whose guest
	// called or stopped while it polled goes back to polling for long: the polls of a guest that computes are processor time taken
	// from the guests that run.
	static thread_local unsigned patience = 200;
	const auto came = [&](int result) {
		patience = std::min(200u, patience * 2);
		return result;
	};
	for (unsigned polls = poller.polls ? 200 - patience : 199;; ++polls) {
		if (block && polls < 200)
			for (unsigned looks = 0; looks < 64; ++looks) {
				if (__atomic_load_n(&block[0], __ATOMIC_ACQUIRE))
					return came(-4);
				__builtin_ia32_pause();
			}
		int status = 0;
		const pid_t result = waitpid(task, &status, polls < 200 ? WNOHANG | __WALL : __WALL);
		if (result == task)
			return polls < 200 ? came(status) : status;
		if (result == -1 && errno != EINTR)
			throw std::runtime_error("cannot wait for a native guest task");
		if (block && __atomic_load_n(&block[0], __ATOMIC_ACQUIRE))
			return came(-4);
		if (interrupted && interrupted())
			return -3;
		if (std::chrono::steady_clock::now() >= deadline)
			return -1;
		if (polls == 199) {
			// The next wait sleeps: an alarm ends it at the deadline.
			if (poller.polls)
				patience = std::max(4u, patience / 2);
			const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
			itimerspec alarm{};
			alarm.it_value.tv_sec = left / 1000000000;
			alarm.it_value.tv_nsec = std::max<long long>(left % 1000000000, 1);
			timer_settime(wait_timer(), 0, &alarm, nullptr);
		}
	}
}

void NativeProcess::interrupt_host(pid_t host_thread)
{
	if (host_thread > 0)
		::syscall(SYS_tgkill, ::getpid(), host_thread, SIGRTMIN + 5);
}

void NativeProcess::hand_task(pid_t task)
{
	// The task waits in pause() at the runtime's gate, without a tracer, until the host thread that will serve it takes it. A task handed
	// over stopped by a signal would be left in a group stop, which a tracer that seized it cannot resume reliably.
	const uint64_t gate = state->child->gate;
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, task, &registers);
	registers.rip = gate + 1;
	registers.cs = 0x33;
	registers.ss = 0x2b;
	registers.rax = SYS_pause;
	registers.orig_rax = uint64_t(-1);
	trace(PTRACE_SETREGS, task, &registers);
	trace(PTRACE_SYSCALL, task);
	for (;;) {
		int status = 0;
		const pid_t result = waitpid(task, &status, __WALL);
		if (result == -1 && errno == EINTR)
			continue;
		if (result != task || !WIFSTOPPED(status) || WSTOPSIG(status) != (SIGTRAP | 0x80))
			throw std::runtime_error("a native guest task did not enter its handover wait");
		break;
	}
	if (ptrace(PTRACE_DETACH, task, nullptr, nullptr) == -1)
		throw std::runtime_error("cannot hand a native guest task to its host thread: " + std::string(std::strerror(errno)));
	forget_registers();
}

void NativeProcess::adopt_task(pid_t task)
{
	if (ptrace(PTRACE_SEIZE, task, nullptr,
			reinterpret_cast<void *>(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACESECCOMP)) == -1)
		throw std::runtime_error("cannot take a native guest task: " + std::string(std::strerror(errno)));
	if (ptrace(PTRACE_INTERRUPT, task, nullptr, nullptr) == -1)
		throw std::runtime_error("cannot stop a native guest task");
	for (;;) {
		int status = 0;
		const pid_t result = waitpid(task, &status, __WALL);
		if (result == -1 && errno == EINTR)
			continue;
		if (result != task || !WIFSTOPPED(status))
			throw std::runtime_error("a native guest task did not stop for its host thread");
		if ((status >> 16) == PTRACE_EVENT_STOP)
			break;
	}
	// The interrupted pause() is not taken up again: the task stands after the gate's system call, as after any other.
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, task, &registers);
	registers.orig_rax = uint64_t(-1);
	registers.rax = 0;
	trace(PTRACE_SETREGS, task, &registers);
	forget_registers();
}

static int wait_own_task(pid_t task)
{
	for (;;) {
		int status = 0;
		const pid_t result = waitpid(task, &status, __WALL);
		if (result == task)
			return status;
		if (result == -1 && errno != EINTR)
			throw std::runtime_error("cannot wait for a native guest task");
	}
}

int NativeProcess::pause_own_task(pid_t task)
{
	if (kill(task, SIGSTOP) != 0)
		throw std::runtime_error("cannot interrupt native task");
	const int status = wait_own_task(task);
	if (!WIFSTOPPED(status))
		throw std::runtime_error("native task ended while being suspended");
	if (WSTOPSIG(status) != SIGSTOP) {
		// Its own trap won the race. Drain the pending signal without losing the trap or its fault information.
		siginfo_t info{};
		trace(PTRACE_GETSIGINFO, task, &info);
		trace(PTRACE_SYSEMU, task);
		const int interrupted = wait_own_task(task);
		if (!WIFSTOPPED(interrupted) || WSTOPSIG(interrupted) != SIGSTOP)
			throw std::runtime_error("native task did not receive its suspend interrupt");
		trace(PTRACE_SETSIGINFO, task, &info);
	}
	forget_registers();
	return status;
}

// Whoever works on a task from outside its call, as a pause does, sees it where it is and does not settle the call it waits for.
struct LeaveWaiting {
	pid_t task = std::exchange(waiting.task, 0);
	~LeaveWaiting() { waiting.task = task; }
};

void NativeProcess::hold_call(pid_t task)
{
	if (waiting.task) {
		if (!waiting.held) {
			const LeaveWaiting leave;
			pause_own_task(leave.task);
			waiting.held = true;
		}
		return;
	}
	// The host thread may have seen the call in the block and not taken it up yet: the task then runs in its stub all the same.
	user_regs_struct registers{};
	if (held_in_stub || ptrace(PTRACE_GETREGS, task, nullptr, &registers) == 0)
		return;
	pause_own_task(task);
	held_in_stub = true;
}

void NativeProcess::settle_call()
{
	if (!waiting.task)
		return;
	const WaitingCall call = waiting;
	waiting.task = 0;
	if (!call.held)
		pause_own_task(call.task);
	// The task stands at the breakpoint its stub falls back to, with the registers the caller left: the call goes on as one that
	// stopped there. One answered already returns from there with its answer.
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, call.task, &registers);
	registers.rip = call.trap + 1;
	registers.orig_rax = uint64_t(-1);
	if (call.answered)
		registers.rax = call.block[10];
	if (call.ran) {
		registers.rcx = call.words[0];
		registers.rdx = call.words[1];
		registers.r8 = call.words[2];
		registers.r9 = call.words[3];
		user_fpregs_struct vectors{};
		trace(PTRACE_GETFPREGS, call.task, &vectors);
		for (size_t index = 0; index < 4; ++index)
			std::memcpy(reinterpret_cast<uint8_t *>(vectors.xmm_space) + index * 16, &call.floats[index], 8);
		trace(PTRACE_SETFPREGS, call.task, &vectors);
	}
	trace(PTRACE_SETREGS, call.task, &registers);
	CallCycles::instance().answered(call.block, call.trap - state->served_stub_size, call.seen);
	__atomic_store_n(&call.block[0], uint64_t(0), __ATOMIC_RELEASE);
	state->stopped = true;
	++state->call_settles;
}

bool NativeProcess::call_waiting() const
{
	return waiting.task != 0;
}

NativeContext NativeProcess::task_context(pid_t task)
{
	const LeaveWaiting leave;
	NativeContext result{};
	trace(PTRACE_GETREGS, task, &result.registers);
	result.xstate.resize(65536);
	iovec buffer{result.xstate.data(), result.xstate.size()};
	if (ptrace(PTRACE_GETREGSET, task, reinterpret_cast<void *>(NT_X86_XSTATE), &buffer) == -1)
		throw std::runtime_error("host cannot expose native XSTATE");
	result.xstate.resize(buffer.iov_len);
	return result;
}

void NativeProcess::set_task_registers(pid_t task, const user_regs_struct &registers)
{
	const LeaveWaiting leave;
	trace(PTRACE_SETREGS, task, const_cast<user_regs_struct *>(&registers));
}

void NativeProcess::set_task_xstate(pid_t task, std::span<const uint8_t> xstate)
{
	const LeaveWaiting leave;
	iovec buffer{const_cast<uint8_t *>(xstate.data()), xstate.size()};
	if (ptrace(PTRACE_SETREGSET, task, reinterpret_cast<void *>(NT_X86_XSTATE), &buffer) == -1)
		throw std::runtime_error("host cannot set native XSTATE");
}

void NativeProcess::arm_host_thread()
{
	wait_timer();
}

pid_t NativeProcess::administrative_task() const
{
	return state->memory_task;
}

void NativeProcess::stop_thread(const NativeThreadState &thread)
{
	if (parallel() && thread.task > 0 && thread.task != state->child->pid) {
		state->child->stop_task(thread.task);
		drain_queued_calls(true);
	}
}

void NativeProcess::pause_thread(NativeThreadState &thread)
{
	if (!parallel() || thread.stopped)
		return;
	if (thread.task <= 0)
		throw std::runtime_error("native thread has no execution task");
	auto &child = *state->child;
	const pid_t caller = child.pid;
	const bool stopped = state->stopped;
	try {
		child.pid = thread.task;
		child.pause_task();
		state->stopped = true;
		drain_queued_calls(true);
		forget_registers();
		thread.context = context();
	} catch (...) {
		child.pid = caller;
		state->stopped = stopped;
		forget_registers();
		throw;
	}
	child.pid = caller;
	state->stopped = stopped;
	forget_registers();
}

void NativeProcess::set_thread_registers(NativeThreadState &thread, const user_regs_struct &registers)
{
	thread.context.registers = registers;
	if (!parallel() || (thread.stopped && !thread.context_in_task))
		return;
	if (thread.task <= 0)
		throw std::runtime_error("native thread has no execution task");
	auto &child = *state->child;
	const pid_t caller = child.pid;
	const bool stopped = state->stopped;
	try {
		child.pid = thread.task;
		state->stopped = true;
		forget_registers();
		trace(PTRACE_SETREGS, child.pid, &thread.context.registers);
	} catch (...) {
		child.pid = caller;
		state->stopped = stopped;
		forget_registers();
		throw;
	}
	child.pid = caller;
	state->stopped = stopped;
	forget_registers();
}

void NativeProcess::enter_fault_dispatch()
{
	state->require_stopped();
	if (!state->active_call || state->awaiting_api || state->awaiting_syscall || state->mode != NativeMode::x64)
		throw std::runtime_error("native fault dispatch requires a stopped x64 call");
	state->awaiting_api = true;
	state->pending_cleanup = 0;
}

void NativeProcess::redirect(const NativeContext &target, size_t depth)
{
	state->require_stopped();
	if (!state->active_call || !state->awaiting_api || depth > state->callbacks.size())
		throw std::runtime_error("native redirection requires a pending API call");
	set_context(target);
	state->callbacks.resize(depth);
	state->callback_cleanups.resize(depth);
	state->awaiting_api = false;
	state->awaiting_syscall = false;
}

NativeMode NativeProcess::mode() const
{
	return state->mode;
}

void NativeProcess::set_teb(uint64_t base)
{
	state->require_stopped();
	install_segment(0, base);
	// LDT index 0, table indicator 1, requested privilege level 3.
	state->fs_selector = 7;
	state->fs_base = base;
}

void NativeProcess::install_segment(uint32_t entry, uint64_t base)
{
	if (state->mode != NativeMode::x86 || base > 0xfffff000ULL || base % 4096)
		throw std::runtime_error("native TEB descriptor requires an x86 process and a page-aligned 32-bit address");
	// struct user_desc: entry, base, limit, flags (32-bit segment, data, byte granularity, usable).
	std::vector<uint8_t> descriptor(16);
	const uint32_t fields[] = {entry, uint32_t(base), 0xfff, 0x41};
	for (size_t index = 0; index < 4; ++index)
		for (size_t byte = 0; byte < 4; ++byte)
			descriptor[index * 4 + byte] = uint8_t(fields[index] >> (byte * 8));
	const auto scratch = map_memory(4096, PROT_READ | PROT_WRITE);
	try {
		write_memory(scratch, descriptor);
		const auto result = memory_syscall(SYS_modify_ldt, 1, scratch, descriptor.size());
		if (result != 0)
			throw std::runtime_error("host rejected the native TEB descriptor");
	} catch (...) {
		if (state->child->pid != -1)
			unmap_memory(scratch);
		throw;
	}
	unmap_memory(scratch);
}

void NativeProcess::add_api_trap(uint64_t address, uint32_t callee_cleanup)
{
	state->require_stopped();
	// Traps are registered by the thousand, mostly next to each other: the page around one is read once for those that follow.
	const auto byte_at = [&](uint64_t at) {
		const uint64_t page = at & ~uint64_t(4095);
		if (page != state->trap_page_address || state->trap_page.empty()) {
			state->trap_page = read_memory(page, state->covers(page, 4096, PROT_EXEC) ? 4096 : size_t(at - page + 1));
			state->trap_page_address = page;
		}
		return at - page < state->trap_page.size() ? state->trap_page[at - page] : read_memory(at, 1)[0];
	};
	if (!state->covers(address, 1, PROT_EXEC) || byte_at(address) != 0xcc)
	{
		char text[96];
		std::snprintf(text, sizeof text, "native API trap must be an executable INT3: %llx", static_cast<unsigned long long>(address));
		throw std::runtime_error(text);
	}
	if (state->mode == NativeMode::x86 && (address >= state->limit() || callee_cleanup % 4))
		throw std::runtime_error("native x86 API trap requires a 32-bit address and dword-aligned cleanup");
	const bool returns = state->mode != NativeMode::x86 && state->covers(address + 1, 1, PROT_EXEC) && byte_at(address + 1) == 0xc3;
	auto hint = state->last_trap == state->api_traps.end() ? state->api_traps.end() : std::next(state->last_trap);
	if (hint != state->api_traps.end() && hint->first == address)
		hint->second = {callee_cleanup, returns};
	else
		hint = state->api_traps.insert_or_assign(hint, address, ApiTrap{callee_cleanup, returns});
	state->last_trap = hint;
}

void NativeProcess::add_api_traps(std::map<uint64_t, ApiTrap> traps)
{
	state->require_stopped();
	if (state->api_traps.empty())
		state->api_traps = std::move(traps);
	else
		for (auto &[address, trap] : traps)
			state->api_traps.insert_or_assign(address, trap);
	state->last_trap = state->api_traps.end();
}

NativeResult NativeProcess::call(uint64_t address, std::span<const uint64_t> arguments,
	std::chrono::milliseconds timeout)
{
	state->require_stopped();
	if (state->active_call)
		throw std::runtime_error("native call is already active");
	return begin_call(address, arguments, state->stack_top, timeout);
}

NativeResult NativeProcess::callback(uint64_t address, std::span<const uint64_t> arguments,
	std::chrono::milliseconds timeout)
{
	state->require_stopped();
	if (!state->active_call || !state->awaiting_api) {
		if (std::getenv("RAVEN_BACKTRACE")) {
			void *frames[40];
			const int count = backtrace(frames, 40);
			backtrace_symbols_fd(frames, count, 2);
		}
		throw std::runtime_error(std::string("native callback requires a pending API call (last API ") + api_in_progress + ")");
	}
	if (state->callbacks.size() >= 64)
		throw std::runtime_error(std::string("native callback exceeds initial nesting limit (last API ") + api_in_progress + ")");
	if (waiting.task && state->request_gate && !waiting.answered && arguments.size() <= request_arguments) {
		// The guest waits in the stub of the call that makes the callback: it runs the procedure from there and waits again. What is
		// kept of the call is where its frames go on, for an exception that searches or unwinds past the callback; the nonvolatile
		// registers are those the procedure's frames give back.
		if (!state->covers(address, 1, PROT_EXEC))
			throw std::runtime_error("native entry is outside executable image memory");
		NativeContext origin{};
		origin.registers.rip = waiting.trap + 1;
		origin.registers.rsp = waiting.stack;
		origin.registers.rcx = waiting.words[0];
		origin.registers.rdx = waiting.words[1];
		origin.registers.r8 = waiting.words[2];
		origin.registers.r9 = waiting.words[3];
		origin.registers.gs_base = thread_base();
		origin.registers.cs = 0x33;
		origin.registers.ss = 0x2b;
		origin.registers.eflags = 0x202;
		origin.registers.orig_rax = stub_callback;
		state->callbacks.push_back(origin);
		state->callback_cleanups.push_back(state->pending_cleanup);
		leave_request(waiting, waiting.trap - state->served_stub_size, address, arguments);
		while (!outer_calls.empty() && outer_calls.back().depth >= state->callbacks.size())
			outer_calls.pop_back();
		outer_calls.push_back({waiting, state->callbacks.size()});
		waiting.task = 0;
		requesting = true;
		state->child->set_timeout(timeout);
		state->awaiting_api = false;
		state->awaiting_syscall = false;
		state->debug_print_fault = false;
		return resume(timeout);
	}
	// The procedure called keeps the nonvolatile vector and control registers, as the calling convention requires, and the volatile
	// ones are the API call's to change: the general registers are all that has to come back.
	NativeContext saved{};
	trace(PTRACE_GETREGS, state->child->pid, &saved.registers);
	state->callbacks.push_back(saved);
	// A nested API trap overwrites the pending stdcall cleanup of the call that was interrupted.
	state->callback_cleanups.push_back(state->pending_cleanup);
	try {
		return begin_call(address, arguments, saved.registers.rsp & ~uint64_t(15), timeout);
	} catch (...) {
		if (state->stopped && state->child->pid != -1) {
			try {
				set_context(saved);
			} catch (...) {
				state->child->stop();
				throw;
			}
			state->callbacks.pop_back();
			state->pending_cleanup = state->callback_cleanups.back();
			state->callback_cleanups.pop_back();
			state->awaiting_api = true;
		} else {
			state->child->stop();
		}
		throw;
	}
}

NativeResult NativeProcess::begin_call(uint64_t address, std::span<const uint64_t> arguments,
	uint64_t stack_ceiling, std::chrono::milliseconds timeout)
{
	if (arguments.size() > 128)
		throw std::runtime_error("native call exceeds initial argument limit");
	if (!state->covers(address, 1, PROT_EXEC))
		throw std::runtime_error("native entry is outside executable image memory");
	state->child->set_timeout(timeout);
	if (state->mode == NativeMode::x86)
		return begin_x86_call(address, arguments, stack_ceiling, timeout);
	const size_t extra = arguments.size() > 4 ? (arguments.size() - 4) * 8 : 0;
	const size_t frame_size = ((32 + extra + 15) & ~size_t(15)) + 8;
	if (stack_ceiling < frame_size)
		throw std::runtime_error("native call stack address underflows");
	const uint64_t rsp = stack_ceiling - frame_size;
	std::vector<uint8_t> frame(frame_size);
	const auto put = [&](size_t offset, uint64_t value) {
		for (size_t byte = 0; byte < 8; ++byte)
			frame[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
	};
	put(0, state->child->gate);
	for (size_t index = 4; index < arguments.size(); ++index)
		put(40 + (index - 4) * 8, arguments[index]);
	write_memory(rsp, frame);
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	registers.rax = 0;
	registers.rcx = arguments.size() > 0 ? arguments[0] : 0;
	registers.rdx = arguments.size() > 1 ? arguments[1] : 0;
	registers.r8 = arguments.size() > 2 ? arguments[2] : 0;
	registers.r9 = arguments.size() > 3 ? arguments[3] : 0;
	registers.orig_rax = uint64_t(-1);
	registers.eflags = 0x202;
	registers.rip = address;
	registers.rsp = rsp;
	trace(PTRACE_SETREGS, state->child->pid, &registers);
	state->active_call = true;
	state->awaiting_api = false;
	state->awaiting_syscall = false;
	state->debug_print_fault = false;
	return resume(timeout);
}

NativeResult NativeProcess::begin_x86_call(uint64_t address, std::span<const uint64_t> arguments,
	uint64_t stack_ceiling, std::chrono::milliseconds timeout)
{
	// A negative 32-bit value the handlers widened, such as a pseudo handle or a message's -1, goes back as its low half.
	const auto fits = [](uint64_t value) { return value <= UINT32_MAX || (int64_t(value) < 0 && int64_t(value) >= INT32_MIN); };
	if (!std::all_of(arguments.begin(), arguments.end(), fits))
		throw std::runtime_error("native x86 call argument exceeds 32 bits");
	const uint64_t size = arguments.size() * 4;
	if (stack_ceiling > state->limit() || stack_ceiling < size + 16)
		throw std::runtime_error("native call stack address is invalid");
	// Win32 entry points see ESP+4 on a 16-byte boundary so compiler-aligned locals stay aligned.
	const uint64_t first_argument = (stack_ceiling - size) & ~uint64_t(15);
	const uint64_t esp = first_argument - 4;
	std::vector<uint8_t> frame(4 + size);
	const auto put = [&](size_t offset, uint64_t value) {
		for (size_t byte = 0; byte < 4; ++byte)
			frame[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
	};
	put(0, state->return_gate);
	for (size_t index = 0; index < arguments.size(); ++index)
		put(4 + index * 4, arguments[index]);
	write_memory(esp, frame);
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	registers.rax = registers.rcx = registers.rdx = 0;
	registers.orig_rax = uint64_t(-1);
	registers.eflags = 0x202;
	registers.rip = address;
	registers.rsp = esp;
	registers.cs = 0x23;
	registers.ss = registers.ds = registers.es = 0x2b;
	// The host reloads FS bases from thread state on every switch-in, so the selector and the descriptor base must agree.
	registers.fs = state->fs_selector;
	registers.fs_base = state->fs_base;
	registers.gs = 0;
	registers.gs_base = 0;
	trace(PTRACE_SETREGS, state->child->pid, &registers);
	state->active_call = true;
	state->awaiting_api = false;
	state->awaiting_syscall = false;
	state->debug_print_fault = false;
	return resume(timeout);
}

NativeResult NativeProcess::resume(std::chrono::milliseconds timeout)
{
	state->require_stopped();
	if (!state->active_call || state->awaiting_api) {
		if (std::getenv("RAVEN_BACKTRACE")) {
			void *frames[40];
			const int count = backtrace(frames, 40);
			backtrace_symbols_fd(frames, count, 2);
		}
		throw std::runtime_error(std::string("native process has no resumable call or awaits an API response (last API ") + api_in_progress + ")");
	}
	auto &child = *state->child;
	drain_queued_calls(true);
	child.set_timeout(timeout);
	const auto execution_deadline = std::chrono::steady_clock::now() + timeout;
	int status;
	user_regs_struct registers{};
	int signal;
	std::optional<siginfo_t> event_info;
	// A call answered in the guest's block is given up there: the guest, which did not stop for it, takes the answer and goes on.
	bool runs = false;
	if (waiting.task) {
		if (state->resume_hook)
			state->resume_hook();
		const WaitingCall call = waiting;
		waiting.task = 0;
		call.block[11] = 0;
		CallCycles::instance().answered(call.block, call.trap - state->served_stub_size, call.seen);
		__atomic_store_n(&call.block[0], uint64_t(0), __ATOMIC_RELEASE);
		if (call.held && ptrace(state->guest_resume(), call.task, nullptr, nullptr) == -1)
			throw std::runtime_error("cannot resume a native guest task after its call");
		forget_registers();
		state->stopped = false;
		++state->call_answers;
		runs = true;
	} else if (requesting) {
		// The guest goes from its stub to the procedure asked of it.
		requesting = false;
		if (state->resume_hook)
			state->resume_hook();
		WaitingCall &call = outer_calls.back().call;
		__atomic_store_n(&call.block[0], uint64_t(0), __ATOMIC_RELEASE);
		if (call.held && ptrace(state->guest_resume(), call.task, nullptr, nullptr) == -1)
			throw std::runtime_error("cannot resume a native guest task for its callback");
		call.held = false;
		forget_registers();
		state->stopped = false;
		runs = true;
	}
	// The procedure a guest ran inside a stub returned: the call that asked for it is the one in progress again.
	const auto returned = [&](uint64_t *block, uint64_t marker, bool held) {
		while (!outer_calls.empty() && outer_calls.back().depth > state->callbacks.size())
			outer_calls.pop_back();
		if (outer_calls.empty() || outer_calls.back().depth != state->callbacks.size() || outer_calls.back().call.block != block ||
			outer_calls.back().call.trap != marker - 1 + state->served_stub_size)
			throw std::runtime_error("a native callback returned to no call of the runtime");
		waiting = outer_calls.back().call;
		outer_calls.pop_back();
		waiting.held = held;
		waiting.ran = true;
		state->callbacks.pop_back();
		state->pending_cleanup = state->callback_cleanups.back();
		state->callback_cleanups.pop_back();
		state->awaiting_api = true;
		state->awaiting_syscall = false;
		return NativeResult{NativeStop::returned, block[request_slot + 1], state->child->gate + 1, SIGTRAP};
	};
	for (;;) {
		// SYSEMU blocks host syscall dispatch; invalid syscall gates can fault first. Diagnostics may single-step instead.
		const bool slicing = !parallel() && state->slice.count() > 0 && state->slice_yield && state->slice_wanted && state->slice_wanted();
		// A guest that keeps calling the runtime never has a quiet slice, so the time it has had the processor is checked at every resumption too.
		// A thread inside a served stub keeps the processor until it leaves it: the block it uses is shared by every thread.
		std::chrono::steady_clock::duration period = state->slice;
		if (slicing && std::chrono::steady_clock::now() - state->slice_started >= state->slice) {
			if (inside_served_stub()) {
				++state->slice_deferrals;
				period = std::chrono::microseconds(100);
			} else {
				++state->slice_yield_requests;
				state->slice_yield();
				state->slice_started = std::chrono::steady_clock::now();
			}
		}
		// Sampling interrupts the guest on its own, shorter period; a thread change still waits for the whole slice.
		const bool sampling = state->sample_period.count() > 0 && state->sampler;
		if (slicing || sampling)
			child.set_slice(slicing && sampling ? std::min<std::chrono::steady_clock::duration>(period, state->sample_period) : slicing ? period :
				state->sample_period);
		else
			child.clear_slice();
		event_info.reset();
		// The runtime's own short waits, and other threads run while this one yielded, leave the child a deadline of theirs: this call's
		// comes back before each wait, or a thread that computes long after another ended would be stopped as if its time had run out.
		child.set_deadline(execution_deadline);
		if (runs) {
			runs = false;
		} else {
			if (state->resume_hook)
				state->resume_hook();
			trace(state->steps_left ? PTRACE_SINGLESTEP : state->seccomp ? PTRACE_CONT : PTRACE_SYSEMU, child.pid);
		}
		state->stopped = false;
		bool sampled_stop = false;
		if (parallel() && state->execution_park) {
			// The host thread lets the others serve their calls while this guest runs, and waits for the guest's stop by itself.
			const auto until = sampling ? std::min(execution_deadline, std::chrono::steady_clock::now() + state->sample_period) : execution_deadline;
			uint64_t *const block = state->thread_served_block;
			status = state->execution_park(child.pid, until, block);
			forget_registers();
			if (status == -4) {
				// The guest left a call in its block and waits for the answer without having stopped.
				const uint64_t stub = __atomic_load_n(&block[0], __ATOMIC_ACQUIRE);
				if (stub & 1)
					return returned(block, stub, std::exchange(held_in_stub, false));
				if (!held_in_stub && serve_alone(block)) {
					runs = true;
					continue;
				}
				if (!std::binary_search(state->call_stubs.begin(), state->call_stubs.end(), stub)) {
					serve_pending();
					if (std::exchange(held_in_stub, false) && ptrace(state->guest_resume(), child.pid, nullptr, nullptr) == -1)
						throw std::runtime_error("cannot resume a native guest task after its served call");
					runs = true;
					continue;
				}
				const uint64_t seen = CallCycles::instance().seen(block, stub);
				drain_queued_calls(true);
				waiting = WaitingCall{this, child.pid, block, stub + state->served_stub_size, block[5], {block[1], block[2], block[3], block[4]},
					{block[6], block[7], block[8], block[9]}, false, std::exchange(held_in_stub, false), false, seen};
				state->awaiting_api = true;
				state->awaiting_syscall = false;
				state->debug_print_fault = false;
				state->pending_cleanup = 0;
				return {NativeStop::api, waiting.trap, waiting.trap + 1, SIGTRAP};
			}
			// The sampler looks at where the guest is at each period: it is stopped there, then goes on.
			if (status == -1 && sampling && std::chrono::steady_clock::now() < execution_deadline) {
				status = pause_own_task(child.pid);
				sampled_stop = true;
			}
		} else if (parallel() && state->execution_wait) {
			const pid_t task = child.pid;
			bool observed = false;
			uint64_t *const block = state->thread_served_block;
			const auto command = [block] { return block && __atomic_load_n(&block[0], __ATOMIC_ACQUIRE); };
			const auto ready = [&] { return command() || queued_pending() || observed || (observed = child.take_stop(task, status)); };
			const auto until = sampling ? std::min(execution_deadline, std::chrono::steady_clock::now() + state->sample_period) : execution_deadline;
			// Other threads that stopped for the runtime are served before this one is looked at again, as a thread that calls the runtime
			// every few microseconds would otherwise be found stopped again at each look and keep the runtime to itself while the calls of
			// the others waited for seconds.
			if (state->execution_turn && child.stops_queued())
				state->execution_turn(ready);
			while (!observed && std::chrono::steady_clock::now() < until) {
				// A fixed count parks too early when an empty event probe avoids the mutex; leave a short window for a guest reply.
				const auto spin_until = std::min(until, std::chrono::steady_clock::now() + std::chrono::microseconds(5));
				// The clock is read every few probes: reading it at each one was the costliest part of the spin. The pause keeps the probes
				// from taking the lines the guest is writing its request to.
				for (unsigned spins = 1; !ready() && (spins++ % 4 || std::chrono::steady_clock::now() < spin_until);)
					__builtin_ia32_pause();
				if (!ready())
					state->execution_wait(ready, until);
				if (command() || queued_pending())
					serve_pending();
			}
			if (!observed) {
				if (sampling && std::chrono::steady_clock::now() < execution_deadline) {
					child.pause_task();
					child.set_timeout(std::chrono::seconds(2));
					status = child.wait();
					sampled_stop = true;
				} else {
					status = -1;
				}
			}
			forget_registers();
		} else {
			status = child.wait(state->poll_served ? &state->poll_served : nullptr);
		}
		// The slice belongs to this resume only; a later wait for a memory operation of the runtime must not see it expire.
		child.clear_slice();
		// A guest the kernel holds within an instruction is let finish it: the interrupt would cut the kernel's delay short and the instruction
		// would start over, so a thread spinning on a value that splits a cache line, as the .NET Framework's Interlocked.Read can, never
		// got past it while its slices kept ending. It is waited for a few times, then interrupted all the same.
		for (int held = 0; status == -2 && held < 20 && held_in_kernel(child.pid); ++held) {
			child.set_slice(std::chrono::milliseconds(15));
			status = child.wait(state->poll_served ? &state->poll_served : nullptr);
			child.clear_slice();
		}
		if (status == -2) {
			++state->slice_interrupts;
			// The guest ran a whole slice without a stop of its own: interrupt it where it is and let another thread have the processor.
			child.clear_slice();
			kill(child.pid, SIGSTOP);
			int interrupted = 0;
			while (waitpid(child.pid, &interrupted, 0) == -1 && errno == EINTR) {}
			if (!WIFSTOPPED(interrupted))
				throw std::runtime_error("native guest exited outside the runtime boundary");
			if (WSTOPSIG(interrupted) == SIGSTOP) {
				state->stopped = true;
				if (sampling) {
					user_regs_struct where{};
					trace(PTRACE_GETREGS, child.pid, &where);
					state->sampler(where.rip);
				}
				// Whether another thread wants the processor is asked again: it may have become ready through a served call meanwhile. When
				// none does, this thread starts a new slice, so the next served call does not interrupt it at once.
				const bool wanted = state->slice.count() > 0 && state->slice_yield && state->slice_wanted && state->slice_wanted();
				if (wanted && std::chrono::steady_clock::now() - state->slice_started >= state->slice && !inside_served_stub()) {
					++state->slice_yield_requests;
					state->slice_yield();
					state->slice_started = std::chrono::steady_clock::now();
				} else if (!wanted) {
					state->slice_started = std::chrono::steady_clock::now();
				}
				continue;
			}
			// A stop of the guest's own came first and the interrupt is still pending. Keep what describes that stop, then let the
			// pending signal be delivered: it arrives before the guest runs another instruction, and a resume without a signal drops it.
			siginfo_t kept{};
			trace(PTRACE_GETSIGINFO, child.pid, &kept);
			event_info = kept;
			status = interrupted;
			forget_registers();
			ptrace(state->seccomp ? PTRACE_CONT : PTRACE_SYSEMU, child.pid, nullptr, nullptr);
			int drained = 0;
			while (waitpid(child.pid, &drained, 0) == -1 && errno == EINTR) {}
			if (!WIFSTOPPED(drained) || WSTOPSIG(drained) != SIGSTOP)
				throw std::runtime_error("native guest did not stop for the pending interrupt");
		}
		if (status == -1) {
			if (std::getenv("RAVEN_TRACE_API")) {
				user_regs_struct where{};
				if (parallel()) {
					child.pause_task();
				} else {
					kill(child.pid, SIGSTOP);
					int ignored = 0;
					waitpid(child.pid, &ignored, 0);
				}
				if (ptrace(PTRACE_GETREGS, child.pid, 0, &where) == 0)
					std::cerr << "raven: guest timeout at rip=0x" << std::hex << where.rip << " rsp=0x" << where.rsp << std::dec << '\n';
			}
			child.stop();
			return {NativeStop::timeout, 0, 0, 0};
		}
		if (!WIFSTOPPED(status))
			throw std::runtime_error("native guest exited outside the runtime boundary");
		state->stopped = true;
		drain_queued_calls(true);
		trace(PTRACE_GETREGS, child.pid, &registers);
		signal = WSTOPSIG(status);
		if (parallel() && signal == SIGSTOP) {
			if (sampled_stop && state->sampler)
				state->sampler(registers.rip);
			continue;
		}
		if (state->seccomp && signal == SIGTRAP && (status >> 16) == PTRACE_EVENT_SECCOMP)
			signal = SIGTRAP | 0x80;
		// A stub that waited too long for the host thread stops on its own breakpoint. A call still in the block is taken there, the task
		// held in its stub; one answered meanwhile has nothing left to wait for.
		if (signal == SIGTRAP && registers.rip - 1 >= state->served_wait_offset && std::binary_search(state->call_stubs.begin(),
				state->call_stubs.end(), registers.rip - 1 - state->served_wait_offset)) {
			const uint64_t stub = registers.rip - 1 - state->served_wait_offset;
			uint64_t *const block = state->thread_served_block;
			if (block && __atomic_load_n(&block[0], __ATOMIC_ACQUIRE) == stub + 1 && !outer_calls.empty())
				return returned(block, stub + 1, true);
			if (!block || __atomic_load_n(&block[0], __ATOMIC_ACQUIRE) != stub)
				continue;
			++state->call_waits;
			const uint64_t seen = CallCycles::instance().seen(block, stub);
			waiting = WaitingCall{this, child.pid, block, stub + state->served_stub_size, block[5], {block[1], block[2], block[3], block[4]},
				{block[6], block[7], block[8], block[9]}, false, true, false, seen};
			state->awaiting_api = true;
			state->awaiting_syscall = false;
			state->debug_print_fault = false;
			state->pending_cleanup = 0;
			return {NativeStop::api, waiting.trap, waiting.trap + 1, SIGTRAP};
		}
		// A served stub that waited too long stops on its own breakpoint: its call is answered and it goes on where it stopped.
		if (signal == SIGTRAP && served_wait_trap(registers.rip - 1)) {
			++state->served_waits;
			serve_pending();
			continue;
		}
		if (signal == SIGTRAP && std::binary_search(state->served_yield_traps.begin(), state->served_yield_traps.end(), registers.rip - 1)) {
			++state->served_yields;
			state->served_block[11] = 0;
			if (state->slice.count() > 0 && state->slice_yield && state->slice_wanted && state->slice_wanted()) {
				++state->slice_yield_requests;
				state->slice_yield();
			}
			state->slice_started = std::chrono::steady_clock::now();
			continue;
		}
		if (state->steps_left && signal == SIGTRAP) {
			siginfo_t step{};
			trace(PTRACE_GETSIGINFO, child.pid, &step);
			if (step.si_code == TRAP_TRACE || step.si_code == 0) {
				if (state->step_sink)
					state->step_sink(registers.rip);
				--state->steps_left;
				continue;
			}
		}
		break;
	}
	if (signal == (SIGTRAP | 0x80)) {
		state->awaiting_syscall = true;
		return {NativeStop::syscall, registers.orig_rax, registers.rip, signal};
	}
	state->awaiting_syscall = false;
	siginfo_t info{};
	const bool x86 = state->mode == NativeMode::x86;
	// A trap right after the gate or an API breakpoint is that breakpoint: the stop reason is known without asking for it.
	if (event_info)
		info = *event_info;
	else if (signal == SIGTRAP && (registers.rip == (x86 ? state->return_gate : child.gate) + 1 || state->api_traps.contains(registers.rip - 1)))
		info.si_code = SI_KERNEL;
	else
		trace(PTRACE_GETSIGINFO, child.pid, &info);
	state->debug_print_fault=signal==SIGSEGV && info.si_code==SI_KERNEL && registers.rax==1;
	if (signal == SIGTRAP && info.si_code == TRAP_HWBKPT) {
		// Reading the stop reason is enough; DR6 must be cleared or the next trap reports a stale cause.
		ptrace(PTRACE_POKEUSER, child.pid, reinterpret_cast<void *>(offsetof(struct user, u_debugreg) + 6 * sizeof(unsigned long)), nullptr);
	}
	const bool breakpoint = signal == SIGTRAP && (info.si_code == TRAP_BRKPT || info.si_code == SI_KERNEL);
	if (breakpoint && registers.rip == (x86 ? state->return_gate : child.gate) + 1) {
		if (state->callbacks.empty()) {
			state->active_call = false;
		} else {
			try {
				set_context(state->callbacks.back());
			} catch (...) {
				child.stop();
				throw;
			}
			state->callbacks.pop_back();
			state->pending_cleanup = state->callback_cleanups.back();
			state->callback_cleanups.pop_back();
			state->awaiting_api = true;
		}
		return {NativeStop::returned, x86 ? registers.rax & 0xffffffff : registers.rax, registers.rip, signal};
	}
	if (breakpoint && state->api_traps.contains(registers.rip - 1)) {
		state->awaiting_api = true;
		state->pending_cleanup = state->api_traps.at(registers.rip - 1).cleanup;
		return {NativeStop::api, registers.rip - 1, registers.rip, signal};
	}
	const uint64_t fault_address = signal == SIGSEGV || signal == SIGBUS || signal == SIGILL || signal == SIGFPE ?
		reinterpret_cast<uint64_t>(info.si_addr) : 0;
	return {NativeStop::fault, registers.rax, registers.rip, signal, fault_address, info.si_code};
}

void NativeProcess::set_api_argument(size_t index, uint32_t value)
{
	state->require_stopped();
	if (!state->awaiting_api || state->mode != NativeMode::x86 || index > 127)
		throw std::runtime_error("native argument rewrite requires a pending x86 API call");
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	const uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
	write_memory(registers.rsp + 4 + index * 4, bytes);
}

void NativeProcess::set_time_slicing(std::chrono::milliseconds slice, std::function<bool()> wanted, std::function<void()> yield)
{
	state->slice = slice;
	state->slice_wanted = std::move(wanted);
	state->slice_yield = std::move(yield);
}

void NativeProcess::set_guest_sampler(std::chrono::milliseconds period, std::function<void(uint64_t)> sampler)
{
	state->sample_period = period;
	state->sampler = std::move(sampler);
}

void NativeProcess::make_served_block(uint64_t teb, uint64_t &guest, uint64_t *&host)
{
	const int descriptor = memfd_create("raven-thread-calls", MFD_CLOEXEC);
	if (descriptor < 0)
		throw std::runtime_error("cannot create thread call block");
	try {
		if (ftruncate(descriptor, 4096) != 0)
			throw std::runtime_error("cannot size thread call block");
		guest = map_descriptor(descriptor, 4096);
		// Raven's extended TEB holds this thread's command block beyond the Windows fields.
		write_memory(teb + 0x1ff0, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&guest), 8));
		host = static_cast<uint64_t *>(state->shared_views.at(guest)->host);
		state->served_owners.emplace(host, teb);
		host[request_routine] = state->request_gate;
		if (state->mode == NativeMode::x64 && std::getenv("RAVEN_QUEUED_GRAPHICS")) {
			host[ServedQueue::enabled] = 1;
			state->queued_blocks.emplace_back(guest, host);
		}
	} catch (...) {
		close(descriptor);
		throw;
	}
	close(descriptor);
}

uint64_t NativeProcess::serving_teb() const
{
	if (!state->serving) return 0;
	if (!parallel()) return state->thread_teb;
	const auto found = state->served_owners.find(state->thread_served_block);
	if (found == state->served_owners.end()) throw std::runtime_error("served call has no thread-block owner");
	return found->second;
}

bool NativeProcess::serving() const
{
	return state->serving;
}

void NativeProcess::decline_served()
{
	if (!state->serving || state->serving_queued)
		throw std::runtime_error("only a synchronous served call can be declined");
	state->served_declined = true;
}

bool NativeProcess::serving_queued() const
{
	return state->serving && state->serving_queued;
}

void NativeProcess::set_queued_gl_elements(bool available)
{
	if (parallel() && state->thread_served_block)
		__atomic_store_n(&state->thread_served_block[ServedQueue::gl_elements], uint64_t(available), __ATOMIC_RELEASE);
}

void NativeProcess::set_served_calls(uint64_t *block, std::vector<uint64_t> stubs, size_t stub_size, size_t wait_offset,
	std::vector<uint64_t> yield_traps, std::function<void(uint64_t)> handler, std::vector<uint64_t> queued_stubs)
{
	std::sort(stubs.begin(), stubs.end());
	std::sort(yield_traps.begin(), yield_traps.end());
	std::sort(queued_stubs.begin(), queued_stubs.end());
	state->queued_stubs = std::move(queued_stubs);
	state->served_wait_offset = wait_offset;
	state->served_block = block;
	state->served_stubs = std::move(stubs);
	state->served_yield_traps = std::move(yield_traps);
	state->served_stub_size = stub_size;
	state->served_handler = std::move(handler);
	if (parallel() && state->served_handler && !state->thread_served_block)
		make_served_block(context().registers.gs_base, state->thread_served_guest, state->thread_served_block);
	state->poll_served = [this] {
		if (!serve_pending())
			return false;
		// A served call can make another thread ready (a wake) without the guest stopping, so the guest gets a slice to be measured
		// against if it has none.
		// Give the acknowledged stub time to reach its own safe breakpoint before forcing a signal stop.
		const auto *block = parallel() ? state->thread_served_block : state->served_block;
		if (block[11])
			state->child->set_slice(state->slice);
		else if (state->slice.count() > 0 && state->slice_yield)
			state->child->ensure_slice(state->slice_started + state->slice);
		return true;
	};
}

void NativeProcess::set_call_stubs(std::vector<uint64_t> stubs)
{
	std::sort(stubs.begin(), stubs.end());
	state->call_stubs = std::move(stubs);
}

std::array<uint64_t, 3> NativeProcess::call_counts() const
{
	return {state->call_answers, state->call_waits, state->call_settles};
}

bool NativeProcess::served_wait_trap(uint64_t address) const
{
	const auto after = std::upper_bound(state->served_stubs.begin(), state->served_stubs.end(), address);
	return after != state->served_stubs.begin() && address - *(after - 1) == state->served_wait_offset;
}

bool NativeProcess::inside_served_stub() const
{
	if (state->served_stubs.empty())
		return false;
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	if (std::binary_search(state->served_yield_traps.begin(), state->served_yield_traps.end(), registers.rip - 1))
		return false;
	const auto after = std::upper_bound(state->served_stubs.begin(), state->served_stubs.end(), registers.rip);
	return after != state->served_stubs.begin() && registers.rip - *(after - 1) < state->served_stub_size;
}

uint64_t NativeProcess::serve_call(uint64_t stub, std::vector<uint64_t> words, std::array<uint64_t, 4> floats, uint64_t stack, bool queued)
{
	++state->served_calls;
	auto translated_words = std::move(state->translated_words);
	const auto translated_depth = state->translated_depth;
	const auto translated_floats = state->translated_floats;
	const auto translated_result = state->translated_result;
	state->translated_words.reset();
	const bool was_serving = state->serving;
	const bool was_queued = state->serving_queued;
	const uint64_t was_stack = state->served_stack;
	state->serving = true;
	state->serving_queued = queued;
	state->served_stack = stack;
	const auto restore = [&] {
		state->translated_words = std::move(translated_words);
		state->translated_depth = translated_depth;
		state->translated_floats = translated_floats;
		state->translated_result = translated_result;
		state->serving = was_serving;
		state->serving_queued = was_queued;
		state->served_stack = was_stack;
	};
	uint64_t result = 0;
	try {
		begin_translated_call(std::move(words), floats);
		state->served_handler(stub);
		result = end_translated_call();
	} catch (...) {
		state->translated_words.reset();
		restore();
		throw;
	}
	restore();
	return result;
}

bool NativeProcess::queued_pending() const
{
	const auto *block = parallel() ? state->thread_served_block : nullptr;
	return block && __atomic_load_n(&block[ServedQueue::head], __ATOMIC_ACQUIRE) != block[ServedQueue::tail];
}

bool NativeProcess::drain_queued_calls(bool all)
{
	if (all) {
		// A caller may have observed another thread's atomic publication without a Windows wait. The served handler selects each
		// producer's TLS, so flush every thread before a synchronous operation can observe their commands.
		auto *current = state->thread_served_block;
		bool drained = false;
		try {
			for (const auto &[guest, block] : state->queued_blocks) {
				(void)guest;
				if (__atomic_load_n(&block[ServedQueue::head], __ATOMIC_ACQUIRE) == block[ServedQueue::tail])
					continue;
				state->thread_served_block = block;
				drained |= drain_queued_calls();
			}
		} catch (...) {
			state->thread_served_block = current;
			throw;
		}
		state->thread_served_block = current;
		return drained;
	}
	if (!queued_pending())
		return false;
	auto *block = state->thread_served_block;
	const uint64_t head = __atomic_load_n(&block[ServedQueue::head], __ATOMIC_ACQUIRE);
	uint64_t tail = block[ServedQueue::tail];
	if (head - tail > ServedQueue::capacity)
		throw std::runtime_error("native served queue indices exceed capacity");
	// Consume a bounded snapshot. The producer may reuse a slot only after its arguments have been copied and served.
	while (tail != head) {
		const auto *record = block + ServedQueue::records + (tail % ServedQueue::capacity) * ServedQueue::words;
		const uint64_t stub = record[0];
		if (!std::binary_search(state->queued_stubs.begin(), state->queued_stubs.end(), stub))
			throw std::runtime_error("native served queue contains a synchronous or unknown call");
		serve_call(stub, {record[1], record[2], record[3], record[4], record[5], record[6], record[7], record[8]},
			{record[9], record[10], record[11], record[12]}, 0, true);
		++state->queued_calls;
		__atomic_store_n(&block[ServedQueue::tail], ++tail, __ATOMIC_RELEASE);
	}
	return true;
}

bool NativeProcess::serve_pending()
{
	const bool queued = drain_queued_calls();
	uint64_t *block = parallel() ? state->thread_served_block : state->served_block;
	if (!block)
		return queued;
	const uint64_t stub = __atomic_load_n(&block[0], __ATOMIC_ACQUIRE);
	if (!stub && !queued)
		return false;
	// A synchronous call observes every earlier queued record on this thread.
	bool declined = false;
	const uint64_t seen = stub ? CallCycles::instance().seen(block, stub) : 0;
	if (stub) {
		drain_queued_calls(true);
		state->served_declined = false;
		block[10] = serve_call(stub, {block[1], block[2], block[3], block[4]}, {block[6], block[7], block[8], block[9]}, block[5] + 40);
		declined = std::exchange(state->served_declined, false);
	}
	if (state->resume_hook)
		state->resume_hook();
	// The stub saves its result in RAX before obeying this request; later threads may then overwrite the shared result slot.
	const bool due = state->slice.count() > 0 && state->slice_yield &&
		std::chrono::steady_clock::now() - state->slice_started >= state->slice;
	block[11] = declined ? 2 : !parallel() && !state->served_yield_traps.empty() && due;
	if (stub) {
		CallCycles::instance().answered(block, stub, seen);
		__atomic_store_n(&block[0], 0, __ATOMIC_RELEASE);
	}
	// Parallel threads own their results, so another host frame can run as soon as this reply is published.
	if (parallel() && due) {
		state->slice_started = std::chrono::steady_clock::now();
		if (state->slice_wanted && state->slice_wanted())
			state->slice_yield();
	}
	return true;
}

void NativeProcess::set_resume_hook(std::function<void()> hook)
{
	state->resume_hook = std::move(hook);
}

void NativeProcess::begin_translated_call(std::vector<uint64_t> words, std::array<uint64_t, 4> floats)
{
	if (state->translated_words)
		throw std::runtime_error("a translated native call is already running");
	state->translated_words = std::move(words);
	state->translated_depth = state->callbacks.size();
	state->translated_floats = floats;
	state->translated_result = 0;
}

uint64_t NativeProcess::end_translated_call()
{
	state->translated_words.reset();
	return state->translated_result;
}

std::vector<uint64_t> NativeProcess::api_arguments(size_t count) const
{
	state->require_stopped();
	if (state->translated_words && state->callbacks.size() == state->translated_depth) {
		auto words = *state->translated_words;
		if (state->serving && count > words.size() && count <= 128) {
			const auto stack = read_memory(state->served_stack, (count - words.size()) * 8);
			for (size_t offset = 0; offset < stack.size(); offset += 8) {
				uint64_t value = 0;
				std::memcpy(&value, stack.data() + offset, 8);
				words.push_back(value);
			}
		}
		words.resize(count);
		return words;
	}
	if (!state->awaiting_api || count > 128)
		throw std::runtime_error("native process is not at an API or exceeds argument limit");
	if (waiting.task) {
		// The stub left the register arguments in the block; the others are on the stack it was called with.
		std::vector<uint64_t> arguments(waiting.words.begin(), waiting.words.end());
		arguments.resize(count);
		if (count > 4) {
			if (waiting.stack > UINT64_MAX - 40)
				throw std::runtime_error("native API stack address overflows");
			const auto stack = read_memory(waiting.stack + 40, (count - 4) * 8);
			std::memcpy(arguments.data() + 4, stack.data(), stack.size());
		}
		return arguments;
	}
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	if (state->mode == NativeMode::x86) {
		std::vector<uint64_t> values(count);
		if (!count)
			return values;
		const auto stack = read_memory(registers.rsp + 4, count * 4);
		for (size_t index = 0; index < count; ++index)
			for (size_t byte = 0; byte < 4; ++byte)
				values[index] |= uint64_t(stack[index * 4 + byte]) << (byte * 8);
		return values;
	}
	const uint64_t first[] = {registers.rcx, registers.rdx, registers.r8, registers.r9};
	std::vector<uint64_t> arguments(count);
	for (size_t index = 0; index < std::min(count, size_t(4)); ++index)
		arguments[index] = first[index];
	if (count > 4) {
		if (registers.rsp > UINT64_MAX - 40)
			throw std::runtime_error("native API stack address overflows");
		const auto stack = read_memory(registers.rsp + 40, (count - 4) * 8);
		for (size_t index = 4; index < count; ++index) {
			for (size_t byte = 0; byte < 8; ++byte)
				arguments[index] |= uint64_t(stack[(index - 4) * 8 + byte]) << (byte * 8);
		}
	}
	return arguments;
}

std::array<uint64_t, 4> NativeProcess::api_float_arguments() const
{
	state->require_stopped();
	if (state->translated_words && state->callbacks.size() == state->translated_depth)
		return state->translated_floats;
	if (!state->awaiting_api || state->mode != NativeMode::x64)
		throw std::runtime_error("native process is not at an x64 API call");
	if (waiting.task)
		return waiting.floats;
	user_fpregs_struct registers{};
	trace(PTRACE_GETFPREGS, state->child->pid, &registers);
	std::array<uint64_t, 4> result{};
	for (size_t index = 0; index < 4; ++index)
		std::memcpy(&result[index], reinterpret_cast<const uint8_t *>(registers.xmm_space) + index * 16, 8);
	return result;
}

void NativeProcess::enter_nt_service(uint64_t instruction, uint32_t number)
{
	state->require_stopped();
	if (state->mode != NativeMode::x64)
		throw std::runtime_error("native NT services are x64 only");
	if (!state->active_call || state->awaiting_api || !state->awaiting_syscall)
		throw std::runtime_error("native process is not at a pending syscall service");
	auto saved=context();
	if (saved.registers.rip!=instruction || saved.registers.orig_rax!=number || instruction<2 ||
		read_memory(instruction-2,2)!=std::vector<uint8_t>{0x0f,0x05} || !state->covers(instruction-2,3,PROT_EXEC) ||
		read_memory(instruction,1)!=std::vector<uint8_t>{0xc3})
		throw std::runtime_error("native NT service boundary does not match the captured syscall");
	// x64 NT service stubs preserve argument one in R10 before SYSCALL replaces RCX.
	saved.registers.rcx=saved.registers.r10;
	saved.registers.orig_rax=uint64_t(-1);
	set_context(saved);
	state->awaiting_syscall=false;
	state->awaiting_api=true;
}

void NativeProcess::complete_debug_print(uint64_t instruction)
{
	state->require_stopped();
	if (state->mode != NativeMode::x64)
		throw std::runtime_error("native debug print services are x64 only");
	const auto saved=context();
	if (!state->active_call || state->awaiting_api || state->awaiting_syscall || !state->debug_print_fault ||
		saved.registers.rip!=instruction || saved.registers.rax!=1 || !state->covers(instruction,4,PROT_EXEC) ||
		read_memory(instruction,4)!=std::vector<uint8_t>{0xcd,0x2d,0xcc,0xc3})
		throw std::runtime_error("native debug print does not match a captured INT 2D service");
	state->debug_print_fault=false;
	state->awaiting_api=true;
	return_api(0);
}

void NativeProcess::return_api(uint64_t value)
{
	state->require_stopped();
	if (state->translated_words && state->callbacks.size() == state->translated_depth) {
		state->translated_result = value;
		return;
	}
	if (!state->awaiting_api)
		throw std::runtime_error("native process is not awaiting an API result");
	if (waiting.task) {
		// The stub takes the answer from the block once the call is given up (see resume).
		waiting.block[10] = value;
		waiting.answered = true;
		state->awaiting_api = false;
		return;
	}
	user_regs_struct registers{};
	trace(PTRACE_GETREGS, state->child->pid, &registers);
	if (state->mode == NativeMode::x86) {
		if (std::getenv("RAVEN_TRACE_X86"))
			std::fprintf(stderr, "  -> %llx\n", static_cast<unsigned long long>(value & 0xffffffffULL));
		const auto stack = read_memory(registers.rsp, 4);
		registers.rip = 0;
		for (size_t byte = 0; byte < 4; ++byte)
			registers.rip |= uint64_t(stack[byte]) << (byte * 8);
		registers.rsp += 4 + state->pending_cleanup;
		registers.rax = value & 0xffffffff;
		registers.rdx = value >> 32;
		registers.orig_rax = uint64_t(-1);
		trace(PTRACE_SETREGS, state->child->pid, &registers);
		state->awaiting_api = false;
		return;
	}
	// Stopped right after a breakpoint that a RET follows: the guest returns by itself.
	if (const auto trap = state->api_traps.find(registers.rip - 1); trap != state->api_traps.end() && trap->second.returns) {
		registers.rax = value;
		registers.orig_rax = uint64_t(-1);
		trace(PTRACE_SETREGS, state->child->pid, &registers);
		state->awaiting_api = false;
		return;
	}
	if (registers.rsp > UINT64_MAX - 8)
		throw std::runtime_error("native API return stack address overflows");
	const auto stack = read_memory(registers.rsp, 8);
	registers.rip = 0;
	for (size_t byte = 0; byte < 8; ++byte)
		registers.rip |= uint64_t(stack[byte]) << (byte * 8);
	registers.rsp += 8;
	registers.rax = value;
	registers.orig_rax = uint64_t(-1);
	trace(PTRACE_SETREGS, state->child->pid, &registers);
	state->awaiting_api = false;
}

static NativeResult execute(const PEImage &image, LoadedImage &loaded, std::span<LoadedImage *const> images,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	NativeProcess process(images, bootstrap, timeout);
	return process.call(loaded.base() + image.entry, {}, timeout);
}

NativeResult run_native_entry(const PEImage &image, LoadedImage &loaded,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	if (!image.is_64)
		throw std::runtime_error("native PE32 execution is not implemented");
	if ((image.characteristics & 0x2000) != 0 || !image.imports().empty() ||
		image.directories[9].size != 0 || image.directories[13].size != 0 || image.directories[14].size != 0)
		throw std::runtime_error("native entry requires an executable without imports, TLS, delayed imports or CLR initialization");
	LoadedImage *images[] = {&loaded};
	return execute(image, loaded, images, bootstrap, timeout);
}

NativeResult run_native_modules(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	if ((entry.image.characteristics & 0x2000) != 0 ||
		!std::any_of(modules.all().begin(), modules.all().end(), [&](const auto &item) { return item.second.get() == &entry; }))
		throw std::runtime_error("native module entry must be an executable in the supplied graph");
	std::vector<LoadedImage *> images;
	for (const auto &[name, module] : modules.all()) {
		if (!module->image.is_64)
			throw std::runtime_error("native PE32 execution is not implemented");
		if (module->image.directories[9].size || module->image.directories[13].size || module->image.directories[14].size ||
			((module->image.characteristics & 0x2000) != 0 && module->image.entry))
			throw std::runtime_error("native module requires unsupported DLL, TLS, delayed import or CLR initialization: " + name);
		images.push_back(&module->loaded);
	}
	modules.bind_imports();
	return execute(entry.image, entry.loaded, images, bootstrap, timeout);
}

}
