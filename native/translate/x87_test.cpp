#include "runtime.h"
#include "x87_check.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace raven::translate;

namespace {

class Snippets final : public GuestMemory {
public:
	std::vector<uint8_t> bytes = std::vector<uint8_t>(4 << 20);
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

// Every case runs under the translator in this process and, through x87_oracle, on an x86-64 CPU or Rosetta; both must leave the same
// registers, flags, x87 state and memory.
int main(int argc, char **argv)
{
	if (argc != 3) {
		std::fprintf(stderr, "usage: x87_test ORACLE_RESULTS SILICON_RESULTS\n");
		return 2;
	}
	std::vector<x87_oracle::Case> cases = x87_oracle::read_cases(argv[1]);
	if (cases.empty()) {
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	if (x87_oracle::apply_silicon(cases, argv[2]) < 0) {
		std::fprintf(stderr, "%s does not match the cases in %s\n", argv[2], argv[1]);
		return 2;
	}
	// Both orderings this process can run.
	size_t failures = 0, run = 0;
	for (const MemoryOrdering ordering : {MemoryOrdering::rcpc, MemoryOrdering::barriers}) {
		Snippets memory;
		JitCodeCache cache(64 << 20);
		TranslatorOptions options;
		options.ordering = ordering;
		Runtime runtime(memory, cache, options);
		std::vector<uint8_t> stack(1 << 16);
		alignas(16) uint8_t buffer[256];
		x87_oracle::Report report;
		for (const x87_oracle::Case &item : cases) {
			if (!x87_oracle::runs(item)) {
				report.skip(item);
				continue;
			}
			const uint64_t address = memory.add(item.code);
			CpuState &state = runtime.state();
			x87_oracle::load(state, item.input);
			std::memcpy(buffer, item.input.buffer, sizeof buffer);
			state.gpr[rsp] = reinterpret_cast<uint64_t>(stack.data() + stack.size());
			state.gpr[r15] = reinterpret_cast<uint64_t>(buffer);
			const RunResult result = runtime.call(address);
			std::string problem;
			if (item.output.signal) {
				if (result.reason == ExitReason::returned)
					problem = "returned, x86 raised signal " + std::to_string(item.output.signal);
			} else if (result.reason != ExitReason::returned)
				problem = "did not return (exit " + std::to_string(static_cast<unsigned>(result.reason)) + ")";
			else
				problem = x87_oracle::compare(item, state, buffer);
			report.add(item, problem);
		}
		report.print(ordering == MemoryOrdering::rcpc ? "in process, LDAPR/STLR" : "in process, barriers");
		failures += report.failures;
		run += cases.size() - report.skipped;
	}
	std::printf("%zu cases run, %zu failures\n", run, failures);
	return failures == 0 ? 0 : 1;
}
