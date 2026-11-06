/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

/* Features, costs and source lists, as programs ask for them from outside a sequence. */

static UINT feature_line(LPCWSTR product, const WCHAR *line)
{
	return MsiConfigureProductExW(product, INSTALLLEVEL_DEFAULT, (INSTALLSTATE)INSTALLSTATE_UNKNOWN, line);
}

UINT WINAPI MsiConfigureFeatureW(LPCWSTR product, LPCWSTR feature, INSTALLSTATE state)
{
	if (!product || !feature || lstrlenW(product) != 38)
		return ERROR_INVALID_PARAMETER;
	const WCHAR *name = state == INSTALLSTATE_LOCAL || state == INSTALLSTATE_DEFAULT ? L"ADDLOCAL"
		: state == INSTALLSTATE_SOURCE ? L"ADDSOURCE" : state == INSTALLSTATE_ABSENT ? L"REMOVE" : state == INSTALLSTATE_ADVERTISED ? L"ADVERTISE" : NULL;
	if (!name)
		return ERROR_INVALID_PARAMETER;
	WCHAR *line = msi_alloc((lstrlenW(feature) + 16) * sizeof(WCHAR));
	wsprintfW(line, L"%s=%s", name, feature);
	UINT status = feature_line(product, line);
	msi_free(line);
	return status;
}

UINT WINAPI MsiConfigureFeatureA(LPCSTR product, LPCSTR feature, INSTALLSTATE state)
{
	WCHAR *a = msi_from_ansi(product), *b = msi_from_ansi(feature);
	UINT status = MsiConfigureFeatureW(a, b, state);
	msi_free(a);
	msi_free(b);
	return status;
}

// The flags of a reinstall spell the letters REINSTALLMODE takes.
static void mode_letters(DWORD mode, WCHAR *out)
{
	static const struct { DWORD flag; WCHAR letter; } letters[] = {
		{ REINSTALLMODE_FILEMISSING, 'p' }, { REINSTALLMODE_FILEOLDERVERSION, 'o' }, { REINSTALLMODE_FILEEQUALVERSION, 'e' },
		{ REINSTALLMODE_FILEEXACT, 'd' }, { REINSTALLMODE_FILEVERIFY, 'c' }, { REINSTALLMODE_FILEREPLACE, 'a' },
		{ REINSTALLMODE_MACHINEDATA, 'm' }, { REINSTALLMODE_USERDATA, 'u' }, { REINSTALLMODE_SHORTCUT, 's' }, { REINSTALLMODE_PACKAGE, 'v' },
	};
	for (UINT i = 0; i < ARRAYSIZE(letters); i++)
		if (mode & letters[i].flag)
			*out++ = letters[i].letter;
	*out = 0;
}

UINT WINAPI MsiReinstallFeatureW(LPCWSTR product, LPCWSTR feature, DWORD mode)
{
	if (!product || !feature || lstrlenW(product) != 38)
		return ERROR_INVALID_PARAMETER;
	WCHAR letters[16];
	mode_letters(mode, letters);
	WCHAR *line = msi_alloc((lstrlenW(feature) + 48) * sizeof(WCHAR));
	wsprintfW(line, L"REINSTALL=%s REINSTALLMODE=%s", feature, *letters ? letters : L"omus");
	UINT status = feature_line(product, line);
	msi_free(line);
	return status;
}

UINT WINAPI MsiReinstallFeatureA(LPCSTR product, LPCSTR feature, DWORD mode)
{
	WCHAR *a = msi_from_ansi(product), *b = msi_from_ansi(feature);
	UINT status = MsiReinstallFeatureW(a, b, mode);
	msi_free(a);
	msi_free(b);
	return status;
}

UINT WINAPI MsiReinstallProductW(LPCWSTR product, DWORD mode)
{
	return MsiReinstallFeatureW(product, L"ALL", mode);
}

UINT WINAPI MsiInstallMissingComponentW(LPCWSTR product, LPCWSTR component, INSTALLSTATE state)
{
	if (!product || !component)
		return ERROR_INVALID_PARAMETER;
	if (state != INSTALLSTATE_LOCAL && state != INSTALLSTATE_SOURCE && state != INSTALLSTATE_DEFAULT)
		return ERROR_INVALID_PARAMETER;
	return MsiReinstallFeatureW(product, L"ALL", REINSTALLMODE_FILEMISSING | REINSTALLMODE_FILEOLDERVERSION | REINSTALLMODE_MACHINEDATA | REINSTALLMODE_USERDATA);
}

UINT WINAPI MsiInstallMissingComponentA(LPCSTR product, LPCSTR component, INSTALLSTATE state)
{
	WCHAR *a = msi_from_ansi(product), *b = msi_from_ansi(component);
	UINT status = MsiInstallMissingComponentW(a, b, state);
	msi_free(a);
	msi_free(b);
	return status;
}

