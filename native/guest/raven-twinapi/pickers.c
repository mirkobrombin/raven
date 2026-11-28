/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Storage.Pickers.FileOpenPicker, FileSavePicker and FolderPicker: the shell's file dialog, which Raven's runtime shows, asked
 * for one file, several, a place to save or a folder, on a thread of the operation's own.
 *
 * - The open and folder pickers show the extensions of FileTypeFilter, the save picker one choice per entry of FileTypeChoices, in the
 *   order they were added; "*" is every file.
 * - The dialog starts in the known folder of SuggestedStartLocation, or in the folder of SuggestedSaveFile, with SuggestedFileName or
 *   that file's name; CommitButtonText names its button and DefaultFileExtension completes a name typed without one.
 * - A file chosen to save is made empty when it is not there yet, so the storage file stands for it. Closing the dialog without a choice
 *   ends the operation with no file.
 * - The dialog belongs to the window IInitializeWithWindow gave, as desktop apps give it, or else to the app's CoreWindow.
 * - ViewMode and SettingsIdentifier are kept but the dialog has one view and does not remember folders; every user is the process's. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <shobjidl.h>
#include <shlobj.h>

static const IID iid_open_picker = {0x2ca8278a, 0x12c5, 0x4c5f, {0x89, 0x77, 0x94, 0x54, 0x77, 0x93, 0xc2, 0x41}};
static const IID iid_open_picker3 = {0xd9a5c5b3, 0xc5dc, 0x5b98, {0xbd, 0x80, 0xa8, 0xd0, 0xca, 0x05, 0x84, 0xd8}};
static const IID iid_open_statics = {0x6821573b, 0x2f02, 0x4833, {0x96, 0xd4, 0xab, 0xbf, 0xad, 0x72, 0xb6, 0x7b}};
static const IID iid_open_statics2 = {0xe8917415, 0xeddd, 0x5c98, {0xb6, 0xf3, 0x36, 0x6f, 0xdf, 0xca, 0xd3, 0x92}};
static const IID iid_save_picker = {0x3286ffcb, 0x617f, 0x4cc5, {0xaf, 0x6a, 0xb3, 0xfd, 0xf2, 0x9a, 0xd1, 0x45}};
static const IID iid_save_picker3 = {0x698aec69, 0xba3c, 0x4e51, {0xbd, 0x90, 0x4a, 0xbc, 0xbb, 0xf4, 0xcf, 0xaf}};
static const IID iid_save_picker4 = {0xe7d83a5a, 0xddfa, 0x5de0, {0x8b, 0x70, 0xc8, 0x42, 0xc2, 0x19, 0x88, 0xec}};
static const IID iid_save_statics = {0x28e3cf9e, 0x961c, 0x5e2c, {0xae, 0xd7, 0xe6, 0x47, 0x37, 0xf4, 0xce, 0x37}};
static const IID iid_folder_picker = {0x084f7799, 0xf3fb, 0x400a, {0x99, 0xb1, 0x7b, 0x4a, 0x77, 0x2f, 0xd6, 0x0d}};
static const IID iid_folder_picker3 = {0x673b1e29, 0xd326, 0x53c0, {0xbd, 0x24, 0xa2, 0x5c, 0x71, 0x4c, 0xee, 0x36}};
static const IID iid_folder_statics = {0x9be34740, 0x7ca1, 0x5942, {0xa3, 0xc8, 0x46, 0xf2, 0x55, 0x1e, 0xcf, 0xf3}};
static const IID iid_string_vector = {0x98b9acc1, 0x4b56, 0x532e, {0xac, 0x73, 0x03, 0xd5, 0x29, 0x1c, 0xca, 0x90}};

/* FileTypeChoices: IMap<String, IVector<String>>, its view, its pairs and their IIterable and IIterator, from the identifiers the
 * parameterized types make: pinterface({3c2925fe-8519-45c1-aa79-197b6718c1c1};string;pinterface({913337e9-...};string)) and so on. */
