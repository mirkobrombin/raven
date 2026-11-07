/* SPDX-License-Identifier: MIT */
#include "wshom.h"

/* RegRead, RegWrite and RegDelete, and the environments: the process's own, and the ones the registry keeps for the system, the user and
 * the session. */

static HRESULT win32(LONG error) { return HRESULT_FROM_WIN32(error); }

/* The root a name starts with, and where the rest of it begins. */
static HKEY root_of(const WCHAR *name, const WCHAR **rest)
{
	static const struct {
		const WCHAR *name;
		HKEY key;
	} roots[] = {
		{ L"HKCU", HKEY_CURRENT_USER }, { L"HKEY_CURRENT_USER", HKEY_CURRENT_USER }, { L"HKLM", HKEY_LOCAL_MACHINE },
		{ L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE }, { L"HKCR", HKEY_CLASSES_ROOT }, { L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT },
		{ L"HKU", HKEY_USERS }, { L"HKEY_USERS", HKEY_USERS }, { L"HKCC", HKEY_CURRENT_CONFIG }, { L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG },
	};
	const WCHAR *slash = name;
	while (*slash && *slash != L'\\')
		slash++;
	for (UINT i = 0; i < ARRAYSIZE(roots); i++)
		if ((UINT)lstrlenW(roots[i].name) == (UINT)(slash - name) &&
			CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, roots[i].name, -1, name, (int)(slash - name)) == CSTR_EQUAL) {
			*rest = *slash ? slash + 1 : slash;
			return roots[i].key;
		}
	return NULL;
}

/* A name split into its root, its key and its value: a name that ends with a backslash names the key, and its default value. */
typedef struct {
	HKEY root;
	WCHAR *key;
	const WCHAR *value;
	WCHAR *copy;
} reg_name;

static HRESULT split(const WCHAR *name, reg_name *out)
{
	const WCHAR *rest;
	out->root = root_of(name, &rest);
	if (!out->root)
		return win32(ERROR_FILE_NOT_FOUND);
	out->copy = sr_copy(rest);
	WCHAR *last = NULL;
	for (WCHAR *c = out->copy; *c; c++)
		if (*c == L'\\')
			last = c;
	if (!last) {
		/* A value right under the root. */
		out->value = out->copy;
		out->key = out->copy + lstrlenW(out->copy);
		return S_OK;
	}
	*last = 0;
	out->key = out->copy;
	out->value = last + 1;
	return S_OK;
}

static HRESULT array_of_bytes(const BYTE *data, DWORD size, VARIANT *result)
{
	SAFEARRAYBOUND bound = { size, 0 };
	SAFEARRAY *array = SafeArrayCreate(VT_VARIANT, 1, &bound);
	if (!array)
		return E_OUTOFMEMORY;
	for (LONG i = 0; i < (LONG)size; i++) {
		VARIANT byte;
		V_VT(&byte) = VT_UI1;
		V_UI1(&byte) = data[i];
		SafeArrayPutElement(array, &i, &byte);
	}
	V_VT(result) = VT_ARRAY | VT_VARIANT;
	V_ARRAY(result) = array;
	return S_OK;
}

static HRESULT array_of_strings(const WCHAR *data, DWORD chars, VARIANT *result)
{
	LONG count = 0;
	for (const WCHAR *s = data; s < data + chars && *s; s += lstrlenW(s) + 1)
		count++;
	SAFEARRAYBOUND bound = { (ULONG)count, 0 };
	SAFEARRAY *array = SafeArrayCreate(VT_VARIANT, 1, &bound);
	if (!array)
		return E_OUTOFMEMORY;
	LONG i = 0;
	for (const WCHAR *s = data; s < data + chars && *s; s += lstrlenW(s) + 1, i++) {
		VARIANT text;
		V_VT(&text) = VT_BSTR;
		V_BSTR(&text) = SysAllocString(s);
		SafeArrayPutElement(array, &i, &text);
		VariantClear(&text);
	}
	V_VT(result) = VT_ARRAY | VT_VARIANT;
	V_ARRAY(result) = array;
	return S_OK;
}

HRESULT wsh_reg_read(const WCHAR *name, VARIANT *result)
{
	reg_name parts;
	HRESULT hr = split(name, &parts);
	if (FAILED(hr))
		return hr;
	HKEY key;
	LONG error = RegOpenKeyExW(parts.root, parts.key, 0, KEY_READ, &key);
	if (!error) {
		DWORD type, size = 0;
		error = RegQueryValueExW(key, parts.value, NULL, &type, NULL, &size);
		BYTE *data = sr_alloc(size + 2 * sizeof(WCHAR));
		if (!error)
			error = RegQueryValueExW(key, parts.value, NULL, &type, data, &size);
		RegCloseKey(key);
		if (!error) {
			VARIANT got;
			VariantInit(&got);
			switch (type) {
			case REG_SZ:
			case REG_EXPAND_SZ:
				hr = sr_return_text(&got, (const WCHAR *)data);
				break;
			case REG_DWORD:
				hr = sr_return_long(&got, *(LONG *)data);
				break;
			case REG_MULTI_SZ:
				hr = array_of_strings((const WCHAR *)data, size / sizeof(WCHAR), &got);
				break;
			case REG_QWORD:
				V_VT(&got) = VT_R8;
				V_R8(&got) = (double)*(ULONGLONG *)data;
				break;
			default:
				hr = array_of_bytes(data, size, &got);
				break;
			}
			if (result)
				*result = got;
			else
				VariantClear(&got);
		}
		sr_free(data);
	}
	sr_free(parts.copy);
	return error ? win32(error) : hr;
}

