/* SPDX-License-Identifier: MIT */
#include "msi_own.h"
#include <softpub.h>
#include <shlwapi.h>

/* The rest of the API: ANSI twins, questions answered from what product.c records, and what this engine does not offer,
 * each answering with the documented status for it rather than pretending. */

// Answers an ANSI caller from a wide string function that takes (..., out, size).
static UINT narrow_answer(const WCHAR *text, UINT status, char *out, DWORD *size)
{
	if (status && status != ERROR_MORE_DATA)
		return status;
	return out_text_a(text, lstrlenW(text), out, size);
}

#define WIDE(name) WCHAR *w_##name = name ? msi_from_ansi(name) : NULL
#define DROP(name) msi_free(w_##name)
#define ROOM 4096

INSTALLSTATE WINAPI MsiQueryProductStateA(LPCSTR product)
{
	WIDE(product);
	INSTALLSTATE state = MsiQueryProductStateW(w_product);
	DROP(product);
	return state;
}

INSTALLSTATE WINAPI MsiQueryFeatureStateA(LPCSTR product, LPCSTR feature)
{
	WIDE(product);
	WIDE(feature);
	INSTALLSTATE state = MsiQueryFeatureStateW(w_product, w_feature);
	DROP(product);
	DROP(feature);
	return state;
}

UINT WINAPI MsiInstallProductA(LPCSTR path, LPCSTR line)
{
	WIDE(path);
	WIDE(line);
	UINT status = MsiInstallProductW(w_path, w_line);
	DROP(path);
	DROP(line);
	return status;
}

UINT WINAPI MsiConfigureProductExA(LPCSTR product, int level, INSTALLSTATE state, LPCSTR line)
{
	WIDE(product);
	WIDE(line);
	UINT status = MsiConfigureProductExW(w_product, level, state, w_line);
	DROP(product);
	DROP(line);
	return status;
}

UINT WINAPI MsiConfigureProductA(LPCSTR product, int level, INSTALLSTATE state)
{
	return MsiConfigureProductExA(product, level, state, NULL);
}

UINT WINAPI MsiReinstallProductA(LPCSTR product, DWORD mode)
{
	WIDE(product);
	UINT status = MsiReinstallProductW(w_product, mode);
	DROP(product);
	return status;
}

UINT WINAPI MsiOpenProductA(LPCSTR product, MSIHANDLE *handle)
{
	WIDE(product);
	UINT status = MsiOpenProductW(w_product, handle);
	DROP(product);
	return status;
}

UINT WINAPI MsiGetProductCodeA(LPCSTR component, LPSTR out)
{
	WIDE(component);
	WCHAR code[39];
	UINT status = MsiGetProductCodeW(w_component, code);
	DROP(component);
	if (!status)
		WideCharToMultiByte(CP_ACP, 0, code, -1, out, 39, NULL, NULL);
	return status;
}

UINT WINAPI MsiEnumProductsA(DWORD index, LPSTR out)
{
	WCHAR code[39];
	UINT status = MsiEnumProductsW(index, code);
	if (!status)
		WideCharToMultiByte(CP_ACP, 0, code, -1, out, 39, NULL, NULL);
	return status;
}

UINT WINAPI MsiEnumClientsA(LPCSTR component, DWORD index, LPSTR out)
{
	WIDE(component);
	WCHAR code[39];
	UINT status = MsiEnumClientsW(w_component, index, code);
	DROP(component);
	if (!status)
		WideCharToMultiByte(CP_ACP, 0, code, -1, out, 39, NULL, NULL);
	return status;
}

UINT WINAPI MsiEnumRelatedProductsA(LPCSTR upgrade, DWORD reserved, DWORD index, LPSTR out)
{
	WIDE(upgrade);
	WCHAR code[39];
	UINT status = MsiEnumRelatedProductsW(w_upgrade, reserved, index, code);
	DROP(upgrade);
	if (!status)
		WideCharToMultiByte(CP_ACP, 0, code, -1, out, 39, NULL, NULL);
	return status;
}

UINT WINAPI MsiGetProductInfoA(LPCSTR product, LPCSTR attribute, LPSTR out, LPDWORD size)
{
	WIDE(product);
	WIDE(attribute);
	WCHAR text[ROOM];
	DWORD room = ROOM;
	UINT status = MsiGetProductInfoW(w_product, w_attribute, text, &room);
	DROP(product);
	DROP(attribute);
	return narrow_answer(text, status, out, size);
}

UINT WINAPI MsiGetProductInfoExA(LPCSTR product, LPCSTR user, MSIINSTALLCONTEXT context, LPCSTR attribute, LPSTR out, LPDWORD size)
{
	(void)user;
	(void)context;
	return MsiGetProductInfoA(product, attribute, out, size);
}

INSTALLSTATE WINAPI MsiGetComponentPathExA(LPCSTR product, LPCSTR component, LPCSTR user, MSIINSTALLCONTEXT context, LPSTR out, LPDWORD size)
{
	(void)user;
	WIDE(product);
	WIDE(component);
	WCHAR text[MAX_PATH * 2];
	DWORD room = ARRAYSIZE(text);
	INSTALLSTATE state = MsiGetComponentPathExW(w_product, w_component, NULL, context, text, &room);
	DROP(product);
	DROP(component);
	if (state == INSTALLSTATE_LOCAL || state == INSTALLSTATE_ABSENT)
		if (out_text_a(text, room, out, size) == ERROR_MORE_DATA)
			return INSTALLSTATE_MOREDATA;
	return state;
}

