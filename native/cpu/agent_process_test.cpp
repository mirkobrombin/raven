#include "native_cpu.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/user.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// The agent in a guest process: its service thread starts untraced inside the guest and makes the runtime's system calls there, while
// the guest's own task stays traced and runs code as before.

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

template<typename F> static void rejected(F operation, const char *message)
{
	try { operation(); } catch (const std::runtime_error &) { return; }
	throw std::runtime_error(message);
}

// A field of /proc/<task>/status, such as Tgid or TracerPid.
static long status_field(pid_t task, const std::string &name)
{
	std::ifstream status("/proc/" + std::to_string(task) + "/status");
	for (std::string line; std::getline(status, line);) {
		if (line.rfind(name + ":", 0) == 0)
			return std::stol(line.substr(name.size() + 1));
	}
	throw std::runtime_error("no " + name + " for the agent's task");
}

static void agent(const std::string &bootstrap, raven::NativeMode mode)
{
	// RAVEN_PTRACE keeps the guest traced, without an agent until one is started.
	setenv("RAVEN_PTRACE", "1", 1);
	const std::array<raven::LoadedImage *, 0> images{};
	raven::NativeProcess process(images, bootstrap, std::chrono::seconds(2), mode);
	unsetenv("RAVEN_PTRACE");
	rejected([&] { process.agent_syscall(SYS_getpid); }, "a call went to an agent that was not started");
	check(process.agent_task() == 0, "an agent task before the agent started");
	process.start_agent();
	const pid_t task = process.agent_task();
	check(task > 0, "the agent started no task");
	rejected([&] { process.start_agent(); }, "the agent started twice");

	// The service thread lives in the guest's thread group, untraced, beside the guest's own task, which RAVEN_PTRACE left traced.
	const long group = status_field(task, "Tgid");
	check(group != getpid(), "the service thread runs in the runtime");
	check(status_field(pid_t(group), "TracerPid") == getpid(), "RAVEN_PTRACE did not keep the guest's task traced");
	check(status_field(task, "TracerPid") == 0, "the service thread is traced");
	check(long(process.agent_syscall(SYS_getpid)) == group, "the service thread made its call in another process");
	check(long(process.agent_syscall(SYS_gettid)) == task, "another thread made the service thread's call");

	// Memory it maps is the guest's: the runtime reaches it there, and not once it is released.
	const uint64_t mapped = process.agent_syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, uint64_t(-1), 0);
	const uint64_t value = 0x5241564e;
	check(process.write_alone(mapped, &value, sizeof value), "the guest has no memory where the service thread mapped it");
	uint64_t read = 0;
	check(process.read_alone(mapped, &read, sizeof read) && read == value, "the guest's memory did not keep the value");
	check(process.agent_syscall(SYS_munmap, mapped, 4096) == 0, "the service thread did not release memory");
	check(!process.read_alone(mapped, &read, sizeof read), "memory the service thread released is still mapped in the guest");
	rejected([&] { process.agent_syscall(SYS_close, uint64_t(-1)); }, "an error of the service thread's call was lost");

	// The guest's own task still runs its code: return the argument plus one.
	const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE);
	const std::vector<uint8_t> x64{0x48, 0x8d, 0x41, 0x01, 0xc3};
	const std::vector<uint8_t> x86{0x8b, 0x44, 0x24, 0x04, 0x40, 0xc2, 0x04, 0x00};
	process.write_memory(code, mode == raven::NativeMode::x64 ? x64 : x86);
	process.protect_memory(code, 4096, PROT_READ | PROT_EXEC);
	const std::array<uint64_t, 1> arguments{41};
	const auto result = process.call(code, arguments, std::chrono::seconds(2));
	check(result.stop == raven::NativeStop::returned && result.value == 42, "the guest stopped running its code once the agent started");
	check(long(process.agent_syscall(SYS_getpid)) == group, "the service thread stopped answering after the guest ran");
}

