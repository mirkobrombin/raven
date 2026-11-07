/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven management instrumentation component: the object path parser of wmiutils and the locator of wbemprox, through which programs
 * and the .NET Framework's System.Management read what the machine is made of.
 *
 * Windows serves the classes from a service of its own, with a repository of their schemas and providers for each; here the locator
 * hands out the root\cimv2 namespace in the program that asks, and its classes are filled from the system calls when a query runs:
 * Win32_OperatingSystem, Win32_Processor, Win32_ComputerSystem and Win32_Process. Queries are WQL's SELECT, with a WHERE clause of
 * comparisons joined by AND, OR and NOT. Values follow the conventions of the automation types: unsigned 16 and 32 bit integers travel as
 * VT_I4, 64 bit integers as decimal strings, dates as DMTF text. Methods, events, writes back to the namespace and other namespaces are
 * not provided. */
#define CINTERFACE
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <tlhelp32.h>
#include <wbemcli.h>
#include <wmiutils.h>
#include <cpuid.h>

static const CLSID path_class = {0xcf4cc405, 0xe2c5, 0x4ddd, {0xb3, 0xce, 0x5e, 0x75, 0x82, 0xd8, 0xc9, 0xfa}};
static const CLSID locator_class = {0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
static const IID path_iid = {0x3bc15af2, 0x736c, 0x477e, {0x9e, 0x51, 0x23, 0x8a, 0xf8, 0x66, 0x7d, 0xcc}};
static const IID key_list_iid = {0x9ae62877, 0x7544, 0x4bb0, {0xaa, 0x26, 0xa1, 0x38, 0x24, 0x65, 0x9e, 0xd6}};
static const IID locator_iid = {0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
static const IID object_iid = {0xdc12a681, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
static const IID qualifiers_iid = {0xdc12a680, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
static const IID enumerator_iid = {0x027947e1, 0xd731, 0x11ce, {0xa3, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};
static const IID services_iid = {0x9556dc99, 0x828c, 0x11cf, {0xa3, 0x7e, 0x00, 0xaa, 0x00, 0x32, 0x40, 0xc7}};
static const IID client_security_iid = {0x0000013d, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID unknown_iid = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID factory_iid = {0x00000001, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static LONG locks;

__declspec(dllimport) void WINAPI RavenTrace(const char *text);

/* An interface one of the objects is asked for and does not have goes to the runtime's trace, which names what callers expect. */
static HRESULT unanswered(const char *object, REFIID iid, void **out)
{
	char text[96];
	wsprintfA(text, "wmi: %s has no {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", object, iid->Data1, iid->Data2, iid->Data3, iid->Data4[0],
		iid->Data4[1], iid->Data4[2], iid->Data4[3], iid->Data4[4], iid->Data4[5], iid->Data4[6], iid->Data4[7]);
	RavenTrace(text);
	*out = NULL;
	return E_NOINTERFACE;
}

static void *wmi_alloc(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size ? size : 1);
}

static void *wmi_grow(void *block, SIZE_T size)
{
	return block ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, block, size) : wmi_alloc(size);
}

static void wmi_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *wmi_copy(const WCHAR *text, int length)
{
	if (!text)
		return NULL;
	if (length < 0)
		length = lstrlenW(text);
	WCHAR *copy = wmi_alloc((length + 1) * sizeof(WCHAR));
	if (copy)
		CopyMemory(copy, text, length * sizeof(WCHAR));
	return copy;
}

static BOOL same_text(const WCHAR *left, const WCHAR *right)
{
	return left && right && CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

static BOOL is_name_character(WCHAR character)
{
	return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_' ||
		character >= 0x80;
}

/* Text handed back through a caller's buffer: the length counts the terminator, a missing buffer asks for the length alone. */
static HRESULT give_text(const WCHAR *text, ULONG *length, WCHAR *buffer)
{
	if (!length)
		return WBEM_E_INVALID_PARAMETER;
	const ULONG needed = lstrlenW(text) + 1;
	if (!buffer) {
		*length = needed;
		return S_OK;
	}
	if (*length < needed) {
		*length = needed;
		return WBEM_E_BUFFER_TOO_SMALL;
	}
	CopyMemory(buffer, text, needed * sizeof(WCHAR));
	*length = needed;
	return S_OK;
}

/* A growing string for the text of paths, queries and objects. */
typedef struct text_builder {
	WCHAR *text;
	int length, room;
} text_builder;

static void append(text_builder *builder, const WCHAR *text, int length)
{
	if (length < 0)
		length = lstrlenW(text);
	if (builder->length + length + 1 > builder->room) {
		int room = builder->room ? builder->room : 64;
		while (builder->length + length + 1 > room)
			room *= 2;
		WCHAR *grown = wmi_grow(builder->text, room * sizeof(WCHAR));
		if (!grown)
			return;
		builder->text = grown;
		builder->room = room;
	}
	CopyMemory(builder->text + builder->length, text, length * sizeof(WCHAR));
	builder->length += length;
	builder->text[builder->length] = 0;
}

static void append_number(text_builder *builder, ULONGLONG value, BOOL negative)
{
	WCHAR digits[24];
	int at = 23;
	digits[at] = 0;
	do {
		digits[--at] = (WCHAR)('0' + value % 10);
		value /= 10;
	} while (value);
	if (negative)
		digits[--at] = '-';
	append(builder, digits + at, -1);
}

static const WCHAR *built(text_builder *builder)
{
	return builder->text ? builder->text : L"";
}

/* Object paths: \\server\namespace\...:Class.Key="value",Other=12, or Class=@ for a singleton. */
typedef struct path_key {
	WCHAR *name;
	VARIANT value;
	ULONG type;
} path_key;

typedef struct object_path {
	IWbemPath IWbemPath_iface;
	IWbemPathKeyList IWbemPathKeyList_iface;
	LONG refs;
	WCHAR *original, *server, *class_name;
	WCHAR **namespaces;
	ULONG namespace_count;
	path_key *keys;
	ULONG key_count;
	BOOL singleton;
} object_path;

static void clear_keys(object_path *path)
{
	for (ULONG i = 0; i < path->key_count; i++) {
		wmi_free(path->keys[i].name);
		VariantClear(&path->keys[i].value);
	}
	wmi_free(path->keys);
	path->keys = NULL;
	path->key_count = 0;
	path->singleton = FALSE;
}

static void clear_path(object_path *path)
{
	clear_keys(path);
	for (ULONG i = 0; i < path->namespace_count; i++)
		wmi_free(path->namespaces[i]);
	wmi_free(path->namespaces);
	path->namespaces = NULL;
	path->namespace_count = 0;
	wmi_free(path->original);
	wmi_free(path->server);
	wmi_free(path->class_name);
	path->original = path->server = path->class_name = NULL;
}

static BOOL add_namespace(object_path *path, const WCHAR *name, int length)
{
	WCHAR **grown = wmi_grow(path->namespaces, (path->namespace_count + 1) * sizeof(*grown));
	if (!grown)
		return FALSE;
	path->namespaces = grown;
	path->namespaces[path->namespace_count] = wmi_copy(name, length);
	return path->namespaces[path->namespace_count++] != NULL;
}

static BOOL add_key(object_path *path, const WCHAR *name, int length, const VARIANT *value, ULONG type)
{
	path_key *grown = wmi_grow(path->keys, (path->key_count + 1) * sizeof(*grown));
	if (!grown)
		return FALSE;
	path->keys = grown;
	path_key *key = &path->keys[path->key_count++];
	key->name = name ? wmi_copy(name, length) : NULL;
	VariantInit(&key->value);
	VariantCopy(&key->value, (VARIANT *)value);
	key->type = type;
	return TRUE;
}

/* One key value: a quoted string with backslash escapes, or a number, TRUE or FALSE. */
static const WCHAR *parse_key_value(const WCHAR *at, VARIANT *value, ULONG *type)
{
	VariantInit(value);
	if (*at == '"' || *at == '\'') {
		const WCHAR quote = *at++;
		text_builder builder = {0};
		while (*at && *at != quote) {
			if (*at == '\\' && at[1])
				at++;
			append(&builder, at++, 1);
		}
		if (*at != quote) {
			wmi_free(builder.text);
			return NULL;
		}
		V_VT(value) = VT_BSTR;
		V_BSTR(value) = SysAllocString(built(&builder));
		wmi_free(builder.text);
		*type = CIM_STRING;
		return at + 1;
	}
	const WCHAR *start = at;
	while (*at && *at != ',' && *at != ' ')
		at++;
	if (at == start)
		return NULL;
	if (at - start == 4 && CompareStringOrdinal(start, 4, L"TRUE", 4, TRUE) == CSTR_EQUAL) {
		V_VT(value) = VT_BOOL;
		V_BOOL(value) = VARIANT_TRUE;
		*type = CIM_BOOLEAN;
		return at;
	}
	if (at - start == 5 && CompareStringOrdinal(start, 5, L"FALSE", 5, TRUE) == CSTR_EQUAL) {
		V_VT(value) = VT_BOOL;
		V_BOOL(value) = VARIANT_FALSE;
		*type = CIM_BOOLEAN;
		return at;
	}
	const WCHAR *digit = start;
	BOOL negative = *digit == '-';
	if (negative)
		digit++;
	LONGLONG number = 0;
	if (digit == at)
		return NULL;
	for (; digit < at; digit++) {
		if (*digit < '0' || *digit > '9')
			return NULL;
		number = number * 10 + (*digit - '0');
	}
	if (negative)
		number = -number;
	if (number >= -0x80000000LL && number <= 0x7fffffffLL) {
		V_VT(value) = VT_I4;
		V_I4(value) = (LONG)number;
		*type = CIM_SINT32;
	} else {
		V_VT(value) = VT_BSTR;
		V_BSTR(value) = SysAllocStringLen(start, (UINT)(at - start));
		*type = CIM_SINT64;
	}
	return at;
}

static HRESULT parse_object_part(object_path *path, const WCHAR *at)
{
	const WCHAR *start = at;
	while (is_name_character(*at))
		at++;
	if (at == start)
		return WBEM_E_INVALID_PARAMETER;
	path->class_name = wmi_copy(start, (int)(at - start));
	if (!*at)
		return S_OK;
	if (*at == '=') {
		if (at[1] == '@' && !at[2]) {
			path->singleton = TRUE;
			return S_OK;
		}
		VARIANT value;
		ULONG type;
		const WCHAR *end = parse_key_value(at + 1, &value, &type);
		if (!end || *end) {
			VariantClear(&value);
			return WBEM_E_INVALID_PARAMETER;
		}
		add_key(path, NULL, 0, &value, type);
		VariantClear(&value);
		return S_OK;
	}
	if (*at != '.')
		return WBEM_E_INVALID_PARAMETER;
	for (at++;;) {
		const WCHAR *name = at;
		while (is_name_character(*at))
			at++;
		if (at == name || *at != '=')
			return WBEM_E_INVALID_PARAMETER;
		VARIANT value;
		ULONG type;
		const WCHAR *end = parse_key_value(at + 1, &value, &type);
		if (!end) {
			VariantClear(&value);
			return WBEM_E_INVALID_PARAMETER;
		}
		add_key(path, name, (int)(at - name), &value, type);
		VariantClear(&value);
		at = end;
		if (!*at)
			return S_OK;
		if (*at != ',')
			return WBEM_E_INVALID_PARAMETER;
		at++;
	}
}

static BOOL is_separator(WCHAR character)
{
	return character == '\\' || character == '/';
}

static HRESULT parse_path(object_path *path, ULONG mode, const WCHAR *text)
{
	clear_path(path);
	if (!text)
		return S_OK;
	path->original = wmi_copy(text, -1);
	const WCHAR *at = text;
	BOOL had_server = FALSE;
	if (is_separator(at[0]) && is_separator(at[1])) {
		at += 2;
		const WCHAR *start = at;
		while (*at && !is_separator(*at))
			at++;
		if (at == start)
			return WBEM_E_INVALID_PARAMETER;
		path->server = wmi_copy(start, (int)(at - start));
		had_server = TRUE;
		if (*at)
			at++;
	}
	/* The object part follows the namespace after a colon; quoted key values may hold colons and separators of their own. */
	const WCHAR *colon = NULL, *separator = NULL;
	BOOL quoted = FALSE;
	for (const WCHAR *scan = at; *scan; scan++) {
		if (*scan == '"')
			quoted = !quoted;
		else if (!quoted && *scan == ':' && !colon)
			colon = scan;
		else if (!quoted && is_separator(*scan) && !colon && !separator)
			separator = scan;
	}
	const WCHAR *spaces_end = colon ? colon : (had_server || separator || (mode & WBEMPATH_TREAT_SINGLE_IDENT_AS_NS)) ? at + lstrlenW(at) : at;
	while (at < spaces_end) {
		const WCHAR *start = at;
		while (at < spaces_end && !is_separator(*at))
			at++;
		if (at == start)
			return WBEM_E_INVALID_PARAMETER;
		add_namespace(path, start, (int)(at - start));
		if (at < spaces_end)
			at++;
	}
	if (colon)
		at = colon + 1;
	return *at ? parse_object_part(path, at) : S_OK;
}

static void append_key_value(text_builder *builder, const path_key *key)
{
	switch (V_VT(&key->value)) {
	case VT_BSTR:
		if (key->type == CIM_SINT64 || key->type == CIM_UINT64) {
			append(builder, V_BSTR(&key->value), -1);
			break;
		}
		append(builder, L"\"", 1);
		for (const WCHAR *at = V_BSTR(&key->value); at && *at; at++) {
			if (*at == '"' || *at == '\\')
				append(builder, L"\\", 1);
			append(builder, at, 1);
		}
		append(builder, L"\"", 1);
		break;
	case VT_BOOL:
		append(builder, V_BOOL(&key->value) ? L"TRUE" : L"FALSE", -1);
		break;
	case VT_I4:
		append_number(builder, V_I4(&key->value) < 0 ? (ULONGLONG)(-(LONGLONG)V_I4(&key->value)) : (ULONGLONG)V_I4(&key->value), V_I4(&key->value) < 0);
		break;
	default: {
		VARIANT text;
		VariantInit(&text);
		if (SUCCEEDED(VariantChangeType(&text, (VARIANT *)&key->value, 0, VT_BSTR)))
			append(builder, V_BSTR(&text), -1);
		VariantClear(&text);
	}
	}
}

static void append_keys(text_builder *builder, const object_path *path)
{
	for (ULONG i = 0; i < path->key_count; i++) {
		if (i)
			append(builder, L",", 1);
		if (path->keys[i].name) {
			append(builder, path->keys[i].name, -1);
			append(builder, L"=", 1);
		}
		append_key_value(builder, &path->keys[i]);
	}
}

static void append_relative(text_builder *builder, const object_path *path)
{
	if (!path->class_name)
		return;
	append(builder, path->class_name, -1);
	if (path->singleton)
		append(builder, L"=@", 2);
	else if (path->key_count == 1 && !path->keys[0].name) {
		append(builder, L"=", 1);
		append_key_value(builder, &path->keys[0]);
	} else if (path->key_count) {
		append(builder, L".", 1);
		append_keys(builder, path);
	}
}

static void append_namespaces(text_builder *builder, const object_path *path)
{
	for (ULONG i = 0; i < path->namespace_count; i++) {
		if (i)
			append(builder, L"\\", 1);
		append(builder, path->namespaces[i], -1);
	}
}

static WCHAR *path_text(const object_path *path, LONG flags)
{
	text_builder builder = {0};
	if (flags & WBEMPATH_GET_ORIGINAL) {
		append(&builder, path->original ? path->original : L"", -1);
	} else if (flags & WBEMPATH_GET_RELATIVE_ONLY) {
		append_relative(&builder, path);
	} else if (flags & WBEMPATH_GET_NAMESPACE_ONLY) {
		append_namespaces(&builder, path);
	} else {
		const BOOL server = (flags & (WBEMPATH_GET_SERVER_TOO | WBEMPATH_GET_SERVER_AND_NAMESPACE_ONLY)) && path->server;
		if (server) {
			append(&builder, L"\\\\", 2);
			append(&builder, path->server, -1);
			if (path->namespace_count)
				append(&builder, L"\\", 1);
		}
		append_namespaces(&builder, path);
		if (!(flags & WBEMPATH_GET_SERVER_AND_NAMESPACE_ONLY) && path->class_name) {
			if (path->namespace_count || server)
				append(&builder, L":", 1);
			append_relative(&builder, path);
		}
	}
	if (!builder.text)
		builder.text = wmi_copy(L"", 0);
	return builder.text;
}

static ULONGLONG path_info(const object_path *path)
{
	ULONGLONG info = WBEMPATH_INFO_V2_COMPLIANT | WBEMPATH_INFO_CIM_COMPLIANT;
	if (path->server) {
		info |= WBEMPATH_INFO_PATH_HAD_SERVER;
		info |= lstrcmpW(path->server, L".") ? WBEMPATH_INFO_HAS_MACHINE_NAME : WBEMPATH_INFO_ANON_LOCAL_MACHINE;
	}
	if (!path->class_name) {
		if (path->namespace_count)
			info |= WBEMPATH_INFO_SERVER_NAMESPACE_ONLY;
	} else if (path->singleton) {
		info |= WBEMPATH_INFO_IS_INST_REF | WBEMPATH_INFO_IS_SINGLETON | WBEMPATH_INFO_CONTAINS_SINGLETON;
	} else if (path->key_count) {
		info |= WBEMPATH_INFO_IS_INST_REF;
		if (path->key_count == 1 && !path->keys[0].name)
			info |= WBEMPATH_INFO_HAS_IMPLIED_KEY;
	} else {
		info |= WBEMPATH_INFO_IS_CLASS_REF;
	}
	return info;
}

static object_path *path_from(IWbemPath *iface)
{
	return CONTAINING_RECORD(iface, object_path, IWbemPath_iface);
}

static object_path *path_from_keys(IWbemPathKeyList *iface)
{
	return CONTAINING_RECORD(iface, object_path, IWbemPathKeyList_iface);
}

static HRESULT WINAPI path_query(IWbemPath *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &path_iid)) {
		*out = iface;
		IWbemPath_AddRef(iface);
		return S_OK;
	}
	return unanswered("path", iid, out);
}

static ULONG WINAPI path_add_ref(IWbemPath *iface)
{
	return InterlockedIncrement(&path_from(iface)->refs);
}

static ULONG WINAPI path_release(IWbemPath *iface)
{
	object_path *path = path_from(iface);
	const LONG refs = InterlockedDecrement(&path->refs);
	if (!refs) {
		clear_path(path);
		wmi_free(path);
		InterlockedDecrement(&locks);
	}
	return refs;
}

static HRESULT WINAPI path_set_text(IWbemPath *iface, ULONG mode, LPCWSTR text)
{
	if (!mode)
		return WBEM_E_INVALID_PARAMETER;
	object_path *path = path_from(iface);
	const HRESULT status = parse_path(path, mode, text);
	if (FAILED(status))
		clear_path(path);
	return status;
}

static HRESULT WINAPI path_get_text(IWbemPath *iface, LONG flags, ULONG *length, LPWSTR text)
{
	WCHAR *made = path_text(path_from(iface), flags);
	const HRESULT status = made ? give_text(made, length, text) : E_OUTOFMEMORY;
	wmi_free(made);
	return status;
}

static HRESULT WINAPI path_get_info(IWbemPath *iface, ULONG requested, ULONGLONG *response)
{
	if (!response)
		return WBEM_E_INVALID_PARAMETER;
	*response = path_info(path_from(iface));
	return S_OK;
}

static HRESULT WINAPI path_set_server(IWbemPath *iface, LPCWSTR name)
{
	object_path *path = path_from(iface);
	wmi_free(path->server);
	path->server = name && *name ? wmi_copy(name, -1) : NULL;
	return S_OK;
}

static HRESULT WINAPI path_get_server(IWbemPath *iface, ULONG *length, LPWSTR name)
{
	object_path *path = path_from(iface);
	if (!length)
		return WBEM_E_INVALID_PARAMETER;
	return path->server ? give_text(path->server, length, name) : WBEM_E_NOT_AVAILABLE;
}

static HRESULT WINAPI path_get_namespace_count(IWbemPath *iface, ULONG *count)
{
	if (!count)
		return WBEM_E_INVALID_PARAMETER;
	*count = path_from(iface)->namespace_count;
	return S_OK;
}

static HRESULT WINAPI path_set_namespace_at(IWbemPath *iface, ULONG index, LPCWSTR name)
{
	object_path *path = path_from(iface);
	if (!name || index > path->namespace_count)
		return WBEM_E_INVALID_PARAMETER;
	if (!add_namespace(path, name, -1))
		return E_OUTOFMEMORY;
	WCHAR *added = path->namespaces[path->namespace_count - 1];
	MoveMemory(path->namespaces + index + 1, path->namespaces + index, (path->namespace_count - 1 - index) * sizeof(WCHAR *));
	path->namespaces[index] = added;
	return S_OK;
}

static HRESULT WINAPI path_get_namespace_at(IWbemPath *iface, ULONG index, ULONG *length, LPWSTR name)
{
	object_path *path = path_from(iface);
	if (index >= path->namespace_count)
		return WBEM_E_INVALID_PARAMETER;
	return give_text(path->namespaces[index], length, name);
}

static HRESULT WINAPI path_remove_namespace_at(IWbemPath *iface, ULONG index)
{
	object_path *path = path_from(iface);
	if (index >= path->namespace_count)
		return WBEM_E_INVALID_PARAMETER;
	wmi_free(path->namespaces[index]);
	MoveMemory(path->namespaces + index, path->namespaces + index + 1, (path->namespace_count - index - 1) * sizeof(WCHAR *));
	path->namespace_count--;
	return S_OK;
}

static HRESULT WINAPI path_remove_all_namespaces(IWbemPath *iface)
{
	object_path *path = path_from(iface);
	for (ULONG i = 0; i < path->namespace_count; i++)
		wmi_free(path->namespaces[i]);
	path->namespace_count = 0;
	return S_OK;
}

static HRESULT WINAPI path_get_scope_count(IWbemPath *iface, ULONG *count)
{
	if (!count)
		return WBEM_E_INVALID_PARAMETER;
	*count = 0;
	return S_OK;
}

static HRESULT WINAPI path_set_scope(IWbemPath *iface, ULONG index, LPWSTR name)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI path_get_scope(IWbemPath *iface, ULONG index, ULONG *length, LPWSTR name, IWbemPathKeyList **keys)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI path_get_scope_text(IWbemPath *iface, ULONG index, ULONG *length, LPWSTR text)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI path_remove_scope(IWbemPath *iface, ULONG index)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI path_remove_all_scopes(IWbemPath *iface)
{
	return S_OK;
}

static HRESULT WINAPI path_set_class_name(IWbemPath *iface, LPCWSTR name)
{
	object_path *path = path_from(iface);
	if (!name)
		return WBEM_E_INVALID_PARAMETER;
	wmi_free(path->class_name);
	path->class_name = wmi_copy(name, -1);
	return S_OK;
}

static HRESULT WINAPI path_get_class_name(IWbemPath *iface, ULONG *length, LPWSTR name)
{
	object_path *path = path_from(iface);
	if (!length)
		return WBEM_E_INVALID_PARAMETER;
	return path->class_name ? give_text(path->class_name, length, name) : WBEM_E_INVALID_OBJECT_PATH;
}

static HRESULT WINAPI path_get_key_list(IWbemPath *iface, IWbemPathKeyList **out)
{
	object_path *path = path_from(iface);
	if (!out)
		return WBEM_E_INVALID_PARAMETER;
	if (!path->class_name) {
		*out = NULL;
		return WBEM_E_INVALID_PARAMETER;
	}
	*out = &path->IWbemPathKeyList_iface;
	IWbemPath_AddRef(iface);
	return S_OK;
}

static HRESULT WINAPI path_create_class_part(IWbemPath *iface, LONG flags, LPCWSTR name)
{
	return path_set_class_name(iface, name);
}

static HRESULT WINAPI path_delete_class_part(IWbemPath *iface, LONG flags)
{
	object_path *path = path_from(iface);
	clear_keys(path);
	wmi_free(path->class_name);
	path->class_name = NULL;
	return S_OK;
}

static BOOL local_machine(const WCHAR *name)
{
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD size = ARRAYSIZE(computer);
	return !name || !*name || !lstrcmpW(name, L".") || same_text(name, L"localhost") || (GetComputerNameW(computer, &size) && same_text(name, computer));
}

static BOOL WINAPI path_is_relative(IWbemPath *iface, LPWSTR machine, LPWSTR space)
{
	object_path *path = path_from(iface);
	if (path->server && !same_text(path->server, machine) && !(local_machine(path->server) && local_machine(machine)))
		return FALSE;
	WCHAR *mine = path_text(path, WBEMPATH_GET_NAMESPACE_ONLY);
	const BOOL same = !path->namespace_count || same_text(mine, space);
	wmi_free(mine);
	return same;
}

static BOOL WINAPI path_is_relative_or_child(IWbemPath *iface, LPWSTR machine, LPWSTR space, LONG flags)
{
	return path_is_relative(iface, machine, space);
}

static BOOL WINAPI path_is_local(IWbemPath *iface, LPCWSTR machine)
{
	object_path *path = path_from(iface);
	return local_machine(path->server) || same_text(path->server, machine);
}

static BOOL WINAPI path_is_same_class_name(IWbemPath *iface, LPCWSTR name)
{
	return same_text(path_from(iface)->class_name, name);
}

static const IWbemPathVtbl path_vtbl = {
	path_query, path_add_ref, path_release, path_set_text, path_get_text, path_get_info, path_set_server, path_get_server,
	path_get_namespace_count, path_set_namespace_at, path_get_namespace_at, path_remove_namespace_at, path_remove_all_namespaces,
	path_get_scope_count, path_set_scope, path_set_scope, path_get_scope, path_get_scope_text, path_remove_scope, path_remove_all_scopes,
	path_set_class_name, path_get_class_name, path_get_key_list, path_create_class_part, path_delete_class_part, path_is_relative,
	path_is_relative_or_child, path_is_local, path_is_same_class_name,
};

static HRESULT WINAPI keys_query(IWbemPathKeyList *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &key_list_iid)) {
		*out = iface;
		IWbemPathKeyList_AddRef(iface);
		return S_OK;
	}
	return unanswered("key list", iid, out);
}

static ULONG WINAPI keys_add_ref(IWbemPathKeyList *iface)
{
	return path_add_ref(&path_from_keys(iface)->IWbemPath_iface);
}

static ULONG WINAPI keys_release(IWbemPathKeyList *iface)
{
	return path_release(&path_from_keys(iface)->IWbemPath_iface);
}

static HRESULT WINAPI keys_get_count(IWbemPathKeyList *iface, ULONG *count)
{
	if (!count)
		return WBEM_E_INVALID_PARAMETER;
	*count = path_from_keys(iface)->key_count;
	return S_OK;
}

static path_key *find_key(object_path *path, LPCWSTR name)
{
	for (ULONG i = 0; i < path->key_count; i++)
		if ((!name || !*name) ? !path->keys[i].name : same_text(path->keys[i].name, name))
			return &path->keys[i];
	return NULL;
}

static HRESULT WINAPI keys_set_key2(IWbemPathKeyList *iface, LPCWSTR name, ULONG flags, ULONG type, VARIANT *value)
{
	object_path *path = path_from_keys(iface);
	if (!value)
		return WBEM_E_INVALID_PARAMETER;
	path->singleton = FALSE;
	path_key *existing = find_key(path, name);
	if (existing) {
		VariantClear(&existing->value);
		existing->type = type;
		return VariantCopy(&existing->value, value);
	}
	return add_key(path, name, name ? -1 : 0, value, type) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI keys_set_key(IWbemPathKeyList *iface, LPCWSTR name, ULONG flags, ULONG type, LPVOID raw)
{
	VARIANT value;
	VariantInit(&value);
	if (!raw)
		return WBEM_E_INVALID_PARAMETER;
	switch (type) {
	case CIM_STRING:
	case CIM_REFERENCE:
	case CIM_DATETIME:
		V_VT(&value) = VT_BSTR;
		V_BSTR(&value) = SysAllocString(raw);
		break;
	case CIM_SINT8: case CIM_UINT8: V_VT(&value) = VT_I4; V_I4(&value) = *(BYTE *)raw; break;
	case CIM_SINT16: case CIM_UINT16: case CIM_CHAR16: V_VT(&value) = VT_I4; V_I4(&value) = *(USHORT *)raw; break;
	case CIM_SINT32: case CIM_UINT32: V_VT(&value) = VT_I4; V_I4(&value) = *(LONG *)raw; break;
	case CIM_BOOLEAN: V_VT(&value) = VT_BOOL; V_BOOL(&value) = *(VARIANT_BOOL *)raw; break;
	default:
		return WBEM_E_NOT_SUPPORTED;
	}
	const HRESULT status = keys_set_key2(iface, name, flags, type, &value);
	VariantClear(&value);
	return status;
}

static HRESULT WINAPI keys_get_key2(IWbemPathKeyList *iface, ULONG index, ULONG flags, ULONG *name_length, LPWSTR name, VARIANT *value, ULONG *type)
{
	object_path *path = path_from_keys(iface);
	if (index >= path->key_count)
		return WBEM_E_INVALID_PARAMETER;
	const path_key *key = &path->keys[index];
	if (name_length) {
		const HRESULT status = give_text(key->name ? key->name : L"", name_length, name);
		if (FAILED(status))
			return status;
	}
	if (value) {
		VariantInit(value);
		VariantCopy(value, (VARIANT *)&key->value);
	}
	if (type)
		*type = key->type;
	return S_OK;
}

static HRESULT WINAPI keys_get_key(IWbemPathKeyList *iface, ULONG index, ULONG flags, ULONG *name_length, LPWSTR name, ULONG *value_size, LPVOID value,
	ULONG *type)
{
	object_path *path = path_from_keys(iface);
	if (index >= path->key_count)
		return WBEM_E_INVALID_PARAMETER;
	if (name_length) {
		const HRESULT status = give_text(path->keys[index].name ? path->keys[index].name : L"", name_length, name);
		if (FAILED(status))
			return status;
	}
	if (type)
		*type = path->keys[index].type;
	if (!value_size)
		return S_OK;
	/* Text wanted: the value as a path writes it, quoted or not. */
	text_builder builder = {0};
	if (flags & WBEMPATH_QUOTEDTEXT)
		append_key_value(&builder, &path->keys[index]);
	else {
		VARIANT text;
		VariantInit(&text);
		if (SUCCEEDED(VariantChangeType(&text, &path->keys[index].value, 0, VT_BSTR)))
			append(&builder, V_BSTR(&text), -1);
		VariantClear(&text);
	}
	const ULONG needed = (ULONG)((builder.length + 1) * sizeof(WCHAR));
	HRESULT status = S_OK;
	if (value && *value_size >= needed)
		CopyMemory(value, built(&builder), needed);
	else if (value)
		status = WBEM_E_BUFFER_TOO_SMALL;
	*value_size = needed;
	wmi_free(builder.text);
	return status;
}

static HRESULT WINAPI keys_remove_key(IWbemPathKeyList *iface, LPCWSTR name, ULONG flags)
{
	object_path *path = path_from_keys(iface);
	path_key *key = find_key(path, name);
	if (!key)
		return S_OK;
	wmi_free(key->name);
	VariantClear(&key->value);
	const ULONG index = (ULONG)(key - path->keys);
	MoveMemory(key, key + 1, (path->key_count - index - 1) * sizeof(*key));
	path->key_count--;
	return S_OK;
}

static HRESULT WINAPI keys_remove_all(IWbemPathKeyList *iface, ULONG flags)
{
	object_path *path = path_from_keys(iface);
	const BOOL singleton = path->singleton;
	clear_keys(path);
	path->singleton = singleton;
	return S_OK;
}

static HRESULT WINAPI keys_make_singleton(IWbemPathKeyList *iface, boolean set)
{
	object_path *path = path_from_keys(iface);
	if (set)
		clear_keys(path);
	path->singleton = set ? TRUE : FALSE;
	return S_OK;
}

static HRESULT WINAPI keys_get_info(IWbemPathKeyList *iface, ULONG requested, ULONGLONG *response)
{
	if (!response)
		return WBEM_E_INVALID_PARAMETER;
	*response = 0;
	return S_OK;
}

static HRESULT WINAPI keys_get_text(IWbemPathKeyList *iface, LONG flags, ULONG *length, LPWSTR text)
{
	text_builder builder = {0};
	append_keys(&builder, path_from_keys(iface));
	const HRESULT status = give_text(built(&builder), length, text);
	wmi_free(builder.text);
	return status;
}

static const IWbemPathKeyListVtbl keys_vtbl = {
	keys_query, keys_add_ref, keys_release, keys_get_count, keys_set_key, keys_set_key2, keys_get_key, keys_get_key2, keys_remove_key,
	keys_remove_all, keys_make_singleton, keys_get_info, keys_get_text,
};

static HRESULT create_path(REFIID iid, void **out)
{
	object_path *path = wmi_alloc(sizeof(*path));
	if (!path)
		return E_OUTOFMEMORY;
	path->IWbemPath_iface.lpVtbl = &path_vtbl;
	path->IWbemPathKeyList_iface.lpVtbl = &keys_vtbl;
	path->refs = 1;
	InterlockedIncrement(&locks);
	const HRESULT status = path_query(&path->IWbemPath_iface, iid, out);
	IWbemPath_Release(&path->IWbemPath_iface);
	return status;
}

/* The classes of root\cimv2: each property's name and CIM type; the first property marked key names the instance. */
typedef struct class_property {
	const WCHAR *name;
	CIMTYPE type;
	BOOL key;
} class_property;

typedef struct class_object class_object;
typedef struct instance_list instance_list;

typedef struct class_schema {
	const WCHAR *name;
	const class_property *properties;
	ULONG count;
	void (*fill)(const struct class_schema *schema, instance_list *list);
} class_schema;

struct class_object {
	IWbemClassObject IWbemClassObject_iface;
	LONG refs;
	const class_schema *schema;
	VARIANT *values;
	BOOL instance;
	/* Where BeginEnumeration left the walk over the properties, the system ones first, and what it was asked for. */
	LONG position, enumeration;
};

struct instance_list {
	class_object **objects;
	ULONG count;
};

static const WCHAR *system_names[] = {L"__GENUS", L"__CLASS", L"__SUPERCLASS", L"__DYNASTY", L"__RELPATH", L"__PROPERTY_COUNT", L"__DERIVATION",
	L"__SERVER", L"__NAMESPACE", L"__PATH"};

static const class_property operating_system_properties[] = {
	{L"BootDevice", CIM_STRING}, {L"BuildNumber", CIM_STRING}, {L"BuildType", CIM_STRING}, {L"Caption", CIM_STRING}, {L"CodeSet", CIM_STRING},
	{L"CountryCode", CIM_STRING}, {L"CreationClassName", CIM_STRING}, {L"CSCreationClassName", CIM_STRING}, {L"CSName", CIM_STRING},
	{L"CurrentTimeZone", CIM_SINT16}, {L"Description", CIM_STRING}, {L"FreePhysicalMemory", CIM_UINT64}, {L"FreeSpaceInPagingFiles", CIM_UINT64},
	{L"FreeVirtualMemory", CIM_UINT64}, {L"InstallDate", CIM_DATETIME}, {L"LastBootUpTime", CIM_DATETIME}, {L"LocalDateTime", CIM_DATETIME},
	{L"Locale", CIM_STRING}, {L"Manufacturer", CIM_STRING}, {L"MaxProcessMemorySize", CIM_UINT64}, {L"Name", CIM_STRING, TRUE},
	{L"NumberOfProcesses", CIM_UINT32}, {L"NumberOfUsers", CIM_UINT32}, {L"OperatingSystemSKU", CIM_UINT32}, {L"OSArchitecture", CIM_STRING},
	{L"OSLanguage", CIM_UINT32}, {L"OSProductSuite", CIM_UINT32}, {L"OSType", CIM_UINT16}, {L"Primary", CIM_BOOLEAN}, {L"ProductType", CIM_UINT32},
	{L"SerialNumber", CIM_STRING}, {L"ServicePackMajorVersion", CIM_UINT16}, {L"ServicePackMinorVersion", CIM_UINT16},
	{L"SizeStoredInPagingFiles", CIM_UINT64}, {L"Status", CIM_STRING}, {L"SystemDirectory", CIM_STRING}, {L"SystemDrive", CIM_STRING},
	{L"TotalVirtualMemorySize", CIM_UINT64}, {L"TotalVisibleMemorySize", CIM_UINT64}, {L"Version", CIM_STRING}, {L"WindowsDirectory", CIM_STRING},
};

static const class_property processor_properties[] = {
	{L"AddressWidth", CIM_UINT16}, {L"Architecture", CIM_UINT16}, {L"Availability", CIM_UINT16}, {L"Caption", CIM_STRING},
	{L"CpuStatus", CIM_UINT16}, {L"CreationClassName", CIM_STRING}, {L"CurrentClockSpeed", CIM_UINT32}, {L"DataWidth", CIM_UINT16},
	{L"Description", CIM_STRING}, {L"DeviceID", CIM_STRING, TRUE}, {L"Family", CIM_UINT16}, {L"L2CacheSize", CIM_UINT32},
	{L"L3CacheSize", CIM_UINT32}, {L"Level", CIM_UINT16}, {L"LoadPercentage", CIM_UINT16}, {L"Manufacturer", CIM_STRING},
	{L"MaxClockSpeed", CIM_UINT32}, {L"Name", CIM_STRING}, {L"NumberOfCores", CIM_UINT32}, {L"NumberOfEnabledCore", CIM_UINT32},
	{L"NumberOfLogicalProcessors", CIM_UINT32}, {L"ProcessorId", CIM_STRING}, {L"ProcessorType", CIM_UINT16}, {L"Revision", CIM_UINT16},
	{L"Role", CIM_STRING}, {L"SocketDesignation", CIM_STRING}, {L"Status", CIM_STRING}, {L"SystemCreationClassName", CIM_STRING},
	{L"SystemName", CIM_STRING}, {L"ThreadCount", CIM_UINT32}, {L"Version", CIM_STRING},
};

static const class_property computer_system_properties[] = {
	{L"Caption", CIM_STRING}, {L"Description", CIM_STRING}, {L"Domain", CIM_STRING}, {L"Manufacturer", CIM_STRING}, {L"Model", CIM_STRING},
	{L"Name", CIM_STRING, TRUE}, {L"NumberOfLogicalProcessors", CIM_UINT32}, {L"NumberOfProcessors", CIM_UINT32}, {L"PartOfDomain", CIM_BOOLEAN},
	{L"PCSystemType", CIM_UINT16}, {L"Status", CIM_STRING}, {L"SystemType", CIM_STRING}, {L"TotalPhysicalMemory", CIM_UINT64},
	{L"UserName", CIM_STRING}, {L"Workgroup", CIM_STRING},
};

static const class_property process_properties[] = {
	{L"Caption", CIM_STRING}, {L"CommandLine", CIM_STRING}, {L"CreationClassName", CIM_STRING}, {L"CreationDate", CIM_DATETIME},
	{L"CSCreationClassName", CIM_STRING}, {L"CSName", CIM_STRING}, {L"Description", CIM_STRING}, {L"ExecutablePath", CIM_STRING},
	{L"Handle", CIM_STRING, TRUE}, {L"HandleCount", CIM_UINT32}, {L"KernelModeTime", CIM_UINT64}, {L"Name", CIM_STRING}, {L"OSCreationClassName", CIM_STRING},
	{L"OSName", CIM_STRING}, {L"ParentProcessId", CIM_UINT32}, {L"Priority", CIM_UINT32}, {L"PrivatePageCount", CIM_UINT64},
	{L"ProcessId", CIM_UINT32}, {L"SessionId", CIM_UINT32}, {L"ThreadCount", CIM_UINT32}, {L"UserModeTime", CIM_UINT64},
	{L"VirtualSize", CIM_UINT64}, {L"WindowsVersion", CIM_STRING}, {L"WorkingSetSize", CIM_UINT64},
};

static void fill_operating_system(const class_schema *schema, instance_list *list);
static void fill_processor(const class_schema *schema, instance_list *list);
static void fill_computer_system(const class_schema *schema, instance_list *list);
static void fill_process(const class_schema *schema, instance_list *list);

static const class_schema schemas[] = {
	{L"Win32_OperatingSystem", operating_system_properties, ARRAYSIZE(operating_system_properties), fill_operating_system},
	{L"Win32_Processor", processor_properties, ARRAYSIZE(processor_properties), fill_processor},
	{L"Win32_ComputerSystem", computer_system_properties, ARRAYSIZE(computer_system_properties), fill_computer_system},
	{L"Win32_Process", process_properties, ARRAYSIZE(process_properties), fill_process},
};

static const class_schema *find_schema(const WCHAR *name, int length)
{
	for (ULONG i = 0; i < ARRAYSIZE(schemas); i++)
		if (CompareStringOrdinal(schemas[i].name, -1, name, length, TRUE) == CSTR_EQUAL)
			return &schemas[i];
	return NULL;
}

static LONG property_index(const class_schema *schema, const WCHAR *name)
{
	for (ULONG i = 0; i < schema->count; i++)
		if (same_text(schema->properties[i].name, name))
			return (LONG)i;
	return -1;
}

static LONG system_index(const WCHAR *name)
{
	for (ULONG i = 0; i < ARRAYSIZE(system_names); i++)
		if (same_text(system_names[i], name))
			return (LONG)i;
	return -1;
}

static const IWbemClassObjectVtbl object_vtbl;

static class_object *make_object(const class_schema *schema, BOOL instance)
{
	class_object *object = wmi_alloc(sizeof(*object));
	if (!object)
		return NULL;
	object->values = wmi_alloc(schema->count * sizeof(VARIANT));
	if (!object->values) {
		wmi_free(object);
		return NULL;
	}
	object->IWbemClassObject_iface.lpVtbl = &object_vtbl;
	object->refs = 1;
	object->schema = schema;
	object->instance = instance;
	object->position = -1;
	for (ULONG i = 0; i < schema->count; i++)
		V_VT(&object->values[i]) = VT_NULL;
	InterlockedIncrement(&locks);
	return object;
}

static class_object *object_from(IWbemClassObject *iface)
{
	return CONTAINING_RECORD(iface, class_object, IWbemClassObject_iface);
}

/* Values in the conventions WMI hands them to automation in. */
static void put_text(class_object *object, const WCHAR *name, const WCHAR *text)
{
	const LONG index = property_index(object->schema, name);
	if (index < 0 || !text)
		return;
	VariantClear(&object->values[index]);
	V_VT(&object->values[index]) = VT_BSTR;
	V_BSTR(&object->values[index]) = SysAllocString(text);
}

static void put_number(class_object *object, const WCHAR *name, ULONGLONG value)
{
	const LONG index = property_index(object->schema, name);
	if (index < 0)
		return;
	VARIANT *slot = &object->values[index];
	VariantClear(slot);
	switch (object->schema->properties[index].type) {
	case CIM_UINT64:
	case CIM_SINT64: {
		text_builder builder = {0};
		append_number(&builder, value, FALSE);
		V_VT(slot) = VT_BSTR;
		V_BSTR(slot) = SysAllocString(built(&builder));
		wmi_free(builder.text);
		break;
	}
	case CIM_UINT8:
		V_VT(slot) = VT_UI1;
		V_UI1(slot) = (BYTE)value;
		break;
	case CIM_SINT16:
	case CIM_SINT8:
		V_VT(slot) = VT_I2;
		V_I2(slot) = (SHORT)value;
		break;
	case CIM_BOOLEAN:
		V_VT(slot) = VT_BOOL;
		V_BOOL(slot) = value ? VARIANT_TRUE : VARIANT_FALSE;
		break;
	default:
		V_VT(slot) = VT_I4;
		V_I4(slot) = (LONG)value;
		break;
	}
}

/* yyyymmddHHMMSS.mmmmmm+UUU, the DMTF form of a moment, with the offset in minutes from UTC. */
static void put_time(class_object *object, const WCHAR *name, const FILETIME *moment)
{
	FILETIME local;
	SYSTEMTIME time;
	TIME_ZONE_INFORMATION zone;
	const DWORD kind = GetTimeZoneInformation(&zone);
	LONG bias = -zone.Bias - (kind == TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias : 0);
	if (!FileTimeToLocalFileTime(moment, &local) || !FileTimeToSystemTime(&local, &time))
		return;
	WCHAR text[32];
	wsprintfW(text, L"%04u%02u%02u%02u%02u%02u.%06u%c%03d", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
		time.wMilliseconds * 1000, bias < 0 ? '-' : '+', bias < 0 ? -bias : bias);
	put_text(object, name, text);
}

static void add_instance(instance_list *list, class_object *object)
{
	if (!object)
		return;
	class_object **grown = wmi_grow(list->objects, (list->count + 1) * sizeof(*grown));
	if (!grown) {
		IWbemClassObject_Release(&object->IWbemClassObject_iface);
		return;
	}
	list->objects = grown;
	list->objects[list->count++] = object;
}

static void computer_name(WCHAR *name, DWORD size)
{
	if (!GetComputerNameW(name, &size))
		lstrcpyW(name, L"RAVEN");
}

static void registry_text(HKEY root, const WCHAR *key, const WCHAR *value, WCHAR *out, DWORD size)
{
	DWORD bytes = size * sizeof(WCHAR);
	if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, NULL, out, &bytes))
		*out = 0;
}

static void fill_operating_system(const class_schema *schema, instance_list *list)
{
	class_object *object = make_object(schema, TRUE);
	if (!object)
		return;
	static const WCHAR version_key[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
	WCHAR product[128], caption[160], windows[MAX_PATH], system[MAX_PATH], name[MAX_PATH * 2], computer[MAX_COMPUTERNAME_LENGTH + 1], text[64];
	registry_text(HKEY_LOCAL_MACHINE, version_key, L"ProductName", product, ARRAYSIZE(product));
	if (!*product)
		lstrcpyW(product, L"Windows 10 Pro");
	wsprintfW(caption, L"Microsoft %s", product);
	GetWindowsDirectoryW(windows, MAX_PATH);
	GetSystemDirectoryW(system, MAX_PATH);
	computer_name(computer, ARRAYSIZE(computer));
	RTL_OSVERSIONINFOW version = {sizeof(version)};
	LONG (WINAPI *get_version)(RTL_OSVERSIONINFOW *) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	if (!get_version || get_version(&version)) {
		version.dwMajorVersion = 10;
		version.dwBuildNumber = 19045;
	}
	put_text(object, L"Caption", caption);
	put_text(object, L"Description", L"");
	wsprintfW(name, L"%s|%s|\\Device\\Harddisk0\\Partition1", caption, windows);
	put_text(object, L"Name", name);
	wsprintfW(text, L"%u.%u.%u", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
	put_text(object, L"Version", text);
	wsprintfW(text, L"%u", version.dwBuildNumber);
	put_text(object, L"BuildNumber", text);
	put_text(object, L"BuildType", L"Multiprocessor Free");
	put_text(object, L"BootDevice", L"\\Device\\HarddiskVolume1");
	put_text(object, L"CreationClassName", L"Win32_OperatingSystem");
	put_text(object, L"CSCreationClassName", L"Win32_ComputerSystem");
	put_text(object, L"CSName", computer);
	put_text(object, L"Manufacturer", L"Microsoft Corporation");
	put_text(object, L"SystemDirectory", system);
	put_text(object, L"WindowsDirectory", windows);
	windows[2] = 0;
	put_text(object, L"SystemDrive", windows);
	put_text(object, L"Status", L"OK");
	put_text(object, L"SerialNumber", L"00330-80000-00000-AA000");
	SYSTEM_INFO info;
	GetNativeSystemInfo(&info);
	put_text(object, L"OSArchitecture", info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? L"64-bit" :
		info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? L"ARM 64-bit Processor" : L"32-bit");
	wsprintfW(text, L"%u", GetACP());
	put_text(object, L"CodeSet", text);
	const LCID locale = GetSystemDefaultLCID();
	wsprintfW(text, L"%04x", locale);
	put_text(object, L"Locale", text);
	put_number(object, L"OSLanguage", GetSystemDefaultUILanguage());
	if (GetLocaleInfoW(locale, LOCALE_ICOUNTRY, text, ARRAYSIZE(text)))
		put_text(object, L"CountryCode", text);
	TIME_ZONE_INFORMATION zone;
	const DWORD kind = GetTimeZoneInformation(&zone);
	put_number(object, L"CurrentTimeZone", (ULONGLONG)(LONGLONG)(-zone.Bias - (kind == TIME_ZONE_ID_DAYLIGHT ? zone.DaylightBias : 0)));
	MEMORYSTATUSEX memory = {sizeof(memory)};
	if (GlobalMemoryStatusEx(&memory)) {
		put_number(object, L"TotalVisibleMemorySize", memory.ullTotalPhys / 1024);
		put_number(object, L"FreePhysicalMemory", memory.ullAvailPhys / 1024);
		put_number(object, L"TotalVirtualMemorySize", memory.ullTotalPageFile / 1024);
		put_number(object, L"FreeVirtualMemory", memory.ullAvailPageFile / 1024);
		put_number(object, L"MaxProcessMemorySize", memory.ullTotalVirtual / 1024);
		const ULONGLONG paging = memory.ullTotalPageFile > memory.ullTotalPhys ? memory.ullTotalPageFile - memory.ullTotalPhys : 0;
		const ULONGLONG paging_free = memory.ullAvailPageFile > memory.ullAvailPhys ? memory.ullAvailPageFile - memory.ullAvailPhys : 0;
		put_number(object, L"SizeStoredInPagingFiles", paging / 1024);
		put_number(object, L"FreeSpaceInPagingFiles", paging_free / 1024);
	}
	put_number(object, L"OSType", 18);
	put_number(object, L"ProductType", 1);
	put_number(object, L"OperatingSystemSKU", 48);
	put_number(object, L"OSProductSuite", 256);
	put_number(object, L"ServicePackMajorVersion", 0);
	put_number(object, L"ServicePackMinorVersion", 0);
	put_number(object, L"Primary", 1);
	put_number(object, L"NumberOfUsers", 1);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	ULONG processes = 0;
	if (snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry = {sizeof(entry)};
		for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
			processes++;
		CloseHandle(snapshot);
	}
	put_number(object, L"NumberOfProcesses", processes ? processes : 1);
	FILETIME now, boot;
	GetSystemTimeAsFileTime(&now);
	put_time(object, L"LocalDateTime", &now);
	ULARGE_INTEGER moment = {{now.dwLowDateTime, now.dwHighDateTime}};
	moment.QuadPart -= GetTickCount64() * 10000;
	boot.dwLowDateTime = moment.LowPart;
	boot.dwHighDateTime = moment.HighPart;
	put_time(object, L"LastBootUpTime", &boot);
	put_time(object, L"InstallDate", &boot);
	add_instance(list, object);
}

static void processor_brand(WCHAR *name, int size, WCHAR *vendor, int vendor_size, unsigned *signature, unsigned *features, unsigned *megahertz)
{
	unsigned a, b, c, d;
	char brand[49] = {0}, maker[13] = {0};
	*signature = *features = *megahertz = 0;
	if (__get_cpuid(0, &a, &b, &c, &d)) {
		CopyMemory(maker, &b, 4);
		CopyMemory(maker + 4, &d, 4);
		CopyMemory(maker + 8, &c, 4);
		const unsigned highest = a;
		if (__get_cpuid(1, &a, &b, &c, &d)) {
			*signature = a;
			*features = d;
		}
		if (highest >= 0x16 && __get_cpuid(0x16, &a, &b, &c, &d))
			*megahertz = a & 0xffff;
	}
	if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004)
		for (unsigned leaf = 0; leaf < 3; leaf++) {
			__get_cpuid(0x80000002 + leaf, &a, &b, &c, &d);
			CopyMemory(brand + leaf * 16, &a, 4);
			CopyMemory(brand + leaf * 16 + 4, &b, 4);
			CopyMemory(brand + leaf * 16 + 8, &c, 4);
			CopyMemory(brand + leaf * 16 + 12, &d, 4);
		}
	const char *start = brand;
	while (*start == ' ')
		start++;
	MultiByteToWideChar(CP_ACP, 0, *start ? start : "Virtual processor", -1, name, size);
	MultiByteToWideChar(CP_ACP, 0, *maker ? maker : "Unknown", -1, vendor, vendor_size);
}

static void fill_processor(const class_schema *schema, instance_list *list)
{
	class_object *object = make_object(schema, TRUE);
	if (!object)
		return;
	SYSTEM_INFO info;
	GetNativeSystemInfo(&info);
	WCHAR name[64], vendor[16], text[96], computer[MAX_COMPUTERNAME_LENGTH + 1];
	unsigned signature, features, megahertz;
	processor_brand(name, ARRAYSIZE(name), vendor, ARRAYSIZE(vendor), &signature, &features, &megahertz);
	DWORD registered = 0, size = sizeof(registered);
	if (!RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, NULL, &registered, &size) && registered)
		megahertz = registered;
	if (!megahertz)
		megahertz = 2000;
	/* Cores from the processor relations, each logical processor counted as a core where they are not given. */
	ULONG cores = 0;
	DWORD length = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &length);
	BYTE *relations = length ? wmi_alloc(length) : NULL;
	if (relations && GetLogicalProcessorInformationEx(RelationProcessorCore, (void *)relations, &length))
		for (DWORD at = 0; at < length; at += ((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(relations + at))->Size) {
			if (!((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(relations + at))->Size)
				break;
			cores++;
		}
	wmi_free(relations);
	if (!cores || cores > info.dwNumberOfProcessors)
		cores = info.dwNumberOfProcessors;
	const unsigned family = (signature >> 8) & 15, model = ((signature >> 4) & 15) | (family >= 6 ? ((signature >> 12) & 0xf0) : 0), stepping = signature & 15;
	const unsigned shown_family = family == 15 ? family + ((signature >> 20) & 0xff) : family;
	put_number(object, L"Architecture", info.wProcessorArchitecture);
	put_number(object, L"AddressWidth", info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? 32 : 64);
	put_number(object, L"DataWidth", info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? 32 : 64);
	put_number(object, L"Availability", 3);
	put_number(object, L"CpuStatus", 1);
	put_number(object, L"CurrentClockSpeed", megahertz);
	put_number(object, L"MaxClockSpeed", megahertz);
	put_number(object, L"Family", 2);
	put_number(object, L"L2CacheSize", 0);
	put_number(object, L"L3CacheSize", 0);
	put_number(object, L"Level", shown_family);
	put_number(object, L"LoadPercentage", 0);
	put_number(object, L"NumberOfCores", cores);
	put_number(object, L"NumberOfEnabledCore", cores);
	put_number(object, L"NumberOfLogicalProcessors", info.dwNumberOfProcessors);
	put_number(object, L"ThreadCount", info.dwNumberOfProcessors);
	put_number(object, L"ProcessorType", 3);
	put_number(object, L"Revision", (model << 8) | stepping);
	put_text(object, L"Name", name);
	put_text(object, L"Manufacturer", vendor);
	wsprintfW(text, L"%s Family %u Model %u Stepping %u", info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? L"x86" : L"Intel64",
		shown_family, model, stepping);
	put_text(object, L"Caption", text);
	put_text(object, L"Description", text);
	wsprintfW(text, L"%08X%08X", features, signature);
	put_text(object, L"ProcessorId", text);
	put_text(object, L"DeviceID", L"CPU0");
	put_text(object, L"CreationClassName", L"Win32_Processor");
	put_text(object, L"Role", L"CPU");
	put_text(object, L"SocketDesignation", L"CPU 0");
	put_text(object, L"Status", L"OK");
	put_text(object, L"SystemCreationClassName", L"Win32_ComputerSystem");
	computer_name(computer, ARRAYSIZE(computer));
	put_text(object, L"SystemName", computer);
	put_text(object, L"Version", L"");
	add_instance(list, object);
}

static void fill_computer_system(const class_schema *schema, instance_list *list)
{
	class_object *object = make_object(schema, TRUE);
	if (!object)
		return;
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1], user[256];
	computer_name(computer, ARRAYSIZE(computer));
	SYSTEM_INFO info;
	GetNativeSystemInfo(&info);
	put_text(object, L"Name", computer);
	put_text(object, L"Caption", computer);
	put_text(object, L"Description", L"AT/AT COMPATIBLE");
	put_text(object, L"Domain", L"WORKGROUP");
	put_text(object, L"Workgroup", L"WORKGROUP");
	put_text(object, L"Manufacturer", L"Raven");
	put_text(object, L"Model", L"Raven");
	put_text(object, L"Status", L"OK");
	put_text(object, L"SystemType", info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? L"X86-based PC" : L"x64-based PC");
	DWORD size = ARRAYSIZE(user);
	WCHAR qualified[MAX_COMPUTERNAME_LENGTH + 258];
	if (GetEnvironmentVariableW(L"USERNAME", user, size)) {
		wsprintfW(qualified, L"%s\\%s", computer, user);
		put_text(object, L"UserName", qualified);
	}
	put_number(object, L"NumberOfLogicalProcessors", info.dwNumberOfProcessors);
	put_number(object, L"NumberOfProcessors", 1);
	put_number(object, L"PartOfDomain", 0);
	put_number(object, L"PCSystemType", 1);
	MEMORYSTATUSEX memory = {sizeof(memory)};
	if (GlobalMemoryStatusEx(&memory))
		put_number(object, L"TotalPhysicalMemory", memory.ullTotalPhys);
	add_instance(list, object);
}

static void fill_process(const class_schema *schema, instance_list *list)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return;
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1], text[32], path[MAX_PATH], version[32];
	computer_name(computer, ARRAYSIZE(computer));
	RTL_OSVERSIONINFOW os = {sizeof(os)};
	LONG (WINAPI *get_version)(RTL_OSVERSIONINFOW *) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	if (get_version && !get_version(&os))
		wsprintfW(version, L"%u.%u.%u", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
	else
		lstrcpyW(version, L"10.0.19045");
	PROCESSENTRY32W entry = {sizeof(entry)};
	for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
		class_object *object = make_object(schema, TRUE);
		if (!object)
			break;
		wsprintfW(text, L"%u", entry.th32ProcessID);
		put_text(object, L"Handle", text);
		put_text(object, L"Name", entry.szExeFile);
		put_text(object, L"Caption", entry.szExeFile);
		put_text(object, L"Description", entry.szExeFile);
		put_text(object, L"CreationClassName", L"Win32_Process");
		put_text(object, L"CSCreationClassName", L"Win32_ComputerSystem");
		put_text(object, L"OSCreationClassName", L"Win32_OperatingSystem");
		put_text(object, L"CSName", computer);
		put_text(object, L"WindowsVersion", version);
		put_number(object, L"ProcessId", entry.th32ProcessID);
		put_number(object, L"ParentProcessId", entry.th32ParentProcessID);
		put_number(object, L"ThreadCount", entry.cntThreads);
		put_number(object, L"Priority", (ULONGLONG)entry.pcPriClassBase);
		put_number(object, L"SessionId", 1);
		if (entry.th32ProcessID == GetCurrentProcessId())
			put_text(object, L"CommandLine", GetCommandLineW());
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
		if (process) {
			DWORD size = ARRAYSIZE(path);
			if (QueryFullProcessImageNameW(process, 0, path, &size))
				put_text(object, L"ExecutablePath", path);
			FILETIME created, ended, kernel, user;
			if (GetProcessTimes(process, &created, &ended, &kernel, &user)) {
				put_time(object, L"CreationDate", &created);
				put_number(object, L"KernelModeTime", ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime);
				put_number(object, L"UserModeTime", ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime);
			}
			CloseHandle(process);
		}
		add_instance(list, object);
	}
	CloseHandle(snapshot);
}

