#pragma once

// The SSE oracle's cases (x86_sse.py, run on x86 by x86_sse_oracle.c) and how the translator's run of one compares with x86's: shared
// by the in-process test (x86_sse_test) and the vCPU one (hvf_sse_test).

#include "cpu_state.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace raven::translate::sse_oracle {

struct Io {
	uint64_t registers[16];
	uint64_t flags;
	uint32_t mxcsr;
	uint32_t reserved;
	uint64_t xmm[16][2];
	uint8_t buffer[256];
};
static_assert(offsetof(Io, xmm) == 144 && offsetof(Io, buffer) == 400, "x86_sse.py's run_case offsets");

struct Case {
	std::vector<uint8_t> code;
	std::string text;
	std::string family;
	Io input;
	Io output;
};

constexpr uint64_t CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128, OF = 2048;
constexpr uint64_t all_flags = CF | PF | AF | ZF | SF | OF;
// MXCSR.DE, the denormal-operand flag: Arm has no such flag (FPSR.IDC reports only inputs it flushed), so the translator leaves DE as
// it was and the tests count where x86 set it apart from the failures.
constexpr uint32_t mxcsr_de = 2;

inline std::vector<Case> read_cases(const char *path)
{
	std::ifstream in(path, std::ios::binary);
	auto get = [&](void *to, size_t size) { in.read(static_cast<char *>(to), static_cast<std::streamsize>(size)); };
	auto get_string = [&](std::string &text) {
		uint32_t length = 0;
		get(&length, 4);
		text.resize(length);
		get(text.data(), length);
	};
	uint32_t count = 0;
	get(&count, 4);
	std::vector<Case> cases(in ? count : 0);
	for (Case &item : cases) {
		uint32_t length = 0;
		get(&length, 4);
		item.code.resize(length);
		get(item.code.data(), length);
		get_string(item.text);
		get_string(item.family);
		get(&item.input, sizeof(Io));
		get(&item.output, sizeof(Io));
	}
	if (!in)
		cases.clear();
	return cases;
}

// A case's registers, flags, XMM registers and MXCSR into state, in the form the runtime keeps between runs. RSP and R15 are the
// caller's: the stack and the buffer.
inline void load(CpuState &state, const Io &input)
{
	std::memcpy(state.gpr, input.registers, sizeof state.gpr);
	const uint64_t flags = input.flags;
	state.nzcv = ((flags & SF) ? 1ull << 31 : 0) | ((flags & ZF) ? 1ull << 30 : 0) | ((flags & CF) ? 0 : 1ull << 29) |
		((flags & OF) ? 1ull << 28 : 0);
	// A low byte of even parity for PF; bit 4 of the xor with the adjust source for AF.
	state.parity_source = (flags & PF) ? 0 : 1;
	state.adjust_source = ((flags & AF) ? 16 : 0) ^ state.parity_source;
	std::memcpy(state.xmm, input.xmm, sizeof state.xmm);
	state.mxcsr = input.mxcsr;
}

inline uint64_t flags_of(const CpuState &state)
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

// What the translator's run left that differs from x86's, or nothing: every general register but RSP and R15, every XMM register,
// the flags, MXCSR and the buffer.
inline std::string compare(const Case &item, const CpuState &state, const uint8_t *buffer)
{
	char text[160];
	const Io &x86 = item.output;
	for (unsigned r = 0; r < 16; ++r) {
		if (r == rsp || r == r15 || state.gpr[r] == x86.registers[r])
			continue;
		std::snprintf(text, sizeof text, "register %u is %#llx, x86 left %#llx", r, static_cast<unsigned long long>(state.gpr[r]),
			static_cast<unsigned long long>(x86.registers[r]));
		return text;
	}
	for (unsigned r = 0; r < 16; ++r) {
		if (state.xmm[r][0] == x86.xmm[r][0] && state.xmm[r][1] == x86.xmm[r][1])
			continue;
		std::snprintf(text, sizeof text, "xmm%u is %016llx%016llx, x86 left %016llx%016llx", r,
			static_cast<unsigned long long>(state.xmm[r][1]), static_cast<unsigned long long>(state.xmm[r][0]),
			static_cast<unsigned long long>(x86.xmm[r][1]), static_cast<unsigned long long>(x86.xmm[r][0]));
		return text;
	}
	if (flags_of(state) != (x86.flags & all_flags)) {
		std::snprintf(text, sizeof text, "flags %#llx, x86 left %#llx", static_cast<unsigned long long>(flags_of(state)),
			static_cast<unsigned long long>(x86.flags & all_flags));
		return text;
	}
	if ((state.mxcsr ^ x86.mxcsr) & ~mxcsr_de) {
		std::snprintf(text, sizeof text, "mxcsr %#x, x86 left %#x", state.mxcsr, x86.mxcsr);
		return text;
	}
	// The same for an MXCSR stmxcsr stored in the buffer.
	unsigned stored = sizeof x86.buffer;
	const size_t at = item.text.find("stmxcsr dword ptr [r15 + ");
	if (at != std::string::npos)
		stored = static_cast<unsigned>(std::stoul(item.text.substr(at + 25)));
	for (unsigned b = 0; b < sizeof x86.buffer; ++b)
		if (buffer[b] != x86.buffer[b] && !(b == stored && (buffer[b] ^ x86.buffer[b]) == mxcsr_de)) {
			std::snprintf(text, sizeof text, "buffer byte %u is %#x, x86 left %#x", b, buffer[b], x86.buffer[b]);
			return text;
		}
	return {};
}

// Failures by family, and the first few in full.
struct Report {
	std::map<std::string, std::pair<size_t, size_t>> families;
	size_t failures = 0;
	size_t printed = 0;
	// Cases that passed with only MXCSR.DE differing.
	size_t denormal_flags = 0;
	void add(const Case &item, const std::string &problem, const CpuState &state)
	{
		if (problem.empty() && ((state.mxcsr ^ item.output.mxcsr) & mxcsr_de))
			++denormal_flags;
		auto &[cases, failed] = families[item.family];
		++cases;
		if (problem.empty())
			return;
		++failed;
		++failures;
		if (printed++ < 40)
			std::fprintf(stderr, "%s: %s: %s\n", item.family.c_str(), item.text.c_str(), problem.c_str());
	}
	void print(const char *where) const
	{
		for (const auto &[family, counts] : families)
			std::printf("%s, %s: %zu cases, %zu failures\n", where, family.c_str(), counts.first, counts.second);
		std::printf("%s: MXCSR.DE alone differs in %zu passing cases\n", where, denormal_flags);
	}
};

}
