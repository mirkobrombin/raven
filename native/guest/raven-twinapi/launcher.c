/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.System.Launcher and LauncherOptions: a file, a folder or an address opened with the program the system has for it, as
 * ShellExecute opens it, on a thread of the operation's own; the operation answers whether something opened it.
 *
 * - Files of executable code are not launched, as on Windows: the kinds the Attachment Manager rates high risk that run code, and
 *   PowerShell scripts.
 * - When nothing opens the target and the options carry a FallbackUri, that address is opened instead.
 * - The options keep every value an app sets; the dialogs some of them ask for (an application picker, a warning for untrusted
 *   content) are not shown, and LauncherOptions.UI is named in Raven's trace. Every user is the process's. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <shellapi.h>

static const IID iid_launcher_statics = {0x277151c3, 0x9e3e, 0x42f6, {0x91, 0xa4, 0x5d, 0xfd, 0xeb, 0x23, 0x24, 0x51}};
static const IID iid_launcher_statics3 = {0x234261a8, 0x9db3, 0x4683, {0xaa, 0x42, 0xdc, 0x6f, 0x51, 0xd3, 0x38, 0x47}};
static const IID iid_launcher_statics5 = {0x5b24ef84, 0xd895, 0x5fea, {0x91, 0x53, 0x1a, 0xc4, 0x9a, 0xed, 0x9b, 0xa9}};
static const IID iid_options = {0xbafa21d8, 0xb071, 0x4cd8, {0x85, 0x3e, 0x34, 0x12, 0x03, 0xe5, 0x57, 0xd3}};
static const IID iid_options2 = {0x3ba08eb4, 0x6e40, 0x4dce, {0xa1, 0xa3, 0x2f, 0x53, 0x95, 0x0a, 0xfb, 0x49}};
static const IID iid_options3 = {0xf0770655, 0x4b63, 0x4e3a, {0x91, 0x07, 0x4e, 0x68, 0x78, 0x41, 0x92, 0x3a}};
static const IID iid_options4 = {0xef6fd10e, 0xe6fb, 0x4814, {0xa4, 0x4e, 0x57, 0xe8, 0xb9, 0xd9, 0xa0, 0x1b}};
static const IID iid_view_options = {0x8a9b29f1, 0x7ca7, 0x49de, {0x9b, 0xd3, 0x3c, 0x5b, 0x71, 0x84, 0xf6, 0x16}};
static const IID iid_storage_item = {0x4207a996, 0xca2f, 0x42f7, {0xbd, 0xe8, 0x8b, 0x10, 0x45, 0x7a, 0x7f, 0x30}};
/* IAsyncOperation<Boolean>. */
static const IID iid_bool_operation = {0xcdb5efb3, 0x5788, 0x509d, {0x9b, 0xe1, 0x71, 0xcc, 0xb8, 0xa3, 0x36, 0x2a}};

typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClass uri_type;

/* LauncherOptions: what an app asks of a launch. */
struct options {
	struct object base;
	SRWLOCK lock;
	boolean untrusted, picker, ignore_handlers, limit_picker;
	INT32 remaining_view;
	HSTRING preferred_family, preferred_name, content_type, target_family;
	uri_type *fallback;
	IUnknown *neighbors;
};

static struct options *options_of(void *iface) { return (struct options *)owner(iface); }

static void destroy_options(struct object *base)
{
	struct options *self = (struct options *)base;
	WindowsDeleteString(self->preferred_family);
	WindowsDeleteString(self->preferred_name);
	WindowsDeleteString(self->content_type);
	WindowsDeleteString(self->target_family);
	release(self->fallback);
	release(self->neighbors);
}

static HRESULT get_flag(boolean *field, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = *(volatile boolean *)field;
	return S_OK;
}

