#pragma once

// The x87 oracle's cases (x87.py, run on x86 by x87_oracle.c) and how the translator's run of one compares with x86's: shared by the
// in-process test (x87_test) and the vCPU one (hvf_x87_test). The build runs the oracle under Rosetta; testdata/x87_silicon.txt holds
// what an x86 CPU left where Rosetta disagrees with it, and replaces Rosetta's result there.

#include "cpu_state.h"
#include "x86_decoder.h"
#include "x87.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace raven::translate::x87_oracle {

struct Io {
	uint64_t registers[16];
	uint64_t flags;
	// In an output: the signal the case raised on x86, 0 when it returned.
	uint64_t signal;
	alignas(16) uint8_t fx[512];
	uint8_t buffer[256];
};
static_assert(offsetof(Io, fx) == 144 && offsetof(Io, buffer) == 656, "x87.py's run_case offsets");

struct Case {
	std::vector<uint8_t> code;
	std::string text;
	std::string family;
	Io input;
	Io output;
	bool silicon = false;
	size_t number = 0;
};

constexpr uint64_t CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128, OF = 2048;
constexpr uint64_t all_flags = CF | PF | AF | ZF | SF | OF;

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
		item.number = static_cast<size_t>(&item - cases.data());
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

// silicon_overrides.py's fingerprint of an output: FNV-1a 64 over the compared bytes, the others zeroed.
inline uint64_t fingerprint(const Case &item)
{
	Io io = item.output;
	io.registers[rsp] = io.registers[r15] = 0;
	io.flags &= 0x8d5;
	std::memset(io.fx + 6, 0, 18);
	for (unsigned i = 0; i < 8; ++i)
		std::memset(io.fx + 42 + 16 * i, 0, 6);
	std::memset(io.fx + 160, 0, sizeof io.fx - 160);
	static const std::regex image(R"((fnstenv|fnsave) \[r15(?: \+ (\d+))?\])");
	std::smatch match;
	if (std::regex_search(item.text, match, image)) {
		const size_t at = match[2].matched ? std::stoul(match[2]) : 0;
		std::memset(io.buffer + at + 12, 0, 14);
	}
	const auto *bytes = reinterpret_cast<const uint8_t *>(&io);
	uint64_t value = 0xcbf29ce484222325;
	for (size_t i = 0; i < sizeof io; ++i)
		value = (value ^ bytes[i]) * 0x100000001b3;
	return value;
}

// Applies the silicon results: one line per case, its number, its text, the fingerprint of Rosetta's result it was written against and
// field=hex pairs (docs/macos/x87-data/silicon_overrides.py says which). Returns how many cases changed, or -1 when a line does not
// match the cases or Rosetta's result for its case is no longer the one the line was written against (the file is then stale).
inline long apply_silicon(std::vector<Case> &cases, const char *path)
{
	std::ifstream in(path);
	if (!in)
		return -1;
	auto bytes_of = [](const std::string &hex) {
		std::vector<uint8_t> bytes;
		for (size_t i = 0; i + 1 < hex.size(); i += 2)
			bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
		return bytes;
	};
	long changed = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		std::vector<std::string> parts;
		std::stringstream stream(line);
		for (std::string part; std::getline(stream, part, '\t');)
			parts.push_back(part);
		if (parts.size() < 3)
			return -1;
		const size_t number = std::stoul(parts[0]);
		if (number >= cases.size() || cases[number].text != parts[1])
			return -1;
		if (parts[2].rfind("rosetta=", 0) != 0 || std::stoull(parts[2].substr(8), nullptr, 16) != fingerprint(cases[number])) {
			std::fprintf(stderr, "case %zu (%s): Rosetta's result is not the one the silicon results were written against\n", number,
				parts[1].c_str());
			return -1;
		}
		Io &io = cases[number].output;
		for (size_t i = 3; i < parts.size(); ++i) {
			const size_t equals = parts[i].find('=');
			const std::string name = parts[i].substr(0, equals);
			const std::vector<uint8_t> value = bytes_of(parts[i].substr(equals + 1));
			uint8_t *to = nullptr;
			if (name[0] == 'r')
				to = reinterpret_cast<uint8_t *>(&io.registers[std::stoul(name.substr(1))]);
			else if (name == "flags")
				to = reinterpret_cast<uint8_t *>(&io.flags);
			else if (name == "signal")
				to = reinterpret_cast<uint8_t *>(&io.signal);
			else if (name == "fcw")
				to = io.fx;
			else if (name == "fsw")
				to = io.fx + 2;
			else if (name == "ftw")
				to = io.fx + 4;
			else if (name.rfind("st", 0) == 0)
				to = io.fx + 32 + 16 * std::stoul(name.substr(2));
			else if (name[0] == 'b')
				to = io.buffer + std::stoul(name.substr(1));
			if (!to)
				return -1;
			std::memcpy(to, value.data(), value.size());
		}
		cases[number].silicon = true;
		++changed;
	}
	return changed;
}

