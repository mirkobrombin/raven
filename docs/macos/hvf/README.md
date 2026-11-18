# x64 translated at EL0 in a Hypervisor.framework vCPU

The first stream of the Hypervisor.framework process model (`docs/macos/process-model.md`): one VM per Windows process, a minimal
guest kernel at EL1, x64 code translated by `native/translate` and run at EL0 in a vCPU with hardware TSO (ACTLR_EL1.EnTSO). No
Rosetta stage, no `native/nt`, no `native/cpu` changes. One vCPU on the run path; two in the litmus and spin tests only.

Labels: **PROVEN** (command and output below), **INFERRED** (from what, and the missing check), **NOT verified**.

## What was built

| File | What |
|---|---|
| `native/hvf/vm.h`, `vm.cpp` | `Vm`: the process's VM (`hv_vm_create` with a 4 KiB IPA granule), guest RAM at IPA 1 GiB mapped once at stage 2, 4 KiB stage-1 tables for EL0 (TTBR0, 39-bit VA), the kernel in the upper half (TTBR1, all guest RAM in one 1 GiB block), EL0 mappings with host aliases, code maintenance |
| `native/hvf/kernel.S` | EL1 vectors and the EL0 dispatcher |
| `native/hvf/vcpu.h`, `vcpu.cpp` | `Vcpu`: a `translate::CodeRunner` that resumes a vCPU instead of calling the enter trampoline; system registers, mailbox, exits, write faults, `quiesce` |
| `native/hvf/vm_process.h`, `vm_process.cpp` | `VmProcess` (a `GuestMemory`: images at their preferred base in guest memory, stacks and TEBs, code watched at stage 2) and `VmCodeCache` (a `CodeAllocator` in guest memory) |
| `native/hvf/hvf_*_test.cpp` | The exit criteria below, the instruction cache test, the shared cache and code change tests |
| `native/hvf/hvf_bench.cpp`, `hvf_bench_run.py`, `testdata/bench_native.c`, `testdata/bench_loop.c` | The benchmark |
| `native/hvf/testdata/hvf_client.c` | x64 guest code of the litmus, the spin and the code changes (`patch_target`) |
| `native/hvf/CMakeLists.txt`, `hypervisor.entitlements` | Builds on Darwin arm64 only, adds `native/translate` as a subdirectory, signs every binary that creates a VM ad hoc with `com.apple.security.hypervisor` (`RAVEN_HVF_SIGN_IDENTITY` names a keychain identity instead). No provisioning profile: the guest's low addresses come from its own page tables |

## The split

The host keeps `Runtime` and the translator (they use `std::map`, `std::unordered_map`, `std::filesystem`). Guest RAM holds what
translated code touches while it runs:

- the translated code (`VmCodeCache`, EL0 read and execute, never writable from the guest);
- the `CpuState` and the lookup table (`CodeAllocator::allocate_data`, EL0 read and write, execute-never);
- the image, stack and TEB (EL0 read and write, execute-never: x64 code is data to the vCPU);
- per vCPU, a mailbox page and a 64 KiB EL0 stack for the trampolines' frame;
- the dispatcher, one EL0 page at the top of the 39-bit range.

The dispatcher loads the CpuState, the code and the enter trampoline from the mailbox (x19), calls the trampoline, and on its return
(any exit reason: lookup miss, chain request, return, fault) executes `svc #0`. The EL1 vector turns that into `hvc #0`, a host exit.
The host reads the reason from x0, does what `Runtime::run` always did (translate, place the block through its alias, fill the lookup
table, patch a chained branch), writes the next code into the mailbox and resumes the vCPU, which `eret`s to the dispatcher. EL0
cannot `hvc` (it is UNDEFINED there), so svc then hvc is the shortest exit that keeps the dispatcher at EL0; a stage-2 fault on an
unmapped IPA would exit directly but needs the host to decode the faulting access. Any other exception reports itself with `hvc #1`
(from EL0) or `hvc #(0x10 + vector)`; `Vcpu` reads ESR_EL1, FAR_EL1 and ELR_EL1 and throws. Nothing in the kernel or the dispatcher
touches x18.

A vCPU starts in the kernel's return path (`raven_hvf_resume`, ELR_EL1 at the dispatcher, SPSR_EL1 EL0t), so the first entry takes the
same path as every later one. System registers: MAIR_EL1 0xff (normal write-back), TCR_EL1 T0SZ = T1SZ = 25, 4 KiB granules, walks
inner shareable write-back, IPS 36 bits; SCTLR_EL1 MMU, caches, UCT; CPACR_EL1.FPEN = 3 (the trampolines save v8-v15 at EL0);
ACTLR_EL1.EnTSO as asked; vtimer masked, DAIF masked.

### Absolute addresses: the audit

Everything translated code reads as an address must be a guest address in the vCPU. What the audit found and what changed in
`native/translate` (commit `refactor: keep the runtime's state where translated code sees it ...`), all of it through the allocator's two views of the same
memory rather than VM-specific code:

| Place | Before | After |
|---|---|---|
| `Runtime::cpu` (CpuState) and `Runtime::lookup` | members and a `std::vector` in host memory | `CodeAllocator::allocate_data` gives the host pointer and the address translated code uses; `cpu.lookup_table` holds the latter |
| `exit_trampoline`, `return_guest` (`Runtime::place_fixed_code`) | execute addresses from `CodeAllocator::allocate` | unchanged: `VmCodeCache` hands out guest addresses as execute addresses, so these already are guest addresses |
| lookup entries, `Block::host`, chained-branch `incoming` | execute addresses | unchanged, same reason; every write goes through `CodeAllocator::write`, which writes the alias |
| `Runtime::chain` reading the original word | `memcpy` from the execute address (a guest address in the VM) | `CodeAllocator::read` |
| `Runtime::call` pushing the return address | `memcpy` to the guest stack address | `GuestMemory::write` |
| `Runtime::run` entering code | call of the enter trampoline as a host function | `CodeRunner::enter` when a runner is given; the in-process path is unchanged |
| native thunks (`Runtime::bind_native`) | `mov` of a host function address | `bind_native` throws when a runner is given (open item below) |
| the emitter (`translator.cpp`) | | no host address: exits load `exit_trampoline` and `lookup_table` from the CpuState (x23); chain sites come from `adr`; RIP-relative operands are guest addresses. The only absolute host address is in `native_thunk`. INFERRED from reading the emitter (`grep -n "reinterpret_cast\|memcpy" translator.cpp` finds nothing); every test above runs its code in the vCPU, but no test scans emitted words for host addresses |

The workload images import nothing, so no native thunk is needed on this path. PROVEN, in `build-macos-hvf`:

```
$ for e in guest/hvf_*.exe translate/guest/{entry,crc32,fnv,histogram}.exe; do printf "%s " $e; llvm-readobj --coff-imports $e | grep -c "Import {"; done
guest/hvf_crc32.exe 0
guest/hvf_entry.exe 0
guest/hvf_fnv.exe 0
guest/hvf_histogram.exe 0
translate/guest/entry.exe 0
translate/guest/crc32.exe 0
translate/guest/fnv.exe 0
translate/guest/histogram.exe 0
$ llvm-readobj --coff-imports translate/guest/api_loop.exe | grep -E "Import \{|Name:" | head -2    # the command does see imports
Import {
  Name: KERNEL32.dll
```

### Instruction cache

The host writes code through its alias and the vCPU fetches it at a guest address. `hvf_code_write_test` rewrites one 16-word block,
at an address the vCPU has just executed, 20 000 times, each time to return a new value, and enters it after each rewrite. PROVEN
(`ctest --test-dir build-macos-hvf -V -R hvf_code_write`):

```
CTR_EL0 at EL0 in the vCPU: 0x99444c004 (IDC 1, DIC 0, L1Ip 3)
host sys_icache_invalidate on the alias (default): 0 stale of 20000, 0 wrong exits, 0 guest invalidations
host sys_dcache_flush + guest IC IALLUIS: 0 stale of 20000, 0 wrong exits, 20000 guest invalidations
control: host sys_dcache_flush only: 19991 stale of 20000, 0 wrong exits, 0 guest invalidations
control: none: 19988 stale of 20000, 0 wrong exits, 0 guest invalidations
```

- An instruction cache invalidation is necessary (PROVEN: without one, about 99.7% of rewritten blocks run their old code), as
  CTR_EL0.DIC = 0 says. A data cache clean is not (IDC = 1).
- What the libkern calls execute on macOS 27.2.1 (PROVEN, `lldb` `disassemble -n` in a running process): `sys_icache_invalidate` is
  DSB ISH, IC IVAU per 64-byte line, DSB ISH, ISB, with no DC; `sys_dcache_flush` is DSB ISH and `ret`, nothing else. So the
  "sys_dcache_flush" control is a barrier only.
- `sys_icache_invalidate` on the host alias suffices (PROVEN, 20 000 rewrites, one vCPU): the instruction cache is PIPT (L1Ip = 3),
  so invalidating by the host's address reaches what the guest fetched through its own, and IC IVAU is broadcast to the inner
  shareable domain. That is the default (`CodeMaintenance`); the guest-side IC IALLUIS in the kernel's return path also suffices
  (PROVEN) and stays as an option for a CPU whose instruction cache is not PIPT.
- The test fails if a control ever shows no stale run while CTR_EL0.DIC = 0, so it proves its own sensitivity on every run.
- The test writes and enters on the same host thread. That the host's IC IVAU also reaches a vCPU on another core, or one whose
  thread moved between the write and the resume, is INFERRED from IC IVAU being broadcast to the inner shareable domain; no test
  writes from one thread and runs on another.
- A vCPU running other code while the host patches a chained branch it may reach: the patch replaces one `b` with another, which the
  architecture allows to be modified concurrently. INFERRED from the Arm ARM's concurrent modification rules; not exercised, since one
  vCPU runs per Runtime here.
- The kernel vectors are copied once at VM creation into pages no vCPU has run, and invalidated like any other code. That
  invalidation is defensive: no test removes it.

## One translation cache per VM

A Windows process is one VM and one `Runtime`; each vCPU is a `Runtime::Thread` with its own CpuState in guest RAM and its `Vcpu` as
runner. Blocks, the lookup table, chains and the granule index are shared.

- **Who translates.** The host thread of the vCPU that missed, under the runtime's mutex. A vCPU that misses the same block meanwhile
  waits on the mutex and then finds the block, so each block is translated once (hvf_shared_test, below). No translator thread: a miss
  needs its block before its vCPU can go on anyway, and the mutex is held only for host work, never while a vCPU runs.