INSTALLSTATE WINAPI MsiGetComponentPathA(LPCSTR product, LPCSTR component, LPSTR out, LPDWORD size)
{
	return MsiGetComponentPathExA(product, component, NULL, MSIINSTALLCONTEXT_ALL, out, size);
}

INSTALLSTATE WINAPI MsiLocateComponentA(LPCSTR component, LPSTR out, LPDWORD size)
{
	char product[39];
	if (MsiGetProductCodeA(component, product))
		return INSTALLSTATE_UNKNOWN;
	return MsiGetComponentPathA(product, component, out, size);
}

UINT WINAPI MsiProvideQualifiedComponentExA(LPCSTR category, LPCSTR qualifier, DWORD mode, LPCSTR product, DWORD a, DWORD b, LPSTR out, LPDWORD size)
{
	WIDE(category);
	WIDE(qualifier);
	WIDE(product);
	WCHAR text[MAX_PATH * 2];
	DWORD room = ARRAYSIZE(text);
	UINT status = MsiProvideQualifiedComponentExW(w_category, w_qualifier, mode, w_product, a, b, text, &room);
	DROP(category);
	DROP(qualifier);
	DROP(product);
	return narrow_answer(text, status, out, size);
}

UINT WINAPI MsiProvideQualifiedComponentA(LPCSTR category, LPCSTR qualifier, DWORD mode, LPSTR out, LPDWORD size)
{
	return MsiProvideQualifiedComponentExA(category, qualifier, mode, NULL, 0, 0, out, size);
}

UINT WINAPI MsiEnumComponentQualifiersA(LPCSTR category, DWORD index, LPSTR qualifier, LPDWORD qualifier_size, LPSTR data, LPDWORD data_size)
{
	WIDE(category);
	WCHAR found[MAX_PATH], text[MAX_PATH];
	DWORD a = MAX_PATH, b = MAX_PATH;
	UINT status = MsiEnumComponentQualifiersW(w_category, index, found, &a, text, &b);
	DROP(category);
	if (status && status != ERROR_MORE_DATA)
		return status;
	status = out_text_a(found, a, qualifier, qualifier_size);
	UINT other = out_text_a(text, b, data, data_size);
	return other ? other : status;
}

UINT WINAPI MsiProvideComponentA(LPCSTR product, LPCSTR feature, LPCSTR component, DWORD mode, LPSTR out, LPDWORD size)
{
	WIDE(product);
	WIDE(feature);
	WIDE(component);
	WCHAR text[MAX_PATH * 2];
	DWORD room = ARRAYSIZE(text);
	UINT status = MsiProvideComponentW(w_product, w_feature, w_component, mode, text, &room);
	DROP(product);
	DROP(feature);
	DROP(component);
	return narrow_answer(text, status, out, size);
}

UINT WINAPI MsiGetFileVersionA(LPCSTR path, LPSTR version, LPDWORD version_size, LPSTR language, LPDWORD language_size)
{
	WIDE(path);
	WCHAR a[64], b[64];
	DWORD a_size = 64, b_size = 64;
	UINT status = MsiGetFileVersionW(w_path, a, &a_size, b, &b_size);
	DROP(path);
	if (status && status != ERROR_MORE_DATA)
		return status;
	if (version || version_size)
		status = out_text_a(a, a_size, version, version_size);
	if (language || language_size) {
		UINT other = out_text_a(b, b_size, language, language_size);
		status = status ? status : other;
	}
	return status;
}

// A descriptor is {product}feature>component, or the compressed form this engine never writes.
/* A descriptor packs each identifier into 20 characters: four little-endian words of five base 85 digits each. */
static BOOL unpack_guid(const WCHAR *text, WCHAR *out)
{
	static const char digits[] = "!$%&'()*+,-.0123456789=?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{}~";
	static const char hex[] = "0123456789ABCDEF";
	BYTE bytes[16];
	for (int word = 0; word < 4; ++word) {
		DWORD value = 0, scale = 1;
		for (int digit = 0; digit < 5; ++digit, scale *= 85) {
			const WCHAR c = text[word * 5 + digit];
			DWORD found = 85;
			for (DWORD index = 0; index < 85; ++index)
				if ((WCHAR)digits[index] == c)
					found = index;
			if (found == 85)
				return FALSE;
			value += found * scale;
		}
		for (int index = 0; index < 4; ++index)
			bytes[word * 4 + index] = (BYTE)(value >> (index * 8));
	}
	if (out) {
		static const BYTE order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
		WCHAR *at = out;
		*at++ = '{';
		for (int index = 0; index < 16; ++index) {
			if (index == 4 || index == 6 || index == 8 || index == 10)
				*at++ = '-';
			*at++ = hex[bytes[order[index]] >> 4];
			*at++ = hex[bytes[order[index]] & 15];
		}
		*at++ = '}';
		*at = 0;
	}
	return TRUE;
}

