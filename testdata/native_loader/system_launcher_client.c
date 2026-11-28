// Windows.System.Launcher as an app uses it: an address of a scheme and a file of a kind the system opens with a program, here this
// program itself registered for raventest: and .raventest, which then writes what it was given to C:\launched.txt. A file of executable
// code is not launched, and an address nothing opens gives way to the FallbackUri of the options. The exit code names the first check
// that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

typedef struct object object;
struct object {
	void **table;
};
#define CALL(self, place, type) ((type)(self)->table[place])

typedef HRESULT (WINAPI *query_method)(object *, const GUID *, object **);
typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *item_method)(object *, object *, object **);
typedef HRESULT (WINAPI *item_options_method)(object *, object *, object *, object **);
typedef HRESULT (WINAPI *name_method)(object *, HSTRING, object **);
typedef HRESULT (WINAPI *status_method)(object *, int *);
typedef HRESULT (WINAPI *boolean_method)(object *, boolean *);
typedef HRESULT (WINAPI *put_object_method)(object *, object *);

static const GUID launcher_statics = {0x277151c3, 0x9e3e, 0x42f6, {0x91, 0xa4, 0x5d, 0xfd, 0xeb, 0x23, 0x24, 0x51}};
static const GUID launcher_options = {0xbafa21d8, 0xb071, 0x4cd8, {0x85, 0x3e, 0x34, 0x12, 0x03, 0xe5, 0x57, 0xd3}};
static const GUID uri_factory = {0x44a9796f, 0x723e, 0x4fdf, {0xa2, 0x18, 0x03, 0x3e, 0x75, 0xb0, 0xc0, 0x84}};
static const GUID file_statics = {0x5984c710, 0xdaf2, 0x43c8, {0x8b, 0xb4, 0xa4, 0xd3, 0xea, 0xcf, 0xd0, 0x3f}};
static const GUID async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static const WCHAR marker[] = L"C:\\launched.txt";

static void release(object *self)
{
	if (self)
		CALL(self, 2, release_method)(self);
}

static HSTRING text_of(const WCHAR *text)
{
	HSTRING made = NULL;
	WindowsCreateString(text, lstrlenW(text), &made);
	return made;
}

static object *statics(const WCHAR *name, const GUID *iid)
{
	HSTRING text = text_of(name);
	object *out = NULL;
	RoGetActivationFactory(text, iid, (void **)&out);
	WindowsDeleteString(text);
	return out;
}

/* Waits for an operation and takes its object result, or its Boolean one when value is set. */
static HRESULT finish(object *operation, object **result, boolean *value)
{
	object *info = NULL;
	int status = 0;
	if (!operation || CALL(operation, 0, query_method)(operation, &async_info, &info) != S_OK)
		return E_FAIL;
	while (CALL(info, 7, status_method)(info, &status) == S_OK && status == 0)
		Sleep(20);
	release(info);
	const HRESULT done = status != 1 ? E_FAIL : value ? CALL(operation, 8, boolean_method)(operation, value) :
		CALL(operation, 8, object_method)(operation, result);
	release(operation);
	return done;
}

