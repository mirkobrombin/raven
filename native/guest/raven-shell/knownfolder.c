/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: the known folder manager, CLSID_KnownFolderManager.
 *
 * A program asks it for the folders it knows, by id, by canonical name, by CSIDL or by a path inside one, and gets an IKnownFolder
 * for each. The ids, canonical names, categories and CSIDLs are the ones the KNOWNFOLDERID reference publishes; the locations come
 * from SHGetKnownFolderPath, which places every folder in the guest's profile and drive. Folders are fixed: none is registered,
 * removed or redirected. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>

HRESULT raven_make_known_folder_manager(REFIID iid, void **out);
HRESULT raven_item_from_path(const WCHAR *path, REFIID iid, void **out);

typedef struct {
	const KNOWNFOLDERID *id;
	const WCHAR *name;
	KF_CATEGORY category;
	int csidl;
} folder_entry;

static const folder_entry folders[] = {
	{&FOLDERID_Desktop, L"Desktop", KF_CATEGORY_PERUSER, CSIDL_DESKTOPDIRECTORY},
	{&FOLDERID_Documents, L"Personal", KF_CATEGORY_PERUSER, CSIDL_PERSONAL},
	{&FOLDERID_Downloads, L"Downloads", KF_CATEGORY_PERUSER, -1},
	{&FOLDERID_Music, L"My Music", KF_CATEGORY_PERUSER, CSIDL_MYMUSIC},
	{&FOLDERID_Pictures, L"My Pictures", KF_CATEGORY_PERUSER, CSIDL_MYPICTURES},
	{&FOLDERID_Videos, L"My Video", KF_CATEGORY_PERUSER, CSIDL_MYVIDEO},
	{&FOLDERID_Favorites, L"Favorites", KF_CATEGORY_PERUSER, CSIDL_FAVORITES},
	{&FOLDERID_LocalAppData, L"Local AppData", KF_CATEGORY_PERUSER, CSIDL_LOCAL_APPDATA},
	{&FOLDERID_LocalAppDataLow, L"LocalAppDataLow", KF_CATEGORY_PERUSER, -1},
	{&FOLDERID_RoamingAppData, L"AppData", KF_CATEGORY_PERUSER, CSIDL_APPDATA},
	{&FOLDERID_Profile, L"Profile", KF_CATEGORY_FIXED, CSIDL_PROFILE},
	{&FOLDERID_SavedGames, L"SavedGames", KF_CATEGORY_PERUSER, -1},
	{&FOLDERID_Programs, L"Programs", KF_CATEGORY_PERUSER, CSIDL_PROGRAMS},
	{&FOLDERID_StartMenu, L"Start Menu", KF_CATEGORY_PERUSER, CSIDL_STARTMENU},
	{&FOLDERID_Startup, L"Startup", KF_CATEGORY_PERUSER, CSIDL_STARTUP},
	{&FOLDERID_InternetCache, L"Cache", KF_CATEGORY_PERUSER, CSIDL_INTERNET_CACHE},
	{&FOLDERID_Cookies, L"Cookies", KF_CATEGORY_PERUSER, CSIDL_COOKIES},
	{&FOLDERID_History, L"History", KF_CATEGORY_PERUSER, CSIDL_HISTORY},
	{&FOLDERID_ProgramData, L"Common AppData", KF_CATEGORY_FIXED, CSIDL_COMMON_APPDATA},
	{&FOLDERID_CommonStartMenu, L"Common Start Menu", KF_CATEGORY_COMMON, CSIDL_COMMON_STARTMENU},
	{&FOLDERID_CommonPrograms, L"Common Programs", KF_CATEGORY_COMMON, CSIDL_COMMON_PROGRAMS},
	{&FOLDERID_Public, L"Public", KF_CATEGORY_FIXED, -1},
	{&FOLDERID_UserProfiles, L"UserProfiles", KF_CATEGORY_FIXED, -1},
	{&FOLDERID_Windows, L"Windows", KF_CATEGORY_FIXED, CSIDL_WINDOWS},
	{&FOLDERID_System, L"System", KF_CATEGORY_FIXED, CSIDL_SYSTEM},
	{&FOLDERID_SystemX86, L"SystemX86", KF_CATEGORY_FIXED, CSIDL_SYSTEMX86},
	{&FOLDERID_Fonts, L"Fonts", KF_CATEGORY_FIXED, CSIDL_FONTS},
	{&FOLDERID_ProgramFiles, L"ProgramFiles", KF_CATEGORY_FIXED, CSIDL_PROGRAM_FILES},
	{&FOLDERID_ProgramFilesX64, L"ProgramFilesX64", KF_CATEGORY_FIXED, -1},
	{&FOLDERID_ProgramFilesX86, L"ProgramFilesX86", KF_CATEGORY_FIXED, CSIDL_PROGRAM_FILESX86},
	{&FOLDERID_ProgramFilesCommon, L"ProgramFilesCommon", KF_CATEGORY_FIXED, CSIDL_PROGRAM_FILES_COMMON},
	{&FOLDERID_ProgramFilesCommonX64, L"ProgramFilesCommonX64", KF_CATEGORY_FIXED, -1},
	{&FOLDERID_ProgramFilesCommonX86, L"ProgramFilesCommonX86", KF_CATEGORY_FIXED, CSIDL_PROGRAM_FILES_COMMONX86},
};
#define FOLDER_COUNT (sizeof folders / sizeof folders[0])

