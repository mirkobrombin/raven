// The integer workloads translated by Raven's runtime run at EL0 in a vCPU with hardware TSO and return what their native builds
// return, through one call each and through several calls in a translated loop.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

using namespace raven;

extern "C" uint64_t workload_crc32(void), workload_fnv(void), workload_histogram(void);
extern "C" unsigned native_entry(void);

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
}

struct Workload {
	const char *name;
	uint64_t expected;
};

}

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: hvf_workloads_test GUEST_DIRECTORY\n");
		return 2;
	}
	const std::filesystem::path guests = argv[1];
	const Workload workloads[] = {
		{"entry", native_entry()},
		{"crc32", workload_crc32()},
		{"fnv", workload_fnv()},
		{"histogram", workload_histogram()},
	};
	try {
		hvf::Vm vm(256 << 20);
		hvf::VmProcess process(vm);
		hvf::VmCodeCache cache(vm, 32 << 20, 1 << 20);
		hvf::Vcpu vcpu(vm, true);
		check(vcpu.actlr() & 2, "ACTLR_EL1.EnTSO reads back clear");
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		// A runtime gives its CpuState and lookup table back: 1 MiB of runtime data holds about 15 at once.
		for (int i = 0; i < 64; ++i)
			translate::Runtime discarded(process, cache, options, &vcpu);
		// Also when the constructor throws: a code cache too small for the fixed code.
		hvf::VmCodeCache tiny(vm, 64, 1 << 20);
		for (int i = 0; i < 64; ++i)
			try {
				translate::Runtime refused(process, tiny, options, &vcpu);
				check(false, "a runtime fit its fixed code in 64 bytes");
			} catch (const std::runtime_error &) {
			}
		check(tiny.allocate_data(1 << 19).host != nullptr, "a runtime that failed to construct kept its runtime data");
		for (const Workload &workload : workloads) {
			const std::string name = workload.name;
			translate::Runtime runtime(process, cache, options, &vcpu);
			process.prepare_thread(runtime);
			const uint64_t entry = process.entry(process.load(guests / ("hvf_" + name + ".exe")));
			const translate::RunResult once = runtime.call(entry, {1});
			check(once.reason == translate::ExitReason::returned, name + ": one call did not return");
			check(once.value == workload.expected, name + ": one call returned " + std::to_string(once.value) + ", native returns " +
				std::to_string(workload.expected));
			// The exit came through the dispatcher's svc: the state it saved says EL0t, and the exception class is SVC from AArch64.
			uint64_t spsr = ~0ull, esr = 0;
			hv_vcpu_get_sys_reg(vcpu.id(), HV_SYS_REG_SPSR_EL1, &spsr);
			hv_vcpu_get_sys_reg(vcpu.id(), HV_SYS_REG_ESR_EL1, &esr);
			check((spsr & 0xf) == 0 && esr >> 26 == 0x15, name + ": the last exit did not come from EL0");
			const translate::RunResult loop = runtime.call(entry, {7});
			check(loop.reason == translate::ExitReason::returned && loop.value == 7 * workload.expected,
				name + ": seven calls in the translated loop returned " + std::to_string(loop.value));
			std::printf("%s: %llu (native %llu), SPSR_EL1 %#llx, %llu runs, %llu lookup misses, %llu chains, %llu blocks\n",
				name.c_str(), static_cast<unsigned long long>(once.value), static_cast<unsigned long long>(workload.expected),
				static_cast<unsigned long long>(spsr), static_cast<unsigned long long>(runtime.stats().runs),
				static_cast<unsigned long long>(runtime.stats().lookup_misses), static_cast<unsigned long long>(runtime.stats().chains),
				static_cast<unsigned long long>(runtime.translation_stats().blocks));
		}
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	if (failures)
		return 1;
	std::printf("hvf workloads: PASS\n");
	return 0;
}
