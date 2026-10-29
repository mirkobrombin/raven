#include "registry.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nls.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace raven {

namespace {

constexpr uint32_t file_not_found = 2, bad_handle = 6, invalid_parameter = 87, key_deleted = 1018;

// A predefined key handle is a 32-bit value; a 64-bit program passes it sign-extended.
bool predefined(uint64_t key)
{
	if ((key >> 32) == 0xffffffffULL)
		key &= 0xffffffffULL;
	return key >= 0x80000000ULL && key <= 0x80000003ULL;
}

std::u16string fold(std::u16string_view text)
{
	std::u16string result(text);
	for (auto &character : result)
		if (character >= u'a' && character <= u'z')
			character = char16_t(character - (u'a' - u'A'));
	return result;
}

std::u16string decode_utf8(std::string_view text)
{
	std::u16string result;
	for (size_t index = 0; index < text.size();) {
		const auto lead = static_cast<unsigned char>(text[index]);
		const size_t extra = lead < 0x80 ? 0 : lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : 3;
		if (extra && index + extra >= text.size())
			throw std::runtime_error("registry hive contains truncated UTF-8");
		uint32_t value = extra == 0 ? lead : lead & (0x3fU >> extra);
		for (size_t part = 1; part <= extra; ++part)
			value = (value << 6) | (static_cast<unsigned char>(text[index + part]) & 0x3fU);
		index += extra + 1;
		if (value >= 0x10000) {
			value -= 0x10000;
			result.push_back(char16_t(0xd800 + (value >> 10)));
			result.push_back(char16_t(0xdc00 + (value & 0x3ff)));
		} else {
			result.push_back(char16_t(value));
		}
	}
	return result;
}

std::string hex(std::span<const uint8_t> bytes)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	for (const auto byte : bytes) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 15]);
	}
	return result;
}

std::vector<uint8_t> unhex(std::string_view text)
{
	if (text.size() % 2)
		throw std::runtime_error("registry hive contains odd hexadecimal data");
	const auto nibble = [](char c) -> uint8_t {
		if (c >= '0' && c <= '9') return uint8_t(c - '0');
		if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
		throw std::runtime_error("registry hive contains invalid hexadecimal data");
	};
	std::vector<uint8_t> result;
	for (size_t index = 0; index < text.size(); index += 2)
		result.push_back(uint8_t(nibble(text[index]) << 4 | nibble(text[index + 1])));
	return result;
}

std::string escape(std::u16string_view text)
{
	std::string result;
	for (const char character : encode_utf8(text)) {
		if (character == '\\' || character == '\t' || character == '\n' || character == '\r') {
			result.push_back('\\');
			result.push_back(character == '\\' ? '\\' : character == '\t' ? 't' : character == '\n' ? 'n' : 'r');
		} else {
			result.push_back(character);
		}
	}
	return result;
}

std::string unescape(std::string_view text)
{
	std::string result;
	for (size_t index = 0; index < text.size(); ++index) {
		if (text[index] != '\\') {
			result.push_back(text[index]);
			continue;
		}
		if (++index >= text.size())
			throw std::runtime_error("registry hive ends inside an escape");
		result.push_back(text[index] == 't' ? '\t' : text[index] == 'n' ? '\n' : text[index] == 'r' ? '\r' : text[index]);
	}
	return result;
}

}

NativeRegistry::NativeRegistry(std::filesystem::path file) : file(std::move(file))
{
	load();
	stamp = current_stamp();
	seed();
	register_runtime_classes();
	register_event_logs();
	register_system_environment();
}

