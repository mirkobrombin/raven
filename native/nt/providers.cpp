#include "providers.h"
#include "child_process.h"
#include "handle_channel.h"
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
#ifdef RAVEN_OPENGL
#include "graphics/opengl_bridge.h"
#endif
#ifdef RAVEN_VULKAN
#include "graphics/vulkan_bridge.h"
#endif

#include <unistd.h>
#include <sys/mman.h>
#include <x86intrin.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <array>
#include <stdexcept>
#include <csignal>

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
static constexpr std::array<std::string_view, 49> names{
	"CloseHandle","CreateEventA","CreateEventW","ExitProcess",
	"FindResourceA", "FindResourceExA", "FindResourceExW", "FindResourceW", "FreeLibrary",
	"GetACP", "GetCommandLineA", "GetCommandLineW", "GetCurrentProcessId", "GetCurrentThreadId", "GetFileType", "GetLastError", "GetModuleHandleA",
	"GetModuleHandleW", "GetProcAddress", "GetProcessHeap", "GetStartupInfoA", "GetStartupInfoW", "GetStdHandle", "HeapAlloc", "HeapCreate", "HeapDestroy", "HeapFree", "HeapReAlloc", "HeapSize",
	"LoadLibraryA", "LoadLibraryW", "LoadResource",
	"LockResource", "MultiByteToWideChar", "PathCchCanonicalize", "PathCchCanonicalizeEx", "PathCchCombineEx", "PathCchSkipRoot", "RaiseException", "ResetEvent", "SetEvent", "SetLastError", "SizeofResource",
	"VirtualAlloc", "VirtualFree", "VirtualProtect", "VirtualQuery", "WaitForSingleObject", "WriteFile"
};

std::vector<uint8_t> environment_provider(std::string_view name, uint64_t base)
{
	if (name != "kernel32.dll" && name != "kernelbase.dll")
		throw std::runtime_error("unsupported environment provider module");
	return runtime_provider(name, base, names);
}