UINT WINAPI MsiInstallMissingFileW(LPCWSTR product, LPCWSTR file)
{
	if (!product || !file)
		return ERROR_INVALID_PARAMETER;
	return MsiReinstallFeatureW(product, L"ALL", REINSTALLMODE_FILEMISSING);
}

UINT WINAPI MsiInstallMissingFileA(LPCSTR product, LPCSTR file)
{
	WCHAR *a = msi_from_ansi(product), *b = msi_from_ansi(file);
	UINT status = MsiInstallMissingFileW(a, b);
	msi_free(a);
	msi_free(b);
	return status;
}

/* Inside a session: what the tables say about a feature. */

static msi_package *session_tables(MSIHANDLE handle)
{
	msi_package *package = package_from_handle(handle);
	if (package)
		tables_load(package);
	return package;
}

UINT WINAPI MsiGetFeatureInfoW(MSIHANDLE handle, LPCWSTR name, LPDWORD attributes, LPWSTR title, LPDWORD title_size, LPWSTR help, LPDWORD help_size)
{
	if (!name)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = session_tables(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_UNKNOWN_FEATURE;
	msi_feature *feature = feature_find(package, name);
	if (feature) {
		status = ERROR_SUCCESS;
		if (attributes)
			*attributes = feature->attributes;
		const WCHAR *a = feature->title ? feature->title : L"", *b = feature->description ? feature->description : L"";
		if (title_size)
			status = out_text_w(a, lstrlenW(a), title, title_size);
		if (help_size) {
			UINT second = out_text_w(b, lstrlenW(b), help, help_size);
			status = status ? status : second;
		}
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetFeatureInfoA(MSIHANDLE handle, LPCSTR name, LPDWORD attributes, LPSTR title, LPDWORD title_size, LPSTR help, LPDWORD help_size)
{
	WCHAR *wide = msi_from_ansi(name);
	UINT status = ERROR_INVALID_PARAMETER;
	msi_package *package = wide ? session_tables(handle) : NULL;
	if (wide && !package)
		status = ERROR_INVALID_HANDLE;
	if (package) {
		msi_feature *feature = feature_find(package, wide);
		status = ERROR_UNKNOWN_FEATURE;
		if (feature) {
			status = ERROR_SUCCESS;
			if (attributes)
				*attributes = feature->attributes;
			const WCHAR *a = feature->title ? feature->title : L"", *b = feature->description ? feature->description : L"";
			if (title_size)
				status = out_text_a(a, lstrlenW(a), title, title_size);
			if (help_size) {
				UINT second = out_text_a(b, lstrlenW(b), help, help_size);
				status = status ? status : second;
			}
		}
		object_unref(&package->base);
	}
	msi_free(wide);
	return status;
}

UINT WINAPI MsiSetFeatureAttributesW(MSIHANDLE handle, LPCWSTR name, DWORD attributes)
{
	if (!name)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = session_tables(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	msi_feature *feature = feature_find(package, name);
	UINT status = feature ? ERROR_SUCCESS : ERROR_UNKNOWN_FEATURE;
	// Valid between costing and the script, as the documentation states.
	if (feature && package->in_script)
		status = ERROR_FUNCTION_FAILED;
	else if (feature)
		feature->attributes = attributes;
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiSetFeatureAttributesA(MSIHANDLE handle, LPCSTR name, DWORD attributes)
{
	WCHAR *wide = msi_from_ansi(name);
	UINT status = MsiSetFeatureAttributesW(handle, wide, attributes);
	msi_free(wide);
	return status;
}

UINT WINAPI MsiGetFeatureValidStatesW(MSIHANDLE handle, LPCWSTR name, LPDWORD states)
{
	if (!name || !states)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = session_tables(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	msi_feature *feature = feature_find(package, name);
	UINT status = ERROR_UNKNOWN_FEATURE;
	if (feature) {
		BOOL local = !feature->component_count, source = !feature->component_count;
		for (UINT i = 0; i < feature->component_count; i++) {
			INT attributes = feature->components[i]->attributes;
			if (attributes & msidbComponentAttributesSourceOnly)
				source = TRUE;
			else if (attributes & msidbComponentAttributesOptional)
				local = source = TRUE;
			else
				local = TRUE;
		}
		DWORD result = 0;
		if (local)
			result |= 1 << INSTALLSTATE_LOCAL;
		if (source)
			result |= 1 << INSTALLSTATE_SOURCE;
		if (!(feature->attributes & msidbFeatureAttributesUIDisallowAbsent))
			result |= 1 << INSTALLSTATE_ABSENT;
		if (!(feature->attributes & msidbFeatureAttributesDisallowAdvertise))
			result |= 1 << INSTALLSTATE_ADVERTISED;
		*states = result;
		status = ERROR_SUCCESS;
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetFeatureValidStatesA(MSIHANDLE handle, LPCSTR name, LPDWORD states)
{
	WCHAR *wide = msi_from_ansi(name);
	UINT status = MsiGetFeatureValidStatesW(handle, wide, states);
	msi_free(wide);
	return status;
}

// The cost of a feature in a state: 512 byte units its components add, or free when it leaves.
static INT feature_cost(msi_package *package, msi_feature *feature, INSTALLSTATE state)
{
	INSTALLSTATE wanted = state == INSTALLSTATE_UNKNOWN ? feature->action : state;
	INT total = 0;
	for (UINT i = 0; i < feature->component_count; i++) {
		INT cost = (INT)component_cost(package, feature->components[i]);
		if (wanted == INSTALLSTATE_LOCAL && feature->components[i]->installed != INSTALLSTATE_LOCAL)
			total += cost;
		else if (wanted == INSTALLSTATE_ABSENT && feature->components[i]->installed == INSTALLSTATE_LOCAL)
			total -= cost;
	}
	return total;
}

UINT WINAPI MsiGetFeatureCostW(MSIHANDLE handle, LPCWSTR name, MSICOSTTREE tree, INSTALLSTATE state, LPINT cost)
{
	if (!name || !cost)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = session_tables(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	msi_feature *feature = feature_find(package, name);
	UINT status = ERROR_UNKNOWN_FEATURE;
	if (feature) {
		INT total = feature_cost(package, feature, state);
		if (tree == MSICOSTTREE_CHILDREN)
			for (UINT i = 0; i < package->feature_count; i++)
				if (package->features[i].parent && !lstrcmpW(package->features[i].parent, feature->key))
					total += feature_cost(package, &package->features[i], state);
		if (tree == MSICOSTTREE_PARENTS)
			for (msi_feature *up = feature_find(package, feature->parent); up; up = feature_find(package, up->parent))
				total += feature_cost(package, up, state);
		*cost = total;
		status = ERROR_SUCCESS;
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiGetFeatureCostA(MSIHANDLE handle, LPCSTR name, MSICOSTTREE tree, INSTALLSTATE state, LPINT cost)
{
	WCHAR *wide = msi_from_ansi(name);
	UINT status = MsiGetFeatureCostW(handle, wide, tree, state, cost);
	msi_free(wide);
	return status;
}

// One volume answers: where the component's folder is, or the install's root for the whole product.
UINT WINAPI MsiEnumComponentCostsW(MSIHANDLE handle, LPCWSTR name, DWORD index, INSTALLSTATE state, LPWSTR drive, LPDWORD drive_size, LPINT cost, LPINT temporary)
{
	if (!drive_size || !cost || !temporary)
		return ERROR_INVALID_PARAMETER;
	msi_package *package = session_tables(handle);
	if (!package)
		return ERROR_INVALID_HANDLE;
	UINT status = ERROR_SUCCESS;
	msi_component *component = name && *name ? component_find(package, name) : NULL;
	if (name && *name && !component)
		status = ERROR_UNKNOWN_COMPONENT;
	else if (index)
		status = ERROR_NO_MORE_ITEMS;
	else {
		INT total = 0;
		if (component)
			total = (state == INSTALLSTATE_ABSENT ? -1 : 1) * (INT)component_cost(package, component);
		else
			for (UINT i = 0; i < package->component_count; i++)
				if (package->components[i].action == INSTALLSTATE_LOCAL)
					total += (INT)component_cost(package, &package->components[i]);
		const WCHAR *folder = component ? directory_target(package, component->directory) : NULL;
		WCHAR *root = folder ? NULL : package_get(package, L"ROOTDRIVE");
		WCHAR volume[3] = { 'C', ':', 0 };
		const WCHAR *from = folder ? folder : root;
		if (from && from[0] && from[1] == ':')
			volume[0] = from[0];
		msi_free(root);
		*cost = total;
		*temporary = 0;
		status = out_text_w(volume, 2, drive, drive_size);
	}
	object_unref(&package->base);
	return status;
}

UINT WINAPI MsiEnumComponentCostsA(MSIHANDLE handle, LPCSTR name, DWORD index, INSTALLSTATE state, LPSTR drive, LPDWORD drive_size, LPINT cost, LPINT temporary)
{
	if (!drive_size)
		return ERROR_INVALID_PARAMETER;
	WCHAR *wide = msi_from_ansi(name), volume[8];
	DWORD room = ARRAYSIZE(volume);
	UINT status = MsiEnumComponentCostsW(handle, wide, index, state, volume, &room, cost, temporary);
	msi_free(wide);
	return status ? status : out_text_a(volume, lstrlenW(volume), drive, drive_size);
}

/* Usage of a feature, which MsiUseFeatureEx counts: per user, in the product's UserData key as Windows keeps it, as the number of uses and
 * the DOS date of the last one. The published key is not used, since a product whose key is there counts as advertised. */
typedef struct {
	DWORD count;
	WORD date, reserved;
} feature_usage;

static BOOL usage_key(LPCWSTR product, BOOL create, HKEY *key)
{
	WCHAR squashed[33], rest[64], path[256];
	if (!guid_squash(product, squashed))
		return FALSE;
	wsprintfW(rest, L"Products\\%s\\Usage", squashed);
	user_data_key(path, FALSE, rest);
	return create ? !RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, key, NULL) :
		!RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, key);
}

static feature_usage read_usage(HKEY key, LPCWSTR feature)
{
	feature_usage usage = {0};
	DWORD size = sizeof usage, type;
	if (RegQueryValueExW(key, feature, NULL, &type, (BYTE *)&usage, &size) || type != REG_BINARY || size != sizeof usage)
		memset(&usage, 0, sizeof usage);
	return usage;
}

void count_feature_use(LPCWSTR product, LPCWSTR feature)
{
	HKEY key;
	if (!usage_key(product, TRUE, &key))
		return;
	feature_usage usage = read_usage(key, feature);
	SYSTEMTIME now;
	GetLocalTime(&now);
	if (usage.count != MAXDWORD)
		usage.count++;
	usage.date = (WORD)(((now.wYear - 1980) << 9) | (now.wMonth << 5) | now.wDay);
	RegSetValueExW(key, feature, 0, REG_BINARY, (const BYTE *)&usage, sizeof usage);
	RegCloseKey(key);
}

UINT WINAPI MsiGetFeatureUsageW(LPCWSTR product, LPCWSTR feature, LPDWORD count, LPWORD date)
{
	if (!product || !feature)
		return ERROR_INVALID_PARAMETER;
	INSTALLSTATE state = MsiQueryFeatureStateW(product, feature);
	if (state == INSTALLSTATE_UNKNOWN)
		return ERROR_UNKNOWN_FEATURE;
	feature_usage usage = {0};
	HKEY key;
	if (usage_key(product, FALSE, &key)) {
		usage = read_usage(key, feature);
		RegCloseKey(key);
	}
	if (count)
		*count = usage.count;
	if (date)
		*date = usage.date;
	return ERROR_SUCCESS;
}

UINT WINAPI MsiGetFeatureUsageA(LPCSTR product, LPCSTR feature, LPDWORD count, LPWORD date)
{
	WCHAR *a = msi_from_ansi(product), *b = msi_from_ansi(feature);
	UINT status = MsiGetFeatureUsageW(a, b, count, date);
	msi_free(a);
	msi_free(b);
	return status;
}

/* Enumerations in the Ex shape: this engine's components are enumerated once and answer for the machine. */

UINT WINAPI MsiEnumComponentsExW(LPCWSTR user, DWORD contexts, DWORD index, WCHAR component[39], MSIINSTALLCONTEXT *context, LPWSTR sid, LPDWORD sid_size)
{
	(void)user;
	if (!(contexts & MSIINSTALLCONTEXT_ALL) || (sid && !sid_size))
		return ERROR_INVALID_PARAMETER;
	WCHAR code[39];
	UINT status = MsiEnumComponentsW(index, code);
	if (status)
		return status;
	if (component)
		lstrcpyW(component, code);
	if (context)
		*context = MSIINSTALLCONTEXT_MACHINE;
	if (sid_size) {
		if (sid && *sid_size)
			*sid = 0;
		*sid_size = 0;
	}
	return ERROR_SUCCESS;
}

UINT WINAPI MsiEnumComponentsExA(LPCSTR user, DWORD contexts, DWORD index, CHAR component[39], MSIINSTALLCONTEXT *context, LPSTR sid, LPDWORD sid_size)
{
	WCHAR *wide = msi_from_ansi(user), code[39];
	UINT status = MsiEnumComponentsExW(wide, contexts, index, code, context, NULL, sid_size);
	msi_free(wide);
	if (status)
		return status;
	if (component)
		WideCharToMultiByte(CP_ACP, 0, code, -1, component, 39, NULL, NULL);
	if (sid && sid_size && *sid_size)
		*sid = 0;
	return ERROR_SUCCESS;
}

UINT WINAPI MsiEnumProductsExA(LPCSTR product, LPCSTR user, DWORD contexts, DWORD index, CHAR code[39], MSIINSTALLCONTEXT *context, LPSTR sid, LPDWORD sid_size)
{
	WCHAR *a = msi_from_ansi(product), *b = msi_from_ansi(user), wide[39], wide_sid[80];
	DWORD room = ARRAYSIZE(wide_sid);
	UINT status = MsiEnumProductsExW(a, b, contexts, index, wide, context, wide_sid, &room);
	msi_free(a);
	msi_free(b);
	if (status)
		return status;
	if (code)
		WideCharToMultiByte(CP_ACP, 0, wide, -1, code, 39, NULL, NULL);
	return sid_size ? out_text_a(wide_sid, room, sid, sid_size) : ERROR_SUCCESS;
}

/* Source lists, under the product's published key: SourceList\Net and \URL number the places, \Media the disks. */

static BOOL source_key(const WCHAR *code, DWORD context, const WCHAR *below, WCHAR *key, HKEY *root)
{
	WCHAR squashed[33], rest[128];
	if (!code || !guid_squash(code, squashed))
		return FALSE;
	wsprintfW(rest, L"Products\\%s\\SourceList%s%s", squashed, below ? L"\\" : L"", below ? below : L"");
	BOOL machine = context == MSIINSTALLCONTEXT_MACHINE;
	published_key(key, machine, rest);
	*root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	return TRUE;
}

// Without a context the product says where it was published.
static DWORD source_context(const WCHAR *code, DWORD context)
{
	if (context == MSIINSTALLCONTEXT_MACHINE || context == MSIINSTALLCONTEXT_USERMANAGED || context == MSIINSTALLCONTEXT_USERUNMANAGED)
		return context;
	WCHAR key[256];
	HKEY root, handle;
	if (source_key(code, MSIINSTALLCONTEXT_MACHINE, NULL, key, &root) && !RegOpenKeyExW(root, key, 0, KEY_READ, &handle)) {
		RegCloseKey(handle);
		return MSIINSTALLCONTEXT_MACHINE;
	}
	return MSIINSTALLCONTEXT_USERUNMANAGED;
}

UINT WINAPI MsiSourceListEnumSourcesW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options, DWORD index, LPWSTR out, LPDWORD size)
{
	(void)user;
	WCHAR key[256];
	HKEY root, handle;
	if (!code || (out && !size))
		return ERROR_INVALID_PARAMETER;
	const WCHAR *kind = options & MSISOURCETYPE_URL ? L"URL" : L"Net";
	if (!source_key(code, source_context(code, context), kind, key, &root))
		return ERROR_INVALID_PARAMETER;
	if (RegOpenKeyExW(root, key, 0, KEY_READ, &handle))
		return ERROR_NO_MORE_ITEMS;
	WCHAR name[16], data[MAX_PATH * 2];
	wsprintfW(name, L"%u", index + 1);
	DWORD bytes = sizeof data, type;
	LONG result = RegQueryValueExW(handle, name, NULL, &type, (BYTE *)data, &bytes);
	RegCloseKey(handle);
	if (result)
		return ERROR_NO_MORE_ITEMS;
	DWORD unused = 0;
	return out_text_w(data, lstrlenW(data), out, size ? size : &unused);
}

UINT WINAPI MsiSourceListEnumSourcesA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options, DWORD index, LPSTR out, LPDWORD size)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user), wide[MAX_PATH * 2];
	DWORD room = ARRAYSIZE(wide), unused = 0;
	UINT status = MsiSourceListEnumSourcesW(a, b, context, options, index, wide, &room);
	msi_free(a);
	msi_free(b);
	return status ? status : out_text_a(wide, lstrlenW(wide), out, size ? size : &unused);
}

UINT WINAPI MsiSourceListGetInfoW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options, LPCWSTR property, LPWSTR out, LPDWORD size)
{
	(void)user;
	(void)options;
	WCHAR key[256], data[MAX_PATH * 2];
	HKEY root;
	if (!code || !property || (out && !size))
		return ERROR_INVALID_PARAMETER;
	if (!source_key(code, source_context(code, context), NULL, key, &root))
		return ERROR_INVALID_PARAMETER;
	HKEY handle;
	if (RegOpenKeyExW(root, key, 0, KEY_READ, &handle))
		return ERROR_UNKNOWN_PRODUCT;
	RegCloseKey(handle);
	DWORD bytes = sizeof data, unused = 0;
	if (!size)
		size = &unused;
	data[0] = 0;
	if (!lstrcmpW(property, INSTALLPROPERTY_PACKAGENAME))
		RegGetValueW(root, key, L"PackageName", RRF_RT_REG_SZ, NULL, data, &bytes);
	else if (!lstrcmpW(property, INSTALLPROPERTY_LASTUSEDSOURCE) || !lstrcmpW(property, INSTALLPROPERTY_LASTUSEDTYPE)) {
		// The first network place is the one used, unless SetInfo said otherwise.
		BOOL type = !lstrcmpW(property, INSTALLPROPERTY_LASTUSEDTYPE);
		if (RegGetValueW(root, key, L"LastUsedSource", RRF_RT_REG_SZ, NULL, data, &bytes)) {
			WCHAR net[280];
			wsprintfW(net, L"%s\\Net", key);
			bytes = sizeof data;
			if (RegGetValueW(root, net, L"1", RRF_RT_REG_SZ, NULL, data, &bytes))
				data[0] = 0;
		}
		if (type) {
			lstrcpyW(data, *data ? L"n" : L"");
		}
	} else if (!lstrcmpW(property, INSTALLPROPERTY_MEDIAPACKAGEPATH) || !lstrcmpW(property, INSTALLPROPERTY_DISKPROMPT)) {
		WCHAR media[280];
		wsprintfW(media, L"%s\\Media", key);
		RegGetValueW(root, media, !lstrcmpW(property, INSTALLPROPERTY_DISKPROMPT) ? L"DiskPrompt" : L"MediaPackage", RRF_RT_REG_SZ, NULL, data, &bytes);
	} else
		return ERROR_UNKNOWN_PROPERTY;
	return out_text_w(data, lstrlenW(data), out, size);
}

UINT WINAPI MsiSourceListGetInfoA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options, LPCSTR property, LPSTR out, LPDWORD size)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user), *c = msi_from_ansi(property), wide[MAX_PATH * 2];
	DWORD room = ARRAYSIZE(wide), unused = 0;
	UINT status = MsiSourceListGetInfoW(a, b, context, options, c, wide, &room);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	return status ? status : out_text_a(wide, lstrlenW(wide), out, size ? size : &unused);
}

// A disk's value: "volume;prompt" under its number.
UINT WINAPI MsiSourceListEnumMediaDisksW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options, DWORD index, LPDWORD disk, LPWSTR volume, LPDWORD volume_size, LPWSTR prompt, LPDWORD prompt_size)
{
	(void)user;
	(void)options;
	WCHAR key[256], name[16], data[1024];
	HKEY root, handle;
	if (!source_key(code, source_context(code, context), L"Media", key, &root))
		return ERROR_INVALID_PARAMETER;
	if (RegOpenKeyExW(root, key, 0, KEY_READ, &handle))
		return ERROR_NO_MORE_ITEMS;
	UINT status = ERROR_NO_MORE_ITEMS;
	for (DWORD i = 0, seen = 0;; i++) {
		DWORD name_room = ARRAYSIZE(name), bytes = sizeof data, type = 0;
		if (RegEnumValueW(handle, i, name, &name_room, NULL, &type, (BYTE *)data, &bytes))
			break;
		if (type != REG_SZ || name[0] < '0' || name[0] > '9' || seen++ != index)
			continue;
		WCHAR *split = data;
		while (*split && *split != ';')
			split++;
		const WCHAR *second = *split ? split + 1 : L"";
		*split = 0;
		UINT number = 0;
		for (const WCHAR *at = name; *at; at++)
			number = number * 10 + (*at - '0');
		if (disk)
			*disk = number;
		status = ERROR_SUCCESS;
		if (volume_size)
			status = out_text_w(data, lstrlenW(data), volume, volume_size);
		if (prompt_size) {
			UINT other = out_text_w(second, lstrlenW(second), prompt, prompt_size);
			status = status ? status : other;
		}
		break;
	}
	RegCloseKey(handle);
	return status;
}

