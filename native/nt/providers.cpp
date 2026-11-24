#include "providers.h"
#include "desktop_lock.h"
#include "child_process.h"
#include "handle_channel.h"
#include "warm_host.h"
#include "section.h"
#include "ansi_api.h"
#include "module_api.h"
#include "loader.h"
#include "tls.h"
#include "exceptions.h"
#include "resources.h"
#include "memory.h"
#include "object_api.h"
#include "heap.h"
#include "api.h"
#include "apiset.h"
#include "text.h"
#include "kernel_context.h"
#include "clock_calibration.h"
#include "event.h"
#include "kernel_waits.h"
#include "event_pulse.h"
#include "actctx.h"
#include "unwind.h"
#include "prefix.h"
#include "profile_api.h"
#include "runtime_profile.h"
#include "registry.h"
#include "system_exports.h"
#include "desktop/api.h"
#include "desktop/windows.h"
#include "cpu/served_queue.h"
#include "cpu/preludes.h"
#include "cpu/served_stub.h"
#include "cpu/call_cycles.h"
#ifdef RAVEN_OPENGL
#include "graphics/opengl_bridge.h"
#endif
#ifdef RAVEN_VULKAN
#include "graphics/vulkan_bridge.h"
#endif

#include <unordered_map>
#include <unistd.h>
#include <sys/mman.h>
#include <x86intrin.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <array>
#include <stdexcept>
#include <csignal>
#include <set>
#include <unordered_set>

namespace raven {

// A whole host file in one read: libraries such as a browser's main DLL run to hundreds of megabytes, and reading them a character at
// a time through a stream iterator cost seconds in every process that loads them.
static std::vector<uint8_t> read_host_file(const std::filesystem::path &path)
{
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	const auto size = stream ? stream.tellg() : std::streampos(-1);
	if (size <= 0)
		return {};
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	stream.seekg(0);
	if (!stream.read(reinterpret_cast<char *>(bytes.data()), size))
		bytes.resize(static_cast<size_t>(stream.gcount()));
	return bytes;
}

// The PathCch functions live in kernelbase.dll on Windows and are not in the import libraries the public lists come from.
static constexpr std::array<std::string_view, 51> names{
	"CloseHandle","CreateEventA","CreateEventW","ExitProcess",
	"FindResourceA", "FindResourceExA", "FindResourceExW", "FindResourceW", "FreeLibrary",
	"GetACP", "GetCommandLineA", "GetCommandLineW", "GetCurrentProcessId", "GetCurrentThreadId", "GetFileType", "GetLastError", "GetModuleHandleA",
	"GetModuleHandleW", "GetProcAddress", "GetProcessHeap", "GetStartupInfoA", "GetStartupInfoW", "GetStdHandle", "HeapAlloc", "HeapCreate", "HeapDestroy", "HeapFree", "HeapReAlloc", "HeapSize",
	"LoadLibraryA", "LoadLibraryW", "LoadResource",
	"LockResource", "MultiByteToWideChar", "PathAllocCanonicalize", "PathCchCanonicalize", "PathCchCanonicalizeEx", "PathCchCombineEx", "PathCchRemoveFileSpec", "PathCchSkipRoot", "RaiseException", "ResetEvent", "SetEvent", "SetLastError", "SizeofResource",
	"VirtualAlloc", "VirtualFree", "VirtualProtect", "VirtualQuery", "WaitForSingleObject", "WriteFile"
};

std::vector<uint8_t> environment_provider(std::string_view name, uint64_t base)
{
	if (name != "kernel32.dll" && name != "kernelbase.dll")
		throw std::runtime_error("unsupported environment provider module");
	return runtime_provider(name, base, names);
}

namespace {

bool ends_with_process = false;
std::atomic<bool> releasing{true};

// Hot kernel32 services that run as guest machine code on the thread block; the slow path traps at offset 0x30.
struct FastStub {
	std::string_view name;
	std::vector<uint8_t> code;
};

const std::vector<FastStub> &fast_stubs()
{
	static const std::vector<FastStub> stubs = {
		// mov eax, gs:[0x68]; ret
		{"GetLastError", {0x65, 0x8b, 0x04, 0x25, 0x68, 0, 0, 0, 0xc3}},
		// mov eax, gs:[0x48]; ret (the thread's ClientId.UniqueThread)
		{"GetCurrentThreadId", {0x65, 0x8b, 0x04, 0x25, 0x48, 0, 0, 0, 0xc3}},
		// mov eax, gs:[0x40]; ret (the thread's ClientId.UniqueProcess)
		{"GetCurrentProcessId", {0x65, 0x8b, 0x04, 0x25, 0x40, 0, 0, 0, 0xc3}},
		// mov gs:[0x68], ecx; ret
		{"SetLastError", {0x65, 0x89, 0x0c, 0x25, 0x68, 0, 0, 0, 0xc3}},
		// mov rax, -1; ret (the pseudo handle of the current process)
		{"GetCurrentProcess", {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, 0xc3}},
		// mov rax, -2; ret (the pseudo handle of the current thread)
		{"GetCurrentThread", {0x48, 0xc7, 0xc0, 0xfe, 0xff, 0xff, 0xff, 0xc3}},
		// cmp ecx,64; jae slow; mov ecx,ecx; mov rax,gs:[rcx*8+0x1480]; mov dword gs:[0x68],0; ret
		{"TlsGetValue", {0x83, 0xf9, 0x40, 0x73, 0x2b, 0x8b, 0xc9, 0x65, 0x48, 0x8b, 0x04, 0xcd, 0x80, 0x14, 0, 0,
			0x65, 0xc7, 0x04, 0x25, 0x68, 0, 0, 0, 0, 0, 0, 0, 0xc3}},
		// cmp ecx,64; jae slow; mov ecx,ecx; mov gs:[rcx*8+0x1480],rdx; mov eax,1; ret
		{"TlsSetValue", {0x83, 0xf9, 0x40, 0x73, 0x2b, 0x8b, 0xc9, 0x65, 0x48, 0x89, 0x14, 0xcd, 0x80, 0x14, 0, 0,
			0xb8, 1, 0, 0, 0, 0xc3}},
		// mov rax,gs:[0x17c8]; test rax,rax; jz slow; cmp ecx,4080; jae slow; mov ecx,ecx; mov rax,[rax+rcx*8]; mov dword gs:[0x68],0; ret
		{"FlsGetValue", {0x65, 0x48, 0x8b, 0x04, 0x25, 0xc8, 0x17, 0, 0, 0x48, 0x85, 0xc0, 0x74, 0x22,
			0x81, 0xf9, 0xf0, 0x0f, 0, 0, 0x73, 0x1a, 0x8b, 0xc9, 0x48, 0x8b, 0x04, 0xc8,
			0x65, 0xc7, 0x04, 0x25, 0x68, 0, 0, 0, 0, 0, 0, 0, 0xc3}},
		// The same without touching the last error, as the second form does.
		{"FlsGetValue2", {0x65, 0x48, 0x8b, 0x04, 0x25, 0xc8, 0x17, 0, 0, 0x48, 0x85, 0xc0, 0x74, 0x22,
			0x81, 0xf9, 0xf0, 0x0f, 0, 0, 0x73, 0x1a, 0x8b, 0xc9, 0x48, 0x8b, 0x04, 0xc8, 0xc3}},
		// A one-time initialization that has completed answers in the guest; any other state is served.
		// mov rax,[rcx]; mov r10,rax; and r10,3; cmp r10,2; jne slow; test r9,r9; jz done; and rax,-4; mov [r9],rax; done: mov eax,1; ret
		{"InitOnceExecuteOnce", {0x48, 0x8b, 0x01, 0x49, 0x89, 0xc2, 0x49, 0x83, 0xe2, 0x03, 0x49, 0x83, 0xfa, 0x02, 0x75, 0x20,
			0x4d, 0x85, 0xc9, 0x74, 0x07, 0x48, 0x83, 0xe0, 0xfc, 0x49, 0x89, 0x01, 0xb8, 1, 0, 0, 0, 0xc3}},
		// Completing a running initialization with its context and no flags stores the word the served form stores; the rest is served.
		// cmp qword [rcx],1; jne slow; test edx,edx; jnz slow; mov rax,r8; and rax,-4; or rax,2; mov [rcx],rax; mov eax,1; ret
		{"InitOnceComplete", {0x48, 0x83, 0x39, 0x01, 0x75, 0x2a, 0x85, 0xd2, 0x75, 0x26, 0x4c, 0x89, 0xc0, 0x48, 0x83, 0xe0, 0xfc,
			0x48, 0x83, 0xc8, 0x02, 0x48, 0x89, 0x01, 0xb8, 1, 0, 0, 0, 0xc3}},
		// UTF-8 and Windows-1252, the pages programs ask about over and over, are valid without a stop; other pages are served.
		// cmp ecx,65001; je yes; cmp ecx,1252; jne slow; yes: mov eax,1; ret
		{"IsValidCodePage", {0x81, 0xf9, 0xe9, 0xfd, 0, 0, 0x74, 0x08, 0x81, 0xf9, 0xe4, 0x04, 0, 0, 0x75, 0x20, 0xb8, 1, 0, 0, 0, 0xc3}},
		// Pointers are not encoded, so encoding and decoding hand the value back.
		// mov rax,rcx; ret
		{"EncodePointer", {0x48, 0x89, 0xc8, 0xc3}},
		{"DecodePointer", {0x48, 0x89, 0xc8, 0xc3}},
		{"EncodeSystemPointer", {0x48, 0x89, 0xc8, 0xc3}},
		{"DecodeSystemPointer", {0x48, 0x89, 0xc8, 0xc3}},
		// Singly linked lists keep the served layout: the head in the first word, the depth in the low 16 bits of the second
		// and a sequence above it that every change advances, so a 16-byte compare and exchange updates both at once.
		// test cl,15; jnz slow; push rbx; mov r9,rcx; mov r8,rdx; mov rax,[r9]; mov rdx,[r9+8]
		// retry: mov [r8],rax; mov rbx,r8; lea rcx,[rdx+0x10001]; lock cmpxchg16b [r9]; jnz retry; pop rbx; ret
		{"InterlockedPushEntrySList", {0xf6, 0xc1, 0x0f, 0x75, 0x2b, 0x53, 0x49, 0x89, 0xc9, 0x49, 0x89, 0xd0, 0x49, 0x8b, 0x01,
			0x49, 0x8b, 0x51, 0x08, 0x49, 0x89, 0x00, 0x4c, 0x89, 0xc3, 0x48, 0x8d, 0x8a, 0x01, 0x00, 0x01, 0x00,
			0xf0, 0x49, 0x0f, 0xc7, 0x09, 0x75, 0xec, 0x5b, 0xc3}},
		// test cl,15; jnz slow; push rbx; mov r9,rcx; mov rax,[r9]; mov rdx,[r9+8]
		// retry: test rax,rax; jz done; mov rbx,[rax]; lea rcx,[rdx+0xffff]; lock cmpxchg16b [r9]; jnz retry; done: pop rbx; ret
		{"InterlockedPopEntrySList", {0xf6, 0xc1, 0x0f, 0x75, 0x2b, 0x53, 0x49, 0x89, 0xc9, 0x49, 0x8b, 0x01, 0x49, 0x8b, 0x51, 0x08,
			0x48, 0x85, 0xc0, 0x74, 0x11, 0x48, 0x8b, 0x18, 0x48, 0x8d, 0x8a, 0xff, 0xff, 0x00, 0x00,
			0xf0, 0x49, 0x0f, 0xc7, 0x09, 0x75, 0xea, 0x5b, 0xc3}},
		// test cl,15; jnz slow; push rbx; mov r9,rcx; mov rax,[r9]; mov rdx,[r9+8]
		// retry: test rax,rax; jz done; xor ebx,ebx; mov rcx,rdx; xor cx,cx; add rcx,0x10000; lock cmpxchg16b [r9]; jnz retry; done: pop rbx; ret
		{"InterlockedFlushSList", {0xf6, 0xc1, 0x0f, 0x75, 0x2b, 0x53, 0x49, 0x89, 0xc9, 0x49, 0x8b, 0x01, 0x49, 0x8b, 0x51, 0x08,
			0x48, 0x85, 0xc0, 0x74, 0x16, 0x31, 0xdb, 0x48, 0x89, 0xd1, 0x66, 0x31, 0xc9, 0x48, 0x81, 0xc1, 0x00, 0x00, 0x01, 0x00,
			0xf0, 0x49, 0x0f, 0xc7, 0x09, 0x75, 0xe5, 0x5b, 0xc3}},
	};
	return stubs;
}

}

// Calls that only read and write guest memory: their stubs leave the arguments in a block the runtime shares with the guest and spin until
// the answer is there, so the guest never stops for them.
static bool served_api(std::string_view module, std::string_view name)
{
	if (module == "kernel32.dll" || module == "kernelbase.dll")
		return name == "QueryPerformanceCounter" || name == "QueryPerformanceFrequency" || name == "QueryUnbiasedInterruptTime" ||
			name == "QueryUnbiasedInterruptTimePrecise" || name == "WakeConditionVariable" ||
			name == "WakeAllConditionVariable" || name == "ReleaseSemaphore" || name == "ReleaseMutex" || name == "CreateSemaphoreExW" ||
			name == "WaitForSingleObject" || name == "WaitForSingleObjectEx" || name == "WaitForMultipleObjects" ||
			name == "WaitForMultipleObjectsEx" || name == "SetEvent" || name == "ResetEvent" || name == "CloseHandle" ||
			name == "WakeByAddressSingle" || name == "WakeByAddressAll";
	// Reading a socket's recorded events polls the host sockets and never blocks.
	if (module == "ws2_32.dll")
		return name == "WSAEnumNetworkEvents";
	// Waking marks the sleepers; they run when the scheduler next picks a thread.
	if (module == "ntdll.dll")
		return name == "RtlWakeConditionVariable" || name == "RtlWakeAllConditionVariable" || name == "RtlWakeAddressSingle" ||
			name == "RtlWakeAddressAll";
#ifdef RAVEN_OPENGL
	if (module == "opengl32.dll")
		return OpenGLBridge::served_api(name);
#endif
	if (module != "vulkan-1.dll")
		return false;
	static constexpr std::array<std::string_view, 23> recording{
		"vkAcquireNextImageKHR", "vkAllocateCommandBuffers", "vkAllocateDescriptorSets", "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkFreeCommandBuffers",
		"vkFreeDescriptorSets", "vkGetDescriptorEXT", "vkGetFenceStatus", "vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR", "vkQueueSubmit",
		"vkQueueSubmit2", "vkQueueSubmit2KHR", "vkResetCommandBuffer", "vkResetCommandPool", "vkResetDescriptorPool", "vkResetFences",
		"vkSignalSemaphore", "vkSignalSemaphoreKHR", "vkUpdateDescriptorSetWithTemplate", "vkUpdateDescriptorSetWithTemplateKHR",
		"vkUpdateDescriptorSets"};
	return name.starts_with("vkCmd") || std::find(recording.begin(), recording.end(), name) != recording.end();
}

// Calls of subsystems that keep their own lock: with a host thread for each guest thread they are answered beside the turn, by the
// handler the usual way runs, unless they need the turn after all (see ApiCall::alone). The registry, the transfers of plain files
// under each file's lock (see read_beside), VirtualQuery, VirtualAlloc, VirtualFree and VirtualProtect under the memory accounting's
// lock (see NativeMemory::serve_alone), and the events made and the handles closed under the handle table's lock (see
// NativeObjects::close_alone).
// Window data and properties a thread changes beside the turn under the desktop lock (see DesktopLock): changes that send no message and
// start no drawing. SetWindowLongPtr is answered beside the turn for the user data, the identifier and the extra bytes; the indexes that
// notify the window go the usual way.
static bool desktop_beside_api(std::string_view name)
{
	static constexpr std::array<std::string_view, 14> names{"SetPropW", "SetPropA", "RemovePropW", "RemovePropA", "SetWindowLongPtrW",
		"SetWindowLongPtrA", "SetWindowLongW", "SetWindowLongA", "InvalidateRect", "ValidateRect", "GetDC", "GetWindowDC", "ReleaseDC",
		"PeekMessageW"};
	return std::find(names.begin(), names.end(), name) != names.end();
}

// Drawing into a context and reading its pixels, under the desktop lock like the window data above: the handlers of the desktop's registry,
// which touch only the contexts and the objects selected into them, and mark the window behind a window's context as drawn. Guest memory a
// handler reads or writes it reaches directly, as any call beside the turn (see ApiCall::alone).
static bool gdi_beside_api(std::string_view name)
{
	static constexpr std::array<std::string_view, 24> names{"PatBlt", "BitBlt", "StretchBlt", "FillRect", "FrameRect", "GetPixel", "Rectangle",
		"Ellipse", "RoundRect", "Polygon", "Polyline", "PolylineTo", "PolyPolyline", "LineTo", "FillRgn", "FrameRgn", "GradientFill", "GdiAlphaBlend",
		"AlphaBlend", "StretchDIBits", "SetDIBits", "GetDIBits", "SaveDC", "RestoreDC"};
	return std::find(names.begin(), names.end(), name) != names.end();
}

// The objects a drawing call beside the turn works on when it may run under the shared desktop lock (see DesktopLock): the memory
// contexts it names and the bitmaps selected into them, and the bitmap GetDIBits and SetDIBits name. None when it may not: a call that
// moves the current position or saves and restores a context's state changes what other calls read, and a window's context, a foreign
// one or the screen's reaches the window's surface and its damage, which the desktop keeps whole. An unknown handle goes the usual way
// as well, which answers it.
static std::optional<std::vector<uint64_t>> drawing_objects(std::string_view name, std::span<const uint64_t> words, const DesktopObjects &objects,
	uint64_t screen)
{
	static constexpr std::array<std::string_view, 14> one{"PatBlt", "FillRect", "FrameRect", "GetPixel", "Rectangle", "Ellipse", "RoundRect",
		"Polygon", "Polyline", "PolyPolyline", "FillRgn", "FrameRgn", "GradientFill", "StretchDIBits"};
	static constexpr std::array<std::string_view, 4> two{"BitBlt", "StretchBlt", "GdiAlphaBlend", "AlphaBlend"};
	static constexpr std::array<std::string_view, 2> bitmaps{"SetDIBits", "GetDIBits"};
	const auto listed = [name](const auto &names) { return std::find(names.begin(), names.end(), name) != names.end(); };
	const bool by_bitmap = listed(bitmaps);
	const size_t contexts = listed(one) || by_bitmap ? 1 : listed(two) ? 2 : 0;
	if (!contexts || words.size() < (contexts == 2 ? 6 : by_bitmap ? 2 : 1))
		return std::nullopt;
	std::vector<uint64_t> handles;
	for (const size_t index : {size_t(0), size_t(5)}) {
		if (index && contexts < 2)
			break;
		// A transfer without a source names none.
		if (index && !words[index])
			continue;
		const auto *dc = objects.dc(words[index]);
		if (!dc || dc->window || dc->foreign || dc->leased || words[index] == screen)
			return std::nullopt;
		handles.push_back(words[index]);
		handles.push_back(dc->bitmap);
	}
	if (by_bitmap)
		handles.push_back(words[1]);
	return handles;
}

// A call on one window of the process's that may run under the shared desktop lock, with the stripes of the window and of the context
// it takes or releases: its user data and extra bytes, and its properties, which nothing else reads from the window's record; its
// update region, whose owner the call tells; a context of its own surface, taken and released. None for any other index, which may
// send the window messages or change what other windows show, and for a window the process does not have.
struct WindowCall {
	uint64_t window = 0;
	// The context ReleaseDC gives back; GetDC's is its result.
	uint64_t context = 0;
	bool takes_context = false;
};

static std::optional<WindowCall> window_call(std::string_view name, std::span<const uint64_t> words, const DesktopObjects &objects)
{
	static constexpr std::array<std::string_view, 4> longs{"SetWindowLongPtrW", "SetWindowLongPtrA", "SetWindowLongW", "SetWindowLongA"};
	static constexpr std::array<std::string_view, 6> own{"SetPropW", "SetPropA", "RemovePropW", "RemovePropA", "InvalidateRect", "ValidateRect"};
	const auto listed = [name](const auto &names) { return std::find(names.begin(), names.end(), name) != names.end(); };
	if (words.empty() || !words[0] || !objects.window(words[0]))
		return std::nullopt;
	if (listed(own))
		return WindowCall{words[0]};
	if (name == "GetDC" || name == "GetWindowDC")
		return WindowCall{words[0], 0, true};
	if (name == "ReleaseDC" && words.size() >= 2)
		return WindowCall{words[0], words[1]};
	if (const int32_t index = words.size() >= 2 ? int32_t(uint32_t(words[1])) : 0; listed(longs) && (index == -21 || index >= 0))
		return WindowCall{words[0]};
	return std::nullopt;
}

static bool beside_api(std::string_view module, std::string_view name)
{
	// The state of an event is under the object's own lock.
	if (module == "ntdll.dll" && name == "NtQueryEvent")
		return true;
	if (module == "user32.dll")
		return desktop_beside_api(name) || name == "FillRect" || name == "FrameRect";
	if (module == "gdi32.dll" || module == "msimg32.dll")
		return gdi_beside_api(name);
	// Recording into a command buffer, which only reads the Vulkan bridge's tables.
	if (module == "vulkan-1.dll")
		return name.starts_with("vkCmd") || name == "vkBeginCommandBuffer" || name == "vkEndCommandBuffer" || name == "vkResetCommandBuffer";
	static constexpr std::array<std::string_view, 12> files{"ReadFile", "WriteFile", "SetFilePointer", "SetFilePointerEx", "GetFileSize",
		"GetFileSizeEx", "VirtualQuery", "VirtualAlloc", "VirtualFree", "VirtualProtect", "CreateEventW", "CloseHandle"};
	if ((module == "kernel32.dll" || module == "kernelbase.dll") && std::find(files.begin(), files.end(), name) != files.end())
		return true;
	if (module != "advapi32.dll" && module != "kernelbase.dll")
		return false;
	static constexpr std::array<std::string_view, 10> registry{"RegOpenKeyEx", "RegCreateKeyEx", "RegQueryValueEx", "RegSetValueEx", "RegCloseKey",
		"RegEnumKeyEx", "RegEnumValue", "RegQueryInfoKey", "RegGetValue", "RegDeleteValue"};
	for (const auto call : registry)
		if (name.starts_with(call) && (name.size() == call.size() || name.substr(call.size()) == "W" || name.substr(call.size()) == "A"))
			return true;
	return false;
}

// With a host thread for each guest thread, every other call of a 64-bit provider has a stub of the same kind: the thread's host
// thread answers it in the thread's block, and the guest stops only when the host thread was not looking or the call needs it stopped.
// RAVEN_TRAPPED_CALLS=1 keeps those calls on their breakpoints, for comparing the two.
static bool call_stubs()
{
	static const bool enabled = std::getenv("RAVEN_PARALLEL") && threaded_host() && !std::getenv("RAVEN_TRAPPED_CALLS");
	return enabled;
}

// The calls that raise an exception or go on from another context: the thread's registers are what they act on, so they stop the guest
// on their breakpoint rather than wait in their stub for the host thread.
static bool changes_context(std::string_view name)
{
	static constexpr std::array<std::string_view, 10> calls{"RaiseException", "RtlRaiseException", "NtRaiseException", "RtlRaiseStatus",
		"RtlUnwind", "RtlUnwindEx", "RtlRestoreContext", "NtContinue", "RcConsolidateFrames", "DebugBreak"};
	return std::find(calls.begin(), calls.end(), name) != calls.end();
}

// Whether a served stub's code stands at `rva`, as generated or with its block taken from the thread (see run_native_process).
static bool stub_at(std::span<const uint8_t> image, uint32_t rva)
{
	const auto code = served_stub::code(), local = served_stub::local_block();
	const size_t trap = served_stub::trap(), store = served_stub::store();
	if (size_t(rva) + trap + 2 > image.size() || image[rva + trap] != 0xcc || image[rva + trap + 1] != 0xc3)
		return false;
	const auto at = image.begin() + rva;
	return (std::equal(code.begin(), code.begin() + 3, at) || std::equal(local.begin(), local.end() - 3, at)) &&
		std::equal(code.begin() + store, code.begin() + store + 4, at + store);
}

// A served stub (gates.S): the provider's own pointer to the block sits `to_pointer` bytes from the stub. Each stub has a room of
// served_stub_size bytes; the offsets within it are the ones served_stub.h names.
static constexpr size_t served_stub_size = served_stub::size;
static const size_t served_trap = served_stub::trap(), served_wait = served_stub::wait(), served_yield = served_stub::yield(),
	served_tail = served_stub::tail();

static std::vector<uint8_t> served_stub_code(int32_t to_pointer, bool asked = false)
{
	const auto stub = served_stub::code();
	std::vector<uint8_t> code(stub.begin(), stub.end());
	const int32_t displacement = to_pointer - 7;
	std::memcpy(code.data() + 3, &displacement, 4);
	if (asked)
		std::copy(served_stub::asked_tail().begin(), served_stub::asked_tail().end(), code.begin() + served_tail);
	return code;
}

// The calls a renderer makes by the thousand that the guest puts in its thread's ring instead of stopping (see the queued preludes of
// preludes.S): how many arguments each records, and its prelude, with where the field for its served stub's address ends.
struct QueuedCall {
	std::string_view name;
	unsigned arguments;
	const uint8_t *code, *stub, *end;
};

static const QueuedCall *queued_call(std::string_view module, std::string_view name)
{
#ifdef RAVEN_OPENGL
	// Only context-local state: shared texture and program changes still flush every producer synchronously.
	static const QueuedCall opengl[]{
		{"glColor4f", 4, raven_prelude_queued_gl4, raven_prelude_queued_gl4_stub, raven_prelude_queued_gl4_end},
		{"glColor4ub", 4, raven_prelude_queued_gl4, raven_prelude_queued_gl4_stub, raven_prelude_queued_gl4_end},
		{"glRotatef", 4, raven_prelude_queued_gl4, raven_prelude_queued_gl4_stub, raven_prelude_queued_gl4_end},
		{"glDrawElements", 4, raven_prelude_queued_gl_draw_elements, raven_prelude_queued_gl_draw_elements_stub, raven_prelude_queued_gl_draw_elements_end},
		{"glVertex3f", 3, raven_prelude_queued_gl3, raven_prelude_queued_gl3_stub, raven_prelude_queued_gl3_end},
		{"glTranslatef", 3, raven_prelude_queued_gl3, raven_prelude_queued_gl3_stub, raven_prelude_queued_gl3_end},
		{"glScalef", 3, raven_prelude_queued_gl3, raven_prelude_queued_gl3_stub, raven_prelude_queued_gl3_end},
		{"glTexCoord2f", 2, raven_prelude_queued_gl2, raven_prelude_queued_gl2_stub, raven_prelude_queued_gl2_end},
		{"glBindBuffer", 2, raven_prelude_queued_gl_bind_buffer, raven_prelude_queued_gl_bind_buffer_stub, raven_prelude_queued_gl_bind_buffer_end},
		{"glActiveTexture", 1, raven_prelude_queued_gl1, raven_prelude_queued_gl1_stub, raven_prelude_queued_gl1_end},
		{"glUseProgram", 1, raven_prelude_queued_gl1, raven_prelude_queued_gl1_stub, raven_prelude_queued_gl1_end},
		{"glEnable", 1, raven_prelude_queued_gl1, raven_prelude_queued_gl1_stub, raven_prelude_queued_gl1_end},
		{"glDisable", 1, raven_prelude_queued_gl1, raven_prelude_queued_gl1_stub, raven_prelude_queued_gl1_end},
	};
	if (module == "opengl32.dll") {
		if (!std::getenv("RAVEN_QUEUED_OPENGL"))
			return nullptr;
		for (const auto &call : opengl)
			if (call.name == name)
				return &call;
		return nullptr;
	}
#endif
	static const QueuedCall vulkan[]{
		{"vkCmdBindDescriptorSets", 8, raven_prelude_queued_vk_descriptors, raven_prelude_queued_vk_descriptors_stub, raven_prelude_queued_vk_descriptors_end},
		{"vkCmdBindVertexBuffers", 5, raven_prelude_queued_vk_vertices, raven_prelude_queued_vk_vertices_stub, raven_prelude_queued_vk_vertices_end},
		{"vkCmdDrawIndexed", 6, raven_prelude_queued_vk6, raven_prelude_queued_vk6_stub, raven_prelude_queued_vk6_end},
		{"vkCmdDraw", 5, raven_prelude_queued_vk5, raven_prelude_queued_vk5_stub, raven_prelude_queued_vk5_end},
		{"vkCmdDrawIndirect", 5, raven_prelude_queued_vk5, raven_prelude_queued_vk5_stub, raven_prelude_queued_vk5_end},
		{"vkCmdDrawIndexedIndirect", 5, raven_prelude_queued_vk5, raven_prelude_queued_vk5_stub, raven_prelude_queued_vk5_end},
		{"vkCmdBindIndexBuffer", 4, raven_prelude_queued_vk4, raven_prelude_queued_vk4_stub, raven_prelude_queued_vk4_end},
		{"vkCmdDispatch", 4, raven_prelude_queued_vk4, raven_prelude_queued_vk4_stub, raven_prelude_queued_vk4_end},
		{"vkCmdBindPipeline", 3, raven_prelude_queued_vk3, raven_prelude_queued_vk3_stub, raven_prelude_queued_vk3_end},
		{"vkCmdSetStencilReference", 3, raven_prelude_queued_vk3, raven_prelude_queued_vk3_stub, raven_prelude_queued_vk3_end},
	};
	if (module != "vulkan-1.dll")
		return nullptr;
	for (const auto &call : vulkan)
		if (call.name == name)
			return &call;
	return nullptr;
}

static unsigned queued_arguments(std::string_view module, std::string_view name)
{
	const auto *call = queued_call(module, name);
	return call ? call->arguments : 0;
}

static constexpr size_t queued_prelude_size = 384;

static std::vector<uint8_t> queued_prelude(int32_t to_served, const QueuedCall &call)
{
	static_assert(ServedQueue::head * 8 == 0x80 && ServedQueue::tail * 8 == 0x88 && ServedQueue::enabled * 8 == 0x90 &&
		ServedQueue::records * 8 == 0x100 && ServedQueue::gl_elements * 8 == 0x98 && ServedQueue::words == 16 && ServedQueue::capacity == 16,
		"the queued preludes' offsets are written into their code");
	auto code = prelude_code(call.code, call.end);
	if (code.size() != queued_prelude_size)
		throw std::logic_error("a queued prelude does not fill its slot");
	// The address of the served stub, which the record names, and the jump to it a full or disabled ring takes.
	const auto relative = [&](size_t end) {
		const int32_t displacement = to_served - int32_t(end);
		std::memcpy(code.data() + end - 4, &displacement, 4);
	};
	relative(size_t(call.stub - call.code));
	relative(queued_prelude_size);
	return code;
}

// The performance counter is read in the guest once the runtime has published how the processor's time stamp counter relates to it: the
// stub exported for it is a prelude that computes the count from RDTSC and falls back to its served stub until then (see ClockCalibration).
static bool guest_clock_api(std::string_view module, std::string_view name)
{
	// The unbiased interrupt time counts the same monotonic clock in the same 100 ns units as the performance counter.
	return (module == "kernel32.dll" || module == "kernelbase.dll") &&
		(name == "QueryPerformanceCounter" || name == "QueryUnbiasedInterruptTime" || name == "QueryUnbiasedInterruptTimePrecise");
}

static constexpr size_t clock_prelude_size = 96;

static std::vector<uint8_t> clock_prelude(int32_t to_pointer, int32_t to_served)
{
	auto code = prelude_code(raven_prelude_clock, raven_prelude_clock_end);
	const int32_t displacement = to_pointer - 7;
	std::memcpy(code.data() + 3, &displacement, 4);
	for (const uint8_t *end : {raven_prelude_clock_served, raven_prelude_clock_served_again}) {
		const auto at = size_t(end - raven_prelude_clock);
		const int32_t relative = to_served - int32_t(at);
		std::memcpy(code.data() + at - 4, &relative, 4);
	}
	return code;
}

// Committing and decommitting pages that are already open in the guest is done by the guest itself: the runtime publishes the open
// ranges in the served block, and the preludes of VirtualAlloc and VirtualFree log a read-write commit, or a small decommit, that falls in
// one of them there instead of stopping; the runtime takes the log into its accounting before its next memory operation. A decommit
// zeroes its pages in place and leaves them open, as the runtime does for pages inside a range. Anything else jumps to the export's
// breakpoint.
enum class GuestMemoryApi { none, commit, decommit };

static GuestMemoryApi guest_memory_api(std::string_view module, std::string_view name)
{
	if (module != "kernel32.dll" && module != "kernelbase.dll")
		return GuestMemoryApi::none;
	return name == "VirtualAlloc" ? GuestMemoryApi::commit : name == "VirtualFree" ? GuestMemoryApi::decommit : GuestMemoryApi::none;
}

static bool guest_commit_api(std::string_view module, std::string_view name)
{
	return guest_memory_api(module, name) != GuestMemoryApi::none;
}

static constexpr size_t commit_prelude_size = 192;

// Where the jump to the breakpoint begins in each prelude (see preludes.S).
static size_t commit_jump(GuestMemoryApi kind)
{
	return kind == GuestMemoryApi::commit ? size_t(raven_prelude_commit_trap - raven_prelude_commit) - 5 :
		size_t(raven_prelude_decommit_trap - raven_prelude_decommit) - 5;
}

static std::vector<uint8_t> decommit_prelude(int32_t to_pointer, int32_t to_trap)
{
	auto code = prelude_code(raven_prelude_decommit, raven_prelude_decommit_end);
	const int32_t displacement = to_pointer - 7;
	std::memcpy(code.data() + 3, &displacement, 4);
	const int32_t relative = to_trap - int32_t(commit_jump(GuestMemoryApi::decommit) + 5);
	std::memcpy(code.data() + commit_jump(GuestMemoryApi::decommit) + 1, &relative, 4);
	return code;
}

// String services that the guest runs itself in their common case: lengths and bounded copies, and conversions of plain ASCII text
// between the ANSI or UTF-8 code pages and UTF-16, which every such code page maps alike. A program that starts with thousands of them
// would otherwise stop for each. Each ends in a jump to the export's breakpoint, taken for null pointers, other code pages or flags,
// text beyond ASCII, and buffers too small, where the runtime answers.
struct GuestPrelude {
	std::string_view name;
	std::vector<uint8_t> code;
};

static const std::vector<GuestPrelude> &string_preludes()
{
	// lstrlen: count to the terminator, 0 for a null pointer. lstrcpyn: copy at most n - 1 characters and terminate, returning the
	// destination. MultiByteToWideChar and WideCharToMultiByte: code page 0, 1252 or UTF-8, only flags that leave plain ASCII as it is
	// (MB_PRECOMPOSED and MB_ERR_INVALID_CHARS, none for UTF-8 beyond the second), the length counted when it is -1, every character
	// below 0x80, and the result written when there is room or its length returned when the buffer is absent or empty; the
	// used-default flag is cleared when asked for. lstrcmpW: two ASCII strings ordered as the runtime orders them, by their upper case
	// letters and then, on a tie, with the lower case letter first. CompareStringEx and CompareStringW: the same order for two ASCII
	// strings, counted or terminated, the case ignored for the flags the runtime ignores it for (the CLR compares names this way by the
	// thousand while Paint.NET draws). GetThreadLocale and SetThreadLocale: the runtime's constant answers, US English and success.
	static const std::vector<uint8_t> compare = prelude_code(raven_prelude_string_compare, raven_prelude_string_compare_end);
	static const std::vector<GuestPrelude> preludes = {
		{"lstrlenW", prelude_code(raven_prelude_string_lstrlen_w, raven_prelude_string_lstrlen_w_end)},
		{"lstrlenA", prelude_code(raven_prelude_string_lstrlen_a, raven_prelude_string_lstrlen_a_end)},
		{"lstrcpynW", prelude_code(raven_prelude_string_lstrcpyn_w, raven_prelude_string_lstrcpyn_w_end)},
		{"lstrcpynA", prelude_code(raven_prelude_string_lstrcpyn_a, raven_prelude_string_lstrcpyn_a_end)},
		{"lstrcmpW", prelude_code(raven_prelude_string_lstrcmp_w, raven_prelude_string_lstrcmp_w_end)},
		{"MultiByteToWideChar", prelude_code(raven_prelude_string_multi_byte_to_wide_char, raven_prelude_string_multi_byte_to_wide_char_end)},
		{"WideCharToMultiByte", prelude_code(raven_prelude_string_wide_char_to_multi_byte, raven_prelude_string_wide_char_to_multi_byte_end)},
		{"CompareStringEx", compare},
		{"CompareStringW", compare},
		{"GetThreadLocale", prelude_code(raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end)},
		// The ANSI code page is Windows-1252 and every default language and locale US English, as the served forms answer.
		{"GetACP", prelude_code(raven_prelude_string_get_a_c_p, raven_prelude_string_get_a_c_p_end)},
		{"GetUserDefaultLCID", prelude_code(raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end)},
		{"GetSystemDefaultLCID", prelude_code(raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end)},
		{"GetUserDefaultLangID", prelude_code(raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end)},
		{"GetSystemDefaultLangID", prelude_code(raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end)},
		{"GetUserDefaultUILanguage", prelude_code(raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end)},
		{"GetSystemDefaultUILanguage", prelude_code(raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end)},
		{"GetThreadUILanguage", prelude_code(raven_prelude_string_get_thread_locale, raven_prelude_string_get_thread_locale_end)},
		{"SetThreadLocale", prelude_code(raven_prelude_string_set_thread_locale, raven_prelude_string_set_thread_locale_end)},
	};
	return preludes;
}

static const GuestPrelude *string_prelude(std::string_view module, std::string_view name)
{
	if (module != "kernel32.dll" && module != "kernelbase.dll")
		return nullptr;
	for (const auto &prelude : string_preludes())
		if (prelude.name == name)
			return &prelude;
	return nullptr;
}

// Services the guest answers from what the runtime publishes in the served block or the shared user data page. The millisecond clocks
// read the page's tick count, as Windows does. Each starts as the others do, with the block pointer's place, and ends in a jump to the
// export's breakpoint.
static const std::vector<uint8_t> &tick_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_tick, raven_prelude_tick_end);
	return code;
}

