#pragma once

#include "pe/loaded_image.h"
#include "pe/module_set.h"

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
// Whether a parallel guest gets a host thread for each of its threads (see native_cpu.cpp).
bool threaded_host();

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
	// The thread's block of the environment while it is served, and on a 32-bit program its FS selector and base.
	uint64_t teb = 0;
	uint32_t fs_selector = 0;
	uint64_t fs_base = 0;
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
	// Reads guest memory from a host thread that does not hold the turn: straight from the child, without the tables of regions the
	// turn guards. False when any of it cannot be read.
	bool read_alone(uint64_t address, void *into, size_t size) const;
	// The same for writing: false when any of it is not writable in the guest, which then sees none of the bytes that did not go.
	bool write_alone(uint64_t address, const void *from, size_t size);
	// Whether a host thread that does not hold the turn may map, protect and free guest memory: the agent's service thread then makes
	// the system calls, for any host thread, and the records of the guest's memory have their own lock.
	bool memory_beside() const;
	void write_memory(uint64_t address, std::span<const uint8_t> bytes);
	void validate_write_memory(uint64_t address,size_t size) const;
	uint64_t compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, size_t size);
	// Host primitives; Windows reservation/commit and PAGE_* semantics belong above this boundary.
	uint64_t map_memory(size_t size, int protection, uint64_t address = 0);
	void protect_memory(uint64_t address, size_t size, int protection);
	// Whether the guest's own mappings give exactly this protection to every page of the range.
	bool has_protection(uint64_t address, size_t size, int protection) const;
	// How many times the guest's mappings changed: equal counts mean every protection stayed as it was.
	uint64_t mapping_changes() const;
	// The same for pages of a mapped program image, which the program may protect although it did not allocate them.
	void protect_image(uint64_t address, size_t size, int protection);
	void unmap_memory(uint64_t address);
	// The agent (see agent.h): its code copied into the guest and its service thread started there, untraced, in the thread group of
	// the task that started it. The guest's own threads stay traced until they move to the agent.
	void start_agent();
	// Whether the kernel lets guest tasks leave ptrace for the agent.
	static bool agent_supported();
	// Takes the current task out of ptrace: from now on it stops in the agent's handler, and the runtime drives it over its block.
	void hand_to_agent();
	// Starts a guest thread in the agent's hands, with its stack top: it waits, held, for the context the runtime gives it.
	pid_t agent_thread(uint64_t stack_top);
	// The service thread's task, 0 before start_agent.
	pid_t agent_task() const;
	// How many of the runtime's system calls the service thread made in place of a stopped task.
	uint64_t agent_calls() const;
	// A system call the service thread makes inside the guest, without stopping any task; an error number in the result throws.
	uint64_t agent_syscall(uint64_t number, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0,
		uint64_t a6 = 0);
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
	// An API breakpoint, with the bytes the callee pops; a 64-bit one that a RET follows returns through it, without the runtime popping
	// the return address.
	struct ApiTrap {
		uint32_t cleanup = 0;
		bool returns = false;
	};
	// Traps made before the process from the bytes of the images it maps, as a warm host makes them once for every start: a process
	// that has none yet takes them whole.
	void add_api_traps(std::map<uint64_t, ApiTrap> traps);
	NativeMode mode() const;
	// x86 only: FS addresses the 4 KiB TEB at base through a host LDT descriptor.
	void set_teb(uint64_t base);
	void enter_nt_service(uint64_t instruction, uint32_t number);
	void complete_debug_print(uint64_t instruction);
	// Address the guest returns to when a host-initiated call finishes.
	uint64_t return_gate() const;
	// Whether `address` is where a callback returns to the runtime: the return gate, or the routine a guest runs a callback from
	// while it waits in a stub. A callback of the second kind keeps no registers of the call that made it but its instruction and
	// stack pointers and its arguments (callback_in_stub): the others are those its frames give back.
	bool callback_boundary(uint64_t address) const;
	bool callback_in_stub(size_t index) const;
	size_t callback_depth() const;
	// The guest's registers at the API call that started the callback at this nesting index (0 is the outermost), with the current
	// extended state: where a stack walk continues past the callback's return gate.
	NativeContext callback_origin(size_t index) const;
	// Diagnostics: RAX after an API result was delivered, without fetching the extended state.
	uint64_t result_register() const;
	// The general registers alone, without the extended state that context() also reads.
	user_regs_struct general_registers() const;
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
	// Whether the guest process has ended, so no thread can be served any more.
	bool gone() const;
	int execution_descriptor() const;
	void drain_execution();
	void set_execution_wait(std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> wait);
	// Lets the threads whose tasks stopped have the runtime first, this one waiting until `ready`.
	void set_execution_turn(std::function<void(std::function<bool()>)> turn);
	// With a host thread for each guest thread: called while the thread's own guest runs, to wait for the guest's stop; it returns the wait
	// status, or -1 at the deadline.
	void set_execution_park(std::function<int(pid_t, std::chrono::steady_clock::time_point, const uint64_t *)> park);
	// Waits for a stop of a task the calling host thread traces: the status, -1 at the deadline, -3 when `interrupted` says so after the
	// wait was broken by interrupt_host. It reads no state of the process, so a thread may call it while others serve their calls.
	// With `block`, the block the task's guest thread leaves its calls in, it returns -4 as soon as a call is there.
	static int wait_task(pid_t task, std::chrono::steady_clock::time_point deadline, const std::function<bool()> &interrupted,
		const uint64_t *block = nullptr);
	// The calls a host thread answers in its guest thread's block without the guest stopping: `stubs` are their stubs, laid out as the
	// served ones. Such a call reaches the runtime as one that stopped at its stub's breakpoint, and is answered the same way.
	void set_call_stubs(std::vector<uint64_t> stubs);
	// Whether the call in progress waits in its stub, its task not stopped for it.
	bool call_waiting() const;
	// Stops the task of the call that waits in its stub at the stub's breakpoint, for work that needs the task stopped.
	void settle_call();
	// Has the guest that waits in a stub run a short routine of the runtime's own there, and returns its result.
	uint64_t run_in_stub(uint64_t function, std::span<const uint64_t> arguments);
	// Stops a task of the calling host thread that waits in a stub for its call's answer where it is, for a pause.
	static void hold_call(pid_t task);
	// Calls answered in the block, of which those whose stub had stopped waiting, and calls whose task had to be stopped.
	std::array<uint64_t, 3> call_counts() const;
	static void interrupt_host(pid_t host_thread);
	// Moves the tracing of a stopped task from the calling host thread to the one that adopts it.
	void hand_task(pid_t task);
	static void adopt_task(pid_t task);
	pid_t administrative_task() const;
	// Work a host thread does on the task it traces: stop it where it runs (the wait status of the stop it was found in, a trap of its own
	// or the interrupt), read its whole context, set its general registers. arm_host_thread prepares a host thread to be interrupted.
	static int pause_own_task(pid_t task);
	static NativeContext task_context(pid_t task);
	static void set_task_registers(pid_t task, const user_regs_struct &registers);
	// Calls a parallel guest leaves in its block that the host thread may answer outside the runtime's turn: `alone` gets the stub and
	// the four argument words and gives the result, or nothing when the call needs the runtime after all.
	void set_concurrent_served(std::function<std::optional<uint64_t>(uint64_t, const uint64_t *)> alone);
	// Answers the call in `block` that way; false when it is not one or needs the runtime.
	bool serve_alone(uint64_t *block);
	bool serve_stopped_alone(pid_t task, int status, uint64_t *block);
	// Writes the floating-point and extended state of a paused task, as PTRACE_GETREGSET with NT_X86_XSTATE gives it.
	static void set_task_xstate(pid_t task, std::span<const uint8_t> xstate);
	static void arm_host_thread();
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
	// Lets a guest that runs for `slice` without stopping be interrupted so that `yield` can give the processor to another thread; `wanted` says
	// whether there is another thread to give it to.
	void set_time_slicing(std::chrono::milliseconds slice, std::function<bool()> wanted, std::function<void()> yield);
	// Runs `handler` as a call of the calling host thread's own guest thread, beside the turn: the arguments and the answer go through
	// that thread's state as for a served call, and guest memory is reached as any host thread may reach it.
	uint64_t serve_beside(std::vector<uint64_t> words, std::array<uint64_t, 4> floats, uint64_t stack, const std::function<void()> &handler);
	// Runs a handler written for x64 argument conventions on behalf of a 32-bit call: api_arguments and api_float_arguments answer from the
	// given words, and return_api keeps its value for end_translated_call instead of resuming the guest.
	void begin_translated_call(std::vector<uint64_t> words, std::array<uint64_t, 4> floats);
	uint64_t end_translated_call();
	// Low 64 bits of XMM0 to XMM3, where an x64 caller passes the first four floating-point arguments.
	std::array<uint64_t, 4> api_float_arguments() const;
	void return_api(uint64_t value);
	// The stack pointer the call in progress was made with.
	uint64_t api_stack_pointer() const;
	// The 64-bit thread whose call is in progress, as its GS base tells.
	uint64_t thread_base() const;
	// The stack pointer the guest had when the callback at this nesting level was entered.
	uint64_t callback_stack(size_t index) const;
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
	NativeResult resume_task(std::chrono::milliseconds timeout);
	void make_served_block(uint64_t teb, uint64_t &guest, uint64_t *&host);
	uint64_t map_file(int descriptor, size_t size, uint64_t offset, bool transfer, uint64_t address = 0);
	uint64_t receive_descriptor(int descriptor, uint64_t scratch);
	void install_seccomp();
	bool serve_pending();
	bool queued_pending() const;
	bool drain_queued_calls(bool all = false);
	uint64_t serve_call(uint64_t stub, std::vector<uint64_t> words, std::array<uint64_t, 4> floats, uint64_t stack, bool queued = false);
	uint64_t translated_call(std::vector<uint64_t> words, std::array<uint64_t, 4> floats, uint64_t stack, bool queued,
		const std::function<void()> &handler);
	bool inside_served_stub() const;
	bool served_wait_trap(uint64_t address) const;
	void protect_range(uint64_t address, size_t size, int protection, bool image);
	// The shared block and alternate signal stack of a task in the agent's hands, as the runtime and the guest see them.
	struct AgentRoom {
		uint64_t *block = nullptr;
		uint64_t block_guest = 0;
		uint8_t *stack = nullptr;
		uint64_t stack_guest = 0;
		size_t slot = 0;
	};
	AgentRoom agent_room();
	struct State;
	std::unique_ptr<State> state;
	void install_segment(uint32_t entry, uint64_t base);
	uint64_t memory_syscall(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3 = 0,
		uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0);
	// Runs the runtime's own code from `entry` to its breakpoint at `trap` in a stopped task, as memory_syscall runs the gate (entry 0).
	uint64_t owned_call(uint64_t entry, uint64_t trap, uint64_t number, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
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
