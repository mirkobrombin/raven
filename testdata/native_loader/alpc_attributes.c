#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

struct message_attributes {
	ULONG allocated, valid;
	BYTE storage[200];
};

typedef SIZE_T (NTAPI *get_header_size)(ULONG);
typedef NTSTATUS (NTAPI *initialize_attributes)(ULONG, struct message_attributes *, SIZE_T, SIZE_T *);
typedef void *(NTAPI *get_attribute)(const struct message_attributes *, ULONG);

int raven_entry(void)
{
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	const get_header_size header_size = (get_header_size)GetProcAddress(ntdll, "AlpcGetHeaderSize");
	const initialize_attributes init = (initialize_attributes)GetProcAddress(ntdll, "AlpcInitializeMessageAttribute");
	const get_attribute get = (get_attribute)GetProcAddress(ntdll, "AlpcGetMessageAttribute");
	if (!header_size || !init || !get)
		return 1;
	const ULONG security = 0x80000000, view = 0x40000000, context = 0x20000000, low_unknown = 0x80;
	const ULONG mask = security | view | context | low_unknown;
	const SIZE_T context_size = sizeof(void *) == 8 ? 32 : 20;
	const SIZE_T total = 8 + 7 * sizeof(void *) + context_size;
	if (header_size(mask) != total || header_size(low_unknown) != 8 || header_size(~0UL) != (sizeof(void *) == 8 ? 160 : 108))
		return 2;
	struct message_attributes message;
	message.allocated = 1;
	message.valid = 2;
	for (SIZE_T offset = 0; offset < sizeof message.storage; ++offset)
		message.storage[offset] = 0xa5;
	SIZE_T needed = 0;
	if (init(mask, &message, total - 1, &needed) != (NTSTATUS)0xc0000023 || needed != total || message.allocated != 1 || message.valid != 2)
		return 3;
	if (init(mask, NULL, total, &needed) || needed != total || init(mask, &message, sizeof message, &needed) ||
		message.allocated != mask || message.valid != 0)
		return 4;
	if (get(&message, security) != message.storage || get(&message, view) != message.storage + 3 * sizeof(void *) ||
		get(&message, context) != message.storage + 7 * sizeof(void *))
		return 5;
	message.valid = 0x10000000;
	if (get(&message, message.valid) || get(&message, view | context) || get(&message, low_unknown) || get(&message, 0))
		return 6;
	for (SIZE_T offset = 0; offset < sizeof message.storage; ++offset)
		if (message.storage[offset] != 0xa5)
			return 7;
	return 0;
}