/* The relative path of an instance: its class and key; a class object's is its name. */
static WCHAR *relative_path(const class_object *object)
{
	text_builder builder = {0};
	append(&builder, object->schema->name, -1);
	if (object->instance)
		for (ULONG i = 0; i < object->schema->count; i++) {
			if (!object->schema->properties[i].key)
				continue;
			append(&builder, L".", 1);
			append(&builder, object->schema->properties[i].name, -1);
			append(&builder, L"=", 1);
			path_key key = {NULL, object->values[i], object->schema->properties[i].type};
			if (V_VT(&key.value) == VT_NULL)
				append(&builder, L"\"\"", 2);
			else
				append_key_value(&builder, &key);
			break;
		}
	return builder.text;
}

static HRESULT system_value(const class_object *object, LONG index, VARIANT *value, CIMTYPE *type)
{
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
	CIMTYPE kind = CIM_STRING;
	VariantInit(value);
	V_VT(value) = VT_BSTR;
	switch (index) {
	case 0:
		kind = CIM_SINT32;
		V_VT(value) = VT_I4;
		V_I4(value) = object->instance ? WBEM_GENUS_INSTANCE : WBEM_GENUS_CLASS;
		break;
	case 1:
	case 3:
		V_BSTR(value) = SysAllocString(object->schema->name);
		break;
	case 2:
		V_VT(value) = VT_NULL;
		break;
	case 4: {
		WCHAR *path = relative_path(object);
		V_BSTR(value) = SysAllocString(path ? path : L"");
		wmi_free(path);
		break;
	}
	case 5:
		kind = CIM_SINT32;
		V_VT(value) = VT_I4;
		V_I4(value) = (LONG)object->schema->count;
		break;
	case 6: {
		kind = CIM_STRING | CIM_FLAG_ARRAY;
		SAFEARRAYBOUND bound = {0, 0};
		V_VT(value) = VT_ARRAY | VT_BSTR;
		V_ARRAY(value) = SafeArrayCreate(VT_BSTR, 1, &bound);
		break;
	}
	case 7:
		computer_name(computer, ARRAYSIZE(computer));
		V_BSTR(value) = SysAllocString(computer);
		break;
	case 8:
		V_BSTR(value) = SysAllocString(L"ROOT\\CIMV2");
		break;
	default: {
		computer_name(computer, ARRAYSIZE(computer));
		WCHAR *path = relative_path(object);
		text_builder builder = {0};
		append(&builder, L"\\\\", 2);
		append(&builder, computer, -1);
		append(&builder, L"\\ROOT\\CIMV2:", -1);
		append(&builder, path ? path : L"", -1);
		V_BSTR(value) = SysAllocString(built(&builder));
		wmi_free(builder.text);
		wmi_free(path);
	}
	}
	if (type)
		*type = kind;
	return S_OK;
}

