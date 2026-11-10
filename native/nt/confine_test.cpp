#include "confine.h"
#include "sandbox.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

static bool opens(const std::filesystem::path &file, int flags = O_RDONLY)
{
	const int descriptor = ::open(file.c_str(), flags | O_CLOEXEC, 0644);
	if (descriptor >= 0)
		::close(descriptor);
	return descriptor >= 0;
}

int main()
{
	if (::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION) < 1) {
		std::cout << "the kernel has no Landlock\n";
		return 0;
	}
	// Beside the runtime's folder, which the confinement leaves readable, not inside it; and in the home when the build itself is under a
	// system folder the confinement leaves readable too.
	auto root = std::filesystem::absolute("..").lexically_normal() / "confine-test-tree";
	for (const char *system : {"/usr/", "/opt/", "/etc/", "/run/", "/tmp/", "/var/cache/"})
		if (root.string().starts_with(system) && std::getenv("HOME"))
			root = std::filesystem::path(std::getenv("HOME")) / ".raven-confine-test";
	std::filesystem::remove_all(root);
	for (const auto *folder : {"prefix", "granted", "later", "home/.ssh"})
		std::filesystem::create_directories(root / folder);
	std::ofstream(root / "home/.ssh/id") << "key";
	std::ofstream(root / "granted/letter.txt") << "letter";
	std::ofstream(root / "later/picture.png") << "picture";

	// The confined checks run in a child, so that this process can remove the tree afterwards.
	const pid_t child = ::fork();
	if (child > 0) {
		int status = 0;
		::waitpid(child, &status, 0);
		std::filesystem::remove_all(root);
		return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
	}
	raven::sandbox().set_program(u"C:\\Tools\\Editor.exe");
	raven::sandbox().load(root / "prefix");
	raven::sandbox().grant(root / "granted", false);
	check(raven::confine(root / "prefix"), "the runtime is not confined");
	check(raven::confined(), "the runtime does not know it is confined");

	check(!opens(root / "home/.ssh/id") && errno == EACCES, "the confined runtime reads a host file it was not granted");
	check(opens(root / "granted/letter.txt"), "the confined runtime cannot read its grant");
	check(!opens(root / "granted/new.txt", O_WRONLY | O_CREAT), "the confined runtime writes into a folder granted for reading");
	check(opens(root / "prefix/settings.ini", O_WRONLY | O_CREAT), "the confined runtime cannot write into its prefix");
	check(opens("/usr"), "the confined runtime cannot read the system");

	// A grant recorded while the program runs reaches it through the broker, which refuses anything else.
	check(!opens(root / "later/picture.png"), "a folder granted later is open before the grant");
	check(raven::brokered_open(root / "later/picture.png", O_RDONLY, 0) < 0, "the broker opens a path not granted");
	std::ofstream(root / "prefix/sandbox.conf") << "app-read C:\\Tools\\Editor.exe\t" << (root / "later/picture.png").string() << "\n";
	const int picture = raven::brokered_open(root / "later/picture.png", O_RDONLY, 0);
	check(picture >= 0, "the broker does not open a path granted later");
	char first = 0;
	check(::read(picture, &first, 1) == 1 && first == 'p', "the broker's file is not the granted one");
	::close(picture);
	check(raven::brokered_open(root / "later/picture.png", O_WRONLY, 0) < 0, "the broker opens a file granted for reading to be written");
	check(raven::brokered_open(root / "home/.ssh/id", O_RDONLY, 0) < 0, "the broker opens the file next to a grant");

	// The broker starts only the runtime's trusted programs.
	char self[4096];
	const auto length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
	check(length > 0, "the test cannot name itself");
	const auto started = raven::brokered_spawn({std::string(self, size_t(length)), "/bin/sh"}, {});
	check(started.first < 0, "the broker starts a program that is not the runtime's trusted one");
	std::cout << "confined\n";
	return 0;
}
