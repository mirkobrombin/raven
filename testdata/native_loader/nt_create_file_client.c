#include <windows.h>
#include <winternl.h>

// NtCreateFile and NtOpenFile: a file is created, then refused as existing, opened, and opened again relative to its directory;
// a directory is created with FILE_DIRECTORY_FILE, and a missing name is not found. Returns 0, or the number of the check that failed.
NTSTATUS NTAPI NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);

static void append(WCHAR *to, const WCHAR *text)
{
	while (*to)
		++to;
	while ((*to++ = *text++))
		;
}

static UNICODE_STRING text_of(WCHAR *text)
{
	USHORT length = 0;
	while (text[length])
		++length;
	UNICODE_STRING result = {(USHORT)(length * 2), (USHORT)(length * 2 + 2), text};
	return result;
}

int raven_entry(void)
{
	static WCHAR temporary[MAX_PATH], native[MAX_PATH + 64], directory_name[MAX_PATH + 64], file_name[MAX_PATH + 96];
	if (!GetTempPathW(MAX_PATH, temporary))
		return 1;
	append(native, L"\\??\\");
	append(native, temporary);
	append(native, L"raven-nt-create");
	append(directory_name, native);
	append(file_name, native);
	append(file_name, L"\\file.txt");
	UNICODE_STRING name = text_of(directory_name);
	static OBJECT_ATTRIBUTES attributes, inside;
	attributes.Length = sizeof attributes;
	attributes.ObjectName = &name;
	attributes.Attributes = OBJ_CASE_INSENSITIVE;
	IO_STATUS_BLOCK status;
	HANDLE directory, file;
	NTSTATUS result = NtCreateFile(&directory, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &status, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_OPEN_IF, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (result)
		return 2;
	name = text_of(file_name);
	result = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &status, NULL, 0, 0, FILE_OVERWRITE_IF,
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (result || (status.Information != FILE_CREATED && status.Information != FILE_OVERWRITTEN))
		return 3;
	NtClose(file);
	result = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &status, NULL, 0, 0, FILE_CREATE,
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (result != (NTSTATUS)0xC0000035L)
		return 4;
	result = NtCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &status, NULL, 0, FILE_SHARE_READ, FILE_OPEN_IF,
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (result || status.Information != FILE_OPENED)
		return 5;
	NtClose(file);
	WCHAR relative[] = L"file.txt";
	name = text_of(relative);
	inside = attributes;
	inside.RootDirectory = directory;
	result = NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &inside, &status, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT);
	if (result)
		return 6;
	NtClose(file);
	result = NtCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &inside, &status, NULL, 0, FILE_SHARE_READ, FILE_OPEN,
		FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (result != (NTSTATUS)0xC0000103L)
		return 7;
	WCHAR missing[] = L"missing.txt";
	name = text_of(missing);
	result = NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &inside, &status, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT);
	if (result != (NTSTATUS)0xC0000034L)
		return 8;
	NtClose(directory);
	return 0;
}
