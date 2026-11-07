/* The running processes, as TASKLIST shows them and TASKKILL ends them: the tool help snapshot, with the memory each one uses, and the
   filters /FI takes. */

#include <tlhelp32.h>
#include <psapi.h>

struct Process {
	DWORD id, parent;
	WCHAR name[MAX_PATH];
	ULONGLONG memory;
};

struct Processes {
	struct Process *items;
	DWORD count;
};

typedef HANDLE(WINAPI *TakeSnapshot)(DWORD, DWORD);
typedef BOOL(WINAPI *WalkProcesses)(HANDLE, LPPROCESSENTRY32W);
typedef BOOL(WINAPI *MemoryInfo)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);

static BOOL list_processes(struct Processes *list)
{
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	TakeSnapshot take = kernel ? (TakeSnapshot)(void (*)(void))GetProcAddress(kernel, "CreateToolhelp32Snapshot") : NULL;
	WalkProcesses first = kernel ? (WalkProcesses)(void (*)(void))GetProcAddress(kernel, "Process32FirstW") : NULL;
	WalkProcesses next = kernel ? (WalkProcesses)(void (*)(void))GetProcAddress(kernel, "Process32NextW") : NULL;
	MemoryInfo memory = kernel ? (MemoryInfo)(void (*)(void))GetProcAddress(kernel, "K32GetProcessMemoryInfo") : NULL;
	list->items = NULL;
	list->count = 0;
	if (!take || !first || !next)
		return FALSE;
	HANDLE snapshot = take(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return FALSE;
	DWORD capacity = 0;
	PROCESSENTRY32W entry;
	memset(&entry, 0, sizeof entry);
	entry.dwSize = sizeof entry;
	for (BOOL more = first(snapshot, &entry); more; more = next(snapshot, &entry)) {
		if (list->count == capacity) {
			capacity = capacity ? capacity * 2 : 32;
			struct Process *grown = allocate(capacity * sizeof *grown);
			if (list->count)
				memcpy(grown, list->items, list->count * sizeof *grown);
			release(list->items);
			list->items = grown;
		}
		struct Process *process = &list->items[list->count++];
		process->id = entry.th32ProcessID;
		process->parent = entry.th32ParentProcessID;
		lstrcpynW(process->name, entry.szExeFile, MAX_PATH);
		process->memory = 0;
		if (memory) {
			HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
			PROCESS_MEMORY_COUNTERS counters;
			memset(&counters, 0, sizeof counters);
			counters.cb = sizeof counters;
			if (handle && memory(handle, &counters, sizeof counters))
				process->memory = counters.WorkingSetSize;
			if (handle)
				CloseHandle(handle);
		}
		entry.dwSize = sizeof entry;
	}
	CloseHandle(snapshot);
	return TRUE;
}

/* A filter of /FI: "name operator value", such as "IMAGENAME eq cmd.exe" or "PID gt 100". */
struct Filter {
	enum { BY_NAME, BY_ID, BY_MEMORY, BY_SESSION, BY_OTHER } field;
	enum { EQ, NE, GT, LT, GE, LE } compare;
	WCHAR *value;
	ULONGLONG number;
};

static BOOL read_filter(const WCHAR *text, struct Filter *filter)
{
	WCHAR parts[3][MAX_PATH];
	DWORD part = 0;
	for (const WCHAR *at = text; *at && part < 3;) {
		while (blank(*at))
			++at;
		if (!*at)
			break;
		DWORD size = 0;
		/* The value is the rest of the text, blanks and all. */
		while (*at && (part == 2 || !blank(*at)) && size < MAX_PATH - 1)
			parts[part][size++] = *at++;
		while (size && blank(parts[part][size - 1]))
			--size;
		parts[part++][size] = 0;
	}
	if (part != 3)
		return FALSE;
	if (equal(parts[0], L"IMAGENAME"))
		filter->field = BY_NAME;
	else if (equal(parts[0], L"PID"))
		filter->field = BY_ID;
	else if (equal(parts[0], L"MEMUSAGE"))
		filter->field = BY_MEMORY;
	else if (equal(parts[0], L"SESSION"))
		filter->field = BY_SESSION;
	else if (equal(parts[0], L"STATUS") || equal(parts[0], L"USERNAME") || equal(parts[0], L"SESSIONNAME") || equal(parts[0], L"CPUTIME") ||
		equal(parts[0], L"WINDOWTITLE") || equal(parts[0], L"SERVICES") || equal(parts[0], L"MODULES"))
		filter->field = BY_OTHER;
	else
		return FALSE;
	static const WCHAR *const operators[] = {L"EQ", L"NE", L"GT", L"LT", L"GE", L"LE"};
	BOOL known = FALSE;
	for (int index = 0; index < 6; ++index)
		if (equal(parts[1], operators[index])) {
			filter->compare = index;
			known = TRUE;
		}
	if (!known)
		return FALSE;
	filter->value = copy_of(parts[2]);
	filter->number = 0;
	if (filter->field == BY_ID || filter->field == BY_MEMORY || filter->field == BY_SESSION)
		return read_number(filter->value, &filter->number);
	return TRUE;
}

static BOOL passes(const struct Filter *filter, const struct Process *process)
{
	int order;
	switch (filter->field) {
	case BY_NAME: order = matches(filter->value, process->name) ? 0 : 1; break;
	case BY_ID: order = process->id < filter->number ? -1 : process->id > filter->number; break;
	case BY_MEMORY: order = process->memory / 1024 < filter->number ? -1 : process->memory / 1024 > filter->number; break;
	case BY_SESSION: order = 1 < filter->number ? -1 : 1 > filter->number; break;
	default: return TRUE;
	}
	switch (filter->compare) {
	case EQ: return order == 0;
	case NE: return order != 0;
	case GT: return order > 0;
	case LT: return order < 0;
	case GE: return order >= 0;
	default: return order <= 0;
	}
}