// The screen's device context for GetDC(NULL), which programs take to read the display resolution and give back at once: the runtime
// publishes one context, its capabilities as a table indexed by GetDeviceCaps' index and a state word (0 free, 1 taken by a GetDC the
// guest answered, 2 changed by a call the runtime served); GetDC takes it free with lock cmpxchg, ReleaseDC frees it unless changed, and
// GetDeviceCaps reads the table. A changed context goes back to the runtime, which releases it and publishes another.
static constexpr size_t screen_caps = 0x400, screen_dc = 0x600, screen_dc_state = 0x608;

static const std::vector<uint8_t> &screen_get_dc_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_screen_get_dc, raven_prelude_screen_get_dc_end);
	return code;
}

static const std::vector<uint8_t> &screen_release_dc_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_screen_release_dc, raven_prelude_screen_release_dc_end);
	return code;
}

static const std::vector<uint8_t> &screen_device_caps_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_screen_device_caps, raven_prelude_screen_device_caps_end);
	return code;
}

// Window state the guest reads from the window table the runtime keeps (see NativeWindows::publish_windows), found through the served
// block: the record at the handle's low nine bits, when it is that window's. GetWindowLong and GetWindowLongPtr answer the extra bytes
// the record holds and the indexes the runtime answers, GetPropW the properties named by an atom when the record holds all of them,
// IsWindow and IsWindowUnicode a window of this process, GetClientRect and GetWindowRect its rectangles. A window without a record,
// another index or a null pointer goes to the runtime.
static constexpr size_t window_table = 0x700;

static const std::vector<uint8_t> &window_long_ptr_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_long_ptr, raven_prelude_window_long_ptr_end);
	return code;
}

static const std::vector<uint8_t> &window_long_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_long, raven_prelude_window_long_end);
	return code;
}

static const std::vector<uint8_t> &window_property_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_property, raven_prelude_window_property_end);
	return code;
}

static const std::vector<uint8_t> &window_exists_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_exists, raven_prelude_window_exists_end);
	return code;
}

// GetWindowThreadProcessId, from the thread and the process the record of the window names; a window without a record, or one whose
// thread the runtime has to say, goes to the runtime.
static const std::vector<uint8_t> &window_thread_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_thread, raven_prelude_window_thread_end);
	return code;
}

static const std::vector<uint8_t> &window_client_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_client, raven_prelude_window_client_end);
	return code;
}

static const std::vector<uint8_t> &window_outer_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_outer, raven_prelude_window_outer_end);
	return code;
}

// Locale information, which the runtime answers by type alone: the guest reads it from the table the runtime publishes once (see
// locale_table), through the served block. A type with flags beyond LOCALE_RETURN_NUMBER and LOCALE_NOUSEROVERRIDE, one without
// text, a buffer too small or a null buffer goes to the runtime.
static constexpr size_t locale_pointer = 0x708;

static const std::vector<uint8_t> &locale_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_locale, raven_prelude_locale_end);
	return code;
}

// NtAllocateVirtualMemory committing read-write pages in an open range, as the commit prelude of VirtualAlloc does for its own
// arguments: the current process, no zero bits, the base and size behind their pointers, MEM_COMMIT and PAGE_READWRITE. The rounded
// base and size are written back and STATUS_SUCCESS returned; anything else goes to the runtime with the arguments as they came.
static const std::vector<uint8_t> &nt_commit_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_nt_commit, raven_prelude_nt_commit_end);
	static_assert(NativeMemory::commit_windows == 0xa00 && NativeMemory::commit_log == 0xd10 && NativeMemory::commit_log_count == 32,
		"the commit preludes' offsets are written into their code");
	return code;
}

// NtFreeVirtualMemory decommitting at most 256 KiB of an open range, as the decommit prelude of VirtualFree does: the pages are
// zeroed in place and logged with the low bit set, and the rounded base and size are written back.
static const std::vector<uint8_t> &nt_decommit_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_nt_decommit, raven_prelude_nt_decommit_end);
	return code;
}

// IsChild walks the parents the window table holds, GetFocus reads the focus window the runtime publishes with it, and GetDpiForWindow
// answers 96 as the runtime does.
static constexpr size_t focus_published = 0x710, focus_window = 0x718;

static const std::vector<uint8_t> &window_child_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_child, raven_prelude_window_child_end);
	return code;
}

static const std::vector<uint8_t> &window_dpi_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_dpi, raven_prelude_window_dpi_end);
	return code;
}

static const std::vector<uint8_t> &focus_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_focus, raven_prelude_focus_end);
	return code;
}

// GetForegroundWindow and GetCursorPos read what the runtime publishes beside the focus as every call returns: the pointer moves for
// the program only as it reads its messages, which is a call. A null point goes to the runtime.
static constexpr size_t foreground_window = 0x798, cursor_position = 0x7a0;

static const std::vector<uint8_t> &foreground_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_foreground, raven_prelude_foreground_end);
	static_assert(foreground_window == 0x798, "the foreground prelude's offset is written into its code");
	return code;
}

// DefWindowProcW of a message it does nothing with but answer zero, for a window the table holds: a registered message, the pointer
// moving and the plain button messages. Everything else goes to the runtime.
static const std::vector<uint8_t> &default_procedure_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_default_procedure, raven_prelude_default_procedure_end);
	return code;
}

static const std::vector<uint8_t> &cursor_position_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_cursor_position, raven_prelude_cursor_position_end);
	static_assert(cursor_position == 0x7a0, "the pointer position prelude's offset is written into its code");
	return code;
}

// Menus by position, read from the menu table the runtime keeps (see NativeWindows::publish_menus): GetMenuItemCount, GetSubMenu,
// GetMenuItemID and GetMenuStringW with MF_BYPOSITION. A menu without a slot goes to the runtime.
static constexpr size_t menu_pointer = 0x720;

static const std::vector<uint8_t> &menu_count_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_menu_count, raven_prelude_menu_count_end);
	return code;
}

static const std::vector<uint8_t> &menu_submenu_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_menu_submenu, raven_prelude_menu_submenu_end);
	return code;
}

static const std::vector<uint8_t> &menu_id_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_menu_id, raven_prelude_menu_id_end);
	return code;
}

// GetMenuState, and GetMenuStringW by command as well as by position: an item looked up by command is searched for in the order the
// runtime searches, a level's own items first and then each submenu in turn, on the guest's stack; a menu missing from the table or
// a tree deeper than the stack allows goes to the runtime.
static const std::vector<uint8_t> &menu_state_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_menu_state, raven_prelude_menu_state_end);
	return code;
}

static const std::vector<uint8_t> &menu_string_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_menu_string, raven_prelude_menu_string_end);
	return code;
}

// GetObject on a bitmap, read from the bitmap table the runtime keeps (see NativeWindows::publish_bitmaps): the size with no buffer,
// nothing for a buffer under 32 bytes, else the BITMAP. A DIB section asked for its DIBSECTION, or another object, goes to the runtime.
static constexpr size_t bitmap_pointer = 0x728;

static const std::vector<uint8_t> &bitmap_object_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_bitmap_object, raven_prelude_bitmap_object_end);
	return code;
}

// Services with nothing behind them but their arguments, answered by the guest as the runtime answers them: MulDiv rounding half away
// from zero and failing with -1 for a zero divisor or a result beyond 32 bits, OffsetRect, SetRect and IntersectRect (an empty
// intersection clears the destination and returns FALSE), PathFindFileNameW after the last separator or drive colon, and GetWindowTheme
// without themes. A null pointer goes to the runtime.
static const std::vector<uint8_t> &muldiv_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_muldiv, raven_prelude_muldiv_end);
	return code;
}

static const std::vector<uint8_t> &offset_rect_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_offset_rect, raven_prelude_offset_rect_end);
	return code;
}

static const std::vector<uint8_t> &set_rect_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_set_rect, raven_prelude_set_rect_end);
	return code;
}

static const std::vector<uint8_t> &intersect_rect_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_intersect_rect, raven_prelude_intersect_rect_end);
	return code;
}

static const std::vector<uint8_t> &window_theme_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_theme, raven_prelude_window_theme_end);
	return code;
}

// Whether visual styles are off is settled as the process starts, from the environment or the prefix registry, and holds for its life.
static bool classic_theme()
{
	const char *chosen = std::getenv("RAVEN_THEME");
	return chosen && std::string_view(chosen) == "classic";
}

// Whether visual styles are on, published once the prefix registry has had its say as the process starts: 1 without them, 2 with
// them, nothing before. IsThemeActive, IsAppThemed and IsCompositionActive answer it; GetThemeAppProperties gives every style flag
// when they are on, none otherwise.
static constexpr size_t theme_state = 0x790;

static const std::vector<uint8_t> &theme_state_prelude(bool properties)
{
	static const std::vector<uint8_t> state = prelude_code(raven_prelude_theme_state, raven_prelude_theme_state_end);
	static const std::vector<uint8_t> flags = prelude_code(raven_prelude_theme_flags, raven_prelude_theme_flags_end);
	static_assert(theme_state == 0x790, "the theme state preludes' offset is written into their code");
	return properties ? flags : state;
}

static const std::vector<uint8_t> &file_name_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_file_name, raven_prelude_file_name_end);
	return code;
}

// CallWindowProc calls the procedure it is given with the other four arguments, as the runtime does: the guest moves them down a
// register and jumps to it, so the procedure returns to the caller. A null procedure goes to the runtime.
static const std::vector<uint8_t> &call_procedure_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_call_procedure, raven_prelude_call_procedure_end);
	return code;
}

// SendMessageW to a window of the calling thread calls its procedure with the same four arguments, as the runtime does: the guest jumps
// to it, so the procedure returns to the caller. The record (window_table) must name the window, the calling thread as its own and a
// procedure, all read while the count of changes (NativeWindows::table_version) is even and the same before and after; otherwise, and
// for a window of another thread or process, the runtime sends it. It checks the count itself, as it cannot come back to an answer
// wrapper, so it runs while other threads run as well (self_checked_api):
//   mov r10,[pointer]; test r10,r10; jz slow; mov r11,[r10+0x700]; test r11,r11; jz slow; test rcx,rcx; jz slow
//   push rsi; push rdi; push 128; try: mov rax,[r10+0x7b0]; test al,1; jnz busy; mov esi,ecx; and esi,0x1ff; shl esi,8; add rsi,r11
//   cmp rcx,[rsi]; jne moved; mov edi,gs:[0x48]; test edi,edi; jz fail; cmp edi,[rsi+0xf0]; jne moved; mov rdi,[rsi+0x10]
//   test rdi,rdi; jz moved; cmp rax,[r10+0x7b0]; jne busy; mov r11,rdi; add rsp,8; pop rdi; pop rsi; jmp r11
//   moved: cmp rax,[r10+0x7b0]; je fail; busy: pause; dec dword [rsp]; jnz try; fail: add rsp,8; pop rdi; pop rsi; slow: jmp stub
static const std::vector<uint8_t> &own_thread_send_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_own_thread_send, raven_prelude_own_thread_send_end);
	static_assert(NativeWindows::table_version == 0x7b0 && NativeWindows::window_owner == 0xf0 && NativeWindows::window_records == 512 &&
		NativeWindows::window_record == 256, "the send prelude's offsets are written into its code");
	return code;
}

// The preludes that check the count of changes themselves and so run while other threads run, without an answer wrapper.
static bool self_checked_api(std::string_view module, std::string_view name)
{
	return module == "user32.dll" && name == "SendMessageW";
}

// GetSystemMetrics and GetSysColor, which do not change while the process runs, read from the table the runtime writes once (see
// NativeWindows::set_system_table) for indexes below 128 and 64.
static constexpr size_t system_pointer = 0x730;

static const std::vector<uint8_t> &system_metrics_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_system_metrics, raven_prelude_system_metrics_end);
	return code;
}

static const std::vector<uint8_t> &system_colors_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_system_colors, raven_prelude_system_colors_end);
	return code;
}

// The message loop's helpers for the messages they leave alone, answered as the runtime answers them: TranslateMessage for a message
// that is not a key, TranslateAcceleratorW for one that is not a key or character with a window and an accelerator table the runtime
// knows (it publishes the tables beside the system metrics), CallMsgFilterW without message filter hooks (a flag the runtime
// publishes at 0x738), and GetDesktopWindow once the desktop window exists (its handle at 0x740).
static const std::vector<uint8_t> &translate_message_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_translate_message, raven_prelude_translate_message_end);
	return code;
}

static const std::vector<uint8_t> &message_filter_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_message_filter, raven_prelude_message_filter_end);
	return code;
}

static const std::vector<uint8_t> &desktop_window_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_desktop_window, raven_prelude_desktop_window_end);
	return code;
}

static const std::vector<uint8_t> &accelerator_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_accelerator, raven_prelude_accelerator_end);
	return code;
}

// Device context attributes the guest sets and reads in the attribute table the runtime keeps (see NativeWindows::apply_dc_log): text
// and background colors, background mode and text alignment, each setter logging the context for the runtime. A context without a
// slot, the published screen context, an invalid value or a full log goes to the runtime.
static constexpr size_t dc_pointer = 0x748;

static const std::vector<uint8_t> &dc_text_color_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_text_color, raven_prelude_dc_text_color_end);
	return code;
}

static const std::vector<uint8_t> &dc_background_color_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_background_color, raven_prelude_dc_background_color_end);
	return code;
}

static const std::vector<uint8_t> &dc_background_mode_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_background_mode, raven_prelude_dc_background_mode_end);
	return code;
}

static const std::vector<uint8_t> &dc_text_align_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_text_align, raven_prelude_dc_text_align_end);
	return code;
}

// LPtoDP and DPtoLP, which move points by where the context's logical origin lies on its pixels: the attribute table holds that as
// two 16-bit numbers at the end of the context's slot, or 0x80008000 for an origin they cannot hold. A context without a slot, the
// published screen context, no points, more than 64 of them or such an origin goes to the runtime.
static const std::vector<uint8_t> &dc_to_device_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_to_device, raven_prelude_dc_to_device_end);
	return code;
}

static const std::vector<uint8_t> &dc_to_logical_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_to_logical, raven_prelude_dc_to_logical_end);
	return code;
}

static const std::vector<uint8_t> &dc_get_text_color_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_get_text_color, raven_prelude_dc_get_text_color_end);
	return code;
}

static const std::vector<uint8_t> &dc_get_background_color_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_get_background_color, raven_prelude_dc_get_background_color_end);
	return code;
}

static const std::vector<uint8_t> &dc_get_background_mode_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_get_background_mode, raven_prelude_dc_get_background_mode_end);
	return code;
}

static const std::vector<uint8_t> &dc_get_text_align_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dc_get_text_align, raven_prelude_dc_get_text_align_end);
	return code;
}

// Releases the guest answers itself and logs for the runtime (see NativeWindows::take_deferred): DestroyIcon of a valid icon, DeleteDC of
// a memory context, and DeleteObject of a bitmap the runtime marked as free to release. Anything else, or a full log, goes to the runtime.
static constexpr size_t deferred_pointer = 0x750, icons_pointer = 0x758;

static const std::vector<uint8_t> &destroy_icon_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_destroy_icon, raven_prelude_destroy_icon_end);
	return code;
}

static const std::vector<uint8_t> &delete_dc_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_delete_dc, raven_prelude_delete_dc_end);
	return code;
}

static const std::vector<uint8_t> &delete_object_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_delete_object, raven_prelude_delete_object_end);
	return code;
}

// A one-time initialization answers in the guest when it has completed, and a fresh one is claimed there with a compare and exchange, in
// the encoding the served form keeps (0 untouched, 1 running, the context with low bits 2 when done); a running one, a failed claim and
// unknown flags are served.
static const std::vector<uint8_t> &init_once_begin_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_init_once_begin, raven_prelude_init_once_begin_end);
	return code;
}

// A rectangle is empty when it has no width or no height; a missing one is served.
static const std::vector<uint8_t> &is_rect_empty_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_is_rect_empty, raven_prelude_is_rect_empty_end);
	return code;
}

// SetRectEmpty clears the sixteen bytes; a missing rectangle is served.
static const std::vector<uint8_t> &set_rect_empty_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_set_rect_empty, raven_prelude_set_rect_empty_end);
	return code;
}

// CopyRect moves the sixteen bytes; a missing rectangle is served.
static const std::vector<uint8_t> &copy_rect_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_copy_rect, raven_prelude_copy_rect_end);
	return code;
}

// InflateRect moves each pair of sides apart by the amounts given; a missing rectangle is served.
static const std::vector<uint8_t> &inflate_rect_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_inflate_rect, raven_prelude_inflate_rect_end);
	return code;
}

// UnionRect gives the rectangle that holds both, an empty one counting for nothing: the other alone when one is empty, zeros and
// FALSE when both are. The four sides are worked out before any is written, as the result may be one of the two. A missing rectangle
// is served.
static const std::vector<uint8_t> &union_rect_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_union_rect, raven_prelude_union_rect_end);
	return code;
}

// Two rectangles are equal when their sixteen bytes are; a missing one is served.
static const std::vector<uint8_t> &equal_rect_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_equal_rect, raven_prelude_equal_rect_end);
	return code;
}

// GetLocalTime reads the local second the runtime publishes (see local_second): the counter where it starts and ends and its
// SYSTEMTIME. The counter is read as the clock prelude reads it, and inside the second the guest copies the time and sets the
// milliseconds; outside it, or when the runtime changed the second while it copied, the runtime answers and publishes the next.
static constexpr size_t local_second_at = 0x840;

static const std::vector<uint8_t> &local_time_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_local_time, raven_prelude_local_time_end);
	static_assert(local_second_at == 0x840, "the local time prelude's offsets are written into its code");
	return code;
}

// The one display is the monitor every point, rectangle and window belongs to, handle 1.
static const std::vector<uint8_t> &monitor_prelude()
{
	// lea r11,[rip+block]; mov eax,1; ret; jmp trap
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_monitor, raven_prelude_monitor_end);
	return code;
}

// Every window reports DPI_AWARENESS_CONTEXT_SYSTEM_AWARE (-2), as the served form answers.
static const std::vector<uint8_t> &window_dpi_context_prelude()
{
	// lea r11,[rip+block]; mov rax,-2; ret; jmp trap
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_dpi_context, raven_prelude_window_dpi_context_end);
	return code;
}

// A window is visible when it and every parent above it carry WS_VISIBLE in the shared window table, as the served form follows the
// parents; a handle missing from the table is served.
static const std::vector<uint8_t> &window_visible_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_visible, raven_prelude_window_visible_end);
	return code;
}

// A window in the shared table is minimized when its style carries WS_MINIMIZE; a handle missing from the table is served.
static const std::vector<uint8_t> &window_iconic_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_iconic, raven_prelude_window_iconic_end);
	return code;
}

// The DPI awareness context the threads set and read, kept in the served block at 0x760 (zero until the first change, which reads as
// DPI_AWARENESS_CONTEXT_UNAWARE, -1): SetThreadDpiAwarenessContext swaps in a valid context (-5 to -1) and returns the one before, as the
// served form does; anything else goes to the runtime.
static constexpr size_t dpi_context_at = 0x760;

