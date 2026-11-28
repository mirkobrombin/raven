/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The package an app of the Windows Runtime was installed from, and the folders it keeps its data in:
 *
 * - Windows.ApplicationModel.Package.Current: the package of the running process, with its PackageId as kernel32's
 *   GetCurrentPackageId reports it and its installed folder; a process that runs from no package gets NO_PACKAGE, as on Windows.
 * - Windows.Storage.ApplicationData.Current: the package's LocalState, RoamingState, TempState and LocalCache folders under
 *   %LOCALAPPDATA%\Packages\<family name>, made when first asked for. DataChanged is raised by SignalDataChanged; nothing roams.
 * - Its LocalSettings and RoamingSettings, which settings.c keeps; nothing roams.
 * - The folders are StorageFolders of storage.c.
 */
#include "object.h"
#include <appmodel.h>

/* The longest names of a package, without their terminating null, as the Windows SDK sets them. */
#ifndef PACKAGE_FULL_NAME_MAX_LENGTH
#define PACKAGE_FULL_NAME_MAX_LENGTH 127
#endif
#ifndef PACKAGE_FAMILY_NAME_MAX_LENGTH
#define PACKAGE_FAMILY_NAME_MAX_LENGTH 64
#endif
#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700L
#endif

static const IID iid_package_statics = {0x4e534bdf, 0x2960, 0x4878, {0x97, 0xa4, 0x96, 0x24, 0xde, 0xb7, 0x2f, 0x2d}};
static const IID iid_package = {0x163c792f, 0xbd75, 0x413c, {0xbf, 0x23, 0xb1, 0xfe, 0x7b, 0x95, 0xd8, 0x25}};
static const IID iid_package_id = {0x1adb665e, 0x37c7, 0x4790, {0x99, 0x80, 0xdd, 0x7a, 0xe7, 0x4e, 0x8b, 0xb2}};
static const IID iid_data_statics = {0x5612147b, 0xe843, 0x45e3, {0x94, 0xd8, 0x06, 0x16, 0x9e, 0x3c, 0x8e, 0x17}};
static const IID iid_data = {0xc3da6fb7, 0xb744, 0x4b45, {0xb0, 0xb8, 0x22, 0x3a, 0x09, 0x38, 0xd0, 0xdc}};
static const IID iid_data2 = {0x9e65cd69, 0x0ba3, 0x4e32, {0xbe, 0x29, 0xb0, 0x2d, 0xe6, 0x60, 0x76, 0x38}};

/* Windows.ApplicationModel.PackageId: the identity GetCurrentPackageId reports, kept as it came. */
struct package_id {
	struct object base;
	BYTE *buffer;
	WCHAR full_name[PACKAGE_FULL_NAME_MAX_LENGTH + 1], family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
};

static const PACKAGE_ID *identity(void *iface) { return (const PACKAGE_ID *)((struct package_id *)owner(iface))->buffer; }

static void destroy_package_id(struct object *base) { FREE(((struct package_id *)base)->buffer); }

static HRESULT answer_text(const WCHAR *text, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsCreateString(text, text ? (UINT32)lstrlenW(text) : 0, out);
}

static HRESULT WINAPI id_name(void *iface, HSTRING *out) { return answer_text(identity(iface)->name, out); }
static HRESULT WINAPI id_resource(void *iface, HSTRING *out) { return answer_text(identity(iface)->resourceId, out); }
static HRESULT WINAPI id_publisher(void *iface, HSTRING *out) { return answer_text(identity(iface)->publisher, out); }
static HRESULT WINAPI id_publisher_id(void *iface, HSTRING *out) { return answer_text(identity(iface)->publisherId, out); }
static HRESULT WINAPI id_full_name(void *iface, HSTRING *out) { return answer_text(((struct package_id *)owner(iface))->full_name, out); }
static HRESULT WINAPI id_family(void *iface, HSTRING *out) { return answer_text(((struct package_id *)owner(iface))->family, out); }

/* PackageVersion holds Major, Minor, Build and Revision in that order. */
static HRESULT WINAPI id_version(void *iface, UINT16 *out)
{
	const PACKAGE_VERSION version = identity(iface)->version;
	if (!out)
		return E_POINTER;
	out[0] = version.Major;
	out[1] = version.Minor;
	out[2] = version.Build;
	out[3] = version.Revision;
	return S_OK;
}

/* ProcessorArchitecture numbers the architectures as a package's identity does. */
static HRESULT WINAPI id_architecture(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = (INT32)identity(iface)->processorArchitecture;
	return S_OK;
}

TABLE(package_id_table, 0, INSPECTABLE, (const void *)id_name, (const void *)id_version, (const void *)id_architecture,
	(const void *)id_resource, (const void *)id_publisher, (const void *)id_publisher_id, (const void *)id_full_name, (const void *)id_family);