inline uint16_t word(const uint8_t *at)
{
	uint16_t value;
	std::memcpy(&value, at, 2);
	return value;
}

// A case's registers, flags and x87 state into state, in the form the runtime keeps between runs. The image holds ST(0)-ST(7);
// CpuState holds the physical registers, ST(i) being R((TOP + i) mod 8). RSP and R15 are the caller's: the stack and the buffer.
inline void load(CpuState &state, const Io &input)
{
	std::memcpy(state.gpr, input.registers, sizeof state.gpr);
	const uint64_t flags = input.flags;
	state.nzcv = ((flags & SF) ? 1ull << 31 : 0) | ((flags & ZF) ? 1ull << 30 : 0) | ((flags & CF) ? 0 : 1ull << 29) |
		((flags & OF) ? 1ull << 28 : 0);
	state.parity_source = (flags & PF) ? 0 : 1;
	state.adjust_source = ((flags & AF) ? 16 : 0) ^ state.parity_source;
	X87State &x87 = state.x87;
	x87 = X87State{};
	x87.fcw = word(input.fx);
	x87.fsw = word(input.fx + 2);
	x87.ftw = input.fx[4];
	const unsigned top = x87.fsw >> 11 & 7;
	for (unsigned i = 0; i < 8; ++i)
		std::memcpy(x87.st[(top + i) & 7], input.fx + 32 + 16 * i, 10);
	std::memcpy(&state.mxcsr, input.fx + 24, 4);
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

inline std::string hex80(const uint8_t *value)
{
	char text[32];
	std::snprintf(text, sizeof text, "%02x%02x:%02x%02x%02x%02x%02x%02x%02x%02x", value[9], value[8], value[7], value[6], value[5],
		value[4], value[3], value[2], value[1], value[0]);
	return text;
}

// What the translator's run left that differs from x86's, or nothing: every general register but RSP and R15, the flags, FCW, FSW,
// the tags, all eight registers (80 bits, empty ones too), MXCSR and the buffer. The instruction and data pointers and the opcode
// are not compared, here or in an image fnstenv or fnsave stored: x86 CPUs differ in what they keep there.
inline std::string compare(const Case &item, const CpuState &state, const uint8_t *buffer)
{
	char text[200];
	const Io &x86 = item.output;
	for (unsigned r = 0; r < 16; ++r) {
		if (r == rsp || r == r15 || state.gpr[r] == x86.registers[r])
			continue;
		std::snprintf(text, sizeof text, "register %u is %#llx, x86 left %#llx", r, static_cast<unsigned long long>(state.gpr[r]),
			static_cast<unsigned long long>(x86.registers[r]));
		return text;
	}
	if (flags_of(state) != (x86.flags & all_flags)) {
		std::snprintf(text, sizeof text, "flags %#llx, x86 left %#llx", static_cast<unsigned long long>(flags_of(state)),
			static_cast<unsigned long long>(x86.flags & all_flags));
		return text;
	}
	const X87State &x87 = state.x87;
	if (x87.fcw != word(x86.fx) || x87.fsw != word(x86.fx + 2) || x87.ftw != x86.fx[4]) {
		std::snprintf(text, sizeof text, "fcw %#x fsw %#x ftw %#x, x86 left %#x %#x %#x", x87.fcw, x87.fsw, x87.ftw, word(x86.fx),
			word(x86.fx + 2), x86.fx[4]);
		return text;
	}
	const unsigned top = x87.fsw >> 11 & 7;
	for (unsigned i = 0; i < 8; ++i) {
		const uint8_t *mine = reinterpret_cast<const uint8_t *>(x87.st[(top + i) & 7]);
		if (std::memcmp(mine, x86.fx + 32 + 16 * i, 10) == 0)
			continue;
		std::snprintf(text, sizeof text, "st(%u) is %s, x86 left %s", i, hex80(mine).c_str(), hex80(x86.fx + 32 + 16 * i).c_str());
		return text;
	}
	uint32_t mxcsr;
	std::memcpy(&mxcsr, x86.fx + 24, 4);
	if (state.mxcsr != mxcsr) {
		std::snprintf(text, sizeof text, "mxcsr %#x, x86 left %#x", state.mxcsr, mxcsr);
		return text;
	}
	// fnstenv and fnsave: bytes 12-25 of the image are the pointers, the code selector and the opcode.
	unsigned skip_from = sizeof x86.buffer, skip_to = 0;
	static const std::regex image(R"((fnstenv|fnsave) \[r15(?: \+ (\d+))?\])");
	std::smatch match;
	if (std::regex_search(item.text, match, image)) {
		skip_from = (match[2].matched ? static_cast<unsigned>(std::stoul(match[2])) : 0) + 12;
		skip_to = skip_from + 14;
	}
	for (unsigned b = 0; b < sizeof x86.buffer; ++b)
		if (buffer[b] != x86.buffer[b] && !(b >= skip_from && b < skip_to)) {
			std::snprintf(text, sizeof text, "buffer byte %u is %#x, x86 left %#x", b, buffer[b], x86.buffer[b]);
			return text;
		}
	return {};
}

// Whether the core runs every instruction of the case; the others (the transcendental instructions, docs/macos/x87.md) are counted
// as not run rather than failed.
inline bool runs(const Case &item)
{
	size_t at = 0;
	while (at < item.code.size()) {
		const Instruction instruction = decode(std::span(item.code).subspan(at), at);
		if (instruction.mnemonic >= x87_first && instruction.mnemonic <= x87_last && !x87_supported(instruction))
			return false;
		if (instruction.mnemonic == Mnemonic::invalid)
			return true;
		at += instruction.length;
	}
	return true;
}

// Failures by family, and the first few of each in full.
struct Report {
	std::map<std::string, std::pair<size_t, size_t>> families;
	size_t failures = 0;
	size_t silicon = 0;
	std::map<std::string, size_t> not_run;
	void add(const Case &item, const std::string &problem)
	{
		auto &[cases, failed] = families[item.family];
		++cases;
		silicon += item.silicon;
		if (problem.empty())
			return;
		++failed;
		++failures;
		if (failed <= 12)
			std::fprintf(stderr, "%s: #%zu %s: %s\n", item.family.c_str(), item.number, item.text.c_str(), problem.c_str());
	}
	size_t skipped = 0;
	void skip(const Case &item)
	{
		++not_run[item.family];
		++skipped;
	}
	void print(const char *where) const
	{
		for (const auto &[family, counts] : families)
			std::printf("%s, %s: %zu cases, %zu failures\n", where, family.c_str(), counts.first, counts.second);
		for (const auto &[family, count] : not_run)
			std::printf("%s, %s: %zu cases not run, their instructions are not implemented\n", where, family.c_str(), count);
		std::printf("%s: %zu cases held to the x86 CPU's result where Rosetta differs\n", where, silicon);
	}
};

}