static const std::vector<uint8_t> &dpi_set_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dpi_set, raven_prelude_dpi_set_end);
	static_assert(dpi_context_at == 0x760, "the DPI context preludes' offset is written into their code");
	return code;
}

static const std::vector<uint8_t> &dpi_get_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_dpi_get, raven_prelude_dpi_get_end);
	static_assert(dpi_context_at == 0x760, "the DPI context preludes' offset is written into their code");
	return code;
}

// Every thread has the US English keyboard layout, and no program has a WinEvent hook: constants the served forms answer.
static const std::vector<uint8_t> &keyboard_layout_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_keyboard_layout, raven_prelude_keyboard_layout_end);
	return code;
}

static const std::vector<uint8_t> &win_event_hook_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_win_event_hook, raven_prelude_win_event_hook_end);
	return code;
}

// QueryDepthSList reads the depth the runtime keeps in the low word of the header's second quadword (see InitializeSListHead).
static const std::vector<uint8_t> &slist_depth_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_slist_depth, raven_prelude_slist_depth_end);
	return code;
}

// PtInRect: a point is inside when it is at or right of and below the top left corner and left of and above the bottom right one.
static const std::vector<uint8_t> &point_in_rect_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_point_in_rect, raven_prelude_point_in_rect_end);
	return code;
}

// StringFromGUID2 writes "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" in upper case hexadecimal and returns 39, or 0 when the buffer holds
// fewer characters; the table after the code pairs each byte of the GUID with the character it starts at.
static const std::vector<uint8_t> &guid_text_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_guid_text, raven_prelude_guid_text_end);
	return code;
}

// RectInRegion, PtInRegion and GetRgnBox read the region table the runtime keeps (see NativeWindows::publish_regions): the
// rectangle ordered as the served form orders it meets one of the region's bands, the point lies in one, and the bounds with the
// complexity. A region the table does not hold, one of more rectangles than a record carries or coordinates the served form refuses
// go to the runtime.
static constexpr size_t region_pointer = 0x768;

static const std::vector<uint8_t> &rect_in_region_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_rect_in_region, raven_prelude_rect_in_region_end);
	static_assert(region_pointer == 0x768 && NativeWindows::region_record == 256 && NativeWindows::region_rectangles == 14,
		"the region preludes' offsets are written into their code");
	return code;
}

static const std::vector<uint8_t> &point_in_region_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_point_in_region, raven_prelude_point_in_region_end);
	return code;
}

static const std::vector<uint8_t> &region_box_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_region_box, raven_prelude_region_box_end);
	return code;
}

// GetInputState, GetQueueStatus and PeekMessage return 0 in the guest while the thread's slot of the quiet table (see
// NativeWindows::mark_quiet) holds its id and a time stamp counter value still ahead, and the kinds of message the slot says the queue
// holds are none the call asks for: keyboard and mouse buttons for GetInputState, those of its flags for GetQueueStatus, any for
// PeekMessage. Otherwise they go to the runtime. Office's idle loop polls them by the thousand at each click.
static constexpr size_t quiet_pointer = 0x770;

static const std::vector<uint8_t> &quiet_peek_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_quiet_peek, raven_prelude_quiet_peek_end);
	static_assert(quiet_pointer == 0x770 && NativeWindows::quiet_slots == 64 && NativeWindows::quiet_slot == 16 &&
		NativeWindows::quiet_slots * NativeWindows::quiet_slot <= 4096,
		"the quiet preludes' offsets are written into their code");
	return code;
}

static const std::vector<uint8_t> &quiet_input_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_quiet_input, raven_prelude_quiet_input_end);
	return code;
}

static const std::vector<uint8_t> &quiet_status_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_quiet_status, raven_prelude_quiet_status_end);
	return code;
}

// ActivateActCtx of the context already in effect on the thread, as the table of ActivationContexts::set_current_table holds it, is
// answered in the guest with a cookie of its own, which DeactivateActCtx takes back there; Office brackets most of its window calls so.
static constexpr size_t current_context_pointer = 0x780;

// GetKeyState, from the key states the runtime publishes whenever the program runs on (see NativeWindows::set_key_table): they
// change only as the program reads its messages, which stops it.
static constexpr size_t key_pointer = 0x788;

static const std::vector<uint8_t> &key_state_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_key_state, raven_prelude_key_state_end);
	static_assert(key_pointer == 0x788, "the key state prelude's offset is written into its code");
	return code;
}

static const std::vector<uint8_t> &context_activate_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_context_activate, raven_prelude_context_activate_end);
	static_assert(current_context_pointer == 0x780 && ActivationContexts::current_slots == 64 && ActivationContexts::current_slot == 16 &&
		ActivationContexts::current_cookie == 0x5276, "the activation preludes' offsets and cookie are written into their code");
	return code;
}

static const std::vector<uint8_t> &context_deactivate_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_context_deactivate, raven_prelude_context_deactivate_end);
	return code;
}

// GetParent and GetAncestor walk the parents and owners the window table holds (see NativeWindows::publish_window_record): a window
// without a record, an owner without one or another ancestor kind goes to the runtime.
static const std::vector<uint8_t> &window_parent_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_parent, raven_prelude_window_parent_end);
	static_assert(window_table == 0x700 && NativeWindows::window_records == 512 && NativeWindows::window_record == 256,
		"the window preludes' offsets are written into their code");
	return code;
}

static const std::vector<uint8_t> &window_ancestor_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_window_ancestor, raven_prelude_window_ancestor_end);
	return code;
}

// MapWindowPoints moves up to 64 points by the difference of the client origins the window table holds, a null window being the screen;
// a window without a record, more points or a null array goes to the runtime.
static const std::vector<uint8_t> &map_points_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_map_points, raven_prelude_map_points_end);
	static_assert(NativeWindows::window_origin == 248 && NativeWindows::window_extra_bytes <= NativeWindows::window_origin - 160,
		"the point mapping prelude's offsets are written into its code");
	return code;
}

// LocaleNameToLCID for the one locale there is, by its name or none, without flags; any other name goes to the runtime.
static const std::vector<uint8_t> &locale_number_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_locale_number, raven_prelude_locale_number_end);
	return code;
}

// GetLocaleInfoW answers every type for the one locale there is, its own name and its parent's included, which GetLocaleInfoEx answers
// for the name it is given: its prelude reads a table of its own (see locale_table).
static constexpr size_t locale_names_pointer = 0x778;

static const std::vector<uint8_t> &locale_names_prelude()
{
	static const std::vector<uint8_t> code = [] {
		auto bytes = locale_prelude();
		const uint8_t load[] = {0x4d, 0x8b, 0x9a, uint8_t(locale_pointer), uint8_t(locale_pointer >> 8), 0x00, 0x00};
		const auto found = std::search(bytes.begin(), bytes.end(), std::begin(load), std::end(load));
		if (found == bytes.end())
			throw std::runtime_error("the locale prelude does not load its table");
		found[3] = uint8_t(locale_names_pointer);
		found[4] = uint8_t(locale_names_pointer >> 8);
		return bytes;
	}();
	return code;
}

// A resource handle is the address of the resource's data entry in its image, as on Windows: LoadResource adds the entry's offset to
// the module, SizeofResource reads its size and LockResource returns the data it is given. A module mapped as a data file (a handle
// with a low bit set), a null module or a null handle goes to the runtime.
static const std::vector<uint8_t> &resource_load_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_resource_load, raven_prelude_resource_load_end);
	return code;
}

static const std::vector<uint8_t> &resource_size_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_resource_size, raven_prelude_resource_size_end);
	return code;
}

static const std::vector<uint8_t> &resource_lock_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_resource_lock, raven_prelude_resource_lock_end);
	return code;
}

// CallNextHookEx returns 0 in the guest when no hook of the same kind for this thread follows the one it names in the chain the runtime
// publishes beside the quiet table (see NativeWindows::publish_hooks); a following hook, a hook not in the chain or a chain too long goes
// to the runtime, which runs it.
static const std::vector<uint8_t> &next_hook_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_next_hook, raven_prelude_next_hook_end);
	static_assert(quiet_pointer == 0x770 && NativeWindows::hook_table_at == 1024 && NativeWindows::hook_entries == 64 &&
		NativeWindows::hook_entry == 16 && NativeWindows::hook_table_at + 16 + NativeWindows::hook_entries * NativeWindows::hook_entry <= 4096,
		"the hook prelude's offsets are written into its code");
	return code;
}

// RavenTrace with no trace asked for: the libraries of the runtime name what they lack through it by the thousand while a program
// draws, and each would stop the program to print nothing.
static const std::vector<uint8_t> &silent_trace_prelude()
{
	static const std::vector<uint8_t> code = prelude_code(raven_prelude_silent_trace, raven_prelude_silent_trace_end);
	return code;
}

static const std::vector<uint8_t> *block_prelude(std::string_view module, std::string_view name)
{
	if (module == "ravenhost.dll" && name == "RavenTrace") {
		static const bool traced = std::getenv("RAVEN_TRACE_DWRITE") != nullptr;
		return traced ? nullptr : &silent_trace_prelude();
	}
	// Without visual styles no window has a theme, and the answer needs no trip to the runtime.
	if (module == "uxtheme.dll" && name == "GetWindowTheme" && classic_theme())
		return &window_theme_prelude();
	if (module == "uxtheme.dll" && (name == "IsThemeActive" || name == "IsAppThemed" || name == "IsCompositionActive" || name == "GetThemeAppProperties"))
		return &theme_state_prelude(name == "GetThemeAppProperties");
	// The others belong to a library and an export name each, in a table made once: a long list of names is otherwise compared
	// with each of the thousands of exports of the system libraries.
	using Prelude = const std::vector<uint8_t> &(*)();
	static const auto table = [] {
		std::unordered_map<std::string_view, std::unordered_map<std::string_view, Prelude>> made;
		const auto add = [&](std::initializer_list<std::string_view> libraries, std::initializer_list<std::string_view> names, Prelude prelude) {
			for (const auto library : libraries)
				for (const auto export_name : names)
					made[library].emplace(export_name, prelude);
		};
		add({"user32.dll"}, {"PeekMessageA", "PeekMessageW"}, quiet_peek_prelude);
		add({"user32.dll"}, {"GetInputState"}, quiet_input_prelude);
		add({"user32.dll"}, {"GetQueueStatus"}, quiet_status_prelude);
		add({"user32.dll"}, {"CallNextHookEx"}, next_hook_prelude);
		add({"user32.dll"}, {"MapWindowPoints"}, map_points_prelude);
		add({"user32.dll"}, {"GetParent"}, window_parent_prelude);
		add({"user32.dll"}, {"GetAncestor"}, window_ancestor_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"LocaleNameToLCID"}, locale_number_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"GetLocaleInfoW"}, locale_names_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"ActivateActCtx"}, context_activate_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"DeactivateActCtx"}, context_deactivate_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"LoadResource"}, resource_load_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"SizeofResource"}, resource_size_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"LockResource"}, resource_lock_prelude);
		add({"gdi32.dll"}, {"RectInRegion"}, rect_in_region_prelude);
		add({"gdi32.dll"}, {"GetRgnBox"}, region_box_prelude);
		add({"gdi32.dll"}, {"PtInRegion"}, point_in_region_prelude);
		add({"user32.dll"}, {"SetThreadDpiAwarenessContext"}, dpi_set_prelude);
		add({"user32.dll"}, {"GetThreadDpiAwarenessContext"}, dpi_get_prelude);
		add({"user32.dll"}, {"GetKeyboardLayout"}, keyboard_layout_prelude);
		add({"user32.dll"}, {"IsWinEventHookInstalled"}, win_event_hook_prelude);
		add({"user32.dll"}, {"PtInRect"}, point_in_rect_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"QueryDepthSList"}, slist_depth_prelude);
		add({"ntdll.dll"}, {"RtlQueryDepthSList"}, slist_depth_prelude);
		add({"ole32.dll", "combase.dll"}, {"StringFromGUID2"}, guid_text_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"GetLocalTime"}, local_time_prelude);
		add({"user32.dll"}, {"IsWindowVisible"}, window_visible_prelude);
		add({"user32.dll"}, {"IsIconic"}, window_iconic_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"InitOnceBeginInitialize"}, init_once_begin_prelude);
		add({"user32.dll"}, {"EqualRect"}, equal_rect_prelude);
		add({"user32.dll"}, {"IsRectEmpty"}, is_rect_empty_prelude);
		add({"user32.dll"}, {"SetRectEmpty"}, set_rect_empty_prelude);
		add({"user32.dll"}, {"CopyRect"}, copy_rect_prelude);
		add({"user32.dll"}, {"InflateRect"}, inflate_rect_prelude);
		add({"user32.dll"}, {"UnionRect"}, union_rect_prelude);
		add({"user32.dll"}, {"MonitorFromPoint", "MonitorFromRect", "MonitorFromWindow"}, monitor_prelude);
		add({"user32.dll"}, {"GetWindowDpiAwarenessContext"}, window_dpi_context_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"GetTickCount", "GetTickCount64"}, tick_prelude);
		add({"winmm.dll"}, {"timeGetTime"}, tick_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"GetLocaleInfoEx"}, locale_prelude);
		add({"ntdll.dll"}, {"NtAllocateVirtualMemory", "ZwAllocateVirtualMemory"}, nt_commit_prelude);
		add({"ntdll.dll"}, {"NtFreeVirtualMemory", "ZwFreeVirtualMemory"}, nt_decommit_prelude);
		add({"kernel32.dll", "kernelbase.dll"}, {"MulDiv"}, muldiv_prelude);
		add({"shlwapi.dll", "kernelbase.dll"}, {"PathFindFileNameW"}, file_name_prelude);
		add({"user32.dll"}, {"TranslateMessage"}, translate_message_prelude);
		add({"user32.dll"}, {"CallMsgFilterW"}, message_filter_prelude);
		add({"user32.dll"}, {"GetDesktopWindow"}, desktop_window_prelude);
		add({"user32.dll"}, {"TranslateAcceleratorW"}, accelerator_prelude);
		add({"user32.dll"}, {"GetSystemMetrics"}, system_metrics_prelude);
		add({"user32.dll"}, {"GetSysColor"}, system_colors_prelude);
		add({"user32.dll"}, {"CallWindowProcW", "CallWindowProcA"}, call_procedure_prelude);
		// RAVEN_TRACE_MSG follows every message the runtime delivers, those to the thread's own windows too.
		if (!std::getenv("RAVEN_TRACE_MSG"))
			add({"user32.dll"}, {"SendMessageW"}, own_thread_send_prelude);
		add({"user32.dll"}, {"OffsetRect"}, offset_rect_prelude);
		add({"user32.dll"}, {"SetRect"}, set_rect_prelude);
		add({"user32.dll"}, {"IntersectRect"}, intersect_rect_prelude);
		add({"gdi32.dll"}, {"SetTextColor"}, dc_text_color_prelude);
		add({"gdi32.dll"}, {"SetBkColor"}, dc_background_color_prelude);
		add({"gdi32.dll"}, {"SetBkMode"}, dc_background_mode_prelude);
		add({"gdi32.dll"}, {"SetTextAlign"}, dc_text_align_prelude);
		add({"gdi32.dll"}, {"GetTextColor"}, dc_get_text_color_prelude);
		add({"gdi32.dll"}, {"GetBkColor"}, dc_get_background_color_prelude);
		add({"gdi32.dll"}, {"GetBkMode"}, dc_get_background_mode_prelude);
		add({"gdi32.dll"}, {"GetTextAlign"}, dc_get_text_align_prelude);
		add({"gdi32.dll"}, {"LPtoDP"}, dc_to_device_prelude);
		add({"gdi32.dll"}, {"DPtoLP"}, dc_to_logical_prelude);
		add({"user32.dll"}, {"DestroyIcon"}, destroy_icon_prelude);
		add({"gdi32.dll"}, {"DeleteDC"}, delete_dc_prelude);
		add({"gdi32.dll"}, {"DeleteObject"}, delete_object_prelude);
		add({"gdi32.dll"}, {"GetObjectW", "GetObjectA"}, bitmap_object_prelude);
		add({"user32.dll"}, {"GetDC"}, screen_get_dc_prelude);
		add({"user32.dll"}, {"ReleaseDC"}, screen_release_dc_prelude);
		add({"gdi32.dll"}, {"GetDeviceCaps"}, screen_device_caps_prelude);
		add({"user32.dll"}, {"GetWindowLongPtrW", "GetWindowLongPtrA"}, window_long_ptr_prelude);
		add({"user32.dll"}, {"GetWindowLongW", "GetWindowLongA"}, window_long_prelude);
		add({"user32.dll"}, {"GetPropW"}, window_property_prelude);
		add({"user32.dll"}, {"IsWindow", "IsWindowUnicode"}, window_exists_prelude);
		add({"user32.dll"}, {"GetWindowThreadProcessId"}, window_thread_prelude);
		add({"user32.dll"}, {"GetKeyState"}, key_state_prelude);
		add({"user32.dll"}, {"GetClientRect"}, window_client_prelude);
		add({"user32.dll"}, {"GetWindowRect"}, window_outer_prelude);
		add({"user32.dll"}, {"IsChild"}, window_child_prelude);
		add({"user32.dll"}, {"GetDpiForWindow"}, window_dpi_prelude);
		add({"user32.dll"}, {"GetFocus"}, focus_prelude);
		add({"user32.dll"}, {"GetForegroundWindow"}, foreground_prelude);
		add({"user32.dll"}, {"GetCursorPos"}, cursor_position_prelude);
		add({"user32.dll"}, {"DefWindowProcW"}, default_procedure_prelude);
		add({"user32.dll"}, {"GetMenuItemCount"}, menu_count_prelude);
		add({"user32.dll"}, {"GetSubMenu"}, menu_submenu_prelude);
		add({"user32.dll"}, {"GetMenuItemID"}, menu_id_prelude);
		add({"user32.dll"}, {"GetMenuStringW"}, menu_string_prelude);
		add({"user32.dll"}, {"GetMenuState"}, menu_state_prelude);
		return made;
	}();
	const auto library = table.find(module);
	if (library == table.end())
		return nullptr;
	const auto found = library->second.find(name);
	return found == library->second.end() ? nullptr : &found->second();
}

// The answers a guest thread takes from the runtime's tables while other threads run and the runtime changes them: those that only
// read, or compute from their arguments alone. Each goes through a wrapper that looks at the count of changes before and after
// (NativeWindows::table_version) and asks the runtime when a change came in between; the others, which write a log the runtime
// takes or keep a state of their own, are left to the runtime while threads run at once. So is one that takes an argument from the
// stack, as SetRect does its fifth: the wrapper keeps its own words there.
static bool answered_api(std::string_view module, std::string_view name)
{
	static const std::unordered_set<std::string_view> user{"IsWindow", "IsWindowUnicode", "GetWindowThreadProcessId", "GetClientRect",
		"GetWindowRect", "GetWindowLongPtrW", "GetWindowLongPtrA", "GetWindowLongW", "GetWindowLongA", "GetParent", "IsWindowVisible",
		"IsIconic", "IsChild", "GetKeyState", "GetSystemMetrics", "GetSysColor", "GetForegroundWindow", "GetCursorPos", "GetDesktopWindow",
		"MapWindowPoints", "GetDpiForWindow", "OffsetRect", "IntersectRect", "EqualRect", "IsRectEmpty", "SetRectEmpty",
		"CopyRect", "InflateRect", "UnionRect", "PtInRect", "GetAncestor", "GetPropW", "GetFocus", "GetMenuItemCount", "GetSubMenu",
		"GetMenuItemID"};
	// GetMenuState is left out: it searches a menu tree on the guest's stack, and the wrapper's way to give up expects the stack it left.
	static const std::unordered_set<std::string_view> gdi{"LPtoDP", "DPtoLP", "GetTextColor", "GetBkColor", "GetBkMode", "GetTextAlign",
		"RectInRegion", "GetRgnBox", "PtInRegion", "GetObjectW", "GetObjectA"};
	static const std::unordered_set<std::string_view> theme{"IsThemeActive", "IsAppThemed", "IsCompositionActive", "GetThemeAppProperties"};
	return (module == "user32.dll" && user.contains(name)) || (module == "gdi32.dll" && gdi.contains(name)) ||
		(module == "uxtheme.dll" && theme.contains(name));
}

// The wrapper: mov r11,[pointer]; test r11,r11; jz slow; mov r10,r11; mov eax,128; try: mov r11,[r10+0x7b0]; test r11b,1; jnz busy;
// push rax; push r10; push r9; push r8; push rdx; push rcx; push r11; call answer; mov r11,[rsp+40]; mov r11,[r11+0x7b0];
// cmp r11,[rsp]; jne again; add rsp,56; ret; again: add rsp,8; pop rcx; pop rdx; pop r8; pop r9; pop r10; pop rax; busy: pause;
// dec eax; jnz try; slow: jmp stub; then where the answer itself gives up, with the wrapper's return address and the seven words still
// on the stack: add rsp,16; pop rcx; pop rdx; pop r8; pop r9; add rsp,16; jmp stub.
// The room each wrapper of an answered call takes (see raven_answer_wrapper in gates.S).
static constexpr size_t answer_wrapper_slot = 128;

static std::vector<uint8_t> answer_wrapper(int32_t to_pointer, int32_t to_answer, int32_t to_stub)
{
	const auto bytes = served_stub::answer_wrapper();
	if (bytes.size() > answer_wrapper_slot)
		throw std::logic_error("the answer wrapper outgrew its slot");
	std::vector<uint8_t> code(bytes.begin(), bytes.end());
	static_assert(NativeWindows::table_version == 0x7b0, "the answer wrapper reads the tables' version at this offset (gates.S)");
	// Each field ends at its label; a target is relative to the wrapper's start, as the field's own end is.
	const auto put = [&](size_t end, int32_t target) {
		const int32_t relative = target - int32_t(end);
		std::memcpy(code.data() + end - 4, &relative, 4);
	};
	put(served_stub::answer_pointer(), to_pointer);
	put(served_stub::answer_call(), to_answer);
	put(served_stub::answer_stub(), to_stub);
	put(served_stub::answer_end(), to_stub);
	return code;
}

static bool answer_wrapper_at(std::span<const uint8_t> image, uint32_t rva)
{
	const auto bytes = served_stub::answer_wrapper();
	return size_t(rva) + bytes.size() <= image.size() && std::equal(bytes.begin(), bytes.begin() + 3, image.begin() + rva) &&
		image[rva + served_stub::answer_call() - 5] == 0xe8 && image[rva + served_stub::answer_stub() - 5] == 0xe9 &&
		image[rva + served_stub::answer_end() - 5] == 0xe9;
}

static size_t string_prelude_slot(const GuestPrelude &prelude)
{
	return (prelude.code.size() + 63) & ~size_t(63);
}

static std::vector<uint8_t> commit_prelude(int32_t to_pointer, int32_t to_trap)
{
	auto code = prelude_code(raven_prelude_commit, raven_prelude_commit_end);
	static_assert(NativeMemory::commit_windows == 0xa00 && NativeMemory::commit_log == 0xd10 && NativeMemory::commit_window_count == 48 &&
		NativeMemory::commit_log_count == 32, "the commit prelude's offsets are written into its code");
	const int32_t displacement = to_pointer - 7;
	std::memcpy(code.data() + 3, &displacement, 4);
	const int32_t relative = to_trap - int32_t(commit_jump(GuestMemoryApi::commit) + 5);
	std::memcpy(code.data() + commit_jump(GuestMemoryApi::commit) + 1, &relative, 4);
	return code;
}

// Where the served stub behind an export sits: the export itself, or the stub a clock prelude falls back to; for a commit prelude, the
// breakpoint it falls back to.
static uint32_t served_stub_rva(std::span<const uint8_t> image, std::string_view module, std::string_view name, uint32_t rva)
{
	if (queued_arguments(module, name) && rva <= image.size() && image.size() - rva >= queued_prelude_size &&
		image[rva] == 0x66 && image[rva + 1] == 0x90) {
		int32_t relative = 0;
		std::memcpy(&relative, image.data() + rva + queued_prelude_size - 4, 4);
		return uint32_t(int64_t(rva) + queued_prelude_size + relative);
	}
	if (module == "ntdll.dll" && rva <= image.size() && image.size() - rva >= 11 &&
		image[rva] == 0x4c && image[rva + 1] == 0x8b && image[rva + 2] == 0xd1 && image[rva + 3] == 0xb8)
		return rva;
	// A plain provider has no guest-side prelude in front of its trap.
	if (rva < image.size() && (image[rva] == 0xcc || (image[rva] == 0x90 && size_t(rva) + 15 < image.size() && image[rva + 14] == 0xcc)))
		return rva;
	if (answer_wrapper_at(image, rva)) {
		int32_t relative = 0;
		std::memcpy(&relative, image.data() + rva + served_stub::answer_stub() - 4, 4);
		return uint32_t(int64_t(rva) + served_stub::answer_stub() + relative);
	}
	if (const auto *prelude = block_prelude(module, name)) {
		int32_t relative = 0;
		std::memcpy(&relative, image.data() + rva + prelude->size() - 4, 4);
		return uint32_t(int64_t(rva) + prelude->size() + relative);
	}
	if (const auto *prelude = string_prelude(module, name)) {
		int32_t relative = 0;
		std::memcpy(&relative, image.data() + rva + prelude->code.size() - 4, 4);
		return uint32_t(int64_t(rva) + prelude->code.size() + relative);
	}
	if (const auto kind = guest_memory_api(module, name); kind != GuestMemoryApi::none) {
		int32_t relative = 0;
		std::memcpy(&relative, image.data() + rva + commit_jump(kind) + 1, 4);
		return uint32_t(int64_t(rva) + commit_jump(kind) + 5 + relative);
	}
	if (!guest_clock_api(module, name))
		return rva;
	int32_t relative = 0;
	std::memcpy(&relative, image.data() + rva + 12, 4);
	return uint32_t(int64_t(rva) + 16 + relative);
}

// The page of NT services, in the low area KUSER_SHARED_DATA opens: the flag the exports test, then an entry for each service, whose
// address is the service's number (see raven_nt_syscall). It is the guest's to read and run.
constexpr uint64_t service_page = 0x7ffe1000;
constexpr size_t service_entry_size = 16;
// The room of a service's export: its 24 bytes, and from service_stub on the stub its entry leads to.
constexpr size_t service_stub = 32, service_room = (service_stub + served_stub_size + 63) & ~size_t(63);

static const std::vector<std::string_view> &nt_services()
{
	static const auto services = [] {
		std::vector<std::string_view> names;
		for (const auto name : system_export_names("ntdll.dll"))
			if (name.starts_with("Nt"))
				names.push_back(name);
		return names;
	}();
	return services;
}

static uint32_t nt_service_number(std::string_view module, std::string_view name)
{
	if (module != "ntdll.dll" || (!name.starts_with("Nt") && !name.starts_with("Zw")))
		return 0;
	const std::string canonical = "Nt" + std::string(name.substr(2));
	const auto &names = nt_services();
	const auto found = std::lower_bound(names.begin(), names.end(), canonical);
	// Use the complete name table so a copied service keeps its identity across different process import graphs.
	return found != names.end() && *found == canonical ? uint32_t(service_page + (1 + size_t(found - names.begin())) * service_entry_size) : 0;
}

// Maps the page before any guest code runs. An entry leads to the stub given for its number, and without one makes the system call;
// the flag is set only where some entry has a stub. With RAVEN_CYCLES the flag is set and the system calls go through the timed routine
// after the entries, which stamps them (see raven_service_timed): it gives the address after that routine's system call, or 0.
static uint64_t map_service_page(NativeProcess &process, const std::map<uint32_t, uint64_t> &stubs)
{
	const size_t entries = (1 + nt_services().size()) * service_entry_size;
	const bool timed = CallCycles::instance().enabled();
	const size_t timed_size = size_t(raven_service_timed_end - raven_service_timed), landing_at = entries + timed_size;
	const size_t used = timed ? landing_at + size_t(raven_service_landing_end - raven_service_landing) : entries;
	std::vector<uint8_t> page((used + 4095) & ~size_t(4095), 0xcc);
	page[0] = timed || !stubs.empty();
	for (size_t at = service_entry_size; at < entries; at += service_entry_size) {
		if (const auto stub = stubs.find(uint32_t(service_page + at)); stub != stubs.end()) {
			const auto jump = std::copy(raven_service_stubbed, raven_service_stubbed_end, page.begin() + ptrdiff_t(at));
			std::memcpy(&*jump, &stub->second, 8);
		} else if (timed) {
			page[at] = 0xe9;
			const int32_t displacement = int32_t(entries) - int32_t(at + 5);
			std::memcpy(&page[at + 1], &displacement, 4);
		} else {
			std::copy(raven_service_trapped, raven_service_trapped_end, page.begin() + ptrdiff_t(at));
		}
	}
	if (timed) {
		std::copy(raven_service_timed, raven_service_timed_end, page.begin() + ptrdiff_t(entries));
		std::copy(raven_service_landing, raven_service_landing_end, page.begin() + ptrdiff_t(landing_at));
		process.set_service_landing(service_page + landing_at);
	}
	const int descriptor = memfd_create("raven-services", MFD_CLOEXEC);
	if (descriptor < 0 || ftruncate(descriptor, off_t(page.size())) != 0) {
		if (descriptor >= 0)
			close(descriptor);
		throw std::runtime_error("cannot create the page of NT services");
	}
	try {
		process.map_shared(descriptor, page.size(), 0, service_page);
	} catch (...) {
		close(descriptor);
		throw;
	}
	close(descriptor);
	process.write_memory(service_page, page);
	process.protect_memory(service_page, page.size(), PROT_READ | PROT_EXEC);
	// The routine ends in its system call and a return.
	return timed ? service_page + entries + timed_size - 1 : 0;
}

