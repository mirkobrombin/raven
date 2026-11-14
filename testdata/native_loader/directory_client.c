#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct { ULONG_PTR status, information; } IoStatus;
typedef struct { USHORT length, maximum; WCHAR *buffer; } UnicodeString;
typedef LONG (WINAPI *QueryDirectory)(HANDLE,HANDLE,void *,void *,IoStatus *,void *,ULONG,ULONG,BYTE,UnicodeString *,BYTE);
// The same call with its BOOLEAN arguments as whole words: callers leave the bytes above the flag as they happen to be.
typedef LONG (WINAPI *QueryDirectoryWords)(HANDLE,HANDLE,void *,void *,IoStatus *,void *,ULONG,ULONG,ULONG,UnicodeString *,ULONG);

#define CHECK(code, condition) do { if (!(condition)) return code; } while (0)

static int same(const WCHAR *left, const WCHAR *right, unsigned bytes)
{
	unsigned i = 0;
	for (; right[i]; ++i) if (i * 2 >= bytes || left[i] != right[i]) return 0;
	return i * 2 == bytes;
}

static void join(WCHAR *out, const WCHAR *left, const WCHAR *right)
{
	unsigned i = 0, j = 0;
	while (left[i]) { out[i] = left[i]; ++i; }
	while (right[j]) out[i++] = right[j++];
	out[i] = 0;
}

