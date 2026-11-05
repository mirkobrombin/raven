#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <patchapi.h>

/* The exports are looked up by name, as installers that carry delta files do. */
static BOOL (WINAPI *apply_w)(LPCWSTR, LPCWSTR, LPCWSTR, ULONG);
static BOOL (WINAPI *apply_a)(LPCSTR, LPCSTR, LPCSTR, ULONG);
static BOOL (WINAPI *apply_ex_w)(LPCWSTR, LPCWSTR, LPCWSTR, ULONG, PPATCH_PROGRESS_CALLBACK, PVOID);
static BOOL (WINAPI *apply_handles)(HANDLE, HANDLE, HANDLE, ULONG);
static BOOL (WINAPI *apply_buffers)(PBYTE, ULONG, PBYTE, ULONG, PBYTE *, ULONG, ULONG *, FILETIME *, ULONG, PPATCH_PROGRESS_CALLBACK, PVOID);
static BOOL (WINAPI *test_w)(LPCWSTR, LPCWSTR, ULONG);
static BOOL (WINAPI *test_buffers)(PBYTE, ULONG, PBYTE, ULONG, ULONG *, ULONG);
static BOOL (WINAPI *signature_w)(LPCWSTR, ULONG, PVOID, ULONG, PPATCH_IGNORE_RANGE, ULONG, PPATCH_RETAIN_RANGE, ULONG, PVOID);

static WCHAR directory[MAX_PATH];
static WCHAR path_buffer[4][MAX_PATH];
static char narrow_buffer[3][MAX_PATH];

/* A file beside the client, which mspatcha_fixtures.py wrote; slot keeps up to four names alive at once. */
static const WCHAR *at(int slot, const WCHAR *name)
{
	lstrcpyW(path_buffer[slot], directory);
	lstrcatW(path_buffer[slot], name);
	return path_buffer[slot];
}

static const char *narrow(int slot, const WCHAR *name)
{
	WideCharToMultiByte(CP_ACP, 0, at(3, name), -1, narrow_buffer[slot], MAX_PATH, NULL, NULL);
	return narrow_buffer[slot];
}