UINT WINAPI MsiSourceListEnumMediaDisksA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options, DWORD index, LPDWORD disk, LPSTR volume, LPDWORD volume_size, LPSTR prompt, LPDWORD prompt_size)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user), wide_volume[512], wide_prompt[512];
	DWORD volume_room = ARRAYSIZE(wide_volume), prompt_room = ARRAYSIZE(wide_prompt);
	UINT status = MsiSourceListEnumMediaDisksW(a, b, context, options, index, disk, wide_volume, &volume_room, wide_prompt, &prompt_room);
	msi_free(a);
	msi_free(b);
	if (status)
		return status;
	if (volume_size)
		status = out_text_a(wide_volume, volume_room, volume, volume_size);
	if (prompt_size) {
		UINT other = out_text_a(wide_prompt, prompt_room, prompt, prompt_size);
		status = status ? status : other;
	}
	return status;
}

/* Changing the list. */

UINT WINAPI MsiSourceListSetInfoW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options, LPCWSTR property, LPCWSTR value)
{
	(void)user;
	(void)options;
	WCHAR key[256];
	HKEY root;
	if (!code || !property || !value || !source_key(code, source_context(code, context), NULL, key, &root))
		return ERROR_INVALID_PARAMETER;
	const WCHAR *name = !lstrcmpW(property, INSTALLPROPERTY_PACKAGENAME) ? L"PackageName" : !lstrcmpW(property, INSTALLPROPERTY_LASTUSEDSOURCE) ? L"LastUsedSource" : NULL;
	if (name)
		return RegSetKeyValueW(root, key, name, REG_SZ, value, (lstrlenW(value) + 1) * sizeof(WCHAR)) ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
	if (!lstrcmpW(property, INSTALLPROPERTY_MEDIAPACKAGEPATH) || !lstrcmpW(property, INSTALLPROPERTY_DISKPROMPT)) {
		WCHAR media[280];
		wsprintfW(media, L"%s\\Media", key);
		return RegSetKeyValueW(root, media, !lstrcmpW(property, INSTALLPROPERTY_DISKPROMPT) ? L"DiskPrompt" : L"MediaPackage", REG_SZ, value, (lstrlenW(value) + 1) * sizeof(WCHAR)) ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
	}
	return ERROR_UNKNOWN_PROPERTY;
}

