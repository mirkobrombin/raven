# macOS process model and the guest/host boundary

Raven's guest does what Windows does in user mode; the host is the half kernel. On Linux the host (`raven-runtime`) drives a child
(`raven-bootstrap`) with ptrace, and the guest's x86 code runs on the host CPU. This document describes how the same boundary works on
macOS, which parts are a stage, and the interface that will let `native/nt` stop depending on how guest code executes.

Status labels: **PROVEN** (a command and its output in this tree or the report), **INFERRED** (from what, missing check), **NOT
verified**.

## 1. Stages

| Stage | Guest code runs as | Status |
|---|---|---|
| Rosetta stage (now) | a separate x86-64 Mach-O process, `raven-guest-bootstrap`, that Rosetta translates; the arm64 host controls it through its task port | approved as a stage only |
| Final | Raven's own translator, in the host process or in a separate arm64 guest process; the choice waits on spike numbers | not started here |

Nothing in `native/channel` depends on how guest code executes. The Rosetta-specific parts are `native/darwin/task.*` (Mach task
control), `guest_bootstrap.c`, `guest_switch.S` and `doorbell_stub.S`. Rosetta remains general-purpose only through macOS 27
(external-facts.md), so the stage has a shelf life.

## 2. The Rosetta stage

### Starting the guest

- The host `posix_spawn`s `raven-guest-bootstrap` with `POSIX_SPAWN_START_SUSPENDED` and takes its task port with `task_for_pid`. The
  bootstrap is ad-hoc signed with `com.apple.security.get-task-allow`; the host needs no entitlement. PROVEN: `channel_rosetta_test`
  and `rosetta_stage_test` pass with Developer Mode off (`DevToolsSecurity -status`: disabled).
- Handing the task port over a Mach message did not work: the bootstrap special port also carries dyld's and Rosetta's own launchd
  traffic, and inherited exception ports came back empty in the child. PROVEN in the probes (`build-macos-e/probe`, not committed).