// A process starts its agent itself, and the runtime's memory calls go to the service thread.
static void agent_mode(const std::string &bootstrap, raven::NativeMode mode)
{
	const std::array<raven::LoadedImage *, 0> images{};
	raven::NativeProcess process(images, bootstrap, std::chrono::seconds(2), mode);
	check(process.agent_task() > 0, "a process did not start its agent");
	const uint64_t before = process.agent_calls();
	const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE);
	check(mode != raven::NativeMode::x86 || code < UINT32_MAX, "an x86 guest got memory above 4 GiB");
	const std::vector<uint8_t> x64{0x48, 0x8d, 0x41, 0x01, 0xc3};
	const std::vector<uint8_t> x86{0x8b, 0x44, 0x24, 0x04, 0x40, 0xc2, 0x04, 0x00};
	process.write_memory(code, mode == raven::NativeMode::x64 ? x64 : x86);
	process.protect_memory(code, 4096, PROT_READ | PROT_EXEC);
	check(process.agent_calls() >= before + 2, "the runtime's memory calls stopped a task instead of going to the service thread");
	const std::array<uint64_t, 1> arguments{6};
	const auto result = process.call(code, arguments, std::chrono::seconds(2));
	check(result.stop == raven::NativeStop::returned && result.value == 7, "code mapped by the service thread did not run");
	const uint64_t called = process.agent_calls();
	process.unmap_memory(code);
	check(process.agent_calls() == called + 1, "the runtime's release did not go to the service thread");
	uint8_t byte = 0;
	check(!process.read_alone(code, &byte, 1), "memory the runtime released is still mapped");
}

// The guest's task leaves ptrace as well: its calls, system calls, breakpoints and faults stop it in the agent's
// handler, and the runtime handles each stop as it handled a traced one.
static void held(const std::string &bootstrap)
{
	const std::array<raven::LoadedImage *, 0> images{};
	const auto timeout = std::chrono::seconds(2);
	raven::NativeProcess process(images, bootstrap, timeout, raven::NativeMode::x64);
	const pid_t guest = pid_t(status_field(process.agent_task(), "Tgid"));
	check(status_field(guest, "TracerPid") == 0, "the guest's task is still traced");

	const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE);
	// 0x000: the argument plus one. 0x010: Linux exit, which must stop for the runtime. 0x020: a breakpoint the runtime serves as an API.
	// 0x030: a read of address zero. 0x038: a store of RAX where the argument points.
	std::vector<uint8_t> bytes(0x40, 0xcc);
	const std::vector<uint8_t> add{0x48, 0x8d, 0x41, 0x01, 0xc3}, exit{0xb8, 60, 0, 0, 0, 0x31, 0xff, 0x0f, 0x05, 0xc3},
		trap{0xcc, 0xc3}, fault{0x8b, 0x04, 0x25, 0, 0, 0, 0, 0xc3};
	std::copy(add.begin(), add.end(), bytes.begin());
	std::copy(exit.begin(), exit.end(), bytes.begin() + 0x10);
	std::copy(trap.begin(), trap.end(), bytes.begin() + 0x20);
	std::copy(fault.begin(), fault.end(), bytes.begin() + 0x30);
	const std::vector<uint8_t> store{0x48, 0x89, 0x01, 0xc3};
	std::copy(store.begin(), store.end(), bytes.begin() + 0x38);
	process.write_memory(code, bytes);
	process.protect_memory(code, 4096, PROT_READ | PROT_EXEC);

	const std::array<uint64_t, 1> arguments{41};
	auto result = process.call(code, arguments, timeout);
	check(result.stop == raven::NativeStop::returned && result.value == 42, "a held task did not run its code");

	result = process.call(code + 0x10, arguments, timeout);
	check(result.stop == raven::NativeStop::syscall && result.value == 60, "a held task's system call did not stop for the runtime");
	const auto before = process.context();
	const auto mapped = process.map_memory(4096, PROT_READ | PROT_WRITE);
	process.unmap_memory(mapped);
	const auto after = process.context();
	check(std::memcmp(&after.registers, &before.registers, sizeof(user_regs_struct)) == 0,
		"the runtime's memory calls changed a held task's stopped registers");
	auto response = before;
	response.registers.rax = 88;
	response.registers.orig_rax = uint64_t(-1);
	process.set_context(response);
	result = process.resume(timeout);
	check(result.stop == raven::NativeStop::returned && result.value == 88, "a held task did not go on with the runtime's answer to its call");

	process.add_api_trap(code + 0x20);
	result = process.call(code + 0x20, arguments, timeout);
	check(result.stop == raven::NativeStop::api, "a held task's breakpoint did not stop as an API");
	process.return_api(77);
	result = process.resume(timeout);
	check(result.stop == raven::NativeStop::returned && result.value == 77, "a held task did not return the API's value");

	// The debug registers stay with ptrace, taken for the moment they are set: a watched write stops the task, which goes on after it.
	const auto watched = process.map_memory(4096, PROT_READ | PROT_WRITE);
	process.watch_write(watched);
	const std::array<uint64_t, 1> target{watched};
	result = process.call(code + 0x38, target, timeout);
	check(result.stop == raven::NativeStop::fault && result.signal == SIGTRAP && result.signal_code == TRAP_HWBKPT,
		"a held task's watched write did not stop for the runtime");
	check(status_field(guest, "TracerPid") == 0, "setting a held task's debug registers left it traced");
	process.watch_write(0);
	result = process.resume(timeout);
	check(result.stop == raven::NativeStop::returned, "a held task did not go on after its watched write");

	result = process.call(code + 0x30, arguments, timeout);
	check(result.stop == raven::NativeStop::fault && result.signal == SIGSEGV, "a held task's fault did not stop for the runtime");
	check(status_field(guest, "TracerPid") == 0, "the guest's task was traced again");
}

