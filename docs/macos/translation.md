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
| Translation speed | 487 ns per guest instruction, median (PROVEN) | First call minus a warm call: 33 to 90 µs per workload on Neoverse N2, ours 26 to 86 µs (PROVEN, Linux arm64 below) | n/a | n/a |
| entry.c loop, steady state | 371 ns per call with LDAPR/STLR, 1.15 times native (PROVEN) | Linux arm64: 1.29 times native; ours 1.06 there (PROVEN) | n/a | 268 ns, whole process translated (PROVEN) |
| Guest to native call | 1.5 ns (PROVEN) | NOT verified | n/a | No boundary |
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

**In-process on macOS: the W^X patch.** Either path in a macOS process needs FEX to keep its code in `MAP_JIT` memory and to switch the
current thread with `pthread_jit_write_protect_np` around every write to it. At FEX-2607 those writes sit in four places (INFERRED from
the instruction-cache flushes that follow them, `grep ClearICache|__builtin___clear_cache` in FEXCore, 13 sites):

| File | Sites | What it writes |
|---|---|---|
| `Interface/Core/JIT/JIT.cpp` | 5 | Emitting blocks and linking them |
| `Utils/ArchHelpers/Arm64.cpp` | 6 | Patching an unaligned atomic into a half-barrier sequence, from the SIGBUS handler |
| `Interface/Core/Dispatcher/Dispatcher.cpp` | 1 | The dispatcher |
| `Interface/Core/CodeCache.cpp` | 1 | Loading the code cache |

The allocation itself goes through `AllocatorHooks.h` (`PAGE_EXECUTE_READWRITE` on Windows, `mmap` on Linux) and would take
`MAP_JIT`.

- **Per thread works.** The switch is per thread, and FEX writes code on the thread that runs it, so this fits. The same holds for the
  backpatching, which runs in the faulting thread's signal handler.
- **The patch is small.** Wrap the 13 sites and change the one allocation.
- **The fork is the cost.** It has to follow every upstream change to those files, and upstream's AGENTS.md policy means the patch
  stays Raven's own.
- **16 KiB host pages.** FEX tracks code pages in 4 KiB units (`FEX_PAGE_SIZE`), but macOS protects 16 KiB at a time. That needs a
  second, larger change in its invalidation tracking (`InvalidationTracker` in the Windows layer, `mprotect` granules in FEXCore),
  NOT verified.

**How it weighs against our own translator on the same path (roadmap item 8):**

| | FEX path A | FEX path B | Own translator |
|---|---|---|---|
| Code Raven maintains | An ARM64EC loader and ntdll (new), plus a fork patch | Glue in the size of FEX's 7.3k-line Windows layer, plus a fork of 92k lines of FEXCore | About 5k lines today, growing with SSE/AVX/x87 |
| x86 coverage | Complete (SSE to AVX2, x87) | Complete | The integer subset, SSE and SSE2 ([sse.md](sse.md); see the limits) |
| Effort to first app | Large: the ARM64EC loader and ntdll come before anything runs | Large: the Darwin port of FEXCore | Done for Raven's five simplest clients. x87 and AVX are the large remaining items |
| Performance on the same workloads | Linux arm64 table below: 1.0 to 1.8 times native | Same engine as path A | Linux arm64 table below: 1.06 to 2.15 times native |

#### FEX against our translator on Linux arm64

FEX runs only on Linux and Windows, so the two engines were compared on GitHub's `ubuntu-24.04-arm` runners, in the private repository
`fabricatorsltd/raven-macos-bench`. Workflow run 44287829572 is PROVEN; its raw CSVs, `cpu.txt` and logs are in
`build-translate/results/linux-arm64/run-44287829572/`.

**The machine.** Azure Neoverse N2, 2 vCPUs, kernel 7.0 (`cpu.txt`). It has `lrcpc`, `ilrcpc` (LRCPC2), `atomics` (LSE) and `flagm`.
It is not an M5, so only ratios to native count.

