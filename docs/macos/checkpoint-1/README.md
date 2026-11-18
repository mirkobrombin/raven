# macOS checkpoint 1: the guest/host boundary and the process model

This report compares the ways Raven can run and serve a Windows guest on Apple Silicon, with numbers measured on one machine, and recommends a process model. Every figure carries a label:

- **PROVEN**: measured or observed here; the data file is named.
- **INFERRED**: derived from other results; the missing check is named.
- **NOT verified**: no evidence yet.

## Machine and method

- MacBook Air, Apple M5 (Mac17,3), 10 cores, 24 GB, macOS 27.2.1, Xcode 27.2. Fanless.
- Every timed block ran on AC power at QoS `USER_INTERACTIVE`, one benchmark at a time, after a 60 s pause. The thermal state was nominal (0) at the start and end of every block; load, power and battery per block are in `data/state.csv`.
- Load gate: the first five blocks (gate, signals, Mach exceptions, HVF exit, HVF spin) started only below a 1-minute load of 1.5 (first run of the driver). The remaining blocks ran in a second run of the driver, gated below 3.0, because the application hosting the measuring session keeps about one core busy and the load did not fall below 2 otherwise. The M5 has 4 performance and 6 efficiency cores; at load 2 to 3 most of them are idle, but which cores the measured threads got is not controlled.
- 30 runs per measure unless stated. Medians, p95 and standard deviation come from the raw files in `data/`. Earlier numbers taken while other work loaded the machine are discarded; none appear here.
- The Wine runs are the exception: 3 runs each plus 1 warm-up, run outside the driver with no load gate, at load 2.9 to 3.6, and labelled as such. The exact commands are in `data/wine-msync/summary.json` under `commands`.
- Sources of every probe are in `spikes/`. `spikes/quiet_pass.sh` is the driver as used for the second run: the first five blocks are commented out because they ran in the first run, and its Wine block (30 repeats) was stopped before it produced data.
- Branches named below are local drafts, none pushed: `macos-translate-draft` (translator and `docs/macos/translation.md`), `e3-stage-draft` (Rosetta stage and `docs/macos/process-model.md`), `macos-metal-draft`, `macos-desktop-draft`, `macos-channel-draft`, `macos-bench-draft`.

## Correction to an earlier conclusion

The earlier conclusion "LDAPR/STLR cost at most 2%, so the hardware TSO bit is not needed" came from a Neoverse N2. On the M5 it does not hold (PROVEN, `data/translator/linux_summary.csv`, `data/spikes/barriers_*.jsonl`):

| Measure (M5) | Plain loads/stores | LDAPR/STLR | Hardware TSO |
|---|---|---|---|
| entry.c loop, translated, ratio to native | 0.829 (not x86-correct) | **1.150** | Rosetta 0.830 |
| one load, 1 thread, ns | ldr 0.075 | ldapr 0.255 | n/a |
| contended load/store mix, 2 threads, ns | 0.299 | 1.962 | n/a |

On the entry.c loop, x86-correct ordering without hardware TSO costs about 39% on the M5. On the compute kernels (CRC-32, FNV-1a, histogram) the difference stays within 1%. entry.c is one memory-dense loop; how much of real programs behaves like it is NOT measured. The line in `docs/macos/translation.md` (branch `macos-translate-draft`) that drops the hardware TSO bit is superseded by this table.

## Findings that decide the model

