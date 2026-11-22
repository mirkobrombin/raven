#include "threads.h"
#include "cpu/agent.h"
#include "cpu/agent_tasks.h"
#include "kernel_waits.h"
#include "cpu/agent_tasks.h"
#include "cpu/call_cycles.h"
#include "desktop_lock.h"
#include "handle_channel.h"

#include <cstdlib>
#include <map>
#include <fcntl.h>
#include <iostream>

#include "api.h"
#include "shared_file.h"
#include "inherit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <climits>
#include <csignal>
#include <sys/wait.h>

namespace raven {

// Each host thread serves one guest thread for its whole life (see host_main), and the first one the main thread.
static thread_local uint32_t own_thread = 0;
// The guest thread whose host thread is parked in NativeThreads::park, and so holds no turn, while it answers calls there.
static thread_local GuestThread *parked_here = nullptr;
// A call asked to wait beside the turn while its host thread still held it (see park): it parks, and the wait is taken there.
static thread_local bool beside_wanted = false;

namespace {

// A guest thread runs its API handlers on this stack, drivers included: shader compilation needs far more than a megabyte.
constexpr size_t host_stack_size = 16 * 1024 * 1024;

}

NativeThreads::NativeThreads(NativeProcess &process, NativeEnvironment &environment, NativeMemory &memory, NativeTLS *tls,
	std::chrono::steady_clock::time_point deadline)
	: process(process), environment(environment), memory(memory), tls(tls), deadline(deadline)
{
	auto main = std::make_shared<GuestThread>();
	main->id = 1;
	main->teb = environment.teb();
	main->state = GuestThread::State::running;
	main->main = true;
	threads.push_back(main);
	running = main.get();
	// A thread that computes or spins without calling the runtime must not keep the others from running: after a few milliseconds the
	// guest is interrupted and the next thread gets the processor.
	// RAVEN_SLICE_MS sets the length of a time slice in milliseconds; zero turns time slicing off.
	long slice = 5;
	if (const char *text = std::getenv("RAVEN_SLICE_MS"))
		slice = std::strtol(text, nullptr, 10);
	if (process.parallel() && threaded_host())
		slice = 0;
	process.set_time_slicing(std::chrono::milliseconds(slice < 0 ? 0 : slice), [this] { return !tearing_down && live() > 1; }, [this] {
		running->spent_slice = true;
		running->set_aside = std::chrono::steady_clock::now();
		// The thread that has just read the user's input keeps the processor unless another is due it: one that woke, or one kept
		// waiting long enough (see took_input).
		if (running->set_aside < running->input_until && !pick(false, true))
			return;
		yield();
	});
	wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (wake < 0)
		throw std::runtime_error("cannot create the scheduler's wake descriptor");
	shared_change_listener = wake;
	if (process.parallel() && threaded_host()) {
		// Each guest thread gets a host thread of its own (see below); the first one is this one.
		threaded = true;
		holder = main.get();
		main->host_tid = pid_t(::gettid());
		own_thread = main->id;
		NativeProcess::arm_host_thread();
		process.set_execution_park([this](pid_t task, std::chrono::steady_clock::time_point limit, const uint64_t *block) {
			return park(task, limit, block);
		});
		admin.id = 0;
		admin.snapshot.task = process.administrative_task();
		admin.snapshot.stopped = true;
		admin.snapshot.context_in_task = true;
		process.hand_task(admin.snapshot.task);
		pthread_attr_t attributes;
		pthread_attr_init(&attributes);
		pthread_attr_setstacksize(&attributes, host_stack_size);
		if (pthread_create(&admin_host, &attributes, [](void *self) -> void * {
				static_cast<NativeThreads *>(self)->admin_main();
				return nullptr;
			}, this) != 0)
			throw std::runtime_error("cannot start the runtime's administrative thread");
		pthread_attr_destroy(&attributes);
		admin_hosted = true;
	} else if (process.parallel()) {
		process.set_execution_wait([this](std::function<bool()> ready, std::chrono::steady_clock::time_point limit) {
			while (!ready() && std::chrono::steady_clock::now() < limit)
				block(ready, limit);
		});
		process.set_execution_turn([this](std::function<bool()> ready) { block(std::move(ready), this->deadline, true); });
	}
}

NativeThreads::~NativeThreads()
{
	if (std::getenv("RAVEN_TRACE_WAITS"))
		std::fprintf(stderr,
			"waits: conditions looked at %llu times\nwaits: processor times read %llu times\nwaits: stops seen late %llu times\n",
			(unsigned long long)condition_looks.load(), (unsigned long long)processor_times.load(), (unsigned long long)late_stops.load());
	if (shared_change_listener == wake)
		shared_change_listener = -1;
	process.set_time_slicing(std::chrono::milliseconds(0), nullptr, nullptr);
	// Host frames of threads that never finished hold objects that only their unwinding releases; a failure here must not escape.
	try {
		unwind_all();
	} catch (...) {
	}
	process.set_execution_wait(nullptr);
	process.set_execution_turn(nullptr);
	if (threaded) {
		process.set_execution_park(nullptr);
		{
			std::lock_guard lock(gate);
			stopping = true;
		}
		nudge();
		// The administrative thread gives the memory task back before it ends: the process still uses it while it is torn down.
		if (holder == running)
			leave(*running, false);
		if (admin_hosted) {
			pthread_join(admin_host, nullptr);
			admin_hosted = false;
			// A process that ended already took the task with it.
			try {
				NativeProcess::adopt_task(admin.snapshot.task);
			} catch (const std::exception &) {
			}
		}
		for (const auto &thread : threads)
			if (thread->hosted) {
				pthread_join(thread->host, nullptr);
				thread->hosted = false;
			}
	}
	::close(wake);
}

void NativeThreads::stop_time_slicing()
{
	process.set_time_slicing(std::chrono::milliseconds(0), nullptr, nullptr);
}

bool NativeThreads::others_waiting() const
{
	return std::any_of(threads.begin(), threads.end(), [this](const auto &thread) {
		return thread.get() != running && thread->state == GuestThread::State::waiting && thread->suspend_count == 0;
	});
}

size_t NativeThreads::live() const
{
	return size_t(std::count_if(threads.begin(), threads.end(), [](const auto &thread) { return thread->state != GuestThread::State::done; }));
}

std::vector<uint32_t> NativeThreads::thread_ids() const
{
	std::vector<uint32_t> result;
	for (const auto &thread : threads)
		if (thread->state != GuestThread::State::done)
			result.push_back(thread->id);
	return result;
}

std::vector<uint64_t> NativeThreads::thread_blocks() const
{
	std::vector<uint64_t> result;
	for (const auto &thread : threads)
		if (thread->state != GuestThread::State::done && thread->teb)
			result.push_back(thread->teb);
	return result;
}

std::shared_ptr<GuestThread> NativeThreads::find(uint32_t id) const
{
	for (const auto &thread : threads)
		if (thread->id == id)
			return thread;
	return nullptr;
}

std::shared_ptr<GuestThread> NativeThreads::find_told(uint32_t id)
{
	std::lock_guard lock(gate);
	return find(id);
}

std::shared_ptr<GuestThread> NativeThreads::create(uint64_t start, uint64_t parameter, uint64_t stack_size, bool suspended)
{
	if (process.parallel())
		reap();
	// Windows sets no small limit: the .NET thread pool parks a few hundred idle workers at once. The cap only stops a runaway loop.
	if (live() >= 8192)
		throw std::runtime_error("native guest thread limit reached");
	auto thread = std::make_shared<GuestThread>();
	const uint64_t size = std::clamp<uint64_t>((stack_size + 0xffff) & ~uint64_t(0xffff), 256 * 1024, 64 * 1024 * 1024);
	// The stack is reserved, as Windows reserves a thread's stack, and only its top is committed: Windows commits more as the thread
	// reaches its guard page, so a new thread is charged for what it starts with, not its whole reservation (a thread pool of one
	// thread per processor with 8 MiB stacks would otherwise exhaust the commit limit). The pages below stay reserved in the
	// accounting and are open to the thread, standing for the growth Windows gives it. One unreadable page below the stack turns an
	// overflow into a fault instead of silent corruption.
	const auto range = memory.allocate(0, size + 4096, 0x2000, 1);
	if (range.status)
		throw std::runtime_error("native guest thread stack allocation failed");
	// The host maps the stack as well: a call's arguments, results and return address are mostly there.
	try {
		process.share_memory(range.address, size + 4096);
	} catch (...) {
		memory.free(range.address, 0, 0x8000);
		throw;
	}
	const uint64_t commit = std::min<uint64_t>(size, 64 * 1024), top = range.address + 4096 + size;
	if (memory.allocate(top - commit, commit, 0x1000, 4).status) {
		memory.free(range.address, 0, 0x8000);
		throw std::runtime_error("native guest thread stack allocation failed");
	}
	if (size > commit)
		process.protect_memory(range.address + 4096, size - commit, PROT_READ | PROT_WRITE);
	thread->stack_low = range.address + 4096;
	thread->stack_high = range.address + size + 4096;
	thread->id = next_id;
	next_id += 4;
	thread->teb = environment.create_thread_block(thread->stack_high, thread->stack_low, thread->id);
	if (tls)
		tls->create_thread(thread->teb);
	if (block_made)
		block_made(thread->teb);
	thread->start = start;
	thread->parameter = parameter;
	thread->snapshot = process.new_thread(thread->stack_high - 64, thread->teb);
	thread->suspend_count = suspended ? 1 : 0;
	if (threaded) {
		thread->state = GuestThread::State::ready;
		{
			// Under the gate as well as the turn: a host thread serving its guest beside the turn looks threads up by id (find_told).
			std::lock_guard lock(gate);
			threads.push_back(thread);
		}
		pthread_attr_t attributes;
		pthread_attr_init(&attributes);
		pthread_attr_setstacksize(&attributes, host_stack_size);
		struct Start {
			NativeThreads *self;
			GuestThread *thread;
		};
		auto *start = new Start{this, thread.get()};
		const int failed = pthread_create(&thread->host, &attributes, [](void *argument) -> void * {
			const Start start = *static_cast<Start *>(argument);
			delete static_cast<Start *>(argument);
			start.self->host_main(*start.thread);
			return nullptr;
		}, start);
		pthread_attr_destroy(&attributes);
		if (failed) {
			delete start;
			threads.pop_back();
			throw std::runtime_error("cannot start a host thread for a guest thread");
		}
		thread->hosted = true;
		return thread;
	}
	// Left uninitialised, so only the pages a handler reaches are ever backed.
	thread->host_stack = std::make_unique_for_overwrite<char[]>(host_stack_size);
	getcontext(&thread->context);
	thread->context.uc_stack.ss_sp = thread->host_stack.get();
	thread->context.uc_stack.ss_size = host_stack_size;
	thread->context.uc_link = nullptr;
	const auto address = reinterpret_cast<uintptr_t>(thread.get());
	makecontext(&thread->context, reinterpret_cast<void (*)()>(&NativeThreads::trampoline), 2, unsigned(address & 0xffffffffU), unsigned(address >> 32));
	thread->state = GuestThread::State::ready;
	threads.push_back(thread);
	return thread;
}

namespace {
NativeThreads *active_scheduler = nullptr;
}

void NativeThreads::trampoline(unsigned low, unsigned high)
{
	auto *thread = reinterpret_cast<GuestThread *>(uintptr_t(low) | uintptr_t(high) << 32);
	active_scheduler->thread_main(*thread);
}

void NativeThreads::thread_main(GuestThread &thread)
{
	uint32_t code = 0;
	try {
		if (thread.unwinding)
			throw ThreadExit{0};
		if (notifier)
			notifier(2);
		const std::vector<uint64_t> arguments = thread.arguments.empty() ? std::vector<uint64_t>{thread.parameter} : thread.arguments;
		const auto result = run(thread.start, arguments);
		if (result.stop != NativeStop::returned)
			throw result;
		code = uint32_t(result.value);
		detach_current();
	} catch (const ThreadExit &exit) {
		code = exit.code;
		if (exit.detach) {
			try {
				detach_current();
			} catch (const ThreadExit &) {
				// A library that ends the thread again while it is told the thread ends: the thread is ending already.
			}
		}
	} catch (...) {
		// A thread that finds the process ended is only leaving with it, unless it is the one that ended it (its time ran out).
		bool ended_it = true;
		try {
			throw;
		} catch (const NativeResult &) {
		} catch (...) {
			ended_it = !(threaded && process.gone());
		}
		if (!pending && ended_it)
			pending = std::current_exception();
		// RAVEN_TRACE_API names the thread and the error that ends the process from a thread other than the first.
		if (std::getenv("RAVEN_TRACE_API")) {
			try {
				throw;
			} catch (const std::exception &error) {
				std::cerr << "raven: guest thread " << thread.id << " failed in " << api_in_progress << ": " << error.what() << "\n";
			} catch (...) {
				std::cerr << "raven: guest thread " << thread.id << " ended with a stop\n";
			}
		}
	}
	finish(thread, code);
	if (threaded)
		return;
	// A finished coroutine never resumes; hand the processor on and stay parked. What the scheduler throws here (the process deadline)
	// is for the main thread, which receives it through the pending exception.
	for (;;) {
		try {
			schedule();
		} catch (...) {
			// The first error is the one that ended the process: failing to save a thread of the process it ended must not replace it.
			if (!pending)
				pending = std::current_exception();
			// The main thread rethrows it from its own switch; hand the processor to it directly.
			auto &main = *threads.front();
			if (main.state != GuestThread::State::done && &main != running) {
				try {
					switch_to(main);
				} catch (...) {
					// The guest process is gone (the deadline ended it), so no guest state can be saved or loaded: only the host stack is handed
					// over, and the main thread ends the run with the pending error.
					GuestThread &previous = *running;
					running = &main;
					if (previous.state == GuestThread::State::running)
						previous.state = GuestThread::State::ready;
					main.state = GuestThread::State::running;
					active_scheduler = this;
					swapcontext(&previous.context, &main.context);
				}
			}
		}
	}
}

void NativeThreads::finish(GuestThread &thread, uint32_t code)
{
	signaled();
	// The mutexes the thread holds in the table are abandoned to their waiters, and a wait of its own there, or on shared objects, ends.
	raven_waits_thread_ended(kernel_waits(), thread.id);
	call_away(thread);
	if (&thread != running && threaded) {
		// Its host thread reaps the task and leaves through the thread's frames once it has its turn.
		if (thread.snapshot.task > 0)
			::kill(thread.snapshot.task, SIGKILL);
		std::lock_guard lock(gate);
		thread.paused = false;
		if (!thread.parked && thread.hosted && !thread.outside)
			enqueue(thread);
		thread.turn.notify_one();
	} else if (&thread != running) {
		process.stop_thread(thread.snapshot);
		thread.snapshot.task = 0;
	}
	thread.exit_code = code;
	thread.state = GuestThread::State::done;
	{
		std::lock_guard lock(thread.end_lock);
		if (thread.end_object)
			raven_waits_set(kernel_waits(), thread.end_object, nullptr);
	}
	for (const auto &listener : end_listeners)
		listener(thread.id);
	moved();
	thread.impersonation.reset();
	thread.ready = nullptr;
	for (const int report : thread.exit_reports) {
		[[maybe_unused]] const auto written = ::write(report, &code, sizeof code);
		::close(report);
	}
	thread.exit_reports.clear();
}

GuestThread::~GuestThread()
{
	if (end_object)
		raven_waits_close(kernel_waits(), end_object);
}

uint32_t NativeThreads::end_object(GuestThread &thread)
{
	// Made under the same lock its end sets it under, after the end marks the thread done: it is made set when the thread has ended.
	std::lock_guard lock(thread.end_lock);
	if (!thread.end_object)
		thread.end_object = raven_waits_create(kernel_waits(), raven_manual_event, thread.state == GuestThread::State::done, 0);
	return thread.end_object;
}

bool ThreadObject::describe(InheritedHandle &record) const
{
	int ends[2];
	if (::pipe2(ends, O_CLOEXEC))
		return false;
	if (thread->state == GuestThread::State::done) {
		[[maybe_unused]] const auto written = ::write(ends[1], &thread->exit_code, sizeof thread->exit_code);
		::close(ends[1]);
	} else {
		thread->exit_reports.push_back(ends[1]);
	}
	record.kind = "thread";
	record.descriptor = ends[0];
	record.size = thread->id;
	return true;
}

RemoteThreadObject::~RemoteThreadObject()
{
	if (descriptor >= 0)
		::close(descriptor);
}

std::optional<uint32_t> RemoteThreadObject::code() const
{
	if (ended || descriptor < 0)
		return ended;
	pollfd watch{descriptor, POLLIN, 0};
	if (::poll(&watch, 1, 0) <= 0)
		return std::nullopt;
	uint32_t value = 0;
	// A process that ended without reporting the thread leaves nothing to read: its threads ended with it.
	ended = ::read(descriptor, &value, sizeof value) == ssize_t(sizeof value) ? value : 1;
	return ended;
}

void NativeThreads::release_resources(GuestThread &thread)
{
	if (thread.main || !thread.teb)
		return;
	process.stop_thread(thread.snapshot);
	if (thread.snapshot.served_guest) {
		process.unmap_memory(thread.snapshot.served_guest);
		thread.snapshot.served_guest = 0;
		thread.snapshot.served_block = nullptr;
	}
	if (tls)
		tls->release_thread(thread.teb);
	if (block_released)
		block_released(thread.teb);
	environment.release_thread_block(thread.teb);
	thread.teb = 0;
	if (thread.stack_low) {
		memory.free(thread.stack_low - 4096, 0, 0x8000);
		thread.stack_low = thread.stack_high = 0;
	}
}

void NativeThreads::reap()
{
	if (threaded) {
		// A host thread that has left the runtime for good is joined, then what its guest thread held is given back.
		for (auto &thread : threads)
			// The environment still stands on the thread that served last, which may be the one that ended: it is given back once
			// another thread has been served.
			if (thread->state == GuestThread::State::done && thread.get() != running && thread->hosted && thread->teb != environment.teb() && [&] {
					std::lock_guard lock(gate);
					return thread->host_tid == -1;
				}()) {
				pthread_join(thread->host, nullptr);
				thread->hosted = false;
				release_resources(*thread);
			}
		return;
	}
	for (auto &thread : threads)
		if (thread->state == GuestThread::State::done && thread.get() != running && thread->host_stack) {
			thread->host_stack.reset();
			release_resources(*thread);
		}
}

std::shared_ptr<GuestThread> NativeThreads::pick(bool include_current, bool only_due)
{
	const auto now = std::chrono::steady_clock::now();
	const size_t count = threads.size();
	size_t start = 0;
	for (size_t index = 0; index < count; ++index)
		if (threads[index].get() == running)
			start = index;
	// A thread that computes until its slice is taken away, as a compiler working in the background, shares one processor here with
	// the threads that draw and answer the pointer. Those go first: the computing thread runs when none of them can, or once it has
	// been kept waiting for a few slices.
	std::shared_ptr<GuestThread> computing;
	for (size_t step = 1; step <= count; ++step) {
		const auto &thread = threads[(start + step) % count];
		if (thread.get() == running && !include_current)
			continue;
		if (thread->state == GuestThread::State::done || thread->suspend_count > 0)
			continue;
		if (thread->state == GuestThread::State::ready) {
			if (!thread->spent_slice || now < thread->input_until || now - thread->set_aside >= std::chrono::milliseconds(30))
				return thread;
			if (!computing)
				computing = thread;
			continue;
		}
		if (thread->state == GuestThread::State::waiting) {
			if (thread->can_go_on() || (thread->wake && now >= *thread->wake))
				return thread;
		}
	}
	return only_due ? nullptr : computing;
}

void NativeThreads::took_input()
{
	if (running)
		running->input_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
}

void NativeThreads::check_pending()
{
	if (!pending || !running->main)
		return;
	const auto exception = pending;
	pending = nullptr;
	// The error that ended another thread ends the process; the call this thread was in, waiting or running, will never complete.
	if (!process.gone())
		process.end_call();
	std::rethrow_exception(exception);
}

void NativeThreads::switch_to(GuestThread &next)
{
	if (&next == running)
		return;
	GuestThread &previous = *running;
	previous.snapshot = process.save_thread(false);
	process.load_thread(next.snapshot);
	environment.set_current(next.teb);
	running = &next;
	if (previous.state == GuestThread::State::running)
		previous.state = GuestThread::State::ready;
	next.state = GuestThread::State::running;
	active_scheduler = this;
	swapcontext(&previous.context, &next.context);
	// Resumed: whoever switched back already loaded this thread's guest state.
	if (process.stopped())
		reap();
	if (running->unwinding && !running->main)
		throw ThreadExit{0};
	check_pending();
}

void NativeThreads::schedule()
{
	serve();
	for (;;) {
		if (!tearing_down && std::chrono::steady_clock::now() >= deadline)
			throw NativeResult{NativeStop::timeout, 0, 0, 0};
		// A pending exception from a finished thread is for the main thread alone.
		if (pending && !running->main) {
			auto &main = *threads.front();
			if (main.state != GuestThread::State::done) {
				switch_to(main);
				continue;
			}
		}
		// The choice is held as a plain pointer: the frame of a thread that is parked for good is freed without unwinding, and an owning
		// reference kept in it would never be released. The thread list owns every thread.
		if (GuestThread *next = pick(true).get()) {
			if (next == running) {
				running->state = GuestThread::State::running;
				return;
			}
			switch_to(*next);
			if (running->state == GuestThread::State::done)
				continue;
			return;
		}
		// Nothing can run: wait for the earliest timeout, letting the host deliver outside events meanwhile.
		serve();
		for (const auto &pump : pumps)
			pump();
		// What the services and pumps finished, such as a pipe read that sets an event, may have made a thread ready: it runs now rather
		// than after the next sleep.
		if (pick(true))
			continue;
		auto wake = std::chrono::steady_clock::time_point::max();
		bool any = false;
		for (const auto &thread : threads)
			if (thread->state == GuestThread::State::waiting && thread->wake && thread->suspend_count == 0) {
				wake = std::min(wake, *thread->wake);
				any = true;
			}
		const auto now = std::chrono::steady_clock::now();
		// A shared object can still be signaled by another process while every local thread waits.
		if (!any && !poll && !shared_object_count.load() && !std::any_of(externals.begin(), externals.end(), [](const auto &pending) { return pending(); }))
			throw std::runtime_error("native guest threads deadlocked: every thread waits without a timeout");
		const auto limit = std::min(wake, deadline);
		// The process sleeps until a host thread nudges it, another process changes a shared object, a pending operation's descriptor is
		// ready or the earliest timeout, to the nanosecond: a wait cut to whole milliseconds would spin through its last one. Conditions
		// nothing announces, such as a child process ending, are looked at every 20 ms.
		const auto slice = std::min<std::chrono::nanoseconds>(limit > now ? limit - now : std::chrono::nanoseconds(0), std::chrono::milliseconds(20));
		if (poll) {
			poll(std::chrono::ceil<std::chrono::milliseconds>(slice));
		} else if (slice.count() > 0) {
			std::vector<pollfd> descriptors{{this->wake, POLLIN, 0}};
			if (process.execution_descriptor() >= 0)
				descriptors.push_back({process.execution_descriptor(), POLLIN, 0});
			if (watched)
				watched(descriptors);
			const timespec timeout{slice.count() / 1000000000, slice.count() % 1000000000};
			::ppoll(descriptors.data(), descriptors.size(), &timeout, nullptr);
			uint64_t drained = 0;
			while (::read(this->wake, &drained, sizeof drained) > 0) {}
			process.drain_execution();
		}
	}
}

std::chrono::steady_clock::time_point NativeThreads::idle_until(std::chrono::steady_clock::time_point limit) const
{
	bool unannounced = poll || shared_object_count.load() || std::any_of(externals.begin(), externals.end(), [](const auto &pending) { return pending(); });
	for (const auto &thread : threads)
		if (thread.get() != running && thread->state == GuestThread::State::waiting && thread->suspend_count == 0) {
			if (thread->wake)
				limit = std::min(limit, *thread->wake);
			else
				unannounced = true;
		} else if (thread.get() != running && thread->state == GuestThread::State::ready) {
			return std::chrono::steady_clock::now();
		}
	return unannounced ? std::min(limit, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)) : limit;
}