**The engines.**

- **FEX** at 1cc4b93e (FEX-2607, the Proton/Soda commit), built natively with its defaults (LTO, `TUNE_CPU=native`). Its `FEX` binary
  runs each workload as a freestanding static x86-64 ELF, with no root filesystem (PROVEN, `fex-smoke.txt`).
- **Raven's translator**, built for Linux arm64. Its tests pass there: decoder, emitter and all 14 runtime cases (PROVEN,
  `ours-ctest.log`). That also proves `platform_linux.cpp`, with the memfd mapped twice.
- **Native**, the baseline: the same C built by the system clang for arm64.

**The workloads** come from one source, `bench_harness.c` plus `testdata/workloads.c` and Raven's `testdata/native_cpu/entry.c`:

- entry.c's loop;
- bitwise CRC-32 over 4 KiB;
- FNV-1a over 32 KiB;
- an xorshift histogram (a load and a store per step).

**Method.**

- **Modes compared.** Ours: plain, LDAPR/STLR, DMB. FEX: `FEX_TSOENABLED=0`; TSO on, its default (LRCPC and LRCPC2 used); TSO on with
  `FEX_HOSTFEATURES=disablelrcpc,disablelrcpc2` (barriers).
- **Rounds.** 30 rounds. Each round runs every workload under every configuration once, interleaved. Each run records one first call,
  which includes translation or JIT, then one timed run of 20 to 2,000 calls.

Medians, as ratios to native (warm). Every warm row's stddev is under 6% of its median (largest: ours DMB on CRC-32, 5.8%); the
CSVs have p95 and stddev for every row.

| Workload | native ns | ours plain | ours LDAPR/STLR | ours DMB | FEX TSO off | FEX TSO on (LRCPC) | FEX TSO, no LRCPC |
|---|---|---|---|---|---|---|---|
| entry.c loop (per 1,000 steps) | 450 | 1.05 | **1.06** | 7.55 | 1.27 | **1.29** | 10.63 |
| CRC-32, 4 KiB | 13,456 | 1.79 | **1.77** | 1.77 | 1.39 | **1.41** | 1.40 |
| FNV-1a, 32 KiB | 29,441 | 1.14 | **1.16** | 2.57 | 1.00 | **1.00** | 1.00 |
| Histogram, 32,768 steps | 38,153 | 2.16 | **2.15** | 2.38 | 1.81 | **1.81** | 4.32 |

First call, translation or JIT included, as a ratio to native's first call (whose own stddev is large: page faults):

| Workload | ours LDAPR/STLR | FEX TSO on |
|---|---|---|
| entry.c | 2.18 (26.7 µs) | 2.72 (33.3 µs) |
| CRC-32 | 2.69 (77.4 µs) | 2.90 (83.4 µs) |
| FNV-1a | 2.70 (120.0 µs) | 2.69 (119.1 µs) |
| Histogram | 2.50 (120.7 µs) | 2.33 (112.4 µs) |

Our translator's own counter puts translation at 12.8 µs for entry's 38 guest instructions (338 ns each) and 54.8 µs for FNV's 308
(178 ns each) (`ours_translation.csv`).

What it says:

- **Steady state.**
  - On the store-and-reload loop (entry.c) ours is ahead: 1.06 against 1.29.
  - On the three compute kernels FEX is ahead: CRC-32 1.41 against 1.77, FNV 1.00 against 1.16, histogram 1.81 against 2.15.
  - FEX's multiblock JIT with register allocation produces tighter code than our block-at-a-time translator. Our gap on those kernels
    is code quality, not memory ordering: our plain and LDAPR/STLR rows match.
  - **Before** the register-copy removal (commit "use 32-bit registers in place...", run 44285513934 in `run-44285513934-before-copy-removal/`) the
    same kernels were 3.2, 1.16 and 4.36 times native on this machine. The same change on the M5 moved entry.c from 1.65 to 1.15
    times native. A wide core like the M5 hides extra instructions that the N2 does not.
