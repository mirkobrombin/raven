#include "providers.h"
#include "handle_channel.h"
#include "object_api.h"
#include "module_api.h"
#include "resources.h"
#include "memory.h"
#include "msvcrt_api.h"
#include "prefix.h"
#include "profile_api.h"
#include "registry.h"
#include "desktop/api.h"
#include "desktop/windows.h"
#include "api.h"
#include "ansi_api.h"
#include "guest_allocator.h"
#include "heap.h"
#include "kernel_context.h"
#include "actctx.h"
#include "child_process.h"
#include "paths.h"
#include "x86_compat_api.h"
#include "registry_words.h"
#include "system_exports.h"
#include "loader.h"
#include "x86_loader.h"
#include "x86_tls.h"
#ifdef RAVEN_VULKAN_THUNK32
#include "graphics/vulkan_thunk32_engine.h"
#endif
#include "x86_system_api.h"
#include "x86_d3d9.h"
#include "x86_dsound.h"
#include "x86_thread_api.h"

#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <sstream>
#include <set>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace raven {

namespace {

// The task allocator as an IMalloc, for CoGetMalloc: its memory is the memory CoTaskMemAlloc hands out.
constexpr ComMethod malloc_methods[] = {{"IMalloc", "QueryInterface", 0, 3}, {"IMalloc", "AddRef", 1, 1}, {"IMalloc", "Release", 2, 1},
	{"IMalloc", "Alloc", 3, 2}, {"IMalloc", "Realloc", 4, 3}, {"IMalloc", "Free", 5, 2}, {"IMalloc", "GetSize", 6, 2}, {"IMalloc", "DidAlloc", 7, 2},
	{"IMalloc", "HeapMinimize", 8, 1}};

// stdcall argument counts for every API the x86 process runner can dispatch.
constexpr std::array<std::pair<std::string_view, uint32_t>, 115> kernel32_exports{{
	{"CloseHandle", 1}, {"CreateEventA", 4}, {"CreateEventW", 4}, {"CreateThread", 6}, {"DeleteCriticalSection", 1}, {"EnterCriticalSection", 1}, {"ExitProcess", 1}, {"ExitThread", 1}, {"FindResourceA", 3},
	{"FindResourceExA", 4}, {"FindResourceExW", 4}, {"FindResourceW", 3}, {"FreeLibrary", 1}, {"GetACP", 0},
	{"GetCommandLineA", 0}, {"GetCommandLineW", 0}, {"GetCurrentProcessId", 0}, {"GetCurrentThreadId", 0}, {"GetExitCodeThread", 2},
	{"GetFileType", 1}, {"GetLastError", 0}, {"GetModuleFileNameA", 3}, {"GetModuleHandleA", 1},
	{"GetModuleHandleW", 1}, {"GetPrivateProfileIntW", 4}, {"GetPrivateProfileStringW", 6}, {"WritePrivateProfileStringW", 4}, {"LoadLibraryExA", 3}, {"LoadLibraryExW", 3}, {"GetProcAddress", 2}, {"GetProcessHeap", 0}, {"GetStartupInfoA", 1}, {"GetStartupInfoW", 1}, {"GetStdHandle", 1},
	{"GetTickCount", 0}, {"ProcessIdToSessionId", 2}, {"HeapAlloc", 3}, {"HeapFree", 3}, {"HeapReAlloc", 4}, {"HeapSize", 3}, {"InitializeCriticalSection", 1}, {"LeaveCriticalSection", 1}, {"LoadLibraryA", 1}, {"LoadLibraryW", 1}, {"LoadResource", 2}, {"LockResource", 1},
	{"OutputDebugStringA", 1}, {"ResetEvent", 1}, {"ResumeThread", 1}, {"SetEvent", 1}, {"SetLastError", 1}, {"SetThreadPriority", 2}, {"SizeofResource", 2}, {"Sleep", 1}, {"SwitchToThread", 0}, {"TryEnterCriticalSection", 1}, {"VirtualAlloc", 4},
	// Services of the 32-bit CRT era: legacy file calls, global memory, code pages, environment and version queries.
	{"_lopen", 2}, {"_lcreat", 2}, {"_lread", 3}, {"_lwrite", 3}, {"_llseek", 3}, {"lstrcpyA", 2}, {"lstrlenA", 1}, {"GetWindowsDirectoryA", 2}, {"GlobalHandle", 1}, {"GetVersionExA", 1}, {"WinExec", 2}, {"GlobalUnlock", 1}, {"GlobalFree", 1}, {"GlobalAlloc", 2}, {"SetErrorMode", 1}, {"GlobalLock", 1}, {"GetCurrentProcess", 0}, {"SetHandleCount", 1}, {"GetOEMCP", 0}, {"GetCPInfo", 2}, {"WideCharToMultiByte", 8}, {"DeleteFileA", 1}, {"GetEnvironmentStrings", 0}, {"FreeEnvironmentStringsW", 1}, {"FreeEnvironmentStringsA", 1}, {"UnhandledExceptionFilter", 1}, {"RtlUnwind", 4}, {"HeapCreate", 3}, {"HeapDestroy", 1}, {"TerminateProcess", 2}, {"GetEnvironmentStringsW", 0}, {"GetVersion", 0}, {"GetFileAttributesA", 1},
	// File and clock services of the C runtime component: the shared handlers read 32-bit words; a 64-bit offset takes two.
	{"RaiseException", 4}, {"AddVectoredExceptionHandler", 2}, {"AddVectoredContinueHandler", 2}, {"RemoveVectoredExceptionHandler", 1},
	{"RemoveVectoredContinueHandler", 1}, {"GlobalMemoryStatus", 1}, {"CreateFileW", 7}, {"DeleteFileW", 1}, {"GetEnvironmentVariableA", 3}, {"GetFileInformationByHandle", 2}, {"MultiByteToWideChar", 6},
	{"QueryPerformanceCounter", 1}, {"QueryPerformanceFrequency", 1}, {"ReadFile", 5}, {"SetFilePointer", 4}, {"SetFilePointerEx", 5},
	{"GetFileSizeEx", 2}, {"FlushFileBuffers", 1},
	{"VirtualFree", 3}, {"VirtualProtect", 4}, {"WaitForSingleObject", 2}, {"WriteFile", 5}, {"_lclose", 1}, {"lstrcpyW", 2}, {"lstrlenW", 1},
}};

// Shared-registry handlers that serve 32-bit programs as they are: every argument is one 32-bit word and no structure holds a pointer.
constexpr std::array shared_exports{
	// A trace identifier occupies two stack words; its controller adapter combines them before shared dispatch.
	NativeX86System::Export{"advapi32.dll", "StopTraceA", 4}, NativeX86System::Export{"advapi32.dll", "StopTraceW", 4},
	NativeX86System::Export{"advapi32.dll", "ControlTraceA", 5}, NativeX86System::Export{"advapi32.dll", "ControlTraceW", 5},
	NativeX86System::Export{"advapi32.dll", "StartTraceA", 3}, NativeX86System::Export{"advapi32.dll", "StartTraceW", 3},
	NativeX86System::Export{"kernel32.dll", "GetPhysicallyInstalledSystemMemory", 1},
	NativeX86System::Export{"ws2_32.dll", "WSASendMsg", 6}, NativeX86System::Export{"ws2_32.dll", "WSARecvMsg", 5},
	NativeX86System::Export{"ntdll.dll", "NtQueryDirectoryFile", 11}, NativeX86System::Export{"ntdll.dll", "ZwQueryDirectoryFile", 11},
	NativeX86System::Export{"kernel32.dll", "CreateDirectoryW", 2}, NativeX86System::Export{"kernel32.dll", "CreateFileA", 7},
	NativeX86System::Export{"kernel32.dll", "CreateEventExA", 4}, NativeX86System::Export{"kernel32.dll", "CreateEventExW", 4},
	NativeX86System::Export{"kernel32.dll", "GetCPInfoExA", 3}, NativeX86System::Export{"kernel32.dll", "GetCPInfoExW", 3},
	NativeX86System::Export{"kernel32.dll", "CreateFileMappingA", 6}, NativeX86System::Export{"kernel32.dll", "FormatMessageA", 7}, NativeX86System::Export{"kernel32.dll", "GetEnvironmentVariableW", 3},
	NativeX86System::Export{"kernel32.dll", "GetModuleFileNameW", 3}, NativeX86System::Export{"kernel32.dll", "GetModuleHandleExA", 3},
	NativeX86System::Export{"kernel32.dll", "GetSystemTimeAsFileTime", 1}, NativeX86System::Export{"kernel32.dll", "GetTempPathA", 2},
	NativeX86System::Export{"kernel32.dll", "GetTickCount64", 0}, NativeX86System::Export{"kernel32.dll", "LocalFree", 1},
	NativeX86System::Export{"kernel32.dll", "MapViewOfFile", 5}, NativeX86System::Export{"kernel32.dll", "UnmapViewOfFile", 1},
	NativeX86System::Export{"user32.dll", "AllowSetForegroundWindow", 1},
	NativeX86System::Export{"user32.dll", "PeekMessageA", 5},
	NativeX86System::Export{"user32.dll", "CallWindowProcA", 5}, NativeX86System::Export{"user32.dll", "DestroyCursor", 1},
	NativeX86System::Export{"user32.dll", "EnumDisplayMonitors", 4}, NativeX86System::Export{"user32.dll", "EnumDisplaySettingsW", 3},
	NativeX86System::Export{"user32.dll", "GetDCEx", 3}, NativeX86System::Export{"user32.dll", "GetDisplayConfigBufferSizes", 3},
	NativeX86System::Export{"user32.dll", "GetForegroundWindow", 0}, NativeX86System::Export{"user32.dll", "GetMonitorInfoW", 2},
	NativeX86System::Export{"user32.dll", "GetRawInputDeviceList", 3},
	NativeX86System::Export{"user32.dll", "GetRawInputBuffer", 3}, NativeX86System::Export{"user32.dll", "GetRawInputData", 5},
	NativeX86System::Export{"user32.dll", "RegisterRawInputDevices", 3},
	NativeX86System::Export{"user32.dll", "GetRawInputDeviceInfoA", 4}, NativeX86System::Export{"user32.dll", "GetRawInputDeviceInfoW", 4},
	NativeX86System::Export{"user32.dll", "IsIconic", 1}, NativeX86System::Export{"user32.dll", "MonitorFromPoint", 3},
	NativeX86System::Export{"user32.dll", "WindowFromPoint", 2}, NativeX86System::Export{"user32.dll", "ChildWindowFromPoint", 3},
	NativeX86System::Export{"user32.dll", "ChildWindowFromPointEx", 4}, NativeX86System::Export{"user32.dll", "RealChildWindowFromPoint", 3},
	NativeX86System::Export{"user32.dll", "OffsetRect", 3}, NativeX86System::Export{"user32.dll", "QueryDisplayConfig", 6},
	NativeX86System::Export{"gdi32.dll", "CreateBitmap", 5}, NativeX86System::Export{"gdi32.dll", "StretchBlt", 11},
};

// More shared handlers a 32-bit program may use as they are; their words come from the registry and their library from the public
// export lists. Anything with a pointer-width structure, a 64-bit argument or a callback into x64 conventions is not here.
constexpr std::string_view shared_allowed[] = {
	// The console: its records and structures have no pointers, and a COORD argument is one word. Control handlers are left out, as they
	// call back into the program.
	"GetConsoleMode", "SetConsoleMode", "WriteConsoleW", "WriteConsoleA", "ReadConsoleW", "ReadConsoleA", "ReadConsoleInputW",
	"ReadConsoleInputA", "ReadConsoleInputExW", "ReadConsoleInputExA", "PeekConsoleInputW", "PeekConsoleInputA",
	"GetNumberOfConsoleInputEvents", "FlushConsoleInputBuffer", "GetConsoleScreenBufferInfo", "GetConsoleScreenBufferInfoEx",
	"GetLargestConsoleWindowSize", "SetConsoleTextAttribute", "SetConsoleCursorPosition", "GetConsoleCursorInfo", "SetConsoleCursorInfo",
	"FillConsoleOutputCharacterW", "FillConsoleOutputCharacterA", "FillConsoleOutputAttribute", "GetConsoleCP", "GetConsoleOutputCP",
	"SetConsoleCP", "SetConsoleOutputCP", "GetConsoleTitleW", "GetConsoleTitleA", "GetConsoleWindow", "AttachConsole", "FreeConsole",
	"GetConsoleProcessList", "WriteConsoleOutputCharacterW", "WriteConsoleOutputCharacterA", "WriteConsoleOutputAttribute",
	"ScrollConsoleScreenBufferW", "ScrollConsoleScreenBufferA",
	// WinINet: the accept types array is read at the program's pointer width.
	"InternetOpenW", "InternetOpenA", "InternetConnectW", "InternetConnectA", "HttpOpenRequestW", "HttpOpenRequestA",
	"HttpAddRequestHeadersW", "HttpAddRequestHeadersA", "HttpSendRequestW", "HttpSendRequestA", "InternetOpenUrlW", "InternetOpenUrlA",
	"InternetReadFile", "InternetQueryDataAvailable", "HttpQueryInfoW", "HttpQueryInfoA", "InternetCloseHandle", "InternetSetOptionW",
	"InternetSetOptionA", "InternetQueryOptionW", "InternetQueryOptionA", "InternetSetStatusCallbackW", "InternetSetStatusCallbackA",
	"InternetSetStatusCallback", "InternetGetConnectedState", "InternetGetLastResponseInfoW", "InternetGetLastResponseInfoA",
	"InternetCrackUrlW", "InternetCrackUrlA", "InternetGetCookieExW", "InternetGetCookieExA", "InternetGetCookieW", "InternetGetCookieA",
	"WaitForSingleObjectEx",
	// ICONINFO is read at the program's pointer width.
	"CreateIconIndirect",
	"EnumProcesses", "K32EnumProcesses",
	"QueryFullProcessImageNameW", "QueryFullProcessImageNameA",
	"NtQueryInformationProcess", "NtReadVirtualMemory", "NtWriteVirtualMemory", "ReadProcessMemory", "WriteProcessMemory", "OpenProcess",
	"NtSetInformationThread", "NtOpenThreadToken", "NtOpenThreadTokenEx", "NtOpenThread", "NtOpenProcess", "NtOpenProcessToken", "NtOpenProcessTokenEx", "NtQueryInformationThread", "OpenThread", "GetThreadId",
	"VirtualAllocEx", "VirtualFreeEx", "VirtualProtectEx", "VirtualQueryEx",
	"SetDllDirectoryW", "SetDllDirectoryA", "SetConsoleTitleW", "SetConsoleTitleA", "OpenSCManagerW", "OpenSCManagerA", "OpenServiceW", "OpenServiceA", "CreateServiceW", "CreateServiceA",
	"CloseServiceHandle", "DeleteService", "ChangeServiceConfigW", "ChangeServiceConfig2W", "ChangeServiceConfig2A", "QueryServiceConfigW", "QueryServiceConfigA", "GetServiceDisplayNameW", "GetServiceDisplayNameA",
	"QueryServiceObjectSecurity", "SetServiceObjectSecurity", "RegQueryInfoKeyA", "RegQueryInfoKeyW", "LookupPrivilegeValueA", "QueryServiceStatus", "QueryServiceStatusEx", "StartServiceW", "ControlService",
	"StartServiceCtrlDispatcherW", "RegisterServiceCtrlHandlerW", "RegisterServiceCtrlHandlerExW", "SetServiceStatus",
	"WriteFile", "GetFileAttributesW", "GetFullPathNameA", "GetFullPathNameW", "GetCurrentDirectoryA", "GetCurrentDirectoryW",
	"SetCurrentDirectoryA", "SetCurrentDirectoryW", "FindFirstFileA", "FindFirstFileW", "FindNextFileA", "FindNextFileW", "FindClose",
	"CreateDirectoryA", "RemoveDirectoryA", "RemoveDirectoryW", "MoveFileA", "MoveFileW", "CopyFileA", "CopyFileW", "GetLocalTime",
	"GetSystemTime", "lstrcmpA", "lstrcmpW", "lstrcmpiA", "lstrcmpiW", "lstrcpynA", "lstrcatA", "OutputDebugStringW", "IsDebuggerPresent",
	"GetLogicalDrives", "GetLogicalDriveStringsA", "GetLogicalDriveStringsW", "GetDriveTypeA", "GetVolumeInformationA", "GetVolumeInformationW", "GetDiskFreeSpaceA", "MulDiv", "GetStringTypeW",
	"GetStringTypeA", "CompareStringA", "CompareStringW", "LCMapStringA", "LCMapStringW", "IdnToAscii", "IdnToUnicode", "GetLocaleInfoA", "GetLocaleInfoW",
	"GetUserDefaultLCID", "GetSystemDefaultLCID", "GetUserDefaultLangID", "GetSystemDefaultLangID", "GetUserDefaultUILanguage", "GetSystemDefaultUILanguage", "GetThreadUILanguage", "IsValidCodePage", "SetEnvironmentVariableA", "SetEnvironmentVariableW",
	"ExpandEnvironmentStringsA", "ExpandEnvironmentStringsW", "CreateMutexA", "CreateMutexW", "ReleaseMutex", "GetFileTime", "SetFileTime",
	"FileTimeToLocalFileTime", "LocalFileTimeToFileTime", "FileTimeToDosDateTime", "DosDateTimeToFileTime", "GetFileSize", "SetEndOfFile",
	"IsWindowEnabled", "EnableWindow", "FindWindowExW", "SetForegroundWindow", "GetMessagePos", "OpenClipboard", "EmptyClipboard",
	"SetClipboardData", "CloseClipboard", "CharNextA", "CharNextW", "CharPrevA", "CharPrevW", "SystemParametersInfoW", "GetSystemMenu",
	"EnableMenuItem", "AppendMenuW", "CreatePopupMenu", "LoadBitmapW", "LoadImageW",
	"SHGetSpecialFolderLocation", "SHCloneSpecialIDList", "SHGetPathFromIDListW", "SHGetFileInfoW", "SHGetFileInfoA", "ShellExecuteW", "SHFileOperationW",
	"OleInitialize", "OleUninitialize", "CoTaskMemFree", "CoCreateInstance", "CoInitializeSecurity",
	"GetTempPathW", "GetTempPath2W", "GetTempPath2A", "CompareFileTime", "MoveFileExW", "lstrcatW", "lstrcpynW", "GetTempFileNameW", "GetTempFileNameA", "GetDiskFreeSpaceW", "GetExitCodeProcess",
	"CreateProcessW", "GetProcessHandleCount", "OpenProcessToken", "LookupPrivilegeValueW", "AdjustTokenPrivileges", "SetFileSecurityW",
	"RegCloseKey", "RegDisablePredefinedCache", "RegDisablePredefinedCacheEx", "RegCreateKeyExA", "RegCreateKeyExW", "RegOpenKeyA", "RegOpenKeyW", "RegOpenKeyExA", "RegOpenKeyExW", "RegQueryValueExA",
	"RegQueryValueExW", "RegSetValueExA", "RegSetValueExW", "RegDeleteKeyA", "RegDeleteKeyW", "RegDeleteValueA", "RegDeleteValueW",
	"RegEnumKeyA", "RegEnumKeyW", "RegEnumKeyExA", "RegEnumKeyExW", "RegEnumValueA", "RegEnumValueW", "RegGetValueA", "RegGetValueW",
	"GetAdaptersInfo", "RtlConvertDeviceFamilyInfoToString", "GetPackagesByPackageFamily", "GetUserDefaultGeoName", "SLGetWindowsInformation", "SLGetWindowsInformationDWORD",
	"GetWindowLongW", "SetWindowLongW", "GetFileVersionInfoA", "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExW",
	"GetFileVersionInfoSizeW", "GetFileVersionInfoW", "VerQueryValueA", "VerQueryValueW", "GetSystemDirectoryW", "GetSystemDirectoryA", "GetWindowsDirectoryW", "LocalAlloc", "LocalReAlloc", "GlobalReAlloc", "GlobalSize", "GetComputerNameA",
	"GetComputerNameW", "FileTimeToSystemTime", "SystemTimeToFileTime", "GetTimeZoneInformation", "GetCurrentProcessId", "SetFileAttributesA",
	"SetFileAttributesW", "GetLongPathNameA", "GetLongPathNameW", "GetShortPathNameA", "GetShortPathNameW", "SearchPathA", "SearchPathW",
	"GetModuleFileNameA", "IsDBCSLeadByte", "IsDBCSLeadByteEx", "GetCPInfoExA", "GetThreadLocale", "SetThreadLocale", "VerifyVersionInfoW",
	"GetUserNameA", "GetUserNameW", "FlsAlloc", "FlsFree", "FlsGetValue", "FlsSetValue", "SetDefaultDllDirectories", "AddDllDirectory", "RemoveDllDirectory", "SetSearchPathMode", "GetLargePageMinimum", "ConvertDefaultLocale", "GetMachineTypeAttributes", "GetShellWindow", "PowerCreateRequest", "PowerSetRequest", "PowerClearRequest", "CreateTransaction", "CommitTransaction", "CommitTransactionAsync",
	"RollbackTransaction", "RollbackTransactionAsync", "CreateFileTransactedW", "DeleteFileTransactedW", "MoveFileTransactedW", "CopyFileTransactedW",
	"CreateDirectoryTransactedW", "RemoveDirectoryTransactedW", "GetFileAttributesTransactedW", "SetFileAttributesTransactedW", "SHGetFolderPathW", "SHGetFolderPathA", "SHChangeNotify", "SHPathPrepareForWriteW", "ShowOwnedPopups",
	"UnrealizeObject", "EnableNonClientDpiScaling", "EnumFontsW", "DisableProcessWindowsGhosting", "ShutdownBlockReasonCreate", "ShutdownBlockReasonDestroy", "WNetOpenEnumW", "WNetOpenEnumA", "WNetEnumResourceW", "WNetEnumResourceA", "WNetCloseEnum", "WNetGetConnectionW",
	"WNetGetConnectionA", "WNetGetUniversalNameW", "WNetGetUniversalNameA", "SetSearchPathMode",
	"CreateMutexW", "CreateMutexA", "CreateMutexExW", "OpenMutexW", "OpenMutexA", "ReleaseMutex", "CreateSemaphoreW", "CreateSemaphoreA", "CreateSemaphoreExW", "ReleaseSemaphore",
};

struct NamedWords {
	std::string_view name;
	uint32_t words;
};

// The names a module exports from the host, with the words each takes: the fixed table, the shared handlers and the system services.
std::vector<NamedWords> host_exports(std::string_view module)
{
	std::vector<NamedWords> result;
	for (const auto &item : shared_exports)
		if (item.module == module)
			result.push_back({item.name, item.words});
	for (const auto &item : NativeX86System::exports())
		if (item.module == module)
			result.push_back({item.name, item.words});
	const auto published = system_export_names(module);
	for (const auto name : shared_allowed) {
		const auto words = registry_words(name);
		const auto duplicate = std::any_of(result.begin(), result.end(), [&](const auto &item) { return item.name == name; });
		if (words != UINT32_MAX && !duplicate && std::find(published.begin(), published.end(), name) != published.end())
			result.push_back({name, words});
	}
	return result;
}

const std::vector<NamedWords> &kernel32_table()
{
	static const auto table = [] {
		std::vector<NamedWords> result;
		for (const auto &[name, words] : kernel32_exports)
			result.push_back({name, words});
		for (const auto &item : host_exports("kernel32.dll"))
			if (std::none_of(result.begin(), result.end(), [&](const auto &fixed) { return fixed.name == item.name; }))
				result.push_back(item);
		return result;
	}();
	return table;
}

// stdcall argument counts of the desktop exports, keyed by export name.
const std::map<std::string_view, uint32_t> &desktop_arguments()
{
	static const auto counts = [] {
	std::map<std::string_view, uint32_t> counts{
		{"AdjustWindowRect",3},{"AdjustWindowRectEx",4},{"BeginPaint",2},{"CallWindowProcW",5},{"ClientToScreen",2},
		{"CopyIcon",1},{"CreateIconFromResourceEx",7},{"CreateWindowExW",12},{"DefWindowProcW",4},{"DestroyIcon",1},
		{"DestroyMenu",1},{"DestroyWindow",1},{"DispatchMessageW",1},{"DrawIcon",4},{"DrawIconEx",9},{"EndPaint",2},{"FillRect",3},
		{"FrameRect",3},{"GetActiveWindow",0},{"GetClassInfoExW",3},{"GetClassInfoW",3},{"GetClassLongA",2},
		{"GetClassLongPtrA",2},{"GetClassLongPtrW",2},{"GetClassLongW",2},{"GetClassNameA",3},{"GetClassNameW",3},
		{"GetClientRect",2},{"GetDC",1},{"GetDlgCtrlID",1},{"GetDlgItem",2},{"GetFocus",0},{"GetIconInfo",2},
		{"GetMenuItemCount",1},{"GetMenuItemID",2},{"GetMessageW",4},{"GetParent",1},{"GetSubMenu",2},{"GetSysColor",1},
		{"GetSysColorBrush",1},{"GetSystemMetrics",1},{"GetUpdateRect",3},{"GetWindowDC",1},{"CheckDlgButton",3},{"MessageBoxIndirectW",1},{"TrackPopupMenu",7},{"DrawTextW",5},{"CreateDialogParamW",5},{"SendMessageTimeoutW",7},
		{"CreateBrushIndirect",1},{"GetWindowLongA",2},
		{"GetWindowLongPtrA",2},{"GetWindowLongPtrW",2},{"GetWindowLongW",2},{"GetWindowRect",2},{"GetWindowTextLengthW",1},
		{"GetWindowTextW",3},{"InternalGetWindowText",3},{"InvalidateRect",3},{"IsChild",2},{"IsMenu",1},{"IsWindow",1},
		{"IsWindowUnicode",1},{"IsWindowVisible",1},{"LoadMenuW",2},{"LoadStringW",4},{"LookupIconIdFromDirectoryEx",5},
		{"MapWindowPoints",4},{"PeekMessageW",5},{"PostMessageW",4},{"SendNotifyMessageW",4},{"PostQuitMessage",1},{"PrivateExtractIconsW",8},
		{"RegisterClassExW",1},{"RegisterClassW",1},{"ReleaseDC",2},{"ScreenToClient",2},{"SendMessageW",4},
		{"SetClassLongA",3},{"SetClassLongPtrA",3},{"SetClassLongPtrW",3},{"SetClassLongW",3},{"SetFocus",1},
		{"SetWindowLongA",3},{"SetWindowLongPtrA",3},{"SetWindowLongPtrW",3},{"SetWindowLongW",3},{"SetWindowTextW",2},
		{"ShowWindow",2},{"TranslateMessage",1},{"UnregisterClassW",2},{"UpdateWindow",1},{"ValidateRect",2},
		{"BitBlt",9},{"CombineRgn",4},{"CreateCompatibleBitmap",3},{"CreateCompatibleDC",1},{"CreateDIBSection",6},
		{"CreateFontIndirectW",1},{"CreatePen",3},{"CreateRectRgn",4},{"CreateSolidBrush",1},{"DPtoLP",3},{"DeleteDC",1},
		{"DeleteObject",1},{"EqualRgn",2},{"ExcludeClipRect",5},{"ExtTextOutW",8},{"GdiFlush",0},{"GetBitmapBits",3},
		{"GetBkColor",1},{"GetBkMode",1},{"GetClipBox",2},{"GetClipRgn",2},{"GetCurrentObject",2},
		{"GetCurrentPositionEx",2},{"GetGraphicsMode",1},{"GetLayout",1},{"GetObjectA",3},{"GetObjectType",1},
		{"GetObjectW",3},{"GetRgnBox",2},{"GetStockObject",1},{"GetTextColor",1},{"GetTextExtentExPointW",7},
		{"GetTextExtentPoint32W",4},{"GetTextExtentPointW",4},{"GetTextMetricsW",2},{"GetViewportExtEx",2},
		{"GetWindowExtEx",2},{"IntersectClipRect",5},{"LPtoDP",3},{"LineTo",3},{"MoveToEx",4},{"OffsetRgn",3},
		{"PtInRegion",3},{"RectInRegion",2},{"SelectClipRgn",2},{"SelectObject",2},{"SetBkColor",2},{"SetBkMode",2},
		{"SetRectRgn",5},{"SetTextColor",2},{"TextOutW",5},{"LoadIconWithScaleDown",5},
		// Exports bound for real programs whose behavior is not implemented yet; each stops the process by name.
		{"CheckMenuItem",3},{"DialogBoxParamW",5},{"EndDialog",2},{"GetDesktopWindow",0},{"GetDlgItemInt",4},
		{"GetDlgItemTextW",4},{"GetMenuItemRect",4},{"KillTimer",2},{"LoadAcceleratorsW",2},{"LoadCursorW",2},
		{"LoadIconW",2},{"MessageBoxW",4},{"MoveWindow",6},{"PtInRect",3},{"ReleaseCapture",0},{"SetCapture",1},
		{"SetDlgItemInt",4},{"SetDlgItemTextW",3},{"SetMenu",2},{"SetRect",5},{"SetTimer",4},{"TranslateAcceleratorW",3},
		{"WinHelpW",4},{"wsprintfW",0},{"wsprintfA",0},{"GetDeviceCaps",2},{"SetDIBitsToDevice",12},{"SetLayout",2},{"SetPixel",4},
		{"SetROP2",2},
		// ANSI forms and cursor, key and text services served by the compat adapters or the shared registry.
		{"CreateWindowExA",12},{"DispatchMessageA",1},{"GetMessageA",4},{"PeekMessageA",5},{"RegisterClassA",1},{"RegisterClassExA",1},
		{"UnregisterClassA",2},{"LoadCursorA",2},{"LoadIconA",2},{"GetAsyncKeyState",1},{"SetCursor",1},{"GetCursorPos",1},{"SetCursorPos",2},
		{"DefWindowProcA",4},{"SendMessageA",4},{"PostMessageA",4},{"SetWindowTextA",2},
		{"CreateFontA",14},{"ExtTextOutA",8},{"GetDIBits",7},{"GetTextExtentPoint32A",4},{"GetTextMetricsA",2},{"SetDIBits",7},
		{"ExitWindowsEx",2},{"MessageBoxA",4},{"SetWindowPos",7},{"RealizePalette",1},{"SelectPalette",3},{"PatBlt",6},{"CreatePalette",1},
	};
	for (const auto module : {"user32.dll", "gdi32.dll"})
		for (const auto &item : host_exports(module))
			counts.emplace(item.name, item.words);
	// The display driver thunks each take one pointer to their parameter structure.
	for (const auto name : system_export_names("gdi32.dll"))
		if (name.starts_with("D3DKMT"))
			counts.emplace(name, 1);
	return counts;
	}();
	return counts;
}

// Services of libraries Raven does not implement yet fail visibly instead of reporting success.
bool unsupported_library(NativeProcess &process, NativeEnvironment &environment, std::string_view name)
{
	// There is no kernel graphics subsystem behind the host's display: the thunks report that the request is not supported (STATUS_NOT_SUPPORTED).
	if (name.starts_with("D3DKMT")) {
		process.api_arguments(0);
		process.return_api(0xC00000BBU);
		return true;
	}
	const bool known = name == "PlaySoundW" || name == "ShellAboutW" || name == "InitCommonControlsEx";
	if (!known)
		return false;
	process.api_arguments(0);
	environment.set_last_error(120);
	process.return_api(0);
	return true;
}

// Guest strings and small kernel32 services that need no shared state.
bool kernel_string_api(NativeProcess &process, NativeEnvironment &environment, const PEModule &entry,
	const NativeLaunch &launch, std::string_view name)
{
	if (name == "GetTickCount" || name == "timeGetTime") {
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
		process.return_api(uint32_t(now.count()));
		return true;
	}
	if (name == "ProcessIdToSessionId") {
		// Only the process itself is known, and it runs in the interactive session.
		const auto args = process.api_arguments(2);
		const auto own = process.read_memory(environment.teb() + 0x20, 4);
		const uint32_t own_id = own[0] | uint32_t(own[1]) << 8 | uint32_t(own[2]) << 16 | uint32_t(own[3]) << 24;
		if (uint32_t(args[0]) != own_id || !args[1]) {
			environment.set_last_error(87);
			process.return_api(0);
			return true;
		}
		const uint8_t session[4] = {1, 0, 0, 0};
		process.write_memory(args[1], session);
		process.return_api(1);
		return true;
	}
	if (name == "timeBeginPeriod" || name == "timeEndPeriod") {
		process.api_arguments(1);
		process.return_api(0);
		return true;
	}
	// The shared handler prints the message when the runtime plays the debugger.
	if (name == "OutputDebugStringA" && !std::getenv("RAVEN_TRACE_DEBUG")) {
		process.api_arguments(1);
		process.return_api(0);
		return true;
	}
	if (name == "lstrlenW" || name == "lstrcpyW") {
		const bool copy = name == "lstrcpyW";
		const auto args = process.api_arguments(copy ? 2 : 1);
		const uint64_t source = args[copy ? 1 : 0];
		size_t length = 0;
		try {
			// The characters up to the terminator, read a page at a time: text that runs into unreadable memory fails as Windows' guarded
			// copy does.
			for (bool ended = false; !ended;) {
				const uint64_t at = source + length * 2;
				const auto block = process.read_memory(at, std::max<uint64_t>(2, (4096 - (at & 4095)) & ~uint64_t(1)));
				for (size_t index = 0; index + 1 < block.size(); index += 2, ++length)
					if (!block[index] && !block[index + 1]) {
						ended = true;
						break;
					}
			}
			if (copy)
				process.write_memory(args[0], process.read_memory(source, length * 2 + 2));
		} catch (const std::runtime_error &failure) {
			const std::string_view message = failure.what();
			if (message.find("readable") == message.npos && message.find("writable") == message.npos)
				throw;
			environment.set_last_error(998);
			process.return_api(0);
			return true;
		}
		process.return_api(copy ? args[0] : length);
		return true;
	}
	if (name == "GetModuleFileNameA") {
		const auto args = process.api_arguments(3);
		// The file of a library is the shared handlers' answer, which know where each module came from.
		if (args[0] && args[0] != entry.loaded.base())
			return false;
		std::string path;
		for (const auto character : launch.image_path)
			path.push_back(character > 127 ? '?' : char(character));
		const size_t size = uint32_t(args[2]);
		if (!size) {
			environment.set_last_error(122);
			process.return_api(0);
			return true;
		}
		const bool truncated = path.size() >= size;
		if (truncated)
			path.resize(size - 1);
		path.push_back('\0');
		process.write_memory(args[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(path.data()), path.size()));
		if (truncated)
			environment.set_last_error(122);
		process.return_api(path.size() - 1);
		return true;
	}
	return false;
}

uint64_t provider_base(const ModuleSet &modules, uint32_t size)
{
	uint64_t base = 0x70000000ULL;
	for (;;) {
		bool overlap = false;
		for (const auto &[name, module] : modules.all()) {
			const uint64_t start = module->loaded.base();
			const uint64_t end = start + module->loaded.data().size();
			if (base < end && start < base + size) {
				base = (end + 65535) & ~uint64_t(65535);
				overlap = true;
				break;
			}
		}
		if (base >= 0xf0000000ULL - size)
			throw std::runtime_error("no address available for native x86 API provider");
		if (!overlap)
			return base;
	}
}

}

