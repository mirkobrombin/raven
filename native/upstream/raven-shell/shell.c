/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: the common file dialogs, shell items and the class registry behind CoCreateInstance.
 *
 * The dialogs are Raven code: one dialog template built in memory serves GetOpenFileName, GetSaveFileName and the
 * IFileOpenDialog / IFileSaveDialog objects, listing directories through the file system API of the guest. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <servprov.h>
#include <propsys.h>
#include <shellapi.h>
#include <string.h>
#include <stddef.h>

#include "shell.h"

void copy_text(WCHAR *to, size_t capacity, const WCHAR *from)
{
	size_t length = from ? lstrlenW(from) : 0;
	if (length >= capacity)
		length = capacity - 1;
	if (length)
		memcpy(to, from, length * sizeof(WCHAR));
	to[length] = 0;
}

/* RAVEN_TRACE_SHELL names a file that receives one line per interface request and dialog call. */
static void trace(const char *text, const GUID *iid)
{
	WCHAR path[MAX_PATH];
	char line[160];
	DWORD written, count = 0;
	HANDLE file;
	if (!GetEnvironmentVariableW(L"RAVEN_TRACE_SHELL", path, MAX_PATH))
		return;
	while (text[count] && count < 100)
		line[count] = text[count], ++count;
	if (iid) {
		static const char digits[] = "0123456789abcdef";
		const unsigned char *bytes = (const unsigned char *)iid;
		unsigned index;
		line[count++] = ' ';
		for (index = 0; index < 16; ++index)
			line[count++] = digits[bytes[index] >> 4], line[count++] = digits[bytes[index] & 15];
	}
	line[count++] = '\n';
	file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	WriteFile(file, line, count, &written, NULL);
	CloseHandle(file);
}

static BOOL is_separator(WCHAR c)
{
	return c == L'\\' || c == L'/';
}

/* Joins a directory and a name with a single separator. */
void join_path(WCHAR *out, size_t capacity, const WCHAR *dir, const WCHAR *name)
{
	size_t length;
	copy_text(out, capacity, dir);
	length = lstrlenW(out);
	if (length && !is_separator(out[length - 1]) && length + 1 < capacity)
		out[length++] = L'\\', out[length] = 0;
	if (length + lstrlenW(name) < capacity)
		lstrcatW(out, name);
}

BOOL is_absolute(const WCHAR *path)
{
	return is_separator(path[0]) || (path[0] && path[1] == L':');
}

const WCHAR *file_part(const WCHAR *path)
{
	const WCHAR *last = path;
	for (; *path; ++path)
		if (is_separator(*path) || *path == L':')
			last = path + 1;
	return last;
}

void parent_directory(WCHAR *dir)
{
	size_t length = lstrlenW(dir);
	while (length > 3 && is_separator(dir[length - 1]))
		dir[--length] = 0;
	while (length > 3 && !is_separator(dir[length - 1]))
		dir[--length] = 0;
	while (length > 3 && is_separator(dir[length - 1]))
		dir[--length] = 0;
	if (length == 2 && dir[1] == L':')
		dir[length++] = L'\\', dir[length] = 0;
}

static WCHAR fold(WCHAR c)
{
	return c >= L'A' && c <= L'Z' ? (WCHAR)(c + 32) : c;
}

/* Case-insensitive wildcard match of one pattern ('*' and '?') against a file name. */
static BOOL match_pattern(const WCHAR *pattern, const WCHAR *name)
{
	while (*pattern) {
		if (*pattern == L'*') {
			while (*pattern == L'*')
				++pattern;
			if (!*pattern)
				return TRUE;
			for (; *name; ++name)
				if (match_pattern(pattern, name))
					return TRUE;
			return FALSE;
		}
		if (!*name || (*pattern != L'?' && fold(*pattern) != fold(*name)))
			return FALSE;
		++pattern;
		++name;
	}
	return !*name;
}

/* A filter specification is a semicolon separated list of patterns. */
BOOL match_spec(const WCHAR *spec, const WCHAR *name)
{
	WCHAR one[128];
	while (*spec) {
		size_t length = 0;
		while (*spec == L';' || *spec == L' ')
			++spec;
		while (spec[length] && spec[length] != L';' && length < 127)
			++length;
		memcpy(one, spec, length * sizeof(WCHAR));
		one[length] = 0;
		if (length && match_pattern(one, name))
			return TRUE;
		spec += length;
	}
	return FALSE;
}

/* Shell items ----------------------------------------------------------------------------------------------- */

typedef struct {
	IShellItemVtbl *vtbl;
	LONG refs;
	WCHAR path[MAX_PATH];
} Item;

typedef struct {
	IShellItemArrayVtbl *vtbl;
	LONG refs;
	UINT count;
	IShellItem *items[MAX_RESULTS];
} ItemArray;

static BOOL same_iid(REFIID a, REFIID b)
{
	const DWORD *left = (const DWORD *)a, *right = (const DWORD *)b;
	return left[0] == right[0] && left[1] == right[1] && left[2] == right[2] && left[3] == right[3];
}