- The bootstrap links with `-pagezero_size 0x1000 -segalign 0x1000`, so the fixed addresses Windows programs use are free:
  `0x7ffe0000` (KUSER_SHARED_DATA) and `0x140000000` map with `MAP_FIXED` (PROVEN by the spike's lowaddr results for x86-64). The
  lowest fixed-allocatable address is not 0x10000: about `0x25cc000` for a position-independent bootstrap and `0x7c000` with `-no_pie`
  (spike data), so images based below that need relocation. With page zero 0 the program links and is killed at launch (PROVEN,
  exit 137). McSoda's loader is
  linked the same way (behavior reference only: its patch sets these linker flags for the 64-bit loader and uses a preloader).

### Memory

- Everything the guest sees is mapped by the host: `mach_vm_allocate` for private memory, `mach_vm_map` of a named Mach memory entry
  (`MAP_MEM_NAMED_CREATE`, all protections allowed) for memory both sides share, `mach_vm_protect`, `mach_vm_read_overwrite` and
  `mach_vm_write`. This is the counterpart of the guest-side `mmap`/`mprotect` and `process_vm_*` on Linux.
- The translated task has 4 KiB pages: a 4 KiB piece of a 16 KiB host object maps at a 4 KiB-aligned guest address and a single
  4 KiB page can be made read-only. PROVEN (probe `reply.c`: `map 4K piece ... kr=0`, `protect 4K ... kr=0`, guest reads the host's
  values).
- An entry made from an existing RW mapping cannot be mapped executable (KERN_INVALID_RIGHT); a named entry created for the purpose
  can. PROVEN. `LoadedImage` therefore keeps its storage in such an entry on macOS (`native/pe/loaded_image.cpp`, `#ifdef __APPLE__`;
  the Linux branch is the previous code).
- `detach()` on macOS copies the image into a fresh entry at a new host address, unlike Linux, where the storage keeps its address.
  The warm host (the only caller) is Linux-only today.
- Self-modifying code: Rosetta translates a page when it first runs it. The stage finishes every write to an image and to the stub page
  before the guest maps them executable. A later write to code through the host's view must drop the page's execute permission and give
  it back so Rosetta translates it again (the behavior CrossOver documents for Rosetta; NOT verified here).

### Threads, TEB and GS

- `thread_get_state`, `thread_set_state` and `thread_create_running` with `x86_THREAD_STATE64` fail from the arm64 host with
  KERN_INVALID_ARGUMENT, and also from a translated x86-64 controller. PROVEN (probes `tfp.c`, `tfp86.c`, `reply.c`). The ARM flavor
  answers with Rosetta's own state, which means nothing to Raven.
- So the guest's threads are started by the bootstrap itself, and the host asks a thread to call into guest code through its control
  page (`native/darwin/bootstrap_protocol.h`): function, stack, TEB, four arguments; the thread runs it and posts the result.
- GS: the bootstrap sets the GS base to the TEB with the machine-dependent call `thread_set_tsd_base` (`syscall` with `eax=0x3000003`),
  which Rosetta carries out. PROVEN (probe `guest.c`: `gs:30=7ff00000 gs:1ff0=1234`). It finds its own base with
  `THREAD_IDENTIFIER_INFO.thread_handle` and checks it against `gs:0` before trusting it, and goes back to it after the call.
  CrossOver instead keeps the system's TSD as GS and writes the TEB's self, TLS and PEB pointers into otherwise unused TSD slots; that
  breaks every other `gs:` offset Windows code reads (Raven's served stubs read `gs:[0x1ff0]`), so Raven does not follow it.
- While GS addresses the TEB no system-library code may run on that thread: its thread data is reached through GS. Guest-side stubs
  therefore use raw system calls (the ulock calls), never libSystem.

### Exceptions

- The host sets the task's exception ports for breakpoint, bad access, bad instruction and arithmetic exceptions with
  `EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES` and the `x86_THREAD_STATE64` flavor. The message carries the x86-64 registers
  (Rosetta translates them), and a reply with a changed RIP and RAX is applied. PROVEN (probe `reply.c`: `exc=6 code=2 rip=284a4bd
  rax=42`, next exception shows `rax=99` at the new RIP).
- After `int3` the reported RIP is the instruction after the breakpoint, as Linux reports it at SIGTRAP. PROVEN (same probe).
- An exception the host does not own is answered with KERN_FAILURE and goes on to the guest's signals (the bootstrap crashes as a
  normal process would). PROVEN (probe: the `ud2` reply with KERN_FAILURE ends the child with SIGILL).
- The x86 registers of a thread are therefore readable and writable only while it is stopped at an exception. A GetThreadContext of a
  running thread in this stage needs the thread brought to a stop first: the planned way is a host request to a bootstrap control thread
  that signals the target, whose handler raises a breakpoint (the host then sees the interrupted context in memory). NOT implemented.
- The floating-point and AVX state is a separate flavor (`x86_FLOAT_STATE64`/`x86_AVX_STATE64`) that one exception port cannot
  deliver together with the thread state; fetching it at a stop needs a second request through the guest. NOT implemented.

### Rosetta workarounds, by design (behavior observed in CrossOver, reference only)

| Behavior CrossOver works around | Raven stage |
|---|---|
| MXCSR in signal contexts is wrong under Rosetta | API calls and faults reach the host as Mach exceptions, never as guest signals; MXCSR comes from the float state at a stop (to do) |
| Debug registers cannot be set | Raven's hardware watchpoints are a Linux diagnostic only; SetThreadContext with debug registers is accepted and ignored in this stage |
| No 16-bit LDT, no far `lretq` 32-to-64 thunk | The Rosetta stage runs x64 only; 32-bit guests wait for the translator, which owns the x86 decoder |
| Code that is written after it ran is not retranslated | Writes finish before execute is granted; later writes toggle execute (section above) |
| XGETBV and CET instructions raise invalid-opcode on some versions | A bad-instruction exception reaches the host first, which can emulate these before dispatching to SEH (to do) |

## 3. The guest channel (`native/channel`)

- `protocol.h` is the served block's layout as preprocessor constants, with a version word (31): `RAVEN_CHANNEL_MAGIC | 1`. It restates
  what `gates.S` and `native_cpu.cpp` use today. The macOS build checks the timestamp and queue words against native/cpu's headers
(`static_assert`s in `rosetta_stage.cpp`), and `rosetta_stage_test` checks the rest through gates.S's own bytes: five distinct
arguments (four registers and the stack word, from which the fifth is read) and the answer cross the block where protocol.h says
(PROVEN). The request words (300, 320) are not exercised by the stage (INFERRED from the identical object on e1-draft). Making
  `gates.S` include it is a shared-layer change that waits for the engine interface (section 4). On the branch `e1-draft` it already
  does, and `gates.S` assembles to an identical object (PROVEN: `cmp gates-before.o gates-after.o`).
- Two new words: 20, the host's doorbell, and 21, the guest's. A side that would rather sleep than watch word 0 sets its bell, looks
  again, and sleeps on it; the other side publishes, then rings: clears the bell and wakes. One of the two always sees the other.
- `wait_word`/`wake_word`: `futex` (not private) on Linux, `os_sync_wait_on_address` with `OS_SYNC_WAIT_ON_ADDRESS_SHARED` on macOS. A
  first wait on a page the process never touched fails with EFAULT on macOS; the wait touches the page and the caller looks again.
  PROVEN (`osync.cpp` probe: `errno=14 after 15 us`, then a 200 ms sleep once touched).
- A guest thread whose GS is its TEB sleeps and wakes with the raw ulock calls (`UL_COMPARE_AND_WAIT_SHARED`), and these meet the host's
  `os_sync` calls on the same word. PROVEN (`channel_rosetta_test`, all three modes).
- `ServedBlock` is the host's view (`pending`, `call`, `answer`, `await_call`), the guest side for guests the host compiled
  (`make_call`), and the queue of calls that need no answer (`push`, `pop`). `SharedMemory` is a memfd on Linux and a named memory entry
  on macOS.
- `native/darwin/task.*` is the Mach process control: spawn, task port, memory, exception messages and replies.

## 4. Execution engine interface (design; not applied to native/nt or native/cpu)

The owner's direction: design first, then land the change on a branch that keeps Linux identical and green in CI. A draft that only
moves the served block's offsets into `protocol.h` is kept on the local branch `e1-draft`; nothing in `native/nt` or `native/cpu`
changes on this branch.

### 4.1 Declarations

```cpp
// native/engine/guest_context.h: no system headers.
namespace raven::engine {

enum class GuestIsa : uint8_t { x64, x86, arm64, arm64ec };

// The x64 register file. The names and order are those of the x86-64 Linux user_regs_struct, so the Linux engine converts by layout
// (static_assert on every offset) and native/nt keeps its field names; nothing in it is Linux-specific.
struct X64Registers {
	uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi;
	// Not a register: the service number at a syscall stop, and no_service when the thread must not restart one on resuming.
	uint64_t service;
	uint64_t rip, cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
	static constexpr uint64_t no_service = ~0ULL;
};

// The x86 file of a 32-bit guest is the low halves of X64Registers with the selectors and FS base; ARM64 and ARM64EC get their own
// file when an engine runs them (x0-x30, sp, pc, cpsr, the TEB in x18/TPIDR, fpcr, fpsr, v0-v31).
struct GuestContext {
	GuestIsa isa = GuestIsa::x64;
	X64Registers registers{};
	// The XSAVE image in its standard layout: FXSAVE in the first 512 bytes, the XSAVE header at 512, the AVX halves at 576.
	std::vector<uint8_t> xstate;
};

enum class StopReason { returned, api, syscall, fault, exception, timeout, exited };
enum class FaultKind { none, access, breakpoint, single_step, illegal_instruction, privileged_instruction, divide, overflow, floating_point };

struct GuestStop {
	StopReason reason;
	uint64_t value;          // returned: RAX; api: the stub; syscall: the service; exited: the status
	uint64_t instruction;
	FaultKind fault = FaultKind::none;
	uint64_t fault_address = 0;
	int access = 0;          // PROT_* bits of the access that faulted
	uint32_t exception_flags = 0;
	std::vector<uint64_t> exception_information{};
};

// What native/nt needs from whatever runs guest code. Every context call is valid only while the thread is stopped at a stop the
// engine delivered; suspend means "bring the thread to such a stop". ptrace already behaves so; a Rosetta guest gives x86 state only
// at a Mach exception; a translator has it in its own memory.
class GuestEngine {
public:
	virtual ~GuestEngine() = default;
	virtual GuestIsa isa() const = 0;

	// Memory, in the guest's 4 KiB pages; protections are PROT_*.
	virtual uint64_t map_memory(size_t size, int protection, uint64_t address = 0) = 0;
	virtual void protect_memory(uint64_t address, size_t size, int protection) = 0;
	virtual void unmap_memory(uint64_t address) = 0;
	virtual uint64_t map_shared(const channel::SharedMemory &memory, size_t size, uint64_t offset = 0, uint64_t address = 0) = 0;
	virtual std::vector<uint8_t> read_memory(uint64_t address, size_t size) const = 0;
	virtual void write_memory(uint64_t address, std::span<const uint8_t> bytes) = 0;
	virtual bool read_alone(uint64_t address, void *into, size_t size) const = 0;
	virtual bool write_alone(uint64_t address, const void *from, size_t size) = 0;
	virtual uint64_t compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, size_t size) = 0;

	// Execution of the current thread.
	virtual GuestStop call(uint64_t function, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout) = 0;
	virtual GuestStop callback(uint64_t function, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout) = 0;
	virtual GuestStop resume(std::chrono::milliseconds timeout) = 0;
	virtual void return_api(uint64_t value) = 0;
	virtual std::vector<uint64_t> api_arguments(size_t count) const = 0;
	virtual std::array<uint64_t, 4> api_float_arguments() const = 0;
	virtual GuestContext context() const = 0;
	virtual void set_context(const GuestContext &context) = 0;
	virtual void redirect(const GuestContext &context, size_t depth) = 0;
	virtual GuestContext callback_origin(size_t index) const = 0;

	// Threads, by an opaque handle of the engine's own (a ptrace task, a bootstrap thread, a translator's vCPU), with the thread's
	// stack ceiling and TEB (GS base on x64, FS on x86, x18/TPIDR on ARM64).
	using Thread = uint64_t;
	virtual Thread new_thread(uint64_t stack_top, uint64_t teb) = 0;
	virtual void pause_thread(Thread thread) = 0;
	virtual void set_thread_registers(Thread thread, const X64Registers &registers) = 0;
	// The served block of a thread: the engine maps it and points the TEB's slot at it.
	virtual void attach_block(Thread thread, const channel::SharedMemory &block) = 0;
};

}
```

Phase 1 changes only types: `NativeContext` becomes `engine::GuestContext` (an alias first), and the five `user_regs_struct`
parameters in `native_cpu.h` and `threads.h` become `engine::X64Registers`.

Phase 2 makes `NativeProcess` derive from `GuestEngine`. Most methods exist today with these signatures; these change:

| Method | Today | Change, and its callers |
|---|---|---|
| `call`, `callback`, `resume` | return `NativeResult` (with Linux `signal`, `signal_code`) | return `GuestStop`; `NativeResult` stays inside the Linux engine for one release and converts |
| `map_shared` | `(int descriptor, size, offset, address)` | `(const channel::SharedMemory &, ...)`; desktop/windows.cpp:173 and :721 and nt/audio_service.cpp:133 pass the shared object instead of its descriptor; `map_descriptor` (device files) stays Linux-only |
| `new_thread`, `pause_thread`, `set_thread_registers`, `stop_thread`, `load_thread`, `save_thread` | take `NativeThreadState`, which holds `pid_t task` and `context_in_task` | take `Thread`; `NativeThreadState` stays as the Linux engine's record, found by the handle |
| `attach_block` | does not exist: `make_served_block` is private and builds a memfd | new; the Linux engine wraps `make_served_block` |
| `context`, `set_context`, `redirect`, `callback_origin` | `NativeContext` | `GuestContext` (phase 1 already made these the same type) |

The ptrace-only statics (`wait_task`, `hand_task`, `adopt_task`, `pause_own_task`, `task_context`, `set_task_registers`,
`set_task_xstate`, `interrupt_host`) stay on the Linux engine; `threads.cpp` reaches them through a `LinuxThreadHost` that only the
ptrace engine provides, and the Darwin engine provides its own (bootstrap threads, Mach exceptions).

### 4.2 Mapping of today's uses

Outside `native_cpu.cpp` (which keeps `user_regs_struct` internally for ptrace and converts at the boundary functions):

| Where (native/nt) | Today | After |
|---|---|---|
| exceptions.cpp:12, 31; startup.cpp:32, 70; multibyte.cpp:205; file.cpp:123; object_api.cpp:142 | `process.context().registers.rip` | unchanged text; the type is `X64Registers` |
| providers.cpp:2790, 2808-2810, 2943, 2970, 3084 | `.rip`, `.rax`, `.rbx`, `.rcx`, `.rdx`, `.rsp`, `.r8`, `.r9` for diagnostics and stack reads | unchanged text |
| environment.cpp:145, 179; tls.cpp:94 | `.gs_base` as the thread's TEB | unchanged text; an ARM64 engine answers from x18/TPIDR in its own file |
| seh.cpp:23-28, 877, 1073, 1087 | `callback_origin(...).registers.rip/.rsp`, `lowered.registers.rsp`, `context().registers.rax` | unchanged text |
| unwind.cpp:64-123 (context_from_process, apply_context, AVX halves) | all general registers, `eflags`, the six selectors, `orig_rax = -1`, `xstate` at 0/24/512/576 | `orig_rax` becomes `service = X64Registers::no_service`; `xstate` keeps the XSAVE layout, so the offsets stay |
| thread_api.cpp:909-925 (GetThreadContext), 959-977 (SetThreadContext) | every general register, `cs`, `ss`, `eflags`, `xstate` | unchanged text |
| x86_process.cpp:1062-1063, 1152, 1263-1264 | `.rsp`, `.rip`, `orig_rax = -1` | `orig_rax` becomes `service = no_service` |
| threads.cpp:796-797, 1553, 1566-1580; threads.h:360 | snapshot `.rip/.rsp`, `task_context`, `set_registers(const user_regs_struct &)`, `xstate` | the parameter type becomes `X64Registers`; the rest unchanged |
| native_cpu.h:13, 35, 143, 193, 207 | `#include <sys/user.h>`, `user_regs_struct registers`, `general_registers()`, `set_task_registers`, `set_thread_registers` | the include moves to native_cpu.cpp; the four types become `X64Registers` |
| native_cpu.cpp: `orig_rax == stub_callback` (callback_in_stub) | a tag in a register field | a `bool in_stub` beside the saved context in the callback record |

Tests that name Linux types and must still compile after phase 1: `native/cpu/native_cpu_test.cpp` (`sizeof(user_regs_struct)` at
:244 and :332, `offsetof(user_fpregs_struct, xmm_space)` at :434 and :659, `orig_rax` at :249, :514, :685), `native_x86_test.cpp`
(2 uses), `nt/providers_test.cpp` (21 uses of register fields), `nt/environment_test.cpp` and `nt/loader_test.cpp` (1 each). They are
Linux engine tests: `sizeof(user_regs_struct)` becomes `sizeof(engine::X64Registers)` (equal by the static_assert), `orig_rax`
becomes `service`, and the `user_fpregs_struct` offset stays, as the XSAVE image keeps the FXSAVE layout. The field-name uses compile
unchanged.

`orig_rax` is the only field whose meaning changes: on Linux it is the syscall number the kernel reports and the "do not restart"
marker (-1) the runtime writes before resuming; `service` keeps exactly that meaning, so the Linux engine copies it to and from
`orig_rax` unchanged.

### 4.3 The Linux ptrace engine without behavior change

- `X64Registers` has the layout of `user_regs_struct`; `native_cpu.cpp` checks every field offset and the size with `static_assert`
  and passes `reinterpret_cast<user_regs_struct *>(&context.registers)` to `PTRACE_GETREGS`/`PTRACE_SETREGS`, so no field is copied
  differently. The expected difference in the binary is symbol names (mangling of the functions whose parameter type changes) and
  debug information, not instructions; that is what the CI check below verifies. NOT verified here: no Linux build is possible on
  this host.
- `xstate` stays the `NT_X86_XSTATE` buffer it is today.
- `NativeResult` stays; `GuestStop` is filled from it in phase 2 (`signal`/`signal_code` become `FaultKind` and `access` in the engine,
  the signal-to-NTSTATUS table in exceptions.cpp moves to a FaultKind table with the same results).

### 4.4 CI plan

1. Phase 1 lands alone. CI builds the Linux tree twice (base and change) and compares `objdump -d --no-show-raw-insn` of every
   object of `raven_cpu` after demangling and normalizing symbol names: the instruction streams must be identical. `gates.S` and
   `bootstrap.S` objects must be byte-identical (`cmp`).
2. The full Linux CTest suite (serial, parallel, seccomp shards) passes unchanged.
3. Phase 2 (virtual interface) is measured with the call bench in both modes before and after; any change beyond noise blocks it.
4. A macOS job builds `native/darwin` standalone (`cmake -S native/darwin`) and runs its tests until the top-level build splits by
   host (stream A).

## 5. Measurements

MacBook Air M5, macOS 27.2.1, on AC power (`pmset -g batt` before and after the run), every measured process at
`QOS_CLASS_USER_INTERACTIVE` (the spikes' QoS), no core pinning. One frozen run of `build-macos-e/results/run.sh`; raw CSV and output
in `build-macos-e/results/` (not committed). 30 runs after warm-up; median, min, max and population standard deviation in ns.

Channel round trip, 20 000 per run (`channel_test`, `channel_rosetta_test`):

| Pair | spin | doorbell (both sleep) | spin 2000 rounds, then doorbell |
|---|---|---|---|
| native-native (fork) | 205 (163-258, sd 16) | 13317 (7940-20996, sd 3011) | 932 (253-23299, sd 4503) |
| Rosetta guest, raw ulock - native host, os_sync | 333 (131-410, sd 86) | 10119 (7278-18152, sd 2321) | 801 (150-3584, sd 1102) |

One call across the boundary, end to end: x64 guest code under Rosetta calls an import, the arm64 host answers, the guest goes on.
20 000 calls per run in one guest loop, less one empty guest call (917 ns) (`boundary_bench`):

| Case | median | min | max | sd | path counted by the host |
|---|---|---|---|---|---|
| served stub (gates.S), host spins | 437 | 328 | 1092 | 182 | 600 000 in the block |
| served stub, host sleeps: the stub spins its 1000 rounds, then its breakpoint reaches the host as a Mach exception | 45460 | 31052 | 116966 | 18094 | 599 910 at the breakpoint, 90 in the block |
| doorbell stub, both sides sleep (raw ulock in the guest, os_sync on the host) | 13555 | 10933 | 16481 | 1348 | 600 000 in the block |
| `int3; ret` export answered at the Mach exception (the reply sets RAX) | 58573 | 24228 | 105176 | 22381 | 600 000 trapped |
| guest-local GetLastError (`mov eax, gs:[0x68]; ret`) | 5.2 | 4.5 | 5.4 | 0.2 | none |

The served stub with a spinning host is two orders of magnitude cheaper than any path that sleeps; the doorbell is about four times
cheaper than a Mach exception round trip. The stage therefore keeps a spinning host thread per guest thread that is inside a call, as
the Linux parallel host does, and the doorbell for threads that wait long. An earlier run at default QoS gave medians two to four times
higher with far wider spread: QoS must be stated with every number.

## 6. Temporary parts and their final form

| Temporary | Final |
|---|---|
| `raven-guest-bootstrap` runs guest code under Rosetta | Raven's translator runs it; the bootstrap and `guest_switch.S` go |
| The stage's handlers in tests stand in for `providers.cpp` | `native/nt` dispatches once the shared layer builds on macOS |
| One guest thread | Bootstrap threads per guest thread, created on the host's request through the control page |
| `doorbell_stub.S` makes Darwin system calls from guest code | The engine waits in its own code; the served block keeps the same doorbell words |
| `protocol.h` restates the layout `gates.S` uses | `gates.S` and `native_cpu.cpp` include it (phase 1 of section 4) |
