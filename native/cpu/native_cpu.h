#pragma once

#include "pe/loaded_image.h"
#include "pe/module_set.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <sys/user.h>
#include <utility>

namespace raven {

enum class NativeStop { returned, syscall, fault, timeout, api, exception, exited };

// x86 runs 32-bit guest code in host compatibility mode; every guest address stays below 4 GiB.
enum class NativeMode { x64, x86 };

struct NativeResult {
	NativeStop stop;
	uint64_t value;
	uint64_t instruction;
	int signal;
	uint64_t fault_address = 0;
	int signal_code = 0;
	uint32_t exception_flags = 0;
	std::vector<uint64_t> exception_information{};
};

struct NativeContext {
	user_regs_struct registers;
	std::vector<uint8_t> xstate;
};

// Everything that distinguishes one guest thread from another while the process is stopped between API calls.
struct NativeThreadState {
	NativeContext context;
	std::vector<NativeContext> callbacks;
	std::vector<uint32_t> callback_cleanups;
	bool active_call = false, awaiting_api = false, awaiting_syscall = false, debug_print_fault = false;
	uint32_t pending_cleanup = 0;
	uint64_t stack_top = 0;
	// A call a handler makes on the guest's behalf with translated arguments belongs to the thread that made it: another thread that
	// runs meanwhile answers its own calls.
	std::optional<std::vector<uint64_t>> translated_words;
	size_t translated_depth = 0;
	std::array<uint64_t, 4> translated_floats{};
	uint64_t translated_result = 0;
	pid_t task = 0;
	bool stopped = true;
	uint64_t served_guest = 0;
	uint64_t *served_block = nullptr;
	bool serving = false;
	uint64_t served_stack = 0;
	bool context_in_task = false;
	bool serving_queued = false;
};

// Images must outlive this stopped/resumable x86-64 Linux process.
class NativeProcess {
public:
	NativeProcess(std::span<LoadedImage *const> images, const std::string &bootstrap,
		std::chrono::milliseconds setup_timeout, NativeMode mode = NativeMode::x64, uint64_t stack_reserve = 1024 * 1024);
	~NativeProcess();
	NativeProcess(const NativeProcess &) = delete;
	NativeProcess &operator=(const NativeProcess &) = delete;
	NativeResult call(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout);
	NativeResult callback(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout);
	NativeResult resume(std::chrono::milliseconds timeout);
	NativeContext context() const;
	void set_context(const NativeContext &context);
	std::vector<uint8_t> read_memory(uint64_t address, size_t size) const;
	void write_memory(uint64_t address, std::span<const uint8_t> bytes);
	void validate_write_memory(uint64_t address,size_t size) const;
	uint64_t compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, size_t size);
	// Host primitives; Windows reservation/commit and PAGE_* semantics belong above this boundary.
	uint64_t map_memory(size_t size, int protection, uint64_t address = 0);
	void protect_memory(uint64_t address, size_t size, int protection);
	// Whether the guest's own mappings give exactly this protection to every page of the range.
	bool has_protection(uint64_t address, size_t size, int protection) const;
	// The same for pages of a mapped program image, which the program may protect although it did not allocate them.
	void protect_image(uint64_t address, size_t size, int protection);
	void unmap_memory(uint64_t address);
	// Maps a host-owned memfd into the guest at a fresh address, or at the free page-aligned address given; release it with unmap_memory.
	uint64_t map_shared(int descriptor, size_t size, uint64_t offset = 0, uint64_t address = 0);
	// Transfers the same open file description, including device mapping permissions, instead of reopening it.
	uint64_t map_descriptor(int descriptor, size_t size, uint64_t offset = 0);
	bool memory_available(uint64_t address, size_t size) const;
	void discard_memory(uint64_t address, size_t size);
	// Dynamic image mappings share LoadedImage storage; the loader owns initialization and lifetime.
	void map_image(LoadedImage &image);
	void unmap_image(uint64_t address);
	std::pair<uint64_t, uint64_t> stack_bounds() const;
	bool active() const;
	void abandon_call();
	// Drops whatever call the current thread state holds, running or waiting on the runtime: the process is ending and that call
	// will never be resumed.
	void end_call();
	// callee_cleanup is the stdcall argument byte count an x86 API pops on return; x64 ignores it.
	void add_api_trap(uint64_t address, uint32_t callee_cleanup = 0);
	NativeMode mode() const;
	// x86 only: FS addresses the 4 KiB TEB at base through a host LDT descriptor.
	void set_teb(uint64_t base);
	void enter_nt_service(uint64_t instruction, uint32_t number);
	void complete_debug_print(uint64_t instruction);
	// Address the guest returns to when a host-initiated call finishes.
	uint64_t return_gate() const;
	size_t callback_depth() const;
	// The guest's registers at the API call that started the callback at this nesting index (0 is the outermost), with the current
	// extended state: where a stack walk continues past the callback's return gate.
	NativeContext callback_origin(size_t index) const;
	// Diagnostics: RAX after an API result was delivered, without fetching the extended state.
	uint64_t result_register() const;
	// Diagnostics: a hardware write watchpoint (eight bytes) that reports as a fault with si_code TRAP_HWBKPT; address zero clears it.
	void watch_write(uint64_t address);
	// Diagnostics: single-step the next count instructions of the guest, reporting each instruction pointer to the sink.
	void trace_instructions(size_t count, std::function<void(uint64_t)> sink);
	// Guest threads share one traced process: the scheduler saves the running thread and loads the next between API calls.
	// The parallel scheduler leaves register ownership with its Linux task; explicit snapshots still copy the complete state.
	NativeThreadState save_thread(bool snapshot_registers = true) const;
	void load_thread(const NativeThreadState &thread);
	// A thread that has not started: no active call, its own stack ceiling and TEB (x64 GS base).
	NativeThreadState new_thread(uint64_t stack_top, uint64_t teb);
	bool parallel() const;
	bool stopped() const;
	int execution_descriptor() const;
	void drain_execution();
	void set_execution_wait(std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> wait);
	void stop_thread(const NativeThreadState &thread);
	void pause_thread(NativeThreadState &thread);
	// The general registers a thread that is not running continues with: a thread paused in its own task gets them there, a saved
	// thread in its snapshot (SetThreadContext on a suspended thread).
	void set_thread_registers(NativeThreadState &thread, const user_regs_struct &registers);
	// A fault stop becomes an API-style stop, so host code may run guest callbacks and redirect execution.
	void enter_fault_dispatch();
	// Continues the guest at a new context, dropping callbacks nested above depth (exception unwinding abandons them).
	void redirect(const NativeContext &context, size_t depth);
	std::vector<uint64_t> api_arguments(size_t count) const;
	// Replaces one stdcall argument word of the pending x86 API call, so an adapter can forward the call to another handler.
	void set_api_argument(size_t index, uint32_t value);
	// Runs a handler written for x64 argument conventions on behalf of a 32-bit call: api_arguments and api_float_arguments answer from the
	// given words, and return_api keeps its value for end_translated_call instead of resuming the guest.
	// Lets a guest that runs for `slice` without stopping be interrupted so that `yield` can give the processor to another thread; `wanted` says
	// whether there is another thread to give it to.
	void set_time_slicing(std::chrono::milliseconds slice, std::function<bool()> wanted, std::function<void()> yield);
	void begin_translated_call(std::vector<uint64_t> words, std::array<uint64_t, 4> floats);
	uint64_t end_translated_call();
	// Low 64 bits of XMM0 to XMM3, where an x64 caller passes the first four floating-point arguments.
	std::array<uint64_t, 4> api_float_arguments() const;
	void return_api(uint64_t value);
	// Calls the guest makes without stopping: a stub leaves its address and arguments in `block`, which host and guest share, and spins
	// until the result is there. The handler runs while the guest waits, and answers through api_arguments, api_float_arguments and
	// return_api as for a trap; it may only read and write guest memory. `stubs` are the addresses of the stubs, each `stub_size` bytes up
	// to its fallback trap: no other thread is given the processor while one is inside a stub. A stub that has spun for a while without its
	// answer stops at its breakpoint `wait_offset` bytes in; the call is answered there and the guest resumes after it. The separate
	// yield traps follow the last shared-block read, so their saved registers can survive another thread using the block.
	void set_served_calls(uint64_t *block, std::vector<uint64_t> stubs, size_t stub_size, size_t wait_offset,
		std::vector<uint64_t> yield_traps, std::function<void(uint64_t stub)> handler, std::vector<uint64_t> queued_stubs = {});
	// Host-recorded TEB of a served call, zero outside that path.
	uint64_t serving_teb() const;
	bool serving() const;
	// A served call that would block gives up its answer: the stub then stops at its fallback trap with its arguments untouched.
	void decline_served();
	bool serving_queued() const;
	void set_queued_gl_elements(bool available);
	// Runs before the guest runs again: before every resumption and before a served call is answered.
	void set_resume_hook(std::function<void()> hook);
	// Diagnostics: interrupts a guest that runs without stopping every `period` and reports where it was.
	void set_guest_sampler(std::chrono::milliseconds period, std::function<void(uint64_t rip)> sampler);

private:
	void make_served_block(uint64_t teb, uint64_t &guest, uint64_t *&host);
	uint64_t map_file(int descriptor, size_t size, uint64_t offset, bool transfer, uint64_t address = 0);
	uint64_t receive_descriptor(int descriptor, uint64_t scratch);
	void install_seccomp();
	bool serve_pending();
	bool queued_pending() const;
	bool drain_queued_calls(bool all = false);
	uint64_t serve_call(uint64_t stub, std::vector<uint64_t> words, std::array<uint64_t, 4> floats, uint64_t stack, bool queued = false);
	bool inside_served_stub() const;
	bool served_wait_trap(uint64_t address) const;
	void protect_range(uint64_t address, size_t size, int protection, bool image);
	struct State;
	std::unique_ptr<State> state;
	void install_segment(uint32_t entry, uint64_t base);
	uint64_t memory_syscall(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3 = 0,
		uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0);
	NativeResult begin_call(uint64_t address, std::span<const uint64_t> arguments,
		uint64_t stack_ceiling, std::chrono::milliseconds timeout);
	NativeResult begin_x86_call(uint64_t address, std::span<const uint64_t> arguments,
		uint64_t stack_ceiling, std::chrono::milliseconds timeout);
};

// Runs one import-free PE64 entry. This is not Windows process initialization.
NativeResult run_native_entry(const PEImage &image, LoadedImage &loaded,
	const std::string &bootstrap, std::chrono::milliseconds timeout);

// Binds and maps supplied modules; DLLs with startup code still require loader initialization.
NativeResult run_native_modules(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout);

}
