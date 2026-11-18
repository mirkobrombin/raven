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
| `native/hvf/vcpu.h`, `vcpu.cpp` | `Vcpu`: a `translate::CodeRunner` that resumes a vCPU instead of calling the enter trampoline; system registers, mailbox, exits |
| `native/hvf/vm_process.h`, `vm_process.cpp` | `VmProcess` (a `GuestMemory`: images at their preferred base in guest memory, stacks and TEBs) and `VmCodeCache` (a `CodeAllocator` in guest memory) |
| `native/hvf/hvf_*_test.cpp` | The exit criteria below, plus the instruction cache test |
| `native/hvf/hvf_bench.cpp`, `hvf_bench_run.py`, `testdata/bench_native.c`, `testdata/bench_loop.c` | The benchmark |
| `native/hvf/testdata/hvf_client.c` | x64 guest code of the litmus and the spin |
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
| `exit_trampoline`, `return_guest` (runtime.cpp:72-73) | execute addresses from `CodeAllocator::allocate` | unchanged: `VmCodeCache` hands out guest addresses as execute addresses, so these already are guest addresses |
| lookup entries, `Block::host`, chained-branch `incoming` | execute addresses | unchanged, same reason; every write goes through `CodeAllocator::write`, which writes the alias |
| `Runtime::chain` reading the original word | `memcpy` from the execute address (a guest address in the VM) | `CodeAllocator::read` |
| `Runtime::call` pushing the return address | `memcpy` to the guest stack address | `GuestMemory::write` |
| `Runtime::run` entering code | call of the enter trampoline as a host function | `CodeRunner::enter` when a runner is given; the in-process path is unchanged |
| native thunks (runtime.cpp:80) | `mov` of a host function address | `bind_native` throws when a runner is given (open item below) |
| the emitter (`translator.cpp`) | | no host address: exits load `exit_trampoline` and `lookup_table` from the CpuState (x23); chain sites come from `adr`; RIP-relative operands are guest addresses. The only absolute host address is in `native_thunk` |

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
host DC CVAU + IC IVAU on the alias (default): 0 stale of 20000, 0 wrong exits, 0 guest invalidations
host DC CIVAC + guest IC IALLUIS: 0 stale of 20000, 0 wrong exits, 20000 guest invalidations
control: host DC CIVAC only: 19948 stale of 20000, 0 wrong exits, 0 guest invalidations
control: none: 19939 stale of 20000, 0 wrong exits, 0 guest invalidations
```

- An instruction cache invalidation is necessary (PROVEN: without one, about 99.7% of rewritten blocks run their old code), as
  CTR_EL0.DIC = 0 says. A data cache clean is not (IDC = 1).
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

## Results

MacBook Air M5 (Mac17,3), macOS 27.2.1, Xcode 27.2, llvm-mingw 20261104. The machine was in use (load average 5 to 10, thermal state
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

`hvf_bench_run.py`, 30 interleaved rounds (each configuration once per round, its own process, one first call, then one timed run),
about 5 s in all. Native is the same `bench_loop` and workload built for arm64 (`hvf_native_*`), so the caller loop is native code
there and translated code in the others. Translation is plain mode (`MemoryOrdering::hardware`) everywhere; in this process that is
not x86 ordering, in the vCPU EnTSO makes it so.

- **loop**: one guest call of `bench_loop(CALLS)` per run, so the vCPU is entered once per run (plus up to 4 chain exits the first
  timed run still takes).
- **call**: CALLS guest calls of `bench_loop(1)`, one entry into translated code per workload call, which is how `translate_bench
  --workload` measured the in-process numbers in the brief. In the vCPU every call is a host exit and a resume.

Median ns per workload call and the ratio to native's median of the same session (PROVEN: `docs/macos/hvf/data/summary.csv`, raw
per-process values in `measurements.jsonl`, machine state in `state.json`: load average 5.35 at the start and 5.24 at the end, thermal
state 1 at both):

| Workload, CALLS | native ns | in-process loop | in-process call | **vCPU loop** | vCPU call | brief: in-process | brief: Rosetta |
|---|---|---|---|---|---|---|---|
| entry, 2000 | 322.8 | 0.823 | 0.860 | **0.832** | 3.618 | 0.829 | 0.830 |
| crc32, 20 | 13425.0 | 1.322 | 1.318 | **1.347** | 1.409 | 1.326 | 1.158 |
| fnv, 50 | 33777.1 | 1.008 | 1.002 | **1.006** | 1.015 | 1.005 | 0.996 |
| histogram, 50 | 52142.9 | 1.528 | 1.527 | **1.524** | 1.545 | 1.526 | 1.478 |

p95 and standard deviation of the vCPU loop configuration, ns: entry 537.5 / 132.3, crc32 32750.0 / 6082.6, fnv 69062.5 / 23534.4,
histogram 215669.2 / 41167.1. The spread is wide in every configuration, native included (summary.csv); medians only are compared.

- Translated code runs in the vCPU at the speed it runs in this process: every vCPU-loop ratio is within 2% of the in-process loop
  ratio of the same session (PROVEN for this session's medians; the differences are inside the spread, so no ranking between the two is
  claimed).
- One host exit and resume costs about 0.9 us: entry's call method is 1168 ns per call in the vCPU against 277 ns in this process
  (PROVEN, same table). Code that leaves translated code once per microsecond or so will feel it; hot loops do not.
- The first call (translation included) is 1.4 to 2.5 times slower in the vCPU than in this process (summary.csv, `first_ns`): the
  first vCPU entry and cold guest TLB. INFERRED; not broken down.
- The brief's columns come from another session and the differences are within its noise, so they are context, not a comparison.

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
vCPU x18 0x5eed5eed00000000: 4868924495 steps in 3.53 s, result right, 402 forced exits, x18 changed at 0 of them and reads 0x5eed5eed00000000 at the end, 321 host CPU changes between exits over 10 distinct CPUs
vCPU x18 0x5eed5eed00000001: 4868924496 steps in 3.50 s, result right, 381 forced exits, x18 changed at 0 of them and reads 0x5eed5eed00000001 at the end, 294 host CPU changes between exits over 10 distinct CPUs
host load: 20 spinning threads; this process: 3337 involuntary and 2 voluntary context switches
hvf spin: PASS
```

