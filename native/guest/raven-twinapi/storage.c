/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Files and folders of the Windows Runtime, over the paths of the guest's file system:
 *
 * - Windows.Storage.StorageFolder and StorageFile, each named by its full path: their name, path, attributes, creation time and basic
 *   properties; folders make, find, list, rename and delete what they hold, files are copied, moved, renamed and deleted. A name given
 *   to a folder's Get methods may reach into its subfolders with backslashes; a name made by Create, Rename, Copy or Move is one name.
 *   Listings come in the order of their names, folders before files. A deleted item is gone, as with PermanentDelete: nothing goes to a
 *   recycle bin. Files open as streams of raven-winrt: for reading, for reading and writing, for a
 *   sequence of reads, or in a transaction whose writes go to a copy that replaces the file when committed.
 * - StorageFile.GetFileFromPathAsync and StorageFolder.GetFolderFromPathAsync, for a full path.
 * - Windows.Storage.FileIO: text in UTF-8 or UTF-16, whose byte order mark wins when reading, read and written whole, appended, or as
 *   lines; and bytes, through buffers. What is written carries no byte order mark.
 * - Windows.Storage.Streams.Buffer, with IBufferByteAccess, and InMemoryRandomAccessStream.
 *
 * Each operation does its work on a thread of its own, through raven-winrt, and ends with what Windows ends it with: a missing item is
 * ERROR_FILE_NOT_FOUND, an existing one a name may not take ERROR_ALREADY_EXISTS, a bad name ERROR_INVALID_NAME, and an item of the
 * other kind E_INVALIDARG. */
#include "object.h"
#include "../raven-winrt/winrt.h"

static const IID iid_storage_item = {0x4207a996, 0xca2f, 0x42f7, {0xbd, 0xe8, 0x8b, 0x10, 0x45, 0x7a, 0x7f, 0x30}};
static const IID iid_storage_folder = {0x72d1cb78, 0xb3ef, 0x4f75, {0xa8, 0x0b, 0x6f, 0xd9, 0xda, 0xe2, 0x94, 0x4b}};
static const IID iid_storage_file = {0xfa3f6186, 0x4214, 0x428c, {0xa6, 0x4c, 0x14, 0xc9, 0xac, 0x73, 0x15, 0xea}};
static const IID iid_file_statics = {0x5984c710, 0xdaf2, 0x43c8, {0x8b, 0xb4, 0xa4, 0xd3, 0xea, 0xcf, 0xd0, 0x3f}};
static const IID iid_folder_statics = {0x08f327ff, 0x85d5, 0x48b9, {0xae, 0xe9, 0x28, 0x51, 0x1e, 0x33, 0x9f, 0x9f}};
static const IID iid_file_io = {0x887411eb, 0x7f54, 0x4732, {0xa5, 0xf0, 0x5e, 0x43, 0xe3, 0xb8, 0xc2, 0xf5}};
static const IID iid_basic_properties = {0xd05d55db, 0x785e, 0x4a66, {0xbe, 0x02, 0x9b, 0xee, 0xc5, 0x8a, 0xea, 0x81}};
static const IID iid_buffer_factory = {0x71af914d, 0xc10f, 0x484b, {0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27}};
/* The collections and operations of these types: pinterface of IVectorView, IIterable, IIterator, IVector and IAsyncOperation over them. */
static const IID iid_file_view = {0x80646519, 0x5e2a, 0x595d, {0xa8, 0xcd, 0x2a, 0x24, 0xb4, 0x06, 0x7f, 0x1b}};
static const IID iid_file_iterable = {0x9ac00304, 0x83ea, 0x5688, {0x87, 0xb6, 0xae, 0x38, 0xaa, 0xb6, 0x5d, 0x0b}};
static const IID iid_file_iterator = {0x43e29f53, 0x0298, 0x55aa, {0xa6, 0xc8, 0x4e, 0xdd, 0x32, 0x3d, 0x95, 0x98}};
static const IID iid_folder_view = {0xe20debc6, 0xdc4e, 0x542e, {0xa2, 0xe7, 0xa2, 0x4d, 0x19, 0xc8, 0xdd, 0x62}};
static const IID iid_folder_iterable = {0x4669befc, 0xae5c, 0x52b1, {0x8a, 0x97, 0x54, 0x66, 0xce, 0x61, 0xe9, 0x4e}};
static const IID iid_folder_iterator = {0x5aac96fb, 0xb3b9, 0x5a7f, {0xa9, 0x20, 0x4b, 0x5a, 0x8d, 0xf8, 0x11, 0x68}};
static const IID iid_item_view = {0x85575a41, 0x06cb, 0x58d0, {0xb9, 0x8a, 0x7c, 0x8f, 0x06, 0xe6, 0xe9, 0xd7}};
static const IID iid_item_iterable = {0xbb8b8418, 0x65d1, 0x544b, {0xb0, 0x83, 0x6d, 0x17, 0x2f, 0x56, 0x8c, 0x73}};
static const IID iid_item_iterator = {0x05b487c2, 0x3830, 0x5d3c, {0x98, 0xda, 0x25, 0xfa, 0x11, 0x54, 0x2d, 0xbd}};
static const IID iid_string_vector = {0x98b9acc1, 0x4b56, 0x532e, {0xac, 0x73, 0x03, 0xd5, 0x29, 0x1c, 0xca, 0x90}};
static const IID iid_string_view = {0x2f13c006, 0xa03a, 0x5f69, {0xb0, 0x90, 0x75, 0xa4, 0x3e, 0x33, 0x42, 0x3e}};
static const IID iid_string_iterable = {0xe2fcc7c1, 0x3bfc, 0x5a0b, {0xb2, 0xb0, 0x72, 0xe7, 0x69, 0xd1, 0xcb, 0x7e}};
static const IID iid_string_iterator = {0x8c304ebb, 0x6615, 0x50a4, {0x88, 0x29, 0x87, 0x9e, 0xcd, 0x44, 0x32, 0x36}};
static const IID iid_file_operation = {0x5e52f8ce, 0xaced, 0x5a42, {0x95, 0xb4, 0xf6, 0x74, 0xdd, 0x84, 0x88, 0x5e}};
const IID iid_folder_operation = {0x6be9e7d7, 0xe83a, 0x5cbc, {0x80, 0x2c, 0x17, 0x68, 0x96, 0x0b, 0x52, 0xc3}};
static const IID iid_item_operation = {0x5fc9c137, 0xebb7, 0x5e6c, {0x9c, 0xba, 0x68, 0x6f, 0x2e, 0xc2, 0xb0, 0xbb}};
static const IID iid_files_operation = {0x03362e33, 0xe413, 0x5f29, {0x97, 0xd0, 0x48, 0xa4, 0x78, 0x09, 0x35, 0xf9}};
static const IID iid_folders_operation = {0xca40b21b, 0xaeb1, 0x5a61, {0x9e, 0x08, 0x3b, 0xd5, 0xd9, 0x59, 0x40, 0x23}};
static const IID iid_items_operation = {0x4b1c0fd7, 0x7a01, 0x5e7a, {0xa6, 0xfe, 0xbe, 0x45, 0x00, 0x28, 0x3f, 0x23}};
static const IID iid_string_operation = {0x3e1fe603, 0xf897, 0x5263, {0xb3, 0x28, 0x08, 0x06, 0x42, 0x6b, 0x8a, 0x79}};
static const IID iid_properties_operation = {0x5186131a, 0x4467, 0x504b, {0x97, 0x7a, 0x07, 0x85, 0xa8, 0x23, 0x04, 0x85}};
static const IID iid_buffer_operation = {0x3bee8834, 0xb9a7, 0x5a80, {0xa7, 0x46, 0x5e, 0xf0, 0x97, 0x22, 0x78, 0x78}};
static const IID iid_stream_reference = {0x33ee3134, 0x1dd6, 0x4e3a, {0x80, 0x67, 0xd1, 0xc1, 0x62, 0xe8, 0x64, 0x2b}};
static const IID iid_input_reference = {0x43929d18, 0x5ec9, 0x4b5a, {0x91, 0x9c, 0x42, 0x05, 0xb0, 0xc8, 0x04, 0xb6}};
static const IID iid_stream_transaction = {0xf67cf363, 0xa53d, 0x4d94, {0xae, 0x2c, 0x67, 0x23, 0x2d, 0x93, 0xac, 0xdd}};
static const IID iid_closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
/* IAsyncOperation of IRandomAccessStream, IRandomAccessStreamWithContentType, IInputStream and StorageStreamTransaction. */
static const IID iid_stream_operation = {0x430ecece, 0x1418, 0x5d19, {0x81, 0xb2, 0x5d, 0xdb, 0x38, 0x16, 0x03, 0xcc}};
static const IID iid_typed_stream_operation = {0xc4a57c5e, 0x32b0, 0x55b3, {0xad, 0x13, 0xce, 0x1c, 0x23, 0x04, 0x1e, 0xd6}};
static const IID iid_input_operation = {0xa8fe0732, 0x556d, 0x5841, {0xb7, 0xee, 0xb3, 0x45, 0x0f, 0xb5, 0x26, 0x66}};
static const IID iid_transaction_operation = {0x0d81405a, 0x9bd3, 0x5e87, {0x82, 0xf4, 0x9b, 0x41, 0x28, 0xa8, 0x87, 0xeb}};
static const IID iid_lines_operation = {0x92b02cd3, 0xaa6e, 0x573d, {0xbc, 0x03, 0x8d, 0x23, 0x09, 0xcb, 0xa3, 0xeb}};