void NativeThreads::idle_descriptors(std::vector<pollfd> &descriptors) const
{
	descriptors.push_back({wake, POLLIN, 0});
	if (process.execution_descriptor() >= 0)
		descriptors.push_back({process.execution_descriptor(), POLLIN, 0});
	if (watched)
		watched(descriptors);
}

void NativeThreads::drain_wake()
{
	uint64_t drained = 0;
	while (::read(wake, &drained, sizeof drained) > 0) {}
	process.drain_execution();
}

void NativeThreads::watch_added(int descriptor, short events)
{
	if (admin_hosted && std::none_of(admin_sources.begin(), admin_sources.end(), [&](const pollfd &source) {
			return source.fd == descriptor && (source.events & events) == events;
		}))
		nudge();
}

void NativeThreads::nudge()
{
	const uint64_t one = 1;
	[[maybe_unused]] const auto written = ::write(wake, &one, sizeof one);
}

// Counts a thread among those that wait in the runtime for as long as its wait lasts, however it ends.
struct CountedWait {
	std::atomic<int> &count;
	explicit CountedWait(std::atomic<int> &count) : count(count) { count.fetch_add(1, std::memory_order_acq_rel); }
	~CountedWait() { count.fetch_sub(1, std::memory_order_release); }
};

std::function<void()> NativeThreads::remote_nudge() const
{
	const int copy = ::fcntl(wake, F_DUPFD_CLOEXEC, 0);
	if (copy < 0)
		return [] {};
	const auto descriptor = std::shared_ptr<int>(new int(copy), [](int *held) {
		::close(*held);
		delete held;
	});
	return [descriptor] {
		const uint64_t one = 1;
		[[maybe_unused]] const auto written = ::write(*descriptor, &one, sizeof one);
	};
}

