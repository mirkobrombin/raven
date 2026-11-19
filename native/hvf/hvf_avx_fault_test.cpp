// A 256-bit instruction whose memory operand faults must leave the guest as it was, as x86's precise exceptions do: no upper half of
// the destination written (it lives in CpuState, which the fault does not discard) and no byte of a 32-byte store. Each case runs in a
// vCPU of its own, since a guest exception ends the run: an operand straddles a mapped page and an unmapped one. vmaskmovps must not
// fault where the elements on the unmapped page are masked off (ymm2's sign bits), and must load or store the others; so must
// vpgatherdd, whose indices (ymm3) name the same 32 bytes, and which keeps the masked-off elements and clears the mask.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

using namespace raven;

namespace {

// The cases' x64 code in guest memory; nothing runs it but the translator, which reads it through the host alias.
class Snippets final : public translate::GuestMemory {
public:
	explicit Snippets(hvf::Vm &vm) : vm(vm), base(vm.map_anywhere(size, hvf::Access::read_write)), alias(vm.host(base)) {}
	uint64_t add(const std::vector<uint8_t> &code)
	{
		const uint64_t address = base + used;
		std::memcpy(alias + used, code.data(), code.size());
		used += (code.size() + 15) & ~size_t(15);
		return address;
	}
	std::span<const uint8_t> code(uint64_t address) const override
	{
		if (address < base || address >= base + used)
			return {};
		return {alias + (address - base), static_cast<size_t>(base + used - address)};
	}
	bool is_native(uint64_t) const override { return false; }
	void write(uint64_t address, const void *from, size_t bytes) override { vm.write(address, from, bytes); }
	void read(uint64_t address, void *into, size_t bytes) const override { vm.read(address, into, bytes); }
	void watch_code(uint64_t, size_t, bool) override {}
	size_t watch_granule() const override { return hvf::Vm::page_size; }

private:
	static constexpr size_t size = 1 << 20;
	hvf::Vm &vm;
	uint64_t base;
	uint8_t *alias;
	size_t used = 0;
};

}

struct Scenario {
	const char *text;
	std::vector<uint8_t> code;
	// The operand starts 16 bytes before the boundary between a mapped page and an unmapped one: the unmapped one comes first (its
	// low half faults) or second (its upper half faults).
	bool low_unmapped;
	// vmaskmovps: ymm2's low and upper halves (every single the same), and whether the access faults.
	uint64_t mask_low = 0, mask_high = 0;
	bool faults = true;
	bool gather = false;
};