NativeResult run_native_process_x86(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout, std::optional<NativeLaunch> launch,
	DesktopPresentation presentation, std::function<std::optional<ModuleBytes>(std::string_view)> source)
{
	if (timeout.count() <= 0 || entry.image.is_64 || (entry.image.characteristics & 0x2000) ||
		!std::any_of(modules.all().begin(), modules.all().end(), [&](const auto &item) { return item.second.get() == &entry; }))
		throw std::runtime_error("native x86 process requires a PE32 executable graph and positive timeout");
	for (const auto &[name, module] : modules.all())
		if (module->image.is_64)
			throw std::runtime_error("native x86 process cannot mix PE64 modules: " + name);
	// Libraries the program ships and imports statically join its graph; the runtime's own libraries answer for the system ones.
	if (source) {
		// The system's libraries are the runtime's whatever copies the program ships or an installer left in the system directory.
		std::vector<std::string_view> provided{"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "setupapi.dll", "msvcrt.dll", "winmm.dll",
			"dsound.dll", "vulkan-1.dll", "shell32.dll", "comctl32.dll", "ntdll.dll", "version.dll", "ole32.dll"};
		provided.insert(provided.end(), system_library_names().begin(), system_library_names().end());
		if (!source("d3d9.dll"))
			provided.push_back("d3d9.dll");
		// A library that is not there is left for the binder, which names every unresolved import at once.
		load_initial_dependencies(modules, source, provided, false, true);
		// Libraries loaded later, such as installer plug-ins, import the C runtime by name: its component answers for them.
		if (!modules.all().contains("ucrtbase.dll"))
			if (auto bytes = source("ucrtbase.dll")) {
				const PEImage image(*bytes);
				if (!image.is_64)
					modules.add("ucrtbase.dll", std::move(*bytes));
			}
	}
	// What the graph imports from each system library by name. The runtime answers for all of it: a name that has a handler runs, a name that has
	// none fails visibly when the program calls it, so a program loads whether or not every function it imports is implemented yet.
	std::map<std::string, std::set<std::string>> imported_names;
	std::map<std::string, std::vector<std::pair<std::string_view, uint16_t>>> unknown_ordinals;
	std::set<std::string> unknown_names;
	{
		std::set<std::string> system_libraries(system_library_names().begin(), system_library_names().end());
		system_libraries.insert("usp10.dll");
		system_libraries.insert("ntdll.dll");
		for (const auto &[module_name, module] : modules.all())
			for (const auto &symbol : module->imports) {
				if (symbol.by_ordinal) {
					// A number with no name known for it still gets an export, so the program binds and the call it ends in names the number.
					auto library = symbol.module;
					for (auto &character : library)
						if (character >= 'A' && character <= 'Z')
							character = static_cast<char>(character + ('a' - 'A'));
					if (library.find('.') == std::string::npos)
						library += ".dll";
					const auto known = system_export_ordinals(library);
					if (system_libraries.contains(library) && !modules.all().contains(library) &&
						std::none_of(known.begin(), known.end(), [&](const auto &item) { return item.second == symbol.ordinal; })) {
						const auto name = "Ordinal" + std::to_string(symbol.ordinal);
						imported_names[library].insert(name);
						unknown_ordinals[library].emplace_back(*unknown_names.insert(name).first, symbol.ordinal);
					}
					continue;
				}
				auto library = symbol.module;
				for (auto &character : library)
					if (character >= 'A' && character <= 'Z')
						character = static_cast<char>(character + ('a' - 'A'));
				if (library.find('.') == std::string::npos)
					library += ".dll";
				if (system_libraries.contains(library) && !modules.all().contains(library))
					imported_names[library].insert(symbol.name);
			}
	}
	// A library's own list of names together with the ones the graph imports from it.
	const auto with_imports = [&](const std::string &library, std::span<const std::string_view> own) {
		std::vector<std::string_view> merged(own.begin(), own.end());
		// A library loaded later, such as an installer plug-in, imports names the initial graph never named: every public name binds.
		const auto published = system_export_names(library);
		merged.insert(merged.end(), published.begin(), published.end());
		if (const auto found = imported_names.find(library); found != imported_names.end())
			for (const auto &name : found->second)
				merged.push_back(name);
		std::sort(merged.begin(), merged.end());
		merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
		return merged;
	};
	// The stdcall argument words of a function: the library's own decorated symbol first, then what Raven's tables say.
	const auto words_of = [](const std::string &library, std::string_view name, uint32_t fallback) {
		const auto authoritative = system_export_words(library, name);
		if (authoritative != UINT32_MAX)
			return authoritative;
		return fallback;
	};
	// A guest component that implements part of a system library receives those calls directly, as for 64-bit programs.
	const auto overlay = [&](PEModule &provider, std::string_view library) {
		struct Rule { const char *library, *component, *prefix; };
		static constexpr Rule rules[] = {{"user32.dll", "raven-user32.dll", "raven-user32."}, {"comdlg32.dll", "raven-shell.dll", "raven-shell."},
			{"ole32.dll", "raven-shell.dll", "raven-shell."}, {"shell32.dll", "raven-shell.dll", "raven-shell."}, {"msvcrt.dll", "ucrtbase.dll", "ucrtbase."}, {"shlwapi.dll", "raven-shell.dll", "raven-shell."}, {"setupapi.dll", "raven-setupapi.dll", "raven-setupapi."},
			{"ole32.dll", "raven-storage.dll", "raven-storage."}, {"combase.dll", "raven-shell.dll", "raven-shell."}, {"runtimeobject.dll", "raven-shell.dll", "raven-shell."}};
		for (const auto &rule : rules) {
			if (library != rule.library)
				continue;
			const auto component = modules.all().find(rule.component);
			if (component == modules.all().end() || component->second->runtime_api)
				continue;
			// The startup and exception entry points of msvcrt.dll stay with the runtime, which knows the process.
			const auto host_crt = std::string_view(rule.library) == "msvcrt.dll" ? NativeCRT::exports() : std::span<const std::string_view>{};
			for (auto &symbol : provider.exports)
				if (!symbol.names.empty() && std::find(host_crt.begin(), host_crt.end(), symbol.names.front()) == host_crt.end() &&
					std::any_of(component->second->exports.begin(), component->second->exports.end(), [&](const auto &candidate) {
						return candidate.forwarder.empty() && std::find(candidate.names.begin(), candidate.names.end(), symbol.names.front()) != candidate.names.end();
					}))
					symbol.forwarder = rule.prefix + symbol.names.front();
		}
		forward_opengl_formats(modules, provider);
	};
	std::map<uint64_t, std::pair<std::string_view, uint32_t>> traps;
	if (!modules.all().contains("kernel32.dll")) {
		std::vector<std::string_view> own_names;
		for (const auto &item : kernel32_table())
			own_names.push_back(item.name);
		const auto names = with_imports("kernel32.dll", own_names);
		const auto size = PEImage(runtime_provider("kernel32.dll", 0x70000000ULL, names, true)).image_size;
		const auto base = provider_base(modules, size);
		auto &provider = modules.add("kernel32.dll", runtime_provider("kernel32.dll", base, names, true), base);
		provider.runtime_api = true;
		for (const auto &symbol : provider.exports) {
			const auto &exported = symbol.names.at(0);
			const auto found = std::find_if(kernel32_table().begin(), kernel32_table().end(), [&](const auto &item) { return item.name == exported; });
			const auto view = std::find(names.begin(), names.end(), exported);
			const uint32_t fixed = found != kernel32_table().end() ? found->words : registry_words(exported) != UINT32_MAX ? registry_words(exported) : 0;
			traps.emplace(provider.loaded.base() + symbol.address, std::pair{*view, words_of("kernel32.dll", exported, fixed) * 4});
		}
	}
	const auto add_desktop = [&](const char *dll, std::span<const std::string_view> implemented, std::span<const std::string_view> pending) {
		if (modules.all().contains(dll))
			return;
		std::vector<std::string_view> own(implemented.begin(), implemented.end());
		own.insert(own.end(), pending.begin(), pending.end());
		const auto exports = with_imports(dll, own);
		const auto size = PEImage(runtime_provider(dll, 0x70000000ULL, exports, true)).image_size;
		const auto base = provider_base(modules, size);
		auto &provider = modules.add(dll, runtime_provider(dll, base, exports, true), base);
		provider.runtime_api = true;
		overlay(provider, dll);
		for (const auto &symbol : provider.exports) {
			if (!symbol.forwarder.empty())
				continue;
			const auto &exported = symbol.names.at(0);
			const auto view = std::find(exports.begin(), exports.end(), exported);
			const auto found = desktop_arguments().find(exported);
			const uint32_t fixed = found != desktop_arguments().end() ? found->second : registry_words(exported) != UINT32_MAX ? registry_words(exported) : 0;
			traps.emplace(provider.loaded.base() + symbol.address, std::pair{*view, words_of(dll, exported, fixed) * 4});
		}
	};
	std::vector<std::string_view> user_pending{"CheckMenuItem", "DialogBoxParamW", "EndDialog", "GetDesktopWindow",
		"GetDlgItemInt", "GetDlgItemTextW", "GetMenuItemRect", "KillTimer", "LoadAcceleratorsW", "LoadCursorW", "LoadIconW",
		"MessageBoxW", "MoveWindow", "PtInRect", "ReleaseCapture", "SetCapture", "SetDlgItemInt", "SetDlgItemTextW", "SetMenu",
		"SetRect", "SetTimer", "TranslateAcceleratorW", "WinHelpW", "wsprintfW", "wsprintfA", "CreateWindowExA", "DispatchMessageA",
		"GetMessageA", "PeekMessageA", "RegisterClassA", "RegisterClassExA", "UnregisterClassA", "LoadCursorA", "LoadIconA", "GetAsyncKeyState",
		"SetCursor", "GetCursorPos", "SetCursorPos", "DefWindowProcA", "SendMessageA", "PostMessageA", "SetWindowTextA",
		"ExitWindowsEx", "MessageBoxA", "SetWindowPos", "CheckDlgButton", "MessageBoxIndirectW", "TrackPopupMenu", "DrawTextW",
		"CreateDialogParamW", "SendMessageTimeoutW"};
	std::vector<std::string_view> gdi_pending{"CreateBrushIndirect", "GetDeviceCaps", "SetDIBitsToDevice", "SetLayout", "SetPixel", "SetROP2",
		"CreateFontA", "ExtTextOutA", "GetDIBits", "GetTextExtentPoint32A", "GetTextMetricsA", "SetDIBits",
		"RealizePalette", "SelectPalette", "PatBlt", "CreatePalette"};
	for (const auto name : system_export_names("gdi32.dll"))
		if (name.starts_with("D3DKMT"))
			gdi_pending.push_back(name);
	for (const auto &item : host_exports("user32.dll"))
		user_pending.push_back(item.name);
	for (const auto &item : host_exports("gdi32.dll"))
		gdi_pending.push_back(item.name);
	add_desktop("user32.dll", NativeDesktop::user_exports(), user_pending);
	add_desktop("gdi32.dll", NativeDesktop::gdi_exports(), gdi_pending);
	// Libraries whose argument counts come from their own implementation or the unsupported-service table.
	const auto add_library = [&](const char *dll, std::span<const std::string_view> exports, std::span<const std::string_view> data,
		const std::function<uint32_t(std::string_view)> &count, std::span<const std::pair<std::string_view, uint16_t>> ordinals = {}) {
		if (modules.all().contains(dll))
			return;
		const auto merged = with_imports(dll, exports);
		const auto published_numbers = ordinals.empty() ? system_export_ordinals(dll) : ordinals;
		std::vector<std::pair<std::string_view, uint16_t>> numbered(published_numbers.begin(), published_numbers.end());
		if (const auto extra = unknown_ordinals.find(dll); extra != unknown_ordinals.end())
			numbered.insert(numbered.end(), extra->second.begin(), extra->second.end());
		const auto size = PEImage(runtime_provider(dll, 0x70000000ULL, merged, true, data, numbered)).image_size;
		const auto base = provider_base(modules, size);
		auto &provider = modules.add(dll, runtime_provider(dll, base, merged, true, data, numbered), base);
		provider.runtime_api = true;
		overlay(provider, dll);
		for (const auto &symbol : provider.exports) {
			if (!symbol.forwarder.empty())
				continue;
			// An export that has only an ordinal is the name the library lists for that ordinal.
			std::string exported;
			if (!symbol.names.empty()) {
				exported = symbol.names.front();
			} else {
				const auto found = std::find_if(numbered.begin(), numbered.end(), [&](const auto &item) { return item.second == symbol.ordinal; });
				// A slot that no name and no listed ordinal claims is padding between ordinals.
				if (found == numbered.end() || !symbol.address)
					continue;
				exported = found->first;
			}
			if (std::find(data.begin(), data.end(), exported) != data.end())
				continue;
			const auto stable = std::find(merged.begin(), merged.end(), exported);
			if (stable == merged.end())
				throw std::runtime_error("native x86 provider export is not in its list: " + std::string(dll) + "!" + exported);
			traps.emplace(provider.loaded.base() + symbol.address, std::pair{*stable, words_of(dll, exported, count(exported)) * 4});
		}
	};
	std::vector<std::string_view> advapi_names;
	for (const auto &item : host_exports("advapi32.dll"))
		advapi_names.push_back(item.name);
	std::sort(advapi_names.begin(), advapi_names.end());
	add_library("advapi32.dll", advapi_names, {}, [](std::string_view name) {
		for (const auto &item : host_exports("advapi32.dll"))
			if (item.name == name)
				return item.words;
		return uint32_t(0);
	});
	std::vector<std::string_view> setupapi_names;
	for (const auto &item : host_exports("setupapi.dll"))
		setupapi_names.push_back(item.name);
	std::sort(setupapi_names.begin(), setupapi_names.end());
	add_library("setupapi.dll", setupapi_names, {}, [](std::string_view name) {
		for (const auto &item : host_exports("setupapi.dll"))
			if (item.name == name)
				return item.words;
		return uint32_t(0);
	});
	// msvcrt.dll answers for the runtime's own entry points and, through forwarders, for everything the C runtime component implements.
	std::vector<std::string_view> crt_names(NativeCRT::exports().begin(), NativeCRT::exports().end());
	if (const auto component = modules.all().find("ucrtbase.dll"); component != modules.all().end())
		for (const auto &symbol : component->second->exports)
			if (!symbol.names.empty() && symbol.forwarder.empty())
				crt_names.push_back(symbol.names.front());
	std::sort(crt_names.begin(), crt_names.end());
	crt_names.erase(std::unique(crt_names.begin(), crt_names.end()), crt_names.end());
	add_library("msvcrt.dll", crt_names, NativeCRT::data_exports(), [](std::string_view name) { return uint32_t(0); });
	static constexpr std::array<std::string_view, 4> winmm_exports{"PlaySoundW", "timeBeginPeriod", "timeEndPeriod", "timeGetTime"};
	static constexpr std::array<std::string_view, 1> shell_exports{"ShellAboutW"};
	// File version resources are read by the shared handlers; the names are in byte order.
	static constexpr std::array<std::string_view, 8> version_exports{"GetFileVersionInfoA", "GetFileVersionInfoExW", "GetFileVersionInfoSizeA",
		"GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW", "VerQueryValueA", "VerQueryValueW"};
	static constexpr std::array<std::string_view, 1> controls_exports{"InitCommonControlsEx"};
	const auto malloc_names = com_names(malloc_methods);
	add_library("raven-com.dll", malloc_names, {}, [](std::string_view name) { return com_words(malloc_methods, name); });
	add_library("winmm.dll", winmm_exports, {}, [](std::string_view name) { return uint32_t(name == "PlaySoundW" ? 3 : name == "timeGetTime" ? 0 : 1); });
	const auto dsound_methods = NativeX86DirectSound::method_names();
	add_library("dsound.dll", NativeX86DirectSound::entry_names(), {}, [](std::string_view name) { return NativeX86DirectSound::entry_words(name); });
	add_library("raven-dsound.dll", dsound_methods, {}, [](std::string_view name) { return NativeX86DirectSound::method_words(name); });
	// A Direct3D 9 library that ships beside the program is the program's choice (DXVK, for one); the built-in one answers otherwise.
	bool d3d9_beside = false;
	if (launch) {
		const auto probe_prefix = NativePrefix::from_environment();
		const NativePaths probe(probe_prefix);
		const auto program = probe.resolve(launch->image_path);
		if (program && program->exists)
			for (const auto &item : std::filesystem::directory_iterator(program->host.parent_path())) {
				auto filename = item.path().filename().string();
				for (auto &character : filename)
					if (character >= 'A' && character <= 'Z')
						character = static_cast<char>(character + ('a' - 'A'));
				d3d9_beside |= filename == "d3d9.dll";
			}
	}
	const auto d3d9_methods = NativeX86D3D9::method_names();
	if (!d3d9_beside) {
		add_library("d3d9.dll", NativeX86D3D9::entry_names(), {}, [](std::string_view) { return uint32_t(1); });
		add_library("raven-d3d9.dll", d3d9_methods, {}, [](std::string_view name) { return NativeX86D3D9::method_words(name); });
	}
#ifdef RAVEN_VULKAN_THUNK32
	// The Vulkan loader for 32-bit programs: every command the bridge implements, taking 32-bit stdcall arguments.
	static const auto vulkan_exports = VulkanThunk32::exports(VulkanBridge::exports());
	std::vector<std::string_view> vulkan_names;
	for (const auto &item : vulkan_exports)
		vulkan_names.push_back(item.first);
	add_library("vulkan-1.dll", vulkan_names, {}, [](std::string_view name) {
		for (const auto &item : vulkan_exports)
			if (item.first == name)
				return item.second;
		return uint32_t(0);
	});
#endif
	add_library("version.dll", version_exports, {}, [](std::string_view name) { return registry_words(name); });
	std::vector<std::string_view> shell_names(shell_exports.begin(), shell_exports.end());
	shell_names.push_back("SHBrowseForFolderW");
	for (const auto &item : host_exports("shell32.dll"))
		shell_names.push_back(item.name);
	std::sort(shell_names.begin(), shell_names.end());
	shell_names.erase(std::unique(shell_names.begin(), shell_names.end()), shell_names.end());
	add_library("shell32.dll", shell_names, {}, [](std::string_view name) {
		if (name == "SHBrowseForFolderW")
			return uint32_t(1);
		for (const auto &item : host_exports("shell32.dll"))
			if (item.name == name)
				return item.words;
		return uint32_t(4);
	});
	// The common controls come from the common controls component once it is built for 32 bits; until then these names resolve and fail when called.
	static constexpr std::array<std::string_view, 5> controls_names{"ImageList_AddMasked", "ImageList_Create", "ImageList_Destroy", "InitCommonControls", "InitCommonControlsEx"};
	static constexpr std::array<std::pair<std::string_view, uint16_t>, 1> controls_ordinals{{{"InitCommonControls", 17}}};
	add_library("comctl32.dll", controls_names, {}, [](std::string_view name) {
		return uint32_t(name == "ImageList_Create" ? 5 : name == "ImageList_AddMasked" ? 3 : name == "ImageList_Destroy" || name == "InitCommonControlsEx" ? 1 : 0);
	}, controls_ordinals);
	std::vector<std::string_view> ole_names;
	for (const auto &item : host_exports("ole32.dll"))
		ole_names.push_back(item.name);
	std::sort(ole_names.begin(), ole_names.end());
	add_library("ole32.dll", ole_names, {}, [](std::string_view name) {
		for (const auto &item : host_exports("ole32.dll"))
			if (item.name == name)
				return item.words;
		return uint32_t(0);
	});
	// A system library the runtime has no provider for yet is served for the names the graph imports from it, with the shared handlers.
	std::set<std::string> served;
	for (const auto &[library, library_names] : imported_names) {
		(void)library_names;
		served.insert(library);
	}
	for (const auto library : system_library_names())
		served.insert(std::string(library));
	for (const auto &library : served) {
		if (modules.all().contains(library))
			continue;
		add_library(library.c_str(), {}, {}, [](std::string_view name) { return registry_words(name) != UINT32_MAX ? registry_words(name) : uint32_t(0); },
			system_export_ordinals(library));
	}
	alias_api_sets(modules);
	modules.bind_imports();
	std::vector<LoadedImage *> images;
	for (const auto &[name, module] : modules.all())
		images.push_back(&module->loaded);
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	NativeProcess process(images, bootstrap, timeout, NativeMode::x86, entry.image.stack_reserve);
	for (const auto &[address, api] : traps)
		process.add_api_trap(address, api.second);
	NativeEnvironment environment(process, entry.loaded.base(), uint32_t(::getpid()), 1);
	NativeX86D3D9 direct3d9(process);
	NativeX86DirectSound directsound(process);
	NativeX86Com task_malloc(process, malloc_methods);
	{
		std::map<std::string, uint64_t> method_traps;
		for (const auto &[address, api] : traps)
			if (direct3d9.com().owns(api.first) || directsound.com().owns(api.first) || task_malloc.owns(api.first))
				method_traps.emplace(std::string(api.first), address);
		direct3d9.com().set_trap_addresses(method_traps);
		task_malloc.set_trap_addresses(method_traps);
		directsound.com().set_trap_addresses(std::move(method_traps));
	}
	if (!launch) {
		NativeLaunch initial;
		for (const char byte : entry.name) {
			const auto character = static_cast<unsigned char>(byte);
			if (character > 127)
				throw std::runtime_error("native default launch identity requires ASCII");
			initial.image_path.push_back(char16_t(character));
		}
		initial.command_line = u"\"" + initial.image_path + u"\"";
		launch = std::move(initial);
	}
	NativeObjects kernel_objects;
	adopt_inherited(kernel_objects, launch->inherited);
	std::array<uint64_t, 3> standard_handles{UINT64_MAX, UINT64_MAX, UINT64_MAX};
	for (size_t i = 0; i < 3; ++i) {
		// A standard handle the creator also let the child inherit is that inherited object, under its own value.
		const auto shared = std::find_if(launch->inherited.begin(), launch->inherited.end(), [&](const InheritedHandle &record) {
			return (record.standard & (1U << i)) && kernel_objects.object(record.handle);
		});
		if (shared != launch->inherited.end()) {
			standard_handles[i] = shared->handle;
			continue;
		}
		if (launch->standard_fds[i] < 0)
			continue;
		const auto inherited = kernel_objects.inherit_file(launch->standard_fds[i]);
		if (inherited.status)
			throw std::runtime_error("cannot inherit native standard file");
		standard_handles[i] = inherited.value;
	}
	environment.prepare_parameters(*launch, standard_handles);
	NativeObjectAPI object_api(process, environment, kernel_objects, deadline);
	const auto prefix = NativePrefix::from_environment();
	prefix.place_runtime_components(std::filesystem::path(bootstrap).parent_path());
	NativeRegistry registry(prefix.registry_file());
	apply_registry_settings(registry);
	NativeProfile profile(process, environment, prefix);
	NativeCRT crt(process, environment);
	crt.initialize([&](std::string_view name) { return modules.resolve({"msvcrt.dll", std::string(name), 0, false, 0}); });
	NativeModuleAPI module_api(process, environment, modules, entry);
	NativePaths paths(prefix);
	paths.set_wow64(true);
	paths.set_package_roots([&registry] { return virtual_package_roots(registry); });
	if (!launch->directory.empty())
		paths.set_current(launch->directory);
	std::function<NativeResult(uint64_t, std::span<const uint64_t>)> call_guest;
	NativeResources resources(process, environment, modules, entry);
	// What a program may commit: 512 MiB by default, as set by RAVEN_COMMIT_LIMIT_MB (large programs, a browser engine for one, need more).
	const size_t commit_limit=(std::getenv("RAVEN_COMMIT_LIMIT_MB") ? size_t(std::strtoull(std::getenv("RAVEN_COMMIT_LIMIT_MB"),nullptr,10)) : size_t(512))*1024*1024;
	NativeMemory memory(process,environment,commit_limit);
	register_images(memory, modules);
	// Declared first so that it outlives the threads, which release their blocks through it when they are torn down.
	NativeX86Tls static_tls(process);
	NativeThreads guest_threads(process, environment, memory, nullptr, deadline);
	// Other processes ask for handles of this one and send theirs; the scheduler answers them.
	guest_threads.add_service([&kernel_objects] { receive_handles(kernel_objects); });
	NativeX86Threads thread_api(process, environment, kernel_objects, guest_threads);
	NativeX86System system_api(process, environment, kernel_objects, guest_threads, memory);
	static_tls.attach_main(environment.teb());
	guest_threads.set_block_hooks([&](uint64_t teb) { static_tls.create_thread(teb); }, [&](uint64_t teb) { static_tls.release_thread(teb); });
	object_api.set_cooperative(true);
	object_api.set_threads(&guest_threads);
	NativeX86Loader library_loader(process, environment, modules, paths,
		[&](uint64_t address, std::span<const uint64_t> arguments) { return call_guest(address, arguments); });
	library_loader.set_tls(&static_tls);
	library_loader.set_mapped([&memory](PEModule &module) { register_image(memory, module); });
	library_loader.set_unmapped([&memory](PEModule &module) { memory.remove_image(module.loaded.base()); });
	if (launch) {
		const auto separator = launch->image_path.find_last_of(u'\\');
		if (separator != std::u16string::npos)
			library_loader.set_application_directory(launch->image_path.substr(0, separator));
	}
	module_api.set_loader([&](std::string name) -> uint64_t {
		// DONT_RESOLVE_DLL_REFERENCES and the data file and resource flags map the library without starting it.
		return library_loader.load(std::move(name), module_api.load_flags() & 0x63, module_api.load_flags());
	}, [&](uint64_t handle) -> bool {
		if (library_loader.release(handle))
			return true;
		const bool known = std::any_of(modules.all().begin(), modules.all().end(),
			[&](const auto &item) { return item.second->loaded.base() == handle; });
		if (!known)
			environment.set_last_error(6);
		return known;
	});
	module_api.set_path_lookup([&](const std::string &path) { return library_loader.find(path); });
	DesktopObjects desktop_objects;
	NativeDesktop desktop(process, environment, desktop_objects, resources);
	// The built-in bitmaps, icons and cursors of user32 live in the component when it is there.
	if (const auto component = modules.all().find("raven-user32.dll"); component != modules.all().end() && !component->second->runtime_api)
		desktop.set_oem_module(component->second->loaded.base());
	NativeWindows windows(process, environment, desktop_objects);
	// Registry handlers whose arguments carry no pointer-width structures also serve 32-bit programs.
	ApiRegistry shared_api;
	windows.register_api(shared_api);
	desktop.register_gdi_api(shared_api);
	desktop.register_text_api(shared_api);
	desktop.register_image_api(shared_api);
	NativeX86Compat compat(process, environment, windows);
	// Shared handlers whose arguments are 32-bit words and whose structures carry no pointers.
	std::set<std::string_view> shared_names{"MoveWindow", "LoadCursorA", "LoadIconA", "GetAsyncKeyState", "SetCursor", "GetCursorPos", "SetCursorPos",
		"DefWindowProcA", "SendMessageA", "PostMessageA", "SetWindowTextA", "CreateFontA", "ExtTextOutA", "GetDIBits", "GetTextExtentPoint32A",
		"GetTextMetricsA", "SetDIBits", "MessageBoxA", "SetWindowPos", "SelectPalette", "PatBlt", "_lopen", "_lcreat", "_lread", "_lwrite", "_llseek", "lstrcpyA", "lstrlenA", "GetWindowsDirectoryA",
		"GlobalUnlock", "GlobalFree", "GlobalAlloc", "SetErrorMode", "GlobalLock", "GetOEMCP", "GetCPInfo", "WideCharToMultiByte", "DeleteFileA",
		"FreeEnvironmentStringsW", "GetEnvironmentStringsW", "GetVersion", "GetFileAttributesA", "CreateFileW", "DeleteFileW",
		"GetEnvironmentVariableA", "GetFileInformationByHandle", "MultiByteToWideChar", "QueryPerformanceCounter", "QueryPerformanceFrequency", "ReadFile",
		"SetFilePointer", "GetFileSizeEx", "FlushFileBuffers",
		"MonitorFromPoint", "WindowFromPoint", "ChildWindowFromPoint", "ChildWindowFromPointEx", "RealChildWindowFromPoint"};
	for (const auto &item : shared_exports)
		shared_names.insert(item.name);
	for (const auto name : shared_allowed)
		shared_names.insert(name);
	windows.set_deadline(deadline);
#ifdef RAVEN_VULKAN_THUNK32
	VulkanBridge vulkan(process, modules);
	vulkan.set_surface_source({[&windows](uint64_t window) { return windows.host_surface(window); },
		[&windows](uint64_t window) { windows.release_host_surface(window); },
		[&windows](uint64_t window) { return windows.client_size(window); }, presentation.platform});
	VulkanThunk32 vulkan32(process, vulkan);
#endif
	windows.set_presentation(std::move(presentation));
	desktop.set_updated([&](uint64_t window) { windows.drawing(window); });
	struct ProcessExit { uint32_t code; uint64_t instruction; };
	// CoGetMalloc hands out one allocator object, whose methods are the task allocator's handlers.
	uint64_t malloc_object = 0;
	const auto malloc_dispatch = [&](std::string_view api) {
		const auto through = [&](const char *name, std::vector<uint64_t> arguments) {
			ApiCall inner(process, environment, std::move(arguments));
			shared_api.find(name)->second(inner);
			return inner.result;
		};
		if (api == "CoGetMalloc") {
			const auto args = process.api_arguments(2);
			// MEMCTX_TASK is the only context; S_OK, E_INVALIDARG or E_POINTER.
			if (!args[1]) {
				process.return_api(0x80004003U);
			} else if (args[0] != 1) {
				task_malloc.write32(args[1], 0);
				process.return_api(0x80070057U);
			} else {
				if (!malloc_object)
					malloc_object = task_malloc.make_object("IMalloc");
				task_malloc.write32(args[1], uint32_t(malloc_object));
				process.return_api(0);
			}
			return true;
		}
		if (!task_malloc.owns(api))
			return false;
		const auto args = process.api_arguments(task_malloc.words(api));
		const auto method = task_malloc.split(api).second;
		if (method == "AddRef" || method == "Release") {
			process.return_api(1);
		} else if (method == "QueryInterface") {
			// IID_IUnknown and IID_IMalloc: {00000000-...} and {00000002-0000-0000-C000-000000000046}.
			const auto iid = args[1] ? process.read_memory(args[1], 16) : std::vector<uint8_t>(16, 0xff);
			static constexpr uint8_t tail[12] = {0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
			const bool known = (iid[0] == 0 || iid[0] == 2) && !iid[1] && !iid[2] && !iid[3] && std::equal(std::begin(tail), std::end(tail), iid.begin() + 4);
			if (args[2])
				task_malloc.write32(args[2], known ? uint32_t(args[0]) : 0);
			process.return_api(!args[2] ? 0x80004003U : known ? 0 : 0x80004002U);
		} else if (method == "Alloc") {
			process.return_api(through("CoTaskMemAlloc", {args[1]}));
		} else if (method == "Realloc") {
			process.return_api(through("CoTaskMemRealloc", {args[1], args[2]}));
		} else if (method == "Free") {
			through("CoTaskMemFree", {args[1]});
			process.return_api(0);
		} else if (method == "GetSize" || method == "DidAlloc") {
			// The size is not known and every block may be this allocator's.
			process.return_api(0xffffffffU);
		} else {
			process.return_api(0);
		}
		return true;
	};
	const auto remaining = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()); };
	// Runs one guest call, answering every API trap until the call returns or stops for another reason.
	const auto invoke = [&](uint64_t address, std::span<const uint64_t> arguments) {
		auto budget = remaining();
		if (budget.count() <= 0)
			return NativeResult{NativeStop::timeout, 0, address, 0};
		auto result = process.active() ? process.callback(address, arguments, budget) : process.call(address, arguments, budget);
		while (result.stop == NativeStop::api) {
			const auto found = traps.find(result.value);
			if (found == traps.end())
				return result;
			const auto api = found->second.first;
			if (getenv("RAVEN_TRACE_X86")) {
				std::ostringstream line;
				if (getenv("RAVEN_TRACE_X86_THREADS"))
					line << '[' << environment.thread_id() << "] ";
				// Argument words only reflect stdcall exports; cdecl and variadic entries report none.
				line << "api " << api << '(' << std::hex;
				const auto count = found->second.second / 4;
				if (count)
					for (const auto value : process.api_arguments(count))
						line << ' ' << value;
				line << std::dec << " )";
				// RAVEN_TRACE_X86=2 also shows every argument that points at readable text, narrow or wide.
				if (const char *level = getenv("RAVEN_TRACE_X86"); level && level[0] == '2' && count)
					for (const auto value : process.api_arguments(count)) {
						if (value < 0x10000 || value > 0x7fffffffULL)
							continue;
						try {
							const auto bytes = process.read_memory(value, 120);
							std::string text;
							const bool wide = bytes[1] == 0 && bytes[0] >= 32 && bytes[0] < 127 && bytes[2] >= 32 && bytes[2] < 127 && bytes[3] == 0;
							for (size_t at = 0; at < bytes.size(); at += wide ? 2 : 1) {
								const auto character = bytes[at];
								if (character == 0 && (!wide || bytes[at + 1] == 0))
									break;
								if (character < 32 || character > 126 || (wide && bytes[at + 1])) {
									text.clear();
									break;
								}
								text.push_back(char(character));
							}
							if (text.size() >= 3)
								line << " <" << text << '>';
						} catch (const std::runtime_error &) {
						}
					}
				// Window classes and texts are what a diagnosis of a window needs most.
				{
					static const std::map<std::string_view, std::vector<size_t>> wide_strings{{"SetWindowTextW", {1}}, {"SetDlgItemTextW", {2}},
						{"CreateWindowExW", {1, 2}}, {"RegisterClassW", {}}, {"MessageBoxW", {1, 2}}, {"LoadStringW", {}}, {"CreateProcessW", {0, 1, 7}}, {"ShellExecuteW", {1, 2, 3}}, {"LoadLibraryW", {0}}, {"CreateFileW", {0}}, {"GetFileAttributesW", {0}},
						{"GetFileAttributesExW", {0}}, {"FindFirstFileExW", {0}}, {"CommandLineToArgvW", {0}},
						{"WinHttpAddRequestHeaders", {1}}, {"WinHttpOpenRequest", {1, 2}}, {"WinHttpConnect", {1}},
						{"PathIsURLW", {0}}, {"PathSearchAndQualifyW", {0}}, {"UrlApplySchemeW", {0}}, {"GetFullPathNameW", {0}}, {"RegOpenKeyExW", {1}},
						{"RegCreateKeyExW", {1}}, {"RegQueryValueExW", {1}}, {"RegSetValueExW", {1}}, {"lstrcmpiW", {0, 1}}, {"OpenThemeData", {1}},
						{"OpenThemeDataEx", {1}}, {"SetWindowTheme", {1, 2}}};
					if (const auto entry = wide_strings.find(api); entry != wide_strings.end() && !entry->second.empty()) {
						const auto words = process.api_arguments(found->second.second / 4);
						for (const auto index : entry->second) {
							if (index >= words.size() || words[index] <= 0xffff)
								continue;
							try {
								// A whole path when the memory holds one; a string near the end of its page is read shorter.
								std::vector<uint8_t> bytes;
								try {
									bytes = process.read_memory(words[index], 520);
								} catch (const std::runtime_error &) {
									bytes = process.read_memory(words[index], 200);
								}
								line << " [" << index << "]=\"";
								for (size_t at = 0; at + 1 < bytes.size() && (bytes[at] || bytes[at + 1]); at += 2)
									line << (bytes[at + 1] == 0 && bytes[at] >= 32 && bytes[at] < 127 ? char(bytes[at]) : '?');
								line << '"';
							} catch (...) {}
						}
					}
				}
				// The caller, as the module and offset the return address falls in.
				try {
					const auto stack = process.read_memory(process.context().registers.rsp, 4);
					const uint64_t caller = uint64_t(stack[0]) | uint64_t(stack[1]) << 8 | uint64_t(stack[2]) << 16 | uint64_t(stack[3]) << 24;
					for (const auto &[module_name, module] : modules.all())
						if (caller >= module->loaded.base() && caller < module->loaded.base() + module->image.image_size)
							line << " from " << module_name << '+' << std::hex << caller - module->loaded.base() << std::dec;
				} catch (const std::runtime_error &) {
				}
				// The names a program resolves at run time are what a diagnosis needs most.
				if (api == "LoadLibraryA" || api == "LoadLibraryW" || api == "GetProcAddress") {
					const auto words = process.api_arguments(api == "GetProcAddress" ? 2 : 1);
					const auto text = words[api == "GetProcAddress" ? 1 : 0];
					if (text > 0xffff) {
						try {
							const bool wide = api == "LoadLibraryW";
							const auto bytes = process.read_memory(text, 64);
							line << " \"";
							for (size_t at = 0; at + (wide ? 1 : 0) < bytes.size() && bytes[at] && (!wide || at % 2 == 0); at += wide ? 2 : 1)
								line << char(bytes[at]);
							line << '"';
						} catch (...) {}
					}
				}
				line << '\n';
				// One write per line, so the traces of a program and of the programs it starts stay readable side by side.
				std::cerr << line.str() << std::flush;
			}
			if (api == "VerSetConditionMask" || api == "VerifyVersionInfoW") {
				// The 64-bit condition mask is two words on the stack of a 32-bit program, and the handler takes it whole.
				const bool set = api == "VerSetConditionMask";
				const auto words = process.api_arguments(4);
				const auto entry = shared_api.find(api);
				if (!entry) {
					std::cerr << "raven: unimplemented native x86 API " << api << '\n';
					return result;
				}
				const uint64_t mask = (words[set ? 0 : 2] & 0xffffffffULL) | words[set ? 1 : 3] << 32;
				ApiCall inner(process, environment, set ? std::vector<uint64_t>{mask, words[2], words[3]} : std::vector<uint64_t>{words[0], words[1], mask});
				entry->second(inner);
				process.return_api(inner.result);
				budget = remaining();
				result = process.resume(budget);
				continue;
			}
			if (api == "StopTraceA" || api == "StopTraceW" || api == "ControlTraceA" || api == "ControlTraceW") {
				const bool control = api.starts_with("Control");
				const auto words = process.api_arguments(control ? 5 : 4);
				const auto entry = shared_api.find(api);
				if (!entry) {
					std::cerr << "raven: unimplemented native x86 API " << api << '\n';
					return result;
				}
				std::vector<uint64_t> arguments{words[0] | words[1] << 32, words[2], words[3]};
				if (control) arguments.push_back(words[4]);
				ApiCall inner(process, environment, std::move(arguments));
				entry->second(inner);
				process.return_api(inner.result);
				budget = remaining();
				result = process.resume(budget);
				continue;
			}
			if (api == "SetFilePointerEx") {
				// The 64-bit distance arrives as two words; the shared handler takes it whole.
				const auto words = process.api_arguments(5);
				const auto entry = shared_api.find("SetFilePointerEx");
				if (!entry) {
					std::cerr << "raven: unimplemented native x86 API " << api << '\n';
					return result;
				}
				ApiCall inner(process, environment, {words[0], uint64_t(int64_t(int32_t(words[2])) << 32 | words[1]), words[3], words[4]});
				entry->second(inner);
				process.return_api(inner.result);
				budget = remaining();
				result = process.resume(budget);
				continue;
			}
			// Exceptions are dispatched in the program, by its vectored handlers and along the frames its threads register, by the C runtime
			// component: the call goes on there with the same arguments, so handlers that resume elsewhere, as a C++ catch does, never return
			// to the runtime. The vectored handler lists live there too, and fibers, which the thread switches by itself.
			static const std::map<std::string_view, std::string_view> in_runtime{{"RaiseException", "raven_raise_exception"},
				{"RtlUnwind", "raven_unwind"}, {"AddVectoredExceptionHandler", "raven_add_vectored_exception"},
				{"AddVectoredContinueHandler", "raven_add_vectored_continue"}, {"RemoveVectoredExceptionHandler", "raven_remove_vectored"},
				{"RemoveVectoredContinueHandler", "raven_remove_vectored"}, {"CreateFiber", "raven_create_fiber"},
				{"CreateFiberEx", "raven_create_fiber_ex"}, {"ConvertThreadToFiber", "raven_convert_thread_to_fiber"},
				{"ConvertThreadToFiberEx", "raven_convert_thread_to_fiber_ex"}, {"ConvertFiberToThread", "raven_convert_fiber_to_thread"},
				{"SwitchToFiber", "raven_switch_to_fiber"}, {"DeleteFiber", "raven_delete_fiber"}, {"IsThreadAFiber", "raven_is_thread_a_fiber"}};
			if (const auto routed = in_runtime.find(api); routed != in_runtime.end())
				if (const auto component = modules.all().find("ucrtbase.dll"); component != modules.all().end()) {
					const std::string_view wanted = routed->second;
					const auto &exports = component->second->exports;
					const auto found = std::find_if(exports.begin(), exports.end(), [&](const auto &symbol) {
						return std::find(symbol.names.begin(), symbol.names.end(), wanted) != symbol.names.end();
					});
					if (found != exports.end()) {
						auto native = process.context();
						native.registers.rip = component->second->loaded.base() + found->address;
						native.registers.orig_rax = uint64_t(-1);
						process.redirect(native, process.callback_depth());
						budget = remaining();
						result = process.resume(budget);
						continue;
					}
				}
			if (api == "RaiseException") {
				const auto words = process.api_arguments(4);
				std::cerr << "raven: unhandled exception 0x" << std::hex << uint32_t(words[0]) << std::dec << " raised by the program\n";
				process.abandon_call();
				throw ProcessExit{uint32_t(words[0]), result.instruction};
			}
			if (api == "CreateFontA") {
				// The face name is an ANSI string; the shared handler takes the wide form of the same call.
				auto words = process.api_arguments(14);
				words[13] = compat.widen(words[13]);
				const auto entry = shared_api.find("CreateFontW");
				if (!entry) {
					std::cerr << "raven: unimplemented native x86 API " << api << '\n';
					return result;
				}
				ApiCall inner(process, environment, words);
				entry->second(inner);
				process.return_api(inner.result);
				budget = remaining();
				result = process.resume(budget);
				continue;
			}
			// A POINT passed by value is two words here and one packed word in the shared handler: the words of each call and where its
			// point starts.
			static const std::map<std::string_view, std::pair<size_t, size_t>> point_calls{{"MonitorFromPoint", {3, 0}}, {"WindowFromPoint", {2, 0}},
				{"ChildWindowFromPoint", {3, 1}}, {"ChildWindowFromPointEx", {4, 1}}, {"RealChildWindowFromPoint", {3, 1}}};
			if (const auto point = point_calls.find(api); point != point_calls.end()) {
				auto words = process.api_arguments(point->second.first);
				const auto entry = shared_api.find(std::string(api));
				if (!entry) {
					std::cerr << "raven: unimplemented native x86 API " << api << '\n';
					return result;
				}
				const size_t at = point->second.second;
				words[at] = uint64_t(uint32_t(words[at + 1])) << 32 | uint32_t(words[at]);
				words.erase(words.begin() + ptrdiff_t(at + 1));
				ApiCall inner(process, environment, words);
				entry->second(inner);
				process.return_api(inner.result);
				budget = remaining();
				result = process.resume(budget);
				continue;
			}
			if (api == "DuplicateHandle") {
				// Local and foreign copies use the same access mapping; pseudo handles widen to the shared handler's values.
				auto words = process.api_arguments(7);
				for (const size_t index : {0, 1, 2})
					if (words[index] >= 0xfffffffaULL)
						words[index] |= 0xffffffff00000000ULL;
				const auto entry = shared_api.find("DuplicateHandle");
				ApiCall inner(process, environment, words);
				entry->second(inner);
				process.return_api(inner.result);
				budget = remaining();
				result = process.resume(budget);
				continue;
			}
			if (api == "TerminateProcess") {
				const auto words = process.api_arguments(2);
				if (words[0] != UINT32_MAX) {
					const auto entry = shared_api.find("TerminateProcess");
					ApiCall inner(process, environment, {uint64_t(words[0]), uint64_t(words[1])});
					entry->second(inner);
					process.return_api(inner.result);
					budget = remaining();
					result = process.resume(budget);
					continue;
				}
				process.abandon_call();
				throw ProcessExit{uint32_t(words[1]), result.instruction};
			}
			if (api == "ExitProcess" || api == "exit" || api == "_exit") {
				const auto code = uint32_t(process.api_arguments(1)[0]);
				process.abandon_call();
				throw ProcessExit{code, result.instruction};
			}
			if (!environment.dispatch(api) && !thread_api.dispatch(api) && !system_api.dispatch(api) && !compat.dispatch(api) && !direct3d9.dispatch(api) && !directsound.dispatch(api) &&
				!malloc_dispatch(api) &&
#ifdef RAVEN_VULKAN_THUNK32
				!vulkan32.dispatch(api) &&
#endif
				 !object_api.dispatch(api) && !module_api.dispatch(api) && !resources.dispatch(api) &&
				!memory.dispatch(api) && !kernel_string_api(process, environment, entry, *launch, api) &&
				!shared_api.dispatch(process, environment, api, true) && !desktop.dispatch(api) && !windows.dispatch(api) && !profile.dispatch(api) &&
				!crt.dispatch(api) && !unsupported_library(process, environment, api)) {
				std::cerr << "raven: unimplemented native x86 API " << api << '\n';
				if (api.starts_with("InitCommonControls") || api.starts_with("ImageList_"))
					std::cerr << "raven: 32-bit programs take the common controls from the component built by scripts/provision-controls.py --arch x86;"
						" configure Raven with RAVEN_CONTROLS32_COMPONENT_DIR\n";
				// RAVEN_CONTINUE_UNIMPLEMENTED lists every missing function of a run: each call fails with ERROR_CALL_NOT_IMPLEMENTED instead.
				if (!std::getenv("RAVEN_CONTINUE_UNIMPLEMENTED"))
					return result;
				environment.set_last_error(120);
				process.return_api(0);
			}
			budget = remaining();
			if (budget.count() <= 0)
				return NativeResult{NativeStop::timeout, 0, result.instruction, 0};
			result = process.resume(budget);
		}
		if (result.stop == NativeStop::fault && getenv("RAVEN_TRACE_X86")) {
			// What a crash needs: the registers, the bytes at the instruction and the top of the stack.
			const auto context = process.context();
			const auto &r = context.registers;
			std::cerr << std::hex << "fault: eip " << uint32_t(r.rip) << " eax " << uint32_t(r.rax) << " ebx " << uint32_t(r.rbx) << " ecx " << uint32_t(r.rcx)
				<< " edx " << uint32_t(r.rdx) << " esi " << uint32_t(r.rsi) << " edi " << uint32_t(r.rdi) << " ebp " << uint32_t(r.rbp) << " esp "
				<< uint32_t(r.rsp) << " address " << result.fault_address << "\ncode:";
			try {
				for (const auto byte : process.read_memory(r.rip - 64, 96))
					std::cerr << ' ' << unsigned(byte);
				std::cerr << "\nstack:";
				for (const auto byte : process.read_memory(r.rsp, 256))
					std::cerr << ' ' << unsigned(byte);
			} catch (...) {}
			std::cerr << std::dec << '\n';
		}
		return result;
	};
	windows.set_call(invoke);
	guest_threads.set_runner(invoke);
	// Threads announce themselves to the images that keep per-thread state: TLS callbacks first, then the library entry points.
	guest_threads.set_notifier([&](uint32_t reason) {
		const auto notify = [&](const PEModule &module) {
			for (const auto callback : static_tls.callbacks(module)) {
				const uint64_t arguments[] = {module.loaded.base(), reason, 0};
				const auto result = invoke(callback, arguments);
				if (result.stop != NativeStop::returned)
					throw result;
			}
			if ((module.image.characteristics & 0x2000) && module.image.entry && !module.runtime_api && &module != &entry) {
				const uint64_t arguments[] = {module.loaded.base(), reason, 0};
				const auto result = invoke(module.loaded.base() + module.image.entry, arguments);
				if (result.stop != NativeStop::returned)
					throw result;
			}
		};
		if (reason == 2) {
			notify(entry);
			for (const auto &[name, module] : modules.all())
				if (module.get() != &entry)
					notify(*module);
		} else {
			for (const auto &[name, module] : modules.all())
				if (module.get() != &entry)
					notify(*module);
			notify(entry);
		}
	});
	call_guest = invoke;
	// File, memory, text and system handlers of the shared registry take 32-bit words as well; they get the same process services.
	NativeHeap guest_heap(process, environment, modules, entry.image);
	GuestAllocator allocator(memory, guest_heap);
	compat.set_global_blocks([&allocator](uint64_t address) { return allocator.size(address); }, [&allocator](uint64_t address) { return allocator.release(address); });
	KernelContext kernel{process, environment, memory, allocator, kernel_objects, guest_threads, paths, modules, entry, *launch, invoke};
	guest_threads.set_watched([reactor = kernel.reactor](std::vector<pollfd> &descriptors) { reactor->watch(descriptors); });
	// Waits that also wake for messages ask the window system, and the window system sleeps as the scheduler does.
	kernel.queue_status = [&windows](uint32_t thread,uint32_t mask,bool clear) { return windows.queue_status(thread,mask,clear); };
	kernel.module_named = [&module_api](uint64_t address, bool wide) { return module_api.module_handle(address, wide); };
	windows.set_idle_sources([&guest_threads](std::chrono::steady_clock::time_point limit) { return guest_threads.idle_until(limit); },
		[&guest_threads](std::vector<pollfd> &descriptors) { guest_threads.idle_descriptors(descriptors); }, [&guest_threads] { guest_threads.drain_wake(); });
	register_file_api(shared_api, kernel);
	register_memory_api(shared_api, kernel);
	register_text_api(shared_api, kernel);
	register_console_api(shared_api, kernel);
	register_system_api(shared_api, kernel);
	register_version_api(shared_api, kernel);
	register_registry_api(shared_api, kernel, registry);
	register_service_api(shared_api, kernel, registry);
	register_shell_api(shared_api, kernel);
	register_shlwapi_api(shared_api, kernel);
	register_platform_api(shared_api, kernel);
	register_actctx_api(shared_api, kernel);
	register_mmio_api(shared_api, kernel);
	register_process_api(shared_api, kernel);
	register_thread_api(shared_api, kernel);
	register_sync_api(shared_api, kernel);
	register_security_api(shared_api, kernel);
	register_child_process_api(shared_api, kernel);
	register_winsock_api(shared_api, kernel);