namespace {

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
		return name == "QueryPerformanceCounter" || name == "QueryPerformanceFrequency" || name == "WakeConditionVariable" ||
			name == "WakeAllConditionVariable" || name == "ReleaseSemaphore" || name == "CreateSemaphoreExW" ||
			name == "WaitForSingleObject" || name == "WaitForSingleObjectEx" || name == "SetEvent" || name == "ResetEvent" || name == "CloseHandle";
	// Reading a socket's recorded events polls the host sockets and never blocks.
	if (module == "ws2_32.dll")
		return name == "WSAEnumNetworkEvents";
	// Waking marks the sleepers; they run when the scheduler next picks a thread.
	if (module == "ntdll.dll")
		return name == "RtlWakeConditionVariable" || name == "RtlWakeAllConditionVariable";
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

// A served stub: it loads the block address from the provider's own pointer, which stays zero until the runtime maps the block, stores
// RCX, RDX, R8, R9, RSP and the low halves of XMM0 to XMM3, publishes its own address and spins until the runtime clears it. After a
// thousand rounds it stops at a breakpoint instead, which gives the processor to the runtime when the two compete for it.
static constexpr size_t served_stub_size = 128, served_trap = 0x70, served_wait = 85, served_yield = 109;

static std::vector<uint8_t> served_stub(int32_t to_pointer)
{
	std::vector<uint8_t> code{0x4c, 0x8b, 0x15, 0, 0, 0, 0, 0x4d, 0x85, 0xd2, 0x74, uint8_t(served_trap - 12),
		0x49, 0x89, 0x4a, 0x08, 0x49, 0x89, 0x52, 0x10, 0x4d, 0x89, 0x42, 0x18, 0x4d, 0x89, 0x4a, 0x20, 0x49, 0x89, 0x62, 0x28,
		0x66, 0x49, 0x0f, 0x7e, 0x42, 0x30, 0x66, 0x49, 0x0f, 0x7e, 0x4a, 0x38, 0x66, 0x49, 0x0f, 0x7e, 0x52, 0x40, 0x66, 0x49, 0x0f, 0x7e, 0x5a, 0x48,
		0x4c, 0x8d, 0x1d, 0xc1, 0xff, 0xff, 0xff, 0x4d, 0x89, 0x1a,
		// mov r11d, 1000; spin: pause; cmp qword [r10], 0; je done; dec r11d; jnz spin; int3; jmp to the compare; nop
		0x41, 0xbb, 0xe8, 0x03, 0x00, 0x00, 0xf3, 0x90, 0x49, 0x83, 0x3a, 0x00, 0x74, 0x0a, 0x41, 0xff, 0xcb, 0x75, 0xf3, 0xcc, 0xeb, 0xf2,
		0x66, 0x90,
		// done: mov rax, [r10+80]; mov r11, [r10+88]; test r11, r11; je return; cmp r11d, 2; je fallback; int3; return: ret
		// A declined call (2) takes the fallback trap that follows, with the argument registers as the caller left them.
		0x49, 0x8b, 0x42, 0x50, 0x4d, 0x8b, 0x5a, 0x58, 0x4d, 0x85, 0xdb, 0x74, 0x07, 0x41, 0x83, 0xfb, 0x02, 0x74, 0x03, 0xcc, 0xc3};
	const int32_t displacement = to_pointer - 7;
	std::memcpy(code.data() + 3, &displacement, 4);
	code.resize(served_trap + 2);
	code[served_trap] = 0xcc;
	code[served_trap + 1] = 0xc3;
	return code;
}

static unsigned queued_arguments(std::string_view module, std::string_view name)
{
#ifdef RAVEN_OPENGL
	if (module == "opengl32.dll") {
		if (!std::getenv("RAVEN_QUEUED_OPENGL")) return 0;
		// Only context-local state: shared texture and program changes still flush every producer synchronously.
		if (name == "glColor4f" || name == "glColor4ub" || name == "glRotatef" || name == "glDrawElements") return 4;
		if (name == "glVertex3f" || name == "glTranslatef" || name == "glScalef") return 3;
		if (name == "glTexCoord2f" || name == "glBindBuffer") return 2;
		if (name == "glActiveTexture" || name == "glUseProgram" || name == "glEnable" || name == "glDisable") return 1;
		return 0;
	}
#endif
	if (module != "vulkan-1.dll")
		return 0;
	if (name == "vkCmdBindDescriptorSets") return 8;
	if (name == "vkCmdBindVertexBuffers") return 5;
	if (name == "vkCmdDrawIndexed") return 6;
	if (name == "vkCmdDraw" || name == "vkCmdDrawIndirect" || name == "vkCmdDrawIndexedIndirect") return 5;
	if (name == "vkCmdBindIndexBuffer" || name == "vkCmdDispatch") return 4;
	if (name == "vkCmdBindPipeline" || name == "vkCmdSetStencilReference") return 3;
	return 0;
}

static constexpr size_t queued_prelude_size = 384;

static std::vector<uint8_t> queued_prelude(int32_t to_served, unsigned arguments, std::string_view name)
{
	static_assert(ServedQueue::head * 8 == 0x80 && ServedQueue::tail * 8 == 0x88 && ServedQueue::enabled * 8 == 0x90 &&
		ServedQueue::records * 8 == 0x100 && ServedQueue::gl_elements * 8 == 0x98 && ServedQueue::words == 16 && ServedQueue::capacity == 16);
	// Read the thread's ring, or use the synchronous stub when disabled, full or the copied arrays would exceed a record.
	std::vector<uint8_t> code{
		0x66, 0x90, 0x65, 0x4c, 0x8b, 0x14, 0x25, 0xf0, 0x1f, 0, 0, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0, 0, 0, 0,
		0x49, 0x83, 0xba, 0x90, 0, 0, 0, 1, 0x0f, 0x85, 0, 0, 0, 0,
		0x49, 0x8b, 0x82, 0x80, 0, 0, 0, 0x49, 0x89, 0xc3, 0x4d, 0x2b, 0x9a, 0x88, 0, 0, 0,
		0x49, 0x83, 0xfb, 0x10, 0x0f, 0x83, 0, 0, 0, 0,
		0x49, 0x89, 0xc3, 0x41, 0x83, 0xe3, 0x0f, 0x49, 0xc1, 0xe3, 7, 0x4d, 0x01, 0xd3,
		0x49, 0x89, 0x8b, 0x08, 1, 0, 0, 0x49, 0x89, 0x93, 0x10, 1, 0, 0,
		0x4d, 0x89, 0x83, 0x18, 1, 0, 0, 0x4d, 0x89, 0x8b, 0x20, 1, 0, 0};
	std::vector<size_t> fallback{16, 30, 57};
	if (name == "glDrawElements") {
		const uint8_t bound[]{0x49, 0x83, 0xba, 0x98, 0, 0, 0, 1, 0x0f, 0x85, 0, 0, 0, 0};
		fallback.push_back(code.size() + 10);
		code.insert(code.end(), std::begin(bound), std::end(bound));
	} else if (name == "glBindBuffer") {
		// Element-buffer changes publish the capability before another draw can enter the ring.
		const uint8_t elements[]{0x81, 0xf9, 0x93, 0x88, 0, 0, 0x0f, 0x84, 0, 0, 0, 0};
		fallback.push_back(code.size() + 8);
		code.insert(code.end(), std::begin(elements), std::end(elements));
	}
	const bool descriptors = name == "vkCmdBindDescriptorSets", vertices = name == "vkCmdBindVertexBuffers";
	if (descriptors) {
		for (const uint8_t at : {uint8_t(40), uint8_t(56)}) {
			const uint8_t bound[]{0x83, 0x7c, 0x24, at, 4, 0x0f, 0x87, 0, 0, 0, 0};
			fallback.push_back(code.size() + 7);
			code.insert(code.end(), std::begin(bound), std::end(bound));
		}
	} else if (vertices) {
		const uint8_t bound[]{0x41, 0x83, 0xf8, 4, 0x0f, 0x87, 0, 0, 0, 0};
		fallback.push_back(code.size() + 6);
		code.insert(code.end(), std::begin(bound), std::end(bound));
	}
	for (unsigned index = 4; index < arguments; ++index) {
		const uint8_t word[]{0x48, 0x8b, 0x4c, 0x24, uint8_t(40 + (index - 4) * 8),
			0x49, 0x89, 0x8b, uint8_t(8 + index * 8), 1, 0, 0};
		code.insert(code.end(), std::begin(word), std::end(word));
	}
	if (name.starts_with("gl")) {
		// Copy the low XMM words before the caller can reuse them. Vulkan's array payload uses these words instead.
		for (unsigned index = 0; index < 4; ++index) {
			const uint8_t word[]{0x66, 0x49, 0x0f, 0x7e, uint8_t(0x83 + index * 8), uint8_t(0x48 + index * 8), 1, 0, 0};
			code.insert(code.end(), std::begin(word), std::end(word));
		}
	}
	if (descriptors || vertices) {
		// Preserve Windows' nonvolatile copy registers; the published pointers name the record, never the caller's scratch arrays.
		code.insert(code.end(), {0x56, 0x57});
		const auto copy = [&](unsigned pointer, unsigned count, unsigned payload, bool wide) {
			const auto displacement = [&](unsigned value) {
				const uint32_t at = 0x100 + value;
				const auto *bytes = reinterpret_cast<const uint8_t *>(&at);
				code.insert(code.end(), bytes, bytes + 4);
			};
			code.insert(code.end(), {0x49, 0x8b, 0xb3}); displacement((pointer + 1) * 8);
			code.insert(code.end(), {0x49, 0x8d, 0xbb}); displacement(payload);
			code.insert(code.end(), {0x49, 0x89, 0xbb}); displacement((pointer + 1) * 8);
			code.insert(code.end(), {0x41, 0x8b, 0x8b}); displacement((count + 1) * 8);
			code.push_back(0xf3);
			if (wide) code.push_back(0x48);
			code.push_back(0xa5);
		};
		if (descriptors) { copy(5, 4, 80, true); copy(7, 6, 112, false); }
		else { copy(3, 2, 64, true); copy(4, 2, 96, true); }
		code.insert(code.end(), {0x5f, 0x5e});
	}
	const size_t stub_pointer = code.size() + 3;
	const uint8_t publish[]{0x48, 0x8d, 0x0d, 0, 0, 0, 0, 0x49, 0x89, 0x8b, 0, 1, 0, 0,
		0x49, 0xff, 0x82, 0x80, 0, 0, 0, 0xc3};
	code.insert(code.end(), std::begin(publish), std::end(publish));
	if (code.size() > queued_prelude_size - 5)
		throw std::runtime_error("queued command prelude exceeds its slot");
	code.resize(queued_prelude_size, 0x90);
	const auto relative = [&](size_t at, int32_t target) {
		const int32_t displacement = target - int32_t(at + 4);
		std::memcpy(code.data() + at, &displacement, 4);
	};
	for (const size_t at : fallback)
		relative(at, queued_prelude_size - 5);
	relative(stub_pointer, to_served);
	code[queued_prelude_size - 5] = 0xe9;
	relative(queued_prelude_size - 4, to_served);
	return code;
}

// The performance counter is read in the guest once the runtime has published how the processor's time stamp counter relates to it: the
// stub exported for it is a prelude that computes the count from RDTSC and falls back to its served stub until then. The calibration sits
// in the served block: the counter value at the base, the count at the base, and the multiplier, a 32.32 fixed-point count per tick of
// the time stamp counter, written last.
static bool guest_clock_api(std::string_view module, std::string_view name)
{
	return (module == "kernel32.dll" || module == "kernelbase.dll") && name == "QueryPerformanceCounter";
}

static constexpr size_t clock_prelude_size = 96, clock_calibration = 0x800;

static std::vector<uint8_t> clock_prelude(int32_t to_pointer, int32_t to_served)
{
	std::vector<uint8_t> code{
		// mov r10, [pointer]; test r10, r10; jz served
		0x4c, 0x8b, 0x15, 0, 0, 0, 0, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0, 0, 0, 0,
		// mov r11, [r10 + multiplier]; test r11, r11; jz served
		0x4d, 0x8b, 0x9a, 0x10, 0x08, 0, 0, 0x4d, 0x85, 0xdb, 0x0f, 0x84, 0, 0, 0, 0,
		// rdtsc; shl rdx, 32; or rax, rdx; sub rax, [r10 + base counter]; mul r11; shrd rax, rdx, 32; add rax, [r10 + base count]
		0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0, 0x49, 0x2b, 0x82, 0x00, 0x08, 0, 0, 0x49, 0xf7, 0xe3, 0x48, 0x0f, 0xac, 0xd0, 0x20,
		0x49, 0x03, 0x82, 0x08, 0x08, 0, 0,
		// mov [rcx], rax; mov eax, 1; ret
		0x48, 0x89, 0x01, 0xb8, 1, 0, 0, 0, 0xc3};
	const int32_t displacement = to_pointer - 7;
	std::memcpy(code.data() + 3, &displacement, 4);
	for (const size_t jump : {size_t(12), size_t(28)}) {
		const int32_t relative = to_served - int32_t(jump + 4);
		std::memcpy(code.data() + jump, &relative, 4);
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

static size_t commit_jump(GuestMemoryApi kind)
{
	return kind == GuestMemoryApi::commit ? 37 : 31;
}

// mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jz slow; test rdx,rdx; jz slow; cmp r8d,MEM_DECOMMIT; je main; slow: jmp trap;
// restore: mov r8d,MEM_DECOMMIT; jmp slow; main: cmp rdx,256 KiB; ja slow; rax = first page, r11 = end page; look through the open
// ranges for one holding [rax, r11), take a log slot with lock xadd, zero the pages with rep stosb (keeping rdi), store the range with
// its low bit set and return TRUE; a full log or no range goes back to restore.
static std::vector<uint8_t> decommit_prelude(int32_t to_pointer, int32_t to_trap)
{
	std::vector<uint8_t> code{0x4c, 0x8b, 0x15, 0, 0, 0, 0, 0x4d, 0x85, 0xd2, 0x74, 0x13, 0x48, 0x85, 0xc9, 0x74, 0x0e, 0x48, 0x85, 0xd2,
		0x74, 0x09, 0x41, 0x81, 0xf8, 0x00, 0x40, 0x00, 0x00, 0x74, 0x0d, 0xe9, 0, 0, 0, 0, 0x41, 0xb8, 0x00, 0x40, 0x00, 0x00,
		0xeb, 0xf3, 0x48, 0x81, 0xfa, 0x00, 0x00, 0x04, 0x00, 0x77, 0xea, 0x48, 0x89, 0xc8, 0x48, 0x25, 0x00, 0xf0, 0xff, 0xff, 0x4c,
		0x8d, 0x9c, 0x11, 0xff, 0x0f, 0x00, 0x00, 0x49, 0x81, 0xe3, 0x00, 0xf0, 0xff, 0xff, 0x45, 0x8b, 0x82, 0x00, 0x0a, 0x00, 0x00,
		0x4d, 0x8d, 0x8a, 0x08, 0x0a, 0x00, 0x00, 0x45, 0x85, 0xc0, 0x74, 0xc4, 0x49, 0x3b, 0x01, 0x72, 0x4b, 0x4d, 0x3b, 0x59, 0x08,
		0x77, 0x45, 0x41, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x45, 0x0f, 0xc1, 0x82, 0x10, 0x0d, 0x00, 0x00, 0x41, 0x83, 0xf8, 0x20,
		0x73, 0xa4, 0x49, 0xc1, 0xe0, 0x04, 0x57, 0x48, 0x89, 0xc7, 0x49, 0x89, 0xc1, 0x4c, 0x89, 0xd9, 0x48, 0x29, 0xc1, 0x31, 0xc0,
		0xf3, 0xaa, 0x5f, 0x49, 0x83, 0xc9, 0x01, 0x4f, 0x89, 0x8c, 0x02, 0x18, 0x0d, 0x00, 0x00, 0x4f, 0x89, 0x9c, 0x02, 0x20, 0x0d,
		0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0x49, 0x83, 0xc1, 0x10, 0x41, 0xff, 0xc8, 0xeb, 0xa2};
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
	static const std::vector<uint8_t> compare = {0x4d, 0x85, 0xc0, 0x0f, 0x84, 0xd1, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x54, 0x24, 0x28, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0xc3, 0x00, 0x00, 0x00, 0x44,
		0x8b, 0x5c, 0x24, 0x30, 0x45, 0x89, 0xc9, 0x45, 0x85, 0xc9, 0x79, 0x10, 0x45, 0x31, 0xc9, 0x66, 0x43, 0x83, 0x3c, 0x48, 0x00, 0x74, 0x05, 0x49,
		0xff, 0xc1, 0xeb, 0xf3, 0x45, 0x85, 0xdb, 0x79, 0x10, 0x45, 0x31, 0xdb, 0x66, 0x43, 0x83, 0x3c, 0x5a, 0x00, 0x74, 0x05, 0x49, 0xff, 0xc3, 0xeb,
		0xf3, 0x53, 0x56, 0x31, 0xc9, 0x4c, 0x39, 0xc9, 0x74, 0x3f, 0x4c, 0x39, 0xd9, 0x74, 0x3a, 0x41, 0x0f, 0xb7, 0x04, 0x48, 0x41, 0x0f, 0xb7, 0x1c,
		0x4a, 0x3d, 0x80, 0x00, 0x00, 0x00, 0x73, 0x70, 0x81, 0xfb, 0x80, 0x00, 0x00, 0x00, 0x73, 0x68, 0x8d, 0x70, 0x9f, 0x83, 0xfe, 0x19, 0x77, 0x03,
		0x83, 0xe8, 0x20, 0x8d, 0x73, 0x9f, 0x83, 0xfe, 0x19, 0x77, 0x03, 0x83, 0xeb, 0x20, 0x39, 0xd8, 0x72, 0x38, 0x77, 0x3d, 0x48, 0xff, 0xc1, 0xeb,
		0xbc, 0x4d, 0x39, 0xd9, 0x72, 0x2c, 0x77, 0x31, 0xf7, 0xc2, 0x11, 0x00, 0x00, 0x10, 0x75, 0x30, 0x31, 0xc9, 0x4c, 0x39, 0xc9, 0x74, 0x29, 0x41,
		0x0f, 0xb7, 0x04, 0x48, 0x41, 0x0f, 0xb7, 0x1c, 0x4a, 0x39, 0xd8, 0x75, 0x05, 0x48, 0xff, 0xc1, 0xeb, 0xe8, 0x8d, 0x70, 0xbf, 0x83, 0xfe, 0x19,
		0x76, 0x07, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xeb, 0x0c, 0xb8, 0x03, 0x00, 0x00, 0x00, 0xeb, 0x05, 0xb8, 0x02, 0x00, 0x00, 0x00, 0x5e, 0x5b, 0xc3,
		0x5e, 0x5b, 0xe9, 0x00, 0x00, 0x00, 0x00};
	static const std::vector<GuestPrelude> preludes = {
		{"lstrlenW", {0x48, 0x85, 0xc9, 0x74, 0x16, 0x48, 0x89, 0xc8, 0x66, 0x83, 0x38, 0x00, 0x74, 0x06, 0x48, 0x83, 0xc0, 0x02, 0xeb, 0xf4, 0x48, 0x29, 0xc8, 0x48,
			0xd1, 0xe8, 0xc3, 0x31, 0xc0, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00}},
		{"lstrlenA", {0x48, 0x85, 0xc9, 0x74, 0x11, 0x48, 0x89, 0xc8, 0x80, 0x38, 0x00, 0x74, 0x05, 0x48, 0xff, 0xc0, 0xeb, 0xf6, 0x48, 0x29, 0xc8, 0xc3, 0x31, 0xc0,
			0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00}},
		{"lstrcpynW", {0x48, 0x85, 0xc9, 0x74, 0x36, 0x48, 0x85, 0xd2, 0x74, 0x31, 0x45, 0x85, 0xc0, 0x7e, 0x2c, 0x48, 0x89, 0xc8, 0x45, 0x8d, 0x48, 0xff, 0x45, 0x85,
			0xc9, 0x74, 0x1a, 0x44, 0x0f, 0xb7, 0x12, 0x45, 0x85, 0xd2, 0x74, 0x11, 0x66, 0x44, 0x89, 0x11, 0x48, 0x83, 0xc1, 0x02, 0x48, 0x83, 0xc2, 0x02,
			0x41, 0xff, 0xc9, 0xeb, 0xe1, 0x66, 0xc7, 0x01, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00}},
		{"lstrcpynA", {0x48, 0x85, 0xc9, 0x74, 0x31, 0x48, 0x85, 0xd2, 0x74, 0x2c, 0x45, 0x85, 0xc0, 0x7e, 0x27, 0x48, 0x89, 0xc8, 0x45, 0x8d, 0x48, 0xff, 0x45, 0x85,
			0xc9, 0x74, 0x17, 0x44, 0x0f, 0xb6, 0x12, 0x45, 0x85, 0xd2, 0x74, 0x0e, 0x44, 0x88, 0x11, 0x48, 0xff, 0xc1, 0x48, 0xff, 0xc2, 0x41, 0xff, 0xc9,
			0xeb, 0xe4, 0xc6, 0x01, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00}},
		{"lstrcmpW", {0x48, 0x85, 0xc9, 0x0f, 0x84, 0x81, 0x00, 0x00, 0x00, 0x48, 0x85, 0xd2, 0x74, 0x7c, 0x45, 0x31, 0xc0, 0x42, 0x0f, 0xb7, 0x04, 0x41, 0x46, 0x0f,
			0xb7, 0x0c, 0x42, 0x3d, 0x80, 0x00, 0x00, 0x00, 0x73, 0x68, 0x41, 0x81, 0xf9, 0x80, 0x00, 0x00, 0x00, 0x73, 0x5f, 0x44, 0x8d, 0x50, 0x9f, 0x41,
			0x83, 0xfa, 0x19, 0x77, 0x03, 0x83, 0xe8, 0x20, 0x45, 0x8d, 0x51, 0x9f, 0x41, 0x83, 0xfa, 0x19, 0x77, 0x04, 0x41, 0x83, 0xe9, 0x20, 0x44, 0x39,
			0xc8, 0x72, 0x30, 0x77, 0x34, 0x85, 0xc0, 0x74, 0x05, 0x49, 0xff, 0xc0, 0xeb, 0xbb, 0x45, 0x31, 0xc0, 0x42, 0x0f, 0xb7, 0x04, 0x41, 0x46, 0x0f,
			0xb7, 0x0c, 0x42, 0x44, 0x39, 0xc8, 0x75, 0x09, 0x85, 0xc0, 0x74, 0x1b, 0x49, 0xff, 0xc0, 0xeb, 0xe8, 0x44, 0x8d, 0x50, 0xbf, 0x41, 0x83, 0xfa,
			0x19, 0x76, 0x06, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0x31, 0xc0, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00}},
		{"MultiByteToWideChar", {0x81, 0xf9, 0xe9, 0xfd, 0x00, 0x00, 0x75, 0x0e, 0xf7, 0xc2, 0xf7, 0xff, 0xff, 0xff, 0x0f, 0x85, 0x88, 0x00, 0x00, 0x00, 0xeb, 0x14, 0x85, 0xc9,
			0x74, 0x08, 0x81, 0xf9, 0xe4, 0x04, 0x00, 0x00, 0x75, 0x7a, 0xf7, 0xc2, 0xf6, 0xff, 0xff, 0xff, 0x75, 0x72, 0x4d, 0x85, 0xc0, 0x74, 0x6d, 0x44,
			0x89, 0xc8, 0x83, 0xf8, 0xff, 0x75, 0x17, 0x4d, 0x89, 0xc2, 0x41, 0x80, 0x3a, 0x00, 0x74, 0x05, 0x49, 0xff, 0xc2, 0xeb, 0xf5, 0x4d, 0x29, 0xc2,
			0x41, 0x8d, 0x42, 0x01, 0xeb, 0x04, 0x85, 0xc0, 0x7e, 0x4a, 0x45, 0x31, 0xd2, 0x41, 0x39, 0xc2, 0x73, 0x0c, 0x43, 0xf6, 0x04, 0x10, 0x80, 0x75,
			0x3b, 0x41, 0xff, 0xc2, 0xeb, 0xef, 0x4c, 0x8b, 0x5c, 0x24, 0x28, 0x44, 0x8b, 0x54, 0x24, 0x30, 0x45, 0x85, 0xd2, 0x74, 0x26, 0x4d, 0x85, 0xdb,
			0x74, 0x22, 0x4d, 0x39, 0xc3, 0x74, 0x1d, 0x41, 0x39, 0xc2, 0x7c, 0x18, 0x45, 0x31, 0xd2, 0x41, 0x39, 0xc2, 0x73, 0x0f, 0x43, 0x0f, 0xb6, 0x14,
			0x10, 0x66, 0x43, 0x89, 0x14, 0x53, 0x41, 0xff, 0xc2, 0xeb, 0xec, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00}},
		{"WideCharToMultiByte", {0x85, 0xc9, 0x74, 0x2c, 0x81, 0xf9, 0xe4, 0x04, 0x00, 0x00, 0x74, 0x24, 0x81, 0xf9, 0xe9, 0xfd, 0x00, 0x00, 0x0f, 0x85, 0xa4, 0x00, 0x00, 0x00,
			0x48, 0x83, 0x7c, 0x24, 0x38, 0x00, 0x0f, 0x85, 0x98, 0x00, 0x00, 0x00, 0x48, 0x83, 0x7c, 0x24, 0x40, 0x00, 0x0f, 0x85, 0x8c, 0x00, 0x00, 0x00,
			0x85, 0xd2, 0x0f, 0x85, 0x84, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xc0, 0x74, 0x7f, 0x44, 0x89, 0xc8, 0x83, 0xf8, 0xff, 0x75, 0x1c, 0x4d, 0x89, 0xc2,
			0x66, 0x41, 0x83, 0x3a, 0x00, 0x74, 0x06, 0x49, 0x83, 0xc2, 0x02, 0xeb, 0xf3, 0x4d, 0x29, 0xc2, 0x49, 0xd1, 0xea, 0x41, 0x8d, 0x42, 0x01, 0xeb,
			0x04, 0x85, 0xc0, 0x7e, 0x57, 0x45, 0x31, 0xd2, 0x41, 0x39, 0xc2, 0x73, 0x0e, 0x66, 0x43, 0x81, 0x3c, 0x50, 0x80, 0x00, 0x73, 0x46, 0x41, 0xff,
			0xc2, 0xeb, 0xed, 0x4c, 0x8b, 0x5c, 0x24, 0x28, 0x44, 0x8b, 0x54, 0x24, 0x30, 0x45, 0x85, 0xd2, 0x74, 0x20, 0x4d, 0x85, 0xdb, 0x74, 0x2d, 0x41,
			0x39, 0xc2, 0x7c, 0x28, 0x45, 0x31, 0xd2, 0x41, 0x39, 0xc2, 0x73, 0x0e, 0x43, 0x0f, 0xb7, 0x14, 0x50, 0x43, 0x88, 0x14, 0x13, 0x41, 0xff, 0xc2,
			0xeb, 0xed, 0x4c, 0x8b, 0x5c, 0x24, 0x40, 0x4d, 0x85, 0xdb, 0x74, 0x07, 0x41, 0xc7, 0x03, 0x00, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00,
			0x00}},
		{"CompareStringEx", compare},
		{"CompareStringW", compare},
		{"GetThreadLocale", {0xb8, 0x09, 0x04, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00}},
		{"SetThreadLocale", {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00}},
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

// Services the guest answers from what the runtime publishes in the served block. The millisecond clocks are the performance counter
// divided by 10000, read from the time stamp counter once the runtime has published its calibration, as QueryPerformanceCounter is.
// Each loads the block pointer first, as the served stubs do, and ends in a jump to the export's breakpoint.
struct BlockPrelude {
	std::string_view module, name;
};

static const std::vector<uint8_t> &tick_prelude()
{
	// mov r10,[pointer]; test r10,r10; jz slow; mov r11,[r10+multiplier]; test r11,r11; jz slow; rdtsc; the count as in the clock
	// prelude; xor edx,edx; mov ecx,10000; div rcx; ret; slow: jmp breakpoint
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x36, 0x4d, 0x8b, 0x9a, 0x10, 0x08, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x2a,
		0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0, 0x49, 0x2b, 0x82, 0x00, 0x08, 0x00, 0x00, 0x49, 0xf7, 0xe3, 0x48, 0x0f, 0xac, 0xd0, 0x20,
		0x49, 0x03, 0x82, 0x08, 0x08, 0x00, 0x00, 0x31, 0xd2, 0xb9, 0x10, 0x27, 0x00, 0x00, 0x48, 0xf7, 0xf1, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// The screen's device context for GetDC(NULL), which programs take to read the display resolution and give back at once: the runtime
// publishes one context, its capabilities as a table indexed by GetDeviceCaps' index and a state word (0 free, 1 taken by a GetDC the
// guest answered, 2 changed by a call the runtime served); GetDC takes it free with lock cmpxchg, ReleaseDC frees it unless changed, and
// GetDeviceCaps reads the table. A changed context goes back to the runtime, which releases it and publishes another.
static constexpr size_t screen_caps = 0x400, screen_dc = 0x600, screen_dc_state = 0x608;

static const std::vector<uint8_t> &screen_get_dc_prelude()
{
	// mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jnz slow; xor eax,eax; mov r11d,1; lock cmpxchg [r10+state],r11d; jne slow;
	// mov rax,[r10+context]; test rax,rax; jz undo; ret; undo: mov dword [r10+state],0; slow: jmp breakpoint
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x30, 0x48, 0x85, 0xc9, 0x75, 0x2b, 0x31, 0xc0, 0x41, 0xbb, 0x01, 0x00, 0x00,
		0x00, 0xf0, 0x45, 0x0f, 0xb1, 0x9a, 0x08, 0x06, 0x00, 0x00, 0x75, 0x18, 0x49, 0x8b, 0x82, 0x00, 0x06, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x01,
		0xc3, 0x41, 0xc7, 0x82, 0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &screen_release_dc_prelude()
{
	// mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jnz slow; test rdx,rdx; jz slow; cmp rdx,[r10+context]; jne slow;
	// mov eax,1; xor r11d,r11d; lock cmpxchg [r10+state],r11d; jne slow; ret; slow: jmp breakpoint
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x27, 0x48, 0x85, 0xc9, 0x75, 0x22, 0x48, 0x85, 0xd2, 0x74, 0x1d, 0x49, 0x3b,
		0x92, 0x00, 0x06, 0x00, 0x00, 0x75, 0x14, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x45, 0x31, 0xdb, 0xf0, 0x45, 0x0f, 0xb1, 0x9a, 0x08, 0x06, 0x00, 0x00,
		0x75, 0x01, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &screen_device_caps_prelude()
{
	// mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jz slow; cmp edx,127; ja slow; the screen's context answers from the
	// caps table; another context answers when the attribute table holds its slot (see dc_text_color_prelude) and the index does not
	// depend on its bitmap: TECHNOLOGY from the slot's window flag (display for a window's context, 0 for a memory one), LOGPIXELSX and
	// LOGPIXELSY 96, PLANES 1, SHADEBLENDCAPS 0, as device_caps answers them; anything else: jmp breakpoint. GDI+ asks these thousands of
	// times while it paints.
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x6f, 0x48, 0x85, 0xc9, 0x74, 0x6a, 0x83, 0xfa, 0x7f, 0x77, 0x65, 0x49, 0x3b,
		0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x51, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x50, 0x89, 0xc8, 0x25, 0xff, 0x03,
		0x00, 0x00, 0xc1, 0xe0, 0x05, 0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x39, 0x83, 0xfa, 0x02, 0x74, 0x16, 0x83,
		0xfa, 0x58, 0x74, 0x15, 0x83, 0xfa, 0x5a, 0x74, 0x10, 0x83, 0xfa, 0x0e, 0x74, 0x11, 0x83, 0xfa, 0x78, 0x74, 0x12, 0xeb, 0x1e, 0x8b, 0x40, 0x18,
		0xc3, 0xb8, 0x60, 0x00, 0x00, 0x00, 0xc3, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0x31, 0xc0, 0xc3, 0x89, 0xd0, 0x41, 0x8b, 0x84, 0x82, 0x00, 0x04,
		0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
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
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0x82, 0x00, 0x00, 0x00, 0x4d, 0x8b, 0x9a, 0x00, 0x07, 0x00, 0x00, 0x4d,
		0x85, 0xdb, 0x74, 0x76, 0x48, 0x85, 0xc9, 0x74, 0x71, 0x89, 0xc8, 0x25, 0xff, 0x01, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x49, 0x01, 0xc3, 0x49, 0x3b,
		0x0b, 0x75, 0x5f, 0x85, 0xd2, 0x78, 0x15, 0x89, 0xd0, 0x4c, 0x8d, 0x50, 0x08, 0x45, 0x3b, 0x53, 0x50, 0x77, 0x4f, 0x49, 0x8b, 0x84, 0x03, 0xa0,
		0x00, 0x00, 0x00, 0xc3, 0x83, 0xfa, 0xeb, 0x75, 0x05, 0x49, 0x8b, 0x43, 0x08, 0xc3, 0x83, 0xfa, 0xfc, 0x75, 0x05, 0x49, 0x8b, 0x43, 0x10, 0xc3,
		0x83, 0xfa, 0xfa, 0x75, 0x05, 0x49, 0x8b, 0x43, 0x18, 0xc3, 0x83, 0xfa, 0xf8, 0x75, 0x05, 0x49, 0x8b, 0x43, 0x20, 0xc3, 0x83, 0xfa, 0xf4, 0x75,
		0x05, 0x49, 0x8b, 0x43, 0x28, 0xc3, 0x83, 0xfa, 0xf0, 0x75, 0x05, 0x41, 0x8b, 0x43, 0x30, 0xc3, 0x83, 0xfa, 0xec, 0x75, 0x05, 0x41, 0x8b, 0x43,
		0x34, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &window_long_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x64, 0x4d, 0x8b, 0x9a, 0x00, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x58,
		0x48, 0x85, 0xc9, 0x74, 0x53, 0x89, 0xc8, 0x25, 0xff, 0x01, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x49, 0x01, 0xc3, 0x49, 0x3b, 0x0b, 0x75, 0x41, 0x85,
		0xd2, 0x78, 0x15, 0x89, 0xd0, 0x4c, 0x8d, 0x50, 0x04, 0x45, 0x3b, 0x53, 0x50, 0x77, 0x31, 0x41, 0x8b, 0x84, 0x03, 0xa0, 0x00, 0x00, 0x00, 0xc3,
		0x83, 0xfa, 0xeb, 0x75, 0x05, 0x41, 0x8b, 0x43, 0x08, 0xc3, 0x83, 0xfa, 0xf4, 0x75, 0x05, 0x41, 0x8b, 0x43, 0x28, 0xc3, 0x83, 0xfa, 0xf0, 0x75,
		0x05, 0x41, 0x8b, 0x43, 0x30, 0xc3, 0x83, 0xfa, 0xec, 0x75, 0x05, 0x41, 0x8b, 0x43, 0x34, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &window_property_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x52, 0x4d, 0x8b, 0x9a, 0x00, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x46,
		0x48, 0x85, 0xc9, 0x74, 0x41, 0x89, 0xc8, 0x25, 0xff, 0x01, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x49, 0x01, 0xc3, 0x49, 0x3b, 0x0b, 0x75, 0x2f, 0x48,
		0x81, 0xfa, 0xff, 0xff, 0x00, 0x00, 0x77, 0x26, 0x41, 0x8b, 0x43, 0x54, 0x83, 0xf8, 0x04, 0x77, 0x1d, 0x4d, 0x8d, 0x53, 0x60, 0x85, 0xc0, 0x74,
		0x0d, 0x49, 0x3b, 0x12, 0x74, 0x0b, 0x49, 0x83, 0xc2, 0x10, 0xff, 0xc8, 0xeb, 0xef, 0x31, 0xc0, 0xc3, 0x49, 0x8b, 0x42, 0x08, 0xc3, 0xe9, 0x00,
		0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &window_exists_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x29, 0x4d, 0x8b, 0x9a, 0x00, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x1d,
		0x48, 0x85, 0xc9, 0x74, 0x18, 0x89, 0xc8, 0x25, 0xff, 0x01, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x49, 0x01, 0xc3, 0x49, 0x3b, 0x0b, 0x75, 0x06, 0xb8,
		0x01, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &window_client_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x3d, 0x4d, 0x8b, 0x9a, 0x00, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x31,
		0x48, 0x85, 0xc9, 0x74, 0x2c, 0x89, 0xc8, 0x25, 0xff, 0x01, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x49, 0x01, 0xc3, 0x49, 0x3b, 0x0b, 0x75, 0x1a, 0x48,
		0x85, 0xd2, 0x74, 0x15, 0x48, 0xc7, 0x02, 0x00, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x43, 0x38, 0x48, 0x89, 0x42, 0x08, 0xb8, 0x01, 0x00, 0x00, 0x00,
		0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &window_outer_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x3d, 0x4d, 0x8b, 0x9a, 0x00, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x31,
		0x48, 0x85, 0xc9, 0x74, 0x2c, 0x89, 0xc8, 0x25, 0xff, 0x01, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x49, 0x01, 0xc3, 0x49, 0x3b, 0x0b, 0x75, 0x1a, 0x48,
		0x85, 0xd2, 0x74, 0x15, 0x49, 0x8b, 0x43, 0x40, 0x48, 0x89, 0x02, 0x49, 0x8b, 0x43, 0x48, 0x48, 0x89, 0x42, 0x08, 0xb8, 0x01, 0x00, 0x00, 0x00,
		0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// Locale information, which the runtime answers by type alone: the guest reads it from the table the runtime publishes once (see
// locale_table), through the served block. A type with flags beyond LOCALE_RETURN_NUMBER and LOCALE_NOUSEROVERRIDE, one without
// text, a buffer too small or a null buffer goes to the runtime.
static constexpr size_t locale_pointer = 0x708;

static const std::vector<uint8_t> &locale_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0x94, 0x00, 0x00, 0x00, 0x4d, 0x8b, 0x9a, 0x08, 0x07, 0x00, 0x00, 0x4d,
		0x85, 0xdb, 0x0f, 0x84, 0x84, 0x00, 0x00, 0x00, 0x89, 0xd0, 0x25, 0xff, 0xff, 0xff, 0x5f, 0x3d, 0x80, 0x00, 0x00, 0x00, 0x72, 0x0f, 0x2d, 0x00,
		0x10, 0x00, 0x00, 0x83, 0xf8, 0x10, 0x73, 0x6c, 0x05, 0x80, 0x00, 0x00, 0x00, 0x45, 0x0f, 0xb7, 0x14, 0xc3, 0x41, 0x81, 0xfa, 0xff, 0xff, 0x00,
		0x00, 0x74, 0x59, 0xf7, 0xc2, 0x00, 0x00, 0x00, 0x20, 0x75, 0x38, 0x41, 0x0f, 0xb7, 0x44, 0xc3, 0x02, 0xff, 0xc0, 0x45, 0x85, 0xc9, 0x74, 0x2a,
		0x78, 0x42, 0x41, 0x39, 0xc1, 0x72, 0x3d, 0x4d, 0x85, 0xc0, 0x74, 0x38, 0x4f, 0x8d, 0x94, 0x53, 0x80, 0x04, 0x00, 0x00, 0x89, 0xc1, 0x66, 0x41,
		0x8b, 0x12, 0x66, 0x41, 0x89, 0x10, 0x49, 0x83, 0xc2, 0x02, 0x49, 0x83, 0xc0, 0x02, 0xff, 0xc9, 0x75, 0xec, 0xc3, 0x41, 0x83, 0xf9, 0x02, 0x7c,
		0x13, 0x4d, 0x85, 0xc0, 0x74, 0x0e, 0x41, 0x8b, 0x44, 0xc3, 0x04, 0x41, 0x89, 0x00, 0xb8, 0x02, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00,
		0x00};
	return code;
}

// NtAllocateVirtualMemory committing read-write pages in an open range, as the commit prelude of VirtualAlloc does for its own
// arguments: the current process, no zero bits, the base and size behind their pointers, MEM_COMMIT and PAGE_READWRITE. The rounded
// base and size are written back and STATUS_SUCCESS returned; anything else goes to the runtime with the arguments as they came.
static const std::vector<uint8_t> &nt_commit_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0xce, 0x00, 0x00, 0x00, 0x48, 0x83, 0xf9, 0xff, 0x0f, 0x85, 0xc4, 0x00,
		0x00, 0x00, 0x4d, 0x85, 0xc0, 0x0f, 0x85, 0xbb, 0x00, 0x00, 0x00, 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xb2, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xc9, 0x0f,
		0x84, 0xa9, 0x00, 0x00, 0x00, 0x81, 0x7c, 0x24, 0x28, 0x00, 0x10, 0x00, 0x00, 0x0f, 0x85, 0x9b, 0x00, 0x00, 0x00, 0x83, 0x7c, 0x24, 0x30, 0x04,
		0x0f, 0x85, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x02, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x84, 0x00, 0x00, 0x00, 0x4d, 0x8b, 0x19, 0x4d, 0x85, 0xdb,
		0x74, 0x7c, 0x4e, 0x8d, 0x9c, 0x18, 0xff, 0x0f, 0x00, 0x00, 0x49, 0x81, 0xe3, 0x00, 0xf0, 0xff, 0xff, 0x48, 0x25, 0x00, 0xf0, 0xff, 0xff, 0x49,
		0x39, 0xc3, 0x76, 0x62, 0x41, 0x8b, 0x8a, 0x00, 0x0a, 0x00, 0x00, 0x4d, 0x8d, 0x82, 0x08, 0x0a, 0x00, 0x00, 0x85, 0xc9, 0x74, 0x46, 0x49, 0x3b,
		0x00, 0x72, 0x39, 0x4d, 0x3b, 0x58, 0x08, 0x77, 0x33, 0xb9, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x41, 0x0f, 0xc1, 0x8a, 0x10, 0x0d, 0x00, 0x00, 0x83,
		0xf9, 0x20, 0x73, 0x28, 0x48, 0xc1, 0xe1, 0x04, 0x49, 0x89, 0x84, 0x0a, 0x18, 0x0d, 0x00, 0x00, 0x4d, 0x89, 0x9c, 0x0a, 0x20, 0x0d, 0x00, 0x00,
		0x48, 0x89, 0x02, 0x49, 0x29, 0xc3, 0x4d, 0x89, 0x19, 0x31, 0xc0, 0xc3, 0x49, 0x83, 0xc0, 0x10, 0xff, 0xc9, 0xeb, 0xb6, 0x48, 0xc7, 0xc1, 0xff,
		0xff, 0xff, 0xff, 0x45, 0x31, 0xc0, 0xe9, 0x00, 0x00, 0x00, 0x00};
	static_assert(NativeMemory::commit_windows == 0xa00 && NativeMemory::commit_log == 0xd10 && NativeMemory::commit_log_count == 32,
		"the commit preludes' offsets are written into their code");
	return code;
}

// NtFreeVirtualMemory decommitting at most 256 KiB of an open range, as the decommit prelude of VirtualFree does: the pages are
// zeroed in place and logged with the low bit set, and the rounded base and size are written back.
static const std::vector<uint8_t> &nt_decommit_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0xe5, 0x00, 0x00, 0x00, 0x48, 0x83, 0xf9, 0xff, 0x0f, 0x85, 0xdb, 0x00,
		0x00, 0x00, 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xd2, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xc0, 0x0f, 0x84, 0xc9, 0x00, 0x00, 0x00, 0x41, 0x81, 0xf9, 0x00,
		0x40, 0x00, 0x00, 0x0f, 0x85, 0xbc, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x02, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0xb0, 0x00, 0x00, 0x00, 0x4d, 0x8b, 0x18,
		0x4d, 0x85, 0xdb, 0x0f, 0x84, 0xa4, 0x00, 0x00, 0x00, 0x49, 0x81, 0xfb, 0x00, 0x00, 0x04, 0x00, 0x0f, 0x87, 0x97, 0x00, 0x00, 0x00, 0x4e, 0x8d,
		0x9c, 0x18, 0xff, 0x0f, 0x00, 0x00, 0x49, 0x81, 0xe3, 0x00, 0xf0, 0xff, 0xff, 0x48, 0x25, 0x00, 0xf0, 0xff, 0xff, 0x49, 0x39, 0xc3, 0x76, 0x7d,
		0x41, 0x8b, 0x8a, 0x00, 0x0a, 0x00, 0x00, 0x4d, 0x8d, 0x8a, 0x08, 0x0a, 0x00, 0x00, 0x85, 0xc9, 0x74, 0x5e, 0x49, 0x3b, 0x01, 0x72, 0x51, 0x4d,
		0x3b, 0x59, 0x08, 0x77, 0x4b, 0xb9, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x41, 0x0f, 0xc1, 0x8a, 0x10, 0x0d, 0x00, 0x00, 0x83, 0xf9, 0x20, 0x73, 0x40,
		0x48, 0xc1, 0xe1, 0x04, 0x49, 0x89, 0xc9, 0x57, 0x50, 0x48, 0x89, 0xc7, 0x4c, 0x89, 0xd9, 0x48, 0x29, 0xc1, 0x31, 0xc0, 0xf3, 0xaa, 0x58, 0x5f,
		0x48, 0x8d, 0x48, 0x01, 0x4b, 0x89, 0x8c, 0x0a, 0x18, 0x0d, 0x00, 0x00, 0x4f, 0x89, 0x9c, 0x0a, 0x20, 0x0d, 0x00, 0x00, 0x48, 0x89, 0x02, 0x49,
		0x29, 0xc3, 0x4d, 0x89, 0x18, 0x31, 0xc0, 0xc3, 0x49, 0x83, 0xc1, 0x10, 0xff, 0xc9, 0xeb, 0x9e, 0x48, 0xc7, 0xc1, 0xff, 0xff, 0xff, 0xff, 0x41,
		0xb9, 0x00, 0x40, 0x00, 0x00, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// IsChild walks the parents the window table holds, GetFocus reads the focus window the runtime publishes with it, and GetDpiForWindow
// answers 96 as the runtime does.
static constexpr size_t focus_published = 0x710, focus_window = 0x718;

static const std::vector<uint8_t> &window_child_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x5f, 0x4d, 0x8b, 0x9a, 0x00, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x53,
		0x31, 0xc0, 0x48, 0x85, 0xc9, 0x74, 0x4b, 0x48, 0x85, 0xd2, 0x74, 0x46, 0x48, 0x39, 0xd1, 0x74, 0x41, 0x41, 0xb8, 0x40, 0x00, 0x00, 0x00, 0x49,
		0x89, 0xd1, 0x44, 0x89, 0xc8, 0x25, 0xff, 0x01, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x4c, 0x01, 0xd8, 0x4c, 0x3b, 0x08, 0x75, 0x15, 0x4c, 0x8b, 0x48,
		0x58, 0x31, 0xc0, 0x4d, 0x85, 0xc9, 0x74, 0x1a, 0x49, 0x39, 0xc9, 0x74, 0x10, 0x41, 0xff, 0xc8, 0x75, 0xd8, 0x41, 0xb8, 0x00, 0x00, 0x00, 0x00,
		0x45, 0x31, 0xc9, 0xeb, 0x06, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &window_dpi_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x60, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &focus_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x12, 0x49, 0x83, 0xba, 0x10, 0x07, 0x00, 0x00, 0x00, 0x74, 0x08, 0x49, 0x8b,
		0x82, 0x18, 0x07, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// Menus by position, read from the menu table the runtime keeps (see NativeWindows::publish_menus): GetMenuItemCount, GetSubMenu,
// GetMenuItemID and GetMenuStringW with MF_BYPOSITION. A menu without a slot goes to the runtime.
static constexpr size_t menu_pointer = 0x720;

static const std::vector<uint8_t> &menu_count_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x27, 0x4d, 0x8b, 0x9a, 0x20, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x1b,
		0x48, 0x85, 0xc9, 0x74, 0x16, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x04, 0x4c, 0x01, 0xd8, 0x48, 0x3b, 0x08, 0x75, 0x04, 0x8b,
		0x40, 0x0c, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &menu_submenu_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x3e, 0x4d, 0x8b, 0x9a, 0x20, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x32,
		0x48, 0x85, 0xc9, 0x74, 0x2d, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x04, 0x4c, 0x01, 0xd8, 0x48, 0x3b, 0x08, 0x75, 0x1b, 0x3b,
		0x50, 0x0c, 0x73, 0x13, 0x44, 0x8b, 0x50, 0x08, 0x4d, 0x01, 0xda, 0x89, 0xd0, 0x48, 0xc1, 0xe0, 0x05, 0x49, 0x8b, 0x44, 0x02, 0x10, 0xc3, 0x31,
		0xc0, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &menu_id_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x4a, 0x4d, 0x8b, 0x9a, 0x20, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x3e,
		0x48, 0x85, 0xc9, 0x74, 0x39, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x04, 0x4c, 0x01, 0xd8, 0x48, 0x3b, 0x08, 0x75, 0x27, 0x3b,
		0x50, 0x0c, 0x73, 0x1c, 0x44, 0x8b, 0x50, 0x08, 0x4d, 0x01, 0xda, 0x89, 0xd0, 0x48, 0xc1, 0xe0, 0x05, 0x49, 0x01, 0xc2, 0x49, 0x83, 0x7a, 0x10,
		0x00, 0x75, 0x05, 0x41, 0x8b, 0x42, 0x08, 0xc3, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// GetMenuState, and GetMenuStringW by command as well as by position: an item looked up by command is searched for in the order the
// runtime searches, a level's own items first and then each submenu in turn, on the guest's stack; a menu missing from the table or
// a tree deeper than the stack allows goes to the runtime.
static const std::vector<uint8_t> &menu_state_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0x41, 0x01, 0x00, 0x00, 0x4d, 0x8b, 0x9a, 0x20, 0x07, 0x00, 0x00, 0x4d,
		0x85, 0xdb, 0x0f, 0x84, 0x31, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x0f, 0x84, 0x28, 0x01, 0x00, 0x00, 0x41, 0xf7, 0xc0, 0x00, 0x04, 0x00, 0x00,
		0x74, 0x2d, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x04, 0x4c, 0x01, 0xd8, 0x48, 0x3b, 0x08, 0x0f, 0x85, 0x09, 0x01, 0x00, 0x00,
		0x3b, 0x50, 0x0c, 0x73, 0x74, 0x44, 0x8b, 0x50, 0x08, 0x4d, 0x01, 0xda, 0x89, 0xd0, 0x48, 0xc1, 0xe0, 0x05, 0x49, 0x01, 0xc2, 0xeb, 0x1f, 0x55,
		0x48, 0x89, 0xe5, 0x51, 0x52, 0xe8, 0x5d, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4d, 0xf8, 0x48, 0x8b, 0x55, 0xf0, 0x48, 0x89, 0xec, 0x5d, 0x48, 0x85,
		0xc0, 0x74, 0x46, 0x49, 0x89, 0xc2, 0x41, 0x8b, 0x02, 0x25, 0x64, 0x09, 0x00, 0x00, 0x41, 0x0b, 0x42, 0x04, 0x49, 0x83, 0x7a, 0x10, 0x00, 0x74,
		0x2f, 0x4d, 0x8b, 0x4a, 0x10, 0x45, 0x89, 0xca, 0x41, 0x81, 0xe2, 0xff, 0x03, 0x00, 0x00, 0x41, 0xc1, 0xe2, 0x04, 0x4d, 0x01, 0xda, 0x4d, 0x3b,
		0x0a, 0x0f, 0x85, 0xa2, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0xc0, 0x83, 0xc8, 0x10, 0x45, 0x8b, 0x4a, 0x0c, 0x41, 0xc1, 0xe1, 0x08, 0x44, 0x09, 0xc8,
		0xc3, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3, 0x48, 0x89, 0xe8, 0x48, 0x29, 0xe0, 0x48, 0x3d, 0x00, 0x10, 0x00, 0x00, 0x77, 0x70, 0x89, 0xc8, 0x25,
		0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x04, 0x4c, 0x01, 0xd8, 0x48, 0x3b, 0x08, 0x75, 0x5e, 0x44, 0x8b, 0x50, 0x08, 0x4d, 0x01, 0xda, 0x8b, 0x40,
		0x0c, 0x41, 0x52, 0x50, 0x85, 0xc0, 0x74, 0x1d, 0x49, 0x83, 0x7a, 0x10, 0x00, 0x75, 0x0e, 0x41, 0x3b, 0x52, 0x08, 0x75, 0x08, 0x4c, 0x89, 0xd0,
		0x48, 0x83, 0xc4, 0x10, 0xc3, 0x49, 0x83, 0xc2, 0x20, 0xff, 0xc8, 0xeb, 0xdf, 0x58, 0x41, 0x5a, 0x85, 0xc0, 0x74, 0x21, 0x49, 0x8b, 0x4a, 0x10,
		0x48, 0x85, 0xc9, 0x74, 0x10, 0x41, 0x52, 0x50, 0xe8, 0x9a, 0xff, 0xff, 0xff, 0x48, 0x85, 0xc0, 0x75, 0x0e, 0x58, 0x41, 0x5a, 0x49, 0x83, 0xc2,
		0x20, 0xff, 0xc8, 0xeb, 0xdb, 0x31, 0xc0, 0xc3, 0x48, 0x83, 0xc4, 0x10, 0xc3, 0x48, 0x8b, 0x4d, 0xf8, 0x48, 0x8b, 0x55, 0xf0, 0x48, 0x89, 0xec,
		0x5d, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &menu_string_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x0f, 0x84, 0x3f, 0x01, 0x00, 0x00, 0x4d, 0x8b, 0x9a, 0x20, 0x07, 0x00, 0x00, 0x4d,
		0x85, 0xdb, 0x0f, 0x84, 0x2f, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x0f, 0x84, 0x26, 0x01, 0x00, 0x00, 0x45, 0x85, 0xc9, 0x0f, 0x88, 0x1d, 0x01,
		0x00, 0x00, 0xf7, 0x44, 0x24, 0x28, 0x00, 0x04, 0x00, 0x00, 0x74, 0x2d, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x04, 0x4c, 0x01,
		0xd8, 0x48, 0x3b, 0x08, 0x0f, 0x85, 0xfd, 0x00, 0x00, 0x00, 0x3b, 0x50, 0x0c, 0x73, 0x6b, 0x44, 0x8b, 0x50, 0x08, 0x4d, 0x01, 0xda, 0x89, 0xd0,
		0x48, 0xc1, 0xe0, 0x05, 0x49, 0x01, 0xc2, 0xeb, 0x1f, 0x55, 0x48, 0x89, 0xe5, 0x51, 0x52, 0xe8, 0x51, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x4d, 0xf8,
		0x48, 0x8b, 0x55, 0xf0, 0x48, 0x89, 0xec, 0x5d, 0x48, 0x85, 0xc0, 0x74, 0x3d, 0x49, 0x89, 0xc2, 0x41, 0x8b, 0x42, 0x0c, 0x45, 0x85, 0xc9, 0x74,
		0x30, 0x4d, 0x85, 0xc0, 0x74, 0x2b, 0x41, 0x8d, 0x49, 0xff, 0x39, 0xc8, 0x0f, 0x47, 0xc1, 0x45, 0x8b, 0x52, 0x18, 0x4d, 0x01, 0xda, 0x31, 0xc9,
		0x39, 0xc1, 0x74, 0x0e, 0x66, 0x41, 0x8b, 0x14, 0x4a, 0x66, 0x41, 0x89, 0x14, 0x48, 0xff, 0xc1, 0xeb, 0xee, 0x66, 0x41, 0xc7, 0x04, 0x48, 0x00,
		0x00, 0xc3, 0x31, 0xc0, 0xc3, 0x48, 0x89, 0xe8, 0x48, 0x29, 0xe0, 0x48, 0x3d, 0x00, 0x10, 0x00, 0x00, 0x77, 0x70, 0x89, 0xc8, 0x25, 0xff, 0x03,
		0x00, 0x00, 0xc1, 0xe0, 0x04, 0x4c, 0x01, 0xd8, 0x48, 0x3b, 0x08, 0x75, 0x5e, 0x44, 0x8b, 0x50, 0x08, 0x4d, 0x01, 0xda, 0x8b, 0x40, 0x0c, 0x41,
		0x52, 0x50, 0x85, 0xc0, 0x74, 0x1d, 0x49, 0x83, 0x7a, 0x10, 0x00, 0x75, 0x0e, 0x41, 0x3b, 0x52, 0x08, 0x75, 0x08, 0x4c, 0x89, 0xd0, 0x48, 0x83,
		0xc4, 0x10, 0xc3, 0x49, 0x83, 0xc2, 0x20, 0xff, 0xc8, 0xeb, 0xdf, 0x58, 0x41, 0x5a, 0x85, 0xc0, 0x74, 0x21, 0x49, 0x8b, 0x4a, 0x10, 0x48, 0x85,
		0xc9, 0x74, 0x10, 0x41, 0x52, 0x50, 0xe8, 0x9a, 0xff, 0xff, 0xff, 0x48, 0x85, 0xc0, 0x75, 0x0e, 0x58, 0x41, 0x5a, 0x49, 0x83, 0xc2, 0x20, 0xff,
		0xc8, 0xeb, 0xdb, 0x31, 0xc0, 0xc3, 0x48, 0x83, 0xc4, 0x10, 0xc3, 0x48, 0x8b, 0x4d, 0xf8, 0x48, 0x8b, 0x55, 0xf0, 0x48, 0x89, 0xec, 0x5d, 0xe9,
		0x00, 0x00, 0x00, 0x00};
	return code;
}

// GetObject on a bitmap, read from the bitmap table the runtime keeps (see NativeWindows::publish_bitmaps): the size with no buffer,
// nothing for a buffer under 32 bytes, else the BITMAP. A DIB section asked for its DIBSECTION, or another object, goes to the runtime.
static constexpr size_t bitmap_pointer = 0x728;

static const std::vector<uint8_t> &bitmap_object_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x64, 0x4d, 0x8b, 0x9a, 0x28, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x58,
		0x48, 0x85, 0xc9, 0x74, 0x53, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0x6b, 0xc0, 0x30, 0x49, 0x01, 0xc3, 0x49, 0x3b, 0x0b, 0x75, 0x41, 0xb8,
		0x20, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xc0, 0x74, 0x33, 0x83, 0xfa, 0x20, 0x7c, 0x2f, 0x41, 0xf7, 0x43, 0x08, 0x01, 0x00, 0x00, 0x00, 0x74, 0x05,
		0x83, 0xfa, 0x68, 0x7d, 0x23, 0x4d, 0x8b, 0x53, 0x10, 0x4d, 0x89, 0x10, 0x4d, 0x8b, 0x53, 0x18, 0x4d, 0x89, 0x50, 0x08, 0x4d, 0x8b, 0x53, 0x20,
		0x4d, 0x89, 0x50, 0x10, 0x4d, 0x8b, 0x53, 0x28, 0x4d, 0x89, 0x50, 0x18, 0xc3, 0x31, 0xc0, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// Services with nothing behind them but their arguments, answered by the guest as the runtime answers them: MulDiv rounding half away
// from zero and failing with -1 for a zero divisor or a result beyond 32 bits, OffsetRect, SetRect and IntersectRect (an empty
// intersection clears the destination and returns FALSE), PathFindFileNameW after the last separator or drive colon, and GetWindowTheme
// without themes. A null pointer goes to the runtime.
static const std::vector<uint8_t> &muldiv_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x45, 0x85, 0xc0, 0x74, 0x48, 0x48, 0x63, 0xc1, 0x4c, 0x63, 0xda, 0x49, 0x0f, 0xaf, 0xc3, 0x4d, 0x63,
		0xd8, 0x4d, 0x89, 0xd9, 0x49, 0xf7, 0xd9, 0x4d, 0x0f, 0x48, 0xcb, 0x49, 0x89, 0xc2, 0x49, 0xf7, 0xda, 0x4c, 0x0f, 0x48, 0xd0, 0x4c, 0x31, 0xd8,
		0x49, 0x89, 0xc3, 0x4c, 0x89, 0xc8, 0x48, 0xd1, 0xe8, 0x4c, 0x01, 0xd0, 0x31, 0xd2, 0x49, 0xf7, 0xf1, 0x4d, 0x85, 0xdb, 0x79, 0x03, 0x48, 0xf7,
		0xd8, 0x4c, 0x63, 0xd0, 0x49, 0x39, 0xc2, 0x75, 0x03, 0x89, 0xc0, 0xc3, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &offset_rect_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x13, 0x01, 0x11, 0x01, 0x51, 0x08, 0x44, 0x01, 0x41, 0x04, 0x44, 0x01, 0x41,
		0x0c, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &set_rect_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x17, 0x89, 0x11, 0x44, 0x89, 0x41, 0x04, 0x44, 0x89, 0x49, 0x08, 0x8b, 0x44,
		0x24, 0x28, 0x89, 0x41, 0x0c, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &intersect_rect_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x62, 0x48, 0x85, 0xd2, 0x74, 0x5d, 0x4d, 0x85, 0xc0, 0x74, 0x58, 0x44, 0x8b,
		0x0a, 0x45, 0x3b, 0x08, 0x45, 0x0f, 0x4c, 0x08, 0x44, 0x8b, 0x52, 0x04, 0x45, 0x3b, 0x50, 0x04, 0x45, 0x0f, 0x4c, 0x50, 0x04, 0x44, 0x8b, 0x5a,
		0x08, 0x45, 0x3b, 0x58, 0x08, 0x45, 0x0f, 0x4f, 0x58, 0x08, 0x8b, 0x42, 0x0c, 0x41, 0x3b, 0x40, 0x0c, 0x41, 0x0f, 0x4f, 0x40, 0x0c, 0x45, 0x39,
		0xcb, 0x7e, 0x19, 0x44, 0x39, 0xd0, 0x7e, 0x14, 0x44, 0x89, 0x09, 0x44, 0x89, 0x51, 0x04, 0x44, 0x89, 0x59, 0x08, 0x89, 0x41, 0x0c, 0xb8, 0x01,
		0x00, 0x00, 0x00, 0xc3, 0x31, 0xc0, 0x48, 0x89, 0x01, 0x48, 0x89, 0x41, 0x08, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &window_theme_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x31, 0xc0, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &file_name_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x42, 0x48, 0x89, 0xc8, 0x49, 0x89, 0xca, 0x45, 0x0f, 0xb7, 0x1a, 0x45, 0x85,
		0xdb, 0x74, 0x32, 0x41, 0x83, 0xfb, 0x5c, 0x74, 0x0c, 0x41, 0x83, 0xfb, 0x2f, 0x74, 0x06, 0x41, 0x83, 0xfb, 0x3a, 0x75, 0x1a, 0x45, 0x0f, 0xb7,
		0x4a, 0x02, 0x45, 0x85, 0xc9, 0x74, 0x10, 0x41, 0x83, 0xf9, 0x5c, 0x74, 0x0a, 0x41, 0x83, 0xf9, 0x2f, 0x74, 0x04, 0x49, 0x8d, 0x42, 0x02, 0x49,
		0x83, 0xc2, 0x02, 0xeb, 0xc5, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// CallWindowProc calls the procedure it is given with the other four arguments, as the runtime does: the guest moves them down a
// register and jumps to it, so the procedure returns to the caller. A null procedure goes to the runtime.
static const std::vector<uint8_t> &call_procedure_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x13, 0x48, 0x89, 0xc8, 0x48, 0x89, 0xd1, 0x4c, 0x89, 0xc2, 0x4d, 0x89, 0xc8, 0x4c, 0x8b, 0x4c, 0x24, 0x28, 0xff, 0xe0, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// GetSystemMetrics and GetSysColor, which do not change while the process runs, read from the table the runtime writes once (see
// NativeWindows::set_system_table) for indexes below 128 and 64.
static constexpr size_t system_pointer = 0x730;

static const std::vector<uint8_t> &system_metrics_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x1b, 0x4d, 0x8b, 0x9a, 0x30, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x0f,
		0x81, 0xf9, 0x80, 0x00, 0x00, 0x00, 0x73, 0x07, 0x89, 0xc8, 0x41, 0x8b, 0x04, 0x83, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &system_colors_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x1c, 0x4d, 0x8b, 0x9a, 0x30, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x10,
		0x83, 0xf9, 0x40, 0x73, 0x0b, 0x89, 0xc8, 0x41, 0x8b, 0x84, 0x83, 0x00, 0x02, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// The message loop's helpers for the messages they leave alone, answered as the runtime answers them: TranslateMessage for a message
// that is not a key, TranslateAcceleratorW for one that is not a key or character with a window and an accelerator table the runtime
// knows (it publishes the tables beside the system metrics), CallMsgFilterW without message filter hooks (a flag the runtime
// publishes at 0x738), and GetDesktopWindow once the desktop window exists (its handle at 0x740).
static const std::vector<uint8_t> &translate_message_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x10, 0x8b, 0x41, 0x08, 0x2d, 0x00, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x09, 0x76,
		0x03, 0x31, 0xc0, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &message_filter_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x12, 0x41, 0x8b, 0x82, 0x38, 0x07, 0x00, 0x00, 0xa8, 0x01, 0x74, 0x07, 0xa8,
		0x02, 0x75, 0x03, 0x31, 0xc0, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &desktop_window_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x0d, 0x49, 0x8b, 0x82, 0x40, 0x07, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x01,
		0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &accelerator_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x6f, 0x4d, 0x85, 0xc0, 0x74, 0x6a, 0x41, 0x8b, 0x40, 0x08, 0x3d, 0x00, 0x01,
		0x00, 0x00, 0x74, 0x5f, 0x3d, 0x02, 0x01, 0x00, 0x00, 0x74, 0x58, 0x3d, 0x04, 0x01, 0x00, 0x00, 0x74, 0x51, 0x4d, 0x8b, 0x9a, 0x00, 0x07, 0x00,
		0x00, 0x4d, 0x85, 0xdb, 0x74, 0x45, 0x48, 0x85, 0xc9, 0x74, 0x40, 0x89, 0xc8, 0x25, 0xff, 0x01, 0x00, 0x00, 0xc1, 0xe0, 0x08, 0x49, 0x3b, 0x0c,
		0x03, 0x75, 0x30, 0x4d, 0x8b, 0x9a, 0x30, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x24, 0x48, 0x85, 0xd2, 0x74, 0x1f, 0x41, 0x8b, 0x83, 0x00,
		0x04, 0x00, 0x00, 0x83, 0xf8, 0x40, 0x77, 0x13, 0x85, 0xc0, 0x74, 0x0f, 0xff, 0xc8, 0x49, 0x3b, 0x94, 0xc3, 0x08, 0x04, 0x00, 0x00, 0x75, 0xf0,
		0x31, 0xc0, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// Device context attributes the guest sets and reads in the attribute table the runtime keeps (see NativeWindows::apply_dc_log): text
// and background colors, background mode and text alignment, each setter logging the context for the runtime. A context without a
// slot, the published screen context, an invalid value or a full log goes to the runtime.
static constexpr size_t dc_pointer = 0x748;

static const std::vector<uint8_t> &dc_text_color_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x57, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x4b,
		0x48, 0x85, 0xc9, 0x74, 0x46, 0x49, 0x3b, 0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x3d, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05,
		0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x26, 0xf7, 0xc2, 0x00, 0x00, 0x00, 0xff, 0x75, 0x1e, 0x49, 0x89, 0xc2,
		0x41, 0x8b, 0x03, 0x83, 0xf8, 0x40, 0x73, 0x13, 0x49, 0x89, 0x4c, 0xc3, 0x08, 0xff, 0xc0, 0x41, 0x89, 0x03, 0x41, 0x8b, 0x42, 0x08, 0x41, 0x89,
		0x52, 0x08, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &dc_background_color_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x57, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x4b,
		0x48, 0x85, 0xc9, 0x74, 0x46, 0x49, 0x3b, 0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x3d, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05,
		0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x26, 0xf7, 0xc2, 0x00, 0x00, 0x00, 0xff, 0x75, 0x1e, 0x49, 0x89, 0xc2,
		0x41, 0x8b, 0x03, 0x83, 0xf8, 0x40, 0x73, 0x13, 0x49, 0x89, 0x4c, 0xc3, 0x08, 0xff, 0xc0, 0x41, 0x89, 0x03, 0x41, 0x8b, 0x42, 0x0c, 0x41, 0x89,
		0x52, 0x0c, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &dc_background_mode_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x59, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x4d,
		0x48, 0x85, 0xc9, 0x74, 0x48, 0x49, 0x3b, 0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x3f, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05,
		0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x28, 0x83, 0xfa, 0x01, 0x74, 0x05, 0x83, 0xfa, 0x02, 0x75, 0x1e, 0x49,
		0x89, 0xc2, 0x41, 0x8b, 0x03, 0x83, 0xf8, 0x40, 0x73, 0x13, 0x49, 0x89, 0x4c, 0xc3, 0x08, 0xff, 0xc0, 0x41, 0x89, 0x03, 0x41, 0x8b, 0x42, 0x10,
		0x41, 0x89, 0x52, 0x10, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &dc_text_align_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x4f, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x43,
		0x48, 0x85, 0xc9, 0x74, 0x3e, 0x49, 0x3b, 0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x35, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05,
		0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x1e, 0x49, 0x89, 0xc2, 0x41, 0x8b, 0x03, 0x83, 0xf8, 0x40, 0x73, 0x13,
		0x49, 0x89, 0x4c, 0xc3, 0x08, 0xff, 0xc0, 0x41, 0x89, 0x03, 0x41, 0x8b, 0x42, 0x14, 0x41, 0x89, 0x52, 0x14, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &dc_get_text_color_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x35, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x29,
		0x48, 0x85, 0xc9, 0x74, 0x24, 0x49, 0x3b, 0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x1b, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05,
		0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x04, 0x8b, 0x40, 0x08, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &dc_get_background_color_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x35, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x29,
		0x48, 0x85, 0xc9, 0x74, 0x24, 0x49, 0x3b, 0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x1b, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05,
		0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x04, 0x8b, 0x40, 0x0c, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &dc_get_background_mode_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x35, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x29,
		0x48, 0x85, 0xc9, 0x74, 0x24, 0x49, 0x3b, 0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x1b, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05,
		0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x04, 0x8b, 0x40, 0x10, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &dc_get_text_align_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x35, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x29,
		0x48, 0x85, 0xc9, 0x74, 0x24, 0x49, 0x3b, 0x8a, 0x00, 0x06, 0x00, 0x00, 0x74, 0x1b, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05,
		0x49, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x48, 0x3b, 0x08, 0x75, 0x04, 0x8b, 0x40, 0x14, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

// Releases the guest answers itself and logs for the runtime (see NativeWindows::take_deferred): DestroyIcon of a valid icon, DeleteDC of
// a memory context, and DeleteObject of a bitmap the runtime marked as free to release. Anything else, or a full log, goes to the runtime.
static constexpr size_t deferred_pointer = 0x750, icons_pointer = 0x758;

static const std::vector<uint8_t> &destroy_icon_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x5e, 0x4d, 0x8b, 0x9a, 0x58, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x52,
		0x48, 0x85, 0xc9, 0x74, 0x4d, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0x4d, 0x8d, 0x04, 0xc3, 0x49, 0x3b, 0x08, 0x75, 0x3d, 0x4d, 0x8b, 0x9a,
		0x50, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x31, 0x41, 0x8b, 0x03, 0x3d, 0xff, 0x00, 0x00, 0x00, 0x73, 0x27, 0x41, 0x89, 0xc1, 0x49, 0xc1,
		0xe1, 0x04, 0x4b, 0x89, 0x4c, 0x0b, 0x10, 0x4b, 0xc7, 0x44, 0x0b, 0x18, 0x01, 0x00, 0x00, 0x00, 0xff, 0xc0, 0x41, 0x89, 0x03, 0x49, 0xc7, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &delete_dc_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x6f, 0x4d, 0x8b, 0x9a, 0x48, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x63,
		0x48, 0x85, 0xc9, 0x74, 0x5e, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0xc1, 0xe0, 0x05, 0x4d, 0x8d, 0x84, 0x03, 0x00, 0x10, 0x00, 0x00, 0x49,
		0x3b, 0x08, 0x75, 0x47, 0x41, 0xf7, 0x40, 0x18, 0x01, 0x00, 0x00, 0x00, 0x75, 0x3d, 0x4d, 0x8b, 0x9a, 0x50, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb,
		0x74, 0x31, 0x41, 0x8b, 0x03, 0x3d, 0xff, 0x00, 0x00, 0x00, 0x73, 0x27, 0x41, 0x89, 0xc1, 0x49, 0xc1, 0xe1, 0x04, 0x4b, 0x89, 0x4c, 0x0b, 0x10,
		0x4b, 0xc7, 0x44, 0x0b, 0x18, 0x02, 0x00, 0x00, 0x00, 0xff, 0xc0, 0x41, 0x89, 0x03, 0x49, 0xc7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x01, 0x00,
		0x00, 0x00, 0xc3, 0xe9, 0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> &delete_object_prelude()
{
	static const std::vector<uint8_t> code{
		0x4c, 0x8b, 0x15, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xd2, 0x74, 0x6b, 0x4d, 0x8b, 0x9a, 0x28, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x5f,
		0x48, 0x85, 0xc9, 0x74, 0x5a, 0x89, 0xc8, 0x25, 0xff, 0x03, 0x00, 0x00, 0x6b, 0xc0, 0x30, 0x4d, 0x8d, 0x04, 0x03, 0x49, 0x3b, 0x08, 0x75, 0x47,
		0x41, 0xf7, 0x40, 0x08, 0x02, 0x00, 0x00, 0x00, 0x74, 0x3d, 0x4d, 0x8b, 0x9a, 0x50, 0x07, 0x00, 0x00, 0x4d, 0x85, 0xdb, 0x74, 0x31, 0x41, 0x8b,
		0x03, 0x3d, 0xff, 0x00, 0x00, 0x00, 0x73, 0x27, 0x41, 0x89, 0xc1, 0x49, 0xc1, 0xe1, 0x04, 0x4b, 0x89, 0x4c, 0x0b, 0x10, 0x4b, 0xc7, 0x44, 0x0b,
		0x18, 0x03, 0x00, 0x00, 0x00, 0xff, 0xc0, 0x41, 0x89, 0x03, 0x49, 0xc7, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0xe9,
		0x00, 0x00, 0x00, 0x00};
	return code;
}

static const std::vector<uint8_t> *block_prelude(std::string_view module, std::string_view name)
{
	static constexpr std::array<BlockPrelude, 5> ticks{{{"kernel32.dll", "GetTickCount"}, {"kernel32.dll", "GetTickCount64"},
		{"kernelbase.dll", "GetTickCount"}, {"kernelbase.dll", "GetTickCount64"}, {"winmm.dll", "timeGetTime"}}};
	for (const auto &prelude : ticks)
		if (prelude.module == module && prelude.name == name)
			return &tick_prelude();
	if ((module == "kernel32.dll" || module == "kernelbase.dll") && (name == "GetLocaleInfoEx" || name == "GetLocaleInfoW"))
		return &locale_prelude();
	if (module == "ntdll.dll" && (name == "NtAllocateVirtualMemory" || name == "ZwAllocateVirtualMemory"))
		return &nt_commit_prelude();
	if (module == "ntdll.dll" && (name == "NtFreeVirtualMemory" || name == "ZwFreeVirtualMemory"))
		return &nt_decommit_prelude();
	if ((module == "kernel32.dll" || module == "kernelbase.dll") && name == "MulDiv")
		return &muldiv_prelude();
	if ((module == "shlwapi.dll" || module == "kernelbase.dll") && name == "PathFindFileNameW")
		return &file_name_prelude();
	// Without visual styles no window has a theme, and the answer needs no trip to the runtime.
	if (module == "uxtheme.dll" && name == "GetWindowTheme" && [] {
			const char *chosen = std::getenv("RAVEN_THEME");
			return chosen && std::string_view(chosen) == "classic";
		}())
		return &window_theme_prelude();
	if (module == "user32.dll" && name == "TranslateMessage")
		return &translate_message_prelude();
	if (module == "user32.dll" && name == "CallMsgFilterW")
		return &message_filter_prelude();
	if (module == "user32.dll" && name == "GetDesktopWindow")
		return &desktop_window_prelude();
	if (module == "user32.dll" && name == "TranslateAcceleratorW")
		return &accelerator_prelude();
	if (module == "user32.dll" && name == "GetSystemMetrics")
		return &system_metrics_prelude();
	if (module == "user32.dll" && name == "GetSysColor")
		return &system_colors_prelude();
	if (module == "user32.dll" && (name == "CallWindowProcW" || name == "CallWindowProcA"))
		return &call_procedure_prelude();
	if (module == "user32.dll" && name == "OffsetRect")
		return &offset_rect_prelude();
	if (module == "user32.dll" && name == "SetRect")
		return &set_rect_prelude();
	if (module == "user32.dll" && name == "IntersectRect")
		return &intersect_rect_prelude();
	if (module == "gdi32.dll") {
		static constexpr std::array<std::pair<std::string_view, const std::vector<uint8_t> &(*)()>, 8> attributes{{
			{"SetTextColor", dc_text_color_prelude}, {"SetBkColor", dc_background_color_prelude}, {"SetBkMode", dc_background_mode_prelude},
			{"SetTextAlign", dc_text_align_prelude}, {"GetTextColor", dc_get_text_color_prelude}, {"GetBkColor", dc_get_background_color_prelude},
			{"GetBkMode", dc_get_background_mode_prelude}, {"GetTextAlign", dc_get_text_align_prelude}}};
		for (const auto &[attribute, prelude] : attributes)
			if (name == attribute)
				return &prelude();
	}
	if (module == "user32.dll" && name == "DestroyIcon")
		return &destroy_icon_prelude();
	if (module == "gdi32.dll" && name == "DeleteDC")
		return &delete_dc_prelude();
	if (module == "gdi32.dll" && name == "DeleteObject")
		return &delete_object_prelude();
	if (module == "gdi32.dll" && (name == "GetObjectW" || name == "GetObjectA"))
		return &bitmap_object_prelude();
	if (module == "user32.dll" && name == "GetDC")
		return &screen_get_dc_prelude();
	if (module == "user32.dll" && name == "ReleaseDC")
		return &screen_release_dc_prelude();
	if (module == "gdi32.dll" && name == "GetDeviceCaps")
		return &screen_device_caps_prelude();
	if (module != "user32.dll")
		return nullptr;
	if (name == "GetWindowLongPtrW" || name == "GetWindowLongPtrA")
		return &window_long_ptr_prelude();
	if (name == "GetWindowLongW" || name == "GetWindowLongA")
		return &window_long_prelude();
	if (name == "GetPropW")
		return &window_property_prelude();
	if (name == "IsWindow" || name == "IsWindowUnicode")
		return &window_exists_prelude();
	if (name == "GetClientRect")
		return &window_client_prelude();
	if (name == "GetWindowRect")
		return &window_outer_prelude();
	if (name == "IsChild")
		return &window_child_prelude();
	if (name == "GetDpiForWindow")
		return &window_dpi_prelude();
	if (name == "GetFocus")
		return &focus_prelude();
	if (name == "GetMenuItemCount")
		return &menu_count_prelude();
	if (name == "GetSubMenu")
		return &menu_submenu_prelude();
	if (name == "GetMenuItemID")
		return &menu_id_prelude();
	if (name == "GetMenuStringW")
		return &menu_string_prelude();
	if (name == "GetMenuState")
		return &menu_state_prelude();
	return nullptr;
}

static size_t string_prelude_slot(const GuestPrelude &prelude)
{
	return (prelude.code.size() + 63) & ~size_t(63);
}

static std::vector<uint8_t> commit_prelude(int32_t to_pointer, int32_t to_trap)
{
	// mov r10,[pointer]; test r10,r10; jz slow; test rcx,rcx; jz slow; test rdx,rdx; jz slow; cmp r8d,MEM_COMMIT; jne slow;
	// cmp r9d,PAGE_READWRITE; je main; slow: jmp trap; restore: mov r8d,MEM_COMMIT; mov r9d,PAGE_READWRITE; jmp slow;
	// main: rax = first page, r11 = end page; jbe slow; look through the open ranges at commit_windows for one holding [rax, r11),
	// take a log slot with lock xadd at commit_log, store the range there and return rax; a full log or no range goes back to restore.
	std::vector<uint8_t> code{0x4c, 0x8b, 0x15, 0, 0, 0, 0, 0x4d, 0x85, 0xd2, 0x74, 0x19, 0x48, 0x85, 0xc9, 0x74, 0x14, 0x48, 0x85, 0xd2,
		0x74, 0x0f, 0x41, 0x81, 0xf8, 0x00, 0x10, 0x00, 0x00, 0x75, 0x06, 0x41, 0x83, 0xf9, 0x04, 0x74, 0x13, 0xe9, 0, 0, 0, 0, 0x41,
		0xb8, 0x00, 0x10, 0x00, 0x00, 0x41, 0xb9, 0x04, 0x00, 0x00, 0x00, 0xeb, 0xed, 0x48, 0x89, 0xc8, 0x48, 0x25, 0x00, 0xf0, 0xff,
		0xff, 0x4c, 0x8d, 0x9c, 0x11, 0xff, 0x0f, 0x00, 0x00, 0x49, 0x81, 0xe3, 0x00, 0xf0, 0xff, 0xff, 0x49, 0x39, 0xc3, 0x76, 0xd0,
		0x45, 0x8b, 0x82, 0x00, 0x0a, 0x00, 0x00, 0x4d, 0x8d, 0x8a, 0x08, 0x0a, 0x00, 0x00, 0x45, 0x85, 0xc0, 0x74, 0xc2, 0x49, 0x3b,
		0x01, 0x72, 0x30, 0x4d, 0x3b, 0x59, 0x08, 0x77, 0x2a, 0x41, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x45, 0x0f, 0xc1, 0x82, 0x10,
		0x0d, 0x00, 0x00, 0x41, 0x83, 0xf8, 0x20, 0x73, 0xa2, 0x49, 0xc1, 0xe0, 0x04, 0x4b, 0x89, 0x84, 0x02, 0x18, 0x0d, 0x00, 0x00,
		0x4f, 0x89, 0x9c, 0x02, 0x20, 0x0d, 0x00, 0x00, 0xc3, 0x49, 0x83, 0xc1, 0x10, 0x41, 0xff, 0xc8, 0xeb, 0xbd};
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

static uint32_t nt_service_number(std::string_view module, std::string_view name)
{
	if (module != "ntdll.dll" || (!name.starts_with("Nt") && !name.starts_with("Zw")))
		return 0;
	const std::string canonical = "Nt" + std::string(name.substr(2));
	const auto names = system_export_names("ntdll.dll");
	const auto found = std::lower_bound(names.begin(), names.end(), canonical);
	// Use the complete name table so a copied service keeps its identity across different process import graphs.
	return found != names.end() && *found == canonical ? 0x10000 + uint32_t(found - names.begin()) : 0;
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
	if (!x86)
		for (const auto symbol : exports)
			served_count += served_api(name, symbol);
	size_t clock_count = 0;
	if (!x86)
		for (const auto symbol : exports)
			clock_count += guest_clock_api(name, symbol);
	size_t commit_count = 0;
	if (!x86 && served_count)
		for (const auto symbol : exports)
			commit_count += guest_commit_api(name, symbol);
	size_t block_size = 0;
	if (fast_paths && !x86)
		for (const auto symbol : exports)
			if (const auto *prelude = block_prelude(name, symbol))
				block_size += (prelude->size() + 63) & ~size_t(63);
	// The block pointer of the served stubs and the block preludes, then the stubs, then the clock preludes, then the commit preludes.
	const size_t served_start = fast_start + fast_count * 64;
	const size_t clock_start = served_start + (served_count || block_size ? 64 + served_count * served_stub_size : 0);
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
	const size_t end = queued_start + queued_count * queued_prelude_size;
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
	size_t queued_next = queued_start;
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
			const std::array<uint8_t, 11> entry{0x4c, 0x8b, 0xd1, 0xb8, 0, 0, 0, 0, 0x0f, 0x05, 0xc3};
			std::copy(entry.begin(), entry.end(), bytes.begin() + ptrdiff_t(code));
			put(code + 4, service);
		}
		if (!x86 && served_api(name, exports[index])) {
			code = served_start + 64 + served_next++ * served_stub_size;
			const auto stub = served_stub(int32_t(served_start) - int32_t(code));
			std::copy(stub.begin(), stub.end(), bytes.begin() + ptrdiff_t(code));
			if (guest_clock_api(name, exports[index])) {
				const size_t served = code;
				code = clock_start + clock_next++ * clock_prelude_size;
				const auto prelude = clock_prelude(int32_t(served_start + 8) - int32_t(code), int32_t(served) - int32_t(code));
				std::copy(prelude.begin(), prelude.end(), bytes.begin() + ptrdiff_t(code));
			}
		}
		if (const auto *prelude = fast_paths && !x86 && !service ? block_prelude(name, exports[index]) : nullptr) {
			const size_t trap = code;
			code = blocks_next;
			blocks_next += (prelude->size() + 63) & ~size_t(63);
			auto bytes_of = *prelude;
			const int32_t displacement = int32_t(served_start) - int32_t(code) - 7;
			std::memcpy(bytes_of.data() + 3, &displacement, 4);
			const int32_t relative = int32_t(trap) - int32_t(code + bytes_of.size());
			std::memcpy(bytes_of.data() + bytes_of.size() - 4, &relative, 4);
			std::copy(bytes_of.begin(), bytes_of.end(), bytes.begin() + ptrdiff_t(code));
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
		if (queued && queued_arguments(name, exports[index])) {
			const size_t served = code;
			code = queued_next;
			queued_next += queued_prelude_size;
			const auto prelude = queued_prelude(int32_t(served) - int32_t(code), queued_arguments(name, exports[index]), exports[index]);
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

static uint64_t provider_base(const ModuleSet &modules, uint32_t size)
{
	uint64_t base = 0x6fff00000000ULL;
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
	std::vector<std::string_view> native_modules{"kernel32.dll","kernelbase.dll","user32.dll","gdi32.dll","comctl32.dll","ntdll.dll",
		"advapi32.dll","ole32.dll","combase.dll","shell32.dll","shlwapi.dll","setupapi.dll","comdlg32.dll"};
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
		const auto bytes = runtime_provider("opengl32.dll", 0x180000000ULL, symbols, false, {}, {}, guest_paths());
		const auto base = provider_base(modules, PEImage(bytes).image_size);
		auto &provider = modules.add("opengl32.dll", runtime_provider("opengl32.dll", base, symbols, false, {}, {}, guest_paths()), base);
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
	for (const auto name : {"NtSetInformationThread", "NtOpenThreadToken", "NtOpenThreadTokenEx", "NtOpenThread", "NtOpenProcess", "NtOpenProcessToken", "NtOpenProcessTokenEx", "NtQueryInformationProcess", "NtReadVirtualMemory", "NtWriteVirtualMemory", "NtQuerySection", "NtFlushInstructionCache", "RtlAddGrowableFunctionTable", "RtlGrowFunctionTable", "RtlDeleteGrowableFunctionTable",
		"NtWriteFile", "NtQueryDirectoryFile", "ZwQueryDirectoryFile", "RtlNtStatusToDosError", "NtQueryObject", "NtMapViewOfSection", "NtUnmapViewOfSection",
		"RtlAcquireSRWLockExclusive", "RtlAcquireSRWLockShared", "RtlWakeConditionVariable", "RtlWakeAllConditionVariable", "RtlWaitOnAddress",
		"RtlWakeAddressSingle", "RtlWakeAddressAll",
		// What the runtime's own components import from ntdll, which they need whether a program loads them at start or later.
		"DbgPrint", "NtCancelIoFile", "NtCreateSection", "NtFsControlFile", "NtReadFile", "RtlCreateUnicodeStringFromAsciiz", "RtlFreeHeap",
		"RtlFreeUnicodeString", "RtlDllShutdownInProgress", "RtlGetVersion", "RtlIpv6AddressToStringExW", "RtlIpv6StringToAddressW", "RtlLengthSid", "RtlUnicodeToMultiByteN",
		"RtlUnicodeToMultiByteSize", "RtlIsTextUnicode", "RtlImageNtHeader", "NtDeleteKey",
		// The unwinding of 64-bit frames, which libraries built with their own exception handling import.
		"RtlCaptureContext", "RtlLookupFunctionEntry", "RtlPcToFileHeader", "RtlVirtualUnwind", "RtlUnwindEx",
		// What the .NET runtime imports from ntdll: the function tables of the code it compiles, and the restoring of a context.
		"VerSetConditionMask", "RtlInstallFunctionTableCallback", "RtlAddFunctionTable", "RtlDeleteFunctionTable", "RtlRestoreContext",
		// What the native layer of WPF imports from ntdll.
		"DbgPrintEx", "DbgPrompt", "RtlVerifyVersionInfo", "RtlInitializeBitMap", "RtlSetBits", "RtlClearBits", "RtlSetAllBits",
		"RtlClearAllBits", "RtlAreBitsClear", "RtlAreBitsSet", "RtlFindClearBits", "RtlFindClearBitsAndSet", "RtlNumberOfSetBits",
		"RtlInitializeSListHead", "RtlInterlockedPushEntrySList", "RtlInterlockedPopEntrySList", "RtlInterlockedFlushSList",
		"RtlQueryDepthSList",
		// What Wine's builds of the Direct3D 10.1 and Direct2D libraries import from ntdll.
		"_vsnprintf",
		// The registry natives, which libraries loaded after start (the Click-to-Run integration of Office among them) import by name.
		"NtCreateKey", "NtOpenKey", "NtOpenKeyEx", "NtQueryKey", "NtQueryValueKey", "NtSetValueKey", "NtDeleteValueKey", "NtSetInformationKey",
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
	std::set<std::string> beside_program;
	if (launch) {
		const auto probe_prefix = NativePrefix::from_environment();
		const NativePaths probe(probe_prefix);
		const auto program = probe.resolve(launch->image_path);
		if (program && program->exists) {
			std::error_code error;
			for (const auto &item : std::filesystem::directory_iterator(program->host.parent_path(), error)) {
				if (!item.is_regular_file(error))
					continue;
				auto filename = item.path().filename().string();
				for (auto &character : filename)
					if (character >= 'A' && character <= 'Z')
						character = static_cast<char>(character + ('a' - 'A'));
				if (const auto mapped = ModuleBytes::map(item.path())) {
					try {
						const PEImage candidate(*mapped);
						if (candidate.is_64 && (candidate.characteristics & 0x2000))
							beside_program.insert(std::move(filename));
					} catch (...) {}
				}
			}
		}
	}
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
		std::sort(symbols.begin(), symbols.end());
		symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
		if (symbols.empty())
			continue;
		const auto published_numbers = system_export_ordinals(library);
		std::vector<std::pair<std::string_view, uint16_t>> ordinals(published_numbers.begin(), published_numbers.end());
		if (const auto extra = unknown_ordinals.find(std::string(name)); extra != unknown_ordinals.end())
			ordinals.insert(ordinals.end(), extra->second.begin(), extra->second.end());
		PEModule *provider = nullptr;
		const bool core = std::find(std::begin(core_system_libraries), std::end(core_system_libraries), std::string_view(name)) != std::end(core_system_libraries);
		const auto found = modules.all().find(name);
		if (found != modules.all().end() && found->second->runtime_api) {
			provider = found->second.get();
		} else if (found != modules.all().end()) {
			if (core)
				continue;
			const auto fallback = "system32-" + name;
			if (!modules.all().contains(fallback)) {
				const auto bytes = runtime_provider(name, 0x180000000ULL, symbols, false, {}, ordinals, guest_paths());
				const auto base = provider_base(modules, PEImage(bytes).image_size);
				provider = &modules.add(fallback, runtime_provider(name, base, symbols, false, {}, ordinals, guest_paths()), base);
				provider->runtime_api = true;
			}
		} else {
			const auto provider_key = (!core && beside_program.contains(name)) ? "system32-" + name : name;
			const auto bytes = runtime_provider(name, 0x180000000ULL, symbols, false, {}, ordinals, guest_paths());
			const auto base = provider_base(modules, PEImage(bytes).image_size);
			provider = &modules.add(provider_key, runtime_provider(name, base, symbols, false, {}, ordinals, guest_paths()), base);
			provider->runtime_api = true;
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
				// The heap, critical sections and the free paths of SRW locks and condition variables run in the guest RTL.
				{"kernel32.dll", "raven-rtl.dll", "raven-rtl."}, {"kernelbase.dll", "raven-rtl.dll", "raven-rtl."},
				// The generic AVL tables of ntdll; its heap stays with the host, which calls the guest RTL itself.
				{"ntdll.dll", "raven-rtl.dll", "raven-rtl."}};
			for (const auto &overlay_rule : overlays) {
				if (name != overlay_rule.library)
					continue;
				const bool tables_only = name == "ntdll.dll";
				const auto overlay = modules.all().find(overlay_rule.component);
				if (overlay == modules.all().end() || overlay->second->runtime_api)
					continue;
				for (auto &symbol : provider->exports)
					if (!symbol.names.empty() && (!tables_only || symbol.names.at(0).find("GenericTable") != std::string::npos) && std::any_of(overlay->second->exports.begin(), overlay->second->exports.end(), [&](const auto &candidate) {
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
		const auto bytes=runtime_provider("ntdll.dll",0x180000000ULL,nt_names,false,{},{},guest_paths());
		const auto base=provider_base(modules,PEImage(bytes).image_size);
		auto &provider=modules.add("ntdll.dll",runtime_provider("ntdll.dll",base,nt_names,false,{},{},guest_paths()),base);
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
		const auto bytes = runtime_provider("vulkan-1.dll", 0x180000000ULL, VulkanBridge::exports(), false, {}, {}, guest_paths());
		const auto base = provider_base(modules, PEImage(bytes).image_size);
		auto &provider = modules.add("vulkan-1.dll", runtime_provider("vulkan-1.dll", base, VulkanBridge::exports(), false, {}, {}, guest_paths()), base);
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
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	NativeProcess process(images, bootstrap, timeout, NativeMode::x64, entry.image.stack_reserve);
	for (const auto &[address, name] : traps)
		process.add_api_trap(address);
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
		standard_handles[i]=inherited.value;
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
	// What a program may commit: 512 MiB by default, as set by RAVEN_COMMIT_LIMIT_MB (large programs, a browser engine for one, need more).
	const size_t commit_limit=(std::getenv("RAVEN_COMMIT_LIMIT_MB") ? size_t(std::strtoull(std::getenv("RAVEN_COMMIT_LIMIT_MB"),nullptr,10)) : size_t(512))*1024*1024;
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
	desktop.set_updated([&](uint64_t window) { windows.drawing(window); });
#ifdef RAVEN_VULKAN
	VulkanBridge vulkan(process, modules);
	vulkan.set_surface_source({[&windows](uint64_t window) { return windows.host_surface(window); },
		[&windows](uint64_t window) { windows.release_host_surface(window); },
		[&windows](uint64_t window) { return windows.client_size(window); }, windows.host_platform()});
#endif
	const auto remaining = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()); };
	size_t debug_bytes=0;
	KernelContext *kernel_ptr=nullptr;
	NativeThreads *threads_ptr=nullptr;
	auto last_service=std::chrono::steady_clock::now();
	const auto invoke = [&](uint64_t address, std::span<const uint64_t> arguments) {
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
				process.enter_nt_service(result.instruction, uint32_t(result.value));
			}
			const auto found = traps.find(result.value);
			if (!service_name && found == traps.end())
				return result;
			const auto &api_name = service_name ? *service_name : found->second;
			if (api_name == "ExitProcess") {
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
			if (tracing) {
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
				std::cerr << '\n';
			}
			if (const char *spec = std::getenv("RAVEN_STEP")) {
				// Diagnosis aid: NAME:OCCURRENCE:COUNT single-steps COUNT instructions after the OCCURRENCE-th return of NAME.
				static std::map<std::string,int> seen;
				const std::string text(spec);
				const auto first = text.find(':'), second = text.find(':', first + 1);
				if (first != text.npos && second != text.npos && api_name == text.substr(0, first) &&
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
			windows.apply_dc_log();
			windows.take_deferred([&desktop](uint32_t operation, uint64_t handle) { desktop.apply_deferred(operation, handle); });
			windows.screen_dc_use(api_name);
			const auto profile_start = RuntimeProfile::instance().enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			static const bool trace_failures = std::getenv("RAVEN_TRACE_FAIL");
			const auto error_before = trace_failures ? environment.last_error() : 0;
			const auto failure_arguments = trace_failures ? process.api_arguments(4) : std::vector<uint64_t>{};
			try {
				const WindowCallScope window_scope(windows);
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
			// Calls that stopped the guest are counted apart from served ones: a stop costs far more than the call.
			if (RuntimeProfile::instance().enabled() && profile_start != std::chrono::steady_clock::time_point{})
				RuntimeProfile::instance().api("(stop) " + api_name, std::chrono::steady_clock::now() - profile_start);
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
				std::cerr << "  -> " << std::hex << process.result_register() << std::dec << " (error " << environment.last_error() << ")\n";
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
						const auto raw = process.read_memory(failure_arguments[name_index], 240);
						for (size_t i = 0; i < 120 && (raw[i * 2] | raw[i * 2 + 1]); ++i)
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
		threads.block(std::move(ready),until);
	});
	windows.set_idle_sources([&threads](std::chrono::steady_clock::time_point limit) { return threads.idle_until(limit); },
		[&threads](std::vector<pollfd> &descriptors) { threads.idle_descriptors(descriptors); }, [&threads] { threads.drain_wake(); });
	KernelContext kernel{process, environment, memory, allocator, kernel_objects, threads, paths, modules, entry, *launch, invoke};
	threads.set_watched([reactor = kernel.reactor](std::vector<pollfd> &descriptors) { reactor->watch(descriptors); });
	windows.set_global_allocator([&allocator](uint64_t size) { return allocator.allocate(size,true); });
	kernel.queue_status=[&windows](uint32_t thread,uint32_t mask,bool clear) { return windows.queue_status(thread,mask,clear); };
	windows.set_block([&threads](std::function<bool()> ready) {
		HostWait waiting;
		threads.block(std::move(ready), std::nullopt);
	});
#ifdef RAVEN_VULKAN
	vulkan.set_blocking([&threads](std::function<bool()> ready) {
		HostWait waiting;
		threads.block(std::move(ready), std::nullopt);
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
	// The pointers live in the module images, which outlast this process: they are cleared when it ends so that another process mapping
	// the same images starts with its stubs trapping.
	struct ServedPointers {
		std::vector<uint8_t *> places;
		std::vector<std::pair<uint8_t *, std::array<uint8_t, 12>>> entries;
		~ServedPointers()
		{
			for (auto *place : places)
				std::memset(place, 0, 8);
			for (const auto &[place, bytes] : entries)
				std::copy(bytes.begin(), bytes.end(), place);
		}
	} served_pointers;
	for (const auto &[module_key, module] : modules.all()) {
		if (!module->runtime_api)
			continue;
		const std::string_view module_name = module_key.starts_with("system32-") ? std::string_view(module_key).substr(9) : std::string_view(module_key);
		for (const auto &symbol : module->exports) {
			// The clock and the tick counts only read calibration; their pointers are safe while other threads run.
			if (symbol.address && !symbol.names.empty() && symbol.forwarder.empty() &&
				(guest_clock_api(module_name, symbol.names.at(0)) || block_prelude(module_name, symbol.names.at(0)) == &tick_prelude() ||
					(!process.parallel() && block_prelude(module_name, symbol.names.at(0)))) &&
				served_stub_rva(module->loaded.data(), module_name, symbol.names.at(0), symbol.address) != symbol.address) {
				auto image = module->loaded.data();
				int32_t displacement = 0;
				std::memcpy(&displacement, image.data() + symbol.address + 3, 4);
				std::memcpy(image.data() + symbol.address + 7 + displacement, &served_guest, 8);
				served_pointers.places.push_back(image.data() + symbol.address + 7 + displacement);
			}
			if (!symbol.address || symbol.names.empty() || !symbol.forwarder.empty() || !served_api(module_name, symbol.names.at(0)))
				continue;
			auto image = module->loaded.data();
			const uint32_t stub = served_stub_rva(image, module_name, symbol.names.at(0), symbol.address);
			if (process.parallel()) {
				std::array<uint8_t, 12> previous{};
				std::copy_n(image.data() + stub, previous.size(), previous.begin());
				served_pointers.entries.emplace_back(image.data() + stub, previous);
				// Load this thread's block through GS; keep every completion trap at its original offset.
				const std::array<uint8_t, 12> local{0x65, 0x4c, 0x8b, 0x14, 0x25, 0xf0, 0x1f, 0, 0, 0x90, 0x90, 0x90};
				std::copy(local.begin(), local.end(), image.data() + stub);
			} else {
				int32_t displacement = 0;
				std::memcpy(&displacement, image.data() + stub + 3, 4);
				std::memcpy(image.data() + stub + 7 + displacement, &served_guest, 8);
				served_pointers.places.push_back(image.data() + stub + 7 + displacement);
			}
			served_names.emplace(module->loaded.base() + stub, symbol.names.at(0));
		}
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
	// So must one between taking the screen context and reading which it is, or reading a window's record.
	for (const auto &[module_key, module] : modules.all())
		if (module->runtime_api) {
			const std::string_view module_name = module_key.starts_with("system32-") ? std::string_view(module_key).substr(9) : std::string_view(module_key);
			for (const auto &symbol : module->exports)
				if (symbol.address && !symbol.names.empty() && symbol.forwarder.empty() && (module_name == "user32.dll" || module_name == "ntdll.dll"))
					if (const auto *prelude = block_prelude(module_name, symbol.names.at(0)); prelude && prelude != &screen_release_dc_prelude() &&
						served_stub_rva(module->loaded.data(), module_name, symbol.names.at(0), symbol.address) != symbol.address)
						for (uint64_t piece = 0; piece < prelude->size(); piece += served_stub_size)
							served_stubs.push_back(module->loaded.base() + symbol.address + piece);
		}
	memory.set_commit_block(served_block);
	windows.set_screen_block(served_block);
	static_assert(focus_published == NativeWindows::focus_published && focus_window == NativeWindows::focus_window,
		"the focus prelude reads what NativeWindows publishes");
	// The window table the window preludes read, mapped into the guest and published in the served block.
	const size_t windows_size = NativeWindows::window_records * NativeWindows::window_record, table_size = windows_size + 8192 +
		NativeWindows::menu_area + NativeWindows::bitmap_records * NativeWindows::bitmap_record + 4096 + NativeWindows::dc_table_size + 4096 +
		NativeWindows::icon_slots * 8;
	const int table_file = memfd_create("raven-window-table", MFD_CLOEXEC);
	if (table_file < 0 || ftruncate(table_file, off_t(table_size)) != 0)
		throw std::runtime_error("cannot create the window table");
	auto *table = static_cast<uint8_t *>(mmap(nullptr, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, table_file, 0));
	if (table == MAP_FAILED)
		throw std::runtime_error("cannot map the window table");
	const uint64_t table_guest = process.map_shared(table_file, table_size);
	close(table_file);
	windows.set_window_table(table);
	process.set_resume_hook([&windows] { windows.publish_windows(); });
	__atomic_store_n(&served_block[window_table / 8], table_guest, __ATOMIC_RELEASE);
	// The locale table follows the window table.
	const auto locales = locale_table();
	if (locales.size() > 8192)
		throw std::runtime_error("the locale table does not fit its page");
	std::memcpy(table + windows_size, locales.data(), locales.size());
	__atomic_store_n(&served_block[locale_pointer / 8], table_guest + windows_size, __ATOMIC_RELEASE);
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
	process.set_served_calls(served_block, std::move(served_stubs), served_trap, served_wait, std::move(yield_traps), [&, served_names = std::move(served_names)](uint64_t stub) {
		const auto found = served_names.find(stub);
		if (found == served_names.end())
			throw std::runtime_error("a served call came from an unknown stub");
		const ServedThreadScope thread_scope(environment, process.serving_teb());
		api_in_progress = found->second.c_str();
		// Deferred releases wait for a stop of the guest: releasing a DIB section unmaps guest memory, which a running guest cannot have
		// done to it.
		windows.apply_dc_log();
		windows.screen_dc_use(found->second);
		const WindowCallScope window_scope(windows);
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
	// The guest reads the performance counter itself once the time stamp counter is calibrated against it over 200 ms: the base is taken
	// when published, so the guest's count goes on from the runtime's, and the multiplier is written last.
	struct ClockCalibration {
		uint64_t counter = __rdtsc(), ticks = counter_ticks();
		bool published = false;
	};
	threads.add_service([served_block, calibration = std::make_shared<ClockCalibration>()] {
		if (calibration->published || counter_ticks() - calibration->ticks < 2000000)
			return;
		const uint64_t counter = __rdtsc(), ticks = counter_ticks();
		if (counter <= calibration->counter)
			return;
		uint64_t *published = served_block + clock_calibration / 8;
		published[0] = counter;
		published[1] = ticks;
		const unsigned __int128 multiplier = (static_cast<unsigned __int128>(ticks - calibration->ticks) << 32) / (counter - calibration->counter);
		__atomic_store_n(&published[2], static_cast<uint64_t>(multiplier), __ATOMIC_RELEASE);
		calibration->published = true;
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
	register_crypt32_api(api_registry, kernel);
	register_bcrypt_api(api_registry, kernel);
	register_process_info_api(api_registry, kernel);
	register_font_service(api_registry, kernel);
	register_canvas_service(api_registry, kernel);
	register_sync_api(api_registry, kernel);
	register_thread_api(api_registry, kernel);
	NativeObjectAPI::register_served(api_registry, kernel.objects);
	register_text_api(api_registry, kernel);
	register_console_api(api_registry, kernel);
	register_memory_api(api_registry, kernel);
	register_file_api(api_registry, kernel);
	register_shlwapi_api(api_registry, kernel);
	register_shell_api(api_registry, kernel);
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
	NativeRegistry hive(prefix.registry_file());
	apply_registry_settings(hive);
	paths.set_package_roots([&hive] { return virtual_package_roots(hive); });
	register_registry_api(api_registry, kernel, hive);
	register_service_api(api_registry, kernel, hive);
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
		if ((module.image.characteristics & 0x2000) && module.image.entry) {
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
				const bool core = std::find(std::begin(core_system_libraries), std::end(core_system_libraries), std::string_view(identity)) != std::end(core_system_libraries);
				if (!system_folder && !core && apiset_host(identity).empty()) {
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
				const bool core = std::find(std::begin(core_system_libraries), std::end(core_system_libraries), std::string_view(identity)) != std::end(core_system_libraries);
				if (has_launch && !core && !activation_contexts().library_folder(identity)) {
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
					} else if ((flags & 0x1f00) && !system) {
						throw LoadFailure{126};
					}
				}
				if (!bytes && !(flags & 0x1f00))
					if (auto read = source(identity)) bytes.emplace(std::move(*read));
				if (!bytes && system) {
					if (const auto fallback = modules.all().find("system32-" + identity); fallback != modules.all().end())
						return *fallback->second;
				}
			}
			// A library loaded by path finds its dependencies in its own directory, as LOAD_WITH_ALTERED_SEARCH_PATH and a DLL directory
			// set to that folder do on Windows; programs that unpack themselves to a temporary folder rely on it.
			for (auto file = explicit_files.begin(); !bytes && file != explicit_files.end(); ++file) {
				std::error_code error;
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
			// Reject unsupported initialization before publishing an image.
			const PEImage &image = *parsed;
			const bool only_mapped = unresolved && staged.empty();
			// An image of the other width maps only as data, or when it holds managed code alone, as a 64-bit process maps the 32-bit form
			// of a class library for the runtime; either stays apart from the libraries that run.
			const bool apart = only_mapped && (as_data || (!image.is_64 && image.il_only()));
			if (!image.is_64 && !apart)
				throw LoadFailure{193};
			if (!(image.characteristics & 0x2000) && !only_mapped)
				throw LoadFailure{120};
			if ((image.directories[13].address || image.directories[13].size) && !only_mapped)
				for (const auto &descriptor : image.delay_imports())
					if (!(image.file_span(descriptor.descriptor_address, 4)[0] & 1))
						throw LoadFailure{120};
			// Mixed managed and native code starts through its entry point like any library, which hands it to the runtime; an image
			// of managed code alone is the runtime's to map, unless it is only mapped.
			if (image.il_only() && !(unresolved && staged.empty()))
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
				if (module->image.entry) {
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
			if ((module.image.characteristics & 0x2000) && module.image.entry && !module.runtime_api) {
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
			if (!(module->image.characteristics & 0x2000) || !module->image.entry) {
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