- **Memory ordering.**
  - With LRCPC, both engines pay little for TSO on these kernels. Ours: plain against LDAPR/STLR within 2% (largest: FNV-1a, 1.7%).
    FEX: TSO on against off within 2%. Both are inside the run-to-run spread.
  - Without LRCPC, both fall back to barriers and lose a lot where memory traffic is dense: entry.c 7.6 (ours) and 10.6 (FEX), the
    histogram 2.4 (ours) and 4.3 (FEX).
  - On the M5 the same LDAPR/STLR cost is 38% on entry.c (macOS table above), so the N2 and M5 differ here: NOT verified why.
- **First-call cost** (translation or JIT) is about the same for both engines. Ours is 20% lower on entry.c, and the two are within
  8% of each other on the other three.

Weighed on the same path, roadmap item 8:

- FEX would bring complete x86 coverage today and better code on compute kernels.
- FEX would also bring an ARM64EC loader and ntdll to write (path A), or a Darwin port of 92k lines (path B). Either way it needs a
  maintained W^X and 16 KiB-page fork for in-process macOS.
- Our translator matches FEX's JIT cost and the TSO cost with LRCPC, and beats it on memory-bound loops. On the compute kernels it
  takes 16 to 26% longer than FEX. The next subsection breaks that gap down by source.

The decision stays with our own translator. The measured gap is in code generation, which is ours to improve. FEX's gap is
integration work that Raven would carry forever.

#### Where the gap comes from: the code each engine emits

Static analysis only, no timing. One iteration of each workload's hot loop was counted instruction by instruction and each host
instruction was put in one category. The listings:

- **Ours:** `translate_bench --dump-block` on the PE's loop block, LDAPR/STLR and plain modes. Only the steady path is counted: the
  block up to its conditional branch, plus the chained back-edge `b`. Exit stubs and the out-of-line misaligned paths are left out.
- **FEX:** `FEX_DISASSEMBLE=blocks` and `FEX_DUMPIR` at 1cc4b93e, TSO on and off. FEX compiles each workload function as one
  multiblock.
- **Native:** `llvm-objdump` of the arm64 build.

Ours was dumped at b3c86a62. Its translator, emitter and decoder sources are identical to the copy timed in run 44287829572
(PROVEN, file comparison). FEX's and native's listings come from workflow run 44291036149 on the same runner type. All listings and the counts
(`breakdown.csv`) are in `build-translate/results/linux-arm64/fex-dump-44291036149/`.

The categories:

| Category | Instructions counted |
|---|---|
| memory | Guest loads and stores |
| ordering | Ours: the alignment `and` and `cbnz` before an ordered access wider than a byte. FEX: the `nop` it places beside each such access, which it patches when the access faults misaligned |
| address | Instructions whose result is only used as a memory address, including rebuilding a constant address |
| flags | Flag state the loop never reads: ours' PF and AF sources (x13, x14) with their constant, and `cfinv`; FEX's AF copy (`mov x27`) |
| exit | The branches that close the iteration |
| moves | Register-to-register copies |
| constants | `mov`/`movk` of an immediate inside the loop |
| work | Everything else |

Loads and stores to the CPU state (x23 for ours, x28 for FEX) are zero in every loop for both engines. Every guest register stays in
a host register, so nothing spills (PROVEN, listings).

**Confound.** Ours runs the mingw PE (llvm-mingw clang 23). FEX runs the ELF built by Ubuntu's clang. The entry.c, FNV-1a and
histogram loops are the same instructions in both builds. The CRC-32 loop is 49 x86 instructions in the PE and 48 in the ELF,
scheduled differently (PROVEN, by diffing the two disassemblies).

**How the table counts.** Native's loops are not unrolled. Every count is therefore per x86 loop iteration: 5 entry.c steps, 1 CRC-32
byte, 4 FNV-1a bytes, or 2 histogram steps. Ours is in LDAPR/STLR mode and FEX has TSO on, the configurations timed in the table
above.