UINT WINAPI MsiSourceListAddSourceExW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options, LPCWSTR source, DWORD index)
{
	(void)user;
	WCHAR key[256], name[16];
	HKEY root, handle;
	if (!code || !source || !*source)
		return ERROR_INVALID_PARAMETER;
	if (!source_key(code, source_context(code, context), options & MSISOURCETYPE_URL ? L"URL" : L"Net", key, &root))
		return ERROR_INVALID_PARAMETER;
	if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_READ | KEY_SET_VALUE, NULL, &handle, NULL))
		return ERROR_ACCESS_DENIED;
	// An index asked for pushes the later ones down; none asked for goes last. A source already listed stays where it is.
	DWORD count = 0;
	WCHAR data[MAX_PATH * 2];
	for (;; count++) {
		DWORD bytes = sizeof data;
		wsprintfW(name, L"%u", count + 1);
		if (RegQueryValueExW(handle, name, NULL, NULL, (BYTE *)data, &bytes))
			break;
		if (!lstrcmpiW(data, source)) {
			RegCloseKey(handle);
			return ERROR_SUCCESS;
		}
	}
	DWORD place = index && index <= count ? index : count + 1;
	for (DWORD i = count; i >= place; i--) {
		DWORD bytes = sizeof data;
		wsprintfW(name, L"%u", i);
		if (!RegQueryValueExW(handle, name, NULL, NULL, (BYTE *)data, &bytes)) {
			wsprintfW(name, L"%u", i + 1);
			RegSetValueExW(handle, name, 0, REG_SZ, (BYTE *)data, bytes);
		}
	}
	wsprintfW(name, L"%u", place);
	RegSetValueExW(handle, name, 0, REG_SZ, (const BYTE *)source, (lstrlenW(source) + 1) * sizeof(WCHAR));
	RegCloseKey(handle);
	return ERROR_SUCCESS;
}