- Nothing on the guest side writes x18: not the translated code, the dispatcher, the enter and exit trampolines or the kernel, across
  host exits, preemption and migration (PROVEN for these runs: 0 changes over 783 checks; an earlier run gave 0 over about 1470).
  Hypervisor.framework itself keeps guest registers across host scheduling, so the host side could not have changed it; what the test
  guards is the guest's own code, and a kernel that clears x18 makes it fail (Failing first, below).
- Host migrations: each vCPU thread was seen on all 10 CPUs and on a different CPU at about 78% of consecutive forced exits (PROVEN as
  sampled at the exits).
- Preemptions: 3337 involuntary context switches for the whole process (PROVEN); how many fell on the two vCPU threads is not
  observable per thread here (NOT verified).
- The spin length is calibrated from a short run under the same load, and the test fails under three quarters of the target; the real
  length varies with contention (3.5 s here, 5.8 s in an earlier run with a 2 s target).
- The result of each spin matches the LCG computed on the host by squaring (PROVEN).

### Failing first

Exit criterion 1 and the runtime data reuse check have failing-first commits with failing runs. The VM commit that made criterion 1
pass already set EnTSO, handled forced exits and did instruction cache maintenance, so the litmus, spin and code write tests passed
when they were written. For those, the failing runs below are against a build mutated on purpose and then restored (PROVEN, outputs as
printed; the restored build passes):

- Litmus, with `Vcpu` writing ACTLR_EL1 = 0 whatever it is asked:

  ```
  FAIL: ACTLR_EL1.EnTSO does not read back as set
  FAIL: x86 ordering broken with EnTSO set
  EnTSO 1 (ACTLR_EL1 read back 0, 0): 11587135 violations in 30 trials of 2000000 writes, median 358301 per trial, median reader loops 2252639
  EnTSO 0 (ACTLR_EL1 read back 0, 0): 11442044 violations in 30 trials of 2000000 writes, median 312552 per trial, median reader loops 2690053
  ```
- Spin, with `mov x18, xzr` before the kernel's `hvc #0`:

  ```
  FAIL: x18 changed
  FAIL: x18 changed
  vCPU x18 0x5eed5eed00000000: 2194494347 steps in 2.82 s, result right, 293 forced exits, x18 changed at 293 of them and reads 0 at the end, ...
  ```
- Code write: its controls fail the test whenever they do not run stale code, on every run (Instruction cache, above).
- Runtime data reuse (`hvf_workloads_test` builds 64 runtimes on one code cache with 1 MiB of runtime data): before
  `CodeAllocator::free_data`, `FAIL: the VM's runtime data is full`, exit 1.

## Reproduction

```sh
export PATH=/path/to/llvm-mingw/bin:$PATH        # x86_64-w64-mingw32-clang and llvm-objdump
cmake -S native/hvf -B build-macos-hvf -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-macos-hvf
ctest --test-dir build-macos-hvf -V               # hvf_* and native/translate's own tests
python3 -I native/hvf/hvf_bench_run.py build-macos-hvf docs/macos/hvf/data 30 /path/to/thermal
```

The last argument is optional: a program that prints the thermal state (`NSProcessInfo.thermalState`, 0 nominal to 3 critical).
`hvf_spin` loads every core for 3 to 6 s.

## Open items

- **Native thunks.** `bind_native` refuses to work with a runner. In the VM a call to native code has to become an exit (an `svc`
  from a thunk in guest code, the host calling the function, the result written to the CpuState), which is the design of section 4's
  `GuestStop::api`. Not needed by these workloads.
- **Code writes from the guest.** `VmProcess::watch_code` does nothing: images are EL0-writable. The plan: map watched pages read-only
  to EL0, let the kernel's vector pass the permission fault to the host (`hvc` with FAR), call `Runtime::code_written`, make the page
  writable, and return to the faulting store. Or stage-2 write protection, which the 4 KiB IPA granule now allows.
- **Page faults from translated code** in general: any fault at EL0 ends the run with an exception today. They should become
  `GuestStop::fault` with FaultKind and access, from ESR_EL1 and FAR_EL1.
- **Multithreading.** One Runtime per vCPU and thread here; a Windows process needs one translation cache and lookup table shared by
  its threads (locks on the host side, or a per-thread table), and invalidation that reaches the other vCPUs.
- **Scheduler and waits.** One host thread per vCPU, always running. No M:N scheduling, no blocking waits (a wait would leave the vCPU
  exited and the host thread asleep).
- **Mapping while vCPUs run.** `Vm::map` only adds entries, which needs no TLB invalidation; unmapping or changing a mapping will need
  TLBI from the guest kernel or a pause of every vCPU.
- **Address space.** 39-bit EL0 addresses are enough for these images at 0x140000000-0x180000000; Windows' 47-bit user space needs
  T0SZ = 16 and a fourth table level.
- **Guest RAM** is one bump allocation (up to 1 GiB) that is never freed.
- **Concurrent chain patching** is INFERRED safe (instruction cache section), not exercised.
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