static const IID iid_choices_map = {0xe475ca9d, 0x6afb, 0x5992, {0x99, 0x3e, 0x53, 0xe6, 0xef, 0x7a, 0x9e, 0xcd}};
static const IID iid_choices_view = {0x153f9c9c, 0xd22a, 0x5c9e, {0x9c, 0x74, 0x8b, 0x85, 0xc9, 0x08, 0xb0, 0x00}};
static const IID iid_choices_pair = {0x174f26c7, 0x79ea, 0x5f7c, {0xbd, 0x70, 0xac, 0x44, 0x57, 0xf2, 0xca, 0xc8}};
static const IID iid_choices_iterable = {0x4fed2669, 0xd0d3, 0x59f6, {0x91, 0xd9, 0x95, 0x90, 0x2d, 0x72, 0x8d, 0x6a}};
static const IID iid_choices_iterator = {0xa11824c9, 0xe458, 0x502a, {0xaf, 0xd8, 0xce, 0x3c, 0xe0, 0xab, 0xd6, 0xfe}};

/* The shell's dialog classes and interfaces. */
static const CLSID clsid_open_dialog = {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
static const CLSID clsid_save_dialog = {0xc0b4e2f3, 0xba21, 0x4773, {0x8d, 0xba, 0x33, 0x5e, 0xc9, 0x46, 0xeb, 0x8b}};
static const IID iid_file_dialog = {0x42f85136, 0xdb7e, 0x439c, {0x85, 0xf1, 0xe4, 0x07, 0x5d, 0x13, 0x5f, 0xc8}};
static const IID iid_file_open_dialog = {0xd57c7288, 0xd4ad, 0x4768, {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
static const IID iid_shell_item = {0x43826d1e, 0xe718, 0x42ee, {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};

static struct map_types choices_types = {&iid_choices_view, &iid_choices_iterable, &iid_choices_iterator, &iid_choices_pair,
	L"Windows.Foundation.Collections.IMapView`2<String, Windows.Foundation.Collections.IVector`1<String>>",
	L"Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Foundation.Collections.IVector`1<String>>",
	L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, "
	L"Windows.Foundation.Collections.IVector`1<String>>>",
	&iid_choices_map, L"Windows.Storage.Pickers.FilePickerFileTypesOrderedMap"};
static INIT_ONCE choices_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK make_choices_types(INIT_ONCE *once, void *parameter, void **context)
{
	map_types_init(&choices_types);
	return TRUE;
}

enum picker_type { OPEN_PICKER, SAVE_PICKER, FOLDER_PICKER };

struct picker {
	struct object base;
	SRWLOCK lock;
	enum picker_type type;
	INT32 view_mode, location;
	HSTRING settings, commit, extension, name, enterprise;
	/* FileTypeFilter, an IVector<String>, or FileTypeChoices. */
	IUnknown *filter;
	struct property_set *choices;
	IUnknown *suggested;
	/* What IInitializeWithWindow gave. */
	HWND window;
};

static struct picker *picker_of(void *iface) { return (struct picker *)owner(iface); }

static void destroy_picker(struct object *base)
{
	struct picker *self = (struct picker *)base;
	WindowsDeleteString(self->settings);
	WindowsDeleteString(self->commit);
	WindowsDeleteString(self->extension);
	WindowsDeleteString(self->name);
	WindowsDeleteString(self->enterprise);
	release(self->filter);
	if (self->choices)
		release(map_face(self->choices));
	release(self->suggested);
}

static HRESULT get_text(struct picker *self, HSTRING *field, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = WindowsDuplicateString(*field, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT put_text(struct picker *self, HSTRING *field, HSTRING value)
{
	HSTRING copy = NULL;
	const HRESULT result = WindowsDuplicateString(value, &copy);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&self->lock);
	HSTRING old = *field;
	*field = copy;
	ReleaseSRWLockExclusive(&self->lock);
	WindowsDeleteString(old);
	return S_OK;
}

static HRESULT get_number(struct picker *self, INT32 *field, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = *(volatile INT32 *)field;
	return S_OK;
}

static HRESULT WINAPI view_mode(void *iface, INT32 *out) { return get_number(picker_of(iface), &picker_of(iface)->view_mode, out); }
static HRESULT WINAPI set_view_mode(void *iface, INT32 value)
{
	if (value != 0 && value != 1)
		return E_INVALIDARG;
	picker_of(iface)->view_mode = value;
	return S_OK;
}
static HRESULT WINAPI settings(void *iface, HSTRING *out) { return get_text(picker_of(iface), &picker_of(iface)->settings, out); }
static HRESULT WINAPI set_settings(void *iface, HSTRING value) { return put_text(picker_of(iface), &picker_of(iface)->settings, value); }
static HRESULT WINAPI location(void *iface, INT32 *out) { return get_number(picker_of(iface), &picker_of(iface)->location, out); }
static HRESULT WINAPI set_location(void *iface, INT32 value)
{
	if (value < 0 || value > 9)
		return E_INVALIDARG;
	picker_of(iface)->location = value;
	return S_OK;
}
static HRESULT WINAPI commit(void *iface, HSTRING *out) { return get_text(picker_of(iface), &picker_of(iface)->commit, out); }
static HRESULT WINAPI set_commit(void *iface, HSTRING value) { return put_text(picker_of(iface), &picker_of(iface)->commit, value); }
static HRESULT WINAPI extension(void *iface, HSTRING *out) { return get_text(picker_of(iface), &picker_of(iface)->extension, out); }
static HRESULT WINAPI set_extension(void *iface, HSTRING value) { return put_text(picker_of(iface), &picker_of(iface)->extension, value); }
static HRESULT WINAPI file_name(void *iface, HSTRING *out) { return get_text(picker_of(iface), &picker_of(iface)->name, out); }
static HRESULT WINAPI set_file_name(void *iface, HSTRING value) { return put_text(picker_of(iface), &picker_of(iface)->name, value); }
static HRESULT WINAPI enterprise(void *iface, HSTRING *out) { return get_text(picker_of(iface), &picker_of(iface)->enterprise, out); }
static HRESULT WINAPI set_enterprise(void *iface, HSTRING value) { return put_text(picker_of(iface), &picker_of(iface)->enterprise, value); }

static HRESULT WINAPI filter(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = picker_of(iface)->filter);
	return S_OK;
}

static HRESULT WINAPI choices(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = map_face(picker_of(iface)->choices));
	return S_OK;
}

static HRESULT WINAPI suggested_file(void *iface, void **out)
{
	struct picker *self = picker_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	reference(*out = self->suggested);
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT WINAPI set_suggested_file(void *iface, IUnknown *file)
{
	struct picker *self = picker_of(iface);
	if (!file)
		return E_INVALIDARG;
	reference(file);
	AcquireSRWLockExclusive(&self->lock);
	IUnknown *old = self->suggested;
	self->suggested = file;
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	return S_OK;
}

static HRESULT WINAPI user(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

/* The patterns of a list of extensions, "*.txt;*.md", or "*.*" for "*", into text of capacity characters. */
static HRESULT patterns_of(IUnknown *extensions, WCHAR *text, size_t capacity)
{
	UINT32 count = 0;
	size_t used = 0;
	IUnknown *vector = NULL;
	text[0] = 0;
	HRESULT result = extensions->lpVtbl->QueryInterface(extensions, &iid_string_vector, (void **)&vector);
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, UINT32 *))method(vector, 7))(vector, &count);
	for (UINT32 at = 0; SUCCEEDED(result) && at < count; ++at) {
		HSTRING item = NULL;
		UINT32 length = 0;
		if (FAILED(result = ((HRESULT (WINAPI *)(void *, UINT32, HSTRING *))method(vector, 6))(vector, at, &item)))
			break;
		const WCHAR *raw = WindowsGetStringRawBuffer(item, &length);
		const BOOL all = length == 1 && raw[0] == L'*';
		const BOOL dotted = length && raw[0] == L'.';
		const size_t needed = (used ? 1 : 0) + (all ? 3 : 1 + (dotted ? 0 : 1) + length);
		if (used + needed + 1 > capacity)
			result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
		else {
			if (used)
				text[used++] = L';';
			if (all) {
				lstrcpyW(text + used, L"*.*");
				used += 3;
			} else {
				text[used++] = L'*';
				if (!dotted)
					text[used++] = L'.';
				memcpy(text + used, raw, (size_t)length * sizeof(WCHAR));
				used += length;
			}
			text[used] = 0;
		}
		WindowsDeleteString(item);
	}
	release(vector);
	return result;
}

/* The file types the dialog shows: one entry of every extension for the filter, one per choice for the save picker. */
struct types {
	COMDLG_FILTERSPEC *specs;
	WCHAR *text;
	UINT count;
};

static void free_types(struct types *types)
{
	FREE(types->specs);
	FREE(types->text);
}

#define PATTERNS 1024

static HRESULT types_of(struct picker *self, struct types *types)
{
	struct entry *entries = NULL;
	UINT32 count = 0;
	HRESULT result = S_OK;
	types->specs = NULL;
	types->text = NULL;
	types->count = 0;
	if (self->type == SAVE_PICKER && FAILED(result = property_set_entries(self->choices, &entries, &count)))
		return result;
	const UINT32 wanted = self->type == SAVE_PICKER ? count : 1;
	if (wanted && (!(types->specs = ALLOC(wanted * sizeof *types->specs)) || !(types->text = ALLOC(wanted * 2 * PATTERNS * sizeof(WCHAR)))))
		result = E_OUTOFMEMORY;
	for (UINT32 at = 0; SUCCEEDED(result) && at < wanted; ++at) {
		WCHAR *name = types->text + at * 2 * PATTERNS, *patterns = name + PATTERNS;
		result = patterns_of(self->type == SAVE_PICKER ? (IUnknown *)entries[at].value : self->filter, patterns, PATTERNS);
		if (FAILED(result) || !patterns[0])
			continue;
		UINT32 length = 0;
		const WCHAR *label = self->type == SAVE_PICKER ? WindowsGetStringRawBuffer(entries[at].key, &length) : patterns;
		lstrcpynW(name, label, PATTERNS);
		types->specs[types->count++] = (COMDLG_FILTERSPEC){name, patterns};
	}
	entries_free(entries, count);
	if (FAILED(result))
		free_types(types);
	return result;
}

/* The known folder a start location names, or NULL. */
static const KNOWNFOLDERID *start_folder(INT32 location)
{
	static const GUID documents = {0xfdd39ad0, 0x238f, 0x46af, {0xad, 0xb4, 0x6c, 0x85, 0x48, 0x03, 0x69, 0xc7}};
	static const GUID desktop = {0xb4bfcc3a, 0xdb2c, 0x424c, {0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41}};
	static const GUID downloads = {0x374de290, 0x123f, 0x4565, {0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b}};
	static const GUID music = {0x4bd8d571, 0x6d19, 0x48d3, {0xbe, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0e, 0x43}};
	static const GUID pictures = {0x33e28130, 0x4e1e, 0x4676, {0x83, 0x5a, 0x98, 0x39, 0x5c, 0x3b, 0xc3, 0xbb}};
	static const GUID videos = {0x18989b1d, 0x99b5, 0x455b, {0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc}};
	static const GUID objects = {0x31c0dd25, 0x9439, 0x4f12, {0xbf, 0x41, 0x7f, 0xf4, 0xed, 0xa3, 0x87, 0x22}};
	static const GUID *const folders[] = {&documents, NULL, &desktop, &downloads, NULL, &music, &pictures, &videos, &objects, NULL};
	return location >= 0 && location < (INT32)ARRAYSIZE(folders) ? folders[location] : NULL;
}

static HRESULT path_of_item(IUnknown *thing, WCHAR *path)
{
	IUnknown *item = NULL;
	HSTRING text = NULL;
	UINT32 length = 0;
	HRESULT result = thing->lpVtbl->QueryInterface(thing, &iid_storage_item, (void **)&item);
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, HSTRING *))method(item, 12))(item, &text);
	const WCHAR *raw = SUCCEEDED(result) ? WindowsGetStringRawBuffer(text, &length) : NULL;
	if (SUCCEEDED(result) && length >= MAX_PATH)
		result = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	if (SUCCEEDED(result)) {
		memcpy(path, raw, (size_t)length * sizeof(WCHAR));
		path[length] = 0;
	}
	WindowsDeleteString(text);
	release(item);
	return result;
}