static const struct winrt_items_kind file_list = {&iid_file_view, &iid_file_iterable, &iid_file_iterator, FALSE, NULL};
static const struct winrt_items_kind folder_list = {&iid_folder_view, &iid_folder_iterable, &iid_folder_iterator, FALSE, NULL};
static const struct winrt_items_kind item_list = {&iid_item_view, &iid_item_iterable, &iid_item_iterator, FALSE, NULL};
static const struct winrt_items_kind line_list = {&iid_string_view, &iid_string_iterable, &iid_string_iterator, TRUE, &iid_string_vector};

/* FileAttributes, StorageItemTypes, CreationCollisionOption, NameCollisionOption and UnicodeEncoding. */
#define READ_ONLY_ATTRIBUTE 1
#define DIRECTORY_ATTRIBUTE 16
#define ARCHIVE_ATTRIBUTE 32
#define FILE_TYPE 1
#define FOLDER_TYPE 2
#define CREATE_UNIQUE 0
#define CREATE_REPLACE 1
#define CREATE_FAIL 2
#define CREATE_OPEN 3
#define NAME_UNIQUE 0
#define NAME_REPLACE 1
#define NAME_FAIL 2
#define ENCODING_UTF8 0
#define ENCODING_UTF16LE 1
#define ENCODING_UTF16BE 2

#define NOT_THERE HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
#define ALREADY_THERE HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)
#define BAD_NAME HRESULT_FROM_WIN32(ERROR_INVALID_NAME)
#define TOO_LONG HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE)

/* Objects made here own nothing beyond themselves. */
static void destroy_nothing(struct object *base) {}

/* A file or folder, named by its full path, which renaming and moving change under the lock. */
struct item {
	struct object base;
	SRWLOCK lock;
	WCHAR path[MAX_PATH];
	BOOL folder;
};

static struct item *item_of(void *iface) { return (struct item *)owner(iface); }

static void path_of(struct item *self, WCHAR *path)
{
	AcquireSRWLockShared(&self->lock);
	lstrcpyW(path, self->path);
	ReleaseSRWLockShared(&self->lock);
}

static void set_path(struct item *self, const WCHAR *path)
{
	AcquireSRWLockExclusive(&self->lock);
	lstrcpyW(self->path, path);
	ReleaseSRWLockExclusive(&self->lock);
}

/* The last part of a path, without a closing separator, and its length. */
static const WCHAR *leaf(const WCHAR *path, UINT32 *length)
{
	const WCHAR *name = path;
	for (const WCHAR *at = path; *at; ++at)
		if ((*at == L'\\' || *at == L'/') && at[1])
			name = at + 1;
	*length = (UINT32)lstrlenW(name);
	if (*length && (name[*length - 1] == L'\\' || name[*length - 1] == L'/'))
		--*length;
	return name;
}

/* The extension of a path's last part, with its dot, or NULL. */
static const WCHAR *extension(const WCHAR *path, UINT32 *length)
{
	UINT32 name_length;
	const WCHAR *name = leaf(path, &name_length), *dot = NULL;
	for (UINT32 at = 0; at < name_length; ++at)
		if (name[at] == L'.')
			dot = name + at;
	*length = dot ? (UINT32)(name + name_length - dot) : 0;
	return dot;
}

/* Whether a name can be one part of a path: not empty, not . or .., none of the characters Windows refuses, no trailing dot or space. */
static BOOL good_part(const WCHAR *name, UINT32 length)
{
	static const WCHAR refused[] = L"<>:\"/\\|?*";
	if (!length || (length == 1 && name[0] == L'.') || (length == 2 && name[0] == L'.' && name[1] == L'.'))
		return FALSE;
	if (name[length - 1] == L'.' || name[length - 1] == L' ')
		return FALSE;
	for (UINT32 at = 0; at < length; ++at) {
		if (name[at] < 32)
			return FALSE;
		for (const WCHAR *bad = refused; *bad; ++bad)
			if (name[at] == *bad)
				return FALSE;
	}
	return TRUE;
}

/* The path of a name under a folder. With deep, the name may hold parts separated by backslashes. */
static HRESULT join(const WCHAR *folder, const WCHAR *text, UINT32 length, BOOL deep, WCHAR *path)
{
	UINT32 start = 0;
	for (UINT32 at = 0; at <= length; ++at) {
		if (at < length && text[at] != L'\\')
			continue;
		if ((at < length && !deep) || !good_part(text + start, at - start))
			return BAD_NAME;
		start = at + 1;
	}
	const size_t base = (size_t)lstrlenW(folder);
	if (base + 1 + length >= MAX_PATH)
		return TOO_LONG;
	lstrcpyW(path, folder);
	if (base && path[base - 1] != L'\\')
		lstrcatW(path, L"\\");
	const size_t end = (size_t)lstrlenW(path);
	memcpy(path + end, text, (size_t)length * sizeof(WCHAR));
	path[end + length] = 0;
	return S_OK;
}

static HRESULT join_string(const WCHAR *folder, HSTRING name, BOOL deep, WCHAR *path)
{
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	return join(folder, text, length, deep, path);
}

/* Whether something is at a path, and whether it is a folder. */
static BOOL exists(const WCHAR *path, BOOL *folder)
{
	const DWORD attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return FALSE;
	if (folder)
		*folder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	return TRUE;
}

/* A free name beside a path for GenerateUniqueName: "name (2).ext", then 3 and on. */
static HRESULT unique(WCHAR *path)
{
	WCHAR stem[MAX_PATH], suffix_part[MAX_PATH] = L"";
	UINT32 length, name_length;
	const WCHAR *name = leaf(path, &name_length), *dot = extension(path, &length);
	lstrcpyW(stem, path);
	if (dot && dot != name) {
		lstrcpyW(suffix_part, dot);
		stem[dot - path] = 0;
	}
	for (UINT32 number = 2; number < 10000; ++number) {
		WCHAR suffix[16];
		wsprintfW(suffix, L" (%u)", number);
		if ((size_t)lstrlenW(stem) + lstrlenW(suffix) + lstrlenW(suffix_part) >= MAX_PATH)
			return TOO_LONG;
		lstrcpyW(path, stem);
		lstrcatW(path, suffix);
		lstrcatW(path, suffix_part);
		if (!exists(path, NULL))
			return S_OK;
	}
	return ALREADY_THERE;
}

static HRESULT last_error(void)
{
	const DWORD error = GetLastError();
	return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

static HRESULT new_item(const WCHAR *path, BOOL folder, struct item **out);

/* A new item for a path, through its IStorageFile or IStorageFolder, or through IStorageItem with as_item: the reference an operation
 * gives as its result. */
static HRESULT item_result(const WCHAR *path, BOOL folder, BOOL as_item, union winrt_value *result)
{
	struct item *item;
	const HRESULT made = new_item(path, folder, &item);
	if (SUCCEEDED(made))
		result->object = face_of(&item->base, as_item ? 0 : 1);
	return made;
}

/* The full path of any IStorageItem, ours or not. */
static HRESULT path_of_any(void *iface, WCHAR *path)
{
	IUnknown *item = NULL;
	HSTRING text = NULL;
	if (!iface)
		return E_INVALIDARG;
	HRESULT result = ((IUnknown *)iface)->lpVtbl->QueryInterface(iface, &iid_storage_item, (void **)&item);
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, HSTRING *))method(item, 12))(item, &text);
	release(item);
	if (FAILED(result))
		return result;
	UINT32 length = 0;
	const WCHAR *raw = WindowsGetStringRawBuffer(text, &length);
	if (length >= MAX_PATH)
		result = TOO_LONG;
	else {
		memcpy(path, raw, (size_t)length * sizeof(WCHAR));
		path[length] = 0;
	}
	WindowsDeleteString(text);
	return result;
}

/* The work of one operation, run on its thread: what the method was given, which the job holds references to and copies of. */
struct job {
	HRESULT (*run)(struct job *job, union winrt_value *result);
	struct item *item;
	HSTRING name, text;
	INT32 option;
	IUnknown *other, *extra;
	BYTE *bytes;
	UINT32 count;
};

static void free_job(void *context)
{
	struct job *job = context;
	if (job->item)
		release(face_of(&job->item->base, 0));
	WindowsDeleteString(job->name);
	WindowsDeleteString(job->text);
	release(job->other);
	release(job->extra);
	FREE(job->bytes);
	FREE(job);
}

static HRESULT work(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct job *job = context;
	return job->run(job, result);
}

