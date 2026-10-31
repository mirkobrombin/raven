/* SPDX-License-Identifier: MIT */
#include "atl.h"
#include <wchar.h>

/* The registrar reads registration scripts: a tree of keys, each with its values, over the words of a small language. The same
 * walk registers and unregisters, so a script always undoes exactly what it does. */

const IID IID_IRegistrar = {0x44ec053b, 0x400f, 0x11d0, {0x9d, 0xcd, 0x00, 0xa0, 0xc9, 0x03, 0x91, 0xd3}};
const CLSID CLSID_Registrar = {0x44ec053a, 0x400f, 0x11d0, {0x9d, 0xcd, 0x00, 0xa0, 0xc9, 0x03, 0x91, 0xd3}};

typedef struct replacement
{
	struct replacement *next;
	WCHAR *key;
	WCHAR *value;
} replacement;

typedef struct
{
	WCHAR *text;
	size_t size;
	size_t room;
} growing;

typedef struct
{
	const WCHAR *at;
	BOOL registering;
	WCHAR token[1024];
	BOOL quoted;
} script;

static BOOL append(growing *out, const WCHAR *text, size_t size)
{
	if (out->size + size + 1 > out->room)
	{
		size_t room = (out->size + size + 1) * 2;
		WCHAR *more = atl_alloc(room * sizeof(WCHAR));
		if (!more)
			return FALSE;
		if (out->text)
		{
			memcpy(more, out->text, out->size * sizeof(WCHAR));
			atl_free(out->text);
		}
		out->text = more;
		out->room = room;
	}
	memcpy(out->text + out->size, text, size * sizeof(WCHAR));
	out->size += size;
	out->text[out->size] = 0;
	return TRUE;
}

/* %NAME% takes its value here, once, before the script is read; %% stands for one percent sign. */
static HRESULT expand(atl_registrar *self, const WCHAR *text, WCHAR **out)
{
	growing result = {0};
	const WCHAR *at = text;
	if (!append(&result, L"", 0))
		return E_OUTOFMEMORY;
	while (*at)
	{
		const WCHAR *mark = wcschr(at, '%'), *end;
		const replacement *entry;
		if (!mark)
		{
			append(&result, at, wcslen(at));
			break;
		}
		append(&result, at, mark - at);
		if (!(end = wcschr(mark + 1, '%')))
		{
			atl_free(result.text);
			return DISP_E_EXCEPTION;
		}
		if (end == mark + 1)
			append(&result, L"%", 1);
		else
		{
			for (entry = self->replacements; entry; entry = entry->next)
				if ((size_t)(end - mark - 1) == wcslen(entry->key) && !_wcsnicmp(entry->key, mark + 1, end - mark - 1))
					break;
			if (!entry)
			{
				atl_free(result.text);
				return DISP_E_EXCEPTION;
			}
			append(&result, entry->value, wcslen(entry->value));
		}
		at = end + 1;
	}
	*out = result.text;
	return S_OK;
}

static BOOL is_space(WCHAR letter)
{
	return letter == ' ' || letter == '\t' || letter == '\r' || letter == '\n';
}

/* One word: a single-quoted text with '' for a quote, one brace or equals sign, or a run up to white space. */
static BOOL next_token(script *reader)
{
	const WCHAR *at = reader->at;
	size_t used = 0;
	reader->quoted = FALSE;
	while (is_space(*at))
		at++;
	if (!*at)
	{
		reader->token[0] = 0;
		reader->at = at;
		return FALSE;
	}
	if (*at == '\'')
	{
		reader->quoted = TRUE;
		for (at++; *at; at++)
		{
			if (*at == '\'')
			{
				if (at[1] != '\'')
					break;
				at++;
			}
			if (used + 1 < ARRAYSIZE(reader->token))
				reader->token[used++] = *at;
		}
		if (*at)
			at++;
	}
	else if (*at == '{' || *at == '}' || *at == '=')
		reader->token[used++] = *at++;
	else
		while (*at && !is_space(*at) && *at != '{' && *at != '}' && *at != '=' && used + 1 < ARRAYSIZE(reader->token))
			reader->token[used++] = *at++;
	reader->token[used] = 0;
	reader->at = at;
	return TRUE;
}

