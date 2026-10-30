/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <msi.h>

/* Products streamed by Click-to-Run are not registered with the installer. Each one describes itself in a manifest under the package
 * folder: its product code, its components with their key paths and its features. The installer's queries fall back to those
 * manifests, so a product asking about its own components finds them installed where the package keeps them. */

#define GUID_LENGTH 38

struct manifest
{
	struct manifest *next;
	WCHAR *text;
};

static struct manifest none;
static struct manifest *manifests;
static WCHAR root[MAX_PATH];

static WCHAR *read_manifest(const WCHAR *path)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER size;
	WCHAR *text = NULL;
	DWORD read;

	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	if (GetFileSizeEx(file, &size) && size.QuadPart < 64 << 20 && (text = HeapAlloc(GetProcessHeap(), 0, size.QuadPart + sizeof(WCHAR))))
	{
		if (ReadFile(file, text, size.LowPart, &read, NULL) && read == size.LowPart)
			text[read / sizeof(WCHAR)] = 0;
		else
		{
			HeapFree(GetProcessHeap(), 0, text);
			text = NULL;
		}
	}
	CloseHandle(file);
	return text;
}

/* The manifests are read once, by whichever thread asks first. */
static struct manifest *load(void)
{
	struct manifest *list = &none, *entry;
	WCHAR folder[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
	DWORD size = sizeof(folder) - sizeof(WCHAR), type;
	WIN32_FIND_DATAW data;
	HANDLE find;
	HKEY key;
	LONG result;

	if (manifests)
		return manifests;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Office\\ClickToRun", 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key))
		result = ERROR_FILE_NOT_FOUND;
	else
	{
		result = RegQueryValueExW(key, L"PackageFolder", NULL, &type, (BYTE *)folder, &size);
		RegCloseKey(key);
		folder[size / sizeof(WCHAR)] = 0;
	}
	if (!result && type == REG_SZ && lstrlenW(folder) < MAX_PATH - 64)
	{
		lstrcpyW(root, folder);
		lstrcatW(root, L"\\root\\");
		lstrcpyW(pattern, root);
		lstrcatW(pattern, L"Integration\\C2RManifest.*.xml");
		if ((find = FindFirstFileW(pattern, &data)) != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (lstrlenW(root) + lstrlenW(data.cFileName) >= MAX_PATH - 16)
					continue;
				lstrcpyW(path, root);
				lstrcatW(path, L"Integration\\");
				lstrcatW(path, data.cFileName);
				if (!(entry = HeapAlloc(GetProcessHeap(), 0, sizeof(*entry))))
					break;
				if (!(entry->text = read_manifest(path)))
				{
					HeapFree(GetProcessHeap(), 0, entry);
					continue;
				}
				entry->next = list;
				list = entry;
			} while (FindNextFileW(find, &data));
			FindClose(find);
		}
	}
	if (InterlockedCompareExchangePointer((void **)&manifests, list, NULL))
	{
		while (list != &none)
		{
			entry = list->next;
			HeapFree(GetProcessHeap(), 0, list->text);
			HeapFree(GetProcessHeap(), 0, list);
			list = entry;
		}
	}
	return manifests;
}

/* The value of the attribute that follows a match of prefix, which ends at its opening quote. */
static const WCHAR *find_attribute(const WCHAR *text, const WCHAR *prefix, const WCHAR *value)
{
	const int length = lstrlenW(prefix), size = lstrlenW(value);

	for (const WCHAR *at = text; (at = wcsstr(at, prefix)); at += length)
		if (!_wcsnicmp(at + length, value, size) && at[length + size] == '"')
			return at + length;
	return NULL;
}

static BOOL has_product(const struct manifest *entry, const WCHAR *product)
{
	const WCHAR *code = wcsstr(entry->text, L"ProductCode=\"");

	return code && (!product || !_wcsnicmp(code + 13, product, GUID_LENGTH));
}

/* Where the package keeps the folder a key path variable names: its own folder for the program files of Microsoft Office, and
 * otherwise the virtual file system folders under its root, the first of them that holds the file. */
