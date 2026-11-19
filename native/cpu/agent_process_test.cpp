#include "native_cpu.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
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
	const std::array<raven::LoadedImage *, 0> images{};
	raven::NativeProcess process(images, bootstrap, std::chrono::seconds(2), mode);
	rejected([&] { process.agent_syscall(SYS_getpid); }, "a call went to an agent that was not started");
	check(process.agent_task() == 0, "an agent task before the agent started");
	process.start_agent();
	const pid_t task = process.agent_task();
	check(task > 0, "the agent started no task");
	rejected([&] { process.start_agent(); }, "the agent started twice");

	// The service thread lives in the guest's thread group, untraced.
	const long group = status_field(task, "Tgid");
	check(group != getpid(), "the service thread runs in the runtime");
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

// With RAVEN_AGENT set the process starts its agent itself, and the runtime's memory calls go to the service thread.
static void agent_mode(const std::string &bootstrap, raven::NativeMode mode)
{
	setenv("RAVEN_AGENT", "1", 1);
	const std::array<raven::LoadedImage *, 0> images{};
	raven::NativeProcess process(images, bootstrap, std::chrono::seconds(2), mode);
	unsetenv("RAVEN_AGENT");
	check(process.agent_task() > 0, "RAVEN_AGENT did not start the agent");
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

int main(int argc, char **argv)
{
	try {
		if (argc != 2)
			throw std::runtime_error("bootstrap path required");
		agent(argv[1], raven::NativeMode::x64);
		agent(argv[1], raven::NativeMode::x86);
		agent_mode(argv[1], raven::NativeMode::x64);
		agent_mode(argv[1], raven::NativeMode::x86);
		std::cout << "Agent service thread in x64/x86 guests: untraced, in the guest's thread group, its calls and memory the guest's, "
			"and the runtime's memory calls under RAVEN_AGENT\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
