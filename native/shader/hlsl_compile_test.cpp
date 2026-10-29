#include "d3dbc.h"
#include "dxbc.h"
#include "hlsl_compile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace raven::shader;

std::string read_file(const std::filesystem::path &path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

bool has_tool(const char *name)
{
	const std::string command = std::string("command -v ") + name + " >/dev/null 2>&1";
	return std::system(command.c_str()) == 0;
}

// One compile a test file asks for: "// compile: entry target", or "// error: entry target text" for a compile that must fail. An effect
// target takes "-" for its entry, and every "// contains: text" line of the file must appear in the effect as vkd3d-shader prints it.
struct Case {
	std::string entry, target, error;
	bool fails = false;
};

std::vector<Case> cases_of(const std::string &source)
{
	std::vector<Case> cases;
	std::istringstream lines(source);
	std::string line;
	while (std::getline(lines, line)) {
		Case item;
		std::istringstream words(line);
		std::string slashes, kind;
		words >> slashes >> kind;
		if (slashes != "//" || (kind != "compile:" && kind != "error:"))
			continue;
		words >> item.entry >> item.target;
		if (item.entry == "-")
			item.entry.clear();
		item.fails = kind == "error:";
		std::getline(words, item.error);
		item.error.erase(0, item.error.find_first_not_of(' '));
		cases.push_back(item);
	}
	return cases;
}

bool well_formed(const std::vector<uint8_t> &blob, std::string &problem)
{
	const auto chunks = read_container(blob);
	if (chunks.empty()) {
		problem = "the container does not parse";
		return false;
	}
	const auto sum = container_checksum(blob);
	for (int i = 0; i < 4; ++i) {
		uint32_t stored = 0;
		std::memcpy(&stored, blob.data() + 4 + 4 * i, 4);
		if (stored != sum[size_t(i)]) {
			problem = "the checksum does not match";
			return false;
		}
	}
	for (const char *tag : {"RDEF", "ISGN", "STAT"}) {
		const bool found = std::any_of(chunks.begin(), chunks.end(), [&](const Chunk &chunk) { return std::memcmp(chunk.tag.data(), tag, 4) == 0; });
		if (!found) {
			problem = std::string("no ") + tag + " chunk";
			return false;
		}
	}
	return true;
}

// A shader model 2 or 3 program: the version token of its target, a constant table comment whose offsets stay inside it, and the end
// token.
bool legacy_well_formed(const std::vector<uint8_t> &blob, const std::string &target, std::string &problem)
{
	std::vector<uint32_t> tokens(blob.size() / 4);
	std::memcpy(tokens.data(), blob.data(), tokens.size() * 4);
	if (tokens.size() < 2 || blob.size() % 4) {
		problem = "the program is too short";
		return false;
	}
	const uint32_t major = uint32_t(target[3] - '0');
	const uint32_t minor = target[5] == '0' ? 0 : 1;
	const uint32_t version = (target[0] == 'p' ? 0xffff0000u : 0xfffe0000u) | major << 8 | minor;
	if (tokens[0] != version) {
		problem = "the version token does not name " + target;
		return false;
	}
	if (tokens.back() != 0x0000ffff) {
		problem = "the program does not end with the end token";
		return false;
	}
	if ((tokens[1] & 0xffff) != 0xfffe || tokens.size() < 3 || tokens[2] != 0x42415443) {
		problem = "no constant table follows the version token";
		return false;
	}
	const uint32_t length = tokens[1] >> 16;
	if (length + 2 > tokens.size() || length < 8) {
		problem = "the constant table runs past the program";
		return false;
	}
	const uint8_t *table = blob.data() + 12;
	const uint32_t table_size = (length - 1) * 4;
	uint32_t header[7];
	std::memcpy(header, table, sizeof(header));
	if (header[0] != 28 || header[1] >= table_size || header[4] > table_size || header[4] + header[3] * 20 > table_size || header[6] >= table_size) {
		problem = "the constant table header points outside it";
		return false;
	}
	for (uint32_t i = 0; i < header[3]; ++i) {
		uint32_t info[5];
		std::memcpy(info, table + header[4] + i * 20, sizeof(info));
		if (info[0] >= table_size || info[3] + 16 > table_size) {
			problem = "a constant's name or type lies outside the table";
			return false;
		}
	}
	// Every instruction must be one the listing knows.
	const std::string listing = d3dbc::disassemble(tokens);
	// ps_2_a, ps_2_b and vs_2_a are version 2.1 in the program.
	const std::string listed = target.back() == 'a' || target.back() == 'b' ? target.substr(0, 5) + "1" : target;
	if (listing.find("    " + listed + "\n") == std::string::npos || listing.find("unknown opcode") != std::string::npos ||
		listing.find(" ?") != std::string::npos) {
		problem = "the listing does not read back:\n" + listing;
		return false;
	}
	return true;
}

// The lines an effect must print: "// contains: text".
std::vector<std::string> contained_of(const std::string &source)
{
	std::vector<std::string> texts;
	std::istringstream lines(source);
	for (std::string line; std::getline(lines, line);)
		if (line.starts_with("// contains: "))
			texts.push_back(line.substr(13));
	return texts;
}

// Reads an effect back with vkd3d-shader, which must print it without errors and with every expected text.
bool effect_reads(const std::vector<uint8_t> &blob, const std::string &target, const std::filesystem::path &work, const std::string &stem,
	const std::vector<std::string> &texts, std::string &problem)
{
	const bool contained = target != "fx_5_0";
	if (contained) {
		const auto chunks = read_container(blob);
		if (chunks.size() != 1 || std::memcmp(chunks[0].tag.data(), "FX10", 4) != 0) {
			problem = "the effect is not one FX10 chunk in a container";
			return false;
		}
	}
	const auto input = work / (stem + ".fx"), printed = work / (stem + ".txt"), log = work / (stem + ".log");
	std::ofstream(input, std::ios::binary).write(reinterpret_cast<const char *>(blob.data()), std::streamsize(blob.size()));
	const std::string command = "vkd3d-compiler -x " + std::string(contained ? "dxbc-fx" : "fx") + " -b d3d-asm -o '" + printed.string() + "' '" +
		input.string() + "' >'" + log.string() + "' 2>&1";
	if (std::system(command.c_str()) != 0 || !read_file(log).empty()) {
		problem = "vkd3d-compiler could not read it: " + read_file(log);
		return false;
	}
	const std::string text = read_file(printed);
	for (const auto &expected : texts)
		if (text.find(expected) == std::string::npos) {
			problem = "the effect does not show '" + expected + "'";
			return false;
		}
	return true;
}

// Translates the program to SPIR-V with vkd3d-shader and validates the result, when both tools are present. The expected texts must
// appear in vkd3d-shader's listing; a program that indexes descriptor arrays is only listed, since the translation needs a Vulkan
// environment the command line cannot ask for.
bool translates(const std::vector<uint8_t> &blob, const std::filesystem::path &work, const std::string &stem, bool legacy,
	const std::vector<std::string> &texts, bool listed_only, std::string &problem)
{
	const auto dxbc = work / (stem + ".dxbc"), spirv = work / (stem + ".spv"), log = work / (stem + ".log"), listing = work / (stem + ".txt");
	{
		std::ofstream out(dxbc, std::ios::binary);
		out.write(reinterpret_cast<const char *>(blob.data()), std::streamsize(blob.size()));
	}
	if (!texts.empty() || listed_only) {
		const std::string list = "vkd3d-compiler -x " + std::string(legacy ? "d3dbc" : "dxbc-tpf") + " -b d3d-asm -o '" + listing.string() + "' '" +
			dxbc.string() + "' >'" + log.string() + "' 2>&1";
		if (std::system(list.c_str()) != 0 || !read_file(log).empty()) {
			problem = "vkd3d-compiler could not list it: " + read_file(log);
			return false;
		}
		const std::string text = read_file(listing);
		for (const auto &expected : texts)
			if (text.find(expected) == std::string::npos) {
				problem = "the listing does not show '" + expected + "'";
				return false;
			}
		if (listed_only)
			return true;
	}
	const std::string translate = "vkd3d-compiler -x " + std::string(legacy ? "d3dbc" : "dxbc-tpf") + " -o '" + spirv.string() + "' '" + dxbc.string() + "' >'" + log.string() + "' 2>&1";
	if (std::system(translate.c_str()) != 0) {
		problem = "vkd3d-compiler rejected it: " + read_file(log);
		return false;
	}
	const std::string validate = "spirv-val '" + spirv.string() + "' >'" + log.string() + "' 2>&1";
	if (std::system(validate.c_str()) != 0) {
		problem = "spirv-val rejected the translation: " + read_file(log);
		return false;
	}
	return true;
}

int compile_one(int argc, char **argv)
{
	if (argc != 6) {
		std::cerr << "usage: hlsl_compile_test --compile source entry target output\n";
		return 2;
	}
	CompileRequest request;
	request.source = read_file(argv[2]);
	request.name = argv[2];
	request.entry = argv[3];
	request.target = argv[4];
	const auto directory = std::filesystem::path(argv[2]).parent_path();
	request.include = [&](std::string name, bool) -> std::optional<std::string> {
		// Sources written for Windows separate directories with backslashes.
		std::replace(name.begin(), name.end(), '\\', '/');
		auto path = directory / name;
		// Names on Windows ignore case.
		if (!std::filesystem::exists(path) && std::filesystem::is_directory(path.parent_path())) {
			const auto lower = [](std::string text) {
				std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
				return text;
			};
			for (const auto &entry : std::filesystem::directory_iterator(path.parent_path()))
				if (lower(entry.path().filename().string()) == lower(path.filename().string()))
					path = entry.path();
		}
		if (!std::filesystem::exists(path))
			return std::nullopt;
		return read_file(path);
	};
	const auto result = compile(request);
	std::cerr << result.messages;
	if (result.failed)
		return 1;
	std::ofstream out(argv[5], std::ios::binary);
	out.write(reinterpret_cast<const char *>(result.code.data()), std::streamsize(result.code.size()));
	return 0;
}

}

