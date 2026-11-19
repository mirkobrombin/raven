// What one VM costs a process, for hvf_vm_cost_run.py: the time of each setup stage up to the first translated call, and the process's
// phys_footprint after each. Hypervisor.framework allows one VM per process, so every run is a process of its own.
//
// usage: hvf_vm_cost none | bare | vm GUEST_DIRECTORY RAM_MIB
//   none: return at once (the process cost of this binary alone); bare: hv_vm_create and hv_vm_destroy only; vm: the whole setup.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <Hypervisor/Hypervisor.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mach/mach.h>

using namespace raven;

namespace {

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Memory the kernel charges to this process, in KiB. Hypervisor.framework's kernel-side state (stage-2 tables, vCPU state) is not in it.
unsigned long long footprint_kib()
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
		return 0;
	return info.phys_footprint >> 10;
}

double us(uint64_t from, uint64_t to)
{
	return double(to - from) / 1e3;
}

}

int main(int argc, char **argv)
{
	if (argc < 2 || (std::strcmp(argv[1], "vm") == 0 && argc != 4)) {
		std::fprintf(stderr, "usage: hvf_vm_cost none | bare | vm GUEST_DIRECTORY RAM_MIB\n");
		return 2;
	}
	const unsigned long long start_kib = footprint_kib();
	const uint64_t start = now_ns();
	if (std::strcmp(argv[1], "none") == 0) {
		std::printf("start_kib=%llu\n", start_kib);
		return 0;
	}
	if (std::strcmp(argv[1], "bare") == 0) {
		if (hv_vm_create(nullptr) != HV_SUCCESS)
			return 1;
		const uint64_t created = now_ns();
		const unsigned long long created_kib = footprint_kib();
		hv_vm_destroy();
		std::printf("create_us=%.1f destroy_us=%.1f start_kib=%llu created_kib=%llu\n", us(start, created), us(created, now_ns()),
			start_kib, created_kib);
		return 0;
	}
	try {
		uint64_t vm_ready, loaded, thread_ready, called;
		unsigned long long vm_kib, loaded_kib, thread_kib, called_kib;
		{
			hvf::Vm vm(size_t(std::atoi(argv[3])) << 20);
			vm_ready = now_ns();
			vm_kib = footprint_kib();
			hvf::VmProcess process(vm);
			const uint64_t entry = process.entry(process.load(std::filesystem::path(argv[2]) / "hvf_client.exe"));
			loaded = now_ns();
			loaded_kib = footprint_kib();
			hvf::Vcpu vcpu(vm, true);
			hvf::VmCodeCache cache(vm, 4 << 20, 1 << 20);
			translate::Runtime runtime(process, cache, translate::TranslatorOptions{}, &vcpu);
			hvf::VmProcess::Attachment attached(process, runtime);
			process.prepare_thread(runtime);
			thread_ready = now_ns();
			thread_kib = footprint_kib();
			// hvf_client.c's role 2: one step of its LCG from seed 1.
			const translate::RunResult done = runtime.call(entry, {2, 0, 1});
			called = now_ns();
			called_kib = footprint_kib();
			if (done.reason != translate::ExitReason::returned || done.value != 1 * 6364136223846793005ull + 1442695040888963407ull) {
				std::fprintf(stderr, "FAIL: the first call did not return the LCG step\n");
				return 1;
			}
		}
		std::printf("vm_us=%.1f load_us=%.1f thread_us=%.1f first_call_us=%.1f teardown_us=%.1f start_kib=%llu vm_kib=%llu loaded_kib=%llu "
			"thread_kib=%llu called_kib=%llu\n",
			us(start, vm_ready), us(vm_ready, loaded), us(loaded, thread_ready), us(thread_ready, called), us(called, now_ns()), start_kib,
			vm_kib, loaded_kib, thread_kib, called_kib);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	return 0;
}