// The classes Windows registers in both registry views whose libraries the runtime brings: the .NET runtime's metadata dispensers, runtime
// hosts, meta host and strong names in mscoree, which programs and the Framework's own libraries create through COM, and the Task
// Scheduler, which installers use to start a program once they finish, the accessibility annotation services of oleacc, which
// programs create for their windows' controls, the MSXML 6.0 classes, which the XML component serves beside its own, the network list,
// which programs ask whether they are online, and the package manager of the Windows Runtime. A hive that lacks them, a new one or one
// made before they were registered, gets them.
void NativeRegistry::register_runtime_classes()
{
	static constexpr std::pair<const char16_t *, const char16_t *> classes[] = {{u"{E5CB7A31-7512-11D2-89CE-0080C792E5D8}", u"mscoree.dll"},
		{u"{1EC2DE53-75CC-11D2-9775-00A0C9B4D50C}", u"mscoree.dll"}, {u"{CB2F6723-AB3A-11D2-9C40-00C04FA30A3E}", u"mscoree.dll"},
		{u"{90F1A06E-7712-4762-86B5-7A5EBA6BDB02}", u"mscoree.dll"}, {u"{9280188D-0E8E-4867-B30C-7FA83884E8DE}", u"mscoree.dll"},
		{u"{B79B0ACD-F5CD-409B-B5A5-A16244610B92}", u"mscoree.dll"}, {u"{0F87369F-A4E5-4CFC-BD3E-73E6154572DD}", u"taskschd.dll"},
		{u"{B5F8350B-0548-48B1-A6EE-88BD00B4A5E7}", u"raven-accprop.dll"}, {u"{88D96A05-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"},
		{u"{88D96A06-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"}, {u"{88D96A07-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"},
		{u"{88D96A08-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"}, {u"{88D96A0A-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"},
		{u"{88D96A0B-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"}, {u"{88D96A0C-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"},
		{u"{88D96A0E-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"}, {u"{88D96A0F-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"},
		{u"{88D96A11-F192-11D4-A65F-0040963251E5}", u"msxml3.dll"}, {u"{DCB00C01-570F-4A9B-8D69-199FDBA5723B}", u"raven-netlist.dll"}};
	register_animation_classes();
	const auto store = [&](uint64_t key, const char16_t *name, std::u16string_view value) {
		std::vector<uint8_t> data;
		for (const auto character : value) {
			data.push_back(uint8_t(character));
			data.push_back(uint8_t(character >> 8));
		}
		data.insert(data.end(), {0, 0});
		set(key, name, 1, data);
	};
	for (const bool wow : {false, true})
		for (const auto &[clsid, library] : classes) {
			const std::u16string path = std::u16string(wow ? u"Wow6432Node\\CLSID\\" : u"CLSID\\") + clsid + u"\\InprocServer32";
			if (keys.contains(fold(u"HKEY_CLASSES_ROOT\\" + path)))
				continue;
			uint64_t key = 0;
			uint32_t disposition = 0;
			if (create(0x80000000ULL, path, key, disposition))
				continue;
			store(key, u"", std::u16string(wow ? u"C:\\Windows\\SysWOW64\\" : u"C:\\Windows\\System32\\") + library);
			store(key, u"ThreadingModel", u"Both");
			close(key);
		}
	// Scripts create the scheduler, and programs the MSXML 6.0 classes, by name.
	static constexpr std::pair<const char16_t *, const char16_t *> names[] = {{u"Schedule.Service", u"{0F87369F-A4E5-4CFC-BD3E-73E6154572DD}"},
		{u"Msxml2.DOMDocument.6.0", u"{88D96A05-F192-11D4-A65F-0040963251E5}"}, {u"Msxml2.FreeThreadedDOMDocument.6.0", u"{88D96A06-F192-11D4-A65F-0040963251E5}"},
		{u"Msxml2.XMLSchemaCache.6.0", u"{88D96A07-F192-11D4-A65F-0040963251E5}"}, {u"Msxml2.XSLTemplate.6.0", u"{88D96A08-F192-11D4-A65F-0040963251E5}"},
		{u"Msxml2.XMLHTTP.6.0", u"{88D96A0A-F192-11D4-A65F-0040963251E5}"}, {u"Msxml2.ServerXMLHTTP.6.0", u"{88D96A0B-F192-11D4-A65F-0040963251E5}"},
		{u"Msxml2.SAXXMLReader.6.0", u"{88D96A0C-F192-11D4-A65F-0040963251E5}"}, {u"Msxml2.SAXAttributes.6.0", u"{88D96A0E-F192-11D4-A65F-0040963251E5}"},
		{u"Msxml2.MXXMLWriter.6.0", u"{88D96A0F-F192-11D4-A65F-0040963251E5}"}, {u"Msxml2.MXNamespaceManager.6.0", u"{88D96A11-F192-11D4-A65F-0040963251E5}"}};
	for (const auto &[name, clsid] : names) {
		const std::u16string path = std::u16string(name) + u"\\CLSID";
		if (keys.contains(fold(u"HKEY_CLASSES_ROOT\\" + path)))
			continue;
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (!create(0x80000000ULL, path, key, disposition)) {
			store(key, u"", clsid);
			close(key);
		}
	}
	// The Windows Runtime classes the runtime brings, in-process (activation type 0) and callable from any apartment (threading 0):
	// installers ask the package manager whether a packaged version of what they install is present, and Office asks the UI settings
	// for the theme's colours and the text scale while it starts, along with the network, the device's profile, the system's accounts
	// and the application model.
	static constexpr std::pair<const char16_t *, const char16_t *> runtime_classes[] = {
		{u"Windows.Management.Deployment.PackageManager", u"raven-deployment.dll"},
		{u"Windows.UI.ViewManagement.UISettings", u"raven-uisettings.dll"},
		{u"Windows.System.Profile.RetailInfo", u"raven-twinapi.dll"}, {u"Windows.System.Profile.SharedModeSettings", u"raven-twinapi.dll"},
		{u"Windows.Networking.Connectivity.NetworkInformation", u"raven-twinapi.dll"},
		{u"Windows.Security.Authentication.OnlineId.OnlineIdSystemAuthenticator", u"raven-twinapi.dll"},
		{u"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager", u"raven-twinapi.dll"},
		{u"Windows.ApplicationModel.Core.CoreApplication", u"raven-twinapi.dll"}};
	for (const bool wow : {false, true})
		for (const auto &[name, library] : runtime_classes) {
			const std::u16string path = std::u16string(wow ? u"SOFTWARE\\Wow6432Node" : u"SOFTWARE") +
				u"\\Microsoft\\WindowsRuntime\\ActivatableClassId\\" + name;
			if (keys.contains(fold(u"HKEY_LOCAL_MACHINE\\" + path)))
				continue;
			uint64_t key = 0;
			uint32_t disposition = 0;
			if (create(0x80000002ULL, path, key, disposition))
				continue;
			store(key, u"DllPath", std::u16string(wow ? u"C:\\Windows\\SysWOW64\\" : u"C:\\Windows\\System32\\") + library);
			static constexpr uint8_t zero[4]{};
			for (const auto *word : {u"ActivationType", u"Threading", u"TrustLevel"})
				set(key, word, 4, zero);
			close(key);
		}
}