- **Publishing.** The block's code is written through the alias and `sys_icache_invalidate`d (DSB ISH, IC IVAU, DSB ISH, ISB) first;
  then a DMB ISH and one 16-byte STP write the lookup entry. With FEAT_LSE2 (PROVEN present: `sysctl hw.optional.arm.FEAT_LSE2` gives
  1) a 16-byte aligned STP and the translated code's LDP of the entry are single-copy atomic, so a vCPU reads the whole old or the whole
  new entry, and the code an entry names is written and invalidated before the entry. Entries are cleared the same way. A host without
  FEAT_LSE2 needs another publication scheme (open item).
- **Fetching freshly published code: NOT verified.** The Arm ARM wants a context synchronization event on the PE that executes code
  another PE wrote and invalidated with IC IVAU. A vCPU that reaches new code through an LDP of the lookup table and a BR, or through a
  chain into a block it never ran, takes none. What reduces the gap: new code always goes to addresses never used before in the cache
  (nothing is reused until a reset), so no PE can hold earlier instructions from those addresses unless it fetched them speculatively
  before they were written; and every miss and chain request is an exit, which is one. An ISB on the indirect-branch path would close it
  for indirect branches at a cost on every one; not measured, not added. No test has shown a stale fetch.
- **Chains.** Patching replaces one aligned `b` with another, which the Arm ARM allows while other PEs execute it, followed by the same
  instruction cache invalidation. Two vCPUs taking the same chain exit patch it once: the second finds the branch already there.
- **Guest stores to translated code: stage 2.** A watched 4 KiB page loses write permission with `hv_vm_protect`. The VM asks for a
  4 KiB IPA granule, and protection really works in 4 KiB pages: hvf_granule_test makes one page read-only inside a 16 KiB-aligned IPA
  block, and a store to the next 4 KiB page takes no fault while a store to the page itself takes one (PROVEN, output below; with the
  16 KiB granule, `hv_vm_protect` of a 4 KiB page fails). PE sections are 4 KiB-aligned, so code and data never share a watched page,
  and `watch_granule` is 4 KiB, matching. Chosen over the guest's own page tables because the host changes stage 2 synchronously while vCPUs run, and
  Hypervisor.framework does the TLB maintenance; a stage-1 change would need TLBI from inside the guest on some vCPU before the block
  could be published. The fault also exits straight to the host, without the guest kernel. A protect and unprotect pair costs about
  0.8 us (below).
- **The write path.** The store's stage-2 fault exits on the storing vCPU's thread: ESR class 0x24 with WnR, not from a table walk.
  Hypervisor.framework reports it as a translation fault at level 3 (DFSC 0b000111, exit syndrome 0x93880047; PROVEN by the run that
  checked for permission faults only and failed), so translation and permission faults both count. `VmProcess` passes it to
  `Runtime::code_written` of every runtime attached to it (every runtime that translates its code must be: `watch_code` refuses with
  none attached), which drops every block of that granule: chains into them are restored to their stubs, lookup entries naming
  them are cleared, the pages are unwatched through their counts; then every other vCPU of the runtime is quiesced. The vCPU resumes at
  the store, which is retried. If another vCPU translated the page again meanwhile, the page is watched again and the store faults
  again, dropping that translation too (hvf_code_race_test). The retry is bounded: after `Vcpu::store_retries` (8) faults in a row of
  one execution of a store, the host performs it, writing through `VmProcess::write` (which drops translations again) and stepping
  over it. That needs the syndrome to describe the store (ISV: one register, no writeback), as it does for the translator's `str`
  forms; `stp`, writeback and atomic stores are retried without bound (INFERRED to end, open item). A store whose fault arrives after
  another store's handler unwatched the page is simply retried. A write the runtime makes through the alias (`VmProcess::write`) drops translations the same way.
- **Translation against concurrent stores.** A block is translated from a copy of the guest bytes, then watched, then the bytes are
  compared again: a store that landed between the copy and the watch faulted nowhere, so a difference means translating again.
- **The guarantee.** When `code_written` returns, no vCPU reaches a dropped block through a chain or the lookup table: nothing chains to
  it, the lookup table does not name it, and every other vCPU that was in the guest has taken an exit (a context synchronization event)
  since the chains were undone. `Vcpu::quiesce` does that with `hv_vcpus_exit` (latched for a vCPU about to run, per the header; the
  request is also sent again while waiting, a fallback no test or mutation exercises: NOT verified) and waits for the exit, or returns at once if the vCPU is in the host. A vCPU can still run
  a dropped block once to its end: one that was inside it, or one that got its address from the runtime just before (the address is
  read under the lock, the vCPU entered after). Blocks are straight-line code with no backward branch of their own, so that is bounded.
  It matches what x86 promises another processor that runs code being modified: the Intel SDM's cross-modifying code rules make the
  executing processor see the new code only after it serializes, so it may run the old code meanwhile (INFERRED; no x86 comparison
  was run). Dropped code stays in the cache until a reset.
- **Limits.** A page holding code written often will fault and translate again on every round. `Runtime::Thread`s must be removed
  before their `Vcpu` goes, and runtimes detached (`VmProcess::Attachment` does it) before they go.
- **In this process** the runtime stays single-threaded: `add_thread` refuses a thread without a runner and any thread once native
  functions are bound (runtime_test), since `TestProcess`'s fault state is not synchronized and its SIGSEGV path calls `code_written`.

## Results

MacBook Air M5 (Mac17,3), macOS 27.2.1, Xcode 27.2, llvm-mingw 20261104. The machine was in use (load average 5 to 86 across the runs below, thermal state
1, i.e. not nominal) for all of these.

### 1. The workloads at EL0 with correct results