uint64_t api_trap_address(const PEModule &provider, std::string_view module, std::string_view name, uint32_t address)
{
	// An export that is its own breakpoint has no stub in front of it: a provider built without guest-side paths.
	const auto image = provider.loaded.data();
	if (address < image.size() && image[address] == 0xcc)
		return provider.loaded.base() + address;
	const uint32_t stub = served_stub_rva(image, module, name, address);
	// A plain x64 slot keeps its breakpoint past the space hooking libraries overwrite.
	if (!served_api(module, name) && size_t(stub) + 15 < image.size() && image[stub] == 0x90 && image[stub + 14] == 0xcc)
		return provider.loaded.base() + stub + 14;
	if (!served_api(module, name) && call_stubs() && stub_at(image, stub))
		return provider.loaded.base() + stub + served_trap;
	return provider.loaded.base() + stub + fast_stub_trap_offset(module, name);
}

// Offset of the slow-path breakpoint inside a fast or served stub.
size_t fast_stub_trap_offset(std::string_view module, std::string_view name)
{
	if (served_api(module, name))
		return served_trap;
	if (module == "user32.dll")
		return 14;
	if (module != "kernel32.dll" && module != "kernelbase.dll")
		return 0;
	for (const auto &stub : fast_stubs())
		if (stub.name == name)
			return 0x30;
	return 0;
}

std::vector<uint8_t> moved_provider(std::vector<uint8_t> bytes, uint64_t base)
{
	if (bytes.size() < 0xb8 || bytes[0x98] != 0x0b || bytes[0x99] != 0x02)
		throw std::runtime_error("only a 64-bit provider can be moved");
	std::memcpy(bytes.data() + 0xb0, &base, 8);
	return bytes;
}

std::vector<uint8_t> runtime_provider(std::string_view name, uint64_t base,
	std::span<const std::string_view> exports, bool x86, std::span<const std::string_view> data_exports,
	std::span<const std::pair<std::string_view, uint16_t>> ordinal_exports, bool fast_paths)
{
	if (!x86 && !data_exports.empty())
		throw std::runtime_error("native data exports require an x86 provider");
	for (const auto symbol : data_exports)
		if (!std::binary_search(exports.begin(), exports.end(), symbol))
			throw std::runtime_error("native data export is missing from the export table");
	if (exports.empty() || exports.size() > 4096 || name.empty() || name.size() > 63 || name.find('\0') != name.npos ||
		!std::is_sorted(exports.begin(), exports.end()) || std::adjacent_find(exports.begin(), exports.end()) != exports.end())
		throw std::runtime_error("invalid native provider export table");
	size_t string_size = name.size() + 1;
	for (const auto symbol : exports) {
		if (symbol.empty() || symbol.find('\0') != symbol.npos || symbol.size() + 1 > 1024 * 1024 - string_size)
			throw std::runtime_error("native provider export names exceed policy limit");
		string_size += symbol.size() + 1;
	}
	// Every export owns a slot; public ordinals pin a name to its Windows slot and the other names fill the gaps.
	std::vector<uint32_t> slot_of(exports.size(), 0);
	uint32_t slot_count = 0;
	for (const auto &[exported, ordinal] : ordinal_exports) {
		const auto found = std::lower_bound(exports.begin(), exports.end(), exported);
		if (found == exports.end() || *found != exported || !ordinal)
			throw std::runtime_error("native provider ordinal names an unknown export");
		auto &slot = slot_of[size_t(found - exports.begin())];
		if (slot)
			throw std::runtime_error("native provider ordinal is assigned twice");
		slot = ordinal;
		slot_count = std::max<uint32_t>(slot_count, ordinal);
	}
	// The other names take numbers above every public one Windows gives these libraries, so a program that asks for a number Raven does
	// not publish gets no function rather than an unrelated one.
	const uint32_t first_free = slot_count ? std::max<uint32_t>(slot_count + 1, 1024) : 1;
	std::vector<bool> taken(size_t(first_free) + exports.size() + 1, false);
	for (const auto slot : slot_of)
		if (slot && taken[slot])
			throw std::runtime_error("native provider ordinal collides with another export");
		else if (slot)
			taken[slot] = true;
	uint32_t next_slot = first_free;
	for (auto &slot : slot_of)
		if (!slot) {
			while (taken[next_slot])
				++next_slot;
			slot = next_slot;
			taken[slot] = true;
			slot_count = std::max(slot_count, slot);
		}
	const bool numbered = !ordinal_exports.empty();
	const size_t function_count = numbered ? slot_count : exports.size();
	const size_t directory = 0x200 + exports.size() * 16;
	const size_t functions = directory + 40;
	const size_t name_table = functions + function_count * 4;
	const size_t ordinals = name_table + exports.size() * 4;
	const size_t module_name = ordinals + exports.size() * 2;
	// Each data export owns one zeroed 16-byte slot after the strings; the section becomes writable for them.
	const size_t data_start = (module_name + string_size + 15) & ~size_t(15);
	const bool kernel_module = fast_paths && !x86 && (name == "kernel32.dll" || name == "kernelbase.dll");
	size_t fast_count = 0;
	if (kernel_module)
		for (const auto symbol : exports)
			for (const auto &stub : fast_stubs())
				fast_count += stub.name == symbol;
	const size_t fast_start = (data_start + data_exports.size() * 16 + 63) & ~size_t(63);
	size_t served_count = 0;
	// The calls the host threads answer have stubs of the same kind, after the served ones; a system service keeps its own entry.
	const bool calls = fast_paths && !x86 && call_stubs();
	size_t call_count = 0, service_count = 0;
	if (!x86)
		for (const auto symbol : exports) {
			const bool service = nt_service_number(name, symbol);
			served_count += served_api(name, symbol);
			service_count += service;
			call_count += calls && !served_api(name, symbol) && !service;
		}
	size_t clock_count = 0;
	if (!x86)
		for (const auto symbol : exports)
			clock_count += guest_clock_api(name, symbol);
	size_t commit_count = 0;
	if (!x86 && served_count)
		for (const auto symbol : exports)
			commit_count += guest_commit_api(name, symbol);
	// The block prelude of each export, looked up once for both the sizes and the code.
	std::vector<const std::vector<uint8_t> *> block_preludes(exports.size());
	size_t block_size = 0;
	if (fast_paths && !x86)
		for (size_t index = 0; index < exports.size(); ++index)
			if (const auto *prelude = block_preludes[index] = block_prelude(name, exports[index]))
				block_size += ((prelude->size() + 63) & ~size_t(63)) + (call_stubs() && answered_api(name, exports[index]) ? answer_wrapper_slot : 0);
	// The block pointer of the served stubs and the block preludes, then the stubs, then the clock preludes, then the commit preludes.
	const size_t served_start = fast_start + fast_count * 64;
	const bool served_area = served_count || call_count || block_size || (calls && service_count);
	const size_t clock_start = served_start + (served_area ? 64 + (served_count + call_count) * served_stub_size : 0);
	const size_t commit_start = clock_start + clock_count * clock_prelude_size;
	const size_t strings_start = commit_start + commit_count * commit_prelude_size;
	size_t strings_size = 0;
	if (kernel_module)
		for (const auto symbol : exports)
			if (const auto *prelude = string_prelude(name, symbol))
				strings_size += string_prelude_slot(*prelude);
	const size_t blocks_start = strings_start + strings_size;
	const bool queued = fast_paths && !x86 && std::getenv("RAVEN_PARALLEL") && std::getenv("RAVEN_QUEUED_GRAPHICS");
	size_t queued_count = 0;
	if (queued)
		for (const auto symbol : exports)
			queued_count += queued_arguments(name, symbol) != 0;
	const size_t queued_start = blocks_start + block_size;
	const size_t queued_end = queued_start + queued_count * queued_prelude_size;
	const size_t services_start = service_count ? (queued_end + 63) & ~size_t(63) : queued_end;
	const size_t end = services_start + service_count * service_room;
	std::vector<uint8_t> bytes((end + 511) & ~size_t(511));
	const auto rva = [](size_t offset) { return 0x1000 + offset - 0x200; };
	const auto put = [&](size_t offset, uint64_t value, size_t width = 4) {
		for (size_t index = 0; index < width; ++index)
			bytes.at(offset + index) = static_cast<uint8_t>(value >> (index * 8));
	};
	const auto text = [&](size_t offset, std::string_view value) {
		std::copy(value.begin(), value.end(), bytes.begin() + static_cast<ptrdiff_t>(offset));
	};
	put(0, 0x5a4d, 2);
	put(0x3c, 0x80);
	put(0x80, 0x4550);
	// PE32 headers end 16 bytes earlier: no BaseOfData widening, a 4-byte image base and 224 optional bytes.
	const size_t count_field = x86 ? 0xf4 : 0x104;
	const size_t section = x86 ? 0x178 : 0x188;
	put(0x84, x86 ? 0x14c : 0x8664, 2);
	put(0x86, 1, 2);
	put(0x94, x86 ? 224 : 240, 2);
	put(0x96, x86 ? 0x2103 : 0x2003, 2);
	put(0x98, x86 ? 0x10b : 0x20b, 2);
	if (x86 && base > UINT32_MAX - 0x100000)
		throw std::runtime_error("native x86 provider base exceeds the 32-bit address space");
	put(x86 ? 0xb4 : 0xb0, base, x86 ? 4 : 8);
	put(0xb8, 0x1000);
	put(0xbc, 0x200);
	put(0xd0, (rva(bytes.size()) + 4095) & ~size_t(4095));
	put(0xd4, 0x200);
	put(0xdc, 3, 2);
	put(count_field, 16);
	put(count_field + 4, rva(directory));
	// Data slots stay outside the export directory range, or loaders would treat them as forwarders.
	put(count_field + 8, data_start - directory);
	text(section, ".api");
	put(section + 8, bytes.size() - 0x200);
	put(section + 12, 0x1000);
	put(section + 16, bytes.size() - 0x200);
	put(section + 20, 0x200);
	put(section + 36, data_exports.empty() ? 0x60000020 : 0xe0000020);
	put(directory + 12, rva(module_name));
	// Only the independently verified common-controls identity receives a public ordinal.
	const bool icon_control = name == "comctl32.dll" && exports.size() == 1 && exports[0] == "LoadIconWithScaleDown";
	// Names without a public number take the top of the 16-bit ordinal range, where no program imports by number; ordinals are
	// words, and code that adds the base to an index in a word must not wrap.
	put(directory + 16, numbered ? 1 : icon_control ? 381 : uint32_t(0x10000 - function_count));
	put(directory + 20, function_count);
	put(directory + 24, exports.size());
	put(directory + 28, rva(functions));
	put(directory + 32, rva(name_table));
	put(directory + 36, rva(ordinals));
	text(module_name, name);
	size_t cursor = module_name + name.size() + 1;
	size_t fast_next = 0, served_next = 0, clock_next = 0, commit_next = 0, strings_next = strings_start, blocks_next = blocks_start;
	size_t queued_next = queued_start, services_next = services_start;
	for (size_t index = 0; index < exports.size(); ++index) {
		size_t code = 0x200 + index * 16;
		const uint32_t service = x86 ? 0 : nt_service_number(name, exports[index]);
		bytes[code] = 0xcc;
		bytes[code + 1] = 0xc3;
		// Hooking libraries copy an x64 entry preamble into a trampoline; keep the breakpoint beyond their 14-byte jump space.
		if (!x86 && (fast_paths || name == "user32.dll")) {
			std::fill_n(bytes.begin() + ptrdiff_t(code), 14, 0x90);
			bytes[code + 14] = 0xcc;
			bytes[code + 15] = 0xc3;
		}
		if (service) {
			code = services_next;
			services_next += service_room;
			std::copy(raven_nt_syscall, raven_nt_syscall_end, bytes.begin() + ptrdiff_t(code));
			put(code + size_t(raven_nt_syscall_service - raven_nt_syscall) - 4, service);
			put(code + size_t(raven_nt_syscall_flag - raven_nt_syscall) - 4, service_page);
			if (calls) {
				const size_t stub = code + service_stub;
				const auto served = served_stub_code(int32_t(served_start + 16) - int32_t(stub), true);
				std::copy(served.begin(), served.end(), bytes.begin() + ptrdiff_t(stub));
			}
		}
		if (!x86 && served_api(name, exports[index])) {
			code = served_start + 64 + served_next++ * served_stub_size;
			const auto stub = served_stub_code(int32_t(served_start) - int32_t(code));
			std::copy(stub.begin(), stub.end(), bytes.begin() + ptrdiff_t(code));
			if (guest_clock_api(name, exports[index])) {
				const size_t served = code;
				code = clock_start + clock_next++ * clock_prelude_size;
				const auto prelude = clock_prelude(int32_t(served_start + 8) - int32_t(code), int32_t(served) - int32_t(code));
				std::copy(prelude.begin(), prelude.end(), bytes.begin() + ptrdiff_t(code));
			}
		} else if (calls && !service) {
			// The stub reads a pointer of its own that stays zero, so it falls to its breakpoint until the runtime has it take the
			// thread's block instead.
			code = served_start + 64 + served_next++ * served_stub_size;
			const auto stub = served_stub_code(int32_t(served_start + 16) - int32_t(code), true);
			std::copy(stub.begin(), stub.end(), bytes.begin() + ptrdiff_t(code));
		}
		if (const auto *prelude = !service ? block_preludes[index] : nullptr) {
			const size_t trap = code;
			code = blocks_next;
			blocks_next += (prelude->size() + 63) & ~size_t(63);
			auto bytes_of = *prelude;
			// An answer taken while other threads run reads a pointer of its own, which the runtime gives only then, and is entered
			// through its wrapper, to which it also gives up.
			const bool wrapped = calls && answered_api(name, exports[index]);
			const size_t answer = code, wrapper = blocks_next;
			if (wrapped)
				blocks_next += answer_wrapper_slot;
			const int32_t displacement = int32_t(served_start + (wrapped ? 24 : 0)) - int32_t(code) - 7;
			std::memcpy(bytes_of.data() + 3, &displacement, 4);
			const int32_t relative = int32_t(wrapped ? wrapper + served_stub::answer_gives_up() : trap) - int32_t(code + bytes_of.size());
			std::memcpy(bytes_of.data() + bytes_of.size() - 4, &relative, 4);
			std::copy(bytes_of.begin(), bytes_of.end(), bytes.begin() + ptrdiff_t(code));
			if (wrapped) {
				const auto entry = answer_wrapper(int32_t(served_start + 24) - int32_t(wrapper), int32_t(answer) - int32_t(wrapper),
					int32_t(trap) - int32_t(wrapper));
				std::copy(entry.begin(), entry.end(), bytes.begin() + ptrdiff_t(wrapper));
				code = wrapper;
			}
		}
		if (const auto *prelude = kernel_module ? string_prelude(name, exports[index]) : nullptr) {
			const size_t trap = code;
			code = strings_next;
			strings_next += string_prelude_slot(*prelude);
			auto bytes_of = prelude->code;
			const int32_t relative = int32_t(trap) - int32_t(code + bytes_of.size());
			std::memcpy(bytes_of.data() + bytes_of.size() - 4, &relative, 4);
			std::copy(bytes_of.begin(), bytes_of.end(), bytes.begin() + ptrdiff_t(code));
		}
		if (!x86 && !service && served_count && guest_commit_api(name, exports[index])) {
			const size_t trap = code;
			code = commit_start + commit_next++ * commit_prelude_size;
			const auto prelude = guest_memory_api(name, exports[index]) == GuestMemoryApi::commit ?
				commit_prelude(int32_t(served_start) - int32_t(code), int32_t(trap) - int32_t(code)) :
				decommit_prelude(int32_t(served_start) - int32_t(code), int32_t(trap) - int32_t(code));
			std::copy(prelude.begin(), prelude.end(), bytes.begin() + ptrdiff_t(code));
		}
		if (kernel_module)
			for (const auto &stub : fast_stubs())
				if (stub.name == exports[index]) {
					code = fast_start + fast_next++ * 64;
					std::copy(stub.code.begin(), stub.code.end(), bytes.begin() + ptrdiff_t(code));
					bytes[code + 0x30] = 0xcc;
					bytes[code + 0x31] = 0xc3;
				}
		if (const auto *call = queued ? queued_call(name, exports[index]) : nullptr) {
			const size_t served = code;
			code = queued_next;
			queued_next += queued_prelude_size;
			const auto prelude = queued_prelude(int32_t(served) - int32_t(code), *call);
			std::copy(prelude.begin(), prelude.end(), bytes.begin() + ptrdiff_t(code));
		}
		const auto data = std::find(data_exports.begin(), data_exports.end(), exports[index]);
		const size_t entry = numbered ? slot_of[index] - 1 : index;
		put(functions + entry * 4, data == data_exports.end() ? rva(code) : rva(data_start + size_t(data - data_exports.begin()) * 16));
		put(name_table + index * 4, rva(cursor));
		put(ordinals + index * 2, entry, 2);
		text(cursor, exports[index]);
		cursor += exports[index].size() + 1;
	}
	return bytes;
}

// The native API providers sit high in the address space, as Windows places its system libraries: the low default bases are what
// programs' own libraries ask for (0x180000000 for most 64-bit DLLs), and a library that finds its base taken is relocated in every
// process that loads it. The host keeps its own mappings above 0x700000000000.
// A call the runtime is serving: the windows it touches stay touched until the outermost one returns.
struct WindowCallScope {
	NativeWindows &windows;
	explicit WindowCallScope(NativeWindows &windows) : windows(windows) { windows.api_entered(); }
	~WindowCallScope() { windows.api_left(); }
	WindowCallScope(const WindowCallScope &) = delete;
	WindowCallScope &operator=(const WindowCallScope &) = delete;
};

// A queue may be drained by a different guest thread from the one that published it.
struct ServedThreadScope {
	NativeEnvironment &environment;
	uint64_t previous;
	ServedThreadScope(NativeEnvironment &environment, uint64_t producer) : environment(environment), previous(environment.teb())
	{
		if (producer) environment.set_current(producer);
	}
	~ServedThreadScope() { environment.set_current(previous); }
};

// RAVEN_NO_GUEST_PATHS builds the providers without the paths the guest answers itself, for comparing the two.
static bool guest_paths()
{
	static const bool enabled = !std::getenv("RAVEN_NO_GUEST_PATHS");
	return enabled;
}

// ntdll, kernel32 and kernelbase sit at the same address in every process, as on Windows, where a program finds them in another
// process at the address they have in its own; the other providers follow in the order they are made, so their addresses depend
// on the program.
static uint64_t preferred_base(std::string_view name)
{
	if (name == "ntdll.dll")
		return 0x6fff00000000ULL;
	if (name == "kernel32.dll")
		return 0x6fff01000000ULL;
	if (name == "kernelbase.dll")
		return 0x6fff02000000ULL;
	return 0x6fff03000000ULL;
}

static uint64_t provider_base(const ModuleSet &modules, uint32_t size, std::string_view name = {})
{
	uint64_t base = preferred_base(name);
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
		if (base >= 0x800000000000ULL - size)
			throw std::runtime_error("no address available for native API provider");
		if (!overlap)
			return base;
	}
}

void end_with_process()
{
	ends_with_process = true;
}

bool releasing_memory()
{
	return releasing.load(std::memory_order_relaxed);
}