// The machine's environment, which installers extend (a program's folder added to Path) and the system reads when it starts a session.
// A hive that lacks the key gets the variables every installation has there.
void NativeRegistry::register_system_environment()
{
	const std::u16string path = u"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
	if (keys.contains(fold(u"HKEY_LOCAL_MACHINE\\" + path)))
		return;
	uint64_t key = 0;
	uint32_t disposition = 0;
	if (create(0x80000002ULL, path, key, disposition))
		return;
	static constexpr std::pair<const char16_t *, const char16_t *> values[] = {{u"ComSpec", u"%SystemRoot%\\system32\\cmd.exe"},
		{u"OS", u"Windows_NT"}, {u"Path", u"%SystemRoot%\\system32;%SystemRoot%;%SystemRoot%\\System32\\Wbem"},
		{u"PATHEXT", u".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"}, {u"PROCESSOR_ARCHITECTURE", u"AMD64"},
		{u"TEMP", u"%SystemRoot%\\TEMP"}, {u"TMP", u"%SystemRoot%\\TEMP"}, {u"windir", u"%SystemRoot%"}};
	for (const auto &[name, value] : values) {
		std::vector<uint8_t> data;
		for (const char16_t *at = value; *at; ++at) {
			data.push_back(uint8_t(*at));
			data.push_back(uint8_t(*at >> 8));
		}
		data.insert(data.end(), {0, 0});
		const std::u16string_view text(value);
		set(key, name, text.find(u'%') != text.npos ? 2 : 1, data);
	}
	close(key);
}

// The event logs every Windows installation keeps, under the service that writes them: programs register their event sources as subkeys
// of a log and fail when the log's key is missing. A hive that lacks them, a new one or one made before they were added, gets them.
void NativeRegistry::register_event_logs()
{
	for (const std::u16string log : {u"Application", u"Security", u"System"}) {
		const auto path = u"SYSTEM\\CurrentControlSet\\Services\\EventLog\\" + log;
		if (keys.contains(fold(u"HKEY_LOCAL_MACHINE\\" + path)))
			continue;
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (create(0x80000002ULL, path, key, disposition))
			continue;
		std::vector<uint8_t> data;
		for (const auto character : u"%SystemRoot%\\System32\\Winevt\\Logs\\" + log + u".evtx") {
			data.push_back(uint8_t(character));
			data.push_back(uint8_t(character >> 8));
		}
		data.insert(data.end(), {0, 0});
		set(key, u"File", 2, data);
		close(key);
	}
}