static BOOL token_is(const script *reader, const WCHAR *word)
{
	return !reader->quoted && !lstrcmpiW(reader->token, word);
}

HRESULT atl_delete_tree(HKEY parent, const WCHAR *name)
{
	HKEY key;
	WCHAR child[256];
	DWORD size;
	LONG status = RegOpenKeyExW(parent, name, 0, KEY_READ | KEY_WRITE, &key);
	if (status)
		return status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
	for (;;)
	{
		size = ARRAYSIZE(child);
		if (RegEnumKeyExW(key, 0, child, &size, NULL, NULL, NULL, NULL))
			break;
		if (FAILED(atl_delete_tree(key, child)))
			break;
	}
	RegCloseKey(key);
	status = RegDeleteKeyW(parent, name);
	return status ? HRESULT_FROM_WIN32(status) : S_OK;
}

static WORD hex_value(WCHAR letter)
{
	return letter >= '0' && letter <= '9' ? letter - '0' : letter >= 'a' && letter <= 'f' ? letter - 'a' + 10 : letter >= 'A' && letter <= 'F' ? letter - 'A' + 10 : 16;
}

/* After the equals sign: a type letter and its value. Numbers are read in decimal only, as these scripts always were. */
static HRESULT write_value(script *reader, HKEY key, const WCHAR *name)
{
	WCHAR kind;
	if (!next_token(reader) || reader->quoted || !reader->token[0] || reader->token[1])
		return DISP_E_EXCEPTION;
	kind = reader->token[0] | 0x20;
	if (!next_token(reader))
		return DISP_E_EXCEPTION;
	if (!reader->registering)
		return S_OK;
	switch (kind)
	{
	case 's':
		return HRESULT_FROM_WIN32(RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)reader->token, (DWORD)(wcslen(reader->token) + 1) * sizeof(WCHAR)));
	case 'd':
	{
		DWORD value = 0;
		const WCHAR *at = reader->token;
		for (; *at >= '0' && *at <= '9'; at++)
			value = value * 10 + (*at - '0');
		return HRESULT_FROM_WIN32(RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value)));
	}
	case 'b':
	{
		size_t size = wcslen(reader->token) / 2, index;
		BYTE *bytes = atl_alloc(size ? size : 1);
		HRESULT status;
		if (!bytes)
			return E_OUTOFMEMORY;
		for (index = 0; index < size; index++)
			bytes[index] = (BYTE)(hex_value(reader->token[index * 2]) << 4 | hex_value(reader->token[index * 2 + 1]));
		status = HRESULT_FROM_WIN32(RegSetValueExW(key, name, 0, REG_BINARY, bytes, (DWORD)size));
		atl_free(bytes);
		return status;
	}
	default:
		return DISP_E_EXCEPTION;
	}
}

static BOOL key_is_empty(HKEY key)
{
	DWORD keys = 1, values = 1;
	if (RegQueryInfoKeyW(key, NULL, NULL, NULL, &keys, NULL, NULL, &values, NULL, NULL, NULL, NULL))
		return FALSE;
	return !keys && !values;
}

