/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven background transfer component: the BackgroundCopyManager class of the Background Intelligent Transfer Service, through which
 * bootstrappers and updaters download files while they wait or show progress.
 *
 * Windows runs the service in its own process and keeps jobs across sign-ins; here the manager lives in the program that creates it, as
 * the class Raven serves for a local service, and a job lasts as long as that program. A job downloads its files one after another on a
 * thread of its own, over HTTP or HTTPS through WinHTTP or from a file share, each into a temporary file beside the file it names; the
 * job's Complete moves them into place. A transfer that is suspended starts the file over when it resumes, and a failure ends in the error
 * state at once, without the retries Windows makes until the job's timeout. Upload jobs, file ranges and client certificates are not
 * provided, and the time limit of a download is kept but not enforced. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <objbase.h>
#include <stddef.h>
#include <sddl.h>
#include <winhttp.h>
#include <bits.h>
#include <bits1_5.h>
#include <bits2_0.h>
#include <bits2_5.h>
#include <bits3_0.h>
#include <bitsmsg.h>

/* The security flags of the HTTP options, which the MinGW headers leave out. */
#ifndef BG_SSL_IGNORE_CERT_CN_INVALID
#define BG_SSL_IGNORE_CERT_CN_INVALID 0x0002
#define BG_SSL_IGNORE_CERT_DATE_INVALID 0x0004
#define BG_SSL_IGNORE_UNKNOWN_CA 0x0008
#define BG_SSL_IGNORE_CERT_WRONG_USAGE 0x0010
#define BG_HTTP_REDIRECT_POLICY_MASK 0x0700
#define BG_HTTP_REDIRECT_POLICY_DISALLOW 0x0200
#endif

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define GROW(block, size) ((block) ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (block), (size)) : ALLOC(size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

__declspec(dllimport) void WINAPI RavenTrace(const char *text);

static HRESULT unimplemented(const char *method)
{
	char text[120] = "bits: ";
	int at = 6;
	for (int index = 0; method[index] && at < 90; ++index)
		text[at++] = method[index];
	lstrcpyA(text + at, " is not implemented");
	RavenTrace(text);
	return E_NOTIMPL;
}

static BOOL same(REFIID left, REFIID right)
{
	const DWORD *a = (const DWORD *)left, *b = (const DWORD *)right;
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* Objects alive, which keep the library loaded, and the one lock every job and file of the process shares. */
static LONG objects;
static CRITICAL_SECTION queue;

/* ---- strings ---- */
static WCHAR *copy_text(const WCHAR *text)
{
	if (!text)
		return NULL;
	const int length = lstrlenW(text);
	WCHAR *copy = ALLOC((length + 1) * sizeof(WCHAR));
	if (copy)
		memcpy(copy, text, (length + 1) * sizeof(WCHAR));
	return copy;
}

/* A string handed to the caller, who frees it with CoTaskMemFree. */
static HRESULT hand_text(const WCHAR *text, LPWSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	const int length = text ? lstrlenW(text) : 0;
	*out = CoTaskMemAlloc((length + 1) * sizeof(WCHAR));
	if (!*out)
		return E_OUTOFMEMORY;
	if (length)
		memcpy(*out, text, length * sizeof(WCHAR));
	(*out)[length] = 0;
	return S_OK;
}

static BOOL starts_with(const WCHAR *text, const WCHAR *prefix)
{
	for (; *prefix; ++text, ++prefix) {
		WCHAR a = *text, b = *prefix;
		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z')
			b += 'a' - 'A';
		if (a != b)
			return FALSE;
	}
	return TRUE;
}

static BOOL web_name(const WCHAR *name)
{
	return starts_with(name, L"http://") || starts_with(name, L"https://");
}

/* A remote file is a web address or a file share; a local one is a full path, on a drive or a share. */
static HRESULT check_names(const WCHAR *remote, const WCHAR *local)
{
	if (!remote || !local || !*remote || !*local)
		return E_INVALIDARG;
	if (!web_name(remote) && !(remote[0] == '\\' && remote[1] == '\\'))
		return BG_E_PROTOCOL_NOT_AVAILABLE;
	const BOOL drive = ((local[0] | 0x20) >= 'a' && (local[0] | 0x20) <= 'z' && local[1] == ':' && local[2] == '\\');
	if (!drive && !(local[0] == '\\' && local[1] == '\\'))
		return E_INVALIDARG;
	return S_OK;
}

/* ---- files ---- */
typedef struct file {
	IBackgroundCopyFile2 iface;
	LONG refs;
	WCHAR *remote, *local, *temporary; /* the temporary file exists once the file is transferred */
	BG_FILE_PROGRESS progress;
} file;

static file *file_of(IBackgroundCopyFile2 *iface) { return (file *)iface; }

static HRESULT WINAPI file_query(IBackgroundCopyFile2 *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IBackgroundCopyFile) || same(iid, &IID_IBackgroundCopyFile2)) {
		IBackgroundCopyFile2_AddRef(iface);
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI file_reference(IBackgroundCopyFile2 *iface) { return InterlockedIncrement(&file_of(iface)->refs); }

static ULONG WINAPI file_release(IBackgroundCopyFile2 *iface)
{
	file *self = file_of(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		FREE(self->remote);
		FREE(self->local);
		FREE(self->temporary);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return refs;
}

static HRESULT WINAPI file_remote(IBackgroundCopyFile2 *iface, LPWSTR *out)
{
	EnterCriticalSection(&queue);
	const HRESULT result = hand_text(file_of(iface)->remote, out);
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT WINAPI file_local(IBackgroundCopyFile2 *iface, LPWSTR *out) { return hand_text(file_of(iface)->local, out); }

static HRESULT WINAPI file_progress(IBackgroundCopyFile2 *iface, BG_FILE_PROGRESS *out)
{
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&queue);
	*out = file_of(iface)->progress;
	LeaveCriticalSection(&queue);
	return S_OK;
}

static HRESULT WINAPI file_ranges(IBackgroundCopyFile2 *iface, DWORD *count, BG_FILE_RANGE **ranges)
{
	if (!count || !ranges)
		return E_INVALIDARG;
	*count = 0;
	*ranges = NULL;
	return S_OK;
}

/* A new address for a file not yet transferred, of the same kind as the old one. */
static HRESULT WINAPI file_rename_remote(IBackgroundCopyFile2 *iface, LPCWSTR name)
{
	file *self = file_of(iface);
	if (!name || check_names(name, L"C:\\") != S_OK)
		return E_INVALIDARG;
	WCHAR *copy = copy_text(name);
	if (!copy)
		return E_OUTOFMEMORY;
	EnterCriticalSection(&queue);
	HRESULT result = S_OK;
	if (self->progress.Completed || web_name(name) != web_name(self->remote)) {
		result = self->progress.Completed ? BG_E_INVALID_STATE : E_INVALIDARG;
		FREE(copy);
	} else {
		FREE(self->remote);
		self->remote = copy;
	}
	LeaveCriticalSection(&queue);
	return result;
}

static IBackgroundCopyFile2Vtbl file_table = {file_query, file_reference, file_release, file_remote, file_local, file_progress, file_ranges,
	file_rename_remote};

/* ---- enumerators ---- */
/* An enumerator of jobs or files holds a reference to each, taken when it was made, and its own position. */
#define ENUMERATOR(prefix, enum_type, item_type, item_iid, enum_iid) \
	typedef struct { \
		enum_type iface; \
		LONG refs; \
		ULONG count, at; \
		item_type **items; \
	} prefix##_enumerator; \
	static enum_type##Vtbl prefix##_enum_table; \
	static HRESULT prefix##_enumerate(item_type **items, ULONG count, ULONG at, enum_type **out) \
	{ \
		prefix##_enumerator *self = ALLOC(sizeof *self); \
		if (!self) \
			return E_OUTOFMEMORY; \
		self->items = count ? ALLOC(count * sizeof *items) : NULL; \
		if (count && !self->items) { \
			FREE(self); \
			return E_OUTOFMEMORY; \
		} \
		self->iface.lpVtbl = &prefix##_enum_table; \
		self->refs = 1; \
		self->count = count; \
		self->at = at; \
		for (ULONG index = 0; index < count; ++index) { \
			self->items[index] = items[index]; \
			item_type##_AddRef(items[index]); \
		} \
		InterlockedIncrement(&objects); \
		*out = &self->iface; \
		return S_OK; \
	} \
	static HRESULT WINAPI prefix##_enum_query(enum_type *iface, REFIID iid, void **out) \
	{ \
		if (!out) \
			return E_POINTER; \
		if (same(iid, &IID_IUnknown) || same(iid, enum_iid)) { \
			enum_type##_AddRef(iface); \
			*out = iface; \
			return S_OK; \
		} \
		*out = NULL; \
		return E_NOINTERFACE; \
	} \
	static ULONG WINAPI prefix##_enum_reference(enum_type *iface) { return InterlockedIncrement(&((prefix##_enumerator *)iface)->refs); } \
	static ULONG WINAPI prefix##_enum_release(enum_type *iface) \
	{ \
		prefix##_enumerator *self = (prefix##_enumerator *)iface; \
		const LONG refs = InterlockedDecrement(&self->refs); \
		if (!refs) { \
			for (ULONG index = 0; index < self->count; ++index) \
				item_type##_Release(self->items[index]); \
			FREE(self->items); \
			FREE(self); \
			InterlockedDecrement(&objects); \
		} \
		return refs; \
	} \
	static HRESULT WINAPI prefix##_enum_next(enum_type *iface, ULONG wanted, item_type **out, ULONG *fetched) \
	{ \
		prefix##_enumerator *self = (prefix##_enumerator *)iface; \
		if (!out || (wanted > 1 && !fetched)) \
			return E_INVALIDARG; \
		ULONG given = 0; \
		while (given < wanted && self->at < self->count) { \
			out[given] = self->items[self->at++]; \
			item_type##_AddRef(out[given]); \
			++given; \
		} \
		if (fetched) \
			*fetched = given; \
		return given == wanted ? S_OK : S_FALSE; \
	} \
	static HRESULT WINAPI prefix##_enum_skip(enum_type *iface, ULONG count) \
	{ \
		prefix##_enumerator *self = (prefix##_enumerator *)iface; \
		const ULONG left = self->count - self->at; \
		self->at += count < left ? count : left; \
		return count <= left ? S_OK : S_FALSE; \
	} \
	static HRESULT WINAPI prefix##_enum_reset(enum_type *iface) \
	{ \
		((prefix##_enumerator *)iface)->at = 0; \
		return S_OK; \
	} \
	static HRESULT WINAPI prefix##_enum_clone(enum_type *iface, enum_type **out) \
	{ \
		prefix##_enumerator *self = (prefix##_enumerator *)iface; \
		if (!out) \
			return E_POINTER; \
		return prefix##_enumerate(self->items, self->count, self->at, out); \
	} \
	static HRESULT WINAPI prefix##_enum_count(enum_type *iface, ULONG *out) \
	{ \
		if (!out) \
			return E_INVALIDARG; \
		*out = ((prefix##_enumerator *)iface)->count; \
		return S_OK; \
	} \
	static enum_type##Vtbl prefix##_enum_table = {prefix##_enum_query, prefix##_enum_reference, prefix##_enum_release, prefix##_enum_next, \
		prefix##_enum_skip, prefix##_enum_reset, prefix##_enum_clone, prefix##_enum_count};

ENUMERATOR(files, IEnumBackgroundCopyFiles, IBackgroundCopyFile, IID_IBackgroundCopyFile, &IID_IEnumBackgroundCopyFiles)
ENUMERATOR(jobs, IEnumBackgroundCopyJobs, IBackgroundCopyJob, IID_IBackgroundCopyJob, &IID_IEnumBackgroundCopyJobs)

/* ---- descriptions ---- */
static void append_number(WCHAR *text, ULONG value, int base, int digits)
{
	WCHAR reversed[16];
	int count = 0;
	do {
		const int digit = (int)(value % base);
		reversed[count++] = (WCHAR)(digit < 10 ? '0' + digit : 'A' + digit - 10);
		value /= base;
	} while (value || count < digits);
	text += lstrlenW(text);
	while (count)
		*text++ = reversed[--count];
	*text = 0;
}

static const struct {
	HRESULT code;
	const WCHAR *text;
} descriptions[] = {
	{BG_E_NOT_FOUND, L"The requested job was not found."},
	{BG_E_INVALID_STATE, L"The requested action is not allowed in the current job state."},
	{BG_E_EMPTY, L"There are no files attached to this job."},
	{BG_E_FILE_NOT_AVAILABLE, L"No file is available because no URL generated an error."},
	{BG_E_PROTOCOL_NOT_AVAILABLE, L"No protocol is available for the remote file."},
	{BG_E_ERROR_INFORMATION_UNAVAILABLE, L"No error has occurred for this job."},
	{BG_E_INVALID_SERVER_RESPONSE, L"The server's response was not valid."},
	{BG_E_TOO_MANY_FILES, L"The job has too many files."},
	{HRESULT_FROM_WIN32(ERROR_WINHTTP_CANNOT_CONNECT), L"The connection with the server could not be established."},
	{HRESULT_FROM_WIN32(ERROR_WINHTTP_NAME_NOT_RESOLVED), L"The server name or address could not be resolved."},
	{HRESULT_FROM_WIN32(ERROR_WINHTTP_TIMEOUT), L"The operation timed out."},
	{HRESULT_FROM_WIN32(ERROR_WINHTTP_CONNECTION_ERROR), L"The connection with the server was reset or terminated."},
	{HRESULT_FROM_WIN32(ERROR_WINHTTP_SECURE_FAILURE), L"A security error occurred while connecting to the server."},
};

/* The text of an error: a status the server answered, one of this service's own, or one the system describes. */
static HRESULT describe(HRESULT code, DWORD language, LPWSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (((ULONG)code & 0xffff0000UL) == 0x80190000UL) {
		WCHAR text[64] = L"The server answered with HTTP status ";
		append_number(text, (ULONG)code & 0xffff, 10, 1);
		lstrcatW(text, L".");
		return hand_text(text, out);
	}
	for (unsigned index = 0; index < sizeof descriptions / sizeof *descriptions; ++index)
		if (descriptions[index].code == code)
			return hand_text(descriptions[index].text, out);
	WCHAR *system = NULL;
	if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)code, language,
			(WCHAR *)&system, 0, NULL) && system) {
		const HRESULT result = hand_text(system, out);
		LocalFree(system);
		return result;
	}
	return HRESULT_FROM_WIN32(ERROR_MR_MID_NOT_FOUND);
}

static const WCHAR *context_text(BG_ERROR_CONTEXT context)
{
	switch (context) {
	case BG_ERROR_CONTEXT_NONE: return L"No error occurred.";
	case BG_ERROR_CONTEXT_GENERAL_QUEUE_MANAGER: return L"The error occurred in the transfer queue manager.";
	case BG_ERROR_CONTEXT_QUEUE_MANAGER_NOTIFICATION: return L"The error occurred while notifying the application.";
	case BG_ERROR_CONTEXT_LOCAL_FILE: return L"The error occurred while processing the local file.";
	case BG_ERROR_CONTEXT_REMOTE_FILE: return L"The error occurred while processing the remote file.";
	case BG_ERROR_CONTEXT_GENERAL_TRANSPORT: return L"The error occurred in the transport layer.";
	case BG_ERROR_CONTEXT_REMOTE_APPLICATION: return L"The error occurred in the server application.";
	default: return L"The error occurred in an unknown location.";
	}
}

/* ---- errors ---- */
typedef struct {
	IBackgroundCopyError iface;
	LONG refs;
	BG_ERROR_CONTEXT context;
	HRESULT code;
	IBackgroundCopyFile2 *file;
} error;

static HRESULT WINAPI error_query(IBackgroundCopyError *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IBackgroundCopyError)) {
		IBackgroundCopyError_AddRef(iface);
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI error_reference(IBackgroundCopyError *iface) { return InterlockedIncrement(&((error *)iface)->refs); }

static ULONG WINAPI error_release(IBackgroundCopyError *iface)
{
	error *self = (error *)iface;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		if (self->file)
			IBackgroundCopyFile2_Release(self->file);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return refs;
}

static HRESULT WINAPI error_get(IBackgroundCopyError *iface, BG_ERROR_CONTEXT *context, HRESULT *code)
{
	if (!context || !code)
		return E_INVALIDARG;
	*context = ((error *)iface)->context;
	*code = ((error *)iface)->code;
	return S_OK;
}

static HRESULT WINAPI error_file(IBackgroundCopyError *iface, IBackgroundCopyFile **out)
{
	error *self = (error *)iface;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->file)
		return BG_E_FILE_NOT_AVAILABLE;
	IBackgroundCopyFile2_AddRef(self->file);
	*out = (IBackgroundCopyFile *)self->file;
	return S_OK;
}

static HRESULT WINAPI error_description(IBackgroundCopyError *iface, DWORD language, LPWSTR *out)
{
	return describe(((error *)iface)->code, language, out);
}

static HRESULT WINAPI error_context_description(IBackgroundCopyError *iface, DWORD language, LPWSTR *out)
{
	return hand_text(context_text(((error *)iface)->context), out);
}

/* The protocol of the file that failed: "http" for the web, "file" for a share; none for an error of no file. */
static HRESULT WINAPI error_protocol(IBackgroundCopyError *iface, LPWSTR *out)
{
	error *self = (error *)iface;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!self->file)
		return BG_E_PROTOCOL_NOT_AVAILABLE;
	EnterCriticalSection(&queue);
	const BOOL web = web_name(file_of(self->file)->remote);
	LeaveCriticalSection(&queue);
	return hand_text(web ? L"http" : L"file", out);
}

static IBackgroundCopyErrorVtbl error_table = {error_query, error_reference, error_release, error_get, error_file, error_description,
	error_context_description, error_protocol};

/* ---- jobs ---- */
typedef struct job {
	IBackgroundCopyJob4 iface;
	IBackgroundCopyJobHttpOptions http;
	LONG refs;
	GUID id;
	BG_JOB_STATE state;
	BG_JOB_PRIORITY priority;
	BG_JOB_TIMES times;
	WCHAR *name, *description, *program, *parameters, *headers, *proxies, *bypass;
	BG_JOB_PROXY_USAGE proxy_usage;
	ULONG notify_flags, retry_delay, no_progress_timeout, maximum_time, security_flags, errors;
	DWORD acl_flags, peer_flags;
	IUnknown *callback;
	file **files;
	ULONG count, transferred;
	/* The last error, and the file it happened on. */
	HRESULT code;
	BG_ERROR_CONTEXT context;
	file *failed;
	/* Credentials for the server and for a proxy. */
	BG_AUTH_SCHEME schemes[2];
	WCHAR *users[2], *passwords[2];
	/* Each transfer counts as one generation: suspending or cancelling the job starts a new one, and a thread whose generation has passed
	 * leaves the job as it is. */
	LONG generation;
	struct job *next;
} job;

static job *jobs_list;

static job *job_of(IBackgroundCopyJob4 *iface) { return (job *)iface; }

static void touch(job *self)
{
	GetSystemTimeAsFileTime(&self->times.ModificationTime);
}

static HRESULT WINAPI job_query(IBackgroundCopyJob4 *iface, REFIID iid, void **out)
{
	job *self = job_of(iface);
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IBackgroundCopyJob) || same(iid, &IID_IBackgroundCopyJob2) ||
		same(iid, &IID_IBackgroundCopyJob3) || same(iid, &IID_IBackgroundCopyJob4))
		*out = &self->iface;
	else if (same(iid, &IID_IBackgroundCopyJobHttpOptions))
		*out = &self->http;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG WINAPI job_reference(IBackgroundCopyJob4 *iface) { return InterlockedIncrement(&job_of(iface)->refs); }

static ULONG WINAPI job_release(IBackgroundCopyJob4 *iface)
{
	job *self = job_of(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		for (ULONG index = 0; index < self->count; ++index)
			IBackgroundCopyFile2_Release(&self->files[index]->iface);
		if (self->failed)
			IBackgroundCopyFile2_Release(&self->failed->iface);
		if (self->callback)
			IUnknown_Release(self->callback);
		FREE(self->files);
		WCHAR *texts[] = {self->name, self->description, self->program, self->parameters, self->headers, self->proxies, self->bypass,
			self->users[0], self->users[1], self->passwords[0], self->passwords[1]};
		for (unsigned index = 0; index < sizeof texts / sizeof *texts; ++index)
			FREE(texts[index]);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return refs;
}

static BOOL finished(BG_JOB_STATE state)
{
	return state == BG_JOB_STATE_ACKNOWLEDGED || state == BG_JOB_STATE_CANCELLED;
}

static HRESULT add_files(job *self, ULONG count, const WCHAR *const *remotes, const WCHAR *const *locals)
{
	for (ULONG index = 0; index < count; ++index) {
		const HRESULT result = check_names(remotes[index], locals[index]);
		if (result != S_OK)
			return result;
	}
	file **made = ALLOC((count ? count : 1) * sizeof *made);
	if (!made)
		return E_OUTOFMEMORY;
	for (ULONG index = 0; index < count; ++index) {
		made[index] = ALLOC(sizeof **made);
		if (made[index]) {
			made[index]->iface.lpVtbl = &file_table;
			made[index]->refs = 1;
			made[index]->remote = copy_text(remotes[index]);
			made[index]->local = copy_text(locals[index]);
			made[index]->progress.BytesTotal = BG_SIZE_UNKNOWN;
			InterlockedIncrement(&objects);
		}
		if (!made[index] || !made[index]->remote || !made[index]->local) {
			for (ULONG undo = 0; undo <= index; ++undo)
				if (made[undo])
					IBackgroundCopyFile2_Release(&made[undo]->iface);
			FREE(made);
			return E_OUTOFMEMORY;
		}
	}
	EnterCriticalSection(&queue);
	HRESULT result = S_OK;
	file **grown = NULL;
	if (finished(self->state) || self->state == BG_JOB_STATE_TRANSFERRED)
		result = BG_E_INVALID_STATE;
	else if (!(grown = GROW(self->files, (self->count + count) * sizeof *grown)))
		result = E_OUTOFMEMORY;
	if (result == S_OK) {
		self->files = grown;
		memcpy(self->files + self->count, made, count * sizeof *made);
		self->count += count;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	if (result != S_OK)
		for (ULONG index = 0; index < count; ++index)
			IBackgroundCopyFile2_Release(&made[index]->iface);
	FREE(made);
	return result;
}

static HRESULT WINAPI job_add_file_set(IBackgroundCopyJob4 *iface, ULONG count, BG_FILE_INFO *set)
{
	if (!set && count)
		return E_INVALIDARG;
	const WCHAR **names = ALLOC((count ? count : 1) * 2 * sizeof *names);
	if (!names)
		return E_OUTOFMEMORY;
	for (ULONG index = 0; index < count; ++index) {
		names[index] = set[index].RemoteName;
		names[count + index] = set[index].LocalName;
	}
	const HRESULT result = add_files(job_of(iface), count, names, names + count);
	FREE(names);
	return result;
}

static HRESULT WINAPI job_add_file(IBackgroundCopyJob4 *iface, LPCWSTR remote, LPCWSTR local)
{
	return add_files(job_of(iface), 1, &remote, &local);
}

static HRESULT WINAPI job_enum_files(IBackgroundCopyJob4 *iface, IEnumBackgroundCopyFiles **out)
{
	job *self = job_of(iface);
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&queue);
	const HRESULT result = files_enumerate((IBackgroundCopyFile **)self->files, self->count, 0, out);
	LeaveCriticalSection(&queue);
	return result;
}

/* ---- the transfer ---- */
/* What a transfer reads from its job before it starts, so it runs without the lock. */
typedef struct {
	BG_JOB_PROXY_USAGE proxy_usage;
	WCHAR *proxies, *bypass, *headers;
	ULONG security_flags;
	BG_AUTH_SCHEME schemes[2];
	WCHAR *users[2], *passwords[2];
} settings;

static void read_settings(job *self, settings *out)
{
	out->proxy_usage = self->proxy_usage;
	out->proxies = copy_text(self->proxies);
	out->bypass = copy_text(self->bypass);
	out->headers = copy_text(self->headers);
	out->security_flags = self->security_flags;
	for (int target = 0; target < 2; ++target) {
		out->schemes[target] = self->schemes[target];
		out->users[target] = copy_text(self->users[target]);
		out->passwords[target] = copy_text(self->passwords[target]);
	}
}

static void free_settings(settings *values)
{
	FREE(values->proxies);
	FREE(values->bypass);
	FREE(values->headers);
	for (int target = 0; target < 2; ++target) {
		FREE(values->users[target]);
		FREE(values->passwords[target]);
	}
}

/* Moves the job to a state while its generation lasts; FALSE once the job has moved on. */
static BOOL advance(job *self, LONG generation, BG_JOB_STATE state)
{
	EnterCriticalSection(&queue);
	const BOOL current = self->generation == generation;
	if (current && self->state != state) {
		self->state = state;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	return current;
}

/* Bytes that arrived for a file: FALSE once the job has moved on. */
static BOOL arrived(job *self, LONG generation, file *item, UINT64 total, DWORD count)
{
	EnterCriticalSection(&queue);
	const BOOL current = self->generation == generation;
	if (current) {
		if (total != BG_SIZE_UNKNOWN)
			item->progress.BytesTotal = total;
		item->progress.BytesTransferred += count;
		if (self->state != BG_JOB_STATE_TRANSFERRING) {
			self->state = BG_JOB_STATE_TRANSFERRING;
			touch(self);
		}
	}
	LeaveCriticalSection(&queue);
	return current;
}

typedef struct {
	job *owner;
	LONG generation;
	file *item;
	HANDLE output;
	BG_ERROR_CONTEXT context;
	BYTE *buffer;
} transfer;

#define CHUNK 65536
#define STOPPED S_FALSE

static HRESULT write_chunk(transfer *work, DWORD count, UINT64 total)
{
	DWORD written = 0;
	if (!WriteFile(work->output, work->buffer, count, &written, NULL) || written != count) {
		work->context = BG_ERROR_CONTEXT_LOCAL_FILE;
		return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
	}
	return arrived(work->owner, work->generation, work->item, total, count) ? S_OK : STOPPED;
}

static HRESULT transport_error(transfer *work)
{
	work->context = BG_ERROR_CONTEXT_GENERAL_TRANSPORT;
	const DWORD code = GetLastError();
	return code ? HRESULT_FROM_WIN32(code) : BG_E_INVALID_SERVER_RESPONSE;
}

static UINT64 parse_size(const WCHAR *text)
{
	UINT64 value = 0;
	if (!*text)
		return BG_SIZE_UNKNOWN;
	for (; *text; ++text) {
		if (*text < '0' || *text > '9')
			return BG_SIZE_UNKNOWN;
		value = value * 10 + (UINT64)(*text - '0');
	}
	return value;
}

static DWORD winhttp_scheme(BG_AUTH_SCHEME scheme)
{
	switch (scheme) {
	case BG_AUTH_SCHEME_BASIC: return WINHTTP_AUTH_SCHEME_BASIC;
	case BG_AUTH_SCHEME_DIGEST: return WINHTTP_AUTH_SCHEME_DIGEST;
	case BG_AUTH_SCHEME_NTLM: return WINHTTP_AUTH_SCHEME_NTLM;
	case BG_AUTH_SCHEME_NEGOTIATE: return WINHTTP_AUTH_SCHEME_NEGOTIATE;
	case BG_AUTH_SCHEME_PASSPORT: return WINHTTP_AUTH_SCHEME_PASSPORT;
	default: return 0;
	}
}

/* A file from the web: one GET, answered once more with the job's credentials when the server or proxy asks for them. */
static HRESULT fetch_web(transfer *work, const WCHAR *remote, const settings *values)
{
	WCHAR host[256], path[2048], extra[2048];
	URL_COMPONENTS parts;
	memset(&parts, 0, sizeof parts);
	parts.dwStructSize = sizeof parts;
	parts.lpszHostName = host;
	parts.dwHostNameLength = sizeof host / sizeof *host;
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = sizeof path / sizeof *path;
	parts.lpszExtraInfo = extra;
	parts.dwExtraInfoLength = sizeof extra / sizeof *extra;
	if (!WinHttpCrackUrl(remote, 0, 0, &parts)) {
		work->context = BG_ERROR_CONTEXT_REMOTE_FILE;
		return HRESULT_FROM_WIN32(ERROR_WINHTTP_INVALID_URL);
	}
	if (lstrlenW(path) + lstrlenW(extra) < (int)(sizeof path / sizeof *path))
		lstrcatW(path, extra);
	if (!*path)
		lstrcpyW(path, L"/");
	DWORD access = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
	const WCHAR *proxies = WINHTTP_NO_PROXY_NAME, *bypass = WINHTTP_NO_PROXY_BYPASS;
	if (values->proxy_usage == BG_JOB_PROXY_USAGE_NO_PROXY)
		access = WINHTTP_ACCESS_TYPE_NO_PROXY;
	else if (values->proxy_usage == BG_JOB_PROXY_USAGE_OVERRIDE && values->proxies) {
		access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
		proxies = values->proxies;
		bypass = values->bypass ? values->bypass : WINHTTP_NO_PROXY_BYPASS;
	}
	HRESULT result = S_OK;
	HINTERNET session = WinHttpOpen(L"Microsoft BITS/7.8", access, proxies, bypass, 0), connection = NULL, request = NULL;
	if (session)
		connection = WinHttpConnect(session, host, parts.nPort, 0);
	if (connection)
		request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	if (!request) {
		result = transport_error(work);
		goto done;
	}
	/* The certificate problems the job accepts, and whether redirects are followed. */
	DWORD ignore = 0;
	if (values->security_flags & BG_SSL_IGNORE_CERT_CN_INVALID)
		ignore |= SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
	if (values->security_flags & BG_SSL_IGNORE_CERT_DATE_INVALID)
		ignore |= SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
	if (values->security_flags & BG_SSL_IGNORE_UNKNOWN_CA)
		ignore |= SECURITY_FLAG_IGNORE_UNKNOWN_CA;
	if (values->security_flags & BG_SSL_IGNORE_CERT_WRONG_USAGE)
		ignore |= SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
	if (ignore)
		WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &ignore, sizeof ignore);
	if ((values->security_flags & BG_HTTP_REDIRECT_POLICY_MASK) == BG_HTTP_REDIRECT_POLICY_DISALLOW) {
		DWORD feature = WINHTTP_DISABLE_REDIRECTS;
		WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &feature, sizeof feature);
	}
	if (values->headers && *values->headers)
		WinHttpAddRequestHeaders(request, values->headers, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
	DWORD status = 0;
	for (int attempt = 0;; ++attempt) {
		if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
			!WinHttpReceiveResponse(request, NULL)) {
			result = transport_error(work);
			goto done;
		}
		DWORD size = sizeof status;
		if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
				WINHTTP_NO_HEADER_INDEX)) {
			result = transport_error(work);
			goto done;
		}
		const int target = status == 407 ? 1 : 0;
		if (attempt || (status != 401 && status != 407) || !values->users[target])
			break;
		if (!WinHttpSetCredentials(request, target ? WINHTTP_AUTH_TARGET_PROXY : WINHTTP_AUTH_TARGET_SERVER,
				winhttp_scheme(values->schemes[target]), values->users[target], values->passwords[target], NULL))
			break;
	}
	if (status != 200) {
		work->context = BG_ERROR_CONTEXT_REMOTE_FILE;
		result = (HRESULT)(0x80190000UL | (status & 0xffff));
		goto done;
	}
	WCHAR length[32];
	DWORD length_size = sizeof length;
	UINT64 total = BG_SIZE_UNKNOWN;
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &length_size, WINHTTP_NO_HEADER_INDEX))
		total = parse_size(length);
	if (!arrived(work->owner, work->generation, work->item, total, 0)) {
		result = STOPPED;
		goto done;
	}
	for (;;) {
		DWORD count = 0;
		if (!WinHttpReadData(request, work->buffer, CHUNK, &count)) {
			result = transport_error(work);
			break;
		}
		if (!count)
			break;
		if ((result = write_chunk(work, count, total)) != S_OK)
			break;
	}