// The Windows Animation classes of UIAnimation.dll (the manager, the transition library, the transition factory and the timer), which the runtime's own component
// serves to 64-bit programs.
void NativeRegistry::register_animation_classes()
{
	static constexpr const char16_t *classes[] = {u"{4C1FC63A-695C-47E8-A339-1A194BE3D0B8}", u"{1D6322AD-AA85-4EF5-A828-86D71067D145}",
		u"{BFCD4A0C-06B6-4384-B768-0DAA792C380E}", u"{8A9B1CDD-FCD7-419C-8B44-42FD17DB1887}"};
	for (const auto *clsid : classes) {
		const std::u16string path = std::u16string(u"CLSID\\") + clsid + u"\\InprocServer32";
		if (keys.contains(fold(u"HKEY_CLASSES_ROOT\\" + path)))
			continue;
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (create(0x80000000ULL, path, key, disposition))
			continue;
		const auto store = [&](const char16_t *name, std::u16string_view value) {
			std::vector<uint8_t> data;
			for (const auto character : value) {
				data.push_back(uint8_t(character));
				data.push_back(uint8_t(character >> 8));
			}
			data.insert(data.end(), {0, 0});
			set(key, name, 1, data);
		};
		store(u"", u"C:\\Windows\\System32\\UIAnimation.dll");
		store(u"ThreadingModel", u"Both");
		close(key);
	}
}

