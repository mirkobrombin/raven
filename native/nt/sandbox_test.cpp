#include "sandbox.h"

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
	std::filesystem::remove_all(root);
	return 0;
}