done:
	if (request)
		WinHttpCloseHandle(request);
	if (connection)
		WinHttpCloseHandle(connection);
	if (session)
		WinHttpCloseHandle(session);
	return result;
}

/* A file from a share, read as it is. */
static HRESULT fetch_share(transfer *work, const WCHAR *remote)
{
	HANDLE input = CreateFileW(remote, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (input == INVALID_HANDLE_VALUE) {
		work->context = BG_ERROR_CONTEXT_REMOTE_FILE;
		return HRESULT_FROM_WIN32(GetLastError());
	}
	LARGE_INTEGER size;
	const UINT64 total = GetFileSizeEx(input, &size) ? (UINT64)size.QuadPart : BG_SIZE_UNKNOWN;
	HRESULT result = arrived(work->owner, work->generation, work->item, total, 0) ? S_OK : STOPPED;
	while (result == S_OK) {
		DWORD count = 0;
		if (!ReadFile(input, work->buffer, CHUNK, &count, NULL)) {
			work->context = BG_ERROR_CONTEXT_REMOTE_FILE;
			result = HRESULT_FROM_WIN32(GetLastError());
			break;
		}
		if (!count)
			break;
		result = write_chunk(work, count, total);
	}
	CloseHandle(input);
	return result;
}

/* One file into a temporary file of its own beside its local name, which the file keeps once it is all there. */
static HRESULT fetch(transfer *work)
{
	settings values;
	EnterCriticalSection(&queue);
	WCHAR *remote = copy_text(work->item->remote), *folder = copy_text(work->item->local);
	work->item->progress.BytesTransferred = 0;
	read_settings(work->owner, &values);
	LeaveCriticalSection(&queue);
	HRESULT result = S_OK;
	WCHAR temporary[MAX_PATH];
	if (!remote || !folder) {
		result = E_OUTOFMEMORY;
		goto done;
	}
	for (int at = lstrlenW(folder); at > 0; --at)
		if (folder[at - 1] == '\\') {
			folder[at] = 0;
			break;
		}
	if (!GetTempFileNameW(folder, L"BIT", 0, temporary)) {
		work->context = BG_ERROR_CONTEXT_LOCAL_FILE;
		result = HRESULT_FROM_WIN32(GetLastError());
		goto done;
	}
	work->output = CreateFileW(temporary, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (work->output == INVALID_HANDLE_VALUE) {
		work->context = BG_ERROR_CONTEXT_LOCAL_FILE;
		result = HRESULT_FROM_WIN32(GetLastError());
		DeleteFileW(temporary);
		goto done;
	}
	result = web_name(remote) ? fetch_web(work, remote, &values) : fetch_share(work, remote);
	CloseHandle(work->output);
	WCHAR *kept = result == S_OK ? copy_text(temporary) : NULL;
	BOOL stored = FALSE;
	if (kept) {
		EnterCriticalSection(&queue);
		if (work->owner->generation == work->generation) {
			FREE(work->item->temporary);
			work->item->temporary = kept;
			work->item->progress.Completed = TRUE;
			if (work->item->progress.BytesTotal == BG_SIZE_UNKNOWN)
				work->item->progress.BytesTotal = work->item->progress.BytesTransferred;
			++work->owner->transferred;
			touch(work->owner);
			stored = TRUE;
		}
		LeaveCriticalSection(&queue);
		if (!stored) {
			FREE(kept);
			result = STOPPED;
		}
	} else if (result == S_OK)
		result = E_OUTOFMEMORY;
	if (!stored)
		DeleteFileW(temporary);
done:
	FREE(remote);
	FREE(folder);
	free_settings(&values);
	return result;
}

/* ---- notifications ---- */
/* The callback the job notifies, with a reference, when its flags ask for the event; NULL otherwise. */
static IUnknown *notified(job *self, ULONG flag)
{
	EnterCriticalSection(&queue);
	IUnknown *callback = (self->notify_flags & flag) && !(self->notify_flags & BG_NOTIFY_DISABLE) ? self->callback : NULL;
	if (callback)
		IUnknown_AddRef(callback);
	LeaveCriticalSection(&queue);
	return callback;
}

/* Without a callback, the job starts the program it names for the event. */
static void run_program(job *self, ULONG flag)
{
	EnterCriticalSection(&queue);
	const BOOL wanted = (self->notify_flags & flag) && !(self->notify_flags & BG_NOTIFY_DISABLE) && !self->callback && self->program;
	WCHAR *program = wanted ? copy_text(self->program) : NULL, *parameters = wanted ? copy_text(self->parameters) : NULL;
	LeaveCriticalSection(&queue);
	if (program) {
		STARTUPINFOW startup;
		PROCESS_INFORMATION process;
		memset(&startup, 0, sizeof startup);
		startup.cb = sizeof startup;
		if (CreateProcessW(program, parameters, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		}
	}
	FREE(program);
	FREE(parameters);
}

static HRESULT make_error(job *self, IBackgroundCopyError **out);

static void notify_transferred(job *self)
{
	IUnknown *unknown = notified(self, BG_NOTIFY_JOB_TRANSFERRED);
	IBackgroundCopyCallback *callback = NULL;
	if (unknown && SUCCEEDED(IUnknown_QueryInterface(unknown, &IID_IBackgroundCopyCallback, (void **)&callback))) {
		IBackgroundCopyCallback_JobTransferred(callback, (IBackgroundCopyJob *)&self->iface);
		IBackgroundCopyCallback_Release(callback);
	}
	if (unknown)
		IUnknown_Release(unknown);
	else
		run_program(self, BG_NOTIFY_JOB_TRANSFERRED);
}

static void notify_error(job *self)
{
	IUnknown *unknown = notified(self, BG_NOTIFY_JOB_ERROR);
	IBackgroundCopyCallback *callback = NULL;
	IBackgroundCopyError *failure = NULL;
	if (unknown && SUCCEEDED(IUnknown_QueryInterface(unknown, &IID_IBackgroundCopyCallback, (void **)&callback))) {
		if (make_error(self, &failure) == S_OK) {
			IBackgroundCopyCallback_JobError(callback, (IBackgroundCopyJob *)&self->iface, failure);
			IBackgroundCopyError_Release(failure);
		}
		IBackgroundCopyCallback_Release(callback);
	}
	if (unknown)
		IUnknown_Release(unknown);
	else
		run_program(self, BG_NOTIFY_JOB_ERROR);
}

static void notify_file(job *self, file *item)
{
	IUnknown *unknown = notified(self, BG_NOTIFY_FILE_TRANSFERRED);
	IBackgroundCopyCallback2 *callback = NULL;
	if (unknown && SUCCEEDED(IUnknown_QueryInterface(unknown, &IID_IBackgroundCopyCallback2, (void **)&callback))) {
		IBackgroundCopyCallback2_FileTransferred(callback, (IBackgroundCopyJob *)&self->iface, (IBackgroundCopyFile *)&item->iface);
		IBackgroundCopyCallback2_Release(callback);
	}
	if (unknown)
		IUnknown_Release(unknown);
}

static void notify_modified(job *self)
{
	IUnknown *unknown = notified(self, BG_NOTIFY_JOB_MODIFICATION);
	IBackgroundCopyCallback *callback = NULL;
	if (unknown && SUCCEEDED(IUnknown_QueryInterface(unknown, &IID_IBackgroundCopyCallback, (void **)&callback))) {
		IBackgroundCopyCallback_JobModification(callback, (IBackgroundCopyJob *)&self->iface, 0);
		IBackgroundCopyCallback_Release(callback);
	}
	if (unknown)
		IUnknown_Release(unknown);
}

/* The thread of one generation of a job: each file not yet transferred, in order, then the job's transferred or error state. */
static DWORD WINAPI run_transfer(void *parameter)
{
	transfer work;
	memset(&work, 0, sizeof work);
	work.owner = parameter;
	job *self = work.owner;
	EnterCriticalSection(&queue);
	work.generation = self->generation;
	LeaveCriticalSection(&queue);
	work.buffer = ALLOC(CHUNK);
	if (advance(self, work.generation, BG_JOB_STATE_CONNECTING))
		notify_modified(self);
	for (ULONG index = 0;; ++index) {
		EnterCriticalSection(&queue);
		const BOOL current = self->generation == work.generation;
		file *item = current && index < self->count ? self->files[index] : NULL;
		if (item)
			IBackgroundCopyFile2_AddRef(&item->iface);
		const BOOL done = item && item->progress.Completed;
		LeaveCriticalSection(&queue);
		if (!current)
			break;
		if (!item) {
			EnterCriticalSection(&queue);
			const BOOL ended = self->generation == work.generation;
			if (ended) {
				self->state = BG_JOB_STATE_TRANSFERRED;
				GetSystemTimeAsFileTime(&self->times.TransferCompletionTime);
				self->times.ModificationTime = self->times.TransferCompletionTime;
			}
			LeaveCriticalSection(&queue);
			if (ended)
				notify_transferred(self);
			break;
		}
		if (done) {
			IBackgroundCopyFile2_Release(&item->iface);
			continue;
		}
		work.item = item;
		work.context = BG_ERROR_CONTEXT_NONE;
		const HRESULT result = work.buffer ? fetch(&work) : E_OUTOFMEMORY;
		if (result == S_OK) {
			notify_file(self, item);
			IBackgroundCopyFile2_Release(&item->iface);
			continue;
		}
		if (result != STOPPED) {
			EnterCriticalSection(&queue);
			const BOOL failed = self->generation == work.generation;
			if (failed) {
				self->state = BG_JOB_STATE_ERROR;
				self->code = result;
				self->context = work.context ? work.context : BG_ERROR_CONTEXT_GENERAL_QUEUE_MANAGER;
				if (self->failed)
					IBackgroundCopyFile2_Release(&self->failed->iface);
				self->failed = item;
				IBackgroundCopyFile2_AddRef(&item->iface);
				++self->errors;
				touch(self);
			}
			LeaveCriticalSection(&queue);
			if (failed)
				notify_error(self);
		}
		IBackgroundCopyFile2_Release(&item->iface);
		break;
	}
	FREE(work.buffer);
	IBackgroundCopyJob4_Release(&self->iface);
	return 0;
}

/* ---- job methods ---- */
/* Ends the current generation: a transfer in flight leaves the job and deletes what it wrote. */
static void stop_transfer(job *self)
{
	++self->generation;
	for (ULONG index = 0; index < self->count; ++index)
		if (!self->files[index]->progress.Completed)
			self->files[index]->progress.BytesTransferred = 0;
}

static HRESULT WINAPI job_suspend(IBackgroundCopyJob4 *iface)
{
	job *self = job_of(iface);
	EnterCriticalSection(&queue);
	HRESULT result = S_OK;
	if (finished(self->state))
		result = BG_E_INVALID_STATE;
	else if (self->state != BG_JOB_STATE_SUSPENDED) {
		if (self->state != BG_JOB_STATE_TRANSFERRED)
			stop_transfer(self);
		self->state = BG_JOB_STATE_SUSPENDED;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT WINAPI job_resume(IBackgroundCopyJob4 *iface)
{
	job *self = job_of(iface);
	EnterCriticalSection(&queue);
	HRESULT result = S_OK;
	BOOL start = FALSE;
	if (finished(self->state))
		result = BG_E_INVALID_STATE;
	else if (!self->count)
		result = BG_E_EMPTY;
	else if (self->transferred == self->count) {
		if (self->state != BG_JOB_STATE_TRANSFERRED) {
			self->state = BG_JOB_STATE_TRANSFERRED;
			touch(self);
		}
	} else if (self->state == BG_JOB_STATE_SUSPENDED || self->state == BG_JOB_STATE_ERROR || self->state == BG_JOB_STATE_TRANSIENT_ERROR ||
		self->state == BG_JOB_STATE_TRANSFERRED) {
		stop_transfer(self);
		self->state = BG_JOB_STATE_QUEUED;
		touch(self);
		start = TRUE;
	}
	LeaveCriticalSection(&queue);
	if (start) {
		IBackgroundCopyJob4_AddRef(iface);
		HANDLE thread = CreateThread(NULL, 0, run_transfer, self, 0, NULL);
		if (thread)
			CloseHandle(thread);
		else {
			IBackgroundCopyJob4_Release(iface);
			EnterCriticalSection(&queue);
			self->state = BG_JOB_STATE_SUSPENDED;
			LeaveCriticalSection(&queue);
			result = HRESULT_FROM_WIN32(GetLastError());
		}
	}
	return result;
}

static void forget(job *self)
{
	for (job **link = &jobs_list; *link; link = &(*link)->next)
		if (*link == self) {
			*link = self->next;
			self->next = NULL;
			return;
		}
}

static HRESULT WINAPI job_cancel(IBackgroundCopyJob4 *iface)
{
	job *self = job_of(iface);
	EnterCriticalSection(&queue);
	if (finished(self->state)) {
		LeaveCriticalSection(&queue);
		return BG_E_INVALID_STATE;
	}
	stop_transfer(self);
	self->state = BG_JOB_STATE_CANCELLED;
	touch(self);
	for (ULONG index = 0; index < self->count; ++index)
		if (self->files[index]->temporary) {
			DeleteFileW(self->files[index]->temporary);
			FREE(self->files[index]->temporary);
			self->files[index]->temporary = NULL;
		}
	forget(self);
	LeaveCriticalSection(&queue);
	IBackgroundCopyJob4_Release(iface);
	return S_OK;
}

/* The files that arrived take their local names; those that did not are given up, which the answer tells. */
static HRESULT WINAPI job_complete(IBackgroundCopyJob4 *iface)
{
	job *self = job_of(iface);
	EnterCriticalSection(&queue);
	if (finished(self->state)) {
		LeaveCriticalSection(&queue);
		return BG_E_INVALID_STATE;
	}
	stop_transfer(self);
	HRESULT result = S_OK;
	BOOL partial = FALSE;
	for (ULONG index = 0; index < self->count; ++index) {
		file *item = self->files[index];
		if (!item->temporary) {
			partial = TRUE;
			continue;
		}
		if (!MoveFileExW(item->temporary, item->local, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) && result == S_OK)
			result = HRESULT_FROM_WIN32(GetLastError());
		FREE(item->temporary);
		item->temporary = NULL;
	}
	self->state = BG_JOB_STATE_ACKNOWLEDGED;
	touch(self);
	forget(self);
	LeaveCriticalSection(&queue);
	IBackgroundCopyJob4_Release(iface);
	return result == S_OK && partial ? BG_S_PARTIAL_COMPLETE : result;
}

static HRESULT WINAPI job_id(IBackgroundCopyJob4 *iface, GUID *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = job_of(iface)->id;
	return S_OK;
}

static HRESULT WINAPI job_type(IBackgroundCopyJob4 *iface, BG_JOB_TYPE *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = BG_JOB_TYPE_DOWNLOAD;
	return S_OK;
}

/* The job's bytes and files; a total stays unknown while any file's size is. */
static HRESULT WINAPI job_progress(IBackgroundCopyJob4 *iface, BG_JOB_PROGRESS *out)
{
	job *self = job_of(iface);
	if (!out)
		return E_INVALIDARG;
	memset(out, 0, sizeof *out);
	EnterCriticalSection(&queue);
	out->FilesTotal = self->count;
	out->FilesTransferred = self->transferred;
	for (ULONG index = 0; index < self->count; ++index) {
		const BG_FILE_PROGRESS *progress = &self->files[index]->progress;
		out->BytesTransferred += progress->BytesTransferred;
		if (out->BytesTotal != BG_SIZE_UNKNOWN)
			out->BytesTotal = progress->BytesTotal == BG_SIZE_UNKNOWN ? BG_SIZE_UNKNOWN : out->BytesTotal + progress->BytesTotal;
	}
	LeaveCriticalSection(&queue);
	return S_OK;
}

static HRESULT WINAPI job_times(IBackgroundCopyJob4 *iface, BG_JOB_TIMES *out)
{
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&queue);
	*out = job_of(iface)->times;
	LeaveCriticalSection(&queue);
	return S_OK;
}

static HRESULT WINAPI job_state(IBackgroundCopyJob4 *iface, BG_JOB_STATE *out)
{
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&queue);
	*out = job_of(iface)->state;
	LeaveCriticalSection(&queue);
	return S_OK;
}

static HRESULT make_error(job *self, IBackgroundCopyError **out)
{
	EnterCriticalSection(&queue);
	HRESULT result = S_OK;
	if (self->state != BG_JOB_STATE_ERROR && self->state != BG_JOB_STATE_TRANSIENT_ERROR)
		result = BG_E_ERROR_INFORMATION_UNAVAILABLE;
	error *made = result == S_OK ? ALLOC(sizeof *made) : NULL;
	if (result == S_OK && !made)
		result = E_OUTOFMEMORY;
	if (made) {
		made->iface.lpVtbl = &error_table;
		made->refs = 1;
		made->code = self->code;
		made->context = self->context;
		if (self->failed) {
			made->file = &self->failed->iface;
			IBackgroundCopyFile2_AddRef(made->file);
		}
		InterlockedIncrement(&objects);
		*out = &made->iface;
	}
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT WINAPI job_error(IBackgroundCopyJob4 *iface, IBackgroundCopyError **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return make_error(job_of(iface), out);
}

/* The owner is the account the program runs as, by its security identifier. */
static HRESULT WINAPI job_owner(IBackgroundCopyJob4 *iface, LPWSTR *out)
{
	HANDLE token = NULL;
	BYTE buffer[256];
	DWORD size = 0;
	WCHAR *text = NULL;
	if (!out)
		return E_INVALIDARG;
	HRESULT result = E_FAIL;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		if (GetTokenInformation(token, TokenUser, buffer, sizeof buffer, &size) && ConvertSidToStringSidW(((TOKEN_USER *)buffer)->User.Sid, &text)) {
			result = hand_text(text, out);
			LocalFree(text);
		}
		CloseHandle(token);
	}
	return result == S_OK ? S_OK : HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_NO_TOKEN);
}

static HRESULT set_text(WCHAR **field, const WCHAR *value, int limit, job *self)
{
	if (!value)
		return E_INVALIDARG;
	if (limit && lstrlenW(value) > limit)
		return BG_E_STRING_TOO_LONG;
	WCHAR *copy = copy_text(value);
	if (!copy)
		return E_OUTOFMEMORY;
	EnterCriticalSection(&queue);
	HRESULT result = S_OK;
	if (finished(self->state)) {
		result = BG_E_INVALID_STATE;
		FREE(copy);
	} else {
		FREE(*field);
		*field = copy;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT get_text(WCHAR *const *field, LPWSTR *out)
{
	EnterCriticalSection(&queue);
	const HRESULT result = hand_text(*field, out);
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT WINAPI job_set_name(IBackgroundCopyJob4 *iface, LPCWSTR value) { return set_text(&job_of(iface)->name, value, 256, job_of(iface)); }
static HRESULT WINAPI job_name(IBackgroundCopyJob4 *iface, LPWSTR *out) { return get_text(&job_of(iface)->name, out); }

static HRESULT WINAPI job_set_description(IBackgroundCopyJob4 *iface, LPCWSTR value)
{
	return set_text(&job_of(iface)->description, value, 1024, job_of(iface));
}

static HRESULT WINAPI job_description(IBackgroundCopyJob4 *iface, LPWSTR *out) { return get_text(&job_of(iface)->description, out); }

/* A setting of the job, kept while it is not finished. */
static HRESULT set_value(job *self, ULONG *field, ULONG value)
{
	EnterCriticalSection(&queue);
	const HRESULT result = finished(self->state) ? BG_E_INVALID_STATE : S_OK;
	if (result == S_OK) {
		*field = value;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT get_value(const ULONG *field, ULONG *out)
{
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&queue);
	*out = *field;
	LeaveCriticalSection(&queue);
	return S_OK;
}

static HRESULT WINAPI job_set_priority(IBackgroundCopyJob4 *iface, BG_JOB_PRIORITY value)
{
	if ((ULONG)value > BG_JOB_PRIORITY_LOW)
		return E_INVALIDARG;
	return set_value(job_of(iface), (ULONG *)&job_of(iface)->priority, (ULONG)value);
}

static HRESULT WINAPI job_priority(IBackgroundCopyJob4 *iface, BG_JOB_PRIORITY *out) { return get_value((ULONG *)&job_of(iface)->priority, (ULONG *)out); }

static HRESULT WINAPI job_set_notify_flags(IBackgroundCopyJob4 *iface, ULONG value)
{
	if (value & ~(ULONG)(BG_NOTIFY_JOB_TRANSFERRED | BG_NOTIFY_JOB_ERROR | BG_NOTIFY_DISABLE | BG_NOTIFY_JOB_MODIFICATION | BG_NOTIFY_FILE_TRANSFERRED))
		return E_NOTIMPL;
	return set_value(job_of(iface), &job_of(iface)->notify_flags, value);
}

static HRESULT WINAPI job_notify_flags(IBackgroundCopyJob4 *iface, ULONG *out) { return get_value(&job_of(iface)->notify_flags, out); }

static HRESULT WINAPI job_set_notify_interface(IBackgroundCopyJob4 *iface, IUnknown *value)
{
	job *self = job_of(iface);
	if (value)
		IUnknown_AddRef(value);
	EnterCriticalSection(&queue);
	IUnknown *old = finished(self->state) ? value : self->callback;
	const HRESULT result = finished(self->state) ? BG_E_INVALID_STATE : S_OK;
	if (result == S_OK) {
		self->callback = value;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	if (old)
		IUnknown_Release(old);
	return result;
}

static HRESULT WINAPI job_notify_interface(IBackgroundCopyJob4 *iface, IUnknown **out)
{
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&queue);
	*out = job_of(iface)->callback;
	if (*out)
		IUnknown_AddRef(*out);
	LeaveCriticalSection(&queue);
	return S_OK;
}

static HRESULT WINAPI job_set_retry_delay(IBackgroundCopyJob4 *iface, ULONG seconds)
{
	return set_value(job_of(iface), &job_of(iface)->retry_delay, seconds < 5 ? 5 : seconds);
}

static HRESULT WINAPI job_retry_delay(IBackgroundCopyJob4 *iface, ULONG *out) { return get_value(&job_of(iface)->retry_delay, out); }

static HRESULT WINAPI job_set_no_progress_timeout(IBackgroundCopyJob4 *iface, ULONG seconds)
{
	return set_value(job_of(iface), &job_of(iface)->no_progress_timeout, seconds);
}

static HRESULT WINAPI job_no_progress_timeout(IBackgroundCopyJob4 *iface, ULONG *out) { return get_value(&job_of(iface)->no_progress_timeout, out); }
static HRESULT WINAPI job_error_count(IBackgroundCopyJob4 *iface, ULONG *out) { return get_value(&job_of(iface)->errors, out); }

static HRESULT WINAPI job_set_proxy(IBackgroundCopyJob4 *iface, BG_JOB_PROXY_USAGE usage, const WCHAR *list, const WCHAR *bypass)
{
	job *self = job_of(iface);
	if ((ULONG)usage > BG_JOB_PROXY_USAGE_AUTODETECT || (usage == BG_JOB_PROXY_USAGE_OVERRIDE && (!list || !*list)) ||
		(usage != BG_JOB_PROXY_USAGE_OVERRIDE && (list || bypass)))
		return E_INVALIDARG;
	if (list && lstrlenW(list) > 32767)
		return BG_E_PROXY_LIST_TOO_LARGE;
	if (bypass && lstrlenW(bypass) > 32767)
		return BG_E_PROXY_BYPASS_LIST_TOO_LARGE;
	WCHAR *list_copy = copy_text(list), *bypass_copy = copy_text(bypass);
	EnterCriticalSection(&queue);
	const HRESULT result = finished(self->state) ? BG_E_INVALID_STATE : S_OK;
	if (result == S_OK) {
		FREE(self->proxies);
		FREE(self->bypass);
		self->proxies = list_copy;
		self->bypass = bypass_copy;
		self->proxy_usage = usage;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	if (result != S_OK) {
		FREE(list_copy);
		FREE(bypass_copy);
	}
	return result;
}

static HRESULT WINAPI job_proxy(IBackgroundCopyJob4 *iface, BG_JOB_PROXY_USAGE *usage, LPWSTR *list, LPWSTR *bypass)
{
	job *self = job_of(iface);
	if (!usage || !list || !bypass)
		return E_INVALIDARG;
	EnterCriticalSection(&queue);
	*usage = self->proxy_usage;
	*list = *bypass = NULL;
	HRESULT result = S_OK;
	if (self->proxies && (result = hand_text(self->proxies, list)) == S_OK && self->bypass && (result = hand_text(self->bypass, bypass)) != S_OK) {
		CoTaskMemFree(*list);
		*list = NULL;
	}
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT WINAPI job_take_ownership(IBackgroundCopyJob4 *iface) { return S_OK; }

static HRESULT WINAPI job_set_command(IBackgroundCopyJob4 *iface, LPCWSTR program, LPCWSTR parameters)
{
	job *self = job_of(iface);
	if (parameters && !program)
		return E_INVALIDARG;
	WCHAR *program_copy = copy_text(program), *parameters_copy = copy_text(parameters);
	EnterCriticalSection(&queue);
	const HRESULT result = finished(self->state) ? BG_E_INVALID_STATE : S_OK;
	if (result == S_OK) {
		FREE(self->program);
		FREE(self->parameters);
		self->program = program_copy;
		self->parameters = parameters_copy;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	if (result != S_OK) {
		FREE(program_copy);
		FREE(parameters_copy);
	}
	return result;
}

static HRESULT WINAPI job_command(IBackgroundCopyJob4 *iface, LPWSTR *program, LPWSTR *parameters)
{
	job *self = job_of(iface);
	if (!program || !parameters)
		return E_INVALIDARG;
	*program = *parameters = NULL;
	EnterCriticalSection(&queue);
	HRESULT result = S_OK;
	if (self->program && (result = hand_text(self->program, program)) == S_OK && self->parameters &&
		(result = hand_text(self->parameters, parameters)) != S_OK) {
		CoTaskMemFree(*program);
		*program = NULL;
	}
	LeaveCriticalSection(&queue);
	return result;
}

/* A download job has no reply, which only upload jobs that ask for one carry. */
static HRESULT WINAPI job_reply_progress(IBackgroundCopyJob4 *iface, BG_JOB_REPLY_PROGRESS *out) { return BG_E_INVALID_STATE; }
static HRESULT WINAPI job_reply_data(IBackgroundCopyJob4 *iface, byte **buffer, UINT64 *length) { return BG_E_INVALID_STATE; }
static HRESULT WINAPI job_set_reply_name(IBackgroundCopyJob4 *iface, LPCWSTR name) { return BG_E_INVALID_STATE; }
static HRESULT WINAPI job_reply_name(IBackgroundCopyJob4 *iface, LPWSTR *name) { return BG_E_INVALID_STATE; }

static HRESULT WINAPI job_set_credentials(IBackgroundCopyJob4 *iface, BG_AUTH_CREDENTIALS *credentials)
{
	job *self = job_of(iface);
	if (!credentials)
		return E_INVALIDARG;
	if (credentials->Target != BG_AUTH_TARGET_SERVER && credentials->Target != BG_AUTH_TARGET_PROXY)
		return BG_E_INVALID_AUTH_TARGET;
	if (!winhttp_scheme(credentials->Scheme))
		return BG_E_INVALID_AUTH_SCHEME;
	const WCHAR *user = credentials->Credentials.Basic.UserName, *password = credentials->Credentials.Basic.Password;
	if (user && lstrlenW(user) > 300)
		return BG_E_USERNAME_TOO_LARGE;
	if (password && lstrlenW(password) > 65536)
		return BG_E_PASSWORD_TOO_LARGE;
	const int target = credentials->Target == BG_AUTH_TARGET_PROXY;
	WCHAR *user_copy = copy_text(user ? user : L""), *password_copy = copy_text(password ? password : L"");
	EnterCriticalSection(&queue);
	FREE(self->users[target]);
	FREE(self->passwords[target]);
	self->users[target] = user_copy;
	self->passwords[target] = password_copy;
	self->schemes[target] = credentials->Scheme;
	touch(self);
	LeaveCriticalSection(&queue);
	return S_OK;
}

static HRESULT WINAPI job_remove_credentials(IBackgroundCopyJob4 *iface, BG_AUTH_TARGET target, BG_AUTH_SCHEME scheme)
{
	job *self = job_of(iface);
	if (target != BG_AUTH_TARGET_SERVER && target != BG_AUTH_TARGET_PROXY)
		return BG_E_INVALID_AUTH_TARGET;
	const int index = target == BG_AUTH_TARGET_PROXY;
	EnterCriticalSection(&queue);
	const HRESULT result = self->users[index] && self->schemes[index] == scheme ? S_OK : S_FALSE;
	if (result == S_OK) {
		FREE(self->users[index]);
		FREE(self->passwords[index]);
		self->users[index] = self->passwords[index] = NULL;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	return result;
}

/* Every remote name that starts with the old prefix starts with the new one instead. */
static HRESULT WINAPI job_replace_prefix(IBackgroundCopyJob4 *iface, LPCWSTR old_prefix, LPCWSTR new_prefix)
{
	job *self = job_of(iface);
	if (!old_prefix || !new_prefix || !*old_prefix || !*new_prefix)
		return E_INVALIDARG;
	const int old_length = lstrlenW(old_prefix), new_length = lstrlenW(new_prefix);
	EnterCriticalSection(&queue);
	HRESULT result = finished(self->state) ? BG_E_INVALID_STATE : S_OK;
	for (ULONG index = 0; result == S_OK && index < self->count; ++index) {
		file *item = self->files[index];
		if (!starts_with(item->remote, old_prefix))
			continue;
		const int rest = lstrlenW(item->remote) - old_length;
		WCHAR *name = ALLOC((new_length + rest + 1) * sizeof(WCHAR));
		if (!name) {
			result = E_OUTOFMEMORY;
			break;
		}
		memcpy(name, new_prefix, new_length * sizeof(WCHAR));
		memcpy(name + new_length, item->remote + old_length, (rest + 1) * sizeof(WCHAR));
		FREE(item->remote);
		item->remote = name;
	}
	if (result == S_OK)
		touch(self);
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT WINAPI job_add_ranges(IBackgroundCopyJob4 *iface, LPCWSTR remote, LPCWSTR local, DWORD count, BG_FILE_RANGE ranges[])
{
	return unimplemented("IBackgroundCopyJob3::AddFileWithRanges");
}

static HRESULT WINAPI job_set_acl_flags(IBackgroundCopyJob4 *iface, DWORD flags) { return set_value(job_of(iface), &job_of(iface)->acl_flags, flags); }
static HRESULT WINAPI job_acl_flags(IBackgroundCopyJob4 *iface, DWORD *out) { return get_value(&job_of(iface)->acl_flags, out); }
static HRESULT WINAPI job_set_peer_flags(IBackgroundCopyJob4 *iface, DWORD flags) { return set_value(job_of(iface), &job_of(iface)->peer_flags, flags); }
static HRESULT WINAPI job_peer_flags(IBackgroundCopyJob4 *iface, DWORD *out) { return get_value(&job_of(iface)->peer_flags, out); }

/* The program runs at medium integrity, as an account that is not elevated. */
static HRESULT WINAPI job_integrity(IBackgroundCopyJob4 *iface, ULONG *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = SECURITY_MANDATORY_MEDIUM_RID;
	return S_OK;
}

static HRESULT WINAPI job_elevated(IBackgroundCopyJob4 *iface, WINBOOL *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = FALSE;
	return S_OK;
}

static HRESULT WINAPI job_set_maximum_time(IBackgroundCopyJob4 *iface, ULONG seconds)
{
	return set_value(job_of(iface), &job_of(iface)->maximum_time, seconds);
}

static HRESULT WINAPI job_maximum_time(IBackgroundCopyJob4 *iface, ULONG *out) { return get_value(&job_of(iface)->maximum_time, out); }

static IBackgroundCopyJob4Vtbl job_table = {job_query, job_reference, job_release, job_add_file_set, job_add_file, job_enum_files, job_suspend,
	job_resume, job_cancel, job_complete, job_id, job_type, job_progress, job_times, job_state, job_error, job_owner, job_set_name, job_name,
	job_set_description, job_description, job_set_priority, job_priority, job_set_notify_flags, job_notify_flags, job_set_notify_interface,
	job_notify_interface, job_set_retry_delay, job_retry_delay, job_set_no_progress_timeout, job_no_progress_timeout, job_error_count,
	job_set_proxy, job_proxy, job_take_ownership, job_set_command, job_command, job_reply_progress, job_reply_data, job_set_reply_name,
	job_reply_name, job_set_credentials, job_remove_credentials, job_replace_prefix, job_add_ranges, job_set_acl_flags, job_acl_flags,
	job_set_peer_flags, job_peer_flags, job_integrity, job_elevated, job_set_maximum_time, job_maximum_time};

/* ---- HTTP options ---- */
static job *job_of_http(IBackgroundCopyJobHttpOptions *iface) { return (job *)((BYTE *)iface - offsetof(job, http)); }

static HRESULT WINAPI http_query(IBackgroundCopyJobHttpOptions *iface, REFIID iid, void **out)
{
	return job_query(&job_of_http(iface)->iface, iid, out);
}

static ULONG WINAPI http_reference(IBackgroundCopyJobHttpOptions *iface) { return job_reference(&job_of_http(iface)->iface); }
static ULONG WINAPI http_release(IBackgroundCopyJobHttpOptions *iface) { return job_release(&job_of_http(iface)->iface); }

static HRESULT WINAPI http_certificate_id(IBackgroundCopyJobHttpOptions *iface, BG_CERT_STORE_LOCATION location, LPCWSTR store, BYTE *hash)
{
	return unimplemented("IBackgroundCopyJobHttpOptions::SetClientCertificateByID");
}

static HRESULT WINAPI http_certificate_name(IBackgroundCopyJobHttpOptions *iface, BG_CERT_STORE_LOCATION location, LPCWSTR store, LPCWSTR subject)
{
	return unimplemented("IBackgroundCopyJobHttpOptions::SetClientCertificateByName");
}

/* No job carries a client certificate. */
static HRESULT WINAPI http_remove_certificate(IBackgroundCopyJobHttpOptions *iface) { return S_FALSE; }

static HRESULT WINAPI http_certificate(IBackgroundCopyJobHttpOptions *iface, BG_CERT_STORE_LOCATION *location, LPWSTR *store, BYTE **hash,
	LPWSTR *subject)
{
	if (!location || !store || !hash || !subject)
		return E_INVALIDARG;
	*store = *subject = NULL;
	*hash = NULL;
	return HRESULT_FROM_WIN32(RPC_X_BAD_STUB_DATA);
}

/* Headers sent with every request, as lines that each end with a line break; NULL takes them away. */
static HRESULT WINAPI http_set_headers(IBackgroundCopyJobHttpOptions *iface, LPCWSTR headers)
{
	job *self = job_of_http(iface);
	if (headers && lstrlenW(headers) > 16384)
		return BG_E_STRING_TOO_LONG;
	WCHAR *copy = headers && *headers ? copy_text(headers) : NULL;
	if (headers && *headers && !copy)
		return E_OUTOFMEMORY;
	EnterCriticalSection(&queue);
	const HRESULT result = finished(self->state) ? BG_E_INVALID_STATE : S_OK;
	if (result == S_OK) {
		FREE(self->headers);
		self->headers = copy;
		touch(self);
	}
	LeaveCriticalSection(&queue);
	if (result != S_OK)
		FREE(copy);
	return result;
}

static HRESULT WINAPI http_headers(IBackgroundCopyJobHttpOptions *iface, LPWSTR *out)
{
	job *self = job_of_http(iface);
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	EnterCriticalSection(&queue);
	const HRESULT result = self->headers ? hand_text(self->headers, out) : S_FALSE;
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT WINAPI http_set_flags(IBackgroundCopyJobHttpOptions *iface, ULONG flags)
{
	return set_value(job_of_http(iface), &job_of_http(iface)->security_flags, flags);
}

static HRESULT WINAPI http_flags(IBackgroundCopyJobHttpOptions *iface, ULONG *out) { return get_value(&job_of_http(iface)->security_flags, out); }

static IBackgroundCopyJobHttpOptionsVtbl http_table = {http_query, http_reference, http_release, http_certificate_id, http_certificate_name,
	http_remove_certificate, http_certificate, http_set_headers, http_headers, http_set_flags, http_flags};

/* ---- the manager ---- */
static HRESULT WINAPI manager_query(IBackgroundCopyManager *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IBackgroundCopyManager)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI manager_reference(IBackgroundCopyManager *iface) { return 2; }
static ULONG WINAPI manager_release(IBackgroundCopyManager *iface) { return 1; }

/* A new download job starts suspended, with no files, and stays in the queue until it is completed or cancelled. */
static HRESULT WINAPI manager_create(IBackgroundCopyManager *iface, LPCWSTR name, BG_JOB_TYPE type, GUID *id, IBackgroundCopyJob **out)
{
	if (!name || !id || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (type == BG_JOB_TYPE_UPLOAD || type == BG_JOB_TYPE_UPLOAD_REPLY)
		return unimplemented("IBackgroundCopyManager::CreateJob for an upload");
	if (type != BG_JOB_TYPE_DOWNLOAD)
		return E_INVALIDARG;
	if (lstrlenW(name) > 256)
		return BG_E_STRING_TOO_LONG;
	job *self = ALLOC(sizeof *self);
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &job_table;
	self->http.lpVtbl = &http_table;
	self->refs = 2; /* the caller's and the queue's */
	self->name = copy_text(name);
	self->description = copy_text(L"");
	if (!self->name || !self->description || FAILED(CoCreateGuid(&self->id))) {
		FREE(self->name);
		FREE(self->description);
		FREE(self);
		return E_OUTOFMEMORY;
	}
	self->state = BG_JOB_STATE_SUSPENDED;
	self->priority = BG_JOB_PRIORITY_NORMAL;
	self->notify_flags = BG_NOTIFY_JOB_TRANSFERRED | BG_NOTIFY_JOB_ERROR;
	self->retry_delay = 600;
	self->no_progress_timeout = 1209600;
	self->maximum_time = 7776000;
	self->proxy_usage = BG_JOB_PROXY_USAGE_PRECONFIG;
	GetSystemTimeAsFileTime(&self->times.CreationTime);
	self->times.ModificationTime = self->times.CreationTime;
	InterlockedIncrement(&objects);
	EnterCriticalSection(&queue);
	self->next = jobs_list;
	jobs_list = self;
	LeaveCriticalSection(&queue);
	*id = self->id;
	*out = (IBackgroundCopyJob *)&self->iface;
	return S_OK;
}

static HRESULT WINAPI manager_job(IBackgroundCopyManager *iface, REFGUID id, IBackgroundCopyJob **out)
{
	if (!id || !out)
		return E_INVALIDARG;
	*out = NULL;
	EnterCriticalSection(&queue);
	for (job *self = jobs_list; self; self = self->next)
		if (same(&self->id, id)) {
			IBackgroundCopyJob4_AddRef(&self->iface);
			*out = (IBackgroundCopyJob *)&self->iface;
			break;
		}
	LeaveCriticalSection(&queue);
	return *out ? S_OK : BG_E_NOT_FOUND;
}

/* Every job of the queue, which are all the caller's own; asking for every user's takes nothing away. */
static HRESULT WINAPI manager_enum(IBackgroundCopyManager *iface, DWORD flags, IEnumBackgroundCopyJobs **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (flags & ~(DWORD)BG_JOB_ENUM_ALL_USERS)
		return E_INVALIDARG;
	EnterCriticalSection(&queue);
	ULONG count = 0;
	for (job *self = jobs_list; self; self = self->next)
		++count;
	IBackgroundCopyJob **items = ALLOC((count ? count : 1) * sizeof *items);
	HRESULT result = E_OUTOFMEMORY;
	if (items) {
		ULONG index = 0;
		for (job *self = jobs_list; self; self = self->next)
			items[index++] = (IBackgroundCopyJob *)&self->iface;
		result = jobs_enumerate(items, count, 0, out);
		FREE(items);
	}
	LeaveCriticalSection(&queue);
	return result;
}

static HRESULT WINAPI manager_describe(IBackgroundCopyManager *iface, HRESULT code, DWORD language, LPWSTR *out)
{
	return describe(code, language, out);
}

static IBackgroundCopyManagerVtbl manager_table = {manager_query, manager_reference, manager_release, manager_create, manager_job, manager_enum,
	manager_describe};
static IBackgroundCopyManager manager = {&manager_table};

/* ---- the class ---- */
static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same(iid, &IID_IUnknown) || same(iid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_reference(IClassFactory *iface) { return 2; }
static ULONG WINAPI factory_release(IClassFactory *iface) { return 1; }

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return manager_query(&manager, iid, out);
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL lock) { return S_OK; }

static IClassFactoryVtbl factory_table = {factory_query, factory_reference, factory_release, factory_create, factory_lock};
static IClassFactory factory = {&factory_table};

/* Each version of the service has a class of its own, and every one of them is the same manager. */
static const CLSID *const classes[] = {&CLSID_BackgroundCopyManager, &CLSID_BackgroundCopyManager1_5, &CLSID_BackgroundCopyManager2_0,
	&CLSID_BackgroundCopyManager2_5, &CLSID_BackgroundCopyManager3_0};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	for (unsigned index = 0; index < sizeof classes / sizeof *classes; ++index)
		if (same(clsid, classes[index]))
			return factory_query(&factory, iid, out);
	return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
		InitializeCriticalSection(&queue);
	}
	return TRUE;
}