/* The entries of one block: the current token starts the first of them, and the closing brace ends the walk. */
static HRESULT walk_block(script *reader, HKEY parent, BOOL parent_exists)
{
	HRESULT status;
	while (reader->token[0] && !token_is(reader, L"}"))
	{
		BOOL force_remove = FALSE, no_remove = FALSE, delete_key = FALSE;
		HKEY key = NULL;
		WCHAR name[256];
		if (token_is(reader, L"val"))
		{
			if (!next_token(reader))
				return DISP_E_EXCEPTION;
			lstrcpynW(name, reader->token, ARRAYSIZE(name));
			if (!next_token(reader) || !token_is(reader, L"="))
				return DISP_E_EXCEPTION;
			if (FAILED(status = write_value(reader, parent, name)))
				return status;
			if (!reader->registering && parent_exists)
				RegDeleteValueW(parent, name);
			next_token(reader);
			continue;
		}
		if (token_is(reader, L"ForceRemove"))
			force_remove = TRUE;
		else if (token_is(reader, L"NoRemove"))
			no_remove = TRUE;
		else if (token_is(reader, L"Delete"))
			delete_key = TRUE;
		if ((force_remove || no_remove || delete_key) && !next_token(reader))
			return DISP_E_EXCEPTION;
		lstrcpynW(name, reader->token, ARRAYSIZE(name));
		if (reader->registering)
		{
			if (force_remove || delete_key)
				atl_delete_tree(parent, name);
			if (!delete_key)
			{
				LONG error = RegCreateKeyExW(parent, name, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL);
				if (error)
					return HRESULT_FROM_WIN32(error);
			}
		}
		else if (parent_exists && RegOpenKeyExW(parent, name, 0, KEY_READ | KEY_WRITE, &key))
			key = NULL;
		next_token(reader);
		if (token_is(reader, L"="))
		{
			if (FAILED(status = write_value(reader, key, NULL)))
				goto failed;
			next_token(reader);
		}
		if (token_is(reader, L"{"))
		{
			next_token(reader);
			if (FAILED(status = walk_block(reader, key, key != NULL)))
				goto failed;
			if (!token_is(reader, L"}"))
			{
				status = DISP_E_EXCEPTION;
				goto failed;
			}
			next_token(reader);
		}
		if (!reader->registering && key)
		{
			if (force_remove)
			{
				RegCloseKey(key);
				key = NULL;
				atl_delete_tree(parent, name);
			}
			else if (!no_remove && !delete_key && key_is_empty(key))
			{
				RegCloseKey(key);
				key = NULL;
				RegDeleteKeyW(parent, name);
			}
		}
		if (key)
			RegCloseKey(key);
		continue;
failed:
		if (key)
			RegCloseKey(key);
		return status;
	}
	return S_OK;
}

static BOOL root_of(const WCHAR *name, HKEY *out)
{
	static const struct
	{
		const WCHAR *names[2];
		HKEY key;
	} roots[] = {
		{{L"HKCR", L"HKEY_CLASSES_ROOT"}, HKEY_CLASSES_ROOT},
		{{L"HKCU", L"HKEY_CURRENT_USER"}, HKEY_CURRENT_USER},
		{{L"HKLM", L"HKEY_LOCAL_MACHINE"}, HKEY_LOCAL_MACHINE},
		{{L"HKU", L"HKEY_USERS"}, HKEY_USERS},
		{{L"HKPD", L"HKEY_PERFORMANCE_DATA"}, HKEY_PERFORMANCE_DATA},
		{{L"HKDD", L"HKEY_DYN_DATA"}, HKEY_DYN_DATA},
		{{L"HKCC", L"HKEY_CURRENT_CONFIG"}, HKEY_CURRENT_CONFIG},
	};
	UINT index;
	for (index = 0; index < ARRAYSIZE(roots); index++)
		if (!lstrcmpiW(roots[index].names[0], name) || !lstrcmpiW(roots[index].names[1], name))
		{
			*out = roots[index].key;
			return TRUE;
		}
	return FALSE;
}

