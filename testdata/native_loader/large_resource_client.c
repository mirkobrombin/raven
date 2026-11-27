#define UNICODE
#include <windows.h>

// More than 256 MiB of uninitialized data: the image is that large, the file is not.
static unsigned char payload[257u << 20];

static IMAGE_RESOURCE_DIRECTORY_ENTRY *child(BYTE *root, DWORD offset, WORD id)
{
	IMAGE_RESOURCE_DIRECTORY *directory = (IMAGE_RESOURCE_DIRECTORY *)(root + offset);
	IMAGE_RESOURCE_DIRECTORY_ENTRY *entries = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(directory + 1);
	for (WORD index = 0; index < directory->NumberOfNamedEntries + directory->NumberOfIdEntries; ++index)
		if (!entries[index].NameIsString && entries[index].Id == id)
			return &entries[index];
	return NULL;
}

// A self-extracting installer keeps its whole package in one resource. This one points RCDATA 102's data entry at the payload above,
// so its resource is as large as such a package without making the fixture as large.
int raven_entry(void)
{
	HMODULE module = GetModuleHandleW(NULL);
	BYTE *base = (BYTE *)module;
	IMAGE_NT_HEADERS *headers = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	BYTE *root = base + headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress;
	IMAGE_RESOURCE_DIRECTORY_ENTRY *type = child(root, 0, 10);
	if (!type || !type->DataIsDirectory)
		return 1;
	IMAGE_RESOURCE_DIRECTORY_ENTRY *name = child(root, type->OffsetToDirectory, 102);
	if (!name || !name->DataIsDirectory)
		return 2;
	IMAGE_RESOURCE_DIRECTORY_ENTRY *language = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)((IMAGE_RESOURCE_DIRECTORY *)(root + name->OffsetToDirectory) + 1);
	if (language->DataIsDirectory)
		return 3;
	IMAGE_RESOURCE_DATA_ENTRY *entry = (IMAGE_RESOURCE_DATA_ENTRY *)(root + language->OffsetToData);
	DWORD protection;
	if (!VirtualProtect(entry, sizeof *entry, PAGE_READWRITE, &protection))
		return 4;
	entry->OffsetToData = (DWORD)(payload - base);
	entry->Size = sizeof payload;
	payload[sizeof payload - 1] = 0x5a;
	// A null module is the program's own, and the two handles may be served apart.
	HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(102), RT_RCDATA);
	if (!resource || resource != FindResourceW(NULL, MAKEINTRESOURCEW(102), RT_RCDATA) ||
		SizeofResource(module, resource) != sizeof payload || SizeofResource(NULL, resource) != sizeof payload)
		return 5;
	HMODULE owners[] = {module, NULL};
	for (int index = 0; index < 2; ++index) {
		HGLOBAL data = LoadResource(owners[index], resource);
		const volatile BYTE *bytes = (const volatile BYTE *)LockResource(data);
		if ((void *)data != payload || (const void *)bytes != payload || bytes[sizeof payload - 1] != 0x5a)
			return 6 + index;
	}
	return 0;
}