// A 32-bit guest leaves ptrace as well: its task runs compatibility-mode code and stops for the runtime over the 64-bit handler.
static void held_x86(const std::string &bootstrap)
{
	const std::array<raven::LoadedImage *, 0> images{};
	const auto timeout = std::chrono::seconds(2);
	raven::NativeProcess process(images, bootstrap, timeout, raven::NativeMode::x86);
	const pid_t guest = pid_t(status_field(process.agent_task(), "Tgid"));
	check(status_field(guest, "TracerPid") == 0, "the 32-bit guest's task is still traced");
	const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE);
	// 0x00: the stdcall argument plus one. 0x10: a breakpoint the runtime serves as an API.
	std::vector<uint8_t> bytes(0x20, 0xcc);
	const std::vector<uint8_t> add{0x8b, 0x44, 0x24, 0x04, 0x40, 0xc2, 0x04, 0x00};
	std::copy(add.begin(), add.end(), bytes.begin());
	process.write_memory(code, bytes);
	process.protect_memory(code, 4096, PROT_READ | PROT_EXEC);
	const std::array<uint64_t, 1> arguments{41};
	auto result = process.call(code, arguments, timeout);
	check(result.stop == raven::NativeStop::returned && result.value == 42, "a held 32-bit task did not run its code");
	// The API returns as stdcall, taking its one argument off the stack.
	process.add_api_trap(code + 0x10, 4);
	result = process.call(code + 0x10, arguments, timeout);
	check(result.stop == raven::NativeStop::api, "a held 32-bit task's breakpoint did not stop as an API");
	check(process.api_arguments(1) == std::vector<uint64_t>{41}, "a held 32-bit task's API argument differs");
	process.return_api(77);
	result = process.resume(timeout);
	check(result.stop == raven::NativeStop::returned && result.value == 77, "a held 32-bit task did not return the API's value");
	check(status_field(guest, "TracerPid") == 0, "the 32-bit guest's task was traced again");
}

// A guest out of ptrace ends with the runtime even when the runtime dies without its cleanup, as no tracer ends it then.
static void orphaned(const std::string &bootstrap)
{
	int channel[2];
	check(pipe(channel) == 0, "no pipe for the dying runtime");
	const pid_t runtime = fork();
	check(runtime >= 0, "cannot start the dying runtime");
	if (runtime == 0) {
		close(channel[0]);
		pid_t guest = 0;
		try {
			const std::array<raven::LoadedImage *, 0> images{};
			// Left to die with the process: no destructor runs.
			auto *process = new raven::NativeProcess(images, bootstrap, std::chrono::seconds(2), raven::NativeMode::x64);
			guest = pid_t(status_field(process->agent_task(), "Tgid"));
		} catch (...) {
		}
		(void)!write(channel[1], &guest, sizeof guest);
		_exit(0);
	}
	close(channel[1]);
	pid_t guest = 0;
	const bool told = read(channel[0], &guest, sizeof guest) == sizeof guest && guest > 0;
	close(channel[0]);
	int status = 0;
	waitpid(runtime, &status, 0);
	check(told, "the dying runtime started no guest out of ptrace");
	const auto ended = [&] {
		std::ifstream file("/proc/" + std::to_string(guest) + "/status");
		for (std::string line; std::getline(file, line);) {
			if (line.rfind("State:", 0) == 0)
				return line.find('Z') != std::string::npos || line.find('X') != std::string::npos;
		}
		return true;
	};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!ended() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	if (!ended()) {
		kill(-getpgid(guest), SIGKILL);
		throw std::runtime_error("a guest out of ptrace outlived its runtime");
	}
}

int main(int argc, char **argv)
{
	try {
		if (argc != 2)
			throw std::runtime_error("bootstrap path required");
		orphaned(argv[1]);
		agent(argv[1], raven::NativeMode::x64);
		agent(argv[1], raven::NativeMode::x86);
		agent_mode(argv[1], raven::NativeMode::x64);
		agent_mode(argv[1], raven::NativeMode::x86);
		held(argv[1]);
		held_x86(argv[1]);
		std::cout << "Agent service thread in x64/x86 guests: untraced, in the guest's thread group, its calls and memory the guest's, "
			"the runtime's memory calls, and guest tasks out of ptrace that end with their runtime, or traced under RAVEN_PTRACE\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
