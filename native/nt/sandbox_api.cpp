#include "kernel_context.h"
#include "sandbox.h"
#include "snapshot.h"
#include "text.h"

#include <fstream>
#include <sstream>
#include <unistd.h>

namespace raven {

namespace {

// The runtime's own control panel, which sees every host file already: what it chooses needs no grant.
bool runtime_panel(std::u16string_view program)
{
	std::u16string lower(program);
	for (auto &character : lower)
		if (character >= u'A' && character <= u'Z')
			character = char16_t(character - u'A' + u'a');
	return lower == u"c:\\windows\\system32\\control.exe" || lower == u"c:\\windows\\syswow64\\control.exe";
}

std::string file_text(const std::filesystem::path &file)
{
	std::ifstream input(file);
	std::ostringstream text;
	text << input.rdbuf();
	return text.str();
}

// A snapshot's name as the panel passes it back: one folder name of the store, never a path.
bool plain_name(std::string_view name)
{
	return !name.empty() && name != "." && name != ".." && name.find('/') == name.npos;
}

}

// The runtime's private calls for its own control panel and file dialog, imported from ravenhost.dll. Each refuses a program that is not
// trusted: what they change is what keeps the programs of the prefix apart from the host.
void register_sandbox_api(ApiRegistry &registry, KernelContext &context)
{
	auto &paths = context.paths;
	const auto prefix = [] { return NativePrefix::from_environment().root(); };
	// The text a call hands back: copied when it fits, its length (without the terminator) returned either way.
	const auto give = [](ApiCall &call, const std::string &text) { call.ret(call.put_wide(call[0], from_utf8(text), call.u32(1))); };

	// RavenGrantPath(path, write): the file dialog grants the file or folder the person chose to the program that opened the dialog.
	registry.add("RavenGrantPath", 2, [&paths](ApiCall &call) {
		const auto &owner = sandbox().parent();
		if (!sandbox().trusted() || owner.empty()) {
			call.fail(5, 0);
			return;
		}
		const auto chosen = paths.resolve(call.wide(call[0]));
		if (!chosen || chosen->denied) {
			call.fail(3, 0);
			return;
		}
		// A place in the prefix needs no grant, and neither does the control panel.
		if (paths.in_prefix(chosen->host) || runtime_panel(owner)) {
			call.ret(1);
			return;
		}
		call.ret(sandbox().record(chosen->host, call.u32(1) != 0, owner) ? 1 : 0);
	});
	// RavenSandboxRead(buffer, capacity) and RavenSandboxWrite(text): the prefix's sandbox.conf, which the control panel shows and edits.
	registry.add("RavenSandboxRead", 2, [prefix, give](ApiCall &call) {
		if (!sandbox().trusted()) {
			call.fail(5, 0);
			return;
		}
		give(call, file_text(prefix() / "sandbox.conf"));
	});
	registry.add("RavenSandboxWrite", 1, [prefix](ApiCall &call) {
		if (!sandbox().trusted()) {
			call.fail(5, 0);
			return;
		}
		const auto text = to_utf8(call.wide(call[0], 1 << 20));
		const auto file = prefix() / "sandbox.conf", partial = prefix() / "sandbox.conf.partial";
		{
			std::ofstream output(partial, std::ios::trunc);
			output << text;
			if (!output) {
				call.fail(29, 0);
				return;
			}
		}
		std::error_code error;
		std::filesystem::rename(partial, file, error);
		call.ret(error ? 0 : 1);
		if (error)
			call.set_last_error(29);
	});
	// RavenSandboxLog(buffer, capacity): the refusals noted in sandbox.log, newest last.
	registry.add("RavenSandboxLog", 2, [prefix, give](ApiCall &call) {
		if (!sandbox().trusted()) {
			call.fail(5, 0);
			return;
		}
		give(call, file_text(prefix() / "sandbox.log"));
	});
	// RavenSnapshotList(buffer, capacity): the snapshots of the prefix, one name a line, oldest first.
	registry.add("RavenSnapshotList", 2, [prefix, give](ApiCall &call) {
		if (!sandbox().trusted()) {
			call.fail(5, 0);
			return;
		}
		std::string text;
		for (const auto &snapshot : list_snapshots(prefix()))
			text += snapshot.filename().string() + "\n";
		give(call, text);
	});
	// RavenSnapshotTake(label), RavenSnapshotRestore(name) and RavenSnapshotDelete(name).
	registry.add("RavenSnapshotTake", 1, [prefix](ApiCall &call) {
		if (!sandbox().trusted()) {
			call.fail(5, 0);
			return;
		}
		try {
			take_snapshot(prefix(), to_utf8(call.wide(call[0], 64)));
			call.ret(1);
		} catch (const std::exception &) {
			call.fail(29, 0);
		}
	});
	registry.add("RavenSnapshotRestore", 1, [prefix](ApiCall &call) {
		const auto name = to_utf8(call.wide(call[0], 260));
		if (!sandbox().trusted() || !plain_name(name)) {
			call.fail(sandbox().trusted() ? 87 : 5, 0);
			return;
		}
		try {
			restore_snapshot(prefix(), name);
			call.ret(1);
		} catch (const std::exception &) {
			call.fail(2, 0);
		}
	});
	registry.add("RavenSnapshotDelete", 1, [prefix](ApiCall &call) {
		const auto name = to_utf8(call.wide(call[0], 260));
		if (!sandbox().trusted() || !plain_name(name)) {
			call.fail(sandbox().trusted() ? 87 : 5, 0);
			return;
		}
		const auto snapshot = snapshot_directory(prefix()) / name;
		std::error_code error;
		if (!std::filesystem::is_directory(snapshot, error)) {
			call.fail(2, 0);
			return;
		}
		std::filesystem::remove_all(snapshot, error);
		call.ret(error ? 0 : 1);
	});
}

}