1. **Hardware TSO is reachable only inside a Hypervisor.framework guest.** A message-passing litmus test (`spikes/litmus.c`, `spikes/hvf.c`, `data/spikes/litmus_summary.csv`) gives 0 violations with ACTLR_EL1.TSO set in the vCPU, and millions with it clear, against 8 to 11 million natively and 0 under Rosetta (PROVEN). The cross-architecture entitlement does not enable TSO for a process (PROVEN, `data/entitlement/RESULTS.md`).
2. **x18 cannot be relied on in a macOS process.** `os_set_custom_x18_abi_enabled` (macOS 26.4+) toggles in user space for 3.35 ns per pair (p95 3.50, sd 0.08, PROVEN, `data/spikes/x18c.jsonl`; the 2.93 ns in `x18_custom_abi.md` is an earlier unquiet run). The mode lives in bit 48 of TPIDR_EL0 next to the CPU number. When the kernel moves the thread to another CPU the mode is dropped and x18 is zeroed: 3 of 3 pure-preemption runs, 13 of 16 trap-loop runs, with or without the entitlement (PROVEN, `data/spikes/x18_custom_abi.md`, `data/spikes/x18_custom_cases.txt`). A brk handled by a POSIX signal keeps the mode (3 of 3). Calls into macOS code while the mode is on (malloc, printf, getpid) return normally with no error (3 of 3 each), so a mistake is silent, not loud. Enabling twice aborts with a brk. The Mach exception case crashed the test child with SIGBUS in all three runs; whether that is the probe or the mode is NOT verified. Without the API, x18 is zeroed by every kernel entry (`data/spikes/x18.txt`). Consequence: native Windows ARM64 and ARM64EC code, which keeps the TEB in x18, does not run reliably in a macOS process on this build. Raven's own ARM64 DLLs can avoid x18 because we compile them; third-party ARM64 programs such as Notepad++ ARM64 cannot.
3. **4 KiB pages and low addresses.** A Hypervisor.framework guest maps 4 KiB stage-1 pages from one 16 KiB host page and runs code from 4 KiB L3 pages; a 4 KiB IPA granule maps a single 4 KiB page and faults on its neighbour (PROVEN, `data/spikes/hvf_functional.txt`). A macOS process cannot do either. Below 4 GB, an arm64 process maps 0x10000, 0x400000 and 0x7ffe0000 only with the cross-architecture entitlement; the exact test and its negative controls follow.
4. **Hypercalls from a VM are cheap; Mach exceptions are not.** A guest HVC to the host and back takes 672 ns, a Mach exception in the same process 8.2 µs, and a breakpoint in a Rosetta guest caught by the host 25.8 µs (PROVEN, table below). A data-abort exit costs 755 ns and a fault resolved inside the guest 68 ns (PROVEN, not quiet, next section).
5. **A VM has 64 vCPUs.** `hv_vm_get_max_vcpu_count` returns 64 and the 65th creation fails with 0xfae94005; each vCPU costs about 91 KiB including its 64 KiB thread stack (PROVEN, `data/spikes/hvf_functional.txt`). Creating a vCPU and setting its registers takes 22.6 µs median, p95 36.5 µs, with outliers up to about 1.7 ms (PROVEN, `hvf_roundtrip.jsonl`). Whether one host process can hold more than one VM is NOT verified. Windows programs routinely run more threads, so a VM model needs an M:N scheduler in the guest.

### The cross-architecture entitlement, exact test

`com.apple.developer.cross-architecture-support` appears in Xcode 27's capability catalog as "Cross-architecture Compatibility Framework", macOS only, Apple Developer Program, no distribution approval (PROVEN, excerpt of Xcode's capability catalog in `data/entitlement/xcode-capability.json`). No page on developer.apple.com documents it (NOT verified as official documentation). The team obtained development profiles carrying it on the same day it enabled the capability; whether Apple reviewed anything is NOT verified.

Probe: `data/entitlement/probe.c`, built for arm64 and signed either ad hoc or with the team's Apple Development certificate inside an `.app` with `embedded.provisionprofile` and the entitlement (`data/entitlement/ent.plist`). It maps 16 KiB `MAP_FIXED` at each address and writes to it.

| Binary | __PAGEZERO | Launch | 0x10000 | 0x400000 | 0x7ffe0000 | 0x140000000 |
|---|---|---|---|---|---|---|
| ad hoc | 4 GB (default) | runs | ENOMEM | ENOMEM | ENOMEM | ok |
| entitled `.app` | 4 GB (default) | runs | ok | ok | ok | ok |
| ad hoc | 0x4000 | killed, status 137 | | | | |
| entitled `.app` | 0x4000 | runs | ok | ok | ok | ok |
| entitled, bare Mach-O without bundle or profile | 4 GB (default) | killed, status 137 | | | | |