UINT WINAPI MsiDecomposeDescriptorW(LPCWSTR descriptor, LPWSTR product, LPWSTR feature, LPWSTR component, DWORD *used)
{
	if (!descriptor)
		return ERROR_INVALID_PARAMETER;
	const UINT length = lstrlenW(descriptor);
	const BOOL packed = descriptor[0] != '{';
	const UINT start = packed ? 20 : 38;
	if (length < start || (packed && !unpack_guid(descriptor, NULL)))
		return ERROR_INVALID_PARAMETER;
	const WCHAR *end = descriptor + start;
	while (*end && *end != '>' && *end != '<')
		++end;
	const UINT feature_length = (UINT)(end - descriptor) - start;
	if (feature_length > MAX_FEATURE_CHARS)
		return ERROR_INVALID_PARAMETER;
	const WCHAR *rest = *end == '>' ? end + 1 : NULL;
	UINT consumed = (UINT)(end - descriptor) + (*end == '<' ? 1 : 0);
	if (rest) {
		const UINT size = packed ? 20 : 38;
		if ((UINT)lstrlenW(rest) < size || (packed && !unpack_guid(rest, NULL)))
			return ERROR_INVALID_PARAMETER;
		consumed = (UINT)(rest - descriptor) + size;
	}
	if (product) {
		if (packed)
			unpack_guid(descriptor, product);
		else
			lstrcpynW(product, descriptor, 39);
	}
	if (feature)
		lstrcpynW(feature, descriptor + start, feature_length + 1);
	if (component) {
		component[0] = 0;
		if (rest) {
			if (packed)
				unpack_guid(rest, component);
			else
				lstrcpynW(component, rest, 39);
		}
	}
	if (used)
		*used = consumed;
	return ERROR_SUCCESS;
}

UINT WINAPI MsiDecomposeDescriptorA(LPCSTR descriptor, LPSTR product, LPSTR feature, LPSTR component, LPDWORD used)
{
	WIDE(descriptor);
	WCHAR a[39], b[MAX_FEATURE_CHARS + 1], c[39];
	UINT status = MsiDecomposeDescriptorW(w_descriptor, a, b, c, used);
	DROP(descriptor);
	if (!status) {
		if (product)
			WideCharToMultiByte(CP_ACP, 0, a, -1, product, 39, NULL, NULL);
		if (feature)
			WideCharToMultiByte(CP_ACP, 0, b, -1, feature, MAX_FEATURE_CHARS + 1, NULL, NULL);
		if (component)
			WideCharToMultiByte(CP_ACP, 0, c, -1, component, 39, NULL, NULL);
	}
	return status;
}

INSTALLSTATE WINAPI MsiUseFeatureExW(LPCWSTR product, LPCWSTR feature, DWORD mode, DWORD reserved)
{
	if (reserved || !product || !feature)
		return INSTALLSTATE_INVALIDARG;
	if (mode == INSTALLMODE_NODETECTION)
		return INSTALLSTATE_UNKNOWN;
	INSTALLSTATE state = MsiQueryFeatureStateW(product, feature);
	// A use of an installed feature counts towards what MsiGetFeatureUsage reports.
	if (state == INSTALLSTATE_LOCAL || state == INSTALLSTATE_SOURCE || state == INSTALLSTATE_DEFAULT)
		count_feature_use(product, feature);
	return state;
}

INSTALLSTATE WINAPI MsiUseFeatureExA(LPCSTR product, LPCSTR feature, DWORD mode, DWORD reserved)
{
	WIDE(product);
	WIDE(feature);
	INSTALLSTATE state = MsiUseFeatureExW(w_product, w_feature, mode, reserved);
	DROP(product);
	DROP(feature);
	return state;
}

INSTALLSTATE WINAPI MsiUseFeatureW(LPCWSTR product, LPCWSTR feature)
{
	return MsiUseFeatureExW(product, feature, 0, 0);
}

INSTALLSTATE WINAPI MsiUseFeatureA(LPCSTR product, LPCSTR feature)
{
	return MsiUseFeatureExA(product, feature, 0, 0);
}

UINT WINAPI MsiQueryFeatureStateExW(LPCWSTR product, LPCWSTR user, MSIINSTALLCONTEXT context, LPCWSTR feature, INSTALLSTATE *state)
{
	(void)user;
	(void)context;
	if (!product || !feature)
		return ERROR_INVALID_PARAMETER;
	if (product_state(product) != INSTALLSTATE_DEFAULT)
		return ERROR_UNKNOWN_PRODUCT;
	INSTALLSTATE found = product_feature_installed(product, feature);
	if (state)
		*state = found;
	return found == INSTALLSTATE_UNKNOWN ? ERROR_UNKNOWN_FEATURE : ERROR_SUCCESS;
}

UINT WINAPI MsiQueryFeatureStateExA(LPCSTR product, LPCSTR user, MSIINSTALLCONTEXT context, LPCSTR feature, INSTALLSTATE *state)
{
	(void)user;
	WIDE(product);
	WIDE(feature);
	UINT status = MsiQueryFeatureStateExW(w_product, NULL, context, w_feature, state);
	DROP(product);
	DROP(feature);
	return status;
}

UINT WINAPI MsiQueryComponentStateW(LPCWSTR product, LPCWSTR user, MSIINSTALLCONTEXT context, LPCWSTR component, INSTALLSTATE *state)
{
	(void)user;
	(void)context;
	if (!product || !component)
		return ERROR_INVALID_PARAMETER;
	if (product_state(product) != INSTALLSTATE_DEFAULT)
		return ERROR_UNKNOWN_PRODUCT;
	INSTALLSTATE found = MsiGetComponentPathW(product, component, NULL, NULL);
	if (state)
		*state = found;
	return found == INSTALLSTATE_UNKNOWN ? ERROR_UNKNOWN_COMPONENT : ERROR_SUCCESS;
}

