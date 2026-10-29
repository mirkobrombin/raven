/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven HLSL compiler component: d3dcompiler_47.dll for programs that compile their shaders at run time.
 *
 * The compiler itself lives in the runtime (native/shader). This component packs each call into a request for it through
 * ravenhost.dll and turns the answer back into blobs. An #include stops a compile: the runtime names the file it needs, the component
 * opens it through the program's ID3DInclude (or from disk for D3D_COMPILE_STANDARD_FILE_INCLUDE) and asks again with every file read so
 * far attached. Reflection is read here, from the RDEF, signature and shader chunks of the container. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <stddef.h>
#include <string.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define GROW(block, size) ((block) ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (block), (size)) : ALLOC(size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))
#define EXPORT __declspec(dllexport)

__declspec(dllimport) void WINAPI RavenTrace(const char *text);
__declspec(dllimport) UINT WINAPI RavenShaderRequest(const void *request, UINT size);
__declspec(dllimport) UINT WINAPI RavenShaderResult(UINT handle, UINT part, void *buffer, UINT capacity);
__declspec(dllimport) void WINAPI RavenShaderRelease(UINT handle);

/* The runtime's request modes and answer states (native/nt/shader_service.cpp). */
enum { MODE_COMPILE, MODE_PREPROCESS, MODE_PART, MODE_STRIP, MODE_SET_PART, MODE_DISASSEMBLE };
enum { DONE, FAILED, NEEDS_INCLUDE, INVALID };
enum { PART_CODE, PART_MESSAGES, PART_INCLUDE, PART_STATUS };

static size_t length_of(const char *text)
{
	size_t length = 0;
	while (text[length])
		++length;
	return length;
}

static BOOL same_text(const char *a, const char *b)
{
	while (*a && *a == *b)
		++a, ++b;
	return *a == *b;
}

static HRESULT unimplemented(const char *name)
{
	char text[128];
	size_t at = 0, length;
	const char *part[] = {"d3dcompiler: ", name, " is not implemented"};
	int index;
	for (index = 0; index < 3; ++index)
		for (length = 0; part[index][length] && at < sizeof(text) - 1; ++length)
			text[at++] = part[index][length];
	text[at] = 0;
	RavenTrace(text);
	return E_NOTIMPL;
}

/* Blobs: the bytes follow the object. */

typedef struct {
	ID3DBlob iface;
	LONG refs;
	SIZE_T size;
	BYTE *data;
} blob;

