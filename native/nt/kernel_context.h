#pragma once

#include "api.h"
#include "access_token.h"
#include "guest_allocator.h"
#include "memory.h"
#include "objects.h"
#include "paths.h"
#include "completion_port.h"
#include "pipe.h"
#include "threads.h"
#include <string>
#include "pe/module_set.h"

namespace raven {

// Thrown by a host handler that moved the guest to another place; the call loop at the named nesting level resumes it.
struct ExecutionRedirected {
	size_t level;
};

// Raised to end the process from deep inside a handler; the call loop turns it into the process result.
struct ProcessExit {
	uint32_t code;
	uint64_t instruction;
	// Terminated rather than exited: no DLL or TLS detach notification runs, as with TerminateProcess and fatal faults on Windows.
	bool terminated = false;
};

// Process-wide services that the native kernel32 handlers share; every member outlives the handlers.
struct KernelContext {
	NativeProcess &process;
	NativeEnvironment &environment;
	NativeMemory &memory;
	GuestAllocator &allocator;
	NativeObjects &objects;
	NativeThreads &threads;
	NativePaths &paths;
	ModuleSet &modules;
	const PEModule &entry;
	const NativeLaunch &launch;
	// Runs a guest function to completion (callbacks, thread start routines); stops propagate as exceptions.
	std::function<NativeResult(uint64_t, std::span<const uint64_t>)> call;
	// Top-level exception filter installed by the program (SetUnhandledExceptionFilter).
	uint64_t unhandled_filter = 0;
	bool shutdown_in_progress = false;
	// Delivers a CPU fault to the guest's handlers; never returns normally (it redirects or ends the process).
	std::function<void(const NativeResult &)> raise_fault;
	// Walks the guest stack with the unwind tables and names each caller as module+offset (diagnostics only).
	std::function<std::string()> describe_stack;
	// Current and new queue categories for a thread; the last argument clears the requested new bits.
	std::function<uint32_t(uint32_t,uint32_t,bool)> queue_status;
	// Takes a reference on the loaded module at this base, or pins it for the life of the process (GetModuleHandleEx).
	std::function<void(uint64_t, bool)> reference_module;
	// The loaded module a guest string names, by name or by path, as GetModuleHandle finds it (zero, with the last error set, if none).
	std::function<uint64_t(uint64_t, bool)> module_named;
	// The environment variables the program has now, as the block a new process receives (set by the system handlers).
	std::function<std::u16string()> environment_block;
	// Started operations that finish later, such as overlapped socket transfers.
	std::shared_ptr<Reactor> reactor = std::make_shared<Reactor>();
	// Named pipes served by this process and the pipe operations that have not finished.
	std::shared_ptr<PipeState> pipes = std::make_shared<PipeState>();
	std::shared_ptr<AccessToken> process_token = std::make_shared<AccessToken>();
};

// Windows FILETIME ticks are 100 ns units since 1601-01-01.
inline constexpr uint64_t filetime_epoch_offset = 11644473600ULL * 10000000ULL;
// The performance counter in 100 ns units: the host's monotonic clock, as QueryPerformanceCounter reports it.
uint64_t counter_ticks();

void register_exception_api(ApiRegistry &registry, KernelContext &context, class NativeUnwinder &unwinder);
class NativeRegistry;
void register_registry_api(ApiRegistry &registry, KernelContext &context, NativeRegistry &hive);
void apply_registry_settings(NativeRegistry &hive);
void register_service_api(ApiRegistry &registry, KernelContext &context, NativeRegistry &hive);
void register_platform_api(ApiRegistry &registry, KernelContext &context);
void register_shell_api(ApiRegistry &registry, KernelContext &context);
void register_version_api(ApiRegistry &registry, KernelContext &context);
void register_shlwapi_api(ApiRegistry &registry, KernelContext &context);
void register_file_api(ApiRegistry &registry, KernelContext &context);
void register_system_api(ApiRegistry &registry, KernelContext &context);
void register_sync_api(ApiRegistry &registry, KernelContext &context);
void register_thread_api(ApiRegistry &registry, KernelContext &context);
void register_memory_api(ApiRegistry &registry, KernelContext &context);
void register_text_api(ApiRegistry &registry, KernelContext &context);
// The console functions over the host terminal.
void register_console_api(ApiRegistry &registry, KernelContext &context);
// ReadFile on the console's input: a line, or typed characters, as ReadConsoleA reads them. False for any other handle.
bool read_console_file(KernelContext &context, ApiCall &call);
// CreateFile of CONIN$, CONOUT$ or CON: the host terminal. False when the name is another device or no terminal is there.
bool open_console_device(KernelContext &context, ApiCall &call, std::u16string_view name, uint32_t access, bool inheritable);
void register_process_api(ApiRegistry &registry, KernelContext &context);
void register_security_api(ApiRegistry &registry, KernelContext &context);
// The Schannel TLS client of SSPI over the host's OpenSSL.
void register_schannel_api(ApiRegistry &registry, KernelContext &context);
void register_child_process_api(ApiRegistry &registry, KernelContext &context);
void register_winsock_api(ApiRegistry &registry, KernelContext &context);
// WinINet's synchronous HTTP client over host sockets and the host's OpenSSL.
void register_wininet_api(ApiRegistry &registry, KernelContext &context);
// SetFileCompletionNotificationModes for a socket handle; false when the handle is not a socket.
bool set_socket_completion_modes(NativeObjects &objects, uint64_t handle, uint32_t modes);
void register_host_api(ApiRegistry &registry, KernelContext &context);
void register_job_api(ApiRegistry &registry, KernelContext &context);
// winmm wave output through the host sound server.
void register_wave_api(ApiRegistry &registry, KernelContext &context);
void register_mmio_api(ApiRegistry &registry, KernelContext &context);
void register_process_info_api(ApiRegistry &registry, KernelContext &context);
void register_crypt32_api(ApiRegistry &registry, KernelContext &context);
void register_bcrypt_api(ApiRegistry &registry, KernelContext &context);
void register_font_service(ApiRegistry &registry, KernelContext &context);
// The HLSL compiler behind d3dcompiler_47.dll: requests from the guest component, results read back in parts.
void register_shader_service(ApiRegistry &registry, KernelContext &context);
// An installed font family as GDI enumerates it: the face closest to regular, and the scripts and code pages its OS/2 table claims.
struct InstalledFamily {
	std::u16string name;
	uint32_t weight = 400;
	bool italic = false;
	bool fixed = false;
	uint32_t unicode_ranges[4]{};
	uint32_t code_pages[2]{};
};
// The families of the fonts installed in the prefix, in name order; none without FreeType.
std::vector<InstalledFamily> installed_font_families();
void register_canvas_service(ApiRegistry &registry, KernelContext &context);
// Starts the program an application name or command line names, looked up the way Windows does; null with the Windows error otherwise.
std::shared_ptr<class ProcessObject> start_program(KernelContext &context, const std::u16string &application, const std::u16string &line,
	const std::u16string &directory, const int (&standard)[3], const std::u16string &environment, int *gate, uint32_t &error,
	std::vector<InheritedHandle> inherited = {});
// Puts a child process into a job; false with the Windows error otherwise.
bool assign_to_job(KernelContext &context, uint64_t job, const std::shared_ptr<class ProcessObject> &process, uint32_t &error);

}
