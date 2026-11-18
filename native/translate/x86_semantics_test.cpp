#include "runtime.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace raven::translate;

namespace {

struct Io {
	uint64_t registers[16];
	uint64_t flags;
	uint8_t buffer[64];
};

struct Case {
	std::vector<uint8_t> code;
	std::string text;
	uint64_t defined;
	Io input;
	Io output;
};

constexpr uint64_t CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128, OF = 2048;

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
	void watch_code(uint64_t, size_t, bool) override {}
	size_t watch_granule() const override { return 4096; }
};

// RFLAGS into the form the runtime keeps between runs, and back.
void set_flags(CpuState &state, uint64_t flags)
{
	state.nzcv = ((flags & SF) ? 1ull << 31 : 0) | ((flags & ZF) ? 1ull << 30 : 0) | ((flags & CF) ? 0 : 1ull << 29) |
		((flags & OF) ? 1ull << 28 : 0);
	// A low byte of even parity for PF; bit 4 of the xor with the adjust source for AF.
	state.parity_source = (flags & PF) ? 0 : 1;
	state.adjust_source = ((flags & AF) ? 16 : 0) ^ state.parity_source;
}

uint64_t get_flags(const CpuState &state)
{
	uint64_t flags = 0;
	flags |= (state.nzcv >> 31 & 1) ? SF : 0;
	flags |= (state.nzcv >> 30 & 1) ? ZF : 0;
	flags |= (state.nzcv >> 29 & 1) ? 0 : CF;
	flags |= (state.nzcv >> 28 & 1) ? OF : 0;
	flags |= __builtin_popcount(static_cast<unsigned>(state.parity_source & 0xff)) % 2 == 0 ? PF : 0;
	flags |= ((state.parity_source ^ state.adjust_source) & 16) ? AF : 0;
	return flags;
}

}

// Every case runs both under the translator and, through x86_semantics_oracle, on an x86-64 CPU or Rosetta; both must leave the same
// registers, memory and defined flags.
int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: x86_semantics_test ORACLE_RESULTS\n");
		return 2;
	}
	std::ifstream in(argv[1], std::ios::binary);
	auto get = [&](void *to, size_t size) { in.read(static_cast<char *>(to), static_cast<std::streamsize>(size)); };
	uint32_t count = 0;
	get(&count, 4);
	std::vector<Case> cases(count);
	for (Case &item : cases) {
		uint32_t length = 0, text_length = 0;
		get(&length, 4);
		item.code.resize(length);
		get(item.code.data(), length);
		get(&text_length, 4);
		item.text.resize(text_length);
		get(item.text.data(), text_length);
		get(&item.defined, 8);
		get(&item.input, sizeof(Io));
		get(&item.output, sizeof(Io));
	}
	if (!in || cases.empty()) {
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	Snippets memory;
	JitCodeCache cache(64 << 20);
	Runtime runtime(memory, cache);
	std::vector<uint8_t> stack(1 << 16);
	size_t failures = 0, reported = 0;
	for (const Case &item : cases) {
		const uint64_t address = memory.add(item.code);
		Io io = item.input;
		CpuState &state = runtime.state();
		std::memcpy(state.gpr, io.registers, sizeof state.gpr);
		state.gpr[rsp] = reinterpret_cast<uint64_t>(stack.data() + stack.size());
		state.gpr[r15] = reinterpret_cast<uint64_t>(io.buffer);
		set_flags(state, io.flags);
		const RunResult result = runtime.call(address);
		std::string problem;
		if (result.reason != ExitReason::returned)
			problem = "did not return (exit " + std::to_string(static_cast<unsigned>(result.reason)) + ")";
		for (unsigned r = 0; r < 16 && problem.empty(); ++r) {
			if (r == rsp || r == r15)
				continue;
			if (state.gpr[r] != item.output.registers[r]) {
				char text[128];
				std::snprintf(text, sizeof text, "register %u is %#llx, x86 left %#llx", r, static_cast<unsigned long long>(state.gpr[r]),
					static_cast<unsigned long long>(item.output.registers[r]));
				problem = text;
			}
		}
		if (problem.empty() && std::memcmp(io.buffer, item.output.buffer, 64) != 0)
			problem = "memory differs";
		const uint64_t flags = get_flags(state) & item.defined, expected = item.output.flags & item.defined;
		if (problem.empty() && flags != expected) {
			char text[96];
			std::snprintf(text, sizeof text, "flags %#llx, x86 left %#llx (defined %#llx)", static_cast<unsigned long long>(flags),
				static_cast<unsigned long long>(expected), static_cast<unsigned long long>(item.defined));
			problem = text;
		}
		if (!problem.empty()) {
			++failures;
			if (reported++ < 30)
				std::fprintf(stderr, "%s: %s\n", item.text.c_str(), problem.c_str());
		}
	}
	std::printf("%zu cases, %zu failures; %llu blocks, %llu guest instructions, %llu host words\n", cases.size(), failures,
		static_cast<unsigned long long>(runtime.translation_stats().blocks),
		static_cast<unsigned long long>(runtime.translation_stats().guest_instructions),
		static_cast<unsigned long long>(runtime.translation_stats().host_words));
	return failures == 0 ? 0 : 1;
}
