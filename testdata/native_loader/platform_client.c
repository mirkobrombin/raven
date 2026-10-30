/* Host services: known folders, the computer name, network adapters, data protection, security checks and job objects. */
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define PSAPI_VERSION 2
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <sddl.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <psapi.h>

static void clear(void *memory, int length)
{
	unsigned char *bytes = memory;
	int index;
	for (index = 0; index < length; ++index)
		bytes[index] = 0;
}

static int same(const void *left, const void *right, int length)
{
	const unsigned char *a = left, *b = right;
	int index;
	for (index = 0; index < length; ++index)
		if (a[index] != b[index])
			return 0;
	return 1;
}

static int contains_narrow(const char *text, char part)
{
	for (; *text; ++text)
		if (*text == part)
			return 1;
	return 0;
}

static int contains(const WCHAR *text, const WCHAR *part)
{
	int at, index;
	for (at = 0; text[at]; ++at) {
		for (index = 0; part[index] && text[at + index] == part[index]; ++index)
			;
		if (!part[index])
			return 1;
	}
	return 0;
}

/* Imported from ntdll by name, as the programs that set a key's information do. */
__declspec(dllimport) LONG WINAPI NtSetInformationKey(HANDLE, int, void *, ULONG);


/* The natives the virtualization layer of Click-to-Run Office finds by name in ntdll. */
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } counted_string;
typedef enum { avl_less, avl_more, avl_match } avl_order;
/* Opaque room for RTL_AVL_TABLE, which is smaller than this on both architectures. */
typedef struct { void *slots[32]; } avl_table_shape;
static avl_order __stdcall avl_compare(void *table, void *first, void *second)
{
	int left = *(int *)first, right = *(int *)second;
	return left < right ? avl_less : left > right ? avl_more : avl_match;
}
static void *__stdcall avl_allocate(void *table, ULONG size) { return HeapAlloc(GetProcessHeap(), 0, size); }
static void __stdcall avl_release(void *table, void *block) { HeapFree(GetProcessHeap(), 0, block); }
static void *native(const char *name) { return (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name); }

