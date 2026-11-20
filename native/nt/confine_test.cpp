#include "confine.h"
#include "sandbox.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
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

// A runtime the confined one starts, as a program of its own: it confines itself in turn and opens a document with a Linux program of
// the prefix. It holds nothing of the runtime that started it but the channel it was handed, as a runtime started by exec does.
static int inner_runtime(const std::filesystem::path &root, const char *channel)
{
	if (channel)
		raven::adopt_host_broker(std::atoi(channel));
	raven::sandbox().set_program(u"C:\\Program Files\\Raven\\Host\\Viewer.exe");
	raven::sandbox().set_parent(u"C:\\Tools\\Editor.exe");
	raven::sandbox().load(root / "prefix");
	check(raven::confine(root / "prefix"), "the inner runtime is not confined");
	check(raven::sandbox().open_on_host("Viewer", {}), "the inner runtime cannot open with a Linux program of the prefix");
	check(!raven::sandbox().open_on_host("Viewer", {(root / "home/.ssh/id").string()}),
		"the inner runtime hands a Linux program a file neither program reaches");
	return 0;
}

int main(int count, char **arguments)
{
	if (count >= 3 && std::string_view(arguments[1]) == "--inner")
		return inner_runtime(arguments[2], count >= 4 ? arguments[3] : nullptr);
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
	for (const auto *folder : {"prefix", "granted", "later", "drafts", "home/.ssh", "dxvk", "vkd3d"})
		std::filesystem::create_directories(root / folder);
	std::ofstream(root / "dxvk/dxgi.dll") << "MZ";
	std::ofstream(root / "vkd3d/d3d12.dll") << "MZ";
	std::ofstream(root / "home/.ssh/id") << "key";
	std::ofstream(root / "granted/letter.txt") << "letter";
	std::ofstream(root / "later/picture.png") << "picture";
	// A Linux program the desktop lists, outside every folder the confinement leaves open, and a gio that notes the entry it launches.
	std::filesystem::create_directories(root / "host/applications");
	std::filesystem::create_directories(root / "host/bin");
	std::ofstream(root / "host/applications/viewer.desktop") << "[Desktop Entry]\nName=Viewer\n";
	std::ofstream(root / "host/bin/gio") << "#!/bin/sh\ncat \"$2\" > '" << (root / "launched").string() << "'\n";
	std::filesystem::permissions(root / "host/bin/gio", std::filesystem::perms::owner_all);
	::setenv("XDG_DATA_HOME", (root / "host").c_str(), 1);
	::setenv("PATH", ((root / "host/bin").string() + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin")).c_str(), 1);

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
		// The Linux program the inner runtime opened was started outside the confinement, where it reads its desktop entry.
		bool launched = false;
		for (int waited = 0; waited < 100 && !launched; ++waited) {
			std::ifstream note(root / "launched");
			std::string line;
			launched = std::getline(note, line) && line == "[Desktop Entry]";
			if (!launched)
				::usleep(50000);
		}
		std::filesystem::remove_all(root);
		if (WIFEXITED(status) && !WEXITSTATUS(status) && !launched) {
			std::cerr << "the Linux program a runtime started inside the confinement opens is not started outside it\n";
			return 1;
		}
		return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
	}
	raven::sandbox().set_program(u"C:\\Tools\\Editor.exe");
	raven::sandbox().load(root / "prefix");
	raven::sandbox().grant(root / "granted", false);
	// The Direct3D libraries come from the folders RAVEN_D3D_DIRECTORY lists, each of which the confined runtime still reads.
	::setenv("RAVEN_D3D_DIRECTORY", ((root / "dxvk").string() + ":" + (root / "vkd3d").string()).c_str(), 1);
	check(raven::confine(root / "prefix"), "the runtime is not confined");
	check(raven::confined(), "the runtime does not know it is confined");

	check(!opens(root / "home/.ssh/id") && errno == EACCES, "the confined runtime reads a host file it was not granted");
	check(opens(root / "granted/letter.txt"), "the confined runtime cannot read its grant");
	check(!opens(root / "granted/new.txt", O_WRONLY | O_CREAT), "the confined runtime writes into a folder granted for reading");
	check(opens(root / "prefix/settings.ini", O_WRONLY | O_CREAT), "the confined runtime cannot write into its prefix");
	check(opens("/usr"), "the confined runtime cannot read the system");
	check(opens(root / "dxvk/dxgi.dll") && opens(root / "vkd3d/d3d12.dll"), "the confined runtime cannot read the Direct3D libraries");

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
	self[length] = '\0';
	const auto started = raven::brokered_spawn({std::string(self, size_t(length)), "/bin/sh"}, {});
	check(started.first < 0, "the broker starts a program that is not the runtime's trusted one");
	// A runtime started inside the confinement opens a document with a Linux program of the prefix: its own broker is inside the
	// confinement, so the broker outside it starts the program, for the program that asked as for this one.
	std::ofstream(root / "prefix/sandbox.conf", std::ios::app) << "host-app Viewer\tviewer\n";
	const int channel = raven::host_broker_channel();
	check(channel >= 0, "the broker hands no channel to a runtime started inside the confinement");
	const pid_t inner = ::fork();
	if (inner == 0) {
		::fcntl(channel, F_SETFD, 0);
		const std::string tree = root.string(), handed = std::to_string(channel);
		::execl(self, self, "--inner", tree.c_str(), handed.c_str(), nullptr);
		std::_Exit(127);
	}
	int inner_status = 0;
	::waitpid(inner, &inner_status, 0);
	::close(channel);
	check(WIFEXITED(inner_status) && !WEXITSTATUS(inner_status), "the inner runtime failed");
	std::cout << "confined\n";
	return 0;
}