static const struct
{
	const WCHAR *name;
	BOOL program_files;
	const WCHAR *folders[2];
}
variables[] =
{
	{L"%SFT_PROGRAM_FILES_X64%", TRUE, {L"ProgramFilesX64", L"ProgramFilesX86"}},
	{L"%CSIDL_PROGRAM_FILES%", TRUE, {L"ProgramFilesX64", L"ProgramFilesX86"}},
	{L"%SFT_PROGRAM_FILES_X86%", TRUE, {L"ProgramFilesX86"}},
	{L"%SFT_PROGRAM_FILES_COMMON_X64%", FALSE, {L"ProgramFilesCommonX64", L"ProgramFilesCommonX86"}},
	{L"%CSIDL_PROGRAM_FILES_COMMON%", FALSE, {L"ProgramFilesCommonX64", L"ProgramFilesCommonX86"}},
	{L"%SFT_PROGRAM_FILES_COMMON_X86%", FALSE, {L"ProgramFilesCommonX86"}},
	{L"%SFT_SYSTEM32_X64%", FALSE, {L"System"}},
	{L"%SFT_SYSTEM32_X86%", FALSE, {L"SystemX86"}},
	{L"%CSIDL_FONTS%", FALSE, {L"Fonts"}},
	{L"%CSIDL_WINDOWS%", FALSE, {L"Windows"}},
	{L"%CSIDL_COMMON_APPDATA%", FALSE, {L"Common AppData"}},
};

/* An XML attribute value of the given length as text in a MAX_PATH buffer. */
static void decode(const WCHAR *value, int length, WCHAR *text)
{
	static const struct { const WCHAR *entity; WCHAR value; } entities[] = {{L"&lt;", '<'}, {L"&gt;", '>'}, {L"&quot;", '"'}, {L"&amp;", '&'}};
	int out = 0;

	for (int index = 0; index < length && out < MAX_PATH - 1; ++index)
	{
		WCHAR character = value[index];

		for (int entity = 0; entity < ARRAY_SIZE(entities); ++entity)
		{
			const int size = lstrlenW(entities[entity].entity);
			if (index + size <= length && !wcsncmp(value + index, entities[entity].entity, size))
			{
				character = entities[entity].value;
				index += size - 1;
				break;
			}
		}
		text[out++] = character;
	}
	text[out] = 0;
}

/* The value of a named attribute inside the element that starts at tag, decoded into a MAX_PATH buffer. */
static BOOL tag_attribute(const WCHAR *tag, const WCHAR *name, WCHAR *text)
{
	const WCHAR *end = wcschr(tag, '>'), *at = wcsstr(tag, name), *close;

	if (!end || !at || at > end || !(close = wcschr(at += lstrlenW(name), '"')) || close > end)
		return FALSE;
	decode(at, close - at, text);
	return TRUE;
}

