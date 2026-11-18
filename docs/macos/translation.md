# Running x64 code on Apple Silicon

Raven runs Windows programs by executing their x86 and x64 code directly on an x86 CPU. On Apple Silicon, and later on Linux ARM64,
that code has to be translated. This note compares the ways to do that, using numbers measured on a MacBook Air M5 running macOS
27.2.1. It records the decision and the design of the translator in `native/translate/`.

Each claim carries a label:

- **PROVEN**: a command in this worktree produced the result. The raw data is in `build-translate/results/`.
- **INFERRED**: derived from sources. The source and the missing check are named.
- **NOT verified**: open.

## The model: native system libraries, translated application

Raven's own DLLs become ARM64 code. Only the application's x86 and x64 code is translated. That is Microsoft's ARM64EC model, with
Raven's DLLs in the role of the Arm64X system binaries.

The translator does not depend on where it runs. It touches the outside world through three interfaces:

| Interface | What it gives the translator |
|---|---|
| `GuestMemory` | Code bytes, the code map (native or x64), and watched pages |
| `CodeAllocator` | Where translated code is placed |
| The exit protocol (`ExitReason`, `CpuState`) | The way out to the runtime |

The process model is still open: in-process, or a VM under Hypervisor.framework. The same translated code fits either, because it
addresses guest memory at guest virtual addresses.

## Options compared

