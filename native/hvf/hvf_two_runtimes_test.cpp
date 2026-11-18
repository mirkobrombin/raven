// Two runtimes on one VmProcess translate patch_target, so both watch its page. A store to it from one must end (a watchdog fails the
// test if it does not within 10 s) and drop both runtimes' translations: each must then return the new value. With no runtime
// attached, translating must be refused rather than watch a page no write could release.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

using namespace raven;

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_two_runtimes_test GUEST_DIRECTORY\n");
		return 2;
	}
	std::atomic<bool> done{false};
	std::thread([&] {
		for (int i = 0; i < 1000 && !done; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		if (!done) {
			std::fprintf(stderr, "FAIL: the store to a page two runtimes watch did not end within 10 s\n");
			std::_Exit(1);
		}
	}).detach();
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
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		hvf::Vcpu vcpu(vm, true);
		hvf::VmCodeCache first_cache(vm, 4 << 20, 1 << 20), second_cache(vm, 4 << 20, 1 << 20);
		translate::Runtime first(process, first_cache, options, &vcpu), second(process, second_cache, options, &vcpu);
		process.attach(first);
		process.attach(second);
		process.prepare_thread(first);
		process.prepare_thread(second);
		first.call(entry, {7, 0, 1});
		second.call(entry, {7, 0, 1});
		const uint64_t stored = first.call(entry, {3, 0, 55}).value;
		const uint64_t seen = second.call(entry, {7, 0, 1}).value;
		std::printf("store from the first runtime: it returned %llu, the second runtime then %llu, %llu write faults\n",
			static_cast<unsigned long long>(stored), static_cast<unsigned long long>(seen),
			static_cast<unsigned long long>(vcpu.stats().write_faults));
		check(stored == 55, "the storing runtime returned " + std::to_string(stored));
		check(seen == 55, "the other runtime returned " + std::to_string(seen));
		process.detach(first);
		process.detach(second);
		hvf::VmCodeCache third_cache(vm, 4 << 20, 1 << 20);
		translate::Runtime third(process, third_cache, options, &vcpu);
		process.prepare_thread(third);
		bool refused = false;
		try {
			third.call(entry, {7, 0, 1});
		} catch (const std::logic_error &) {
			refused = true;
		}
		check(refused, "a runtime no VmProcess::attach named translated and watched code");
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	done = true;
	if (failures)
		return 1;
	std::printf("hvf two runtimes: PASS\n");
	return 0;
}
