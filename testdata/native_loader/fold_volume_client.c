#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

// FoldStringW's mappings, and the volume of an open file and the file itself through NtQueryVolumeInformationFile and
// NtQueryInformationFile, as setup programs call them.

typedef struct {
	ULONG FileSystemAttributes;
	LONG MaximumComponentNameLength;
	ULONG FileSystemNameLength;
	WCHAR FileSystemName[1];
} ATTRIBUTE_INFORMATION;

typedef struct {
	LARGE_INTEGER TotalAllocationUnits;
	LARGE_INTEGER AvailableAllocationUnits;
	ULONG SectorsPerAllocationUnit;
	ULONG BytesPerSector;
} SIZE_INFORMATION;

static int same(const WCHAR *left, const WCHAR *right, int count)
{
	for (int index = 0; index < count; ++index)
		if (left[index] != right[index]) return 0;
	return 1;
}

int raven_entry(void)
{
	WCHAR folded[16];
	// Fullwidth letters and Arabic-Indic digits fold to ASCII; the terminator is counted when the source length is -1.
	if (FoldStringW(MAP_FOLDCZONE | MAP_FOLDDIGITS, L"\xff21\xff42\x0661\x0662", -1, folded, 16) != 5 || !same(folded, L"Ab12", 5)) return 1;
	if (FoldStringW(MAP_EXPAND_LIGATURES, L"\x00e6\xfb01", 2, folded, 16) != 4 || !same(folded, L"aefi", 4)) return 2;
	if (FoldStringW(MAP_COMPOSITE, L"\x00e9", 1, folded, 16) != 2 || !same(folded, L"e\x0301", 2)) return 3;
	if (FoldStringW(MAP_PRECOMPOSED, L"e\x0301x", 3, folded, 16) != 2 || !same(folded, L"\x00e9x", 2)) return 4;
	if (FoldStringW(MAP_FOLDDIGITS, L"\x0663", 1, NULL, 0) != 1) return 5;
	if (FoldStringW(MAP_FOLDDIGITS, L"\x0663\x0664", 2, folded, 1) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return 6;
	if (FoldStringW(MAP_PRECOMPOSED | MAP_COMPOSITE, L"a", 1, folded, 16) || GetLastError() != ERROR_INVALID_FLAGS) return 7;

	HANDLE file = CreateFileW(L"C:\\Windows", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (file == INVALID_HANDLE_VALUE) return 10;
	IO_STATUS_BLOCK status;
	union { ATTRIBUTE_INFORMATION attributes; SIZE_INFORMATION sizes; BYTE bytes[64]; } buffer;
	if (NtQueryVolumeInformationFile(file, &status, &buffer, sizeof(buffer), (FS_INFORMATION_CLASS)5) || status.Information != 20 ||
		buffer.attributes.FileSystemNameLength != 8 || !same(buffer.attributes.FileSystemName, L"NTFS", 4)) return 11;
	// A name cut by the buffer is reported as an overflow with the part that fit.
	if (NtQueryVolumeInformationFile(file, &status, &buffer, 14, (FS_INFORMATION_CLASS)5) != (NTSTATUS)0x80000005 || status.Information != 14) return 12;
	if (NtQueryVolumeInformationFile(file, &status, &buffer, 8, (FS_INFORMATION_CLASS)5) != (NTSTATUS)0xc0000004) return 13;
	if (NtQueryVolumeInformationFile(file, &status, &buffer, sizeof(buffer), (FS_INFORMATION_CLASS)3) || status.Information != 24 ||
		buffer.sizes.BytesPerSector != 512 || !buffer.sizes.TotalAllocationUnits.QuadPart) return 14;
	if (NtQueryVolumeInformationFile(file, &status, &buffer, sizeof(buffer), (FS_INFORMATION_CLASS)99) != (NTSTATUS)0xc0000003) return 15;
	// The file: its times and attributes, its sizes and that it is a directory, its name on the volume, where it is read from; an
	// unknown class is refused.
	union { FILE_BASIC_INFORMATION basic; FILE_STANDARD_INFORMATION standard; FILE_POSITION_INFORMATION position; BYTE bytes[128]; } file_buffer;
	if (NtQueryInformationFile(file, &status, &file_buffer, sizeof(file_buffer), FileBasicInformation) || status.Information != 40 ||
		!(file_buffer.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !file_buffer.basic.LastWriteTime.QuadPart) return 16;
	if (NtQueryInformationFile(file, &status, &file_buffer, sizeof(file_buffer), FileStandardInformation) || status.Information != 24 ||
		!file_buffer.standard.Directory) return 17;
	if (NtQueryInformationFile(file, &status, &file_buffer, sizeof(file_buffer), (FILE_INFORMATION_CLASS)9) || status.Information != 4 + 16 ||
		*(ULONG *)file_buffer.bytes != 16 || !same((WCHAR *)(file_buffer.bytes + 4), L"\\Windows", 8)) return 18;
	if (NtQueryInformationFile(file, &status, &file_buffer, sizeof(file_buffer), (FILE_INFORMATION_CLASS)14) || status.Information != 8 ||
		file_buffer.position.CurrentByteOffset.QuadPart) return 19;
	if (NtQueryInformationFile(file, &status, &file_buffer, 8, FileBasicInformation) != (NTSTATUS)0xc0000004) return 20;
	if (NtQueryInformationFile(file, &status, &file_buffer, sizeof(file_buffer), (FILE_INFORMATION_CLASS)999) != (NTSTATUS)0xc0000003) return 21;
	CloseHandle(file);
	return 0;
}
