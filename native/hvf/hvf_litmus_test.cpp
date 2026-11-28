// Message passing between two vCPUs running translated x64 at EL0, translated with plain loads and stores (MemoryOrdering::hardware).
// With ACTLR_EL1.EnTSO set the reader must never see y's new value with x's old one; with it clear, the same code on the same
// machine must show it, or the zero above proves nothing.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <algorithm>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace raven;

namespace {

struct Trials {
	uint64_t violations = 0;
	std::vector<uint64_t> per_trial, reader_loops;
	uint64_t actlr[2] = {};
};

Trials run(hvf::Vm &vm, hvf::VmProcess &process, uint64_t entry, uint64_t shared, bool tso, int trials, uint64_t writes)
{
	Trials result;
	auto *words = reinterpret_cast<volatile uint64_t *>(vm.host(shared));
	std::barrier start(2, [&]() noexcept {
		for (int i = 0; i < 64; ++i)
			words[i] = 0;
	});
	auto thread = [&](unsigned role) {
		try {
			hvf::Vcpu vcpu(vm, tso);
			result.actlr[role] = vcpu.actlr();
			hvf::VmCodeCache cache(vm, 4 << 20, 1 << 20);
			translate::TranslatorOptions options;
			options.ordering = translate::MemoryOrdering::hardware;
			translate::Runtime runtime(process, cache, options, &vcpu);
			hvf::VmProcess::Attachment attached(process, runtime);
			process.prepare_thread(runtime);
			for (int trial = 0; trial < trials; ++trial) {
				start.arrive_and_wait();
				const translate::RunResult done = runtime.call(entry, {role, shared, writes});
				if (done.reason != translate::ExitReason::returned)
					throw std::runtime_error("role " + std::to_string(role) + " did not return");
				if (role == 1) {
					result.violations += done.value;
					result.per_trial.push_back(done.value);
					result.reader_loops.push_back(uint64_t(words[33]));
				}
				start.arrive_and_wait();
			}
		} catch (const std::exception &error) {
			std::fprintf(stderr, "FAIL: %s\n", error.what());
			std::_Exit(1);
		}
	};
	std::thread writer(thread, 0u), reader(thread, 1u);
	writer.join();
	reader.join();
	return result;
}

uint64_t median(std::vector<uint64_t> values)
{
	std::sort(values.begin(), values.end());
	return values.empty() ? 0 : values[values.size() / 2];
}

}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_litmus_test GUEST_DIRECTORY [TRIALS [WRITES]]\n");
		return 2;
	}
	const int trials = argc > 2 ? std::atoi(argv[2]) : 30;
	const uint64_t writes = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 2000000;
	int failures = 0;
	try {
		hvf::Vm vm(256 << 20);
		hvf::VmProcess process(vm);
		const uint64_t entry = process.entry(process.load(std::filesystem::path(argv[1]) / "hvf_client.exe"));
		const uint64_t shared = vm.map_anywhere(hvf::Vm::page_size, hvf::Access::read_write);
		for (const bool tso : {true, false}) {
			const Trials result = run(vm, process, entry, shared, tso, trials, writes);
			const uint64_t loops = median(result.reader_loops);
			std::printf("EnTSO %d (ACTLR_EL1 read back %#llx, %#llx): %llu violations in %d trials of %llu writes, median %llu per "
				"trial, median reader loops %llu\n", tso, static_cast<unsigned long long>(result.actlr[0]),
				static_cast<unsigned long long>(result.actlr[1]), static_cast<unsigned long long>(result.violations), trials,
				static_cast<unsigned long long>(writes), static_cast<unsigned long long>(median(result.per_trial)),
				static_cast<unsigned long long>(loops));
			if (bool(result.actlr[0] & 2) != tso || bool(result.actlr[1] & 2) != tso) {
				std::fprintf(stderr, "FAIL: ACTLR_EL1.EnTSO does not read back as set\n");
				++failures;
			}
			// A reader that finished before the writer began would see no reordering whatever the ordering.
			if (loops < 1000) {
				std::fprintf(stderr, "FAIL: the reader and the writer barely overlapped\n");
				++failures;
			}
			if (tso && result.violations) {
				std::fprintf(stderr, "FAIL: x86 ordering broken with EnTSO set\n");
				++failures;
			}
			if (!tso && !result.violations) {
				std::fprintf(stderr, "FAIL: no reordering with EnTSO clear, so the test cannot tell\n");
				++failures;
			}
		}
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		++failures;
	}
	if (failures)
		return 1;
	std::printf("hvf litmus: PASS\n");
	return 0;
}
