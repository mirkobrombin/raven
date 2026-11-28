#include "runtime.h"
#include "x87_check.h"

#include <array>
#include <chrono>
#include <map>
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

// The measurements of docs/macos/x87.md, not tests, in the x87_measure build (RAVEN_X87_DOUBLES): --doubles runs every case once with
// add, sub, mul, div and sqrt done in doubles (option b) and counts the failures by family and by precision control; --core-time
// times the core alone on the single-instruction arithmetic cases, exact and in doubles.
#if RAVEN_X87_DOUBLES
static int measure(std::vector<x87_oracle::Case> &cases, const std::string &what)
{
	if (what == "--core-time") {
		for (const bool doubles : {false, true}) {
			x87_double_arithmetic = doubles;
			Snippets memory;
			CpuState state;
			size_t instructions = 0;
			const auto start = std::chrono::steady_clock::now();
			for (const x87_oracle::Case &item : cases) {
				const Instruction instruction = decode(item.code, 0);
				if (item.family != "arithmetic" || instruction.length + 1u != item.code.size() || !x87_supported(instruction) ||
					instruction.operands[0].kind == OperandKind::mem)
					continue;
				for (int i = 0; i < 2000; ++i) {
					x87_oracle::load(state, item.input);
					x87_execute(state, memory, instruction, instruction.length);
				}
				instructions += 2000;
			}
			const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
			std::printf("core, %s: %zu instructions, %.1f ns each (the state reload included)\n", doubles ? "doubles" : "exact",
				instructions, ns / double(instructions));
		}
		x87_double_arithmetic = false;
		return 0;
	}
	x87_double_arithmetic = true;
	Snippets memory;
	JitCodeCache cache(64 << 20);
	Runtime runtime(memory, cache, TranslatorOptions{});
	std::vector<uint8_t> stack(1 << 16);
	alignas(16) uint8_t buffer[256];
	// Cases, those that differ, and those that differ in a value (a register or memory) rather than in the status word alone.
	std::map<std::string, std::array<size_t, 3>> by_precision;
	x87_oracle::Report report;
	for (const x87_oracle::Case &item : cases) {
		if (!x87_oracle::runs(item))
			continue;
		const uint64_t address = memory.add(item.code);
		CpuState &state = runtime.state();
		x87_oracle::load(state, item.input);
		std::memcpy(buffer, item.input.buffer, sizeof buffer);
		state.gpr[rsp] = reinterpret_cast<uint64_t>(stack.data() + stack.size());
		state.gpr[r15] = reinterpret_cast<uint64_t>(buffer);
		const RunResult result = runtime.call(address);
		const std::string problem = item.output.signal || result.reason != ExitReason::returned ? std::string() :
			x87_oracle::compare(item, state, buffer);
		report.add(item, problem);
		// The same with the status word taken as x86's: what differs in a register, memory or EFLAGS.
		CpuState values = state;
		values.x87.fsw = x87_oracle::word(item.output.fx + 2);
		const bool value_differs = !problem.empty() && !x87_oracle::compare(item, values, buffer).empty();
		static const char *const precisions[] = {"PC 24", "PC reserved", "PC 53", "PC 64"};
		auto &counts = by_precision[item.family + ", " + precisions[x87_oracle::word(item.input.fx) >> 8 & 3]];
		++counts[0];
		counts[1] += !problem.empty();
		counts[2] += value_differs;
	}
	x87_double_arithmetic = false;
	report.print("doubles");
	for (const auto &[key, counts] : by_precision)
		std::printf("doubles, %s: %zu cases, %zu differ from x86, %zu in a value\n", key.c_str(), counts[0], counts[1], counts[2]);
	return 0;
}
#endif

// Every case runs under the translator in this process and, through x87_oracle, on an x86-64 CPU or Rosetta; both must leave the same
// registers, flags, x87 state and memory.
int main(int argc, char **argv)
{
	if (argc != 3 && argc != 4) {
		std::fprintf(stderr, "usage: x87_test ORACLE_RESULTS SILICON_RESULTS [--doubles|--core-time]\n");
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
	if (argc == 4) {
#if RAVEN_X87_DOUBLES
		return measure(cases, argv[3]);
#else
		std::fprintf(stderr, "%s: the measurements are in x87_measure (RAVEN_X87_DOUBLES)\n", argv[3]);
		return 2;
#endif
	}
	// A fault from translated code (ud2) says so in exit_info, apart from the x87 core's #MF the cases check.
	{
		Snippets memory;
		JitCodeCache cache(1 << 20);
		Runtime runtime(memory, cache, TranslatorOptions{});
		std::vector<uint8_t> stack(1 << 12);
		runtime.state().gpr[rsp] = reinterpret_cast<uint64_t>(stack.data() + stack.size());
		runtime.state().exit_info = ~uint64_t(0);
		const RunResult result = runtime.call(memory.add({0x0f, 0x0b}));
		if (result.reason != ExitReason::fault || runtime.state().exit_info != uint64_t(FaultKind::instruction)) {
			std::fprintf(stderr, "FAIL: ud2 gave exit %u, exit_info %#llx\n", static_cast<unsigned>(result.reason),
				static_cast<unsigned long long>(runtime.state().exit_info));
			return 1;
		}
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
				// SIGFPE is #MF: the runtime stops at an instruction of the case with a fault.
				if (item.output.signal == 8 ? result.reason != ExitReason::fault || result.value < address ||
						result.value >= address + item.code.size() || state.exit_info != uint64_t(FaultKind::x87)
											: result.reason == ExitReason::returned)
					problem = "exit " + std::to_string(static_cast<unsigned>(result.reason)) + ", x86 raised signal " +
						std::to_string(item.output.signal);
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