static HRESULT get_text(struct options *self, HSTRING *field, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = WindowsDuplicateString(*field, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT put_text(struct options *self, HSTRING *field, HSTRING value)
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

static HRESULT get_object(struct options *self, void *field, void **out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	reference(*out = *(IUnknown **)field);
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT put_object(struct options *self, void *field, IUnknown *value)
{
	reference(value);
	AcquireSRWLockExclusive(&self->lock);
	IUnknown *old = *(IUnknown **)field;
	*(IUnknown **)field = value;
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	return S_OK;
}

static HRESULT WINAPI untrusted(void *iface, boolean *out) { return get_flag(&options_of(iface)->untrusted, out); }
static HRESULT WINAPI set_untrusted(void *iface, boolean value) { options_of(iface)->untrusted = value; return S_OK; }
static HRESULT WINAPI picker(void *iface, boolean *out) { return get_flag(&options_of(iface)->picker, out); }
static HRESULT WINAPI set_picker(void *iface, boolean value) { options_of(iface)->picker = value; return S_OK; }
static HRESULT WINAPI options_ui(void *iface, void **out) { return missing("LauncherOptions.UI"); }
static HRESULT WINAPI preferred_family(void *iface, HSTRING *out) { return get_text(options_of(iface), &options_of(iface)->preferred_family, out); }
static HRESULT WINAPI set_preferred_family(void *iface, HSTRING value) { return put_text(options_of(iface), &options_of(iface)->preferred_family, value); }
static HRESULT WINAPI preferred_name(void *iface, HSTRING *out) { return get_text(options_of(iface), &options_of(iface)->preferred_name, out); }
static HRESULT WINAPI set_preferred_name(void *iface, HSTRING value) { return put_text(options_of(iface), &options_of(iface)->preferred_name, value); }
static HRESULT WINAPI fallback(void *iface, void **out) { return get_object(options_of(iface), &options_of(iface)->fallback, out); }
static HRESULT WINAPI set_fallback(void *iface, IUnknown *value) { return put_object(options_of(iface), &options_of(iface)->fallback, value); }
static HRESULT WINAPI content_type(void *iface, HSTRING *out) { return get_text(options_of(iface), &options_of(iface)->content_type, out); }
static HRESULT WINAPI set_content_type(void *iface, HSTRING value) { return put_text(options_of(iface), &options_of(iface)->content_type, value); }
static HRESULT WINAPI target_family(void *iface, HSTRING *out) { return get_text(options_of(iface), &options_of(iface)->target_family, out); }
static HRESULT WINAPI set_target_family(void *iface, HSTRING value) { return put_text(options_of(iface), &options_of(iface)->target_family, value); }
static HRESULT WINAPI neighbors(void *iface, void **out) { return get_object(options_of(iface), &options_of(iface)->neighbors, out); }
static HRESULT WINAPI set_neighbors(void *iface, IUnknown *value) { return put_object(options_of(iface), &options_of(iface)->neighbors, value); }
static HRESULT WINAPI ignore_handlers(void *iface, boolean *out) { return get_flag(&options_of(iface)->ignore_handlers, out); }
static HRESULT WINAPI set_ignore_handlers(void *iface, boolean value) { options_of(iface)->ignore_handlers = value; return S_OK; }
static HRESULT WINAPI limit_picker(void *iface, boolean *out) { return get_flag(&options_of(iface)->limit_picker, out); }
static HRESULT WINAPI set_limit_picker(void *iface, boolean value) { options_of(iface)->limit_picker = value; return S_OK; }

static HRESULT WINAPI remaining_view(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = options_of(iface)->remaining_view;
	return S_OK;
}

static HRESULT WINAPI set_remaining_view(void *iface, INT32 value) { options_of(iface)->remaining_view = value; return S_OK; }

TABLE(options_table, 0, INSPECTABLE, (const void *)untrusted, (const void *)set_untrusted, (const void *)picker, (const void *)set_picker,
	(const void *)options_ui, (const void *)preferred_family, (const void *)set_preferred_family, (const void *)preferred_name,
	(const void *)set_preferred_name, (const void *)fallback, (const void *)set_fallback, (const void *)content_type,
	(const void *)set_content_type);
TABLE(options2_table, 1, INSPECTABLE, (const void *)target_family, (const void *)set_target_family, (const void *)neighbors,
	(const void *)set_neighbors);
TABLE(options3_table, 2, INSPECTABLE, (const void *)ignore_handlers, (const void *)set_ignore_handlers);
TABLE(options4_table, 3, INSPECTABLE, (const void *)limit_picker, (const void *)set_limit_picker);
TABLE(view_options_table, 4, INSPECTABLE, (const void *)remaining_view, (const void *)set_remaining_view);
static const struct face_iid options_iids[] = {{&iid_options, 0}, {&iid_options2, 1}, {&iid_options3, 2}, {&iid_options4, 3},
	{&iid_view_options, 4}, {NULL, 0}};
static const struct kind options_kind = {"LauncherOptions", L"Windows.System.LauncherOptions", options_iids, destroy_options};

static HRESULT WINAPI options_activate(void *iface, IInspectable **out)
{
	static const void *const *const tables[] = {options_table.slots, options2_table.slots, options3_table.slots, options4_table.slots,
		view_options_table.slots};
	if (!out)
		return E_POINTER;
	struct options *self = make(sizeof *self, &options_kind, tables, 5);
	if (!self) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	InitializeSRWLock(&self->lock);
	*out = face_of(&self->base, 0);
	return S_OK;
}

TABLE(options_factory_table, 0, INSPECTABLE, (const void *)options_activate);
static const struct face_iid options_class_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind options_class_kind = {"LauncherOptions", L"Windows.System.LauncherOptions", options_class_iids, NULL};
struct object launcher_options_class = {{options_factory_table.slots}, &options_class_kind, 1};

/* The kinds of file whose content runs as code, which a launch refuses. */
static BOOL runs_code(const WCHAR *path)
{
	static const WCHAR *const kinds[] = {L"exe", L"com", L"scr", L"pif", L"bat", L"cmd", L"msi", L"msp", L"mst", L"js", L"jse", L"vb",
		L"vbs", L"vbe", L"ws", L"wsc", L"wsf", L"wsh", L"hta", L"cpl", L"msc", L"reg", L"inf", L"lnk", L"url", L"scf", L"sct", L"shb",
		L"shs", L"ps1"};
	const WCHAR *dot = NULL;
	for (const WCHAR *at = path; *at; ++at)
		if (*at == L'.')
			dot = at;
		else if (*at == L'\\' || *at == L'/')
			dot = NULL;
	if (!dot)
		return FALSE;
	for (size_t index = 0; index < ARRAYSIZE(kinds); ++index)
		if (CompareStringOrdinal(dot + 1, -1, kinds[index], -1, TRUE) == CSTR_EQUAL)
			return TRUE;
	return FALSE;
}

/* A launch: the target as text, whether it is a file whose kind is checked, and the address to open when nothing opens the target. */
struct launch {
	WCHAR *target;
	BOOL file;
	uri_type *fallback;
};

static void free_launch(void *context)
{
	struct launch *launch = context;
	FREE(launch->target);
	release(launch->fallback);
	FREE(launch);
}

static BOOL open_target(const WCHAR *target)
{
	return (INT_PTR)ShellExecuteW(NULL, L"open", target, NULL, NULL, SW_SHOWNORMAL) > 32;
}

static HRESULT run_launch(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct launch *launch = context;
	result->boolean = !(launch->file && runs_code(launch->target)) && open_target(launch->target);
	if (!result->boolean && launch->fallback) {
		HSTRING address = NULL;
		if (SUCCEEDED(launch->fallback->lpVtbl->get_AbsoluteUri(launch->fallback, &address)))
			result->boolean = open_target(WindowsGetStringRawBuffer(address, NULL));
		WindowsDeleteString(address);
	}
	return S_OK;
}

/* Starts the launch of text, length characters, with the options' fallback address. */
static HRESULT start_launch(const WCHAR *text, UINT32 length, BOOL file, IUnknown *options, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct launch *launch = ALLOC(sizeof *launch);
	if (!launch)
		return E_OUTOFMEMORY;
	launch->file = file;
	launch->fallback = NULL;
	if (!(launch->target = ALLOC(((size_t)length + 1) * sizeof(WCHAR)))) {
		FREE(launch);
		return E_OUTOFMEMORY;
	}
	memcpy(launch->target, text, (size_t)length * sizeof(WCHAR));
	launch->target[length] = 0;
	IUnknown *values = NULL;
	if (options && SUCCEEDED(options->lpVtbl->QueryInterface(options, &iid_options, (void **)&values))) {
		((HRESULT (WINAPI *)(void *, void **))method(values, 15))(values, (void **)&launch->fallback);
		release(values);
	}
	return winrt_async_start(&iid_bool_operation, FALSE, WINRT_RESULT_BOOLEAN, run_launch, launch, free_launch, out);
}

/* A storage file's or folder's path. */
static HRESULT launch_item(IUnknown *item, IUnknown *options, BOOL file, void **out)
{
	IUnknown *storage = NULL;
	HSTRING path = NULL;
	UINT32 length = 0;
	if (!item)
		return E_INVALIDARG;
	HRESULT result = item->lpVtbl->QueryInterface(item, &iid_storage_item, (void **)&storage);
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, HSTRING *))method(storage, 12))(storage, &path);
	if (SUCCEEDED(result)) {
		const WCHAR *text = WindowsGetStringRawBuffer(path, &length);
		result = start_launch(text, length, file, options, out);
	}
	WindowsDeleteString(path);
	release(storage);
	return result;
}

