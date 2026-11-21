#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

typedef LONG (WINAPI *query_process)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG (WINAPI *copy_memory)(HANDLE, void *, void *, SIZE_T, SIZE_T *);
typedef LONG (WINAPI *open_thread)(HANDLE *, DWORD, void *, void *);

struct thread_attributes {
	ULONG length;
	HANDLE root;
	void *name;
	ULONG attributes;
	void *security, *quality;
};

struct thread_information {
	LONG exit_status;
	void *teb;
	ULONG_PTR pid, tid, affinity;
	LONG priority, base_priority;
};

struct basic_information {
	LONG exit_status;
	void *peb;
	ULONG_PTR affinity;
	LONG priority;
	ULONG_PTR pid, parent;
};

static volatile DWORD child_value = 0x13579bdf;
static volatile BYTE child_buffer[120017];
static BYTE transferred[120017];

static int test_image_name(HANDLE process, const WCHAR *expected)
{
	WCHAR path[512];
	char narrow[512], expected_a[512];
	DWORD size = 512, length = 0;
	while (expected[length]) ++length;
	if (!QueryFullProcessImageNameW(process, 0, path, &size) || size != length || path[size]) return 100;
	for (unsigned index = 0; index < length; ++index)
		if (path[index] != expected[index]) return 101;
	const int bytes = WideCharToMultiByte(CP_ACP, 0, expected, -1, expected_a, sizeof expected_a, 0, 0);
	size = sizeof narrow;
	if (!bytes || !QueryFullProcessImageNameA(process, 0, narrow, &size) || size != (DWORD)bytes - 1) return 102;
	for (unsigned index = 0; index < (unsigned)bytes; ++index)
		if (narrow[index] != expected_a[index]) return 103;
	return 0;
}

static int test_handle_count(query_process query, HANDLE process, BOOL own)
{
	DWORD before, during, count;
	DWORD handles[256];
	ULONG length;
	HANDLE event, sent, received;
	if (!GetProcessHandleCount(process, &before) || query(process, 20, &count, sizeof(count), &length) || count != before || length != sizeof(count))
		return 90;
	event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!event || !GetProcessHandleCount(process, &during) || during != before + (own ? 1 : 0) ||
		!DuplicateHandle(GetCurrentProcess(), event, process, &sent, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
		!GetProcessHandleCount(process, &during) || during != before + (own ? 2 : 1))
		return 91;
	if (query(process, 58, handles, sizeof(handles), &length) || length != during * sizeof(DWORD))
		return 97;
	BOOL found = FALSE;
	for (unsigned index = 0; index < length / sizeof(DWORD); ++index) {
		if (handles[index] == (DWORD)(ULONG_PTR)sent) found = TRUE;
		if (!handles[index] || (index && handles[index] <= handles[index - 1])) return 98;
	}
	if (!found || query(process, 58, handles, sizeof(DWORD), &length) || length != sizeof(DWORD) || !handles[0] ||
		(ULONG)query(process, 58, (void *)1, sizeof(DWORD), &length) != 0xc0000005)
		return 99;
	if (!DuplicateHandle(process, sent, GetCurrentProcess(), &received, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE) ||
		!GetProcessHandleCount(process, &during) || during != before + (own ? 2 : 0))
		return 92;
	CloseHandle(received);
	CloseHandle(event);
	if (!GetProcessHandleCount(process, &during) || during != before)
		return 93;
	return 0;
}