static BOOL same_id(REFGUID a, REFGUID b)
{
	const DWORD *left = (const DWORD *)a, *right = (const DWORD *)b;
	return left[0] == right[0] && left[1] == right[1] && left[2] == right[2] && left[3] == right[3];
}

static WCHAR *copy_text(const WCHAR *text)
{
	const SIZE_T size = (lstrlenW(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = CoTaskMemAlloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

/* The location of a folder, in task memory the caller frees, from the shell's own SHGetKnownFolderPath. */
static HRESULT folder_path(const folder_entry *entry, DWORD flags, WCHAR **out)
{
	static HRESULT (WINAPI *known_path)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *);
	if (!known_path) {
		HMODULE shell = LoadLibraryW(L"shell32.dll");
		known_path = shell ? (void *)GetProcAddress(shell, "SHGetKnownFolderPath") : NULL;
		if (!known_path)
			return E_FAIL;
	}
	return known_path(entry->id, flags, NULL, out);
}

/* Known folder ---------------------------------------------------------------------------------------------- */

typedef struct {
	IKnownFolder face;
	LONG refs;
	const folder_entry *entry;
} KnownFolder;

static KnownFolder *folder_of(IKnownFolder *self) { return (KnownFolder *)self; }

static HRESULT STDMETHODCALLTYPE kf_query(IKnownFolder *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_id(iid, &IID_IUnknown) || same_id(iid, &IID_IKnownFolder)) {
		*out = self;
		InterlockedIncrement(&folder_of(self)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE kf_add_ref(IKnownFolder *self) { return InterlockedIncrement(&folder_of(self)->refs); }
static ULONG STDMETHODCALLTYPE kf_release(IKnownFolder *self)
{
	const LONG refs = InterlockedDecrement(&folder_of(self)->refs);
	if (!refs)
		HeapFree(GetProcessHeap(), 0, self);
	return refs;
}
static HRESULT STDMETHODCALLTYPE kf_get_id(IKnownFolder *self, KNOWNFOLDERID *id)
{
	if (!id)
		return E_INVALIDARG;
	*id = *folder_of(self)->entry->id;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE kf_get_category(IKnownFolder *self, KF_CATEGORY *category)
{
	if (!category)
		return E_INVALIDARG;
	*category = folder_of(self)->entry->category;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE kf_get_shell_item(IKnownFolder *self, DWORD flags, REFIID iid, void **out)
{
	WCHAR *path;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	HRESULT result = folder_path(folder_of(self)->entry, flags, &path);
	if (FAILED(result))
		return result;
	result = raven_item_from_path(path, iid, out);
	CoTaskMemFree(path);
	return result;
}
static HRESULT STDMETHODCALLTYPE kf_get_path(IKnownFolder *self, DWORD flags, LPWSTR *path)
{
	if (!path)
		return E_INVALIDARG;
	*path = NULL;
	return folder_path(folder_of(self)->entry, flags, path);
}
static HRESULT STDMETHODCALLTYPE kf_set_path(IKnownFolder *self, DWORD flags, LPCWSTR path) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE kf_get_id_list(IKnownFolder *self, DWORD flags, PIDLIST_ABSOLUTE *list)
{
	if (list)
		*list = NULL;
	return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE kf_get_folder_type(IKnownFolder *self, FOLDERTYPEID *type)
{
	if (!type)
		return E_INVALIDARG;
	memset(type, 0, sizeof *type);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE kf_get_redirection(IKnownFolder *self, KF_REDIRECTION_CAPABILITIES *capabilities)
{
	if (!capabilities)
		return E_INVALIDARG;
	*capabilities = KF_REDIRECTION_CAPABILITIES_DENY_ALL;
	return S_OK;
}
/* The definition carries the category and canonical name; the other strings stay empty, as FreeKnownFolderDefinitionFields allows. */
static HRESULT STDMETHODCALLTYPE kf_get_definition(IKnownFolder *self, KNOWNFOLDER_DEFINITION *definition)
{
	if (!definition)
		return E_INVALIDARG;
	memset(definition, 0, sizeof *definition);
	definition->category = folder_of(self)->entry->category;
	definition->pszName = copy_text(folder_of(self)->entry->name);
	return definition->pszName ? S_OK : E_OUTOFMEMORY;
}

static IKnownFolderVtbl folder_vtbl = {kf_query, kf_add_ref, kf_release, kf_get_id, kf_get_category, kf_get_shell_item, kf_get_path,
	kf_set_path, kf_get_id_list, kf_get_folder_type, kf_get_redirection, kf_get_definition};

static HRESULT make_folder(const folder_entry *entry, IKnownFolder **out)
{
	KnownFolder *folder = HeapAlloc(GetProcessHeap(), 0, sizeof *folder);
	if (!folder)
		return E_OUTOFMEMORY;
	folder->face.lpVtbl = &folder_vtbl;
	folder->refs = 1;
	folder->entry = entry;
	*out = &folder->face;
	return S_OK;
}

/* Known folder manager -------------------------------------------------------------------------------------- */

typedef struct {
	IKnownFolderManager face;
	LONG refs;
} FolderManager;

static HRESULT STDMETHODCALLTYPE km_query(IKnownFolderManager *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_id(iid, &IID_IUnknown) || same_id(iid, &IID_IKnownFolderManager)) {
		*out = self;
		InterlockedIncrement(&((FolderManager *)self)->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE km_add_ref(IKnownFolderManager *self) { return InterlockedIncrement(&((FolderManager *)self)->refs); }
static ULONG STDMETHODCALLTYPE km_release(IKnownFolderManager *self)
{
	const LONG refs = InterlockedDecrement(&((FolderManager *)self)->refs);
	if (!refs)
		HeapFree(GetProcessHeap(), 0, self);
	return refs;
}
static HRESULT STDMETHODCALLTYPE km_from_csidl(IKnownFolderManager *self, int csidl, KNOWNFOLDERID *id)
{
	if (!id)
		return E_INVALIDARG;
	for (UINT index = 0; index < FOLDER_COUNT; ++index)
		if (folders[index].csidl >= 0 && folders[index].csidl == (csidl & 0xff)) {
			*id = *folders[index].id;
			return S_OK;
		}
	return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
static HRESULT STDMETHODCALLTYPE km_to_csidl(IKnownFolderManager *self, REFKNOWNFOLDERID id, int *csidl)
{
	if (!csidl)
		return E_INVALIDARG;
	for (UINT index = 0; index < FOLDER_COUNT; ++index)
		if (same_id(folders[index].id, id) && folders[index].csidl >= 0) {
			*csidl = folders[index].csidl;
			return S_OK;
		}
	return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
static HRESULT STDMETHODCALLTYPE km_get_ids(IKnownFolderManager *self, KNOWNFOLDERID **ids, UINT *count)
{
	if (!ids || !count)
		return E_INVALIDARG;
	*ids = CoTaskMemAlloc(FOLDER_COUNT * sizeof(KNOWNFOLDERID));
	if (!*ids)
		return E_OUTOFMEMORY;
	for (UINT index = 0; index < FOLDER_COUNT; ++index)
		(*ids)[index] = *folders[index].id;
	*count = FOLDER_COUNT;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE km_get_folder(IKnownFolderManager *self, REFKNOWNFOLDERID id, IKnownFolder **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	for (UINT index = 0; index < FOLDER_COUNT; ++index)
		if (same_id(folders[index].id, id))
			return make_folder(&folders[index], out);
	return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}
static HRESULT STDMETHODCALLTYPE km_get_by_name(IKnownFolderManager *self, LPCWSTR name, IKnownFolder **out)
{
	if (!out || !name)
		return E_INVALIDARG;
	*out = NULL;
	for (UINT index = 0; index < FOLDER_COUNT; ++index)
		if (!lstrcmpiW(folders[index].name, name))
			return make_folder(&folders[index], out);
	return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}
static HRESULT STDMETHODCALLTYPE km_register(IKnownFolderManager *self, REFKNOWNFOLDERID id, const KNOWNFOLDER_DEFINITION *definition)
{
	return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE km_unregister(IKnownFolderManager *self, REFKNOWNFOLDERID id) { return E_NOTIMPL; }
/* The folder a path names, or with FFFP_NEARESTPARENTMATCH the deepest folder that contains it. */
static HRESULT STDMETHODCALLTYPE km_from_path(IKnownFolderManager *self, LPCWSTR path, FFFP_MODE mode, IKnownFolder **out)
{
	const folder_entry *best = NULL;
	int best_length = 0;
	if (!out || !path)
		return E_INVALIDARG;
	*out = NULL;
	const int length = lstrlenW(path);
	for (UINT index = 0; index < FOLDER_COUNT; ++index) {
		WCHAR *location;
		if (FAILED(folder_path(&folders[index], 0, &location)))
			continue;
		const int size = lstrlenW(location);
		const BOOL inside = size <= length && CompareStringOrdinal(location, size, path, size, TRUE) == CSTR_EQUAL &&
			(size == length || path[size] == L'\\');
		CoTaskMemFree(location);
		if (!inside || (size != length && mode != FFFP_NEARESTPARENTMATCH) || size <= best_length)
			continue;
		best = &folders[index];
		best_length = size;
	}
	return best ? make_folder(best, out) : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}
static HRESULT STDMETHODCALLTYPE km_from_id_list(IKnownFolderManager *self, PCIDLIST_ABSOLUTE list, IKnownFolder **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE km_redirect(IKnownFolderManager *self, REFKNOWNFOLDERID id, HWND window, KF_REDIRECT_FLAGS flags,
	LPCWSTR target, UINT count, const KNOWNFOLDERID *exclusions, LPWSTR *error)
{
	if (error)
		*error = NULL;
	return E_NOTIMPL;
}

static IKnownFolderManagerVtbl manager_vtbl = {km_query, km_add_ref, km_release, km_from_csidl, km_to_csidl, km_get_ids, km_get_folder,
	km_get_by_name, km_register, km_unregister, km_from_path, km_from_id_list, km_redirect};

HRESULT raven_make_known_folder_manager(REFIID iid, void **out)
{
	FolderManager *manager = HeapAlloc(GetProcessHeap(), 0, sizeof *manager);
	if (!manager)
		return E_OUTOFMEMORY;
	manager->face.lpVtbl = &manager_vtbl;
	manager->refs = 1;
	const HRESULT result = km_query(&manager->face, iid, out);
	km_release(&manager->face);
	return result;
}