static void set_start(IFileDialog *dialog, const WCHAR *folder)
{
	IShellItem *item = NULL;
	if (SUCCEEDED(SHCreateItemFromParsingName(folder, NULL, &iid_shell_item, (void **)&item))) {
		dialog->lpVtbl->SetFolder(dialog, item);
		item->lpVtbl->Release(item);
	}
}

/* Sets up the dialog from what the picker holds. */
static HRESULT prepare(struct picker *self, IFileDialog *dialog, BOOL several)
{
	struct types types;
	FILEOPENDIALOGOPTIONS options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
	options |= self->type == SAVE_PICKER ? FOS_OVERWRITEPROMPT : self->type == FOLDER_PICKER ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
	if (several)
		options |= FOS_ALLOWMULTISELECT;
	HRESULT result = dialog->lpVtbl->SetOptions(dialog, options);
	if (SUCCEEDED(result) && SUCCEEDED(result = types_of(self, &types))) {
		if (types.count)
			result = dialog->lpVtbl->SetFileTypes(dialog, types.count, types.specs);
		free_types(&types);
	}
	if (FAILED(result))
		return result;
	AcquireSRWLockShared(&self->lock);
	if (WindowsGetStringLen(self->commit))
		dialog->lpVtbl->SetOkButtonLabel(dialog, WindowsGetStringRawBuffer(self->commit, NULL));
	const WCHAR *extension = WindowsGetStringRawBuffer(self->extension, NULL);
	if (*extension)
		dialog->lpVtbl->SetDefaultExtension(dialog, extension + (*extension == L'.'));
	if (WindowsGetStringLen(self->name))
		dialog->lpVtbl->SetFileName(dialog, WindowsGetStringRawBuffer(self->name, NULL));
	IUnknown *suggested = self->suggested;
	reference(suggested);
	const KNOWNFOLDERID *known = start_folder(self->location);
	ReleaseSRWLockShared(&self->lock);
	WCHAR path[MAX_PATH];
	if (suggested && SUCCEEDED(path_of_item(suggested, path))) {
		WCHAR *slash = path + lstrlenW(path);
		while (slash > path && *slash != L'\\')
			--slash;
		if (*slash == L'\\') {
			dialog->lpVtbl->SetFileName(dialog, slash + 1);
			*slash = 0;
			set_start(dialog, path);
		}
	} else if (known) {
		WCHAR *folder = NULL;
		if (SUCCEEDED(SHGetKnownFolderPath(known, KF_FLAG_CREATE, NULL, &folder)))
			set_start(dialog, folder);
		CoTaskMemFree(folder);
	}
	release(suggested);
	return S_OK;
}