/* A job for an item's method (iface may be NULL), with copies of its string arguments and references to its objects. */
static struct job *new_job(HRESULT (*run)(struct job *, union winrt_value *), void *iface, HSTRING name, INT32 option, void *other)
{
	struct job *job = ALLOC(sizeof *job);
	if (!job)
		return NULL;
	job->run = run;
	job->option = option;
	if (FAILED(WindowsDuplicateString(name, &job->name))) {
		FREE(job);
		return NULL;
	}
	if (iface) {
		job->item = item_of(iface);
		reference(face_of(&job->item->base, 0));
	}
	reference(job->other = other);
	return job;
}

/* Starts a job: an operation of the identifier that gives a result of the kind, or an action when operation is NULL. */
static HRESULT start(const IID *operation, enum winrt_result kind, struct job *job, void **out)
{
	if (!out) {
		if (job)
			free_job(job);
		return E_POINTER;
	}
	*out = NULL;
	if (!job)
		return E_OUTOFMEMORY;
	return operation ? winrt_async_start(operation, FALSE, kind, work, job, free_job, out) : winrt_action_start(work, job, free_job, out);
}

/* IStorageItem. */
static HRESULT WINAPI item_name(void *iface, HSTRING *out)
{
	WCHAR path[MAX_PATH];
	UINT32 length;
	if (!out)
		return E_POINTER;
	path_of(item_of(iface), path);
	const WCHAR *name = leaf(path, &length);
	return WindowsCreateString(name, length, out);
}

static HRESULT WINAPI item_path(void *iface, HSTRING *out)
{
	WCHAR path[MAX_PATH];
	if (!out)
		return E_POINTER;
	path_of(item_of(iface), path);
	return WindowsCreateString(path, (UINT32)lstrlenW(path), out);
}

static HRESULT WINAPI item_attributes(void *iface, UINT32 *out)
{
	WCHAR path[MAX_PATH];
	if (!out)
		return E_POINTER;
	path_of(item_of(iface), path);
	const DWORD attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return last_error();
	*out = (attributes & FILE_ATTRIBUTE_DIRECTORY ? DIRECTORY_ATTRIBUTE : 0) | (attributes & FILE_ATTRIBUTE_READONLY ? READ_ONLY_ATTRIBUTE : 0) |
		(attributes & FILE_ATTRIBUTE_ARCHIVE ? ARCHIVE_ATTRIBUTE : 0);
	return S_OK;
}

static INT64 date_of(FILETIME time) { return (INT64)(((UINT64)time.dwHighDateTime << 32) | time.dwLowDateTime); }

/* A DateTime counts hundreds of nanoseconds from 1601, as a FILETIME does. */
static HRESULT WINAPI item_created(void *iface, INT64 *out)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	WCHAR path[MAX_PATH];
	if (!out)
		return E_POINTER;
	path_of(item_of(iface), path);
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
		return last_error();
	*out = date_of(data.ftCreationTime);
	return S_OK;
}

static HRESULT WINAPI item_is_of_type(void *iface, INT32 type, boolean *out)
{
	return answer_flag(out, type == (item_of(iface)->folder ? FOLDER_TYPE : FILE_TYPE));
}

/* Renames the item in its folder; the item then names its new path. */
static HRESULT run_rename(struct job *job, union winrt_value *result)
{
	WCHAR path[MAX_PATH], folder[MAX_PATH], target[MAX_PATH];
	UINT32 length;
	path_of(job->item, path);
	lstrcpyW(folder, path);
	folder[leaf(path, &length) - path] = 0;
	HRESULT outcome = job->option < NAME_UNIQUE || job->option > NAME_FAIL ? E_INVALIDARG : join_string(folder, job->name, FALSE, target);
	if (SUCCEEDED(outcome) && CompareStringOrdinal(target, -1, path, -1, TRUE) != CSTR_EQUAL && exists(target, NULL)) {
		if (job->option == NAME_FAIL)
			outcome = ALREADY_THERE;
		else if (job->option == NAME_UNIQUE)
			outcome = unique(target);
	}
	if (SUCCEEDED(outcome) && !MoveFileExW(path, target, job->option == NAME_REPLACE ? MOVEFILE_REPLACE_EXISTING : 0))
		outcome = last_error();
	if (SUCCEEDED(outcome))
		set_path(job->item, target);
	return outcome;
}

static HRESULT WINAPI item_rename_as(void *iface, HSTRING name, INT32 option, void **out)
{
	return start(NULL, WINRT_RESULT_NONE, new_job(run_rename, iface, name, option, NULL), out);
}

static HRESULT WINAPI item_rename(void *iface, HSTRING name, void **out) { return item_rename_as(iface, name, NAME_FAIL, out); }

/* Removes a folder and all it holds. */
static BOOL remove_tree(const WCHAR *path)
{
	WCHAR pattern[MAX_PATH + 2], child[MAX_PATH];
	WIN32_FIND_DATAW found;
	if ((size_t)lstrlenW(path) + 2 >= ARRAYSIZE(pattern))
		return FALSE;
	lstrcpyW(pattern, path);
	lstrcatW(pattern, L"\\*");
	HANDLE search = FindFirstFileW(pattern, &found);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			if (!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L".."))
				continue;
			if ((size_t)lstrlenW(path) + 1 + lstrlenW(found.cFileName) >= MAX_PATH) {
				FindClose(search);
				SetLastError(ERROR_FILENAME_EXCED_RANGE);
				return FALSE;
			}
			lstrcpyW(child, path);
			lstrcatW(child, L"\\");
			lstrcatW(child, found.cFileName);
			if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? !remove_tree(child) : !DeleteFileW(child)) {
				FindClose(search);
				return FALSE;
			}
		} while (FindNextFileW(search, &found));
		FindClose(search);
	}
	return RemoveDirectoryW(path);
}

static HRESULT run_delete(struct job *job, union winrt_value *result)
{
	WCHAR path[MAX_PATH];
	BOOL folder = FALSE;
	path_of(job->item, path);
	if (!exists(path, &folder))
		return NOT_THERE;
	return (folder ? remove_tree(path) : DeleteFileW(path)) ? S_OK : last_error();
}

static HRESULT WINAPI item_delete_as(void *iface, INT32 option, void **out)
{
	return start(NULL, WINRT_RESULT_NONE, new_job(run_delete, iface, NULL, option, NULL), out);
}

static HRESULT WINAPI item_delete(void *iface, void **out) { return item_delete_as(iface, 0, out); }

/* Windows.Storage.FileProperties.BasicProperties: what the item's file system said when they were asked for. */
struct properties {
	struct object base;
	UINT64 size;
	INT64 modified;
};

static HRESULT WINAPI properties_size(void *iface, UINT64 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct properties *)owner(iface))->size;
	return S_OK;
}

static HRESULT WINAPI properties_modified(void *iface, INT64 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct properties *)owner(iface))->modified;
	return S_OK;
}

TABLE(properties_table, 0, INSPECTABLE, (const void *)properties_size, (const void *)properties_modified, (const void *)properties_modified);
static const struct face_iid properties_iids[] = {{&iid_basic_properties, 0}, {NULL, 0}};
static const struct kind properties_kind = {"BasicProperties", L"Windows.Storage.FileProperties.BasicProperties", properties_iids,
	destroy_nothing};