PROVEN (`ctest --test-dir build-macos-hvf -V -R hvf_workloads`): each workload returns its native build's value for one call and
seven times it for seven calls in the translated loop, and the last exit's SPSR_EL1 is 0x200003c0 (M = EL0t) with ESR_EL1 class SVC:

```
entry: 500500 (native 500500), SPSR_EL1 0x200003c0, 13 runs, 2 lookup misses, 9 chains, 9 blocks
crc32: 822885996 (native 822885996), SPSR_EL1 0x200003c0, 19 runs, 2 lookup misses, 15 chains, 13 blocks
fnv: 12445895775379708965 (native 12445895775379708965), SPSR_EL1 0x200003c0, 40 runs, 2 lookup misses, 36 chains, 27 blocks
histogram: 2945718301815661106 (native 2945718301815661106), SPSR_EL1 0x200003c0, 19 runs, 2 lookup misses, 15 chains, 13 blocks
hvf workloads: PASS
```

Failing first (commit `test: run the integer workloads translated at EL0 ...`, against stub VM classes): `FAIL: the VM is not
implemented`, exit 1.

### Speed against native

`hvf_bench_run.py`, 30 interleaved rounds (each configuration once per round, as its own process), about 4.5 minutes in all. Every
process makes one first call (translation included), then 6 timed runs of CALLS workload calls, each run 50 ms or longer natively; the
first timed run is a warm-up dropped in every configuration alike, and the process's value is the median of the other 5. Native is the
same `bench_loop` and workload built for arm64 (`hvf_native_*`), so the caller loop is native code there and translated code in the
others. Translation is plain mode (`MemoryOrdering::hardware`) everywhere; in this process that is not x86 ordering, in the vCPU EnTSO
makes it so.

- **loop**: one guest call of `bench_loop(CALLS)` per run, so the vCPU is entered once per run.
- **call**: CALLS guest calls of `bench_loop(1)`, one entry into translated code per workload call, which is how `translate_bench
  --workload` measured the in-process numbers in the brief. In the vCPU every call is a host exit and a resume.

Median ns per workload call over the 30 processes and the ratio to native's median of the same session, measured after the shared
cache and stage-2 watching landed (PROVEN: `docs/macos/hvf/data/summary.csv`, every run of every process in `measurements.jsonl`,
machine state in `state.json`: load average 3.78 at the start and 5.00 at the end, thermal state 1 at both; the machine was in use).
The vCPU-loop column of the pass before those changes is in parentheses:

| Workload, CALLS | native ns | in-process loop | in-process call | **vCPU loop** (before) | vCPU call | brief: in-process | brief: Rosetta |
|---|---|---|---|---|---|---|---|
| entry, 200000 | 294.8 | 0.837 | 0.861 | **0.825** (0.832) | 3.641 | 0.829 | 0.830 |
| crc32, 4000 | 12329.8 | 1.305 | 1.328 | **1.312** (1.311) | 1.375 | 1.326 | 1.158 |
| fnv, 1500 | 31041.4 | 1.015 | 1.000 | **0.996** (1.002) | 1.025 | 1.005 | 0.996 |
| histogram, 1000 | 47799.8 | 1.520 | 1.532 | **1.522** (1.510) | 1.543 | 1.526 | 1.478 |

p95 and standard deviation of the vCPU loop configuration, ns: entry 468.7 / 92.0, crc32 24718.6 / 3226.9, fnv 52462.4 / 8144.9,
histogram 117019.6 / 16197.1. Native entry's standard deviation is 61.7 ns on a 294.8 ns median (it was 707 on 323 with one short run
per process, before the warm-up was added).