static int click_to_run_natives(HKEY key)
{
	BYTE details[512];
	ULONG size = 0;
	LONG (WINAPI *enumerate_key)(HANDLE, ULONG, int, void *, ULONG, ULONG *) = native("NtEnumerateKey");
	LONG (WINAPI *enumerate_value)(HANDLE, ULONG, int, void *, ULONG, ULONG *) = native("NtEnumerateValueKey");
	LONG (WINAPI *flush_key)(HANDLE) = native("NtFlushKey");
	if (!enumerate_key || !enumerate_value || !flush_key) return 1;
	RtlSecureZeroMemory(details, sizeof details);
	if (enumerate_key(key, 0, 0, details, sizeof details, &size) || size != 16 + 10 || lstrcmpW((WCHAR *)(details + 16), L"Inner")) return 2;
	if (enumerate_key(key, 1, 0, details, sizeof details, &size) != (LONG)0x8000001a) return 3;
	RtlSecureZeroMemory(details, sizeof details);
	if (enumerate_value(key, 0, 1, details, sizeof details, &size) || *(ULONG *)(details + 4) != REG_DWORD || *(ULONG *)(details + 12) != 4 ||
		*(ULONG *)(details + *(ULONG *)(details + 8)) != 7 || lstrcmpW((WCHAR *)(details + 20), L"Seven")) return 4;
	if (enumerate_value(key, 1, 0, details, sizeof details, &size) != (LONG)0x8000001a || flush_key(key)) return 5;

	LONG (WINAPI *compare)(const counted_string *, const counted_string *, BOOLEAN) = native("RtlCompareUnicodeString");
	BOOLEAN (WINAPI *equal)(const counted_string *, const counted_string *, BOOLEAN) = native("RtlEqualUnicodeString");
	BOOLEAN (WINAPI *prefix)(const counted_string *, const counted_string *, BOOLEAN) = native("RtlPrefixUnicodeString");
	void (WINAPI *copy)(counted_string *, const counted_string *) = native("RtlCopyUnicodeString");
	LONG (WINAPI *number)(ULONG, ULONG, counted_string *) = native("RtlIntegerToUnicodeString");
	BOOLEAN (WINAPI *in_expression)(const counted_string *, const counted_string *, BOOLEAN, WCHAR *) = native("RtlIsNameInExpression");
	if (!compare || !equal || !prefix || !copy || !number || !in_expression) return 6;
	WCHAR upper[] = L"OFFICE16", lower[] = L"office16\\winword.exe", room[8];
	counted_string first = {16, 18, upper}, second = {16, 40, lower}, longer = {38, 40, lower}, target = {0, sizeof room, room};
	if (!equal(&first, &second, TRUE) || equal(&first, &second, FALSE) || compare(&first, &second, TRUE) || compare(&first, &second, FALSE) >= 0) return 7;
	if (!prefix(&first, &longer, TRUE) || prefix(&longer, &first, TRUE)) return 8;
	copy(&target, &longer);
	if (target.Length != sizeof room || room[7] != L'6') return 9;
	if (number(255, 16, &target) || target.Length != 4 || lstrcmpW(room, L"FF") || number(1234567890, 10, &target) != (LONG)0x80000005) return 10;
	WCHAR expression[] = L"*.EXE", dos[] = L"WIN<.EXE", name_text[] = L"winword.exe", other[] = L"winword.dll";
	counted_string pattern = {10, 12, expression}, dos_pattern = {16, 18, dos}, name = {22, 24, name_text}, wrong = {22, 24, other};
	if (!in_expression(&pattern, &name, TRUE, NULL) || in_expression(&pattern, &wrong, TRUE, NULL) || in_expression(&pattern, &name, FALSE, NULL) ||
		!in_expression(&dos_pattern, &name, TRUE, NULL)) return 11;

	void (WINAPI *initialize)(avl_table_shape *, void *, void *, void *, void *) = native("RtlInitializeGenericTableAvl");
	void *(WINAPI *insert)(avl_table_shape *, void *, ULONG, BOOLEAN *) = native("RtlInsertElementGenericTableAvl");
	void *(WINAPI *lookup)(avl_table_shape *, void *) = native("RtlLookupElementGenericTableAvl");
	void *(WINAPI *walk)(avl_table_shape *, BOOLEAN) = native("RtlEnumerateGenericTableAvl");
	BOOLEAN (WINAPI *remove)(avl_table_shape *, void *) = native("RtlDeleteElementGenericTableAvl");
	BOOLEAN (WINAPI *empty)(avl_table_shape *) = native("RtlIsGenericTableEmptyAvl");
	if (!initialize || !insert || !lookup || !walk || !remove || !empty) return 12;
	avl_table_shape table;
	initialize(&table, avl_compare, avl_allocate, avl_release, NULL);
	BOOLEAN created = FALSE;
	for (int value = 5; value > 0; --value)
		if (!insert(&table, &value, sizeof value, &created) || !created) return 13;
	int wanted = 3, previous = 0, seen = 0;
	if (!lookup(&table, &wanted)) return 14;
	if (*(int *)lookup(&table, &wanted) != 3) return 23;
	if (!remove(&table, &wanted)) return 24;
	if (lookup(&table, &wanted)) return 25;
	for (int *entry = walk(&table, TRUE); entry; entry = walk(&table, FALSE), ++seen)
		if (*entry <= previous) return 15;
		else previous = *entry;
	if (seen != 4 || empty(&table)) return 16;

	LONG (WINAPI *duplicate)(HANDLE, HANDLE, HANDLE, HANDLE *, ULONG, ULONG, ULONG) = native("NtDuplicateObject");
	LONG (WINAPI *query_security)(HANDLE, ULONG, void *, ULONG, ULONG *) = native("NtQuerySecurityObject");
	LONG (WINAPI *open_job)(HANDLE *, ULONG, void *) = native("NtOpenJobObject");
	if (!duplicate || !query_security || !open_job) return 17;
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL), copy_handle = NULL;
	if (duplicate(GetCurrentProcess(), event, GetCurrentProcess(), &copy_handle, 0, 0, DUPLICATE_SAME_ACCESS) || !copy_handle || !SetEvent(copy_handle) ||
		WaitForSingleObject(event, 0) != WAIT_OBJECT_0) return 18;
	if (duplicate(GetCurrentProcess(), (HANDLE)(ULONG_PTR)0x7ffc, GetCurrentProcess(), &copy_handle, 0, 0, DUPLICATE_SAME_ACCESS) != (LONG)0xc0000008) return 19;
	if (query_security(event, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, details, 8, &size) != (LONG)0xc0000023 || size < 20) return 20;
	if (query_security(event, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, details, sizeof details, &size) ||
		!IsValidSecurityDescriptor(details)) return 21;
	HANDLE job = NULL;
	if (open_job(&job, 0, NULL) != (LONG)0xc0000034) return 22;
	CloseHandle(copy_handle);
	CloseHandle(event);
	return 0;
}


static int same_guid(const GUID *a, const GUID *b)
{
	const DWORD *left = (const DWORD *)a, *right = (const DWORD *)b;
	return left[0] == right[0] && left[1] == right[1] && left[2] == right[2] && left[3] == right[3];
}