static HRESULT run_properties(struct job *job, union winrt_value *result)
{
	static const void *const *const tables[] = {properties_table.slots};
	WIN32_FILE_ATTRIBUTE_DATA data;
	WCHAR path[MAX_PATH];
	path_of(job->item, path);
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
		return last_error();
	struct properties *self = make(sizeof *self, &properties_kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	self->size = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 0 : ((UINT64)data.nFileSizeHigh << 32) | data.nFileSizeLow;
	self->modified = date_of(data.ftLastWriteTime);
	result->object = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI item_properties(void *iface, void **out)
{
	return start(&iid_properties_operation, WINRT_RESULT_OBJECT, new_job(run_properties, iface, NULL, 0, NULL), out);
}

/* IStorageFolder. The job's count says whether a folder (1) or a file (0) is made or wanted. */
static HRESULT run_create(struct job *job, union winrt_value *result)
{
	const BOOL folder = job->count != 0;
	WCHAR base[MAX_PATH], path[MAX_PATH];
	path_of(job->item, base);
	HRESULT outcome = job->option < CREATE_UNIQUE || job->option > CREATE_OPEN ? E_INVALIDARG : join_string(base, job->name, FALSE, path);
	BOOL there_folder = FALSE, opened = FALSE;
	if (SUCCEEDED(outcome) && exists(path, &there_folder)) {
		if (job->option == CREATE_FAIL)
			outcome = ALREADY_THERE;
		else if (job->option == CREATE_UNIQUE)
			outcome = unique(path);
		else if (job->option == CREATE_OPEN)
			outcome = there_folder == folder ? (opened = TRUE, S_OK) : ALREADY_THERE;
		else if (there_folder != folder)
			outcome = ALREADY_THERE;
		else if (folder && !remove_tree(path))
			outcome = last_error();
	}
	if (SUCCEEDED(outcome) && !opened) {
		if (folder) {
			if (!CreateDirectoryW(path, NULL))
				outcome = last_error();
		} else {
			HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
				job->option == CREATE_REPLACE ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
			if (file == INVALID_HANDLE_VALUE)
				outcome = last_error();
			else
				CloseHandle(file);
		}
	}
	return SUCCEEDED(outcome) ? item_result(path, folder, FALSE, result) : outcome;
}

static HRESULT create(void *iface, HSTRING name, INT32 option, BOOL folder, void **out)
{
	struct job *job = new_job(run_create, iface, name, option, NULL);
	if (job)
		job->count = folder;
	return start(folder ? &iid_folder_operation : &iid_file_operation, WINRT_RESULT_OBJECT, job, out);
}

static HRESULT WINAPI folder_create_file_as(void *iface, HSTRING name, INT32 option, void **out) { return create(iface, name, option, FALSE, out); }
static HRESULT WINAPI folder_create_file(void *iface, HSTRING name, void **out) { return create(iface, name, CREATE_FAIL, FALSE, out); }
static HRESULT WINAPI folder_create_folder_as(void *iface, HSTRING name, INT32 option, void **out) { return create(iface, name, option, TRUE, out); }
static HRESULT WINAPI folder_create_folder(void *iface, HSTRING name, void **out) { return create(iface, name, CREATE_FAIL, TRUE, out); }

/* What a name under the folder is, when it is of a wanted type (the option: FILE_TYPE, FOLDER_TYPE or both). */
static HRESULT run_get(struct job *job, union winrt_value *result)
{
	WCHAR base[MAX_PATH], path[MAX_PATH];
	BOOL folder = FALSE;
	path_of(job->item, base);
	HRESULT outcome = join_string(base, job->name, TRUE, path);
	if (SUCCEEDED(outcome) && !exists(path, &folder))
		outcome = NOT_THERE;
	if (SUCCEEDED(outcome) && !(job->option & (folder ? FOLDER_TYPE : FILE_TYPE)))
		outcome = E_INVALIDARG;
	return SUCCEEDED(outcome) ? item_result(path, folder, job->option == (FILE_TYPE | FOLDER_TYPE), result) : outcome;
}

static HRESULT WINAPI folder_get_file(void *iface, HSTRING name, void **out)
{
	return start(&iid_file_operation, WINRT_RESULT_OBJECT, new_job(run_get, iface, name, FILE_TYPE, NULL), out);
}

static HRESULT WINAPI folder_get_folder(void *iface, HSTRING name, void **out)
{
	return start(&iid_folder_operation, WINRT_RESULT_OBJECT, new_job(run_get, iface, name, FOLDER_TYPE, NULL), out);
}

static HRESULT WINAPI folder_get_item(void *iface, HSTRING name, void **out)
{
	return start(&iid_item_operation, WINRT_RESULT_OBJECT, new_job(run_get, iface, name, FILE_TYPE | FOLDER_TYPE, NULL), out);
}

/* The names in a folder, sorted, folders first when both are wanted. */
struct listing {
	WCHAR (*names)[MAX_PATH];
	BOOL *folders;
	UINT32 count, capacity;
};

static int order(const struct listing *list, UINT32 one, UINT32 other)
{
	if (list->folders[one] != list->folders[other])
		return list->folders[one] ? -1 : 1;
	return CompareStringOrdinal(list->names[one], -1, list->names[other], -1, TRUE) - CSTR_EQUAL;
}

static HRESULT add_name(struct listing *list, const WCHAR *path, const WCHAR *name, BOOL folder)
{
	if (list->count == list->capacity) {
		const UINT32 capacity = list->capacity ? list->capacity * 2 : 32;
		void *names = list->names ? HeapReAlloc(GetProcessHeap(), 0, list->names, capacity * sizeof *list->names)
			: ALLOC(capacity * sizeof *list->names);
		if (!names)
			return E_OUTOFMEMORY;
		list->names = names;
		void *folders = list->folders ? HeapReAlloc(GetProcessHeap(), 0, list->folders, capacity * sizeof *list->folders)
			: ALLOC(capacity * sizeof *list->folders);
		if (!folders)
			return E_OUTOFMEMORY;
		list->folders = folders;
		list->capacity = capacity;
	}
	lstrcpyW(list->names[list->count], path);
	lstrcatW(list->names[list->count], L"\\");
	lstrcatW(list->names[list->count], name);
	list->folders[list->count++] = folder;
	return S_OK;
}

static HRESULT list(const WCHAR *path, INT32 wanted, struct listing *out)
{
	WCHAR pattern[MAX_PATH + 2];
	WIN32_FIND_DATAW found;
	*out = (struct listing){0};
	if ((size_t)lstrlenW(path) + 2 >= ARRAYSIZE(pattern))
		return TOO_LONG;
	lstrcpyW(pattern, path);
	lstrcatW(pattern, L"\\*");
	HANDLE search = FindFirstFileW(pattern, &found);
	if (search == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND ? S_OK : last_error();
	HRESULT result = S_OK;
	do {
		const BOOL folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L"..") || !(wanted & (folder ? FOLDER_TYPE : FILE_TYPE)))
			continue;
		if ((size_t)lstrlenW(path) + 1 + lstrlenW(found.cFileName) < MAX_PATH)
			result = add_name(out, path, found.cFileName, folder);
	} while (SUCCEEDED(result) && FindNextFileW(search, &found));
	FindClose(search);
	/* Insertion sort: folders hold few items. */
	for (UINT32 at = 1; SUCCEEDED(result) && at < out->count; ++at)
		for (UINT32 back = at; back && order(out, back - 1, back) > 0; --back) {
			WCHAR name[MAX_PATH];
			const BOOL folder = out->folders[back];
			lstrcpyW(name, out->names[back]);
			lstrcpyW(out->names[back], out->names[back - 1]);
			lstrcpyW(out->names[back - 1], name);
			out->folders[back] = out->folders[back - 1];
			out->folders[back - 1] = folder;
		}
	return result;
}

static HRESULT run_list(struct job *job, union winrt_value *result)
{
	const BOOL both = job->option == (FILE_TYPE | FOLDER_TYPE);
	WCHAR path[MAX_PATH];
	struct listing found;
	path_of(job->item, path);
	HRESULT outcome = list(path, job->option, &found);
	IInspectable **faces = SUCCEEDED(outcome) && found.count ? ALLOC(found.count * sizeof *faces) : NULL;
	if (SUCCEEDED(outcome) && found.count && !faces)
		outcome = E_OUTOFMEMORY;
	UINT32 made = 0;
	for (; SUCCEEDED(outcome) && made < found.count; ++made) {
		struct item *item;
		if (SUCCEEDED(outcome = new_item(found.names[made], found.folders[made], &item)))
			faces[made] = face_of(&item->base, both ? 0 : 1);
	}
	if (SUCCEEDED(outcome))
		outcome = winrt_items(both ? &item_list : job->option == FILE_TYPE ? &file_list : &folder_list, faces, found.count, FALSE,
			&result->object);
	for (UINT32 index = 0; index < made; ++index)
		release(faces[index]);
	FREE(faces);
	FREE(found.names);
	FREE(found.folders);
	return outcome;
}

static HRESULT WINAPI folder_files(void *iface, void **out)
{
	return start(&iid_files_operation, WINRT_RESULT_OBJECT, new_job(run_list, iface, NULL, FILE_TYPE, NULL), out);
}

static HRESULT WINAPI folder_folders(void *iface, void **out)
{
	return start(&iid_folders_operation, WINRT_RESULT_OBJECT, new_job(run_list, iface, NULL, FOLDER_TYPE, NULL), out);
}

static HRESULT WINAPI folder_items(void *iface, void **out)
{
	return start(&iid_items_operation, WINRT_RESULT_OBJECT, new_job(run_list, iface, NULL, FILE_TYPE | FOLDER_TYPE, NULL), out);
}

/* IStorageFile. */
static HRESULT WINAPI file_type(void *iface, HSTRING *out)
{
	WCHAR path[MAX_PATH];
	UINT32 length;
	if (!out)
		return E_POINTER;
	path_of(item_of(iface), path);
	const WCHAR *dot = extension(path, &length);
	return WindowsCreateString(dot, length, out);
}

/* The MIME type the registry gives a path's extension, as Windows reads it, into room for 128 characters. */
static void content_type_of(const WCHAR *path, WCHAR *type)
{
	UINT32 length;
	WCHAR key[MAX_PATH];
	const WCHAR *dot = extension(path, &length);
	DWORD size = 128 * sizeof(WCHAR);
	if (dot) {
		memcpy(key, dot, (size_t)length * sizeof(WCHAR));
		key[length] = 0;
		if (!RegGetValueW(HKEY_CLASSES_ROOT, key, L"Content Type", RRF_RT_REG_SZ, NULL, type, &size))
			return;
	}
	lstrcpyW(type, L"application/octet-stream");
}

static HRESULT WINAPI file_content_type(void *iface, HSTRING *out)
{
	WCHAR path[MAX_PATH], type[128];
	if (!out)
		return E_POINTER;
	path_of(item_of(iface), path);
	content_type_of(path, type);
	return WindowsCreateString(type, (UINT32)lstrlenW(type), out);
}

/* FileAccessMode, and how the job's option asks a file to be opened. */
#define ACCESS_READ 0
#define ACCESS_READ_WRITE 1
#define OPEN_TYPED 2
#define OPEN_SEQUENTIAL 3

/* A stream over a path: others may read and write beside a reader, and only read beside a writer. */
static HRESULT open_stream(const WCHAR *path, BOOL write, const WCHAR *content_type, void **out)
{
	HANDLE file = CreateFileW(path, GENERIC_READ | (write ? GENERIC_WRITE : 0),
		FILE_SHARE_READ | FILE_SHARE_DELETE | (write ? 0 : FILE_SHARE_WRITE), NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return last_error();
	return winrt_file_stream(file, TRUE, write, content_type, out);
}

HRESULT open_typed_stream(const WCHAR *path, void **out)
{
	WCHAR type[128];
	content_type_of(path, type);
	return open_stream(path, FALSE, type, out);
}

static HRESULT run_open(struct job *job, union winrt_value *result)
{
	WCHAR path[MAX_PATH], type[128];
	path_of(job->item, path);
	if (job->option == OPEN_TYPED)
		content_type_of(path, type);
	void *stream = NULL;
	HRESULT outcome = open_stream(path, job->option == ACCESS_READ_WRITE, job->option == OPEN_TYPED ? type : NULL, &stream);
	/* A sequence of reads is the stream's IInputStream. */
	if (SUCCEEDED(outcome) && job->option == OPEN_SEQUENTIAL) {
		void *input = NULL;
		outcome = ((IUnknown *)stream)->lpVtbl->QueryInterface(stream, &IID___x_ABI_CWindows_CStorage_CStreams_CIInputStream, &input);
		release(stream);
		stream = input;
	}
	if (SUCCEEDED(outcome))
		result->object = stream;
	return outcome;
}

static HRESULT WINAPI file_open(void *iface, INT32 mode, void **out)
{
	if (mode != ACCESS_READ && mode != ACCESS_READ_WRITE) {
		answer_null(out);
		return E_INVALIDARG;
	}
	return start(&iid_stream_operation, WINRT_RESULT_OBJECT, new_job(run_open, iface, NULL, mode, NULL), out);
}

/* IRandomAccessStreamReference.OpenReadAsync and IInputStreamReference.OpenSequentialReadAsync of a file. */
static HRESULT WINAPI file_open_read(void *iface, void **out)
{
	return start(&iid_typed_stream_operation, WINRT_RESULT_OBJECT, new_job(run_open, iface, NULL, OPEN_TYPED, NULL), out);
}

static HRESULT WINAPI file_open_sequential(void *iface, void **out)
{
	return start(&iid_input_operation, WINRT_RESULT_OBJECT, new_job(run_open, iface, NULL, OPEN_SEQUENTIAL, NULL), out);
}

/* Windows.Storage.StorageStreamTransaction: a stream over a copy of the file beside it, which CommitAsync puts in the file's place.
 * A transaction closed or let go without a commit leaves the file as it was and removes the copy. */
struct transaction {
	struct object base;
	void *stream;
	WCHAR target[MAX_PATH], copy[MAX_PATH];
	LONG committed;
};

static struct transaction *transaction_of(void *iface) { return (struct transaction *)owner(iface); }

/* Closes a stream through its IClosable, so that it lets the copy go. */
static void close_stream(void *stream)
{
	IUnknown *closable = NULL;
	if (stream && SUCCEEDED(((IUnknown *)stream)->lpVtbl->QueryInterface(stream, &iid_closable, (void **)&closable))) {
		((HRESULT (WINAPI *)(void *))method(closable, 6))(closable);
		release(closable);
	}
}

static void forget_copy(struct transaction *self)
{
	if (!InterlockedCompareExchange(&self->committed, 1, 0))
		DeleteFileW(self->copy);
}

static void destroy_transaction(struct object *base)
{
	struct transaction *self = (struct transaction *)base;
	close_stream(self->stream);
	release(self->stream);
	forget_copy(self);
}

static HRESULT WINAPI transaction_stream(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = transaction_of(iface)->stream);
	return S_OK;
}

static HRESULT run_commit(struct job *job, union winrt_value *result)
{
	struct transaction *self = transaction_of(job->other);
	if (InterlockedCompareExchange(&self->committed, 1, 0))
		return E_ILLEGAL_METHOD_CALL;
	if (!MoveFileExW(self->copy, self->target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		const HRESULT error = last_error();
		InterlockedExchange(&self->committed, 0);
		return error;
	}
	return S_OK;
}

static HRESULT WINAPI transaction_commit(void *iface, void **out)
{
	return start(NULL, WINRT_RESULT_NONE, new_job(run_commit, NULL, NULL, 0, iface), out);
}

static HRESULT WINAPI transaction_close(void *iface)
{
	struct transaction *self = transaction_of(iface);
	close_stream(self->stream);
	forget_copy(self);
	return S_OK;
}

TABLE(transaction_table, 0, INSPECTABLE, (const void *)transaction_stream, (const void *)transaction_commit);
TABLE(transaction_closable_table, 1, INSPECTABLE, (const void *)transaction_close);
static const struct face_iid transaction_iids[] = {{&iid_stream_transaction, 0}, {&iid_closable, 1}, {NULL, 0}};
static const struct kind transaction_kind = {"StorageStreamTransaction", L"Windows.Storage.StorageStreamTransaction", transaction_iids,
	destroy_transaction};

/* The copy is a new file in the same folder, so that putting it in the file's place is a rename. */
static HRESULT run_transacted(struct job *job, union winrt_value *result)
{
	static const void *const *const tables[] = {transaction_table.slots, transaction_closable_table.slots};
	WCHAR folder[MAX_PATH];
	UINT32 length;
	struct transaction *self = make(sizeof *self, &transaction_kind, tables, 2);
	if (!self)
		return E_OUTOFMEMORY;
	path_of(job->item, self->target);
	lstrcpyW(folder, self->target);
	folder[leaf(self->target, &length) - self->target] = 0;
	HRESULT outcome = exists(self->target, NULL) ? S_OK : NOT_THERE;
	if (SUCCEEDED(outcome) && !GetTempFileNameW(folder, L"rvt", 0, self->copy))
		outcome = last_error();
	if (FAILED(outcome))
		InterlockedExchange(&self->committed, 1);
	if (SUCCEEDED(outcome) && !CopyFileW(self->target, self->copy, FALSE))
		outcome = last_error();
	if (SUCCEEDED(outcome))
		outcome = open_stream(self->copy, TRUE, NULL, &self->stream);
	if (FAILED(outcome)) {
		release(face_of(&self->base, 0));
		return outcome;
	}
	result->object = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI file_open_transacted(void *iface, void **out)
{
	return start(&iid_transaction_operation, WINRT_RESULT_OBJECT, new_job(run_transacted, iface, NULL, 0, NULL), out);
}

/* The path a copy or a move goes to: the name (or the file's own) in the folder, chosen by the option when something is there. */
static HRESULT destination(struct job *job, const WCHAR *source, WCHAR *path)
{
	WCHAR base[MAX_PATH];
	HRESULT result = job->option < NAME_UNIQUE || job->option > NAME_FAIL ? E_INVALIDARG : path_of_any(job->other, base);
	if (SUCCEEDED(result)) {
		UINT32 length = 0;
		const WCHAR *name = job->name ? WindowsGetStringRawBuffer(job->name, &length) : leaf(source, &length);
		result = join(base, name, length, FALSE, path);
	}
	if (SUCCEEDED(result) && CompareStringOrdinal(path, -1, source, -1, TRUE) != CSTR_EQUAL && exists(path, NULL)) {
		if (job->option == NAME_FAIL)
			result = ALREADY_THERE;
		else if (job->option == NAME_UNIQUE)
			result = unique(path);
	}
	return result;
}

static HRESULT run_copy(struct job *job, union winrt_value *result)
{
	WCHAR source[MAX_PATH], path[MAX_PATH];
	path_of(job->item, source);
	HRESULT outcome = destination(job, source, path);
	if (SUCCEEDED(outcome) && !CopyFileW(source, path, job->option != NAME_REPLACE))
		outcome = last_error();
	return SUCCEEDED(outcome) ? item_result(path, FALSE, FALSE, result) : outcome;
}

static HRESULT WINAPI file_copy_as(void *iface, void *folder, HSTRING name, INT32 option, void **out)
{
	return start(&iid_file_operation, WINRT_RESULT_OBJECT, new_job(run_copy, iface, name, option, folder), out);
}

static HRESULT WINAPI file_copy(void *iface, void *folder, void **out) { return file_copy_as(iface, folder, NULL, NAME_FAIL, out); }
static HRESULT WINAPI file_copy_named(void *iface, void *folder, HSTRING name, void **out)
{
	return file_copy_as(iface, folder, name, NAME_FAIL, out);
}

static HRESULT run_copy_replace(struct job *job, union winrt_value *result)
{
	WCHAR source[MAX_PATH], path[MAX_PATH];
	path_of(job->item, source);
	HRESULT outcome = path_of_any(job->other, path);
	if (SUCCEEDED(outcome) && !CopyFileW(source, path, FALSE))
		outcome = last_error();
	return outcome;
}

static HRESULT WINAPI file_copy_replace(void *iface, void *file, void **out)
{
	return start(NULL, WINRT_RESULT_NONE, new_job(run_copy_replace, iface, NULL, 0, file), out);
}

/* A move leaves the item naming the path it moved to. */
static HRESULT run_move(struct job *job, union winrt_value *result)
{
	WCHAR source[MAX_PATH], path[MAX_PATH];
	path_of(job->item, source);
	HRESULT outcome = destination(job, source, path);
	if (SUCCEEDED(outcome) && !MoveFileExW(source, path, MOVEFILE_COPY_ALLOWED | (job->option == NAME_REPLACE ? MOVEFILE_REPLACE_EXISTING : 0)))
		outcome = last_error();
	if (SUCCEEDED(outcome))
		set_path(job->item, path);
	return outcome;
}

static HRESULT WINAPI file_move_as(void *iface, void *folder, HSTRING name, INT32 option, void **out)
{
	return start(NULL, WINRT_RESULT_NONE, new_job(run_move, iface, name, option, folder), out);
}

static HRESULT WINAPI file_move(void *iface, void *folder, void **out) { return file_move_as(iface, folder, NULL, NAME_FAIL, out); }
static HRESULT WINAPI file_move_named(void *iface, void *folder, HSTRING name, void **out)
{
	return file_move_as(iface, folder, name, NAME_FAIL, out);
}

static HRESULT run_move_replace(struct job *job, union winrt_value *result)
{
	WCHAR source[MAX_PATH], path[MAX_PATH];
	path_of(job->item, source);
	HRESULT outcome = path_of_any(job->other, path);
	if (SUCCEEDED(outcome) && !MoveFileExW(source, path, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING))
		outcome = last_error();
	if (SUCCEEDED(outcome))
		set_path(job->item, path);
	return outcome;
}

static HRESULT WINAPI file_move_replace(void *iface, void *file, void **out)
{
	return start(NULL, WINRT_RESULT_NONE, new_job(run_move_replace, iface, NULL, 0, file), out);
}

TABLE(item_table, 0, INSPECTABLE, (const void *)item_rename, (const void *)item_rename_as, (const void *)item_delete,
	(const void *)item_delete_as, (const void *)item_properties, (const void *)item_name, (const void *)item_path,
	(const void *)item_attributes, (const void *)item_created, (const void *)item_is_of_type);
TABLE(folder_table, 1, INSPECTABLE, (const void *)folder_create_file, (const void *)folder_create_file_as, (const void *)folder_create_folder,
	(const void *)folder_create_folder_as, (const void *)folder_get_file, (const void *)folder_get_folder, (const void *)folder_get_item,
	(const void *)folder_files, (const void *)folder_folders, (const void *)folder_items);
TABLE(file_read_table, 2, INSPECTABLE, (const void *)file_open_read);
TABLE(file_sequential_table, 3, INSPECTABLE, (const void *)file_open_sequential);
TABLE(file_table, 1, INSPECTABLE, (const void *)file_type, (const void *)file_content_type, (const void *)file_open,
	(const void *)file_open_transacted, (const void *)file_copy, (const void *)file_copy_named, (const void *)file_copy_as,
	(const void *)file_copy_replace, (const void *)file_move, (const void *)file_move_named, (const void *)file_move_as,
	(const void *)file_move_replace);
static const struct face_iid folder_iids[] = {{&iid_storage_item, 0}, {&iid_storage_folder, 1}, {NULL, 0}};
static const struct face_iid file_iids[] = {{&iid_storage_item, 0}, {&iid_storage_file, 1}, {&iid_stream_reference, 2}, {&iid_input_reference, 3},
	{NULL, 0}};
static const struct kind folder_kind = {"StorageFolder", L"Windows.Storage.StorageFolder", folder_iids, destroy_nothing};
static const struct kind file_kind = {"StorageFile", L"Windows.Storage.StorageFile", file_iids, destroy_nothing};

static HRESULT new_item(const WCHAR *path, BOOL folder, struct item **out)
{
	static const void *const *const folder_tables[] = {item_table.slots, folder_table.slots};
	static const void *const *const file_tables[] = {item_table.slots, file_table.slots, file_read_table.slots, file_sequential_table.slots};
	*out = NULL;
	if ((size_t)lstrlenW(path) >= MAX_PATH)
		return TOO_LONG;
	struct item *self = make(sizeof *self, folder ? &folder_kind : &file_kind, folder ? folder_tables : file_tables, folder ? 2 : 4);
	if (!self)
		return E_OUTOFMEMORY;
	InitializeSRWLock(&self->lock);
	lstrcpyW(self->path, path);
	self->folder = folder;
	*out = self;
	return S_OK;
}

HRESULT make_folder(const WCHAR *path, void **out)
{
	struct item *self;
	BOOL folder = FALSE;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!exists(path, &folder) || !folder)
		return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	const HRESULT result = new_item(path, TRUE, &self);
	if (SUCCEEDED(result))
		*out = face_of(&self->base, 1);
	return result;
}

/* StorageFile.GetFileFromPathAsync and StorageFolder.GetFolderFromPathAsync take a full path, in the job's name. */
static HRESULT run_from_path(struct job *job, union winrt_value *result)
{
	const BOOL folder = job->count != 0;
	UINT32 length = 0;
	const WCHAR *raw = WindowsGetStringRawBuffer(job->name, &length);
	WCHAR path[MAX_PATH];
	if (length >= MAX_PATH)
		return TOO_LONG;
	memcpy(path, raw, (size_t)length * sizeof(WCHAR));
	path[length] = 0;
	const BOOL full = (length >= 3 && path[1] == L':' && path[2] == L'\\') || (length >= 2 && path[0] == L'\\' && path[1] == L'\\');
	BOOL there_folder = FALSE;
	if (!full)
		return E_INVALIDARG;
	if (!exists(path, &there_folder))
		return NOT_THERE;
	if (there_folder != folder)
		return E_INVALIDARG;
	/* A folder is named without the separator that closed it. */
	while (length > 3 && path[length - 1] == L'\\')
		path[--length] = 0;
	return item_result(path, folder, FALSE, result);
}

static HRESULT from_path(HSTRING path, BOOL folder, void **out)
{
	struct job *job = new_job(run_from_path, NULL, path, 0, NULL);
	if (job)
		job->count = folder;
	return start(folder ? &iid_folder_operation : &iid_file_operation, WINRT_RESULT_OBJECT, job, out);
}

static HRESULT WINAPI file_from_path(void *iface, HSTRING path, void **out) { return from_path(path, FALSE, out); }
static HRESULT WINAPI folder_from_path(void *iface, HSTRING path, void **out) { return from_path(path, TRUE, out); }

static HRESULT WINAPI file_from_uri(void *iface, void *uri, void **out)
{
	answer_null(out);
	return missing("StorageFile.GetFileFromApplicationUriAsync");
}

static HRESULT WINAPI file_streamed(void *iface, HSTRING name, void *writer, void *thumbnail, void **out)
{
	answer_null(out);
	return missing("StorageFile.CreateStreamedFileAsync");
}

static HRESULT WINAPI file_replace_streamed(void *iface, void *file, void *writer, void *thumbnail, void **out)
{
	answer_null(out);
	return missing("StorageFile.ReplaceWithStreamedFileAsync");
}

static HRESULT WINAPI file_streamed_uri(void *iface, HSTRING name, void *uri, void *thumbnail, void **out)
{
	answer_null(out);
	return missing("StorageFile.CreateStreamedFileFromUriAsync");
}

static HRESULT WINAPI file_replace_streamed_uri(void *iface, void *file, void *uri, void *thumbnail, void **out)
{
	answer_null(out);
	return missing("StorageFile.ReplaceWithStreamedFileFromUriAsync");
}

TABLE(file_factory_table, 0, FACTORY);
TABLE(file_statics_table, 1, INSPECTABLE, (const void *)file_from_path, (const void *)file_from_uri, (const void *)file_streamed,
	(const void *)file_replace_streamed, (const void *)file_streamed_uri, (const void *)file_replace_streamed_uri);
static const struct face_iid file_class_iids[] = {{&iid_factory, 0}, {&iid_file_statics, 1}, {NULL, 0}};
static const struct kind file_class_kind = {"StorageFile", L"Windows.Storage.StorageFile", file_class_iids, NULL};
struct object storage_file_class = {{file_factory_table.slots, file_statics_table.slots}, &file_class_kind, 1};

TABLE(folder_factory_table, 0, FACTORY);
TABLE(folder_statics_table, 1, INSPECTABLE, (const void *)folder_from_path);
static const struct face_iid folder_class_iids[] = {{&iid_factory, 0}, {&iid_folder_statics, 1}, {NULL, 0}};
static const struct kind folder_class_kind = {"StorageFolder", L"Windows.Storage.StorageFolder", folder_class_iids, NULL};
struct object storage_folder_class = {{folder_factory_table.slots, folder_statics_table.slots}, &folder_class_kind, 1};

/* Windows.Storage.Streams.Buffer, made by its factory with a capacity and no bytes yet. */
static HRESULT WINAPI buffer_create(void *iface, UINT32 capacity, void **out)
{
	return winrt_buffer(NULL, 0, capacity, (__x_ABI_CWindows_CStorage_CStreams_CIBuffer **)out);
}

TABLE(buffer_factory_table, 0, FACTORY);
TABLE(buffer_create_table, 1, INSPECTABLE, (const void *)buffer_create);
static const struct face_iid buffer_class_iids[] = {{&iid_factory, 0}, {&iid_buffer_factory, 1}, {NULL, 0}};
static const struct kind buffer_class_kind = {"Buffer", L"Windows.Storage.Streams.Buffer", buffer_class_iids, NULL};
struct object buffer_class = {{buffer_factory_table.slots, buffer_create_table.slots}, &buffer_class_kind, 1};

/* new Windows.Storage.Streams.InMemoryRandomAccessStream(): an empty stream over bytes in memory, from raven-winrt. */
static HRESULT WINAPI memory_activate(void *iface, IInspectable **out) { return winrt_memory_stream((void **)out); }

TABLE(memory_factory_table, 0, INSPECTABLE, (const void *)memory_activate);
static const struct face_iid memory_class_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind memory_class_kind = {"InMemoryRandomAccessStream", L"Windows.Storage.Streams.InMemoryRandomAccessStream",
	memory_class_iids, NULL};
struct object memory_stream_class = {{memory_factory_table.slots}, &memory_class_kind, 1};

/* Windows.Storage.FileIO. The job's other is the file; its option the encoding. */
static HRESULT read_whole(void *file, BYTE **bytes, DWORD *size)
{
	WCHAR path[MAX_PATH];
	*bytes = NULL;
	*size = 0;
	HRESULT result = path_of_any(file, path);
	if (FAILED(result))
		return result;
	HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return last_error();
	LARGE_INTEGER length;
	if (!GetFileSizeEx(handle, &length))
		result = last_error();
	else if (length.QuadPart > 0x7fffffff)
		result = E_OUTOFMEMORY;
	else if (!(*bytes = ALLOC((size_t)length.QuadPart + 2)))
		result = E_OUTOFMEMORY;
	else if (!ReadFile(handle, *bytes, (DWORD)length.QuadPart, size, NULL))
		result = last_error();
	CloseHandle(handle);
	if (FAILED(result)) {
		FREE(*bytes);
		*bytes = NULL;
	}
	return result;
}

static HRESULT write_whole(void *file, const void *bytes, DWORD size, BOOL append)
{
	WCHAR path[MAX_PATH];
	HRESULT result = path_of_any(file, path);
	if (FAILED(result))
		return result;
	HANDLE handle = CreateFileW(path, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, NULL, append ? OPEN_EXISTING : TRUNCATE_EXISTING,
		0, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return last_error();
	DWORD written = 0;
	if (size && (!WriteFile(handle, bytes, size, &written, NULL) || written != size))
		result = last_error();
	CloseHandle(handle);
	return result;
}

/* Text from bytes: a byte order mark chooses the encoding, else the one given. */
static HRESULT decode(const BYTE *bytes, DWORD size, INT32 encoding, HSTRING *out)
{
	if (size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
		encoding = ENCODING_UTF8;
		bytes += 3;
		size -= 3;
	} else if (size >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
		encoding = ENCODING_UTF16LE;
		bytes += 2;
		size -= 2;
	} else if (size >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
		encoding = ENCODING_UTF16BE;
		bytes += 2;
		size -= 2;
	}
	if (encoding != ENCODING_UTF8 && encoding != ENCODING_UTF16LE && encoding != ENCODING_UTF16BE)
		return E_INVALIDARG;
	if (encoding != ENCODING_UTF8 && size % 2)
		return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
	const int length = encoding != ENCODING_UTF8 ? (int)(size / 2) :
		size ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, (int)size, NULL, 0) : 0;
	if (size && !length)
		return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
	if (!length)
		return WindowsCreateString(NULL, 0, out);
	/* winstring.h of mingw declares the string buffer functions without their calling convention, so the text is built here first. */
	WCHAR *text = ALLOC((size_t)length * sizeof *text);
	if (!text)
		return E_OUTOFMEMORY;
	if (encoding == ENCODING_UTF8)
		MultiByteToWideChar(CP_UTF8, 0, (const char *)bytes, (int)size, text, length);
	else
		for (int at = 0; at < length; ++at)
			text[at] = encoding == ENCODING_UTF16LE ? (WCHAR)(bytes[2 * at] | bytes[2 * at + 1] << 8) : (WCHAR)(bytes[2 * at] << 8 | bytes[2 * at + 1]);
	const HRESULT result = WindowsCreateString(text, (UINT32)length, out);
	FREE(text);
	return result;
}

/* Bytes of text in an encoding, which the caller frees. */
static HRESULT encode(const WCHAR *text, UINT32 length, INT32 encoding, BYTE **bytes, DWORD *size)
{
	*bytes = NULL;
	*size = 0;
	if (encoding == ENCODING_UTF8) {
		const int needed = length ? WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL) : 0;
		if (length && !needed)
			return last_error();
		if (!(*bytes = ALLOC((size_t)needed + 1)))
			return E_OUTOFMEMORY;
		*size = (DWORD)(needed ? WideCharToMultiByte(CP_UTF8, 0, text, (int)length, (char *)*bytes, needed, NULL, NULL) : 0);
		return S_OK;
	}
	if (encoding != ENCODING_UTF16LE && encoding != ENCODING_UTF16BE)
		return E_INVALIDARG;
	if (!(*bytes = ALLOC((size_t)length * 2 + 1)))
		return E_OUTOFMEMORY;
	const int low = encoding == ENCODING_UTF16LE ? 0 : 1;
	for (UINT32 at = 0; at < length; ++at) {
		(*bytes)[2 * at + low] = (BYTE)text[at];
		(*bytes)[2 * at + 1 - low] = (BYTE)(text[at] >> 8);
	}
	*size = length * 2;
	return S_OK;
}

static HRESULT read_text(void *file, INT32 encoding, HSTRING *out)
{
	BYTE *bytes;
	DWORD size;
	*out = NULL;
	HRESULT result = read_whole(file, &bytes, &size);
	if (SUCCEEDED(result))
		result = decode(bytes, size, encoding, out);
	FREE(bytes);
	return result;
}

static HRESULT write_text(void *file, HSTRING text, INT32 encoding, BOOL append)
{
	UINT32 length = 0;
	const WCHAR *raw = WindowsGetStringRawBuffer(text, &length);
	BYTE *bytes;
	DWORD size;
	HRESULT result = encode(raw, length, encoding, &bytes, &size);
	if (SUCCEEDED(result))
		result = write_whole(file, bytes, size, append);
	FREE(bytes);
	return result;
}

static struct job *io_job(HRESULT (*run)(struct job *, union winrt_value *), void *file, HSTRING text, INT32 encoding, void *extra)
{
	struct job *job = new_job(run, NULL, NULL, encoding, file);
	if (job && FAILED(WindowsDuplicateString(text, &job->text))) {
		free_job(job);
		return NULL;
	}
	if (job)
		reference(job->extra = extra);
	return job;
}

static HRESULT run_read_text(struct job *job, union winrt_value *result) { return read_text(job->other, job->option, &result->string); }

static HRESULT WINAPI io_read_text_as(void *iface, void *file, INT32 encoding, void **out)
{
	return start(&iid_string_operation, WINRT_RESULT_STRING, io_job(run_read_text, file, NULL, encoding, NULL), out);
}

static HRESULT WINAPI io_read_text(void *iface, void *file, void **out) { return io_read_text_as(iface, file, ENCODING_UTF8, out); }

/* The job's count says whether it appends (1) or replaces (0) what the file holds. */
static HRESULT run_write_text(struct job *job, union winrt_value *result)
{
	return write_text(job->other, job->text, job->option, job->count != 0);
}

static HRESULT write_text_job(void *file, HSTRING text, INT32 encoding, BOOL append, void **out)
{
	struct job *job = io_job(run_write_text, file, text, encoding, NULL);
	if (job)
		job->count = append;
	return start(NULL, WINRT_RESULT_NONE, job, out);
}

static HRESULT WINAPI io_write_text_as(void *iface, void *file, HSTRING text, INT32 encoding, void **out)
{
	return write_text_job(file, text, encoding, FALSE, out);
}

static HRESULT WINAPI io_write_text(void *iface, void *file, HSTRING text, void **out) { return write_text_job(file, text, ENCODING_UTF8, FALSE, out); }

static HRESULT WINAPI io_append_text_as(void *iface, void *file, HSTRING text, INT32 encoding, void **out)
{
	return write_text_job(file, text, encoding, TRUE, out);
}

static HRESULT WINAPI io_append_text(void *iface, void *file, HSTRING text, void **out) { return write_text_job(file, text, ENCODING_UTF8, TRUE, out); }

/* Lines end at CR LF, LF or CR; a last line ending is not an empty line after it. */
static HRESULT run_read_lines(struct job *job, union winrt_value *result)
{
	HSTRING text = NULL, *lines = NULL;
	UINT32 count = 0, capacity = 0, length = 0;
	HRESULT outcome = read_text(job->other, job->option, &text);
	const WCHAR *raw = WindowsGetStringRawBuffer(text, &length);
	for (UINT32 start = 0, at = 0; SUCCEEDED(outcome) && start < length; start = at) {
		while (at < length && raw[at] != L'\r' && raw[at] != L'\n')
			++at;
		if (count == capacity) {
			const UINT32 grown = capacity ? capacity * 2 : 64;
			HSTRING *bigger = lines ? HeapReAlloc(GetProcessHeap(), 0, lines, grown * sizeof *lines) : ALLOC(grown * sizeof *lines);
			if (!bigger) {
				outcome = E_OUTOFMEMORY;
				break;
			}
			lines = bigger;
			capacity = grown;
		}
		if (SUCCEEDED(outcome = WindowsCreateString(raw + start, at - start, &lines[count])))
			++count;
		if (at < length && raw[at] == L'\r' && at + 1 < length && raw[at + 1] == L'\n')
			++at;
		if (at < length)
			++at;
	}
	if (SUCCEEDED(outcome))
		outcome = winrt_vector(&line_list, (IInspectable **)lines, count, &result->object);
	for (UINT32 index = 0; index < count; ++index)
		WindowsDeleteString(lines[index]);
	FREE(lines);
	WindowsDeleteString(text);
	return outcome;
}

static HRESULT WINAPI io_read_lines_as(void *iface, void *file, INT32 encoding, void **out)
{
	return start(&iid_lines_operation, WINRT_RESULT_OBJECT, io_job(run_read_lines, file, NULL, encoding, NULL), out);
}

static HRESULT WINAPI io_read_lines(void *iface, void *file, void **out) { return io_read_lines_as(iface, file, ENCODING_UTF8, out); }

/* The lines of any IIterable<String>, each closed by CR LF, as one string. */
static HRESULT join_lines(void *lines, HSTRING *out)
{
	void *iterator = NULL;
	boolean more = FALSE;
	WCHAR *text = NULL;
	size_t length = 0, capacity = 0;
	*out = NULL;
	if (!lines)
		return E_INVALIDARG;
	HRESULT result = ((HRESULT (WINAPI *)(void *, void **))method(lines, 6))(lines, &iterator);
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, boolean *))method(iterator, 7))(iterator, &more);
	while (SUCCEEDED(result) && more) {
		HSTRING line = NULL;
		result = ((HRESULT (WINAPI *)(void *, HSTRING *))method(iterator, 6))(iterator, &line);
		UINT32 size = 0;
		const WCHAR *raw = WindowsGetStringRawBuffer(line, &size);
		if (SUCCEEDED(result) && length + size + 2 > capacity) {
			size_t grown = capacity ? capacity : 256;
			while (grown < length + size + 2)
				grown *= 2;
			WCHAR *bigger = text ? HeapReAlloc(GetProcessHeap(), 0, text, grown * sizeof *text) : ALLOC(grown * sizeof *text);
			if (!bigger)
				result = E_OUTOFMEMORY;
			else {
				text = bigger;
				capacity = grown;
			}
		}
		if (SUCCEEDED(result)) {
			memcpy(text + length, raw, (size_t)size * sizeof *text);
			length += size;
			text[length++] = L'\r';
			text[length++] = L'\n';
			result = ((HRESULT (WINAPI *)(void *, boolean *))method(iterator, 8))(iterator, &more);
		}
		WindowsDeleteString(line);
	}
	release(iterator);
	if (SUCCEEDED(result))
		result = length > 0xffffffffu ? E_OUTOFMEMORY : WindowsCreateString(text, (UINT32)length, out);
	FREE(text);
	return result;
}