int main()
{
	const Scenario scenarios[] = {
		{"vaddps ymm1, ymm1, ymmword ptr [rax]", {0xc5, 0xf4, 0x58, 0x08, 0xc3}, true},
		{"vfmadd231ps ymm1, ymm2, ymmword ptr [rax]", {0xc4, 0xe2, 0x6d, 0xb8, 0x08, 0xc3}, true},
		{"vmovups ymmword ptr [rax], ymm1", {0xc5, 0xfc, 0x11, 0x08, 0xc3}, false},
		{"vmaskmovps ymm1, ymm2, [rax], unmapped elements masked off", {0xc4, 0xe2, 0x6d, 0x2c, 0x08, 0xc3}, true, 0,
			0x8000000080000000ull, false},
		{"vmaskmovps [rax], ymm2, ymm1, unmapped elements masked off", {0xc4, 0xe2, 0x6d, 0x2e, 0x08, 0xc3}, false,
			0x8000000080000000ull, 0, false},
		{"vmaskmovps ymm1, ymm2, [rax], an unmapped element selected", {0xc4, 0xe2, 0x6d, 0x2c, 0x08, 0xc3}, true,
			0x8000000000000000ull, 0x8000000080000000ull},
		{"vmaskmovps [rax], ymm2, ymm1, an unmapped element selected", {0xc4, 0xe2, 0x6d, 0x2e, 0x08, 0xc3}, false,
			0x8000000080000000ull, 0x0000000080000000ull},
		{"vpgatherdd ymm1, [rax + 4*ymm3], ymm2, unmapped elements masked off", {0xc4, 0xe2, 0x6d, 0x90, 0x0c, 0x98, 0xc3}, true, 0,
			0x8000000080000000ull, false, true},
		{"vpgatherdd ymm1, [rax + 4*ymm3], ymm2, an unmapped element selected", {0xc4, 0xe2, 0x6d, 0x90, 0x0c, 0x98, 0xc3}, true,
			0x8000000000000000ull, 0x8000000080000000ull, true, true},
	};
	constexpr uint64_t base = uint64_t(0x60) << 32;
	size_t failures = 0;
	for (const Scenario &scenario : scenarios) {
		try {
			hvf::Vm vm(64 << 20);
			Snippets memory(vm);
			const uint64_t stack = vm.map_anywhere(64 << 10, hvf::Access::read_write);
			// Mapped: one page; the page before it (low_unmapped) or after it stays unmapped.
			const uint64_t page = scenario.low_unmapped ? base + hvf::Vm::page_size : base;
			uint8_t *alias = vm.map(page, hvf::Vm::page_size, hvf::Access::read_write);
			std::memset(alias, 0x5a, hvf::Vm::page_size);
			const uint64_t operand = scenario.low_unmapped ? page - 16 : page + hvf::Vm::page_size - 16;
			translate::TranslatorOptions options;
			options.ordering = translate::MemoryOrdering::hardware;
			hvf::VmCodeCache cache(vm, 16 << 20, 1 << 20);
			hvf::Vcpu vcpu(vm, true);
			translate::Runtime runtime(memory, cache, options, &vcpu);
			const uint64_t address = memory.add(scenario.code);
			translate::CpuState &state = runtime.state();
			state.gpr[translate::rsp] = stack + (64 << 10);
			state.gpr[translate::rax] = operand;
			for (unsigned r = 0; r < 16; ++r) {
				state.xmm[r][0] = state.xmm[r][1] = 0x3f8000003f800000ull;
				state.ymm_high[r][0] = state.ymm_high[r][1] = 0x4000000040000000ull;
			}
			state.xmm[2][0] = state.xmm[2][1] = scenario.mask_low;
			state.ymm_high[2][0] = state.ymm_high[2][1] = scenario.mask_high;
			// vpgatherdd's indices: element i at 4 * i.
			state.xmm[3][0] = uint64_t(1) << 32;
			state.xmm[3][1] = uint64_t(3) << 32 | 2;
			state.ymm_high[3][0] = uint64_t(5) << 32 | 4;
			state.ymm_high[3][1] = uint64_t(7) << 32 | 6;
			bool faulted = false;
			try {
				runtime.call(address);
			} catch (const std::exception &) {
				faulted = true;
			}
			if (!scenario.faults && scenario.gather) {
				// The masked-off elements keep their value, the others come from memory, and the mask is all clear.
				const bool right = !faulted && state.xmm[1][0] == 0x3f8000003f800000ull && state.xmm[1][1] == 0x3f8000003f800000ull &&
					state.ymm_high[1][0] == 0x5a5a5a5a5a5a5a5aull && state.ymm_high[1][1] == 0x5a5a5a5a5a5a5a5aull &&
					state.xmm[2][0] == 0 && state.xmm[2][1] == 0 && state.ymm_high[2][0] == 0 && state.ymm_high[2][1] == 0;
				if (!right) {
					std::fprintf(stderr, "FAIL: %s: %s\n", scenario.text, faulted ? "faulted" : "wrong result");
					++failures;
				} else
					std::printf("%s: did not fault, gathered the selected elements and cleared the mask\n", scenario.text);
				continue;
			}
			if (!scenario.faults) {
				// The masked-off elements of a load are zero, the others what memory holds; a store writes the 16 mapped bytes alone.
				const bool load = scenario.low_unmapped;
				bool right = !faulted;
				if (load)
					right &= state.xmm[1][0] == 0 && state.xmm[1][1] == 0 && state.ymm_high[1][0] == 0x5a5a5a5a5a5a5a5aull &&
						state.ymm_high[1][1] == 0x5a5a5a5a5a5a5a5aull;
				for (size_t i = 0; i < hvf::Vm::page_size; ++i) {
					const bool stored = !load && i >= hvf::Vm::page_size - 16;
					right &= alias[i] == (stored ? (i % 4 == 3 ? 0x3f : i % 4 == 2 ? 0x80 : 0) : 0x5a);
				}
				if (!right) {
					std::fprintf(stderr, "FAIL: %s: %s\n", scenario.text, faulted ? "faulted" : "wrong result");
					++failures;
				} else
					std::printf("%s: did not fault, and moved the selected elements\n", scenario.text);
				continue;
			}
			bool high_kept = true;
			for (unsigned r = 0; r < 16; ++r)
				high_kept &= r == 2 || r == 3 || (state.ymm_high[r][0] == 0x4000000040000000ull && state.ymm_high[r][1] == 0x4000000040000000ull);
			bool memory_kept = true;
			for (size_t i = 0; i < hvf::Vm::page_size; ++i)
				memory_kept &= alias[i] == 0x5a;
			if (!faulted || !high_kept || !memory_kept) {
				std::fprintf(stderr, "FAIL: %s: %s%s%s\n", scenario.text, faulted ? "" : "did not fault; ",
					high_kept ? "" : "an upper half changed before the fault; ", memory_kept ? "" : "memory was written before the fault");
				++failures;
			} else
				std::printf("%s: faulted with the upper halves and memory as they were\n", scenario.text);
		} catch (const std::exception &error) {
			std::fprintf(stderr, "FAIL: %s: %s\n", scenario.text, error.what());
			++failures;
		}
	}
	std::printf("%zu failures\n", failures);
	return failures == 0 ? 0 : 1;
}