static HRESULT run_script(atl_registrar *self, const WCHAR *text, BOOL registering)
{
	script reader;
	WCHAR *expanded;
	HRESULT status;
	if (!text)
		return E_INVALIDARG;
	if (FAILED(status = expand(self, text, &expanded)))
		return status;
	reader.at = expanded;
	reader.registering = registering;
	next_token(&reader);
	while (SUCCEEDED(status) && reader.token[0])
	{
		HKEY root, opened = NULL;
		if (!root_of(reader.token, &root))
		{
			status = DISP_E_EXCEPTION;
			break;
		}
		if (atl_per_user && root == HKEY_CLASSES_ROOT && !RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &opened, NULL))
			root = opened;
		next_token(&reader);
		if (!token_is(&reader, L"{"))
			status = DISP_E_EXCEPTION;
		else
		{
			next_token(&reader);
			status = walk_block(&reader, root, TRUE);
			if (SUCCEEDED(status) && !token_is(&reader, L"}"))
				status = DISP_E_EXCEPTION;
			next_token(&reader);
		}
		if (opened)
			RegCloseKey(opened);
	}
	atl_free(expanded);
	return status;
}

static WCHAR *wide_of_bytes(const char *bytes, DWORD size)
{
	int length;
	WCHAR *text;
	if (size >= 2 && (BYTE)bytes[0] == 0xff && (BYTE)bytes[1] == 0xfe)
	{
		if (!(text = atl_alloc(size + sizeof(WCHAR))))
			return NULL;
		memcpy(text, bytes + 2, size - 2);
		text[(size - 2) / 2] = 0;
		return text;
	}
	length = MultiByteToWideChar(CP_ACP, 0, bytes, size, NULL, 0);
	if (!(text = atl_alloc((length + 1) * sizeof(WCHAR))))
		return NULL;
	MultiByteToWideChar(CP_ACP, 0, bytes, size, text, length);
	text[length] = 0;
	return text;
}

static HRESULT run_file(atl_registrar *self, const WCHAR *path, BOOL registering)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	DWORD size, got = 0;
	char *bytes;
	WCHAR *text;
	HRESULT status = E_OUTOFMEMORY;
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());
	size = GetFileSize(file, NULL);
	if ((bytes = atl_alloc(size + 1)))
	{
		if (ReadFile(file, bytes, size, &got, NULL) && (text = wide_of_bytes(bytes, got)))
		{
			status = run_script(self, text, registering);
			atl_free(text);
		}
		atl_free(bytes);
	}
	CloseHandle(file);
	return status;
}

static HRESULT run_resource(atl_registrar *self, const WCHAR *module, const WCHAR *name, const WCHAR *type, BOOL registering)
{
	HMODULE image = LoadLibraryExW(module, NULL, LOAD_LIBRARY_AS_DATAFILE);
	HRSRC found;
	HRESULT status = HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);
	if (!image)
		return HRESULT_FROM_WIN32(GetLastError());
	if ((found = FindResourceW(image, name, type)))
	{
		HGLOBAL loaded = LoadResource(image, found);
		WCHAR *text = loaded ? wide_of_bytes(LockResource(loaded), SizeofResource(image, found)) : NULL;
		if (text)
		{
			status = run_script(self, text, registering);
			atl_free(text);
		}
	}
	FreeLibrary(image);
	return status;
}

static HRESULT WINAPI registrar_QueryInterface(atl_registrar *self, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IRegistrar))
	{
		*out = self;
		InterlockedIncrement(&self->refs);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI registrar_AddRef(atl_registrar *self)
{
	return InterlockedIncrement(&self->refs);
}

static HRESULT WINAPI registrar_ClearReplacements(atl_registrar *self)
{
	while (self->replacements)
	{
		replacement *entry = self->replacements;
		self->replacements = entry->next;
		atl_free(entry->key);
		atl_free(entry->value);
		atl_free(entry);
	}
	return S_OK;
}

static ULONG WINAPI registrar_Release(atl_registrar *self)
{
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		registrar_ClearReplacements(self);
		atl_free(self);
	}
	return left;
}

