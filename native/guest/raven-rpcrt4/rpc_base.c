/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

// ---- protected calls ----

// C here has no __try, so the one place that must survive a stub's exception builds its frame by hand.
#ifdef _WIN64

EXCEPTION_DISPOSITION rpc_guard_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, void *dispatcher);
void rpc_guard_landing(void);

__asm__(
	".text\n"
	".globl rpc_guard\n"
	".def rpc_guard; .scl 2; .type 32; .endef\n"
	".seh_proc rpc_guard\n"
	"rpc_guard:\n"
	"	subq $40, %rsp\n"
	"	.seh_stackalloc 40\n"
	"	.seh_endprologue\n"
	"	.seh_handler rpc_guard_handler, @except\n"
	"	movq %rcx, %rax\n"
	"	movq %rdx, %rcx\n"
	"	callq *%rax\n"
	"	xorl %eax, %eax\n"
	"	nop\n"
	".globl rpc_guard_landing\n"
	"rpc_guard_landing:\n"
	"	nop\n"
	"	addq $40, %rsp\n"
	"	retq\n"
	"	.seh_endproc\n");

EXCEPTION_DISPOSITION rpc_guard_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, void *dispatcher)
{
	(void)dispatcher;
	if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
		return ExceptionContinueSearch;
	if (record->ExceptionFlags & EXCEPTION_NONCONTINUABLE && record->ExceptionCode == STATUS_STACK_OVERFLOW)
		return ExceptionContinueSearch;
	RtlUnwindEx(frame, (void *)rpc_guard_landing, record, (void *)(ULONG_PTR)record->ExceptionCode, context, NULL);
	return ExceptionContinueSearch;
}

#else

typedef struct guard_frame
{
	void *previous;
	void *handler;
	void *jump[5];
	LONG status;
} guard_frame;

static EXCEPTION_DISPOSITION __cdecl guard_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, void *dispatcher)
{
	(void)context; (void)dispatcher;
	if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
		return ExceptionContinueSearch;
	guard_frame *self = frame;
	self->status = (LONG)record->ExceptionCode;
	RtlUnwind(frame, NULL, record, NULL);
	__builtin_longjmp(self->jump, 1);
}

// Returns the exception code, or zero when the call came back on its own.
__attribute__((noinline)) LONG rpc_guard(void (*call)(void *), void *arg)
{
	volatile guard_frame frame;
	frame.handler = (void *)guard_handler;
	frame.status = 0;
	__asm__ volatile("movl %%fs:0, %0" : "=r"(frame.previous));
	__asm__ volatile("movl %0, %%fs:0" : : "r"(&frame));
	if (!__builtin_setjmp((void **)frame.jump))
		call(arg);
	__asm__ volatile("movl %0, %%fs:0" : : "r"(frame.previous));
	return frame.status;
}

#endif

void RPC_ENTRY RpcRaiseException(RPC_STATUS status)
{
	ULONG_PTR none = 0;
	RaiseException((DWORD)status, EXCEPTION_NONCONTINUABLE, 0, &none);
	__builtin_trap();
}

