// Builds the tables of the Windows double-byte code pages from their text descriptions, the form the Windows Supported Code
// Page Data Files are published in. It runs during the build and writes a header that the conversions embed.
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
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

struct Line {
	std::vector<std::string> words;
	std::string comment;
};

// The next line's words and its comment; no words at the end of the file.
Line next_line(std::istream &input)
{
	for (std::string text; std::getline(input, text);) {
		const auto mark = text.find(';');
		std::istringstream stream(text.substr(0, mark));
		Line line;
		for (std::string word; stream >> word;)
			line.words.push_back(word);
		if (mark != text.npos)
			line.comment = text.substr(mark + 1);
		if (!line.words.empty())
			return line;
	}
	return {};
}

struct Page {
	uint16_t id = 0, default_byte = 0, default_wide = 0;
	std::array<uint16_t, 256> bytes{};
	std::vector<std::array<uint8_t, 2>> ranges;
	std::map<uint8_t, std::array<uint16_t, 256>> pairs;
	std::map<uint16_t, uint16_t> fits;
};

bool is_lead(const Page &page, unsigned byte)
{
	for (const auto &range : page.ranges)
		if (byte >= range[0] && byte <= range[1])
			return true;
	return false;
}

Page read_page(const std::string &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot read " + path);
	Page page;
	bool bytes_seen = false, wide_seen = false, ended = false;
	std::vector<uint8_t> leads;
	const auto pair = [&]() {
		const auto entry = next_line(input);
		if (entry.words.size() != 2)
			throw std::runtime_error(path + ": expected a pair");
		return std::array<uint16_t, 2>{number(entry.words[0]), number(entry.words[1])};
	};
	for (auto line = next_line(input); !line.words.empty() && !ended; line = next_line(input)) {
		const auto &words = line.words;
		if (words[0] == "CODEPAGE")
			page.id = number(words.at(1));
		else if (words[0] == "CPINFO") {
			if (number(words.at(1)) != 2)
				throw std::runtime_error(path + ": not a double-byte page");
			page.default_byte = number(words.at(2));
			page.default_wide = number(words.at(3));
		} else if (words[0] == "MBTABLE") {
			bytes_seen = true;
			for (int left = number(words.at(1)); left > 0; --left) {
				const auto [byte, wide] = pair();
				if (byte > 0xff)
					throw std::runtime_error(path + ": single byte out of range");
				page.bytes[byte] = wide;
			}
		} else if (words[0] == "DBCSRANGE") {
			// Each range is followed by the tables of its lead bytes, in order; a table's comment names its lead byte, and it must agree.
			for (int left = number(words.at(1)); left > 0; --left) {
				const auto [first, last] = pair();
				if (first < 0x80 || last > 0xff || first > last)
					throw std::runtime_error(path + ": lead byte range out of order");
				page.ranges.push_back({uint8_t(first), uint8_t(last)});
				for (unsigned lead = first; lead <= last; ++lead) {
					leads.push_back(uint8_t(lead));
					const auto header = next_line(input);
					if (header.words.size() != 2 || header.words[0] != "DBCSTABLE")
						throw std::runtime_error(path + ": a lead byte has no double-byte table");
					if (const auto at = header.comment.find("0x"); at != header.comment.npos && number(header.comment.substr(at, 4)) != lead)
						throw std::runtime_error(path + ": double-byte table out of order");
					auto &table = page.pairs[uint8_t(lead)];
					for (int entries = number(header.words[1]); entries > 0; --entries) {
						const auto [trail, wide] = pair();
						if (trail > 0xff || !wide)
							throw std::runtime_error(path + ": trail byte out of range");
						table[trail] = wide;
					}
				}
			}
		} else if (words[0] == "WCTABLE") {
			wide_seen = true;
			for (int left = number(words.at(1)); left > 0; --left) {
				const auto [wide, bytes] = pair();
				if (!page.fits.emplace(wide, bytes).second)
					throw std::runtime_error(path + ": character listed twice");
				if (bytes > 0xff ? !is_lead(page, bytes >> 8) : is_lead(page, bytes))
					throw std::runtime_error(path + ": best fit is not a whole character");
			}
		} else if (words[0] == "ENDCODEPAGE")
			ended = true;
		else
			throw std::runtime_error(path + ": unexpected section " + words[0]);
	}
	// CPINFO holds five ranges and the pair of zeros that ends them.
	if (page.ranges.size() > 5)
		throw std::runtime_error(path + ": more lead byte ranges than CPINFO holds");
	if (!page.id || !bytes_seen || !wide_seen || !ended || page.ranges.empty())
		throw std::runtime_error(path + ": incomplete code page");
	for (const auto lead : leads)
		if (page.bytes[lead])
			throw std::runtime_error(path + ": a lead byte is also a character");
	return page;
}