The entitlement is necessary as well as sufficient for addresses below 4 GB in an arm64 process (PROVEN). The linker rounds a 0x1000 page zero up to 0x4000. `os_cross_arch_is_supported(OS_CROSS_ARCH_X86_64)` returns 1 with or without the entitlement.

### Checks of the HVF model

Run after the first version of this report, with `spikes/hvf2.c` and `spikes/hvf2_guest.S` (MMU on, 4 KiB guest pages). These ran while the machine was in normal use: 1-minute load 1.9 to 3.7, thermal state nominal, AC power. They are not quiet measurements; the timed ones should be repeated in a quiet pass.

| Check | Result | Label and data |
|---|---|---|
| Speed of x86 ordering in a vCPU | loop of 5 load/store pairs: host plain 1.673 ns per iteration, vCPU plain with ACTLR.TSO clear 1.685, vCPU plain with ACTLR.TSO set 1.679, LDAPR/STLR 3.17 on host and in the vCPU | PROVEN for this single-thread loop, `data/spikes/hvf_tso_speed.jsonl`; hardware TSO costs nothing here, LDAPR/STLR cost 88%. A contended multi-thread loop under TSO is NOT measured |
| x18 in a vCPU, EL1 | preserved across 200000 host exits with 3125 host yields and 20 busy host threads forcing preemption and migration | PROVEN, `data/spikes/hvf2_functional.txt` |
| x18 in a vCPU, EL0 | preserved across 2000000 svc round trips to the EL1 vector and a brk | PROVEN, same file; this phase ran 0.07 s, so preemption during it is not shown |
| Fault resolved by the guest's EL1 vector | 68.1 ns median, p95 70.2, sd 0.8 per translation fault and eret, no host exit | PROVEN, `data/spikes/hvf_guest_fault.jsonl` |
| Data-abort exit to the host | 755 ns median, p95 762, sd 3.1 (stage-2 fault, host skips the load, resume) | PROVEN, `data/spikes/hvf_stage2_exit.jsonl` |
| Low virtual addresses | VA 0x10000 (4 KiB page) and 0x400000 written and read through the guest's own tables, host sees the frames | PROVEN, `hvf2_functional.txt` |
| More than one VM per host process | second `hv_vm_create` fails with 0xfae94002; after `hv_vm_destroy` a new VM can be created | PROVEN, `hvf2_functional.txt`: one VM per host process |
| Translator inside a vCPU | not run | NOT verified. The plain loop runs at host speed in a vCPU (row 1), so translated code is expected to as well (INFERRED) |

## Boundary costs

All PROVEN, medians with p95 and standard deviation, 30 runs.

| Path | Median | p95 | sd | Data |
|---|---|---|---|---|
| In-process gate, general registers and stack switch | 1.75 ns | 1.77 | 0.02 | `data/spikes/gate.jsonl` |
| In-process gate, also SIMD and flags | 8.76 ns | 8.83 | 0.04 | same |
| vCPU and host thread, spin on shared guest memory | 115 ns | 126 | 5.6 | `data/spikes/hvf_shmspin.jsonl` |
| Two native processes, spin on a shared word | 144 ns | 189 | 19 | `data/rosetta-stage/channel-native.csv` |
| Same, spin 2000 rounds then doorbell | 134 ns | 141 | 2.7 | same |
| Rosetta x64 guest call served by a spinning arm64 host | 1110 ns | 1762 | 447 | `data/rosetta-stage/boundary.csv` |
| Guest HVC exit, host, resume | 672 ns | 678 | 48 | `data/spikes/hvf_roundtrip.jsonl` |
| POSIX signal round-trip in process: svc, brk, fault | 1.32, 1.58, 1.67 µs | 1.33, 1.72, 1.68 | 0.004, 0.054, 0.007 µs | `data/spikes/exc_signal.jsonl` |
| Two native processes, both sides sleep, doorbell | 6.86 µs | 7.07 | 0.11 | `channel-native.csv` |
| Rosetta guest call, both sides sleep, doorbell | 7.42 µs | 7.81 | 0.17 | `boundary.csv` |
| Mach exception in process: brk, fault | 8.23, 8.25 µs | 8.39, 8.43 | 0.59, 0.17 | `data/spikes/exc_mach.jsonl` |
| Rosetta guest call, host asleep, guest spins then traps | 31.2 µs | 32.0 | 0.35 | `boundary.csv` |
| Rosetta guest int3 answered as a Mach exception by the host | 25.8 µs | 26.2 | 0.63 | `boundary.csv` |
| Guest-local fast path (GetLastError-style) | 1.65 ns | 1.92 | 0.10 | `boundary.csv` |

