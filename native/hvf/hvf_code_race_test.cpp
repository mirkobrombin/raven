// Races around a guest store to translated code, forced deterministically.
//   Retranslation in the window: after the store's fault has dropped patch_target's translation and before the store is retried, the
//   page is translated again from its old bytes (test seam VmProcess::after_code_written, standing in for another vCPU). The store must
//   still reach that translation: the vCPU's next call must return the new value.
//   A second store to the same page: its permission fault reaches the host after the first store's handler unwatched the page. The
//   handler must have it retried (true), not report a guest exception; an address outside guest memory stays unhandled.
//   The kick: a guest thread whose runner only records calls stands for a vCPU elsewhere; every store that drops translations must
//   quiesce it, and the storing vCPU's own thread is not.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

using namespace raven;

namespace {

int failures = 0;

struct Recorder final : translate::CodeRunner {
	std::atomic<uint64_t> quiesced{0};
	uint32_t enter(uint64_t, uint64_t, uint64_t) override { throw std::logic_error("the recorder runs no code"); }
	void quiesce() override { ++quiesced; }
};

void check(bool condition, const std::string &message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
}

}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_code_race_test GUEST_DIRECTORY\n");
		return 2;
	}
	try {
		hvf::Vm vm(256 << 20);
		hvf::VmProcess process(vm);
		const uint64_t base = process.load(std::filesystem::path(argv[1]) / "hvf_client.exe");
		const uint64_t entry = process.entry(base);
		// patch_target opens .text: mov eax, imm32; ret.
		const uint64_t patch_target = base + 0x1000;
		uint8_t code[6];
		vm.read(patch_target, code, sizeof code);
		if (code[0] != 0xb8 || code[5] != 0xc3)
			throw std::runtime_error("patch_target is not where the test expects it");
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		hvf::VmCodeCache cache(vm, 16 << 20, 1 << 20);
		translate::Runtime runtime(process, cache, options);
		process.attach(runtime);
		Recorder elsewhere;
		translate::Runtime::Thread &elsewhere_thread = runtime.add_thread(&elsewhere);
		std::thread([&] {
			try {
				hvf::Vcpu vcpu(vm, true);
				translate::Runtime::Thread &thread = runtime.add_thread(&vcpu);
				process.prepare_thread(thread.state());
				for (uint64_t value = 20; value < 25; ++value) {
					bool once = false;
					process.after_code_written = [&](uint64_t) {
						if (!once) {
							once = true;
							runtime.translate_ahead(patch_target);
						}
					};
					const translate::RunResult result = thread.call(entry, {3, 0, value});
					check(result.value == value, "retranslated in the window: rewrote patch_target to return " + std::to_string(value) +
						", it returned " + std::to_string(result.value));
				}
				process.after_code_written = nullptr;
				check(elsewhere.quiesced >= vcpu.stats().write_faults && vcpu.stats().write_faults >= 5,
					"code_written quiesced the other guest thread " + std::to_string(elsewhere.quiesced) + " times for " +
						std::to_string(vcpu.stats().write_faults) + " write faults");
				std::printf("kick: %llu quiesce calls on the other guest thread for %llu write faults\n",
					static_cast<unsigned long long>(elsewhere.quiesced.load()),
					static_cast<unsigned long long>(vcpu.stats().write_faults));
				runtime.remove_thread(thread);
				// patch_target is translated and watched: the first store's handler drops it, the second finds the page unwatched.
				check(vm.write_fault && vm.write_fault(patch_target + 1), "the first store's write fault is not retried");
				check(vm.write_fault(patch_target + 2), "a write fault on a guest page that is no longer watched is not retried");
				check(vm.write_fault && !vm.write_fault(uint64_t(1) << 37), "a write fault outside guest memory is retried");
			} catch (const std::exception &error) {
				check(false, error.what());
			}
		}).join();
		runtime.remove_thread(elsewhere_thread);
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	if (failures)
		return 1;
	std::printf("hvf code race: PASS\n");
	return 0;
}