void write_list(std::ostream &output, const auto &values)
{
	size_t column = 0;
	for (const auto value : values) {
		output << "0x" << std::hex << unsigned(value) << std::dec << ',';
		if (++column % 16 == 0)
			output << '\n';
	}
}

}

int main(int argc, char **argv)
{
	try {
		if (argc < 3)
			throw std::runtime_error("usage: dbcs-tables <output.h> <bestfit.txt>...");
		std::ostringstream output;
		output << "#pragma once\n#include <array>\n#include <cstdint>\nnamespace raven {\n";
		std::string list;
		for (int index = 2; index < argc; ++index) {
			const auto page = read_page(argv[index]);
			const auto name = std::to_string(page.id);
			output << "inline constexpr std::array<uint16_t,256> native_dbcs" << name << "_bytes{\n";
			write_list(output, page.bytes);
			// A lead byte's slot in the pair table, or 0 when the byte is not a lead byte.
			std::array<uint8_t, 256> slots{};
			std::vector<uint16_t> pairs;
			uint8_t slot = 0;
			for (const auto &[lead, table] : page.pairs) {
				slots[lead] = ++slot;
				pairs.insert(pairs.end(), table.begin(), table.end());
			}
			output << "};\ninline constexpr std::array<uint8_t,256> native_dbcs" << name << "_slots{\n";
			write_list(output, slots);
			output << "};\ninline constexpr std::array<uint16_t," << pairs.size() << "> native_dbcs" << name << "_pairs{\n";
			write_list(output, pairs);
			output << "};\ninline constexpr std::array<std::array<uint16_t,2>," << page.fits.size() << "> native_dbcs" << name << "_fits{{\n";
			for (const auto &[wide, bytes] : page.fits)
				output << "{0x" << std::hex << wide << ",0x" << bytes << std::dec << "},\n";
			output << "}};\ninline constexpr std::array<uint8_t,12> native_dbcs" << name << "_ranges{";
			for (const auto &range : page.ranges)
				output << unsigned(range[0]) << ',' << unsigned(range[1]) << ',';
			output << "};\n";
			list += "\t{" + name + "," + std::to_string(page.default_byte) + "," + std::to_string(page.default_wide) + ",native_dbcs" + name +
				"_ranges.data(),native_dbcs" + name + "_bytes.data(),native_dbcs" + name + "_slots.data(),native_dbcs" + name +
				"_pairs.data(),native_dbcs" + name + "_fits.data(),native_dbcs" + name + "_fits.size()},\n";
		}
		output << "inline constexpr NativeDoubleBytePage native_double_byte_pages[]{\n" << list << "};\n}\n";
		std::ofstream file(argv[1], std::ios::binary);
		file << output.str();
		if (!file)
			throw std::runtime_error(std::string("cannot write ") + argv[1]);
	} catch (const std::exception &error) {
		std::cerr << "dbcs-tables: " << error.what() << '\n';
		return 1;
	}
	return 0;
}