static HANDLE open_read(const WCHAR *name)
{
	return CreateFileW(at(3, name), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* The whole of a file, from VirtualAlloc; NULL when it cannot be read. */
static BYTE *load(const WCHAR *name, ULONG *size)
{
	HANDLE file = open_read(name);
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	*size = GetFileSize(file, NULL);
	BYTE *data = VirtualAlloc(NULL, *size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	DWORD read = 0;
	BOOL done = data && ReadFile(file, data, *size, &read, NULL) && read == *size;
	CloseHandle(file);
	return done ? data : NULL;
}

static BOOL same_bytes(const BYTE *left, const BYTE *right, ULONG size)
{
	for (ULONG index = 0; index < size; index++)
		if (left[index] != right[index])
			return FALSE;
	return TRUE;
}

static BOOL same_files(const WCHAR *made, const WCHAR *wanted)
{
	ULONG made_size = 0, wanted_size = 0;
	BYTE *left = load(made, &made_size), *right = load(wanted, &wanted_size);
	BOOL same = left && right && made_size == wanted_size && same_bytes(left, right, made_size);
	if (left)
		VirtualFree(left, 0, MEM_RELEASE);
	if (right)
		VirtualFree(right, 0, MEM_RELEASE);
	return same;
}

static BOOL exists(const WCHAR *name)
{
	return GetFileAttributesW(at(3, name)) != INVALID_FILE_ATTRIBUTES;
}

/* A failure with the error it must leave behind. */
static BOOL fails_with(BOOL result, DWORD error)
{
	return !result && GetLastError() == error;
}

static unsigned progress_calls;

static BOOL CALLBACK stop_at_once(PVOID context, ULONG done, ULONG total)
{
	(void)context;
	(void)done;
	(void)total;
	progress_calls++;
	return FALSE;
}

/* mspatcha.dll through its exports: patches made from the old files and patches made from nothing, both kinds of block, chunks crossed,
 * x86 calls translated, two old files with ignored and retained ranges, a patch between identical files, one that is only a header,
 * and patches that are damaged or meant for another file. */
int raven_entry(void)
{
	GetModuleFileNameW(NULL, directory, MAX_PATH);
	for (WCHAR *end = directory + lstrlenW(directory); end > directory && end[-1] != '\\'; --end)
		end[-1] = 0;
	HMODULE library = LoadLibraryW(L"mspatcha.dll");
	if (!library) return 1;
	apply_w = (void *)GetProcAddress(library, "ApplyPatchToFileW");
	apply_a = (void *)GetProcAddress(library, "ApplyPatchToFileA");
	apply_ex_w = (void *)GetProcAddress(library, "ApplyPatchToFileExW");
	apply_handles = (void *)GetProcAddress(library, "ApplyPatchToFileByHandles");
	apply_buffers = (void *)GetProcAddress(library, "ApplyPatchToFileByBuffers");
	test_w = (void *)GetProcAddress(library, "TestApplyPatchToFileW");
	test_buffers = (void *)GetProcAddress(library, "TestApplyPatchToFileByBuffers");
	signature_w = (void *)GetProcAddress(library, "GetFilePatchSignatureW");
	if (!apply_w || !apply_a || !apply_ex_w || !apply_handles || !apply_buffers || !test_w || !test_buffers || !signature_w) return 2;

	/* Pieces of the old file, new text and repeats over three chunks; the new file takes the time the patch carries. */
	if (!apply_w(at(0, L"delta.pa19"), at(1, L"old.txt"), at(2, L"made-delta.txt"), 0)) return 3;
	if (!same_files(L"made-delta.txt", L"new.txt")) return 4;
	HANDLE made = open_read(L"made-delta.txt");
	FILETIME written = { 0 };
	if (made == INVALID_HANDLE_VALUE || !GetFileTime(made, NULL, NULL, &written)) return 5;
	CloseHandle(made);
	/* 1700000000 seconds after 1970, in 100 ns steps after 1601. */
	ULARGE_INTEGER stamp = { .LowPart = written.dwLowDateTime, .HighPart = written.dwHighDateTime };
	if (stamp.QuadPart != 133444736000000000ull) return 6;

	/* No old file: uncompressed blocks across chunks, then a compressed one; through the narrow names. */
	if (!apply_a(narrow(0, L"plain.pa19"), NULL, narrow(1, L"made-plain.txt"), 0)) return 7;
	if (!same_files(L"made-plain.txt", L"plain.txt")) return 8;

	/* x86 calls, in memory: a buffer the library allocates, then the caller's, first too small. */
	ULONG patch_size = 0, wanted_size = 0, new_size = 0;
	BYTE *patch = load(L"calls.pa19", &patch_size), *wanted = load(L"calls.bin", &wanted_size);
	if (!patch || !wanted) return 9;
	BYTE *output = NULL;
	FILETIME time = { 1, 1 };
	if (!apply_buffers(patch, patch_size, NULL, 0, &output, 0, &new_size, &time, 0, NULL, NULL)) return 10;
	if (!output || new_size != wanted_size || !same_bytes(output, wanted, new_size)) return 11;
	if (time.dwLowDateTime || time.dwHighDateTime) return 12;
	VirtualFree(output, 0, MEM_RELEASE);
	BYTE *own = VirtualAlloc(NULL, wanted_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	output = own;
	new_size = 0;
	if (!fails_with(apply_buffers(patch, patch_size, NULL, 0, &output, wanted_size - 1, &new_size, NULL, 0, NULL, NULL),
		ERROR_INSUFFICIENT_BUFFER)) return 13;
	if (new_size != wanted_size || output != own) return 14;
	if (!apply_buffers(patch, patch_size, NULL, 0, &output, wanted_size, &new_size, NULL, 0, NULL, NULL)) return 15;
	if (output != own || !same_bytes(own, wanted, wanted_size)) return 16;
	new_size = 0;
	if (!test_buffers(patch, patch_size, NULL, 0, &new_size, 0) || new_size != wanted_size) return 17;
	/* A patch is refused whole when a byte of it changed or it ends early. */
	patch[patch_size / 2] ^= 0x5a;
	if (!fails_with(apply_buffers(patch, patch_size, NULL, 0, &output, wanted_size, &new_size, NULL, 0, NULL, NULL), ERROR_PATCH_CORRUPT))
		return 18;
	patch[patch_size / 2] ^= 0x5a;
	if (!fails_with(apply_buffers(patch, patch_size - 3, NULL, 0, &output, wanted_size, &new_size, NULL, 0, NULL, NULL), ERROR_PATCH_CORRUPT))
		return 19;
	/* Without anywhere to put the new file, only a test can run. */
	if (!fails_with(apply_buffers(patch, patch_size, NULL, 0, NULL, 0, &new_size, NULL, 0, NULL, NULL), ERROR_INVALID_PARAMETER)) return 20;
	if (!apply_buffers(patch, patch_size, NULL, 0, NULL, 0, &new_size, NULL, APPLY_OPTION_TEST_ONLY, NULL, NULL)) return 21;

	/* One patch for two old files, each with a range it ignores and one it carries over to the new file; through handles. */
	if (!test_w(at(0, L"two.pa19"), at(1, L"first.bin"), 0) || !test_w(at(0, L"two.pa19"), at(1, L"second.bin"), 0)) return 22;
	if (!fails_with(test_w(at(0, L"two.pa19"), at(1, L"old.txt"), 0), ERROR_PATCH_WRONG_FILE)) return 23;
	HANDLE patch_file = open_read(L"two.pa19"), old_file = open_read(L"second.bin");
	HANDLE new_file = CreateFileW(at(0, L"made-second.bin"), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (patch_file == INVALID_HANDLE_VALUE || old_file == INVALID_HANDLE_VALUE || new_file == INVALID_HANDLE_VALUE) return 24;
	BOOL applied = apply_handles(patch_file, old_file, new_file, 0);
	CloseHandle(patch_file);
	CloseHandle(old_file);
	CloseHandle(new_file);
	if (!applied || !same_files(L"made-second.bin", L"target-second.bin")) return 25;
	if (!apply_w(at(0, L"two.pa19"), at(1, L"first.bin"), at(2, L"made-first.bin"), 0)) return 26;
	if (!same_files(L"made-first.bin", L"target-first.bin")) return 27;
	/* A file the patch was not made for leaves no new file behind. */
	if (!fails_with(apply_w(at(0, L"two.pa19"), at(1, L"old.txt"), at(2, L"made-wrong.bin"), 0), ERROR_PATCH_WRONG_FILE)) return 28;
	if (exists(L"made-wrong.bin")) return 29;

	/* A patch that is only a header: it tells whether it fits, and cannot make the new file. */
	if (!test_w(at(0, L"two-header.pa19"), at(1, L"first.bin"), 0)) return 30;
	if (!fails_with(apply_w(at(0, L"two-header.pa19"), at(1, L"first.bin"), at(2, L"made-header.bin"), 0), ERROR_PATCH_NOT_AVAILABLE)) return 31;

	/* Between identical files there is nothing to do, which the caller may want to hear about. */
	if (!apply_w(at(0, L"same.pa19"), at(1, L"old.txt"), at(2, L"made-same.txt"), 0) || !same_files(L"made-same.txt", L"old.txt")) return 32;
	if (!fails_with(apply_w(at(0, L"same.pa19"), at(1, L"old.txt"), at(2, L"made-exact.txt"), APPLY_OPTION_FAIL_IF_EXACT),
		ERROR_PATCH_NOT_NECESSARY)) return 33;

	/* A progress callback that says stop. */
	if (!fails_with(apply_ex_w(at(0, L"delta.pa19"), at(1, L"old.txt"), at(2, L"made-stopped.txt"), 0, stop_at_once, NULL), ERROR_CANCELLED))
		return 34;
	if (progress_calls != 1 || exists(L"made-stopped.txt")) return 35;

	/* Making patches is not part of this library. */
	if (!fails_with(signature_w(at(0, L"old.txt"), 0, NULL, 0, NULL, 0, NULL, 0, NULL), ERROR_CALL_NOT_IMPLEMENTED)) return 36;
	return 0;
}
