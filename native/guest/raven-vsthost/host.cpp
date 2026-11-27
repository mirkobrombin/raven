// raven-vsthost.exe: the Windows side of a VST 3 plugin that a Linux host loads through Raven. It loads the plugin's module, a single file
// or the DLL inside a bundle (NAME.vst3/Contents/x86_64-win/NAME.vst3), as a Windows host does, and calls the plugin's own code natively.
//
//   raven-vsthost.exe --list PLUGIN    one line for each class of the plugin's factory: its id, as the four words of a FUID in hex, its
//                                      category, its subcategories (empty without IPluginFactory2), and its name.
//
// The exit status is 0 on success, 1 for a module that does not load or has no factory, 2 for a wrong command line.

#include "pluginterfaces/base/ipluginbase.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace Steinberg;

namespace {

// The module a path names, in full, as LoadLibraryEx searches the module's folder for its own imports only for a full path: the path itself
// for a file, the DLL of its own name inside it for a bundle folder.
std::wstring module_path(const std::wstring &given)
{
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		const DWORD length = GetFullPathNameW(given.c_str(), DWORD(path.size()), path.data(), nullptr);
		if (!length)
			return given;
		if (length < path.size()) {
			path.resize(length);
			break;
		}
		path.resize(length);
	}
	const DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
		return path;
	std::wstring folder = path;
	while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/'))
		folder.pop_back();
	const size_t slash = folder.find_last_of(L"\\/");
	return folder + L"\\Contents\\x86_64-win\\" + folder.substr(slash == std::wstring::npos ? 0 : slash + 1);
}

class Module {
public:
	explicit Module(const std::wstring &path) : handle(LoadLibraryExW(module_path(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
	{
		if (!handle)
			return;
		// A plugin may set itself up in InitDll and refuse to load from there; GetPluginFactory is all a module must have.
		using Init = bool (*)();
		if (const auto init = reinterpret_cast<Init>(GetProcAddress(handle, "InitDll")); init && !init())
			return;
		initialized = true;
		using Get = IPluginFactory *(PLUGIN_API *)();
		if (const auto get = reinterpret_cast<Get>(GetProcAddress(handle, "GetPluginFactory")))
			factory = get();
	}
	~Module()
	{
		if (factory)
			factory->release();
		using Exit = bool (*)();
		if (initialized)
			if (const auto exit = reinterpret_cast<Exit>(GetProcAddress(handle, "ExitDll")))
				exit();
		if (handle)
			FreeLibrary(handle);
	}
	Module(const Module &) = delete;
	Module &operator=(const Module &) = delete;

	HMODULE handle;
	bool initialized = false;
	IPluginFactory *factory = nullptr;
};

void print_id(const TUID id)
{
	uint32 words[4];
	FUID::fromTUID(id).to4Int(words[0], words[1], words[2], words[3]);
	std::printf("%08X%08X%08X%08X", words[0], words[1], words[2], words[3]);
}

int list(IPluginFactory &factory)
{
	IPluginFactory2 *second = nullptr;
	if (factory.queryInterface(IPluginFactory2::iid, reinterpret_cast<void **>(&second)) != kResultOk)
		second = nullptr;
	const int32 count = factory.countClasses();
	for (int32 index = 0; index < count; ++index) {
		PClassInfo info{};
		if (factory.getClassInfo(index, &info) != kResultOk)
			continue;
		PClassInfo2 more{};
		const bool described = second && second->getClassInfo2(index, &more) == kResultOk;
		print_id(info.cid);
		std::printf("\t%.*s\t%.*s\t%.*s\n", int(sizeof info.category), info.category, described ? int(sizeof more.subCategories) : 0,
			more.subCategories, int(sizeof info.name), info.name);
	}
	if (second)
		second->release();
	std::fflush(stdout);
	return 0;
}

}

int wmain(int count, wchar_t **arguments)
{
	if (count != 3 || std::wstring(arguments[1]) != L"--list") {
		std::fputs("usage: raven-vsthost.exe --list PLUGIN\n", stderr);
		return 2;
	}
	Module module(arguments[2]);
	if (!module.factory) {
		std::fprintf(stderr, "raven-vsthost: %ls has no plugin factory (error %lu)\n", arguments[2], GetLastError());
		return 1;
	}
	return list(*module.factory);
}
