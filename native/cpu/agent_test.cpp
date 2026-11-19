#include "agent.h"
#include "agent_context.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <linux/futex.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>

// The agent without ptrace: a thread that takes a breakpoint and a system call Syscall User Dispatch turns away is stopped in its signal
// handler, the runtime reads the signal and changes the interrupted registers over the thread's block, and the thread goes on with them.
// Here one thread plays the guest and another the runtime, in one process.

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

// The kernel's own sigaction, which takes the restorer as given.
struct kernel_action {
	void *handler;
	unsigned long flags;
	void *restorer;
	uint64_t mask;
};

static uint64_t *block;
static uint8_t *agent_code;
static volatile uint8_t selector = SYSCALL_DISPATCH_FILTER_ALLOW;

static uint32_t *state() { return reinterpret_cast<uint32_t *>(&block[raven::agent::state]); }

static void install(int signal)
{
	kernel_action action{agent_code + raven::agent::handler(), SA_SIGINFO | SA_ONSTACK | 0x04000000 /* SA_RESTORER */,
		agent_code + raven::agent::restorer(), 0};
	check(syscall(SYS_rt_sigaction, signal, &action, nullptr, 8) == 0, "the agent's handler was refused");
}

// R12 carries a value across the breakpoint, which the runtime reads and replaces.
static uint64_t breakpoint(uint64_t &kept)
{
	uint64_t value = 0;
	asm volatile("mov %1, %%r12\n\tint3\n\tmov %%r12, %1" : "+a"(value), "+r"(kept) : : "r12", "memory");
	return value;
}

static uint64_t dispatched_call(uint64_t number)
{
	uint64_t value = number;
	selector = SYSCALL_DISPATCH_FILTER_BLOCK;
	asm volatile("syscall" : "+a"(value) : : "rcx", "r11", "memory");
	selector = SYSCALL_DISPATCH_FILTER_ALLOW;
	return value;
}

// The runtime's side: wait for the guest's signal, check it and answer with RAX changed.
static const raven::agent::Segments segments{0x1000, 0x2000, 0, 0};

static void answer(int expected_signal, long expected_call, uint64_t rax, uint64_t r12_seen, uint64_t r12)
{
	// The state may still read answered from the last round until the guest clears it: the wait then returns at once, so the deadline
	// counts time, not rounds.
	const timespec slice{0, 100'000'000};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (uint32_t now = __atomic_load_n(state(), __ATOMIC_ACQUIRE)) {
		if (now == raven::agent::posted)
			break;
		check(std::chrono::steady_clock::now() < deadline, "the agent never posted its signal");
		sched_yield();
	}
	while (__atomic_load_n(state(), __ATOMIC_ACQUIRE) != raven::agent::posted) {
		check(std::chrono::steady_clock::now() < deadline, "the agent never posted its signal");
		syscall(SYS_futex, state(), FUTEX_WAIT, raven::agent::idle, &slice, nullptr, 0);
	}
	check(int(block[raven::agent::signal]) == expected_signal, "the agent posted another signal");
	const auto *information = reinterpret_cast<const siginfo_t *>(block[raven::agent::information]);
	check(information->si_signo == expected_signal, "the agent's siginfo names another signal");
	if (expected_call >= 0)
		check(information->si_syscall == expected_call, "the turned-away call is not the one made");
	auto *context = reinterpret_cast<ucontext_t *>(block[raven::agent::context]);
	auto registers = raven::agent::registers(*context, segments, expected_call);
	check(registers.orig_rax == uint64_t(expected_call), "orig_rax does not name the turned-away call");
	check(registers.fs_base == segments.fs_base && registers.gs_base == segments.gs_base, "the segment bases were not kept");
	if (r12_seen)
		check(registers.r12 == r12_seen, "the registers read from the frame are not the thread's");
	registers.rax = rax;
	if (r12_seen)
		registers.r12 = r12;
	auto moved = registers;
	moved.gs_base = 0x3000;
	check(!raven::agent::set_registers(*context, moved, segments), "a frame took a new GS base");
	check(context->uc_mcontext.gregs[REG_RAX] != int64_t(rax), "a refused change was written");
	check(raven::agent::set_registers(*context, registers, segments), "the registers were not put back");
	__atomic_store_n(state(), raven::agent::answered, __ATOMIC_RELEASE);
	syscall(SYS_futex, state(), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

int main()
{
	try {
		block = static_cast<uint64_t *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
		check(block != MAP_FAILED, "no block");
		const auto code = raven::agent::code();
		agent_code = static_cast<uint8_t *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		check(agent_code != MAP_FAILED && code.size() <= 4096, "no agent page");
		std::memcpy(agent_code, code.data(), code.size());
		check(mprotect(agent_code, 4096, PROT_READ | PROT_EXEC) == 0, "agent page not executable");
		install(SIGTRAP);
		install(SIGSYS);

		uint64_t from_breakpoint = 0, from_call = 0, kept = 0x1122;
		std::string failure;
		std::thread guest([&] {
			try {
				// The alternate stack lies on a boundary of its size and names the block in its first word.
				auto *area = static_cast<uint8_t *>(mmap(nullptr, 2 * raven::agent::stack_size, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
				check(area != MAP_FAILED, "no alternate stack");
				auto *stack = reinterpret_cast<uint8_t *>((uintptr_t(area) + raven::agent::stack_size - 1) & ~(raven::agent::stack_size - 1));
				*reinterpret_cast<uint64_t **>(stack) = block;
				stack_t alternate{stack, 0, raven::agent::stack_size};
				check(sigaltstack(&alternate, nullptr) == 0, "alternate stack refused");
				from_breakpoint = breakpoint(kept);
				check(prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, uintptr_t(agent_code), code.size(), &selector) == 0,
					"Syscall User Dispatch refused");
				from_call = dispatched_call(SYS_getpid);
				prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_OFF, 0, 0, 0);
			} catch (const std::exception &error) {
				failure = error.what();
			}
		});
		try {
			answer(SIGTRAP, -1, 77, 0x1122, 0x3344);
			answer(SIGSYS, SYS_getpid, 4242, 0, 0);
		} catch (const std::exception &error) {
			// The guest may be held in its handler for good: end here rather than wait for it.
			std::cerr << error.what() << '\n';
			_exit(1);
		}
		guest.join();
		check(failure.empty(), failure.c_str());
		check(from_breakpoint == 77, "the thread did not go on with the runtime's RAX after its breakpoint");
		check(kept == 0x3344, "the thread did not go on with the runtime's R12 after its breakpoint");
		check(from_call == 4242, "the turned-away call did not return the runtime's answer");
		check(*state() == raven::agent::idle, "the agent left its state set");
		std::cout << "Agent breakpoint and Syscall User Dispatch round trips over the thread block passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
