// A program that reads a system library's PE export directory directly (as a protector or manual resolver does) obtains the address
// of the provider stub rather than the component target that GetProcAddress follows. Calling that stub by address runs the forwarded
// component function with the caller's frame instead of faulting on the stub's breakpoint.
#include <windows.h>

static int equal_ascii(const char *a, const char *b)
{
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

static void *export_stub_address(HMODULE module, const char *wanted)
{
	const BYTE *base = (const BYTE *)module;
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return NULL;
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return NULL;
	const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (!dir->VirtualAddress || !dir->Size)
		return NULL;
	const IMAGE_EXPORT_DIRECTORY *exp = (const IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
	const DWORD *names = (const DWORD *)(base + exp->AddressOfNames);
	const WORD *ordinals = (const WORD *)(base + exp->AddressOfNameOrdinals);
	const DWORD *functions = (const DWORD *)(base + exp->AddressOfFunctions);
	for (DWORD index = 0; index < exp->NumberOfNames; ++index) {
		const char *name = (const char *)(base + names[index]);
		if (equal_ascii(name, wanted))
			return (void *)(base + functions[ordinals[index]]);
	}
	return NULL;
}

int raven_entry(void)
{
	// Import a user32 function so user32.dll and raven-user32.dll are loaded; GetDlgItemTextW and GetDlgItemInt are not imported,
	// yet user32.dll exports them and forwards both to raven-user32.dll.
	if (IsWindow((HWND)0))
		return 1;
	HMODULE user32 = GetModuleHandleA("user32.dll");
	if (!user32)
		return 2;
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)user32;
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const BYTE *)user32 + dos->e_lfanew);
	const BYTE *image_start = (const BYTE *)user32;
	const BYTE *image_end = image_start + nt->OptionalHeader.SizeOfImage;

	void *text_stub = export_stub_address(user32, "GetDlgItemTextW");
	void *int_stub = export_stub_address(user32, "GetDlgItemInt");
	if (!text_stub || !int_stub)
		return 3;
	if ((const BYTE *)text_stub < image_start || (const BYTE *)text_stub >= image_end)
		return 4;
	void *resolved_text = (void *)GetProcAddress(user32, "GetDlgItemTextW");
	if (!resolved_text || resolved_text == text_stub)
		return 5;

	WCHAR buffer[8] = {L'a', L'b', L'c', 0};
	typedef UINT (WINAPI *get_dlg_item_text_w_fn)(HWND, int, LPWSTR, int);
	if (((get_dlg_item_text_w_fn)text_stub)(NULL, 1, buffer, 8) != 0)
		return 6;
	if (buffer[0] != 0)
		return 7;

	BOOL translated = TRUE;
	typedef UINT (WINAPI *get_dlg_item_int_fn)(HWND, int, BOOL *, BOOL);
	if (((get_dlg_item_int_fn)int_stub)(NULL, 1, &translated, FALSE) != 0)
		return 8;
	if (translated != FALSE)
		return 9;
	return 0;
}
