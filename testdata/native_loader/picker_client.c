#define COBJMACROS
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

/* A host file the person picks in the file dialog. Without arguments the program asks to open a file: it may then read the file but not
   change it. With --save it asks where to save: it may then write the file. With --events it asks to open a file through the dialog
   object and learns the choice only in the handler's OnFileOk, as Qt does. The exit code says which part went wrong. */

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

int raven_entry(void)
{
	const BOOL save = option(GetCommandLineW(), L"--save");
	WCHAR path[MAX_PATH] = {0};
	OPENFILENAMEW request = {sizeof request};
	request.lpstrFile = path;
	request.nMaxFile = MAX_PATH;
	request.lpstrFilter = L"Text\0*.txt\0All files\0*.*\0";
	request.Flags = OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
	if (option(GetCommandLineW(), L"--events")) {
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