int main(int argc, char **argv)
{
	if (argc > 1 && std::strcmp(argv[1], "--compile") == 0)
		return compile_one(argc, argv);
	if (argc != 3) {
		std::cerr << "usage: hlsl_compile_test shader-directory work-directory\n";
		return 2;
	}
	const std::filesystem::path directory = argv[1], work = argv[2];
	std::filesystem::create_directories(work);
	const bool validate = has_tool("vkd3d-compiler") && has_tool("spirv-val");
	if (!validate)
		std::cout << "vkd3d-compiler or spirv-val is missing: containers are checked, translations are not\n";
	std::vector<std::filesystem::path> files;
	for (const auto &entry : std::filesystem::directory_iterator(directory))
		if (entry.path().extension() == ".hlsl")
			files.push_back(entry.path());
	std::sort(files.begin(), files.end());
	int failures = 0, passed = 0;
	for (const auto &file : files) {
		const std::string source = read_file(file);
		for (const auto &item : cases_of(source)) {
			CompileRequest request;
			request.source = source;
			request.name = file.filename().string();
			request.entry = item.entry;
			request.target = item.target;
			request.include = [&](const std::string &name, bool) -> std::optional<std::string> {
				const auto path = directory / name;
				if (!std::filesystem::exists(path))
					return std::nullopt;
				return read_file(path);
			};
			const auto result = compile(request);
			const std::string label = file.filename().string() + " " + item.entry + " " + item.target;
			std::string problem;
			if (item.fails) {
				if (!result.failed)
					problem = "compiled although it must fail";
				else if (result.messages.find(item.error) == std::string::npos)
					problem = "failed with an unexpected message: " + result.messages;
			} else if (result.failed)
				problem = result.messages;
			else if (item.target.starts_with("fx_")) {
				if (validate)
					effect_reads(result.code, item.target, work, file.stem().string() + "-" + item.target, contained_of(source), problem);
			} else {
				const bool legacy = item.target.size() > 3 && item.target[3] < '4';
				if ((legacy ? legacy_well_formed(result.code, item.target, problem) : well_formed(result.code, problem)) && validate)
					translates(result.code, work, file.stem().string() + "-" + item.entry + "-" + item.target, legacy, contained_of(source),
						source.find("// listed only") != std::string::npos, problem);
			}
			if (problem.empty()) {
				++passed;
				continue;
			}
			++failures;
			std::cout << "FAIL " << label << ": " << problem << "\n";
		}
	}
	std::cout << passed << " passed, " << failures << " failed\n";
	return failures ? 1 : 0;
}
