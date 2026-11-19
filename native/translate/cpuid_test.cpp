#include "cpuid.h"
#include "cpuid_check.h"
#include "runtime.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace raven::translate;

namespace {

class Snippets final : public GuestMemory {
public:
	std::vector<uint8_t> bytes = std::vector<uint8_t>(1 << 16);
	size_t used = 0;
	uint64_t add(const std::vector<uint8_t> &code)
	{
		const uint64_t address = reinterpret_cast<uint64_t>(bytes.data()) + used;
		std::memcpy(bytes.data() + used, code.data(), code.size());
		used += (code.size() + 15) & ~size_t(15);
		return address;
	}
	std::span<const uint8_t> code(uint64_t address) const override
	{
		const uint64_t start = reinterpret_cast<uint64_t>(bytes.data());
		if (address < start || address >= start + used)
			return {};
		return {reinterpret_cast<const uint8_t *>(address), static_cast<size_t>(start + used - address)};
	}
	bool is_native(uint64_t) const override { return false; }
	void write(uint64_t address, const void *from, size_t size) override { std::memcpy(reinterpret_cast<void *>(address), from, size); }
	void read(uint64_t address, void *into, size_t size) const override
	{
		std::memcpy(into, reinterpret_cast<const void *>(address), size);
	}
	void watch_code(uint64_t, size_t, bool) override {}
	size_t watch_granule() const override { return 4096; }
};

}

// CPUID and XGETBV as translated x64 code sees them in this process (cpuid_check.h has the rules).
int main()
{
	Snippets memory;
	JitCodeCache cache(4 << 20);
	Runtime runtime(memory, cache, TranslatorOptions{});
	std::vector<uint8_t> stack(1 << 14);
	const size_t failures = cpuid_check::check(
		[&](const std::vector<uint8_t> &code) {
			const uint64_t address = memory.add(code);
			CpuState &state = runtime.state();
			std::memset(state.gpr, 0, sizeof state.gpr);
			state.gpr[rsp] = reinterpret_cast<uint64_t>(stack.data() + stack.size());
			const RunResult result = runtime.call(address);
			return cpuid_check::Answer{result.reason, static_cast<uint32_t>(state.gpr[rax]), static_cast<uint32_t>(state.gpr[rbx]),
				static_cast<uint32_t>(state.gpr[rcx]), static_cast<uint32_t>(state.gpr[rdx])};
		},
		"in process");
	// The answers with every feature cpuid.h knows turned on must be as consistent (OSXSAVE, XCR0 and leaf 0xd with AVX), though
	// nothing translates their instructions yet: straight from the model, the leaf and subleaf read back from the code.
	GuestFeatures all;
	all.avx = all.avx2 = all.fma = all.f16c = all.bmi1 = all.bmi2 = true;
	const size_t model_failures = cpuid_check::check(
		[&](const std::vector<uint8_t> &code) {
			uint32_t leaf = 0, subleaf = 0;
			std::memcpy(&leaf, &code[1], 4);
			std::memcpy(&subleaf, &code[6], 4);
			if (code[10] == 0x0f && code[11] == 0x01) {
				const auto value = xgetbv(subleaf, all);
				return cpuid_check::Answer{value ? ExitReason::returned : ExitReason::fault, static_cast<uint32_t>(value.value_or(0)), 0,
					0, static_cast<uint32_t>(value.value_or(0) >> 32)};
			}
			const CpuidAnswer answer = cpuid(leaf, subleaf, all);
			return cpuid_check::Answer{ExitReason::returned, answer.eax, answer.ebx, answer.ecx, answer.edx};
		},
		"every feature on, model", false);
	std::printf("%zu failures, %zu with every feature on\n", failures, model_failures);
	return failures + model_failures == 0 ? 0 : 1;
}