static HRESULT run_write_lines(struct job *job, union winrt_value *result)
{
	HSTRING text = NULL;
	HRESULT outcome = join_lines(job->extra, &text);
	if (SUCCEEDED(outcome))
		outcome = write_text(job->other, text, job->option, job->count != 0);
	WindowsDeleteString(text);
	return outcome;
}

static HRESULT write_lines_job(void *file, void *lines, INT32 encoding, BOOL append, void **out)
{
	struct job *job = io_job(run_write_lines, file, NULL, encoding, lines);
	if (job)
		job->count = append;
	return start(NULL, WINRT_RESULT_NONE, job, out);
}

static HRESULT WINAPI io_write_lines_as(void *iface, void *file, void *lines, INT32 encoding, void **out)
{
	return write_lines_job(file, lines, encoding, FALSE, out);
}

static HRESULT WINAPI io_write_lines(void *iface, void *file, void *lines, void **out) { return write_lines_job(file, lines, ENCODING_UTF8, FALSE, out); }

static HRESULT WINAPI io_append_lines_as(void *iface, void *file, void *lines, INT32 encoding, void **out)
{
	return write_lines_job(file, lines, encoding, TRUE, out);
}

static HRESULT WINAPI io_append_lines(void *iface, void *file, void *lines, void **out) { return write_lines_job(file, lines, ENCODING_UTF8, TRUE, out); }

