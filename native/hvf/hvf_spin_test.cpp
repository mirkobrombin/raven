// Translated x64 spins at EL0 for seconds on two vCPUs while spinning host threads fill every core, so the host preempts and moves
// the vCPU threads. x18, which the translator never touches, holds a sentinel per vCPU: it must read back unchanged at every forced
// exit and at the end, and each spin must return the value the same LCG gives on the host.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <pthread.h>
#include <set>
#include <string>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace raven;

namespace {

constexpr uint64_t multiplier = 6364136223846793005ull, increment = 1442695040888963407ull;

// n steps of the LCG from seed, by squaring: what hvf_client.c's role 2 computes step by step.
uint64_t lcg_skip(uint64_t seed, uint64_t steps)
{
	uint64_t step_multiplier = multiplier, step_increment = increment, total_multiplier = 1, total_increment = 0;
	for (; steps; steps >>= 1) {
		if (steps & 1) {
			total_multiplier *= step_multiplier;
			total_increment = total_increment * step_multiplier + step_increment;
		}
		step_increment *= step_multiplier + 1;
		step_multiplier *= step_multiplier;
	}
	return total_multiplier * seed + total_increment;
}

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Spinner {
	std::atomic<hv_vcpu_t> id{0};
	std::atomic<bool> ready{false};
	uint64_t sentinel = 0;
	uint64_t steps = 0;
	uint64_t result = 0;
	double seconds = 0;
	uint64_t cancels = 0, x18_lost = 0, migrations = 0, final_x18 = 0;
	std::set<size_t> cpus;
	std::string error;
};

void spin(hvf::Vm &vm, hvf::VmProcess &process, uint64_t entry, Spinner &spinner)
{
	try {
		hvf::Vcpu vcpu(vm, true);
		hvf::VmCodeCache cache(vm, 4 << 20, 1 << 20);
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		translate::Runtime runtime(process, cache, options, &vcpu);
		process.prepare_thread(runtime);
		vcpu.set_reg(HV_REG_X18, spinner.sentinel);
		size_t last_cpu = ~size_t(0);
		vcpu.on_cancel = [&](hvf::Vcpu &self) {
			++spinner.cancels;
			if (self.reg(HV_REG_X18) != spinner.sentinel)
				++spinner.x18_lost;
			size_t cpu = 0;
			pthread_cpu_number_np(&cpu);
			if (last_cpu != ~size_t(0) && cpu != last_cpu)
				++spinner.migrations;
			last_cpu = cpu;
			spinner.cpus.insert(cpu);
		};
		spinner.id = vcpu.id();
		spinner.ready = true;
		const uint64_t start = now_ns();
		const translate::RunResult done = runtime.call(entry, {2, 0, spinner.steps});
		spinner.seconds = double(now_ns() - start) / 1e9;
		if (done.reason != translate::ExitReason::returned)
			throw std::runtime_error("the spin did not return");
		spinner.result = done.value;
		spinner.final_x18 = vcpu.reg(HV_REG_X18);
		spinner.ready = false;
	} catch (const std::exception &error) {
		spinner.error = error.what();
		spinner.ready = false;
	}
}

}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_spin_test GUEST_DIRECTORY [SECONDS]\n");
		return 2;
	}
	const double target_seconds = argc > 2 ? std::atof(argv[2]) : 3;
	int failures = 0;
	auto fail = [&](const std::string &message) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	};
	if (lcg_skip(5, 5) != [] { uint64_t v = 5; for (int i = 0; i < 5; ++i) v = v * multiplier + increment; return v; }())
		fail("lcg_skip disagrees with the LCG");

	// The load: a child process, forked before the VM exists, with two spinning threads per core. It ends when this process does, and
	// after a minute whatever happens.
	int cores = 0;
	size_t length = sizeof cores;
	sysctlbyname("hw.ncpu", &cores, &length, nullptr, 0);
	const pid_t parent = getpid();
	const pid_t load = fork();
	if (load == 0) {
		alarm(60);
		for (int i = 0; i < 2 * cores; ++i)
			std::thread([] { for (volatile uint64_t n = 0;; n = n + 1) {} }).detach();
		while (getppid() == parent)
			sleep(1);
		_exit(0);
	}
	rusage before{};
	getrusage(RUSAGE_SELF, &before);
	try {
		hvf::Vm vm(256 << 20);
		hvf::VmProcess process(vm);
		const uint64_t entry = process.entry(process.load(std::filesystem::path(argv[1]) / "hvf_client.exe"));

		// Steps per second of the translated LCG on this machine under this load, from a short run.
		Spinner probe;
		probe.sentinel = 0x5eed000000000000ull;
		probe.steps = 50000000;
		std::thread(spin, std::ref(vm), std::ref(process), entry, std::ref(probe)).join();
		if (!probe.error.empty())
			throw std::runtime_error(probe.error);
		const uint64_t steps = static_cast<uint64_t>(double(probe.steps) / probe.seconds * target_seconds);

		Spinner spinners[2];
		for (unsigned i = 0; i < 2; ++i) {
			spinners[i].sentinel = 0x5eed5eed00000000ull | i;
			spinners[i].steps = steps + i;
		}
		std::atomic<bool> done{false};
		std::thread watcher([&] {
			while (!done) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				for (Spinner &spinner : spinners)
					if (spinner.ready) {
						hv_vcpu_t id = spinner.id;
						hv_vcpus_exit(&id, 1);
					}
			}
		});
		std::thread first(spin, std::ref(vm), std::ref(process), entry, std::ref(spinners[0]));
		std::thread second(spin, std::ref(vm), std::ref(process), entry, std::ref(spinners[1]));
		first.join();
		second.join();
		done = true;
		watcher.join();
		for (const Spinner &spinner : spinners) {
			std::printf("vCPU x18 %#llx: %llu steps in %.2f s, result %s, %llu forced exits, x18 changed at %llu of them and reads %#llx "
				"at the end, %llu host CPU changes between exits over %zu distinct CPUs\n",
				static_cast<unsigned long long>(spinner.sentinel), static_cast<unsigned long long>(spinner.steps), spinner.seconds,
				spinner.result == lcg_skip(spinner.steps, spinner.steps) ? "right" : "WRONG",
				static_cast<unsigned long long>(spinner.cancels), static_cast<unsigned long long>(spinner.x18_lost),
				static_cast<unsigned long long>(spinner.final_x18), static_cast<unsigned long long>(spinner.migrations),
				spinner.cpus.size());
			if (!spinner.error.empty())
				fail(spinner.error);
			if (spinner.result != lcg_skip(spinner.steps, spinner.steps))
				fail("a spin returned a wrong value");
			if (spinner.x18_lost || spinner.final_x18 != spinner.sentinel)
				fail("x18 changed");
			if (spinner.seconds < target_seconds / 2 || spinner.cancels < 10)
				fail("a vCPU spun too briefly to tell");
		}
	} catch (const std::exception &error) {
		fail(error.what());
	}
	kill(load, SIGKILL);
	waitpid(load, nullptr, 0);
	rusage after{};
	getrusage(RUSAGE_SELF, &after);
	std::printf("host load: %d spinning threads; this process: %ld involuntary and %ld voluntary context switches\n", 2 * cores,
		after.ru_nivcsw - before.ru_nivcsw, after.ru_nvcsw - before.ru_nvcsw);
	if (failures)
		return 1;
	std::printf("hvf spin: PASS\n");
	return 0;
}