// Faults of the program itself, as opposed to failures of a call, are no business of an RPC handler.
int RPC_ENTRY I_RpcExceptionFilter(ULONG code)
{
	switch (code) {
	case STATUS_ACCESS_VIOLATION:
	case STATUS_BREAKPOINT:
	case STATUS_DATATYPE_MISALIGNMENT:
	case STATUS_GUARD_PAGE_VIOLATION:
	case STATUS_ILLEGAL_INSTRUCTION:
	case 0xc00000aa:
	case STATUS_IN_PAGE_ERROR:
	case 0xc0000194:
	case STATUS_PRIVILEGED_INSTRUCTION:
	case 0xc0000235:
	case STATUS_STACK_OVERFLOW:
		return EXCEPTION_CONTINUE_SEARCH;
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

int RPC_ENTRY RpcExceptionFilter(ULONG code)
{
	return I_RpcExceptionFilter(code);
}

void *RPC_ENTRY I_RpcAllocate(unsigned int size)
{
	return HeapAlloc(GetProcessHeap(), 0, size);
}

void RPC_ENTRY I_RpcFree(void *block)
{
	HeapFree(GetProcessHeap(), 0, block);
}

// RPC failures have a status of their own in the NT numbering, listed in ntstatus.h.
static const struct { ULONG status, nt; } nt_statuses[] = {
	{5, 0xc0000022}, {6, 0xc0030005}, {14, 0xc0000017}, {87, 0xc000000d},
	{122, 0xc0000023}, {164, 0xc0000017}, {998, 0xc0000005}, {1130, 0xc0000205},
	{1323, 0xc000006a}, {1328, 0xc000006f}, {1330, 0xc0000071}, {1331, 0xc0000072},
	{1338, 0xc0000079}, {1700, 0xc0020001}, {1701, 0xc0020002}, {1702, 0xc0020003},
	{1703, 0xc0020004}, {1704, 0xc0020005}, {1705, 0xc0020006}, {1706, 0xc0020007},
	{1707, 0xc0020008}, {1708, 0xc0020009}, {1709, 0xc002000a}, {1710, 0xc002000b},
	{1711, 0xc002000c}, {1712, 0xc002000d}, {1713, 0xc002000e}, {1714, 0xc002000f},
	{1715, 0xc0020010}, {1716, 0xc0020011}, {1717, 0xc0020012}, {1718, 0xc0020013},
	{1719, 0xc0020014}, {1720, 0xc0020015}, {1721, 0xc0020016}, {1722, 0xc0020017},
	{1723, 0xc0020018}, {1724, 0xc0020019}, {1725, 0xc002001a}, {1726, 0xc002001b},
	{1727, 0xc002001c}, {1728, 0xc002001d}, {1730, 0xc002001f}, {1732, 0xc0020021},
	{1733, 0xc0020022}, {1734, 0xc0020023}, {1735, 0xc0020024}, {1736, 0xc0020025},
	{1737, 0xc0020026}, {1739, 0xc0020028}, {1740, 0xc0020029}, {1741, 0xc002002a},
	{1742, 0xc002002b}, {1743, 0xc002002c}, {1744, 0xc002002d}, {1745, 0xc002002e},
	{1746, 0xc002002f}, {1747, 0xc0020030}, {1748, 0xc0020031}, {1749, 0xc0020032},
	{1750, 0xc0020033}, {1751, 0xc0020034}, {1752, 0xc0020035}, {1753, 0xc0020036},
	{1754, 0xc0020037}, {1755, 0xc0020038}, {1756, 0xc0020039}, {1757, 0xc002003a},
	{1758, 0xc002003b}, {1759, 0xc002003c}, {1760, 0xc002003d}, {1761, 0xc002003e},
	{1762, 0xc002003f}, {1763, 0xc0020040}, {1764, 0xc0020041}, {1765, 0xc0020042},
	{1766, 0xc0020043}, {1767, 0xc0020044}, {1768, 0xc0020045}, {1769, 0xc0020046},
	{1770, 0xc0020047}, {1771, 0xc0020048}, {1772, 0xc0030001}, {1773, 0xc0030002},
	{1774, 0xc0030003}, {1775, 0xc0030004}, {1777, 0xc0030006}, {1778, 0xc0030007},
	{1779, 0xc0030008}, {1780, 0xc0030009}, {1781, 0xc003000a}, {1782, 0xc003000b},
	{1783, 0xc003000c}, {1791, 0xc0020049}, {1806, 0xc002004a}, {1818, 0xc0020050},
	{1898, 0xc002004b}, {1899, 0xc002004c}, {1900, 0xc002004d}, {1907, 0xc0000224},
	{1909, 0xc0000234}, {1914, 0xc0020062}, {1915, 0xc0020063}, {1916, 0xc003005f},
	{1917, 0xc0030060}, {1918, 0xc0030061},
};

long RPC_ENTRY I_RpcMapWin32Status(RPC_STATUS status)
{
	if (status == RPC_S_OK)
		return 0;
	for (SIZE_T i = 0; i < sizeof(nt_statuses) / sizeof(nt_statuses[0]); i++)
		if (nt_statuses[i].status == (ULONG)status)
			return (long)nt_statuses[i].nt;
	return (long)0xc0000001;
}

// ---- strings ----

char *rpc_narrow(const WCHAR *wide)
{
	if (!wide)
		return NULL;
	INT size = WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
	char *text = HeapAlloc(GetProcessHeap(), 0, size);
	if (text)
		WideCharToMultiByte(CP_ACP, 0, wide, -1, text, size, NULL, NULL);
	return text;
}

WCHAR *rpc_widen(const char *narrow)
{
	if (!narrow)
		return NULL;
	INT size = MultiByteToWideChar(CP_ACP, 0, narrow, -1, NULL, 0);
	WCHAR *text = HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR));
	if (text)
		MultiByteToWideChar(CP_ACP, 0, narrow, -1, text, size);
	return text;
}