UINT WINAPI MsiSourceListAddSourceW(LPCWSTR code, LPCWSTR user, DWORD reserved, LPCWSTR source)
{
	(void)reserved;
	return MsiSourceListAddSourceExW(code, user, MSIINSTALLCONTEXT_ALL, MSISOURCETYPE_NETWORK, source, 0);
}

UINT WINAPI MsiSourceListClearSourceW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options, LPCWSTR source)
{
	(void)user;
	WCHAR key[256], name[16], data[MAX_PATH * 2];
	HKEY root, handle;
	if (!code || !source || !source_key(code, source_context(code, context), options & MSISOURCETYPE_URL ? L"URL" : L"Net", key, &root))
		return ERROR_INVALID_PARAMETER;
	if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_SET_VALUE, &handle))
		return ERROR_SUCCESS;
	BOOL shifted = FALSE;
	for (DWORD i = 1;; i++) {
		DWORD bytes = sizeof data;
		wsprintfW(name, L"%u", i);
		if (RegQueryValueExW(handle, name, NULL, NULL, (BYTE *)data, &bytes))
			break;
		if (shifted) {
			wsprintfW(name, L"%u", i - 1);
			RegSetValueExW(handle, name, 0, REG_SZ, (BYTE *)data, bytes);
			wsprintfW(name, L"%u", i);
			RegDeleteValueW(handle, name);
		} else if (!lstrcmpiW(data, source)) {
			RegDeleteValueW(handle, name);
			shifted = TRUE;
		}
	}
	RegCloseKey(handle);
	return ERROR_SUCCESS;
}