static HRESULT path_of_shell_item(IShellItem *item, WCHAR **out) { return item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, out); }

/* What the dialog chose, as the operation's result. */
static HRESULT chosen(struct picker *self, IFileDialog *dialog, BOOL several, union winrt_value *result)
{
	if (several) {
		IFileOpenDialog *open = NULL;
		IShellItemArray *items = NULL;
		DWORD count = 0;
		WCHAR **paths = NULL;
		HRESULT outcome = dialog->lpVtbl->QueryInterface(dialog, &iid_file_open_dialog, (void **)&open);
		if (SUCCEEDED(outcome))
			outcome = open->lpVtbl->GetResults(open, &items);
		if (SUCCEEDED(outcome))
			outcome = items->lpVtbl->GetCount(items, &count);
		if (SUCCEEDED(outcome) && count && !(paths = ALLOC(count * sizeof *paths)))
			outcome = E_OUTOFMEMORY;
		DWORD made = 0;
		for (; SUCCEEDED(outcome) && made < count; ++made) {
			IShellItem *item = NULL;
			paths[made] = NULL;
			if (SUCCEEDED(outcome = items->lpVtbl->GetItemAt(items, made, &item))) {
				outcome = path_of_shell_item(item, &paths[made]);
				item->lpVtbl->Release(item);
			}
		}
		if (SUCCEEDED(outcome))
			outcome = make_files((const WCHAR *const *)paths, count, &result->object);
		for (DWORD at = 0; at < made; ++at)
			CoTaskMemFree(paths[at]);
		FREE(paths);
		if (items)
			items->lpVtbl->Release(items);
		if (open)
			open->lpVtbl->Release(open);
		return outcome;
	}
	IShellItem *item = NULL;
	WCHAR *path = NULL;
	HRESULT outcome = dialog->lpVtbl->GetResult(dialog, &item);
	if (SUCCEEDED(outcome))
		outcome = path_of_shell_item(item, &path);
	if (SUCCEEDED(outcome) && self->type == SAVE_PICKER) {
		HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			outcome = HRESULT_FROM_WIN32(GetLastError());
		else
			CloseHandle(file);
	}
	if (SUCCEEDED(outcome))
		outcome = self->type == FOLDER_PICKER ? make_folder(path, &result->object) : make_file(path, &result->object);
	CoTaskMemFree(path);
	if (item)
		item->lpVtbl->Release(item);
	return outcome;
}