- The shared cache and the watching cost the workloads nothing measurable: every vCPU-loop ratio is within 1% of the pass before them
  (PROVEN for the two sessions' medians; the differences are inside the spread). Hot loops never take the runtime's lock, and the
  workloads write no code.
- Translated code runs in the vCPU at the speed it runs in this process: every vCPU-loop ratio is within 2% of the in-process loop
  ratio of the same session (PROVEN for this session's medians; no ranking between the two is claimed).
- Translated entry.c stays faster than native (0.83) with equal warm-up and 60 ms windows, so it is not an artifact of native timing a
  cold core. Why is NOT verified.
- The host exit and resume cost about 0.8 us per call: entry's call method is 1073.4 ns per call in the vCPU against 253.7 ns in this
  process. INFERRED: a difference of two medians, which also differ in where the runtime's per-call work runs; no exit was timed alone.
  Code that leaves translated code once per microsecond or so will feel it; hot loops do not.
- The first call (translation included) is 1.4 to 2.5 times slower in the vCPU than in this process (summary.csv, `first_ns`): the
  first vCPU entry and cold guest TLB. INFERRED; not broken down.
- The brief's columns come from another session, so they are context, not a comparison.

### 2. Message passing on two vCPUs

PROVEN (`ctest --test-dir build-macos-hvf -V -R hvf_litmus`). x64 writer (`x = i; y = i`) and reader (`a = y; b = x; count b < a`)
from `hvf_client.c`, translated in plain mode, each in its own vCPU at EL0 with its own Runtime, started together through a `lock xadd`
counter; 30 trials of 2 000 000 writes per setting. A violation is one reader iteration that saw y's new value with x's old one.

```
EnTSO 1 (ACTLR_EL1 read back 0x2, 0x2): 0 violations in 30 trials of 2000000 writes, median 0 per trial, median reader loops 1085689
EnTSO 0 (ACTLR_EL1 read back 0, 0): 11420726 violations in 30 trials of 2000000 writes, median 237565 per trial, median reader loops 2360954
hvf litmus: PASS
```

The negative control (EnTSO clear) shows violations on the same code and machine; the reader's loop count proves it ran while the
writer wrote. An earlier run of the same test gave 0 and 244 330 323. The test fails if EnTSO set shows any violation, if EnTSO clear
shows none, or if the median reader loop count is under 1000.

### 3. EL0 spinning under host load, x18

PROVEN (`ctest --test-dir build-macos-hvf -V -R hvf_spin`, a 3 s target). A child process spins 20 threads (two per core) for the
whole test. Two vCPUs each run a translated 64-bit LCG for seconds with x18 set to a sentinel; a watcher thread forces an exit of each
vCPU every 5 ms (`hv_vcpus_exit`), at which the host checks x18 and records the host CPU it runs on.

```
vCPU x18 0x5eed5eed00000000: 3600000153 steps in 18 batches of 169 ms, 3.05 s, 0 wrong results, 331 forced exits, x18 changed at 0 of them and reads 0x5eed5eed00000000 at the end, 264 host CPU changes between exits over 10 distinct CPUs
vCPU x18 0x5eed5eed00000001: 3600000171 steps in 18 batches of 173 ms, 3.11 s, 0 wrong results, 332 forced exits, x18 changed at 0 of them and reads 0x5eed5eed00000001 at the end, 282 host CPU changes between exits over 10 distinct CPUs
host load: 20 spinning threads; this process: 2337 involuntary and 1 voluntary context switches
hvf spin: PASS
```

That is run 10 of the 10 below, with 20 `yes` processes added to the test's own 20 spinning threads.

- Nothing on the guest side writes x18: not the translated code, the dispatcher, the enter and exit trampolines or the kernel, across
  host exits, preemption and migration (PROVEN for these runs: 0 changes at 663 forced exits and at the end). Hypervisor.framework
  itself keeps guest registers across host scheduling, so the host side could not have changed it; what the test guards is the guest's
  own code, and a kernel that clears x18 makes it fail (Failing runs, below).
- Host migrations: each vCPU thread was seen on all 10 CPUs and on a different CPU at about 82% of consecutive forced exits (PROVEN as
  sampled at the exits).
- Preemptions: 2337 involuntary context switches for the whole process (PROVEN); how many fell on the two vCPU threads is not
  observable per thread here (NOT verified).
- Each vCPU spins in batches of 200 million LCG steps (80 to 260 ms each as measured, depending on load) until the target time has
  passed, so host load changes how many batches run, never whether the spin lasts long enough; every batch's result is checked against
  the LCG computed on the host by squaring. The test fails on a wrong result, a changed x18, fewer than 50 forced exits, no host CPU
  change between exits, or a load process that ended early.
- 10 consecutive runs under extra load passed (PROVEN):

  ```
  $ for i in $(seq 20); do (yes > /dev/null) & done
  $ for r in $(seq 10); do ./build-macos-hvf/hvf_spin_test build-macos-hvf/guest 3 > run$r.txt 2>&1; echo "run $r exit $? ..."; done
  load before: { 11.18 14.01 19.76 }
  run 1 exit 0 0 FAIL lines; exits/changes: 361/306/350/297
  run 2 exit 0 0 FAIL lines; exits/changes: 355/304/376/326
  run 3 exit 0 0 FAIL lines; exits/changes: 313/265/327/285
  run 4 exit 0 0 FAIL lines; exits/changes: 296/250/309/246
  run 5 exit 0 0 FAIL lines; exits/changes: 345/279/340/272
  run 6 exit 0 0 FAIL lines; exits/changes: 349/285/334/259
  run 7 exit 0 0 FAIL lines; exits/changes: 329/254/325/251
  run 8 exit 0 0 FAIL lines; exits/changes: 328/259/326/256
  run 9 exit 0 0 FAIL lines; exits/changes: 361/300/361/308
  run 10 exit 0 0 FAIL lines; exits/changes: 331/264/332/282
  load after: { 31.66 18.69 21.26 }
  ```

  (forced exits / host CPU changes of the first vCPU, then of the second.)

### 4. One translation cache shared by 2 and 4 vCPUs

PROVEN (`ctest --test-dir build-macos-hvf -V -R hvf_shared`). The crc32 image runs `bench_loop(20)` on N vCPUs started together through
a barrier, all from one runtime, 20 trials per N; every vCPU must return 20 times native's value, and the runtime must have translated
what one vCPU translates alone. Then N separate runtimes, one per vCPU:

```
one vCPU: 10 blocks
2 vCPUs: shared runtime, most blocks in 20 trials 10; 2 separate runtimes 20 blocks
4 vCPUs: shared runtime, most blocks in 20 trials 10; 4 separate runtimes 40 blocks
hvf shared: PASS
```

Translation count: N separate runtimes translate N times what one does; a shared one translates it once, however many vCPUs miss a block
together. The test fails on any duplicate translation; how often vCPUs actually missed the same block at the same moment is not counted
(NOT verified), only that the runs overlapped in time by construction.

### 5. Guest stores to translated code, on one vCPU and across vCPUs

PROVEN (`ctest --test-dir build-macos-hvf -V -R hvf_code_change`). `patch_target` (`mov eax, imm32; ret`, alone on its 4 KiB page):

- One vCPU calls it, stores a new immediate into its code, calls it again: the second call must return the new value, 10 times with 10
  values.
- vCPU A calls it in a loop the runtime has chained (the call site branches straight into the translation) until it returns something
  new, at most 300 million times; vCPU B waits for A to run, spins, and stores a new immediate. A must stop, and within 100 ms of B's
  call returning. 10 trials.
- `quiesce` of a vCPU spinning in translated code no write touches: 100 calls must force exits and return.

```
two vCPUs: A saw B's store within 18.2 us of B's call returning, worst of 10 trials; B took 10 write faults, A was kicked 0 times
quiesce of a spinning vCPU: 100 calls, 101 forced exits, slowest 34.9 us
runtime: 48 blocks translated, 26 invalidated
hvf code change: PASS
```

- Every store to translated code reached the runtime: one stage-2 write fault per store (PROVEN, B's count).
- A's loop left the old code within 18.2 us of B's call returning, worst of 10 (PROVEN). A was never kicked: undoing the chain sends A
  to a stub and out of the guest before `quiesce` looks, so A is already in the host. The kick is what the guarantee rests on when a
  vCPU does not leave by itself; that it is needed on this CPU is NOT PROVEN (with `quiesce` disabled the two-vCPU trials still pass),
  and that it works is PROVEN by the spinning-vCPU check, which fails when `quiesce` does nothing (Failing runs).
- Undoing chains is necessary (PROVEN: without it A never sees the store and one vCPU gets the old value 9 times of 10; Failing runs).

### 6. Races around a store to translated code

PROVEN (`ctest --test-dir build-macos-hvf -V -R hvf_code_race`), each part forced deterministically:

```
kick: 10 quiesce calls on the other guest thread for 10 write faults
hvf code race: PASS
```

- Translation again in the window: `VmProcess::after_code_written` (a test seam) translates patch_target again from its old bytes after
  the store's fault dropped it and before the store is retried. Five rewrites must each take effect; each costs two write faults, one
  for the store and one for the translation made in the window.
- A second store to a page another store's fault already unwatched: the handler must retry it; a fault outside guest memory stays
  unhandled.
- The kick: a guest thread whose runner only counts `quiesce` calls stands for a vCPU elsewhere; it must be quiesced at least once per
  write fault.
- A host write through `VmProcess::write` to translated code: the next call must run the new code.

### 7. A store into the next instruction of the same block: known gap

`hvf_same_block_test` runs `patch_next`, `mov dword [rip + x], 2; mov eax, 1; ret` where the store hits the second instruction's
immediate, in one translated block. x86 returns 2. Here the storing vCPU resumes inside the translation its store dropped and runs the
old `mov eax, 1` (PROVEN: `known gap: patch_next stored 2 into its own next instruction and ran the old one, returning 1`, exit 3).
ctest passes it only when that line appears (PASS_REGULAR_EXPRESSION): any error fails it (a missing guest directory gives `Required
regular expression not found`), and so does the gap closing, which should then turn it into an ordinary test. Open item.

### 8. Stage-2 granule, and several runtimes on one process

PROVEN (`ctest --test-dir build-macos-hvf -V -R "hvf_granule|hvf_two_runtimes"`):

```
first page IPA 0x40014000 read-only: store to the next 4 KiB page took 0 faults, store to the page itself 1
hvf granule: PASS
store from the first runtime: it returned 55, the second runtime then 55, 1 write faults
hvf two runtimes: PASS
```

Two runtimes on one VmProcess both translate patch_target; a store from one must end (a 10 s watchdog) and both must then run the new
code; with no runtime attached, translating is refused. hvf_code_race_test's bound part translates the page again after every fault:

```
translated again after every fault: 8 faults, 1 stores emulated, the call returned 30
```

### Cost of a guest store to translated code

`hvf_bench write` in the same pass (PROVEN: `docs/macos/hvf/data/write_fault.txt`, 10 runs of 20 000 iterations): one call of
`patch_target` plus one store to its code per iteration, so each iteration takes the write fault, the invalidation, a lookup miss and a
translation of the block again:

| Per iteration | median ns | min | max | sd |
|---|---|---|---|---|
| call, store, fault, invalidate, translate again | 11793.5 | 10065.0 | 12738.6 | 680.1 |
| the same call without the store | 2.3 | 1.9 | 2.4 | 0.2 |
| `hv_vm_protect` read-only and back, one page (alone) | 1136.6 | 1111.7 | 1158.1 | 13.5 |

200 000 write faults for 200 000 stores (PROVEN). An earlier interactive run gave 7600 ns per iteration and 800 ns per protect pair,
so the absolute figures move with the machine's state; about 12 us per store-and-retranslate round is the order to plan with. How the
12 us divide between the fault exit, `code_written`, two `hv_vm_protect` calls, the miss exit and translation is NOT verified. Code
that writes its own code page often (or data that shares a page with code) pays this on every write after a translation.

### Failing runs

Exit criterion 1 and the two runtime data checks have failing-first commits: before the VM, `FAIL: the VM is not implemented`; before
`CodeAllocator::free_data`, and before the runtime owned its data across a throwing constructor, `FAIL: the VM's runtime data is
full`; each exit 1. The VM commit that made criterion 1 pass already set EnTSO, handled forced exits and did instruction cache
maintenance, so the litmus, spin and code write tests passed when they were written. Their failing runs are against the current code
with the one-line mutation below, then restored; the restored build passes (PROVEN, outputs as printed, exit 1 each).

The shared cache and code change tests have failing-first commits (exit 1 each, PROVEN):

- `hvf_shared_test`, before guest threads could share a runtime: `FAIL: guest threads cannot share a runtime yet` (120 times) and
  `FAIL: 2 vCPUs sharing a runtime translated 0 blocks, one translates 10` (20 times, and the same for 4).
- `hvf_code_change_test`, before stage-2 watching: `FAIL: one vCPU: rewrote patch_target to return 10, it returned 1` (10 times, one
  per value) and `FAIL: two vCPUs, trial 0: A ran the old patch_target 300000000 times and never saw B's store` (10 trials).

`hvf_code_race_test` and runtime_test's single-thread check have failing-first commits too (exit 1 each, PROVEN):

- Before the fix of the window: `FAIL: retranslated in the window: rewrote patch_target to return 20, it returned 1`, and the same
  for 21 to 24 (the stale translation outlived every later store).
- Before the retry of a second store: `FAIL: a write fault on a guest page that is no longer watched is not retried`.
- Before `add_thread` checked: `FAIL: a second guest thread without a runner was accepted` and `FAIL: a guest thread was added to a
  runtime with native functions bound`.

The second review's tests have failing-first commits too (exit 1 each, PROVEN):

- `hvf_two_runtimes_test`, while only one runtime could be attached: `FAIL: the store to a page two runtimes watch did not end within
  10 s` (the store faulted forever on the other runtime's watch).
- `hvf_code_race_test`'s bound part, before the bound: `FAIL: the code cache is full` (every retry translated the page again until
  the 16 MiB cache ran out).

The parts that landed with their implementation fail under these mutations:

| Test | Mutation | Command | Output |
|---|---|---|---|
| litmus | `vcpu.cpp`: `-{HV_SYS_REG_ACTLR_EL1, tso ? actlr_tso : 0},` `+{HV_SYS_REG_ACTLR_EL1, 0},` | `hvf_litmus_test build-macos-hvf/guest 30 2000000` | `FAIL: ACTLR_EL1.EnTSO does not read back as set`, `FAIL: x86 ordering broken with EnTSO set`; `EnTSO 1 (ACTLR_EL1 read back 0, 0): 229720312 violations in 30 trials` |
| spin | `kernel.S`: `+mov x18, xzr` before the lower-EL handler's `hvc #0` | `hvf_spin_test build-macos-hvf/guest 3` | `FAIL: x18 changed` (twice); `x18 changed at 436 of them and reads 0 at the end` for each vCPU |
| code write | `vm.cpp` `Vm::code_written`: `-sys_icache_invalidate(bytes, size);` `+;` | `hvf_code_write_test 20000` | `FAIL: host sys_icache_invalidate on the alias (default) ran stale code`; `19976 stale of 20000` |
| code change, unlink | `runtime.cpp` `Runtime::invalidate`: `-cache.write(site, &original, 4);` `+(void)site, (void)original;` | `hvf_code_change_test build-macos-hvf/guest 10` | `FAIL: one vCPU: rewrote patch_target to return 11, it returned 10` (9 of 10); `FAIL: two vCPUs, trial 0: A ran the old patch_target 300000000 times and never saw B's store` (10 of 10) |
| code race, kick | `runtime.cpp` `Runtime::code_written`: `-thread->runner->quiesce();` `+(void)thread;` | `hvf_code_race_test build-macos-hvf/guest` | `FAIL: code_written quiesced the other guest thread 0 times for 10 write faults` |
| code race, host write | `vm_process.cpp` `VmProcess::write`: `-if (code && attached) attached->code_written(page);` `+(void)code;` | `hvf_code_race_test build-macos-hvf/guest` | `FAIL: after the host wrote patch_target to return 77, it returned 24` |
| granule | `vm.cpp`: `-hv_vm_config_set_ipa_granule(config, HV_IPA_GRANULE_4KB)` `+...HV_IPA_GRANULE_16KB)` | `hvf_granule_test build-macos-hvf/guest` | `FAIL: hv_vm_protect failed: 4209590275` |
| same block, error | `CMakeLists.txt`: the test's guest directory `/nonexistent` | `ctest -R hvf_same_block` | `***Failed  Required regular expression not found` |
| code change, quiesce | `vcpu.cpp` `Vcpu::quiesce`: `+return;` as its first line | `hvf_code_change_test build-macos-hvf/guest 10` | `FAIL: quiesce rarely made the spinning vCPU exit, or took 100 ms`; `100 calls, 1 forced exits`. The two-vCPU trials pass under this mutation (above) |

## Reproduction

```sh
export PATH=/path/to/llvm-mingw/bin:$PATH        # x86_64-w64-mingw32-clang and llvm-objdump
cmake -S native/hvf -B build-macos-hvf -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-macos-hvf
ctest --test-dir build-macos-hvf -V               # hvf_* and native/translate's own tests
python3 -I native/hvf/hvf_bench_run.py build-macos-hvf docs/macos/hvf/data 30 /path/to/thermal
```

The last argument is optional: a program that prints the thermal state (`NSProcessInfo.thermalState`, 0 nominal to 3 critical).
`hvf_spin` loads every core for about 3.5 s.

## Open items

- **Native thunks.** `bind_native` refuses to work with a runner. In the VM a call to native code has to become an exit (an `svc`
  from a thunk in guest code, the host calling the function, the result written to the CpuState), which is the design of section 4's
  `GuestStop::api`. Not needed by these workloads.
- **Code writes, further.** A store the syndrome does not describe (`stp`, writeback, LSE atomics on code) is retried without bound
  if other vCPUs keep translating its page again; emulating those needs decoding the instruction. Code written often pays about 12 us
  per write after each translation; a per-page policy (stop translating a page that keeps being written, or emulate its stores
  always) is open.
- **A store into its own block.** The storing vCPU runs the rest of the dropped block (hvf_same_block_test, which passes only on the gap's line). Closing it
  means ending the block at such a store: emulate the store in the host and resume through the dispatcher at the next x64
  instruction, which needs a map from host PCs back to guest addresses the translator does not keep yet.
- **Smaller gaps in the write path.** Faults are keyed on the guest virtual address, so a page mapped at two addresses would be watched
  under one only (no aliases exist here). A translation retried after a concurrent store leaves its first copy in the cache until a
  reset. `VmProcess` holds plain pointers to attached runtimes: detach before a runtime goes. Kicks from `quiesce` call
  the kicked vCPU's `on_cancel`, which is meant for forced exits of any origin.
- **Lookup publication without FEAT_LSE2.** The 16-byte STP and LDP are single-copy atomic only with FEAT_LSE2 (present on the M5); a
  Linux arm64 host without it needs a sequence number or acquire loads in the lookup path.
- **Page faults from translated code** in general: any fault at EL0 ends the run with an exception today. They should become
  `GuestStop::fault` with FaultKind and access, from ESR_EL1 and FAR_EL1.
- **Multithreading, further.** One mutex serializes all host-side runtime work: misses on different blocks wait for each other. Fine at
  these miss rates; a startup with many threads translating at once would want finer locking. Threads are added and removed by hand
  (`add_thread`, `remove_thread` before the `Vcpu` goes); the engine's `new_thread` (section 4) will own that.
- **Scheduler and waits.** One host thread per vCPU, always running. No M:N scheduling, no blocking waits (a wait would leave the vCPU
  exited and the host thread asleep).
- **Mapping while vCPUs run.** `Vm::map` only adds entries, which needs no TLB invalidation; unmapping or changing a mapping will need
  TLBI from the guest kernel or a pause of every vCPU.
- **Address space.** 39-bit EL0 addresses are enough for these images at 0x140000000-0x180000000; Windows' 47-bit user space needs
  T0SZ = 16 and a fourth table level.
- **Guest RAM** is one bump allocation (up to 1 GiB) that is never freed.
- **Concurrent chain patching** runs in hvf_shared_test (vCPUs chain the same sites at once) and passes; that it is safe in general
  rests on the Arm ARM's concurrent modification rules (INFERRED).
- **Instructions that trap at EL0.** SCTLR_EL1 leaves nTWE, nTWI, DZE and UCI clear and CNTKCTL_EL1 is not set, so WFE, WFI, DC ZVA,
  EL0 cache maintenance and CNTVCT_EL0 reads trap to the kernel, which reports them as a fatal guest exception. The translator emits
  none of them today; an `rdtsc` translated to a counter read will need CNTKCTL_EL1.EL0VCTEN.
- **Kernel.** One kernel stack for every vCPU (the kernel never uses sp); any `svc` from EL0 counts as the dispatcher's exit, since
  translated x64 cannot emit one; the guest-side invalidation mode only reaches the vCPU that next enters.

## What shared code must settle before merge

- `native/translate` gained `CodeRunner`, `CodeAllocator::read`, `allocate_data` and `free_data`, `GuestMemory::write` and
  `lay_out_image`; the `Runtime` constructor takes an optional runner, and its CpuState and lookup table now come from the allocator
  (heap blocks in `JitCodeCache`, given back when the runtime goes). The emitted code is unchanged. PROVEN on macOS that translate's
  own tests pass before the change and after every commit (`ctest --test-dir build-macos-translate` before the change and
  `ctest --test-dir build-macos-hvf` after: `100% tests passed out of 4` and `out of 8`, the four `translate_*` among them); NOT
  verified on Linux, where the standalone translate build was not run.
- `Runtime` is now shared by guest threads: `Runtime::Thread` holds a thread's CpuState and runner, `add_thread` and `remove_thread`
  manage them, a mutex covers translation, chaining and invalidation, lookup entries are written with one 16-byte STP, and translation
  rechecks the guest bytes after watching them. `CodeRunner` gained `quiesce`, which `code_written` calls on every other thread's
  runner. Single-thread callers keep the old API (`Runtime::call`, `state`), which goes to the first thread; translate's own tests pass
  unchanged (PROVEN, `ctest --test-dir build-macos-hvf`, 10 of 10).
- `CpuState::lookup_table` stays a `LookupEntry *` but holds the address translated code uses, which is not a host pointer in a VM: it
  could become a `uint64_t`.
- `native/hvf` reuses `test_process.h`'s `lay_out_image`, which lives in a test-only library; the real loader should provide it.
- Neither `native/translate` nor `native/hvf` is part of the top-level build (PROVEN: `grep -n "translate\|hvf" native/CMakeLists.txt
  CMakeLists.txt` prints nothing). Configured as a cross build for Linux, `native/hvf` generates nothing (PROVEN):

  ```
  $ cmake -S native/hvf -B linuxcfg -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
  -- native/hvf needs Darwin on arm64; nothing to build here
  -- Configuring done (4.3s)
  ```

  A real Linux configure was not run (NOT verified).
- No change to `native/nt` or `native/cpu`. The engine interface of section 4 fits this engine as it is: `resume` and `call` map to
  `Runtime::call` and `CodeRunner::enter`, and `Thread` to a `Vcpu` with its host thread. No interface change is proposed.