static HRESULT WINAPI object_query(IWbemClassObject *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &object_iid)) {
		*out = iface;
		IWbemClassObject_AddRef(iface);
		return S_OK;
	}
	return unanswered("class object", iid, out);
}

static ULONG WINAPI object_add_ref(IWbemClassObject *iface)
{
	return InterlockedIncrement(&object_from(iface)->refs);
}

static ULONG WINAPI object_release(IWbemClassObject *iface)
{
	class_object *object = object_from(iface);
	const LONG refs = InterlockedDecrement(&object->refs);
	if (!refs) {
		for (ULONG i = 0; i < object->schema->count; i++)
			VariantClear(&object->values[i]);
		wmi_free(object->values);
		wmi_free(object);
		InterlockedDecrement(&locks);
	}
	return refs;
}

static HRESULT create_qualifiers(const class_object *object, LONG property, IWbemQualifierSet **out);

static HRESULT WINAPI object_get_qualifier_set(IWbemClassObject *iface, IWbemQualifierSet **out)
{
	return create_qualifiers(object_from(iface), -1, out);
}

static HRESULT WINAPI object_get(IWbemClassObject *iface, LPCWSTR name, LONG flags, VARIANT *value, CIMTYPE *type, LONG *flavor)
{
	class_object *object = object_from(iface);
	if (!name)
		return WBEM_E_INVALID_PARAMETER;
	const LONG system = system_index(name);
	if (system >= 0) {
		VARIANT made;
		system_value(object, system, &made, type);
		if (value)
			*value = made;
		else
			VariantClear(&made);
		if (flavor)
			*flavor = WBEM_FLAVOR_ORIGIN_SYSTEM;
		return S_OK;
	}
	const LONG index = property_index(object->schema, name);
	if (index < 0)
		return WBEM_E_NOT_FOUND;
	if (value) {
		VariantInit(value);
		VariantCopy(value, &object->values[index]);
	}
	if (type)
		*type = object->schema->properties[index].type;
	if (flavor)
		*flavor = object->instance ? WBEM_FLAVOR_ORIGIN_PROPAGATED : WBEM_FLAVOR_ORIGIN_LOCAL;
	return S_OK;
}