The Rosetta served call is the noisiest row: 407 ns at best and 2027 ns at worst over 30 runs. Which cores the two threads land on is the likely cause (INFERRED, not checked with thread pinning, which macOS does not offer).

Wake latency for blocking waits, two threads (PROVEN, `data/spikes/waits.jsonl`): `os_sync_wait_on_address` 2.41 µs, `__ulock_wait` 2.42 µs, Mach semaphore 2.11 µs, pthread condition 2.90 µs, spin 107 ns. Across processes: shared `os_sync` 3.48 µs (sd 0.78), Mach semaphore 2.37 µs, POSIX named semaphore 3.36 µs, spin 111 ns.

## Comparison of the models

1. In process: Raven, its ARM64 DLLs and our translator in one macOS process.
2. Rosetta stage: an x86-64 guest process run by Rosetta, served by the native arm64 host (working today, `e3-stage-draft`).
3. Separate arm64 guest process running our translator, controlled through Mach.
4. HVF: a Hypervisor.framework VM per Windows process, with a small guest kernel at EL1 and the host as the other half.

| Event or property | 1 In process | 2 Rosetta stage | 3 arm64 guest process | 4 HVF |
|---|---|---|---|---|
| API call served by the host, host spinning | 1.75 to 8.8 ns gate (PROVEN) | 1.11 µs (PROVEN) | 144 ns channel (PROVEN); translator exit on top NOT measured | 115 ns (PROVEN) |
| Same, both sides asleep | n/a, direct call | 7.42 µs (PROVEN) | 6.86 µs (PROVEN) | NOT measured; HVC 672 ns plus two wakes, about 5.5 µs (INFERRED) |
| Fault or breakpoint | 1.6 µs signal, 8.2 µs Mach (PROVEN) | 25.8 µs (PROVEN) | cross-process Mach exception NOT measured, at least 8.2 µs (INFERRED) | 68 ns in the guest kernel, 755 ns as a data-abort exit to the host (PROVEN, not quiet) |
| Isolation from the host | none; Raven loses the separation it has on Linux (INFERRED from the design) | separate process (PROVEN, stage) | separate process (INFERRED from the design) | separate VM inside the host process (INFERRED from the design) |
| x86 memory order | LDAPR/STLR, +39% on entry.c (PROVEN) | hardware, Rosetta (PROVEN) | LDAPR/STLR, +39% on entry.c (PROVEN) | hardware ACTLR.TSO: correct, and no measurable cost on a single-thread loop (PROVEN, not quiet) |
| 4 KiB page semantics | no; emulated for translated code only | 4 KiB mappings into the guest (PROVEN on `e3-stage-draft`, `docs/macos/process-model.md`) | no | yes (PROVEN) |
| Addresses below 4 GB | cross-architecture entitlement (PROVEN) | page zero 0x1000 under Rosetta (PROVEN on `e3-stage-draft`) | cross-architecture entitlement (PROVEN) | own page tables, 0x10000 and 0x400000 mapped (PROVEN) |
| Entitlements | cross-architecture | none beyond signing | cross-architecture | hypervisor (`com.apple.security.hypervisor`), cross-architecture not needed (INFERRED) |
| x18 for native ARM64 PE code | lost on CPU migration (PROVEN) | n/a, x64 only | lost on CPU migration (PROVEN) | preserved at EL1 and EL0 across exits, svc and host migration (PROVEN) |
| Guest threads | host threads | host threads | host threads | 64 vCPUs per VM (PROVEN); needs an M:N scheduler |
| Processes | one host process for many guests possible | one guest process per Windows process | same as 2 | one VM per host process (PROVEN): one host process per Windows process |
| Thread creation | host thread | host thread | host thread | vCPU creation 22.6 µs median, up to about 1.7 ms (PROVEN); guest threads beyond 64 cost a guest context switch instead (INFERRED) |
| Every host service | function call | channel or trap | channel or trap | channel (115 ns spin) or exit; nothing reaches the host without one (INFERRED from the design) |
| JIT and W^X for the translator | one MAP_JIT region, per-thread toggle; RWX refused (PROVEN, `data/spikes/jit_probes.txt`) | Rosetta's own | same as 1 | guest page tables of our own (INFERRED) |
| Translator speed | measured in a process (table below) | n/a, Rosetta | as model 1 | NOT measured inside a vCPU; plain code runs at host speed there (PROVEN), so expected the same (INFERRED) |
| Lifetime | ours | Rosetta general use through macOS 27, only a games subset from macOS 28 (Apple, support.apple.com/en-us/102527, 2026-09-21) | ours | ours |
| Main new code | translator, signal and exception plumbing | done as a stage | translator, guest process, Mach control | translator, EL1 guest kernel (traps, page tables, M:N scheduler), host device side |

