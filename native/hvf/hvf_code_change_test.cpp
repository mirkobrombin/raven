// Guest stores to x64 code that has been translated. On one vCPU: hvf_client.c's role 3 runs patch_target, rewrites its immediate and
// runs it again, and must get the new value. Across vCPUs: role 4 keeps calling patch_target on vCPU A, in a loop the runtime has
// chained, while role 5 on vCPU B rewrites it; A must see the new value within a bounded time after B's store returns.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

using namespace raven;

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
}

// A guest thread leaves the runtime before its vCPU is destroyed, so that code_written never quiesces a vCPU that is gone.
struct Leave {
	translate::Runtime &runtime;
	translate::Runtime::Thread &thread;
	~Leave() { runtime.remove_thread(thread); }
};

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_code_change_test GUEST_DIRECTORY [TRIALS]\n");
		return 2;
	}
	const int trials = argc > 2 ? std::atoi(argv[2]) : 10;
	constexpr uint64_t bound = 300000000;
	try {
		hvf::Vm vm(256 << 20);
		hvf::VmProcess process(vm);
		const uint64_t entry = process.entry(process.load(std::filesystem::path(argv[1]) / "hvf_client.exe"));
		const uint64_t shared = vm.map_anywhere(hvf::Vm::page_size, hvf::Access::read_write);
		auto *words = reinterpret_cast<volatile uint64_t *>(vm.host(shared));
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		hvf::VmCodeCache cache(vm, 16 << 20, 1 << 20);
		translate::Runtime runtime(process, cache, options);
		process.attach(runtime);

		// One vCPU writes code it has run, then runs it.
		std::thread([&] {
			try {
				hvf::Vcpu vcpu(vm, true);
				translate::Runtime::Thread &thread = runtime.add_thread(&vcpu);
				Leave leave{runtime, thread};
				process.prepare_thread(thread.state());
				for (uint64_t value = 10; value < 10 + uint64_t(trials); ++value) {
					const translate::RunResult result = thread.call(entry, {3, shared, value});
					check(result.reason == translate::ExitReason::returned && result.value == value,
						"one vCPU: rewrote patch_target to return " + std::to_string(value) + ", it returned " +
							std::to_string(result.value));
				}
			} catch (const std::exception &error) {
				check(false, error.what());
			}
		}).join();

		// vCPU A runs patch_target in a chained loop while vCPU B rewrites it.
		uint64_t worst_ns = 0, kicks = 0, faults = 0;
		for (int trial = 0; trial < trials; ++trial) {
			words[48] = 0;
			words[50] = 1000 + uint64_t(trial);
			uint64_t written = 0, seen = 0, count = 0;
			std::thread a([&] {
				try {
					hvf::Vcpu vcpu(vm, true);
					translate::Runtime::Thread &thread = runtime.add_thread(&vcpu);
					Leave leave{runtime, thread};
					process.prepare_thread(thread.state());
					const translate::RunResult result = thread.call(entry, {4, shared, bound});
					seen = now_ns();
					count = result.value;
					kicks += vcpu.stats().cancels;
				} catch (const std::exception &error) {
					check(false, error.what());
				}
			});
			std::thread b([&] {
				try {
					hvf::Vcpu vcpu(vm, true);
					translate::Runtime::Thread &thread = runtime.add_thread(&vcpu);
					Leave leave{runtime, thread};
					process.prepare_thread(thread.state());
					thread.call(entry, {5, shared, 1000000});
					written = now_ns();
					faults += vcpu.stats().write_faults;
				} catch (const std::exception &error) {
					check(false, error.what());
				}
			});
			a.join();
			b.join();
			const uint64_t latency = seen > written ? seen - written : 0;
			worst_ns = std::max(worst_ns, latency);
			check(count < bound, "two vCPUs, trial " + std::to_string(trial) + ": A ran the old patch_target " + std::to_string(count) +
				" times and never saw B's store");
			check(latency < 100000000, "two vCPUs, trial " + std::to_string(trial) + ": A saw B's store " +
				std::to_string(latency / 1000) + " us after it");
		}
		std::printf("two vCPUs: A saw B's store within %.1f us of B's call returning, worst of %d trials; B took %llu write faults, A "
			"was kicked %llu times\n", double(worst_ns) / 1e3, trials, static_cast<unsigned long long>(faults),
			static_cast<unsigned long long>(kicks));
		// The kick itself, which the trials above may not need: A is usually out of the guest already, through the undone chain. Here
		// a vCPU spins in translated code that no write touches, and quiesce must make it exit and return.
		{
			std::atomic<hvf::Vcpu *> spinning{nullptr};
			std::atomic<bool> stop{false}, released{false};
			uint64_t cancels = 0;
			std::thread spinner([&] {
				try {
					hvf::Vcpu vcpu(vm, true);
					translate::Runtime::Thread &thread = runtime.add_thread(&vcpu);
					Leave leave{runtime, thread};
					process.prepare_thread(thread.state());
					vcpu.on_cancel = [&](hvf::Vcpu &) {
						if (stop)
							throw std::runtime_error("stopped");
					};
					spinning = &vcpu;
					try {
						thread.call(entry, {2, 0, ~uint64_t(0) >> 8});
					} catch (const std::runtime_error &) {
					}
					cancels = vcpu.stats().cancels;
					// The main thread may still be in quiesce on this vCPU.
					while (!released)
						std::this_thread::yield();
				} catch (const std::exception &error) {
					check(false, error.what());
				}
			});
			while (!spinning)
				std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			uint64_t worst = 0;
			for (int i = 0; i < 100; ++i) {
				const uint64_t start = now_ns();
				spinning.load()->quiesce();
				worst = std::max(worst, now_ns() - start);
			}
			stop = true;
			spinning.load()->quiesce();
			released = true;
			spinner.join();
			std::printf("quiesce of a spinning vCPU: 100 calls, %llu forced exits, slowest %.1f us\n",
				static_cast<unsigned long long>(cancels), double(worst) / 1e3);
			// A call that finds the vCPU between two exits, in the host, returns without a kick: there is nothing to synchronize.
			check(cancels >= 50 && worst < 100000000, "quiesce rarely made the spinning vCPU exit, or took 100 ms");
		}
		std::printf("runtime: %llu blocks translated, %llu invalidated\n",
			static_cast<unsigned long long>(runtime.translation_stats().blocks),
			static_cast<unsigned long long>(runtime.stats().invalidated_blocks));
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	if (failures)
		return 1;
	std::printf("hvf code change: PASS\n");
	return 0;
}