Host instructions per x86 loop iteration (host instructions per guest instruction in parentheses):

| Loop (guest instructions) | Engine | Total | memory | ordering | address | flags | exit | moves | constants | work | Instructions vs native | Time vs native |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| entry.c (17) | native | 30 | 10 | 0 | 0 | 0 | 5 | 0 | 0 | 15 | 1.00 | 1.00 |
| | ours | 37 (2.2) | 10 | 0 | 3 | 3 | 2 | 8 | 0 | 11 | 1.23 | 1.06 |
| | FEX | 57 (3.4) | 10 | 10 | 20 | 1 | 1 | 4 | 0 | 11 | 1.90 | 1.29 |
| CRC-32 (49 PE, 48 ELF) | native | 29 | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 27 | 1.00 | 1.00 |
| | ours | 73 (1.5) | 1 | 0 | 1 | 2 | 2 | 10 | 18 | 39 | 2.52 | 1.77 |
| | FEX | 54 (1.1) | 1 | 0 | 1 | 1 | 1 | 0 | 12 | 38 | 1.86 | 1.41 |
| FNV-1a (15) | native | 24 | 4 | 0 | 0 | 0 | 4 | 0 | 0 | 16 | 1.00 | 1.00 |
| | ours | 30 (2.0) | 4 | 0 | 7 | 2 | 2 | 4 | 1 | 10 | 1.25 | 1.16 |
| | FEX | 19 (1.3) | 4 | 0 | 4 | 0 | 1 | 0 | 0 | 10 | 0.79 | 1.00 |
| Histogram (24) | native | 20 | 4 | 0 | 2 | 0 | 2 | 0 | 0 | 12 | 1.00 | 1.00 |
| | ours | 44 (1.8) | 4 | 8 | 4 | 3 | 2 | 6 | 0 | 17 | 2.20 | 2.15 |
| | FEX | 30 (1.3) | 4 | 4 | 4 | 0 | 1 | 0 | 0 | 17 | 1.50 | 1.81 |

Without ordering, the totals are:

- **Ours, plain:** entry.c 37, CRC-32 73, FNV-1a 27 (LDRB takes the offset that LDAPRB cannot), histogram 36.
- **FEX, TSO off:** entry.c 47, CRC-32 53, FNV-1a 18, histogram 22.

**Counts are not time.**

- **Where the counts agree with the timings.** On every loop the instruction ratio puts the two engines in the same order as the
  measured time.
- **Where they do not.** FEX's histogram loop is 22 instructions with TSO off and 30 with TSO on, and the two timings are within
  2%. Ours is 36 plain and 44 with LDAPR/STLR, and the timings are within 1%. On the N2 the alignment checks and FEX's `nop`s cost
  close to nothing, and loops like these are bound by dependency chains more than by instruction count (PROVEN for the counts and
  timings, INFERRED for the reason).

What each loop says:

- **Histogram (ours 2.15, FEX 1.81 times native).** The loop-carried path is the xorshift: three shift-and-xor pairs per step.
  - Native folds each pair into one `eor w, w, w, lsl #n`: 3 dependent instructions per step.
  - FEX emits a `lsl` and then an `eor`: 6.
  - Ours emits the x86 `mov`, the shift, then the `eor`: 9, of which 3 are copies that x86's two-operand form asks for.
  - The rest of our overhead is off that chain: the flag sources and `cfinv` (3), the second branch, and `inc dword ptr
    [rcx+4*rdx]` computing its address and alignment check twice, once for the load and once for the store.
  - So the gap to FEX is the 3 copies on the chain (INFERRED). Whether the N2 renames those copies away at no cost is NOT
    verified.
  - The gap to native would be the shifted-operand folding that neither translator does, but only if an `eor` with a shifted
    operand has the latency of a plain `eor` on the N2. That latency is NOT verified, so this half of the explanation is open.