static void expand(const WCHAR *key_path, int length, WCHAR *path)
{
	WCHAR text[MAX_PATH];

	decode(key_path, length, text);
	lstrcpyW(path, text);
	for (int index = 0; index < ARRAY_SIZE(variables); ++index)
	{
		const int size = lstrlenW(variables[index].name);
		const WCHAR *rest = text + size;

		if (_wcsnicmp(text, variables[index].name, size) || *rest != '\\')
			continue;
		++rest;
		if (variables[index].program_files && !_wcsnicmp(rest, L"Microsoft Office\\", 17))
		{
			if (lstrlenW(root) + lstrlenW(rest + 17) < MAX_PATH)
			{
				lstrcpyW(path, root);
				lstrcatW(path, rest + 17);
			}
			return;
		}
		for (int pass = 0; pass < 2; ++pass)
			for (int folder = 0; folder < ARRAY_SIZE(variables[index].folders); ++folder)
			{
				const WCHAR *name = variables[index].folders[folder];

				if (!name || lstrlenW(root) + lstrlenW(name) + lstrlenW(rest) + 6 >= MAX_PATH)
					continue;
				lstrcpyW(path, root);
				lstrcatW(path, L"vfs\\");
				lstrcatW(path, name);
				lstrcatW(path, L"\\");
				lstrcatW(path, rest);
				/* The second pass settles on the first folder when none holds the file. */
				if (pass || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
					return;
			}
		return;
	}
}

/* The key path of a component a manifest installs, or NULL. Features name their components too, without one. */
static const WCHAR *key_path_of(const struct manifest *entry, const WCHAR *component)
{
	for (const WCHAR *found = entry->text; (found = find_attribute(found, L"<Component ComponentId=\"", component)); )
		if (!wcsncmp(found += GUID_LENGTH + 1, L" KeyPath=\"", 10) && wcschr(found + 10, '"'))
			return found + 10;
	return NULL;
}

/* The product a component belongs to, the index-th when several install it, and, when path is given, its key path as a MAX_PATH
 * buffer. A product names the one to look in. */
BOOL raven_clicktorun_component(const WCHAR *component, const WCHAR *product, DWORD index, WCHAR *product_code, WCHAR *path)
{
	if (!component || lstrlenW(component) != GUID_LENGTH)
		return FALSE;
	for (const struct manifest *entry = load(); entry != &none; entry = entry->next)
	{
		const WCHAR *key_path = key_path_of(entry, component), *code = wcsstr(entry->text, L"ProductCode=\"");
		BOOL listed = FALSE;

		if (!key_path || !has_product(entry, product))
			continue;
		/* A product counts once, however many of its manifests name the component. */
		for (const struct manifest *earlier = load(); earlier != entry && !listed; earlier = earlier->next)
			listed = key_path_of(earlier, component) && has_product(earlier, code + 13);
		if (listed || index--)
			continue;
		if (product_code)
			lstrcpynW(product_code, code + 13, GUID_LENGTH + 1);
		if (path)
			expand(key_path, wcschr(key_path, '"') - key_path, path);
		return TRUE;
	}
	return FALSE;
}

/* The index-th qualifier published in a category and its application data, each in a MAX_PATH buffer. */
BOOL raven_clicktorun_qualifier(const WCHAR *category, DWORD index, WCHAR *qualifier, WCHAR *data)
{
	if (!category || lstrlenW(category) != GUID_LENGTH)
		return FALSE;
	for (const struct manifest *entry = load(); entry != &none; entry = entry->next)
		for (const WCHAR *at = entry->text; (at = find_attribute(at, L"<PublishComponent PublishComponentId=\"", category)); ++at)
		{
			WCHAR name[MAX_PATH], other[MAX_PATH];
			BOOL listed = FALSE;

			if (!tag_attribute(at, L"Qualifier=\"", name))
				continue;
			/* A qualifier counts once, however many manifests publish it. */
			for (const struct manifest *earlier = load(); earlier != entry->next && !listed; earlier = earlier->next)
				for (const WCHAR *seen = earlier->text; !listed && (seen = find_attribute(seen, L"<PublishComponent PublishComponentId=\"", category)) &&
						(earlier != entry || seen < at); ++seen)
					listed = tag_attribute(seen, L"Qualifier=\"", other) && !lstrcmpiW(other, name);
			if (listed || index--)
				continue;
			lstrcpyW(qualifier, name);
			if (!tag_attribute(at, L"AppData=\"", data))
				data[0] = 0;
			return TRUE;
		}
	return FALSE;
}

/* The component a manifest publishes a qualifier of a category through, as a GUID_LENGTH + 1 buffer. A component that holds its
 * publications names them without repeating itself, so the enclosing component tag stands for it there. */
BOOL raven_clicktorun_published(const WCHAR *category, const WCHAR *qualifier, WCHAR *component)
{
	if (!category || !qualifier || lstrlenW(category) != GUID_LENGTH)
		return FALSE;
	for (const struct manifest *entry = load(); entry != &none; entry = entry->next)
		for (const WCHAR *at = entry->text; (at = find_attribute(at, L"<PublishComponent PublishComponentId=\"", category)); ++at)
		{
			WCHAR name[MAX_PATH], owner[MAX_PATH];
			const WCHAR *outer;

			if (!tag_attribute(at, L"Qualifier=\"", name) || lstrcmpiW(name, qualifier))
				continue;
			if (tag_attribute(at, L" ComponentId=\"", owner) && lstrlenW(owner) == GUID_LENGTH)
			{
				lstrcpyW(component, owner);
				return TRUE;
			}
			for (outer = at; outer > entry->text && wcsncmp(outer, L"<Component ComponentId=\"", 24); --outer)
				;
			if (outer > entry->text && wcschr(outer + 24, '"') == outer + 24 + GUID_LENGTH)
			{
				lstrcpynW(component, outer + 24, GUID_LENGTH + 1);
				return TRUE;
			}
		}
	return FALSE;
}

BOOL raven_clicktorun_product(const WCHAR *product)
{
	if (!product || lstrlenW(product) != GUID_LENGTH)
		return FALSE;
	for (const struct manifest *entry = load(); entry != &none; entry = entry->next)
		if (has_product(entry, product))
			return TRUE;
	return FALSE;
}

/* Every feature a manifest lists is part of what the package streams. */
BOOL raven_clicktorun_feature(const WCHAR *product, const WCHAR *feature)
{
	if (!product || !feature || lstrlenW(product) != GUID_LENGTH)
		return FALSE;
	for (const struct manifest *entry = load(); entry != &none; entry = entry->next)
		if (has_product(entry, product) && find_attribute(entry->text, L"<Feature FeatureId=\"", feature))
			return TRUE;
	return FALSE;
}