HRESULT wsh_reg_write(const WCHAR *name, VARIANT *value, const WCHAR *type)
{
	static const struct {
		const WCHAR *name;
		DWORD type;
	} types[] = { { L"REG_SZ", REG_SZ }, { L"REG_EXPAND_SZ", REG_EXPAND_SZ }, { L"REG_DWORD", REG_DWORD }, { L"REG_BINARY", REG_BINARY } };
	DWORD kind = REG_SZ;
	if (type) {
		UINT i = 0;
		while (i < ARRAYSIZE(types) && lstrcmpiW(types[i].name, type))
			i++;
		if (i == ARRAYSIZE(types))
			return sr_error(5);
		kind = types[i].type;
	}
	if (!value)
		return DISP_E_PARAMNOTFOUND;
	reg_name parts;
	HRESULT hr = split(name, &parts);
	if (FAILED(hr))
		return hr;
	BSTR text = NULL;
	LONG number = 0;
	if (kind == REG_SZ || kind == REG_EXPAND_SZ)
		hr = sr_text(value, &text);
	else
		hr = sr_long(value, &number);
	if (SUCCEEDED(hr)) {
		HKEY key;
		LONG error = RegCreateKeyExW(parts.root, parts.key, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
		if (!error) {
			if (text)
				error = RegSetValueExW(key, parts.value, 0, kind, (const BYTE *)text, (SysStringLen(text) + 1) * sizeof(WCHAR));
			else
				error = RegSetValueExW(key, parts.value, 0, kind, (const BYTE *)&number, sizeof(number));
			RegCloseKey(key);
		}
		hr = error ? win32(error) : S_OK;
	}
	SysFreeString(text);
	sr_free(parts.copy);
	return hr;
}

HRESULT wsh_reg_delete(const WCHAR *name)
{
	reg_name parts;
	HRESULT hr = split(name, &parts);
	if (FAILED(hr))
		return hr;
	LONG error;
	if (!*parts.value) {
		/* A key goes only when it holds no keys. */
		error = RegDeleteKeyW(parts.root, parts.key);
	} else {
		HKEY key;
		error = RegOpenKeyExW(parts.root, parts.key, 0, KEY_SET_VALUE, &key);
		if (!error) {
			error = RegDeleteValueW(key, parts.value);
			RegCloseKey(key);
		}
	}
	sr_free(parts.copy);
	return error ? win32(error) : S_OK;
}

/* The environments. */

typedef struct {
	sr_object base;
	/* The key that holds the variables; none for the process's own. */
	HKEY root;
	const WCHAR *key;
} environment;

enum { ENV_COUNT = 1, ENV_LENGTH, ENV_REMOVE };

static const sr_member environment_members[] = {
	{ L"Item", DISPID_VALUE, 1 }, { L"Count", ENV_COUNT, 0 }, { L"length", ENV_LENGTH, 0 }, { L"Remove", ENV_REMOVE, 1 },
	{ L"_NewEnum", DISPID_NEWENUM, 0 }, { NULL },
};

/* Every variable as NAME=value, in a block that ends with an empty string. */
static WCHAR *variables_of(environment *env, ULONG *count)
{
	WCHAR *block = NULL;
	SIZE_T used = 0, room = 0;
	*count = 0;
	const WCHAR *adding = NULL;
#define ADD(text) do { \
		adding = (text); \
		SIZE_T n = lstrlenW(adding) + 1; \
		if (used + n + 1 > room) { \
			room = (used + n + 1) * 2; \
			block = sr_grow(block, room * sizeof(WCHAR)); \
		} \
		memcpy(block + used, adding, n * sizeof(WCHAR)); \
		used += n; \
	} while (0)
	if (!env->root) {
		WCHAR *strings = GetEnvironmentStringsW();
		for (const WCHAR *s = strings; s && *s; s += lstrlenW(s) + 1)
			/* The current directories of the drives hide behind names that start with '='. */
			if (*s != L'=') {
				ADD(s);
				(*count)++;
			}
		if (strings)
			FreeEnvironmentStringsW(strings);
	} else {
		HKEY key;
		if (!RegOpenKeyExW(env->root, env->key, 0, KEY_READ, &key)) {
			WCHAR name[16384];
			for (DWORD index = 0;; index++) {
				DWORD name_size = ARRAYSIZE(name), type, size = 0;
				if (RegEnumValueW(key, index, name, &name_size, NULL, &type, NULL, &size))
					break;
				if (type != REG_SZ && type != REG_EXPAND_SZ)
					continue;
				WCHAR *entry = sr_alloc((name_size + 2) * sizeof(WCHAR) + size + sizeof(WCHAR));
				lstrcpyW(entry, name);
				lstrcatW(entry, L"=");
				DWORD value_size = size;
				if (!RegQueryValueExW(key, name, NULL, NULL, (BYTE *)(entry + name_size + 1), &value_size)) {
					ADD(entry);
					(*count)++;
				}
				sr_free(entry);
			}
			RegCloseKey(key);
		}
	}
#undef ADD
	if (!block)
		block = sr_alloc(2 * sizeof(WCHAR));
	block[used] = 0;
	return block;
}