void NativeRegistry::seed()
{
	seeding = true;
	// A hive that has some of these already, such as one imported from an installation, keeps what it holds and gains only what it lacks.
	bool added = false;
	const auto fill = [&](uint64_t key, std::u16string_view name, uint32_t type, std::span<const uint8_t> data) {
		uint32_t present_type = 0;
		std::vector<uint8_t> present;
		if (query(key, name, present_type, present) == 2) {
			set(key, name, type, data);
			added = true;
		}
	};
	const auto bytes_of = [](std::u16string_view text) {
		std::vector<uint8_t> data;
		for (const auto character : text) {
			data.push_back(uint8_t(character));
			data.push_back(uint8_t(character >> 8));
		}
		data.push_back(0);
		data.push_back(0);
		return data;
	};
	const auto text = [&](const char16_t *path, const char16_t *name, std::u16string_view value) {
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (create(0x80000002ULL, path, key, disposition))
			return;
		fill(key, name, 1, bytes_of(value));
		close(key);
	};
	const auto number = [&](const char16_t *path, const char16_t *name, uint32_t value) {
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (create(0x80000002ULL, path, key, disposition))
			return;
		const uint8_t data[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
		fill(key, name, 4, data);
		close(key);
	};
	// The 64-bit view of the machine hive, and the 32-bit view that 32-bit programs reach through Wow6432Node.
	for (const bool wow : {false, true}) {
		const std::u16string root = wow ? u"SOFTWARE\\Wow6432Node" : u"SOFTWARE";
		const auto files = root + u"\\Microsoft\\Windows\\CurrentVersion";
		const auto system = root + u"\\Microsoft\\Windows NT\\CurrentVersion";
		const std::u16string program = wow ? u"C:\\Program Files (x86)" : u"C:\\Program Files";
		text(files.c_str(), u"ProgramFilesDir", program);
		text(files.c_str(), u"CommonFilesDir", program + u"\\Common Files");
		text(files.c_str(), u"ProgramFilesDir (x86)", u"C:\\Program Files (x86)");
		text(files.c_str(), u"CommonFilesDir (x86)", u"C:\\Program Files (x86)\\Common Files");
		text(files.c_str(), u"ProgramW6432Dir", u"C:\\Program Files");
		text(files.c_str(), u"CommonW6432Dir", u"C:\\Program Files\\Common Files");
		text(files.c_str(), u"ProgramFilesPath", u"%ProgramFiles%");
		text(files.c_str(), u"DevicePath", u"%SystemRoot%\\inf");
		text(system.c_str(), u"ProductName", u"Windows 10 Pro");
		text(system.c_str(), u"EditionID", u"Professional");
		text(system.c_str(), u"InstallationType", u"Client");
		text(system.c_str(), u"CurrentVersion", u"6.3");
		text(system.c_str(), u"CurrentBuild", u"19045");
		text(system.c_str(), u"CurrentBuildNumber", u"19045");
		text(system.c_str(), u"DisplayVersion", u"22H2");
		text(system.c_str(), u"ReleaseId", u"2009");
		text(system.c_str(), u"SystemRoot", u"C:\\Windows");
		text(system.c_str(), u"RegisteredOwner", u"User");
		text(system.c_str(), u"RegisteredOrganization", u"");
		number(system.c_str(), u"CurrentMajorVersionNumber", 10);
		number(system.c_str(), u"CurrentMinorVersionNumber", 0);
		number(system.c_str(), u"UBR", 2364);
		// Where the .NET Framework's versions live, which the system's mscoree finds the installed runtime under: each registry view names
		// the Framework of its width.
		text((root + u"\\Microsoft\\.NETFramework").c_str(), u"InstallRoot", wow ? u"C:\\Windows\\Microsoft.NET\\Framework\\" :
			u"C:\\Windows\\Microsoft.NET\\Framework64\\");
		// DirectX 9.0c, which Windows 10 reports as its DirectX version and the DirectX redistributable checks before it installs.
		const auto directx = root + u"\\Microsoft\\DirectX";
		text(directx.c_str(), u"Version", u"4.09.00.0904");
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (!create(0x80000002ULL, directx, key, disposition)) {
			const uint8_t installed[8] = {0, 0, 0, 9, 0, 0, 0, 0};
			fill(key, u"InstalledVersion", 3, installed);
			close(key);
		}
	}
	// The services every Windows installation has: the Windows Installer, started on demand, which installers stop and start.
	{
		const auto service = u"SYSTEM\\CurrentControlSet\\Services\\msiserver";
		text(service, u"DisplayName", u"Windows Installer");
		text(service, u"Description", u"Adds, modifies, and removes applications provided as a Windows Installer (*.msi, *.msp) package.");
		text(service, u"ObjectName", u"LocalSystem");
		number(service, u"Type", 0x10);
		number(service, u"Start", 3);
		number(service, u"ErrorControl", 1);
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (!create(0x80000002ULL, service, key, disposition)) {
			fill(key, u"ImagePath", 2, bytes_of(u"%systemroot%\\system32\\msiexec.exe /V"));
			close(key);
		}
	}
	seeding = false;
	if (added)
		save();
}

bool NativeRegistry::resolve(uint64_t parent, std::u16string_view path, std::u16string &folded, std::u16string &display) const
{
	std::u16string base;
	std::u16string shown;
	if (predefined(parent)) {
		static const char16_t *const names[] = {u"HKEY_CLASSES_ROOT", u"HKEY_CURRENT_USER", u"HKEY_LOCAL_MACHINE", u"HKEY_USERS"};
		shown = names[uint32_t(parent) - 0x80000000U];
		base = fold(shown);
	} else {
		const auto found = open_keys.find(parent);
		if (found == open_keys.end())
			return false;
		base = found->second;
		shown = keys.at(base).path;
	}
	std::u16string tail(path);
	while (!tail.empty() && tail.front() == u'\\')
		tail.erase(0, 1);
	while (!tail.empty() && tail.back() == u'\\')
		tail.pop_back();
	display = shown;
	folded = base;
	if (!tail.empty()) {
		display += u'\\';
		display += tail;
		folded += u'\\';
		folded += fold(tail);
	}
	// The same keys under their other names: the classes of the machine and of the user are the classes root, and the user's own
	// hive under HKEY_USERS is the current user.
	static const std::pair<std::u16string_view, std::u16string_view> aliases[] = {
		{u"hkey_local_machine\\software\\classes", u"HKEY_CLASSES_ROOT"},
		{u"hkey_current_user\\software\\classes", u"HKEY_CLASSES_ROOT"},
		{u"hkey_users\\s-1-5-21-1000-1000-1000-1000_classes", u"HKEY_CLASSES_ROOT"},
		{u"hkey_users\\s-1-5-21-1000-1000-1000-1000", u"HKEY_CURRENT_USER"}};
	for (const auto &[alias, target] : aliases)
		if (folded.starts_with(alias) && (folded.size() == alias.size() || folded[alias.size()] == u'\\')) {
			folded = fold(std::u16string(target)) + folded.substr(alias.size());
			display = std::u16string(target) + display.substr(alias.size());
			break;
		}
	return true;
}

uint32_t NativeRegistry::create(uint64_t parent, std::u16string_view path, uint64_t &key, uint32_t &disposition)
{
	refresh();
	std::u16string folded, display;
	if (!resolve(parent, path, folded, display))
		return bad_handle;
	if (display.find(u"\\\\") != display.npos)
		return invalid_parameter;
	if (open_keys.size() >= 10000)
		return 8;
	disposition = 2;
	// Every ancestor exists once a key does, so enumeration of parents stays consistent.
	std::string records;
	size_t end = 0;
	while (end != folded.npos) {
		end = folded.find(u'\\', end + 1);
		const auto partial = folded.substr(0, end);
		if (!keys.contains(partial)) {
			keys.emplace(partial, Key{display.substr(0, end), {}});
			records += "K\t" + escape(display.substr(0, end)) + '\n';
			if (partial == folded)
				disposition = 1;
		}
	}
	key = new_key_handle();
	if (!key)
		return 8;
	open_keys.emplace(key, folded);
	if (!records.empty())
		journal(records);
	return 0;
}

uint32_t NativeRegistry::open(uint64_t parent, std::u16string_view path, uint64_t &key)
{
	refresh();
	std::u16string folded, display;
	if (!resolve(parent, path, folded, display))
		return bad_handle;
	if (!keys.contains(folded))
		return file_not_found;
	if (open_keys.size() >= 10000)
		return 8;
	key = new_key_handle();
	if (!key)
		return 8;
	open_keys.emplace(key, folded);
	return 0;
}

uint32_t NativeRegistry::close(uint64_t key)
{
	if (predefined(key))
		return 0;
	return open_keys.erase(key) ? 0 : bad_handle;
}

uint32_t NativeRegistry::query(uint64_t key, std::u16string_view name, uint32_t &type, std::vector<uint8_t> &data) const
{
	refresh();
	const auto found = open_keys.find(key);
	if (found == open_keys.end())
		return predefined(key) ? file_not_found : bad_handle;
	const auto &values = keys.at(found->second).values;
	const auto value = values.find(fold(name));
	if (value == values.end())
		return file_not_found;
	type = value->second.type;
	data = value->second.data;
	return 0;
}

uint32_t NativeRegistry::set(uint64_t key, std::u16string_view name, uint32_t type, std::span<const uint8_t> data)
{
	refresh();
	const auto found = open_keys.find(key);
	if (found == open_keys.end())
		return predefined(key) ? 5 : bad_handle;
	if (!keys.contains(found->second))
		return key_deleted;
	if (data.size() > 1024 * 1024)
		return 234;
	auto &target = keys.at(found->second);
	target.values[fold(name)] = Value{std::u16string(name), type, std::vector<uint8_t>(data.begin(), data.end())};
	journal("V\t" + escape(target.path) + '\t' + escape(std::u16string(name)) + '\t' + std::to_string(type) + '\t' +
		hex(std::vector<uint8_t>(data.begin(), data.end())) + '\n');
	return 0;
}

std::vector<std::u16string> NativeRegistry::children(const std::u16string &folded) const
{
	std::vector<std::u16string> result;
	const auto prefix = folded + u'\\';
	for (auto found = keys.lower_bound(prefix); found != keys.end() && found->first.starts_with(prefix); ++found)
		if (found->first.find(u'\\', prefix.size()) == found->first.npos)
			result.push_back(found->first);
	return result;
}

uint32_t NativeRegistry::remove_key(uint64_t parent, std::u16string_view path)
{
	refresh();
	std::u16string folded, display;
	if (!resolve(parent, path, folded, display))
		return bad_handle;
	if (path.empty())
		return 5;
	if (!keys.contains(folded))
		return file_not_found;
	// A key with subkeys cannot be deleted; its values go with it.
	if (!children(folded).empty())
		return 5;
	const auto shown = keys.at(folded).path;
	keys.erase(folded);
	for (auto &[handle, target] : open_keys)
		if (target == folded)
			target.clear();
	journal("X\t" + escape(shown) + "\t1\n");
	return 0;
}

uint32_t NativeRegistry::remove_tree(uint64_t parent, std::u16string_view path)
{
	refresh();
	std::u16string folded, display;
	if (!resolve(parent, path, folded, display))
		return bad_handle;
	const auto found = keys.find(folded);
	if (found == keys.end())
		return file_not_found;
	const auto prefix = folded + u'\\';
	const auto shown = found->second.path;
	keys.erase(keys.lower_bound(prefix), std::find_if(keys.lower_bound(prefix), keys.end(), [&](const auto &item) { return !item.first.starts_with(prefix); }));
	if (path.empty())
		keys[folded].values.clear();
	else
		keys.erase(folded);
	for (auto &[handle, target] : open_keys)
		if ((target == folded && !path.empty()) || target.starts_with(prefix))
			target.clear();
	journal("X\t" + escape(shown) + (path.empty() ? "\t0\n" : "\t1\n"));
	return 0;
}

uint32_t NativeRegistry::remove_value(uint64_t key, std::u16string_view name)
{
	refresh();
	const auto found = open_keys.find(key);
	if (found == open_keys.end())
		return predefined(key) ? 5 : bad_handle;
	const auto entry = keys.find(found->second);
	if (entry == keys.end())
		return key_deleted;
	if (!entry->second.values.erase(fold(name)))
		return file_not_found;
	journal("Y\t" + escape(entry->second.path) + '\t' + escape(std::u16string(name)) + '\n');
	return 0;
}

uint32_t NativeRegistry::enum_key(uint64_t key, uint32_t index, std::u16string &name) const
{
	refresh();
	std::u16string folded;
	if (predefined(key)) {
		static const char16_t *const names[] = {u"HKEY_CLASSES_ROOT", u"HKEY_CURRENT_USER", u"HKEY_LOCAL_MACHINE", u"HKEY_USERS"};
		folded = fold(names[uint32_t(key) - 0x80000000U]);
	} else {
		const auto found = open_keys.find(key);
		if (found == open_keys.end())
			return bad_handle;
		folded = found->second;
	}
	if (!keys.contains(folded) && !predefined(key))
		return key_deleted;
	const auto subkeys = children(folded);
	if (index >= subkeys.size())
		return 259;
	const auto &display = keys.at(subkeys[index]).path;
	name = display.substr(display.find_last_of(u'\\') + 1);
	return 0;
}

uint32_t NativeRegistry::enum_value(uint64_t key, uint32_t index, std::u16string &name, uint32_t &type, std::vector<uint8_t> &data) const
{
	refresh();
	const auto found = open_keys.find(key);
	if (found == open_keys.end())
		return predefined(key) ? 259 : bad_handle;
	const auto entry = keys.find(found->second);
	if (entry == keys.end())
		return key_deleted;
	if (index >= entry->second.values.size())
		return 259;
	auto value = entry->second.values.begin();
	std::advance(value, index);
	name = value->second.name;
	type = value->second.type;
	data = value->second.data;
	return 0;
}

uint32_t NativeRegistry::info(uint64_t key, Info &result) const
{
	refresh();
	std::u16string folded, shown;
	if (predefined(key)) {
		static const char16_t *const names[] = {u"HKEY_CLASSES_ROOT", u"HKEY_CURRENT_USER", u"HKEY_LOCAL_MACHINE", u"HKEY_USERS"};
		shown = names[uint32_t(key) - 0x80000000U];
		folded = fold(shown);
	} else {
		const auto found = open_keys.find(key);
		if (found == open_keys.end())
			return bad_handle;
		folded = found->second;
	}
	result = {};
	const auto own = keys.find(folded);
	result.path = own != keys.end() ? own->second.path : shown.empty() ? folded : shown;
	for (const auto &child : children(folded)) {
		++result.subkeys;
		const auto &display = keys.at(child).path;
		result.longest_subkey = std::max<uint32_t>(result.longest_subkey, uint32_t(display.size() - display.find_last_of(u'\\') - 1));
	}
	if (own != keys.end())
		for (const auto &[unused, value] : own->second.values) {
			++result.values;
			result.longest_name = std::max<uint32_t>(result.longest_name, uint32_t(value.name.size()));
			result.longest_data = std::max<uint32_t>(result.longest_data, uint32_t(value.data.size()));
		}
	return 0;
}

NativeRegistry::Stamp NativeRegistry::current_stamp() const
{
	struct stat status{};
	if (file.empty() || ::stat(file.c_str(), &status))
		return {};
	return {status.st_mtim.tv_sec, status.st_mtim.tv_nsec, status.st_size, static_cast<long long>(status.st_ino)};
}

// Several processes of a prefix share the hive file: a change made by another one is read before this one looks at or changes the hive.
void NativeRegistry::refresh() const
{
	if (file.empty() || seeding)
		return;
	const auto now = current_stamp();
	if (now == stamp)
		return;
	auto &self = const_cast<NativeRegistry &>(*this);
	// Records appended to the same file are read from where reading stopped; a file written again whole is read from its start.
	if (now.inode == stamp.inode && now.size >= loaded && loaded > 0)
		self.replay(loaded);
	else {
		self.keys.clear();
		self.load();
	}
	stamp = now;
}

void NativeRegistry::save() const
{
	if (file.empty() || seeding)
		return;
	auto temporary = file;
	temporary += "." + std::to_string(::getpid()) + ".tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("cannot write the registry hive");
		for (const auto &[folded, key] : keys) {
			output << "K\t" << escape(key.path) << '\n';
			for (const auto &[value_folded, value] : key.values)
				output << "V\t" << escape(key.path) << '\t' << escape(value.name) << '\t' << value.type << '\t' << hex(value.data) << '\n';
		}
		output.flush();
		if (!output)
			throw std::runtime_error("cannot write the registry hive");
	}
	std::filesystem::rename(temporary, file);
	stamp = current_stamp();
	loaded = compacted = stamp.size;
}