bool NativeThreads::block(std::function<bool()> ready, std::optional<std::chrono::steady_clock::time_point> wake, bool turn, bool polled,
	bool told)
{
	static const bool count_looks = std::getenv("RAVEN_TRACE_WAITS");
	if (!std::exchange(lined_block, false) && count_looks && !turn)
		ready = [this, inner = std::move(ready)] {
			condition_looks.fetch_add(1, std::memory_order_relaxed);
			return inner();
		};
	// A thread that gives its turn waits even when it could go on: the others that can run go first.
	if (!turn && ready())
		return true;
	if (wake && std::chrono::steady_clock::now() >= *wake) {
		// A wait that does not wait still sees what the host finished meanwhile, such as a pipe read completing: a program that only
		// polls with a zero timeout would otherwise keep a thread runnable and never let the pumps run. Once a millisecond is enough.
		const auto now = std::chrono::steady_clock::now();
		if (now - pumped < std::chrono::milliseconds(1))
			return false;
		// A pump may start a callback thread, which maps its stack: a serial guest answered without stopping cannot have memory mapped, so
		// the call is given up and runs again stopped at its trap.
		if (process.serving() && !process.parallel()) {
			process.decline_served();
			return false;
		}
		pumped = now;
		serve();
		for (const auto &pump : pumps)
			pump();
		return ready();
	}
	auto &me = *running;
	const CountedWait counted(waiting_threads);
	// A wait can begin inside another: guest code the scheduler runs for this thread while it waits (a callback) comes back here. The
	// wait outside is put back when this one ends, or its thread would be left waiting for nothing and never run again.
	auto outer_ready = std::move(me.ready);
	const auto outer_wake = me.wake;
	const auto outer_state = me.state;
	me.ready = std::move(ready);
	me.wake = wake;
	me.state = GuestThread::State::waiting;
	me.spent_slice = false;
	// The condition reads the waiting frame: it goes with the wait, also when an error or the end of the process leaves the frame. A thread
	// that leaves it that way runs on, unwinding: left waiting, with nothing that could wake it, the teardown would never come back to it.
	struct Leave {
		GuestThread &thread;
		std::function<bool()> &outer_ready;
		std::optional<std::chrono::steady_clock::time_point> outer_wake;
		GuestThread::State outer_state;
		~Leave()
		{
			const bool nested = outer_state == GuestThread::State::waiting && outer_ready;
			thread.ready = nested ? std::move(outer_ready) : nullptr;
			thread.wake = nested ? outer_wake : std::nullopt;
			if (thread.state == GuestThread::State::waiting && !nested)
				thread.state = GuestThread::State::running;
			else if (nested && thread.state != GuestThread::State::done)
				thread.state = GuestThread::State::waiting;
		}
	} leave{me, outer_ready, outer_wake, outer_state};
	// A wait only another thread ends: whoever tells it changes what it waits for and then looks at its state under the gate, so the
	// condition looked at once more after the gate sees a tell that came before the thread was marked as waiting.
	const bool outer_told = me.told_only;
	struct Told {
		GuestThread &thread;
		bool outer;
		~Told() { thread.told_only = outer; thread.told_holding = false; }
	} told_back{me, outer_told};
	if (threaded) {
		{
			std::lock_guard lock(gate);
			me.told_only = told;
		}
		if (told && me.ready())
			return true;
	}
	// RAVEN_TRACE_WAITS names the waits that last longer than a quarter of a second, or than the milliseconds it gives when more than
	// one: the API and how long it kept its thread.
	static const bool traced = std::getenv("RAVEN_TRACE_WAITS") != nullptr;
	static const long traced_from = traced && std::atol(std::getenv("RAVEN_TRACE_WAITS")) > 1 ? std::atol(std::getenv("RAVEN_TRACE_WAITS")) : 250;
	const auto began = std::chrono::steady_clock::now();
	const char *api = api_in_progress;
	me.waiting_in = api;
	me.waiting_since = began;
	me.waits.fetch_add(1, std::memory_order_release);
	moved();
	const bool outer_polled = std::exchange(me.polled, polled);
	me.seen_signals = 0;
	if (threaded)
		threaded_block(me.ready, wake);
	else
		schedule();
	me.polled = outer_polled;
	me.waiting_in = nullptr;
	const bool satisfied = me.ready && me.ready();
	me.state = GuestThread::State::running;
	if (traced) {
		const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
		if (waited >= traced_from)
		{
			static const auto started = began;
			std::fprintf(stderr, "raven: at %lld ms thread %u waited %lld ms in %s (%s)\n",
				(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), me.id,
				(long long)waited, api, satisfied ? "woken" : "timed out");
		}
	}
	return satisfied;
}

