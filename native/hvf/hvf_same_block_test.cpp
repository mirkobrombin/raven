// x64 code that stores into a later instruction of its own basic block (hvf_client.c role 8, patch_next) must run the stored
// instruction, as an x86 CPU does. Here the storing vCPU resumes inside the translation the store dropped and runs the old
// instruction: a known gap, so ctest expects this test to fail (WILL_FAIL) until the store ends its block.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <thread>

using namespace raven;

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_same_block_test GUEST_DIRECTORY\n");
		return 2;
	}
	uint64_t value = 0;
	try {
		hvf::Vm vm(256 << 20);
		hvf::VmProcess process(vm);
		const uint64_t entry = process.entry(process.load(std::filesystem::path(argv[1]) / "hvf_client.exe"));
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		hvf::VmCodeCache cache(vm, 16 << 20, 1 << 20);
		hvf::Vcpu vcpu(vm, true);
		translate::Runtime runtime(process, cache, options, &vcpu);
		process.attach(runtime);
		process.prepare_thread(runtime);
		value = runtime.call(entry, {8, 0, 0}).value;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	if (value != 2) {
		std::fprintf(stderr, "FAIL: patch_next stored 2 into its own next instruction and returned %llu\n",
			static_cast<unsigned long long>(value));
		return 1;
	}
	std::printf("hvf same block: PASS\n");
	return 0;
}
