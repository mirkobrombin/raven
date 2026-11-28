#define COBJMACROS
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <roapi.h>
#include <winstring.h>

/* A host file the person picks in the file dialog. Without arguments the program asks to open a file: it may then read the file but not
   change it. With --save it asks where to save: it may then write the file. With --events it asks to open a file through the dialog
   object and learns the choice only in the handler's OnFileOk, as Qt does. With --winrt, --winrt-save and --winrt-folder it asks the same
   through FileOpenPicker and FileSavePicker of the Windows Runtime, and takes the Documents folder FolderPicker starts in, which it may
   write into. The exit code says which part went wrong. */

static BOOL option(const WCHAR *line, const WCHAR *name)
{
	for (; *line; ++line) {
		unsigned index = 0;
		while (name[index] && line[index] == name[index])
			++index;
		if (!name[index])
			return TRUE;
	}
	return FALSE;
}

static WCHAR chosen[MAX_PATH];
static LONG file_ok;

static BOOL same_iid(REFIID left, REFIID right)
{
	const BYTE *a = (const BYTE *)left, *b = (const BYTE *)right;
	unsigned index;
	for (index = 0; index < sizeof(GUID); ++index)
		if (a[index] != b[index])
			return FALSE;
	return TRUE;
}

static HRESULT STDMETHODCALLTYPE events_query(IFileDialogEvents *self, REFIID iid, void **out)
{
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IFileDialogEvents)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE events_add_ref(IFileDialogEvents *self) { return 2; }
static ULONG STDMETHODCALLTYPE events_release(IFileDialogEvents *self) { return 1; }

/* The results are read here and only here: after the dialog closes a program may not ask for them again. */
static HRESULT STDMETHODCALLTYPE events_file_ok(IFileDialogEvents *self, IFileDialog *dialog)
{
	IFileOpenDialog *open = NULL;
	IShellItemArray *items = NULL;
	IShellItem *item = NULL;
	LPWSTR text = NULL;
	++file_ok;
	if (SUCCEEDED(IFileDialog_QueryInterface(dialog, &IID_IFileOpenDialog, (void **)&open)) &&
		SUCCEEDED(IFileOpenDialog_GetResults(open, &items)) && SUCCEEDED(IShellItemArray_GetItemAt(items, 0, &item)) &&
		SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &text))) {
		lstrcpynW(chosen, text, MAX_PATH);
		CoTaskMemFree(text);
	}
	if (item)
		IShellItem_Release(item);
	if (items)
		IShellItemArray_Release(items);
	if (open)
		IFileOpenDialog_Release(open);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE events_ignored(IFileDialogEvents *self, IFileDialog *dialog) { return S_OK; }
static HRESULT STDMETHODCALLTYPE events_folder_changing(IFileDialogEvents *self, IFileDialog *dialog, IShellItem *folder) { return S_OK; }

static HRESULT STDMETHODCALLTYPE events_share(IFileDialogEvents *self, IFileDialog *dialog, IShellItem *item, FDE_SHAREVIOLATION_RESPONSE *answer)
{
	*answer = FDESVR_DEFAULT;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE events_overwrite(IFileDialogEvents *self, IFileDialog *dialog, IShellItem *item, FDE_OVERWRITE_RESPONSE *answer)
{
	*answer = FDEOR_DEFAULT;
	return S_OK;
}

static IFileDialogEventsVtbl events_vtbl = {
	events_query, events_add_ref, events_release, events_file_ok, events_folder_changing, events_ignored, events_ignored, events_share,
	events_ignored, events_overwrite
};

static int choose_with_events(WCHAR *path)
{
	IFileDialogEvents handler = {&events_vtbl};
	IFileOpenDialog *dialog = NULL;
	DWORD cookie = 0;
	HRESULT shown;
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog)))
		return 12;
	IFileOpenDialog_SetTitle(dialog, L"Open");
	IFileOpenDialog_Advise(dialog, &handler, &cookie);
	shown = IFileOpenDialog_Show(dialog, NULL);
	IFileOpenDialog_Unadvise(dialog, cookie);
	IFileOpenDialog_Release(dialog);
	if (FAILED(shown))
		return 10;
	if (file_ok != 1 || !chosen[0])
		return 13;
	lstrcpynW(path, chosen, MAX_PATH);
	return 0;
}

