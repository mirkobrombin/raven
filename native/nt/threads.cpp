#include "threads.h"

#include <cstdlib>
#include <fcntl.h>
#include <iostream>

#include "api.h"
#include "shared_file.h"
#include "inherit.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace raven {

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
	process.set_time_slicing(std::chrono::milliseconds(slice < 0 ? 0 : slice), [this] { return !tearing_down && live() > 1; }, [this] { yield(); });
	wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (wake < 0)
		throw std::runtime_error("cannot create the scheduler's wake descriptor");
	shared_change_listener = wake;
	if (process.parallel()) {
		process.set_execution_wait([this](std::function<bool()> ready, std::chrono::steady_clock::time_point limit) {
			while (!ready() && std::chrono::steady_clock::now() < limit)
				block(ready, limit);
		});
	}
}

NativeThreads::~NativeThreads()
{
	if (shared_change_listener == wake)
		shared_change_listener = -1;
	process.set_time_slicing(std::chrono::milliseconds(0), nullptr, nullptr);
	// Host frames of threads that never finished hold objects that only their unwinding releases; a failure here must not escape.
	try {
		unwind_all();
	} catch (...) {
	}
	process.set_execution_wait(nullptr);
	::close(wake);
}

void NativeThreads::stop_time_slicing()
{
	process.set_time_slicing(std::chrono::milliseconds(0), nullptr, nullptr);
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

std::shared_ptr<GuestThread> NativeThreads::create(uint64_t start, uint64_t parameter, uint64_t stack_size, bool suspended)
{
	if (process.parallel())
		reap();
	if (live() >= 256)
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
		if (notifier)
			notifier(3);
	} catch (const ThreadExit &exit) {
		code = exit.code;
	} catch (...) {
		pending = std::current_exception();
		// RAVEN_TRACE_API names the thread and the error that ends the process from a thread other than the first.
		if (std::getenv("RAVEN_TRACE_API")) {
			try {
				std::rethrow_exception(pending);
			} catch (const std::exception &error) {
				std::cerr << "raven: guest thread " << thread.id << " failed in " << api_in_progress << ": " << error.what() << "\n";
			} catch (...) {
				std::cerr << "raven: guest thread " << thread.id << " ended with a stop\n";
			}
		}
	}
	finish(thread, code);
	// A finished coroutine never resumes; hand the processor on and stay parked. What the scheduler throws here (the process deadline)
	// is for the main thread, which receives it through the pending exception.
	for (;;) {
		try {
			schedule();
		} catch (...) {
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
	if (&thread != running) {
		process.stop_thread(thread.snapshot);
		thread.snapshot.task = 0;
	}
	thread.exit_code = code;
	thread.state = GuestThread::State::done;
	thread.impersonation.reset();
	thread.ready = nullptr;
	for (const int report : thread.exit_reports) {
		[[maybe_unused]] const auto written = ::write(report, &code, sizeof code);
		::close(report);
	}
	thread.exit_reports.clear();
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
	for (auto &thread : threads)
		if (thread->state == GuestThread::State::done && thread.get() != running && thread->host_stack) {
			thread->host_stack.reset();
			release_resources(*thread);
		}
}

std::shared_ptr<GuestThread> NativeThreads::pick(bool include_current)
{
	const auto now = std::chrono::steady_clock::now();
	const size_t count = threads.size();
	size_t start = 0;
	for (size_t index = 0; index < count; ++index)
		if (threads[index].get() == running)
			start = index;
	for (size_t step = 1; step <= count; ++step) {
		const auto &thread = threads[(start + step) % count];
		if (thread.get() == running && !include_current)
			continue;
		if (thread->state == GuestThread::State::done || thread->suspend_count > 0)
			continue;
		if (thread->state == GuestThread::State::ready)
			return thread;
		if (thread->state == GuestThread::State::waiting) {
			if ((thread->ready && thread->ready()) || (thread->wake && now >= *thread->wake))
				return thread;
		}
	}
	return nullptr;
}

void NativeThreads::check_pending()
{
	if (!pending || !running->main)
		return;
	const auto exception = pending;
	pending = nullptr;
	// The error that ended another thread ends the process; the call this thread was in, waiting or running, will never complete.
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

void NativeThreads::nudge()
{
	const uint64_t one = 1;
	[[maybe_unused]] const auto written = ::write(wake, &one, sizeof one);
}

bool NativeThreads::block(std::function<bool()> ready, std::optional<std::chrono::steady_clock::time_point> wake)
{
	if (ready())
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
	me.ready = std::move(ready);
	me.wake = wake;
	me.state = GuestThread::State::waiting;
	// The condition reads the waiting frame: it goes with the wait, also when an error or the end of the process leaves the frame.
	struct Leave {
		GuestThread &thread;
		~Leave()
		{
			thread.ready = nullptr;
			thread.wake.reset();
		}
	} leave{me};
	// RAVEN_TRACE_WAITS names the waits that last longer than a quarter of a second: the API and how long it kept its thread.
	static const bool traced = std::getenv("RAVEN_TRACE_WAITS") != nullptr;
	const auto began = std::chrono::steady_clock::now();
	const char *api = api_in_progress;
	schedule();
	const bool satisfied = me.ready && me.ready();
	me.state = GuestThread::State::running;
	if (traced) {
		const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
		if (waited >= 250)
		{
			static const auto started = began;
			std::fprintf(stderr, "raven: at %lld ms thread %u waited %lld ms in %s (%s)\n",
				(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), me.id,
				(long long)waited, api, satisfied ? "woken" : "timed out");
		}
	}
	return satisfied;
}

bool NativeThreads::yield()
{
	serve();
	GuestThread *next = pick(false).get();
	if (!next)
		return false;
	running->state = GuestThread::State::ready;
	switch_to(*next);
	running->state = GuestThread::State::running;
	return true;
}

void NativeThreads::exit_current(uint32_t code)
{
	throw ThreadExit{code};
}

void NativeThreads::terminate(const std::shared_ptr<GuestThread> &thread, uint32_t code)
{
	if (thread.get() == running)
		exit_current(code);
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
	if (thread.suspend_count > 0)
		--thread.suspend_count;
}

uint32_t NativeThreads::suspend(GuestThread &thread)
{
	if (thread.state == GuestThread::State::done)
		return UINT32_MAX;
	if (&thread != running)
		process.pause_thread(thread.snapshot);
	const uint32_t previous = thread.suspend_count++;
	if (&thread == running)
		block([&thread] { return thread.suspend_count == 0; }, std::nullopt);
	return previous;
}

}
