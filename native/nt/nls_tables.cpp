// Builds a single-byte code page's NLS file from its text description, the form the Windows Supported Code Page Data Files are published in.
// It runs during the build: the result is checked against a known hash before it is embedded.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint16_t number(const std::string &text)
{
	const bool hex = text.starts_with("0x") || text.starts_with("0X");
	const char *first = text.data() + (hex ? 2 : 0), *last = text.data() + text.size();
	unsigned value = 0;
	const auto [end, error] = std::from_chars(first, last, value, hex ? 16 : 10);
	if (error != std::errc() || end != last || value > 0xffff)
		throw std::runtime_error("not a 16-bit number: " + text);
	return uint16_t(value);
}

// The next line's words, without its comment; none at the end of the file.
std::vector<std::string> next_words(std::istream &input)
{
	for (std::string line; std::getline(input, line);) {
		std::istringstream stream(line.substr(0, line.find(';')));
		std::vector<std::string> words;
		for (std::string word; stream >> word;)
			words.push_back(word);
		if (!words.empty())
			return words;
	}
	return {};
}

void put(std::vector<uint8_t> &out, uint16_t value)
{
	out.push_back(uint8_t(value));
	out.push_back(uint8_t(value >> 8));
}

}

int main(int argc, char **argv)
{
	try {
		if (argc != 3)
			throw std::runtime_error("usage: nls-tables <page.txt> <page.nls>");
		std::ifstream input(argv[1]);
		if (!input)
			throw std::runtime_error(std::string("cannot read ") + argv[1]);
		uint16_t page = 0, default_byte = 0, default_wide = 0;
		std::array<uint16_t, 256> to_wide{};
		std::vector<uint8_t> to_byte(65536);
		bool bytes_seen = false, wide_seen = false, ended = false;
		const auto entries = [&](const std::vector<std::string> &words, auto &&store) {
			for (int left = number(words.at(1)); left > 0; --left) {
				const auto entry = next_words(input);
				if (entry.size() != 2)
					throw std::runtime_error("expected a pair after " + words[0]);
				store(number(entry[0]), number(entry[1]));
			}
		};
		for (auto words = next_words(input); !words.empty() && !ended; words = next_words(input)) {
			if (words[0] == "CODEPAGE")
				page = number(words.at(1));
			else if (words[0] == "CPINFO") {
				if (number(words.at(1)) != 1)
					throw std::runtime_error("only single-byte pages are built");
				default_byte = number(words.at(2));
				default_wide = number(words.at(3));
				to_wide.fill(default_wide);
				std::fill(to_byte.begin(), to_byte.end(), uint8_t(default_byte));
			} else if (words[0] == "MBTABLE") {
				bytes_seen = true;
				entries(words, [&](uint16_t byte, uint16_t wide) { to_wide.at(byte) = wide; });
			} else if (words[0] == "WCTABLE") {
				wide_seen = true;
				entries(words, [&](uint16_t wide, uint16_t byte) { to_byte[wide] = uint8_t(byte); });
			} else if (words[0] == "ENDCODEPAGE")
				ended = true;
			else
				throw std::runtime_error("unexpected section " + words[0]);
		}
		if (!bytes_seen || !wide_seen || !ended)
			throw std::runtime_error("incomplete code page");
		std::vector<uint8_t> out;
		// The header: its size in words, the page, the longest character, both default characters and what each translates to, and no lead bytes.
		for (const uint16_t field : {uint16_t(13), page, uint16_t(1), default_byte, default_wide, to_wide.at(default_byte), uint16_t(to_byte[default_wide])})
			put(out, field);
		out.resize(out.size() + 12);
		// The words up to the Unicode table: the byte table, and no glyph table, lead bytes or flags.
		put(out, uint16_t(to_wide.size() + 3));
		for (const uint16_t wide : to_wide)
			put(out, wide);
		for (int empty = 0; empty < 3; ++empty)
			put(out, 0);
		out.insert(out.end(), to_byte.begin(), to_byte.end());
		std::ofstream output(argv[2], std::ios::binary);
		output.write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
		if (!output)
			throw std::runtime_error(std::string("cannot write ") + argv[2]);
	} catch (const std::exception &error) {
		std::cerr << "nls-tables: " << error.what() << '\n';
		return 1;
	}
	return 0;
}
