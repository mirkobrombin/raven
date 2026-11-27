#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <iphlpapi.h>

/* A program reads how the machine is networked before it opens its first socket: how many interfaces there are, a row for each, and one
   row by index, and the LUID and name the newer calls know each interface by. Every answer has to agree with the others, and a bad
   request has to fail the way Windows fails it. */
typedef char mib_if_row_is_860_bytes[sizeof(MIB_IFROW) == 860 ? 1 : -1];

static int failure;

static void expect(int condition, int code)
{
	if (!condition && !failure)
		failure = code;
}

static void zero(void *buffer, unsigned size)
{
	BYTE *bytes = buffer;
	for (unsigned at = 0; at < size; ++at)
		bytes[at] = 0;
}

static int same_name(const WCHAR *left, const WCHAR *right)
{
	for (unsigned at = 0; at < 256; ++at)
		if (left[at] != right[at])
			return 0;
	return 1;
}

int raven_entry(void)
{
	ULONG count = 0;
	expect(GetNumberOfInterfaces(&count) == NO_ERROR && count > 0, 1);

	ULONG size = 0;
	expect(GetIfTable(NULL, &size, 0) == ERROR_INSUFFICIENT_BUFFER && size >= sizeof(MIB_IFTABLE), 2);
	MIB_IFTABLE *table = LocalAlloc(LMEM_FIXED, size);
	expect(table != NULL, 3);
	if (!table)
		return failure;
	expect(GetIfTable(table, &size, 0) == NO_ERROR, 4);
	expect(table->dwNumEntries == count, 5);
	expect(size == 4 + count * sizeof(MIB_IFROW), 6);

	for (DWORD at = 0; at < table->dwNumEntries; ++at) {
		const MIB_IFROW *row = &table->table[at];
		expect(row->dwIndex != 0, 7);
		expect(row->dwDescrLen <= MAXLEN_IFDESCR, 8);
		expect(row->dwPhysAddrLen <= MAXLEN_PHYSADDR, 9);
		// The name is a wide string in a fixed field, so a terminator has to be somewhere in it.
		int terminated = 0;
		for (unsigned character = 0; character < 256; ++character)
			terminated |= row->wszName[character] == 0;
		expect(terminated, 10);
		// The same interface, asked for by the index the row carries, is described the same way.
		MIB_IFROW one;
		zero(&one, sizeof one);
		one.dwIndex = row->dwIndex;
		expect(GetIfEntry(&one) == NO_ERROR, 11);
		expect(same_name(one.wszName, row->wszName), 12);
		expect(one.dwType == row->dwType && one.dwMtu == row->dwMtu, 13);
		expect(one.dwDescrLen == row->dwDescrLen, 14);
		for (DWORD character = 0; character < one.dwDescrLen; ++character)
			expect(one.bDescr[character] == row->bDescr[character], 15);
		// The index, the LUID and the interface's name lead to one another, wide and narrow alike; the LUID carries the type.
		NET_LUID luid, back;
		NET_IFINDEX index = 0;
		WCHAR name[IF_MAX_STRING_SIZE + 1];
		char narrow[IF_MAX_STRING_SIZE + 1];
		luid.Value = back.Value = 0;
		expect(ConvertInterfaceIndexToLuid(row->dwIndex, &luid) == NO_ERROR && luid.Value != 0, 21);
		expect(luid.Info.IfType == row->dwType, 22);
		expect(ConvertInterfaceLuidToIndex(&luid, &index) == NO_ERROR && index == row->dwIndex, 23);
		expect(ConvertInterfaceLuidToNameW(&luid, name, IF_MAX_STRING_SIZE + 1) == NO_ERROR && name[0], 24);
		expect(ConvertInterfaceNameToLuidW(name, &back) == NO_ERROR && back.Value == luid.Value, 25);
		back.Value = 0;
		expect(ConvertInterfaceLuidToNameA(&luid, narrow, sizeof narrow) == NO_ERROR && narrow[0] == (char)name[0], 26);
		expect(ConvertInterfaceNameToLuidA(narrow, &back) == NO_ERROR && back.Value == luid.Value, 27);
		// A name buffer of two characters cannot hold any name.
		expect(ConvertInterfaceLuidToNameW(&luid, name, 2) == ERROR_NOT_ENOUGH_MEMORY, 28);
	}

	// An index or a name no interface could have is refused.
	NET_LUID none;
	none.Value = 0;
	expect(ConvertInterfaceIndexToLuid(0x7fffffffu, &none) == ERROR_FILE_NOT_FOUND, 29);
	expect(ConvertInterfaceNameToLuidW(L"no such interface", &none) == ERROR_INVALID_NAME, 30);
	expect(ConvertInterfaceIndexToLuid(1, NULL) == ERROR_INVALID_PARAMETER, 31);

	// An index no interface has is refused, as Windows refuses it.
	MIB_IFROW missing;
	zero(&missing, sizeof missing);
	missing.dwIndex = 0x7fffffffu;
	expect(GetIfEntry(&missing) == ERROR_INVALID_PARAMETER, 16);

	// A table one byte short of the rows is refused, and the size it needs comes back.
	ULONG short_size = size - 1;
	expect(GetIfTable(table, &short_size, 0) == ERROR_INSUFFICIENT_BUFFER, 17);
	expect(short_size == size, 18);

	// The sorted order lists the rows by index.
	expect(GetIfTable(table, &size, 1) == NO_ERROR, 19);
	for (DWORD at = 1; at < table->dwNumEntries; ++at)
		expect(table->table[at - 1].dwIndex < table->table[at].dwIndex, 20);

	LocalFree(table);
	return failure;
}