| | Own translator (`native/translate`) | FEX ARM64EC/WoW64 DLLs | Box64 | Rosetta 2 |
|---|---|---|---|---|
| Licence | Raven's | MIT (PROVEN, `fex/LICENSE`) | MIT (PROVEN, `box64/LICENSE`) | Proprietary OS component |
| Builds for the port | Yes, standalone CMake project, all 4 tests pass (PROVEN) | Windows DLLs: yes (PROVEN, see below). Unix side: no, `sys/prctl.h` (PROVEN). A native macOS build of FEX: configure refuses Darwin (PROVEN) | No: 577 compile errors on macOS, starting with `elf.h` and Linux `mcontext` (PROVEN) | Not a library |
| Runs part of an arm64 process | Yes, by design | Only under an ARM64EC ntdll that Raven would have to write | No (Linux userspace) | No (PROVEN, see below) |
| Translation speed | 365 ns per guest instruction, median (PROVEN) | NOT verified: no host can run it here (see below) | n/a | n/a |
| entry.c loop, steady state | 819 ns per call with LDAPR/STLR, 1.65 times native (PROVEN) | NOT verified | n/a | 414 ns, whole process translated (PROVEN) |
| Guest to native call | 2.3 ns (PROVEN) | NOT verified | n/a | No boundary |
| Lifetime on macOS | Raven's | Raven's, as a fork | n/a | General use ends with macOS 27; macOS 28 keeps a subset for older games (INFERRED from Apple's Rosetta page and Support 102527) |

### FEX

FEX was cloned at upstream HEAD be66e03d (2026-11-16). It was also checked out at 1cc4b93e (FEX-2607), the commit Proton and Bottles'
Soda aarch64 build use, into `/Users/pietrodc/dev/tools/raven-mac/fex` and `fex-2607`.

Build results, with llvm-mingw 20261104 (clang 23.1.5):

- **HEAD.** `libarm64ecfex.dll` (ARM64EC, 5.5 MB) and `libwow64fex.dll` (ARM64, 5.0 MB) build with
  `-DTUNE_CPU=none`. Without that flag, configure reads `/proc/cpuinfo` (PROVEN).
- **FEX-2607.**
  - Two missing includes against this libc++: `std::strtoull` in `StringConv.h`, and `va_start` in `CRT/IO.cpp`. With
    `-include cstdlib -include cstdarg`, `libarm64ecfex.dll` builds (PROVEN).
  - `libwow64fex.dll` does not link: libc++'s `locale_win32.cpp` imports `GetACP` and `GetLocaleInfoEx`, and FEX links only ntdll
    (PROVEN).
  - The Soda build uses an older pinned toolchain (INFERRED).

What the DLLs need from Raven (PROVEN by `llvm-readobj` on the built DLLs; the lists are in `fex-eval/`):

- **Imports.** `libarm64ecfex.dll` at HEAD imports 126 functions from ntdll.dll: 36 `Nt*`, 55 `Rtl*`/`Ldr*` and 35 others, CRT
  functions (`memcpy`, `sprintf`, `wcs*`, `sin`/`cos`...) and `ProcessPendingCrossProcessEmulatorWork`. Among them are the ARM64-only
  `RtlIsEcCode` and `RtlWow64SuspendThread`. The FEX-2607 build has 120 imports: 31 `Nt*`, 53 `Rtl*`/`Ldr*` and 36 others, two of
  them from `api-ms-win-core-localization`. `libwow64fex.dll` at HEAD has 131: 38, 56 and 37.
- **WoW64 imports.** `libwow64fex.dll` adds `NtContinue`, `NtRaiseException`, `RtlWow64Get/SetThreadContext`,
  `RtlWow64GetCurrentCpuArea`, `__chkstk`, and the `wow64.dll` exports `Wow64SystemServiceEx` and
  `Wow64ProcessPendingCrossProcessItems`.
- **Emulator interface it exports.**
  - ARM64EC (`xtajit64` replacement): `ProcessInit`, `ProcessTerm`, `ThreadInit`, `ThreadTerm`, `BeginSimulation`, `ResetToConsistentState`,
    `NotifyMemoryAlloc/Free/Protect`, `NotifyMap/UnmapViewOfSection`, `BTCpu64FlushInstructionCache`, `FlushInstructionCacheHeavy`,
    `BTCpu64NotifyMemoryDirty`, `BTCpu64NotifyReadFile`, `BTCpu64IsProcessorFeaturePresent`, `UpdateProcessorInformation`.
  - Three DATA exports that ntdll's dispatcher jumps through: `DispatchJump`, `RetToEntryThunk` and `ExitToX64`.
  - WoW64: the 22 `BTCpu*` functions plus `__wine_get_unix_opcode`.
- **What it reads out of ntdll at run time** (source: `Source/Windows/ARM64EC/Module.cpp` and `Common/FEXUnixLib.cpp`):
  - The ARM64EC load-config metadata (`CHPEMetadataPointer`) and its redirection table, used to map x64 fast-forward stubs to native code.
  - `KiUserExceptionDispatcher`, used for exception delivery.
  - Either Wine's `__wine_syscall_dispatcher` and `__arm64ec_syscall_ffs`, or Windows' syscall stubs.
  - Optionally Wine's `__wine_unix_call_dispatcher` and `wine_get_version`.
- **The unix side** (`libarm64ecfex.so`/`libwow64fex.so` in Soda, `Source/Windows/UnixLib/FEXUnixLib.cpp`, 175 lines at
  FEX-2607). It holds six unix calls: hardware TSO (`prctl(PR_SET_MEM_MODEL)`), the kernel's unaligned-atomic emulation
  (`PR_ARM64_SET_UNALIGN_ATOMIC`), `madvise`, VMA naming, and two for the statistics shared memory. Nothing in it does JIT,
  signals or futexes: those reach FEX through ntdll. For macOS it fails on `sys/prctl.h` (PROVEN).
  - In Raven it would become a few native handlers. TSO: not supported in-process, and a vCPU's ACTLR_EL1.TSOEN in a VM.
    Unaligned atomics: no kernel switch, so FEX's own backpatching. `madvise`: as is. Naming and statistics: no-ops.
- **Code memory.** FEX allocates its code buffer as `PAGE_EXECUTE_READWRITE` (`AllocatorHooks.h`), then writes and runs it from the
  same thread without any switch. In a macOS process that fails (PROVEN, `build-translate/probe/rwx.c`): an RWX mapping without
  `MAP_JIT` is refused, and writing to a `MAP_JIT` page without `pthread_jit_write_protect_np(0)` raises SIGBUS. Whether the
  `allow-unsigned-executable-memory` entitlement changes this is NOT verified. FEX's 4 KiB page constant (`FEX_PAGE_SIZE`)
  against the host's 16 KiB pages is a second porting cost (PROVEN that `pagesize` prints 16384).
- **Upstream policy.** FEX's AGENTS.md restricts how contributions may be produced. Nothing here is meant for upstream, and no FEX code is copied into Raven.

Hosting FEX therefore means four pieces of work:

1. An ARM64X ntdll with CHPE metadata, x64 fast-forward stubs and the ARM64EC dispatcher (`check_icall`,
   `dispatch_call_no_redirect`, `dispatch_ret`).
2. The 120 to 130 functions above.
3. A FEX fork that switches W^X around its code writes and handles 16 KiB host pages, unless the process model is a VM with 4 KiB
   pages and RWX allowed in guest page tables.
4. Wine's unix-call ABI or a replacement for it.

That is what Bottles does on Linux ARM64 with Wine. On macOS it would be CodeWeavers' path, also with a fork (UNCERTAIN: the
external-facts note, section 3, has it only from a search snippet).

