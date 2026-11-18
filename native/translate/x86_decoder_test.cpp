#include "x86_decoder.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
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

struct Tally {
	size_t instructions = 0;
	size_t semantic = 0;
	size_t invalid_seen = 0;
	size_t failures = 0;
};

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
			else if (instruction.mnemonic != Mnemonic::other) {
				++tally.semantic;
				const std::string text = format_intel(instruction);
				if (text != expected)
					problem = "printed '" + text + "'";
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
	size_t failures = 0;
	for (const std::string &name : order) {
		const Tally &tally = tallies[name];
		failures += tally.failures;
		std::printf("%s: %zu instructions, %zu with semantics (%.1f%%), %zu rejected by LLVM, %zu failures\n", name.c_str(),
			tally.instructions, tally.semantic, tally.instructions ? 100.0 * double(tally.semantic) / double(tally.instructions) : 0.0,
			tally.invalid_seen, tally.failures);
	}
	for (const auto &[reason, count] : divergences)
		std::printf("known divergence from LLVM, %s: %zu\n", reason.c_str(), count);
	if (order.empty()) {
		std::cerr << "empty corpus\n";
		return 1;
	}
	return failures == 0 ? 0 : 1;
}