static BOOL put(const WCHAR *path, const WCHAR *name, const WCHAR *value)
{
	HKEY key;
	if (RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
		return FALSE;
	const BOOL done = !RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
	RegCloseKey(key);
	return done;
}

/* Waits until the launched program wrote the marker, and whether it holds text. */
static BOOL launched(const char *text)
{
	char buffer[MAX_PATH * 3];
	for (int tries = 0; tries < 500; ++tries) {
		HANDLE file = CreateFileW(marker, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (file != INVALID_HANDLE_VALUE) {
			DWORD read = 0;
			ReadFile(file, buffer, sizeof buffer - 1, &read, NULL);
			CloseHandle(file);
			buffer[read] = 0;
			for (DWORD at = 0; at < read; ++at) {
				int index = 0;
				while (text[index] && buffer[at + index] == text[index])
					++index;
				if (!text[index])
					return TRUE;
			}
			return FALSE;
		}
		Sleep(20);
	}
	return FALSE;
}

static BOOL write_file(const WCHAR *path, const char *text, DWORD length)
{
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	DWORD written = 0;
	const BOOL done = WriteFile(file, text, length, &written, NULL) && written == length;
	CloseHandle(file);
	return done;
}

/* Where --child starts in the command line, or NULL. */
static const WCHAR *child_option(const WCHAR *line)
{
	static const WCHAR option[] = L"--child ";
	for (; *line; ++line) {
		int index = 0;
		while (option[index] && line[index] == option[index])
			++index;
		if (!option[index])
			return line + index;
	}
	return NULL;
}

/* Run as the registered program: what follows --child is written, through a temporary file renamed in place. */
static int child(const WCHAR *argument)
{
	char text[MAX_PATH * 3];
	const int length = WideCharToMultiByte(CP_UTF8, 0, argument, -1, text, sizeof text, NULL, NULL);
	if (length <= 0 || !write_file(L"C:\\launched.tmp", text, (DWORD)length - 1))
		return 1;
	return MoveFileExW(L"C:\\launched.tmp", marker, MOVEFILE_REPLACE_EXISTING) ? 0 : 2;
}

static object *uri(object *uris, const WCHAR *address)
{
	HSTRING text = text_of(address);
	object *made = NULL;
	CALL(uris, 6, name_method)(uris, text, &made);
	WindowsDeleteString(text);
	return made;
}

static object *file_at(object *files, const WCHAR *path)
{
	HSTRING text = text_of(path);
	object *operation = NULL, *file = NULL;
	CALL(files, 6, name_method)(files, text, &operation);
	WindowsDeleteString(text);
	return finish(operation, &file, NULL) == S_OK ? file : NULL;
}

int raven_entry(void)
{
	const WCHAR *argument = child_option(GetCommandLineW());
	if (argument)
		return child(argument);
	WCHAR self[MAX_PATH], command[MAX_PATH + 32] = L"\"";
	if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return 3;
	lstrcatW(command, self);
	lstrcatW(command, L"\" --child %1");
	if (!put(L"raventest", L"URL Protocol", L"") || !put(L"raventest\\shell\\open\\command", L"", command) ||
		!put(L".raventest", L"", L"RavenTest.File") || !put(L"RavenTest.File\\shell\\open\\command", L"", command))
		return 4;
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 5;
	object *launcher = statics(L"Windows.System.Launcher", &launcher_statics);
	object *uris = statics(L"Windows.Foundation.Uri", &uri_factory);
	object *files = statics(L"Windows.Storage.StorageFile", &file_statics);
	object *operation = NULL, *address = NULL, *file = NULL;
	boolean opened = FALSE;
	if (!launcher || !uris || !files) return 6;

	/* An address of the registered scheme opens the program with it. */
	DeleteFileW(marker);
	if (!(address = uri(uris, L"raventest://hello"))) return 10;
	CALL(launcher, 8, item_method)(launcher, address, &operation);
	if (finish(operation, NULL, &opened) != S_OK || !opened) return 11;
	if (!launched("raventest://hello")) return 12;
	release(address);

	/* A file of the registered kind opens the program with its path. */
	CreateDirectoryW(L"C:\\launch", NULL);
	if (!write_file(L"C:\\launch\\doc.raventest", "doc", 3) || !write_file(L"C:\\launch\\tool.exe", "MZ", 2)) return 13;
	if (!(file = file_at(files, L"C:\\launch\\doc.raventest"))) return 14;
	DeleteFileW(marker);
	CALL(launcher, 6, item_method)(launcher, file, &operation);
	if (finish(operation, NULL, &opened) != S_OK || !opened) return 15;
	if (!launched("doc.raventest")) return 16;
	release(file);

	/* A file of executable code is not launched. */
	if (!(file = file_at(files, L"C:\\launch\\tool.exe"))) return 17;
	CALL(launcher, 6, item_method)(launcher, file, &operation);
	if (finish(operation, NULL, &opened) != S_OK || opened) return 18;
	release(file);

	/* An address nothing opens gives way to the fallback. */
	HSTRING options_name = text_of(L"Windows.System.LauncherOptions");
	object *instance = NULL, *options = NULL, *fallback = NULL;
	if (RoActivateInstance(options_name, (IInspectable **)&instance) != S_OK || !instance ||
		CALL(instance, 0, query_method)(instance, &launcher_options, &options) != S_OK)
		return 19;
	WindowsDeleteString(options_name);
	release(instance);
	if (!(fallback = uri(uris, L"raventest://fallback")) || CALL(options, 16, put_object_method)(options, fallback) != S_OK) return 20;
	if (!(address = uri(uris, L"ravennone://nothing"))) return 21;
	DeleteFileW(marker);
	CALL(launcher, 9, item_options_method)(launcher, address, options, &operation);
	if (finish(operation, NULL, &opened) != S_OK || !opened) return 22;
	if (!launched("raventest://fallback")) return 23;
	release(address);
	release(fallback);
	release(options);
	release(files);
	release(uris);
	release(launcher);
	return 0;
}