static HRESULT STDMETHODCALLTYPE item_query(IShellItem *self, REFIID iid, void **out)
{
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IShellItem)) {
		*out = self;
		InterlockedIncrement(&((Item *)self)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE item_add_ref(IShellItem *self) { return (ULONG)InterlockedIncrement(&((Item *)self)->refs); }

static ULONG STDMETHODCALLTYPE item_release(IShellItem *self)
{
	LONG left = InterlockedDecrement(&((Item *)self)->refs);
	if (!left)
		HeapFree(GetProcessHeap(), 0, self);
	return (ULONG)left;
}

static HRESULT STDMETHODCALLTYPE item_bind(IShellItem *self, IBindCtx *context, REFGUID handler, REFIID iid, void **out)
{
	*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE item_parent(IShellItem *self, IShellItem **parent);

static HRESULT STDMETHODCALLTYPE item_display_name(IShellItem *self, SIGDN kind, LPWSTR *name)
{
	const WCHAR *source = ((Item *)self)->path;
	size_t length;
	WCHAR *copy;
	if ((DWORD)kind != 0x80058000 && (DWORD)kind != 0x80028000 && (DWORD)kind != 0x8004c000 && (DWORD)kind != 0x80068000)
		source = file_part(source);
	length = lstrlenW(source);
	copy = CoTaskMemAlloc((length + 1) * sizeof(WCHAR));
	if (!copy) {
		*name = NULL;
		return E_OUTOFMEMORY;
	}
	memcpy(copy, source, (length + 1) * sizeof(WCHAR));
	*name = copy;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE item_attributes(IShellItem *self, SFGAOF mask, SFGAOF *out)
{
	DWORD attributes = GetFileAttributesW(((Item *)self)->path);
	SFGAOF value = 0;
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		value |= SFGAO_FILESYSTEM;
		if (attributes & FILE_ATTRIBUTE_DIRECTORY)
			value |= SFGAO_FOLDER;
	}
	*out = value & mask;
	return (value & mask) == mask ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE item_compare(IShellItem *self, IShellItem *other, SICHINTF hint, int *order)
{
	LPWSTR text = NULL;
	HRESULT result = IShellItem_GetDisplayName(other, SIGDN_FILESYSPATH, &text);
	if (FAILED(result))
		return result;
	*order = lstrcmpiW(((Item *)self)->path, text);
	CoTaskMemFree(text);
	return *order ? S_FALSE : S_OK;
}

static IShellItemVtbl item_vtbl = {
	item_query, item_add_ref, item_release, item_bind, item_parent, item_display_name, item_attributes, item_compare
};

static HRESULT make_item(const WCHAR *path, IShellItem **out)
{
	Item *item = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Item));
	if (!item)
		return E_OUTOFMEMORY;
	item->vtbl = &item_vtbl;
	item->refs = 1;
	copy_text(item->path, MAX_PATH, path);
	*out = (IShellItem *)item;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE item_parent(IShellItem *self, IShellItem **parent)
{
	WCHAR dir[MAX_PATH];
	copy_text(dir, MAX_PATH, ((Item *)self)->path);
	parent_directory(dir);
	return make_item(dir, parent);
}

static HRESULT STDMETHODCALLTYPE array_query(IShellItemArray *self, REFIID iid, void **out)
{
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IShellItemArray)) {
		*out = self;
		InterlockedIncrement(&((ItemArray *)self)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE array_add_ref(IShellItemArray *self) { return (ULONG)InterlockedIncrement(&((ItemArray *)self)->refs); }

static ULONG STDMETHODCALLTYPE array_release(IShellItemArray *self)
{
	ItemArray *array = (ItemArray *)self;
	LONG left = InterlockedDecrement(&array->refs);
	if (!left) {
		UINT index;
		for (index = 0; index < array->count; ++index)
			IShellItem_Release(array->items[index]);
		HeapFree(GetProcessHeap(), 0, array);
	}
	return (ULONG)left;
}

static HRESULT STDMETHODCALLTYPE array_bind(IShellItemArray *s, IBindCtx *c, REFGUID h, REFIID i, void **o) { *o = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE array_store(IShellItemArray *s, GETPROPERTYSTOREFLAGS f, REFIID i, void **o) { *o = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE array_descriptions(IShellItemArray *s, REFPROPERTYKEY k, REFIID i, void **o) { *o = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE array_attributes(IShellItemArray *s, SIATTRIBFLAGS f, SFGAOF m, SFGAOF *o) { *o = 0; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE array_count(IShellItemArray *self, DWORD *count) { *count = ((ItemArray *)self)->count; return S_OK; }

static HRESULT STDMETHODCALLTYPE array_item(IShellItemArray *self, DWORD index, IShellItem **item)
{
	ItemArray *array = (ItemArray *)self;
	if (index >= array->count) {
		*item = NULL;
		return E_INVALIDARG;
	}
	*item = array->items[index];
	IShellItem_AddRef(*item);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE array_enum(IShellItemArray *s, IEnumShellItems **o) { *o = NULL; return E_NOTIMPL; }

static IShellItemArrayVtbl array_vtbl = {
	array_query, array_add_ref, array_release, array_bind, array_store, array_descriptions, array_attributes, array_count, array_item, array_enum
};

/* IFileDialog ----------------------------------------------------------------------------------------------- */

typedef struct {
	void *vtbl;
	void *custom_vtbl; /* IFileDialogCustomize shares the object */
	LONG refs;
	BOOL save;
	DialogState state;
} FileDialog;

static FileDialog *dialog_of(void *self) { return (FileDialog *)self; }
static FileDialog *dialog_of_custom(void *self) { return (FileDialog *)((char *)self - offsetof(FileDialog, custom_vtbl)); }

static HRESULT STDMETHODCALLTYPE fd_query(void *self, REFIID iid, void **out)
{
	FileDialog *dialog = dialog_of(self);
	trace("dialog QueryInterface", iid);
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IModalWindow) || same_iid(iid, &IID_IFileDialog) ||
		same_iid(iid, dialog->save ? &IID_IFileSaveDialog : &IID_IFileOpenDialog)) {
		*out = self;
		InterlockedIncrement(&dialog->refs);
		return S_OK;
	}
	if (same_iid(iid, &IID_IFileDialogCustomize)) {
		*out = &dialog->custom_vtbl;
		InterlockedIncrement(&dialog->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE fd_add_ref(void *self) { return (ULONG)InterlockedIncrement(&dialog_of(self)->refs); }

static ULONG STDMETHODCALLTYPE fd_release(void *self)
{
	LONG left = InterlockedDecrement(&dialog_of(self)->refs);
	if (!left)
		HeapFree(GetProcessHeap(), 0, self);
	return (ULONG)left;
}

static HRESULT STDMETHODCALLTYPE fd_show(void *self, HWND owner)
{
	FileDialog *dialog = dialog_of(self);
	trace("dialog Show", NULL);
	dialog->state.has_result = FALSE;
	if (!run_dialog(owner, &dialog->state))
		return HRESULT_FROM_WIN32(ERROR_CANCELLED);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE fd_set_types(void *self, UINT count, const COMDLG_FILTERSPEC *specs)
{
	DialogState *state = &dialog_of(self)->state;
	UINT index;
	if (count > MAX_FILTERS)
		count = MAX_FILTERS;
	for (index = 0; index < count; ++index) {
		copy_text(state->filters[index].name, 64, specs[index].pszName);
		copy_text(state->filters[index].spec, 256, specs[index].pszSpec);
	}
	state->filter_count = count;
	if (state->filter_index < 1 || state->filter_index > count)
		state->filter_index = count ? 1 : 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE fd_set_index(void *self, UINT index) { dialog_of(self)->state.filter_index = index; return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_get_index(void *self, UINT *index) { *index = dialog_of(self)->state.filter_index; return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_advise(void *self, IFileDialogEvents *events, DWORD *cookie) { *cookie = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_unadvise(void *self, DWORD cookie) { return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_set_options(void *self, FILEOPENDIALOGOPTIONS options) { dialog_of(self)->state.options = options; return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_get_options(void *self, FILEOPENDIALOGOPTIONS *options) { *options = dialog_of(self)->state.options; return S_OK; }

static HRESULT item_path(IShellItem *item, WCHAR *out)
{
	LPWSTR text = NULL;
	HRESULT result = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &text);
	if (FAILED(result))
		return result;
	copy_text(out, MAX_PATH, text);
	CoTaskMemFree(text);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE fd_set_folder(void *self, IShellItem *item)
{
	WCHAR path[MAX_PATH];
	HRESULT result = item ? item_path(item, path) : E_INVALIDARG;
	if (SUCCEEDED(result))
		copy_text(dialog_of(self)->state.dir, MAX_PATH, path);
	return result;
}

static HRESULT STDMETHODCALLTYPE fd_get_folder(void *self, IShellItem **item)
{
	return make_item(dialog_of(self)->state.dir, item);
}

static HRESULT STDMETHODCALLTYPE fd_get_selection(void *self, IShellItem **item)
{
	DialogState *state = &dialog_of(self)->state;
	if (!state->has_result) {
		*item = NULL;
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	return make_item(state->result, item);
}

static HRESULT STDMETHODCALLTYPE fd_set_name(void *self, LPCWSTR name) { copy_text(dialog_of(self)->state.name, MAX_PATH, name); return S_OK; }

static HRESULT STDMETHODCALLTYPE fd_get_name(void *self, LPWSTR *name)
{
	DialogState *state = &dialog_of(self)->state;
	const WCHAR *source = state->has_result ? file_part(state->result) : state->name;
	WCHAR *copy = CoTaskMemAlloc((lstrlenW(source) + 1) * sizeof(WCHAR));
	if (!copy)
		return E_OUTOFMEMORY;
	lstrcpyW(copy, source);
	*name = copy;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE fd_set_title(void *self, LPCWSTR text) { copy_text(dialog_of(self)->state.title, 128, text); return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_set_ok(void *self, LPCWSTR text) { copy_text(dialog_of(self)->state.ok_label, 32, text); return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_set_name_label(void *self, LPCWSTR text) { copy_text(dialog_of(self)->state.name_label, 32, text); return S_OK; }

static HRESULT STDMETHODCALLTYPE fd_get_result(void *self, IShellItem **item)
{
	return fd_get_selection(self, item);
}

static HRESULT STDMETHODCALLTYPE fd_add_place(void *self, IShellItem *item, FDAP placement) { return S_OK; }

static HRESULT STDMETHODCALLTYPE fd_set_extension(void *self, LPCWSTR text)
{
	if (text && text[0] == L'.')
		++text;
	copy_text(dialog_of(self)->state.defext, 16, text);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE fd_close(void *self, HRESULT result) { return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_set_guid(void *self, REFGUID guid) { return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_clear_client(void *self) { return S_OK; }
static HRESULT STDMETHODCALLTYPE fd_set_filter(void *self, IShellItemFilter *filter) { return S_OK; }

static HRESULT STDMETHODCALLTYPE fd_get_results(void *self, IShellItemArray **array)
{
	DialogState *state = &dialog_of(self)->state;
	ItemArray *created;
	if (!state->has_result) {
		*array = NULL;
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	created = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ItemArray));
	if (!created)
		return E_OUTOFMEMORY;
	created->vtbl = &array_vtbl;
	created->refs = 1;
	/* Every chosen file, or the one result of a dialog that chose a single path. */
	for (created->count = 0; created->count < (state->result_count ? state->result_count : 1); ++created->count)
		if (FAILED(make_item(state->result_count ? state->results[created->count] : state->result, &created->items[created->count]))) {
			IShellItemArray_Release((IShellItemArray *)created);
			return E_OUTOFMEMORY;
		}
	*array = (IShellItemArray *)created;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE fd_not_implemented() { return E_NOTIMPL; }

#define COMMON_ENTRIES \
	(void *)fd_query, (void *)fd_add_ref, (void *)fd_release, (void *)fd_show, (void *)fd_set_types, (void *)fd_set_index, \
	(void *)fd_get_index, (void *)fd_advise, (void *)fd_unadvise, (void *)fd_set_options, (void *)fd_get_options, \
	(void *)fd_set_folder /* default folder */, (void *)fd_set_folder, (void *)fd_get_folder, (void *)fd_get_selection, \
	(void *)fd_set_name, (void *)fd_get_name, (void *)fd_set_title, (void *)fd_set_ok, (void *)fd_set_name_label, \
	(void *)fd_get_result, (void *)fd_add_place, (void *)fd_set_extension, (void *)fd_close, (void *)fd_set_guid, \
	(void *)fd_clear_client, (void *)fd_set_filter

static void *open_vtbl[] = { COMMON_ENTRIES, (void *)fd_get_results, (void *)fd_get_results };
static void *save_vtbl[] = { COMMON_ENTRIES, (void *)fd_not_implemented, (void *)fd_not_implemented, (void *)fd_not_implemented,
	(void *)fd_not_implemented, (void *)fd_not_implemented };

/* IFileDialogCustomize: the controls are recorded and answer their state queries; check boxes are shown. */
static HRESULT STDMETHODCALLTYPE cu_query(void *self, REFIID iid, void **out) { return fd_query(dialog_of_custom(self), iid, out); }
static ULONG STDMETHODCALLTYPE cu_add_ref(void *self) { return fd_add_ref(dialog_of_custom(self)); }
static ULONG STDMETHODCALLTYPE cu_release(void *self) { return fd_release(dialog_of_custom(self)); }
static HRESULT STDMETHODCALLTYPE cu_ok() { trace("customize call", NULL); return S_OK; }
static HRESULT STDMETHODCALLTYPE cu_add_check(void *self, DWORD id, LPCWSTR label, BOOL checked)
{
	DialogState *state = &dialog_of_custom(self)->state;
	trace("customize AddCheckButton", NULL);
	if (state->check_count < MAX_CHECKS) {
		state->checks[state->check_count].id = id;
		state->checks[state->check_count].checked = checked;
		copy_text(state->checks[state->check_count].label, 64, label);
		++state->check_count;
	}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE cu_get_check(void *self, DWORD id, BOOL *checked)
{
	DialogState *state = &dialog_of_custom(self)->state;
	UINT index;
	for (index = 0; index < state->check_count; ++index)
		if (state->checks[index].id == id) {
			*checked = state->checks[index].checked;
			return S_OK;
		}
	return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE cu_set_check(void *self, DWORD id, BOOL checked)
{
	DialogState *state = &dialog_of_custom(self)->state;
	UINT index;
	for (index = 0; index < state->check_count; ++index)
		if (state->checks[index].id == id) {
			state->checks[index].checked = checked;
			return S_OK;
		}
	return E_INVALIDARG;
}

static void *custom_vtbl[] = {
	(void *)cu_query, (void *)cu_add_ref, (void *)cu_release, (void *)cu_ok /*EnableOpenDropDown*/, (void *)cu_ok /*AddMenu*/,
	(void *)cu_ok /*AddPushButton*/, (void *)cu_ok /*AddComboBox*/, (void *)cu_ok /*AddRadioButtonList*/, (void *)cu_add_check,
	(void *)cu_ok /*AddEditBox*/, (void *)cu_ok /*AddSeparator*/, (void *)cu_ok /*AddText*/, (void *)cu_ok /*SetControlLabel*/,
	(void *)cu_ok /*GetControlState*/, (void *)cu_ok /*SetControlState*/, (void *)cu_ok /*GetEditBoxText*/, (void *)cu_ok /*SetEditBoxText*/,
	(void *)cu_get_check, (void *)cu_set_check, (void *)cu_ok /*AddControlItem*/, (void *)cu_ok /*RemoveControlItem*/,
	(void *)cu_ok /*RemoveAllControlItems*/, (void *)cu_ok /*GetControlItemState*/, (void *)cu_ok /*SetControlItemState*/,
	(void *)cu_ok /*GetSelectedControlItem*/, (void *)cu_ok /*SetSelectedControlItem*/, (void *)cu_ok /*StartVisualGroup*/,
	(void *)cu_ok /*EndVisualGroup*/, (void *)cu_ok /*MakeProminent*/, (void *)cu_ok /*SetControlItemText*/
};

static HRESULT make_dialog(BOOL save, REFIID iid, void **out)
{
	FileDialog *dialog = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FileDialog));
	HRESULT result;
	if (!dialog)
		return E_OUTOFMEMORY;
	dialog->vtbl = save ? save_vtbl : open_vtbl;
	dialog->custom_vtbl = custom_vtbl;
	dialog->refs = 1;
	dialog->save = save;
	dialog->state.save = save;
	dialog->state.options = FOS_PATHMUSTEXIST | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST);
	result = fd_query(dialog, iid, out);
	fd_release(dialog);
	return result;
}

HRESULT raven_make_shell_link(REFIID iid, void **out);
HRESULT raven_create_registered(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, void **out);
HRESULT raven_registered_class_object(REFCLSID clsid, DWORD context, REFIID iid, void **out);

/* Taskbar list ---------------------------------------------------------------------------------------------- */

/* The taskbar's buttons, progress, overlays and thumbnail toolbars belong to the desktop, which shows none of them for a guest: the
 * object takes every request and has nothing to change. */
typedef struct {
	ITaskbarList4 face;
	LONG refs;
} TaskbarObject;

static HRESULT STDMETHODCALLTYPE tl_query(ITaskbarList4 *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_ITaskbarList) || same_iid(iid, &IID_ITaskbarList2) ||
		same_iid(iid, &IID_ITaskbarList3) || same_iid(iid, &IID_ITaskbarList4)) {
		*out = self;
		InterlockedIncrement(&((TaskbarObject *)self)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE tl_add_ref(ITaskbarList4 *self) { return InterlockedIncrement(&((TaskbarObject *)self)->refs); }
static ULONG STDMETHODCALLTYPE tl_release(ITaskbarList4 *self)
{
	const LONG refs = InterlockedDecrement(&((TaskbarObject *)self)->refs);
	if (!refs)
		HeapFree(GetProcessHeap(), 0, self);
	return refs;
}
static HRESULT STDMETHODCALLTYPE tl_init(ITaskbarList4 *self) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_window(ITaskbarList4 *self, HWND window) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_fullscreen(ITaskbarList4 *self, HWND window, BOOL fullscreen) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_progress(ITaskbarList4 *self, HWND window, ULONGLONG done, ULONGLONG total) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_state(ITaskbarList4 *self, HWND window, TBPFLAG flags) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_pair(ITaskbarList4 *self, HWND tab, HWND window) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_active(ITaskbarList4 *self, HWND tab, HWND window, DWORD reserved) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_buttons(ITaskbarList4 *self, HWND window, UINT count, LPTHUMBBUTTON buttons) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_images(ITaskbarList4 *self, HWND window, HIMAGELIST images) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_overlay(ITaskbarList4 *self, HWND window, HICON icon, LPCWSTR description) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_tooltip(ITaskbarList4 *self, HWND window, LPCWSTR tip) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_clip(ITaskbarList4 *self, HWND window, RECT *clip) { return S_OK; }
static HRESULT STDMETHODCALLTYPE tl_properties(ITaskbarList4 *self, HWND tab, STPFLAG flags) { return S_OK; }

static ITaskbarList4Vtbl taskbar_vtbl = {tl_query, tl_add_ref, tl_release, tl_init, tl_window, tl_window, tl_window, tl_window,
	tl_fullscreen, tl_progress, tl_state, tl_pair, tl_window, tl_pair, tl_active, tl_buttons, tl_buttons, tl_images, tl_overlay,
	tl_tooltip, tl_clip, tl_properties};

static HRESULT make_taskbar_list(REFIID iid, void **out)
{
	TaskbarObject *list = HeapAlloc(GetProcessHeap(), 0, sizeof *list);
	if (!list)
		return E_OUTOFMEMORY;
	list->face.lpVtbl = &taskbar_vtbl;
	list->refs = 1;
	const HRESULT result = tl_query(&list->face, iid, out);
	tl_release(&list->face);
	return result;
}

/* The classes this component serves itself, which CoCreateInstance makes directly and CoGetClassObject through a class factory. */
static HRESULT make_builtin(REFCLSID clsid, REFIID iid, void **out, BOOL *served)
{
	*served = TRUE;
	if (same_iid(clsid, &CLSID_FileOpenDialog))
		return make_dialog(FALSE, iid, out);
	if (same_iid(clsid, &CLSID_FileSaveDialog))
		return make_dialog(TRUE, iid, out);
	if (same_iid(clsid, &CLSID_ShellLink))
		return raven_make_shell_link(iid, out);
	if (same_iid(clsid, &CLSID_TaskbarList))
		return make_taskbar_list(iid, out);
	*served = FALSE;
	return REGDB_E_CLASSNOTREG;
}

typedef struct {
	IClassFactory face;
	LONG refs;
	CLSID clsid;
} BuiltinFactory;

static HRESULT STDMETHODCALLTYPE bf_query(IClassFactory *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_iid(iid, &IID_IUnknown) || same_iid(iid, &IID_IClassFactory)) {
		*out = self;
		InterlockedIncrement(&((BuiltinFactory *)self)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE bf_add_ref(IClassFactory *self) { return InterlockedIncrement(&((BuiltinFactory *)self)->refs); }
static ULONG STDMETHODCALLTYPE bf_release(IClassFactory *self)
{
	const LONG refs = InterlockedDecrement(&((BuiltinFactory *)self)->refs);
	if (!refs)
		HeapFree(GetProcessHeap(), 0, self);
	return refs;
}
static HRESULT STDMETHODCALLTYPE bf_create(IClassFactory *self, IUnknown *outer, REFIID iid, void **out)
{
	BOOL served;
	if (!out)
		return E_POINTER;
	*out = NULL;
	return outer ? CLASS_E_NOAGGREGATION : make_builtin(&((BuiltinFactory *)self)->clsid, iid, out, &served);
}
static HRESULT STDMETHODCALLTYPE bf_lock(IClassFactory *self, BOOL lock) { return S_OK; }
static IClassFactoryVtbl builtin_factory_vtbl = {bf_query, bf_add_ref, bf_release, bf_create, bf_lock};

static BOOL builtin_class(REFCLSID clsid)
{
	return same_iid(clsid, &CLSID_FileOpenDialog) || same_iid(clsid, &CLSID_FileSaveDialog) || same_iid(clsid, &CLSID_ShellLink) ||
		same_iid(clsid, &CLSID_TaskbarList);
}


/* Class registry -------------------------------------------------------------------------------------------- */

HRESULT WINAPI CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID *out)
{
	trace("CoCreateInstance", clsid);
	trace("  requested", iid);
	if (out)
		*out = NULL;
	if (!out)
		return E_POINTER;
	if (builtin_class(clsid)) {
		BOOL served;
		return outer ? CLASS_E_NOAGGREGATION : make_builtin(clsid, iid, out, &served);
	}
	/* Any other class is served by the library the registry names for it. */
	return raven_create_registered(clsid, outer, context, iid, out);
}

HRESULT WINAPI CoGetClassObject(REFCLSID clsid, DWORD context, LPVOID server, REFIID iid, LPVOID *out)
{
	if (!out)
		return E_INVALIDARG;
	if (builtin_class(clsid)) {
		BuiltinFactory *factory = HeapAlloc(GetProcessHeap(), 0, sizeof *factory);
		if (!factory)
			return E_OUTOFMEMORY;
		factory->face.lpVtbl = &builtin_factory_vtbl;
		factory->refs = 1;
		factory->clsid = *clsid;
		const HRESULT result = bf_query(&factory->face, iid, out);
		bf_release(&factory->face);
		return result;
	}
	return raven_registered_class_object(clsid, context, iid, out);
}

/* shlwapi names this export by its ordinal (176): ask an object for a service through its service provider. */
HRESULT WINAPI IUnknown_QueryService(IUnknown *object, REFGUID service, REFIID iid, void **out)
{
	IServiceProvider *provider;
	HRESULT result;
	if (out)
		*out = NULL;
	if (!object || !out)
		return E_FAIL;
	result = IUnknown_QueryInterface(object, &IID_IServiceProvider, (void **)&provider);
	if (FAILED(result))
		return result;
	result = IServiceProvider_QueryService(provider, service, iid, out);
	IServiceProvider_Release(provider);
	return result;
}

HRESULT WINAPI SHCreateItemFromParsingName(PCWSTR path, IBindCtx *context, REFIID iid, void **out)
{
	IShellItem *item;
	HRESULT result;
	WCHAR full[MAX_PATH];
	if (out)
		*out = NULL;
	if (!path || !out)
		return E_INVALIDARG;
	if (is_absolute(path)) {
		copy_text(full, MAX_PATH, path);
	} else {
		GetCurrentDirectoryW(MAX_PATH, full);
		join_path(full, MAX_PATH, full, path);
	}
	if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
		return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	result = make_item(full, &item);
	if (SUCCEEDED(result)) {
		result = IShellItem_QueryInterface(item, iid, out);
		IShellItem_Release(item);
	}
	return result;
}

/* Classic dialogs ------------------------------------------------------------------------------------------- */

static void load_classic_filters(DialogState *state, const WCHAR *filter)
{
	state->filter_count = 0;
	while (filter && *filter && state->filter_count < MAX_FILTERS) {
		Filter *entry = &state->filters[state->filter_count];
		copy_text(entry->name, 64, filter);
		filter += lstrlenW(filter) + 1;
		if (!*filter)
			break;
		copy_text(entry->spec, 256, filter);
		filter += lstrlenW(filter) + 1;
		++state->filter_count;
	}
}

static BOOL classic_dialog(OPENFILENAMEW *request, BOOL save)
{
	DialogState *state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DialogState));
	BOOL accepted;
	if (!state)
		return FALSE;
	state->save = save;
	state->options = 0;
	if (request->Flags & OFN_FILEMUSTEXIST)
		state->options |= FOS_FILEMUSTEXIST;
	if (request->Flags & OFN_PATHMUSTEXIST)
		state->options |= FOS_PATHMUSTEXIST;
	if (request->Flags & OFN_OVERWRITEPROMPT)
		state->options |= FOS_OVERWRITEPROMPT;
	if (request->Flags & OFN_ALLOWMULTISELECT)
		state->options |= FOS_ALLOWMULTISELECT;
	load_classic_filters(state, request->lpstrFilter);
	state->filter_index = request->nFilterIndex ? request->nFilterIndex : 1;
	copy_text(state->title, 128, request->lpstrTitle);
	copy_text(state->dir, MAX_PATH, request->lpstrInitialDir);
	if (request->lpstrFile && request->lpstrFile[0]) {
		const WCHAR *initial = request->lpstrFile;
		if (is_absolute(initial)) {
			copy_text(state->name, MAX_PATH, file_part(initial));
			if (!state->dir[0]) {
				copy_text(state->dir, MAX_PATH, initial);
				parent_directory(state->dir);
			}
		} else {
			copy_text(state->name, MAX_PATH, initial);
		}
	}
	copy_text(state->defext, 16, request->lpstrDefExt);
	accepted = run_dialog(request->hwndOwner, state);
	if (accepted && state->result_count > 1) {
		/* Several files: the folder, then each name, each ended by a null and the list by another. */
		WCHAR *out = request->lpstrFile;
		size_t needed = lstrlenW(state->dir) + 2, at;
		UINT index;
		for (index = 0; index < state->result_count; ++index)
			needed += lstrlenW(file_part(state->results[index])) + 1;
		if (!out || needed > request->nMaxFile) {
			if (out && request->nMaxFile >= 2)
				*(WORD *)out = (WORD)needed;
			accepted = FALSE;
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
		} else {
			lstrcpyW(out, state->dir);
			at = lstrlenW(out) + 1;
			request->nFileOffset = (WORD)at;
			request->nFileExtension = 0;
			for (index = 0; index < state->result_count; ++index) {
				lstrcpyW(out + at, file_part(state->results[index]));
				at += lstrlenW(out + at) + 1;
			}
			out[at] = 0;
			request->nFilterIndex = state->filter_index;
		}
	} else if (accepted) {
		size_t length = lstrlenW(state->result);
		if (!request->lpstrFile || length + 1 > request->nMaxFile) {
			request->nMaxFile = (DWORD)(length + 1);
			accepted = FALSE;
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
		} else {
			const WCHAR *name = file_part(state->result);
			const WCHAR *dot = NULL;
			const WCHAR *scan;
			lstrcpyW(request->lpstrFile, state->result);
			request->nFileOffset = (WORD)(name - state->result);
			for (scan = name; *scan; ++scan)
				if (*scan == L'.')
					dot = scan;
			request->nFileExtension = dot ? (WORD)(dot + 1 - state->result) : 0;
			request->nFilterIndex = state->filter_index;
			if (request->lpstrFileTitle && request->nMaxFileTitle)
				copy_text(request->lpstrFileTitle, request->nMaxFileTitle, name);
		}
	}
	HeapFree(GetProcessHeap(), 0, state);
	return accepted;
}

BOOL WINAPI GetOpenFileNameW(LPOPENFILENAMEW request) { return classic_dialog(request, FALSE); }
BOOL WINAPI GetSaveFileNameW(LPOPENFILENAMEW request) { return classic_dialog(request, TRUE); }
DWORD WINAPI CommDlgExtendedError(void) { return 0; }

short WINAPI GetFileTitleW(LPCWSTR file, LPWSTR title, WORD size)
{
	const WCHAR *name;
	size_t length;
	if (!file || !*file)
		return -1;
	name = file_part(file);
	length = lstrlenW(name);
	if (!title || length + 1 > size)
		return (short)(length + 1);
	lstrcpyW(title, name);
	return 0;
}

/* A window's property store: the values a program sets on it (its application id, for one) kept in memory for as long as the process
   runs. Strings are copied; other values are kept as they are. */
typedef struct {
	PROPERTYKEY key;
	PROPVARIANT value;
} StoredProperty;

typedef struct {
	IPropertyStore store;
	HWND window;
	StoredProperty *items;
	DWORD count;
} WindowStore;

static WindowStore **window_stores;
static DWORD window_store_count;

static BOOL same_key(const PROPERTYKEY *a, const PROPERTYKEY *b)
{
	return InlineIsEqualGUID(&a->fmtid, &b->fmtid) && a->pid == b->pid;
}

static HRESULT copy_value(PROPVARIANT *to, const PROPVARIANT *from)
{
	*to = *from;
	if (from->vt == VT_LPWSTR && from->pwszVal) {
		size_t length = (size_t)lstrlenW(from->pwszVal) + 1;
		to->pwszVal = CoTaskMemAlloc(length * sizeof(WCHAR));
		if (!to->pwszVal)
			return E_OUTOFMEMORY;
		memcpy(to->pwszVal, from->pwszVal, length * sizeof(WCHAR));
	}
	return S_OK;
}

static HRESULT WINAPI store_query(IPropertyStore *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (InlineIsEqualGUID(iid, &IID_IUnknown) || InlineIsEqualGUID(iid, &IID_IPropertyStore)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI store_reference(IPropertyStore *self) { return 1; }
static HRESULT WINAPI store_count(IPropertyStore *self, DWORD *count)
{
	if (!count)
		return E_POINTER;
	*count = ((WindowStore *)self)->count;
	return S_OK;
}
static HRESULT WINAPI store_at(IPropertyStore *self, DWORD index, PROPERTYKEY *key)
{
	WindowStore *store = (WindowStore *)self;
	if (!key)
		return E_POINTER;
	if (index >= store->count)
		return E_INVALIDARG;
	*key = store->items[index].key;
	return S_OK;
}
static HRESULT WINAPI store_get(IPropertyStore *self, REFPROPERTYKEY key, PROPVARIANT *value)
{
	WindowStore *store = (WindowStore *)self;
	DWORD index;
	if (!key || !value)
		return E_POINTER;
	for (index = 0; index < store->count; ++index)
		if (same_key(&store->items[index].key, key))
			return copy_value(value, &store->items[index].value);
	memset(value, 0, sizeof(*value));
	return S_OK;
}
static HRESULT WINAPI store_set(IPropertyStore *self, REFPROPERTYKEY key, REFPROPVARIANT value)
{
	WindowStore *store = (WindowStore *)self;
	StoredProperty *grown;
	DWORD index;
	if (!key || !value)
		return E_POINTER;
	for (index = 0; index < store->count; ++index)
		if (same_key(&store->items[index].key, key)) {
			if (store->items[index].value.vt == VT_LPWSTR)
				CoTaskMemFree(store->items[index].value.pwszVal);
			return copy_value(&store->items[index].value, value);
		}
	grown = store->items ? HeapReAlloc(GetProcessHeap(), 0, store->items, (store->count + 1) * sizeof(StoredProperty))
		: HeapAlloc(GetProcessHeap(), 0, sizeof(StoredProperty));
	if (!grown)
		return E_OUTOFMEMORY;
	store->items = grown;
	store->items[store->count].key = *key;
	if (FAILED(copy_value(&store->items[store->count].value, value)))
		return E_OUTOFMEMORY;
	++store->count;
	return S_OK;
}
static HRESULT WINAPI store_commit(IPropertyStore *self) { return S_OK; }

static IPropertyStoreVtbl store_table = {store_query, store_reference, store_reference, store_count, store_at, store_get, store_set, store_commit};

HRESULT WINAPI SHGetPropertyStoreForWindow(HWND window, REFIID iid, void **out)
{
	WindowStore *store = NULL, **grown;
	DWORD index;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsWindow(window))
		return E_INVALIDARG;
	for (index = 0; index < window_store_count; ++index)
		if (window_stores[index]->window == window)
			store = window_stores[index];
	if (!store) {
		store = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WindowStore));
		grown = window_stores ? HeapReAlloc(GetProcessHeap(), 0, window_stores, (window_store_count + 1) * sizeof(WindowStore *))
			: HeapAlloc(GetProcessHeap(), 0, sizeof(WindowStore *));
		if (!store || !grown)
			return E_OUTOFMEMORY;
		store->store.lpVtbl = &store_table;
		store->window = window;
		window_stores = grown;
		window_stores[window_store_count++] = store;
	}
	return store_query(&store->store, iid, out);
}

/* Frees what a PROPVARIANT owns and empties it. */
HRESULT WINAPI PropVariantClear(PROPVARIANT *value)
{
	if (!value)
		return S_OK;
	switch (value->vt) {
	case VT_LPWSTR:
	case VT_LPSTR:
	case VT_CLSID:
		CoTaskMemFree(value->pwszVal);
		break;
	case VT_BSTR: {
		HMODULE automation = GetModuleHandleW(L"oleaut32.dll");
		void (WINAPI *free_string)(BSTR) = automation ? (void (WINAPI *)(BSTR))GetProcAddress(automation, "SysFreeString") : NULL;
		if (free_string)
			free_string(value->bstrVal);
		break;
	}
	case VT_UNKNOWN:
	case VT_DISPATCH:
		if (value->punkVal)
			IUnknown_Release(value->punkVal);
		break;
	case VT_BLOB:
		CoTaskMemFree(value->blob.pBlobData);
		break;
	default:
		break;
	}
	memset(value, 0, sizeof(*value));
	return S_OK;
}

/* The size of one element of a counted vector of fixed-size values; zero for element types that own memory or are not known. */
static SIZE_T vector_element(VARTYPE type)
{
	switch (type) {
	case VT_I1:
	case VT_UI1:
		return 1;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		return 2;
	case VT_I4:
	case VT_UI4:
	case VT_R4:
	case VT_ERROR:
		return 4;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_CY:
	case VT_DATE:
	case VT_FILETIME:
		return 8;
	case VT_CLSID:
		return sizeof(CLSID);
	default:
		return 0;
	}
}

/* A copy of memory from the task allocator; NULL stays NULL. */
static void *task_copy(const void *source, SIZE_T size)
{
	void *copy;
	if (!source)
		return NULL;
	if ((copy = CoTaskMemAlloc(size ? size : 1)))
		memcpy(copy, source, size);
	return copy;
}

/* Copies a PROPVARIANT with what it owns: strings, blobs, class identifiers and vectors are duplicated, interfaces referenced again. */
HRESULT WINAPI PropVariantCopy(PROPVARIANT *target, const PROPVARIANT *source)
{
	BOOL failed = FALSE;
	if (!target || !source)
		return E_INVALIDARG;
	*target = *source;
	switch (source->vt) {
	case VT_LPWSTR:
		target->pwszVal = task_copy(source->pwszVal, source->pwszVal ? (lstrlenW(source->pwszVal) + 1) * sizeof(WCHAR) : 0);
		failed = source->pwszVal && !target->pwszVal;
		break;
	case VT_LPSTR:
		target->pszVal = task_copy(source->pszVal, source->pszVal ? lstrlenA(source->pszVal) + 1 : 0);
		failed = source->pszVal && !target->pszVal;
		break;
	case VT_CLSID:
		target->puuid = task_copy(source->puuid, sizeof(CLSID));
		failed = source->puuid && !target->puuid;
		break;
	case VT_BLOB:
		target->blob.pBlobData = task_copy(source->blob.pBlobData, source->blob.cbSize);
		failed = source->blob.pBlobData && !target->blob.pBlobData;
		break;
	case VT_BSTR: {
		HMODULE automation = LoadLibraryW(L"oleaut32.dll");
		BSTR (WINAPI *copy_string)(LPCSTR, UINT) = automation ? (BSTR (WINAPI *)(LPCSTR, UINT))GetProcAddress(automation, "SysAllocStringByteLen") : NULL;
		if (source->bstrVal) {
			if (!copy_string)
				return E_OUTOFMEMORY;
			target->bstrVal = copy_string((LPCSTR)source->bstrVal, ((const UINT *)source->bstrVal)[-1]);
			failed = !target->bstrVal;
		}
		break;
	}
	case VT_UNKNOWN:
	case VT_DISPATCH:
		if (source->punkVal)
			IUnknown_AddRef(source->punkVal);
		break;
	default:
		if (source->vt & VT_VECTOR) {
			const VARTYPE element = source->vt & ~VT_VECTOR;
			const ULONG count = source->cac.cElems;
			if (element == VT_LPWSTR || element == VT_LPSTR) {
				target->calpwstr.pElems = CoTaskMemAlloc(count ? count * sizeof(LPWSTR) : 1);
				failed = !target->calpwstr.pElems;
				for (ULONG index = 0; !failed && index < count; ++index) {
					const void *text = source->calpwstr.pElems[index];
					const SIZE_T size = !text ? 0 : element == VT_LPWSTR ? (lstrlenW(text) + 1) * sizeof(WCHAR) : lstrlenA(text) + 1;
					target->calpwstr.pElems[index] = task_copy(text, size);
					failed = text && !target->calpwstr.pElems[index];
				}
				break;
			}
			const SIZE_T size = vector_element(element);
			if (!size)
				return DISP_E_BADVARTYPE;
			target->cac.pElems = task_copy(source->cac.pElems, count * size);
			failed = source->cac.pElems && !target->cac.pElems;
		} else if (source->vt & (VT_ARRAY | VT_BYREF)) {
			return DISP_E_BADVARTYPE;
		}
		break;
	}
	if (failed) {
		memset(target, 0, sizeof(*target));
		return E_OUTOFMEMORY;
	}
	return S_OK;
}

/* There is no taskbar here: it is not auto-hidden, has no position and holds no application bars. */
UINT_PTR WINAPI SHAppBarMessage(DWORD message, PAPPBARDATA data)
{
	switch (message) {
	case ABM_GETSTATE:
	case ABM_GETAUTOHIDEBAR:
	case 11: /* ABM_GETAUTOHIDEBAREX */
		return 0;
	default:
		return FALSE;
	}
}

/* The icons of a program or library are its RT_GROUP_ICON resources, counted in the order the resources list them; a negative index
 * names a resource identifier instead. An .ico file holds a single icon. */
struct icon_search {
	int wanted, seen;
	LPWSTR found;
};

static BOOL CALLBACK icon_group(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR context)
{
	struct icon_search *search = (struct icon_search *)context;
	if (search->wanted >= 0 && search->seen++ == search->wanted) {
		search->found = name;
		return FALSE;
	}
	if (search->wanted < 0)
		++search->seen;
	return TRUE;
}

static BOOL icon_file(LPCWSTR file)
{
	const size_t length = wcslen(file);
	return length > 4 && !lstrcmpiW(file + length - 4, L".ico");
}

UINT WINAPI ExtractIconExW(LPCWSTR file, int index, HICON *large, HICON *small, UINT count)
{
	HMODULE module;
	struct icon_search search = {-1, 0, NULL};
	UINT made = 0;
	if (!file)
		return 0;
	if (icon_file(file)) {
		if (index == -1 && !large && !small)
			return GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES ? 0 : 1;
		if (index != 0 || !count)
			return 0;
		if (large)
			*large = LoadImageW(NULL, file, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
		if (small)
			*small = LoadImageW(NULL, file, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
		return (large && *large) || (small && *small) ? 1 : 0;
	}
	if (!(module = LoadLibraryExW(file, NULL, LOAD_LIBRARY_AS_DATAFILE)))
		return 0;
	if (index == -1 && !large && !small) {
		EnumResourceNamesW(module, (LPCWSTR)RT_GROUP_ICON, icon_group, (LONG_PTR)&search);
		FreeLibrary(module);
		return (UINT)search.seen;
	}
	for (; made < count; ++made) {
		LPCWSTR name;
		HICON big = NULL, little = NULL;
		if (index < 0) {
			name = MAKEINTRESOURCEW(-index);
			if (made)
				break;
		} else {
			search.wanted = index + (int)made;
			search.seen = 0;
			search.found = NULL;
			EnumResourceNamesW(module, (LPCWSTR)RT_GROUP_ICON, icon_group, (LONG_PTR)&search);
			if (!search.found)
				break;
			name = search.found;
		}
		if (large)
			big = LoadImageW(module, name, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
		if (small)
			little = LoadImageW(module, name, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
		if ((large && !big) || (small && !little)) {
			if (big)
				DestroyIcon(big);
			if (little)
				DestroyIcon(little);
			break;
		}
		if (large)
			large[made] = big;
		if (small)
			small[made] = little;
	}
	FreeLibrary(module);
	return made;
}

HICON WINAPI ExtractIconW(HINSTANCE instance, LPCWSTR file, UINT index)
{
	HICON icon = NULL;
	if (index == UINT_MAX)
		return (HICON)(UINT_PTR)ExtractIconExW(file, -1, NULL, NULL, 0);
	return ExtractIconExW(file, (int)index, &icon, NULL, 1) ? icon : NULL;
}

/* UI Automation's reserved values: two objects of the process that stand for a property a provider does not support and for a text
 * attribute that changes across a range. Clients compare against them by identity, so each is one object that lives as long as the
 * process does. */
static HRESULT WINAPI reserved_query(IUnknown *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_iid(iid, &IID_IUnknown)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI reserved_reference(IUnknown *self) { return 1; }

static const IUnknownVtbl reserved_vtbl = { reserved_query, reserved_reference, reserved_reference };
static IUnknown reserved_not_supported = { (IUnknownVtbl *)&reserved_vtbl };
static IUnknown reserved_mixed = { (IUnknownVtbl *)&reserved_vtbl };

HRESULT WINAPI UiaGetReservedNotSupportedValue(IUnknown **value)
{
	if (!value)
		return E_INVALIDARG;
	*value = &reserved_not_supported;
	return S_OK;
}

HRESULT WINAPI UiaGetReservedMixedAttributeValue(IUnknown **value)
{
	if (!value)
		return E_INVALIDARG;
	*value = &reserved_mixed;
	return S_OK;
}

/* The task allocator as an IMalloc: its memory is the memory CoTaskMemAlloc hands out. */
static HRESULT WINAPI malloc_query(IMalloc *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (InlineIsEqualGUID(iid, &IID_IUnknown) || InlineIsEqualGUID(iid, &IID_IMalloc)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI malloc_reference(IMalloc *self) { return 1; }
static void *WINAPI malloc_alloc(IMalloc *self, SIZE_T bytes) { return CoTaskMemAlloc(bytes); }
static void *WINAPI malloc_realloc(IMalloc *self, void *memory, SIZE_T bytes) { return CoTaskMemRealloc(memory, bytes); }
static void WINAPI malloc_free(IMalloc *self, void *memory) { CoTaskMemFree(memory); }
static SIZE_T WINAPI malloc_size(IMalloc *self, void *memory) { return (SIZE_T)-1; }
static int WINAPI malloc_did_alloc(IMalloc *self, void *memory) { return -1; }
static void WINAPI malloc_minimize(IMalloc *self) {}

static IMallocVtbl malloc_table = {malloc_query, malloc_reference, malloc_reference, malloc_alloc, malloc_realloc,
	malloc_free, malloc_size, malloc_did_alloc, malloc_minimize};
static IMalloc task_malloc = {&malloc_table};

HRESULT WINAPI CoGetMalloc(DWORD context, LPMALLOC *out)
{
	if (!out)
		return E_POINTER;
	if (context != MEMCTX_TASK) {
		*out = NULL;
		return E_INVALIDARG;
	}
	*out = &task_malloc;
	return S_OK;
}

/* The shell hands out the same task allocator. */
HRESULT WINAPI SHGetMalloc(LPMALLOC *out)
{
	return CoGetMalloc(MEMCTX_TASK, out);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