UINT WINAPI MsiSourceListClearAllExW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options)
{
	(void)user;
	WCHAR key[256];
	HKEY root;
	if (!code || !source_key(code, source_context(code, context), options & MSISOURCETYPE_URL ? L"URL" : L"Net", key, &root))
		return ERROR_INVALID_PARAMETER;
	RegDeleteTreeW(root, key);
	return ERROR_SUCCESS;
}

UINT WINAPI MsiSourceListClearAllW(LPCWSTR code, LPCWSTR user, DWORD reserved)
{
	(void)reserved;
	return MsiSourceListClearAllExW(code, user, MSIINSTALLCONTEXT_ALL, MSISOURCETYPE_NETWORK);
}

UINT WINAPI MsiSourceListAddMediaDiskW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options, DWORD disk, LPCWSTR volume, LPCWSTR prompt)
{
	(void)user;
	(void)options;
	WCHAR key[256], name[16];
	HKEY root;
	if (!disk || !source_key(code, source_context(code, context), L"Media", key, &root))
		return ERROR_INVALID_PARAMETER;
	WCHAR *value = msi_alloc((lstrlenW(volume ? volume : L"") + lstrlenW(prompt ? prompt : L"") + 2) * sizeof(WCHAR));
	wsprintfW(value, L"%s;%s", volume ? volume : L"", prompt ? prompt : L"");
	wsprintfW(name, L"%u", disk);
	LONG result = RegSetKeyValueW(root, key, name, REG_SZ, value, (lstrlenW(value) + 1) * sizeof(WCHAR));
	msi_free(value);
	return result ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}

