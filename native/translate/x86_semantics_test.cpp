#include "runtime.h"
#include "x86_semantics_check.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace raven::translate;

namespace {

class Snippets final : public GuestMemory {
public:
	std::vector<uint8_t> bytes = std::vector<uint8_t>(1 << 20);
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

// Every case runs under the translator in this process and, through x86_semantics_oracle, on Rosetta, held to an x86 CPU where the
// two differ (testdata/x86_silicon.txt); both must leave the same registers, defined flags and memory.
int main(int argc, char **argv)
{
	if (argc != 3) {
		std::fprintf(stderr, "usage: x86_semantics_test ORACLE_RESULTS SILICON_RESULTS\n");
		return 2;
	}
	std::vector<integer_oracle::Case> cases = integer_oracle::read_cases(argv[1]);
	if (cases.empty()) {
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	if (integer_oracle::apply_silicon(cases, argv[2]) < 0) {
		std::fprintf(stderr, "cannot apply %s\n", argv[2]);
		return 2;
	}
	// Both orderings this process can run: their memory accesses take different instructions.
	size_t failures = 0;
	for (const MemoryOrdering ordering : {MemoryOrdering::rcpc, MemoryOrdering::barriers}) {
		Snippets memory;
		JitCodeCache cache(64 << 20);
		TranslatorOptions options;
		options.ordering = ordering;
		Runtime runtime(memory, cache, options);
		std::vector<uint8_t> stack(1 << 16);
		uint8_t buffer[64];
		integer_oracle::Report report;
		report.first = cases.data();
		for (const integer_oracle::Case &item : cases) {
			const uint64_t address = memory.add(item.code);
			CpuState &state = runtime.state();
			integer_oracle::load(state, item.input);
			std::memcpy(buffer, item.input.buffer, sizeof buffer);
			state.gpr[rsp] = reinterpret_cast<uint64_t>(stack.data() + stack.size());
			state.gpr[r15] = reinterpret_cast<uint64_t>(buffer);
			const RunResult result = runtime.call(address);
			std::string problem;
			if (result.reason != ExitReason::returned)
				problem = "did not return (exit " + std::to_string(static_cast<unsigned>(result.reason)) + ")";
			else
				problem = integer_oracle::compare(item, state, buffer);
			report.add(item, problem);
		}
		report.print(ordering == MemoryOrdering::rcpc ? "in process, LDAPR/STLR" : "in process, barriers");
		std::printf("%llu blocks, %llu guest instructions, %llu host words\n",
			static_cast<unsigned long long>(runtime.translation_stats().blocks),
			static_cast<unsigned long long>(runtime.translation_stats().guest_instructions),
			static_cast<unsigned long long>(runtime.translation_stats().host_words));
		failures += report.failures;
	}
	std::printf("%zu cases, %zu failures\n", cases.size() * 2, failures);
	return failures == 0 ? 0 : 1;
}