#### FEX as an engine inside Raven

Raven has its own loader and its own ntdll, so it would not use FEX the way Wine does. There are two ways to use FEX inside Raven.

**A. Load `libarm64ecfex.dll` as Wine does.**

- **What Raven provides:**
  - an ARM64EC-capable loader for ARM64X images: CHPE metadata, the redirection table, and the EC code bitmap that
    `RtlIsEcCode` answers from;
  - an ntdll exporting the 120 to 131 functions above, built as ARM64EC;
  - the dispatcher pieces ntdll owns (`check_icall`, `dispatch_call_no_redirect`, `dispatch_ret`), which jump to FEX's
    `DispatchJump`, `RetToEntryThunk` and `ExitToX64`;
  - syscall numbering FEX can find (Windows' stubs or Wine's `__arm64ec_syscall_ffs`);
  - `KiUserExceptionDispatcher` and `NtContinue` semantics for exceptions.
- **Entry points Raven calls:** `ProcessInit`, then per thread `ThreadInit` and `BeginSimulation`. The `Notify*` and
  `BTCpu64FlushInstructionCache` functions go on every memory change.
- **The unix side** becomes six native handlers in Raven, or none: FEX runs without its unix library (`UnixLib::Available()`
  is false).
- **macOS specifics.**
  - FEX's code buffer is RWX: in-process it needs a fork that switches W^X, or a VM.
  - FEX's code is ARM64EC, so it uses x18 as the TEB. In a macOS process every call out of it into Raven's macOS-facing code
    needs the custom-x18 toggle described below.

**B. Link FEXCore as a library and write Raven's own glue in place of `Source/Windows`.**

- FEXCore's interface is `FEXCore::Context` (`CreateNewContext`, `InitCore`, the thread entry), a `SyscallHandler` and a
  `SignalDelegator`. The Windows glue that drives it is 7,266 lines.