UINT WINAPI MsiSourceListClearMediaDiskW(LPCWSTR code, LPCWSTR user, MSIINSTALLCONTEXT context, DWORD options, DWORD disk)
{
	(void)user;
	(void)options;
	WCHAR key[256], name[16];
	HKEY root, handle;
	if (!source_key(code, source_context(code, context), L"Media", key, &root))
		return ERROR_INVALID_PARAMETER;
	if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &handle))
		return ERROR_SUCCESS;
	wsprintfW(name, L"%u", disk);
	RegDeleteValueW(handle, name);
	RegCloseKey(handle);
	return ERROR_SUCCESS;
}

// The ANSI side of the changers: each text becomes wide, the call stays the same.
UINT WINAPI MsiSourceListSetInfoA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options, LPCSTR property, LPCSTR value)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user), *c = msi_from_ansi(property), *d = msi_from_ansi(value);
	UINT status = MsiSourceListSetInfoW(a, b, context, options, c, d);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	msi_free(d);
	return status;
}

UINT WINAPI MsiSourceListAddSourceExA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options, LPCSTR source, DWORD index)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user), *c = msi_from_ansi(source);
	UINT status = MsiSourceListAddSourceExW(a, b, context, options, c, index);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	return status;
}

UINT WINAPI MsiSourceListAddSourceA(LPCSTR code, LPCSTR user, DWORD reserved, LPCSTR source)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user), *c = msi_from_ansi(source);
	UINT status = MsiSourceListAddSourceW(a, b, reserved, c);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	return status;
}