- **CRC-32 (1.77 against 1.41).** This loop has no long serial chain, so the instruction count shows: 73 against FEX's 54 and
  native's 29.
  - **Constants (18).** 16 of them rebuild the 8 table constants every iteration; those values are not logical immediates. The
    other 2 are `and r9d, 1` and `cmp rcx, 0x1000`, which go through a scratch register although both encode as immediates.
  - **Copies (10).** 8 come from x86's `mov`-then-operate form. 2 come from results written to a scratch and copied back (`neg`,
    `inc`).
  - **FEX** also rebuilds constants inside the loop (12), but builds a repeated one only once.
  - **Native** hoists all of them and folds every `shl`/`sar 31` pair into `sbfx` or a shifted operand.
- **FNV-1a (1.16 against 1.00).** 30 against 19 and native's 24.
  - **Addresses (7).** Ours recomputes `r8 + rdx` before every load and adds the offset separately; FEX has 4, using LDAPURB's
    offset.
  - **Copies (4).** Each `imul` writes a scratch that is then copied back. Those copies sit on the `eor`-`mul` chain that bounds
    the loop.
  - **Flags and exit (4).** The flag sources and the second branch.
  - FEX's 19 instructions run at native's time with fewer instructions than native's 24. That points at the chain, not the count:
    our chain copies are the likely difference (INFERRED).
- **entry.c (1.06 against 1.29).** Ours is ahead.
  - **FEX** rebuilds the constant address of `observed` with `mov` and `movk` before each of the 10 accesses (20 instructions) and
    places a `nop` beside each.
  - **Ours** builds the address once per iteration into x7 (3 instructions). It spends 8 copies on the four `lea edx, [rcx+k]`,
    which go through the address register x16, and 3 on PF and AF sources that only the `ret` path after the loop could read.
  - For all three, store-to-load forwarding on `observed` likely sets the pace (INFERRED).

Which planned change removes what, counted in instructions per iteration from the table above. The effects are static counts, not
times, and are INFERRED:

| Change | What it removes | entry.c | CRC-32 | FNV-1a | Histogram |
|---|---|---|---|---|---|
| Register allocator, with SSA values and copy coalescing | The copies, including x86's `mov`-then-operate pairs, which become one three-operand instruction. Constants kept in spare registers across the loop, once the loop is translated as a unit | 0 | -10 copies, -16 constants | -4 | -6 |
| Multi-block translation, a loop or function as one unit | The second branch: a self back-edge becomes one `b.ne` to the loop head. x7 is built once before the loop. It also gives the other changes the loop to work on | -4 | -1 | -1 | -1 |
| Flag elimination with liveness across blocks | PF/AF sources and `cfinv`. They are dropped where the next block redefines the flags (FNV-1a and histogram: the code after the loop starts with `xor`). Where the loop exits to `ret` (entry.c, CRC-32), they move onto the exit edge | -3 | -2 | -2 | -3 |
| Address-mode folding | Histogram: one address and one alignment check for the read-modify-write. FNV-1a: one base `add`, then LDAPURB with an immediate offset (FEAT_LRCPC2: on the M5, PROVEN by `sysctl hw.optional.arm.FEAT_LRCPC2`, and on the N2, `ilrcpc`). entry.c: `lea` lowered to one `add w1, w0, #k` | -8 | 0 | -6 | -4 |

What none of the four removes:

- **Immediates the emitter misses.** `and #1` and `cmp #1, lsl #12` in CRC-32, and the `cmp` in FNV-1a: 3 instructions. This is a
  local fix in the emitter.
- **Results routed through a scratch.** 6 copies in CRC-32 and FNV-1a can be removed by writing to the destination directly, a
  local fix that needs no allocator. The table counts them under the register allocator.
