// The granule stage-2 write protection works in: two 4 KiB guest pages inside one 16 KiB-aligned IPA block, the first made read-only
// with Vm::protect. A guest store to the second must not fault (a 16 KiB granule would make it), a store to the first must.
// VmProcess and the runtime watch code in 4 KiB pages, so they rely on this.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

using namespace raven;

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_granule_test GUEST_DIRECTORY\n");
		return 2;
	}
	int failures = 0;
	auto check = [&](bool condition, const std::string &message) {
		if (!condition) {
			std::fprintf(stderr, "FAIL: %s\n", message.c_str());
			++failures;
		}
	};
	try {
		hvf::Vm vm(256 << 20);
		hvf::VmProcess process(vm);
		const uint64_t entry = process.entry(process.load(std::filesystem::path(argv[1]) / "hvf_client.exe"));
		const uint64_t region = vm.map_anywhere(64 << 10, hvf::Access::read_write);
		uint64_t first = region;
		while (vm.physical(first) % (16 << 10))
			first += hvf::Vm::page_size;
		const uint64_t second = first + hvf::Vm::page_size;
		std::vector<uint64_t> faults;
		vm.write_fault = [&](uint64_t address) {
			faults.push_back(address);
			vm.protect(address, true);
			return true;
		};
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		hvf::VmCodeCache cache(vm, 4 << 20, 1 << 20);
		hvf::Vcpu vcpu(vm, true);
		translate::Runtime runtime(process, cache, options, &vcpu);
		hvf::VmProcess::Attachment attached(process, runtime);
		process.prepare_thread(runtime);
		vm.protect(first, false);
		runtime.call(entry, {9, second, 0x22});
		const size_t on_second = faults.size();
		runtime.call(entry, {9, first, 0x11});
		const size_t on_first = faults.size() - on_second;
		uint64_t a = 0, b = 0;
		vm.read(first, &a, 8);
		vm.read(second, &b, 8);
		std::printf("first page IPA %#llx read-only: store to the next 4 KiB page took %zu faults, store to the page itself %zu\n",
			static_cast<unsigned long long>(vm.physical(first)), on_second, on_first);
		check(on_second == 0, "a store 4 KiB past a read-only page faulted: stage 2 protects more than 4 KiB");
		check(on_first == 1, "a store to a read-only page did not fault once");
		check(a == 0x11 && b == 0x22, "the stores did not land");
		vm.write_fault = nullptr;
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	if (failures)
		return 1;
	std::printf("hvf granule: PASS\n");
	return 0;
}
