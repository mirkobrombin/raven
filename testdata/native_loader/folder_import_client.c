#include <windows.h>

/* The import of Sub/Folder.dll, written out because the MinGW tools keep only the file name of a library they import: one descriptor,
 * its lookup and address tables with their ends, the name of the function and the library's path. */
__asm__(
	".section .idata$2,\"w\"\n"
	".rva folder_lookup\n.long 0, 0\n.rva folder_library, __imp_folder_value\n"
	".section .idata$4,\"w\"\n.p2align 3\n"
	"folder_lookup: .rva folder_hint\n.long 0\n.quad 0\n"
	".section .idata$5,\"w\"\n.p2align 3\n"
	".globl __imp_folder_value\n__imp_folder_value: .rva folder_hint\n.long 0\n.quad 0\n"
	".section .idata$6,\"w\"\n.p2align 1\n"
	"folder_hint: .short 0\n.asciz \"folder_value\"\n"
	".section .idata$7,\"w\"\n"
	"folder_library: .asciz \"Sub/Folder.dll\"\n"
	".text\n");

extern int (*__imp_folder_value)(void);
#define folder_value (*__imp_folder_value)

static int ends_with(const WCHAR *text, const WCHAR *tail)
{
	int length = lstrlenW(text), tail_length = lstrlenW(tail);
	return length >= tail_length && !lstrcmpiW(text + length - tail_length, tail);
}

/* The program imports Sub/Folder.dll, a path relative to its own folder, as some games import their protection library: the library is
 * loaded from that folder, known by its file name and reported at its path. A relative path given to LoadLibrary is looked for the same
 * way when the current directory does not hold it. */
int raven_entry(void)
{
	WCHAR path[MAX_PATH];
	HMODULE folder = GetModuleHandleW(L"folder.dll"), later;
	int (*value)(void);
	if (folder_value() != 42)
		return 1;
	if (!folder)
		return 2;
	if (!GetModuleFileNameW(folder, path, MAX_PATH) || !ends_with(path, L"\\sub\\folder.dll"))
		return 3;
	if (!(later = LoadLibraryW(L"Sub\\Later.dll")))
		return 4;
	if (!(value = (int (*)(void))GetProcAddress(later, "folder_value")) || value() != 42)
		return 5;
	if (!GetModuleFileNameW(later, path, MAX_PATH) || !ends_with(path, L"\\sub\\later.dll"))
		return 6;
	return 0;
}
