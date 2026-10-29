#include "dxbc.h"
#include "hlsl_compile.h"

#include <algorithm>
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

// One compile a test file asks for: "// compile: entry target", or "// error: entry target text" for a compile that must fail.
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

// Translates the program to SPIR-V with vkd3d-shader and validates the result, when both tools are present.
bool translates(const std::vector<uint8_t> &blob, const std::filesystem::path &work, const std::string &stem, std::string &problem)
{
	const auto dxbc = work / (stem + ".dxbc"), spirv = work / (stem + ".spv"), log = work / (stem + ".log");
	{
		std::ofstream out(dxbc, std::ios::binary);
		out.write(reinterpret_cast<const char *>(blob.data()), std::streamsize(blob.size()));
	}
	const std::string translate = "vkd3d-compiler -x dxbc-tpf -o '" + spirv.string() + "' '" + dxbc.string() + "' >'" + log.string() + "' 2>&1";
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
			else if (well_formed(result.code, problem) && validate)
				translates(result.code, work, file.stem().string() + "-" + item.entry + "-" + item.target, problem);
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