UINT WINAPI MsiQueryComponentStateA(LPCSTR product, LPCSTR user, MSIINSTALLCONTEXT context, LPCSTR component, INSTALLSTATE *state)
{
	(void)user;
	WIDE(product);
	WIDE(component);
	UINT status = MsiQueryComponentStateW(w_product, NULL, context, w_component, state);
	DROP(product);
	DROP(component);
	return status;
}

UINT WINAPI MsiEnumProductsExW(LPCWSTR product, LPCWSTR user, DWORD context, DWORD index, WCHAR found[39], MSIINSTALLCONTEXT *installed, LPWSTR sid, LPDWORD sid_size)
{
	(void)user;
	(void)context;
	(void)product;
	WCHAR code[39];
	UINT status = MsiEnumProductsW(index, code);
	if (status)
		return status;
	if (found)
		lstrcpyW(found, code);
	if (installed)
		*installed = MSIINSTALLCONTEXT_MACHINE;
	if (sid_size)
		*sid_size = 0;
	if (sid)
		sid[0] = 0;
	return ERROR_SUCCESS;
}

UINT WINAPI MsiGetProductPropertyW(MSIHANDLE handle, LPCWSTR name, LPWSTR out, LPDWORD size)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	WCHAR *value = package_get(package, name);
	UINT status = out_text_w(value ? value : L"", value ? lstrlenW(value) : 0, out, size);
	msi_free(value);
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetProductPropertyA(MSIHANDLE handle, LPCSTR name, LPSTR out, LPDWORD size)
{
	return MsiGetPropertyA(handle, name, out, size);
}

UINT WINAPI MsiVerifyPackageW(LPCWSTR path)
{
	MSIHANDLE handle;
	if (!path)
		return ERROR_INVALID_PARAMETER;
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return ERROR_FILE_NOT_FOUND;
	if (MsiOpenDatabaseW(path, MSIDBOPEN_READONLY, &handle))
		return ERROR_INSTALL_PACKAGE_INVALID;
	MsiCloseHandle(handle);
	return ERROR_SUCCESS;
}

UINT WINAPI MsiVerifyPackageA(LPCSTR path)
{
	WIDE(path);
	UINT status = MsiVerifyPackageW(w_path);
	DROP(path);
	return status;
}

INT WINAPI MsiMessageBoxExW(HWND parent, LPCWSTR text, LPCWSTR caption, UINT type, DWORD unused, WORD language, DWORD timeout)
{
	(void)unused;
	(void)language;
	(void)timeout;
	return MessageBoxW(parent, text, caption, type);
}

INT WINAPI MsiMessageBoxExA(HWND parent, LPCSTR text, LPCSTR caption, UINT type, DWORD unused, WORD language, DWORD timeout)
{
	(void)unused;
	(void)language;
	(void)timeout;
	return MessageBoxA(parent, text, caption, type);
}

INT WINAPI MsiMessageBoxW(HWND parent, LPCWSTR text, LPCWSTR caption, UINT type, WORD language, DWORD unused)
{
	(void)language;
	(void)unused;
	return MessageBoxW(parent, text, caption, type);
}

INT WINAPI MsiMessageBoxA(HWND parent, LPCSTR text, LPCSTR caption, UINT type, WORD language, DWORD unused)
{
	(void)language;
	(void)unused;
	return MessageBoxA(parent, text, caption, type);
}

INT WINAPI MsiLoadStringW(MSIHANDLE instance, UINT id, LPWSTR out, int size, LANGID language)
{
	(void)language;
	return LoadStringW((HINSTANCE)(ULONG_PTR)instance, id, out, size);
}

INT WINAPI MsiLoadStringA(MSIHANDLE instance, UINT id, LPSTR out, int size, LANGID language)
{
	(void)language;
	return LoadStringA((HINSTANCE)(ULONG_PTR)instance, id, out, size);
}

UINT WINAPI MsiIsProductElevatedW(LPCWSTR product, BOOL *elevated)
{
	if (!product || !elevated)
		return ERROR_INVALID_PARAMETER;
	*elevated = product_state(product) == INSTALLSTATE_DEFAULT;
	return *elevated ? ERROR_SUCCESS : ERROR_UNKNOWN_PRODUCT;
}

UINT WINAPI MsiIsProductElevatedA(LPCSTR product, BOOL *elevated)
{
	WIDE(product);
	UINT status = MsiIsProductElevatedW(w_product, elevated);
	DROP(product);
	return status;
}

