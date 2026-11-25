#include "paths.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>

static void check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

static bool same(const struct stat &left, const struct stat &right)
{
	return left.st_dev == right.st_dev && left.st_ino == right.st_ino && left.st_mode == right.st_mode && left.st_size == right.st_size;
}

// What NativePaths::status reads from a drive's folder held open is what lstat and stat read from the whole path: files, folders, the
// drive's root, links followed or not, missing names, and paths outside the drives.
int main()
{
	const auto root = std::filesystem::absolute("paths-status-tree");
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root / "drive_c/windows");
	std::filesystem::create_directories(root / "drive_d");
	std::ofstream(root / "drive_c/windows/win.ini") << "[fonts]\n";
	std::ofstream(root / "drive_d/data.bin") << "0123456789";
	std::filesystem::create_symlink(root / "drive_c/windows/win.ini", root / "drive_c/link.ini");
	const raven::NativePrefix prefix(root);
	const raven::NativePaths paths(prefix);
	for (const auto &host : {root / "drive_c/windows/win.ini", root / "drive_c/windows", root / "drive_c/", root / "drive_d/data.bin",
			 root / "drive_c/link.ini", root / "registry.dat.missing", root, std::filesystem::path("/")}) {
		for (const bool follow : {false, true}) {
			struct stat expected{}, found{};
			const int wanted = follow ? ::stat(host.c_str(), &expected) : ::lstat(host.c_str(), &expected);
			const int got = paths.status(host, found, follow);
			check((wanted == 0) == (got == 0), "a path is found by one of the two reads only");
			check(wanted != 0 || same(expected, found), "the two reads of a path differ");
		}
	}
	struct stat link{};
	check(paths.status(root / "drive_c/link.ini", link, false) == 0 && S_ISLNK(link.st_mode), "a link is followed though it should not");
	struct stat missing{};
	check(paths.status(root / "drive_c/windows/none.ini", missing, false) != 0 && errno == ENOENT, "a missing name is found");
	std::filesystem::remove_all(root);
	return 0;
}
