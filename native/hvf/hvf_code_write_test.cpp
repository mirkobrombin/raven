// Code the host rewrites through its alias, at an address a vCPU has already executed, is what the vCPU runs next: the case of a
// chained branch patched in place or of a code cache reused after reset. Each iteration rewrites one block to return a new value and
// enters it once. Both maintenance modes must never run a stale block. The controls leave out the instruction cache invalidation and,
// on a CPU that says it needs one (CTR_EL0.DIC = 0), must run stale blocks: proof that the test sees the failure it guards against.
#include "a64_emitter.h"
#include "cpu_state.h"
#include "translator.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

using namespace raven;

namespace {

constexpr size_t block_words = 16;

std::vector<uint32_t> block_returning(uint64_t value)
{
	translate::a64::Assembler a;
	a.mov_imm(8, value);
	a.mov_imm(9, static_cast<uint32_t>(translate::ExitReason::returned), false);
	a.ldr(16, 23, translate::state_offset::exit_trampoline, 8);
	a.br(16);
	a.finish();
	a.words.resize(block_words, 0xd503201f); // nop
	return a.words;
}

struct Result {
	uint64_t stale = 0;
	uint64_t wrong_exits = 0;
};

}

int main(int argc, char **argv)
{
	const uint64_t iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
	struct Mode {
		const char *name;
		hvf::CodeMaintenance maintenance;
		bool control;
	};
	const Mode modes[] = {
		{"host sys_icache_invalidate on the alias (default)", {false, true, false}, false},
		{"host sys_dcache_flush + guest IC IALLUIS", {true, false, true}, false},
		{"control: host sys_dcache_flush only", {true, false, false}, true},
		{"control: none", {false, false, false}, true},
	};
	int failures = 0;
	try {
		hvf::Vm vm(64 << 20);
		hvf::VmCodeCache cache(vm, 1 << 20, 1 << 16);
		hvf::Vcpu vcpu(vm, true);
		const auto place = [&](const std::vector<uint32_t> &code) {
			uint8_t *at = cache.allocate(code.size() * 4);
			cache.write(at, code.data(), code.size() * 4);
			return at;
		};
		const uint64_t enter = reinterpret_cast<uint64_t>(place(translate::enter_trampoline()));
		const uint64_t exit = reinterpret_cast<uint64_t>(place(translate::exit_trampoline()));
		const translate::CodeAllocator::Data state_memory = cache.allocate_data(sizeof(translate::CpuState));
		auto &state = *new (state_memory.host) translate::CpuState{};
		state.exit_trampoline = exit;

		// CTR_EL0 as EL0 of the vCPU reads it: IDC (bit 28) and DIC (bit 29) say which maintenance the CPU makes unnecessary.
		uint8_t *probe = cache.allocate(block_words * 4);
		std::vector<uint32_t> read_ctr = block_returning(0);
		read_ctr[0] = 0xd53b0028; // mrs x8, ctr_el0
		read_ctr[1] = 0xd503201f;
		cache.write(probe, read_ctr.data(), read_ctr.size() * 4);
		vcpu.enter(enter, state_memory.address, reinterpret_cast<uint64_t>(probe));
		const uint64_t ctr = state.gpr[translate::rax];
		std::printf("CTR_EL0 at EL0 in the vCPU: %#llx (IDC %llu, DIC %llu, L1Ip %llu)\n", static_cast<unsigned long long>(ctr),
			static_cast<unsigned long long>(ctr >> 28 & 1), static_cast<unsigned long long>(ctr >> 29 & 1),
			static_cast<unsigned long long>(ctr >> 14 & 3));

		uint8_t *block = cache.allocate(block_words * 4);
		for (const Mode &mode : modes) {
			vm.maintenance = mode.maintenance;
			Result result;
			const uint64_t invalidations = vcpu.stats().instruction_cache_invalidations;
			for (uint64_t i = 1; i <= iterations; ++i) {
				const std::vector<uint32_t> code = block_returning(i);
				cache.write(block, code.data(), code.size() * 4);
				const uint32_t reason = vcpu.enter(enter, state_memory.address, reinterpret_cast<uint64_t>(block));
				if (reason != static_cast<uint32_t>(translate::ExitReason::returned))
					++result.wrong_exits;
				else if (state.gpr[translate::rax] != i)
					++result.stale;
			}
			std::printf("%s: %llu stale of %llu, %llu wrong exits, %llu guest invalidations\n", mode.name,
				static_cast<unsigned long long>(result.stale), static_cast<unsigned long long>(iterations),
				static_cast<unsigned long long>(result.wrong_exits),
				static_cast<unsigned long long>(vcpu.stats().instruction_cache_invalidations - invalidations));
			if (result.wrong_exits || (!mode.control && result.stale)) {
				std::fprintf(stderr, "FAIL: %s ran stale code\n", mode.name);
				++failures;
			}
			if (mode.control && !(ctr >> 29 & 1) && !result.stale) {
				std::fprintf(stderr, "FAIL: %s never ran stale code, so the test cannot tell\n", mode.name);
				++failures;
			}
		}
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		++failures;
	}
	if (failures)
		return 1;
	std::printf("hvf code write: PASS\n");
	return 0;
}