/* The Windows Runtime pickers, called through their tables: an object's method at a place of its table. */
typedef struct object object;
struct object {
	void **table;
};
#define CALL(self, place, type) ((type)(self)->table[place])

static const GUID iid_open_picker = {0x2ca8278a, 0x12c5, 0x4c5f, {0x89, 0x77, 0x94, 0x54, 0x77, 0x93, 0xc2, 0x41}};
static const GUID iid_save_picker = {0x3286ffcb, 0x617f, 0x4cc5, {0xaf, 0x6a, 0xb3, 0xfd, 0xf2, 0x9a, 0xd1, 0x45}};
static const GUID iid_folder_picker = {0x084f7799, 0xf3fb, 0x400a, {0x99, 0xb1, 0x7b, 0x4a, 0x77, 0x2f, 0xd6, 0x0d}};
static const GUID iid_async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID iid_storage_item = {0x4207a996, 0xca2f, 0x42f7, {0xbd, 0xe8, 0x8b, 0x10, 0x45, 0x7a, 0x7f, 0x30}};

typedef HRESULT (WINAPI *query_method)(object *, const GUID *, object **);
typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *string_method)(object *, HSTRING);
typedef HRESULT (WINAPI *status_method)(object *, int *);
typedef HRESULT (WINAPI *path_method)(object *, HSTRING *);
typedef HRESULT (WINAPI *insert_method)(object *, HSTRING, object *, boolean *);
typedef HRESULT (WINAPI *number_method)(object *, int);

static object *made(const WCHAR *name, const GUID *iid)
{
	HSTRING text = NULL;
	object *instance = NULL, *face = NULL;
	if (SUCCEEDED(WindowsCreateString(name, lstrlenW(name), &text)) && SUCCEEDED(RoActivateInstance(text, (IInspectable **)&instance)))
		CALL(instance, 0, query_method)(instance, iid, &face);
	if (instance)
		CALL(instance, 2, release_method)(instance);
	WindowsDeleteString(text);
	return face;
}

/* Appends text to an IVector<String>. */
static BOOL append(object *vector, const WCHAR *text)
{
	HSTRING item = NULL;
	const BOOL done = SUCCEEDED(WindowsCreateString(text, lstrlenW(text), &item)) && CALL(vector, 13, string_method)(vector, item) == S_OK;
	WindowsDeleteString(item);
	return done;
}

/* Waits for a pick and puts the path of what was picked. */
static int picked(object *operation, WCHAR *path)
{
	object *info = NULL, *item = NULL, *storage = NULL;
	HSTRING text = NULL;
	int status = 0, failed = 0;
	if (!operation || CALL(operation, 0, query_method)(operation, &iid_async_info, &info) != S_OK)
		return 40;
	while (CALL(info, 7, status_method)(info, &status) == S_OK && status == 0)
		Sleep(50);
	CALL(info, 2, release_method)(info);
	if (status != 1 || CALL(operation, 8, object_method)(operation, &item) != S_OK || !item)
		failed = 10;
	else if (CALL(item, 0, query_method)(item, &iid_storage_item, &storage) != S_OK || CALL(storage, 12, path_method)(storage, &text) != S_OK)
		failed = 41;
	else
		lstrcpynW(path, WindowsGetStringRawBuffer(text, NULL), MAX_PATH);
	WindowsDeleteString(text);
	if (storage)
		CALL(storage, 2, release_method)(storage);
	if (item)
		CALL(item, 2, release_method)(item);
	CALL(operation, 2, release_method)(operation);
	return failed;
}