#ifdef _WIN64
static int test_thread_thunk(open_thread open, ULONG_PTR *client)
{
	BYTE original[11], *source = (BYTE *)open;
	BYTE *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	HANDLE handle = NULL;
	DWORD previous, ignored;
	LONG patched, copied;
	unsigned index;
	if (!code || source[0] != 0x4c || source[1] != 0x8b || source[2] != 0xd1 || source[3] != 0xb8 ||
		source[8] != 0x0f || source[9] != 0x05 || source[10] != 0xc3)
		return 64;
	for (index = 0; index < sizeof(original); ++index)
		code[index] = original[index] = source[index];
	if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &previous) ||
		!VirtualProtect(source, sizeof(original), PAGE_EXECUTE_WRITECOPY, &previous))
		return 65;
	source[0] = 0xb8;
	source[1] = 0x22;
	source[2] = source[3] = 0;
	source[4] = 0xc0;
	source[5] = 0xc3;
	FlushInstructionCache(GetCurrentProcess(), source, sizeof(original));
	patched = open(&handle, THREAD_QUERY_INFORMATION, NULL, client);
	copied = ((open_thread)code)(&handle, THREAD_QUERY_INFORMATION, NULL, client);
	for (index = 0; index < sizeof(original); ++index)
		source[index] = original[index];
	VirtualProtect(source, sizeof(original), previous, &ignored);
	FlushInstructionCache(GetCurrentProcess(), source, sizeof(original));
	VirtualFree(code, 0, MEM_RELEASE);
	if ((ULONG)patched != 0xc0000022 || copied || !handle || GetThreadId(handle) != client[1])
		return 66;
	CloseHandle(handle);
	return 0;
}
#endif

static int test_open_thread(HMODULE ntdll)
{
	open_thread open = (open_thread)GetProcAddress(ntdll, "NtOpenThread");
	query_process query = (query_process)GetProcAddress(ntdll, "NtQueryInformationThread");
	struct thread_attributes attributes = {sizeof(attributes), NULL, NULL, 2, NULL, NULL};
	struct thread_information information;
	ULONG_PTR client[2] = {GetCurrentProcessId(), GetCurrentThreadId()};
	HANDLE handle = NULL, sentinel = (HANDLE)(ULONG_PTR)0x12345678;
	ULONG length = 0;
	DWORD flags = 0, previous;
	void *readonly;
	if (!open || !query)
		return 50;
	if (open(&handle, THREAD_QUERY_INFORMATION, &attributes, client) || !handle || GetThreadId(handle) != client[1] ||
		!GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT))
		return 51;
	if (query(handle, 0, &information, sizeof(information), &length) || length != sizeof(information) ||
		information.pid != client[0] || information.tid != client[1] || !information.teb || information.exit_status != 259)
		return 52;
	if ((ULONG)query(handle, 0, (void *)1, sizeof(information), &length) != 0xc0000005 ||
		(ULONG)query(handle, 0, &information, sizeof(information), (ULONG *)1) != 0xc0000005)
		return 53;
	CloseHandle(handle);
	if (open(&handle, READ_CONTROL, NULL, client) ||
		(ULONG)query(handle, 0, &information, sizeof(information), &length) != 0xc0000022)
		return 54;
	CloseHandle(handle);
	handle = OpenThread(GENERIC_READ, FALSE, (DWORD)client[1]);
	if (!handle || query(handle, 0, &information, sizeof(information), &length) ||
		!GetHandleInformation(handle, &flags) || (flags & HANDLE_FLAG_INHERIT))
		return 55;
	CloseHandle(handle);
	handle = sentinel;
	client[0] = GetCurrentProcessId() + 1;
	if ((ULONG)open(&handle, THREAD_ALL_ACCESS, NULL, client) != 0xc000000b || handle != sentinel)
		return 56;
	client[0] = GetCurrentProcessId();
	client[1] = 0xffffffff;
	if ((ULONG)open(&handle, THREAD_ALL_ACCESS, NULL, client) != 0xc000000b || handle != sentinel)
		return 57;
	client[1] = GetCurrentThreadId();
	if ((ULONG)open(NULL, THREAD_ALL_ACCESS, NULL, client) != 0xc0000005 ||
		(ULONG)open((HANDLE *)1, THREAD_ALL_ACCESS, NULL, client) != 0xc0000005 ||
		(ULONG)open(&handle, THREAD_ALL_ACCESS, NULL, (void *)1) != 0xc0000005 || handle != sentinel)
		return 58;
	attributes.length = 0;
	if ((ULONG)open(&handle, THREAD_ALL_ACCESS, &attributes, client) != 0xc000000d || handle != sentinel)
		return 59;
	attributes.length = sizeof(attributes);
	attributes.name = (void *)1;
	if ((ULONG)open(&handle, THREAD_ALL_ACCESS, &attributes, client) != 0xc000000d || handle != sentinel)
		return 60;
	attributes.name = NULL;
	attributes.attributes = 0x80000000;
	if ((ULONG)open(&handle, THREAD_ALL_ACCESS, &attributes, client) != 0xc00000bb || handle != sentinel)
		return 61;
	readonly = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!readonly || !VirtualProtect(readonly, 4096, PAGE_READONLY, &previous))
		return 62;
	if ((ULONG)open((HANDLE *)readonly, THREAD_ALL_ACCESS, NULL, client) != 0xc0000005 ||
		(ULONG)query(GetCurrentThread(), 0, readonly, sizeof(information), &length) != 0xc0000005)
		return 63;
	VirtualFree(readonly, 0, MEM_RELEASE);