UINT WINAPI MsiGetFileHashW(LPCWSTR path, DWORD options, PMSIFILEHASHINFO hash)
{
	(void)options;
	if (!path || !hash || hash->dwFileHashInfoSize != sizeof *hash)
		return ERROR_INVALID_PARAMETER;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return ERROR_FILE_NOT_FOUND;
	// The digest is MD5, as the MsiFileHash table holds it.
	HCRYPTPROV provider = 0;
	HCRYPTHASH digest = 0;
	UINT status = ERROR_FUNCTION_FAILED;
	if (CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider, CALG_MD5, 0, 0, &digest)) {
		BYTE buffer[65536];
		DWORD got, length = sizeof hash->dwData;
		BOOL good = TRUE;
		while (good && ReadFile(file, buffer, sizeof buffer, &got, NULL) && got)
			good = CryptHashData(digest, buffer, got, 0);
		if (good && CryptGetHashParam(digest, HP_HASHVAL, (BYTE *)hash->dwData, &length, 0))
			status = ERROR_SUCCESS;
	}
	if (digest)
		CryptDestroyHash(digest);
	if (provider)
		CryptReleaseContext(provider, 0);
	CloseHandle(file);
	return status;
}

UINT WINAPI MsiGetFileHashA(LPCSTR path, DWORD options, PMSIFILEHASHINFO hash)
{
	WIDE(path);
	UINT status = MsiGetFileHashW(w_path, options, hash);
	DROP(path);
	return status;
}

UINT WINAPI MsiEnumFeaturesW(LPCWSTR product, DWORD index, LPWSTR feature, LPWSTR parent)
{
	MSIHANDLE handle;
	if (!product || !feature)
		return ERROR_INVALID_PARAMETER;
	UINT status = MsiOpenProductW(product, &handle);
	if (status)
		return status;
	msi_package *package = package_from_handle(handle);
	tables_load(package);
	status = ERROR_NO_MORE_ITEMS;
	if (index < package->feature_count) {
		lstrcpynW(feature, package->features[index].key, MAX_FEATURE_CHARS + 1);
		if (parent)
			lstrcpynW(parent, package->features[index].parent ? package->features[index].parent : L"", MAX_FEATURE_CHARS + 1);
		status = ERROR_SUCCESS;
	}
	object_unref(&package->base);
	MsiCloseHandle(handle);
	return status;
}

UINT WINAPI MsiEnumFeaturesA(LPCSTR product, DWORD index, LPSTR feature, LPSTR parent)
{
	WIDE(product);
	WCHAR a[MAX_FEATURE_CHARS + 1], b[MAX_FEATURE_CHARS + 1];
	UINT status = MsiEnumFeaturesW(w_product, index, a, b);
	DROP(product);
	if (!status) {
		WideCharToMultiByte(CP_ACP, 0, a, -1, feature, MAX_FEATURE_CHARS + 1, NULL, NULL);
		if (parent)
			WideCharToMultiByte(CP_ACP, 0, b, -1, parent, MAX_FEATURE_CHARS + 1, NULL, NULL);
	}
	return status;
}

UINT WINAPI MsiEnumComponentsW(DWORD index, LPWSTR out)
{
	WCHAR sid[80], key[256], name[40];
	(void)sid;
	wsprintfW(key, L"Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Components");
	HKEY handle;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle))
		return ERROR_NO_MORE_ITEMS;
	DWORD size = ARRAYSIZE(name);
	LSTATUS status = RegEnumKeyExW(handle, index, name, &size, NULL, NULL, NULL, NULL);
	RegCloseKey(handle);
	if (status)
		return ERROR_NO_MORE_ITEMS;
	return out && guid_unsquash(name, out) ? ERROR_SUCCESS : ERROR_BAD_CONFIGURATION;
}

UINT WINAPI MsiEnumComponentsA(DWORD index, LPSTR out)
{
	WCHAR code[39];
	UINT status = MsiEnumComponentsW(index, code);
	if (!status)
		WideCharToMultiByte(CP_ACP, 0, code, -1, out, 39, NULL, NULL);
	return status;
}

// Sources are recorded once, at installation: changing them later succeeds without a further effect.
#define SOURCE_NOOP_W(name, ...) UINT WINAPI name(__VA_ARGS__) { return ERROR_SUCCESS; }

SOURCE_NOOP_W(MsiSourceListForceResolutionW, LPCWSTR a, LPCWSTR b, DWORD c)
SOURCE_NOOP_W(MsiSourceListForceResolutionA, LPCSTR a, LPCSTR b, DWORD c)
SOURCE_NOOP_W(MsiSourceListForceResolutionExW, LPCWSTR a, LPCWSTR b, MSIINSTALLCONTEXT c, DWORD d)
SOURCE_NOOP_W(MsiSourceListForceResolutionExA, LPCSTR a, LPCSTR b, MSIINSTALLCONTEXT c, DWORD d)
SOURCE_NOOP_W(MsiInvalidateFeatureCache, DWORD a, LPCWSTR b)
SOURCE_NOOP_W(MsiCreateAndVerifyInstallerDirectory, DWORD a)
SOURCE_NOOP_W(MsiNotifySidChangeW, LPCWSTR a, LPCWSTR b)
SOURCE_NOOP_W(MsiNotifySidChangeA, LPCSTR a, LPCSTR b)
SOURCE_NOOP_W(MsiDeleteUserDataW, LPCWSTR a, LPCWSTR b, LPVOID c)
SOURCE_NOOP_W(MsiDeleteUserDataA, LPCSTR a, LPCSTR b, LPVOID c)
SOURCE_NOOP_W(Migrate10CachedPackagesW, LPCWSTR a, LPCWSTR b, LPCWSTR c, DWORD d)
SOURCE_NOOP_W(Migrate10CachedPackagesA, LPCSTR a, LPCSTR b, LPCSTR c, DWORD d)
SOURCE_NOOP_W(MsiSetOfflineContextW, DWORD a, DWORD b)
SOURCE_NOOP_W(MsiEnableUIPreview, MSIHANDLE a, MSIHANDLE *b)