NativeResult run_native_process(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout,
	std::function<std::optional<ModuleBytes>(std::string_view)> source,
	std::function<void(std::span<const uint8_t>)> debug, DesktopPresentation presentation,
	std::optional<NativeLaunch> launch)
{
	if (timeout.count() <= 0 || (entry.image.characteristics & 0x2000) ||
		!std::any_of(modules.all().begin(), modules.all().end(), [&](const auto &item) { return item.second.get() == &entry; }))
		throw std::runtime_error("native process requires an executable graph and positive timeout");
	for (const auto &[name,module]:modules.all()) validate_native_image(module->image,name);
	// The system libraries are the runtime's, except those a copy in the program's folder may replace: those are looked for there first.
	std::vector<std::string_view> native_modules{"kernel32.dll","kernelbase.dll","user32.dll","gdi32.dll","comctl32.dll","ntdll.dll"};
	for (const auto library : system_library_names())
		if (!program_may_replace(library) && std::find(native_modules.begin(), native_modules.end(), library) == native_modules.end())
			native_modules.push_back(library);
#ifdef RAVEN_OPENGL
	// The bridge is the program's OpenGL unless the program brings its own opengl32.dll and the bridge was not asked for.
	const bool native_opengl = OpenGLBridge::enabled() && (OpenGLBridge::requested() || !modules.all().contains("opengl32.dll"));
	if (native_opengl) native_modules.push_back("opengl32.dll");
#endif
#ifdef RAVEN_VULKAN
	native_modules.push_back("vulkan-1.dll");
#endif
	load_initial_dependencies(modules,source,native_modules);
	std::map<uint64_t, std::string> traps;
#ifdef RAVEN_OPENGL
	if (native_opengl) {
		if (modules.all().contains("opengl32.dll")) throw std::runtime_error("native OpenGL cannot share a supplied opengl32.dll");
		const auto symbols = OpenGLBridge::exports();
		auto bytes = runtime_provider("opengl32.dll", 0x180000000ULL, symbols, false, {}, {}, guest_paths());
		const auto base = provider_base(modules, PEImage(bytes).image_size);
		auto &provider = modules.add("opengl32.dll", moved_provider(std::move(bytes), base), base);
		provider.runtime_api = true;
		for (const auto &symbol : provider.exports)
			traps.emplace(api_trap_address(provider, "opengl32.dll", symbol.names.at(0), symbol.address), symbol.names.at(0));
	}
#endif
	std::map<uint64_t,std::pair<uint32_t,std::string>> services;
	std::map<uint32_t, std::string> generated_services;
	std::vector<std::string_view> nt_names(NativeMemory::nt_exports().begin(),NativeMemory::nt_exports().end());
	nt_names.insert(nt_names.end(),NativeEnvironment::nt_exports().begin(),NativeEnvironment::nt_exports().end());
	nt_names.insert(nt_names.end(),NativeObjectAPI::nt_exports().begin(),NativeObjectAPI::nt_exports().end());
	const bool has_heap=modules.all().contains("raven-rtl.dll");
	if (has_heap) {
		if (modules.all().contains("ntdll.dll") && !modules.all().at("ntdll.dll")->runtime_api)
			throw std::runtime_error("native RTL component cannot share heap ownership with a supplied ntdll");
		nt_names.insert(nt_names.end(),NativeHeap::nt_exports().begin(),NativeHeap::nt_exports().end());
		for (const auto name : {"DbgBreakPoint","DbgPrint","NtQueryInformationThread","NtReleaseKeyedEvent",
			"NtWaitForKeyedEvent","RtlRaiseException","RtlRaiseStatus","ZwAllocateVirtualMemory","ZwFreeVirtualMemory",
			"ZwProtectVirtualMemory","ZwQueryPerformanceCounter","ZwQuerySystemInformation","ZwQueryVirtualMemory"})
			nt_names.push_back(name);
	}
	// Natives that the shared API registry implements.
	for (const auto name : {"NtSetInformationThread", "NtOpenThreadToken", "NtOpenThreadTokenEx", "NtOpenThread", "NtOpenProcess", "NtOpenProcessToken", "NtOpenProcessTokenEx", "NtCompareTokens", "NtQueryInformationProcess", "NtReadVirtualMemory", "NtWriteVirtualMemory", "NtQuerySection", "NtFlushInstructionCache", "RtlAddGrowableFunctionTable", "RtlGrowFunctionTable", "RtlDeleteGrowableFunctionTable",
		"NtWriteFile", "NtQueryDirectoryFile", "ZwQueryDirectoryFile", "RtlNtStatusToDosError", "NtQueryObject", "NtMapViewOfSection", "NtUnmapViewOfSection",
		"RtlAcquireSRWLockExclusive", "RtlAcquireSRWLockShared", "RtlWakeConditionVariable", "RtlWakeAllConditionVariable", "RtlWaitOnAddress",
		"RtlWakeAddressSingle", "RtlWakeAddressAll",
		// The processor a thread runs on.
		"NtGetCurrentProcessorNumber", "NtGetCurrentProcessorNumberEx", "RtlGetCurrentProcessorNumber", "RtlGetCurrentProcessorNumberEx",
		// The C runtime string functions ntdll exports.
		"strlen", "strcmp", "strncmp",
		// Cancelling one pending operation, which mio uses for its AFD polls.
		"NtCancelIoFileEx",
		// Opening files by their native names, which Rust's standard library and mio look up at run time.
		"NtCreateFile", "NtOpenFile",
		// What an open file and its volume are, which paint.net's file system layer looks up at run time.
		"NtQueryInformationFile", "NtQueryVolumeInformationFile",
		// What libuv, the event loop of Node.js and Electron, looks up when it starts and does not run without.
		"NtDeviceIoControlFile", "NtSetInformationFile",
		// What the runtime's own components import from ntdll, which they need whether a program loads them at start or later.
		"DbgPrint", "NtCancelIoFile", "NtCreateSection", "NtFsControlFile", "NtReadFile", "RtlCreateUnicodeStringFromAsciiz", "RtlFreeHeap",
		"RtlFreeUnicodeString", "RtlDllShutdownInProgress", "RtlGetVersion", "RtlIpv6AddressToStringExW", "RtlIpv6StringToAddressW", "RtlLengthSid", "RtlUnicodeToMultiByteN",
		"RtlUnicodeToMultiByteSize", "RtlIsTextUnicode", "RtlImageNtHeader", "NtDeleteKey",
		// What the Chromium Embedded Framework imports from ntdll, which the WebBrowser control loads when it first shows a page.
		"RtlFormatCurrentUserKeyPath", "RtlGetLastNtStatus",
		// What a browser installer looks up about the machine it runs on.
		"RtlGetDeviceFamilyInfoEnum", "RtlConvertDeviceFamilyInfoToString",
		// The unwinding of 64-bit frames, which libraries built with their own exception handling import.
		"RtlCaptureContext", "RtlLookupFunctionEntry", "RtlPcToFileHeader", "RtlVirtualUnwind", "RtlUnwindEx",
		// What the .NET runtime imports from ntdll: the function tables of the code it compiles, and the restoring of a context.
		"VerSetConditionMask", "RtlInstallFunctionTableCallback", "RtlAddFunctionTable", "RtlDeleteFunctionTable", "RtlRestoreContext",
		// What the native layer of WPF imports from ntdll.
		"DbgPrintEx", "DbgPrompt", "RtlVerifyVersionInfo", "RtlInitializeBitMap", "RtlSetBits", "RtlClearBits", "RtlSetAllBits",
		"RtlClearAllBits", "RtlAreBitsClear", "RtlAreBitsSet", "RtlFindClearBits", "RtlFindClearBitsAndSet", "RtlNumberOfSetBits",
		"RtlInitializeSListHead", "RtlInterlockedPushEntrySList", "RtlInterlockedPopEntrySList", "RtlInterlockedFlushSList",
		"RtlQueryDepthSList", "RtlFirstEntrySList",
		// What Wine's builds of the Direct3D 10.1 and Direct2D libraries import from ntdll.
		"_vsnprintf",
		// The registry natives, which libraries loaded after start (the Click-to-Run integration of Office among them) import by name.
		"NtCreateKey", "NtOpenKey", "NtOpenKeyEx", "NtQueryKey", "NtQueryValueKey", "NtSetValueKey", "NtDeleteValueKey", "NtSetInformationKey",
		// Creating and opening keyed events, which PDF-XChange Editor's core library imports beside waiting on and releasing them.
		"NtCreateKeyedEvent", "NtOpenKeyedEvent",
		// Looked up by diagnostics code that records where a call came from.
		"RtlCaptureStackBackTrace",
		// What the virtualization layer of Click-to-Run Office imports from ntdll: more of the registry and the objects, counted strings,
		// file name expressions, and the generic tables, which the guest RTL holds.
		"NtDuplicateObject", "NtEnumerateKey", "NtEnumerateValueKey", "NtFlushKey", "NtNotifyChangeKey", "NtNotifyChangeMultipleKeys",
		"NtOpenJobObject", "NtQuerySecurityObject", "NtRenameKey", "NtSetSecurityObject", "RtlInitUnicodeString", "RtlCompareUnicodeString",
		"RtlCopyUnicodeString", "RtlEqualUnicodeString", "RtlPrefixUnicodeString", "RtlIntegerToUnicodeString", "RtlIsNameInExpression",
		"RtlInitializeGenericTableAvl", "RtlInsertElementGenericTableAvl", "RtlInsertElementGenericTableFullAvl", "RtlIsGenericTableEmptyAvl",
		"RtlNumberGenericTableElementsAvl", "RtlLookupElementGenericTableAvl", "RtlLookupElementGenericTableFullAvl", "RtlEnumerateGenericTableAvl",
		"RtlEnumerateGenericTableWithoutSplayingAvl", "RtlLookupFirstMatchingElementGenericTableAvl", "RtlGetElementGenericTableAvl",
		"RtlDeleteElementGenericTableAvl"})
		if (std::find(nt_names.begin(),nt_names.end(),name)==nt_names.end()) nt_names.push_back(name);
	std::sort(nt_names.begin(),nt_names.end());
	// Every Windows system library that a program may import gets a provider; names without a handler stop visibly when called.
	std::vector<std::string> system_modules{"kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll", "comctl32.dll"};
	for (const auto library : system_library_names())
		if (std::find(system_modules.begin(), system_modules.end(), library) == system_modules.end())
			system_modules.emplace_back(library);
	ProgramLibraries program_libraries;
	if (launch)
		program_libraries = ProgramLibraries(launch->image_path, true);
	// A number with no name known for it still gets an export, so a program that imports by that number binds, and the call it ends in names the number.
	std::map<std::string, std::vector<std::pair<std::string_view, uint16_t>>> unknown_ordinals;
	std::map<std::string, std::vector<std::string>> unknown_names;
	std::deque<std::string> unknown_storage;
	for (const auto &[module_name, module] : modules.all())
		for (const auto &symbol : module->imports) {
			if (!symbol.by_ordinal)
				continue;
			auto library = symbol.module;
			for (auto &character : library)
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character + ('a' - 'A'));
			if (library.find('.') == library.npos)
				library += ".dll";
			const auto known = system_export_ordinals(library);
			if (std::find(system_modules.begin(), system_modules.end(), library) == system_modules.end() || modules.all().contains(library) ||
				std::any_of(known.begin(), known.end(), [&](const auto &item) { return item.second == symbol.ordinal; }))
				continue;
			const auto text = "Ordinal" + std::to_string(symbol.ordinal);
			if (std::find(unknown_names[library].begin(), unknown_names[library].end(), text) != unknown_names[library].end())
				continue;
			unknown_names[library].push_back(text);
			unknown_ordinals[library].emplace_back(unknown_storage.emplace_back(text), symbol.ordinal);
		}
	for (const auto &name : system_modules) {
		// The DirectSound component is the system's dsound.dll by any name, path or search; the runtime's own answers only beside a
		// copy the program ships, or without the component.
		if (name == "dsound.dll" && !program_libraries.contains(name) && std::filesystem::exists(std::filesystem::read_symlink("/proc/self/exe").parent_path() / name))
			continue;
		const auto base_symbols = name == "ntdll.dll" ? std::span<const std::string_view>(nt_names) : name == "user32.dll" ? NativeDesktop::user_exports() :
			name == "gdi32.dll" ? NativeDesktop::gdi_exports() :
			name == "comctl32.dll" ? NativeDesktop::control_exports() :
			name == "kernel32.dll" || name == "kernelbase.dll" ? std::span<const std::string_view>(names) : std::span<const std::string_view>();
		const auto library = name == "kernelbase.dll" ? std::string_view("kernel32.dll") : std::string_view(name);
		std::vector<std::string_view> symbols(base_symbols.begin(), base_symbols.end());
		// ntdll.dll answers for what it implements and what the program imports: a program probing for other functions by name must not
		// find ones that cannot run, as it does not on a system that lacks them.
		const auto listed = library == "ntdll.dll" ? std::span<const std::string_view>() : system_export_names(library);
		symbols.insert(symbols.end(), listed.begin(), listed.end());
		if (library == "ntdll.dll")
			for (const auto &[module_name, module] : modules.all())
				for (const auto &symbol : module->imports) {
					auto imported = symbol.module;
					for (auto &character : imported)
						if (character >= 'A' && character <= 'Z')
							character = static_cast<char>(character + ('a' - 'A'));
					if ((imported == "ntdll.dll" || imported == "ntdll") && !symbol.by_ordinal)
						symbols.push_back(unknown_storage.emplace_back(symbol.name));
				}
		if (const auto extra = unknown_ordinals.find(std::string(name)); extra != unknown_ordinals.end())
			for (const auto &item : extra->second)
				symbols.push_back(item.first);
		if (library == "kernel32.dll")
			symbols.push_back("RcConsolidateFrames");
		// Windows keeps the waits on an address in kernelbase.dll alone. A program that looks for them in kernel32.dll (VLC among them)
		// takes their absence for an older system and sets up its own waits; finding them there it skips that and calls what it never set.
		// kernel32.dll lends them only to a program that imports them from it, as some import libraries have it do.
		if (name == "kernel32.dll")
			std::erase_if(symbols, [&](std::string_view symbol) {
				if (symbol != "WaitOnAddress" && symbol != "WakeByAddressAll" && symbol != "WakeByAddressSingle")
					return false;
				for (const auto &[module_name, module] : modules.all())
					for (const auto &import : module->imports) {
						std::string imported = import.module;
						for (auto &character : imported)
							character = char(std::tolower(static_cast<unsigned char>(character)));
						if ((imported == "kernel32.dll" || imported == "kernel32") && import.name == symbol)
							return false;
					}
				return true;
			});
		std::sort(symbols.begin(), symbols.end());
		symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
		if (symbols.empty())
			continue;
		const auto published_numbers = system_export_ordinals(library);
		std::vector<std::pair<std::string_view, uint16_t>> ordinals(published_numbers.begin(), published_numbers.end());
		if (const auto extra = unknown_ordinals.find(std::string(name)); extra != unknown_ordinals.end())
			ordinals.insert(ordinals.end(), extra->second.begin(), extra->second.end());
		PEModule *provider = nullptr;
		const auto found = modules.all().find(name);
		if (found != modules.all().end() && found->second->runtime_api) {
			provider = found->second.get();
		} else if (found != modules.all().end()) {
			if (!program_may_replace(name))
				continue;
			const auto fallback = "system32-" + name;
			if (!modules.all().contains(fallback)) {
				auto bytes = runtime_provider(name, 0x180000000ULL, symbols, false, {}, ordinals, guest_paths());
				const auto base = provider_base(modules, PEImage(bytes).image_size);
				provider = &modules.add(fallback, moved_provider(std::move(bytes), base), base);
				provider->runtime_api = true;
			}
		} else {
			// The common controls are the component's when a program that does not import them loads them later; the provider is
			// then only the fallback a search of the system directory finds, as it is for a library the program ships.
			const bool late_controls = name == "comctl32.dll" && std::none_of(modules.all().begin(), modules.all().end(), [](const auto &item) {
				return std::any_of(item.second->imports.begin(), item.second->imports.end(), [](const auto &symbol) {
					auto library = symbol.module;
					for (auto &character : library)
						if (character >= 'A' && character <= 'Z')
							character = static_cast<char>(character + ('a' - 'A'));
					return library == "comctl32.dll" || library == "comctl32";
				});
			});
			const auto provider_key = program_libraries.contains(name) || late_controls ? "system32-" + name : name;
			// A later pass over the same names finds the fallback it staged before under the system directory's key.
			if (const auto staged = modules.all().find(provider_key); staged != modules.all().end()) {
				provider = staged->second.get();
				if (!provider->runtime_api)
					continue;
			} else {
				auto bytes = runtime_provider(name, 0x180000000ULL, symbols, false, {}, ordinals, guest_paths());
				const auto base = provider_base(modules, PEImage(bytes).image_size, name);
				provider = &modules.add(provider_key, moved_provider(std::move(bytes), base), base);
				provider->runtime_api = true;
			}
		}
		if (!provider)
			continue;
		// A guest component that implements part of a system library receives those calls directly instead of through a trap.
		if (provider->runtime_api) {
			struct Overlay { const char *library, *component, *prefix; };
			static constexpr Overlay overlays[] = {
				{"user32.dll", "raven-user32.dll", "raven-user32."}, {"comdlg32.dll", "raven-shell.dll", "raven-shell."},
				{"ole32.dll", "raven-shell.dll", "raven-shell."}, {"shell32.dll", "raven-shell.dll", "raven-shell."},
				{"shlwapi.dll", "raven-shell.dll", "raven-shell."}, {"setupapi.dll", "raven-setupapi.dll", "raven-setupapi."},
				{"ole32.dll", "raven-storage.dll", "raven-storage."}, {"combase.dll", "raven-shell.dll", "raven-shell."},
				{"uiautomationcore.dll", "raven-shell.dll", "raven-shell."}, {"runtimeobject.dll", "raven-shell.dll", "raven-shell."},
				{"api-ms-win-core-winrt-l1-1-0.dll", "raven-shell.dll", "raven-shell."},
				// The multimedia timers, wave devices, mixer and PlaySound call the program back on guest threads; the clock stays here.
				{"winmm.dll", "raven-winmm.dll", "raven-winmm."},
				{"msacm32.dll", "raven-msacm32.dll", "raven-msacm32."},
				// The heap, critical sections and the free paths of SRW locks and condition variables run in the guest RTL.
				{"kernel32.dll", "raven-rtl.dll", "raven-rtl."}, {"kernelbase.dll", "raven-rtl.dll", "raven-rtl."},
				// The generic AVL tables, critical sections, SRW locks, frame unwinding and the growth of code generators' function tables
				// of ntdll; its heap stays with the host, which calls the guest RTL itself.
				{"ntdll.dll", "raven-rtl.dll", "raven-rtl."},
				// COM's task allocator hands out the blocks of the process heap, which the guest RTL holds.
				{"ole32.dll", "raven-rtl.dll", "raven-rtl."}, {"combase.dll", "raven-rtl.dll", "raven-rtl."}};
			for (const auto &overlay_rule : overlays) {
				if (name != overlay_rule.library)
					continue;
				const bool nt_only = name == "ntdll.dll";
				const auto overlay = modules.all().find(overlay_rule.component);
				if (overlay == modules.all().end() || overlay->second->runtime_api)
					continue;
				for (auto &symbol : provider->exports)
					if (!symbol.names.empty() && (!nt_only || symbol.names.at(0).find("GenericTable") != std::string::npos || symbol.names.at(0).find("CriticalSection") != std::string::npos || symbol.names.at(0).find("SRWLock") != std::string::npos || symbol.names.at(0) == "RtlVirtualUnwind" || symbol.names.at(0) == "RtlGrowFunctionTable") && std::any_of(overlay->second->exports.begin(), overlay->second->exports.end(), [&](const auto &candidate) {
							return candidate.forwarder.empty() && std::find(candidate.names.begin(), candidate.names.end(), symbol.names.at(0)) != candidate.names.end();
						}))
						symbol.forwarder = overlay_rule.prefix + symbol.names.at(0);
			}
			// The structured exception handler of C code, which kernel32 publishes for compilers that import it from there, is the C
			// runtime's.
			if (name == "kernel32.dll")
				if (const auto runtime = modules.all().find("ucrtbase.dll"); runtime != modules.all().end() && !runtime->second->runtime_api)
					for (auto &symbol : provider->exports)
						if (!symbol.names.empty() && symbol.names.at(0) == "__C_specific_handler")
							symbol.forwarder = "ucrtbase.__C_specific_handler";
		}
		forward_opengl_formats(modules, *provider);
		// The traps of ntdll.dll are made below, where the Zw names answer as their Nt twins.
		if (name != "ntdll.dll")
			for (const auto &symbol : provider->exports)
				if (symbol.address && !symbol.names.empty() && symbol.forwarder.empty())
					traps.emplace(provider->runtime_api ? api_trap_address(*provider, name, symbol.names.at(0), symbol.address) :
						provider->loaded.base() + symbol.address, symbol.names.at(0));
	}
	const auto nt=modules.all().find("ntdll.dll");
	const bool needs_nt=std::any_of(modules.all().begin(),modules.all().end(),[](const auto &item) {
		return std::any_of(item.second->imports.begin(),item.second->imports.end(),[](const auto &symbol) {
			auto name=symbol.module;
			for (auto &character : name)
				if (character>='A' && character<='Z') character=char(character+('a'-'A'));
			return name=="ntdll.dll" || name=="ntdll";
		});
	});
	if (nt==modules.all().end() && needs_nt) {
		auto bytes=runtime_provider("ntdll.dll",0x180000000ULL,nt_names,false,{},{},guest_paths());
		const auto base=provider_base(modules,PEImage(bytes).image_size,"ntdll.dll");
		auto &provider=modules.add("ntdll.dll",moved_provider(std::move(bytes),base),base);
		provider.runtime_api=true;
	}
	if (modules.all().contains("ntdll.dll"))
		for (const auto &symbol : modules.all().at("ntdll.dll")->exports)
			for (const auto &name : symbol.names)
				if (symbol.address && (modules.all().at("ntdll.dll")->runtime_api || std::find(nt_names.begin(),nt_names.end(),name)!=nt_names.end()) && symbol.forwarder.empty()) {
					const auto &image=*modules.all().at("ntdll.dll");
					if (image.runtime_api) {
						if (const uint32_t number = nt_service_number("ntdll.dll", name)) {
							generated_services.emplace(number, "Nt" + name.substr(2));
							// The breakpoint of the stub behind the service: a call the host thread does not answer in the block stops there.
							if (call_stubs() && stub_at(image.loaded.data(), uint32_t(symbol.address + service_stub)))
								traps.emplace(image.loaded.base() + symbol.address + service_stub + served_trap, "Nt" + name.substr(2));
							continue;
						}
						traps.emplace(api_trap_address(image,"ntdll.dll",name,symbol.address),name.starts_with("Zw") ? "Nt"+name.substr(2) : name);
						continue;
					}
					const auto bytes=image.image.file_span(symbol.address,11);
					const bool first=bytes[0]==0xb8 && bytes[5]==0x4c && bytes[6]==0x8b && bytes[7]==0xd1;
					const bool second=bytes[0]==0x4c && bytes[1]==0x8b && bytes[2]==0xd1 && bytes[3]==0xb8;
					if ((!first && !second) || bytes[8]!=0x0f || bytes[9]!=0x05 || bytes[10]!=0xc3)
						throw std::runtime_error("unsupported native NT syscall stub: "+name);
					uint32_t number=0;
					for (size_t byte=0; byte<4; ++byte) number|=uint32_t(bytes[(first ? 1 : 4)+byte])<<(byte*8);
					services.emplace(image.loaded.base()+symbol.address+10,std::pair{number,name});
				}
#ifdef RAVEN_VULKAN
	const auto found_vulkan = modules.all().find("vulkan-1.dll");
	if (found_vulkan == modules.all().end()) {
		auto bytes = runtime_provider("vulkan-1.dll", 0x180000000ULL, VulkanBridge::exports(), false, {}, {}, guest_paths());
		const auto base = provider_base(modules, PEImage(bytes).image_size);
		auto &provider = modules.add("vulkan-1.dll", moved_provider(std::move(bytes), base), base);
		provider.runtime_api = true;
	}
	if (modules.all().at("vulkan-1.dll")->runtime_api)
		for (const auto &symbol : modules.all().at("vulkan-1.dll")->exports)
			traps.emplace(api_trap_address(*modules.all().at("vulkan-1.dll"), "vulkan-1.dll", symbol.names.at(0), symbol.address),
				symbol.names.at(0));
#endif
	alias_api_sets(modules);
	modules.bind_imports();
	auto order = initialization_order(modules, entry);
	// The user32 component registers the built-in classes in its DllMain, before any library that subclasses them.
	if (const auto controls = modules.all().find("raven-user32.dll"); controls != modules.all().end() && !controls->second->runtime_api) {
		const auto found = std::find(order.begin(), order.end(), controls->second.get());
		if (found != order.end()) {
			order.erase(found);
			order.insert(order.begin(), controls->second.get());
		}
	}
	std::vector<LoadedImage *> images;
	for (const auto &[name, module] : modules.all())
		images.push_back(&module->loaded);
	// The API traps, from the images' own bytes: each is an INT3, which a RET follows when the call returns through it.
	std::map<uint64_t, NativeProcess::ApiTrap> api_traps;
	{
		std::map<uint64_t, std::span<const uint8_t>> spans;
		for (const auto *image : images)
			spans.emplace(image->base(), image->data());
		for (const auto &[address, name] : traps) {
			const auto found = spans.upper_bound(address);
			const auto *bytes = found == spans.begin() ? nullptr : &std::prev(found)->second;
			const auto offset = bytes ? address - std::prev(found)->first : 0;
			if (!bytes || offset >= bytes->size() || (*bytes)[offset] != 0xcc) {
				char text[96];
				std::snprintf(text, sizeof text, "native API trap must be an executable INT3: %llx", static_cast<unsigned long long>(address));
				throw std::runtime_error(text);
			}
			api_traps.emplace_hint(api_traps.end(), address, NativeProcess::ApiTrap{0, offset + 1 < bytes->size() && (*bytes)[offset + 1] == 0xc3});
		}
	}
	// Stubs of exports forwarded to a component keep their breakpoint; trapping them lets callers that read the export table directly
	// reach the component's implementation with their own stack frame intact.
	std::unordered_map<uint64_t, uint64_t> forwarded_traps;
	for (const auto &[name, module] : modules.all()) {
		if (!module->runtime_api)
			continue;
		const std::string_view module_name = name.starts_with("system32-") ? std::string_view(name).substr(9) : std::string_view(name);
		const auto &bytes = module->loaded.data();
		for (const auto &symbol : module->exports) {
			if (symbol.forwarder.empty() || !symbol.address || symbol.names.empty())
				continue;
			const uint64_t stub = api_trap_address(*module, module_name, symbol.names.front(), symbol.address);
			const uint64_t offset = stub - module->loaded.base();
			if (traps.contains(stub) || offset >= bytes.size() || bytes[offset] != 0xcc)
				continue;
			try {
				forwarded_traps.emplace(stub, modules.resolve({name, symbol.names.front(), 0, false, 0}));
				api_traps.emplace(stub, NativeProcess::ApiTrap{0, offset + 1 < bytes.size() && bytes[offset + 1] == 0xc3});
			} catch (const std::runtime_error &) {
			}
		}
	}
	// The prefix's registry is read before a warm host stops: each start forked from it reads only what changed since.
	NativeRegistry hive(NativePrefix::from_environment().registry_file());
	// A warm host stops here, before the guest process exists: each start forked from it goes on with images of its own.
	warm_point([&] {
		for (const auto &[name, module] : modules.all())
			module->loaded.detach();
	});
	NativeProcess process(images, bootstrap, timeout, NativeMode::x64, entry.image.stack_reserve);
	process.add_api_traps(std::move(api_traps));
	// A start a warm host forked ahead has no time limit until its request comes (warm_ready below).
	auto deadline = std::chrono::steady_clock::now() + (warm_waiting() ? std::chrono::milliseconds(86400000) : timeout);
	NativeEnvironment environment(process, entry.loaded.base(), uint32_t(::getpid()), 1);
	const bool has_launch = launch.has_value();
	if (!launch) {
		NativeLaunch initial;
		for (const char byte : entry.name) {
			const auto character=static_cast<unsigned char>(byte);
			if (character>127) throw std::runtime_error("native default launch identity requires ASCII");
			initial.image_path.push_back(char16_t(character));
		}
		initial.command_line=u"\""+initial.image_path+u"\"";
		launch=std::move(initial);
	}
	NativeObjects kernel_objects;
	adopt_inherited(kernel_objects,launch->inherited);
	std::array<uint64_t,3> standard_handles{UINT64_MAX,UINT64_MAX,UINT64_MAX};
	for (size_t i=0;i<3;++i) {
		// A standard handle the creator also let the child inherit is that inherited object, under its own value.
		const auto shared=std::find_if(launch->inherited.begin(),launch->inherited.end(),[&](const InheritedHandle &record) {
			return (record.standard & (1U << i)) && kernel_objects.object(record.handle);
		});
		if (shared!=launch->inherited.end()) { standard_handles[i]=shared->handle; continue; }
		if (launch->standard_fds[i]<0) continue;
		const auto inherited=kernel_objects.inherit_file(launch->standard_fds[i]);
		if (inherited.status) throw std::runtime_error("cannot inherit native standard file");
		// The standard handles came from the creator by inheritance, so they are inheritable, as programs that pass them on in a
		// handle list (PROC_THREAD_ATTRIBUTE_HANDLE_LIST) require.
		kernel_objects.set_inherit(inherited.value,true);
		standard_handles[i]=inherited.value;
		// A start a warm host forked ahead holds the host's streams until its request's come: the handle then names those.
		warm_when_ready([&kernel_objects,handle=inherited.value,descriptor=launch->standard_fds[i]] {
			if (kernel_objects.refresh_file(handle,descriptor)) throw std::runtime_error("cannot inherit native standard file");
		});
	}
	environment.prepare_parameters(*launch,standard_handles);
	ApiRegistry api_registry, desktop_api;
	NativeLoaderData loader_data(process, environment.peb(), modules, entry);
	// Compilers can address executable TLS through slot zero without reading its index.
	auto tls_order = order;
	std::erase(tls_order, &entry);
	tls_order.insert(tls_order.begin(), &entry);
	NativeTLS tls(process, environment, tls_order);
	NativeModuleAPI module_api(process, environment, modules, entry);
	NativeResources resources(process, environment, modules, entry);
	// What a program may commit, as set by RAVEN_COMMIT_LIMIT_MB. By default the host's memory, as Windows lets a 64-bit program commit up
	// to its memory and page file: a .NET program maps assemblies of more than 100 MiB as images, and each is committed whole.
	const size_t host_memory=size_t(std::max<long>(::sysconf(_SC_PHYS_PAGES),0))*size_t(std::max<long>(::sysconf(_SC_PAGESIZE),0));
	// An empty or zero setting leaves the limit to the host, as the control panel describes it.
	const char *setting=std::getenv("RAVEN_COMMIT_LIMIT_MB");
	const size_t requested=setting ? size_t(std::strtoull(setting,nullptr,10)) : 0;
	const size_t commit_limit=requested ? requested*1024*1024 : std::max(host_memory,size_t(512)*1024*1024);
	NativeMemory memory(process,environment,commit_limit);
	register_images(memory,modules);
	NativeObjectAPI object_api(process,environment,kernel_objects,deadline);
	NativeHeap heap(process,environment,modules,entry.image);
	DesktopObjects desktop_objects;
	NativeDesktop desktop(process, environment, desktop_objects, resources);
	desktop.set_source(source);
	if (const auto component = modules.all().find("raven-user32.dll"); component != modules.all().end() && !component->second->runtime_api)
		desktop.set_oem_module(component->second->loaded.base());
	NativeWindows windows(process,environment,desktop_objects);
	windows.set_deadline(deadline);
	windows.set_menu_loader([&desktop](uint64_t instance,uint64_t identifier) { return desktop.menu_resource(instance,identifier); });
	windows.set_presentation(std::move(presentation));
#ifdef RAVEN_OPENGL
	std::unique_ptr<OpenGLBridge> opengl;
	if (native_opengl) opengl = std::make_unique<OpenGLBridge>(process, modules, desktop_objects, windows);
#endif
	desktop.set_updated([&](uint64_t window,const DesktopRect *area) { windows.drawing(window,area); });
#ifdef RAVEN_VULKAN
	VulkanBridge vulkan(process, modules);
	// The bridge asks for a window's surface from calls served beside the turn and from its present threads too: each takes the desktop
	// lock.
	vulkan.set_surface_source({[&windows](uint64_t window) { const DesktopScope desktop_scope; return windows.host_surface(window,true); },
		[&windows](uint64_t window) { const DesktopScope desktop_scope; windows.release_host_surface(window); },
		[&windows](uint64_t window) { const DesktopScope desktop_scope; return windows.client_size(window); },
		[&windows](uint64_t window) { const DesktopScope desktop_scope; return windows.shown(window); },
		windows.host_platform()});