static HRESULT environment_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	environment *env = (environment *)self;
	BOOL put = sr_is_put(flags);
	BSTR name = NULL, value = NULL;
	HRESULT hr = S_OK;
	ULONG count;
	WCHAR *block;
	switch (id) {
	case DISPID_VALUE:
		if (FAILED(hr = sr_text(sr_arg(params, flags, 0), &name)))
			return hr;
		if (put) {
			if (SUCCEEDED(hr = sr_text(sr_put_value(params), &value))) {
				if (!env->root)
					hr = SetEnvironmentVariableW(name, value) ? S_OK : win32(GetLastError());
				else {
					HKEY key;
					LONG error = RegCreateKeyExW(env->root, env->key, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
					if (!error) {
						error = RegSetValueExW(key, name, 0, wcschr(value, L'%') ? REG_EXPAND_SZ : REG_SZ, (const BYTE *)value,
							(SysStringLen(value) + 1) * sizeof(WCHAR));
						RegCloseKey(key);
					}
					hr = error ? win32(error) : S_OK;
				}
			}
		} else if (!env->root) {
			DWORD size = GetEnvironmentVariableW(name, NULL, 0);
			WCHAR *text = sr_alloc((size + 1) * sizeof(WCHAR));
			if (size)
				GetEnvironmentVariableW(name, text, size);
			hr = sr_return_text(result, text);
			sr_free(text);
		} else {
			/* A variable that is not there reads as empty. */
			HKEY key;
			DWORD type, size = 0;
			WCHAR *text = NULL;
			if (!RegOpenKeyExW(env->root, env->key, 0, KEY_READ, &key)) {
				if (!RegQueryValueExW(key, name, NULL, &type, NULL, &size)) {
					text = sr_alloc(size + sizeof(WCHAR));
					RegQueryValueExW(key, name, NULL, &type, (BYTE *)text, &size);
				}
				RegCloseKey(key);
			}
			hr = sr_return_text(result, text ? text : L"");
			sr_free(text);
		}
		break;
	case ENV_COUNT:
	case ENV_LENGTH:
		sr_free(variables_of(env, &count));
		return sr_return_long(result, (LONG)count);
	case ENV_REMOVE:
		if (FAILED(hr = sr_text(sr_arg(params, flags, 0), &name)))
			return hr;
		if (!env->root)
			hr = SetEnvironmentVariableW(name, NULL) ? S_OK : win32(GetLastError());
		else {
			HKEY key;
			LONG error = RegOpenKeyExW(env->root, env->key, 0, KEY_SET_VALUE, &key);
			if (!error) {
				error = RegDeleteValueW(key, name);
				RegCloseKey(key);
			}
			hr = error ? win32(error) : S_OK;
		}
		break;
	case DISPID_NEWENUM: {
		block = variables_of(env, &count);
		VARIANT *items = sr_alloc((count ? count : 1) * sizeof(VARIANT));
		ULONG i = 0;
		for (const WCHAR *s = block; *s; s += lstrlenW(s) + 1, i++) {
			V_VT(&items[i]) = VT_BSTR;
			V_BSTR(&items[i]) = SysAllocString(s);
		}
		hr = sr_return_enum(result, items, count);
		for (i = 0; i < count; i++)
			VariantClear(&items[i]);
		sr_free(items);
		sr_free(block);
		return hr;
	}
	default:
		return DISP_E_MEMBERNOTFOUND;
	}
	SysFreeString(name);
	SysFreeString(value);
	return hr;
}

static const sr_class environment_class = { environment_members, environment_invoke, NULL };

HRESULT wsh_environment_new(const WCHAR *kind, VARIANT *result)
{
	static const struct {
		const WCHAR *name;
		HKEY root;
		const WCHAR *key;
	} kinds[] = {
		{ L"System", HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment" },
		{ L"User", HKEY_CURRENT_USER, L"Environment" },
		{ L"Volatile", HKEY_CURRENT_USER, L"Volatile Environment" },
		{ L"Process", NULL, NULL },
	};
	/* Without a kind it is the system's. */
	UINT i = 0;
	if (kind && *kind)
		while (i < ARRAYSIZE(kinds) && lstrcmpiW(kinds[i].name, kind))
			i++;
	if (i == ARRAYSIZE(kinds))
		return sr_error(5);
	environment *env = sr_object_new(&environment_class, sizeof(environment));
	env->root = kinds[i].root;
	env->key = kinds[i].key;
	return sr_return_object(result, sr_dispatch(env));
}