## Recommendation

Use HVF (4), with no Rosetta stage in between; models 1 and 3 are not recommended. Decided after this checkpoint: the port goes straight to HVF, and the Rosetta stage on `e3-stage-draft` stays only as a measurement reference.

- The three properties that separate HVF from models 1 and 3 are now PROVEN: 4 KiB pages; hardware TSO that is correct and costs nothing measurable on a single-thread loop, against 88% for LDAPR/STLR on the same loop and 39% on entry.c; and x18 preserved at EL1 and EL0 across exits and host migration. The timed checks ran with the machine in use and should be repeated quietly, and TSO under contention is not measured yet.
- Low addresses are not a differentiator: models 1 and 3 get them through the cross-architecture entitlement (PROVEN), HVF through its own page tables with the hypervisor entitlement (PROVEN).
- Faults resolved in the guest cost 68 ns, a data-abort exit to the host 755 ns and a hypercall 672 ns, against 8.2 µs for a Mach exception in process.
- HVF is the only model that runs third-party ARM64 and ARM64EC programs and avoids the ordering cost. Models 1 and 3 cannot run ARM64 PE code with x18 on this macOS build (PROVEN) and pay LDAPR/STLR on memory-dense loops. Model 1 also gives up process isolation.
- This conflicts with the stated preference to avoid a VM, and the costs are real: a guest kernel at EL1 that we write and verify (exception vectors, page tables, an M:N scheduler on 64 vCPUs, waits), one host process per Windows process because a process holds one VM, vCPU creation up to milliseconds, every host service crossing an exit or the shared-memory channel, and harder debugging. The waits crate and the TLA+ models (`native/waits`, `native/model`) are the natural base for the in-guest scheduler.
- Everything built around the stage stays: the channel, the served block protocol, the D3D command ring, the AppKit and Metal backends and the translator are independent of how guest code executes.

Checks 1 to 4 and 6 of the first version of this report are done (section "Checks of the HVF model"). Still open:

1. The translator running inside a vCPU on the four workloads.
2. TSO cost under contention (two vCPUs on one line), and the timed checks repeated on a quiet machine.
3. Preemption of a vCPU while it runs at EL0 for seconds, with x18 checked.

## Spin then park

