#include "sandbox.h"
#include "registry.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <sys/un.h>

static void check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

int main()
{
	using Access = raven::Sandbox::Access;
	const auto root = std::filesystem::absolute("sandbox-test-tree");
	std::filesystem::remove_all(root);
	for (const auto *folder : {"home/user/Documents/letters", "home/user/Pictures", "home/user/.ssh", "prefix"})
		std::filesystem::create_directories(root / folder);
	std::ofstream(root / "home/user/.ssh/id") << "key";
	std::ofstream(root / "home/user/Pictures/cat.png") << "cat";
	// A link inside a granted folder that leads out of it, and another name for the folder above the grants.
	std::filesystem::create_directory_symlink(root / "home/user/.ssh", root / "home/user/Documents/keys");
	std::filesystem::create_directory_symlink(root / "home", root / "house");

	raven::Sandbox sandbox;
	check(sandbox.access(root / "home") == Access::none, "a sandbox without grants reaches the host");
	std::ofstream(root / "prefix/sandbox.conf") << "write " << (root / "home/user/Documents").string() << "\nread "
		<< (root / "home/user/Pictures/cat.png").string() << "\nnetwork off\nread relative/path\n";
	sandbox.load(root / "prefix");
	check(sandbox.grants().size() == 2, "the configuration's grants differ");
	check(!sandbox.network(), "the network stays on");

	check(sandbox.access(root / "home/user/Documents") == Access::write, "the granted folder cannot be written");
	check(sandbox.access(root / "home/user/Documents/letters/new.txt") == Access::write, "a new file in the granted folder cannot be made");
	check(sandbox.access(root / "home/user/Pictures/cat.png") == Access::read, "the file granted for reading differs");
	check(sandbox.access(root / "home/user/Pictures") == Access::traverse, "the folder of a granted file does not lead to it");
	check(sandbox.access(root / "home/user/Pictures/dog.png") == Access::none, "a file beside a granted one is reachable");
	check(sandbox.access(root / "home") == Access::traverse, "a folder above the grants does not lead to them");
	check(sandbox.access(root / "home/user/.ssh/id") == Access::none, "a folder beside the grants is reachable");
	check(sandbox.access(root / "home/user/Documents/keys/id") == Access::none, "a link leads out of a granted folder");
	check(sandbox.access(root / "home/user/Documents/../.ssh/id") == Access::none, "a parent step leads out of a granted folder");
	check(sandbox.access(root / "house/user/Documents/letters") == Access::write, "another name of a granted folder is refused");

	// A grant of this run goes on to the programs it starts; the configuration's are read by each of them.
	sandbox.grant(root / "home/user/Pictures", false);
	const auto options = sandbox.child_options();
	check(options.size() == 1 && options[0] == "--grant-read=" + (root / "home/user/Pictures").string(), "the child's options differ");
	check(sandbox.access(root / "home/user/Pictures/dog.png") == Access::read, "the grant of this run is not in effect");

	// A grant for one program, recorded by the file dialog: only a trusted program records one, and only its program sees it.
	check(!sandbox.record(root / "home/user/.ssh", true, u"C:\\Tools\\Editor.exe"), "a program that is not trusted records a grant");
	raven::Sandbox dialog;
	dialog.set_trusted(true);
	dialog.load(root / "prefix");
	check(dialog.access(root / "home/user/.ssh/id") == Access::write, "the file dialog does not see the host");
	check(dialog.record(root / "home/user/.ssh/id", false, u"C:\\Tools\\Editor.exe"), "the file dialog cannot record a grant");
	raven::Sandbox editor, other;
	editor.set_program(u"c:\\tools\\editor.exe");
	editor.load(root / "prefix");
	other.set_program(u"C:\\Tools\\Other.exe");
	other.load(root / "prefix");
	check(editor.access(root / "home/user/.ssh/id") == Access::read, "the program's own grant is not in effect");
	check(other.access(root / "home/user/.ssh/id") == Access::none, "another program's grant is in effect");
	// A program that is running sees a grant recorded meanwhile.
	check(dialog.record(root / "home/user/.ssh", true, u"C:\\Tools\\Other.exe"), "the second grant is not recorded");
	check(other.access(root / "home/user/.ssh/id") == Access::write, "a grant recorded while the program runs is not seen");
	check(!sandbox.open_address("file:///etc/passwd") && !sandbox.open_address("https://example.org/a b") &&
		!sandbox.open_address("javascript:alert(1)"), "an address that is not a web or mail one leaves the sandbox");

	sandbox.refuse(root / "home/user/.ssh/id", "open");
	std::ifstream log(root / "prefix/sandbox.log");
	std::string line, last;
	int lines = 0;
	for (; std::getline(log, line); ++lines)
		last = line;
	check(lines == 4 && last.ends_with("\topen\t" + (root / "home/user/.ssh/id").string()), "the refusals are not logged");

	// Without the network a program reaches only the computer itself, and a refused address is logged.
	sockaddr_in outside{}, loopback{};
	outside.sin_family = loopback.sin_family = AF_INET;
	outside.sin_addr.s_addr = htonl(0x08080808);
	loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sockaddr_in6 own{};
	own.sin6_family = AF_INET6;
	own.sin6_addr = in6addr_loopback;
	sockaddr_un local{};
	local.sun_family = AF_UNIX;
	check(!sandbox.reaches(reinterpret_cast<sockaddr *>(&outside), sizeof(outside)), "an address outside the computer is reached");
	check(sandbox.reaches(reinterpret_cast<sockaddr *>(&loopback), sizeof(loopback)) &&
		sandbox.reaches(reinterpret_cast<sockaddr *>(&own), sizeof(own)) && sandbox.reaches(reinterpret_cast<sockaddr *>(&local), sizeof(local)),
		"the computer's own addresses are refused");
	std::ifstream again(root / "prefix/sandbox.log");
	for (lines = 0; std::getline(again, line); ++lines)
		last = line;
	check(lines == 5 && last.ends_with("\tnetwork\t8.8.8.8"), "the refused address is not logged");
	// The network taken from one program only.
	std::ofstream(root / "prefix/sandbox.conf", std::ios::trunc) << "app-network-off C:\\Tools\\Editor.exe\n";
	check(!editor.network() && other.network(), "the network of one program is not taken away alone");
	check(editor.reaches(reinterpret_cast<sockaddr *>(&loopback), sizeof(loopback)) &&
		!editor.reaches(reinterpret_cast<sockaddr *>(&outside), sizeof(outside)) && other.reaches(reinterpret_cast<sockaddr *>(&outside), sizeof(outside)),
		"one program's network is not kept apart");

	// The Linux programs the prefix shows its programs, and who opens a kind of file.
	std::filesystem::create_directories(root / "data/applications");
	std::ofstream(root / "data/applications/viewer.desktop") << "[Desktop Entry]\nType=Application\nName=Viewer\nExec=true %F\n";
	::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
	std::ofstream(root / "prefix/sandbox.conf", std::ios::trunc) << "host-app Viewer\tviewer.desktop\nhandler .PDF\tlinux:Viewer\napp-read C:\\Tools\\Editor.exe\t"
		<< (root / "home/user/Pictures").string() << "\nhost-app broken\n";
	const auto apps = other.host_apps();
	check(apps.size() == 1 && apps[0].name == "Viewer" && apps[0].desktop == "viewer.desktop", "the exposed Linux programs differ");
	check(other.handler(".pdf") == "linux:Viewer" && other.handler(".txt").empty(), "the choice of who opens a kind differs");
	check(other.access_as(root / "home/user/Pictures/cat.png", u"C:\\Tools\\Editor.exe") == Access::read &&
		other.access_as(root / "home/user/Pictures/cat.png", u"C:\\Tools\\Other.exe") == Access::none, "another program's access differs");
	// A bridge hands on only what the program that started it reaches.
	check(!other.open_on_host("Viewer", {(root / "home/user/.ssh/id").string()}), "a bridge hands a Linux program a file its caller does not reach");
	check(!other.open_on_host("Missing", {}), "a program the prefix does not expose is started");
	check(!other.open_on_host("Viewer", {"file:///etc/passwd"}), "a bridge hands a Linux program a file address");

	// The desktop gets an entry for a kind of address only when the prefix lets it hand that kind in and a program registered for it.
	const auto entry = [&] {
		for (const auto &file : std::filesystem::directory_iterator(root / "data/applications"))
			if (file.path().filename().string().starts_with("raven-link-"))
				return file.path();
		return std::filesystem::path();
	};
	std::ofstream(root / "prefix/sandbox.conf", std::ios::trunc) << "link NXM\nlink ../x\nlink ravtest\n";
	check(other.links() == std::vector<std::string>{"nxm", "ravtest"}, "the kinds of address the desktop may hand in differ");
	other.publish_links();
	check(entry().empty(), "an entry is written for a kind no program registered");
	{
		raven::NativeRegistry hive(root / "prefix/registry.dat");
		uint64_t key = 0;
		uint32_t disposition = 0;
		check(!hive.create(0x80000000ULL, u"nxm", key, disposition) && !hive.set(key, u"URL Protocol", 1, std::vector<uint8_t>{0, 0}),
			"the kind cannot be registered");
	}
	other.publish_links();
	std::ifstream written(entry());
	const std::string text((std::istreambuf_iterator<char>(written)), {});
	check(entry().filename().string().ends_with("-nxm.desktop") && text.find("\nMimeType=x-scheme-handler/nxm;\n") != text.npos &&
		text.find("\nExec=env \"RAVEN_PREFIX=" + root.string() + "/prefix\" ") != text.npos &&
		text.find("rundll32.exe\" -- shell32.dll,ShellExec_RunDLL %u\n") != text.npos, "the entry differs");
	// A prefix whose path holds a blank, a dollar and a percent sign: the command quotes and escapes it as the specification asks.
	std::filesystem::create_directories(root / "odd $%");
	std::filesystem::copy_file(root / "prefix/registry.dat", root / "odd $%/registry.dat");
	std::ofstream(root / "odd $%/sandbox.conf") << "link nxm\n";
	raven::Sandbox odd;
	odd.load(root / "odd $%");
	odd.publish_links();
	bool quoted = false;
	for (const auto &file : std::filesystem::directory_iterator(root / "data/applications")) {
		std::ifstream input(file.path());
		const std::string content((std::istreambuf_iterator<char>(input)), {});
		quoted = quoted || content.find("Exec=env \"RAVEN_PREFIX=" + root.string() + "/odd \\\\$%%\" ") != content.npos;
	}
	check(quoted, "the command of a prefix with a blank, a dollar and a percent sign in its path is not escaped");
	std::ofstream(root / "odd $%/sandbox.conf", std::ios::trunc) << "\n";
	odd.publish_links();
	std::ofstream(root / "prefix/sandbox.conf", std::ios::trunc) << "handler nxm\twindows\n";
	other.publish_links();
	check(entry().empty(), "the entry stays when the prefix no longer lets the desktop hand the kind in");
	std::filesystem::remove_all(root);
	return 0;
}
