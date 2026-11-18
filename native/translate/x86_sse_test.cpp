#include "runtime.h"
#include "x86_sse_check.h"

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
	void watch_code(uint64_t, size_t, bool) override {}
	size_t watch_granule() const override { return 4096; }
};

}

// Every case runs under the translator in this process and, through x86_sse_oracle, on an x86-64 CPU or Rosetta; both must leave the
// same registers, XMM registers, flags, MXCSR and memory.
int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: x86_sse_test ORACLE_RESULTS\n");
		return 2;
	}
	const std::vector<sse_oracle::Case> cases = sse_oracle::read_cases(argv[1]);
	if (cases.empty()) {
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	Snippets memory;
	JitCodeCache cache(64 << 20);
	Runtime runtime(memory, cache);
	std::vector<uint8_t> stack(1 << 16);
	alignas(16) uint8_t buffer[256];
	sse_oracle::Report report;
	for (const sse_oracle::Case &item : cases) {
		const uint64_t address = memory.add(item.code);
		CpuState &state = runtime.state();
		sse_oracle::load(state, item.input);
		std::memcpy(buffer, item.input.buffer, sizeof buffer);
		state.gpr[rsp] = reinterpret_cast<uint64_t>(stack.data() + stack.size());
		state.gpr[r15] = reinterpret_cast<uint64_t>(buffer);
		const RunResult result = runtime.call(address);
		std::string problem;
		if (result.reason != ExitReason::returned)
			problem = "did not return (exit " + std::to_string(static_cast<unsigned>(result.reason)) + ")";
		else
			problem = sse_oracle::compare(item, state, buffer);
		report.add(item, problem, state);
	}
	report.print("in process");
	std::printf("%zu cases, %zu failures\n", cases.size(), report.failures);
	return report.failures == 0 ? 0 : 1;
}
