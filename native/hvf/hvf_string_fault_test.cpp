// rep stos and rep movs that run into an unmapped page fault part way, as on x86: RCX, RDI and RSI show the elements done, which are
// in memory, and nothing after them is. Forward and backward (DF set), a load (movs) and a store fault; and rep with RCX 0 at an
// unmapped address does not fault at all. Each case runs in a vCPU of its own, since a guest exception ends the run.
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

struct Scenario {
	const char *text;
	std::vector<uint8_t> code;
	bool backward;
	// RDI and RSI as offsets from the mapped page's start (negative: below it, where nothing is mapped), and RCX.
	int64_t rdi, rsi;
	uint64_t rcx;
	bool faults;
	// What RCX, RDI and RSI must be after (RDI and RSI as offsets), and the bytes written from rdi_written for written of them.
	uint64_t rcx_after;
	int64_t rdi_after, rsi_after;
	int64_t written_at;
	unsigned written;
};

}

int main()
{
	constexpr int64_t page = hvf::Vm::page_size;
	const std::vector<Scenario> scenarios = {
		{"rep stosb, 5 bytes before an unmapped page, rcx 100", {0xf3, 0xaa, 0xc3}, false, page - 5, 0, 100, true, 95, page, 0,
			page - 5, 5},
		{"rep stosq, 16 bytes before an unmapped page, rcx 10", {0xf3, 0x48, 0xab, 0xc3}, false, page - 16, 0, 10, true, 8, page, 0,
			page - 16, 16},
		{"rep movsb, the source 3 bytes before an unmapped page, rcx 10", {0xf3, 0xa4, 0xc3}, false, 64, page - 3, 10, true, 7, 67,
			page, 64, 3},
		{"rep stosw backward (std), from 4 bytes above an unmapped page, rcx 10", {0x66, 0xf3, 0xab, 0xc3}, true, 4, 0, 10, true, 7,
			-2, 0, 0, 6},
		{"rep movsd backward (std), the destination 8 bytes above an unmapped page, rcx 5", {0xf3, 0xa5, 0xc3}, true, 8, 512, 5, true,
			2, -4, 500, 0, 12},
		{"rep stosb with rcx 0 at an unmapped address", {0xf3, 0xaa, 0xc3}, false, -64, 0, 0, false, 0, -64, 0, 0, 0},
	};
	const uint64_t base = uint64_t(0x60) << 32;
	size_t failures = 0;
	for (const Scenario &scenario : scenarios) {
		try {
			hvf::Vm vm(64 << 20);
			Snippets memory(vm);
			const uint64_t stack = vm.map_anywhere(64 << 10, hvf::Access::read_write);
			// One mapped page at base + page, the pages on both sides unmapped.
			const uint64_t start = base + page;
			uint8_t *alias = vm.map(start, page, hvf::Access::read_write);
			std::memset(alias, 0x5a, page);
			for (int64_t i = 0; i < 64; ++i)
				alias[page - 64 + i] = alias[512 - 64 + i] = static_cast<uint8_t>(i + 1);
			translate::TranslatorOptions options;
			options.ordering = translate::MemoryOrdering::hardware;
			hvf::VmCodeCache cache(vm, 16 << 20, 1 << 20);
			hvf::Vcpu vcpu(vm, true);
			translate::Runtime runtime(memory, cache, options, &vcpu);
			const uint64_t address = memory.add(scenario.code);
			translate::CpuState &state = runtime.state();
			state.gpr[translate::rsp] = stack + (64 << 10);
			state.gpr[translate::rax] = 0xa1a2a3a4a5a6a7a8ull;
			state.gpr[translate::rdi] = start + static_cast<uint64_t>(scenario.rdi);
			state.gpr[translate::rsi] = start + static_cast<uint64_t>(scenario.rsi);
			state.gpr[translate::rcx] = scenario.rcx;
			state.direction = scenario.backward;
			bool faulted = false;
			uint64_t rcx = 0, rdi = 0, rsi = 0;
			try {
				runtime.call(address);
				rcx = state.gpr[translate::rcx];
				rdi = state.gpr[translate::rdi];
				rsi = state.gpr[translate::rsi];
			} catch (const std::exception &) {
				faulted = true;
				// The registers as the faulting access left them, in the vCPU (translator.cpp's mapping: RCX x0, RSI x25, RDI x26).
				rcx = vcpu.reg(HV_REG_X0);
				rsi = vcpu.reg(HV_REG_X25);
				rdi = vcpu.reg(HV_REG_X26);
			}
			bool right = faulted == scenario.faults && rcx == scenario.rcx_after &&
				rdi == start + static_cast<uint64_t>(scenario.rdi_after);
			if (scenario.code[1] == 0xa4 || scenario.code[1] == 0xa5)
				right = right && rsi == start + static_cast<uint64_t>(scenario.rsi_after);
			// The elements done are in memory, and the bytes around them are as they were.
			std::vector<uint8_t> expected(alias, alias + page);
			if (scenario.written) {
				std::vector<uint8_t> before(page);
				std::memset(before.data(), 0x5a, page);
				for (int64_t i = 0; i < 64; ++i)
					before[size_t(page - 64 + i)] = before[size_t(512 - 64 + i)] = static_cast<uint8_t>(i + 1);
				for (unsigned i = 0; i < scenario.written; ++i) {
					const int64_t at = scenario.written_at + i;
					const bool movs = scenario.code[1] == 0xa4 || scenario.code[1] == 0xa5;
					const int64_t from = movs ? scenario.rsi + (at - scenario.rdi) : 0;
					before[size_t(at)] = movs ? before[size_t(from)] : static_cast<uint8_t>(0xa1a2a3a4a5a6a7a8ull >> (8 * ((at - scenario.written_at) % (scenario.code[1] == 0x48 ? 8 : scenario.code[0] == 0x66 ? 2 : 1))));
				}
				right = right && std::memcmp(before.data(), alias, page) == 0;
			}
			std::printf("%s: %s, rcx %llu, rdi %+lld, rsi %+lld%s\n", scenario.text, faulted ? "faulted" : "returned",
				static_cast<unsigned long long>(rcx), static_cast<long long>(rdi - start), static_cast<long long>(rsi - start),
				right ? "" : " (FAIL)");
			failures += !right;
		} catch (const std::exception &error) {
			std::fprintf(stderr, "FAIL: %s: %s\n", scenario.text, error.what());
			++failures;
		}
	}
	return failures == 0 ? 0 : 1;
}
