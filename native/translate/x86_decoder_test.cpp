#include "translator.h"
#include "x86_decoder.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <vector>

using namespace raven::translate;

static std::vector<uint8_t> parse_hex(const std::string &text)
{
	std::vector<uint8_t> bytes;
	for (size_t i = 0; i + 1 < text.size(); i += 2)
		bytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
	return bytes;
}

// The first byte after the legacy and REX prefixes, and whether 66 was among them.
static std::pair<uint8_t, bool> first_opcode(const std::vector<uint8_t> &bytes, size_t &at)
{
	bool operand16 = false;
	for (at = 0; at < bytes.size(); ++at) {
		const uint8_t byte = bytes[at];
		operand16 |= byte == 0x66;
		if (byte == 0x66 || byte == 0x67 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 || byte == 0x26 || byte == 0x2e || byte == 0x36 ||
			byte == 0x3e || byte == 0x64 || byte == 0x65 || (byte >= 0x40 && byte <= 0x4f))
			continue;
		return {byte, operand16};
	}
	return {0, operand16};
}

// Where the decoder follows the hardware the port targets and LLVM's disassembler does not: each is counted, not failed.
static const char *divergence(const std::vector<uint8_t> &bytes, const std::string &expected, const Instruction &instruction)
{
	size_t at = 0;
	const auto [opcode, operand16] = first_opcode(bytes, at);
	const uint8_t next = at + 1 < bytes.size() ? bytes[at + 1] : 0;
	// Intel CPUs ignore 66 on near branches in 64-bit mode and take a rel32; LLVM decodes AMD's rel16.
	if (operand16 && instruction.length == bytes.size() + 2 &&
		(opcode == 0xe8 || opcode == 0xe9 || (opcode == 0x0f && next >= 0x80 && next <= 0x8f)))
		return "66 on a near branch";
	// LLVM accepts LOCK on forms the CPU raises #UD for.
	if (expected.rfind("lock ", 0) == 0 && instruction.mnemonic == Mnemonic::invalid)
		return "lock on a form without a memory destination";
	// APX (REX2, EVEX map 4): no CPU the port runs x64 code for has it, and nothing translates it.
	if (opcode == 0xd5 || (opcode == 0x62 && (next & 7) == 4))
		return "APX encoding";
	return nullptr;
}

// SSE and SSE2: the legacy 0F map (no VEX, EVEX, 0F 38 or 0F 3A) on XMM registers or MXCSR, without SSE3's few 0F opcodes and the
// forms with an MMX register.
static bool sse_or_sse2(const std::vector<uint8_t> &bytes, const std::string &text)
{
	size_t at = 0;
	const uint8_t opcode = first_opcode(bytes, at).first;
	if (opcode != 0x0f || at + 1 >= bytes.size() || bytes[at + 1] == 0x38 || bytes[at + 1] == 0x3a)
		return false;
	const std::string name = text.substr(0, text.find(' '));
	const bool to_general = name == "cvtss2si" || name == "cvttss2si" || name == "cvtsd2si" || name == "cvttsd2si";
	if (text.find("xmm") == std::string::npos && text.find("mxcsr") == std::string::npos && !to_general)
		return false;
	static const std::regex mmx(R"(\bmm[0-7]\b)");
	if (std::regex_search(text, mmx))
		return false;
	for (const char *sse3 : {"movddup", "movshdup", "movsldup", "addsubps", "addsubpd", "haddps", "haddpd", "hsubps", "hsubpd", "lddqu"})
		if (name == sse3)
			return false;
	return true;
}

// x87: D8-DF and wait (9B), as LLVM prints them.
static bool x87(const std::vector<uint8_t> &bytes)
{
	size_t at = 0;
	const uint8_t opcode = first_opcode(bytes, at).first;
	return (opcode >= 0xd8 && opcode <= 0xdf) || opcode == 0x9b;
}