void NativeThreads::serve()
{
	for (const auto &service : services)
		service();
	// RAVEN_DUMP_THREADS_ON names a file: whenever it is touched every process lists its threads with their state and the wait each
	// is in, and where it started. A program that stops answering shows which thread waits for what.
	static const char *trigger = std::getenv("RAVEN_DUMP_THREADS_ON");
	static auto checked = std::chrono::steady_clock::now();
	static timespec dumped{};
	if (!trigger || std::chrono::steady_clock::now() - checked < std::chrono::milliseconds(200))
		return;
	checked = std::chrono::steady_clock::now();
	struct stat touched{};
	if (::stat(trigger, &touched) != 0 || (touched.st_mtim.tv_sec == dumped.tv_sec && touched.st_mtim.tv_nsec == dumped.tv_nsec))
		return;
	dumped = touched.st_mtim;
	static constexpr const char *states[] = {"ready", "running", "waiting", "done"};
	for (const auto &thread : threads) {
		std::fprintf(stderr, "raven: thread %u %s", thread->id, states[size_t(thread->state)]);
		if (thread->state == GuestThread::State::waiting && thread->waiting_in)
			std::fprintf(stderr, " in %s for %lld ms", thread->waiting_in,
				(long long)std::chrono::duration_cast<std::chrono::milliseconds>(checked - thread->waiting_since).count());
		// The one that runs is inside the call the dump was asked from.
		if (thread->state == GuestThread::State::running)
			std::fprintf(stderr, " in %s", api_in_progress);
		if (thread->suspend_count)
			std::fprintf(stderr, " suspended %d", thread->suspend_count);
		std::fprintf(stderr, " rip %llx rsp %llx start %llx\n", (unsigned long long)thread->snapshot.context.registers.rip,
			(unsigned long long)thread->snapshot.context.registers.rsp, (unsigned long long)thread->start);
	}
}

bool NativeThreads::yield()
{
	// Each thread has a processor of its own then: there is nothing to give.
	if (threaded)
		return false;
	serve();
	GuestThread *next = pick(false).get();
	if (!next)
		return false;
	running->state = GuestThread::State::ready;
	switch_to(*next);
	running->state = GuestThread::State::running;
	return true;
}

// On Windows a thread whose wait ends wakes at once on another processor while the thread that ended it goes on. Programs count on it
// having moved on: a dialog that a worker closes through the window's thread asks whether the worker is still alive, and on Windows it
// has ended by then. Here the woken thread runs first, until it waits again or its time slice ends.
bool NativeThreads::hand_over()
{
	if (tearing_down || threaded)
		return false;
	for (const auto &thread : threads) {
		if (thread.get() == running || thread->state != GuestThread::State::waiting || thread->suspend_count > 0)
			continue;
		if (thread->can_go_on()) {
			running->state = GuestThread::State::ready;
			switch_to(*thread);
			running->state = GuestThread::State::running;
			return true;
		}
	}
	return false;
}

// The nanoseconds a task has run on a processor, from the scheduler's own count; zero when it cannot be read (the task is gone).
static uint64_t run_time(const char *path)
{
	const int file = ::open(path, O_RDONLY | O_CLOEXEC);
	if (file < 0)
		return 0;
	char text[64];
	const auto length = ::read(file, text, sizeof text - 1);
	::close(file);
	if (length <= 0)
		return 0;
	text[length] = 0;
	return std::strtoull(text, nullptr, 10);
}

// What a guest thread has run: its guest code in its task and the runtime's part in its host thread.
static uint64_t thread_run_time(pid_t host, pid_t task)
{
	char path[64];
	uint64_t total = 0;
	if (host > 0) {
		std::snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", int(host));
		total += run_time(path);
	}
	if (task > 0) {
		std::snprintf(path, sizeof path, "/proc/%d/schedstat", int(task));
		total += run_time(path);
	}
	return total;
}

// Whether the task of a stat file stands ready for a processor, or runs.
static bool runnable_at(const char *path)
{
	char text[256];
	const int file = ::open(path, O_RDONLY | O_CLOEXEC);
	if (file < 0)
		return false;
	const auto length = ::read(file, text, sizeof text - 1);
	::close(file);
	if (length <= 0)
		return false;
	text[length] = 0;
	const char *state = std::strrchr(text, ')');
	return state && state[1] == ' ' && state[2] == 'R';
}

// Whether a woken thread has not yet had its turn only for want of a processor: its host thread, or its guest task, which a wait
// outside the runtime wakes by itself while the host thread sleeps, stands ready or runs.
static bool runnable(pid_t host, pid_t task)
{
	char path[64];
	if (host > 0) {
		std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", int(host));
		if (runnable_at(path))
			return true;
	}
	if (task > 0) {
		std::snprintf(path, sizeof path, "/proc/%d/stat", int(task));
		if (runnable_at(path))
			return true;
	}
	return false;
}

std::vector<NativeThreads::Waiter> NativeThreads::waiting_for(uint32_t object)
{
	std::vector<Waiter> found;
	if (!object || table_waiters.load(std::memory_order_acquire) <= 0)
		return found;
	std::lock_guard lock(gate);
	for (const auto &thread : threads)
		if (thread.get() != running && thread->outside && thread->state == GuestThread::State::waiting &&
			std::find(thread->waits_on.begin(), thread->waits_on.end(), object) != thread->waits_on.end())
			found.push_back({thread.get(), thread->turns.load(std::memory_order_relaxed), thread->waits.load(std::memory_order_acquire)});
	return found;
}

bool NativeThreads::hand_over_to(const std::vector<Waiter> &waited)
{
	if (!threaded || tearing_down || !running)
		return false;
	GuestThread &me = *running;
	struct Woken {
		GuestThread *thread;
		uint64_t turns;
		std::chrono::steady_clock::time_point changed;
		uint64_t waits;
		// The processor time it had run when first read after its turns last changed, and after it woke.
		uint64_t ran = 0, woke = 0;
	};
	struct Busy {
		Woken *woken;
		pid_t host, task;
	};
	std::vector<Woken> waiting;
	const auto started = std::chrono::steady_clock::now();
	for (const auto &[thread, turns, waits] : waited)
		waiting.push_back({thread, turns, started, waits});
	// The waits that block in the runtime (those of a 32-bit guest, or on objects outside the table) are looked at as the serial
	// hand_over does, with the turn still held.
	for (const auto &thread : threads)
		if (thread.get() != &me && !thread->outside && thread->state == GuestThread::State::waiting && thread->suspend_count == 0 &&
			!thread->told_only && thread->ready && thread->ready())
			waiting.push_back({thread.get(), thread->turns.load(std::memory_order_relaxed), started, thread->waits.load(std::memory_order_acquire)});
	if (waiting.empty())
		return false;
	leave(me);
	// Those the set woke come back for their turn within microseconds; each then runs, as on Windows, until it waits again or ends, or
	// until it has run a millisecond of processor time without a call (ending a thread takes about that), or five in all, when the setter
	// goes on beside it. Processor time, not time on the clock: on a loaded machine a thread that is ending may wait for a processor for
	// longer than that, and the setter would go on before it ended. A wait on the clock of a tenth of a second bounds a thread that gets
	// no processor at all. One the set did not wake (an event reset on its own wakes one waiter) is not waited for. The setter sleeps
	// between looks: on two processors, spinning took the one the woken thread needed and made each round trip a hundred times slower.
	// Processor time is two files of /proc for each thread, so it is read only of a thread that has not waited again within a tenth of a
	// millisecond, no more often than that, and without the gate, which the woken thread takes to wait again.
	handing.fetch_add(1, std::memory_order_acq_rel);
	std::vector<Busy> busy;
	auto count_at = started + std::chrono::microseconds(100);
	for (;;) {
		const uint32_t seen = moves.load(std::memory_order_acquire);
		const auto now = std::chrono::steady_clock::now();
		bool any = false, settled = true;
		busy.clear();
		{
			std::lock_guard lock(gate);
			for (auto &woken : waiting) {
				const uint64_t turns = woken.thread->turns.load(std::memory_order_relaxed);
				if (turns == woken.turns && woken.changed == started)
					continue;
				any = true;
				if (turns != woken.turns) {
					woken.turns = turns;
					woken.changed = now;
					woken.ran = 0;
				}
				// A thread that woke is still in its old wait when it takes the turn, and its next call may be served in that same turn:
				// only a wait it began since is a new one.
				const bool waits = woken.thread->waits.load(std::memory_order_acquire) != woken.waits;
				if (woken.thread->state != GuestThread::State::done && !waits)
					busy.push_back({&woken, woken.thread->host_tid, woken.thread->snapshot.task});
			}
		}
		if (!busy.empty()) {
			settled = now >= count_at;
			if (settled) {
				for (const auto &[woken, host, task] : busy) {
					const uint64_t ran = thread_run_time(host, task);
					processor_times.fetch_add(1, std::memory_order_relaxed);
					if (!woken->ran)
						woken->ran = ran;
					if (!woken->woke)
						woken->woke = ran;
					// Where the scheduler keeps no count, the clock stands in for it.
					const bool spent = ran ? ran - woken->ran >= 1000000 || ran - woken->woke >= 5000000 :
						now - woken->changed >= std::chrono::milliseconds(1) || now - started >= std::chrono::milliseconds(5);
					if (!spent)
						settled = false;
				}
				count_at = now + std::chrono::microseconds(100);
			}
		}
		// None came yet: the set woke none of them, unless one stands woken and waits for a processor.
		const bool none = !any && now - started >= std::chrono::microseconds(300) &&
			std::none_of(waiting.begin(), waiting.end(), [](const Woken &woken) { return runnable(woken.thread->host_tid, woken.thread->snapshot.task); });
		if ((any && settled) || none || now - started >= std::chrono::milliseconds(100))
			break;
		const timespec nap{0, 100000};
		::syscall(SYS_futex, &moves, FUTEX_WAIT_PRIVATE, seen, &nap, nullptr, 0);
	}
	handing.fetch_sub(1, std::memory_order_acq_rel);
	{
		std::lock_guard lock(gate);
		enqueue(me);
	}
	wait_turn(me, std::nullopt);
	install(me);
	return true;
}

