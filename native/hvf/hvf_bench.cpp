// Times one workload image (hvf_<workload>.exe, entry bench_loop) translated in plain mode, in a vCPU with hardware TSO or in this
// process, as bench_native.c times the same loop natively. Two methods:
//   loop  one guest call of bench_loop(CALLS) per run: the caller loop is translated code and the vCPU is entered once per run.
//   call  CALLS guest calls of bench_loop(1) per run: one entry into translated code per workload call, as translate_bench
//         --workload does, so in a vCPU every call is a host exit and a resume.
// hvf_bench write CLIENT RUNS N times hvf_client.c's roles 6 and 7 in a vCPU: N calls of patch_target, each followed by a store to
// its code (a stage-2 write fault, invalidation, and translation again at the next call) or not, and prints "write NS" and "plain NS"
// per iteration for each run, the vCPU's write faults, and "protect_pair NS" for one hv_vm_protect to read-only and back.
// Prints "first NS", "translated INSTRUCTIONS BLOCKS NS", "run INDEX NS_PER_CALL", "entries N" (entries into translated code over
// the runs) and "checksum N".
#include "code_cache.h"
#include "runtime.h"
#include "test_process.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <pthread/qos.h>
#include <string>

using namespace raven;

namespace {

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void measure(translate::Runtime &runtime, uint64_t entry, int runs, uint64_t calls, bool loop)
{
	uint64_t start = now_ns();
	uint64_t sink = runtime.call(entry, {1}).value;
	std::printf("first %.3f\n", double(now_ns() - start));
	std::printf("translated %llu %llu %llu\n", static_cast<unsigned long long>(runtime.translation_stats().guest_instructions),
		static_cast<unsigned long long>(runtime.translation_stats().blocks), static_cast<unsigned long long>(runtime.stats().translate_ns));
	const uint64_t entries = runtime.stats().runs;
	for (int run = 0; run < runs; ++run) {
		start = now_ns();
		if (loop) {
			const translate::RunResult result = runtime.call(entry, {calls});
			if (result.reason != translate::ExitReason::returned)
				throw std::runtime_error("the workload did not return");
			sink += result.value;
		} else {
			for (uint64_t i = 0; i < calls; ++i)
				sink += runtime.call(entry, {1}).value;
		}
		std::printf("run %d %.3f\n", run, double(now_ns() - start) / double(calls));
	}
	std::printf("entries %llu\n", static_cast<unsigned long long>(runtime.stats().runs - entries));
	std::printf("checksum %llu\n", static_cast<unsigned long long>(sink));
}

}

int main(int argc, char **argv)
{
	if (argc == 5 && std::string(argv[1]) == "write") {
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
		try {
			hvf::Vm vm(256 << 20);
			hvf::VmProcess process(vm);
			hvf::VmCodeCache cache(vm, 64 << 20, 1 << 20);
			hvf::Vcpu vcpu(vm, true);
			translate::TranslatorOptions options;
			options.ordering = translate::MemoryOrdering::hardware;
			translate::Runtime runtime(process, cache, options, &vcpu);
			process.attach(runtime);
			process.prepare_thread(runtime);
			const uint64_t entry = process.entry(process.load(argv[2]));
			const int runs = std::atoi(argv[3]);
			const uint64_t n = std::strtoull(argv[4], nullptr, 10);
			runtime.call(entry, {6, 0, 2});
			runtime.call(entry, {7, 0, 2});
			const uint64_t faults = vcpu.stats().write_faults;
			for (int run = 0; run < runs; ++run)
				for (const uint64_t role : {6, 7}) {
					const uint64_t start = now_ns();
					runtime.call(entry, {role, 0, n});
					std::printf("%s %.3f\n", role == 6 ? "write" : "plain", double(now_ns() - start) / double(n));
				}
			std::printf("write_faults %llu\n", static_cast<unsigned long long>(vcpu.stats().write_faults - faults));
			// One part of the cycle on its own: taking write permission from a page and giving it back.
			const uint64_t page = vm.map_anywhere(hvf::Vm::page_size, hvf::Access::read_write);
			for (int run = 0; run < runs; ++run) {
				const uint64_t start = now_ns();
				for (uint64_t i = 0; i < 10000; ++i) {
					vm.protect(page, false);
					vm.protect(page, true);
				}
				std::printf("protect_pair %.3f\n", double(now_ns() - start) / 10000);
			}
		} catch (const std::exception &error) {
			std::fprintf(stderr, "hvf_bench: %s\n", error.what());
			return 1;
		}
		return 0;
	}
	if (argc != 6) {
		std::fprintf(stderr, "usage: hvf_bench vm|process IMAGE RUNS CALLS loop|call\n");
		return 2;
	}
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	const std::string engine = argv[1], method = argv[5];
	const int runs = std::atoi(argv[3]);
	const uint64_t calls = std::strtoull(argv[4], nullptr, 10);
	translate::TranslatorOptions options;
	options.ordering = translate::MemoryOrdering::hardware;
	try {
		if (engine == "vm") {
			hvf::Vm vm(256 << 20);
			hvf::VmProcess process(vm);
			hvf::VmCodeCache cache(vm, 64 << 20, 1 << 20);
			hvf::Vcpu vcpu(vm, true);
			translate::Runtime runtime(process, cache, options, &vcpu);
			process.prepare_thread(runtime);
			const uint64_t entry = process.entry(process.load(argv[2]));
			measure(runtime, entry, runs, calls, method == "loop");
		} else {
			translate::TestProcess process;
			translate::JitCodeCache cache(64 << 20);
			translate::Runtime runtime(process, cache, options);
			process.attach(runtime);
			const uint64_t entry = process.entry(process.load(argv[2]));
			process.prepare_thread();
			measure(runtime, entry, runs, calls, method == "loop");
		}
	} catch (const std::exception &error) {
		std::fprintf(stderr, "hvf_bench: %s\n", error.what());
		return 1;
	}
	return 0;
}