struct Tally {
	size_t instructions = 0;
	size_t semantic = 0;
	size_t invalid_seen = 0;
	size_t failures = 0;
	size_t sse = 0;
	size_t sse_semantic = 0;
	size_t sse_translated = 0;
	// SSE and SSE2 instructions the translator has no code for, by LLVM's mnemonic.
	std::map<std::string, size_t> sse_missing;
	size_t x87 = 0;
	size_t x87_semantic = 0;
	size_t x87_translated = 0;
	std::map<std::string, size_t> x87_missing;
	size_t vex = 0;
	size_t vex_translated = 0;
	size_t translated = 0;
	std::map<std::string, size_t> missing;
	std::map<std::string, size_t> vex_missing;
};

// VEX-encoded: C4 or C5 after the legacy prefixes (in 64-bit mode both always start a VEX prefix).
static bool vex_encoded(const std::vector<uint8_t> &bytes)
{
	for (const uint8_t byte : bytes) {
		if (byte == 0x66 || byte == 0x67 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 || byte == 0x26 || byte == 0x2e ||
			byte == 0x36 || byte == 0x3e || byte == 0x64 || byte == 0x65)
			continue;
		return byte == 0xc4 || byte == 0xc5;
	}
	return false;
}

// The source of random SSE and SSE2 instances: each must have semantics.
static const std::string sse_instances = "sse_instances.o";
// The same for SSE3 to SSE4.2, crc32 and popcnt.
static const std::string sse4_instances = "sse4_instances.o";
// The same for x87.
static const std::string x87_instances = "x87_instances.o";

