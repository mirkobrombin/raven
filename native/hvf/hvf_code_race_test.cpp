// Races around a guest store to translated code, forced deterministically.
//   Retranslation in the window: after the store's fault has dropped patch_target's translation and before the store is retried, the
//   page is translated again from its old bytes (test seam VmProcess::after_code_written, standing in for another vCPU). The store must
//   still reach that translation: the vCPU's next call must return the new value.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <cstdio>
#include <cstring>
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
				runtime.remove_thread(thread);
			} catch (const std::exception &error) {
				check(false, error.what());
			}
		}).join();
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	if (failures)
		return 1;
	std::printf("hvf code race: PASS\n");
	return 0;
}