/* The known folder manager, as the application virtualization of Click-to-Run Office asks it for the folders it turns into tokens. */
static int known_folder_manager(void)
{
	IKnownFolderManager *manager = NULL;
	IKnownFolder *folder = NULL;
	KNOWNFOLDER_DEFINITION definition;
	KNOWNFOLDERID id, *ids = NULL;
	UINT count = 0;
	WCHAR *path = NULL;
	int csidl = 0;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (CoCreateInstance(&CLSID_KnownFolderManager, NULL, CLSCTX_INPROC_SERVER, &IID_IKnownFolderManager, (void **)&manager) != S_OK || !manager)
		return 1;
	if (manager->lpVtbl->GetFolderIds(manager, &ids, &count) != S_OK || count < 20)
		return 2;
	CoTaskMemFree(ids);
	if (manager->lpVtbl->GetFolder(manager, &FOLDERID_ProgramFilesCommonX64, &folder) != S_OK || !folder)
		return 3;
	if (folder->lpVtbl->GetPath(folder, 0, &path) != S_OK || !contains(path, L"Program Files\\Common Files"))
		return 4;
	CoTaskMemFree(path);
	if (folder->lpVtbl->GetFolderDefinition(folder, &definition) != S_OK || definition.category != KF_CATEGORY_FIXED ||
		lstrcmpW(definition.pszName, L"ProgramFilesCommonX64"))
		return 5;
	FreeKnownFolderDefinitionFields(&definition);
	folder->lpVtbl->Release(folder);
	if (manager->lpVtbl->GetFolderByName(manager, L"common appdata", &folder) != S_OK || folder->lpVtbl->GetId(folder, &id) != S_OK ||
		!same_guid(&id, &FOLDERID_ProgramData))
		return 6;
	folder->lpVtbl->Release(folder);
	if (manager->lpVtbl->FolderIdFromCsidl(manager, CSIDL_SYSTEM, &id) != S_OK || !same_guid(&id, &FOLDERID_System) ||
		manager->lpVtbl->FolderIdToCsidl(manager, &FOLDERID_Fonts, &csidl) != S_OK || csidl != CSIDL_FONTS)
		return 7;
	if (manager->lpVtbl->FindFolderFromPath(manager, L"C:\\Windows\\System32\\drivers", FFFP_NEARESTPARENTMATCH, &folder) != S_OK ||
		folder->lpVtbl->GetId(folder, &id) != S_OK || !same_guid(&id, &FOLDERID_System))
		return 8;
	folder->lpVtbl->Release(folder);
	if (manager->lpVtbl->FindFolderFromPath(manager, L"C:\\Windows\\System32\\drivers", FFFP_EXACTMATCH, &folder) == S_OK)
		return 9;
	manager->lpVtbl->Release(manager);
	return 0;
}