static HRESULT WINAPI object_put(IWbemClassObject *iface, LPCWSTR name, LONG flags, VARIANT *value, CIMTYPE type)
{
	class_object *object = object_from(iface);
	if (!name)
		return WBEM_E_INVALID_PARAMETER;
	if (system_index(name) >= 0)
		return WBEM_E_READ_ONLY;
	const LONG index = property_index(object->schema, name);
	if (index < 0)
		return WBEM_E_NOT_FOUND;
	VariantClear(&object->values[index]);
	if (!value || V_VT(value) == VT_EMPTY) {
		V_VT(&object->values[index]) = VT_NULL;
		return S_OK;
	}
	return VariantCopy(&object->values[index], value);
}

static HRESULT WINAPI object_delete(IWbemClassObject *iface, LPCWSTR name)
{
	class_object *object = object_from(iface);
	const LONG index = name ? property_index(object->schema, name) : -1;
	if (index < 0)
		return WBEM_E_NOT_FOUND;
	VariantClear(&object->values[index]);
	V_VT(&object->values[index]) = VT_NULL;
	return WBEM_S_RESET_TO_DEFAULT;
}

/* Which properties an enumeration of names or values covers: system ones, keys alone, or the others. */
static BOOL property_wanted(const class_object *object, LONG position, LONG flags)
{
	const LONG origin = flags & WBEM_MASK_CONDITION_ORIGIN;
	const BOOL system = position < (LONG)ARRAYSIZE(system_names);
	if (origin == WBEM_FLAG_SYSTEM_ONLY)
		return system;
	if (origin == WBEM_FLAG_NONSYSTEM_ONLY && system)
		return FALSE;
	if (origin == WBEM_FLAG_LOCAL_ONLY && (system || object->instance))
		return FALSE;
	if (origin == WBEM_FLAG_PROPAGATED_ONLY && (system || !object->instance))
		return FALSE;
	if (flags & WBEM_FLAG_KEYS_ONLY)
		return !system && object->schema->properties[position - ARRAYSIZE(system_names)].key;
	if (flags & WBEM_FLAG_REFS_ONLY)
		return FALSE;
	return TRUE;
}