void NativeThreads::exit_current(uint32_t code)
{
	throw ThreadExit{code, true};
}

// A thread that ends by returning or by ExitThread runs the destructors of its fiber-local values, then tells the libraries, as Windows
// does: the .NET runtime learns that a thread is gone from its fiber-local destructor and stops tracking it there.
void NativeThreads::detach_current()
{
	if (exit_hook)
		exit_hook();
	if (notifier)
		notifier(3);
}

void NativeThreads::terminate(const std::shared_ptr<GuestThread> &thread, uint32_t code)
{
	// A thread ended from outside, even by itself, runs no destructors and tells no library.
	if (thread.get() == running)
		throw ThreadExit{code};
	finish(*thread, code);
}

void NativeThreads::terminate_others(uint32_t code)
{
	for (const auto &thread : threads)
		if (thread.get() != running && thread->state != GuestThread::State::done)
			finish(*thread, code);
}

void NativeThreads::unwind_all()
{
	if (!running || !running->main)
		return;
	if (threaded)
		return threaded_unwind();
	tearing_down = true;
	std::vector<std::shared_ptr<GuestThread>> remaining;
	for (const auto &thread : threads)
		if (!thread->main && thread->state != GuestThread::State::done) {
			thread->unwinding = true;
			thread->ready = nullptr;
			thread->wake.reset();
			thread->apcs.clear();
			thread->suspend_count = 0;
			thread->state = GuestThread::State::ready;
			remaining.push_back(thread);
		}
	for (const auto &thread : remaining)
		if (thread->state != GuestThread::State::done)
			switch_to(*thread);
	pending = nullptr;
}

void NativeThreads::resume(GuestThread &thread)
{
	if (thread.suspend_count > 0 && --thread.suspend_count == 0)
		raven_waits_resume(kernel_waits(), thread.id);
	if (threaded && thread.suspend_count == 0)
		resume_task_of(thread);
}

uint32_t NativeThreads::suspend(GuestThread &thread)
{
	if (thread.state == GuestThread::State::done)
		return UINT32_MAX;
	if (&thread != running && threaded)
		pause_task_of(thread);
	else if (&thread != running)
		process.pause_thread(thread.snapshot);
	const uint32_t previous = thread.suspend_count++;
	if (!previous)
		raven_waits_suspend(kernel_waits(), thread.id);
	if (&thread == running)
		block([&thread] { return thread.suspend_count == 0; }, std::nullopt);
	return previous;
}

}

