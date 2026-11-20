#include "elevation.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::cerr << "usage: installer_name_test PE_FILE\n";
		return 1;
	}
	std::ifstream file(argv[1], std::ios::binary);
	if (!file) {
		std::cerr << "cannot read " << argv[1] << '\n';
		return 2;
	}
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
	const raven::PEImage image(bytes);
	if (std::filesystem::path(argv[1]).filename() == "BvSshClient-Inst.exe") {
		if (!raven::installer_like(image, "BvSshClient-Inst.exe")) {
			std::cerr << "Bitvise SSH Client installer was not recognized\n";
			return 3;
		}
		return 0;
	}
	if (!raven::installer_like(image, "BvSshClient-Inst.exe") ||
		raven::installer_like(image, "BvSshClient-Instance.exe") ||
		raven::installer_like(image, "BvSshClient.exe")) {
		std::cerr << "installer filename token matching is incorrect\n";
		return 4;
	}
	return 0;
}