- **Instruction combining:** shifted operands, and `lsl` with `asr #31` into `sbfx`. This is what native has and FEX does not. It
  shortens the histogram chain from 6 to 3 and removes 7 pairs in CRC-32. It belongs on the list after the allocator.
- **A block that loops to itself** can already chain its back-edge straight to its own head, past the x7 setup, with no
  multi-block translation: the same 4, 1, 1 and 1 instructions the table credits to multi-block translation.

With the four changes and the two local fixes, the counts would be (INFERRED):

| Loop | Ours now | Ours after | FEX | Native |
|---|---|---|---|---|
| entry.c | 37 | 22 | 57 | 30 |
| CRC-32 | 73 | 42 | 54 | 29 |
| FNV-1a | 30 | 16 | 19 | 24 |
| Histogram | 44 | 30 | 30 | 20 |

In the histogram and FNV-1a, the loop-carried chain would then match FEX's. In CRC-32, the count would fall below FEX's. Whether
the time follows is NOT verified until the quiet re-measure.

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

Run with `python3 native/translate/bench_run.py BUILD RESULTS notepad++.exe 30`, on AC power, after the register-copy removal
that the Linux runs prompted. `pmset` and `uptime` before and after the run are in `bench_conditions.txt`: AC attached both times,
load average 9 to 10 from other work. The entry.c rows interleave every configuration within each round. The benchmark threads ask
for `QOS_CLASS_USER_INTERACTIVE`. The previous run, before that change, is in `results/macos-prev/`: entry.c translated was 1.65
times native there.