UINT WINAPI QueryInstanceCount(void)
{
	return 0;
}

/* Advertising now, to the machine or the user. Writing an advertise script instead is not offered. */
UINT WINAPI MsiAdvertiseProductExW(LPCWSTR path, LPCWSTR script, LPCWSTR transforms, LANGID language, DWORD platform, DWORD options)
{
	const ULONG_PTR assignment = (ULONG_PTR)script;
	(void)language, (void)platform, (void)options;
	if (!path)
		return ERROR_INVALID_PARAMETER;
	if (assignment > 1)
		return ERROR_CALL_NOT_IMPLEMENTED;
	WCHAR line[ROOM] = L"ACTION=ADVERTISE ADVERTISE=ALL ";
	lstrcatW(line, assignment ? L"ALLUSERS=\"\"" : L"ALLUSERS=1");
	if (transforms && transforms[0]) {
		if (lstrlenW(transforms) > ROOM - 80)
			return ERROR_INVALID_PARAMETER;
		lstrcatW(line, L" TRANSFORMS=\"");
		lstrcatW(line, transforms);
		lstrcatW(line, L"\"");
	}
	return MsiInstallProductW(path, line);
}

UINT WINAPI MsiAdvertiseProductExA(LPCSTR path, LPCSTR script, LPCSTR transforms, LANGID language, DWORD platform, DWORD options)
{
	if ((ULONG_PTR)script > 1)
		return path ? ERROR_CALL_NOT_IMPLEMENTED : ERROR_INVALID_PARAMETER;
	WIDE(path);
	WIDE(transforms);
	const UINT status = MsiAdvertiseProductExW(w_path, (LPCWSTR)script, w_transforms, language, platform, options);
	DROP(path);
	DROP(transforms);
	return status;
}

UINT WINAPI MsiAdvertiseProductW(LPCWSTR path, LPCWSTR script, LPCWSTR transforms, LANGID language)
{
	return MsiAdvertiseProductExW(path, script, transforms, language, 0, 0);
}

UINT WINAPI MsiAdvertiseProductA(LPCSTR path, LPCSTR script, LPCSTR transforms, LANGID language)
{
	return MsiAdvertiseProductExA(path, script, transforms, language, 0, 0);
}

// What this engine does not offer. Each answers with the documented status of that situation.
#define NOT_OFFERED(name, status, ...) UINT WINAPI name(__VA_ARGS__) { return status; }

NOT_OFFERED(MsiAdvertiseScriptW, ERROR_CALL_NOT_IMPLEMENTED, LPCWSTR a, DWORD b, PHKEY c, BOOL d)
NOT_OFFERED(MsiAdvertiseScriptA, ERROR_CALL_NOT_IMPLEMENTED, LPCSTR a, DWORD b, PHKEY c, BOOL d)
NOT_OFFERED(MsiProcessAdvertiseScriptW, ERROR_CALL_NOT_IMPLEMENTED, LPCWSTR a, LPCWSTR b, HKEY c, BOOL d, BOOL e)
NOT_OFFERED(MsiProcessAdvertiseScriptA, ERROR_CALL_NOT_IMPLEMENTED, LPCSTR a, LPCSTR b, HKEY c, BOOL d, BOOL e)
NOT_OFFERED(MsiGetProductInfoFromScriptW, ERROR_CALL_NOT_IMPLEMENTED, LPCWSTR a, LPWSTR b, LANGID *c, LPDWORD d, LPWSTR e, LPDWORD f, LPWSTR g, LPDWORD h)
NOT_OFFERED(MsiGetProductInfoFromScriptA, ERROR_CALL_NOT_IMPLEMENTED, LPCSTR a, LPSTR b, LANGID *c, LPDWORD d, LPSTR e, LPDWORD f, LPSTR g, LPDWORD h)
/* Without an interface of its own nothing is asked of the user: a known product simply has nothing to collect. */
UINT WINAPI MsiCollectUserInfoW(LPCWSTR product)
{
	if (!product)
		return ERROR_INVALID_PARAMETER;
	const INSTALLSTATE state = MsiQueryProductStateW(product);
	return state == INSTALLSTATE_UNKNOWN || state == INSTALLSTATE_INVALIDARG ? ERROR_UNKNOWN_PRODUCT : ERROR_SUCCESS;
}

UINT WINAPI MsiCollectUserInfoA(LPCSTR product)
{
	if (!product)
		return ERROR_INVALID_PARAMETER;
	WIDE(product);
	const UINT status = MsiCollectUserInfoW(w_product);
	DROP(product);
	return status;
}

