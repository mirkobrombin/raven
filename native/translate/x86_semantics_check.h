#pragma once

// The integer oracle's cases (x86_semantics.py, run on x86 by x86_semantics_oracle.c) and how the translator's run of one compares
// with x86's: shared by the in-process test (x86_semantics_test) and the vCPU one (hvf_integer_test). The build runs the oracle under
// Rosetta; testdata/x86_silicon.txt holds what an x86 CPU left where it differs, each line refused if Rosetta's result for its case
// has changed since (its fingerprint), as x86_avx_check.h does for the AVX oracle.

#include "cpu_state.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace raven::translate::integer_oracle {

struct Io {
	uint64_t registers[16];
	uint64_t flags;
	uint8_t buffer[64];
};
static_assert(sizeof(Io) == 200, "x86_semantics.py's run_case offsets");

struct Case {
	std::vector<uint8_t> code;
	std::string text;
	std::string family;
	// The flags x86 defines after the case: only those are compared.
	uint64_t defined = 0;
	Io input;
	Io output;
	bool silicon = false;
	// Whether the case's family was run on the x86 CPU (a corpus line); if not, the case fails.
	bool compared = false;
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
		uint32_t length = 0;
		get(&length, 4);
		item.code.resize(length);
		get(item.code.data(), length);
		get_string(item.text);
		get_string(item.family);
		get(&item.defined, 8);
		get(&item.input, sizeof(Io));
		get(&item.output, sizeof(Io));
	}
	if (!in)
		cases.clear();
	return cases;
}

// FNV-1a 64 of an output: the general registers but RSP and R15, the six flags and the buffer
// (docs/macos/integer-data/silicon_overrides.py computes the same).
inline uint64_t fingerprint(const Io &output)
{
	Io io = output;
	io.registers[rsp] = io.registers[r15] = 0;
	io.flags &= all_flags;
	uint64_t value = 0xcbf29ce484222325ull;
	const auto *bytes = reinterpret_cast<const uint8_t *>(&io);
	for (size_t i = 0; i < sizeof io; ++i)
		value = (value ^ bytes[i]) * 0x100000001b3ull;
	return value;
}

// FNV-1a 64 of one family's cases: of every case's code and text (what x86_semantics.py generated), and of every case's fingerprint
// (what Rosetta left). The silicon file has a `corpus` line per family compared with the x86 CPU holding both.
inline std::pair<uint64_t, uint64_t> corpus_fingerprints(const std::vector<Case> &cases, const std::string &family)
{
	uint64_t generated = 0xcbf29ce484222325ull, results = 0xcbf29ce484222325ull;
	auto mix = [](uint64_t &value, const void *data, size_t size) {
		for (size_t i = 0; i < size; ++i)
			value = (value ^ static_cast<const uint8_t *>(data)[i]) * 0x100000001b3ull;
	};
	for (const Case &item : cases) {
		if (item.family != family)
			continue;
		mix(generated, item.code.data(), item.code.size());
		mix(generated, item.text.data(), item.text.size());
		const uint64_t one = fingerprint(item.output);
		mix(results, &one, sizeof one);
	}
	return {generated, results};
}

inline std::vector<uint8_t> hex_bytes(const std::string &text)
{
	std::vector<uint8_t> bytes;
	for (size_t i = 0; i + 1 < text.size(); i += 2)
		bytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
	return bytes;
}