static HRESULT run_read_buffer(struct job *job, union winrt_value *result)
{
	BYTE *bytes;
	DWORD size;
	HRESULT outcome = read_whole(job->other, &bytes, &size);
	if (SUCCEEDED(outcome))
		outcome = winrt_buffer(bytes, size, size, (__x_ABI_CWindows_CStorage_CStreams_CIBuffer **)&result->object);
	FREE(bytes);
	return outcome;
}

static HRESULT WINAPI io_read_buffer(void *iface, void *file, void **out)
{
	return start(&iid_buffer_operation, WINRT_RESULT_OBJECT, io_job(run_read_buffer, file, NULL, 0, NULL), out);
}

/* The bytes to write: the job's own copy, or those of the buffer in extra. */
static HRESULT run_write_bytes(struct job *job, union winrt_value *result)
{
	BYTE *bytes = job->bytes;
	UINT32 length = job->count;
	if (job->extra) {
		const HRESULT got = winrt_buffer_bytes((__x_ABI_CWindows_CStorage_CStreams_CIBuffer *)job->extra, &bytes, &length);
		if (FAILED(got))
			return got;
	}
	return write_whole(job->other, bytes, length, FALSE);
}

static HRESULT WINAPI io_write_buffer(void *iface, void *file, void *buffer, void **out)
{
	if (!buffer) {
		answer_null(out);
		return E_INVALIDARG;
	}
	return start(NULL, WINRT_RESULT_NONE, io_job(run_write_bytes, file, NULL, 0, buffer), out);
}