static const WCHAR *position_name(const class_object *object, LONG position)
{
	return position < (LONG)ARRAYSIZE(system_names) ? system_names[position] : object->schema->properties[position - ARRAYSIZE(system_names)].name;
}

static HRESULT WINAPI object_get_names(IWbemClassObject *iface, LPCWSTR qualifier, LONG flags, VARIANT *qualifier_value, SAFEARRAY **names)
{
	class_object *object = object_from(iface);
	if (!names)
		return WBEM_E_INVALID_PARAMETER;
	const LONG total = (LONG)(ARRAYSIZE(system_names) + object->schema->count);
	LONG count = 0;
	for (LONG position = 0; position < total; position++)
		if (property_wanted(object, position, flags) && (!qualifier || (same_text(qualifier, L"key") && position >= (LONG)ARRAYSIZE(system_names) &&
			object->schema->properties[position - ARRAYSIZE(system_names)].key)))
			count++;
	SAFEARRAYBOUND bound = {(ULONG)count, 0};
	*names = SafeArrayCreate(VT_BSTR, 1, &bound);
	if (!*names)
		return E_OUTOFMEMORY;
	LONG at = 0;
	for (LONG position = 0; position < total; position++)
		if (property_wanted(object, position, flags) && (!qualifier || (same_text(qualifier, L"key") && position >= (LONG)ARRAYSIZE(system_names) &&
			object->schema->properties[position - ARRAYSIZE(system_names)].key))) {
			BSTR name = SysAllocString(position_name(object, position));
			SafeArrayPutElement(*names, &at, name);
			SysFreeString(name);
			at++;
		}
	return S_OK;
}

static HRESULT WINAPI object_begin_enumeration(IWbemClassObject *iface, LONG flags)
{
	class_object *object = object_from(iface);
	object->position = 0;
	object->enumeration = flags;
	return S_OK;
}

static HRESULT WINAPI object_next(IWbemClassObject *iface, LONG flags, BSTR *name, VARIANT *value, CIMTYPE *type, LONG *flavor)
{
	class_object *object = object_from(iface);
	if (object->position < 0)
		return WBEM_E_UNEXPECTED;
	const LONG total = (LONG)(ARRAYSIZE(system_names) + object->schema->count);
	while (object->position < total && !property_wanted(object, object->position, object->enumeration))
		object->position++;
	if (object->position >= total)
		return WBEM_S_NO_MORE_DATA;
	const WCHAR *found = position_name(object, object->position++);
	if (name)
		*name = SysAllocString(found);
	return object_get(iface, found, 0, value, type, flavor);
}

static HRESULT WINAPI object_end_enumeration(IWbemClassObject *iface)
{
	object_from(iface)->position = -1;
	return S_OK;
}

static HRESULT WINAPI object_get_property_qualifier_set(IWbemClassObject *iface, LPCWSTR property, IWbemQualifierSet **out)
{
	class_object *object = object_from(iface);
	if (!property)
		return WBEM_E_INVALID_PARAMETER;
	if (system_index(property) >= 0)
		return WBEM_E_SYSTEM_PROPERTY;
	const LONG index = property_index(object->schema, property);
	if (index < 0)
		return WBEM_E_NOT_FOUND;
	return create_qualifiers(object, index, out);
}

static class_object *copy_object(const class_object *source)
{
	class_object *copy = make_object(source->schema, source->instance);
	if (copy)
		for (ULONG i = 0; i < source->schema->count; i++)
			VariantCopy(&copy->values[i], &source->values[i]);
	return copy;
}

static HRESULT WINAPI object_clone(IWbemClassObject *iface, IWbemClassObject **out)
{
	if (!out)
		return WBEM_E_INVALID_PARAMETER;
	class_object *copy = copy_object(object_from(iface));
	*out = copy ? &copy->IWbemClassObject_iface : NULL;
	return copy ? S_OK : E_OUTOFMEMORY;
}

static const WCHAR *cim_type_name(CIMTYPE type)
{
	switch (type & ~CIM_FLAG_ARRAY) {
	case CIM_SINT8: return L"sint8";
	case CIM_UINT8: return L"uint8";
	case CIM_SINT16: return L"sint16";
	case CIM_UINT16: return L"uint16";
	case CIM_SINT32: return L"sint32";
	case CIM_UINT32: return L"uint32";
	case CIM_SINT64: return L"sint64";
	case CIM_UINT64: return L"uint64";
	case CIM_REAL32: return L"real32";
	case CIM_REAL64: return L"real64";
	case CIM_BOOLEAN: return L"boolean";
	case CIM_DATETIME: return L"datetime";
	case CIM_REFERENCE: return L"ref";
	case CIM_CHAR16: return L"char16";
	case CIM_OBJECT: return L"object";
	default: return L"string";
	}
}

