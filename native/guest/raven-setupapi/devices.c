/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "setup.h"
#include <objbase.h>
#include <string.h>

/* This runtime has no plug and play manager and no device tree, so a device set is honest about
 * being empty: it opens, it enumerates nothing and it closes. Device classes do exist, as registry
 * data under Control\Class, and those queries answer from there. */

#define SET_MAGIC 0x56444652
#define CLASS_ROOT L"System\\CurrentControlSet\\Control\\Class"

struct device_set {
	DWORD magic;
	GUID class_guid;
	BOOL has_class;
};

static HDEVINFO new_set(const GUID *guid)
{
	struct device_set *set = setup_alloc(sizeof(*set));

	if (!set) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	set->magic = SET_MAGIC;
	if (guid) {
		set->class_guid = *guid;
		set->has_class = TRUE;
	}
	return set;
}

static struct device_set *set_of(HDEVINFO handle)
{
	struct device_set *set = handle;

	if (!handle || handle == INVALID_HANDLE_VALUE || set->magic != SET_MAGIC) {
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	return set;
}

HDEVINFO WINAPI SetupDiCreateDeviceInfoListExW(const GUID *guid, HWND parent, PCWSTR machine, PVOID reserved)
{
	if (machine && *machine) {
		SetLastError(ERROR_INVALID_MACHINENAME);
		return INVALID_HANDLE_VALUE;
	}
	return new_set(guid);
}

HDEVINFO WINAPI SetupDiCreateDeviceInfoListExA(const GUID *guid, HWND parent, PCSTR machine, PVOID reserved)
{
	if (machine && *machine) {
		SetLastError(ERROR_INVALID_MACHINENAME);
		return INVALID_HANDLE_VALUE;
	}
	return new_set(guid);
}

HDEVINFO WINAPI SetupDiCreateDeviceInfoList(const GUID *guid, HWND parent)
{
	return new_set(guid);
}

HDEVINFO WINAPI SetupDiGetClassDevsExW(const GUID *guid, PCWSTR enumerator, HWND parent, DWORD flags, HDEVINFO existing, PCWSTR machine, PVOID reserved)
{
	if (existing && existing != INVALID_HANDLE_VALUE)
		return existing;
	if (!guid && !(flags & DIGCF_ALLCLASSES)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	return new_set(flags & DIGCF_ALLCLASSES ? NULL : guid);
}

HDEVINFO WINAPI SetupDiGetClassDevsExA(const GUID *guid, PCSTR enumerator, HWND parent, DWORD flags, HDEVINFO existing, PCSTR machine, PVOID reserved)
{
	return SetupDiGetClassDevsExW(guid, NULL, parent, flags, existing, NULL, reserved);
}

HDEVINFO WINAPI SetupDiGetClassDevsW(const GUID *guid, PCWSTR enumerator, HWND parent, DWORD flags)
{
	return SetupDiGetClassDevsExW(guid, enumerator, parent, flags, NULL, NULL, NULL);
}

HDEVINFO WINAPI SetupDiGetClassDevsA(const GUID *guid, PCSTR enumerator, HWND parent, DWORD flags)
{
	return SetupDiGetClassDevsExA(guid, enumerator, parent, flags, NULL, NULL, NULL);
}

BOOL WINAPI SetupDiDestroyDeviceInfoList(HDEVINFO handle)
{
	struct device_set *set = set_of(handle);

	if (!set)
		return FALSE;
	set->magic = 0;
	setup_free(set);
	return TRUE;
}

BOOL WINAPI SetupDiEnumDeviceInfo(HDEVINFO handle, DWORD index, PSP_DEVINFO_DATA data)
{
	if (!set_of(handle))
		return FALSE;
	SetLastError(ERROR_NO_MORE_ITEMS);
	return FALSE;
}

BOOL WINAPI SetupDiEnumDeviceInterfaces(HDEVINFO handle, PSP_DEVINFO_DATA device, const GUID *guid, DWORD index, PSP_DEVICE_INTERFACE_DATA data)
{
	if (!set_of(handle))
		return FALSE;
	SetLastError(ERROR_NO_MORE_ITEMS);
	return FALSE;
}

BOOL WINAPI SetupDiGetDeviceInfoListClass(HDEVINFO handle, LPGUID guid)
{
	struct device_set *set = set_of(handle);

	if (!set)
		return FALSE;
	if (!set->has_class) {
		SetLastError(ERROR_NO_ASSOCIATED_CLASS);
		return FALSE;
	}
	*guid = set->class_guid;
	return TRUE;
}

/* Classes. */

static HKEY open_class_root(REGSAM access)
{
	HKEY key;

	return RegOpenKeyExW(HKEY_LOCAL_MACHINE, CLASS_ROOT, 0, access, &key) == ERROR_SUCCESS ? key : NULL;
}

HKEY WINAPI SetupDiOpenClassRegKeyExW(const GUID *guid, REGSAM access, DWORD flags, PCWSTR machine, PVOID reserved)
{
	HKEY root, key = INVALID_HANDLE_VALUE;
	WCHAR text[40];

	if (machine && *machine) {
		SetLastError(ERROR_INVALID_MACHINENAME);
		return INVALID_HANDLE_VALUE;
	}
	if (flags == DIOCR_INTERFACE) {
		SetLastError(ERROR_INVALID_CLASS);
		return INVALID_HANDLE_VALUE;
	}
	root = open_class_root(guid ? KEY_READ : access);
	if (!root) {
		SetLastError(ERROR_INVALID_CLASS);
		return INVALID_HANDLE_VALUE;
	}
	if (!guid)
		return root;
	StringFromGUID2(guid, text, ARRAYSIZE(text));
	{
		LONG error = RegOpenKeyExW(root, text, 0, access, &key);
		RegCloseKey(root);
		if (error != ERROR_SUCCESS) {
			SetLastError(ERROR_INVALID_CLASS);
			return INVALID_HANDLE_VALUE;
		}
	}
	return key;
}

HKEY WINAPI SetupDiOpenClassRegKeyExA(const GUID *guid, REGSAM access, DWORD flags, PCSTR machine, PVOID reserved)
{
	if (machine && *machine) {
		SetLastError(ERROR_INVALID_MACHINENAME);
		return INVALID_HANDLE_VALUE;
	}
	return SetupDiOpenClassRegKeyExW(guid, access, flags, NULL, reserved);
}

HKEY WINAPI SetupDiOpenClassRegKey(const GUID *guid, REGSAM access)
{
	return SetupDiOpenClassRegKeyExW(guid, access, DIOCR_INSTALLER, NULL, NULL);
}

BOOL WINAPI SetupDiClassNameFromGuidExW(const GUID *guid, PWSTR buffer, DWORD size, PDWORD required, PCWSTR machine, PVOID reserved)
{
	HKEY key = SetupDiOpenClassRegKeyExW(guid, KEY_READ, DIOCR_INSTALLER, machine, reserved);
	WCHAR name[MAX_CLASS_NAME_LEN];
	DWORD bytes = sizeof(name), type;
	BOOL result = FALSE;

	if (key == INVALID_HANDLE_VALUE)
		return FALSE;
	if (RegQueryValueExW(key, L"Class", NULL, &type, (BYTE *)name, &bytes) == ERROR_SUCCESS && type == REG_SZ)
		result = setup_give_wide(name, buffer, size, required);
	else
		SetLastError(ERROR_INVALID_CLASS);
	RegCloseKey(key);
	return result;
}

BOOL WINAPI SetupDiClassNameFromGuidW(const GUID *guid, PWSTR buffer, DWORD size, PDWORD required)
{
	return SetupDiClassNameFromGuidExW(guid, buffer, size, required, NULL, NULL);
}

BOOL WINAPI SetupDiClassNameFromGuidExA(const GUID *guid, PSTR buffer, DWORD size, PDWORD required, PCSTR machine, PVOID reserved)
{
	WCHAR name[MAX_CLASS_NAME_LEN];
	BOOL result = SetupDiClassNameFromGuidExW(guid, name, ARRAYSIZE(name), NULL, NULL, reserved);

	return setup_wide_to_ansi_result(result, name, buffer, size, required);
}

BOOL WINAPI SetupDiClassNameFromGuidA(const GUID *guid, PSTR buffer, DWORD size, PDWORD required)
{
	return SetupDiClassNameFromGuidExA(guid, buffer, size, required, NULL, NULL);
}

BOOL WINAPI SetupDiClassGuidsFromNameExW(PCWSTR name, LPGUID list, DWORD size, PDWORD required, PCWSTR machine, PVOID reserved)
{
	HKEY root = open_class_root(KEY_READ);
	DWORD found = 0, at = 0;
	WCHAR text[40];

	if (machine && *machine) {
		if (root)
			RegCloseKey(root);
		SetLastError(ERROR_INVALID_MACHINENAME);
		return FALSE;
	}
	if (root) {
		for (;;) {
			DWORD length = ARRAYSIZE(text);
			HKEY key;

			if (RegEnumKeyExW(root, at++, text, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
				break;
			if (RegOpenKeyExW(root, text, 0, KEY_READ, &key) == ERROR_SUCCESS) {
				WCHAR value[MAX_CLASS_NAME_LEN];
				DWORD bytes = sizeof(value), type;

				if (RegQueryValueExW(key, L"Class", NULL, &type, (BYTE *)value, &bytes) == ERROR_SUCCESS && !lstrcmpiW(value, name)) {
					if (found < size)
						CLSIDFromString(text, &list[found]);
					found++;
				}
				RegCloseKey(key);
			}
		}
		RegCloseKey(root);
	}
	if (required)
		*required = found;
	if (found > size) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI SetupDiClassGuidsFromNameW(PCWSTR name, LPGUID list, DWORD size, PDWORD required)
{
	return SetupDiClassGuidsFromNameExW(name, list, size, required, NULL, NULL);
}

BOOL WINAPI SetupDiClassGuidsFromNameExA(PCSTR name, LPGUID list, DWORD size, PDWORD required, PCSTR machine, PVOID reserved)
{
	WCHAR *wide = setup_to_wide(name);
	BOOL result = SetupDiClassGuidsFromNameExW(wide, list, size, required, NULL, reserved);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupDiClassGuidsFromNameA(PCSTR name, LPGUID list, DWORD size, PDWORD required)
{
	return SetupDiClassGuidsFromNameExA(name, list, size, required, NULL, NULL);
}

BOOL WINAPI SetupDiGetClassDescriptionExW(const GUID *guid, PWSTR buffer, DWORD size, PDWORD required, PCWSTR machine, PVOID reserved)
{
	HKEY key = SetupDiOpenClassRegKeyExW(guid, KEY_READ, DIOCR_INSTALLER, machine, reserved);
	WCHAR text[LINE_LEN];
	DWORD bytes = sizeof(text), type;
	BOOL result = FALSE;

	if (key == INVALID_HANDLE_VALUE)
		return FALSE;
	if (RegQueryValueExW(key, NULL, NULL, &type, (BYTE *)text, &bytes) == ERROR_SUCCESS && type == REG_SZ)
		result = setup_give_wide(text, buffer, size, required);
	else
		SetLastError(ERROR_INVALID_CLASS);
	RegCloseKey(key);
	return result;
}

BOOL WINAPI SetupDiGetClassDescriptionW(const GUID *guid, PWSTR buffer, DWORD size, PDWORD required)
{
	return SetupDiGetClassDescriptionExW(guid, buffer, size, required, NULL, NULL);
}

BOOL WINAPI SetupDiGetClassDescriptionExA(const GUID *guid, PSTR buffer, DWORD size, PDWORD required, PCSTR machine, PVOID reserved)
{
	WCHAR text[LINE_LEN];
	BOOL result = SetupDiGetClassDescriptionExW(guid, text, ARRAYSIZE(text), NULL, NULL, reserved);

	return setup_wide_to_ansi_result(result, text, buffer, size, required);
}

BOOL WINAPI SetupDiGetClassDescriptionA(const GUID *guid, PSTR buffer, DWORD size, PDWORD required)
{
	return SetupDiGetClassDescriptionExA(guid, buffer, size, required, NULL, NULL);
}

BOOL WINAPI SetupDiGetINFClassW(PCWSTR path, LPGUID guid, PWSTR name, DWORD size, PDWORD required)
{
	HINF hinf = SetupOpenInfFileW(path, NULL, INF_STYLE_WIN4, NULL);
	INFCONTEXT context;
	BOOL result = FALSE;

	if (hinf == INVALID_HANDLE_VALUE)
		return FALSE;
	if (SetupFindFirstLineW(hinf, L"Version", L"ClassGUID", &context)) {
		const WCHAR *text = pSetupGetField(&context, 1);
		if (text && SUCCEEDED(CLSIDFromString((LPOLESTR)text, guid))) {
			if (SetupFindFirstLineW(hinf, L"Version", L"Class", &context)) {
				const WCHAR *class_name = pSetupGetField(&context, 1);
				result = setup_give_wide(class_name ? class_name : L"", name, size, required);
			} else {
				result = SetupDiClassNameFromGuidW(guid, name, size, required);
			}
		}
	} else {
		SetLastError(ERROR_INVALID_CLASS);
	}
	SetupCloseInfFile(hinf);
	return result;
}

BOOL WINAPI SetupDiGetINFClassA(PCSTR path, LPGUID guid, PSTR name, DWORD size, PDWORD required)
{
	WCHAR *wide = setup_to_wide(path), text[MAX_CLASS_NAME_LEN];
	BOOL result = SetupDiGetINFClassW(wide, guid, text, ARRAYSIZE(text), NULL);

	setup_free(wide);
	return setup_wide_to_ansi_result(result, text, name, size, required);
}

BOOL WINAPI SetupDiBuildClassInfoListExW(DWORD flags, LPGUID list, DWORD size, PDWORD required, PCWSTR machine, PVOID reserved)
{
	HKEY root = open_class_root(KEY_READ);
	DWORD found = 0, at = 0;
	WCHAR text[40];

	if (root) {
		for (;;) {
			DWORD length = ARRAYSIZE(text);
			if (RegEnumKeyExW(root, at++, text, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
				break;
			if (found < size)
				CLSIDFromString(text, &list[found]);
			found++;
		}
		RegCloseKey(root);
	}
	if (required)
		*required = found;
	if (found > size) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI SetupDiBuildClassInfoListExA(DWORD flags, LPGUID list, DWORD size, PDWORD required, PCSTR machine, PVOID reserved)
{
	return SetupDiBuildClassInfoListExW(flags, list, size, required, NULL, reserved);
}

BOOL WINAPI SetupDiBuildClassInfoList(DWORD flags, LPGUID list, DWORD size, PDWORD required)
{
	return SetupDiBuildClassInfoListExW(flags, list, size, required, NULL, NULL);
}