struct pick {
	struct picker *picker;
	BOOL several;
};

static void free_pick(void *context)
{
	struct pick *pick = context;
	release(face_of(&pick->picker->base, 0));
	FREE(pick);
}

static HRESULT run_pick(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct pick *pick = context;
	struct picker *self = pick->picker;
	IFileDialog *dialog = NULL;
	const HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	HRESULT outcome = CoCreateInstance(self->type == SAVE_PICKER ? &clsid_save_dialog : &clsid_open_dialog, NULL, CLSCTX_INPROC_SERVER,
		&iid_file_dialog, (void **)&dialog);
	if (SUCCEEDED(outcome))
		outcome = prepare(self, dialog, pick->several);
	if (SUCCEEDED(outcome)) {
		outcome = dialog->lpVtbl->Show(dialog, self->window ? self->window : application_window());
		/* No choice is no file, not an error. */
		if (outcome == HRESULT_FROM_WIN32(ERROR_CANCELLED))
			outcome = pick->several ? make_files(NULL, 0, &result->object) : S_OK;
		else if (SUCCEEDED(outcome))
			outcome = chosen(self, dialog, pick->several, result);
	}
	if (dialog)
		dialog->lpVtbl->Release(dialog);
	if (SUCCEEDED(initialized))
		CoUninitialize();
	return outcome;
}