/* The text of an object in the Managed Object Format WMI writes it in. */
static HRESULT WINAPI object_get_object_text(IWbemClassObject *iface, LONG flags, BSTR *text)
{
	class_object *object = object_from(iface);
	if (!text)
		return WBEM_E_INVALID_PARAMETER;
	text_builder builder = {0};
	append(&builder, object->instance ? L"\ninstance of " : L"\nclass ", -1);
	append(&builder, object->schema->name, -1);
	append(&builder, L"\n{\n", -1);
	for (ULONG i = 0; i < object->schema->count; i++) {
		const VARIANT *value = &object->values[i];
		if (object->instance && V_VT(value) == VT_NULL)
			continue;
		append(&builder, L"\t", 1);
		if (!object->instance) {
			append(&builder, cim_type_name(object->schema->properties[i].type), -1);
			append(&builder, L" ", 1);
		}
		append(&builder, object->schema->properties[i].name, -1);
		if (object->instance) {
			append(&builder, L" = ", 3);
			path_key key = {NULL, *value, object->schema->properties[i].type};
			append_key_value(&builder, &key);
		}
		append(&builder, L";\n", 2);
	}
	append(&builder, L"};\n", 3);
	*text = SysAllocString(built(&builder));
	wmi_free(builder.text);
	return *text ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI object_spawn_derived_class(IWbemClassObject *iface, LONG flags, IWbemClassObject **out)
{
	if (out)
		*out = NULL;
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI object_spawn_instance(IWbemClassObject *iface, LONG flags, IWbemClassObject **out)
{
	class_object *object = object_from(iface);
	if (!out)
		return WBEM_E_INVALID_PARAMETER;
	if (object->instance)
		return WBEM_E_INCOMPLETE_CLASS;
	class_object *instance = make_object(object->schema, TRUE);
	*out = instance ? &instance->IWbemClassObject_iface : NULL;
	return instance ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI object_compare_to(IWbemClassObject *iface, LONG flags, IWbemClassObject *other)
{
	class_object *object = object_from(iface);
	if (!other)
		return WBEM_E_INVALID_PARAMETER;
	if (other->lpVtbl != &object_vtbl)
		return WBEM_S_DIFFERENT;
	class_object *compared = object_from(other);
	if (compared->schema != object->schema || compared->instance != object->instance)
		return WBEM_S_DIFFERENT;
	for (ULONG i = 0; i < object->schema->count; i++)
		if (VarCmp(&object->values[i], &compared->values[i], LOCALE_USER_DEFAULT, 0) != VARCMP_EQ &&
			!(V_VT(&object->values[i]) == VT_NULL && V_VT(&compared->values[i]) == VT_NULL))
			return WBEM_S_DIFFERENT;
	return WBEM_S_SAME;
}

static HRESULT WINAPI object_get_property_origin(IWbemClassObject *iface, LPCWSTR name, BSTR *origin)
{
	class_object *object = object_from(iface);
	if (!name || !origin)
		return WBEM_E_INVALID_PARAMETER;
	if (system_index(name) < 0 && property_index(object->schema, name) < 0)
		return WBEM_E_NOT_FOUND;
	*origin = SysAllocString(object->schema->name);
	return S_OK;
}

static HRESULT WINAPI object_inherits_from(IWbemClassObject *iface, LPCWSTR ancestor)
{
	return WBEM_S_FALSE;
}

static HRESULT WINAPI object_get_method(IWbemClassObject *iface, LPCWSTR name, LONG flags, IWbemClassObject **in, IWbemClassObject **out)
{
	if (in)
		*in = NULL;
	if (out)
		*out = NULL;
	return WBEM_E_NOT_FOUND;
}

static HRESULT WINAPI object_put_method(IWbemClassObject *iface, LPCWSTR name, LONG flags, IWbemClassObject *in, IWbemClassObject *out)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI object_delete_method(IWbemClassObject *iface, LPCWSTR name)
{
	return WBEM_E_NOT_FOUND;
}

static HRESULT WINAPI object_begin_method_enumeration(IWbemClassObject *iface, LONG flags)
{
	return S_OK;
}

static HRESULT WINAPI object_next_method(IWbemClassObject *iface, LONG flags, BSTR *name, IWbemClassObject **in, IWbemClassObject **out)
{
	if (name)
		*name = NULL;
	if (in)
		*in = NULL;
	if (out)
		*out = NULL;
	return WBEM_S_NO_MORE_DATA;
}

static HRESULT WINAPI object_end_method_enumeration(IWbemClassObject *iface)
{
	return S_OK;
}

static HRESULT WINAPI object_get_method_qualifier_set(IWbemClassObject *iface, LPCWSTR name, IWbemQualifierSet **out)
{
	if (out)
		*out = NULL;
	return WBEM_E_NOT_FOUND;
}

static HRESULT WINAPI object_get_method_origin(IWbemClassObject *iface, LPCWSTR name, BSTR *origin)
{
	if (origin)
		*origin = NULL;
	return WBEM_E_NOT_FOUND;
}

static const IWbemClassObjectVtbl object_vtbl = {
	object_query, object_add_ref, object_release, object_get_qualifier_set, object_get, object_put, object_delete, object_get_names,
	object_begin_enumeration, object_next, object_end_enumeration, object_get_property_qualifier_set, object_clone, object_get_object_text,
	object_spawn_derived_class, object_spawn_instance, object_compare_to, object_get_property_origin, object_inherits_from, object_get_method,
	object_put_method, object_delete_method, object_begin_method_enumeration, object_next_method, object_end_method_enumeration,
	object_get_method_qualifier_set, object_get_method_origin,
};

/* The qualifiers of a class or one of its properties: CIMTYPE, key where it applies, and dynamic and provider for the class. */
typedef struct qualifier_set {
	IWbemQualifierSet IWbemQualifierSet_iface;
	LONG refs;
	class_object *object;
	LONG property, position;
} qualifier_set;

static qualifier_set *qualifiers_from(IWbemQualifierSet *iface)
{
	return CONTAINING_RECORD(iface, qualifier_set, IWbemQualifierSet_iface);
}

static const WCHAR *qualifier_name(const qualifier_set *set, LONG position)
{
	if (set->property < 0)
		return position == 0 ? L"dynamic" : position == 1 ? L"provider" : NULL;
	if (position == 0)
		return L"CIMTYPE";
	if (position == 1 && set->object->schema->properties[set->property].key)
		return L"key";
	return NULL;
}

static HRESULT qualifier_value(const qualifier_set *set, LPCWSTR name, VARIANT *value, LONG *flavor)
{
	VARIANT made;
	VariantInit(&made);
	if (set->property < 0 && same_text(name, L"dynamic")) {
		V_VT(&made) = VT_BOOL;
		V_BOOL(&made) = VARIANT_TRUE;
	} else if (set->property < 0 && same_text(name, L"provider")) {
		V_VT(&made) = VT_BSTR;
		V_BSTR(&made) = SysAllocString(L"CIMWin32");
	} else if (set->property >= 0 && same_text(name, L"CIMTYPE")) {
		V_VT(&made) = VT_BSTR;
		V_BSTR(&made) = SysAllocString(cim_type_name(set->object->schema->properties[set->property].type));
	} else if (set->property >= 0 && same_text(name, L"key") && set->object->schema->properties[set->property].key) {
		V_VT(&made) = VT_BOOL;
		V_BOOL(&made) = VARIANT_TRUE;
	} else {
		return WBEM_E_NOT_FOUND;
	}
	if (value)
		*value = made;
	else
		VariantClear(&made);
	if (flavor)
		*flavor = set->object->instance ? WBEM_FLAVOR_ORIGIN_PROPAGATED : WBEM_FLAVOR_ORIGIN_LOCAL;
	return S_OK;
}

static HRESULT WINAPI qualifiers_query(IWbemQualifierSet *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &qualifiers_iid)) {
		*out = iface;
		IWbemQualifierSet_AddRef(iface);
		return S_OK;
	}
	return unanswered("qualifier set", iid, out);
}

static ULONG WINAPI qualifiers_add_ref(IWbemQualifierSet *iface)
{
	return InterlockedIncrement(&qualifiers_from(iface)->refs);
}

static ULONG WINAPI qualifiers_release(IWbemQualifierSet *iface)
{
	qualifier_set *set = qualifiers_from(iface);
	const LONG refs = InterlockedDecrement(&set->refs);
	if (!refs) {
		IWbemClassObject_Release(&set->object->IWbemClassObject_iface);
		wmi_free(set);
		InterlockedDecrement(&locks);
	}
	return refs;
}

static HRESULT WINAPI qualifiers_get(IWbemQualifierSet *iface, LPCWSTR name, LONG flags, VARIANT *value, LONG *flavor)
{
	return name ? qualifier_value(qualifiers_from(iface), name, value, flavor) : WBEM_E_INVALID_PARAMETER;
}

static HRESULT WINAPI qualifiers_put(IWbemQualifierSet *iface, LPCWSTR name, VARIANT *value, LONG flavor)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI qualifiers_delete(IWbemQualifierSet *iface, LPCWSTR name)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI qualifiers_get_names(IWbemQualifierSet *iface, LONG flags, SAFEARRAY **names)
{
	qualifier_set *set = qualifiers_from(iface);
	if (!names)
		return WBEM_E_INVALID_PARAMETER;
	LONG count = 0;
	while (qualifier_name(set, count))
		count++;
	SAFEARRAYBOUND bound = {(ULONG)count, 0};
	*names = SafeArrayCreate(VT_BSTR, 1, &bound);
	if (!*names)
		return E_OUTOFMEMORY;
	for (LONG at = 0; at < count; at++) {
		BSTR name = SysAllocString(qualifier_name(set, at));
		SafeArrayPutElement(*names, &at, name);
		SysFreeString(name);
	}
	return S_OK;
}

static HRESULT WINAPI qualifiers_begin_enumeration(IWbemQualifierSet *iface, LONG flags)
{
	qualifiers_from(iface)->position = 0;
	return S_OK;
}

static HRESULT WINAPI qualifiers_next(IWbemQualifierSet *iface, LONG flags, BSTR *name, VARIANT *value, LONG *flavor)
{
	qualifier_set *set = qualifiers_from(iface);
	if (set->position < 0)
		return WBEM_E_UNEXPECTED;
	const WCHAR *found = qualifier_name(set, set->position);
	if (!found)
		return WBEM_S_NO_MORE_DATA;
	set->position++;
	if (name)
		*name = SysAllocString(found);
	return qualifier_value(set, found, value, flavor);
}

static HRESULT WINAPI qualifiers_end_enumeration(IWbemQualifierSet *iface)
{
	qualifiers_from(iface)->position = -1;
	return S_OK;
}

static const IWbemQualifierSetVtbl qualifiers_vtbl = {
	qualifiers_query, qualifiers_add_ref, qualifiers_release, qualifiers_get, qualifiers_put, qualifiers_delete, qualifiers_get_names,
	qualifiers_begin_enumeration, qualifiers_next, qualifiers_end_enumeration,
};

static HRESULT create_qualifiers(const class_object *object, LONG property, IWbemQualifierSet **out)
{
	if (!out)
		return WBEM_E_INVALID_PARAMETER;
	qualifier_set *set = wmi_alloc(sizeof(*set));
	if (!set)
		return E_OUTOFMEMORY;
	set->IWbemQualifierSet_iface.lpVtbl = &qualifiers_vtbl;
	set->refs = 1;
	set->object = (class_object *)object;
	set->property = property;
	set->position = -1;
	IWbemClassObject_AddRef(&set->object->IWbemClassObject_iface);
	InterlockedIncrement(&locks);
	*out = &set->IWbemQualifierSet_iface;
	return S_OK;
}

/* A query's results, taken when it runs; clones share the objects and keep a place of their own. */
typedef struct result_set {
	LONG refs;
	instance_list list;
} result_set;

/* Callers expect the proxies of a service in another process and set their security, so the services and the enumerators keep it and
 * give it back; it changes nothing here. */
typedef struct proxy_security {
	IClientSecurity IClientSecurity_iface;
	IUnknown *outer;
	DWORD authentication, authorization, level, impersonation, capabilities;
} proxy_security;

static proxy_security *security_from(IClientSecurity *iface)
{
	return CONTAINING_RECORD(iface, proxy_security, IClientSecurity_iface);
}

static HRESULT WINAPI security_query(IClientSecurity *iface, REFIID iid, void **out)
{
	return IUnknown_QueryInterface(security_from(iface)->outer, iid, out);
}

static ULONG WINAPI security_add_ref(IClientSecurity *iface)
{
	return IUnknown_AddRef(security_from(iface)->outer);
}

static ULONG WINAPI security_release(IClientSecurity *iface)
{
	return IUnknown_Release(security_from(iface)->outer);
}

static HRESULT WINAPI security_query_blanket(IClientSecurity *iface, IUnknown *proxy, DWORD *authentication, DWORD *authorization, OLECHAR **server,
	DWORD *level, DWORD *impersonation, void **identity, DWORD *capabilities)
{
	const proxy_security *security = security_from(iface);
	if (authentication)
		*authentication = security->authentication;
	if (authorization)
		*authorization = security->authorization;
	if (server)
		*server = NULL;
	if (level)
		*level = security->level;
	if (impersonation)
		*impersonation = security->impersonation;
	if (identity)
		*identity = NULL;
	if (capabilities)
		*capabilities = security->capabilities;
	return S_OK;
}

static HRESULT WINAPI security_set_blanket(IClientSecurity *iface, IUnknown *proxy, DWORD authentication, DWORD authorization, OLECHAR *server,
	DWORD level, DWORD impersonation, void *identity, DWORD capabilities)
{
	proxy_security *security = security_from(iface);
	if (authentication != RPC_C_AUTHN_DEFAULT)
		security->authentication = authentication;
	if (authorization != RPC_C_AUTHZ_DEFAULT)
		security->authorization = authorization;
	if (level != RPC_C_AUTHN_LEVEL_DEFAULT)
		security->level = level;
	if (impersonation != RPC_C_IMP_LEVEL_DEFAULT)
		security->impersonation = impersonation;
	if (capabilities != EOAC_DEFAULT)
		security->capabilities = capabilities;
	return S_OK;
}

/* There is no proxy, so a copy is the object itself. */
static HRESULT WINAPI security_copy_proxy(IClientSecurity *iface, IUnknown *proxy, IUnknown **copy)
{
	if (!proxy || !copy)
		return E_INVALIDARG;
	return IUnknown_QueryInterface(proxy, &unknown_iid, (void **)copy);
}

static const IClientSecurityVtbl security_vtbl = {
	security_query, security_add_ref, security_release, security_query_blanket, security_set_blanket, security_copy_proxy,
};

static void init_security(proxy_security *security, IUnknown *outer)
{
	security->IClientSecurity_iface.lpVtbl = &security_vtbl;
	security->outer = outer;
	security->authentication = RPC_C_AUTHN_WINNT;
	security->authorization = RPC_C_AUTHZ_NONE;
	security->level = RPC_C_AUTHN_LEVEL_CONNECT;
	security->impersonation = RPC_C_IMP_LEVEL_IMPERSONATE;
	security->capabilities = EOAC_NONE;
}

typedef struct object_enumerator {
	IEnumWbemClassObject IEnumWbemClassObject_iface;
	proxy_security security;
	LONG refs;
	result_set *results;
	ULONG position;
} object_enumerator;

static void release_results(result_set *results)
{
	if (InterlockedDecrement(&results->refs))
		return;
	for (ULONG i = 0; i < results->list.count; i++)
		IWbemClassObject_Release(&results->list.objects[i]->IWbemClassObject_iface);
	wmi_free(results->list.objects);
	wmi_free(results);
}

static object_enumerator *enumerator_from(IEnumWbemClassObject *iface)
{
	return CONTAINING_RECORD(iface, object_enumerator, IEnumWbemClassObject_iface);
}

static HRESULT WINAPI enumerator_query(IEnumWbemClassObject *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &enumerator_iid))
		*out = iface;
	else if (IsEqualIID(iid, &client_security_iid))
		*out = &enumerator_from(iface)->security.IClientSecurity_iface;
	else
		return unanswered("enumerator", iid, out);
	IEnumWbemClassObject_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI enumerator_add_ref(IEnumWbemClassObject *iface)
{
	return InterlockedIncrement(&enumerator_from(iface)->refs);
}