static HRESULT STDMETHODCALLTYPE blob_query(ID3DBlob *iface, REFIID riid, void **out)
{
	if (IsEqualGUID(riid, &IID_ID3D10Blob) || IsEqualGUID(riid, &IID_IUnknown)) {
		*out = iface;
		ID3D10Blob_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE blob_add_ref(ID3DBlob *iface)
{
	return (ULONG)InterlockedIncrement(&((blob *)iface)->refs);
}

static ULONG STDMETHODCALLTYPE blob_release(ID3DBlob *iface)
{
	const LONG refs = InterlockedDecrement(&((blob *)iface)->refs);
	if (!refs)
		FREE(iface);
	return (ULONG)refs;
}

static void *STDMETHODCALLTYPE blob_pointer(ID3DBlob *iface)
{
	return ((blob *)iface)->data;
}

static SIZE_T STDMETHODCALLTYPE blob_size(ID3DBlob *iface)
{
	return ((blob *)iface)->size;
}

static ID3D10BlobVtbl blob_vtbl = {blob_query, blob_add_ref, blob_release, blob_pointer, blob_size};

static blob *new_blob(SIZE_T size)
{
	blob *object = ALLOC(sizeof(blob) + size + 1);
	if (!object)
		return NULL;
	object->iface.lpVtbl = &blob_vtbl;
	object->refs = 1;
	object->size = size;
	object->data = (BYTE *)(object + 1);
	return object;
}

static HRESULT blob_of(const void *data, SIZE_T size, ID3DBlob **out)
{
	blob *object = new_blob(size);
	*out = NULL;
	if (!object)
		return E_OUTOFMEMORY;
	if (size)
		memcpy(object->data, data, size);
	*out = &object->iface;
	return S_OK;
}

/* A text blob keeps its terminator inside the size, as the compiler's messages and listings do. */
static HRESULT text_blob(const char *text, size_t length, ID3DBlob **out)
{
	blob *object = new_blob(length + 1);
	*out = NULL;
	if (!object)
		return E_OUTOFMEMORY;
	memcpy(object->data, text, length);
	object->data[length] = 0;
	*out = &object->iface;
	return S_OK;
}

EXPORT HRESULT WINAPI D3DCreateBlob(SIZE_T size, ID3DBlob **out)
{
	blob *object;
	if (!out)
		return E_INVALIDARG;
	object = new_blob(size);
	*out = object ? &object->iface : NULL;
	return object ? S_OK : E_OUTOFMEMORY;
}

/* Requests: 32-bit words and length-prefixed byte strings. */

typedef struct {
	BYTE *data;
	size_t size, capacity;
	BOOL failed;
} request;

static void put(request *out, const void *bytes, size_t size)
{
	if (out->failed)
		return;
	if (out->size + size > out->capacity) {
		size_t capacity = out->capacity ? out->capacity : 256;
		BYTE *grown;
		while (capacity < out->size + size)
			capacity *= 2;
		grown = GROW(out->data, capacity);
		if (!grown) {
			out->failed = TRUE;
			return;
		}
		out->data = grown;
		out->capacity = capacity;
	}
	if (size)
		memcpy(out->data + out->size, bytes, size);
	out->size += size;
}

static void put_word(request *out, UINT value)
{
	put(out, &value, 4);
}

static void put_bytes(request *out, const void *bytes, size_t size)
{
	put_word(out, (UINT)size);
	put(out, bytes, size);
}

static void put_text(request *out, const char *text)
{
	put_bytes(out, text ? text : "", text ? length_of(text) : 0);
}

/* Sends the request and frees it. Returns the answer's handle, or 0 when the runtime could not read it. */
static UINT send(request *out, UINT *status, UINT *system)
{
	UINT words[2] = {INVALID, 0}, handle = 0;
	if (!out->failed && out->size <= 0xffffffffu)
		handle = RavenShaderRequest(out->data, (UINT)out->size);
	FREE(out->data);
	out->data = NULL;
	if (handle)
		RavenShaderResult(handle, PART_STATUS, words, sizeof(words));
	*status = words[0];
	if (system)
		*system = words[1];
	return handle;
}

/* One part of an answer as a blob; an empty part gives no blob. */
static HRESULT answer_part(UINT handle, UINT part, BOOL text, ID3DBlob **out)
{
	const UINT size = RavenShaderResult(handle, part, NULL, 0);
	blob *object;
	*out = NULL;
	if (!size)
		return S_OK;
	object = new_blob(text ? size + 1 : size);
	if (!object)
		return E_OUTOFMEMORY;
	RavenShaderResult(handle, part, object->data, size);
	if (text)
		object->data[size] = 0;
	*out = &object->iface;
	return S_OK;
}

static BOOL is_container(const void *data, SIZE_T size)
{
	const BYTE *bytes = data;
	return data && size >= 32 && bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'B' && bytes[3] == 'C';
}

/* Compiling and preprocessing. */

typedef struct {
	char *name;
	BYTE *data;
	UINT size;
} include_file;

typedef struct {
	include_file *files;
	UINT count;
} include_set;

static void free_includes(include_set *set)
{
	UINT index;
	for (index = 0; index < set->count; ++index) {
		FREE(set->files[index].name);
		FREE(set->files[index].data);
	}
	FREE(set->files);
}

/* Reads an include from disk next to the file that started the compile, then from the current directory. */
static BOOL read_standard_include(const char *base, UINT codepage, const char *name, BYTE **data, UINT *size)
{
	char path[MAX_PATH * 3];
	WCHAR wide[MAX_PATH];
	size_t directory = 0, index, length = length_of(name);
	int attempt;
	if (base)
		for (index = 0; base[index]; ++index)
			if (base[index] == '\\' || base[index] == '/')
				directory = index + 1;
	for (attempt = 0; attempt < 2; ++attempt) {
		HANDLE file;
		DWORD high = 0, low, read = 0;
		const size_t prefix = attempt == 0 ? directory : 0;
		if (attempt == 1 && !directory)
			break;
		if (prefix + length >= sizeof(path))
			continue;
		memcpy(path, base, prefix);
		memcpy(path + prefix, name, length + 1);
		if (!MultiByteToWideChar(codepage, 0, path, -1, wide, MAX_PATH))
			continue;
		file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			continue;
		low = GetFileSize(file, &high);
		if (high || low == INVALID_FILE_SIZE || !(*data = ALLOC(low + 1))) {
			CloseHandle(file);
			return FALSE;
		}
		if (!ReadFile(file, *data, low, &read, NULL) || read != low) {
			FREE(*data);
			CloseHandle(file);
			return FALSE;
		}
		CloseHandle(file);
		*size = low;
		return TRUE;
	}
	return FALSE;
}

static BOOL open_include(ID3DInclude *include, const char *base, UINT codepage, const char *name, BOOL system, include_file *file)
{
	const void *data = NULL;
	UINT size = 0;
	size_t length = length_of(name);
	if (!(file->name = ALLOC(length + 1)))
		return FALSE;
	memcpy(file->name, name, length + 1);
	if (include == D3D_COMPILE_STANDARD_FILE_INCLUDE)
		return read_standard_include(base, codepage, name, &file->data, &file->size);
	if (FAILED(ID3DInclude_Open(include, system ? D3D_INCLUDE_SYSTEM : D3D_INCLUDE_LOCAL, name, NULL, &data, &size)))
		return FALSE;
	file->data = ALLOC(size + 1);
	if (file->data) {
		if (size)
			memcpy(file->data, data, size);
		file->size = size;
	}
	ID3DInclude_Close(include, data);
	return file->data != NULL;
}

static HRESULT include_failure(const char *source, const char *name, ID3DBlob **errors)
{
	char text[MAX_PATH * 2 + 96];
	size_t at = 0, length;
	const char *part[] = {source && *source ? source : "memory", "(1,1): error X1507: failed to open source file: '", name, "'\n"};
	int index;
	for (index = 0; index < 4; ++index)
		for (length = 0; part[index][length] && at < sizeof(text) - 1; ++length)
			text[at++] = part[index][length];
	if (errors)
		text_blob(text, at, errors);
	return E_FAIL;
}

static HRESULT run_compile(UINT mode, const void *data, SIZE_T size, const char *name, UINT codepage, const D3D_SHADER_MACRO *defines,
	ID3DInclude *include, const char *entry, const char *target, UINT flags, ID3DBlob **code, ID3DBlob **errors)
{
	include_set files = {NULL, 0};
	HRESULT result = E_FAIL;
	if (code)
		*code = NULL;
	if (errors)
		*errors = NULL;
	if (!data || !code)
		return E_INVALIDARG;
	for (;;) {
		request out = {NULL, 0, 0, FALSE};
		const D3D_SHADER_MACRO *macro;
		UINT count = 0, index, status, system, handle;
		put_word(&out, mode);
		put_word(&out, flags);
		put_bytes(&out, data, size);
		put_text(&out, name);
		put_text(&out, entry);
		put_text(&out, target);
		for (macro = defines; macro && macro->Name; ++macro)
			++count;
		put_word(&out, count);
		for (macro = defines; macro && macro->Name; ++macro) {
			put_text(&out, macro->Name);
			/* A macro without a value is defined as 1, as on the command line. */
			put_text(&out, macro->Definition ? macro->Definition : "1");
		}
		put_word(&out, files.count);
		for (index = 0; index < files.count; ++index) {
			put_text(&out, files.files[index].name);
			put_bytes(&out, files.files[index].data, files.files[index].size);
		}
		handle = send(&out, &status, &system);
		if (!handle) {
			result = out.failed ? E_OUTOFMEMORY : E_FAIL;
			break;
		}
		if (status == NEEDS_INCLUDE) {
			const UINT needed = RavenShaderResult(handle, PART_INCLUDE, NULL, 0);
			char *wanted = ALLOC(needed + 1);
			include_file *grown;
			BOOL opened = FALSE;
			if (wanted)
				RavenShaderResult(handle, PART_INCLUDE, wanted, needed);
			RavenShaderRelease(handle);
			grown = wanted && files.count < 256 ? GROW(files.files, (files.count + 1) * sizeof(include_file)) : NULL;
			if (grown) {
				files.files = grown;
				opened = include && open_include(include, name, codepage, wanted, system, &files.files[files.count]);
				++files.count;
			}
			if (!opened) {
				result = include_failure(name, wanted ? wanted : "", errors);
				FREE(wanted);
				break;
			}
			FREE(wanted);
			continue;
		}
		if (status == DONE || status == FAILED) {
			result = answer_part(handle, PART_CODE, mode == MODE_PREPROCESS, code);
			if (SUCCEEDED(result) && errors)
				result = answer_part(handle, PART_MESSAGES, TRUE, errors);
			if (SUCCEEDED(result) && status == FAILED)
				result = E_FAIL;
		}
		RavenShaderRelease(handle);
		break;
	}
	free_includes(&files);
	return result;
}

EXPORT HRESULT WINAPI D3DCompile2(const void *data, SIZE_T size, const char *name, const D3D_SHADER_MACRO *defines, ID3DInclude *include,
	const char *entry, const char *target, UINT flags, UINT effect_flags, UINT secondary_flags, const void *secondary, SIZE_T secondary_size,
	ID3DBlob **code, ID3DBlob **errors)
{
	if (!target || !entry) {
		if (code)
			*code = NULL;
		if (errors)
			*errors = NULL;
		return E_INVALIDARG;
	}
	/* Effects (fx_*) and secondary data would change what comes out; they are refused rather than ignored. */
	if ((target[0] == 'f' && target[1] == 'x' && target[2] == '_') || (secondary_flags && secondary)) {
		if (code)
			*code = NULL;
		if (errors)
			*errors = NULL;
		return unimplemented(secondary_flags ? "D3DCompile2 secondary data" : "D3DCompile effect targets");
	}
	return run_compile(MODE_COMPILE, data, size, name, CP_ACP, defines, include, entry, target, flags, code, errors);
}

EXPORT HRESULT WINAPI D3DCompile(const void *data, SIZE_T size, const char *name, const D3D_SHADER_MACRO *defines, ID3DInclude *include,
	const char *entry, const char *target, UINT flags, UINT effect_flags, ID3DBlob **code, ID3DBlob **errors)
{
	return D3DCompile2(data, size, name, defines, include, entry, target, flags, effect_flags, 0, NULL, 0, code, errors);
}

EXPORT HRESULT WINAPI D3DPreprocess(const void *data, SIZE_T size, const char *name, const D3D_SHADER_MACRO *defines, ID3DInclude *include,
	ID3DBlob **text, ID3DBlob **errors)
{
	return run_compile(MODE_PREPROCESS, data, size, name, CP_ACP, defines, include, "", "", 0, text, errors);
}

static HRESULT read_whole_file(const WCHAR *path, BYTE **data, DWORD *size)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD high = 0, read = 0;
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());
	*size = GetFileSize(file, &high);
	if (high || *size == INVALID_FILE_SIZE) {
		CloseHandle(file);
		return E_OUTOFMEMORY;
	}
	if (!(*data = ALLOC(*size + 1))) {
		CloseHandle(file);
		return E_OUTOFMEMORY;
	}
	if (!ReadFile(file, *data, *size, &read, NULL) || read != *size) {
		const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
		FREE(*data);
		CloseHandle(file);
		return FAILED(error) ? error : E_FAIL;
	}
	CloseHandle(file);
	return S_OK;
}