static WCHAR *duplicate(const WCHAR *text)
{
	size_t size = (wcslen(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = atl_alloc(size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

static HRESULT WINAPI registrar_AddReplacement(atl_registrar *self, const WCHAR *key, const WCHAR *value)
{
	replacement *entry;
	if (!key || !value)
		return E_INVALIDARG;
	if (!(entry = atl_alloc(sizeof(*entry))))
		return E_OUTOFMEMORY;
	entry->key = duplicate(key);
	entry->value = duplicate(value);
	entry->next = self->replacements;
	self->replacements = entry;
	return entry->key && entry->value ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI registrar_ResourceRegisterSz(atl_registrar *self, const WCHAR *module, const WCHAR *name, const WCHAR *type)
{
	return run_resource(self, module, name, type, TRUE);
}

static HRESULT WINAPI registrar_ResourceUnregisterSz(atl_registrar *self, const WCHAR *module, const WCHAR *name, const WCHAR *type)
{
	return run_resource(self, module, name, type, FALSE);
}

static HRESULT WINAPI registrar_FileRegister(atl_registrar *self, const WCHAR *path)
{
	return run_file(self, path, TRUE);
}

static HRESULT WINAPI registrar_FileUnregister(atl_registrar *self, const WCHAR *path)
{
	return run_file(self, path, FALSE);
}

static HRESULT WINAPI registrar_StringRegister(atl_registrar *self, const WCHAR *text)
{
	return run_script(self, text, TRUE);
}

static HRESULT WINAPI registrar_StringUnregister(atl_registrar *self, const WCHAR *text)
{
	return run_script(self, text, FALSE);
}

static HRESULT WINAPI registrar_ResourceRegister(atl_registrar *self, const WCHAR *module, UINT id, const WCHAR *type)
{
	return run_resource(self, module, MAKEINTRESOURCEW(id), type, TRUE);
}

static HRESULT WINAPI registrar_ResourceUnregister(atl_registrar *self, const WCHAR *module, UINT id, const WCHAR *type)
{
	return run_resource(self, module, MAKEINTRESOURCEW(id), type, FALSE);
}

static const atl_registrar_vtbl registrar_vtbl = {
	registrar_QueryInterface,
	registrar_AddRef,
	registrar_Release,
	registrar_AddReplacement,
	registrar_ClearReplacements,
	registrar_ResourceRegisterSz,
	registrar_ResourceUnregisterSz,
	registrar_FileRegister,
	registrar_FileUnregister,
	registrar_StringRegister,
	registrar_StringUnregister,
	registrar_ResourceRegister,
	registrar_ResourceUnregister,
};

HRESULT atl_registrar_create(REFIID riid, void **out)
{
	atl_registrar *self = atl_alloc(sizeof(*self));
	HRESULT status;
	if (!self)
		return E_OUTOFMEMORY;
	self->vtbl = &registrar_vtbl;
	self->refs = 1;
	self->replacements = NULL;
	status = registrar_QueryInterface(self, riid, out);
	registrar_Release(self);
	return status;
}

HRESULT WINAPI AtlCreateRegistrar(void **out)
{
	return atl_registrar_create(&IID_IRegistrar, out);
}

HRESULT WINAPI AtlUpdateRegistryFromResourceD(HINSTANCE instance, const WCHAR *name, BOOL registering, const atl_replacement *map, atl_registrar *given)
{
	WCHAR module[MAX_PATH];
	atl_registrar *registrar = given;
	HRESULT status = S_OK;
	if (!name)
		return E_INVALIDARG;
	if (!GetModuleFileNameW(instance, module, ARRAYSIZE(module)))
		return HRESULT_FROM_WIN32(GetLastError());
	if (!registrar && FAILED(status = atl_registrar_create(&IID_IRegistrar, (void **)&registrar)))
		return status;
	registrar->vtbl->AddReplacement(registrar, L"MODULE", module);
	for (; map && map->key; map++)
		registrar->vtbl->AddReplacement(registrar, map->key, map->value);
	status = run_resource(registrar, module, name, L"REGISTRY", registering);
	if (given)
		registrar->vtbl->ClearReplacements(registrar);
	else
		registrar->vtbl->Release(registrar);
	return status;
}