UINT WINAPI MsiSourceListClearSourceA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options, LPCSTR source)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user), *c = msi_from_ansi(source);
	UINT status = MsiSourceListClearSourceW(a, b, context, options, c);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	return status;
}

UINT WINAPI MsiSourceListClearAllExA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user);
	UINT status = MsiSourceListClearAllExW(a, b, context, options);
	msi_free(a);
	msi_free(b);
	return status;
}

UINT WINAPI MsiSourceListClearAllA(LPCSTR code, LPCSTR user, DWORD reserved)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user);
	UINT status = MsiSourceListClearAllW(a, b, reserved);
	msi_free(a);
	msi_free(b);
	return status;
}

UINT WINAPI MsiSourceListAddMediaDiskA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options, DWORD disk, LPCSTR volume, LPCSTR prompt)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user), *c = msi_from_ansi(volume), *d = msi_from_ansi(prompt);
	UINT status = MsiSourceListAddMediaDiskW(a, b, context, options, disk, c, d);
	msi_free(a);
	msi_free(b);
	msi_free(c);
	msi_free(d);
	return status;
}

UINT WINAPI MsiSourceListClearMediaDiskA(LPCSTR code, LPCSTR user, MSIINSTALLCONTEXT context, DWORD options, DWORD disk)
{
	WCHAR *a = msi_from_ansi(code), *b = msi_from_ansi(user);
	UINT status = MsiSourceListClearMediaDiskW(a, b, context, options, disk);
	msi_free(a);
	msi_free(b);
	return status;
}