// Applies the silicon results: corpus lines (corpus<TAB>FAMILY<TAB>cases=N<TAB>generated=HEX<TAB>rosetta=HEX) and one line per case,
// NUMBER<TAB>TEXT<TAB>rosetta=FINGERPRINT, then FIELD=HEX for each field the x86 CPU left otherwise: rN, flags or bOFFSET (a run of
// buffer bytes). Returns how many cases changed, or -1 when a line does not fit the cases.
inline long apply_silicon(std::vector<Case> &cases, const char *path)
{
	std::map<std::string, bool> compared;
	std::ifstream in(path);
	if (!in)
		return -1;
	long changed = 0;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		std::vector<std::string> parts;
		std::stringstream stream(line);
		for (std::string part; std::getline(stream, part, '\t');)
			parts.push_back(part);
		if (parts[0] == "corpus") {
			if (parts.size() < 5) {
				std::fprintf(stderr, "%s: a corpus line needs a family, cases, generated and rosetta\n", path);
				return -1;
			}
			const std::string family = parts[1];
			size_t count = 0;
			for (const Case &item : cases)
				count += item.family == family;
			const auto [generated, results] = corpus_fingerprints(cases, family);
			char expected[96];
			std::snprintf(expected, sizeof expected, "cases=%zu", count);
			if (parts[2] != expected || std::stoull(parts[3].substr(10), nullptr, 16) != generated) {
				std::fprintf(stderr, "%s: family %s: the cases are not those the silicon results were compared on (x86_semantics.py "
					"changed: %s, generated=%016llx); run the oracle on the x86 CPU again\n", path, family.c_str(), expected,
					static_cast<unsigned long long>(generated));
				return -1;
			}
			if (std::stoull(parts[4].substr(8), nullptr, 16) != results) {
				std::fprintf(stderr, "%s: family %s: Rosetta's results are not those the silicon results were compared with "
					"(rosetta=%016llx); compare with the x86 CPU again\n", path, family.c_str(), static_cast<unsigned long long>(results));
				return -1;
			}
			compared[family] = true;
			continue;
		}
		const size_t number = std::stoul(parts[0]);
		if (parts.size() < 3 || number >= cases.size() || parts[1] != cases[number].text) {
			std::fprintf(stderr, "silicon line for case %zu does not fit the cases\n", number);
			return -1;
		}
		if (parts[2].rfind("rosetta=", 0) != 0 || std::stoull(parts[2].substr(8), nullptr, 16) != fingerprint(cases[number].output)) {
			std::fprintf(stderr, "case %zu (%s): Rosetta's result is not the one the silicon results were written against\n", number,
				cases[number].text.c_str());
			return -1;
		}
		Io &io = cases[number].output;
		for (size_t i = 3; i < parts.size(); ++i) {
			const size_t equals = parts[i].find('=');
			const std::string field = parts[i].substr(0, equals);
			const std::vector<uint8_t> bytes = hex_bytes(parts[i].substr(equals + 1));
			void *to = nullptr;
			if (field == "flags")
				to = &io.flags;
			else if (field[0] == 'r')
				to = &io.registers[std::stoul(field.substr(1))];
			else if (field[0] == 'b')
				to = io.buffer + std::stoul(field.substr(1));
			if (!to)
				return -1;
			std::memcpy(to, bytes.data(), bytes.size());
		}
		cases[number].silicon = true;
		++changed;
	}
	for (Case &item : cases)
		item.compared = compared.count(item.family) != 0;
	return changed;
}

inline void load(CpuState &state, const Io &input)
{
	std::memcpy(state.gpr, input.registers, sizeof state.gpr);
	const uint64_t flags = input.flags;
	state.nzcv = ((flags & SF) ? 1ull << 31 : 0) | ((flags & ZF) ? 1ull << 30 : 0) | ((flags & CF) ? 0 : 1ull << 29) |
		((flags & OF) ? 1ull << 28 : 0);
	// A low byte of even parity for PF; bit 4 of the xor with the adjust source for AF.
	state.parity_source = (flags & PF) ? 0 : 1;
	state.adjust_source = ((flags & AF) ? 16 : 0) ^ state.parity_source;
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

// What the translator's run left that differs from x86's, or nothing: every general register but RSP and R15, the flags the case
// defines and the buffer.
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
	if ((flags_of(state) ^ x86.flags) & item.defined) {
		std::snprintf(text, sizeof text, "flags %#llx, x86 left %#llx (defined %#llx)", static_cast<unsigned long long>(flags_of(state)),
			static_cast<unsigned long long>(x86.flags & all_flags), static_cast<unsigned long long>(item.defined));
		return text;
	}
	for (unsigned b = 0; b < sizeof x86.buffer; ++b)
		if (buffer[b] != x86.buffer[b]) {
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
	size_t silicon = 0;
	const Case *first = nullptr;
	std::set<std::string> rosetta_only;
	void add(const Case &item, const std::string &problem)
	{
		silicon += item.silicon;
		auto &[cases, failed] = families[item.family];
		++cases;
		// Every family must have been run on an x86 CPU: one without a corpus line fails, even where it agrees with Rosetta.
		if (!item.compared) {
			rosetta_only.insert(item.family);
			if (problem.empty()) {
				++failed;
				++failures;
				return;
			}
		}
		if (problem.empty())
			return;
		++failed;
		++failures;
		if (printed++ < 40)
			std::fprintf(stderr, "%s: #%zu %s: %s\n", item.family.c_str(), static_cast<size_t>(&item - first), item.text.c_str(),
				problem.c_str());
	}
	void print(const char *where) const
	{
		for (const auto &[family, counts] : families)
			std::printf("%s, %s: %zu cases, %zu failures%s\n", where, family.c_str(), counts.first, counts.second,
				rosetta_only.count(family) ? " (no x86 CPU run of this family: every case fails)" : "");
		std::printf("%s: %zu cases held to the x86 CPU where Rosetta differs\n", where, silicon);
	}
};

}