// Checks the decoder against the corpus x86_corpus.py wrote from LLVM's disassembler. Every instruction LLVM decodes must decode to the
// same length; those the translator gives semantics to must also print as LLVM prints them; bytes LLVM rejects must not decode to an
// instruction with semantics. Prints the share of each source the translator could take.
int main(int argc, char **argv)
{
	if (argc != 2) {
		std::cerr << "usage: x86_decoder_test CORPUS\n";
		return 2;
	}
	std::ifstream corpus(argv[1]);
	if (!corpus) {
		std::cerr << "cannot open " << argv[1] << "\n";
		return 2;
	}
	// Encodings pinned as not translated: x86 raises #UD for rorx with VEX.vvvv other than 1111b.
	struct Pinned {
		const char *what;
		std::vector<uint8_t> bytes;
	};
	const Pinned pinned[] = {
		{"rorx eax, ebx, 3 with VEX.vvvv 0001 (#UD)", {0xc4, 0xe3, 0x73, 0xf0, 0xc3, 0x03}},
	};
	size_t pinned_failures = 0;
	for (const Pinned &item : pinned) {
		std::vector<uint8_t> window = item.bytes;
		window.resize(item.bytes.size() + 15, 0x90);
		const Instruction instruction = decode(window, 0x1000);
		if (instruction.mnemonic != Mnemonic::invalid && translates(instruction)) {
			std::cerr << "pinned: " << item.what << ": translated as '" << format_intel(instruction) << "'\n";
			++pinned_failures;
		}
	}
	std::printf("pinned encodings: %zu, %zu failures\n", std::size(pinned), pinned_failures);
	std::map<std::string, Tally> tallies;
	std::vector<std::string> order;
	std::string source = "?";
	std::string line;
	std::map<std::string, size_t> reported;
	std::map<std::string, size_t> divergences;
	while (std::getline(corpus, line)) {
		if (line.empty())
			continue;
		if (line[0] == '#') {
			source = line.substr(2);
			order.push_back(source);
			continue;
		}
		const size_t tab = line.find('\t');
		const size_t second = line.find('\t', tab + 1);
		std::vector<uint8_t> bytes = parse_hex(line.substr(0, tab));
		const uint64_t address = std::stoull(line.substr(tab + 1, second - tab - 1), nullptr, 16);
		std::string expected = second == std::string::npos ? "" : line.substr(second + 1);
		// LLVM prints a lock prefix that starts an instruction as an instruction of its own; join it to the one it prefixes.
		std::string next;
		if (expected == "lock" && std::getline(corpus, next) && !next.empty() && next[0] != '#') {
			const size_t next_tab = next.find('\t');
			const size_t next_second = next.find('\t', next_tab + 1);
			const std::vector<uint8_t> rest = parse_hex(next.substr(0, next_tab));
			bytes.insert(bytes.end(), rest.begin(), rest.end());
			expected = next_second == std::string::npos || next_second + 1 == next.size() ? "" : "lock " + next.substr(next_second + 1);
			line = next;
		}
		// LLVM also prints a prefix it cannot attach to anything as an instruction of its own; the hardware would not split it.
		static const char *const lone_prefixes[] = {"fs", "gs", "cs", "ds", "es", "ss", "xacquire", "xrelease", "data16", "addr32", "rep",
			"repne", "lock", "rex", "rex.W"};
		bool lone = false;
		for (const char *prefix : lone_prefixes)
			lone |= expected == prefix;
		if (lone)
			continue;
		// Decode from a window longer than the instruction, as the translator does, so a decoder that reads too far shows.
		std::vector<uint8_t> window = bytes;
		window.resize(bytes.size() + 15, 0x90);
		const Instruction instruction = decode(window, address);
		Tally &tally = tallies[source];
		std::string problem;
		if (expected.empty()) {
			++tally.invalid_seen;
			if (instruction.mnemonic != Mnemonic::invalid && instruction.mnemonic != Mnemonic::other)
				problem = "LLVM rejects it, decoded as " + format_intel(instruction);
		} else {
			++tally.instructions;
			if (instruction.mnemonic == Mnemonic::invalid)
				problem = "not decoded";
			else if (instruction.length != bytes.size())
				problem = "length " + std::to_string(instruction.length) + " instead of " + std::to_string(bytes.size());
			else if (instruction.mnemonic == Mnemonic::other && source == sse4_instances)
				problem = "no semantics";
			else if (instruction.mnemonic != Mnemonic::other) {
				++tally.semantic;
				const std::string text = format_intel(instruction);
				if (text != expected)
					problem = "printed '" + text + "'";
			}
			if (sse_or_sse2(bytes, expected)) {
				++tally.sse;
				if (instruction.mnemonic != Mnemonic::invalid && instruction.mnemonic != Mnemonic::other)
					++tally.sse_semantic;
				else if (source == sse_instances && problem.empty())
					problem = "no semantics";
				if (instruction.mnemonic != Mnemonic::invalid && translates(instruction))
					++tally.sse_translated;
				else
					++tally.sse_missing[expected.substr(0, expected.find(' '))];
			}
			if (instruction.mnemonic != Mnemonic::invalid && translates(instruction))
				++tally.translated;
			else
				++tally.missing[expected.substr(0, expected.find(' '))];
			if (vex_encoded(bytes)) {
				++tally.vex;
				if (instruction.mnemonic != Mnemonic::invalid && translates(instruction))
					++tally.vex_translated;
				else
					++tally.vex_missing[expected.substr(0, expected.find(' '))];
			}
			if (x87(bytes)) {
				++tally.x87;
				if (instruction.mnemonic != Mnemonic::invalid && instruction.mnemonic != Mnemonic::other)
					++tally.x87_semantic;
				else if (source == x87_instances && problem.empty())
					problem = "no semantics";
				if (instruction.mnemonic != Mnemonic::invalid && translates(instruction))
					++tally.x87_translated;
				else
					++tally.x87_missing[expected.substr(0, expected.find(' '))];
			}
		}
		if (!problem.empty() && !expected.empty())
			if (const char *reason = divergence(bytes, expected, instruction)) {
				++divergences[reason];
				problem.clear();
			}
		if (!problem.empty()) {
			++tally.failures;
			if (reported[source]++ < 12)
				std::cerr << source << " " << line.substr(0, tab) << " '" << expected << "': " << problem << "\n";
		}
	}
	size_t failures = pinned_failures;
	for (const std::string &name : order) {
		const Tally &tally = tallies[name];
		failures += tally.failures;
		std::printf("%s: %zu instructions, %zu with semantics (%.1f%%), %zu rejected by LLVM, %zu failures\n", name.c_str(),
			tally.instructions, tally.semantic, tally.instructions ? 100.0 * double(tally.semantic) / double(tally.instructions) : 0.0,
			tally.invalid_seen, tally.failures);
		if (tally.x87) {
			std::printf("%s: %zu x87 instructions, %zu with semantics (%.1f%%), %zu translated (%.1f%%)", name.c_str(), tally.x87,
				tally.x87_semantic, 100.0 * double(tally.x87_semantic) / double(tally.x87), tally.x87_translated,
				100.0 * double(tally.x87_translated) / double(tally.x87));
			std::vector<std::pair<size_t, std::string>> missing;
			for (const auto &[mnemonic, count] : tally.x87_missing)
				missing.push_back({count, mnemonic});
			std::sort(missing.rbegin(), missing.rend());
			for (size_t i = 0; i < missing.size() && i < 12; ++i)
				std::printf("%s %s %zu", i ? "," : "; without:", missing[i].second.c_str(), missing[i].first);
			std::printf("\n");
		}
		if (name.find(".exe") != std::string::npos) {
			std::printf("%s: %zu instructions translated of %zu", name.c_str(), tally.translated, tally.instructions);
			std::vector<std::pair<size_t, std::string>> left;
			for (const auto &[mnemonic, count] : tally.missing)
				left.push_back({count, mnemonic});
			std::sort(left.rbegin(), left.rend());
			for (size_t i = 0; i < left.size() && i < 16; ++i)
				std::printf("%s %s %zu", i ? "," : "; without:", left[i].second.c_str(), left[i].first);
			std::printf("\n");
		}
		if (tally.vex && name.find("instances") == std::string::npos) {
			std::printf("%s: %zu VEX instructions, %zu translated (%.1f%%)", name.c_str(), tally.vex, tally.vex_translated,
				100.0 * double(tally.vex_translated) / double(tally.vex));
			std::vector<std::pair<size_t, std::string>> missing;
			for (const auto &[mnemonic, count] : tally.vex_missing)
				missing.push_back({count, mnemonic});
			std::sort(missing.rbegin(), missing.rend());
			for (size_t i = 0; i < missing.size() && i < 16; ++i)
				std::printf("%s %s %zu", i ? "," : "; without:", missing[i].second.c_str(), missing[i].first);
			std::printf("\n");
		}
		if (!tally.sse)
			continue;
		std::printf("%s: %zu SSE and SSE2 instructions, %zu with semantics (%.1f%%), %zu translated (%.1f%%)", name.c_str(), tally.sse,
			tally.sse_semantic, 100.0 * double(tally.sse_semantic) / double(tally.sse), tally.sse_translated,
			100.0 * double(tally.sse_translated) / double(tally.sse));
		std::vector<std::pair<size_t, std::string>> missing;
		for (const auto &[mnemonic, count] : tally.sse_missing)
			missing.push_back({count, mnemonic});
		std::sort(missing.rbegin(), missing.rend());
		for (size_t i = 0; i < missing.size() && i < 12; ++i)
			std::printf("%s %s %zu", i ? "," : "; without:", missing[i].second.c_str(), missing[i].first);
		std::printf("\n");
	}
	for (const auto &[reason, count] : divergences)
		std::printf("known divergence from LLVM, %s: %zu\n", reason.c_str(), count);
	if (order.empty()) {
		std::cerr << "empty corpus\n";
		return 1;
	}
	return failures == 0 ? 0 : 1;
}
