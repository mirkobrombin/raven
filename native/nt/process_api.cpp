#include "access_token.h"
#include "child_process.h"
#include "kernel_context.h"
#include "text.h"
#include "pe/resources.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <map>
#include <memory>
#include <set>

namespace raven {

namespace {

constexpr uint64_t current_process = UINT64_MAX;

// Whether the guest range can be read, and optionally written back unchanged.
bool accessible(ApiCall &call, uint64_t address, size_t size, bool writable)
{
	if (!size)
		return true;
	try {
		const auto bytes = call.read(address, size);
		if (writable)
			call.write(address, bytes);
		return true;
	} catch (const ApiFault &) {
		return false;
	}
}

std::u16string atom_text(ApiCall &call, uint64_t name)
{
	return call.wide(name, 256);
}

int compare_text(const std::string &left, const std::string &right, uint32_t flags)
{
	const bool ignore_case = flags & 1;
	const size_t length = std::min(left.size(), right.size());
	for (size_t i = 0; i < length; ++i) {
		auto a = static_cast<unsigned char>(left[i]), b = static_cast<unsigned char>(right[i]);
		if (ignore_case) {
			a = static_cast<unsigned char>(std::tolower(a));
			b = static_cast<unsigned char>(std::tolower(b));
		}
		if (a != b)
			return a < b ? 1 : 3;
	}
	return left.size() == right.size() ? 2 : left.size() < right.size() ? 1 : 3;
}

// printf formatting of a guest format string with a 64-bit va_list: every argument takes an eight-byte slot, doubles as their bits.
std::string format_guest(ApiCall &call, uint64_t format, uint64_t arguments)
{
	const auto text = call.narrow(format, 4096);
	std::string out;
	const auto next = [&]() {
		const uint64_t value = call.load<uint64_t>(arguments);
		arguments += 8;
		return value;
	};
	for (size_t at = 0; at < text.size(); ++at) {
		if (text[at] != '%') {
			out += text[at];
			continue;
		}
		std::string spec = "%";
		++at;
		while (at < text.size() && std::strchr("-+ #0", text[at]))
			spec += text[at++];
		if (at < text.size() && text[at] == '*') {
			spec += std::to_string(int32_t(next()));
			++at;
		}
		while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])))
			spec += text[at++];
		if (at < text.size() && text[at] == '.') {
			spec += text[at++];
			if (at < text.size() && text[at] == '*') {
				spec += std::to_string(int32_t(next()));
				++at;
			}
			while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])))
				spec += text[at++];
		}
		int longs = 0;
		bool wide = false;
		while (at < text.size() && std::strchr("hlLIwzjt", text[at])) {
			if (text[at] == 'l' || text[at] == 'w')
				++longs, wide = true;
			if (text[at] == 'I' && text.compare(at, 3, "I64") == 0)
				longs = 2, at += 2;
			else if (text[at] == 'I' && text.compare(at, 3, "I32") == 0)
				at += 2;
			else if (text[at] == 'z' || text[at] == 'j' || text[at] == 't' || text[at] == 'I')
				longs = 2;
			++at;
		}
		if (at >= text.size())
			break;
		const char conversion = text[at];
		char buffer[512];
		switch (conversion) {
		case 'd': case 'i': {
			const uint64_t value = next();
			const long long number = longs >= 2 ? (long long)value : (long long)int32_t(value);
			std::snprintf(buffer, sizeof buffer, (spec + "lld").c_str(), number);
			out += buffer;
			break;
		}
		case 'u': case 'x': case 'X': case 'o': {
			const uint64_t value = next();
			std::snprintf(buffer, sizeof buffer, (spec + "ll" + conversion).c_str(), (unsigned long long)(longs >= 2 ? value : uint32_t(value)));
			out += buffer;
			break;
		}
		case 'p':
			std::snprintf(buffer, sizeof buffer, "%016llX", (unsigned long long)next());
			out += buffer;
			break;
		case 'c':
			out += char(next());
			break;
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
			const uint64_t bits = next();
			double value;
			std::memcpy(&value, &bits, 8);
			std::snprintf(buffer, sizeof buffer, (spec + conversion).c_str(), value);
			out += buffer;
			break;
		}
		case 's': case 'S': {
			const uint64_t address = next();
			std::string value = !address ? "(null)" : (conversion == 'S') != wide ? to_utf8(call.wide(address, 4096)) : call.narrow(address, 4096);
			if (conversion == 'S' && wide)
				value = call.narrow(address, 4096);
			std::snprintf(buffer, sizeof buffer, (spec + "s").c_str(), value.c_str());
			out += buffer;
			break;
		}
		case '%':
			out += '%';
			break;
		default:
			out += spec;
			out += conversion;
		}
	}
	return out;
}

}