/* A text file to open, a text file to save as the choice "Text", or the Documents folder the folder picker starts in. */
static int pick_winrt(BOOL save, BOOL folder, WCHAR *path)
{
	object *picker = NULL, *types = NULL, *operation = NULL, *choices = NULL;
	boolean replaced = FALSE;
	HSTRING text = NULL;
	if (FAILED(RoInitialize(RO_INIT_SINGLETHREADED)))
		return 42;
	object *open = made(L"Windows.Storage.Pickers.FileOpenPicker", &iid_open_picker);
	if (!open || CALL(open, 14, object_method)(open, &types) != S_OK || !types || !append(types, folder ? L"*" : L".txt"))
		return 43;
	if (save) {
		if (!(picker = made(L"Windows.Storage.Pickers.FileSavePicker", &iid_save_picker)) || CALL(picker, 12, object_method)(picker, &choices) != S_OK)
			return 44;
		if (FAILED(WindowsCreateString(L"Text", 4, &text)) || CALL(choices, 10, insert_method)(choices, text, types, &replaced) != S_OK)
			return 45;
		WindowsDeleteString(text);
		CALL(choices, 2, release_method)(choices);
		CALL(picker, 19, object_method)(picker, &operation);
	} else if (folder) {
		if (!(picker = made(L"Windows.Storage.Pickers.FolderPicker", &iid_folder_picker)))
			return 46;
		object *filter = NULL;
		if (CALL(picker, 14, object_method)(picker, &filter) != S_OK || !append(filter, L"*"))
			return 47;
		if (CALL(picker, 11, number_method)(picker, 0) != S_OK)
			return 48;
		CALL(filter, 2, release_method)(filter);
		CALL(picker, 15, object_method)(picker, &operation);
	} else
		CALL(open, 15, object_method)(open, &operation);
	const int failed = picked(operation, path);
	CALL(types, 2, release_method)(types);
	CALL(open, 2, release_method)(open);
	if (picker)
		CALL(picker, 2, release_method)(picker);
	return failed;
}

int raven_entry(void)
{
	const BOOL save = option(GetCommandLineW(), L"--save") || option(GetCommandLineW(), L"--winrt-save");
	WCHAR path[MAX_PATH] = {0};
	OPENFILENAMEW request = {sizeof request};
	request.lpstrFile = path;
	request.nMaxFile = MAX_PATH;
	request.lpstrFilter = L"Text\0*.txt\0All files\0*.*\0";
	request.Flags = OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
	const BOOL winrt = option(GetCommandLineW(), L"--winrt");
	const BOOL folder = option(GetCommandLineW(), L"--winrt-folder");
	if (winrt) {
		const int failed = pick_winrt(option(GetCommandLineW(), L"--winrt-save"), folder, path);
		if (failed)
			return failed;
		const int length = lstrlenW(path);
		if (folder ? length < 10 || lstrcmpiW(path + length - 10, L"\\Documents") : path[0] == L'C' || path[0] == L'c')
			return 11;
		if (folder) {
			/* A folder picked is the program's to write into. */
			WCHAR inside[MAX_PATH + 16];
			lstrcpyW(inside, path);
			lstrcatW(inside, L"\\inside.txt");
			HANDLE file = CreateFileW(inside, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (file == INVALID_HANDLE_VALUE)
				return 50;
			DWORD written = 0;
			const BOOL done = WriteFile(file, "inside", 6, &written, NULL) && written == 6;
			CloseHandle(file);
			return done ? 0 : 51;
		}
	} else if (option(GetCommandLineW(), L"--events")) {
		const int failed = choose_with_events(path);
		if (failed)
			return failed;
	} else if (!(save ? GetSaveFileNameW(&request) : GetOpenFileNameW(&request))) {
		return 10;
	}
	if (path[0] == L'C' || path[0] == L'c')
		return 11;
	if (save) {
		HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			return 20;
		DWORD written = 0;
		const BOOL done = WriteFile(file, "saved", 5, &written, NULL) && written == 5;
		CloseHandle(file);
		return done ? 0 : 21;
	}
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 30;
	char buffer[16];
	DWORD read = 0;
	const BOOL done = ReadFile(file, buffer, sizeof buffer, &read, NULL) && read == 6 && buffer[0] == 'p';
	CloseHandle(file);
	if (!done)
		return 31;
	/* Opening a file grants reading it, not changing it. */
	file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
		return 32;
	}
	return GetLastError() == ERROR_ACCESS_DENIED ? 0 : 33;
}
