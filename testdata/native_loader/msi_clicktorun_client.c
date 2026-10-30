#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <msi.h>

/* A Click-to-Run product the installer never registered, described only by the manifest in its package folder: the product of one of
   its components, and the files it publishes under a category, found both as the component lists them and as a component holds them. */

static const char manifest[] =
	"<?xml version=\"1.0\" encoding=\"utf-16\"?>\n"
	"<Package ProductCode=\"{90160000-0000-0000-1000-0000000FF1CE}\" Platform=\"x64\">\n"
	"\t<PublishComponentList>\n"
	"\t\t<PublishComponent PublishComponentId=\"{11111111-2222-3333-4444-555555555555}\" Qualifier=\"listed.ttf\" "
	"ComponentId=\"{AAAAAAAA-0000-0000-0000-000000000001}\" AppData=\"\" Feature=\"Files\" KeyFile=\"%SFT_PROGRAM_FILES_X64%\\Microsoft Office\\Office16\\listed.ttf\"></PublishComponent>\n"
	"\t</PublishComponentList>\n"
	"\t<ComponentList>\n"
	"\t\t<Component ComponentId=\"{AAAAAAAA-0000-0000-0000-000000000001}\" KeyPath=\"%SFT_PROGRAM_FILES_X64%\\Microsoft Office\\Office16\\listed.ttf\"></Component>\n"
	"\t\t<Component ComponentId=\"{AAAAAAAA-0000-0000-0000-000000000002}\" KeyPath=\"%SFT_PROGRAM_FILES_X64%\\Microsoft Office\\Office16\\held.xml\">\n"
	"\t\t\t<PublishComponent PublishComponentId=\"{11111111-2222-3333-4444-555555555555}\" Qualifier=\"held.xml\" AppData=\"\" Feature=\"Files\"></PublishComponent>\n"
	"\t\t</Component>\n"
	"\t</ComponentList>\n"
	"</Package>\n";

static void beside(WCHAR *target, const WCHAR *name)
{
	unsigned cut = GetModuleFileNameW(NULL, target, MAX_PATH);
	while (cut && target[cut - 1] != L'\\')
		--cut;
	for (unsigned i = 0; name[i]; ++i)
		target[cut++] = name[i];
	target[cut] = 0;
}

static BOOL ends_with(const WCHAR *text, const WCHAR *tail)
{
	int length = lstrlenW(text), size = lstrlenW(tail);
	return length >= size && !lstrcmpiW(text + length - size, tail);
}

static BOOL write_file(const WCHAR *path, const void *data, DWORD size)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD written = 0;
	BOOL done = file != INVALID_HANDLE_VALUE && WriteFile(file, data, size, &written, NULL) && written == size;
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	return done;
}

int raven_entry(void)
{
	static WCHAR text[sizeof(manifest)];
	WCHAR folder[MAX_PATH], path[MAX_PATH], found[MAX_PATH];
	DWORD length;
	HKEY key;
	int at;

	beside(folder, L"c2r");
	lstrcpyW(path, folder);
	CreateDirectoryW(path, NULL);
	lstrcatW(path, L"\\root");
	CreateDirectoryW(path, NULL);
	lstrcpyW(found, path);
	lstrcatW(path, L"\\Integration");
	CreateDirectoryW(path, NULL);
	lstrcatW(path, L"\\C2RManifest.Test.x-none.xml");
	text[0] = 0xfeff;
	for (at = 0; manifest[at]; ++at)
		text[at + 1] = (WCHAR)manifest[at];
	if (!write_file(path, text, (at + 1) * sizeof(WCHAR)))
		return 1;
	lstrcatW(found, L"\\Office16");
	CreateDirectoryW(found, NULL);
	lstrcpyW(path, found);
	lstrcatW(path, L"\\listed.ttf");
	if (!write_file(path, "x", 1))
		return 2;
	lstrcpyW(path, found);
	lstrcatW(path, L"\\held.xml");
	if (!write_file(path, "x", 1))
		return 3;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Office\\ClickToRun", 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, NULL))
		return 4;
	RegSetValueExW(key, L"PackageFolder", 0, REG_SZ, (const BYTE *)folder, (lstrlenW(folder) + 1) * sizeof(WCHAR));
	RegCloseKey(key);

	if (MsiGetProductCodeW(L"{AAAAAAAA-0000-0000-0000-000000000001}", found) || lstrcmpiW(found, L"{90160000-0000-0000-1000-0000000FF1CE}"))
		return 5;
	length = MAX_PATH;
	if (MsiProvideQualifiedComponentExW(L"{11111111-2222-3333-4444-555555555555}", L"listed.ttf", INSTALLMODE_DEFAULT, NULL, 0, 0, found, &length) ||
		!ends_with(found, L"\\root\\Office16\\listed.ttf"))
		return 6;
	length = MAX_PATH;
	if (MsiProvideQualifiedComponentExW(L"{11111111-2222-3333-4444-555555555555}", L"held.xml", INSTALLMODE_DEFAULT, NULL, 0, 0, found, &length) ||
		!ends_with(found, L"\\root\\Office16\\held.xml"))
		return 7;
	length = MAX_PATH;
	if (MsiProvideQualifiedComponentExW(L"{11111111-2222-3333-4444-555555555555}", L"absent.dll", INSTALLMODE_DEFAULT, NULL, 0, 0, found, &length) ==
		ERROR_SUCCESS)
		return 8;
	return 0;
}