static HRESULT start_pick(void *iface, const IID *operation, BOOL several, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct pick *pick = ALLOC(sizeof *pick);
	if (!pick)
		return E_OUTOFMEMORY;
	pick->picker = picker_of(iface);
	pick->several = several;
	reference(face_of(&pick->picker->base, 0));
	return winrt_async_start(operation, FALSE, WINRT_RESULT_OBJECT, run_pick, pick, free_pick, out);
}

static HRESULT WINAPI pick_file(void *iface, void **out) { return start_pick(iface, &iid_file_operation, FALSE, out); }
static HRESULT WINAPI initialize_window(void *iface, HWND window)
{
	if (window && !IsWindow(window))
		return E_INVALIDARG;
	picker_of(iface)->window = window;
	return S_OK;
}

static HRESULT WINAPI pick_files(void *iface, void **out) { return start_pick(iface, &iid_files_operation, TRUE, out); }
static HRESULT WINAPI pick_folder(void *iface, void **out) { return start_pick(iface, &iid_folder_operation, FALSE, out); }

TABLE(open_table, 0, INSPECTABLE, (const void *)view_mode, (const void *)set_view_mode, (const void *)settings, (const void *)set_settings,
	(const void *)location, (const void *)set_location, (const void *)commit, (const void *)set_commit, (const void *)filter,
	(const void *)pick_file, (const void *)pick_files);
TABLE(open3_table, 1, INSPECTABLE, (const void *)user);
TABLE(open_window_table, 2, UNKNOWN, (const void *)initialize_window);
static const struct face_iid open_iids[] = {{&iid_open_picker, 0}, {&iid_open_picker3, 1}, {&iid_initialize_with_window, 2}, {NULL, 0}};
static const struct kind open_kind = {"FileOpenPicker", L"Windows.Storage.Pickers.FileOpenPicker", open_iids, destroy_picker};

TABLE(save_table, 0, INSPECTABLE, (const void *)settings, (const void *)set_settings, (const void *)location, (const void *)set_location,
	(const void *)commit, (const void *)set_commit, (const void *)choices, (const void *)extension, (const void *)set_extension,
	(const void *)suggested_file, (const void *)set_suggested_file, (const void *)file_name, (const void *)set_file_name,
	(const void *)pick_file);
TABLE(save3_table, 1, INSPECTABLE, (const void *)enterprise, (const void *)set_enterprise);
TABLE(save4_table, 2, INSPECTABLE, (const void *)user);
TABLE(save_window_table, 3, UNKNOWN, (const void *)initialize_window);
static const struct face_iid save_iids[] = {{&iid_save_picker, 0}, {&iid_save_picker3, 1}, {&iid_save_picker4, 2},
	{&iid_initialize_with_window, 3}, {NULL, 0}};
static const struct kind save_kind = {"FileSavePicker", L"Windows.Storage.Pickers.FileSavePicker", save_iids, destroy_picker};

TABLE(folder_table, 0, INSPECTABLE, (const void *)view_mode, (const void *)set_view_mode, (const void *)settings,
	(const void *)set_settings, (const void *)location, (const void *)set_location, (const void *)commit, (const void *)set_commit,
	(const void *)filter, (const void *)pick_folder);
TABLE(folder3_table, 1, INSPECTABLE, (const void *)user);
TABLE(folder_window_table, 2, UNKNOWN, (const void *)initialize_window);
static const struct face_iid folder_iids[] = {{&iid_folder_picker, 0}, {&iid_folder_picker3, 1}, {&iid_initialize_with_window, 2}, {NULL, 0}};
static const struct kind folder_kind = {"FolderPicker", L"Windows.Storage.Pickers.FolderPicker", folder_iids, destroy_picker};