void register_process_api(ApiRegistry &registry, KernelContext &context)
{
	registry.add("RtlDllShutdownInProgress", 0, [&context](ApiCall &call) { call.ret(context.shutdown_in_progress); });
	// _vsnprintf(buffer, count, format, arguments): at most count characters; -1 when the text did not fit, with count of it written.
	registry.add("_vsnprintf", 4, [](ApiCall &call) {
		const auto text = format_guest(call, call[2], call[3]);
		const size_t count = call[1];
		std::vector<uint8_t> bytes(text.begin(), text.end());
		if (bytes.size() < count)
			bytes.push_back(0);
		if (bytes.size() > count)
			bytes.resize(count);
		if (call[0] && !bytes.empty())
			call.write(call[0], bytes);
		call.ret(text.size() < count ? uint64_t(text.size()) : uint64_t(-1));
	});
	// Debug output goes nowhere; the function is cdecl and variadic, so it takes no counted arguments.
	registry.add("DbgPrint", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("DbgPrintEx", 0, [](ApiCall &call) { call.ret(0); });
	// No kernel debugger answers a prompt: nothing is read.
	registry.add("DbgPrompt", 3, [](ApiCall &call) { call.ret(0); });
	registry.add("DisableThreadLibraryCalls", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("AreFileApisANSI", 0, [](ApiCall &call) { call.ret(1); });
	registry.add("FreeLibraryWhenCallbackReturns", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("GetProcessVersion", 1, [](ApiCall &call) { call.ret(0x0006000a); });
	registry.add("FreeResource", 1, [](ApiCall &call) { call.ret(0); });
	// The names of a module's resources of one type, each handed to the program's callback until it answers FALSE: identifiers as they are,
	// names as strings in memory that lasts for the call.
	const auto enumerate_names = [&context](ApiCall &call, bool wide) {
		const uint64_t base = call[0] ? call[0] : context.entry.loaded.base();
		const auto module = std::find_if(context.modules.all().begin(), context.modules.all().end(),
			[&](const auto &item) { return item.second->loaded.base() == base; });
		if (module == context.modules.all().end()) {
			call.fail(1812);
			return;
		}
		const auto &image = *module->second;
		const auto directory = image.image.directories[2];
		if (!directory.address) {
			call.fail(1812);
			return;
		}
		const auto key_of = [&](uint64_t value) -> ResourceKey {
			if (value < 0x10000)
				return uint32_t(value);
			auto text = wide ? call.wide(value, 256) : from_utf8(call.narrow(value, 256));
			for (auto &character : text)
				if (character >= u'a' && character <= u'z')
					character = char16_t(character - 32);
			return text;
		};
		const auto type = key_of(call[1]);
		std::vector<ResourceKey> names;
		const auto bytes = call.read(base + directory.address, directory.size);
		for (const auto &resource : read_resources(directory, bytes, image.image.image_size))
			if (resource.type == type && std::find(names.begin(), names.end(), resource.name) == names.end())
				names.push_back(resource.name);
		if (names.empty()) {
			call.fail(1813);
			return;
		}
		for (const auto &name : names) {
			uint64_t argument = 0, text = 0;
			if (const auto *number = std::get_if<uint32_t>(&name)) {
				argument = *number;
			} else {
				const auto &string = std::get<std::u16string>(name);
				std::vector<uint8_t> bytes;
				if (wide) {
					bytes.resize((string.size() + 1) * 2);
					std::memcpy(bytes.data(), string.data(), string.size() * 2);
				} else {
					const auto narrow = to_utf8(string);
					bytes.assign(narrow.begin(), narrow.end());
					bytes.push_back(0);
				}
				text = context.allocator.allocate(bytes.size());
				call.write(text, bytes);
				argument = text;
			}
			const uint64_t arguments[] = {base, call[1], argument, call[3]};
			const auto result = context.call(call[2], arguments);
			if (text)
				context.allocator.release(text);
			if (!uint32_t(result.value))
				break;
		}
		call.ret(1);
	};
	registry.add("EnumResourceNamesW", 4, [enumerate_names](ApiCall &call) { enumerate_names(call, true); });
	registry.add("EnumResourceNamesA", 4, [enumerate_names](ApiCall &call) { enumerate_names(call, false); });
	registry.add("ReadDirectoryChangesW", 8, [](ApiCall &call) { call.fail(50, 0); });
	registry.add("FindFirstStreamW", 4, [](ApiCall &call) { call.fail(38, UINT64_MAX); });
	const auto directory = [](ApiCall &call, bool wide, const std::string &path) {
		const size_t capacity = call.u32(1);
		if (wide) {
			const auto text = from_utf8(path);
			if (capacity <= text.size()) {
				call.ret(text.size() + 1);
				return;
			}
			call.put_wide(call[0], text, capacity);
			call.ret(text.size());
		} else {
			if (capacity <= path.size()) {
				call.ret(path.size() + 1);
				return;
			}
			call.put_narrow(call[0], path, capacity);
			call.ret(path.size());
		}
	};
	const std::string windows = "C:\\Windows", system = "C:\\Windows\\System32", wow64 = "C:\\Windows\\SysWOW64";
	const auto lookup_privilege = [](ApiCall &call, bool wide) {
		static const std::pair<std::u16string_view, uint32_t> privileges[] = {
			{u"SeCreateTokenPrivilege", 2}, {u"SeAssignPrimaryTokenPrivilege", 3}, {u"SeLockMemoryPrivilege", 4}, {u"SeIncreaseQuotaPrivilege", 5},
			{u"SeTcbPrivilege", 7}, {u"SeSecurityPrivilege", 8}, {u"SeTakeOwnershipPrivilege", 9}, {u"SeLoadDriverPrivilege", 10},
			{u"SeSystemProfilePrivilege", 11}, {u"SeSystemtimePrivilege", 12}, {u"SeProfileSingleProcessPrivilege", 13},
			{u"SeIncreaseBasePriorityPrivilege", 14}, {u"SeCreatePagefilePrivilege", 15}, {u"SeCreatePermanentPrivilege", 16},
			{u"SeBackupPrivilege", 17}, {u"SeRestorePrivilege", 18}, {u"SeShutdownPrivilege", 19}, {u"SeDebugPrivilege", 20},
			{u"SeAuditPrivilege", 21}, {u"SeSystemEnvironmentPrivilege", 22}, {u"SeChangeNotifyPrivilege", 23},
			{u"SeRemoteShutdownPrivilege", 24}, {u"SeUndockPrivilege", 25}, {u"SeManageVolumePrivilege", 28},
			{u"SeImpersonatePrivilege", 29}, {u"SeCreateGlobalPrivilege", 30}, {u"SeIncreaseWorkingSetPrivilege", 33},
			{u"SeTimeZonePrivilege", 34}, {u"SeCreateSymbolicLinkPrivilege", 35}};
		if (call[0] || !call[1] || !call[2]) {
			call.fail(call[0] ? 1722 : 87);
			return;
		}
		const auto name = wide ? call.wide(call[1], 128) : from_utf8(call.narrow(call[1], 128));
		for (const auto &[known, value] : privileges)
			if (known == name) {
				call.store<uint32_t>(call[2], value);
				call.store<uint32_t>(call[2] + 4, 0);
				call.ret(1);
				return;
			}
		call.fail(1313);
	};
	registry.add("LookupPrivilegeValueW", 3, [lookup_privilege](ApiCall &call) { lookup_privilege(call, true); });
	registry.add("LookupPrivilegeValueA", 3, [lookup_privilege](ApiCall &call) { lookup_privilege(call, false); });
	registry.add("AdjustTokenPrivileges", 6, [&context](ApiCall &call) {
		const auto token = context.objects.object_as<AccessToken>(call[0]);
		if (!token) {
			call.fail(6);
			return;
		}
		const uint32_t required = 0x20 | (call[4] ? 8 : 0);
		if ((context.objects.access_of(call[0]) & required) != required) { call.fail(5); return; }
		std::vector<AccessToken::Privilege> changes;
		if (!call.u32(1) && call[2]) {
			const auto count = call.load<uint32_t>(call[2]);
			if (count > 1024) { call.fail(87); return; }
			const auto entries = call.read(call[2] + 4, size_t(count) * 12);
			for (uint32_t index = 0; index < count; ++index) {
				AccessToken::Privilege entry;
				std::memcpy(&entry.luid, entries.data() + 12 * index, 8);
				std::memcpy(&entry.attributes, entries.data() + 12 * index + 8, 4);
				changes.push_back(entry);
			}
		}
		const auto error = token->adjust_privileges(call.u32(1) != 0, changes, [&](std::span<const AccessToken::Privilege> previous) -> uint32_t {
			if (!call[4]) return 0;
			const uint32_t size = uint32_t(4 + 12 * previous.size());
			if (call[5]) call.store<uint32_t>(call[5], size);
			if (call.u32(3) < size) return 122;
			std::vector<uint8_t> data(size);
			const uint32_t count = uint32_t(previous.size());
			std::memcpy(data.data(), &count, 4);
			for (size_t index = 0; index < previous.size(); ++index) {
				std::memcpy(data.data() + 4 + 12 * index, &previous[index].luid, 8);
				std::memcpy(data.data() + 12 + 12 * index, &previous[index].attributes, 4);
			}
			call.write(call[4], data);
			return 0;
		});
		call.set_last_error(error);
		call.ret(error == 0 || error == 1300);
	});
	// ntdll's token calls take the arguments of their advapi32 forms and answer an NTSTATUS for the error those set:
	// STATUS_NOT_ALL_ASSIGNED when the token lacks some of the privileges, STATUS_BUFFER_TOO_SMALL when the buffer is short.
	const auto native_token_call = [&registry](ApiCall &call, const char *function, size_t arguments) {
		std::vector<uint64_t> words;
		for (size_t index = 0; index < arguments; ++index)
			words.push_back(call[index]);
		ApiCall inner(call.process, call.environment, std::move(words));
		const uint32_t before = call.last_error();
		call.set_last_error(0);
		registry.find(function)->second(inner);
		const uint32_t error = inner.result ? (call.last_error() == 1300 ? 1300 : 0) : call.last_error() ? call.last_error() : 87;
		call.set_last_error(before);
		switch (error) {
		case 0: call.ret(0); break;
		case 1300: call.ret(0x106); break;
		case 1: call.ret(0xc0000003U); break;
		case 5: call.ret(0xc0000022U); break;
		case 6: call.ret(0xc0000008U); break;
		case 24: call.ret(0xc0000004U); break;
		case 122: call.ret(0xc0000023U); break;
		default: call.ret(0xc000000dU); break;
		}
	};
	registry.add("NtAdjustPrivilegesToken", 6, [native_token_call](ApiCall &call) { native_token_call(call, "AdjustTokenPrivileges", 6); });
	registry.add("NtQueryInformationToken", 5, [native_token_call](ApiCall &call) { native_token_call(call, "GetTokenInformation", 5); });
	registry.add("SetFileSecurityW", 3, [&context](ApiCall &call) {
		const auto resolved = context.paths.resolve(call.wide(call[0]));
		if (!resolved) {
			call.fail(123);
			return;
		}
		if (!resolved->exists) {
			call.fail(resolved->parent_exists ? 2 : 3);
			return;
		}
		call.ret(1);
	});
	// Mitigation policies are all off; asking for one reads zeros and setting one is accepted.
	registry.add("GetProcessMitigationPolicy", 4, [](ApiCall &call) {
		if (!call[2]) {
			call.fail(87);
			return;
		}
		call.write(call[2], std::vector<uint8_t>(call.u32(3)));
		call.ret(1);
	});
	registry.add("SetProcessMitigationPolicy", 3, [](ApiCall &call) { call.ret(1); });
	// There is no debugger to receive these messages, so they go nowhere, as on Windows without one; RAVEN_TRACE_DEBUG plays the debugger
	// and prints them.
	static const bool debugger = std::getenv("RAVEN_TRACE_DEBUG") != nullptr;
	registry.add("OutputDebugStringA", 1, [](ApiCall &call) {
		if (debugger && call[0])
			std::fprintf(stderr, "debug: %s", call.narrow(call[0]).c_str());
	});
	registry.add("OutputDebugStringW", 1, [](ApiCall &call) {
		if (debugger && call[0])
			std::fprintf(stderr, "debug: %s", to_utf8(call.wide(call[0])).c_str());
	});
	// Only the inherit flag is kept: HANDLE_FLAG_INHERIT decides which handles a child created with handle inheritance receives.
	registry.add("SetHandleInformation", 3, [&context](ApiCall &call) {
		if (call[0] == UINT64_MAX || call[0] == UINT64_MAX - 1) {
			call.ret(1);
			return;
		}
		if (context.objects.signaled(call[0], 0) == -2) {
			call.fail(6);
			return;
		}
		if (call.u32(1) & 1)
			context.objects.set_inherit(call[0], call.u32(2) & 1);
		call.ret(1);
	});
	registry.add("GetHandleInformation", 2, [&context](ApiCall &call) {
		if (!call[1]) {
			call.fail(87);
			return;
		}
		const bool pseudo = call[0] == UINT64_MAX || call[0] == UINT64_MAX - 1;
		if (!pseudo && context.objects.signaled(call[0], 0) == -2) {
			call.fail(6);
			return;
		}
		call.store<uint32_t>(call[1], !pseudo && context.objects.inherit(call[0]) ? 1 : 0);
		call.ret(1);
	});
	// The search path for dependent libraries is not restricted by anything Raven resolves, so the request is accepted as it stands.
	registry.add("SetDefaultDllDirectories", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("AddDllDirectory", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("RemoveDllDirectory", 1, [](ApiCall &call) { call.ret(1); });
	// Safe search mode moves the current directory after the system ones for SearchPath; Raven's search already finds the same files, so a
	// valid request (enable or disable, perhaps permanent) is accepted and anything else is refused as Windows refuses it.
	registry.add("SetSearchPathMode", 1, [](ApiCall &call) {
		const uint32_t flags = call.u32(0), mode = flags & 0x10001U;
		if ((flags & ~0x18001U) || (mode != 0x1U && mode != 0x10000U)) {
			call.fail(87);
			return;
		}
		call.ret(1);
	});
	// The DEP policy is a 32-bit process setting; a 64-bit process always runs with it, and Windows refuses to change it.
	registry.add("SetProcessDEPPolicy", 1, [](ApiCall &call) { call.fail(50); });
	registry.add("GetWindowsDirectoryW", 2, [=](ApiCall &call) { directory(call, true, windows); });
	registry.add("GetSystemWindowsDirectoryW", 2, [=](ApiCall &call) { directory(call, true, windows); });
	registry.add("GetWindowsDirectoryA", 2, [=](ApiCall &call) { directory(call, false, windows); });
	registry.add("GetSystemWindowsDirectoryA", 2, [=](ApiCall &call) { directory(call, false, windows); });
	registry.add("GetSystemDirectoryW", 2, [=](ApiCall &call) { directory(call, true, system); });
	registry.add("GetSystemDirectoryA", 2, [=](ApiCall &call) { directory(call, false, system); });
	// The 64-bit system Raven presents keeps its 32-bit libraries in SysWOW64, whichever width asks.
	registry.add("GetSystemWow64DirectoryW", 2, [=](ApiCall &call) { directory(call, true, wow64); });
	registry.add("GetSystemWow64DirectoryA", 2, [=](ApiCall &call) { directory(call, false, wow64); });
	registry.add("lstrcatW", 2, [](ApiCall &call) {
		auto first = call.wide(call[0], 65536);
		const auto second = call.wide(call[1], 65536);
		first += second;
		call.put_wide(call[0], first, first.size() + 1);
		call.ret(call[0]);
	});
	registry.add("lstrcatA", 2, [](ApiCall &call) {
		auto first = call.narrow(call[0], 65536);
		first += call.narrow(call[1], 65536);
		call.put_narrow(call[0], first, first.size() + 1);
		call.ret(call[0]);
	});

	// One atom table serves the local and global names; atoms count from 0xc000 and integer atoms pass through.
	struct Atoms {
		std::map<std::u16string, std::pair<uint16_t, uint32_t>> names;
		uint16_t next = 0xc000;
	};
	const auto atoms = std::make_shared<Atoms>();
	const auto key = [](std::u16string text) {
		for (auto &c : text)
			if (c >= 'A' && c <= 'Z')
				c = char16_t(c + 32);
		return text;
	};
	const auto add_atom = [atoms, key](ApiCall &call) {
		if (call[0] < 0x10000) {
			call.ret(call[0]);
			return;
		}
		const auto text = atom_text(call, call[0]);
		if (text.empty() || text.size() > 255) {
			call.fail(87, 0);
			return;
		}
		auto found = atoms->names.find(key(text));
		if (found == atoms->names.end()) {
			if (atoms->next == 0xffff) {
				call.fail(8, 0);
				return;
			}
			found = atoms->names.emplace(key(text), std::pair<uint16_t, uint32_t>{atoms->next++, 0}).first;
		}
		++found->second.second;
		call.ret(found->second.first);
	};
	const auto find_atom = [atoms, key](ApiCall &call) {
		if (call[0] < 0x10000) {
			call.ret(call[0]);
			return;
		}
		const auto found = atoms->names.find(key(atom_text(call, call[0])));
		if (found == atoms->names.end())
			call.fail(2, 0);
		else
			call.ret(found->second.first);
	};
	const auto delete_atom = [atoms](ApiCall &call) {
		for (auto found = atoms->names.begin(); found != atoms->names.end(); ++found)
			if (found->second.first == call.u32(0)) {
				if (--found->second.second == 0)
					atoms->names.erase(found);
				call.ret(0);
				return;
			}
		call.fail(6, call.u32(0));
	};
	const auto atom_name = [atoms](ApiCall &call) {
		for (const auto &[text, entry] : atoms->names)
			if (entry.first == call.u32(0)) {
				const size_t capacity = call.u32(2);
				if (!capacity) {
					call.fail(122, 0);
					return;
				}
				const auto copied = call.put_wide(call[1], text, capacity);
				call.ret(std::min(copied, capacity - 1));
				return;
			}
		call.fail(6, 0);
	};
	for (const auto *name : {"GlobalAddAtomW", "AddAtomW"})
		registry.add(name, 1, add_atom);
	for (const auto *name : {"GlobalFindAtomW", "FindAtomW"})
		registry.add(name, 1, find_atom);
	for (const auto *name : {"GlobalDeleteAtom", "DeleteAtom"})
		registry.add(name, 1, delete_atom);
	for (const auto *name : {"GlobalGetAtomNameW", "GetAtomNameW"})
		registry.add(name, 3, atom_name);

	registry.add("IsBadReadPtr", 2, [](ApiCall &call) { call.ret(!accessible(call, call[0], call[1], false)); });
	registry.add("IsBadWritePtr", 2, [](ApiCall &call) { call.ret(!accessible(call, call[0], call[1], true)); });
	registry.add("IsBadCodePtr", 1, [](ApiCall &call) { call.ret(!accessible(call, call[0], 1, false)); });
	registry.add("IsBadStringPtrW", 2, [](ApiCall &call) {
		try {
			call.wide(call[0], std::min<size_t>(call[1] ? call[1] : 1, 1 << 20));
			call.ret(0);
		} catch (const ApiFault &) {
			call.ret(1);
		}
	});
	registry.add("IsBadStringPtrA", 2, [](ApiCall &call) {
		try {
			call.narrow(call[0], std::min<size_t>(call[1] ? call[1] : 1, 1 << 20));
			call.ret(0);
		} catch (const ApiFault &) {
			call.ret(1);
		}
	});
	registry.add("IsDBCSLeadByte", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("IsDBCSLeadByteEx", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("CompareStringA", 6, [](ApiCall &call) {
		const int32_t left_count = call.i32(3), right_count = call.i32(5);
		auto left = call.narrow(call[2], left_count < 0 ? 65536 : size_t(left_count));
		auto right = call.narrow(call[4], right_count < 0 ? 65536 : size_t(right_count));
		if (left_count >= 0)
			left = call.narrow(call[2], 65536).substr(0, size_t(left_count));
		if (right_count >= 0)
			right = call.narrow(call[4], 65536).substr(0, size_t(right_count));
		call.ret(uint64_t(compare_text(left, right, call.u32(1))));
	});
	registry.add("GetStringTypeExA", 5, [](ApiCall &call) {
		// C1 classification of each byte: upper 1, lower 2, digit 4, space 8, punctuation 0x10, control 0x20, blank 0x40, hex 0x80.
		const int32_t count = call.i32(3) < 0 ? int32_t(call.narrow(call[2]).size()) : call.i32(3);
		for (int32_t i = 0; i < count; ++i) {
			const auto c = call.load<uint8_t>(call[2] + uint64_t(i));
			uint16_t type = 0;
			if (c >= 'A' && c <= 'Z') type |= 1 | 0x100;
			if (c >= 'a' && c <= 'z') type |= 2 | 0x100;
			if (c >= '0' && c <= '9') type |= 4 | 0x80;
			if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) type |= 0x80;
			if (c == ' ' || (c >= 9 && c <= 13)) type |= 8;
			if (c == ' ' || c == 9) type |= 0x40;
			if (c < 32 || c == 127) type |= 0x20;
			if (c > 32 && c < 127 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) type |= 0x10;
			call.store<uint16_t>(call[4] + uint64_t(i) * 2, type);
		}
		call.ret(1);
	});
	registry.add("SetStdHandle", 2, [](ApiCall &call) { call.ret(1); });
}

}