#ifdef _WIN64
	return test_thread_thunk(open, client);
#else
	return 0;
#endif
}

static int test_allocation(HANDLE process)
{
	MEMORY_BASIC_INFORMATION information;
	DWORD value = 1234, read_back = 1, previous = 0;
	SIZE_T copied = 0;
	void *block = VirtualAllocEx(process, NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!block || !ReadProcessMemory(process, block, &read_back, sizeof(read_back), &copied) || copied != sizeof(read_back) || read_back)
		return 30;
	if (!WriteProcessMemory(process, block, &value, sizeof(value), &copied) || copied != sizeof(value) ||
		!VirtualProtectEx(process, block, 8192, PAGE_READONLY, &previous) || previous != PAGE_READWRITE)
		return 31;
	if (VirtualQueryEx(process, block, &information, sizeof(information)) != sizeof(information) || information.BaseAddress != block ||
		information.RegionSize != 8192 || information.State != MEM_COMMIT || information.Protect != PAGE_READONLY)
		return 32;
	if (WriteProcessMemory(process, block, &value, sizeof(value), &copied) || GetLastError() != ERROR_PARTIAL_COPY || copied ||
		!ReadProcessMemory(process, block, &read_back, sizeof(read_back), &copied) || read_back != value)
		return 33;
	if (VirtualProtectEx(process, block, 8192, PAGE_READWRITE, NULL) ||
		VirtualQueryEx(process, block, &information, sizeof(information)) != sizeof(information) || information.Protect != PAGE_READONLY)
		return 34;
	if (!VirtualFreeEx(process, block, 0, MEM_RELEASE) || VirtualQueryEx(process, block, &information, sizeof(information)) != sizeof(information) ||
		information.State != MEM_FREE)
		return 35;
	return 0;
}