| Metric | Median | p95 | Stddev |
|---|---|---|---|
| Translation, ns per guest instruction (linear sweep of Notepad++'s code, 1,176,615 instructions in 403,307 blocks; the time includes attempts that translate nothing and the decoding that skips them) | 487 | 826 | 135 |
| Translation, ns per block | 1,422 | 2,411 | 393 |
| Translate and place one block in the runtime, ns (cache write, W^X switch, icache flush, page watch) | 1,246 | 1,956 | 712 |
| Load one block from the ahead-of-time cache, ns | 591 | 924 | 213 |
| Whole Notepad++ image: translate, ms | 503 | 789 | 287 |
| Whole Notepad++ image: load from cache (44 MiB file), ms | 238 | 373 | 86 |
| entry.c, one call of 1,000 iterations, native arm64, ns | 322 | 330 | 5 |
| entry.c, x86_64 Mach-O under Rosetta, ns | 268 | 270 | 12 |
| entry.c translated, plain accesses (correct only with hardware TSO), ns | 268 | 270 | 3 |
| entry.c translated, LDAPR/STLR, ns | 371 | 376 | 5 |
| entry.c translated, DMB barriers, ns | 1,948 | 1,953 | 17 |
| Guest calls a native function and returns (api_loop: call through the IAT, thunk, native, ret), ns | 1.50 | 1.56 | 0.02 |
| Native code calls a guest function and returns (`Runtime::call`), ns | 9.6 | 15.7 | 2.6 |

What the numbers say:

- **Steady state with LDAPR/STLR is 1.15 times native and 1.38 times Rosetta** on entry.c's loop.
  - With plain accesses, which hardware TSO makes correct, the translated loop runs as fast as Rosetta's.
  - Rosetta and the plain translation both beat the native arm64 build. Why is NOT verified: the compilers emit different loops.
- **Memory ordering is now the visible cost on this loop.** LDAPR/STLR cost 38% over plain accesses (`entry.c` stores and reloads one
  global every step).
- **DMB barriers cost 5.3 times LDAPR/STLR.** LDAPR/STLR is the mode to use wherever the CPU has FEAT_LRCPC, as the M5 does.
- **Hardware TSO in a VM** would take the "plain" row: 28% faster than LDAPR/STLR on this loop in this run. On the N2 the two are
  within 2%, and the M5 figure awaits a quiet re-measure, so the hardware TSO bit is not planned (see Memory ordering). Notepad++'s sweep emits 474,933
  ordered accesses and 278,734 alignment checks out of 8.6 million host instructions, so real code pays the same kind of cost.
- **Translation emits 7.3 host instructions per guest instruction**, and the translator itself is unoptimized: it decodes the
  instruction after a block twice and allocates per block (INFERRED from the code, not profiled).
- **The ahead-of-time cache loads blocks 2.1 times faster than translating them.** Placing a block takes one W^X switch, one icache
  flush and one `mprotect`, likely the bulk of the load time (INFERRED, not profiled). Batching those per cache load is the next
  step.
- **Crossing into native code costs 1.5 ns** per call and return, including the loop around it and the native function. The register
  mapping is why no argument moves.
- **Calling into the guest costs about 10 ns**: the C++ dispatcher, the enter and exit trampolines, and saving 20 callee-saved
  registers.

## Decision

Write the translator, and keep FEX as the engine to measure against. The reasons, with the Linux arm64 comparison above:

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

FEX is better today at breadth (SSE/AVX, x87, a code cache) and on compute kernels. Its first-call cost matches ours, and our
translator is ahead on the memory-bound loop. The runtime's outer interface should follow FEX's emulator interface (`BTCpu*`-style
notifications, `ProcessInit`/`ThreadInit`), so swapping engines stays possible. That is planned, not done.

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
  into native code therefore moves no register, and native code preserves everything the x64 caller expects. XMMn is vn, as in
  ARM64EC; XMM6 to XMM15, callee-saved in x64, are whole only through CpuState ([sse.md](sse.md)).

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

**The hardware TSO bit is not needed.**

- On the N2, LDAPR/STLR runs within 2% of plain accesses on all four workloads (PROVEN, run 44287829572). The largest
  difference is FNV-1a at 1.7%, inside the run-to-run spread.
- `rcpc` therefore stays the default, both in-process and in a VM.
- On the M5, the earlier macOS run measured 38% on entry.c. A quiet re-measure is pending, so the M5 cost is NOT verified as final.
- The cross-architecture entitlement is kept for mapping guest memory below 4 GB, not for TSO. That is the project's position for
  this work. The below-4 GB mapping itself is NOT verified here.

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
  On a Linux arm64 runner the whole project builds and its decoder, emitter and runtime tests pass (PROVEN, run 44287829572,
  `ours-ctest.log`).
- **x18:** it is free on Linux, which removes the custom-ABI toggle question.
- **TSO:** upstream Linux has no TSO switch, so `rcpc` stays the default. Apple's kernel patch adds `PR_SET_MEM_MODEL` on Apple
  hardware.
- **The semantics oracle** needs an x86 machine or QEMU to produce its results file there. It is built only on macOS today.

## Limits of the current core

- **Not translated yet:**
  - AVX, x87, and SSE3 and later (SSE and SSE2 on XMM registers are translated: [sse.md](sse.md));
  - string instructions and `rep`;
  - `mul`/`div`, `bt*`, rotates, `pushf`/`popf`;
  - 16-bit push and pop;
  - narrow `adc`/`sbb`;
  - narrow shifts by `cl` or beyond their width;
  - `cmovp`.
  
  Each ends the block with an `unsupported` exit that names the instruction's address.
- **Native thunks:** integer arguments only, up to eight. They store XMM0 to XMM15 and MXCSR in CpuState, run the function with the
  host's FPCR and load XMM6 to XMM15 back after it ([sse.md](sse.md)).
- **Threads:** one guest thread. The dispatcher, block maps and invalidation take no locks. Multi-threading needs:
  - a lock around translation;
  - stop-the-world, or epochs, for invalidation and chain patching;
  - a per-thread `CpuState`.
- **Test-only process model:** `TestProcess` (test-only) maps images with `mmap` hints and handles code-write faults in a SIGSEGV/SIGBUS
  handler. Invalidation running inside that handler is safe only because the fault comes from translated code. The final process
  model replaces it.