static const struct face_iid package_id_iids[] = {{&iid_package_id, 0}, {NULL, 0}};
static const struct kind package_id_kind = {"PackageId", L"Windows.ApplicationModel.PackageId", package_id_iids, destroy_package_id};

static HRESULT package_name(LONG (WINAPI *get)(UINT32 *, PWSTR), WCHAR *out, UINT32 capacity)
{
	const LONG error = get(&capacity, out);
	return error == ERROR_SUCCESS ? S_OK : error == APPMODEL_ERROR_NO_PACKAGE ? NO_PACKAGE : HRESULT_FROM_WIN32(error);
}

static HRESULT WINAPI package_id(void *iface, void **out)
{
	static const void *const *const tables[] = {package_id_table.slots};
	UINT32 size = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	LONG error = GetCurrentPackageId(&size, NULL);
	if (error != ERROR_INSUFFICIENT_BUFFER)
		return error == APPMODEL_ERROR_NO_PACKAGE ? NO_PACKAGE : HRESULT_FROM_WIN32(error);
	struct package_id *self = make(sizeof *self, &package_id_kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	HRESULT result = (self->buffer = ALLOC(size)) ? S_OK : E_OUTOFMEMORY;
	if (SUCCEEDED(result) && (error = GetCurrentPackageId(&size, self->buffer)) != ERROR_SUCCESS)
		result = HRESULT_FROM_WIN32(error);
	if (SUCCEEDED(result))
		result = package_name(GetCurrentPackageFullName, self->full_name, ARRAYSIZE(self->full_name));
	if (SUCCEEDED(result))
		result = package_name(GetCurrentPackageFamilyName, self->family, ARRAYSIZE(self->family));
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI package_location(void *iface, void **out)
{
	WCHAR path[MAX_PATH];
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = package_name(GetCurrentPackagePath, path, ARRAYSIZE(path));
	return FAILED(result) ? result : make_folder(path, out);
}

static HRESULT WINAPI package_is_framework(void *iface, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI package_dependencies(void *iface, void **out)
{
	answer_null(out);
	return missing("Package.Dependencies");
}

TABLE(package_table, 0, INSPECTABLE, (const void *)package_id, (const void *)package_location, (const void *)package_is_framework,
	(const void *)package_dependencies);
static const struct face_iid package_iids[] = {{&iid_package, 0}, {NULL, 0}};
static const struct kind package_kind = {"Package", L"Windows.ApplicationModel.Package", package_iids, NULL};
static struct object current_package = {{package_table.slots}, &package_kind, 1};

/* Package.Current: the one package of the process, for a process that runs from one. */
static HRESULT WINAPI package_current(void *iface, void **out)
{
	WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = package_name(GetCurrentPackageFamilyName, family, ARRAYSIZE(family));
	if (FAILED(result))
		return result;
	*out = face_of(&current_package, 0);
	return S_OK;
}

TABLE(package_factory_table, 0, FACTORY);
TABLE(package_statics_table, 1, INSPECTABLE, (const void *)package_current);
static const struct face_iid package_factory_iids[] = {{&iid_factory, 0}, {&iid_package_statics, 1}, {NULL, 0}};
static const struct kind package_factory_kind = {"Package", L"Windows.ApplicationModel.Package", package_factory_iids, NULL};
struct object package_class = {{package_factory_table.slots, package_statics_table.slots}, &package_factory_kind, 1};

/* Windows.Storage.ApplicationData of the package. */
static struct handlers data_changed;
static SRWLOCK data_lock = SRWLOCK_INIT;

/* Appends a part to a path and makes the folder it names, unless it is there. */
static HRESULT make_part(WCHAR *path, const WCHAR *part)
{
	if ((size_t)lstrlenW(path) + 1 + (size_t)lstrlenW(part) >= MAX_PATH)
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	lstrcatW(path, L"\\");
	lstrcatW(path, part);
	if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		return HRESULT_FROM_WIN32(GetLastError());
	return S_OK;
}

/* A folder of the package's data, %LOCALAPPDATA%\Packages\<family name>\<leaf>, made with its parents when it is missing. */
HRESULT package_data_path(const WCHAR *leaf, WCHAR *path)
{
	WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
	HRESULT result = package_name(GetCurrentPackageFamilyName, family, ARRAYSIZE(family));
	if (FAILED(result))
		return result;
	const DWORD base = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
	if (!base || base >= MAX_PATH)
		return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	if (FAILED(result = make_part(path, L"Packages")) || FAILED(result = make_part(path, family)) || FAILED(result = make_part(path, leaf)))
		return result;
	return S_OK;
}

static HRESULT data_folder(const WCHAR *leaf, void **out)
{
	WCHAR path[MAX_PATH];
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = package_data_path(leaf, path);
	return FAILED(result) ? result : make_folder(path, out);
}

static HRESULT WINAPI data_version(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI data_set_version(void *iface, UINT32 version, void *handler, void **out)
{
	answer_null(out);
	return missing("ApplicationData.SetVersionAsync");
}

static HRESULT WINAPI data_clear_all(void *iface, void **out)
{
	answer_null(out);
	return missing("ApplicationData.ClearAsync");
}

static HRESULT WINAPI data_clear(void *iface, INT32 locality, void **out)
{
	answer_null(out);
	return missing("ApplicationData.ClearAsync");
}

static HRESULT WINAPI data_local_settings(void *iface, void **out) { return settings_root(0, out); }
static HRESULT WINAPI data_roaming_settings(void *iface, void **out) { return settings_root(1, out); }

static HRESULT WINAPI data_local(void *iface, void **out) { return data_folder(L"LocalState", out); }
static HRESULT WINAPI data_roaming(void *iface, void **out) { return data_folder(L"RoamingState", out); }
static HRESULT WINAPI data_temporary(void *iface, void **out) { return data_folder(L"TempState", out); }
static HRESULT WINAPI data_cache(void *iface, void **out) { return data_folder(L"LocalCache", out); }

static HRESULT WINAPI data_add_changed(void *iface, void *handler, INT64 *token)
{
	AcquireSRWLockExclusive(&data_lock);
	const HRESULT result = handlers_add(&data_changed, handler, token);
	ReleaseSRWLockExclusive(&data_lock);
	return result;
}

static HRESULT WINAPI data_remove_changed(void *iface, INT64 token)
{
	AcquireSRWLockExclusive(&data_lock);
	const HRESULT result = handlers_remove(&data_changed, token);
	ReleaseSRWLockExclusive(&data_lock);
	return result;
}

/* SignalDataChanged raises DataChanged with the application data as the sender and no arguments. The handlers are called outside the
 * lock, so that one may add or remove handlers. */
static HRESULT WINAPI data_signal(void *iface)
{
	struct handlers copy = {0};
	AcquireSRWLockExclusive(&data_lock);
	HRESULT result = S_OK;
	for (UINT32 index = 0; index < data_changed.count && SUCCEEDED(result); ++index) {
		INT64 token;
		result = handlers_add(&copy, data_changed.entries[index].handler, &token);
	}
	ReleaseSRWLockExclusive(&data_lock);
	if (SUCCEEDED(result))
		handlers_raise(&copy, iface, NULL);
	handlers_clear(&copy);
	return result;
}

static HRESULT WINAPI data_quota(void *iface, UINT64 *out)
{
	if (!out)
		return E_POINTER;
	/* Nothing roams, so there is no room for roaming data. */
	*out = 0;
	return S_OK;
}

TABLE(data_table, 0, INSPECTABLE, (const void *)data_version, (const void *)data_set_version, (const void *)data_clear_all,
	(const void *)data_clear, (const void *)data_local_settings, (const void *)data_roaming_settings, (const void *)data_local,
	(const void *)data_roaming, (const void *)data_temporary, (const void *)data_add_changed, (const void *)data_remove_changed,
	(const void *)data_signal, (const void *)data_quota);
TABLE(data2_table, 1, INSPECTABLE, (const void *)data_cache);
static const struct face_iid data_iids[] = {{&iid_data, 0}, {&iid_data2, 1}, {NULL, 0}};
static const struct kind data_kind = {"ApplicationData", L"Windows.Storage.ApplicationData", data_iids, NULL};
static struct object current_data = {{data_table.slots, data2_table.slots}, &data_kind, 1};

/* ApplicationData.Current: the one object of the package; a process that runs from no package has none. */
static HRESULT WINAPI data_current(void *iface, void **out)
{
	WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = package_name(GetCurrentPackageFamilyName, family, ARRAYSIZE(family));
	if (FAILED(result))
		return result;
	*out = face_of(&current_data, 0);
	return S_OK;
}

TABLE(data_factory_table, 0, FACTORY);
TABLE(data_statics_table, 1, INSPECTABLE, (const void *)data_current);
static const struct face_iid data_factory_iids[] = {{&iid_factory, 0}, {&iid_data_statics, 1}, {NULL, 0}};
static const struct kind data_factory_kind = {"ApplicationData", L"Windows.Storage.ApplicationData", data_factory_iids, NULL};
struct object application_data_class = {{data_factory_table.slots, data_statics_table.slots}, &data_factory_kind, 1};