static LONGLONG elapsed_ms(LARGE_INTEGER start)
{
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	return (now.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart;
}

/* A process whose threads all wait answers the calls of another process at once: these 125 calls take a few milliseconds. A process
 * that answers only when its scheduler wakes on its own (every 20 ms) takes 2.5 s. */
static int test_prompt_calls(HANDLE process)
{
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	void *block = VirtualAllocEx(process, NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	LARGE_INTEGER start;
	unsigned round;
	if (!event || !block) {
		if (event)
			CloseHandle(event);
		return 80;
	}
	QueryPerformanceCounter(&start);
	for (round = 0; round < 25; ++round) {
		HANDLE sent = NULL, back = NULL;
		SIZE_T copied = 0;
		void *page;
		if (!DuplicateHandle(GetCurrentProcess(), event, process, &sent, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
			!DuplicateHandle(process, sent, GetCurrentProcess(), &back, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE) ||
			!CloseHandle(back) || !WriteProcessMemory(process, block, &round, sizeof(round), &copied) || copied != sizeof(round) ||
			!(page = VirtualAllocEx(process, NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) || !VirtualFreeEx(process, page, 0, MEM_RELEASE)) {
			CloseHandle(event);
			return 81;
		}
	}
	CloseHandle(event);
	if (elapsed_ms(start) > 1000)
		return 82;
	return 0;
}

static int test_child(query_process query, copy_memory read_memory, copy_memory write_memory, PROCESS_INFORMATION *child)
{
	open_thread open = (open_thread)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenProcess");
	struct basic_information information;
	ULONG length = 0;
	SIZE_T copied = 0;
	ULONG_PTR image = 0, address, buffer;
	DWORD value = 0, replacement = 0x2468ace0, exit_code = 0;
	HANDLE limited;
	unsigned index;
	int result;
	{
		ULONG_PTR client[2] = {child->dwProcessId, 0};
		HANDLE handle = NULL;
		if (!open || open(&handle, PROCESS_QUERY_INFORMATION, NULL, client) ||
			query(handle, 0, &information, sizeof(information), &length) || information.pid != child->dwProcessId)
			return 72;
		CloseHandle(handle);
	}
	if (query(child->hProcess, 0, &information, sizeof(information), &length) || length != sizeof(information) ||
		information.pid != child->dwProcessId || information.parent != GetCurrentProcessId() || information.exit_status != 259 || !information.peb)
		return 10;
	if (!ReadProcessMemory(child->hProcess, (BYTE *)information.peb + 2 * sizeof(void *), &image, sizeof(image), &copied) ||
		copied != sizeof(image) || !image)
		return 11;
	address = image + (ULONG_PTR)&child_value - (ULONG_PTR)GetModuleHandleW(NULL);
	buffer = image + (ULONG_PTR)child_buffer - (ULONG_PTR)GetModuleHandleW(NULL);
	if (read_memory(child->hProcess, (void *)address, &value, sizeof(value), &copied) || copied != sizeof(value) || value != child_value)
		return 12;
	if (write_memory(child->hProcess, (void *)address, &replacement, sizeof(replacement), &copied) || copied != sizeof(replacement))
		return 13;
	if (!ReadProcessMemory(child->hProcess, (void *)address, &value, sizeof(value), &copied) || copied != sizeof(value) || value != replacement)
		return 14;
	for (index = 0; index < sizeof(transferred); ++index)
		transferred[index] = (BYTE)(index * 17 + 3);
	if (!WriteProcessMemory(child->hProcess, (void *)buffer, transferred, sizeof(transferred), &copied) || copied != sizeof(transferred))
		return 15;
	for (index = 0; index < sizeof(transferred); ++index)
		transferred[index] = 0;
	if (!ReadProcessMemory(child->hProcess, (void *)buffer, transferred, sizeof(transferred), &copied) || copied != sizeof(transferred))
		return 16;
	for (index = 0; index < sizeof(transferred); ++index)
		if (transferred[index] != (BYTE)(index * 17 + 3))
			return 17;
	if (ReadProcessMemory(child->hProcess, (void *)1, &value, sizeof(value), &copied) || GetLastError() != ERROR_PARTIAL_COPY || copied)
		return 18;
	if (WriteProcessMemory(child->hProcess, (void *)1, &value, sizeof(value), &copied) || GetLastError() != ERROR_PARTIAL_COPY || copied)
		return 19;
	limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child->dwProcessId);
	if (!limited)
		return 20;
	if (ReadProcessMemory(limited, (void *)address, &value, sizeof(value), &copied) || GetLastError() != ERROR_ACCESS_DENIED || copied ||
		WriteProcessMemory(limited, (void *)address, &replacement, sizeof(replacement), &copied) || GetLastError() != ERROR_ACCESS_DENIED || copied) {
		CloseHandle(limited);
		return 21;
	}
	if (query(limited, 0, &information, sizeof(information), &length)) {
		CloseHandle(limited);
		return 22;
	}
	if (VirtualAllocEx(limited, NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) || GetLastError() != ERROR_ACCESS_DENIED) {
		CloseHandle(limited);
		return 24;
	}
	CloseHandle(limited);
	result = test_allocation(child->hProcess);
	if (result)
		return result;
#ifdef _WIN64
	{
		BYTE patch[6] = {0xb8, 0x22, 0, 0, 0xc0, 0xc3};
		MEMORY_BASIC_INFORMATION memory;
		DWORD previous, ignored;
		void *target = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenThread");
		if (VirtualQueryEx(child->hProcess, target, &memory, sizeof(memory)) != sizeof(memory) || memory.Type != MEM_IMAGE ||
			!VirtualProtectEx(child->hProcess, target, sizeof(patch), PAGE_WRITECOPY, &previous))
			return 70;
		if (!WriteProcessMemory(child->hProcess, target, patch, sizeof(patch), &copied) || copied != sizeof(patch) ||
			!VirtualProtectEx(child->hProcess, target, sizeof(patch), previous, &ignored) || ignored != PAGE_WRITECOPY)
			return 71;
	}
#endif
	if (WaitForSingleObject(child->hProcess, 0) != WAIT_TIMEOUT || ResumeThread(child->hThread) != 1 ||
		WaitForSingleObject(child->hProcess, 15000) != WAIT_OBJECT_0 || !GetExitCodeProcess(child->hProcess, &exit_code) || exit_code)
		return 23;
	return 0;
}

static void reply_pipe_name(WCHAR *name, DWORD owner)
{
	static const WCHAR prefix[] = L"\\\\.\\pipe\\raven-remote-reply-";
	unsigned at = 0, digits = 0;
	DWORD rest = owner;
	for (; prefix[at]; ++at)
		name[at] = prefix[at];
	do ++digits; while (rest /= 10);
	name[at + digits] = 0;
	do name[at + --digits] = (WCHAR)(L'0' + owner % 10); while (owner /= 10);
}

/* The child started with --r: sends back each byte its parent writes, until a zero, with overlapped I/O as a message loop does. */
static int echo_replies(query_process query)
{
	struct basic_information information;
	ULONG length = 0;
	WCHAR name[64];
	OVERLAPPED overlapped = {0};
	HANDLE pipe;
	if (query(GetCurrentProcess(), 0, &information, sizeof(information), &length))
		return 2;
	reply_pipe_name(name, (DWORD)information.parent);
	pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (pipe == INVALID_HANDLE_VALUE || !overlapped.hEvent)
		return 3;
	for (;;) {
		BYTE value = 0xff;
		DWORD count = 0;
		if ((!ReadFile(pipe, &value, 1, &count, &overlapped) && (GetLastError() != ERROR_IO_PENDING ||
			!GetOverlappedResult(pipe, &overlapped, &count, TRUE))) || count != 1)
			return 4;
		if ((!WriteFile(pipe, &value, 1, &count, &overlapped) && (GetLastError() != ERROR_IO_PENDING ||
			!GetOverlappedResult(pipe, &overlapped, &count, TRUE))) || count != 1)
			return 5;
		if (!value)
			return 0;
	}
}

/* A reply over a pipe reaches a thread that waits for it while nothing else in its process runs at once: 40 round trips take a few
 * milliseconds. A process that finds finished host I/O only when its scheduler wakes on its own (every 20 ms) takes 800 ms. */
static int test_pipe_replies(WCHAR *command, unsigned used, STARTUPINFOW *startup)
{
	WCHAR name[64];
	OVERLAPPED overlapped = {0};
	PROCESS_INFORMATION child;
	LARGE_INTEGER start;
	DWORD exit_code = 1, count;
	HANDLE pipe;
	unsigned round;
	int result = 0;
	reply_pipe_name(name, GetCurrentProcessId());
	pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 64, 64, 0, NULL);
	overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (pipe == INVALID_HANDLE_VALUE || !overlapped.hEvent)
		return 83;
	command[used - 1] = 'r';
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, startup, &child)) {
		CloseHandle(pipe);
		return 83;
	}
	if (!ConnectNamedPipe(pipe, &overlapped) && GetLastError() != ERROR_PIPE_CONNECTED &&
		(GetLastError() != ERROR_IO_PENDING || WaitForSingleObject(overlapped.hEvent, 15000) != WAIT_OBJECT_0))
		result = 84;
	/* The child waits for the first byte now, and nothing else runs in it. */
	if (!result)
		result = test_prompt_calls(child.hProcess);
	QueryPerformanceCounter(&start);
	for (round = 0; !result && round < 40; ++round) {
		BYTE value = (BYTE)(round == 39 ? 0 : round + 1), back = 0xff;
		if ((!WriteFile(pipe, &value, 1, &count, &overlapped) && (GetLastError() != ERROR_IO_PENDING ||
			!GetOverlappedResult(pipe, &overlapped, &count, TRUE))) || count != 1)
			result = 85;
		else if (!ReadFile(pipe, &back, 1, &count, &overlapped) && (GetLastError() != ERROR_IO_PENDING ||
			WaitForSingleObject(overlapped.hEvent, 5000) != WAIT_OBJECT_0 || !GetOverlappedResult(pipe, &overlapped, &count, FALSE)))
			result = 85;
		else if (back != value)
			result = 86;
	}
	if (!result && elapsed_ms(start) > 300)
		result = 87;
	/* A read left pending writes into this frame when it ends: it ends before the frame does, and the child with it. */
	if (result) {
		CancelIoEx(pipe, NULL);
		GetOverlappedResult(pipe, &overlapped, &count, TRUE);
		TerminateProcess(child.hProcess, 99);
	}
	if (WaitForSingleObject(child.hProcess, 15000) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &exit_code) || (!result && exit_code))
		result = result ? result : 88;
	/* The child ended and its handle is still open, so the channel to it has hung up: that must not keep this process busy. A process
	 * that wakes for it again and again uses the whole 500 ms; a quiet one uses a few. */
	if (!result) {
		FILETIME created, ended, kernel, user;
		ULONGLONG before, after;
		if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
			result = 89;
		before = ((ULONGLONG)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) + ((ULONGLONG)user.dwHighDateTime << 32 | user.dwLowDateTime);
		Sleep(500);
		if (!result && !GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
			result = 89;
		after = ((ULONGLONG)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) + ((ULONGLONG)user.dwHighDateTime << 32 | user.dwLowDateTime);
		if (!result && after - before > 800000)
			result = 89;
	}
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	CloseHandle(overlapped.hEvent);
	CloseHandle(pipe);
	return result;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	query_process query = (query_process)GetProcAddress(ntdll, "NtQueryInformationProcess");
	copy_memory read_memory = (copy_memory)GetProcAddress(ntdll, "NtReadVirtualMemory");
	copy_memory write_memory = (copy_memory)GetProcAddress(ntdll, "NtWriteVirtualMemory");
	open_thread open_process = (open_thread)GetProcAddress(ntdll, "NtOpenProcess");
	struct basic_information information;
	STARTUPINFOW startup;
	PROCESS_INFORMATION child;
	WCHAR command[512], source[512], child_image[512], child_directory[512];
	ULONG length = 0;
	ULONG_PTR peb = 0;
	DWORD used;
	DWORD value = 123, replacement = 456, count;
	HANDLE limited;
	SIZE_T copied;
	unsigned index;
	int result;
	for (index = 0; line[index]; ++index)
#ifdef _WIN64
		if (line[index] == '-' && line[index + 1] == '-' && line[index + 2] == 'u') {
			__asm__ volatile("mov $39, %%eax; syscall" : : : "rax", "rcx", "r11", "memory");
			return 0;
		} else
#endif
		if (line[index] == '-' && line[index + 1] == '-' && line[index + 2] == 'r') {
			return echo_replies(query);
		} else
		if (line[index] == '-' && line[index + 1] == '-' && line[index + 2] == 'c') {
#ifdef _WIN64
			ULONG_PTR client[2] = {GetCurrentProcessId(), GetCurrentThreadId()};
			HANDLE handle;
			open_thread open = (open_thread)GetProcAddress(ntdll, "NtOpenThread");
			if (!open || (ULONG)open(&handle, THREAD_QUERY_INFORMATION, NULL, client) != 0xc0000022)
				return 92;
#endif
			if (child_value != 0x2468ace0)
				return 90;
			for (index = 0; index < sizeof(child_buffer); ++index)
				if (child_buffer[index] != (BYTE)(index * 17 + 3))
					return 91;
			return 0;
		}
	if (!query || !read_memory || !write_memory || !open_process)
		return 1;
	result = test_handle_count(query, GetCurrentProcess(), TRUE);
	if (result) return result;
	if (GetProcessHandleCount((HANDLE)(ULONG_PTR)0x12345678, &count) || GetLastError() != ERROR_INVALID_HANDLE ||
		GetProcessHandleCount(GetCurrentProcess(), (DWORD *)1) || GetLastError() != ERROR_NOACCESS)
		return 94;
	limited = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	if (!limited || GetProcessHandleCount(limited, &count) || GetLastError() != ERROR_ACCESS_DENIED) return 95;
	CloseHandle(limited);
	limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	if (!limited || !GetProcessHandleCount(limited, &count)) return 96;
	CloseHandle(limited);
	result = test_open_thread(ntdll);
	if (result)
		return result;
	{
		ULONG_PTR client[2] = {GetCurrentProcessId(), 0};
		struct thread_attributes attributes = {sizeof(attributes), NULL, NULL, 2, NULL, NULL};
		HANDLE handle = NULL, sentinel = (HANDLE)(ULONG_PTR)0x12345678;
		DWORD flags = 0;
		if (open_process(&handle, GENERIC_READ, &attributes, client) || !GetHandleInformation(handle, &flags) ||
			!(flags & HANDLE_FLAG_INHERIT) || query(handle, 0, &information, sizeof(information), &length) || information.pid != client[0])
			return 73;
		CloseHandle(handle);
		if (open_process(&handle, READ_CONTROL, NULL, client) ||
			(ULONG)query(handle, 0, &information, sizeof(information), &length) != 0xc0000022)
			return 74;
		CloseHandle(handle);
		handle = sentinel;
		if ((ULONG)open_process(NULL, GENERIC_READ, NULL, client) != 0xc0000005 ||
			(ULONG)open_process((HANDLE *)1, GENERIC_READ, NULL, client) != 0xc0000005 ||
			(ULONG)open_process(&handle, GENERIC_READ, NULL, (void *)1) != 0xc0000005 || handle != sentinel)
			return 75;
		if ((ULONG)open_process(&handle, GENERIC_READ, NULL, NULL) != 0xc0000030 || handle != sentinel)
			return 76;
		attributes.name = (void *)1;
		if ((ULONG)open_process(&handle, GENERIC_READ, &attributes, client) != 0xc0000030 || handle != sentinel)
			return 77;
		client[0] = 0xffffffff;
		if ((ULONG)open_process(&handle, GENERIC_READ, NULL, client) != 0xc000000b || handle != sentinel)
			return 78;
	}
#ifdef _WIN64
	__asm__("movq %%gs:0x60,%0" : "=r"(peb));
#else
	__asm__("movl %%fs:0x30,%0" : "=r"(peb));
#endif
	if (query(GetCurrentProcess(), 0, &information, sizeof(information), &length) || length != sizeof(information) ||
		(ULONG_PTR)information.peb != peb || information.pid != GetCurrentProcessId() || !information.affinity || information.priority != 8)
		return 2;
	if ((ULONG)query(GetCurrentProcess(), 0, &information, sizeof(information) - 1, &length) != 0xc0000004 || length != sizeof(information))
		return 3;
	if ((ULONG)query((HANDLE)(ULONG_PTR)0x12345678, 0, &information, sizeof(information), &length) != 0xc0000008)
		return 4;
	if ((ULONG)query(GetCurrentProcess(), 0xffffffff, &information, sizeof(information), &length) != 0xc0000003)
		return 5;
	if ((ULONG)query(GetCurrentProcess(), 0, NULL, sizeof(information), &length) != 0xc0000005)
		return 6;
	if ((ULONG)query(GetCurrentProcess(), 0, (void *)1, sizeof(information), &length) != 0xc0000005 ||
		(ULONG)query(GetCurrentProcess(), 0, &information, sizeof(information), (ULONG *)1) != 0xc0000005)
		return 42;
	if ((ULONG)read_memory(GetCurrentProcess(), &value, &replacement, sizeof(value), (SIZE_T *)1) != 0xc0000005 || replacement != 456)
		return 43;
	if ((ULONG)write_memory(GetCurrentProcess(), &value, &replacement, sizeof(value), (SIZE_T *)1) != 0xc0000005 || value != 123)
		return 44;
	limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	if (!limited || (ULONG)read_memory(limited, &value, &replacement, sizeof(value), &copied) != 0xc0000022 || copied ||
		ReadProcessMemory(limited, &value, &replacement, sizeof(value), &copied) || GetLastError() != ERROR_ACCESS_DENIED ||
		VirtualAllocEx(limited, NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) || GetLastError() != ERROR_ACCESS_DENIED)
		return 45;
	CloseHandle(limited);
	limited = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	if (!limited || (ULONG)query(limited, 0, &information, sizeof(information), &length) != 0xc0000022)
		return 46;
	CloseHandle(limited);
	{
		MEMORY_BASIC_INFORMATION memory;
		void *block = VirtualAllocEx(GetCurrentProcess(), NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!block || VirtualProtectEx(GetCurrentProcess(), block, 4096, PAGE_READONLY, (DWORD *)block) ||
			VirtualQueryEx(GetCurrentProcess(), block, &memory, sizeof(memory)) != sizeof(memory) || memory.Protect != PAGE_READWRITE ||
			!VirtualFreeEx(GetCurrentProcess(), block, 0, MEM_RELEASE))
			return 47;
	}
	for (index = 0; index < sizeof(startup); ++index)
		((BYTE *)&startup)[index] = 0;
	startup.cb = sizeof(startup);
	command[0] = '"';
	used = GetModuleFileNameW(NULL, source, 400);
	if (!used || used >= 400)
		return 7;
	for (index = 0; index < used; ++index) child_directory[index] = source[index];
	const WCHAR suffix[] = L".image-\u00e9";
	for (index = 0; index < sizeof suffix / sizeof(WCHAR); ++index) child_directory[used + index] = suffix[index];
	if (!CreateDirectoryW(child_directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 105;
	DWORD name = used;
	while (name && source[name - 1] != '\\') --name;
	used += sizeof suffix / sizeof(WCHAR) - 1;
	if (used + 1 + lstrlenW(source + name) >= 500) return 106;
	for (index = 0; index < used; ++index) child_image[index] = child_directory[index];
	child_image[used++] = '\\';
	for (index = name; source[index]; ++index) child_image[used++] = source[index];
	child_image[used] = 0;
	if (!CopyFileW(source, child_image, FALSE)) return 104;
	for (index = 0; index < used; ++index) command[index + 1] = child_image[index];
	command[++used] = '"';
	command[++used] = ' ';
	command[++used] = '-';
	command[++used] = '-';
	command[++used] = 'c';
	command[++used] = 0;
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &child)) {
		DeleteFileW(child_image);
		RemoveDirectoryW(child_directory);
		return 8;
	}
	result = test_image_name(child.hProcess, child_image);
	if (!result) result = test_handle_count(query, child.hProcess, FALSE);
	if (!result) result = test_child(query, read_memory, write_memory, &child);
	if (result) {
		TerminateProcess(child.hProcess, 99);
		WaitForSingleObject(child.hProcess, 5000);
	}
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);
	if (!result)
		result = test_pipe_replies(command, used, &startup);
#ifdef _WIN64
	if (!result) {
		DWORD exit_code = 0;
		command[used - 1] = 'u';
		if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child))
			return 67;
		if (WaitForSingleObject(child.hProcess, 15000) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &exit_code) || exit_code != 1)
			result = 68;
		CloseHandle(child.hThread);
		CloseHandle(child.hProcess);
	}
#endif
	DeleteFileW(child_image);
	RemoveDirectoryW(child_directory);
	return result;
}