EXPORT HRESULT WINAPI D3DCompileFromFile(const WCHAR *path, const D3D_SHADER_MACRO *defines, ID3DInclude *include, const char *entry,
	const char *target, UINT flags, UINT effect_flags, ID3DBlob **code, ID3DBlob **errors)
{
	char name[MAX_PATH * 3];
	BYTE *data;
	DWORD size;
	HRESULT result;
	if (code)
		*code = NULL;
	if (errors)
		*errors = NULL;
	if (!path || !entry || !target)
		return E_INVALIDARG;
	if (FAILED(result = read_whole_file(path, &data, &size)))
		return result;
	/* Messages and standard includes use the path as given, in UTF-8. */
	if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, name, sizeof(name), NULL, NULL))
		name[0] = 0;
	if (target[0] == 'f' && target[1] == 'x' && target[2] == '_')
		result = unimplemented("D3DCompile effect targets");
	else
		result = run_compile(MODE_COMPILE, data, size, name, CP_UTF8, defines, include, entry, target, flags, code, errors);
	FREE(data);
	return result;
}

/* Container parts. */

static HRESULT container_request(UINT mode, const void *data, SIZE_T size, UINT word, const void *extra, SIZE_T extra_size, BOOL text,
	ID3DBlob **out)
{
	request in = {NULL, 0, 0, FALSE};
	UINT status, handle;
	HRESULT result = E_FAIL;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!is_container(data, size))
		return E_FAIL;
	put_word(&in, mode);
	put_bytes(&in, data, size);
	put_word(&in, word);
	if (mode == MODE_SET_PART)
		put_bytes(&in, extra, extra_size);
	if (!(handle = send(&in, &status, NULL)))
		return in.failed ? E_OUTOFMEMORY : E_FAIL;
	if (status == DONE)
		result = answer_part(handle, PART_CODE, text, out);
	RavenShaderRelease(handle);
	if (SUCCEEDED(result) && !*out)
		result = E_FAIL;
	return result;
}

EXPORT HRESULT WINAPI D3DGetBlobPart(const void *data, SIZE_T size, D3D_BLOB_PART part, UINT flags, ID3DBlob **out)
{
	return container_request(MODE_PART, data, size, (UINT)part, NULL, 0, FALSE, out);
}

EXPORT HRESULT WINAPI D3DGetInputSignatureBlob(const void *data, SIZE_T size, ID3DBlob **out)
{
	return D3DGetBlobPart(data, size, D3D_BLOB_INPUT_SIGNATURE_BLOB, 0, out);
}

EXPORT HRESULT WINAPI D3DGetOutputSignatureBlob(const void *data, SIZE_T size, ID3DBlob **out)
{
	return D3DGetBlobPart(data, size, D3D_BLOB_OUTPUT_SIGNATURE_BLOB, 0, out);
}

EXPORT HRESULT WINAPI D3DGetInputAndOutputSignatureBlob(const void *data, SIZE_T size, ID3DBlob **out)
{
	return D3DGetBlobPart(data, size, D3D_BLOB_INPUT_AND_OUTPUT_SIGNATURE_BLOB, 0, out);
}

EXPORT HRESULT WINAPI D3DGetDebugInfo(const void *data, SIZE_T size, ID3DBlob **out)
{
	return D3DGetBlobPart(data, size, D3D_BLOB_DEBUG_INFO, 0, out);
}

EXPORT HRESULT WINAPI D3DStripShader(const void *data, SIZE_T size, UINT flags, ID3DBlob **out)
{
	return container_request(MODE_STRIP, data, size, flags, NULL, 0, FALSE, out);
}

EXPORT HRESULT WINAPI D3DSetBlobPart(const void *data, SIZE_T size, D3D_BLOB_PART part, UINT flags, const void *part_data, SIZE_T part_size,
	ID3DBlob **out)
{
	return container_request(MODE_SET_PART, data, size, (UINT)part, part_data, part_size, FALSE, out);
}

EXPORT HRESULT WINAPI D3DDisassemble(const void *data, SIZE_T size, UINT flags, const char *comments, ID3DBlob **out)
{
	return container_request(MODE_DISASSEMBLE, data, size, flags, NULL, 0, TRUE, out);
}

EXPORT HRESULT WINAPI D3DReadFileToBlob(const WCHAR *path, ID3DBlob **out)
{
	BYTE *data;
	DWORD size;
	HRESULT result;
	if (!path || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(result = read_whole_file(path, &data, &size)))
		return result;
	result = blob_of(data, size, out);
	FREE(data);
	return result;
}

EXPORT HRESULT WINAPI D3DWriteBlobToFile(ID3DBlob *source, const WCHAR *path, BOOL overwrite)
{
	HANDLE file;
	DWORD written = 0, size;
	BOOL wrote;
	if (!source || !path)
		return E_INVALIDARG;
	file = CreateFileW(path, GENERIC_WRITE, 0, NULL, overwrite ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());
	size = (DWORD)ID3D10Blob_GetBufferSize(source);
	wrote = WriteFile(file, ID3D10Blob_GetBufferPointer(source), size, &written, NULL) && written == size;
	CloseHandle(file);
	return wrote ? S_OK : E_FAIL;
}