static HRESULT new_picker(enum picker_type type, IInspectable **out)
{
	static const void *const *const open_tables[] = {open_table.slots, open3_table.slots, open_window_table.slots};
	static const void *const *const save_tables[] = {save_table.slots, save3_table.slots, save4_table.slots, save_window_table.slots};
	static const void *const *const folder_tables[] = {folder_table.slots, folder3_table.slots, folder_window_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct picker *self = type == SAVE_PICKER ? make(sizeof *self, &save_kind, save_tables, 4) :
		make(sizeof *self, type == OPEN_PICKER ? &open_kind : &folder_kind, type == OPEN_PICKER ? open_tables : folder_tables, 3);
	if (!self)
		return E_OUTOFMEMORY;
	InitializeSRWLock(&self->lock);
	self->type = type;
	self->location = 9;
	HRESULT result = S_OK;
	if (type == SAVE_PICKER) {
		InitOnceExecuteOnce(&choices_once, make_choices_types, NULL, NULL);
		if (!(self->choices = typed_map_make(&choices_types)))
			result = E_OUTOFMEMORY;
	} else
		result = winrt_vector(&string_list, NULL, 0, (void **)&self->filter);
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI open_activate(void *iface, IInspectable **out) { return new_picker(OPEN_PICKER, out); }
static HRESULT WINAPI save_activate(void *iface, IInspectable **out) { return new_picker(SAVE_PICKER, out); }
static HRESULT WINAPI folder_activate(void *iface, IInspectable **out) { return new_picker(FOLDER_PICKER, out); }
static HRESULT WINAPI open_for_user(void *iface, IInspectable *user, IInspectable **out) { return new_picker(OPEN_PICKER, out); }
static HRESULT WINAPI save_for_user(void *iface, IInspectable *user, IInspectable **out) { return new_picker(SAVE_PICKER, out); }
static HRESULT WINAPI folder_for_user(void *iface, IInspectable *user, IInspectable **out) { return new_picker(FOLDER_PICKER, out); }

/* FileOpenPicker.ResumePickSingleFileAsync continues a pick an app was suspended in, which only phones did. */
static HRESULT WINAPI open_resume(void *iface, void **out) { return missing("FileOpenPicker.ResumePickSingleFileAsync"); }

TABLE(open_factory_table, 0, INSPECTABLE, (const void *)open_activate);
TABLE(open_statics2_table, 1, INSPECTABLE, (const void *)open_for_user);
TABLE(open_statics_table, 2, INSPECTABLE, (const void *)open_resume);
static const struct face_iid open_class_iids[] = {{&iid_factory, 0}, {&iid_open_statics2, 1}, {&iid_open_statics, 2}, {NULL, 0}};
static const struct kind open_class_kind = {"FileOpenPicker", L"Windows.Storage.Pickers.FileOpenPicker", open_class_iids, NULL};
struct object open_picker_class = {{open_factory_table.slots, open_statics2_table.slots, open_statics_table.slots}, &open_class_kind, 1};

TABLE(save_factory_table, 0, INSPECTABLE, (const void *)save_activate);
TABLE(save_statics_table, 1, INSPECTABLE, (const void *)save_for_user);
static const struct face_iid save_class_iids[] = {{&iid_factory, 0}, {&iid_save_statics, 1}, {NULL, 0}};
static const struct kind save_class_kind = {"FileSavePicker", L"Windows.Storage.Pickers.FileSavePicker", save_class_iids, NULL};
struct object save_picker_class = {{save_factory_table.slots, save_statics_table.slots}, &save_class_kind, 1};

TABLE(folder_factory_table, 0, INSPECTABLE, (const void *)folder_activate);
TABLE(folder_statics_table, 1, INSPECTABLE, (const void *)folder_for_user);
static const struct face_iid folder_class_iids[] = {{&iid_factory, 0}, {&iid_folder_statics, 1}, {NULL, 0}};
static const struct kind folder_class_kind = {"FolderPicker", L"Windows.Storage.Pickers.FolderPicker", folder_class_iids, NULL};
struct object folder_picker_class = {{folder_factory_table.slots, folder_statics_table.slots}, &folder_class_kind, 1};