The threshold should follow the wake cost of the primitive that parks (INFERRED from the numbers above, the usual competitive rule: spin for about as long as a park and wake costs, then park):

| Primitive | Wake round trip | Spin round trip | Spin budget |
|---|---|---|---|
| private `os_sync` / `__ulock` | 2.41 µs | 107 ns | about 2.4 µs, about 20 round trips |
| shared `os_sync` across processes | 3.48 µs | 111 ns | about 3.5 µs |
| channel doorbell | 6.86 µs | 144 ns | about 7 µs |
| Rosetta stage doorbell | 7.42 µs | 1.11 µs | about 7 µs |

The missing input is the distribution of real service times per API (NOT measured): the budget is only right if most services finish within it. The channel test's fixed 2000-round spin is not derived from these numbers and should be replaced.

## Translator on the M5

Quiet run, 30 interleaved rounds, ratio of warm median to native (PROVEN, `data/translator/linux_summary.csv`; the file name is inherited from the Linux driver).

| Workload | Ours, plain (not x86-correct) | Ours, LDAPR/STLR | Ours, DMB | Rosetta |
|---|---|---|---|---|
| entry.c loop | 0.829 | 1.150 | 6.033 | 0.830 |
| CRC-32 | 1.326 | 1.325 | 1.325 | 1.158 |
| FNV-1a | 1.005 | 1.005 | 0.999 | 0.996 |
| Histogram | 1.526 | 1.535 | 1.552 | 1.478 |

First call, translation included: entry 14.5 µs, CRC-32 47.6 µs, FNV-1a 93.8 µs, histogram 92.1 µs, against 3 to 70 µs for Rosetta. Rosetta translates ahead of time; ours here does not use the ahead-of-time cache.

## Wine on Rosetta (reference)

Bottles McSoda 11.0-4, `wine-experimental.bleeding.edge.11.0.411656.20260810 ( TkG Plain )`, x86-64 under Rosetta. Environment: `WINEMSYNC=1` (Mach-port msync), `WINEDEBUG=-all`, `WINEDLLOVERRIDES=mscoree=;mshtml=`. Prefix created once with `wineboot -i`; each run uses an APFS clone of it. Notepad++ 8.9.9.1 x64 was launched outside the measuring session's sandbox, because its window never appeared inside it. 3 runs plus 1 warm-up, no load gate, load 2.9 to 3.6: not a quiet measurement (`data/wine-msync/`).

| Measure | Median | Min | Max of 3 |
|---|---|---|---|
| set_event, ns | 29 | 28 | 29 |
| wait_signaled, ns | 54 | 54 | 54 |
| event_ping_pong, ns | 6203 | 4231 | 7629 |
| wait_any_8 / wait_all_8, ns | 99 / 112 | 99 / 112 | 100 / 114 |
| section_4_threads / srw_4_threads, ns | 60 / 126 | 59 / 125 | 60 / 127 |
| process_ping_pong, ns | 8084 | 8053 | 8272 |
| startup.exe, cold, s | 1.021 | 1.013 | 1.025 |
| Notepad++ first window, cold, s | 2.493 | 2.483 | 2.495 |

The Linux references for call-bench and for 32-bit startup do not exist yet and are not estimated here.

## Reproduce

```
cmake -S native/darwin -B build-e3 -G Ninja && ninja -C build-e3                     # on e3-stage-draft
cmake -S native/translate -B build-translate -G Ninja && ninja -C build-translate   # on macos-translate-draft
clang -O2 spikes/<probe>.c spikes/<probe>_asm.S -o <probe>            # see each source's header
RAVEN_MAC_QUIET=... RAVEN_MAC_SPIKES=... RAVEN_BENCH_CHECKOUT=... MCSODA=... WINEPREFIX_BASE=... LLVM_MINGW=... NPP_X64=... \
    spikes/quiet_pass.sh
```

`hvf` needs the `com.apple.security.hypervisor` entitlement (ad hoc signing is enough). The entitlement probes need a team-signed `.app` as described above.