NOT_OFFERED(MsiConfigureFeatureFromDescriptorW, ERROR_CALL_NOT_IMPLEMENTED, LPCWSTR a, INSTALLSTATE b)
NOT_OFFERED(MsiConfigureFeatureFromDescriptorA, ERROR_CALL_NOT_IMPLEMENTED, LPCSTR a, INSTALLSTATE b)
NOT_OFFERED(MsiReinstallFeatureFromDescriptorW, ERROR_CALL_NOT_IMPLEMENTED, LPCWSTR a, DWORD b)
NOT_OFFERED(MsiReinstallFeatureFromDescriptorA, ERROR_CALL_NOT_IMPLEMENTED, LPCSTR a, DWORD b)
/* The package's files against the free space of the volume it installs to. Only the File table counts: no registry, no temporary space. */
UINT WINAPI MsiVerifyDiskSpace(MSIHANDLE handle)
{
	msi_package *package = package_from_handle(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	object_unref(&package->base);
	MSIHANDLE database = MsiGetActiveDatabase(handle), view = 0, record = 0;
	if (!database)
		return ERROR_FUNCTION_FAILED;
	ULONGLONG needed = 0;
	if (MsiDatabaseOpenViewW(database, L"SELECT `FileSize` FROM `File`", &view) == ERROR_SUCCESS) {
		if (MsiViewExecute(view, 0) == ERROR_SUCCESS)
			while (MsiViewFetch(view, &record) == ERROR_SUCCESS) {
				const int size = MsiRecordGetInteger(record, 1);
				if (size > 0)
					needed += (ULONGLONG)size;
				MsiCloseHandle(record);
			}
		MsiCloseHandle(view);
	}
	MsiCloseHandle(database);
	WCHAR target[MAX_PATH] = {0};
	DWORD length = MAX_PATH;
	if (MsiGetPropertyW(handle, L"TARGETDIR", target, &length) != ERROR_SUCCESS || !target[0]) {
		length = MAX_PATH;
		MsiGetPropertyW(handle, L"ROOTDRIVE", target, &length);
	}
	ULARGE_INTEGER free_space;
	if (!GetDiskFreeSpaceExW(target[0] ? target : NULL, &free_space, NULL, NULL))
		return ERROR_FUNCTION_FAILED;
	return free_space.QuadPart < needed ? ERROR_DISK_FULL : ERROR_SUCCESS;
}

NOT_OFFERED(MsiPreviewBillboardW, ERROR_CALL_NOT_IMPLEMENTED, MSIHANDLE a, LPCWSTR b, LPCWSTR c)
NOT_OFFERED(MsiPreviewBillboardA, ERROR_CALL_NOT_IMPLEMENTED, MSIHANDLE a, LPCSTR b, LPCSTR c)
NOT_OFFERED(MsiPreviewDialogW, ERROR_CALL_NOT_IMPLEMENTED, MSIHANDLE a, LPCWSTR b)
NOT_OFFERED(MsiPreviewDialogA, ERROR_CALL_NOT_IMPLEMENTED, MSIHANDLE a, LPCSTR b)
NOT_OFFERED(MsiProvideAssemblyW, ERROR_CALL_NOT_IMPLEMENTED, LPCWSTR a, LPCWSTR b, DWORD c, DWORD d, LPWSTR e, LPDWORD f)
NOT_OFFERED(MsiProvideAssemblyA, ERROR_CALL_NOT_IMPLEMENTED, LPCSTR a, LPCSTR b, DWORD c, DWORD d, LPSTR e, LPDWORD f)
NOT_OFFERED(MsiProvideComponentFromDescriptorW, ERROR_CALL_NOT_IMPLEMENTED, LPCWSTR a, LPWSTR b, LPDWORD c, LPDWORD d)
NOT_OFFERED(MsiProvideComponentFromDescriptorA, ERROR_CALL_NOT_IMPLEMENTED, LPCSTR a, LPSTR b, LPDWORD c, LPDWORD d)
INSTALLSTATE WINAPI MsiQueryFeatureStateFromDescriptorW(LPCWSTR a) { return INSTALLSTATE_UNKNOWN; }
INSTALLSTATE WINAPI MsiQueryFeatureStateFromDescriptorA(LPCSTR a) { return INSTALLSTATE_UNKNOWN; }
USERINFOSTATE WINAPI MsiGetUserInfoW(LPCWSTR a, LPWSTR b, LPDWORD c, LPWSTR d, LPDWORD e, LPWSTR f, LPDWORD g) { return USERINFOSTATE_ABSENT; }
USERINFOSTATE WINAPI MsiGetUserInfoA(LPCSTR a, LPSTR b, LPDWORD c, LPSTR d, LPDWORD e, LPSTR f, LPDWORD g) { return USERINFOSTATE_ABSENT; }
NOT_OFFERED(MsiGetProductCodeFromPackageCodeW, ERROR_UNKNOWN_PRODUCT, LPCWSTR a, LPWSTR b)
NOT_OFFERED(MsiGetProductCodeFromPackageCodeA, ERROR_UNKNOWN_PRODUCT, LPCSTR a, LPSTR b)
NOT_OFFERED(MsiCreateTransformSummaryInfoW, ERROR_CALL_NOT_IMPLEMENTED, MSIHANDLE a, MSIHANDLE b, LPCWSTR c, int d, int e)
NOT_OFFERED(MsiCreateTransformSummaryInfoA, ERROR_CALL_NOT_IMPLEMENTED, MSIHANDLE a, MSIHANDLE b, LPCSTR c, int d, int e)
HRESULT WINAPI MsiGetFileSignatureInformationW(LPCWSTR a, DWORD b, PCCERT_CONTEXT *c, LPBYTE d, LPDWORD e) { return TRUST_E_NOSIGNATURE; }
HRESULT WINAPI MsiGetFileSignatureInformationA(LPCSTR a, DWORD b, PCCERT_CONTEXT *c, LPBYTE d, LPDWORD e) { return TRUST_E_NOSIGNATURE; }
NOT_OFFERED(MsiBeginTransactionW, ERROR_CALL_NOT_IMPLEMENTED, LPCWSTR a, DWORD b, MSIHANDLE *c, HANDLE *d)
NOT_OFFERED(MsiBeginTransactionA, ERROR_CALL_NOT_IMPLEMENTED, LPCSTR a, DWORD b, MSIHANDLE *c, HANDLE *d)
NOT_OFFERED(MsiEndTransaction, ERROR_NO_MORE_ITEMS, DWORD a)
NOT_OFFERED(MsiJoinTransaction, ERROR_CALL_NOT_IMPLEMENTED, MSIHANDLE a, DWORD b, HANDLE *c)
NOT_OFFERED(MsiEnumClientsExW, ERROR_NO_MORE_ITEMS, LPCWSTR a, LPCWSTR b, DWORD c, DWORD d, WCHAR e[39], MSIINSTALLCONTEXT *f, LPWSTR g, LPDWORD h)
NOT_OFFERED(MsiEnumClientsExA, ERROR_NO_MORE_ITEMS, LPCSTR a, LPCSTR b, DWORD c, DWORD d, CHAR e[39], MSIINSTALLCONTEXT *f, LPSTR g, LPDWORD h)

/* Patches live in patch.c; a list of a patch's files needs its deltas, which are not read here. */
NOT_OFFERED(MsiGetPatchFileListW, ERROR_PATCH_PACKAGE_OPEN_FAILED, LPCWSTR a, LPCWSTR b, LPDWORD c, MSIHANDLE **d)
NOT_OFFERED(MsiGetPatchFileListA, ERROR_PATCH_PACKAGE_OPEN_FAILED, LPCSTR a, LPCSTR b, LPDWORD c, MSIHANDLE **d)

// Programs check the installer's version here before relying on it: this engine implements the 5.0 interface they ask about.
HRESULT WINAPI DllGetVersion(DLLVERSIONINFO *info)
{
	if (!info || info->cbSize < sizeof *info)
		return E_INVALIDARG;
	info->dwMajorVersion = 5;
	info->dwMinorVersion = 0;
	info->dwBuildNumber = 9600;
	info->dwPlatformID = DLLVER_PLATFORM_WINDOWS;
	return S_OK;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
	(void)clsid;
	(void)iid;
	if (out)
		*out = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllRegisterServer(void)
{
	return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	return S_OK;
}

/* An installer shortcut keeps its descriptor in the Darwin block of the .lnk extra data (MS-SHLLINK 2.5.3). */
static BOOL shortcut_descriptor(LPCWSTR path, WCHAR *descriptor)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	DWORD size = GetFileSize(file, NULL), got = 0;
	BYTE *data = size != INVALID_FILE_SIZE && size >= 0x4c ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
	BOOL found = FALSE;
	if (data && ReadFile(file, data, size, &got, NULL) && got == size) {
		const DWORD flags = *(DWORD *)(data + 0x14);
		DWORD at = 0x4c;
		if ((flags & 1) && at + 2 <= size)
			at += 2 + *(WORD *)(data + at);
		if ((flags & 2) && at + 4 <= size)
			at += *(DWORD *)(data + at);
		for (DWORD bit = 4; bit <= 0x40; bit <<= 1)
			if ((flags & bit) && at + 2 <= size)
				at += 2 + *(WORD *)(data + at) * (flags & 0x80 ? 2 : 1);
		while (at + 8 <= size) {
			const DWORD block = *(DWORD *)(data + at);
			if (block < 8 || block > size - at)
				break;
			if (*(DWORD *)(data + at + 4) == 0xa0000006 && block >= 0x314) {
				lstrcpynW(descriptor, (const WCHAR *)(data + at + 0x10c), 260);
				found = descriptor[0] != 0;
				break;
			}
			at += block;
		}
	}
	if (data)
		HeapFree(GetProcessHeap(), 0, data);
	CloseHandle(file);
	return found;
}

UINT WINAPI MsiGetShortcutTargetW(LPCWSTR path, LPWSTR product, LPWSTR feature, LPWSTR component)
{
	WCHAR descriptor[260];
	if (!path)
		return ERROR_INVALID_PARAMETER;
	if (!shortcut_descriptor(path, descriptor))
		return ERROR_FUNCTION_FAILED;
	return MsiDecomposeDescriptorW(descriptor, product, feature, component, NULL);
}

UINT WINAPI MsiGetShortcutTargetA(LPCSTR path, LPSTR product, LPSTR feature, LPSTR component)
{
	WCHAR w_product[39], w_feature[MAX_FEATURE_CHARS + 1], w_component[39];
	WIDE(path);
	UINT status = MsiGetShortcutTargetW(w_path, w_product, w_feature, w_component);
	DROP(path);
	if (status == ERROR_SUCCESS) {
		if (product)
			WideCharToMultiByte(CP_ACP, 0, w_product, -1, product, 39, NULL, NULL);
		if (feature)
			WideCharToMultiByte(CP_ACP, 0, w_feature, -1, feature, MAX_FEATURE_CHARS + 1, NULL, NULL);
		if (component)
			WideCharToMultiByte(CP_ACP, 0, w_component, -1, component, 39, NULL, NULL);
	}
	return status;
}