void NativeRegistry::journal(const std::string &records)
{
	if (file.empty() || seeding)
		return;
	// A process that compacts the hive renames a new file over it, so the file locked must still be the one the path names.
	int descriptor = -1;
	struct stat opened{};
	for (;;) {
		descriptor = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
		if (descriptor < 0)
			throw std::runtime_error("cannot write the registry hive");
		::flock(descriptor, LOCK_EX);
		struct stat named{};
		if (!::fstat(descriptor, &opened) && !::stat(file.c_str(), &named) && opened.st_ino == named.st_ino)
			break;
		::flock(descriptor, LOCK_UN);
		::close(descriptor);
	}
	// A hive compacted since it was last read holds every change in a new order, so it is read again from its start.
	if (stamp.inode && static_cast<long long>(opened.st_ino) != stamp.inode) {
		keys.clear();
		loaded = 0;
	}
	// What the other processes appended comes first in the file and this change after it: reading their records and then these again
	// leaves the keys as every process that reads the file sees them.
	size_t written = 0;
	while (written < records.size()) {
		const auto done = ::write(descriptor, records.data() + written, records.size() - written);
		if (done <= 0)
			break;
		written += size_t(done);
	}
	replay(loaded);
	stamp = current_stamp();
	if (written == records.size() && stamp.size > 4 * 1024 * 1024 && stamp.size > 3 * compacted)
		save();
	::flock(descriptor, LOCK_UN);
	::close(descriptor);
	if (written != records.size())
		throw std::runtime_error("cannot write the registry hive");
}

