// N vCPUs (2 and 4) run the same x64 workload at once from one runtime: one code cache, one lookup table, one CpuState each. Every
// vCPU must return the native result, and each block must be translated once however many vCPUs miss it together. N separate
// runtimes translate it N times, for comparison.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <barrier>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace raven;

extern "C" uint64_t workload_crc32(void);

namespace {

int failures = 0;
std::mutex report;

void check(bool condition, const std::string &message)
{
	if (!condition) {
		std::lock_guard guard(report);
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
}

constexpr uint64_t calls = 20;

// Runs the workload on n vCPUs started together; shared: one runtime for all, else one each. Returns the blocks translated in all.
uint64_t run(hvf::Vm &vm, hvf::VmProcess &process, uint64_t entry, unsigned n, bool shared, uint64_t expected)
{
	translate::TranslatorOptions options;
	options.ordering = translate::MemoryOrdering::hardware;
	hvf::VmCodeCache shared_cache(vm, 4 << 20, 1 << 20);
	translate::Runtime shared_runtime(process, shared_cache, options);
	std::vector<uint64_t> blocks(n);
	std::barrier start(n);
	std::vector<std::thread> threads;
	for (unsigned i = 0; i < n; ++i)
		threads.emplace_back([&, i] {
			try {
				hvf::Vcpu vcpu(vm, true);
				std::unique_ptr<hvf::VmCodeCache> own_cache;
				std::unique_ptr<translate::Runtime> own_runtime;
				translate::Runtime::Thread *thread;
				std::unique_ptr<void, std::function<void(void *)>> leave;
				if (shared) {
					thread = &shared_runtime.add_thread(&vcpu);
					leave = {thread, [&](void *added) { shared_runtime.remove_thread(*static_cast<translate::Runtime::Thread *>(added)); }};
				} else {
					own_cache = std::make_unique<hvf::VmCodeCache>(vm, 4 << 20, 1 << 20);
					own_runtime = std::make_unique<translate::Runtime>(process, *own_cache, options, &vcpu);
					thread = &own_runtime->main_thread();
				}
				process.prepare_thread(thread->state());
				start.arrive_and_wait();
				const translate::RunResult result = thread->call(entry, {calls});
				check(result.reason == translate::ExitReason::returned && result.value == calls * expected,
					"vCPU " + std::to_string(i) + " of " + std::to_string(n) + " returned " + std::to_string(result.value));
				if (own_runtime)
					blocks[i] = own_runtime->translation_stats().blocks;
			} catch (const std::exception &error) {
				check(false, error.what());
				start.arrive_and_drop();
			}
		});
	for (std::thread &thread : threads)
		thread.join();
	if (shared)
		return shared_runtime.translation_stats().blocks;
	uint64_t total = 0;
	for (uint64_t count : blocks)
		total += count;
	return total;
}

}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_shared_test GUEST_DIRECTORY [TRIALS]\n");
		return 2;
	}
	const int trials = argc > 2 ? std::atoi(argv[2]) : 20;
	try {
		hvf::Vm vm(512 << 20);
		hvf::VmProcess process(vm);
		const uint64_t entry = process.entry(process.load(std::filesystem::path(argv[1]) / "hvf_crc32.exe"));
		const uint64_t expected = workload_crc32();
		// The first call fills the workload's buffer, which stays filled in guest memory: count the blocks of a later call.
		run(vm, process, entry, 1, false, expected);
		const uint64_t alone = run(vm, process, entry, 1, false, expected);
		std::printf("one vCPU: %llu blocks\n", static_cast<unsigned long long>(alone));
		for (const unsigned n : {2u, 4u}) {
			uint64_t most = 0;
			for (int trial = 0; trial < trials; ++trial) {
				const uint64_t blocks = run(vm, process, entry, n, true, expected);
				most = std::max(most, blocks);
				check(blocks == alone, std::to_string(n) + " vCPUs sharing a runtime translated " + std::to_string(blocks) +
					" blocks, one translates " + std::to_string(alone));
			}
			const uint64_t separate = run(vm, process, entry, n, false, expected);
			std::printf("%u vCPUs: shared runtime, most blocks in %d trials %llu; %u separate runtimes %llu blocks\n", n, trials,
				static_cast<unsigned long long>(most), n, static_cast<unsigned long long>(separate));
		}
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	if (failures)
		return 1;
	std::printf("hvf shared: PASS\n");
	return 0;
}