#ifdef RAVEN_OPENSSL
	register_wininet_api(shared_api, kernel);
#endif
	register_host_api(shared_api, kernel);
	register_pipe_api(shared_api, kernel);
	register_job_api(shared_api, kernel);
	register_crypt32_api(shared_api, kernel);
	register_bcrypt_api(shared_api, kernel);
	register_process_info_api(shared_api, kernel);
	register_font_service(shared_api, kernel);
	// A 32-bit program runs under WOW64 on the 64-bit system Raven presents, its System32 being SysWOW64 until it switches redirection
	// off. The value it keeps for the switch back is 1 when redirection was on.
	shared_api.add("Wow64DisableWow64FsRedirection", 1, [&paths](ApiCall &call) {
		const bool was = paths.redirect(false);
		if (call[0])
			call.store<uint32_t>(call[0], was ? 1 : 0);
		call.ret(1);
	});
	shared_api.add("Wow64RevertWow64FsRedirection", 1, [&paths](ApiCall &call) {
		paths.redirect(call[0] != 0);
		call.ret(1);
	});
	// The older switch: a boolean that turns redirection on or off.
	shared_api.add("Wow64EnableWow64FsRedirection", 1, [&paths](ApiCall &call) {
		paths.redirect((call.u32(0) & 0xff) != 0);
		call.ret(1);
	});
	// Waveform audio for 32-bit programs has no device yet: they hear that none exists, as on a machine without sound, and go on silent.
	shared_api.add("waveOutGetNumDevs", 0, [](ApiCall &call) { call.ret(0); });
	shared_api.add("waveInGetNumDevs", 0, [](ApiCall &call) { call.ret(0); });
	for (const auto *name : {"waveOutGetDevCapsW", "waveOutGetDevCapsA", "waveInGetDevCapsW", "waveInGetDevCapsA"})
		shared_api.add(name, 3, [](ApiCall &call) { call.ret(2); });
	register_profile_api(shared_api, profile);
	register_ansi_adapters(shared_api);
	register_legacy_file_api(shared_api);
	windows.set_message_wait([&guest_threads](std::function<bool()> ready,std::chrono::steady_clock::time_point until) {
		HostWait waiting;
		guest_threads.block(std::move(ready),until);
	});
	// A message sent to a window of another thread waits for that thread to run its procedure.
	windows.set_block([&guest_threads](std::function<bool()> ready) {
		HostWait waiting;
		guest_threads.block(std::move(ready), std::nullopt);
	});
	crt.set_call(invoke);
	try {
		wait_suspended_process(launch->suspended_fd, kernel_objects);
		// Images of the startup graph with thread local storage get their blocks, and their callbacks hear about the process starting.
		static_tls.add(entry);
		for (const auto &[name, module] : modules.all())
			if (module.get() != &entry)
				static_tls.add(*module);
		// Dependencies first, the program last: each image's callbacks, then the entry point of a library that ships with the program.
		const auto start_image = [&](const PEModule &module) {
			for (const auto callback : static_tls.callbacks(module)) {
				const uint64_t arguments[] = {module.loaded.base(), 1, 0};
				const auto started = invoke(callback, arguments);
				if (started.stop != NativeStop::returned)
					throw started;
			}
		};
		auto order = initialization_order(modules, entry);
		// The user32 component registers the built-in classes in its entry point, before any library that subclasses them.
		if (const auto controls = modules.all().find("raven-user32.dll"); controls != modules.all().end() && !controls->second->runtime_api) {
			const auto found = std::find(order.begin(), order.end(), controls->second.get());
			if (found != order.end()) {
				order.erase(found);
				order.insert(order.begin(), controls->second.get());
			}
		}
		for (const auto *module : order) {
			if (module == &entry)
				continue;
			start_image(*module);
			if ((module->image.characteristics & 0x2000) && module->image.entry && !module->runtime_api) {
				const uint64_t arguments[] = {module->loaded.base(), 1, 0};
				const auto attached = invoke(module->loaded.base() + module->image.entry, arguments);
				if (attached.stop != NativeStop::returned)
					throw attached;
				if (static_cast<uint32_t>(attached.value) == 0)
					throw std::runtime_error("DLL_PROCESS_ATTACH failed: " + module->name);
			}
		}
		start_image(entry);
		const auto result = invoke(program_start(modules, entry), {});
		windows.shutdown();
		return result;
	} catch (const ProcessExit &request) {
		guest_threads.stop_time_slicing();
		windows.shutdown();
		return {NativeStop::exited, request.code, request.instruction, 0};
	} catch (const NativeResult &stopped) {
		windows.shutdown();
		return stopped;
	}
}

}
