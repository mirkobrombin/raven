/* SPDX-License-Identifier: MIT */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <msxml2.h>
#include <initguid.h>
#include <fusion.h>

#define LENGTH_LIMIT 140

/* --- An assembly identity: a name and attributes kept in order of their names. --- */

typedef struct attribute
{
	struct attribute *next;
	WCHAR *key, *value;
} attribute;

typedef struct
{
	WCHAR *name;
	attribute *attributes;
} identity;

static void *memory(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *copy_counted(const WCHAR *text, SIZE_T length)
{
	WCHAR *copy = memory((length + 1) * sizeof(WCHAR));
	if (copy)
		memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

static WCHAR *copy_text(const WCHAR *text)
{
	return copy_counted(text, lstrlenW(text));
}

static void identity_clear(identity *self)
{
	attribute *at;
	while ((at = self->attributes))
	{
		self->attributes = at->next;
		release(at->key);
		release(at->value);
		release(at);
	}
	release(self->name);
	self->name = NULL;
}

static const WCHAR *identity_get(const identity *self, const WCHAR *key)
{
	const attribute *at;
	for (at = self->attributes; at; at = at->next)
		if (!lstrcmpiW(at->key, key))
			return at->value;
	return NULL;
}

static int digit_value(WCHAR c, int base)
{
	int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
	return value < base ? value : -1;
}

/* Reads the one reference at the start of the text. Text the format does not know reads as nothing, unless it is long
 * enough to be meant as a name. FALSE is a reference that cannot be read at all; *out stays 0 when it reads as nothing. */
static BOOL read_reference(const WCHAR *text, const WCHAR *end, WCHAR *out, const WCHAR **after)
{
	static const struct { const WCHAR *name; WCHAR value; } named[] = {
		{L"lt", '<'}, {L"gt", '>'}, {L"amp", '&'}, {L"quot", '"'}, {L"apos", '\''},
	};
	const WCHAR *close = text + 1;
	unsigned index;
	*out = 0;
	while (close < end && *close != ';')
		close++;
	if (close >= end)
	{
		*after = end;
		return TRUE;
	}
	*after = close + 1;
	if (text[1] == '#')
	{
		const WCHAR *digit = text + 2;
		int base = 10;
		unsigned value = 0;
		if (*digit == 'x' || *digit == 'X')
		{
			base = 16;
			digit++;
		}
		for (; digit < close; digit++)
		{
			int part = digit_value(*digit, base);
			if (part < 0)
				return FALSE;
			value = value * base + part;
		}
		*out = (WCHAR)value;
		return TRUE;
	}
	for (index = 0; index < ARRAYSIZE(named); index++)
		if ((int)(close - text - 1) == lstrlenW(named[index].name) && !memcmp(text + 1, named[index].name, (close - text - 1) * sizeof(WCHAR)))
		{
			*out = named[index].value;
			return TRUE;
		}
	return close - text - 1 <= 1;
}

/* A character reference of 0 ends the value: what follows it up to the end of the part is gone. */
static WCHAR *decode_part(const WCHAR *text, const WCHAR *end)
{
	WCHAR *result = memory((end - text + 1) * sizeof(WCHAR));
	WCHAR *to = result;
	if (!result)
		return NULL;
	while (text < end)
	{
		if (*text == '&')
		{
			WCHAR value;
			const WCHAR *after;
			BOOL zero = text[1] == '#';
			if (!read_reference(text, end, &value, &after))
			{
				release(result);
				return NULL;
			}
			if (zero && !value && after > text + 2)
				break;
			if (value)
				*to++ = value;
			text = after;
		}
		else
			*to++ = *text++;
	}
	return result;
}

static const WCHAR *find_comma(const WCHAR *text)
{
	while (*text && *text != ',')
		text++;
	return text;
}

static void insert_sorted(identity *self, attribute *item)
{
	attribute **at = &self->attributes;
	while (*at && lstrcmpiW((*at)->key, item->key) < 0)
		at = &(*at)->next;
	item->next = *at;
	*at = item;
}

static HRESULT identity_parse(const WCHAR *text, identity *self)
{
	const WCHAR *end = find_comma(text);
	ZeroMemory(self, sizeof(*self));
	if (end == text)
		return E_INVALIDARG;
	self->name = decode_part(text, end);
	if (!self->name)
		return E_INVALIDARG;
	while (*end)
	{
		const WCHAR *key = end + 1, *equals, *close;
		attribute *item;
		for (equals = key; *equals && *equals != '='; equals++)
			if (*equals == ' ')
				goto bad_name;
		if (!*equals || equals == key)
			goto invalid;
		if (equals[1] != '"')
			goto invalid;
		for (close = equals + 2; *close && *close != '"'; close++)
			;
		if (!*close)
			goto invalid;
		item = memory(sizeof(*item));
		if (!item)
			goto invalid;
		item->key = copy_counted(key, equals - key);
		item->value = decode_part(equals + 2, close);
		if (!item->key || !item->value)
		{
			release(item->key);
			release(item->value);
			release(item);
			goto invalid;
		}
		insert_sorted(self, item);
		end = close + 1;
		if (*end && *end != ',')
			goto invalid;
	}
	return S_OK;
bad_name:
	identity_clear(self);
	return HRESULT_FROM_WIN32(ERROR_SXS_INVALID_ASSEMBLY_IDENTITY_ATTRIBUTE_NAME);
invalid:
	identity_clear(self);
	return E_INVALIDARG;
}

/* Writes to `out` when it is given; the count returned excludes the terminator. */
static SIZE_T encode_text(const WCHAR *text, WCHAR *out)
{
	static const struct { WCHAR value; const WCHAR *text; } named[] = {
		{'<', L"&lt;"}, {'>', L"&gt;"}, {'&', L"&amp;"}, {'"', L"&quot;"}, {'\'', L"&apos;"},
	};
	SIZE_T count = 0;
	for (; *text; text++)
	{
		WCHAR c = *text;
		WCHAR piece[16];
		int size = 0;
		unsigned index;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')
			piece[size++] = c;
		else
		{
			const WCHAR *word = NULL;
			for (index = 0; index < ARRAYSIZE(named); index++)
				if (named[index].value == c)
					word = named[index].text;
			size = word ? lstrlenW(lstrcpyW(piece, word)) : wsprintfW(piece, L"&#x%x;", c);
		}
		if (out)
			memcpy(out + count, piece, size * sizeof(WCHAR));
		count += size;
	}
	return count;
}

static SIZE_T identity_display(const identity *self, WCHAR *out)
{
	const attribute *at;
	SIZE_T count = encode_text(self->name, out);
	for (at = self->attributes; at; at = at->next)
	{
		SIZE_T key = lstrlenW(at->key);
		if (out)
		{
			out[count] = ',';
			memcpy(out + count + 1, at->key, key * sizeof(WCHAR));
			out[count + 1 + key] = '=';
			out[count + 2 + key] = '"';
		}
		count += key + 3;
		count += encode_text(at->value, out ? out + count : NULL);
		if (out)
			out[count] = '"';
		count++;
	}
	if (out)
		out[count] = 0;
	return count;
}

/* --- IAssemblyName. --- */

HRESULT WINAPI CreateAssemblyNameObject(IAssemblyName **out, LPCWSTR text, DWORD flags, LPVOID reserved);

typedef struct
{
	const IAssemblyNameVtbl *vtbl;
	LONG refs;
	identity id;
} name_object;

static HRESULT WINAPI name_QueryInterface(IAssemblyName *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IAssemblyName))
	{
		*out = iface;
		IAssemblyName_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI name_AddRef(IAssemblyName *iface)
{
	return InterlockedIncrement(&((name_object *)iface)->refs);
}

static ULONG WINAPI name_Release(IAssemblyName *iface)
{
	name_object *self = (name_object *)iface;
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		identity_clear(&self->id);
		release(self);
	}
	return left;
}

static HRESULT WINAPI name_SetProperty(IAssemblyName *iface, DWORD property, void *value, DWORD size)
{
	return E_NOTIMPL;
}

static void version_parts(const WCHAR *text, WORD parts[4])
{
	int index = 0;
	parts[0] = parts[1] = parts[2] = parts[3] = 0;
	for (; text && *text && index < 4; text++)
	{
		if (*text == '.')
			index++;
		else if (*text >= '0' && *text <= '9')
			parts[index] = parts[index] * 10 + (*text - '0');
	}
}

static HRESULT give_bytes(const void *value, DWORD size, void *out, DWORD *room)
{
	if (!room)
		return E_INVALIDARG;
	if (*room < size || !out)
	{
		*room = size;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(out, value, size);
	*room = size;
	return S_OK;
}

static HRESULT WINAPI name_GetProperty(IAssemblyName *iface, DWORD property, void *out, DWORD *room)
{
	const identity *id = &((name_object *)iface)->id;
	const WCHAR *text = NULL;
	if (property >= ASM_NAME_MAJOR_VERSION && property <= ASM_NAME_REVISION_NUMBER)
	{
		WORD parts[4];
		const WCHAR *version = identity_get(id, L"version");
		if (!version)
		{
			if (room)
				*room = 0;
			return S_OK;
		}
		version_parts(version, parts);
		return give_bytes(&parts[property - ASM_NAME_MAJOR_VERSION], sizeof(WORD), out, room);
	}
	if (property == ASM_NAME_NAME)
		text = id->name;
	else if (property == ASM_NAME_CULTURE)
		text = identity_get(id, L"language");
	else if (property == ASM_NAME_PUBLIC_KEY_TOKEN)
		text = identity_get(id, L"publicKeyToken");
	if (!text)
	{
		if (room)
			*room = 0;
		return S_OK;
	}
	return give_bytes(text, (lstrlenW(text) + 1) * sizeof(WCHAR), out, room);
}

static HRESULT WINAPI name_Finalize(IAssemblyName *iface)
{
	return S_OK;
}

static HRESULT WINAPI name_GetDisplayName(IAssemblyName *iface, WCHAR *out, DWORD *room, DWORD flags)
{
	const identity *id = &((name_object *)iface)->id;
	DWORD needed;
	if (!room)
		return E_INVALIDARG;
	needed = (DWORD)identity_display(id, NULL) + 1;
	if (!out || *room < needed)
	{
		*room = needed;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	identity_display(id, out);
	*room = needed;
	return S_OK;
}

static HRESULT WINAPI name_Reserved(IAssemblyName *iface, REFIID riid, IUnknown *first, IUnknown *second, const WCHAR *text,
	LONGLONG number, void *pointer, DWORD size, void **out)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI name_GetName(IAssemblyName *iface, DWORD *room, WCHAR *out)
{
	const identity *id = &((name_object *)iface)->id;
	DWORD needed;
	if (!room)
		return E_INVALIDARG;
	needed = lstrlenW(id->name) + 1;
	if (!out || *room < needed)
	{
		*room = needed;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	lstrcpyW(out, id->name);
	*room = needed;
	return S_OK;
}

static HRESULT WINAPI name_GetVersion(IAssemblyName *iface, DWORD *high, DWORD *low)
{
	const WCHAR *version = identity_get(&((name_object *)iface)->id, L"version");
	WORD parts[4];
	if (!high || !low)
		return E_INVALIDARG;
	if (!version)
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	version_parts(version, parts);
	*high = MAKELONG(parts[1], parts[0]);
	*low = MAKELONG(parts[3], parts[2]);
	return S_OK;
}

static HRESULT WINAPI name_IsEqual(IAssemblyName *iface, IAssemblyName *other, DWORD flags)
{
	WCHAR *mine, *theirs;
	DWORD size_mine = 0, size_theirs = 0;
	HRESULT status = S_FALSE;
	if (!other)
		return E_INVALIDARG;
	IAssemblyName_GetDisplayName(iface, NULL, &size_mine, 0);
	IAssemblyName_GetDisplayName(other, NULL, &size_theirs, 0);
	mine = memory(size_mine * sizeof(WCHAR));
	theirs = memory(size_theirs * sizeof(WCHAR));
	if (mine && theirs && SUCCEEDED(IAssemblyName_GetDisplayName(iface, mine, &size_mine, 0))
		&& SUCCEEDED(IAssemblyName_GetDisplayName(other, theirs, &size_theirs, 0)) && !lstrcmpiW(mine, theirs))
		status = S_OK;
	release(mine);
	release(theirs);
	return status;
}

static HRESULT WINAPI name_Clone(IAssemblyName *iface, IAssemblyName **out)
{
	WCHAR *display;
	DWORD size = 0;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	IAssemblyName_GetDisplayName(iface, NULL, &size, 0);
	display = memory(size * sizeof(WCHAR));
	if (!display)
		return E_OUTOFMEMORY;
	IAssemblyName_GetDisplayName(iface, display, &size, 0);
	status = CreateAssemblyNameObject(out, display, CANOF_PARSE_DISPLAY_NAME, NULL);
	release(display);
	return status;
}

static const IAssemblyNameVtbl name_vtbl = {
	name_QueryInterface,
	name_AddRef,
	name_Release,
	name_SetProperty,
	name_GetProperty,
	name_Finalize,
	name_GetDisplayName,
	name_Reserved,
	name_GetName,
	name_GetVersion,
	name_IsEqual,
	name_Clone,
};

HRESULT WINAPI CreateAssemblyNameObject(IAssemblyName **out, LPCWSTR text, DWORD flags, LPVOID reserved)
{
	name_object *self;
	HRESULT status;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!text || !*text || flags != CANOF_PARSE_DISPLAY_NAME)
		return E_INVALIDARG;
	self = memory(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	status = identity_parse(text, &self->id);
	if (FAILED(status))
	{
		release(self);
		return status;
	}
	self->vtbl = &name_vtbl;
	self->refs = 1;
	*out = (IAssemblyName *)self;
	return S_OK;
}

/* --- Where an assembly lives. --- */

static WCHAR lower_ascii(WCHAR c)
{
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/* Lower case, cut in the middle when longer than `limit`, with `side` characters kept at each end. */
static SIZE_T put_short(const WCHAR *text, int limit, int side, WCHAR *out)
{
	int length = lstrlenW(text), index;
	SIZE_T count = 0;
	if (length <= limit)
	{
		for (index = 0; index < length; index++)
			out[count++] = lower_ascii(text[index]);
		return count;
	}
	for (index = 0; index < side; index++)
		out[count++] = lower_ascii(text[index]);
	out[count++] = '.';
	out[count++] = '.';
	for (index = length - side; index < length; index++)
		out[count++] = lower_ascii(text[index]);
	return count;
}

static SIZE_T put_plain(const WCHAR *text, WCHAR *out)
{
	int length = lstrlenW(text);
	memcpy(out, text, length * sizeof(WCHAR));
	return length;
}

/* The directory name of an assembly under the side-by-side store: a part given empty leaves no trace, a wildcard leaves an
 * empty place, and only letters, digits, dashes and dots of the name survive. */
static WCHAR *manifest_name(const identity *id)
{
	const WCHAR *arch = identity_get(id, L"processorArchitecture");
	const WCHAR *token = identity_get(id, L"publicKeyToken");
	const WCHAR *version = identity_get(id, L"version");
	const WCHAR *language = identity_get(id, L"language");
	SIZE_T room = lstrlenW(id->name) + (arch ? lstrlenW(arch) : 4) + (token ? lstrlenW(token) : 4) + lstrlenW(version)
		+ (language ? lstrlenW(language) : 4) + 32;
	WCHAR *result = memory(room * sizeof(WCHAR));
	WCHAR *filtered = memory((lstrlenW(id->name) + 1) * sizeof(WCHAR));
	SIZE_T count = 0, kept = 0;
	const WCHAR *at;
	if (!result || !filtered)
	{
		release(result);
		release(filtered);
		return NULL;
	}
	for (at = id->name; *at; at++)
		if ((*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') || (*at >= '0' && *at <= '9') || *at == '-' || *at == '.')
			filtered[kept++] = *at;
	if (!arch || *arch)
	{
		if (arch && lstrcmpW(arch, L"*"))
			count += put_short(arch, 16, 7, result + count);
		else if (!arch)
			count += put_plain(L"none", result + count);
		result[count++] = '_';
	}
	count += put_short(filtered, 40, 19, result + count);
	release(filtered);
	result[count++] = '_';
	count += put_plain(token ? token : L"none", result + count);
	result[count++] = '_';
	count += put_plain(version, result + count);
	if (!language || *language)
	{
		result[count++] = '_';
		if (language && lstrcmpW(language, L"*"))
			count += put_short(language, 8, 3, result + count);
		else if (!language)
			count += put_plain(L"none", result + count);
	}
	result[count++] = '_';
	{
		/* A digest of the parts, so that names cut short stay apart. */
		ULONGLONG digest = 0xcbf29ce484222325ull;
		SIZE_T index;
		for (index = 0; index < count; index++)
			digest = (digest ^ result[index]) * 0x100000001b3ull;
		count += wsprintfW(result + count, L"%08x%08x", (DWORD)(digest >> 32), (DWORD)digest);
	}
	result[count] = 0;
	return result;
}

BOOL WINAPI SxspGenerateManifestPathOnAssemblyIdentity(const WCHAR *text, WCHAR *out, DWORD *room, void *reserved)
{
	identity id;
	WCHAR *path;
	DWORD needed;
	if (!text || !room || FAILED(identity_parse(text, &id)))
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!identity_get(&id, L"version"))
	{
		identity_clear(&id);
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	path = manifest_name(&id);
	identity_clear(&id);
	if (!path)
	{
		SetLastError(ERROR_OUTOFMEMORY);
		return FALSE;
	}
	needed = lstrlenW(path) + 1;
	if (needed - 1 > LENGTH_LIMIT)
	{
		release(path);
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (!out || *room < needed)
	{
		*room = needed;
		release(path);
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	lstrcpyW(out, path);
	release(path);
	return TRUE;
}

/* The store itself: the names the runtime's activation contexts search. */
static const WCHAR store_suffix[] = L"_none_deadbeef";

typedef struct
{
	WCHAR *type, *name, *version, *arch, *token;
} store_identity;

static void store_clear(store_identity *self)
{
	release(self->type);
	release(self->name);
	release(self->version);
	release(self->arch);
	release(self->token);
	ZeroMemory(self, sizeof(*self));
}

static BOOL safe_part(const WCHAR *text)
{
	for (; *text; text++)
		if (*text == '\\' || *text == '/' || *text == ':' || *text == '*' || *text == '?')
			return FALSE;
	return TRUE;
}

static WCHAR *lowered_copy(const WCHAR *text)
{
	WCHAR *copy = copy_text(text);
	WCHAR *at;
	if (copy)
		for (at = copy; *at; at++)
			*at = lower_ascii(*at);
	return copy;
}

/* `<windows>\winsxs\` followed by what is given, in order. NULL pieces are skipped. */
static WCHAR *store_path(const WCHAR *first, const WCHAR *second, const WCHAR *third, const WCHAR *fourth, const WCHAR *fifth,
	const WCHAR *sixth, const WCHAR *seventh, const WCHAR *eighth, const WCHAR *ninth)
{
	const WCHAR *pieces[] = {first, second, third, fourth, fifth, sixth, seventh, eighth, ninth};
	WCHAR windows[MAX_PATH];
	SIZE_T length;
	WCHAR *path;
	unsigned index;
	UINT base = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
	if (!base || base >= ARRAYSIZE(windows))
		return NULL;
	length = base + 16;
	for (index = 0; index < ARRAYSIZE(pieces); index++)
		if (pieces[index])
			length += lstrlenW(pieces[index]);
	path = memory(length * sizeof(WCHAR));
	if (!path)
		return NULL;
	lstrcpyW(path, windows);
	lstrcatW(path, L"\\winsxs\\");
	for (index = 0; index < ARRAYSIZE(pieces); index++)
		if (pieces[index])
			lstrcatW(path, pieces[index]);
	return path;
}

static WCHAR *assembly_directory(const store_identity *id)
{
	return store_path(id->arch, L"_", id->name, L"_", id->token, L"_", id->version, store_suffix, L"\\");
}

static WCHAR *manifest_file(const store_identity *id)
{
	WCHAR *directory = store_path(L"manifests\\", id->arch, L"_", id->name, L"_", id->token, L"_", id->version, store_suffix);
	WCHAR *path = NULL;
	if (directory)
	{
		path = memory((lstrlenW(directory) + 16) * sizeof(WCHAR));
		if (path)
		{
			lstrcpyW(path, directory);
			lstrcatW(path, L".manifest");
		}
		release(directory);
	}
	return path;
}

static WCHAR *policy_file(const store_identity *id)
{
	WCHAR *directory = store_path(L"policies\\", id->arch, L"_", id->name, L"_", id->token, store_suffix, L"\\", id->version);
	WCHAR *path = NULL;
	if (directory)
	{
		path = memory((lstrlenW(directory) + 16) * sizeof(WCHAR));
		if (path)
		{
			lstrcpyW(path, directory);
			lstrcatW(path, L".policy");
		}
		release(directory);
	}
	return path;
}

/* Makes every directory above a file path, or the path itself when it ends in a separator. */
static void make_directories(const WCHAR *path)
{
	WCHAR *copy = copy_text(path);
	WCHAR *at;
	if (!copy)
		return;
	for (at = copy + 3; *at; at++)
		if (*at == '\\')
		{
			*at = 0;
			CreateDirectoryW(copy, NULL);
			*at = '\\';
		}
	release(copy);
}

/* --- The manifest being installed. --- */

static const GUID clsid_dom_document30 = {0xf5078f32, 0xc551, 0x11d3, {0x89, 0xb9, 0x00, 0x00, 0xf8, 0x1f, 0xe2, 0x21}};

static WCHAR *attribute_of(IXMLDOMNode *node, const WCHAR *key)
{
	IXMLDOMNamedNodeMap *map = NULL;
	IXMLDOMNode *found = NULL;
	BSTR name = SysAllocString(key);
	WCHAR *result = NULL;
	if (name && SUCCEEDED(IXMLDOMNode_get_attributes(node, &map)) && map
		&& IXMLDOMNamedNodeMap_getNamedItem(map, name, &found) == S_OK && found)
	{
		BSTR text = NULL;
		if (SUCCEEDED(IXMLDOMNode_get_text(found, &text)) && text)
		{
			result = copy_text(text);
			SysFreeString(text);
		}
		IXMLDOMNode_Release(found);
	}
	if (map)
		IXMLDOMNamedNodeMap_Release(map);
	SysFreeString(name);
	return result;
}

static HRESULT open_manifest(const WCHAR *path, IXMLDOMDocument **out)
{
	IXMLDOMDocument *document;
	VARIANT source;
	VARIANT_BOOL loaded = VARIANT_FALSE;
	HRESULT status = CoCreateInstance(&clsid_dom_document30, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument, (void **)&document);
	if (FAILED(status))
		return status;
	IXMLDOMDocument_put_async(document, VARIANT_FALSE);
	IXMLDOMDocument_put_resolveExternals(document, VARIANT_FALSE);
	IXMLDOMDocument_put_validateOnParse(document, VARIANT_FALSE);
	V_VT(&source) = VT_BSTR;
	V_BSTR(&source) = SysAllocString(path);
	status = IXMLDOMDocument_load(document, source, &loaded);
	VariantClear(&source);
	if (FAILED(status) || !loaded)
	{
		IXMLDOMDocument_Release(document);
		return FAILED(status) ? status : HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_FORMAT_ERROR);
	}
	*out = document;
	return S_OK;
}

static HRESULT nodes_named(IXMLDOMDocument *document, const WCHAR *tag, IXMLDOMNodeList **out)
{
	BSTR name = SysAllocString(tag);
	HRESULT status = name ? IXMLDOMDocument_getElementsByTagName(document, name, out) : E_OUTOFMEMORY;
	SysFreeString(name);
	return status;
}

/* The identity at the head of a manifest. Dependencies carry their own further down, so the first is the one. */
static HRESULT read_identity(IXMLDOMDocument *document, store_identity *id)
{
	IXMLDOMNodeList *list = NULL;
	IXMLDOMNode *node = NULL;
	WCHAR *name = NULL;
	HRESULT status = nodes_named(document, L"assemblyIdentity", &list);
	ZeroMemory(id, sizeof(*id));
	if (FAILED(status))
		return status;
	if (IXMLDOMNodeList_get_item(list, 0, &node) != S_OK || !node)
	{
		IXMLDOMNodeList_Release(list);
		return HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_FORMAT_ERROR);
	}
	IXMLDOMNodeList_Release(list);
	id->type = attribute_of(node, L"type");
	name = attribute_of(node, L"name");
	id->version = attribute_of(node, L"version");
	id->arch = attribute_of(node, L"processorArchitecture");
	id->token = attribute_of(node, L"publicKeyToken");
	IXMLDOMNode_Release(node);
	if (name)
	{
		id->name = lowered_copy(name);
		release(name);
	}
	if (id->arch)
	{
		WCHAR *lowered = lowered_copy(id->arch);
		release(id->arch);
		id->arch = lowered;
	}
	if (!id->type || !id->name || !id->version || !id->arch || !id->token || !safe_part(id->name) || !safe_part(id->version)
		|| !safe_part(id->arch) || !safe_part(id->token) || (lstrcmpW(id->type, L"win32") && lstrcmpW(id->type, L"win32-policy")))
	{
		store_clear(id);
		return HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_FORMAT_ERROR);
	}
	return S_OK;
}

/* Runs through the files a manifest lists. */
typedef HRESULT (*file_action)(const WCHAR *name, void *context);

static HRESULT for_each_file(IXMLDOMDocument *document, file_action action, void *context)
{
	IXMLDOMNodeList *list = NULL;
	IXMLDOMNode *node;
	HRESULT status = nodes_named(document, L"file", &list);
	if (FAILED(status))
		return status;
	while (SUCCEEDED(status) && IXMLDOMNodeList_nextNode(list, &node) == S_OK && node)
	{
		WCHAR *name = attribute_of(node, L"name");
		IXMLDOMNode_Release(node);
		if (name)
			status = safe_part(name) ? action(name, context) : HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_FORMAT_ERROR);
		release(name);
	}
	IXMLDOMNodeList_Release(list);
	return status;
}

typedef struct
{
	const WCHAR *source, *target;
} copy_job;

static HRESULT copy_listed_file(const WCHAR *name, void *context)
{
	const copy_job *job = context;
	WCHAR *from = memory((lstrlenW(job->source) + lstrlenW(name) + 1) * sizeof(WCHAR));
	WCHAR *to = memory((lstrlenW(job->target) + lstrlenW(name) + 1) * sizeof(WCHAR));
	HRESULT status = S_OK;
	if (from && to)
	{
		lstrcpyW(from, job->source);
		lstrcatW(from, name);
		lstrcpyW(to, job->target);
		lstrcatW(to, name);
		if (!CopyFileW(from, to, FALSE))
			status = HRESULT_FROM_WIN32(GetLastError());
	}
	else
		status = E_OUTOFMEMORY;
	release(from);
	release(to);
	return status;
}

static HRESULT delete_listed_file(const WCHAR *name, void *context)
{
	const WCHAR *directory = context;
	WCHAR *path = memory((lstrlenW(directory) + lstrlenW(name) + 1) * sizeof(WCHAR));
	if (!path)
		return E_OUTOFMEMORY;
	lstrcpyW(path, directory);
	lstrcatW(path, name);
	DeleteFileW(path);
	release(path);
	return S_OK;
}

/* --- IAssemblyCache. --- */

typedef struct
{
	const IAssemblyCacheVtbl *vtbl;
	LONG refs;
	HANDLE lock;
} cache_object;

static void cache_lock(cache_object *self)
{
	WaitForSingleObject(self->lock, INFINITE);
}

static void cache_unlock(cache_object *self)
{
	ReleaseMutex(self->lock);
}

static HRESULT WINAPI cache_QueryInterface(IAssemblyCache *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IAssemblyCache))
	{
		*out = iface;
		IAssemblyCache_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI cache_AddRef(IAssemblyCache *iface)
{
	return InterlockedIncrement(&((cache_object *)iface)->refs);
}

static ULONG WINAPI cache_Release(IAssemblyCache *iface)
{
	cache_object *self = (cache_object *)iface;
	LONG left = InterlockedDecrement(&self->refs);
	if (!left)
	{
		CloseHandle(self->lock);
		release(self);
	}
	return left;
}

/* A display name reads into store terms once it says which assembly it is, all the way. */
static HRESULT identity_for_store(const WCHAR *text, store_identity *out)
{
	identity id;
	const WCHAR *type, *version, *arch, *token;
	HRESULT status;
	ZeroMemory(out, sizeof(*out));
	if (!text || !*text)
		return E_INVALIDARG;
	status = identity_parse(text, &id);
	if (FAILED(status))
		return status;
	type = identity_get(&id, L"type");
	version = identity_get(&id, L"version");
	arch = identity_get(&id, L"processorArchitecture");
	token = identity_get(&id, L"publicKeyToken");
	if (!type || !version || !arch || !token)
	{
		identity_clear(&id);
		return token || !(type && version && arch) ? HRESULT_FROM_WIN32(ERROR_SXS_MISSING_ASSEMBLY_IDENTITY_ATTRIBUTE)
			: HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	if (!safe_part(id.name) || !safe_part(version) || !safe_part(arch) || !safe_part(token))
	{
		identity_clear(&id);
		return E_INVALIDARG;
	}
	out->type = copy_text(type);
	out->name = lowered_copy(id.name);
	out->version = copy_text(version);
	out->arch = lowered_copy(arch);
	out->token = copy_text(token);
	identity_clear(&id);
	return S_OK;
}

static HRESULT WINAPI cache_QueryAssemblyInfo(IAssemblyCache *iface, DWORD flags, LPCWSTR text, ASSEMBLY_INFO *info)
{
	store_identity id;
	WCHAR *path;
	DWORD attributes;
	HRESULT status;
	if (flags)
		return E_INVALIDARG;
	status = identity_for_store(text, &id);
	if (FAILED(status))
		return status;
	path = lstrcmpW(id.type, L"win32") ? policy_file(&id) : assembly_directory(&id);
	store_clear(&id);
	if (!path)
		return E_OUTOFMEMORY;
	attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		release(path);
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	status = S_OK;
	if (info)
	{
		DWORD needed = lstrlenW(path) + 1;
		info->dwAssemblyFlags = ASSEMBLYINFO_FLAG_INSTALLED;
		info->uliAssemblySizeInKB.QuadPart = 0;
		if (info->pszCurrentAssemblyPathBuf)
		{
			if (info->cchBuf < needed)
			{
				info->cchBuf = needed;
				status = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
			}
			else
				lstrcpyW(info->pszCurrentAssemblyPathBuf, path);
		}
	}
	release(path);
	return status;
}

static HRESULT WINAPI cache_CreateAssemblyCacheItem(IAssemblyCache *iface, DWORD flags, void *reserved, IAssemblyCacheItem **out, LPCWSTR name)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI cache_CreateAssemblyScavenger(IAssemblyCache *iface, IUnknown **out)
{
	return E_NOTIMPL;
}

static HRESULT install_policy(const WCHAR *manifest, const store_identity *id)
{
	WCHAR *target = policy_file(id);
	HRESULT status = S_OK;
	if (!target)
		return E_OUTOFMEMORY;
	make_directories(target);
	if (!CopyFileW(manifest, target, FALSE))
		status = HRESULT_FROM_WIN32(GetLastError());
	release(target);
	return status;
}

static HRESULT install_assembly(IXMLDOMDocument *document, const WCHAR *manifest, const store_identity *id)
{
	WCHAR *directory = assembly_directory(id);
	WCHAR *target = manifest_file(id);
	WCHAR *source = copy_text(manifest);
	HRESULT status = E_OUTOFMEMORY;
	if (directory && target && source)
	{
		WCHAR *separator = NULL, *at;
		copy_job job;
		for (at = source; *at; at++)
			if (*at == '\\' || *at == '/')
				separator = at;
		if (separator)
			separator[1] = 0;
		else
			source[0] = 0;
		make_directories(directory);
		make_directories(target);
		status = S_OK;
		if (!CopyFileW(manifest, target, FALSE))
			status = HRESULT_FROM_WIN32(GetLastError());
		if (SUCCEEDED(status))
		{
			job.source = source;
			job.target = directory;
			status = for_each_file(document, copy_listed_file, &job);
		}
	}
	release(directory);
	release(target);
	release(source);
	return status;
}

static HRESULT WINAPI cache_InstallAssembly(IAssemblyCache *iface, DWORD flags, LPCWSTR manifest, LPCFUSION_INSTALL_REFERENCE reference)
{
	cache_object *self = (cache_object *)iface;
	IXMLDOMDocument *document = NULL;
	store_identity id;
	HRESULT status;
	HRESULT initialized;
	if (!manifest || !*manifest)
		return E_INVALIDARG;
	initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	status = open_manifest(manifest, &document);
	if (SUCCEEDED(status))
	{
		status = read_identity(document, &id);
		if (SUCCEEDED(status))
		{
			cache_lock(self);
			status = lstrcmpW(id.type, L"win32") ? install_policy(manifest, &id) : install_assembly(document, manifest, &id);
			cache_unlock(self);
			store_clear(&id);
		}
		IXMLDOMDocument_Release(document);
	}
	if (SUCCEEDED(initialized))
		CoUninitialize();
	return status;
}

static HRESULT uninstall_assembly(const store_identity *id)
{
	WCHAR *manifest = manifest_file(id);
	WCHAR *directory = assembly_directory(id);
	IXMLDOMDocument *document = NULL;
	HRESULT status = E_OUTOFMEMORY;
	if (manifest && directory)
	{
		status = open_manifest(manifest, &document);
		if (SUCCEEDED(status))
		{
			status = for_each_file(document, delete_listed_file, directory);
			IXMLDOMDocument_Release(document);
			RemoveDirectoryW(directory);
			DeleteFileW(manifest);
		}
	}
	release(manifest);
	release(directory);
	return status;
}

static HRESULT WINAPI cache_UninstallAssembly(IAssemblyCache *iface, DWORD flags, LPCWSTR text, LPCFUSION_INSTALL_REFERENCE reference, ULONG *disposition)
{
	cache_object *self = (cache_object *)iface;
	store_identity id;
	WCHAR *path;
	HRESULT status, initialized;
	if (disposition)
		*disposition = 0;
	if (flags)
		return E_INVALIDARG;
	status = identity_for_store(text, &id);
	if (FAILED(status))
		return status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? E_INVALIDARG : status;
	path = lstrcmpW(id.type, L"win32") ? policy_file(&id) : manifest_file(&id);
	if (!path)
	{
		store_clear(&id);
		return E_OUTOFMEMORY;
	}
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
	{
		if (disposition)
			*disposition = IASSEMBLYCACHE_UNINSTALL_DISPOSITION_ALREADY_UNINSTALLED;
		release(path);
		store_clear(&id);
		return S_FALSE;
	}
	initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	cache_lock(self);
	if (lstrcmpW(id.type, L"win32"))
		status = DeleteFileW(path) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
	else
		status = uninstall_assembly(&id);
	cache_unlock(self);
	if (SUCCEEDED(initialized))
		CoUninitialize();
	if (SUCCEEDED(status) && disposition)
		*disposition = IASSEMBLYCACHE_UNINSTALL_DISPOSITION_UNINSTALLED;
	release(path);
	store_clear(&id);
	return status;
}

static const IAssemblyCacheVtbl cache_vtbl = {
	cache_QueryInterface,
	cache_AddRef,
	cache_Release,
	cache_UninstallAssembly,
	cache_QueryAssemblyInfo,
	cache_CreateAssemblyCacheItem,
	cache_CreateAssemblyScavenger,
	cache_InstallAssembly,
};

HRESULT WINAPI CreateAssemblyCache(IAssemblyCache **out, DWORD reserved)
{
	cache_object *self;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return E_INVALIDARG;
	self = memory(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->lock = CreateMutexW(NULL, FALSE, L"Raven_SideBySide_Store");
	if (!self->lock)
	{
		release(self);
		return HRESULT_FROM_WIN32(GetLastError());
	}
	self->vtbl = &cache_vtbl;
	self->refs = 1;
	*out = (IAssemblyCache *)self;
	return S_OK;
}

/* --- The managed classes a context names. --- */

/* The runtime's activation contexts hold no class sections for managed code, so there is never a class to report. */
BOOL WINAPI SxsLookupClrGuid(DWORD flags, GUID *clsid, HANDLE context, void *buffer, SIZE_T size, SIZE_T *needed)
{
	if (needed)
		*needed = 0;
	SetLastError(ERROR_NOT_FOUND);
	return FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