#endif
	const auto remaining = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()); };
	size_t debug_bytes=0;
	KernelContext *kernel_ptr=nullptr;
	NativeThreads *threads_ptr=nullptr;
	auto last_service=std::chrono::steady_clock::now();
	const auto invoke = [&](uint64_t address, std::span<const uint64_t> arguments) {
		// The guest code runs without the desktop lock, as Windows leaves USER over a callback; the calls it makes take it again.
		const DesktopRelease desktop_release;
		auto budget = remaining();
		if (budget.count() <= 0)
			return NativeResult{NativeStop::timeout, 0, address, 0};
		// Level of this loop in the callback nesting; an exception dispatch that redirects the guest resumes at its own level.
		const size_t level = process.active() ? process.callback_depth() + 1 : 0;
		auto result = process.active() ? process.callback(address, arguments, budget) : process.call(address, arguments, budget);
		while (result.stop == NativeStop::api || result.stop == NativeStop::syscall || result.stop==NativeStop::fault) {
			if (result.stop==NativeStop::fault) {
				const auto image=modules.all().find("ntdll.dll");
				if (!debug || image==modules.all().end() || result.signal!=SIGSEGV || result.signal_code!=SI_KERNEL || result.value!=1 ||
					result.instruction<image->second->loaded.base() || result.instruction-image->second->loaded.base()+4>image->second->image.image_size ||
					process.read_memory(result.instruction,4)!=std::vector<uint8_t>{0xcd,0x2d,0xcc,0xc3}) {
					if (result.signal==SIGTRAP && result.signal_code==TRAP_HWBKPT) {
						std::cerr << "watch: write by";
						if (kernel_ptr && kernel_ptr->describe_stack) {
							const auto saved=process.context();
							(void)saved;
						}
						const auto rip=process.context().registers.rip;
						for (const auto &[module_name,module]:modules.all())
							if (rip>=module->loaded.base() && rip<module->loaded.base()+module->image.image_size)
								std::cerr << ' ' << module_name << '+' << std::hex << rip-module->loaded.base() << std::dec;
						std::cerr << '\n';
						budget=remaining();
						result=process.resume(budget);
						continue;
					}
					if (std::getenv("RAVEN_TRACE_FAULT")) {
						const auto describe=[&](uint64_t at) {
							for (const auto &[module_name,module]:modules.all())
								if (at>=module->loaded.base() && at<module->loaded.base()+module->image.image_size)
									return module_name+"+"+std::to_string(at-module->loaded.base());
							return std::string("?");
						};
						const auto registers=process.context().registers;
						std::cerr << "fault: signal " << result.signal << " at " << std::hex << result.instruction << " (" << describe(result.instruction) << ")"
							<< " address=" << result.fault_address << " rax=" << registers.rax << " rbx=" << registers.rbx
							<< " rcx=" << registers.rcx << " rdx=" << registers.rdx << " rsp=" << registers.rsp;
						for (const auto address : {registers.rsp, registers.rbx}) {
							try {
								const auto bytes = process.read_memory(address, 64);
								for (size_t at = 0; at < bytes.size(); at += 8) {
									uint64_t value = 0;
									std::memcpy(&value, bytes.data() + at, 8);
									std::cerr << " [" << address + at << "]=" << value << " (" << describe(value) << ")";
								}
							} catch (const std::runtime_error &) {}
						}
						std::cerr << std::dec << '\n';
					}
					// A CPU fault is a Windows exception: structured handlers of the faulting frames get the first chance.
					if (!kernel_ptr || !kernel_ptr->raise_fault || result.signal==0)
						return result;
					try {
						kernel_ptr->raise_fault(result);
					} catch (const ExecutionRedirected &redirected) {
						if (redirected.level!=level) throw;
						budget=remaining();
						if (budget.count()<=0)
							return NativeResult{NativeStop::timeout,0,result.instruction,0};
						result=process.resume(budget);
						continue;
					}
					return result;
				}
				const auto context=process.context().registers;
				const auto length=uint32_t(context.rdx);
				if (length>512 || length>1024*1024-debug_bytes)
					return result;
				const auto message=process.read_memory(context.rcx,length);
				debug(message);
				debug_bytes+=length;
				process.complete_debug_print(result.instruction);
				const auto budget=remaining();
				if (budget.count()<=0)
					return NativeResult{NativeStop::timeout,0,result.instruction,0};
				result=process.resume(budget);
				continue;
			}
			const std::string *service_name = nullptr;
			if (result.stop == NativeStop::syscall) {
				const auto supplied = services.find(result.instruction);
				const auto generated = generated_services.find(uint32_t(result.value));
				if (supplied != services.end() && supplied->second.first == result.value)
					service_name = &supplied->second.second;
				else if (result.value <= UINT32_MAX && generated != generated_services.end())
					service_name = &generated->second;
				else
					return result;
				CallCycles::instance().name_service(result.value, *service_name);
				process.enter_nt_service(result.instruction, uint32_t(result.value));
			}
			if (!service_name)
				if (const auto forwarded = forwarded_traps.find(result.value); forwarded != forwarded_traps.end()) {
					auto native = process.context();
					native.registers.rip = forwarded->second;
					native.registers.orig_rax = uint64_t(-1);
					process.redirect(native, process.callback_depth());
					budget = remaining();
					if (budget.count() <= 0)
						return NativeResult{NativeStop::timeout, 0, result.instruction, 0};
					result = process.resume(budget);
					continue;
				}
			const auto found = traps.find(result.value);
			if (!service_name && found == traps.end())
				return result;
			const auto &api_name = service_name ? *service_name : found->second;
			// Any call may post, send or draw, and the guest then asks the runtime again at its next poll of the queue, but for the polls
			// themselves and calls that leave every queue as it is: waits (a message that arrives meanwhile clears the slot), waitable
			// timers, regions, activation contexts, atoms and reading state. Office's idle loop makes some of them between its polls.
			static const std::unordered_set<std::string_view> keep_quiet{"GetInputState", "GetQueueStatus", "PeekMessageA", "PeekMessageW",
				"MsgWaitForMultipleObjectsEx", "MsgWaitForMultipleObjects", "WaitForSingleObject", "WaitForSingleObjectEx",
				"WaitForMultipleObjects", "WaitForMultipleObjectsEx", "SetWaitableTimerEx", "SetWaitableTimer", "CancelWaitableTimer",
				"SetRectRgn", "OffsetRgn", "CombineRgn", "ActivateActCtx", "DeactivateActCtx", "FindAtomW", "AddAtomW", "DeleteAtom",
				"GetActiveWindow", "GetKeyState", "GetAsyncKeyState", "GetCapture", "GetFocus", "GetCursorPos", "ReleaseMutex",
				"EventProviderEnabled", "GetSystemTimeAsFileTime", "GetSystemTimePreciseAsFileTime", "GetProcAddress", "FindResourceW",
				"GetCurrentInputMessageSource", "GetWindowThreadProcessId", "GetClassNameW", "GetClassLongPtrW", "GetClassLongW"};
			// A waiting thread's condition is looked at again after any call but those that end no wait, which say so here: they ask for
			// state, draw, or work on memory, and they are most of what a program calls.
			static const std::unordered_set<std::string_view> inert{"GetWindowThreadProcessId", "GetKeyState", "GetAsyncKeyState", "LPtoDP",
				"DPtoLP", "GetSystemMetrics", "GetSysColor", "GetClientRect", "GetWindowRect", "IsWindow", "IsWindowVisible", "IsWindowUnicode",
				"IsChild", "IsIconic", "GetParent", "GetAncestor", "GetWindowLongPtrW", "GetWindowLongPtrA", "GetWindowLongW", "GetWindowLongA",
				"GetClassLongPtrW", "GetClassLongW", "GetClassNameW", "GetPropW", "GetDeviceCaps", "SelectObject", "GetStockObject", "GetObjectW",
				"GetObjectA", "GetCurrentObject", "FlsGetValue", "FlsSetValue", "TlsGetValue", "TlsSetValue", "RtlGrowFunctionTable",
				"LocateXStateFeature", "RtlDllShutdownInProgress", "GetSystemTimePreciseAsFileTime", "GetSystemTimeAsFileTime",
				"QueryPerformanceCounter", "QueryPerformanceFrequency", "GetTickCount", "GetTickCount64", "MapWindowPoints", "ScreenToClient",
				"ClientToScreen", "GetFocus", "GetCapture", "GetActiveWindow", "GetForegroundWindow", "GetCursorPos", "GetDesktopWindow",
				"GetDpiForWindow", "GetTextMetricsW", "GetTextExtentPoint32W", "IsAppThemed",
				"IsThemeActive", "GetThemeAppProperties", "VirtualAlloc", "VirtualFree", "VirtualQuery", "VirtualProtect",
				"NtAllocateVirtualMemory", "NtFreeVirtualMemory", "NtQueryVirtualMemory", "NtProtectVirtualMemory", "MapViewOfFile",
				"BitBlt", "StretchBlt", "PatBlt", "FillRect", "MoveToEx", "LineTo", "SaveDC", "RestoreDC", "SetBkMode", "SetBkColor",
				"SetTextColor", "GetTextColor", "GetBkColor", "CreateRectRgn", "CombineRgn", "SetRectRgn", "GetRgnBox", "SelectClipRgn",
				"CreateCompatibleDC", "CreateCompatibleBitmap", "CreateSolidBrush", "CreatePen", "GdiFlush", "GetLastError", "SetLastError",
				"GetProcAddress", "GetModuleHandleW", "GetModuleHandleExW", "EventProviderEnabled", "MultiByteToWideChar",
				"WideCharToMultiByte", "lstrcmpiW", "lstrlenW", "CompareStringOrdinal", "GetCurrentThreadId", "GetCurrentProcessId",
				"IsProcessorFeaturePresent", "RavenCanvasPixels", "RavenCanvasCreate", "RavenCanvasSize", "RavenCanvasBounds",
				"RavenCanvasBatch", "GetKeyboardState", "GetQueueStatus", "GetInputState", "GetMessageTime", "GetMessagePos",
				"GetDoubleClickTime", "GetMonitorInfoW", "MonitorFromWindow", "MonitorFromPoint", "MonitorFromRect"};
			// Nor do the calls that read state or draw, nor changes of a window's own data and properties or taking its contexts: they put
			// nothing in a queue, and a window they leave needing paint wakes its thread itself (see NativeWindows::set_tell).
			if (!keep_quiet.contains(api_name) && !inert.contains(api_name) &&
				!window_call(api_name, process.api_arguments(2), std::as_const(desktop_objects)))
				windows.wake(environment.thread_id());
			if (threads_ptr && (inert.contains(api_name) || api_name.starts_with("vkCmd") || api_name.starts_with("gl")))
				threads_ptr->quiet_call();
			// ntdll's RtlExitUserProcess is the ExitProcess of programs that do without kernel32.
			if (api_name == "ExitProcess" || api_name == "RtlExitUserProcess") {
				const auto code=uint32_t(process.api_arguments(1)[0]);
				process.abandon_call();
				throw ProcessExit{code,result.instruction};
			}
			// RAVEN_TRACE_API is 1 for every call or a comma-separated list of API names to follow.
			bool tracing = false;
			static const char *const trace_selection = std::getenv("RAVEN_TRACE_API");
			static const char *const trace_program = std::getenv("RAVEN_TRACE_PROGRAM");
			if (const char *selection = trace_selection; selection &&
				(!trace_program || equal_fold(from_utf8(entry.name), from_utf8(trace_program)))) {
				const std::string wanted = std::string(",") + selection + ",";
				tracing = wanted == ",1," || wanted.find("," + api_name + ",") != wanted.npos;
			}
			// RAVEN_TRACE_API_ON names a file: while it exists every call is traced, checked at most ten times a second.
			static const char *const trace_switch = std::getenv("RAVEN_TRACE_API_ON");
			if (trace_switch && !tracing) {
				static auto checked = std::chrono::steady_clock::time_point{};
				static bool switched = false;
				if (const auto now = std::chrono::steady_clock::now(); now - checked > std::chrono::milliseconds(100)) {
					checked = now;
					switched = ::access(trace_switch, F_OK) == 0;
				}
				tracing = switched;
			}
			if (tracing) {
				static const bool trace_time = std::getenv("RAVEN_TRACE_TIME") != nullptr;
				if (trace_time)
					std::cerr << "t=" << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() << ' ';
				std::cerr << "api " << api_name << '(' << std::hex;
				// RAVEN_TRACE_ARGS widens the argument list shown (the default is the four register arguments).
				const char *width = std::getenv("RAVEN_TRACE_ARGS");
				for (const auto value : process.api_arguments(width ? std::clamp(std::atoi(width), 1, 12) : 4))
					std::cerr << ' ' << value;
				std::cerr << std::dec << " )";
				if (std::getenv("RAVEN_TRACE_STRINGS"))
					for (const auto value : process.api_arguments(4))
						if (const auto text = describe_text(process, value); !text.empty())
							std::cerr << " <" << text << '>';
				// The caller shows which guest module and offset asked, which is what a diagnosis needs most.
				const auto stack = process.read_memory(process.context().registers.rsp, 8);
				uint64_t caller = 0;
				std::memcpy(&caller, stack.data(), 8);
				for (const auto &[module_name, module] : modules.all())
					if (caller >= module->loaded.base() && caller < module->loaded.base() + module->image.image_size)
						std::cerr << " from " << module_name << '+' << std::hex << caller - module->loaded.base() << std::dec;
				if (std::getenv("RAVEN_TRACE_STACK") && kernel_ptr && kernel_ptr->describe_stack)
					std::cerr << " stack:" << kernel_ptr->describe_stack();
				if (threads_ptr)
					std::cerr << " on thread " << threads_ptr->current_id();
				std::cerr << '\n';
			}
			if (const char *spec = std::getenv("RAVEN_STEP")) {
				// Diagnosis aid: NAME:OCCURRENCE:COUNT single-steps COUNT instructions after the OCCURRENCE-th return of NAME.
				static std::map<std::string,int> seen;
				const std::string text(spec);
				const auto first = text.find(':'), second = text.find(':', first + 1);
				if (first != text.npos && second != text.npos && api_name == text.substr(0, first) &&
					(!trace_program || equal_fold(from_utf8(entry.name), from_utf8(trace_program))) &&
					++seen[api_name] == std::stoi(text.substr(first + 1, second - first - 1)))
					process.trace_instructions(size_t(std::stoul(text.substr(second + 1))), [&modules, &process](uint64_t rip) {
						std::cerr << "step";
						for (const auto &[module_name, module] : modules.all())
							if (rip >= module->loaded.base() && rip < module->loaded.base() + module->image.image_size)
								std::cerr << ' ' << module_name << '+' << std::hex << rip - module->loaded.base() << std::dec;
						if (std::getenv("RAVEN_TRACE_STRINGS")) {
							const auto registers = process.context().registers;
							for (const auto value : {registers.rcx, registers.rdx, registers.r8, registers.r9})
								if (const auto value_text = describe_text(process, value); !value_text.empty())
									std::cerr << " <" << value_text << '>';
						}
						std::cerr << '\n';
					});
			}
			if (api_name=="RtlRaiseStatus")
				return native_raise_rtl_exception(process,true);
			// A thread that never waits still answers the other processes, a few times every second at most.
			if (const auto now = std::chrono::steady_clock::now(); now - last_service >= std::chrono::milliseconds(2)) {
				last_service = now;
				if (threads_ptr)
					threads_ptr->serve();
			}
			bool dispatched;
			api_in_progress = api_name.c_str();
			// A Vulkan call takes the desktop lock itself where it reaches a window (see set_surface_source): it waits for the device
			// and for the bridge's own threads, which take the lock too, so it neither holds the lock nor takes it for the desktop's
			// bookkeeping, which the next call that reaches the desktop does.
			// A wait, a thread or a synchronization object is the kernel's alone: holding the desktop through it would keep the threads
			// that draw beside the turn waiting (see ApiRegistry::set_kernel).
			const bool graphics_call = api_name.starts_with("vk"), kernel_call = api_registry.kernel(api_name);
			std::optional<DesktopScope> desktop_scope;
			if (!graphics_call && !kernel_call) {
				desktop_scope.emplace();
				windows.apply_dc_log();
				windows.take_deferred([&desktop](uint32_t operation, uint64_t handle) { desktop.apply_deferred(operation, handle); });
				windows.screen_dc_use(api_name);
			}
			const auto profile_start = RuntimeProfile::instance().enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			static const bool trace_failures = std::getenv("RAVEN_TRACE_FAIL");
			const auto error_before = trace_failures ? environment.last_error() : 0;
			const auto failure_arguments = trace_failures ? process.api_arguments(4) : std::vector<uint64_t>{};
			try {
				if (kernel_ptr && kernel_ptr->leave_abandoned_callbacks && process.callback_depth() > 0)
					kernel_ptr->leave_abandoned_callbacks();
				std::optional<WindowCallScope> window_scope;
				if (!graphics_call)
					window_scope.emplace(windows);
				// The generalized desktop services come first so that they replace the older narrow ones.
				dispatched = desktop_api.dispatch(process, environment, api_name) || environment.dispatch(api_name) || module_api.dispatch(api_name) || resources.dispatch(api_name) || memory.dispatch(api_name) || object_api.dispatch(api_name) || heap.dispatch(api_name) || desktop.dispatch(api_name) || windows.dispatch(api_name) || api_registry.dispatch(process, environment, api_name)
#ifdef RAVEN_VULKAN
					|| vulkan.dispatch(api_name)
#endif
					;
			} catch (const ExecutionRedirected &redirected) {
				if (redirected.level!=level) throw;
				budget=remaining();
				if (budget.count()<=0)
					return NativeResult{NativeStop::timeout,0,result.instruction,0};
				result=process.resume(budget);
				continue;
			}
			// A surface released through the desktop's other handlers is unmapped as the call ends too (see NativeWindows::finish_call).
			if (desktop_scope)
				windows.finish_call();
			// Calls that stopped the guest are counted apart from served ones: a stop costs far more than the call.
			if (RuntimeProfile::instance().enabled() && profile_start != std::chrono::steady_clock::time_point{})
				RuntimeProfile::instance().api((process.call_waiting() ? "(call) " : "(stop) ") + api_name, std::chrono::steady_clock::now() - profile_start);
			if (!dispatched && std::getenv("RAVEN_SURVEY_MISSING")) {
				// Survey aid: report each missing API once and fail it, so one run lists every API a program reaches.
				static std::set<std::string> reported;
				if (reported.insert(api_name).second)
					std::cerr << "raven: missing " << api_name << '\n';
				environment.set_last_error(120);
				process.return_api(0);
				budget = remaining();
				result = process.resume(budget);
				continue;
			}
			if (!dispatched) {
				std::cerr << "raven: unimplemented native API " << api_name << '\n';
				// RAVEN_CONTINUE_UNIMPLEMENTED lists every missing function of a run: each call fails with ERROR_CALL_NOT_IMPLEMENTED instead.
				if (!std::getenv("RAVEN_CONTINUE_UNIMPLEMENTED"))
					return result;
				environment.set_last_error(120);
				process.return_api(0);
				budget = remaining();
				result = process.resume(budget);
				continue;
			}
			if (tracing)
				std::cerr << "  u=" << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() << " -> " << std::hex << process.result_register() << std::dec << " (error " << environment.last_error() << ")\n";
			if (trace_failures && environment.last_error() != error_before) {
				std::cerr << "fail " << api_name << " error " << environment.last_error() << " args";
				for (const auto value : failure_arguments) std::cerr << ' ' << std::hex << value << std::dec;
				if (api_name == "CreateWindowExW" && failure_arguments.size() > 1) {
					std::string class_name;
					try {
						if (failure_arguments[1] > 0xffff) {
							const auto text = process.read_memory(failure_arguments[1], 64);
							for (size_t i = 0; i < 32 && (text[i * 2] | text[i * 2 + 1]); ++i)
								class_name.push_back(char(text[i * 2]));
						} else
							class_name = "#" + std::to_string(failure_arguments[1]);
					} catch (...) {}
					std::cerr << " class=" << class_name;
				}
				// Calls that take a name first say which name failed.
				static constexpr std::string_view named[] = {"LoadLibraryExW", "LoadLibraryW", "CreateFileW", "GetFileAttributesW",
					"GetFileAttributesExW", "FindFirstFileW", "FindFirstFileExW", "CreateDirectoryW", "DeleteFileW", "OpenEventW", "OpenMutexW",
					"CreateFileMappingW", "OpenFileMappingW"};
				const bool name_first = std::find(std::begin(named), std::end(named), api_name) != std::end(named);
				const size_t name_index = api_name == "CreateFileMappingW" ? 5 : api_name.starts_with("Open") ? 2 : 0;
				if (name_first && failure_arguments.size() > name_index && failure_arguments[name_index] > 0xffff) {
					std::string text;
					try {
						const auto raw = process.read_memory(failure_arguments[name_index], 520);
						for (size_t i = 0; i < 260 && (raw[i * 2] | raw[i * 2 + 1]); ++i)
							text.push_back(raw[i * 2 + 1] ? '?' : char(raw[i * 2]));
					} catch (...) {}
					std::cerr << " name=" << text;
				}
				// A failed lookup says which export of which library was missing.
				if (api_name == "GetProcAddress" && failure_arguments.size() > 1) {
					for (const auto &[module_name, module] : modules.all())
						if (module->loaded.base() == failure_arguments[0]) std::cerr << " module=" << module_name;
					if (failure_arguments[1] > 0xffff) {
						std::string text;
						try {
							const auto raw = process.read_memory(failure_arguments[1], 96);
							for (size_t i = 0; i < raw.size() && raw[i]; ++i) text.push_back(char(raw[i]));
						} catch (...) {}
						std::cerr << " name=" << text;
					}
				}

				for (const auto &[module_name, module] : modules.all()) {
					const auto stack = process.read_memory(process.context().registers.rsp, 8);
					uint64_t caller = 0;
					std::memcpy(&caller, stack.data(), 8);
					if (caller >= module->loaded.base() && caller < module->loaded.base() + module->image.image_size)
						std::cerr << " from " << module_name << '+' << std::hex << caller - module->loaded.base() << std::dec;
				}
				std::cerr << '\n';
			}

			budget = remaining();
			if (budget.count() <= 0)
				return NativeResult{NativeStop::timeout, 0, result.instruction, 0};
			if (RuntimeProfile::instance().enabled()) {
				const auto before = std::chrono::steady_clock::now();
				result = process.resume(budget);
				RuntimeProfile::instance().guest(std::chrono::steady_clock::now() - before);
			} else {
				try {
					result = process.resume(budget);
				} catch (const std::runtime_error &failure) {
					// A handler that leaves the call without its answer is named, since the global name may belong to another thread by now.
					throw std::runtime_error(std::string(failure.what()) + " after " + api_name);
				}
			}
		}
		return result;
	};
	const auto prefix = NativePrefix::from_environment();
	prefix.place_runtime_components(std::filesystem::path(bootstrap).parent_path());
	NativePaths paths(prefix);
	if (launch && !launch->directory.empty())
		paths.set_current(launch->directory);
	desktop.set_section_source([&](uint64_t handle) -> std::optional<std::pair<int, uint64_t>> {
		const auto section = kernel_objects.object_as<SectionObject>(handle);
		if (!section || section->descriptor < 0)
			return std::nullopt;
		return std::make_pair(section->descriptor, section->size);
	});
	desktop.set_host_files([&paths](std::u16string_view path) -> std::optional<std::vector<uint8_t>> {
		const auto resolved = paths.resolve(path);
		if (!resolved || !resolved->exists || !std::filesystem::is_regular_file(resolved->host))
			return std::nullopt;
		return read_host_file(resolved->host);
	});
	GuestAllocator allocator(memory, heap);
	NativeThreads threads(process, environment, memory, &tls, deadline);
	threads.set_runner(invoke);
	threads.default_stack_reserve = entry.image.stack_reserve;
	threads_ptr = &threads;
	// Other processes ask for handles of this one and send theirs; the scheduler answers them.
	threads.add_service([&kernel_objects] { receive_handles(kernel_objects); });
	object_api.set_cooperative(true);
	object_api.set_threads(&threads);
	windows.set_message_wait([&threads](std::function<bool()> ready,std::chrono::steady_clock::time_point until) {
		HostWait waiting;
		threads.block(std::move(ready), until, false, false, true);
	});
	windows.set_input_taken([&threads] { threads.took_input(); });
	windows.set_idle_sources([&threads](std::chrono::steady_clock::time_point limit) { return threads.idle_until(limit); },
		[&threads](std::vector<pollfd> &descriptors) { threads.idle_descriptors(descriptors); }, [&threads] { threads.drain_wake(); });
	KernelContext kernel{process, environment, memory, allocator, kernel_objects, threads, paths, modules, entry, *launch, invoke};
	threads.set_watched([reactor = kernel.reactor](std::vector<pollfd> &descriptors) { reactor->watch(descriptors); });
	kernel.reactor->set_watch_added([&threads](int descriptor, short events) { threads.watch_added(descriptor, events); });
	windows.set_global_allocator([&allocator](uint64_t size) { return allocator.allocate(size,true); });
	windows.set_global_data([&allocator](uint64_t handle) { return allocator.data(handle); });
	// The scheduler looks at a waiting thread's condition after the thread gave the desktop lock up with the runtime: what looks at the
	// windows' queues takes it again.
	kernel.queue_status=[&windows](uint32_t thread,uint32_t mask,bool clear) {
		const DesktopScope desktop_scope;
		return windows.queue_status(thread,mask,clear);
	};
	kernel.queue_quiet=[&windows](uint32_t thread) { const DesktopScope desktop_scope; windows.mark_quiet(thread); };
	kernel.present_frames=[&windows] { const DesktopScope desktop_scope; return windows.present_frames(); };
	windows.set_block([&threads](std::function<bool()> ready) {
		HostWait waiting;
		threads.block([ready = std::move(ready)] { const DesktopScope desktop_scope; return ready(); }, std::nullopt, false, false, true);
	});
	windows.set_tell([&threads](uint32_t thread) { threads.tell(thread); });
	threads.add_end_listener([&windows](uint32_t thread) { const DesktopScope desktop_scope; windows.thread_ended(thread); });
#ifdef RAVEN_VULKAN
	vulkan.set_blocking([&threads](std::function<bool()> ready, std::optional<std::chrono::steady_clock::time_point> wake) {
		HostWait waiting;
		return threads.block(std::move(ready), wake);
	}, [&threads] { threads.nudge(); });
