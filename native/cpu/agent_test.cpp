#include "agent.h"
#include "agent_context.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <linux/futex.h>
#include <stdexcept>
#include <sys/mman.h>
#include <asm/prctl.h>
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

// Code below 4 GiB that switches to 32-bit compatibility mode, takes a breakpoint there with EAX 0x11, and comes back to 64-bit
// mode with EAX as the runtime left it.
static uint8_t *compat_code()
{
	auto *low = static_cast<uint8_t *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0));
	check(low != MAP_FAILED && uintptr_t(low) < 0x100000000ULL, "no memory below 4 GiB");
	const uint32_t base = uint32_t(uintptr_t(low));
	const auto put32 = [](uint8_t *at, uint32_t value) { std::memcpy(at, &value, 4); };
	uint8_t *p = low;
	// 64-bit: keep RSP, load the data segments, take a stack below 4 GiB and far-return into the 32-bit code segment.
	*p++ = 0x48; *p++ = 0x89; *p++ = 0x24; *p++ = 0x25; put32(p, base + 0x800); p += 4;
	*p++ = 0xb8; put32(p, 0x2b); p += 4;
	*p++ = 0x8e; *p++ = 0xd8;
	*p++ = 0x8e; *p++ = 0xc0;
	*p++ = 0xbc; put32(p, base + 0xff0); p += 4;
	*p++ = 0x6a; *p++ = 0x23;
	*p++ = 0x68; put32(p, base + 0x100); p += 4;
	*p++ = 0x48; *p++ = 0xcb;
	// 32-bit: EAX 0x11, the breakpoint, then a far return to the 64-bit code segment.
	p = low + 0x100;
	*p++ = 0xb8; put32(p, 0x11); p += 4;
	*p++ = 0xcc;
	*p++ = 0x6a; *p++ = 0x33;
	*p++ = 0x68; put32(p, base + 0x200); p += 4;
	*p++ = 0xcb;
	// 64-bit again: the saved RSP and back to the caller.
	p = low + 0x200;
	*p++ = 0x48; *p++ = 0x8b; *p++ = 0x24; *p++ = 0x25; put32(p, base + 0x800); p += 4;
	*p++ = 0xc3;
	check(mprotect(low, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0, "low code not executable");
	return low;
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

static void answer(int expected_signal, long expected_call, uint64_t rax, uint64_t r12_seen, uint64_t r12, uint16_t code_segment = 0x33)
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
	if (expected_call >= 0) {
		check(information->si_syscall == expected_call, "the turned-away call is not the one made");
		// The kernel rolls RAX back to the call's number and leaves RIP after the SYSCALL instruction.
		const auto *frame = reinterpret_cast<const ucontext_t *>(block[raven::agent::context]);
		const auto *after = reinterpret_cast<const uint8_t *>(frame->uc_mcontext.gregs[REG_RIP]);
		check(frame->uc_mcontext.gregs[REG_RAX] == expected_call, "a turned-away call left RAX other than its number");
		check(after[-2] == 0x0f && after[-1] == 0x05, "a turned-away call left RIP elsewhere than after its SYSCALL");
	}
	auto *context = reinterpret_cast<ucontext_t *>(block[raven::agent::context]);
	auto registers = raven::agent::registers(*context, segments, expected_call);
	check(registers.cs == code_segment, "the frame names another code segment than the one the thread ran in");
	check(registers.orig_rax == uint64_t(expected_call), "orig_rax does not name the turned-away call");
	check(registers.fs_base == segments.fs_base && registers.gs_base == segments.gs_base, "the segment bases were not kept");
	if (r12_seen)
		check(registers.r12 == r12_seen, "the registers read from the frame are not the thread's");
	registers.rax = rax;
	if (r12_seen) {
		registers.r12 = r12;
		// The frame has no segment bases: the handler sets the GS base the runtime asks for before the thread goes on.
		block[raven::agent::gs_base] = 0x5000;
		block[raven::agent::set_bases] = raven::agent::set_gs;
	}
	auto moved = registers;
	moved.gs_base = 0x3000;
	check(!raven::agent::set_registers(*context, moved, segments), "a frame took a new GS base");
	check(context->uc_mcontext.gregs[REG_RAX] != int64_t(rax), "a refused change was written");
	check(raven::agent::set_registers(*context, registers, segments), "the registers were not put back");
	__atomic_store_n(state(), raven::agent::answered, __ATOMIC_RELEASE);
	syscall(SYS_futex, state(), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

// The runtime's side of the service thread: leave a call in its block, wake it and wait for the result.
static int64_t serve(uint64_t *service, long number, std::initializer_list<uint64_t> arguments)
{
	namespace word = raven::agent::service_block;
	auto *state = reinterpret_cast<uint32_t *>(&service[word::state]);
	service[word::number] = uint64_t(number);
	size_t index = word::arguments;
	for (const uint64_t argument : arguments)
		service[index++] = argument;
	__atomic_store_n(state, word::waiting, __ATOMIC_RELEASE);
	syscall(SYS_futex, state, FUTEX_WAKE, 1, nullptr, nullptr, 0);
	const timespec slice{0, 100'000'000};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != word::done) {
		if (std::chrono::steady_clock::now() >= deadline) {
			std::cerr << "the service thread never finished its call\n";
			_exit(1);
		}
		syscall(SYS_futex, state, FUTEX_WAIT, word::waiting, &slice, nullptr, 0);
	}
	__atomic_store_n(state, word::idle, __ATOMIC_RELAXED);
	return int64_t(service[word::result]);
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

		uint64_t from_breakpoint = 0, from_call = 0, from_compat = 0, kept = 0x1122, gs_after = 0;
		uint8_t *const compat = compat_code();
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
				syscall(SYS_arch_prctl, ARCH_GET_GS, &gs_after);
				syscall(SYS_arch_prctl, ARCH_SET_GS, 0UL);
				from_compat = reinterpret_cast<uint32_t (*)()>(compat)();
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
			answer(SIGTRAP, -1, 0x77, 0, 0, 0x23);
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
		check(gs_after == 0x5000, "the thread did not go on with the GS base the runtime asked for");
		check(block[raven::agent::set_bases] == 0, "the handler left the segment request set");
		check(from_compat == 0x77, "a 32-bit thread did not go on in 32-bit code with the runtime's EAX after its breakpoint");
		check(from_call == 4242, "the turned-away call did not return the runtime's answer");
		check(*state() == raven::agent::idle, "the agent left its state set");

		// The service thread makes the calls the runtime leaves it, inside the guest's own process.
		auto *service = static_cast<uint64_t *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
		check(service != MAP_FAILED, "no service block");
		using entry = void (*)(uint64_t *);
		std::thread(reinterpret_cast<entry>(agent_code + raven::agent::service()), service).detach();
		const int64_t mapped = serve(service, SYS_mmap, {0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, uint64_t(-1), 0});
		check(mapped > 0 && (mapped & 4095) == 0, "the service thread did not map memory");
		*reinterpret_cast<volatile uint64_t *>(mapped) = 0x55;
		check(serve(service, SYS_munmap, {uint64_t(mapped), 4096}) == 0, "the service thread did not release memory");
		check(serve(service, SYS_getpid, {}) == getpid(), "the service thread made its call in another process");
		check(serve(service, SYS_close, {uint64_t(-1)}) == -EBADF, "the service thread did not hand back the call's error");
		std::cout << "Agent breakpoint, Syscall User Dispatch and service-thread round trips over the shared blocks passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