static ULONG WINAPI enumerator_release(IEnumWbemClassObject *iface)
{
	object_enumerator *enumerator = enumerator_from(iface);
	const LONG refs = InterlockedDecrement(&enumerator->refs);
	if (!refs) {
		release_results(enumerator->results);
		wmi_free(enumerator);
		InterlockedDecrement(&locks);
	}
	return refs;
}

static HRESULT WINAPI enumerator_reset(IEnumWbemClassObject *iface)
{
	enumerator_from(iface)->position = 0;
	return S_OK;
}

static HRESULT WINAPI enumerator_next(IEnumWbemClassObject *iface, LONG timeout, ULONG count, IWbemClassObject **objects, ULONG *returned)
{
	object_enumerator *enumerator = enumerator_from(iface);
	if (!objects || !returned)
		return WBEM_E_INVALID_PARAMETER;
	ULONG given = 0;
	while (given < count && enumerator->position < enumerator->results->list.count) {
		objects[given] = &enumerator->results->list.objects[enumerator->position++]->IWbemClassObject_iface;
		IWbemClassObject_AddRef(objects[given]);
		given++;
	}
	*returned = given;
	return given == count ? S_OK : WBEM_S_FALSE;
}

static HRESULT WINAPI enumerator_next_async(IEnumWbemClassObject *iface, ULONG count, IWbemObjectSink *sink)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT make_enumerator(result_set *results, ULONG position, IEnumWbemClassObject **out);

static HRESULT WINAPI enumerator_clone(IEnumWbemClassObject *iface, IEnumWbemClassObject **out)
{
	object_enumerator *enumerator = enumerator_from(iface);
	if (!out)
		return WBEM_E_INVALID_PARAMETER;
	InterlockedIncrement(&enumerator->results->refs);
	return make_enumerator(enumerator->results, enumerator->position, out);
}

static HRESULT WINAPI enumerator_skip(IEnumWbemClassObject *iface, LONG timeout, ULONG count)
{
	object_enumerator *enumerator = enumerator_from(iface);
	const ULONG left = enumerator->results->list.count - enumerator->position;
	enumerator->position += count < left ? count : left;
	return count <= left ? S_OK : WBEM_S_FALSE;
}

static const IEnumWbemClassObjectVtbl enumerator_vtbl = {
	enumerator_query, enumerator_add_ref, enumerator_release, enumerator_reset, enumerator_next, enumerator_next_async, enumerator_clone,
	enumerator_skip,
};

/* Takes over the caller's reference to the results. */
static HRESULT make_enumerator(result_set *results, ULONG position, IEnumWbemClassObject **out)
{
	object_enumerator *enumerator = wmi_alloc(sizeof(*enumerator));
	if (!enumerator) {
		release_results(results);
		return E_OUTOFMEMORY;
	}
	enumerator->IEnumWbemClassObject_iface.lpVtbl = &enumerator_vtbl;
	init_security(&enumerator->security, (IUnknown *)&enumerator->IEnumWbemClassObject_iface);
	enumerator->refs = 1;
	enumerator->results = results;
	enumerator->position = position;
	InterlockedIncrement(&locks);
	*out = &enumerator->IEnumWbemClassObject_iface;
	return S_OK;
}

/* WQL: SELECT * or a list of properties FROM a class, then WHERE comparisons of a property with a literal, joined by AND, OR and NOT. */
typedef enum token_kind { token_end, token_name, token_text, token_number, token_symbol, token_bad } token_kind;

typedef struct token {
	token_kind kind;
	const WCHAR *start;
	int length;
} token;

typedef struct query_parser {
	const WCHAR *at;
	token current;
	const class_object *row;
	BOOL failed;
} query_parser;

static void next_token(query_parser *parser)
{
	const WCHAR *at = parser->at;
	while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')
		at++;
	token *current = &parser->current;
	current->start = at;
	if (!*at) {
		current->kind = token_end;
		current->length = 0;
	} else if (is_name_character(*at) && !(*at >= '0' && *at <= '9')) {
		while (is_name_character(*at))
			at++;
		current->kind = token_name;
	} else if ((*at >= '0' && *at <= '9') || (*at == '-' && at[1] >= '0' && at[1] <= '9')) {
		at++;
		while ((*at >= '0' && *at <= '9') || *at == '.')
			at++;
		current->kind = token_number;
	} else if (*at == '"' || *at == '\'') {
		const WCHAR quote = *at++;
		while (*at && *at != quote) {
			if (*at == '\\' && at[1])
				at++;
			at++;
		}
		if (!*at) {
			current->kind = token_bad;
		} else {
			at++;
			current->kind = token_text;
		}
	} else if ((at[0] == '<' && (at[1] == '>' || at[1] == '=')) || (at[0] == '>' && at[1] == '=') || (at[0] == '!' && at[1] == '=')) {
		at += 2;
		current->kind = token_symbol;
	} else {
		at++;
		current->kind = token_symbol;
	}
	current->length = (int)(at - current->start);
	parser->at = at;
}

static BOOL token_is(const token *current, const WCHAR *word)
{
	return (current->kind == token_name || current->kind == token_symbol) && CompareStringOrdinal(current->start, current->length, word, -1, TRUE) == CSTR_EQUAL;
}

/* A literal of the query as a value: quoted text without its escapes, a number, TRUE, FALSE or NULL. */
static BOOL literal_value(const token *current, VARIANT *value)
{
	VariantInit(value);
	if (current->kind == token_text) {
		text_builder builder = {0};
		for (const WCHAR *at = current->start + 1; at < current->start + current->length - 1; at++) {
			if (*at == '\\' && at + 1 < current->start + current->length - 1)
				at++;
			append(&builder, at, 1);
		}
		V_VT(value) = VT_BSTR;
		V_BSTR(value) = SysAllocString(built(&builder));
		wmi_free(builder.text);
		return TRUE;
	}
	if (current->kind == token_number) {
		BSTR text = SysAllocStringLen(current->start, current->length);
		V_VT(value) = VT_BSTR;
		V_BSTR(value) = text;
		const HRESULT status = VariantChangeTypeEx(value, value, MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), 0, VT_R8);
		return SUCCEEDED(status);
	}
	if (token_is(current, L"TRUE") || token_is(current, L"FALSE")) {
		V_VT(value) = VT_BOOL;
		V_BOOL(value) = token_is(current, L"TRUE") ? VARIANT_TRUE : VARIANT_FALSE;
		return TRUE;
	}
	if (token_is(current, L"NULL")) {
		V_VT(value) = VT_NULL;
		return TRUE;
	}
	return FALSE;
}

/* % for any run, _ for one character, compared without case as WQL does. */
static BOOL like(const WCHAR *text, const WCHAR *pattern)
{
	if (!*pattern)
		return !*text;
	if (*pattern == '%') {
		for (;; text++) {
			if (like(text, pattern + 1))
				return TRUE;
			if (!*text)
				return FALSE;
		}
	}
	if (!*text)
		return FALSE;
	if (*pattern != '_' && CharUpperW((LPWSTR)(ULONG_PTR)*text) != CharUpperW((LPWSTR)(ULONG_PTR)*pattern))
		return FALSE;
	return like(text + 1, pattern + 1);
}

static BOOL compare(const VARIANT *property, const WCHAR *operation, const VARIANT *literal)
{
	if (V_VT(property) == VT_NULL || V_VT(literal) == VT_NULL)
		return FALSE;
	if (!lstrcmpiW(operation, L"LIKE")) {
		VARIANT text;
		VariantInit(&text);
		BOOL matched = V_VT(literal) == VT_BSTR && SUCCEEDED(VariantChangeType(&text, (VARIANT *)property, 0, VT_BSTR)) && like(V_BSTR(&text), V_BSTR(literal));
		VariantClear(&text);
		return matched;
	}
	/* A number on either side compares as numbers, text as text without case. */
	VARIANT left, right;
	VariantInit(&left);
	VariantInit(&right);
	HRESULT result;
	if (V_VT(literal) == VT_BSTR && V_VT(property) == VT_BSTR) {
		result = VarBstrCmp(V_BSTR(property), V_BSTR(literal), LOCALE_USER_DEFAULT, NORM_IGNORECASE);
	} else {
		const VARTYPE kind = V_VT(literal) == VT_BOOL || V_VT(property) == VT_BOOL ? VT_BOOL : VT_R8;
		if (FAILED(VariantChangeTypeEx(&left, (VARIANT *)property, MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), 0, kind)) ||
			FAILED(VariantChangeTypeEx(&right, (VARIANT *)literal, MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT), 0, kind)))
			return FALSE;
		result = VarCmp(&left, &right, LOCALE_USER_DEFAULT, 0);
	}
	VariantClear(&left);
	VariantClear(&right);
	if (!lstrcmpW(operation, L"="))
		return result == VARCMP_EQ;
	if (!lstrcmpW(operation, L"<>") || !lstrcmpW(operation, L"!="))
		return result == VARCMP_LT || result == VARCMP_GT;
	if (!lstrcmpW(operation, L"<"))
		return result == VARCMP_LT;
	if (!lstrcmpW(operation, L">"))
		return result == VARCMP_GT;
	if (!lstrcmpW(operation, L"<="))
		return result == VARCMP_LT || result == VARCMP_EQ;
	if (!lstrcmpW(operation, L">="))
		return result == VARCMP_GT || result == VARCMP_EQ;
	return FALSE;
}

static BOOL row_value(query_parser *parser, const token *name, VARIANT *value)
{
	WCHAR *text = wmi_copy(name->start, name->length);
	const HRESULT status = text ? object_get((IWbemClassObject *)&parser->row->IWbemClassObject_iface, text, 0, value, NULL, NULL) : E_OUTOFMEMORY;
	wmi_free(text);
	return SUCCEEDED(status);
}

static BOOL parse_or(query_parser *parser);

static BOOL parse_comparison(query_parser *parser)
{
	if (token_is(&parser->current, L"NOT")) {
		next_token(parser);
		return !parse_comparison(parser);
	}
	if (token_is(&parser->current, L"(")) {
		next_token(parser);
		const BOOL result = parse_or(parser);
		if (!token_is(&parser->current, L")"))
			parser->failed = TRUE;
		next_token(parser);
		return result;
	}
	token left = parser->current;
	next_token(parser);
	if (left.kind == token_name && token_is(&parser->current, L"IS")) {
		next_token(parser);
		BOOL negated = token_is(&parser->current, L"NOT");
		if (negated)
			next_token(parser);
		if (!token_is(&parser->current, L"NULL"))
			parser->failed = TRUE;
		next_token(parser);
		VARIANT value;
		if (!row_value(parser, &left, &value)) {
			parser->failed = TRUE;
			return FALSE;
		}
		const BOOL null = V_VT(&value) == VT_NULL;
		VariantClear(&value);
		return negated ? !null : null;
	}
	WCHAR operation[8] = {0};
	if (parser->current.kind != token_symbol && !token_is(&parser->current, L"LIKE")) {
		parser->failed = TRUE;
		return FALSE;
	}
	lstrcpynW(operation, parser->current.start, parser->current.length + 1 < 8 ? parser->current.length + 1 : 8);
	next_token(parser);
	token right = parser->current;
	next_token(parser);
	/* Either side may name the property; the other is the literal. */
	const BOOL swapped = left.kind != token_name || token_is(&left, L"TRUE") || token_is(&left, L"FALSE") || token_is(&left, L"NULL");
	const token *name = swapped ? &right : &left, *other = swapped ? &left : &right;
	VARIANT property, literal;
	if (name->kind != token_name || !literal_value(other, &literal)) {
		parser->failed = TRUE;
		return FALSE;
	}
	if (!row_value(parser, name, &property)) {
		VariantClear(&literal);
		parser->failed = TRUE;
		return FALSE;
	}
	if (swapped) {
		if (!lstrcmpW(operation, L"<"))
			lstrcpyW(operation, L">");
		else if (!lstrcmpW(operation, L">"))
			lstrcpyW(operation, L"<");
		else if (!lstrcmpW(operation, L"<="))
			lstrcpyW(operation, L">=");
		else if (!lstrcmpW(operation, L">="))
			lstrcpyW(operation, L"<=");
	}
	const BOOL result = compare(&property, operation, &literal);
	VariantClear(&property);
	VariantClear(&literal);
	return result;
}

static BOOL parse_and(query_parser *parser)
{
	BOOL result = parse_comparison(parser);
	while (token_is(&parser->current, L"AND")) {
		next_token(parser);
		result = parse_comparison(parser) && result;
	}
	return result;
}

static BOOL parse_or(query_parser *parser)
{
	BOOL result = parse_and(parser);
	while (token_is(&parser->current, L"OR")) {
		next_token(parser);
		result = parse_and(parser) || result;
	}
	return result;
}

/* Runs a query: the class's instances now, those its condition keeps. */
static HRESULT run_query(const WCHAR *query, result_set **out)
{
	query_parser parser = {query};
	next_token(&parser);
	if (!token_is(&parser.current, L"SELECT"))
		return WBEM_E_INVALID_QUERY;
	next_token(&parser);
	const WCHAR *selected = parser.current.start;
	while (parser.current.kind != token_end && !token_is(&parser.current, L"FROM")) {
		if (parser.current.kind == token_bad)
			return WBEM_E_INVALID_QUERY;
		next_token(&parser);
	}
	const WCHAR *selected_end = parser.current.start;
	if (!token_is(&parser.current, L"FROM"))
		return WBEM_E_INVALID_QUERY;
	next_token(&parser);
	if (parser.current.kind != token_name)
		return WBEM_E_INVALID_QUERY;
	const class_schema *schema = find_schema(parser.current.start, parser.current.length);
	if (!schema)
		return WBEM_E_INVALID_CLASS;
	next_token(&parser);
	/* The properties a query names must be the class's. */
	query_parser names = {selected};
	for (next_token(&names); names.current.start < selected_end; next_token(&names)) {
		if (names.current.kind == token_name) {
			WCHAR *name = wmi_copy(names.current.start, names.current.length);
			const BOOL known = name && (property_index(schema, name) >= 0 || system_index(name) >= 0);
			wmi_free(name);
			if (!known)
				return WBEM_E_INVALID_QUERY;
		} else if (!token_is(&names.current, L",") && !token_is(&names.current, L"*")) {
			return WBEM_E_INVALID_QUERY;
		}
	}
	const WCHAR *condition = NULL;
	if (token_is(&parser.current, L"WHERE"))
		condition = parser.at;
	else if (parser.current.kind != token_end)
		return WBEM_E_INVALID_QUERY;
	result_set *results = wmi_alloc(sizeof(*results));
	if (!results)
		return E_OUTOFMEMORY;
	results->refs = 1;
	instance_list all = {0};
	schema->fill(schema, &all);
	HRESULT status = S_OK;
	for (ULONG i = 0; i < all.count; i++) {
		BOOL kept = TRUE;
		if (condition) {
			query_parser where = {condition};
			where.row = all.objects[i];
			next_token(&where);
			kept = parse_or(&where);
			if (where.failed || where.current.kind != token_end)
				status = WBEM_E_INVALID_QUERY;
		}
		if (kept && status == S_OK)
			add_instance(&results->list, all.objects[i]);
		else
			IWbemClassObject_Release(&all.objects[i]->IWbemClassObject_iface);
	}
	wmi_free(all.objects);
	if (status != S_OK) {
		release_results(results);
		return status;
	}
	*out = results;
	return S_OK;
}