#endif
	// The block the served stubs of every runtime provider share, and which stub serves which name.
	const int served_file = memfd_create("raven-served-calls", MFD_CLOEXEC);
	if (served_file < 0 || ftruncate(served_file, 4096) != 0)
		throw std::runtime_error("cannot create the served call block");
	auto *served_block = static_cast<uint64_t *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, served_file, 0));
	if (served_block == MAP_FAILED)
		throw std::runtime_error("cannot map the served call block");
	const uint64_t served_guest = process.map_shared(served_file, 4096);
	close(served_file);
	std::map<uint64_t, std::string> served_names;
	// Every stub by the API it serves, for what RAVEN_CYCLES reports; made only when it reports, as it copies thousands of names.
	const bool cycles_named = CallCycles::instance().enabled();
	std::map<uint64_t, std::string> stub_names;
	// The pointers live in the module images, which outlast this process: they are cleared when it ends so that another process mapping
	// the same images starts with its stubs trapping.
	struct ServedPointers {
		std::vector<uint8_t *> places;
		std::vector<std::pair<uint8_t *, std::array<uint8_t, 12>>> entries;
		std::vector<std::pair<uint8_t *, std::array<uint8_t, 7>>> tails;
		~ServedPointers()
		{
			for (auto *place : places)
				std::memset(place, 0, 8);
			for (const auto &[place, bytes] : entries)
				std::copy(bytes.begin(), bytes.end(), place);
			for (const auto &[place, bytes] : tails)
				std::copy(bytes.begin(), bytes.end(), place);
		}
	} served_pointers;
	std::vector<uint64_t> called_stubs;
	std::map<uint64_t, std::string> beside_names;
	for (const auto &[module_key, module] : modules.all()) {
		if (!module->runtime_api)
			continue;
		const std::string_view module_name = module_key.starts_with("system32-") ? std::string_view(module_key).substr(9) : std::string_view(module_key);
		for (const auto &symbol : module->exports) {
			// The clock and the tick counts only read calibration; their pointers are safe while other threads run.
			const auto *prelude = symbol.address && !symbol.names.empty() && symbol.forwarder.empty() ? block_prelude(module_name, symbol.names.at(0)) : nullptr;
			if (symbol.address && !symbol.names.empty() && symbol.forwarder.empty() &&
				(guest_clock_api(module_name, symbol.names.at(0)) || prelude == &tick_prelude() || (!process.parallel() && prelude) ||
					(process.parallel() && answer_wrapper_at(module->loaded.data(), symbol.address)) ||
					(process.parallel() && prelude && self_checked_api(module_name, symbol.names.at(0)))) &&
				served_stub_rva(module->loaded.data(), module_name, symbol.names.at(0), symbol.address) != symbol.address) {
				auto image = module->loaded.data();
				int32_t displacement = 0;
				std::memcpy(&displacement, image.data() + symbol.address + 3, 4);
				std::memcpy(image.data() + symbol.address + 7 + displacement, &served_guest, 8);
				served_pointers.places.push_back(image.data() + symbol.address + 7 + displacement);
			}
			if (!symbol.address || symbol.names.empty() || !symbol.forwarder.empty())
				continue;
			auto image = module->loaded.data();
			const uint32_t stub = served_stub_rva(image, module_name, symbol.names.at(0), symbol.address);
			if (!served_api(module_name, symbol.names.at(0))) {
				// A call the thread's host thread answers: its stub takes the thread's block as the served ones do.
				if (!call_stubs() || !process.parallel() || !stub_at(image, stub) || image[stub] != 0x4c || changes_context(symbol.names.at(0)))
					continue;
				std::array<uint8_t, 12> previous{};
				std::copy_n(image.data() + stub, previous.size(), previous.begin());
				served_pointers.entries.emplace_back(image.data() + stub, previous);
				std::copy(served_stub::local_block().begin(), served_stub::local_block().end(), image.data() + stub);
				called_stubs.push_back(module->loaded.base() + stub);
				if (cycles_named)
					stub_names.emplace(module->loaded.base() + stub, symbol.names.at(0));
				if (beside_api(module_name, symbol.names.at(0)))
					beside_names.emplace(module->loaded.base() + stub, symbol.names.at(0));
				continue;
			}
			if (process.parallel()) {
				std::array<uint8_t, 12> previous{};
				std::copy_n(image.data() + stub, previous.size(), previous.begin());
				served_pointers.entries.emplace_back(image.data() + stub, previous);
				// Load this thread's block through GS; keep every completion trap at its original offset.
				std::copy(served_stub::local_block().begin(), served_stub::local_block().end(), image.data() + stub);
			} else {
				int32_t displacement = 0;
				std::memcpy(&displacement, image.data() + stub + 3, 4);
				std::memcpy(image.data() + stub + 7 + displacement, &served_guest, 8);
				served_pointers.places.push_back(image.data() + stub + 7 + displacement);
			}
			served_names.emplace(module->loaded.base() + stub, symbol.names.at(0));
			// With a host thread for each guest thread these calls are answered as the others are: their handlers may need the guest
			// stopped, which only the thread's own host thread can do.
			if (call_stubs() && process.parallel()) {
				called_stubs.push_back(module->loaded.base() + stub);
				if (beside_api(module_name, symbol.names.at(0)))
					beside_names.emplace(module->loaded.base() + stub, symbol.names.at(0));
				std::array<uint8_t, 7> previous{};
				std::copy_n(image.data() + stub + served_tail, previous.size(), previous.begin());
				served_pointers.tails.emplace_back(image.data() + stub + served_tail, previous);
				std::copy(served_stub::asked_tail().begin(), served_stub::asked_tail().end(), image.data() + stub + served_tail);
			}
		}
	}
	// A service's entry leads to a stub its thread's host thread answers, as the other calls' stubs. The services that act on the
	// thread's registers keep the system call, as their calls keep the breakpoint.
	uint64_t timed_service_return = 0;
	if (const auto ntdll = modules.all().find("ntdll.dll"); ntdll != modules.all().end() && ntdll->second->runtime_api) {
		std::map<uint32_t, uint64_t> entered;
		if (call_stubs() && process.parallel())
			for (const auto &symbol : ntdll->second->exports) {
				if (!symbol.address || symbol.names.empty() || !symbol.forwarder.empty())
					continue;
				const uint32_t number = nt_service_number("ntdll.dll", symbol.names.at(0));
				const std::string service = number ? "Nt" + symbol.names.at(0).substr(2) : std::string();
				auto image = ntdll->second->loaded.data();
				const auto stub = uint32_t(symbol.address + service_stub);
				if (!number || changes_context(service) || entered.contains(number) || !stub_at(image, stub))
					continue;
				std::array<uint8_t, 12> previous{};
				std::copy_n(image.data() + stub, previous.size(), previous.begin());
				served_pointers.entries.emplace_back(image.data() + stub, previous);
				std::copy(served_stub::local_block().begin(), served_stub::local_block().end(), image.data() + stub);
				const uint64_t address = ntdll->second->loaded.base() + stub;
				entered.emplace(number, address);
				called_stubs.push_back(address);
				if (cycles_named)
					stub_names.emplace(address, service);
				if (beside_api("ntdll.dll", service))
					beside_names.emplace(address, service);
			}
		timed_service_return = map_service_page(process, entered);
	}
	if (cycles_named) {
		stub_names.insert(served_names.begin(), served_names.end());
		CallCycles::instance().set_names(std::move(stub_names));
	}
	std::vector<uint64_t> served_stubs, yield_traps, queued_stubs;
	for (const auto &[stub, name] : served_names) {
		served_stubs.push_back(stub);
		yield_traps.push_back(stub + served_yield);
		if (process.parallel() && std::getenv("RAVEN_QUEUED_GRAPHICS") &&
			(queued_arguments("vulkan-1.dll", name) || queued_arguments("opengl32.dll", name)))
			queued_stubs.push_back(stub);
	}
	// A thread inside a commit prelude must not lose the processor between taking a log slot and filling it: the prelude counts as a
	// served stub for the scheduler, in two pieces as it is longer than one.
	for (const auto &[module_key, module] : modules.all())
		if (module->runtime_api) {
			const std::string_view module_name = module_key.starts_with("system32-") ? std::string_view(module_key).substr(9) : std::string_view(module_key);
			for (const auto &symbol : module->exports)
				if (symbol.address && !symbol.names.empty() && symbol.forwarder.empty() && guest_commit_api(module_name, symbol.names.at(0)) &&
					served_stub_rva(module->loaded.data(), module_name, symbol.names.at(0), symbol.address) != symbol.address)
					for (const uint64_t piece : {uint64_t(0), uint64_t(served_trap)})
						served_stubs.push_back(module->loaded.base() + symbol.address + piece);
		}
	// So must one between taking the screen context and reading which it is, or reading a window's or a region's record.
	for (const auto &[module_key, module] : modules.all())
		if (module->runtime_api) {
			const std::string_view module_name = module_key.starts_with("system32-") ? std::string_view(module_key).substr(9) : std::string_view(module_key);
			for (const auto &symbol : module->exports)
				if (symbol.address && !symbol.names.empty() && symbol.forwarder.empty() && (module_name == "user32.dll" || module_name == "ntdll.dll" ||
						(module_name == "gdi32.dll" && (symbol.names.at(0) == "RectInRegion" ||
						symbol.names.at(0) == "PtInRegion" || symbol.names.at(0) == "GetRgnBox"))))
					if (const auto *prelude = block_prelude(module_name, symbol.names.at(0)); prelude && prelude != &screen_release_dc_prelude() &&
						served_stub_rva(module->loaded.data(), module_name, symbol.names.at(0), symbol.address) != symbol.address)
						for (uint64_t piece = 0; piece < prelude->size(); piece += served_trap)
							served_stubs.push_back(module->loaded.base() + symbol.address + piece);
		}
	memory.set_commit_block(served_block);
	windows.set_screen_block(served_block);
	static_assert(focus_published == NativeWindows::focus_published && focus_window == NativeWindows::focus_window &&
		foreground_window == NativeWindows::foreground_window && cursor_position == NativeWindows::cursor_position,
		"the focus, foreground and pointer preludes read what NativeWindows publishes");
	// The window table the window preludes read, mapped into the guest and published in the served block.
	const size_t windows_size = NativeWindows::window_records * NativeWindows::window_record, table_size = windows_size + 8192 +
		NativeWindows::menu_area + NativeWindows::bitmap_records * NativeWindows::bitmap_record + 4096 + NativeWindows::dc_table_size + 4096 +
		NativeWindows::icon_slots * 8 + NativeWindows::region_records * NativeWindows::region_record + 4096 + 4096 + 4096;
	const int table_file = memfd_create("raven-window-table", MFD_CLOEXEC);
	if (table_file < 0 || ftruncate(table_file, off_t(table_size)) != 0)
		throw std::runtime_error("cannot create the window table");
	auto *table = static_cast<uint8_t *>(mmap(nullptr, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, table_file, 0));
	if (table == MAP_FAILED)
		throw std::runtime_error("cannot map the window table");
	const uint64_t table_guest = process.map_shared(table_file, table_size);
	close(table_file);
	windows.set_window_table(table);
	// Every change to the desktop is made under its lock, and the guest's drawing log is the one thing written beside it: with neither
	// since the last publish there is nothing to write, and a thread that only draws with Vulkan does not take the lock at all.
	process.set_resume_hook([&windows, published = std::make_shared<std::atomic<uint64_t>>(0)] {
		if (desktop_lock().entries() == published->load(std::memory_order_acquire) && !windows.drawing_logged())
			return;
		const DesktopScope desktop_scope;
		published->store(desktop_lock().entries(), std::memory_order_release);
		windows.publish_windows();
	});
	__atomic_store_n(&served_block[window_table / 8], table_guest, __ATOMIC_RELEASE);
	// The locale table follows the window table.
	const auto locales = locale_table();
	std::memcpy(table + windows_size, locales.data(), locales.size());
	__atomic_store_n(&served_block[locale_pointer / 8], table_guest + windows_size, __ATOMIC_RELEASE);
	const auto named_locales = locale_table(true);
	const size_t named_at = (locales.size() + 7) & ~size_t(7);
	if (named_at + named_locales.size() > 8192)
		throw std::runtime_error("the locale tables do not fit their page");
	std::memcpy(table + windows_size + named_at, named_locales.data(), named_locales.size());
	__atomic_store_n(&served_block[locale_names_pointer / 8], table_guest + windows_size + named_at, __ATOMIC_RELEASE);
	// The menu table follows the locale table.
	windows.set_menu_table(table + windows_size + 8192);
	__atomic_store_n(&served_block[menu_pointer / 8], table_guest + windows_size + 8192, __ATOMIC_RELEASE);
	// The bitmap table follows the menu table.
	const size_t bitmaps_at = windows_size + 8192 + NativeWindows::menu_area;
	windows.set_bitmap_table(table + bitmaps_at);
	__atomic_store_n(&served_block[bitmap_pointer / 8], table_guest + bitmaps_at, __ATOMIC_RELEASE);
	// The system metrics and colors follow the bitmap table.
	const size_t system_at = bitmaps_at + NativeWindows::bitmap_records * NativeWindows::bitmap_record;
	windows.set_system_table(table + system_at);
	__atomic_store_n(&served_block[system_pointer / 8], table_guest + system_at, __ATOMIC_RELEASE);
	// The device context attributes follow the system table.
	const size_t dcs_at = system_at + 4096;
	windows.set_dc_table(table + dcs_at);
	__atomic_store_n(&served_block[dc_pointer / 8], table_guest + dcs_at, __ATOMIC_RELEASE);
	// The deferred release log and the icon set follow the attribute table.
	const size_t deferred_at = dcs_at + NativeWindows::dc_table_size, icons_at = deferred_at + 4096;
	windows.set_deferred_tables(table + deferred_at, table + icons_at);
	__atomic_store_n(&served_block[deferred_pointer / 8], table_guest + deferred_at, __ATOMIC_RELEASE);
	__atomic_store_n(&served_block[icons_pointer / 8], table_guest + icons_at, __ATOMIC_RELEASE);
	// The region table follows the icon set.
	const size_t regions_at = icons_at + NativeWindows::icon_slots * 8;
	windows.set_region_table(table + regions_at);
	__atomic_store_n(&served_block[region_pointer / 8], table_guest + regions_at, __ATOMIC_RELEASE);
	// The quiet table follows the region table, in a page of its own that keeps the table a whole number of pages.
	const size_t quiet_at = regions_at + NativeWindows::region_records * NativeWindows::region_record;
	windows.set_quiet_table(table + quiet_at);
	__atomic_store_n(&served_block[quiet_pointer / 8], table_guest + quiet_at, __ATOMIC_RELEASE);
	// The contexts in effect on each thread follow, in a page of their own.
	const size_t current_at = quiet_at + 4096;
	activation_contexts().set_current_table(table + current_at);
	__atomic_store_n(&served_block[current_context_pointer / 8], table_guest + current_at, __ATOMIC_RELEASE);
	// The key states follow, in a page of their own.
	const size_t keys_at = current_at + 4096;
	windows.set_key_table(table + keys_at);
	__atomic_store_n(&served_block[key_pointer / 8], table_guest + keys_at, __ATOMIC_RELEASE);
	process.set_call_stubs(std::move(called_stubs));
	// Reading an event's state needs nothing of the runtime either, and ntdll's export for it is a system call: that is answered beside
	// the turn where it stopped. One made from anywhere else goes the usual way, which checks the instruction.
	if (const auto ntdll = modules.all().find("ntdll.dll"); process.parallel() && ntdll != modules.all().end() && ntdll->second->runtime_api) {
		const uint32_t number = nt_service_number("ntdll.dll", "NtQueryEvent");
		const auto after_syscall = uint64_t(raven_nt_syscall_return - raven_nt_syscall);
		std::vector<uint64_t> returns;
		for (const auto &symbol : ntdll->second->exports)
			if (symbol.address && symbol.forwarder.empty())
				for (const auto &name : symbol.names)
					if (nt_service_number("ntdll.dll", name) == number)
						returns.push_back(ntdll->second->loaded.base() + symbol.address + after_syscall);
		// With RAVEN_CYCLES the exports make the system call in the timed routine.
		if (!returns.empty() && timed_service_return)
			returns.push_back(timed_service_return);
		if (!returns.empty()) {
			CallCycles::instance().name_service(number, "NtQueryEvent");
			process.set_concurrent_service([&process, &kernel_objects, number, returns](uint64_t after, uint64_t called,
				const uint64_t *arguments) -> std::optional<uint64_t> {
				if (called != number || std::find(returns.begin(), returns.end(), after) == returns.end())
					return std::nullopt;
				return NativeObjectAPI::query_alone(process, kernel_objects, arguments);
			});
		}
	}
	// Setting or resetting an event of the table needs nothing of the runtime: the host thread answers it outside the turn, and threads
	// that set events of their own go side by side. On one processor alone a set an outside waiter awaits takes the turn to hand it over. The threads that wait in the runtime, and one that began to wait
	// meanwhile, are looked at again as after any other change. A wait for an event of the table that is set is taken there beside the
	// others too; one that would block, or report an error, goes the usual way.
	enum class EventCall { reset, set, wait, wait_ex };
	std::vector<std::pair<uint64_t, EventCall>> event_stubs;
	for (const auto &[stub, name] : served_names)
		if (name == "SetEvent" || name == "ResetEvent" || name == "WaitForSingleObject" || name == "WaitForSingleObjectEx")
			event_stubs.emplace_back(stub, name == "SetEvent" ? EventCall::set : name == "ResetEvent" ? EventCall::reset :
				name == "WaitForSingleObject" ? EventCall::wait : EventCall::wait_ex);
	// Waking the threads that wait on an address goes beside the others as well, the woken threads looked at again as for an event.
	std::vector<std::pair<uint64_t, bool>> address_stubs;
	for (const auto &[stub, name] : served_names)
		if (name == "WakeByAddressSingle" || name == "WakeByAddressAll" || name == "RtlWakeAddressSingle" || name == "RtlWakeAddressAll")
			address_stubs.emplace_back(stub, name.ends_with("All"));
	// Releasing a semaphore the table keeps goes beside the others too, when no previous count is asked for: the table has a lock of its
	// own, and a release it refuses changed nothing, so the usual way answers it with the error.
	std::vector<uint64_t> semaphore_stubs;
	for (const auto &[stub, name] : served_names)
		if (name == "ReleaseSemaphore")
			semaphore_stubs.push_back(stub);
	// And releasing a mutex of the table by the thread that owns it; one it does not own goes the usual way, which fails it.
	std::vector<uint64_t> mutex_stubs;
	std::vector<std::pair<uint64_t, bool>> multiple_stubs;
	for (const auto &[stub, name] : served_names) {
		if (name == "ReleaseMutex")
			mutex_stubs.push_back(stub);
		// A wait for several objects of the table, not alertable, goes beside the turn as one for a single object does.
		if (name == "WaitForMultipleObjects" || name == "WaitForMultipleObjectsEx")
			multiple_stubs.emplace_back(stub, name == "WaitForMultipleObjectsEx");
	}
	// Vulkan's recording calls beside the turn: the bridge's handler runs on the calling thread's own state, sharing the bridge's tables
	// with the other threads that record (see VulkanBridge::dispatch_beside). A call that fails to decode goes the usual way, which
	// reports it; the driver has not been called then.
	std::function<std::optional<uint64_t>(const std::string &, const uint64_t *)> graphics_beside;
#ifdef RAVEN_VULKAN
	graphics_beside = [&process, &vulkan](const std::string &name, const uint64_t *arguments) -> std::optional<uint64_t> {
		bool known = false;
		try {
			const auto result = process.serve_beside({arguments[0], arguments[1], arguments[2], arguments[3]},
				{arguments[5], arguments[6], arguments[7], arguments[8]}, arguments[4] + 40, [&] { known = vulkan.dispatch_beside(name); });
			return known ? std::optional<uint64_t>(result) : std::nullopt;
		} catch (...) {
			return std::nullopt;
		}
	};
#endif
	if (process.parallel() && (!event_stubs.empty() || !address_stubs.empty() || !semaphore_stubs.empty() || !mutex_stubs.empty() ||
		!beside_names.empty()))
		process.set_concurrent_served([&process, &environment, &api_registry, &threads, &kernel_objects, &kernel, &memory, event_stubs, address_stubs,
			semaphore_stubs, mutex_stubs, multiple_stubs, beside_names, &windows, &desktop_objects, &desktop_api, graphics_beside](uint64_t stub,
			const uint64_t *arguments) -> std::optional<uint64_t> {
			// A call of a subsystem with a lock of its own runs its usual handler here; the arguments past the four in registers are on the
			// guest's stack above their home space.
			if (const auto named = beside_names.find(stub); named != beside_names.end()) {
				// The page record of an address, written where the guest asked; any other case (a short buffer, an address the accounting
				// does not answer for, a buffer it cannot write) goes the usual way, which reports the error.
				if (named->second == "VirtualAlloc" || named->second == "VirtualFree" || named->second == "VirtualProtect")
					return memory.serve_alone(named->second, arguments);
				if (named->second == "VirtualQuery") {
					if (arguments[2] < 48)
						return std::nullopt;
					try {
						const auto info = memory.query(arguments[0]);
						if (!info || !process.write_alone(arguments[1], info->data(), info->size()))
							return std::nullopt;
					} catch (...) {
						return std::nullopt;
					}
					return 48;
				}
				if (named->second == "NtQueryEvent")
					return NativeObjectAPI::query_alone(process, kernel_objects, arguments);
				if (named->second.starts_with("vk"))
					return graphics_beside ? graphics_beside(named->second, arguments) : std::nullopt;
				// The window manager's handlers are the desktop's registry; the others are the runtime's.
				const bool desktop_call = desktop_beside_api(named->second) || gdi_beside_api(named->second);
				const auto entry = (desktop_call ? desktop_api : api_registry).find(named->second);
				if (!entry)
					return std::nullopt;
				const uint32_t count = entry->first;
				std::vector<uint64_t> words(arguments, arguments + 4);
				if (count > 4) {
					words.resize(count);
					if (!process.read_alone(arguments[4] + 40, words.data() + 4, (count - 4) * sizeof(uint64_t)))
						return std::nullopt;
				}
				words.resize(count);
				// A change of the desktop's state holds its lock, and what it changed is published as the call ends, for the guest's
				// own answers to read.
				if (desktop_call && named->second.starts_with("SetWindowLong")) {
					const int32_t index = int32_t(uint32_t(words[1]));
					if (index < 0 && index != -21 && index != -12)
						return std::nullopt;
				}
				// Drawing that changes nothing but the pixels of memory contexts, and calls on one window of the process's, run under the
				// shared desktop lock with the stripes of what they work on (see drawing_objects and window_call).
				if (desktop_call) {
					const DesktopSharedScope shared;
					const bool logged = windows.dc_logged() || windows.drawing_logged();
					auto handles = logged ? std::nullopt :
						drawing_objects(named->second, words, std::as_const(desktop_objects), windows.published_screen_dc());
					const auto window = logged || handles ? std::nullopt : window_call(named->second, words, std::as_const(desktop_objects));
					if (window)
						handles = std::vector<uint64_t>{window->window, window->context};
					if (handles) {
						const DesktopStripes stripes(*handles);
						ApiCall call(process, environment, std::move(words));
						call.alone = true;
						try {
							entry->second(call);
						} catch (...) {
							return std::nullopt;
						}
						// The guest answers reads of the window's data and of its contexts from their records, which show the change before the
						// call returns.
						if (window) {
							windows.publish_window_alone(window->window);
							if (window->context || (window->takes_context && call.result))
								windows.publish_dc_alone(window->context ? window->context : call.result);
						}
						return call.result;
					}
				}
				std::optional<DesktopScope> desktop_scope;
				std::optional<WindowCallScope> window_scope;
				if (desktop_call) {
					desktop_scope.emplace();
					// The contexts' state the guest changed by itself comes in first, as in the turn; deferred releases wait for a stop
					// of the guest, as for a call answered through its stub.
					windows.apply_dc_log();
					windows.screen_dc_use(named->second, words.empty() ? 0 : words[0]);
					window_scope.emplace(windows);
				}
				ApiCall call(process, environment, std::move(words));
				call.alone = true;
				try {
					entry->second(call);
				} catch (...) {
					return std::nullopt;
				}
				if (desktop_scope)
					windows.finish_call();
				return call.result;
			}
			// The object of the process's table behind a handle the thread may wait on: an event, a semaphore or a mutex. 0 for any
			// other, which the usual way waits for.
			const auto table_of = [&](uint64_t handle) -> uint32_t {
				if (const auto event = kernel_objects.event_alone(handle, 0x100000))
					return event->table_object();
				const auto other = kernel_objects.object_alone(handle, 0x100000);
				return !other ? 0 : other->table_semaphore() ? other->table_semaphore() : other->table_mutex();
			};
			// Takes what is signaled now, or waits beside the turn for one that would block; nothing for the usual way.
			const auto wait_table = [&](std::span<const uint32_t> objects, bool all, uint32_t milliseconds) -> std::optional<uint64_t> {
				const uint32_t thread = NativeThreads::own_id();
				if (!thread)
					return std::nullopt;
				const uint32_t count = uint32_t(objects.size());
				const uint32_t taken = raven_waits_wait(kernel_waits(), thread, objects.data(), count, all, 0, 0);
				// An abandoned mutex is taken too, and the wait says so.
				if (taken < count || (taken >= 0x80 && taken < 0x80 + count))
					return taken;
				if (taken != 0x102 || !milliseconds)
					return std::nullopt;
				return threads.wait_beside(objects, all, milliseconds);
			};
			// Objects other processes share (named events, mutexes and semaphores) keep their state in words the processes map: a wait
			// for those alone sleeps on the words beside the turn too, as the usual way does once it has the turn.
			const auto wait_shared = [&](std::span<const uint64_t> handles, bool all, uint32_t milliseconds) -> std::optional<uint64_t> {
				std::vector<std::shared_ptr<KernelObject>> held;
				std::vector<std::atomic<uint32_t> *> words;
				for (const uint64_t handle : handles) {
					std::shared_ptr<KernelObject> object = kernel_objects.event_alone(handle, 0x100000);
					if (!object)
						object = kernel_objects.object_alone(handle, 0x100000);
					auto *word = object ? object->wait_word() : nullptr;
					if (!word)
						return std::nullopt;
					held.push_back(std::move(object));
					words.push_back(word);
				}
				return threads.wait_shared_beside(held, words, all, milliseconds);
			};
			if (const auto multiple = std::find_if(multiple_stubs.begin(), multiple_stubs.end(), [stub](const auto &entry) { return entry.first == stub; });
				multiple != multiple_stubs.end()) {
				const auto count = uint32_t(arguments[0]);
				if (!count || count > 64)
					return std::nullopt;
				// The fifth argument of the Ex form, alertable, is on the stack above the four the registers carry and their home space.
				uint32_t alertable = 0;
				if (multiple->second && (!process.read_alone(arguments[4] + 40, &alertable, sizeof alertable) || alertable))
					return std::nullopt;
				uint64_t handles[64];
				if (!process.read_alone(arguments[1], handles, count * sizeof(uint64_t)))
					return std::nullopt;
				uint32_t objects[64];
				for (uint32_t index = 0; index < count; ++index)
					if (!(objects[index] = table_of(handles[index])))
						return wait_shared(std::span(handles, count), uint32_t(arguments[2]) != 0, uint32_t(arguments[3]));
				return wait_table(std::span(objects, count), uint32_t(arguments[2]) != 0, uint32_t(arguments[3]));
			}
			if (std::find(mutex_stubs.begin(), mutex_stubs.end(), stub) != mutex_stubs.end()) {
				const auto mutex = kernel_objects.object_alone(arguments[0], 0);
				const uint32_t object = mutex ? mutex->table_mutex() : 0;
				const uint32_t thread = NativeThreads::own_id();
				if (!object || !thread || raven_waits_release_mutex(kernel_waits(), object, thread, nullptr))
					return std::nullopt;
				threads.signaled();
				if (threads.waiting_in_runtime())
					threads.nudge();
				return 1;
			}
			if (std::find(semaphore_stubs.begin(), semaphore_stubs.end(), stub) != semaphore_stubs.end()) {
				const auto semaphore = kernel_objects.object_alone(arguments[0], 0x2);
				const uint32_t object = semaphore ? semaphore->table_semaphore() : 0;
				const auto amount = int32_t(arguments[1]);
				uint32_t previous = 0;
				if (!object || amount <= 0 || arguments[2] || raven_waits_release_semaphore(kernel_waits(), object, uint32_t(amount), &previous))
					return std::nullopt;
				threads.signaled();
				if (threads.waiting_in_runtime())
					threads.nudge();
				return 1;
			}
			if (const auto address = std::find_if(address_stubs.begin(), address_stubs.end(), [stub](const auto &entry) { return entry.first == stub; });
				address != address_stubs.end()) {
				if (!kernel.wake_address_beside)
					return std::nullopt;
				kernel.wake_address_beside(arguments[0], address->second);
				return 0;
			}
			const auto found = std::find_if(event_stubs.begin(), event_stubs.end(), [stub](const auto &entry) { return entry.first == stub; });
			if (found == event_stubs.end())
				return std::nullopt;
			if (found->second == EventCall::wait || found->second == EventCall::wait_ex) {
				// An event, a semaphore with a count or a mutex is taken here; one that would block is waited for here as well, by a host
				// thread whose guest spins in its stub.
				if (found->second == EventCall::wait_ex && uint32_t(arguments[2]))
					return std::nullopt;
				const uint32_t object = table_of(arguments[0]);
				if (!object)
					return wait_shared(std::span(&arguments[0], 1), false, uint32_t(arguments[1]));
				return wait_table(std::span(&object, 1), false, uint32_t(arguments[1]));
			}
			const auto event = kernel_objects.event_alone(arguments[0]);
			if (!event)
				return std::nullopt;
			// An event outside the table (one shared with other processes) has its waiters on its word or in the runtime. One a thread
			// waits for outside the runtime is set here too unless the process has one processor, where the turn hands over to the thread.
			const uint32_t object = event->table_object();
			if ((object && NativeThreads::one_processor() && !threads.quiet_for(object)) || event->set_state(found->second == EventCall::set) < 0)
				return std::nullopt;
			threads.signaled();
			if (threads.waiting_in_runtime() || (object && !threads.quiet_for(object)))
				threads.nudge();
			return 1;
		});
	process.set_served_calls(served_block, std::move(served_stubs), served_trap, served_wait, std::move(yield_traps), [&, served_names = std::move(served_names)](uint64_t stub) {
		const auto found = served_names.find(stub);
		if (found == served_names.end())
			throw std::runtime_error("a served call came from an unknown stub");
		const ServedThreadScope thread_scope(environment, process.serving_teb());
		api_in_progress = found->second.c_str();
		// A Vulkan call leaves the desktop alone, as in the trap path.
		const bool graphics_call = found->second.starts_with("vk"), kernel_call = api_registry.kernel(found->second);
		std::optional<DesktopScope> desktop_scope;
		std::optional<WindowCallScope> window_scope;
		if (!graphics_call && !kernel_call) {
			desktop_scope.emplace();
			// Deferred releases wait for a stop of the guest: releasing a DIB section unmaps guest memory, which a running guest cannot
			// have done to it.
			windows.apply_dc_log();
			windows.screen_dc_use(found->second);
			window_scope.emplace(windows);
		}
		const auto profile_start = RuntimeProfile::instance().enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		const bool dispatched = api_registry.dispatch(process, environment, found->second)
#ifdef RAVEN_VULKAN
			|| vulkan.dispatch(found->second)
#endif
			;
		if (!dispatched)
			throw std::runtime_error("served call " + found->second + " has no handler");
		if (RuntimeProfile::instance().enabled() && profile_start != std::chrono::steady_clock::time_point{})
			RuntimeProfile::instance().api(found->second, std::chrono::steady_clock::now() - profile_start);
	}, std::move(queued_stubs));
	// The guest reads the performance counter itself from its first call (see ClockCalibration).
	const auto calibration = std::make_shared<ClockCalibration>(clock_origin());
	threads.add_service([served_block, calibration] {
		if (!calibration->done())
			calibration->publish(served_block, clock_sample());
	});
	// The local second the guest's GetLocalTime reads is published again once it is over: the end goes first and last, so a guest
	// that copies while it changes sees the end move and asks the runtime.
	threads.add_service([served_block] {
		uint64_t *published = served_block + local_second_at / 8;
		if (const uint64_t end = __atomic_load_n(&published[1], __ATOMIC_ACQUIRE); end && counter_ticks() < end)
			return;
		const auto second = local_second();
		__atomic_store_n(&published[1], uint64_t(0), __ATOMIC_RELEASE);
		published[0] = second[0];
		published[2] = second[2];
		published[3] = second[3];
		__atomic_store_n(&published[1], second[1], __ATOMIC_RELEASE);
	});
	// RAVEN_SAMPLE_GUEST=1 reports every five seconds where the guest spent its running time, by module and 256-byte block.
	if (std::getenv("RAVEN_SAMPLE_GUEST")) {
		auto samples = std::make_shared<std::map<uint64_t, uint32_t>>();
		auto reported = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
		process.set_guest_sampler(std::chrono::milliseconds(2), [&modules, samples, reported](uint64_t rip) {
			++(*samples)[rip & ~uint64_t(255)];
			if (std::chrono::steady_clock::now() - *reported < std::chrono::seconds(5))
				return;
			*reported = std::chrono::steady_clock::now();
			std::vector<std::pair<uint32_t, uint64_t>> top;
			uint32_t total = 0;
			for (const auto &[where, count] : *samples) {
				top.emplace_back(count, where);
				total += count;
			}
			std::sort(top.rbegin(), top.rend());
			std::cerr << "samples: " << total << " in the last period\n";
			for (size_t index = 0; index < std::min<size_t>(top.size(), 15); ++index) {
				std::string place = "?";
				for (const auto &[module_name, module] : modules.all())
					if (top[index].second >= module->loaded.base() && top[index].second < module->loaded.base() + module->image.image_size) {
						std::ostringstream text;
						text << module_name << '+' << std::hex << top[index].second - module->loaded.base();
						place = text.str();
					}
				std::cerr << "  " << top[index].first << ' ' << place << '\n';
			}
			// Then the modules, which show where the time goes when no single block stands out.
			std::map<std::string_view, uint32_t> by_module;
			for (const auto &[where, count] : *samples) {
				std::string_view name = "?";
				for (const auto &[module_name, module] : modules.all())
					if (where >= module->loaded.base() && where < module->loaded.base() + module->image.image_size)
						name = module_name;
				by_module[name] += count;
			}
			std::vector<std::pair<uint32_t, std::string_view>> ranked;
			for (const auto &[name, count] : by_module)
				ranked.emplace_back(count, name);
			std::sort(ranked.rbegin(), ranked.rend());
			for (size_t index = 0; index < std::min<size_t>(ranked.size(), 10); ++index)
				std::cerr << "  module " << ranked[index].first << ' ' << ranked[index].second << '\n';
			samples->clear();
		});
	}
	kernel_ptr=&kernel;
	NativeUnwinder unwinder(process, modules);
	register_exception_api(api_registry, kernel, unwinder);
	register_system_api(api_registry, kernel);
	register_process_api(api_registry, kernel);
	register_security_api(api_registry, kernel);
	register_child_process_api(api_registry, kernel);
	register_winsock_api(api_registry, kernel);