static HRESULT launch_uri(uri_type *uri, IUnknown *options, void **out)
{
	HSTRING address = NULL;
	UINT32 length = 0;
	if (!uri)
		return E_INVALIDARG;
	HRESULT result = uri->lpVtbl->get_AbsoluteUri(uri, &address);
	if (SUCCEEDED(result)) {
		const WCHAR *text = WindowsGetStringRawBuffer(address, &length);
		result = start_launch(text, length, FALSE, options, out);
	}
	WindowsDeleteString(address);
	return result;
}

static HRESULT launch_path(HSTRING path, IUnknown *options, void **out)
{
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(path, &length);
	if (!length)
		return E_INVALIDARG;
	return start_launch(text, length, FALSE, options, out);
}

static HRESULT WINAPI launch_file(void *iface, IUnknown *file, void **out) { return launch_item(file, NULL, TRUE, out); }
static HRESULT WINAPI launch_file_options(void *iface, IUnknown *file, IUnknown *options, void **out) { return launch_item(file, options, TRUE, out); }
static HRESULT WINAPI launch_address(void *iface, uri_type *uri, void **out) { return launch_uri(uri, NULL, out); }
static HRESULT WINAPI launch_address_options(void *iface, uri_type *uri, IUnknown *options, void **out) { return launch_uri(uri, options, out); }
static HRESULT WINAPI launch_folder(void *iface, IUnknown *folder, void **out) { return launch_item(folder, NULL, FALSE, out); }
static HRESULT WINAPI launch_folder_options(void *iface, IUnknown *folder, IUnknown *options, void **out)
{
	return launch_item(folder, options, FALSE, out);
}
static HRESULT WINAPI launch_folder_path(void *iface, HSTRING path, void **out) { return launch_path(path, NULL, out); }
static HRESULT WINAPI launch_folder_path_options(void *iface, HSTRING path, IUnknown *options, void **out) { return launch_path(path, options, out); }
static HRESULT WINAPI launch_folder_path_user(void *iface, IUnknown *user, HSTRING path, void **out) { return launch_path(path, NULL, out); }
static HRESULT WINAPI launch_folder_path_options_user(void *iface, IUnknown *user, HSTRING path, IUnknown *options, void **out)
{
	return launch_path(path, options, out);
}

TABLE(launcher_factory_table, 0, FACTORY);
TABLE(launcher_statics_table, 1, INSPECTABLE, (const void *)launch_file, (const void *)launch_file_options, (const void *)launch_address,
	(const void *)launch_address_options);
TABLE(launcher_statics3_table, 2, INSPECTABLE, (const void *)launch_folder, (const void *)launch_folder_options);
TABLE(launcher_statics5_table, 3, INSPECTABLE, (const void *)launch_folder_path, (const void *)launch_folder_path_options,
	(const void *)launch_folder_path_user, (const void *)launch_folder_path_options_user);
static const struct face_iid launcher_class_iids[] = {{&iid_factory, 0}, {&iid_launcher_statics, 1}, {&iid_launcher_statics3, 2},
	{&iid_launcher_statics5, 3}, {NULL, 0}};
static const struct kind launcher_class_kind = {"Launcher", L"Windows.System.Launcher", launcher_class_iids, NULL};
struct object launcher_class = {{launcher_factory_table.slots, launcher_statics_table.slots, launcher_statics3_table.slots,
	launcher_statics5_table.slots}, &launcher_class_kind, 1};