int raven_entry(void)
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	QueryDirectory query = (QueryDirectory)GetProcAddress(ntdll,"NtQueryDirectoryFile");
	QueryDirectory zw = (QueryDirectory)GetProcAddress(ntdll,"ZwQueryDirectoryFile");
	CHECK(1, query && zw);
	WCHAR temporary[260], root[260], directory[260], path[260];
	CHECK(2, GetTempPathW(260,temporary));
	join(root,temporary,L"raven-directory-query");
	join(directory,root,L"\\");
	CreateDirectoryW(root,0);
	const WCHAR *names[] = {L"alpha.txt",L"bravo.txt",L"accent-\x00e8.txt",L"other.bin"};
	for (unsigned i = 0; i < 4; ++i) {
		join(path,directory,names[i]);
		HANDLE file = CreateFileW(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
		DWORD written;
		CHECK(3, file != INVALID_HANDLE_VALUE && WriteFile(file,"hello",5,&written,0) && written == 5 && CloseHandle(file));
	}
	HANDLE handle = CreateFileW(root,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
	CHECK(4, handle != INVALID_HANDLE_VALUE);
	UnicodeString pattern = {10,10,L"*.TXT"}, ignored = {18,18,L"other.bin"};
	union { ULONGLONG alignment; BYTE bytes[1024]; } output;
	IoStatus io;
	SetLastError(0x1234);
	CHECK(5, query(handle,0,0,0,&io,output.bytes,8,2,0,&pattern,0) == (LONG)0xc0000023 && io.information == 0);
	CHECK(6, query(handle,0,0,0,&io,output.bytes,1024,99,0,&pattern,0) == (LONG)0xc0000003);
	CHECK(7, query(handle,0,0,0,&io,output.bytes,70,2,0,&pattern,0) == (LONG)0x80000005 && io.status == 0x80000005 && io.information == 70);
	CHECK(8, *(DWORD *)(output.bytes+60) > 2 && *(DWORD *)(output.bytes+56) == FILE_ATTRIBUTE_ARCHIVE);
	CHECK(9, query(handle,0,0,0,&io,output.bytes,70,2,0,&ignored,0) == 0 && io.information == 0);
	HANDLE alias = 0;
	CHECK(10, DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&alias,0,FALSE,DUPLICATE_SAME_ACCESS));
	unsigned seen = 0;
	for (unsigned i = 0; i < 3; ++i) {
		CHECK(11, zw(i == 1 ? alias : handle,0,0,0,&io,output.bytes,1024,2,1,&ignored,0) == 0 && io.status == 0 && io.information > 68);
		CHECK(12, *(DWORD *)output.bytes == 0 && *(ULONGLONG *)(output.bytes+40) == 5 && *(ULONGLONG *)(output.bytes+48) >= 5);
		unsigned length = *(DWORD *)(output.bytes+60), found = 0;
		for (unsigned n = 0; n < 3; ++n) if (same((WCHAR *)(output.bytes+68),names[n],length)) found = 1U << n;
		CHECK(13, found && !(seen & found));
		seen |= found;
	}
	CHECK(14, seen == 7 && query(handle,0,0,0,&io,output.bytes,1024,2,0,0,0) == (LONG)0x80000006 && !io.information);
	CHECK(15, query(alias,0,0,0,&io,output.bytes,1024,2,0,&ignored,1) == 0);
	unsigned offset = 0, count = 0;
	for (;;) {
		CHECK(16, offset+68 <= io.information && *(DWORD *)(output.bytes+offset+60)+offset+68 <= io.information);
		++count;
		DWORD next = *(DWORD *)(output.bytes+offset);
		if (!next) break;
		CHECK(17, !(next & 7) && next >= 68 && offset+next < io.information);
		offset += next;
	}
	CHECK(18, count == 3 && GetLastError() == 0x1234);
	for (unsigned i = 0; i < 4; ++i) {
		const DWORD classes[] = {1,2,3,12}, headers[] = {64,68,94,12};
		CHECK(19, query(handle,0,0,0,&io,output.bytes,1024,classes[i],1,0,1) == 0);
		const DWORD length = *(DWORD *)(output.bytes+(classes[i] == 12 ? 8 : 60));
		CHECK(20, io.information == headers[i]+length && length && !*(DWORD *)output.bytes);
	}
	HANDLE denied;
	CHECK(21, DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&denied,0,FALSE,0));
	CHECK(22, query(denied,0,0,0,&io,output.bytes,1024,2,0,0,0) == (LONG)0xc0000022 && !io.information && CloseHandle(denied));
	CHECK(23, query(handle,0,0,0,&io,(void *)1,1024,2,0,0,1) == (LONG)0xc0000005);
	CHECK(24, query(handle,0,0,0,(IoStatus *)1,output.bytes,1024,2,0,0,1) == (LONG)0xc0000005);
	CHECK(25, query((HANDLE)0x123,0,0,0,&io,output.bytes,1024,2,0,0,0) == (LONG)0xc0000008);
	CHECK(26, query(handle,0,(void *)1,0,&io,output.bytes,1024,2,0,0,0) == (LONG)0xc00000bb);
	// The file's own information is found by name as .NET's platform calls find it: the standard class says the handle is a directory.
	typedef LONG (WINAPI *QueryFile)(HANDLE,IoStatus *,void *,ULONG,ULONG);
	QueryFile query_file = (QueryFile)GetProcAddress(ntdll,"NtQueryInformationFile");
	CHECK(34, query_file && query_file(handle,&io,output.bytes,24,5) == 0 && io.information == 24 && output.bytes[21] == 1);
	CHECK(27, CloseHandle(alias) && CloseHandle(handle));
	handle = CreateFileW(root,FILE_LIST_DIRECTORY,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
	UnicodeString missing = {14,14,L"missing"};
	CHECK(28, query(handle,0,0,0,&io,output.bytes,1024,2,0,&missing,0) == (LONG)0xc000000f);
	CHECK(29, query(handle,0,0,0,&io,output.bytes,1024,2,0,0,0) == (LONG)0x80000006 && CloseHandle(handle));
	QueryDirectoryWords words = (QueryDirectoryWords)query;
	handle = CreateFileW(root,FILE_LIST_DIRECTORY,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);
	CHECK(32, words(handle,0,0,0,&io,output.bytes,128,3,0xd7ae0000,0,0xd7ae0001) == 0);
	unsigned calls = 1;
	LONG status;
	while ((status = words(handle,0,0,0,&io,output.bytes,128,3,0xd7ae0000,0,0xd7ae0000)) == 0 && calls < 16)
		++calls;
	CHECK(33, status == (LONG)0x80000006 && calls >= 2 && calls < 16 && CloseHandle(handle));
	for (unsigned i = 0; i < 4; ++i) {
		join(path,directory,names[i]);
		CHECK(30, DeleteFileW(path));
	}
	CHECK(31, RemoveDirectoryW(root));
	return 0;
}
