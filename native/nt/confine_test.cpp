#include "confine.h"
#include "sandbox.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/landlock.h>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
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

// A socket listening outside the confinement, by an abstract name or by a socket file.
static int listening(const std::string &name, bool abstract, sockaddr_un &address, socklen_t &length)
{
	std::memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path + (abstract ? 1 : 0), name.data(), name.size());
	length = socklen_t(offsetof(sockaddr_un, sun_path) + name.size() + (abstract ? 1 : 0));
	const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0 || ::bind(descriptor, reinterpret_cast<sockaddr *>(&address), length) || ::listen(descriptor, 4))
		return -1;
	return descriptor;
}

static bool reaches(const sockaddr_un &address, socklen_t length)
{
	const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	const bool done = descriptor >= 0 && !::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), length);
	if (descriptor >= 0)
		::close(descriptor);
	return done;
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
	for (const auto *folder : {"prefix", "granted", "later", "drafts", "home/.ssh"})
		std::filesystem::create_directories(root / folder);
	std::ofstream(root / "home/.ssh/id") << "key";
	std::ofstream(root / "granted/letter.txt") << "letter";
	std::ofstream(root / "later/picture.png") << "picture";

	// A host service on an abstract name, and one on a socket file in a folder the confinement leaves open.
	sockaddr_un abstract_address{}, file_address{};
	socklen_t abstract_length = 0, file_length = 0;
	const int abstract_service = listening("raven-confine-test-" + std::to_string(::getpid()), true, abstract_address, abstract_length);
	const int file_service = listening((root / "prefix/service").string(), false, file_address, file_length);
	check(abstract_service >= 0 && file_service >= 0, "the test cannot listen");

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

	// A folder granted for writing later: the broker makes, renames and removes things in it, and nowhere else.
	check(raven::host_mkdir(root / "drafts/notes") != 0, "a folder is made where nothing is granted");
	std::ofstream(root / "prefix/sandbox.conf", std::ios::app) << "app-write C:\\Tools\\Editor.exe\t" << (root / "drafts").string() << "\n";
	check(raven::host_mkdir(root / "drafts/notes") == 0, "the broker does not make a folder in a folder granted later");
	check(raven::host_rename(root / "drafts/notes", root / "drafts/kept") == 0, "the broker does not rename in a folder granted later");
	check(raven::host_remove(root / "drafts/kept", true) == 0, "the broker does not remove from a folder granted later");
	check(!std::filesystem::exists(root / "drafts/kept"), "the folder the broker removed is still there");
	check(raven::host_remove(root / "home/.ssh/id", false) != 0, "the broker removes a file nothing grants");
	check(raven::host_rename(root / "later/picture.png", root / "drafts/picture.png") != 0, "the broker moves a file granted only for reading");

	// Abstract socket names outside are closed where the kernel scopes them; socket files stay reachable.
	check(reaches(file_address, file_length), "the confined runtime cannot reach a socket file");
	if (::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION) >= 6)
		check(!reaches(abstract_address, abstract_length), "the confined runtime reaches an abstract socket outside");

	// The broker starts only the runtime's trusted programs.
	char self[4096];
	const auto length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
	check(length > 0, "the test cannot name itself");
	const auto started = raven::brokered_spawn({std::string(self, size_t(length)), "/bin/sh"}, {});
	check(started.first < 0, "the broker starts a program that is not the runtime's trusted one");
	std::cout << "confined\n";
	return 0;
}