/* A file an App-V package keeps under its Root\\VFS folder is seen at the folder the token names, while the package root is registered. */
static int virtual_package_files(void)
{
	HKEY key;
	HANDLE file;
	DWORD done = 0;
	char text[8];
	int result = 0;
	CreateDirectoryW(L"C:\\RavenPackage", NULL);
	CreateDirectoryW(L"C:\\RavenPackage\\Root", NULL);
	CreateDirectoryW(L"C:\\RavenPackage\\Root\\VFS", NULL);
	CreateDirectoryW(L"C:\\RavenPackage\\Root\\VFS\\ProgramFilesCommonX64", NULL);
	CreateDirectoryW(L"C:\\RavenPackage\\Root\\VFS\\ProgramFilesCommonX64\\RavenShared", NULL);
	file = CreateFileW(L"C:\\RavenPackage\\Root\\VFS\\ProgramFilesCommonX64\\RavenShared\\probe.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, "vfs", 3, &done, NULL))
		return 1;
	CloseHandle(file);
	if (GetFileAttributesW(L"C:\\Program Files\\Common Files\\RavenShared\\probe.txt") != INVALID_FILE_ATTRIBUTES)
		return 2;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\AppVISV", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL) ||
		RegSetValueExW(key, L"C:\\RavenPackage", 0, REG_SZ, (const BYTE *)L"package", 16))
		return 3;
	file = CreateFileW(L"C:\\Program Files\\Common Files\\RavenShared\\probe.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE || !ReadFile(file, text, sizeof text, &done, NULL) || done != 3 || text[0] != 'v' || text[2] != 's')
		result = 4;
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	if (!result && GetFileAttributesW(L"C:\\Program Files\\Common Files\\RavenShared\\absent.txt") != INVALID_FILE_ATTRIBUTES)
		result = 5;
	/* Looking a missing file up in the packages takes no handle with it. */
	if (!result) {
		DWORD before = 0, after = 0;
		GetProcessHandleCount(GetCurrentProcess(), &before);
		for (int index = 0; index < 20; ++index)
			GetFileAttributesW(L"C:\\Program Files\\Common Files\\RavenShared\\absent.txt");
		if (!GetProcessHandleCount(GetCurrentProcess(), &after) || after != before)
			result = 7;
	}
	RegDeleteValueW(key, L"C:\\RavenPackage");
	RegCloseKey(key);
	if (!result && GetFileAttributesW(L"C:\\Program Files\\Common Files\\RavenShared\\probe.txt") != INVALID_FILE_ATTRIBUTES)
		result = 6;
	DeleteFileW(L"C:\\RavenPackage\\Root\\VFS\\ProgramFilesCommonX64\\RavenShared\\probe.txt");
	return result;
}

int raven_entry(void)
{
	{
		const UINT requested[] = {CP_ACP, CP_OEMCP, CP_THREAD_ACP, 1252, 437, 850, 28591, 20127, CP_UTF8};
		for (UINT index = 0; index < sizeof(requested) / sizeof(requested[0]); ++index) {
			CPINFO plain;
			CPINFOEXA ansi;
			CPINFOEXW wide;
			UINT page = requested[index] == CP_OEMCP ? GetOEMCP() :
				requested[index] == CP_ACP || requested[index] == CP_THREAD_ACP ? GetACP() : requested[index];
			if (!GetCPInfo(requested[index], &plain) || !GetCPInfoExA(requested[index], 0, &ansi) ||
				!GetCPInfoExW(requested[index], 0, &wide) || ansi.CodePage != page || wide.CodePage != page ||
				wide.MaxCharSize != plain.MaxCharSize || ansi.MaxCharSize != plain.MaxCharSize ||
				wide.MaxCharSize != (page == CP_UTF8 ? 4 : 1) || wide.DefaultChar[0] != plain.DefaultChar[0] ||
				wide.UnicodeDefaultChar != L'?' || !wide.CodePageName[0]) return 160;
			for (UINT character = 0; character < MAX_PATH; ++character) {
				if ((BYTE)ansi.CodePageName[character] != wide.CodePageName[character]) return 161;
				if (!ansi.CodePageName[character]) break;
				if (character == MAX_PATH - 1) return 162;
			}
		}
		CPINFOEXW sentinel;
		BYTE *bytes = (BYTE *)&sentinel;
		for (UINT index = 0; index < sizeof(sentinel); ++index) bytes[index] = 0x5a;
		if (GetCPInfoExW(0x7fffffff, 0, &sentinel) || GetLastError() != ERROR_INVALID_PARAMETER ||
			GetCPInfoExW(CP_UTF8, 1, &sentinel) || GetLastError() != ERROR_INVALID_PARAMETER ||
			GetCPInfoExW(CP_UTF8, 0, NULL) || GetLastError() != ERROR_INVALID_PARAMETER) return 163;
		for (UINT index = 0; index < sizeof(sentinel); ++index) if (bytes[index] != 0x5a) return 164;
	}
	{
		DWORD needed = 0, repeated = 0, found = 0;
		if (!K32EnumProcessModules(GetCurrentProcess(), NULL, 0, &needed) || needed < 3 * sizeof(HMODULE)) return 150;
		HMODULE *modules = HeapAlloc(GetProcessHeap(), 0, needed);
		if (!modules || !K32EnumProcessModulesEx(GetCurrentProcess(), modules, needed, &repeated, LIST_MODULES_ALL) ||
			repeated != needed || modules[0] != GetModuleHandleW(NULL)) return 151;
		for (DWORD index = 0; index < needed / sizeof(HMODULE); ++index) {
			if (modules[index] == GetModuleHandleW(L"ntdll.dll")) found |= 1;
			if (modules[index] == GetModuleHandleW(L"kernel32.dll")) {
				MODULEINFO info;
				WCHAR name[64];
				if (!K32GetModuleBaseNameW(GetCurrentProcess(), modules[index], name, 64) ||
					!K32GetModuleInformation(GetCurrentProcess(), modules[index], &info, sizeof(info)) ||
					info.lpBaseOfDll != modules[index] || !info.SizeOfImage ||
					!GetProcAddress(modules[index], "SetThreadErrorMode")) return 152;
				found |= 2;
			}
		}
		if (found != 3 || !HeapFree(GetProcessHeap(), 0, modules)) return 153;
	}
	{
		char ansi[128];
		WCHAR wide[128];
		DWORD mask = GetLogicalDrives(), seen = 0, expected = 0, index, letter;
		for (letter = 0; letter < 26; ++letter) if (mask & (1u << letter)) expected += 4;
		if (!(mask & (1u << 2)) || !(mask & (1u << 25)) || GetLogicalDriveStringsA(0, NULL) != expected + 1 ||
			GetLogicalDriveStringsW(0, NULL) != expected + 1 || expected + 1 > sizeof(ansi)) return 50;
		for (index = 0; index < sizeof(ansi); ++index) ansi[index] = 0x5a;
		if (GetLogicalDriveStringsA(expected, ansi) != expected + 1) return 51;
		for (index = 0; index < sizeof(ansi); ++index) if (ansi[index] != 0x5a) return 52;
		if (GetLogicalDriveStringsA(expected + 1, ansi) != expected || GetLogicalDriveStringsW(128, wide) != expected ||
			ansi[expected] || wide[expected] || ansi[expected + 1] != 0x5a) return 53;
		for (index = 0; index < expected; ++index) if ((BYTE)ansi[index] != wide[index]) return 54;
		for (index = 0; index < expected; index += 4) {
			if (ansi[index] < 'A' || ansi[index] > 'Z' || ansi[index + 1] != ':' || ansi[index + 2] != '\\' || ansi[index + 3]) return 55;
			seen |= 1u << (ansi[index] - 'A');
		}
		if (seen != mask) return 56;
	}
	WCHAR *folder = NULL, name[64];
	DWORD size, error;
	IP_ADAPTER_ADDRESSES *adapters, *adapter;
	ULONG needed = 0;
	DATA_BLOB plain = {5, (BYTE *)"token"}, sealed, opened, entropy = {3, (BYTE *)"abc"}, wrong = {3, (BYTE *)"xyz"};
	LPWSTR description = NULL;
	HANDLE job, token = NULL, copy = NULL;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits, queried;
	BOOL inside = TRUE;
	DWORD mask;
	GENERIC_MAPPING mapping = {1, 2, 4, 7};
	int loopback = 0;
	PSID everyone = NULL;

	if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &folder) != S_OK || !folder || !contains(folder, L"AppData\\Roaming"))
		return 1;
	CoTaskMemFree(folder);
	if (SHGetKnownFolderPath(&FOLDERID_ProgramFilesX86, 0, NULL, &folder) != S_OK || !contains(folder, L"Program Files (x86)"))
		return 2;
	CoTaskMemFree(folder);
	if (SHGetKnownFolderPath(&FOLDERID_UserProfiles, 0, NULL, &folder) != S_OK || lstrcmpiW(folder, L"C:\\Users"))
		return 240;
	CoTaskMemFree(folder);
	if ((size = known_folder_manager()))
		return 230 + (int)size;
	if ((size = virtual_package_files()))
		return 250 + (int)size;
	{
		/* The runtime's compiler stands behind the library: a compile gives code and, with no warning, no message blob. */
		typedef HRESULT (WINAPI *compile_type)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT,
			void **, void **);
		HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		compile_type compile = compiler ? (compile_type)(void *)GetProcAddress(compiler, "D3DCompile") : NULL;
		void *code = NULL, *errors = (void *)1;
		if (!compile || compile("float4 main() : SV_Target { return 1; }", 39, NULL, NULL, NULL, "main", "ps_4_0", 0, 0, &code, &errors) != S_OK ||
			!code || errors)
			return 260;
	}

	size = 64;
	if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size) || size == 0 || name[0] == 0)
		return 3;
	size = 1;
	if (GetComputerNameExW(ComputerNameDnsHostname, name, &size) || GetLastError() != ERROR_MORE_DATA || size < 2)
		return 4;

	if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &needed) != ERROR_BUFFER_OVERFLOW || needed < sizeof(IP_ADAPTER_ADDRESSES))
		return 5;
	adapters = HeapAlloc(GetProcessHeap(), 0, needed);
	if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, adapters, &needed) != NO_ERROR)
		return 6;
	for (adapter = adapters; adapter; adapter = adapter->Next)
		if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK && adapter->FirstUnicastAddress && adapter->FirstUnicastAddress->Address.lpSockaddr &&
			adapter->FirstUnicastAddress->Address.iSockaddrLength >= 16)
			loopback = 1;
	if (!loopback)
		return 7;
	HeapFree(GetProcessHeap(), 0, adapters);
	/* The IPv4 list leaves loopback out: a host with only loopback has no data, any other adapter has a dotted address and mask. */
	needed = 0;
	error = GetAdaptersInfo(NULL, &needed);
	if (error == ERROR_BUFFER_OVERFLOW) {
		IP_ADAPTER_INFO *infos = HeapAlloc(GetProcessHeap(), 0, needed), *info;
		if (!infos || GetAdaptersInfo(infos, &needed) != NO_ERROR)
			return 60;
		for (info = infos; info; info = info->Next)
			if (info->Type == MIB_IF_TYPE_LOOPBACK || info->AdapterName[0] != '{' || !info->IpAddressList.IpAddress.String[0] ||
				!contains_narrow(info->IpAddressList.IpMask.String, '.') || !contains_narrow(info->GatewayList.IpAddress.String, '.'))
				return 61;
		HeapFree(GetProcessHeap(), 0, infos);
	} else if (error != ERROR_NO_DATA)
		return 62;
	if (GetAdaptersInfo(NULL, NULL) != ERROR_INVALID_PARAMETER)
		return 63;
	{
		FIXED_INFO fixed;
		ULONG fixed_size = 0;
		if (GetNetworkParams(NULL, NULL) != ERROR_INVALID_PARAMETER || GetNetworkParams(NULL, &fixed_size) != ERROR_BUFFER_OVERFLOW ||
			fixed_size != sizeof(fixed))
			return 64;
		if (GetNetworkParams(&fixed, &fixed_size) != NO_ERROR || !fixed.HostName[0])
			return 65;
	}

	if (!CryptProtectData(&plain, L"note", &entropy, NULL, NULL, 0, &sealed) || sealed.cbData <= plain.cbData)
		return 8;
	if (!CryptUnprotectData(&sealed, &description, &entropy, NULL, NULL, 0, &opened) || opened.cbData != 5 || !same(opened.pbData, "token", 5) ||
		!description || description[0] != 'n' || description[3] != 'e')
		return 9;
	LocalFree(description);
	LocalFree(opened.pbData);
	if (CryptUnprotectData(&sealed, NULL, &wrong, NULL, NULL, 0, &opened))
		return 10;
	LocalFree(sealed.pbData);

	/* Account checks: the generic rights of a mask become specific ones, and the descriptor decides the rest. */
	mask = GENERIC_READ | GENERIC_WRITE;
	MapGenericMask(&mask, &mapping);
	if (mask != 3)
		return 11;
	if (!ConvertStringSidToSidW(L"WD", &everyone))
		return 12;
	{
		SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
		PSID expected = NULL;
		if (!AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &expected) || !EqualSid(everyone, expected))
			return 13;
		FreeSid(expected);
	}
	LocalFree(everyone);

	/* Jobs keep the limits the program sets. */
	job = CreateJobObjectW(NULL, NULL);
	if (!job)
		return 14;
	clear(&limits, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
	limits.BasicLimitInformation.ActiveProcessLimit = 5;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
		return 15;
	clear(&queried, sizeof(queried));
	if (!QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &queried, sizeof(queried), NULL) ||
		queried.BasicLimitInformation.ActiveProcessLimit != 5 || !(queried.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE))
		return 16;
	if (!IsProcessInJob(GetCurrentProcess(), job, &inside) || inside)
		return 17;
	CloseHandle(job);

	/* Tokens copy. */
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token))
		return 18;
	if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenImpersonation, &copy) || !copy)
		return 19;
	{
		DWORD privileges[5] = {0xffffffff, 0, 0, 0, 0x12345678};
		size = 0;
		if (GetTokenInformation(token, TokenPrivileges, NULL, 0, &size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || size != sizeof(TOKEN_PRIVILEGES))
			return 20;
		if (GetTokenInformation(token, TokenPrivileges, privileges, size - 1, &size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
			privileges[0] != 0xffffffff)
			return 21;
		if (!GetTokenInformation(token, TokenPrivileges, privileges, sizeof(privileges), &size) || size != sizeof(TOKEN_PRIVILEGES) ||
			privileges[0] != 1 || privileges[1] != 23 || privileges[2] || privileges[3] != (SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT) ||
			privileges[4] != 0x12345678)
			return 22;
		if (GetTokenInformation((HANDLE)(UINT_PTR)0x12345678, TokenPrivileges, privileges, sizeof(privileges), &size) ||
			GetLastError() != ERROR_INVALID_HANDLE)
			return 23;
	}
	{
		TOKEN_GROUPS *groups, *logon;
		DWORD group_size = 0, logon_size = 0, index;
		int found = 0;
		if (GetTokenInformation(token, TokenLogonSid, NULL, 0, &logon_size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
			logon_size < sizeof(TOKEN_GROUPS))
			return 24;
		GetTokenInformation(token, TokenGroups, NULL, 0, &group_size);
		groups = HeapAlloc(GetProcessHeap(), 0, group_size);
		logon = HeapAlloc(GetProcessHeap(), 0, logon_size);
		if (!groups || !logon || !GetTokenInformation(token, TokenGroups, groups, group_size, &group_size) ||
			!GetTokenInformation(token, TokenLogonSid, logon, logon_size, &logon_size) || logon->GroupCount != 1 ||
			(logon->Groups[0].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID || !IsValidSid(logon->Groups[0].Sid))
			return 25;
		for (index = 0; index < groups->GroupCount; ++index)
			if ((groups->Groups[index].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID &&
				EqualSid(groups->Groups[index].Sid, logon->Groups[0].Sid))
				++found;
		if (found != 1)
			return 26;
		{
			BYTE interactive[SECURITY_MAX_SID_SIZE];
			DWORD interactive_size = sizeof(interactive);
			found = 0;
			if (!CreateWellKnownSid(WinInteractiveSid, NULL, interactive, &interactive_size) ||
				*GetSidSubAuthority(interactive, 0) != SECURITY_INTERACTIVE_RID)
				return 261;
			for (index = 0; index < groups->GroupCount; ++index)
				found += EqualSid(groups->Groups[index].Sid, interactive) != 0;
			if (found != 1)
				return 262;
		}
		HeapFree(GetProcessHeap(), 0, groups);
		HeapFree(GetProcessHeap(), 0, logon);
	}
	{
		ACL acl;
		TOKEN_DEFAULT_DACL dacl = {&acl};
		union { ULONGLONG align; BYTE bytes[128]; } buffer;
		TOKEN_DEFAULT_DACL *queried_dacl = (TOKEN_DEFAULT_DACL *)buffer.bytes;
		HANDLE changed = NULL;
		clear(&acl, sizeof(acl));
		acl.AclRevision = ACL_REVISION;
		acl.AclSize = sizeof(acl);
		if (!SetTokenInformation(token, TokenDefaultDacl, &dacl, sizeof(dacl)) ||
			!DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &changed))
			return 27;
		acl.AceCount = 1;
		if (SetTokenInformation(token, TokenDefaultDacl, &dacl, sizeof(dacl)) || GetLastError() != ERROR_INVALID_ACL)
			return 28;
		if (!GetTokenInformation(token, TokenDefaultDacl, &buffer, sizeof(buffer), &size) || size != sizeof(dacl) + sizeof(acl) ||
			queried_dacl->DefaultDacl != (PACL)(buffer.bytes + sizeof(dacl)) || queried_dacl->DefaultDacl->AceCount != 0)
			return 29;
		dacl.DefaultDacl = NULL;
		if (!SetTokenInformation(token, TokenDefaultDacl, &dacl, sizeof(dacl)) ||
			!GetTokenInformation(token, TokenDefaultDacl, &buffer, sizeof(buffer), &size) || size != sizeof(dacl) || queried_dacl->DefaultDacl)
			return 30;
		if (!GetTokenInformation(changed, TokenDefaultDacl, &buffer, sizeof(buffer), &size) || size != sizeof(dacl) + sizeof(acl) ||
			!queried_dacl->DefaultDacl || queried_dacl->DefaultDacl->AceCount != 0)
			return 31;
		CloseHandle(changed);
	}
	{
		TOKEN_MANDATORY_LABEL label;
		union { ULONGLONG align; BYTE bytes[64]; } buffer;
		TOKEN_MANDATORY_LABEL *queried_label = (TOKEN_MANDATORY_LABEL *)buffer.bytes;
		if (!ConvertStringSidToSidW(L"LW", &label.Label.Sid))
			return 32;
		label.Label.Attributes = SE_GROUP_INTEGRITY;
		if (!SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label)) ||
			!GetTokenInformation(token, TokenIntegrityLevel, &buffer, sizeof(buffer), &size) ||
			!EqualSid(label.Label.Sid, queried_label->Label.Sid))
			return 33;
		if (!GetTokenInformation(copy, TokenIntegrityLevel, &buffer, sizeof(buffer), &size) ||
			*GetSidSubAuthority(queried_label->Label.Sid, 0) != SECURITY_MANDATORY_MEDIUM_RID)
			return 34;
		LocalFree(label.Label.Sid);
		if (!ConvertStringSidToSidW(L"WD", &label.Label.Sid) ||
			SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label)) || GetLastError() != ERROR_INVALID_SID)
			return 39;
		if (!GetTokenInformation(token, TokenIntegrityLevel, &buffer, sizeof(buffer), &size) ||
			*GetSidSubAuthority(queried_label->Label.Sid, 0) != SECURITY_MANDATORY_LOW_RID)
			return 40;
		LocalFree(label.Label.Sid);
	}
	{
		EXPLICIT_ACCESSW entries[3], revoke;
		PACL original = NULL, reduced = NULL, empty = NULL;
		PSID world = NULL, users = NULL;
		ACL_SIZE_INFORMATION acl_size;
		ACCESS_ALLOWED_ACE *ace = NULL;
		DWORD index;
		if (!ConvertStringSidToSidW(L"WD", &world) || !ConvertStringSidToSidW(L"S-1-5-32-545", &users))
			return 35;
		clear(entries, sizeof(entries));
		for (index = 0; index < 3; ++index) {
			entries[index].grfAccessPermissions = GENERIC_READ;
			entries[index].grfAccessMode = index == 2 ? DENY_ACCESS : GRANT_ACCESS;
			BuildTrusteeWithSidW(&entries[index].Trustee, index == 0 ? users : world);
		}
		if (SetEntriesInAclW(3, entries, NULL, &original) != ERROR_SUCCESS)
			return 36;
		clear(&revoke, sizeof(revoke));
		revoke.grfAccessMode = REVOKE_ACCESS;
		BuildTrusteeWithSidW(&revoke.Trustee, world);
		if (SetEntriesInAclW(1, &revoke, original, &reduced) != ERROR_SUCCESS ||
			!GetAclInformation(reduced, &acl_size, sizeof(acl_size), AclSizeInformation) || acl_size.AceCount != 1 ||
			!GetAce(reduced, 0, (void **)&ace) || !EqualSid(&ace->SidStart, users) || original->AceCount != 3)
			return 37;
		if (SetEntriesInAclW(1, &revoke, NULL, &empty) != ERROR_SUCCESS || empty->AceCount != 0 || empty->AclSize != sizeof(ACL))
			return 38;
		{
			ACL malformed;
			PACL result = NULL;
			clear(&malformed, sizeof(malformed));
			malformed.AclRevision = ACL_REVISION;
			malformed.AclSize = sizeof(malformed);
			malformed.AceCount = 1;
			if (SetEntriesInAclW(1, &revoke, &malformed, &result) != ERROR_INVALID_ACL || result)
				return 41;
		}
		LocalFree(original);
		LocalFree(reduced);
		LocalFree(empty);
		LocalFree(world);
		LocalFree(users);
	}
	CloseHandle(copy);
	CloseHandle(token);
	(void)error;
	/* The address waits through the synchronization API set, as programs built against it import them: a value already changed returns at
	 * once, an unchanged one times out. */
	HMODULE synch = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
	BOOL (WINAPI *wait_on)(volatile void *, void *, SIZE_T, DWORD) = synch ? (void *)GetProcAddress(synch, "WaitOnAddress") : NULL;
	void (WINAPI *wake_all)(void *) = synch ? (void *)GetProcAddress(synch, "WakeByAddressAll") : NULL;
	if (!wait_on || !wake_all || !GetProcAddress(synch, "WakeByAddressSingle")) return 165;
	LONG watched = 1, compare = 0;
	if (!wait_on(&watched, &compare, sizeof watched, 1000)) return 166;
	compare = 1;
	if (wait_on(&watched, &compare, sizeof watched, 10) || GetLastError() != ERROR_TIMEOUT) return 167;
	wake_all(&watched);
	/* A descriptor naming the local administrator account and a raw access mask, as installers write for their shared folders. */
	PSECURITY_DESCRIPTOR shared_folder = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;OI;GA;;;LS)(A;OI;GA;;;BA)(A;OI;GA;;;LA)(A;OI;GR;;;AU)(A;OI;0x00100000;;;BG)",
		SDDL_REVISION_1, &shared_folder, NULL)) return 168;
	LPWSTR round_trip = NULL;
	if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(shared_folder, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &round_trip, NULL) ||
		!contains(round_trip, L";;;LA)") || !contains(round_trip, L"0x100000;;;BG)")) return 169;
	LocalFree(round_trip);
	LocalFree(shared_folder);
	/* A thread has no I/O pending in the kernel's sense; a key takes its last write time and refuses a class it does not have. */
	BOOL pending = TRUE;
	if (!GetThreadIOPendingFlag(GetCurrentThread(), &pending) || pending) return 170;
	HKEY key = NULL;
	LARGE_INTEGER written = {{0}};
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RavenKeyInformation", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
		return 171;
	if (NtSetInformationKey(key, 0, &written, sizeof written) || NtSetInformationKey(key, 9, &written, sizeof written) != (LONG)0xc0000003)
		return 172;
	/* A key names itself by its last part and by its full native path, and counts what it holds. */
	HKEY child = NULL;
	if (RegCreateKeyExW(key, L"Inner", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &child, NULL) || RegSetValueExW(key, L"Seven", 0, REG_DWORD, (BYTE *)&(DWORD){7}, 4))
		return 173;
	RegCloseKey(child);
	/* Looked up by name, as a library loaded after start finds it. */
	LONG (WINAPI *NtQueryKey)(HANDLE, int, void *, ULONG, ULONG *) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryKey");
	if (!NtQueryKey) return 180;
	ULONG key_size = 0;
	BYTE details[512];
	RtlSecureZeroMemory(details, sizeof details);
	if (NtQueryKey(key, 0, details, sizeof details, &key_size) || key_size != 16 + 38 || *(ULONG *)(details + 12) != 38 ||
		lstrcmpW((WCHAR *)(details + 16), L"RavenKeyInformation")) return 174;
	if (NtQueryKey(key, 3, details, 8, &key_size) != (LONG)0x80000005 || key_size < 4 + 2 * 50) return 175;
	RtlSecureZeroMemory(details, sizeof details);
	if (NtQueryKey(key, 3, details, sizeof details, &key_size) || !contains((WCHAR *)(details + 4), L"\\REGISTRY\\USER\\S-1-5-21-") ||
		!contains((WCHAR *)(details + 4), L"\\Software\\RavenKeyInformation")) return 176;
	if (NtQueryKey(key, 2, details, sizeof details, &key_size) || *(ULONG *)(details + 20) != 1 || *(ULONG *)(details + 24) != 10 ||
		*(ULONG *)(details + 32) != 1 || *(ULONG *)(details + 40) != 4) return 177;
	if (NtQueryKey(key, 2, details, 10, &key_size) != (LONG)0xc0000023 || key_size != 44) return 178;
	/* PathCchCanonicalize removes the . and .. parts and refuses a result that does not fit. */
	HRESULT (WINAPI *canonicalize)(WCHAR *, size_t, const WCHAR *) = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PathCchCanonicalize");
	WCHAR clean[MAX_PATH];
	if (!canonicalize || canonicalize(clean, MAX_PATH, L"C:\\a\\.\\b\\..\\c.txt") || lstrcmpW(clean, L"C:\\a\\c.txt") ||
		canonicalize(clean, 4, L"C:\\a\\c.txt") != (HRESULT)0x8007007a) return 179;
	/* PathCchRemoveFileSpec keeps the root and reports S_FALSE once only the root is left. */
	HRESULT (WINAPI *remove_spec)(WCHAR *, size_t) = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PathCchRemoveFileSpec");
	WCHAR spec[MAX_PATH] = L"C:\\a\\b.txt";
	if (!remove_spec || remove_spec(spec, MAX_PATH) || lstrcmpW(spec, L"C:\\a") || remove_spec(spec, MAX_PATH) || lstrcmpW(spec, L"C:\\") ||
		remove_spec(spec, MAX_PATH) != S_FALSE || lstrcmpW(spec, L"C:\\")) return 179;
	if ((key_size = click_to_run_natives(key))) return 180 + (int)key_size;
	RegDeleteKeyW(key, L"Inner");
	RegCloseKey(key);
	RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\RavenKeyInformation");
	return 0;
}