void NativeRegistry::load()
{
	if (file.empty() || !std::filesystem::exists(file))
		return;
	loaded = compacted = replay(0);
}

long long NativeRegistry::replay(long long from)
{
	std::ifstream input(file, std::ios::binary);
	input.seekg(from);
	std::string line;
	long long position = from;
	// Only whole lines count: a record another process is still writing is read the next time.
	while (std::getline(input, line) && !input.eof()) {
		position += (long long)line.size() + 1;
		apply(line);
	}
	loaded = position;
	return position;
}

void NativeRegistry::apply(const std::string &line)
{
	std::vector<std::string> fields;
	size_t start = 0;
	for (size_t tab; (tab = line.find('\t', start)) != line.npos; start = tab + 1)
		fields.push_back(line.substr(start, tab - start));
	fields.push_back(line.substr(start));
	if (fields[0] == "K" && fields.size() == 2) {
		const auto path = decode_utf8(unescape(fields[1]));
		keys.emplace(fold(path), Key{path, {}});
	} else if (fields[0] == "V" && fields.size() == 5) {
		const auto path = decode_utf8(unescape(fields[1]));
		const auto name = decode_utf8(unescape(fields[2]));
		const auto found = keys.find(fold(path));
		// A value set by a process that had not yet read another one's deletion of the key is lost with the key, as on Windows.
		if (found != keys.end())
			found->second.values[fold(name)] = Value{name, uint32_t(std::stoul(fields[3])), unhex(fields[4])};
	} else if (fields[0] == "Y" && fields.size() == 3) {
		// A value deleted.
		if (const auto found = keys.find(fold(decode_utf8(unescape(fields[1])))); found != keys.end())
			found->second.values.erase(fold(decode_utf8(unescape(fields[2]))));
	} else if (fields[0] == "X" && fields.size() == 3) {
		// A key deleted with everything below it, or emptied ("0") when it stays.
		const auto folded = fold(decode_utf8(unescape(fields[1])));
		const auto prefix = folded + u'\\';
		keys.erase(keys.lower_bound(prefix), std::find_if(keys.lower_bound(prefix), keys.end(), [&](const auto &item) { return !item.first.starts_with(prefix); }));
		if (fields[2] == "0") {
			if (const auto found = keys.find(folded); found != keys.end())
				found->second.values.clear();
		} else
			keys.erase(folded);
	} else if (!line.empty()) {
		throw std::runtime_error("registry hive contains an unknown record");
	}
}

std::vector<std::u16string> virtual_package_roots(NativeRegistry &registry)
{
	std::vector<std::u16string> roots;
	uint64_t key = 0;
	if (registry.open(0x80000002ULL, u"Software\\Microsoft\\AppVISV", key))
		return roots;
	std::u16string name;
	uint32_t type = 0;
	std::vector<uint8_t> data;
	for (uint32_t index = 0; !registry.enum_value(key, index, name, type, data); ++index)
		if (name.size() > 2 && name[1] == u':')
			roots.push_back(name);
	registry.close(key);
	return roots;
}

}