static HRESULT WINAPI io_write_bytes(void *iface, void *file, UINT32 count, const BYTE *bytes, void **out)
{
	if (count && !bytes) {
		answer_null(out);
		return E_POINTER;
	}
	struct job *job = io_job(run_write_bytes, file, NULL, 0, NULL);
	if (job && count && !(job->bytes = ALLOC(count))) {
		free_job(job);
		job = NULL;
	}
	if (job) {
		memcpy(job->bytes, bytes, count);
		job->count = count;
	}
	return start(NULL, WINRT_RESULT_NONE, job, out);
}

TABLE(io_factory_table, 0, FACTORY);
TABLE(io_statics_table, 1, INSPECTABLE, (const void *)io_read_text, (const void *)io_read_text_as, (const void *)io_write_text,
	(const void *)io_write_text_as, (const void *)io_append_text, (const void *)io_append_text_as, (const void *)io_read_lines,
	(const void *)io_read_lines_as, (const void *)io_write_lines, (const void *)io_write_lines_as, (const void *)io_append_lines,
	(const void *)io_append_lines_as, (const void *)io_read_buffer, (const void *)io_write_buffer, (const void *)io_write_bytes);
static const struct face_iid io_class_iids[] = {{&iid_factory, 0}, {&iid_file_io, 1}, {NULL, 0}};
static const struct kind io_class_kind = {"FileIO", L"Windows.Storage.FileIO", io_class_iids, NULL};
struct object file_io_class = {{io_factory_table.slots, io_statics_table.slots}, &io_class_kind, 1};