WCHAR *rpc_copy(const WCHAR *text)
{
	if (!text)
		return NULL;
	SIZE_T size = (lstrlenW(text) + 1) * sizeof(WCHAR);
	WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, size);
	if (copy)
		memcpy(copy, text, size);
	return copy;
}

void rpc_free(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

RPC_STATUS RPC_ENTRY RpcStringFreeA(RPC_CSTR *text)
{
	if (!text)
		return RPC_S_INVALID_ARG;
	rpc_free(*text);
	*text = NULL;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcStringFreeW(RPC_WSTR *text)
{
	if (!text)
		return RPC_S_INVALID_ARG;
	rpc_free(*text);
	*text = NULL;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY DceErrorInqTextW(RPC_STATUS status, RPC_WSTR buffer)
{
	if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, status, 0, buffer, DCE_C_ERROR_STRING_LEN, NULL)
		&& !FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, RPC_S_NOT_RPC_ERROR, 0, buffer, DCE_C_ERROR_STRING_LEN, NULL))
		lstrcpyW(buffer, L"The error code is not valid.");
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY DceErrorInqTextA(RPC_STATUS status, RPC_CSTR buffer)
{
	WCHAR wide[DCE_C_ERROR_STRING_LEN];
	RPC_STATUS result = DceErrorInqTextW(status, wide);
	if (result == RPC_S_OK)
		WideCharToMultiByte(CP_ACP, 0, wide, -1, (char *)buffer, DCE_C_ERROR_STRING_LEN, NULL, NULL);
	return result;
}

// ---- uuids ----

static const UUID nil_uuid;

RPC_STATUS RPC_ENTRY UuidCreateNil(UUID *uuid)
{
	*uuid = nil_uuid;
	return RPC_S_OK;
}

int RPC_ENTRY UuidIsNil(UUID *uuid, RPC_STATUS *status)
{
	*status = RPC_S_OK;
	return !uuid || !memcmp(uuid, &nil_uuid, sizeof(UUID));
}

static signed char uuid_field_compare(ULONG a, ULONG b)
{
	return a == b ? 0 : a < b ? -1 : 1;
}

signed int RPC_ENTRY UuidCompare(UUID *a, UUID *b, RPC_STATUS *status)
{
	*status = RPC_S_OK;
	const UUID *left = a ? a : &nil_uuid, *right = b ? b : &nil_uuid;
	signed char result = uuid_field_compare(left->Data1, right->Data1);
	if (!result)
		result = uuid_field_compare(left->Data2, right->Data2);
	if (!result)
		result = uuid_field_compare(left->Data3, right->Data3);
	for (INT i = 0; i < 8 && !result; i++)
		result = uuid_field_compare(left->Data4[i], right->Data4[i]);
	return result;
}

int RPC_ENTRY UuidEqual(UUID *a, UUID *b, RPC_STATUS *status)
{
	return !UuidCompare(a, b, status);
}

// Version 4 values from the system's generator, with the version and variant bits as RFC 4122 lays them out.
RPC_STATUS RPC_ENTRY UuidCreate(UUID *uuid)
{
	static BOOLEAN (WINAPI *generate)(void *, ULONG);
	if (!generate) {
		HMODULE library = LoadLibraryW(L"advapi32.dll");
		generate = library ? (void *)GetProcAddress(library, "SystemFunction036") : NULL;
	}
	if (!generate || !generate(uuid, sizeof(*uuid))) {
		static LONG counter;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		ULONGLONG state = (ULONGLONG)now.QuadPart ^ (ULONGLONG)GetCurrentProcessId() << 32 ^ (ULONGLONG)InterlockedIncrement(&counter) << 16;
		for (SIZE_T i = 0; i < sizeof(*uuid); i++) {
			state = state * 6364136223846793005ULL + 1442695040888963407ULL;
			((BYTE *)uuid)[i] = (BYTE)(state >> 56);
		}
	}
	uuid->Data3 = (USHORT)((uuid->Data3 & 0x0fff) | 0x4000);
	uuid->Data4[0] = (BYTE)((uuid->Data4[0] & 0x3f) | 0x80);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY I_UuidCreate(UUID *uuid)
{
	return UuidCreateSequential(uuid);
}

// Version 1 values: time since 1582 in 100 ns units, with a random node marked as not being a real network address.
RPC_STATUS RPC_ENTRY UuidCreateSequential(UUID *uuid)
{
	static SRWLOCK lock = SRWLOCK_INIT;
	static ULONGLONG last_time;
	static WORD sequence;
	static BYTE node[6];
	AcquireSRWLockExclusive(&lock);
	if (!last_time) {
		UUID random;
		UuidCreate(&random);
		memcpy(node, random.Data4 + 2, sizeof(node));
		node[0] |= 0x01;
		sequence = (WORD)(random.Data4[0] << 8 | random.Data4[1]);
	}
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	ULONGLONG time = ((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime) + 0x0146bf33e42c000ULL;
	if (time <= last_time)
		time = last_time + 1;
	last_time = time;
	uuid->Data1 = (ULONG)time;
	uuid->Data2 = (USHORT)(time >> 32);
	uuid->Data3 = (USHORT)((time >> 48 & 0x0fff) | 0x1000);
	uuid->Data4[0] = (BYTE)((sequence >> 8 & 0x3f) | 0x80);
	uuid->Data4[1] = (BYTE)sequence;
	memcpy(uuid->Data4 + 2, node, sizeof(node));
	ReleaseSRWLockExclusive(&lock);
	return RPC_S_UUID_LOCAL_ONLY;
}

// The algorithm is the one DCE publishes, so a hash agrees with other implementations.
unsigned short RPC_ENTRY UuidHash(UUID *uuid, RPC_STATUS *status)
{
	*status = RPC_S_OK;
	const BYTE *bytes = (const BYTE *)(uuid ? uuid : &nil_uuid);
	short c0 = 0, c1 = 0;
	for (SIZE_T i = 0; i < sizeof(UUID); i++) {
		c0 += bytes[i];
		c1 += c0;
	}
	short x = -c1 % 255, y = (c1 - c0) % 255;
	if (x < 0)
		x += 255;
	if (y < 0)
		y += 255;
	return (unsigned short)(y * 256 + x);
}

static INT hex_digit(WCHAR c)
{
	if (c >= L'0' && c <= L'9')
		return c - L'0';
	if (c >= L'a' && c <= L'f')
		return c - L'a' + 10;
	if (c >= L'A' && c <= L'F')
		return c - L'A' + 10;
	return -1;
}

RPC_STATUS RPC_ENTRY UuidFromStringW(RPC_WSTR text, UUID *uuid)
{
	if (!text) {
		*uuid = nil_uuid;
		return RPC_S_OK;
	}
	if (lstrlenW(text) != 36)
		return RPC_S_INVALID_STRING_UUID;
	BYTE parts[16];
	static const BYTE where[16] = {0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34};
	if (text[8] != L'-' || text[13] != L'-' || text[18] != L'-' || text[23] != L'-')
		return RPC_S_INVALID_STRING_UUID;
	for (INT i = 0; i < 16; i++) {
		INT high = hex_digit(text[where[i]]), low = hex_digit(text[where[i] + 1]);
		if (high < 0 || low < 0)
			return RPC_S_INVALID_STRING_UUID;
		parts[i] = (BYTE)(high << 4 | low);
	}
	uuid->Data1 = (ULONG)parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
	uuid->Data2 = (USHORT)(parts[4] << 8 | parts[5]);
	uuid->Data3 = (USHORT)(parts[6] << 8 | parts[7]);
	memcpy(uuid->Data4, parts + 8, 8);
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY UuidFromStringA(RPC_CSTR text, UUID *uuid)
{
	WCHAR *wide = rpc_widen((const char *)text);
	RPC_STATUS status = UuidFromStringW(wide, uuid);
	rpc_free(wide);
	return status;
}

RPC_STATUS RPC_ENTRY UuidToStringW(const UUID *uuid, RPC_WSTR *text)
{
	const UUID *value = uuid ? uuid : &nil_uuid;
	WCHAR *out = HeapAlloc(GetProcessHeap(), 0, 37 * sizeof(WCHAR));
	if (!out)
		return RPC_S_OUT_OF_MEMORY;
	static const WCHAR digits[] = L"0123456789abcdef";
	WCHAR *at = out;
	#define PUT_HEX(value, count) for (INT shift = ((count) - 1) * 4; shift >= 0; shift -= 4) *at++ = digits[((value) >> shift) & 15]
	PUT_HEX(value->Data1, 8);
	*at++ = L'-';
	PUT_HEX(value->Data2, 4);
	*at++ = L'-';
	PUT_HEX(value->Data3, 4);
	*at++ = L'-';
	PUT_HEX(value->Data4[0], 2);
	PUT_HEX(value->Data4[1], 2);
	*at++ = L'-';
	for (INT i = 2; i < 8; i++)
		PUT_HEX(value->Data4[i], 2);
	#undef PUT_HEX
	*at = 0;
	*text = out;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY UuidToStringA(const UUID *uuid, RPC_CSTR *text)
{
	RPC_WSTR wide;
	RPC_STATUS status = UuidToStringW(uuid, &wide);
	if (status != RPC_S_OK)
		return status;
	*text = (RPC_CSTR)rpc_narrow(wide);
	rpc_free(wide);
	return *text ? RPC_S_OK : RPC_S_OUT_OF_MEMORY;
}

// ---- entries a program can call that have nothing to do here ----

RPC_STATUS RPC_ENTRY DllRegisterServer(void)
{
	return S_OK;
}

RPC_STATUS RPC_ENTRY RpcErrorStartEnumeration(RPC_ERROR_ENUM_HANDLE *handle)
{
	(void)handle;
	return RPC_S_ENTRY_NOT_FOUND;
}

RPC_STATUS RPC_ENTRY RpcErrorGetNextRecord(RPC_ERROR_ENUM_HANDLE *handle, BOOL copy, RPC_EXTENDED_ERROR_INFO *info)
{
	(void)handle; (void)copy; (void)info;
	return RPC_S_ENTRY_NOT_FOUND;
}

RPC_STATUS RPC_ENTRY RpcErrorEndEnumeration(RPC_ERROR_ENUM_HANDLE *handle)
{
	(void)handle;
	return RPC_S_OK;
}

RPC_STATUS RPC_ENTRY RpcErrorSaveErrorInfo(RPC_ERROR_ENUM_HANDLE *handle, PVOID *blob, size_t *size)
{
	(void)handle; (void)blob; (void)size;
	return RPC_S_ENTRY_NOT_FOUND;
}

RPC_STATUS RPC_ENTRY RpcErrorLoadErrorInfo(PVOID blob, size_t size, RPC_ERROR_ENUM_HANDLE *handle)
{
	(void)blob; (void)size; (void)handle;
	return RPC_S_ENTRY_NOT_FOUND;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