namespace raven {

// ---- One host thread for each guest thread ----
//
// A parallel guest runs each of its threads in a Linux task of its own, and each gets a host thread that traces that task: it waits for
// the task's stops itself and serves its calls, so the guests and their stops go on at once on every processor. The runtime's state is
// not made for several threads at a time, so a host thread serves a call only while it holds the runtime: it gives it up when its guest
// runs on or it waits, and the threads that want it take it in turn. Work on a task (stopping it, reading or setting its registers) is
// done by the one host thread that traces it, at the request of the thread that holds the runtime.

void NativeThreads::enqueue(GuestThread &thread)
{
	if (!thread.queued && holder != &thread && !thread.outside) {
		// The thread that reads the user's input goes before those that only compute, as Windows favours the foreground thread: a window
		// that answers its input thousands of calls a frame would otherwise wait behind every worker's call for each of its own.
		if (thread.input_until > std::chrono::steady_clock::now()) {
			auto place = line.begin();
			while (place != line.end() && (*place)->input_until > std::chrono::steady_clock::now())
				++place;
			line.insert(place, &thread);
		} else {
			line.push_back(&thread);
		}
		lined.store(unsigned(line.size()), std::memory_order_relaxed);
		thread.queued = true;
		// A thread that went to the front is woken to wait for its turn awake (see wait_turn).
		if (line.front() == &thread)
			thread.turn.notify_one();
	}
	// The runtime goes to the first in line when nobody holds it.
	if (!holder && !line.empty()) {
		GuestThread *next = line.front();
		line.pop_front();
		lined.store(unsigned(line.size()), std::memory_order_relaxed);
		next->queued = false;
		holder = next;
		next->turn.notify_one();
		if (!line.empty())
			line.front()->turn.notify_one();
	}
}

uint32_t NativeThreads::own_id()
{
	return own_thread;
}

void NativeThreads::moved()
{
	moves.fetch_add(1, std::memory_order_release);
	if (handing.load(std::memory_order_acquire))
		::syscall(SYS_futex, &moves, FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
}

void NativeThreads::install(GuestThread &me)
{
	desktop_lock().resume(std::exchange(me.desktop_depth, 0u));
	me.turns.fetch_add(1, std::memory_order_relaxed);
	moved();
	running = &me;
	active_scheduler = this;
	// The process ended while this thread waited: nothing of it can be served any more. The main thread carries the error that ended it.
	if (process.gone()) {
		if (&me == &admin)
			return;
		if (!me.main)
			throw ThreadExit{0};
		check_pending();
		return;
	}
	// A thread ended by another while it waited has no task left to load its state into.
	if (&me != &admin && me.state == GuestThread::State::done)
		throw ThreadExit{me.exit_code};
	process.load_thread(me.snapshot);
	if (me.teb)
		environment.set_current(me.teb);
	// A host thread serves its own guest thread's calls beside the turn as well; under one host thread for all, the one it switched to.
	if (&me != &admin)
		NativeEnvironment::set_own(me.teb);
	if (&me == &admin)
		return;
	if (me.unwinding && !me.main)
		throw ThreadExit{0};
	if (me.state == GuestThread::State::done)
		throw ThreadExit{me.exit_code};
	check_pending();
}

void NativeThreads::announce(GuestThread &self, std::span<const uint32_t> objects)
{
	{
		std::lock_guard lock(gate);
		self.waits_on.assign(objects.begin(), objects.end());
	}
	table_waiters.fetch_add(1, std::memory_order_release);
	for (const uint32_t object : objects)
		table_waited[object % table_waited.size()].fetch_add(1, std::memory_order_acq_rel);
}

void NativeThreads::withdraw(GuestThread &self)
{
	// Only its own thread changes waits_on, so it reads them without the gate.
	for (const uint32_t object : self.waits_on)
		table_waited[object % table_waited.size()].fetch_sub(1, std::memory_order_release);
	table_waiters.fetch_sub(1, std::memory_order_release);
	std::lock_guard lock(gate);
	self.waits_on.clear();
}

std::optional<uint32_t> NativeThreads::wait_beside(std::span<const uint32_t> objects, bool all, uint32_t milliseconds)
{
	GuestThread *const me = parked_here;
	if (!me) {
		beside_wanted = true;
		return std::nullopt;
	}
	if (!waits_outside() || objects.empty())
		return std::nullopt;
	std::optional<std::chrono::steady_clock::time_point> until;
	if (milliseconds != UINT32_MAX)
		until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	// The main thread waits no longer than the run may last: past it, the usual way waits again and ends the run. The wait still counts
	// as endless for the check for a deadlock when the program asked for no time-out.
	const bool endless = !until;
	const bool limited = me->main && (!until || deadline < *until);
	if (limited)
		until = deadline;
	{
		std::lock_guard lock(gate);
		if (me->unwinding || me->state == GuestThread::State::done || me->paused || me->job || tearing_down)
			return std::nullopt;
		// As for a wait outside the runtime: another thread calls it away from the table for a job or its end (see wait_outside), and
		// the check for a deadlock counts it.
		me->outside = true;
		me->outside_endless = endless;
		// It waits as one in wait_outside does, so a setter handing over (hand_over_to) finds it and waits for it to go on.
		me->state = GuestThread::State::waiting;
		me->waits.fetch_add(1, std::memory_order_release);
	}
	announce(*me, objects);
	// Going to wait counts as a move, as for a wait outside; waking does not, so a setter handing over waits for the guest to go on.
	moved();
	uint32_t result = raven_wait_interrupted;
	for (;;) {
		uint64_t timeout = raven_wait_infinite;
		if (until)
			timeout = uint64_t(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(*until - std::chrono::steady_clock::now()).count()));
		result = raven_waits_wait(kernel_waits(), me->id, objects.data(), uint32_t(objects.size()), all, 0, timeout);
		if (result != raven_wait_interrupted)
			break;
		std::unique_lock lock(gate);
		if (!me->job)
			break;
		run_job(*me, lock);
		if (me->unwinding || me->state == GuestThread::State::done)
			break;
	}
	withdraw(*me);
	{
		std::lock_guard lock(gate);
		me->outside = false;
		if (me->state == GuestThread::State::waiting)
			me->state = GuestThread::State::running;
	}
	// Woken, it runs again as if it had taken a turn, which a setter handing over finds at its next look. It is not woken to see so: it
	// could only sleep again until this thread waits again or ends, which wake it, and would take the processor this thread needs.
	me->turns.fetch_add(1, std::memory_order_relaxed);
	// Interrupted for anything but a job, or failed, the wait took nothing: the usual way waits again and finds out why.
	if (result == raven_wait_interrupted || result == raven_wait_failed || result == raven_wait_ended || (limited && result == 0x102))
		return std::nullopt;
	return result;
}

// Takes what a wait on objects other processes share can take, or sleeps on their words until one of them changes, the host calls the
// thread away (wait_outside waits again after a job) or `until` passes. A pulse reaches such a wait only if it looks before the event
// is reset, as it does in another process.
uint32_t NativeThreads::sleep_on_words(GuestThread &me, const std::vector<std::shared_ptr<KernelObject>> &held,
	const std::vector<std::atomic<uint32_t> *> &words, bool all, std::optional<std::chrono::steady_clock::time_point> until)
{
	std::vector<uint32_t> seen(words.size());
	for (;;) {
		const uint32_t calls = me.calls.load();
		if (calls != me.calls_seen) {
			me.calls_seen = calls;
			return raven_wait_interrupted;
		}
		for (size_t index = 0; index < words.size(); ++index)
			seen[index] = words[index]->load();
		if (all) {
			if (std::all_of(held.begin(), held.end(), [&](const auto &object) { return object->signaled(me.id) == 1; })) {
				bool taken = true;
				for (const auto &object : held)
					taken = object->try_consume(me.id) && taken;
				if (taken)
					return 0;
			}
		} else {
			for (size_t index = 0; index < held.size(); ++index)
				if (held[index]->signaled(me.id) == 1 && held[index]->try_consume(me.id))
					return uint32_t(index);
		}
		timespec deadline{};
		if (until) {
			const auto left = until->time_since_epoch();
			if (std::chrono::steady_clock::now() >= *until)
				return 0x102;
			deadline.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(left).count();
			deadline.tv_nsec = (std::chrono::duration_cast<std::chrono::nanoseconds>(left) % std::chrono::seconds(1)).count();
		}
		wait_words(words, seen, me.calls, calls, until ? &deadline : nullptr);
	}
}

// Whether the kernel can sleep on several words at once (futex_waitv, Linux 5.16): an empty list is refused as invalid where it can.
bool NativeThreads::can_wait_words()
{
	static const bool supported = ::syscall(SYS_futex_waitv, nullptr, 0, 0, nullptr, CLOCK_MONOTONIC) < 0 && errno != ENOSYS;
	return supported;
}

std::optional<uint32_t> NativeThreads::wait_shared_beside(const std::vector<std::shared_ptr<KernelObject>> &held,
	const std::vector<std::atomic<uint32_t> *> &words, bool all, uint32_t milliseconds)
{
	GuestThread *const me = parked_here;
	if (!me) {
		beside_wanted = true;
		return std::nullopt;
	}
	if (!waits_outside() || held.empty() || held.size() != words.size() || !can_wait_words())
		return std::nullopt;
	std::optional<std::chrono::steady_clock::time_point> until;
	if (milliseconds != UINT32_MAX)
		until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	// As in wait_beside: the main thread waits no longer than the run may last, and a wait without a time-out counts as endless.
	const bool endless = !until;
	const bool limited = me->main && (!until || deadline < *until);
	if (limited)
		until = deadline;
	{
		std::lock_guard lock(gate);
		if (me->unwinding || me->state == GuestThread::State::done || me->paused || me->job || tearing_down)
			return std::nullopt;
		me->outside = true;
		me->outside_endless = endless;
		me->state = GuestThread::State::waiting;
		me->waits.fetch_add(1, std::memory_order_release);
	}
	moved();
	uint32_t result = raven_wait_interrupted;
	for (;;) {
		result = sleep_on_words(*me, held, words, all, until);
		if (result != raven_wait_interrupted)
			break;
		std::unique_lock lock(gate);
		if (!me->job)
			break;
		run_job(*me, lock);
		if (me->unwinding || me->state == GuestThread::State::done)
			break;
	}
	{
		std::lock_guard lock(gate);
		me->outside = false;
		if (me->state == GuestThread::State::waiting)
			me->state = GuestThread::State::running;
	}
	me->turns.fetch_add(1, std::memory_order_relaxed);
	moved();
	// Called away for anything but a job, the wait took nothing: the usual way waits again and finds out why (an APC, a suspension, the
	// thread's end). A time-out of the main thread's limit is the run's, which the usual way ends.
	if (result == raven_wait_interrupted || (limited && result == 0x102))
		return std::nullopt;
	return result;
}

void NativeThreads::tell(GuestThread &thread)
{
	if (!threaded)
		return;
	std::lock_guard lock(gate);
	if (thread.state != GuestThread::State::waiting || !thread.told_only)
		return;
	if (holder == &thread)
		thread.told_holding = true;
	else
		enqueue(thread);
}

void NativeThreads::wake_ready(GuestThread *except, bool all)
{
	// A program keeps a hundred threads waiting, and their conditions took most of the time a call was answered in when each was
	// looked at here every time. One is looked at again only after something that may have ended a wait, unless the guest changes what
	// it reads by itself; every millisecond they are all looked at whatever was told, for what changes without a call: a descriptor
	// that became ready, another process, time. Between those times only a few dozen are looked at each time, in turn, so that
	// a program with hundreds of waiting threads whose every call may end a wait does not pay for all of them at each call.
	const auto now = std::chrono::steady_clock::now();
	const uint64_t told = signals.load(std::memory_order_relaxed);
	if (now - scanned >= std::chrono::milliseconds(1))
		all = true;
	if (all)
		scanned = now;
	unsigned looks = 32;
	const size_t count = threads.size(), first = count ? scan_from % count : 0;
	std::vector<GuestThread *> due;
	for (size_t step = 0; step < count; ++step) {
		GuestThread *candidate = threads[(first + step) % count].get();
		if (candidate == except || candidate->queued || candidate->parked || candidate->suspend_count > 0)
			continue;
		if (candidate->state == GuestThread::State::waiting) {
			// A condition that cannot be looked at from here is looked at by its own thread, which then sees why.
			bool ready = candidate->wake && now >= *candidate->wake;
			// A wait its waker lines up (see tell) is left out of the frequent looks, not of the whole one each millisecond: a tell that came
			// in a gap of the hand-over costs that millisecond instead of the thread.
			if (!ready && candidate->ready && (!candidate->told_only || all) && (all || candidate->polled || (candidate->seen_signals != told && looks))) {
				if (!all && !candidate->polled && !--looks)
					scan_from = first + step + 1;
				ready = true;
				try {
					ready = candidate->ready();
					// It goes in line for its condition alone, which is looked at again when its turn comes (see leave).
					candidate->conditional = ready;
				} catch (...) {
				}
				if (!ready)
					candidate->seen_signals = told;
			}
			if (ready)
				due.push_back(candidate);
		} else if (candidate->state == GuestThread::State::ready && candidate->hosted && candidate->host_tid > 0) {
			due.push_back(candidate);
		}
	}
	if (due.empty())
		return;
	std::lock_guard lock(gate);
	for (auto *thread : due)
		enqueue(*thread);
}

void NativeThreads::leave(GuestThread &me, bool save)
{
	// The desktop lock is only ever taken after the runtime: whoever gives the runtime up gives the lock up with it.
	if (const unsigned held = desktop_lock().suspend())
		me.desktop_depth = held;
	const auto now = std::chrono::steady_clock::now();
	if (save && !process.gone())
		me.snapshot = process.save_thread(false);
	if (std::exchange(told, false))
		signaled();
	// The threads whose waits ended meanwhile join the line before the runtime is given on. While others wait in line for the runtime
	// they are not looked for again but once a millisecond: looking took more of the runtime than the calls it served, and the threads
	// in line keep it busy until then anyway. The runtime looks when it is left with nobody in line.
	if (!lined.load(std::memory_order_relaxed) || now - leave_scanned >= std::chrono::milliseconds(1)) {
		leave_scanned = now;
		wake_ready(&me);
	}
	for (;;) {
		GuestThread *next = nullptr;
		{
			std::lock_guard lock(gate);
			if (holder != &me)
				return;
			// Told while it still held the runtime, the thread goes in line behind those already there.
			if (std::exchange(me.told_holding, false) && !me.queued) {
				line.push_back(&me);
				lined.store(unsigned(line.size()), std::memory_order_relaxed);
				me.queued = true;
			}
			if (line.empty()) {
				holder = nullptr;
				return;
			}
			next = line.front();
		}
		// What the first in line waited for may be gone again: many threads wait for a lock one of them then takes, and each would be
		// woken to find so. Its condition is looked at once more before its host thread is woken for nothing.
		bool waits_on = false;
		if (std::exchange(next->conditional, false) && next->state == GuestThread::State::waiting && next->ready && !next->unwinding &&
			!(next->wake && std::chrono::steady_clock::now() >= *next->wake)) {
			try {
				waits_on = !next->ready();
			} catch (...) {
			}
		}
		std::lock_guard lock(gate);
		if (line.empty() || line.front() != next)
			continue;
		line.pop_front();
		lined.store(unsigned(line.size()), std::memory_order_relaxed);
		next->queued = false;
		if (waits_on) {
			next->seen_signals = signals.load(std::memory_order_relaxed);
			continue;
		}
		holder = next;
		next->turn.notify_one();
		if (!line.empty())
			line.front()->turn.notify_one();
		return;
	}
}

void NativeThreads::run_job(GuestThread &me, std::unique_lock<std::mutex> &lock)
{
	auto work = std::move(me.job);
	me.job = nullptr;
	lock.unlock();
	try {
		work();
	} catch (...) {
		// A task that ended meanwhile cannot be worked on; whoever asked sees the thread as it is.
	}
	lock.lock();
	me.job_done = true;
	jobs_done.notify_all();
}

void NativeThreads::send_job(GuestThread &thread, std::function<void()> work)
{
	if (thread.host_tid <= 0 || thread.host_tid == pid_t(::gettid())) {
		work();
		return;
	}
	{
		std::lock_guard lock(gate);
		thread.job = std::move(work);
		thread.job_done = false;
		thread.turn.notify_one();
	}
	call_away(thread);
	// A host thread that waits for its guest is asleep in the kernel: the interrupt wakes it to look at the job.
	NativeProcess::interrupt_host(thread.host_tid);
	std::unique_lock lock(gate);
	while (!thread.job_done) {
		if (jobs_done.wait_for(lock, std::chrono::milliseconds(50)) == std::cv_status::timeout && !thread.job_done && thread.job)
			NativeProcess::interrupt_host(thread.host_tid);
	}
}

void NativeThreads::wait_turn(GuestThread &me, std::optional<std::chrono::steady_clock::time_point> wake)
{
	std::unique_lock lock(gate);
	for (;;) {
		if (holder == &me) {
			me.conditional = false;
			return;
		}
		// The runtime goes from thread to thread thousands of times a second, mostly for a few microseconds each: waking a sleeping host
		// thread for its turn costs more than the turn. The first in line is woken as the turn before it begins (see enqueue, leave) and waits
		// for its own awake, briefly.
		if (me.queued && !me.job && !line.empty() && line.front() == &me) {
			lock.unlock();
			const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
			for (unsigned looks = 0; holder.load(std::memory_order_acquire) != &me; ++looks) {
				__builtin_ia32_pause();
				if ((looks & 63) == 63 && std::chrono::steady_clock::now() >= until)
					break;
			}
			lock.lock();
			if (holder == &me) {
				me.conditional = false;
				return;
			}
		}
		if (me.job) {
			run_job(me, lock);
			continue;
		}
		if (wake && !me.paused && std::chrono::steady_clock::now() >= *wake) {
			// The wait timed out: the thread takes its turn to see so.
			enqueue(me);
			wake.reset();
			continue;
		}
		if (wake)
			me.turn.wait_until(lock, *wake);
		else
			me.turn.wait(lock);
	}
}

void NativeThreads::enter(GuestThread &me)
{
	{
		std::lock_guard lock(gate);
		enqueue(me);
	}
	wait_turn(me, std::nullopt);
	install(me);
}

int NativeThreads::park(pid_t task, std::chrono::steady_clock::time_point limit, const uint64_t *block)
{
	GuestThread &me = *running;
	if (std::exchange(told, false))
		signaled();
	// A guest that calls again at once finds its host thread still with the runtime: giving the runtime up and taking it back costs more
	// than most calls. It is kept for a few microseconds only, and only while no other thread waits for it.
	if (block) {
		const uint32_t *const stop = agent::stop_word(task);
		unsigned looks = 0;
		for (; looks < 256 && !lined.load(std::memory_order_relaxed); ++looks) {
			if (__atomic_load_n(&block[0], __ATOMIC_ACQUIRE)) {
				// One that needs nothing of the runtime is answered here and the guest goes on; one that has to block parks first.
				beside_wanted = false;
				if (process.serve_alone(const_cast<uint64_t *>(block))) {
					looks = 0;
					continue;
				}
				if (beside_wanted)
					break;
				told = true;
				// The call is seen with the turn kept: nothing was waited for.
				CallCycles::instance().waited_turn(0);
				return -4;
			}
			// A guest that stopped leaves no call in its block: it is waited for, which finds the stop at once.
			if (stop && __atomic_load_n(stop, __ATOMIC_ACQUIRE) == agent::posted)
				break;
			__builtin_ia32_pause();
		}
		if (looks == 256 && stop && __atomic_load_n(stop, __ATOMIC_ACQUIRE) == agent::posted)
			late_stops.fetch_add(1, std::memory_order_relaxed);
	}
	{
		std::lock_guard lock(gate);
		me.parked = true;
	}
	leave(me);
	int status = 0;
	struct Parked {
		explicit Parked(GuestThread &thread) { parked_here = &thread; }
		~Parked() { parked_here = nullptr; }
	};
	for (;;) {
		const Parked parked(me);
		status = NativeProcess::wait_task(task, limit, [&] {
			std::lock_guard lock(gate);
			return bool(me.job);
		}, block);
		// A call that needs nothing of the runtime is answered here, outside its turn, and the guest goes on, whether it still spins in
		// its stub or stopped there.
		auto *const served = const_cast<uint64_t *>(block);
		if (status == -4 ? process.serve_alone(served) : process.serve_stopped_alone(task, status, served))
			continue;
		if (status != -3)
			break;
		std::unique_lock lock(gate);
		if (me.job)
			run_job(me, lock);
		// A pause holds the guest stopped: what it stopped at, its own stop or the pause, is what the thread goes on with.
		if (me.paused || me.held != -2) {
			status = me.held;
			me.held = -2;
			break;
		}
		if (me.state == GuestThread::State::done)
			break;
	}
	{
		std::lock_guard lock(gate);
		me.parked = false;
	}
	for (;;) {
		{
			// A suspended thread stays at its stop until it is resumed or ended.
			std::unique_lock lock(gate);
			while (me.paused && me.state != GuestThread::State::done && !me.unwinding) {
				if (me.job)
					run_job(me, lock);
				else
					me.turn.wait(lock);
			}
		}
		const uint64_t asked = __rdtsc();
		enter(me);
		// RAVEN_CYCLES counts the wait for the turn against the call that ended the park; a stop of any other kind has none.
		CallCycles::instance().waited_turn(status == -4 ? __rdtsc() - asked : 0);
		if (!me.paused || me.state == GuestThread::State::done || me.unwinding)
			break;
		// It was suspended while it waited for its turn.
		leave(me, false);
	}
	told = true;
	return status;
}

void NativeThreads::pause_task_of(GuestThread &thread)
{
	if (thread.paused || thread.host_tid <= 0)
		return;
	// Only the host thread knows for sure whether its guest runs: it stops one that runs and keeps it stopped, and reads the context of
	// one that stopped already, which waits for its turn and stays at its call while it is suspended (see park).
	send_job(thread, [&thread] {
		const pid_t task = thread.snapshot.task;
		if (thread.parked)
			thread.held = NativeProcess::pause_own_task(task);
		else
			// A guest that waits in a stub for its call's answer runs, though it goes nowhere: it is stopped where it is.
			NativeProcess::hold_call(task);
		thread.paused = true;
		thread.snapshot.context = NativeProcess::task_context(task);
	});
}

void NativeThreads::resume_task_of(GuestThread &thread)
{
	if (!thread.paused)
		return;
	std::lock_guard lock(gate);
	thread.paused = false;
	thread.turn.notify_one();
}

void NativeThreads::set_registers(GuestThread &thread, const user_regs_struct &registers)
{
	thread.snapshot.context.registers = registers;
	if (!threaded || &thread == running)
		return process.set_thread_registers(thread.snapshot, registers);
	// The registers live in the task while it is paused or its state was left there; its host thread sets them.
	if (thread.paused || thread.snapshot.context_in_task)
		send_job(thread, [&thread, registers] { NativeProcess::set_task_registers(thread.snapshot.task, registers); });
}

void NativeThreads::set_xstate(GuestThread &thread)
{
	if (!threaded || &thread == running || !(thread.paused || thread.snapshot.context_in_task) || thread.snapshot.context.xstate.size() < 512)
		return;
	send_job(thread, [&thread] { NativeProcess::set_task_xstate(thread.snapshot.task, thread.snapshot.context.xstate); });
}

void NativeThreads::end_host(GuestThread &thread)
{
	// The guest task of a thread that ended goes with it: its host thread is the one that can reap it.
	const pid_t task = thread.snapshot.task;
	if (task > 0) {
		::kill(task, SIGKILL);
		int status = 0;
		while (agent::wait(task, &status, __WALL) == -1 && errno == EINTR) {}
		thread.snapshot.task = 0;
	}
}

void NativeThreads::host_main(GuestThread &thread)
{
	thread.host_tid = pid_t(::gettid());
	own_thread = thread.id;
	NativeProcess::arm_host_thread();
	bool adopted = true;
	try {
		NativeProcess::adopt_task(thread.snapshot.task);
	} catch (const std::runtime_error &) {
		// A thread ended before its host thread took its task, as the threads ExitProcess ends right after one was created: it has
		// nothing left to run, and its host thread only cleans up after it.
		if (thread.state != GuestThread::State::done)
			throw;
		adopted = false;
	}
	if (adopted) {
		// A thread created suspended starts once it is resumed: until then nothing puts it in line.
		wait_turn(thread, std::nullopt);
		try {
			install(thread);
			thread.state = GuestThread::State::running;
			thread_main(thread);
		} catch (const ThreadExit &) {
			if (thread.state != GuestThread::State::done)
				finish(thread, thread.exit_code);
		} catch (...) {
			if (!pending)
				pending = std::current_exception();
			if (thread.state != GuestThread::State::done)
				finish(thread, 1);
		}
	}
	// The runtime is given on while the task is still there: the waits it ends (for this thread, say) are looked at through it.
	if (holder == &thread)
		leave(thread, false);
	end_host(thread);
	std::lock_guard lock(gate);
	if (host_end)
		host_end();
	// Work asked of this thread that it did not get to is over with it.
	if (thread.job) {
		thread.job = nullptr;
		thread.job_done = true;
		jobs_done.notify_all();
	}
	thread.host_tid = -1;
}

void NativeThreads::on_host(pid_t host, std::function<void()> work)
{
	GuestThread *target = nullptr;
	{
		std::lock_guard lock(gate);
		for (const auto &thread : threads)
			if (thread->host_tid == host)
				target = thread.get();
	}
	if (target)
		send_job(*target, std::move(work));
}

void NativeThreads::set_host_end(std::function<void()> end)
{
	std::lock_guard lock(gate);
	host_end = std::move(end);
}

void NativeThreads::admin_main()
{
	admin.host_tid = pid_t(::gettid());
	NativeProcess::arm_host_thread();
	NativeProcess::adopt_task(admin.snapshot.task);
	// Requests of other processes (DuplicateHandle into this one, its memory) and the host I/O that pending operations wait for are
	// answered by serve() and the pumps: their descriptors wake this thread. They are gathered in its turn, where they change. One
	// still ready at two gathers in a row, with the pumps run between them, is one nothing here finishes (closed, hung up, or ready for
	// what no operation asked): it is left out until a gather finds it quiet, or it would wake this thread again and again.
	std::vector<int> ready_before;
	// The set this thread sleeps in, made in its turn from what it gathered there. A poll would hold every file it waits on open, so a
	// socket the guest closed meanwhile would keep its bound address until the sleep ended; the kernel takes a released file out of an
	// epoll set by itself. Without a set the thread sleeps on `wake` alone.
	int sleep_set = -1;
	for (;;) {
		auto now = std::chrono::steady_clock::now();
		auto until = std::min(admin_due, now + std::chrono::milliseconds(20));
		{
			// The administrative thread works only when no guest thread does the host's work for it.
			const auto left = std::chrono::ceil<std::chrono::milliseconds>(until > now ? until - now : std::chrono::steady_clock::duration::zero());
			if (sleep_set < 0) {
				pollfd only{wake, POLLIN, 0};
				::poll(&only, 1, int(left.count()));
			} else {
				epoll_event ready[8];
				::epoll_wait(sleep_set, ready, 8, int(left.count()));
				::close(sleep_set);
				sleep_set = -1;
			}
		}
		uint64_t drained = 0;
		while (::read(wake, &drained, sizeof drained) > 0) {}
		{
			std::unique_lock lock(gate);
			if (stopping) {
				// The memory task outlives this thread: a task whose tracer ends is killed with it.
				try {
					process.hand_task(admin.snapshot.task);
				} catch (const std::exception &) {
				}
				break;
			}
			if (admin.job)
				run_job(admin, lock);
		}
		enter(admin);
		try {
			if (process.gone())
				throw ThreadExit{0};
			admin_sources.clear();
			serve();
			for (const auto &pump : pumps)
				pump();
			std::vector<pollfd> sources;
			for (const int channel : handle_descriptors())
				sources.push_back({channel, POLLIN, 0});
			if (watched)
				watched(sources);
			::poll(sources.data(), sources.size(), 0);
			std::vector<int> ready;
			for (const auto &source : sources)
				if (source.revents)
					ready.push_back(source.fd);
			std::erase_if(sources, [&](const pollfd &source) {
				return source.revents && std::find(ready_before.begin(), ready_before.end(), source.fd) != ready_before.end();
			});
			ready_before = std::move(ready);
			admin_sources = std::move(sources);
			std::map<int, uint32_t> interest{{wake, EPOLLIN}};
			for (const auto &source : admin_sources)
				interest[source.fd] |= uint32_t(uint16_t(source.events));
			sleep_set = ::epoll_create1(EPOLL_CLOEXEC);
			for (const auto &[descriptor, events] : interest) {
				epoll_event event{};
				event.events = events;
				event.data.fd = descriptor;
				if (sleep_set >= 0)
					::epoll_ctl(sleep_set, EPOLL_CTL_ADD, descriptor, &event);
			}
			// When every thread waits without a timeout for what only another of them could give, and no other process, host source
			// or window system can end a wait either, the run is over: the main thread is told so.
			bool stuck = !tearing_down && !pending && !poll && !shared_object_count.load() &&
				!std::any_of(externals.begin(), externals.end(), [](const auto &external) { return external(); });
			bool waits = false;
			for (const auto &thread : threads) {
				if (!stuck || thread->state == GuestThread::State::done)
					continue;
				// A wait in the process's table is stuck while it stands in line there with no time-out of its own.
				if (const bool outside = (std::lock_guard(gate), thread->outside); outside && thread->state == GuestThread::State::waiting) {
					waits = true;
					stuck = thread->outside_endless && raven_waits_waiting(kernel_waits(), thread->id);
					continue;
				}
				if (thread->state != GuestThread::State::waiting || thread->wake || thread->queued || !thread->ready) {
					stuck = false;
					continue;
				}
				waits = true;
				try {
					stuck = !thread->ready();
				} catch (...) {
					stuck = false;
				}
			}
			if (stuck && waits) {
				pending = std::make_exception_ptr(std::runtime_error("native guest threads deadlocked: every thread waits without a timeout"));
				call_away(*threads.front());
				std::lock_guard lock(gate);
				enqueue(*threads.front());
			}
		} catch (const ThreadExit &) {
		} catch (...) {
			if (!pending)
				pending = std::current_exception();
		}
		admin_due = std::chrono::steady_clock::time_point::max();
		for (const auto &thread : threads)
			if (thread->state == GuestThread::State::waiting && thread->wake)
				admin_due = std::min(admin_due, *thread->wake);
		// What the services and pumps brought in, and what no call announces, is looked at for every waiting thread.
		scanned = leave_scanned = {};
		try {
			leave(admin);
		} catch (...) {
			if (!pending)
				pending = std::current_exception();
			leave(admin, false);
		}
	}
}

bool NativeThreads::threaded_block(std::function<bool()> &ready, std::optional<std::chrono::steady_clock::time_point> wake)
{
	auto &me = *running;
	// Another thread can take what this one waited for between its turn being due and the turn coming: the wait goes on then, as it
	// would on Windows, where a thread only returns from a wait whose object it got.
	for (;;) {
		leave(me);
		// The time the run may last ends a wait that would outlast it: the main thread wakes for it and ends the run.
		const bool limited = me.main && !tearing_down && (!wake || deadline < *wake);
		wait_turn(me, limited ? std::optional(deadline) : wake);
		install(me);
		// What the call does once its wait is over counts as the rest of it did.
		told = true;
		if (limited && std::chrono::steady_clock::now() >= deadline) {
			throw NativeResult{NativeStop::timeout, 0, 0, 0};
		}
		if (!ready || ready() || (wake && std::chrono::steady_clock::now() >= *wake))
			return true;
	}
}

uint32_t NativeThreads::wait_in_line(const std::vector<uint32_t> &table, bool all, bool alertable,
	std::optional<std::chrono::steady_clock::time_point> wake)
{
	auto &me = *running;
	if (threaded || me.granted)
		return raven_wait_unlined;
	static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
	std::atomic<uint32_t> told{0};
	const uint32_t lined = raven_waits_line_up(kernel_waits(), me.id, table.data(), uint32_t(table.size()), all, alertable,
		reinterpret_cast<uint32_t *>(&told));
	if (lined != raven_wait_lined)
		return lined;
	// The thread leaves the line however the block ends, an error or the end of the process included: the table keeps the word until then.
	struct Leave {
		GuestThread &thread;
		bool left = false;
		~Leave()
		{
			thread.granted = nullptr;
			if (!left)
				raven_waits_leave(kernel_waits(), thread.id);
		}
	} leave_line{me};
	me.granted = &told;
	lined_block = true;
	block([&told] { return told.load(std::memory_order_acquire) != 0; }, wake);
	me.granted = nullptr;
	leave_line.left = true;
	return raven_waits_leave(kernel_waits(), me.id);
}

uint32_t NativeThreads::wait_outside(const std::function<uint32_t(std::optional<std::chrono::steady_clock::time_point>)> &wait,
	std::optional<std::chrono::steady_clock::time_point> wake)
{
	auto &me = *running;
	// As in block: a wait inside another (a callback that waits) puts the outer one back when it ends.
	auto outer_ready = std::move(me.ready);
	const auto outer_wake = me.wake;
	const auto outer_state = me.state;
	me.ready = nullptr;
	me.wake.reset();
	me.state = GuestThread::State::waiting;
	me.waiting_in = api_in_progress;
	me.waiting_since = std::chrono::steady_clock::now();
	me.waits.fetch_add(1, std::memory_order_release);
	{
		std::lock_guard lock(gate);
		me.outside = true;
		me.outside_endless = !wake;
	}
	moved();
	leave(me);
	// The time the run may last ends a wait that would outlast it: the main thread wakes for it and ends the run.
	const bool limited = me.main && !tearing_down && (!wake || deadline < *wake);
	uint32_t result = raven_wait_interrupted;
	for (;;) {
		result = wait(limited ? std::optional(deadline) : wake);
		if (result != raven_wait_interrupted)
			break;
		// Called away for a job, the thread waits again after it; for anything else (its end, the end of the process, an error the main
		// thread must carry) it takes its turn and finds out there.
		std::unique_lock lock(gate);
		if (!me.job)
			break;
		run_job(me, lock);
		if (me.unwinding || me.state == GuestThread::State::done)
			break;
	}
	{
		std::lock_guard lock(gate);
		me.outside = false;
		enqueue(me);
	}
	wait_turn(me, std::nullopt);
	me.waiting_in = nullptr;
	const bool nested = outer_state == GuestThread::State::waiting && outer_ready;
	me.ready = nested ? std::move(outer_ready) : nullptr;
	me.wake = nested ? outer_wake : std::nullopt;
	if (me.state != GuestThread::State::done)
		me.state = nested ? GuestThread::State::waiting : GuestThread::State::running;
	install(me);
	// What the call does once its wait is over counts as the rest of it did.
	told = true;
	if (limited && std::chrono::steady_clock::now() >= deadline)
		throw NativeResult{NativeStop::timeout, 0, 0, 0};
	return result;
}

void NativeThreads::queue_apc(GuestThread &thread, GuestThread::Apc apc)
{
	thread.apcs.push_back(std::move(apc));
	raven_waits_queue_apc(kernel_waits(), thread.id);
	// A wait on shared objects has no table to tell: it looks at the APC in the runtime, and waits again if it is not alertable.
	thread.calls.fetch_add(1);
	::syscall(SYS_futex, &thread.calls, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

void NativeThreads::call_away(GuestThread &thread)
{
	raven_waits_interrupt(kernel_waits(), thread.id);
	thread.calls.fetch_add(1);
	::syscall(SYS_futex, &thread.calls, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

void NativeThreads::threaded_unwind()
{
	tearing_down = true;
	auto &me = *running;
	for (int round = 0; round < 5000; ++round) {
		bool left = false;
		for (const auto &thread : threads) {
			if (thread->main || thread->state == GuestThread::State::done || !thread->hosted)
				continue;
			left = true;
			thread->unwinding = true;
			thread->ready = nullptr;
			thread->wake.reset();
			thread->apcs.clear();
			thread->suspend_count = 0;
			call_away(*thread);
			// A thread whose guest runs leaves when the guest is gone; one that waits goes through its frames when its turn comes.
			if (thread->parked && thread->snapshot.task > 0)
				::kill(thread->snapshot.task, SIGKILL);
			{
				std::lock_guard lock(gate);
				thread->paused = false;
				thread->turn.notify_one();
			}
			thread->state = thread->state == GuestThread::State::waiting ? GuestThread::State::ready : thread->state;
		}
		if (!left)
			break;
		leave(me);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		{
			std::unique_lock lock(gate);
			enqueue(me);
		}
		wait_turn(me, std::nullopt);
		install(me);
	}
	pending = nullptr;
}

}
