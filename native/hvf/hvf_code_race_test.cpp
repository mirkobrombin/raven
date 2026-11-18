// Races around a guest store to translated code, forced deterministically.
//   Retranslation in the window: after the store's fault has dropped patch_target's translation and before the store is retried, the
//   page is translated again from its old bytes (test seam VmProcess::after_code_written, standing in for another vCPU). The store must
//   still reach that translation: the vCPU's next call must return the new value.
//   A second store to the same page: its permission fault reaches the host after the first store's handler unwatched the page. The
//   handler must have it retried (true), not report a guest exception; an address outside guest memory stays unhandled.
//   Translation again after every fault (the seam on every call): retrying the store can never win, so the store must still end, within
//   a bounded number of faults, and take effect. A watchdog fails the test after 10 s.
//   A write by the host through VmProcess::write, past stage 2, must drop the translations of the code it changes.
//   The kick: a guest thread whose runner only records calls stands for a vCPU elsewhere; every store that drops translations must
//   quiesce it, and the storing vCPU's own thread is not.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <atomic>
#include <chrono>
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
		hvf::VmProcess::Attachment attached(process, runtime);
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
				// Every fault translates the page again.
				std::atomic<bool> ended{false};
				std::thread([&] {
					for (int i = 0; i < 1000 && !ended; ++i)
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					if (!ended) {
						std::fprintf(stderr, "FAIL: a store whose page is translated again after every fault did not end in 10 s\n");
						std::_Exit(1);
					}
				}).detach();
				process.after_code_written = [&](uint64_t) { runtime.translate_ahead(patch_target); };
				const uint64_t faults_before = vcpu.stats().write_faults;
				const uint64_t always = thread.call(entry, {3, 0, 30}).value;
				ended = true;
				process.after_code_written = nullptr;
				std::printf("translated again after every fault: the store ended after %llu faults, the call returned %llu\n",
					static_cast<unsigned long long>(vcpu.stats().write_faults - faults_before), static_cast<unsigned long long>(always));
				check(always == 30, "translated again after every fault: rewrote patch_target to return 30, it returned " +
					std::to_string(always));
				thread.call(entry, {7, 0, 1});
				const uint32_t host_value = 77;
				process.write(patch_target + 1, &host_value, 4);
				const uint64_t after_host_write = thread.call(entry, {7, 0, 1}).value;
				check(after_host_write == host_value,
					"after the host wrote patch_target to return 77, it returned " + std::to_string(after_host_write));
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
