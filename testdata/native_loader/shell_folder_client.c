#define _WIN32_WINNT 0x0601
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <propsys.h>
#include <propvarutil.h>

int raven_entry(void)
{
	WCHAR directory[MAX_PATH], file[MAX_PATH], parsed[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, directory))
		return 1;
	lstrcpyW(file, directory);
	lstrcatW(file, L"raven-folder-test.txt");
	HANDLE handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return 2;
	CloseHandle(handle);

	IShellFolder *desktop = NULL;
	if (SHGetDesktopFolder(&desktop) != S_OK || !desktop)
		return 3;
	PIDLIST_RELATIVE folder_list = NULL;
	ULONG attributes = SFGAO_FOLDER | SFGAO_FILESYSTEM;
	if (IShellFolder_ParseDisplayName(desktop, NULL, NULL, directory, NULL, &folder_list, &attributes) != S_OK)
		return 4;
	if (attributes != (SFGAO_FOLDER | SFGAO_FILESYSTEM))
		return 5;
	WCHAR missing[] = L"C:\\raven-no-such-folder";
	PIDLIST_RELATIVE none = NULL;
	if (IShellFolder_ParseDisplayName(desktop, NULL, NULL, missing, NULL, &none, NULL) != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || none)
		return 6;

	IShellFolder *folder = NULL;
	if (IShellFolder_BindToObject(desktop, folder_list, NULL, &IID_IShellFolder, (void **)&folder) != S_OK)
		return 7;
	WCHAR name[] = L"raven-folder-test.txt";
	PIDLIST_RELATIVE child = NULL;
	if (IShellFolder_ParseDisplayName(folder, NULL, NULL, name, NULL, &child, NULL) != S_OK)
		return 8;
	if (ILFindLastID(child) != (PUITEMID_CHILD)child || ILGetNext(child))
		return 9;

	// The joined list walks as two items, and its last one is the child.
	PIDLIST_ABSOLUTE joined = ILCombine((PCIDLIST_ABSOLUTE)folder_list, child);
	if (!joined || ILGetSize((PCUIDLIST_RELATIVE)joined) != ILGetSize(folder_list) + ILGetSize(child) - sizeof(USHORT))
		return 10;
	PUITEMID_CHILD last = ILFindLastID((PCUIDLIST_RELATIVE)joined);
	if (last == (PUITEMID_CHILD)joined || last->mkid.cb != child->mkid.cb)
		return 11;
	if (!SHGetPathFromIDListW((PCIDLIST_ABSOLUTE)joined, parsed) || lstrcmpiW(parsed, file))
		return 12;

	STRRET text;
	if (IShellFolder_GetDisplayNameOf(folder, (PCUITEMID_CHILD)child, SHGDN_FORPARSING, &text) != S_OK || text.uType != STRRET_WSTR || lstrcmpiW(text.pOleStr, file))
		return 13;
	CoTaskMemFree(text.pOleStr);
	if (IShellFolder_GetDisplayNameOf(folder, (PCUITEMID_CHILD)child, SHGDN_INFOLDER, &text) != S_OK || lstrcmpiW(text.pOleStr, name))
		return 14;
	CoTaskMemFree(text.pOleStr);

	SFGAOF kinds = SFGAO_FOLDER | SFGAO_STREAM;
	PCUITEMID_CHILD one = (PCUITEMID_CHILD)child;
	if (IShellFolder_GetAttributesOf(folder, 1, &one, &kinds) != S_OK || kinds != SFGAO_STREAM)
		return 15;

	// The file comes back from the walk, and only when non-folders are asked for.
	IEnumIDList *walk = NULL;
	if (IShellFolder_EnumObjects(folder, NULL, SHCONTF_NONFOLDERS, &walk) != S_OK)
		return 16;
	BOOL seen = FALSE;
	PITEMID_CHILD item;
	ULONG fetched;
	while (IEnumIDList_Next(walk, 1, &item, &fetched) == S_OK && fetched) {
		if (IShellFolder_CompareIDs(folder, 0, (PCUIDLIST_RELATIVE)item, child) == S_OK)
			seen = TRUE;
		ILFree((PIDLIST_RELATIVE)item);
	}
	IEnumIDList_Release(walk);
	if (!seen)
		return 17;
	if (IShellFolder_EnumObjects(folder, NULL, SHCONTF_FOLDERS, &walk) != S_OK)
		return 18;
	seen = FALSE;
	while (IEnumIDList_Next(walk, 1, &item, &fetched) == S_OK && fetched) {
		if (IShellFolder_CompareIDs(folder, 0, (PCUIDLIST_RELATIVE)item, child) == S_OK)
			seen = TRUE;
		ILFree((PIDLIST_RELATIVE)item);
	}
	IEnumIDList_Release(walk);
	if (seen)
		return 19;

	// The file system's own properties of the file; asking to write them is refused.
	static const PROPERTYKEY name_key = {{0xb725f130, 0x47ef, 0x101a, {0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac}}, 10};
	static const PROPERTYKEY unknown_key = {{0xb725f130, 0x47ef, 0x101a, {0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac}}, 99};
	IPropertyStore *store = NULL;
	PROPVARIANT value;
	if (SHGetPropertyStoreFromParsingName(file, NULL, GPS_READWRITE, &IID_IPropertyStore, (void **)&store) != STG_E_ACCESSDENIED || store)
		return 22;
	if (SHGetPropertyStoreFromParsingName(file, NULL, GPS_DEFAULT, &IID_IPropertyStore, (void **)&store) != S_OK)
		return 23;
	if (IPropertyStore_GetValue(store, &name_key, &value) != S_OK || value.vt != VT_LPWSTR || lstrcmpiW(value.pwszVal, name))
		return 24;
	CoTaskMemFree(value.pwszVal);
	if (IPropertyStore_GetValue(store, &unknown_key, &value) != S_OK || value.vt != VT_EMPTY)
		return 25;
	IPropertyStore_Release(store);
	if (SHGetPropertyStoreFromParsingName(missing, NULL, GPS_DEFAULT, &IID_IPropertyStore, (void **)&store) == S_OK)
		return 26;
	WCHAR short_buffer[4];
	if (!SHGetPathFromIDListEx((PCIDLIST_ABSOLUTE)joined, parsed, MAX_PATH, GPFIDL_DEFAULT) || lstrcmpiW(parsed, file))
		return 27;
	if (SHGetPathFromIDListEx((PCIDLIST_ABSOLUTE)joined, short_buffer, 4, GPFIDL_DEFAULT))
		return 28;

	PIDLIST_RELATIVE clone = ILClone((PCUIDLIST_RELATIVE)joined);
	if (!clone || !ILIsEqual((PCIDLIST_ABSOLUTE)clone, joined))
		return 20;
	PITEMID_CHILD first = ILCloneFirst((PCUIDLIST_RELATIVE)joined);
	if (!first || ILIsEqual((PCIDLIST_ABSOLUTE)first, joined) || first->mkid.cb != folder_list->mkid.cb)
		return 21;

	ILFree((PIDLIST_RELATIVE)first);
	ILFree(clone);
	ILFree((PIDLIST_RELATIVE)joined);
	ILFree(child);
	ILFree(folder_list);
	IShellFolder_Release(folder);
	IShellFolder_Release(desktop);
	DeleteFileW(file);
	return 0;
}
