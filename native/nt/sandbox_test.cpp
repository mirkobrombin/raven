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

	const auto options = sandbox.child_options();
	check(options.size() == 2 && options[0] == "--grant-write=" + (root / "home/user/Documents").string(), "the child's options differ");
	check(!sandbox.open_address("file:///etc/passwd") && !sandbox.open_address("https://example.org/a b") &&
		!sandbox.open_address("javascript:alert(1)"), "an address that is not a web or mail one leaves the sandbox");

	sandbox.refuse(root / "home/user/.ssh/id", "open");
	std::ifstream log(root / "prefix/sandbox.log");
	std::string line, last;
	int lines = 0;
	for (; std::getline(log, line); ++lines)
		last = line;
	check(lines == 4 && last.ends_with(" open " + (root / "home/user/.ssh/id").string()), "the refusals are not logged");
	std::filesystem::remove_all(root);
	return 0;
}