- This bypasses the ARM64EC ntdll entirely.
- FEXCore is 92,332 lines and its CMake refuses Darwin (PROVEN, `fex/build-macos-native.configure.log`: "Unsupported system type
  Darwin"). A native port means porting FEXCore's Linux host code (signals, `mmap` with 4 KiB pages, `/proc`) to macOS, as a fork.
  The maintainers have no macOS plans (external-facts, section 3).

**How it weighs against our own translator on the same path (roadmap item 8):**

| | FEX path A | FEX path B | Own translator |
|---|---|---|---|
| Code Raven maintains | An ARM64EC loader and ntdll (new), plus a fork patch | Glue in the size of FEX's 7.3k-line Windows layer, plus a fork of 92k lines of FEXCore | About 5k lines today, growing with SSE/AVX/x87 |
| x86 coverage | Complete (SSE to AVX2, x87) | Complete | The integer subset (see the limits) |
| Effort to first app | Large: the ARM64EC loader and ntdll come before anything runs | Large: the Darwin port of FEXCore | Done for Raven's five simplest clients. SSE/AVX and x87 are the large remaining items |
| Performance on the same workloads | NOT verified | NOT verified | Measured below |

FEX's speed could not be measured on this machine:

- `which docker container colima limactl podman orb multipass` finds none of them (PROVEN), so there is no Linux arm64 guest to run
  FEX in.
- The macOS build is refused at configure.
- The DLLs need a host ntdll.

The decision below is therefore provisional on that one number. The next step is a Linux arm64 VM (Hypervisor.framework, 4 KiB
granule), running FEXInterpreter on bench_entry_host.c built as an x86-64 ELF next to a native build.

### Box64

MIT. Upstream has an ARM64EC variant (`wine/box64ec`) and a WoW64 one (`wine/wow64`), but the library itself is Linux userspace. A
native macOS configure succeeds. The build then stops with 577 errors (PROVEN, `/Users/pietrodc/dev/tools/raven-mac/box64-build.log`):
303 of them are `elf.h` not found, the rest are Linux `ucontext`/`mcontext` layouts and `sched_getcpu`. The `box64ec` DLL was not
built (NOT verified).

### Rosetta

No supported way exists to run x86 code inside an arm64 process (PROVEN, `build-translate/probe/probe.c`):

- An arm64 process that maps x86 bytes and jumps to them gets SIGILL.
- `libRosettaRuntime` is an `MH_EXECUTE` arm64 binary that exports only `__mh_execute_header` and `___crashreporter_info__`: it has
  no API.
- Apple documents that a process is either all arm64 or all translated (INFERRED from external-facts section 1).

The supported partial uses are two:

- **A whole x86_64 process.** Its entry.c loop result is below.
- **Rosetta for Linux.** It runs inside a Virtualization.framework guest, not Hypervisor.framework.

Neither fits Raven's mixed model, and general Rosetta support ends after macOS 27.

`os_cross_arch_is_supported(OS_CROSS_ARCH_X86_64)` (macOS 26.6) only reports whether the kernel features a user-space
compatibility layer needs are present. It is no translator (PROVEN from `<os/arch/arm64.h>` in SDK 27.2).

## Measured numbers

The machine is a MacBook Air M5 on macOS 27.2.1. Each metric has 30 runs. The raw values are in `build-translate/results/*.csv` and
the summary is in `summary.csv`.

Run with `python3 native/translate/bench_run.py BUILD RESULTS notepad++.exe 30`, on AC power. `pmset` and `uptime` before and after
the run are in `bench_conditions.txt`: AC attached both times. The machine was shared with other work (load average 8 to 9 over one
minute), so the entry.c rows interleave every configuration within each round. The benchmark threads ask for
`QOS_CLASS_USER_INTERACTIVE`. An earlier run under a load average of 28 to 38 gave the same ordering with medians 1.3 to 2 times
higher. It was discarded.

| Metric | Median | p95 | Stddev |
|---|---|---|---|
| Translation, ns per guest instruction (linear sweep of Notepad++'s code, 1,176,498 instructions in 403,423 blocks; the time includes attempts that translate nothing and the decoding that skips them) | 365 | 460 | 59 |
| Translation, ns per block | 1,065 | 1,341 | 172 |
| Translate and place one block in the runtime, ns (cache write, W^X switch, icache flush, page watch) | 1,105 | 1,781 | 311 |
| Load one block from the ahead-of-time cache, ns | 515 | 924 | 193 |
| Whole Notepad++ image: translate, ms | 446 | 718 | 125 |
| Whole Notepad++ image: load from cache (45 MiB file), ms | 208 | 373 | 78 |
| entry.c, one call of 1,000 iterations, native arm64, ns | 497 | 522 | 27 |
| entry.c, x86_64 Mach-O under Rosetta, ns | 414 | 417 | 17 |
| entry.c translated, plain accesses (correct only with hardware TSO), ns | 814 | 827 | 39 |
| entry.c translated, LDAPR/STLR, ns | 819 | 839 | 39 |
| entry.c translated, DMB barriers, ns | 3,136 | 3,246 | 392 |
| Guest calls a native function and returns (api_loop: call through the IAT, thunk, native, ret), ns | 2.27 | 2.64 | 0.14 |
| Native code calls a guest function and returns (`Runtime::call`), ns | 14.8 | 15.1 | 0.16 |

What the numbers say:

- **Steady state is 1.65 times native and 2.0 times Rosetta** on entry.c's loop. Rosetta beating the native arm64 build is itself
  PROVEN here. Why is NOT verified: the two compilers emit different loops.
- **Memory ordering is not the gap on this loop.** LDAPR/STLR cost under 1% over plain accesses here, because the alignment checks
  drop out for constant addresses. The gap is code quality, INFERRED from reading the emitted code (no per-instruction profile was
  taken):
  - `lea` takes three instructions where one would do;
  - the block's last compare keeps the PF and AF sources;
  - every exit normalizes the carry.
- **DMB barriers cost 3.8 times LDAPR/STLR.** The spike measured 1.8 to 2.0 ns per `dmb` (`spikes/results/barriers_1t.jsonl`).
  LDAPR/STLR is the mode to use wherever the CPU has FEAT_LRCPC, as the M5 does.
- **Hardware TSO in a VM** would take the "plain" row, under 1% faster on this loop. Real code may gain more (NOT verified):
  Notepad++'s sweep emits 474,706 ordered accesses and 278,679 alignment checks of two instructions each, out of 8.8 million host
  instructions.
- **Translation emits 7.5 host instructions per guest instruction**, and the translator itself is unoptimized: it decodes the
  instruction after a block twice and allocates per block (INFERRED from the code, not profiled).
- **The ahead-of-time cache loads blocks 2.1 times faster than translating them.** Placing a block takes one W^X switch, one icache
  flush and one `mprotect`, likely the bulk of the load time (INFERRED, not profiled). Batching those per cache load is the next
  step.
- **Crossing into native code costs 2.3 ns** per call and return, including the loop around it and the native function. The
  register mapping is why no argument moves.
- **Calling into the guest costs 15 ns**: the C++ dispatcher, the enter and exit trampolines, and saving 20 callee-saved registers.

## Decision

Write the translator, and keep FEX as the fallback to compare against. The reasons:

1. **FEX is a bigger porting job.** Hosting FEX's DLLs means writing an ARM64X ntdll, the ARM64EC dispatcher, Wine's unix-call ABI
   or a substitute, and keeping a FEX fork for W^X and 16 KiB pages. Our own core is about 5,000 lines with its tests today:
   - it runs all five of Raven's simplest clients;
   - it matches Rosetta on 10,000 random instruction sequences: registers, memory and every defined flag except cmpxchg's
     non-ZF flags;
   - it produces translations for 95% of Notepad++'s code bytes in a linear sweep, a number that includes padding. Correctness is
     proven only on the clients and the random sequences.
2. **Raven controls the boundary.** The ARM64EC-like boundary below is Raven's own design, so the thunks can be as cheap as the
   register mapping allows.
3. **The core is OS-neutral C++20**, so it serves Linux ARM64 as well (roadmap item 8).

FEX is better today at breadth: SSE/AVX, x87, a mature JIT and a code cache. Its numbers could not be measured here because it does
not run without a host ntdll (NOT verified). The translator therefore keeps FEX's model (`BTCpu*`-style notifications,
`ProcessInit`/`ThreadInit`) at its outer interface, so swapping engines stays possible.

## Register mapping

The mapping is Microsoft's ARM64EC one (https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi), with two changes:

| x64 | AArch64 | x64 | AArch64 |
|---|---|---|---|
| RAX | x8 | R8 | x2 |
| RCX | x0 | R9 | x3 |
| RDX | x1 | R10 | x4 |
| RBX | x27 | R11 | x5 |
| **RSP** | **x28** (ARM64EC: sp) | R12 | x19 |
| **RBP** | **x24** (ARM64EC: x29) | R13 | x20 |
| RSI | x25 | R14 | x21 |
| RDI | x26 | R15 | x22 |

- **RSP is not sp.** x64 code leaves RSP at 8 modulo 16, and a load through a misaligned sp faults: SIGBUS, PROVEN by `probe.c`. The
  native thunk aligns into sp only when it calls out.
- **RBP is not x29.** macOS requires x29 to point at a valid frame record at all times (Apple's arm64 ABI page).
- **Both replacements** come from the registers ARM64EC forbids to native code (x13, x14, x23, x24, x28), which belong to the
  emulator.
- **The others.** x23 points at `CpuState`. x13 and x14 hold the PF and AF sources. x7 holds the block's last RIP-relative address.
  x6, x9 to x12 and x15 to x17 are scratch.
  x29 and x30 stay the host's.
- **x18.** It does not matter for translated x64 code. The TEB comes from `gs:`, which the translator reads from `CpuState.gs_base`,
  so it never touches x18. It matters only for native Windows ARM64 and ARM64EC code (see the boundary below).
- **Why this mapping.** RCX, RDX, R8 and R9 are already x0 to x3, and every x64 callee-saved register is AAPCS64 callee-saved. A call
  into native code therefore moves no register, and native code preserves everything the x64 caller expects. The XMM registers do
  not yet follow this rule (see the limits).

## Flags

- **SF, ZF and OF** are N, Z and V.
- **CF is C, possibly inverted.** After SUBS, C is the inverse of a borrow, so CF's sense depends on the last operation. The
  translator tracks the sense per instruction and emits `cfinv` (FEAT_FlagM) only when a reader needs the other sense or a block
  ends. At block boundaries C is always inverted, the form compares leave.
- **PF and AF** are lazy: only their sources are kept (x13 is the result, x14 the xor of the operands), and only when a later reader
  may exist.
- **Liveness.** A backward pass over each block drops all flag work whose flags are redefined before being read. At the end of a
  block every flag counts as read.
- **8- and 16-bit operations** run on operands shifted to the top of a W register, so N, Z, C and V come out for the narrow width.
- **inc and dec** keep CF with `rmif`.
- **Not supported yet: narrow `adc`/`sbb` and `cmovp`.** These end the block as `unsupported`, an explicit exit, never a wrong
  result.

The oracle test found two places where Rosetta differs from the SDM, Bochs and QEMU. Both are INFERRED, since no x86 hardware was
available here:

- **`cmpxchg` flags.** Rosetta leaves the flags of destination minus accumulator; the translator follows the SDM (accumulator minus
  destination). The test compares only ZF there.
- **A 32-bit `cmpxchg` that succeeds** clears the upper half of RAX under Rosetta. The translator follows Rosetta.

## Memory ordering (TSO)

`TranslatorOptions::ordering` picks how ordered accesses are emitted, and `TranslationStats` counts every barrier and ordered access:

| Mode | Load | Store | Used when |
|---|---|---|---|
| `hardware` | `ldr` | `str` | The CPU runs in TSO mode |
| `rcpc` (default) | `ldapr` | `stlr` | FEAT_LRCPC, present on M5 (PROVEN, `sysctl hw.optional`) |
| `barriers` | `ldr; dmb ishld` | `dmb ishst; str` | No LRCPC |

- **Alignment.** LDAPR and STLR fault on a misaligned address; x86 does not. The oracle test crashed on exactly this before the fix
  (PROVEN, `semantics_test_failing.txt`). Every ordered access wider than a byte now tests its alignment and takes an out-of-line
  `ldr`/`str` plus barrier when misaligned. That costs two instructions per access: 295,921 checks across Notepad++'s code.
- **Stack accesses** can skip ordering with `order_stack = false`. It is off by default because another thread may read a stack it
  was handed a pointer into.
- **Locked operations** use LSE atomics with acquire and release semantics (`casal`, `ldaddal`, `swpal`). A split lock, meaning a
  misaligned locked access, is not handled (NOT verified, faults).

The spikes in `spikes/results/litmus_*` give two branches:

- **In-process on macOS.** No public interface turns on TSO for a native thread. A native arm64 litmus run shows message-passing
  reorderings (PROVEN by the spike). The translator must use `rcpc`. Its cost is the gap between `entry_translated_ldapr_stlr` and
  `entry_translated_plain` below.
- **In a Hypervisor.framework VM.** A vCPU with ACTLR_EL1.EnTSO set shows zero reorderings in the same litmus (PROVEN by the spike,
  `litmus_vm_tso1`). Code translated with `hardware` ordering is then correct and as fast as the plain row. Translation itself does
  not change: the mode is one option.

## The native and translated boundary

Names follow Microsoft's: an *entry thunk* takes an x64 caller into native code, and an *exit thunk* takes a native caller into x64
code.

- **x64 calls a Raven native function** (`native_thunk`, the entry thunk, PROVEN by the `startup`, `api_loop` and `arguments_client`
  tests).
  - The IAT holds native addresses, as on ARM64EC.
  - An indirect call misses the lookup table once. The dispatcher asks `GuestMemory::is_native`, the analogue of the EC code
    bitmap, and installs the function's thunk as the target.
  - From then on the call costs one table probe.
  - The thunk stores RSP for callbacks, loads arguments five to eight from the x64 stack into x4 to x7, calls the function, moves x0
    to RAX, checks the stop flag (`ExitProcess`) and returns as `ret` does.
  - Nothing else moves, thanks to the register mapping.
  - Each thunk is generated per signature (integer argument count today). Float arguments, which take XMM0 to XMM3 by position in x64
    and the next free v register in AAPCS64, are not done yet.
- **Native code calls x64** (`Runtime::call`, the exit thunk, PROVEN by the `callback` test: a native function called from guest code
  calls back into guest code 1,000 times).
  - It pushes a return address that the lookup table maps to a stub, which exits with `returned`.
  - Nested calls keep the outer run's host frame and RIP.
- **Fast-forward sequences.** The x64 stubs of Raven's exports are what a hooking program patches. If an export's first bytes are
  still the stub, the code map sends the call to native code directly. Once a hook rewrites them, the bytes are x64 code and get
  translated, as on ARM64EC. Code writes are watched, so a patch is noticed.
- **CHPE-like metadata for Raven's DLLs.** Each native export needs a record of its x64-visible entry (the fast-forward stub), the
  native address, and a signature id that picks the thunk. The CHPE metadata in an ARM64EC image's load config holds this. Raven's
  build can emit it as a table beside each DLL's exports, so that no thunk has to be generated at load time.
- **Native Windows ARM64 and ARM64EC code** (not translated) uses x18 as the TEB.
  - macOS 26.4+ has `os_set_custom_x18_abi_enabled` (PROVEN present in SDK 27.2 `<os/arch/arm64.h>`). While it is on, x18 is a
    general register for the thread, but the thread must not call any macOS library code, and each toggle destroys x18's value.
  - Raven's own ARM64 code serving an API call needs no toggle: it never reads x18 and never calls macOS while it runs Windows-ABI
    code, so the thread stays in custom mode.
  - A toggle is needed only where Raven's runtime calls into macOS (file, Metal, AppKit). That means two toggles per such API call:
    off, then on again with x18 reloaded from the thread's TEB pointer.
  - Calls into macOS are batched on the host side of the boundary, so one guest API call costs at most one off/on pair, and calls
    served entirely by Raven's ARM64 code cost none.
  - Signals must be masked or their handlers must check `os_custom_x18_abi_enabled()`.
  - Translated x64 code never needs the mode.
  - The cost per toggle is being measured by the spike stream (NOT verified here).

## Ahead-of-time cache

- **Code moves as a blob.** Translated code holds no absolute host address. The exit trampoline and the lookup table come from
  `CpuState` through x23, chain exits start as branches to their own stubs, and guest addresses are immediates. A block can therefore
  be written to disk and loaded anywhere.
- **Placement.** Cached blocks run from a different place in the code cache than where they were first translated (PROVEN by the
  `ahead-of-time cache` test).
- **The key** covers the image bytes, the base it was mapped at, the translator version and the options that change code (ordering,
  stack ordering, block size).
  - This deviates from keying by RVA: blocks embed guest addresses as immediates, so an image mapped at another base misses the cache.
  - The plan is to store blocks by RVA with a fixup list for each guest-address immediate (`mov_imm` sites), and apply the delta on
    load as a base relocation does.
- **Loading.** A cache with another key loads nothing (PROVEN by the `ahead-of-time cache` test).
- **Chains** are not stored. They are re-made lazily on the first run.
- **Hashing.** It uses FNV-1a today. A shipped cache should use SHA-256 over the image, as Raven's authenticode code already has.
- **Coverage.** A cache is filled by the blocks one run translated, or by an ahead-of-time sweep (`Runtime::translate_ahead`, which
  the bench runs over a whole image).
- **Future work.** The function starts in `.pdata` are better roots than a linear sweep, and exports and the entry point should be
  added too.

## Self-modifying and generated code (JIT engines)

- **Watching.** Each translated block's guest pages are watched through `GuestMemory::watch_code`. In-process that is `mprotect`
  read-only; in a VM it would be the stage-2 or guest page tables.
- **On a write fault** the runtime drops every block of the page:
  - incoming chains are restored to their stubs;
  - lookup entries are cleared;
  - the page is unprotected and the write retried.
  - PROVEN for a write from native code (rewriting an immediate changes the result from 73 to 42), and for a store from translated
    code: `rewrite.exe` rewrites an instruction of its own and runs it again (the `self-modifying code, guest write` case).
- **16 KiB host pages against 4 KiB PE sections.** A data write next to code in the same 16 KiB page throws away that page's
  translations.
  - In-process, the plan for hot pages is the one FEX and Rosetta use. A page that is invalidated repeatedly switches to
    checksum validation: translations keep a hash of their guest bytes, checked on entry, and the page stays writable.
  - In a VM with a 4 KiB IPA granule (macOS 26+) the problem shrinks to the PE's own granularity.
- **JIT engines** (.NET, V8, LuaJIT) allocate RWX memory and write code continuously. The same mechanism applies:
  - the `NtProtectVirtualMemory` and `FlushInstructionCache` notifications (FEX's `NotifyMemoryProtect` and
    `BTCpu64FlushInstructionCache` equivalents) drop translations eagerly;
  - pages that keep toggling switch to the checksum mode.
- **Limit.** Translations are not freed until the cache is reset. The code that just wrote may still be running its old block on
  the way out.

## WoW64 (32-bit x86)

The decoder handles 64-bit mode only today. The plan:

1. Add a 32-bit mode to the decoder: no REX, the inc/dec short forms at 40 to 4F, 16- and 32-bit addressing.
2. Keep the eight x86 registers in the same host registers as their x64 counterparts.
3. Zero-extend every guest address computation to 32 bits (`mov w16, w16`; the `address_size == 4` path exists already).
4. Add a WoW64-style CPU interface: `BTCpuSimulate`, `BTCpuGetContext`/`SetContext` and the `Wow64SystemServiceEx` call-out, so that
   32-bit code reaches Raven's 64-bit native thunks through the same boundary.

x87 needs a soft-float or F64-based path. It is the largest missing piece for both modes.

## What runs on Linux ARM64 unchanged

- **Unchanged:** the decoder, the emitter, the translator, the runtime (dispatch, chaining, lookup table, thunks, invalidation, AOT
  cache), the tests' oracles for the decoder and emitter, and the `GuestMemory`/`CodeAllocator` interfaces.
- **Per-OS:** only `platform_*.cpp`. Linux uses one memfd mapped twice, writable and executable, so no switch is needed.
  `platform_linux.cpp` is written but not compiled here (NOT verified).
- **x18:** it is free on Linux, which removes the custom-ABI toggle question.
- **TSO:** upstream Linux has no TSO switch, so `rcpc` stays the default. Apple's kernel patch adds `PR_SET_MEM_MODEL` on Apple
  hardware.
- **The semantics oracle** needs an x86 machine or QEMU to produce its results file there. It is built only on macOS today.

## Limits of the current core

- **Not translated yet:**
  - SSE/AVX/x87;
  - string instructions and `rep`;
  - `mul`/`div`, `bt*`, rotates, `pushf`/`popf`;
  - 16-bit push and pop;
  - narrow `adc`/`sbb`;
  - narrow shifts by `cl` or beyond their width;
  - `cmovp`.
  
  Each ends the block with an `unsupported` exit that names the instruction's address.
- **Native thunks:** integer arguments only, up to eight. The XMM6 to XMM15 upper halves will need saving once SSE is translated.
- **Threads:** one guest thread. The dispatcher, block maps and invalidation take no locks. Multi-threading needs:
  - a lock around translation;
  - stop-the-world, or epochs, for invalidation and chain patching;
  - a per-thread `CpuState`.
- **Test-only process model:** `TestProcess` (test-only) maps images with `mmap` hints and handles code-write faults in a SIGSEGV/SIGBUS
  handler. Invalidation running inside that handler is safe only because the fault comes from translated code. The final process
  model replaces it.
