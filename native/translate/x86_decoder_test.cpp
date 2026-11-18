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
	size_t reported = 0;
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
		const std::vector<uint8_t> bytes = parse_hex(line.substr(0, tab));
		const uint64_t address = std::stoull(line.substr(tab + 1, second - tab - 1), nullptr, 16);
		const std::string expected = second == std::string::npos ? "" : line.substr(second + 1);
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
		if (!problem.empty()) {
			++tally.failures;
			if (reported++ < 40)
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
	if (order.empty()) {
		std::cerr << "empty corpus\n";
		return 1;
	}
	return failures == 0 ? 0 : 1;
}