/* The namespace root\cimv2, served in the program that connected to it. */
typedef struct namespace_services {
	IWbemServices IWbemServices_iface;
	proxy_security security;
	LONG refs;
} namespace_services;

static namespace_services *services_from(IWbemServices *iface)
{
	return CONTAINING_RECORD(iface, namespace_services, IWbemServices_iface);
}

static HRESULT WINAPI services_query(IWbemServices *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &services_iid))
		*out = iface;
	else if (IsEqualIID(iid, &client_security_iid))
		*out = &services_from(iface)->security.IClientSecurity_iface;
	else
		return unanswered("services", iid, out);
	IWbemServices_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI services_add_ref(IWbemServices *iface)
{
	return InterlockedIncrement(&services_from(iface)->refs);
}

static ULONG WINAPI services_release(IWbemServices *iface)
{
	namespace_services *services = services_from(iface);
	const LONG refs = InterlockedDecrement(&services->refs);
	if (!refs) {
		wmi_free(services);
		InterlockedDecrement(&locks);
	}
	return refs;
}

static HRESULT WINAPI services_open_namespace(IWbemServices *iface, const BSTR name, LONG flags, IWbemContext *context, IWbemServices **out,
	IWbemCallResult **result)
{
	if (out)
		*out = NULL;
	return WBEM_E_INVALID_NAMESPACE;
}

static HRESULT WINAPI services_cancel_async_call(IWbemServices *iface, IWbemObjectSink *sink)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI services_query_object_sink(IWbemServices *iface, LONG flags, IWbemObjectSink **out)
{
	if (out)
		*out = NULL;
	return WBEM_E_NOT_SUPPORTED;
}

/* A path names a class, or an instance by its class and key. */
static HRESULT WINAPI services_get_object(IWbemServices *iface, const BSTR text, LONG flags, IWbemContext *context, IWbemClassObject **out,
	IWbemCallResult **result)
{
	if (!out)
		return WBEM_E_INVALID_PARAMETER;
	*out = NULL;
	if (!text || !*text)
		return WBEM_E_NOT_SUPPORTED;
	object_path path = {0};
	HRESULT status = parse_path(&path, WBEMPATH_CREATE_ACCEPT_ALL, text);
	const class_schema *schema = SUCCEEDED(status) && path.class_name ? find_schema(path.class_name, -1) : NULL;
	if (FAILED(status) || !path.class_name)
		status = WBEM_E_INVALID_OBJECT_PATH;
	else if (!schema)
		status = WBEM_E_INVALID_CLASS;
	else if (!path.key_count && !path.singleton) {
		class_object *object = make_object(schema, FALSE);
		*out = object ? &object->IWbemClassObject_iface : NULL;
		status = object ? S_OK : E_OUTOFMEMORY;
	} else {
		instance_list all = {0};
		schema->fill(schema, &all);
		status = WBEM_E_NOT_FOUND;
		for (ULONG i = 0; i < all.count; i++) {
			class_object *row = all.objects[i];
			BOOL matched = !path.singleton || all.count == 1;
			for (ULONG k = 0; k < path.key_count && matched; k++) {
				LONG index = -1;
				for (ULONG p = 0; p < schema->count && index < 0; p++)
					if (path.keys[k].name ? same_text(schema->properties[p].name, path.keys[k].name) : schema->properties[p].key)
						index = (LONG)p;
				matched = index >= 0 && compare(&row->values[index], L"=", &path.keys[k].value);
			}
			if (matched && status == WBEM_E_NOT_FOUND) {
				*out = &row->IWbemClassObject_iface;
				status = S_OK;
			} else {
				IWbemClassObject_Release(&row->IWbemClassObject_iface);
			}
		}
		wmi_free(all.objects);
	}
	clear_path(&path);
	return status;
}

static HRESULT WINAPI services_get_object_async(IWbemServices *iface, const BSTR path, LONG flags, IWbemContext *context, IWbemObjectSink *sink)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI services_put_class(IWbemServices *iface, IWbemClassObject *object, LONG flags, IWbemContext *context, IWbemCallResult **result)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI services_put_class_async(IWbemServices *iface, IWbemClassObject *object, LONG flags, IWbemContext *context, IWbemObjectSink *sink)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI services_delete_class(IWbemServices *iface, const BSTR name, LONG flags, IWbemContext *context, IWbemCallResult **result)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI services_delete_class_async(IWbemServices *iface, const BSTR name, LONG flags, IWbemContext *context, IWbemObjectSink *sink)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI services_create_class_enum(IWbemServices *iface, const BSTR superclass, LONG flags, IWbemContext *context,
	IEnumWbemClassObject **out)
{
	if (!out)
		return WBEM_E_INVALID_PARAMETER;
	if (superclass && *superclass)
		return WBEM_E_INVALID_CLASS;
	result_set *results = wmi_alloc(sizeof(*results));
	if (!results)
		return E_OUTOFMEMORY;
	results->refs = 1;
	for (ULONG i = 0; i < ARRAYSIZE(schemas); i++)
		add_instance(&results->list, make_object(&schemas[i], FALSE));
	return make_enumerator(results, 0, out);
}

static HRESULT WINAPI services_create_class_enum_async(IWbemServices *iface, const BSTR superclass, LONG flags, IWbemContext *context,
	IWbemObjectSink *sink)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI services_put_instance(IWbemServices *iface, IWbemClassObject *object, LONG flags, IWbemContext *context, IWbemCallResult **result)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI services_put_instance_async(IWbemServices *iface, IWbemClassObject *object, LONG flags, IWbemContext *context,
	IWbemObjectSink *sink)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI services_delete_instance(IWbemServices *iface, const BSTR path, LONG flags, IWbemContext *context, IWbemCallResult **result)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI services_delete_instance_async(IWbemServices *iface, const BSTR path, LONG flags, IWbemContext *context, IWbemObjectSink *sink)
{
	return WBEM_E_ACCESS_DENIED;
}

static HRESULT WINAPI services_exec_query(IWbemServices *iface, const BSTR language, const BSTR query, LONG flags, IWbemContext *context,
	IEnumWbemClassObject **out)
{
	if (!out || !query)
		return WBEM_E_INVALID_PARAMETER;
	*out = NULL;
	if (!language || !same_text(language, L"WQL"))
		return WBEM_E_INVALID_QUERY_TYPE;
	result_set *results = NULL;
	const HRESULT status = run_query(query, &results);
	return FAILED(status) ? status : make_enumerator(results, 0, out);
}

static HRESULT WINAPI services_create_instance_enum(IWbemServices *iface, const BSTR name, LONG flags, IWbemContext *context, IEnumWbemClassObject **out)
{
	if (!name)
		return WBEM_E_INVALID_PARAMETER;
	text_builder builder = {0};
	append(&builder, L"SELECT * FROM ", -1);
	append(&builder, name, -1);
	BSTR query = SysAllocString(built(&builder)), language = SysAllocString(L"WQL");
	wmi_free(builder.text);
	const HRESULT status = services_exec_query(iface, language, query, flags, context, out);
	SysFreeString(query);
	SysFreeString(language);
	return status;
}

static HRESULT WINAPI services_create_instance_enum_async(IWbemServices *iface, const BSTR name, LONG flags, IWbemContext *context, IWbemObjectSink *sink)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI services_exec_query_async(IWbemServices *iface, const BSTR language, const BSTR query, LONG flags, IWbemContext *context,
	IWbemObjectSink *sink)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI services_exec_notification_query(IWbemServices *iface, const BSTR language, const BSTR query, LONG flags, IWbemContext *context,
	IEnumWbemClassObject **out)
{
	if (out)
		*out = NULL;
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI services_exec_notification_query_async(IWbemServices *iface, const BSTR language, const BSTR query, LONG flags,
	IWbemContext *context, IWbemObjectSink *sink)
{
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI services_exec_method(IWbemServices *iface, const BSTR path, const BSTR method, LONG flags, IWbemContext *context,
	IWbemClassObject *in, IWbemClassObject **out, IWbemCallResult **result)
{
	if (out)
		*out = NULL;
	return WBEM_E_NOT_SUPPORTED;
}

static HRESULT WINAPI services_exec_method_async(IWbemServices *iface, const BSTR path, const BSTR method, LONG flags, IWbemContext *context,
	IWbemClassObject *in, IWbemObjectSink *sink)
{
	return WBEM_E_NOT_SUPPORTED;
}

static const IWbemServicesVtbl services_vtbl = {
	services_query, services_add_ref, services_release, services_open_namespace, services_cancel_async_call, services_query_object_sink,
	services_get_object, services_get_object_async, services_put_class, services_put_class_async, services_delete_class,
	services_delete_class_async, services_create_class_enum, services_create_class_enum_async, services_put_instance,
	services_put_instance_async, services_delete_instance, services_delete_instance_async, services_create_instance_enum,
	services_create_instance_enum_async, services_exec_query, services_exec_query_async, services_exec_notification_query,
	services_exec_notification_query_async, services_exec_method, services_exec_method_async,
};

/* The locator connects to root\cimv2 of this machine, named with or without the machine before it. */
typedef struct locator {
	IWbemLocator IWbemLocator_iface;
	LONG refs;
} locator;

static locator *locator_from(IWbemLocator *iface)
{
	return CONTAINING_RECORD(iface, locator, IWbemLocator_iface);
}

static HRESULT WINAPI locator_query(IWbemLocator *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &locator_iid)) {
		*out = iface;
		IWbemLocator_AddRef(iface);
		return S_OK;
	}
	return unanswered("locator", iid, out);
}

static ULONG WINAPI locator_add_ref(IWbemLocator *iface)
{
	return InterlockedIncrement(&locator_from(iface)->refs);
}

static ULONG WINAPI locator_release(IWbemLocator *iface)
{
	locator *found = locator_from(iface);
	const LONG refs = InterlockedDecrement(&found->refs);
	if (!refs) {
		wmi_free(found);
		InterlockedDecrement(&locks);
	}
	return refs;
}

static HRESULT WINAPI locator_connect_server(IWbemLocator *iface, const BSTR resource, const BSTR user, const BSTR password, const BSTR locale,
	LONG flags, const BSTR authority, IWbemContext *context, IWbemServices **out)
{
	if (!out || !resource)
		return WBEM_E_INVALID_PARAMETER;
	*out = NULL;
	object_path path = {0};
	HRESULT status = parse_path(&path, WBEMPATH_CREATE_ACCEPT_ALL | WBEMPATH_TREAT_SINGLE_IDENT_AS_NS, resource);
	const BOOL cimv2 = SUCCEEDED(status) && !path.class_name && path.namespace_count == 2 && same_text(path.namespaces[0], L"root") &&
		same_text(path.namespaces[1], L"cimv2");
	const BOOL local = SUCCEEDED(status) && local_machine(path.server);
	clear_path(&path);
	if (!local)
		return WBEM_E_TRANSPORT_FAILURE;
	if (!cimv2)
		return WBEM_E_INVALID_NAMESPACE;
	namespace_services *services = wmi_alloc(sizeof(*services));
	if (!services)
		return E_OUTOFMEMORY;
	services->IWbemServices_iface.lpVtbl = &services_vtbl;
	init_security(&services->security, (IUnknown *)&services->IWbemServices_iface);
	services->refs = 1;
	InterlockedIncrement(&locks);
	*out = &services->IWbemServices_iface;
	return S_OK;
}

static const IWbemLocatorVtbl locator_vtbl = {locator_query, locator_add_ref, locator_release, locator_connect_server};

static HRESULT create_locator(REFIID iid, void **out)
{
	locator *made = wmi_alloc(sizeof(*made));
	if (!made)
		return E_OUTOFMEMORY;
	made->IWbemLocator_iface.lpVtbl = &locator_vtbl;
	made->refs = 1;
	InterlockedIncrement(&locks);
	const HRESULT status = locator_query(&made->IWbemLocator_iface, iid, out);
	IWbemLocator_Release(&made->IWbemLocator_iface);
	return status;
}

/* Class factories of the path parser and the locator. */
typedef struct class_factory {
	IClassFactory IClassFactory_iface;
	HRESULT (*create)(REFIID iid, void **out);
} class_factory;

static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &unknown_iid) || IsEqualIID(iid, &factory_iid)) {
		*out = iface;
		return S_OK;
	}
	return unanswered("class factory", iid, out);
}

static ULONG WINAPI factory_add_ref(IClassFactory *iface)
{
	return 2;
}

static ULONG WINAPI factory_release(IClassFactory *iface)
{
	return 1;
}

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return CONTAINING_RECORD(iface, class_factory, IClassFactory_iface)->create(iid, out);
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL lock)
{
	if (lock)
		InterlockedIncrement(&locks);
	else
		InterlockedDecrement(&locks);
	return S_OK;
}

static const IClassFactoryVtbl factory_vtbl = {factory_query, factory_add_ref, factory_release, factory_create, factory_lock};
static class_factory path_factory = {{&factory_vtbl}, create_path};
static class_factory locator_factory = {{&factory_vtbl}, create_locator};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	class_factory *factory = IsEqualCLSID(clsid, &path_class) ? &path_factory : IsEqualCLSID(clsid, &locator_class) ? &locator_factory : NULL;
	if (!factory)
		return CLASS_E_CLASSNOTAVAILABLE;
	return factory_query(&factory->IClassFactory_iface, iid, out);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return locks ? S_FALSE : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