#ifdef RAVEN_OPENSSL
	register_wininet_api(api_registry, kernel);
#endif
	register_host_api(api_registry, kernel);
	register_pipe_api(api_registry, kernel);
	register_job_api(api_registry, kernel);
	register_wave_api(api_registry, kernel);
	register_mmio_api(api_registry, kernel);
	register_pulse_event(api_registry, kernel);
	register_crypt32_api(api_registry, kernel);
	register_bcrypt_api(api_registry, kernel);
	register_process_info_api(api_registry, kernel);
	register_font_service(api_registry, kernel);
	register_sandbox_api(api_registry, kernel);
	register_shader_service(api_registry, kernel);
	register_audio_service(api_registry, kernel);
	register_canvas_service(api_registry, kernel);
	api_registry.set_kernel(true);
	register_sync_api(api_registry, kernel);
	register_thread_api(api_registry, kernel);
	NativeObjectAPI::register_served(api_registry, kernel.objects, kernel.threads);
	api_registry.set_kernel(false);
	register_text_api(api_registry, kernel);
	register_console_api(api_registry, kernel);
	register_memory_api(api_registry, kernel);
	register_file_api(api_registry, kernel);
	register_shlwapi_api(api_registry, kernel);
	register_platform_api(api_registry, kernel);
	register_actctx_api(api_registry, kernel);
	register_version_api(api_registry, kernel);
#ifdef RAVEN_OPENGL
	if (opengl) opengl->register_api(api_registry, threads);
#endif
	windows.register_api(desktop_api);
	desktop.register_gdi_api(desktop_api);
	desktop.register_text_api(desktop_api);
	desktop.register_image_api(desktop_api);
	apply_registry_settings(hive);
	__atomic_store_n(&served_block[theme_state / 8], classic_theme() ? 1 : 2, __ATOMIC_RELEASE);
	paths.set_package_roots([&hive] { return virtual_package_roots(hive); });
	register_registry_api(api_registry, kernel, hive);
	register_service_api(api_registry, kernel, hive);
	register_shell_api(api_registry, kernel, hive);
	NativeProfile profile(process, environment, prefix);
	register_profile_api(api_registry, profile);
	register_ansi_adapters(api_registry);
	register_legacy_file_api(api_registry);
	std::vector<const PEModule *> attached;
	attached.reserve(order.size());
	const auto notify_tls = [&](const PEModule &module, uint64_t reason) {
		const uint64_t arguments[] = {module.loaded.base(), reason, 0};
		for (const auto address : tls.callbacks(module)) {
			const auto result = invoke(address, arguments);
			if (result.stop != NativeStop::returned)
				return result;
		}
		return NativeResult{NativeStop::returned, 0, 0, 0};
	};
	std::set<const PEModule *> detaching;
	const auto detach = [&](const PEModule &module, uint64_t reserved) {
		detaching.insert(&module);
		const auto tls_result = notify_tls(module, 0);
		if (tls_result.stop != NativeStop::returned)
			return tls_result;
		const uint64_t arguments[] = {module.loaded.base(), 0, reserved};
		if ((module.image.characteristics & 0x2000) && module.image.entry && !module.image.il_only()) {
			const auto result = invoke(module.loaded.base() + module.image.entry, arguments);
			if (result.stop != NativeStop::returned)
				return result;
		}
		detaching.erase(&module);
		return NativeResult{NativeStop::returned, 0, 0, 0};
	};
	std::set<const PEModule *> pinned;
	size_t data_serial = 0;
	std::set<const PEModule *> apart_modules;
	std::map<const PEModule *, uint64_t> references;
	for (const auto &[name, module] : modules.all()) {
		pinned.insert(module.get());
		references.emplace(module.get(), 0);
	}
	bool loading = false;
	// The thread that holds the loader: another thread that loads or frees a library meanwhile waits for it, as it would for the
	// loader lock.
	uint32_t loading_thread = 0;
	const auto await_loader = [&] {
		if (!loading || loading_thread == threads.current_id())
			return;
		HostWait waiting;
		threads.block([&] { return !loading; }, std::nullopt);
	};
	struct LoadFailure { uint32_t error; };
	const auto checked = [](NativeResult result) {
		if (result.stop != NativeStop::returned)
			throw result;
	};
	// Libraries loaded by explicit path keep their host file under the module identity (lower-case file name).
	std::map<std::string, std::filesystem::path> explicit_files;
	kernel.reference_module = [&](uint64_t base, bool pin) {
		const auto found = std::find_if(references.begin(), references.end(), [&](const auto &item) { return item.first->loaded.base() == base; });
		if (found == references.end())
			return;
		if (pin)
			pinned.insert(found->first);
		else if (found->second != UINT64_MAX)
			++found->second;
	};
	// A library named by its path is in the process when the module of that file name came from that file. The libraries Raven
	// serves itself live in the system folder, where GetModuleFileName reports them.
	module_api.set_path_lookup([&](const std::string &name) -> uint64_t {
		const auto slash = name.find_last_of("/\\");
		auto identity = name.substr(slash == name.npos ? 0 : slash + 1);
		for (auto &character : identity)
			if (character >= 'A' && character <= 'Z')
				character = static_cast<char>(character + ('a' - 'A'));
		if (identity.find('.') == identity.npos)
			identity += ".dll";
		const auto found = modules.all().find(identity);
		if (found == modules.all().end())
			return 0;
		const auto requested = paths.resolve(from_utf8(name));
		if (!requested)
			return 0;
		std::error_code error;
		const auto explicit_file = explicit_files.find(identity);
		const auto file = explicit_file != explicit_files.end() ? explicit_file->second : found->second->file;
		if (!file.empty())
			return std::filesystem::equivalent(requested->host, file, error) ? found->second->loaded.base() : 0;
		const auto system = paths.resolve(u"C:\\Windows\\System32\\" + from_utf8(identity));
		return system && requested->host == system->host ? found->second->loaded.base() : 0;
	});
	kernel.module_named = [&](uint64_t address, bool wide) { return module_api.module_handle(address, wide); };
	module_api.set_loader([&](std::string name) -> uint64_t {
		await_loader();
		// DONT_RESOLVE_DLL_REFERENCES maps the library alone: its imports are not followed and it is not started, so even an image of
		// managed code alone loads, as the runtime of .NET 2.0 loads its class library. A library loaded as a data file or for its
		// resources is mapped the same way, whatever its width and even when it is a program, and stays out of the loader's list.
		const uint32_t flags = module_api.load_flags();
		const bool as_data = flags & 0x62;
		const bool unresolved = (flags & 1) || as_data;
		if (name.empty()) {
			environment.set_last_error(126);
			return 0;
		}
		for (auto &character : name)
			if (static_cast<unsigned char>(character) < 32) {
				environment.set_last_error(87);
				return 0;
			}
		if (name.find_first_of("/\\:") != name.npos) {
			// A path names its module by file name; Windows system libraries are served natively wherever the path points.
			const auto slash = name.find_last_of("/\\");
			auto identity = name.substr(slash == name.npos ? 0 : slash + 1);
			for (auto &character : identity)
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character + ('a' - 'A'));
			if (identity.find('.') == identity.npos)
				identity += ".dll";
			// A library named by its path in the system directory is looked for as by its name, as Windows finds its own there.
			auto folder = slash == name.npos ? std::string() : name.substr(0, slash);
			for (auto &character : folder) {
				if (character == '/')
					character = '\\';
				else if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character + ('a' - 'A'));
			}
			const bool system_folder = folder == "c:\\windows\\system32" || folder == "c:\\windows\\syswow64";
			if (system_folder && modules.all().contains("system32-" + identity)) {
				name = "system32-" + identity;
			} else {
				if (!system_folder && (program_may_replace(identity) || !modules.all().contains(identity)) && apiset_host(identity).empty()) {
					const auto resolved = paths.resolve(from_utf8(name));
					if (!resolved || !resolved->exists || !std::filesystem::is_regular_file(resolved->host)) {
						environment.set_last_error(126);
						return 0;
					}
					// The file named now is the one to load, even where a library of the same name came from another folder before.
					explicit_files.insert_or_assign(identity, resolved->host);
				}
				name = identity;
			}
		}
		for (auto &character : name) {
			if (static_cast<unsigned char>(character) > 127) {
				environment.set_last_error(120);
				return 0;
			}
			if (character >= 'A' && character <= 'Z')
				character = static_cast<char>(character + ('a' - 'A'));
		}
		if (name.back() == '.')
			name.pop_back();
		else if (name.find('.') == std::string::npos)
			name += ".dll";
		if (const auto host = apiset_host(name); !host.empty())
			name = host;
		if ((flags & 0x1f00) == 0x800 && modules.all().contains("system32-" + name))
			name = "system32-" + name;
		const auto existing = modules.all().find(name);
		// Other threads waited above, so a load already under way is this thread's own: a library starting up loads another, as
		// Windows lets it. A library that is in the process already, even one the outer load has not started yet, is handed back.
		const bool nested = loading;
		if (nested && existing != modules.all().end() && references.contains(existing->second.get())) {
			++references.at(existing->second.get());
			return existing->second->loaded.base();
		}
		if (existing != modules.all().end()) {
			auto &count = references.at(existing->second.get());
			if (count == UINT64_MAX)
				throw std::runtime_error("native DLL reference count overflow");
			++count;
			return existing->second->loaded.base();
		}
		struct Staged { PEModule *module; bool mapped = false; bool listed = false; bool tls = false; bool attached = false; };
		std::vector<Staged> staged;
		bool guest_started = false;
		loading = true;
		loading_thread = threads.current_id();
		const auto rollback = [&] {
			for (auto item = attached.rbegin(); item != attached.rend(); ++item)
				if (std::any_of(staged.begin(), staged.end(), [&](const auto &s) { return s.module == *item && s.attached; }))
					checked(detach(**item, 0));
			std::erase_if(attached, [&](const auto *item) {
				return std::any_of(staged.begin(), staged.end(), [&](const auto &s) { return s.module == item; });
			});
			for (auto item = staged.rbegin(); item != staged.rend(); ++item) {
				if (item->tls)
					tls.remove(*item->module);
				if (item->listed)
					loader_data.remove(*item->module);
				if (item->mapped)
					{ memory.remove_image(item->module->loaded.base()); process.unmap_image(item->module->loaded.base()); }
				references.erase(item->module);
				modules.remove(*item->module);
			}
		};
		const auto acquire = [&](std::string identity) -> PEModule & {
			for (auto &character : identity)
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character + ('a' - 'A'));
			if (identity.find('.') == std::string::npos)
				identity += ".dll";
			// An API set name stands for the library that implements it, as it does for the libraries loaded at startup.
			if (const auto host = apiset_host(identity); !host.empty()) {
				if (const auto present = modules.all().find(host); present != modules.all().end()) {
					modules.add_alias(identity, host);
					return *present->second;
				}
				identity = host;
			}
			if (const auto found = modules.all().find(identity); found != modules.all().end())
				return *found->second;
			if (modules.all().size() >= 10000)
				throw std::runtime_error("native DLL graph exceeds module limit");
			if (!source || identity.find_first_of("/\\:") != std::string::npos) {
				if (std::getenv("RAVEN_TRACE_API"))
					std::cerr << "raven: library not loadable by path: " << identity << '\n';
				throw LoadFailure{126};
			}
			std::optional<ModuleBytes> bytes;
			std::filesystem::path origin;
			if (const auto explicit_file = explicit_files.find(identity); explicit_file != explicit_files.end()) {
				bytes = ModuleBytes::map(explicit_file->second);
				origin = explicit_file->second;
			} else {
				const auto system_names = system_library_names();
				const bool system = std::find(system_names.begin(), system_names.end(), identity) != system_names.end();
				if ((!system || (has_launch && program_may_replace(identity))) && !activation_contexts().library_folder(identity)) {
					if (const auto found = paths.find_library(launch->image_path, from_utf8(identity), true, flags)) {
						bool system_copy = false;
						if (system) {
							auto dos_path = to_utf8(paths.dos(found->host));
							for (auto &character : dos_path)
								if (character >= 'A' && character <= 'Z')
									character = static_cast<char>(character + ('a' - 'A'));
							system_copy = dos_path.starts_with("c:\\windows\\system32\\") || dos_path.starts_with("c:\\windows\\syswow64\\");
						}
						if (!system_copy) {
							bytes = ModuleBytes::map(found->host);
							origin = found->host;
						}
					// A search of the system directory, alone or among the default directories, also finds what the runtime provides as
					// part of it, DirectWrite and Direct2D among them.
					} else if ((flags & 0x1f00) && !(flags & 0x1800) && !system) {
						throw LoadFailure{126};
					}
				}
				// The DirectSound component is what a search of the system directory finds for dsound.dll.
				if (!bytes && (!(system && (flags & 0x1f00)) || identity == "dsound.dll"))
					if (auto read = source(identity)) bytes.emplace(std::move(*read));
				if (!bytes && system) {
					if (const auto fallback = modules.all().find("system32-" + identity); fallback != modules.all().end())
						return *fallback->second;
				}
			}
			// A library loaded by path finds its dependencies in its own directory, as LOAD_WITH_ALTERED_SEARCH_PATH and a DLL directory
			// set to that folder do on Windows; programs that unpack themselves to a temporary folder rely on it.
			// Each folder is looked through once, however many of its libraries were named by path: Office loads dozens from its own.
			std::set<std::filesystem::path> looked;
			for (auto file = explicit_files.begin(); !bytes && file != explicit_files.end(); ++file) {
				std::error_code error;
				if (!looked.insert(file->second.parent_path()).second)
					continue;
				for (const auto &entry : std::filesystem::directory_iterator(file->second.parent_path(), error)) {
					auto candidate = entry.path().filename().string();
					for (auto &character : candidate)
						if (character >= 'A' && character <= 'Z')
							character = static_cast<char>(character + ('a' - 'A'));
					if (candidate != identity || !entry.is_regular_file(error))
						continue;
					bytes = ModuleBytes::map(entry.path());
					origin = entry.path();
					explicit_files.emplace(identity, entry.path());
					break;
				}
			}
			if (!bytes) {
				if (std::getenv("RAVEN_TRACE_API"))
					std::cerr << "raven: library file not found: " << identity << '\n';
				throw LoadFailure{126};
			}
			// A file that is no program image at all, a type library tried as a resource module first, is ERROR_BAD_EXE_FORMAT.
			std::optional<PEImage> parsed;
			try {
				parsed.emplace(*bytes);
			} catch (const std::runtime_error &) {
				throw LoadFailure{193};
			}
			const bool only_mapped = unresolved && staged.empty();
			// 64-bit Windows loads a library of managed code alone built for any CPU in its 64-bit form: the header is widened as for such
			// a program, and the runtime that asked for it runs its code.
			bool widened = false;
			if (!parsed->is_64 && parsed->il_only() && !only_mapped && !as_data) {
				const std::span<const uint8_t> view = *bytes;
				std::vector<uint8_t> copy(view.begin(), view.end());
				if (widen_il_only_image(copy)) {
					bytes.emplace(std::move(copy));
					parsed.emplace(*bytes);
					widened = true;
				}
			}
			// Reject unsupported initialization before publishing an image.
			const PEImage &image = *parsed;
			// An image of the other width maps only as data, or when it holds managed code alone, as a 64-bit process maps the 32-bit form
			// of a class library for the runtime; either stays apart from the libraries that run.
			const bool apart = only_mapped && (as_data || (!image.is_64 && image.il_only()));
			if (!image.is_64 && !apart)
				throw LoadFailure{193};
			// An image with no entry point maps and binds as a library even when it is not one, as the runtime loads a precompiled
			// assembly built as a program: there is nothing of it to run.
			const bool inert = image.is_64 && (!image.entry || widened);
			if (!(image.characteristics & 0x2000) && !only_mapped && !inert)
				throw LoadFailure{120};
			if ((image.directories[13].address || image.directories[13].size) && !only_mapped)
				for (const auto &descriptor : image.delay_imports())
					if (!(image.file_span(descriptor.descriptor_address, 4)[0] & 1))
						throw LoadFailure{120};
			// Mixed managed and native code starts through its entry point like any library, which hands it to the runtime; an image
			// of managed code alone is the runtime's to map, unless it is only mapped.
			if (image.il_only() && !only_mapped && !inert)
				throw LoadFailure{120};
			staged.reserve(staged.size() + 1);
			// A library read as data takes a name of its own, so that nothing loaded later resolves imports through it.
			const auto available = [&](uint64_t base, uint64_t size) { return process.memory_available(base, size); };
			auto &module = apart ? modules.add_data(identity + "#data" + std::to_string(++data_serial), std::move(*bytes), available) :
				modules.add(identity, std::move(*bytes), available);
			if (apart)
				apart_modules.insert(&module);
			module.file = origin;
			staged.push_back({&module});
			references.emplace(&module, 0);
			return module;
		};
		std::map<const PEModule *, uint64_t> entry_contexts;
		try {
			auto &root = acquire(name);
			if (unresolved && staged.size() == 1 && staged.front().module == &root)
				root.imports.clear();
			// Follow only activated import forwarders, not every optional export. Every missing export is reported before the load fails.
			bool missing = false;
			for (size_t index = 0; index < staged.size(); ++index) {
				// A library's own manifest names the side-by-side assemblies its imports come from, the C runtime of its compiler most often.
				// Its context is the one in effect while it and the libraries it brings in start, which the C runtime checks it was found by.
				const auto own = activation_contexts().module_manifest(staged[index].module->image);
				uint64_t context = own ? activation_contexts().create({*own, {}}) : 0;
				if (const auto inherited = entry_contexts.find(staged[index].module); !context && inherited != entry_contexts.end())
					context = inherited->second;
				if (context)
					entry_contexts[staged[index].module] = context;
				for (const auto &symbol : staged[index].module->imports) {
					if (own)
						if (const auto folder = activation_contexts().assembly_folder(*own, symbol.module)) {
							auto wanted = symbol.module;
							for (auto &character : wanted)
								if (character >= 'A' && character <= 'Z')
									character = static_cast<char>(character + ('a' - 'A'));
							std::error_code error;
							if (!modules.all().contains(wanted) && !explicit_files.contains(wanted))
								for (const auto &entry : std::filesystem::directory_iterator(*folder, error)) {
									auto candidate = entry.path().filename().string();
									for (auto &character : candidate)
										if (character >= 'A' && character <= 'Z')
											character = static_cast<char>(character + ('a' - 'A'));
									if (candidate == wanted && entry.is_regular_file(error)) {
										explicit_files.emplace(wanted, entry.path());
										break;
									}
								}
						}
					auto &dependency = acquire(symbol.module);
					if (context)
						entry_contexts.emplace(&dependency, context);
					for (;;) {
						try {
							modules.resolve(symbol);
							break;
						} catch (const std::runtime_error &error) {
							const std::string_view message = error.what();
							if (message.starts_with("missing PE module: "))
								acquire(std::string(message.substr(19)));
							else if (message.starts_with("missing PE export: ")) {
								if (std::getenv("RAVEN_TRACE_API") || std::getenv("RAVEN_TRACE_FAIL"))
									std::cerr << "raven: cannot load " << name << ": " << staged[index].module->name << " imports " << message << '\n';
								missing = true;
								break;
							}
							else
								throw;
						}
					}
				}
			}
			if (missing)
				throw LoadFailure{127};
			modules.bind_imports();
			const auto dynamic_order = initialization_order(modules, entry);
			for (auto &item : staged) {
				process.map_image(item.module->loaded);
				register_image(memory, *item.module);
				item.mapped = true;
				if (apart_modules.contains(item.module))
					continue;
				loader_data.add(*item.module);
				item.listed = true;
				tls.add(*item.module);
				item.tls = true;
			}
			attached.reserve(attached.size() + staged.size());
			for (const auto *module : dynamic_order) {
				const auto found = std::find_if(staged.begin(), staged.end(), [&](const auto &s) { return s.module == module; });
				if (found == staged.end())
					continue;
				guest_started = true;
				// A library only mapped is never told it is attached, nor later that it is detached.
				if (unresolved && module == &root)
					continue;
				checked(notify_tls(*module, 1));
				// Managed code alone has no native entry to start: its entry is the runtime's.
				if (module->image.entry && !module->image.il_only()) {
					const uint64_t arguments[] = {module->loaded.base(), 1, 0};
					const auto context = entry_contexts.find(module);
					const auto thread = threads.current_id();
					const auto cookie = context == entry_contexts.end() ? 0 : activation_contexts().activate(thread, context->second);
					const auto result = invoke(module->loaded.base() + module->image.entry, arguments);
					if (cookie)
						activation_contexts().deactivate(thread, cookie);
					checked(result);
					if (!static_cast<uint32_t>(result.value)) {
						checked(detach(*module, 0));
						throw LoadFailure{1114};
					}
				}
				found->attached = true;
				attached.push_back(module);
				loader_data.initialized(*module);
			}
			++references.at(&root);
			loading = nested;
			return root.loaded.base();
		} catch (const NativeResult &) {
			// A fault or timeout is terminal; never execute extra guest cleanup.
			throw;
		} catch (const LoadFailure &failure) {
			rollback();
			loading = nested;
			environment.set_last_error(failure.error);
			return 0;
		} catch (...) {
			if (!guest_started)
				rollback();
			loading = nested;
			throw;
		}
	}, [&](uint64_t handle) {
		await_loader();
		const auto found = std::find_if(references.begin(), references.end(), [&](const auto &item) { return item.first->loaded.base() == handle; });
		if (found == references.end() || !found->second) {
			environment.set_last_error(found == references.end() ? 126 : 120);
			return false;
		}
		if (loading) {
			environment.set_last_error(120);
			return false;
		}
		--found->second;
		std::set<const PEModule *> reachable = pinned;
		std::vector<const PEModule *> pending;
		for (const auto &[module, count] : references)
			if (count || pinned.contains(module)) {
				reachable.insert(module);
				pending.push_back(module);
			}
		while (!pending.empty()) {
			const auto *module = pending.back();
			pending.pop_back();
			for (const auto &symbol : module->imports)
				for (const auto *dependency : modules.resolution_path(symbol))
					if (reachable.insert(dependency).second)
						pending.push_back(dependency);
		}
		std::vector<const PEModule *> released;
		loading = true;
		loading_thread = threads.current_id();
		for (auto item = attached.rbegin(); item != attached.rend(); ++item)
			if (!reachable.contains(*item)) {
				checked(detach(**item, 0));
				released.push_back(*item);
			}
		std::erase_if(attached, [&](const auto *item) { return !reachable.contains(item); });
		for (const auto *module : released) {
			tls.remove(*module);
			loader_data.remove(*module);
			memory.remove_image(module->loaded.base());
			process.unmap_image(module->loaded.base());
			references.erase(module);
			modules.remove(*module);
		}
		loading = false;
		return true;
	});
	// Thread notifications reach the executable's TLS callbacks first, then every loaded module in load order (reverse on exit).
	threads.set_notifier([&](uint32_t reason) {
		const auto notify = [&](const PEModule &module) {
			if (const auto stopped = notify_tls(module, reason); stopped.stop != NativeStop::returned)
				throw stopped;
			if ((module.image.characteristics & 0x2000) && module.image.entry && !module.runtime_api && !module.image.il_only() && !module.quiet_threads) {
				const uint64_t arguments[] = {module.loaded.base(), reason, 0};
				const auto result = invoke(module.loaded.base() + module.image.entry, arguments);
				if (result.stop != NativeStop::returned)
					throw result;
			}
		};
		// An entry point may load or free libraries, so the walk goes over a copy and skips a module that is gone by its turn.
		const auto modules_now = attached;
		const auto still_attached = [&](const PEModule *module) { return std::find(attached.begin(), attached.end(), module) != attached.end(); };
		if (reason == 2) {
			notify(entry);
			for (const auto *module : modules_now)
				if (module != &entry && still_attached(module))
					notify(*module);
		} else {
			for (auto module = modules_now.rbegin(); module != modules_now.rend(); ++module)
				if (*module != &entry && still_attached(*module))
					notify(**module);
			notify(entry);
		}
	});
	bool entry_tls_ready=false,cleanup_started=false;
	const auto exit_result = [&](ProcessExit request) {
		threads.stop_time_slicing();
		// The other threads end before the libraries are told the process detaches, so none of them runs on what detaching frees.
		threads.terminate_others(request.code);
		try {
			if (!cleanup_started && !request.terminated) {
				kernel_ptr->shutdown_in_progress=true;
				// Detaching counts as a load of this thread's own: a library that frees or loads another in its detach goes on at once
				// instead of waiting for a load no other thread will finish.
				cleanup_started=true; loading=true; loading_thread=threads.current_id();
				if (entry_tls_ready) {
					const auto stopped=notify_tls(entry,0);
					if (stopped.stop!=NativeStop::returned) { windows.shutdown(); return stopped; }
				}
				for (auto module=attached.rbegin();module!=attached.rend();++module) {
					if (detaching.contains(*module)) continue;
					const auto stopped=detach(**module,environment.peb());
					if (stopped.stop!=NativeStop::returned) { windows.shutdown(); return stopped; }
				}
			}
		} catch (const ProcessExit &repeated) { request=repeated; }
		catch (const NativeResult &stopped) { windows.shutdown(); return stopped; }
		windows.shutdown();
		return NativeResult{NativeStop::exited,request.code,request.instruction,0};
	};
	// A start a warm host forked ahead waits here for its request, before its guest runs: its standard streams and its time limit are the
	// request's.
	if (warm_ready()) {
		deadline = std::chrono::steady_clock::now() + timeout;
		object_api.set_deadline(deadline);
		windows.set_deadline(deadline);
		threads.set_deadline(deadline);
	}
	// Only now, as a start forked ahead may have waited through a suspension for its request.
	calibration->publish(served_block, clock_sample());
	// Declared last, so the first thing done when the process ends.
	struct Ending {
		~Ending()
		{
			if (ends_with_process)
				releasing.store(false, std::memory_order_relaxed);
		}
	} ending;
	try {
		windows.set_call(invoke);
		desktop.set_call(invoke);
		wait_suspended_process(launch->suspended_fd, kernel_objects);
		heap.initialize(invoke);
		for (const auto *module : order) {
			if (module == &entry)
				continue;
			const auto tls_result = notify_tls(*module, 1);
			if (tls_result.stop != NativeStop::returned)
				return tls_result;
			if (!(module->image.characteristics & 0x2000) || !module->image.entry || module->image.il_only()) {
				attached.push_back(module);
				continue;
			}
			const uint64_t arguments[] = {module->loaded.base(), 1, environment.peb()};
			const auto result = invoke(module->loaded.base() + module->image.entry, arguments);
			if (result.stop != NativeStop::returned)
				return result;
			if (static_cast<uint32_t>(result.value) == 0) {
				if (detach(*module, 0).stop != NativeStop::returned)
					throw std::runtime_error("native DLL failure cleanup did not return: " + module->name);
				for (auto previous = attached.rbegin(); previous != attached.rend(); ++previous)
					if (detach(**previous, 0).stop != NativeStop::returned)
						throw std::runtime_error("native DLL rollback did not return: " + (*previous)->name);
				throw std::runtime_error("DLL_PROCESS_ATTACH failed: " + module->name);
			}
			attached.push_back(module);
		}
		const auto tls_result = notify_tls(entry, 1);
		if (tls_result.stop != NativeStop::returned)
			return tls_result;
		entry_tls_ready=true;
		const auto result = invoke(program_start(modules, entry), {});
		if (result.stop != NativeStop::returned)
			return result;
		cleanup_started=true; loading=true;
		kernel_ptr->shutdown_in_progress=true;
		const auto tls_detach = notify_tls(entry, 0);
		if (tls_detach.stop != NativeStop::returned)
			return tls_detach;
		for (auto module = attached.rbegin(); module != attached.rend(); ++module) {
			const auto stopped = detach(**module, environment.peb());
			if (stopped.stop != NativeStop::returned)
				return stopped;
		}
		windows.shutdown();
		return result;
	} catch (const ProcessExit &request) {
		return exit_result(request);
	} catch (const NativeResult &stopped) {
		return stopped;
	}
}

}