/* Reflection: everything is read once, when the reflector is made, from a copy of the container it keeps. */

typedef struct reflected_type {
	ID3D11ShaderReflectionType iface;
	D3D11_SHADER_TYPE_DESC desc;
	struct reflected_type **members;
	const char **member_names;
	struct reflected_type *next;
} reflected_type;

typedef struct reflected_buffer reflected_buffer;

typedef struct {
	ID3D11ShaderReflectionVariable iface;
	D3D11_SHADER_VARIABLE_DESC desc;
	reflected_type *type;
	reflected_buffer *buffer;
} reflected_variable;

struct reflected_buffer {
	ID3D11ShaderReflectionConstantBuffer iface;
	D3D11_SHADER_BUFFER_DESC desc;
	reflected_variable *variables;
};

typedef struct {
	D3D11_SIGNATURE_PARAMETER_DESC *elements;
	UINT count;
} reflected_signature;

typedef struct {
	ID3D11ShaderReflection iface;
	LONG refs;
	BYTE *data;
	SIZE_T size;
	D3D11_SHADER_DESC desc;
	reflected_buffer *buffers;
	D3D11_SHADER_INPUT_BIND_DESC *bindings;
	reflected_signature inputs, outputs, patch;
	reflected_type *types;
	UINT group[3], movs, movcs, conversions, bitwise, sample_frequency;
} reflection;

/* What a lookup that finds nothing returns: objects whose GetDesc fails. */
static reflected_type null_type;
static reflected_variable null_variable;
static reflected_buffer null_buffer;

static HRESULT STDMETHODCALLTYPE type_desc(ID3D11ShaderReflectionType *iface, D3D11_SHADER_TYPE_DESC *desc)
{
	const reflected_type *type = (reflected_type *)iface;
	if (type == &null_type || !desc)
		return E_FAIL;
	*desc = type->desc;
	return S_OK;
}

static ID3D11ShaderReflectionType *STDMETHODCALLTYPE type_member(ID3D11ShaderReflectionType *iface, UINT index)
{
	const reflected_type *type = (reflected_type *)iface;
	if (type == &null_type || index >= type->desc.Members)
		return &null_type.iface;
	return &type->members[index]->iface;
}

static ID3D11ShaderReflectionType *STDMETHODCALLTYPE type_member_named(ID3D11ShaderReflectionType *iface, const char *name)
{
	const reflected_type *type = (reflected_type *)iface;
	UINT index;
	if (type == &null_type || !name)
		return &null_type.iface;
	for (index = 0; index < type->desc.Members; ++index)
		if (type->member_names[index] && same_text(type->member_names[index], name))
			return &type->members[index]->iface;
	return &null_type.iface;
}

static const char *STDMETHODCALLTYPE type_member_name(ID3D11ShaderReflectionType *iface, UINT index)
{
	const reflected_type *type = (reflected_type *)iface;
	if (type == &null_type || index >= type->desc.Members)
		return NULL;
	return type->member_names[index];
}

static BOOL types_equal(const reflected_type *a, const reflected_type *b)
{
	UINT index;
	if (a == b)
		return TRUE;
	if (a->desc.Class != b->desc.Class || a->desc.Type != b->desc.Type || a->desc.Rows != b->desc.Rows || a->desc.Columns != b->desc.Columns ||
		a->desc.Elements != b->desc.Elements || a->desc.Members != b->desc.Members)
		return FALSE;
	if ((a->desc.Name == NULL) != (b->desc.Name == NULL) || (a->desc.Name && !same_text(a->desc.Name, b->desc.Name)))
		return FALSE;
	for (index = 0; index < a->desc.Members; ++index)
		if (!types_equal(a->members[index], b->members[index]) || a->members[index]->desc.Offset != b->members[index]->desc.Offset)
			return FALSE;
	return TRUE;
}

static HRESULT STDMETHODCALLTYPE type_equal(ID3D11ShaderReflectionType *iface, ID3D11ShaderReflectionType *other)
{
	if (!other || (reflected_type *)iface == &null_type || (reflected_type *)other == &null_type)
		return E_FAIL;
	return types_equal((reflected_type *)iface, (reflected_type *)other) ? S_OK : S_FALSE;
}

/* HLSL classes and interfaces are not compiled, so no type has a base, a subtype or an interface. */
static ID3D11ShaderReflectionType *STDMETHODCALLTYPE type_none(ID3D11ShaderReflectionType *iface)
{
	return NULL;
}

static UINT STDMETHODCALLTYPE type_interface_count(ID3D11ShaderReflectionType *iface)
{
	return 0;
}

static ID3D11ShaderReflectionType *STDMETHODCALLTYPE type_interface(ID3D11ShaderReflectionType *iface, UINT index)
{
	return NULL;
}

static HRESULT STDMETHODCALLTYPE type_is_of(ID3D11ShaderReflectionType *iface, ID3D11ShaderReflectionType *other)
{
	return type_equal(iface, other);
}

static HRESULT STDMETHODCALLTYPE type_implements(ID3D11ShaderReflectionType *iface, ID3D11ShaderReflectionType *base)
{
	return S_FALSE;
}

static ID3D11ShaderReflectionTypeVtbl type_vtbl = {type_desc, type_member, type_member_named, type_member_name, type_equal, type_none,
	type_none, type_interface_count, type_interface, type_is_of, type_implements};

static HRESULT STDMETHODCALLTYPE variable_desc(ID3D11ShaderReflectionVariable *iface, D3D11_SHADER_VARIABLE_DESC *desc)
{
	const reflected_variable *variable = (reflected_variable *)iface;
	if (variable == &null_variable || !desc)
		return E_FAIL;
	*desc = variable->desc;
	return S_OK;
}

static ID3D11ShaderReflectionType *STDMETHODCALLTYPE variable_type(ID3D11ShaderReflectionVariable *iface)
{
	const reflected_variable *variable = (reflected_variable *)iface;
	return variable == &null_variable ? &null_type.iface : &variable->type->iface;
}

static ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE variable_buffer(ID3D11ShaderReflectionVariable *iface)
{
	const reflected_variable *variable = (reflected_variable *)iface;
	return variable == &null_variable ? &null_buffer.iface : &variable->buffer->iface;
}

static UINT STDMETHODCALLTYPE variable_slot(ID3D11ShaderReflectionVariable *iface, UINT index)
{
	return ~0u;
}

static ID3D11ShaderReflectionVariableVtbl variable_vtbl = {variable_desc, variable_type, variable_buffer, variable_slot};

static HRESULT STDMETHODCALLTYPE buffer_desc(ID3D11ShaderReflectionConstantBuffer *iface, D3D11_SHADER_BUFFER_DESC *desc)
{
	const reflected_buffer *buffer = (reflected_buffer *)iface;
	if (buffer == &null_buffer || !desc)
		return E_FAIL;
	*desc = buffer->desc;
	return S_OK;
}

static ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE buffer_variable(ID3D11ShaderReflectionConstantBuffer *iface, UINT index)
{
	const reflected_buffer *buffer = (reflected_buffer *)iface;
	if (buffer == &null_buffer || index >= buffer->desc.Variables)
		return &null_variable.iface;
	return &buffer->variables[index].iface;
}

static ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE buffer_variable_named(ID3D11ShaderReflectionConstantBuffer *iface, const char *name)
{
	const reflected_buffer *buffer = (reflected_buffer *)iface;
	UINT index;
	if (buffer == &null_buffer || !name)
		return &null_variable.iface;
	for (index = 0; index < buffer->desc.Variables; ++index)
		if (buffer->variables[index].desc.Name && same_text(buffer->variables[index].desc.Name, name))
			return &buffer->variables[index].iface;
	return &null_variable.iface;
}

static ID3D11ShaderReflectionConstantBufferVtbl buffer_vtbl = {buffer_desc, buffer_variable, buffer_variable_named};

static ULONG STDMETHODCALLTYPE reflection_add_ref(ID3D11ShaderReflection *iface);

static HRESULT STDMETHODCALLTYPE reflection_query(ID3D11ShaderReflection *iface, REFIID riid, void **out)
{
	if (IsEqualGUID(riid, &IID_ID3D11ShaderReflection) || IsEqualGUID(riid, &IID_IUnknown)) {
		*out = iface;
		reflection_add_ref(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE reflection_add_ref(ID3D11ShaderReflection *iface)
{
	return (ULONG)InterlockedIncrement(&((reflection *)iface)->refs);
}

static void free_reflection(reflection *object)
{
	UINT index;
	while (object->types) {
		reflected_type *next = object->types->next;
		FREE(object->types->members);
		FREE(object->types->member_names);
		FREE(object->types);
		object->types = next;
	}
	if (object->buffers)
		for (index = 0; index < object->desc.ConstantBuffers; ++index)
			FREE(object->buffers[index].variables);
	FREE(object->buffers);
	FREE(object->bindings);
	FREE(object->inputs.elements);
	FREE(object->outputs.elements);
	FREE(object->patch.elements);
	FREE(object->data);
	FREE(object);
}

static ULONG STDMETHODCALLTYPE reflection_release(ID3D11ShaderReflection *iface)
{
	const LONG refs = InterlockedDecrement(&((reflection *)iface)->refs);
	if (!refs)
		free_reflection((reflection *)iface);
	return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE reflection_desc(ID3D11ShaderReflection *iface, D3D11_SHADER_DESC *desc)
{
	if (!desc)
		return E_FAIL;
	*desc = ((reflection *)iface)->desc;
	return S_OK;
}

static ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE reflection_buffer(ID3D11ShaderReflection *iface, UINT index)
{
	reflection *object = (reflection *)iface;
	return index < object->desc.ConstantBuffers ? &object->buffers[index].iface : &null_buffer.iface;
}

static ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE reflection_buffer_named(ID3D11ShaderReflection *iface, const char *name)
{
	reflection *object = (reflection *)iface;
	UINT index;
	for (index = 0; name && index < object->desc.ConstantBuffers; ++index)
		if (object->buffers[index].desc.Name && same_text(object->buffers[index].desc.Name, name))
			return &object->buffers[index].iface;
	return &null_buffer.iface;
}

static HRESULT STDMETHODCALLTYPE reflection_binding(ID3D11ShaderReflection *iface, UINT index, D3D11_SHADER_INPUT_BIND_DESC *desc)
{
	reflection *object = (reflection *)iface;
	if (!desc || index >= object->desc.BoundResources)
		return E_INVALIDARG;
	*desc = object->bindings[index];
	return S_OK;
}

static HRESULT signature_element(const reflected_signature *signature, UINT index, D3D11_SIGNATURE_PARAMETER_DESC *desc)
{
	if (!desc || index >= signature->count)
		return E_INVALIDARG;
	*desc = signature->elements[index];
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE reflection_input(ID3D11ShaderReflection *iface, UINT index, D3D11_SIGNATURE_PARAMETER_DESC *desc)
{
	return signature_element(&((reflection *)iface)->inputs, index, desc);
}

static HRESULT STDMETHODCALLTYPE reflection_output(ID3D11ShaderReflection *iface, UINT index, D3D11_SIGNATURE_PARAMETER_DESC *desc)
{
	return signature_element(&((reflection *)iface)->outputs, index, desc);
}

static HRESULT STDMETHODCALLTYPE reflection_patch(ID3D11ShaderReflection *iface, UINT index, D3D11_SIGNATURE_PARAMETER_DESC *desc)
{
	return signature_element(&((reflection *)iface)->patch, index, desc);
}

static ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE reflection_variable_named(ID3D11ShaderReflection *iface, const char *name)
{
	reflection *object = (reflection *)iface;
	UINT index;
	for (index = 0; name && index < object->desc.ConstantBuffers; ++index) {
		ID3D11ShaderReflectionVariable *found = buffer_variable_named(&object->buffers[index].iface, name);
		if (found != &null_variable.iface)
			return found;
	}
	return &null_variable.iface;
}

static HRESULT STDMETHODCALLTYPE reflection_binding_named(ID3D11ShaderReflection *iface, const char *name, D3D11_SHADER_INPUT_BIND_DESC *desc)
{
	reflection *object = (reflection *)iface;
	UINT index;
	if (!name || !desc)
		return E_INVALIDARG;
	for (index = 0; index < object->desc.BoundResources; ++index)
		if (object->bindings[index].Name && same_text(object->bindings[index].Name, name)) {
			*desc = object->bindings[index];
			return S_OK;
		}
	return E_INVALIDARG;
}

static UINT STDMETHODCALLTYPE reflection_movs(ID3D11ShaderReflection *iface)
{
	return ((reflection *)iface)->movs;
}

static UINT STDMETHODCALLTYPE reflection_movcs(ID3D11ShaderReflection *iface)
{
	return ((reflection *)iface)->movcs;
}

static UINT STDMETHODCALLTYPE reflection_conversions(ID3D11ShaderReflection *iface)
{
	return ((reflection *)iface)->conversions;
}

static UINT STDMETHODCALLTYPE reflection_bitwise(ID3D11ShaderReflection *iface)
{
	return ((reflection *)iface)->bitwise;
}

static D3D_PRIMITIVE STDMETHODCALLTYPE reflection_gs_input(ID3D11ShaderReflection *iface)
{
	return ((reflection *)iface)->desc.InputPrimitive;
}

static BOOL STDMETHODCALLTYPE reflection_sample_frequency(ID3D11ShaderReflection *iface)
{
	return ((reflection *)iface)->sample_frequency != 0;
}

static UINT STDMETHODCALLTYPE reflection_interface_slots(ID3D11ShaderReflection *iface)
{
	return 0;
}

static HRESULT STDMETHODCALLTYPE reflection_feature_level(ID3D11ShaderReflection *iface, D3D_FEATURE_LEVEL *level)
{
	const UINT version = ((reflection *)iface)->desc.Version, major = version >> 4 & 15, minor = version & 15;
	if (!level)
		return E_INVALIDARG;
	*level = major >= 5 ? D3D_FEATURE_LEVEL_11_0 : major == 4 && minor >= 1 ? D3D_FEATURE_LEVEL_10_1 : D3D_FEATURE_LEVEL_10_0;
	return S_OK;
}

static UINT STDMETHODCALLTYPE reflection_group(ID3D11ShaderReflection *iface, UINT *x, UINT *y, UINT *z)
{
	const reflection *object = (reflection *)iface;
	if (x)
		*x = object->group[0];
	if (y)
		*y = object->group[1];
	if (z)
		*z = object->group[2];
	return object->group[0] * object->group[1] * object->group[2];
}

static UINT64 STDMETHODCALLTYPE reflection_requires(ID3D11ShaderReflection *iface)
{
	return 0;
}

static ID3D11ShaderReflectionVtbl reflection_vtbl = {reflection_query, reflection_add_ref, reflection_release, reflection_desc,
	reflection_buffer, reflection_buffer_named, reflection_binding, reflection_input, reflection_output, reflection_patch,
	reflection_variable_named, reflection_binding_named, reflection_movs, reflection_movcs, reflection_conversions, reflection_bitwise,
	reflection_gs_input, reflection_sample_frequency, reflection_interface_slots, reflection_feature_level, reflection_group,
	reflection_requires};

/* A chunk of the kept container. */
typedef struct {
	const BYTE *data;
	UINT size;
} chunk;

static UINT word_at(const chunk *part, UINT offset)
{
	UINT value = 0;
	if (offset <= part->size && part->size - offset >= 4)
		memcpy(&value, part->data + offset, 4);
	return value;
}

/* A string at an offset in the chunk, or NULL when it does not end inside it. */
static const char *string_at(const chunk *part, UINT offset)
{
	UINT end;
	if (!offset || offset >= part->size)
		return NULL;
	for (end = offset; end < part->size; ++end)
		if (!part->data[end])
			return (const char *)part->data + offset;
	return NULL;
}

static BOOL find_chunk(const reflection *object, const char *tag, chunk *out)
{
	const chunk all = {object->data, (UINT)object->size};
	const UINT count = word_at(&all, 28);
	UINT index;
	for (index = 0; index < count && index < 64; ++index) {
		const UINT offset = word_at(&all, 32 + index * 4);
		if (offset > all.size - 8 || memcmp(all.data + offset, tag, 4))
			continue;
		out->data = all.data + offset + 8;
		out->size = word_at(&all, offset + 4);
		if (out->size > all.size - offset - 8)
			return FALSE;
		return TRUE;
	}
	return FALSE;
}

static reflected_type *read_type(reflection *object, const chunk *part, UINT offset, BOOL sm5, UINT member_offset, UINT depth)
{
	reflected_type *type = ALLOC(sizeof(reflected_type));
	UINT members, index, at;
	if (!type)
		return NULL;
	type->iface.lpVtbl = &type_vtbl;
	type->next = object->types;
	object->types = type;
	type->desc.Class = (D3D_SHADER_VARIABLE_CLASS)(word_at(part, offset) & 0xffff);
	type->desc.Type = (D3D_SHADER_VARIABLE_TYPE)(word_at(part, offset) >> 16);
	type->desc.Rows = word_at(part, offset + 4) & 0xffff;
	type->desc.Columns = word_at(part, offset + 4) >> 16;
	type->desc.Elements = word_at(part, offset + 8) & 0xffff;
	members = word_at(part, offset + 8) >> 16;
	type->desc.Offset = member_offset;
	if (sm5)
		type->desc.Name = string_at(part, word_at(part, offset + 32));
	at = word_at(part, offset + 12);
	if (!members || depth > 32 || !at)
		return type;
	type->members = ALLOC(members * sizeof(reflected_type *));
	type->member_names = ALLOC(members * sizeof(const char *));
	if (!type->members || !type->member_names)
		return NULL;
	for (index = 0; index < members; ++index) {
		const UINT entry = at + index * 12;
		type->member_names[index] = string_at(part, word_at(part, entry));
		if (!(type->members[index] = read_type(object, part, word_at(part, entry + 4), sm5, word_at(part, entry + 8), depth + 1)))
			return NULL;
	}
	type->desc.Members = members;
	return type;
}

static BOOL read_definitions(reflection *object)
{
	chunk part;
	UINT index, k;
	BOOL sm5;
	if (!find_chunk(object, "RDEF", &part))
		return TRUE;
	sm5 = word_at(&part, 28) == 0x31314452;
	object->desc.Creator = string_at(&part, word_at(&part, 24));
	object->desc.Flags = word_at(&part, 20);
	object->desc.ConstantBuffers = word_at(&part, 0);
	object->desc.BoundResources = word_at(&part, 8);
	if (object->desc.ConstantBuffers > 0x10000 || object->desc.BoundResources > 0x10000)
		return FALSE;
	if (object->desc.BoundResources && !(object->bindings = ALLOC(object->desc.BoundResources * sizeof(D3D11_SHADER_INPUT_BIND_DESC))))
		return FALSE;
	for (index = 0; index < object->desc.BoundResources; ++index) {
		const UINT at = word_at(&part, 12) + index * 32;
		D3D11_SHADER_INPUT_BIND_DESC *binding = &object->bindings[index];
		binding->Name = string_at(&part, word_at(&part, at));
		binding->Type = (D3D_SHADER_INPUT_TYPE)word_at(&part, at + 4);
		binding->ReturnType = (D3D_RESOURCE_RETURN_TYPE)word_at(&part, at + 8);
		binding->Dimension = (D3D_SRV_DIMENSION)word_at(&part, at + 12);
		binding->NumSamples = word_at(&part, at + 16);
		binding->BindPoint = word_at(&part, at + 20);
		binding->BindCount = word_at(&part, at + 24);
		binding->uFlags = word_at(&part, at + 28);
	}
	if (object->desc.ConstantBuffers && !(object->buffers = ALLOC(object->desc.ConstantBuffers * sizeof(reflected_buffer))))
		return FALSE;
	for (index = 0; index < object->desc.ConstantBuffers; ++index) {
		const UINT at = word_at(&part, 4) + index * 24, variable_size = sm5 ? 40 : 24;
		reflected_buffer *buffer = &object->buffers[index];
		UINT variables_at;
		buffer->iface.lpVtbl = &buffer_vtbl;
		buffer->desc.Name = string_at(&part, word_at(&part, at));
		buffer->desc.Variables = word_at(&part, at + 4);
		variables_at = word_at(&part, at + 8);
		buffer->desc.Size = word_at(&part, at + 12);
		buffer->desc.uFlags = word_at(&part, at + 16);
		buffer->desc.Type = (D3D_CBUFFER_TYPE)word_at(&part, at + 20);
		if (buffer->desc.Variables > 0x10000)
			return FALSE;
		if (buffer->desc.Variables && !(buffer->variables = ALLOC(buffer->desc.Variables * sizeof(reflected_variable))))
			return FALSE;
		for (k = 0; k < buffer->desc.Variables; ++k) {
			const UINT entry = variables_at + k * variable_size, defaults = word_at(&part, entry + 20);
			reflected_variable *variable = &buffer->variables[k];
			variable->iface.lpVtbl = &variable_vtbl;
			variable->buffer = buffer;
			variable->desc.Name = string_at(&part, word_at(&part, entry));
			variable->desc.StartOffset = word_at(&part, entry + 4);
			variable->desc.Size = word_at(&part, entry + 8);
			variable->desc.uFlags = word_at(&part, entry + 12);
			if (defaults && defaults < part.size && part.size - defaults >= variable->desc.Size)
				variable->desc.DefaultValue = (void *)(part.data + defaults);
			variable->desc.StartTexture = sm5 ? word_at(&part, entry + 24) : ~0u;
			variable->desc.TextureSize = sm5 ? word_at(&part, entry + 28) : 0;
			variable->desc.StartSampler = sm5 ? word_at(&part, entry + 32) : ~0u;
			variable->desc.SamplerSize = sm5 ? word_at(&part, entry + 36) : 0;
			if (!(variable->type = read_type(object, &part, word_at(&part, entry + 16), sm5, 0, 0)))
				return FALSE;
		}
	}
	return TRUE;
}

/* Signatures: ISGN, OSGN and PCSG have 24 byte elements, OSG5 adds the stream in front, the "1" forms add a minimum precision after. */
static BOOL read_signature(reflection *object, const char *const *tags, reflected_signature *out)
{
	chunk part;
	UINT index, size = 0, count;
	BOOL streams = FALSE, precision = FALSE;
	for (; *tags; ++tags)
		if (find_chunk(object, *tags, &part)) {
			streams = (*tags)[3] == '5' || (*tags)[3] == '1';
			precision = (*tags)[3] == '1';
			size = precision ? 32 : streams ? 28 : 24;
			break;
		}
	if (!size)
		return TRUE;
	count = word_at(&part, 0);
	if (count > 0x1000)
		return FALSE;
	if (count && !(out->elements = ALLOC(count * sizeof(D3D11_SIGNATURE_PARAMETER_DESC))))
		return FALSE;
	out->count = count;
	for (index = 0; index < count; ++index) {
		UINT at = word_at(&part, 4) + index * size;
		D3D11_SIGNATURE_PARAMETER_DESC *element = &out->elements[index];
		if (streams) {
			element->Stream = word_at(&part, at);
			at += 4;
		}
		element->SemanticName = string_at(&part, word_at(&part, at));
		element->SemanticIndex = word_at(&part, at + 4);
		element->SystemValueType = (D3D_NAME)word_at(&part, at + 8);
		element->ComponentType = (D3D_REGISTER_COMPONENT_TYPE)word_at(&part, at + 12);
		element->Register = word_at(&part, at + 16);
		element->Mask = (BYTE)word_at(&part, at + 20);
		element->ReadWriteMask = (BYTE)(word_at(&part, at + 20) >> 8);
		if (precision)
			element->MinPrecision = (D3D_MIN_PRECISION)word_at(&part, at + 24);
	}
	return TRUE;
}

/* Shader model 4 and 5 opcodes the description counts (the low 11 bits of an instruction's first token). */
enum {
	OP_CUT = 9, OP_EMIT = 19, OP_FTOI = 27, OP_FTOU = 28, OP_ITOF = 43, OP_LD = 45, OP_LD_MS = 46, OP_CUSTOMDATA = 53, OP_MOV = 54, OP_MOVC = 55,
	OP_SAMPLE = 69, OP_SAMPLE_C = 70, OP_SAMPLE_C_LZ = 71, OP_SAMPLE_L = 72, OP_SAMPLE_D = 73, OP_SAMPLE_B = 74, OP_UTOF = 86,
	OP_DCL_GS_OUTPUT_TOPOLOGY = 92, OP_DCL_GS_INPUT_PRIMITIVE = 93, OP_DCL_MAX_OUTPUT_VERTEX_COUNT = 94, OP_DCL_INPUT_PS = 98,
	OP_DCL_INPUT_PS_SIV = 100, OP_DCL_INDEXABLE_TEMP = 105, OP_GATHER4 = 109, OP_EMIT_STREAM = 117, OP_CUT_STREAM = 118,
	OP_GATHER4_C = 126, OP_GATHER4_PO = 127, OP_GATHER4_PO_C = 128, OP_F32TOF16 = 130, OP_F16TOF32 = 131, OP_DCL_THREAD_GROUP = 155,
	OP_LD_UAV_TYPED = 163, OP_STORE_UAV_TYPED = 164, OP_LD_RAW = 165, OP_LD_STRUCTURED = 167, OP_ATOMIC_FIRST = 169, OP_ATOMIC_LAST = 189,
	OP_SYNC = 190, OP_DCL_GS_INSTANCE_COUNT = 206
};

static BOOL is_bitwise(UINT opcode)
{
	/* and, ishl, ishr, not, or, ushr, xor, and the bit field and bit count instructions. */
	return opcode == 1 || opcode == 41 || opcode == 42 || opcode == 59 || opcode == 60 || opcode == 85 || opcode == 87 ||
		(opcode >= 134 && opcode <= 141);
}

static void read_program(reflection *object)
{
	chunk part;
	UINT at, count;
	if (!find_chunk(object, "SHEX", &part) && !find_chunk(object, "SHDR", &part))
		return;
	object->desc.Version = word_at(&part, 0);
	count = word_at(&part, 4);
	if (count > part.size / 4)
		count = part.size / 4;
	for (at = 2; at < count;) {
		const UINT token = word_at(&part, at * 4), opcode = token & 0x7ff;
		const UINT length = opcode == OP_CUSTOMDATA ? word_at(&part, at * 4 + 4) : token >> 24 & 0x7f;
		D3D11_SHADER_DESC *desc = &object->desc;
		if (!length)
			break;
		switch (opcode) {
		case OP_MOV: ++object->movs; break;
		case OP_MOVC: ++object->movcs; break;
		case OP_FTOI: case OP_FTOU: case OP_ITOF: case OP_UTOF: case OP_F32TOF16: case OP_F16TOF32: ++object->conversions; break;
		case OP_EMIT: case OP_EMIT_STREAM: ++desc->EmitInstructionCount; break;
		case OP_CUT: case OP_CUT_STREAM: ++desc->CutInstructionCount; break;
		case OP_SAMPLE: case OP_SAMPLE_L: case OP_GATHER4: case OP_GATHER4_PO: ++desc->TextureNormalInstructions; break;
		case OP_LD: case OP_LD_MS: case OP_LD_UAV_TYPED: case OP_LD_RAW: case OP_LD_STRUCTURED: ++desc->TextureLoadInstructions; break;
		case OP_SAMPLE_C: case OP_SAMPLE_C_LZ: case OP_GATHER4_C: case OP_GATHER4_PO_C: ++desc->TextureCompInstructions; break;
		case OP_SAMPLE_B: ++desc->TextureBiasInstructions; break;
		case OP_SAMPLE_D: ++desc->TextureGradientInstructions; break;
		case OP_STORE_UAV_TYPED: ++desc->cTextureStoreInstructions; break;
		case OP_SYNC: ++desc->cBarrierInstructions; break;
		case OP_DCL_GS_INPUT_PRIMITIVE: desc->InputPrimitive = (D3D_PRIMITIVE)(token >> 11 & 0x3f); break;
		case OP_DCL_GS_OUTPUT_TOPOLOGY: desc->GSOutputTopology = (D3D_PRIMITIVE_TOPOLOGY)(token >> 11 & 0x7f); break;
		case OP_DCL_MAX_OUTPUT_VERTEX_COUNT: desc->GSMaxOutputVertexCount = word_at(&part, at * 4 + 4); break;
		case OP_DCL_GS_INSTANCE_COUNT: desc->cGSInstanceCount = word_at(&part, at * 4 + 4); break;
		case OP_DCL_INDEXABLE_TEMP: desc->TempArrayCount += word_at(&part, at * 4 + 8); break;
		case OP_DCL_THREAD_GROUP:
			object->group[0] = word_at(&part, at * 4 + 4);
			object->group[1] = word_at(&part, at * 4 + 8);
			object->group[2] = word_at(&part, at * 4 + 12);
			break;
		case OP_DCL_INPUT_PS: case OP_DCL_INPUT_PS_SIV: {
			/* linear sample and linear noperspective sample interpolation run the shader per sample. */
			const UINT mode = token >> 11 & 15;
			if (mode == 6 || mode == 7)
				object->sample_frequency = 1;
			break;
		}
		default:
			if (opcode >= OP_ATOMIC_FIRST && opcode <= OP_ATOMIC_LAST)
				++desc->cInterlockedInstructions;
			else if (is_bitwise(opcode))
				++object->bitwise;
			break;
		}
		at += length;
	}
}

static void read_statistics(reflection *object)
{
	chunk part;
	if (!find_chunk(object, "STAT", &part))
		return;
	object->desc.InstructionCount = word_at(&part, 0);
	object->desc.TempRegisterCount = word_at(&part, 4);
	object->desc.DefCount = word_at(&part, 8);
	object->desc.DclCount = word_at(&part, 12);
}

EXPORT HRESULT WINAPI D3DReflect(const void *data, SIZE_T size, REFIID riid, void **out)
{
	static const char *const inputs[] = {"ISG1", "ISGN", NULL}, *const outputs[] = {"OSG1", "OSG5", "OSGN", NULL}, *const patch[] = {"PSG1", "PCSG", NULL};
	reflection *object;
	UINT index;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!IsEqualGUID(riid, &IID_ID3D11ShaderReflection))
		return E_NOINTERFACE;
	if (!is_container(data, size) || size > 0x7fffffff)
		return E_FAIL;
	if (!(object = ALLOC(sizeof(reflection))) || !(object->data = ALLOC(size))) {
		FREE(object);
		return E_OUTOFMEMORY;
	}
	object->iface.lpVtbl = &reflection_vtbl;
	object->refs = 1;
	object->size = size;
	memcpy(object->data, data, size);
	if (!read_definitions(object) || !read_signature(object, inputs, &object->inputs) || !read_signature(object, outputs, &object->outputs) ||
		!read_signature(object, patch, &object->patch)) {
		free_reflection(object);
		return E_FAIL;
	}
	object->desc.InputParameters = object->inputs.count;
	object->desc.OutputParameters = object->outputs.count;
	object->desc.PatchConstantParameters = object->patch.count;
	for (index = 0; index < 3; ++index)
		object->group[index] = 0;
	read_program(object);
	read_statistics(object);
	*out = &object->iface;
	return S_OK;
}

/* Parts of the library nothing in Raven compiles for: assembling, shader libraries and linking, compression and traces. */

EXPORT HRESULT WINAPI D3DAssemble(const void *data, SIZE_T size, const char *name, const D3D_SHADER_MACRO *defines, ID3DInclude *include,
	UINT flags, ID3DBlob **shader, ID3DBlob **errors)
{
	if (shader)
		*shader = NULL;
	if (errors)
		*errors = NULL;
	return unimplemented("D3DAssemble");
}

EXPORT HRESULT WINAPI D3DCreateLinker(ID3D11Linker **linker)
{
	if (linker)
		*linker = NULL;
	return unimplemented("D3DCreateLinker");
}

EXPORT HRESULT WINAPI D3DLoadModule(const void *data, SIZE_T size, ID3D11Module **module)
{
	if (module)
		*module = NULL;
	return unimplemented("D3DLoadModule");
}

EXPORT HRESULT WINAPI D3DCreateFunctionLinkingGraph(UINT flags, ID3D11FunctionLinkingGraph **graph)
{
	if (graph)
		*graph = NULL;
	return unimplemented("D3DCreateFunctionLinkingGraph");
}

EXPORT HRESULT WINAPI D3DReflectLibrary(const void *data, SIZE_T size, REFIID riid, void **out)
{
	if (out)
		*out = NULL;
	return unimplemented("D3DReflectLibrary");
}

EXPORT HRESULT WINAPI D3DCompressShaders(UINT count, void *shaders, UINT flags, ID3DBlob **out)
{
	if (out)
		*out = NULL;
	return unimplemented("D3DCompressShaders");
}

EXPORT HRESULT WINAPI D3DDecompressShaders(const void *data, SIZE_T size, UINT count, UINT start, UINT *indices, UINT flags,
	ID3DBlob **shaders, UINT *total)
{
	return unimplemented("D3DDecompressShaders");
}

EXPORT HRESULT WINAPI D3DDisassembleRegion(const void *data, SIZE_T size, UINT flags, const char *comments, SIZE_T start, SIZE_T count,
	SIZE_T *finish, ID3DBlob **out)
{
	if (out)
		*out = NULL;
	return unimplemented("D3DDisassembleRegion");
}

EXPORT HRESULT WINAPI D3DDisassemble10Effect(void *effect, UINT flags, ID3DBlob **out)
{
	if (out)
		*out = NULL;
	return unimplemented("D3DDisassemble10Effect");
}

EXPORT HRESULT WINAPI D3DDisassemble11Trace(const void *data, SIZE_T size, void *trace, UINT start, UINT count, UINT flags, ID3DBlob **out)
{
	if (out)
		*out = NULL;
	return unimplemented("D3DDisassemble11Trace");
}

EXPORT HRESULT WINAPI D3DGetTraceInstructionOffsets(const void *data, SIZE_T size, UINT flags, SIZE_T start, SIZE_T count, SIZE_T *offsets,
	SIZE_T *total)
{
	return unimplemented("D3DGetTraceInstructionOffsets");
}

EXPORT HRESULT WINAPI D3DReturnFailure1(void *first, void *second, void *third)
{
	return E_FAIL;
}

EXPORT void WINAPI DebugSetMute(void)
{
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		null_type.iface.lpVtbl = &type_vtbl;
		null_variable.iface.lpVtbl = &variable_vtbl;
		null_buffer.iface.lpVtbl = &buffer_vtbl;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
