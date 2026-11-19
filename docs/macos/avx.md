# AVX, AVX2 and FMA3 in the translator

`native/translate/` translates VEX-encoded instructions: the VEX forms of SSE and SSE2 at 128 and 256 bits, vzeroupper and
vzeroall, the 128-bit lane moves, the broadcasts and FMA3. This note records what x64 code contains of AVX, how CRT code chooses AVX
paths, what cpuid and xgetbv report, the YMM state, the tests against Rosetta and an x86 CPU, a short performance check and the open
items. SSE is in [sse.md](sse.md), x87 in [x87.md](x87.md).

Labels are those of [translation.md](translation.md): **PROVEN** (a command produced the result), **INFERRED** (derived; the missing
check is named), **NOT verified**. MacBook Air M5 (Mac17,3), macOS 27.2.1, llvm-mingw 20261104. The x86 reference is an AMD EPYC 9V74
(GitHub Linux x86_64 runner, the private raven-macos-bench repository, run 44490908127); Rosetta runs the same cases on every build,
and where the two disagree the tests take the EPYC's result.

## Corpus

`docs/macos/avx-data/avx_reach.py` counts VEX-encoded instructions by family, telling VEX (C4, C5) from EVEX (62) by the raw
encoding, in code reachable from the `.pdata` function starts, the entry point and the exports by direct branches and fall-through
(as `x87_reach.py`; a lower bound). For static libraries every instruction counts (object files, exact). PROVEN (`python3 -I
docs/macos/avx-data/avx_reach.py llvm-objdump llvm-readobj FILE` on each):

| Image | Vector VEX reachable | BMI | EVEX (AVX-512) | VEX in `.pdata` ranges | cpuid, xgetbv reachable |
|---|---|---|---|---|---|
| `notepad++.exe` (x64, MSVC) | 903: AVX 625, AVX2 integer 248, FMA3 30 | 2 (shlx) | 12 (vpminuq) | 927 | 8, 2 |
| `updater/GUP.exe` | 257: AVX 191, AVX2 57, FMA3 9 | 0 | 0 | 285 | 8, 2 |
| the five Notepad++ plugin DLLs | 51 to 138 each | 0 | 0 | 105 to 185 | 4 or 8, 2 |
| llvm-mingw `libc++.dll`, `libomp.dll`, `libunwind.dll`, `libwinpthread-1.dll`, `libclang_rt.asan_dynamic-x86_64.dll` | 0 | 0 | 0 | 0 | `libomp.dll`: 1, 0 |
| `libmingwex.a`, `libc++.a`, `libucrt.a`, `libunwind.a`, `libclang_rt.builtins-x86_64.a` | 0 | 0 | 0 | | builtins: 13, 2 (`x86.c.obj`) |

`notepad++.exe` by mnemonic (reachable):

- **AVX** (VEX, FP and 128-bit integer): vmovdqu 127, vzeroupper 120, vmovsd 41, vmovaps 37, vmovq 30, vaddsd 29, vmulsd 28, vmovdqa
  24, vmovd 17, vpxor 16, vmovntdq 16, vinsertf128 13, vsubsd 13, vpinsrd 12, vpand 7, vcomisd 7, then fewer than 7 each (vextractf128
  6, vmovmskps 6, vpsrldq 6, vblendvpd 4, vcmpgt_oqpd 4, vpshufb 3, vmaskmovpd 2, ...).
- **AVX2 integer** (256-bit integer and AVX2-only): vpmovmskb 79, vpcmpeqw 47, vpand 17, vpmaskmovd 13, vpblendvb 10, vpcmpeqb 9,
  vpermd 8, vpsrld 6, vpandn 6, vpcmpeqd 6, vpsllvd 6, vpmaxsd 5, vpcmpeqq 4, vpmovzxwd 4, ... vpbroadcastw 1.
- **FMA3**: vfmadd213sd 16, vfmadd231sd 13, vfmsub213sd 1: scalar doubles only.
- **F16C and gathers: 0** in every image. Neither was implemented (open items); measured, not guessed.

What this says:

- **The mingw and llvm-mingw runtime has no AVX** (PROVEN: 0 VEX in every DLL and static library above). AVX in x64 Windows code
  here comes from MSVC: Notepad++ and its plugins, whose AVX2 code is mostly string scanning (vpcmpeqw or vpcmpeqb, then vpmovmskb)
  and memory copies (vmovdqu, vmovntdq, vzeroupper). That those functions are the statically linked CRT's is INFERRED from their
  shape; the MSVC CRT is proprietary and its code was not read beyond locating the cpuid and xgetbv sites (CONTRIBUTING.md).
- **sse.md's 847 VEX instances** were counted by the decoder test, linearly over `.text`, on XMM or YMM operands only (vzeroupper has
  none, EVEX is apart). The 1027 linear here include vzeroupper (120 reachable), EVEX (46) and BMI (2): the two counts measure
  different sets (INFERRED; not reconciled instruction by instruction).
- **An AVX-512 path exists** (12 reachable EVEX vpminuq): the CPUID policy below must never report AVX-512.

### How code chooses an AVX path

- **compiler-rt** (`libclang_rt.builtins`, `x86.c`, `__cpu_indicator_init`, read in LLVM's source, not disassembled): AVX counts only
  when leaf 1 ECX has OSXSAVE (bit 27) and AVX (bit 28) and `xgetbv(0)` has bits 1 and 2 (SSE and AVX state) set; AVX2 (leaf 7 EBX bit
  5) and FMA (leaf 1 ECX bit 12) count only on top of that. INFERRED from the source; it is what `__builtin_cpu_supports` and
  `target_clones` dispatch on.
- **MSVC's CRT** (Notepad++): 8 reachable cpuid and 2 xgetbv sites in two functions, which store an ISA level in a global before the
  AVX2 and AVX-512 paths can run (PROVEN: the counts and that stores follow the xgetbv; the exact bits tested were not read, see above).
- **libomp**: 1 cpuid (its own CPU detection; no VEX code behind it).

## cpuid and xgetbv

**Before:** cpuid (0F A2) and xgetbv decoded as `other` and ended the block as unsupported: x64 code asking for features stopped
(PROVEN: `ab6c7a84`, `FAIL (in process): cpuid leaf 0x0 subleaf 0 did not return (exit 6)`, and the same in a vCPU).

**Now** (`native/translate/cpuid.h`): a block ends before cpuid or xgetbv with `ExitReason::cpuid`; `Runtime::Thread::run` answers
from one table and goes on after the instruction, in this process and in a vCPU (one run loop). The policy:

- **A feature is reported only when the translator runs every instruction it adds.** `cpuid_check.h` holds sample encodings per
  feature (SSE3, SSSE3, FMA, CMPXCHG16B, SSE4.1, SSE4.2, MOVBE, POPCNT, XSAVE, AVX, F16C, BMI1, AVX2, BMI2, LZCNT) and fails if a
  reported bit has a sample `translates()` refuses.
- **The x86-64 baseline is always reported** (psABI x86-64-v1: FPU, CX8, CMOV, MMX, FXSR, SSE, SSE2; long mode and syscall in
  0x80000001), because x64 code may assume it without asking. MMX, fxsave and cmpxchg8b are not translated yet (open items).
- **Bits that describe the same state agree:** OSXSAVE and XSAVE come with AVX; then XCR0 is 7 and leaf 0xd subleaf 0 lists 7 with
  832 bytes, subleaf 2 places the YMM state at 576 (256 bytes). Without AVX, OSXSAVE is clear and xgetbv faults (`FaultKind::instruction`)
  as x86 raises #UD; xgetbv of an XCR other than 0 faults as #GP. FMA, F16C and AVX2 are never reported without AVX; no AVX-512 bit
  and no XCR0 bit above 2 is ever set.
- Vendor "GenuineIntel" (as Rosetta), family 6, highest basic leaf 0xd, extended 0x80000008, brand "Raven x86-64 translator".

**What it reports today: the baseline only, no AVX** (PROVEN, `translate_cpuid` and `hvf_cpuid`: leaf 1 ECX 0, EDX 0x07808101; leaf
7 EBX 0; xgetbv faults). AVX promises all of AVX1, including the VEX forms of SSE3 to SSE4.2 (vpshufb, vptest, vpblendvb, vpmaxsd,
vpmovzx, vpinsrd ...), which the translator does not run in either encoding, and vblendv, vmaskmov, vpermilps and the AVX
predicates 8-31 of vcmpps; AVX2 also needs vpermq, vpermd, the variable shifts, gathers and vpmaskmov. So the AVX code above is
translated and tested but not yet reachable through CPUID-gated paths: MSVC's CRT takes its SSE2 paths. The model with every feature
on is checked for the same consistency (`every feature on, model`, 0 failures), so turning AVX on later is one table change plus
the samples passing.

## State

- **YMM upper halves are `CpuState::ymm_high[16][2]`**, beside `xmm`, and stay there while translated code runs. The low halves
  remain v0-v15. A 256-bit instruction loads the upper halves it reads into v20-v23 (free: the SSE code uses v16-v19), runs the SSE
  code on them and stores the result's upper half back. So the trampolines, the native thunks and the vCPU exits do not move them.
- **x86's rules**, each PROVEN by the `transitions` family (300 cases; every case starts with nonzero upper halves in all sixteen
  registers, so clearing and keeping differ):
  - a VEX.128 instruction that writes an XMM register clears bits 255:128 of its YMM register;
  - a legacy SSE instruction keeps them (it never touches `ymm_high`);
  - VEX scalar instructions take bits 127:32 (or 127:64) from the first source and clear 255:128;
  - vzeroupper clears all sixteen upper halves; vzeroall clears all of every register;
  - comis and ucomis write no vector register and keep them.
- **ARM64EC.** The ARM64EC ABI page (learn.microsoft.com/en-us/windows/arm/arm64ec-abi, 2025-11-06) maps registers through the x64
  `CONTEXT` and says "x13, x14, x23, x24, x28, and v16 through v31 have no representation and, thus, can't be used in Arm64EC"; it
  says nothing of YMM or AVX. Microsoft's x64 calling convention page (2025-03-19) says "When present, the upper portions of
  YMM0-YMM15 and ZMM0-ZMM15 are also volatile". So (INFERRED from both): native ARM64EC code cannot see or change the upper halves,
  any value is valid across a call to it, and keeping them in CpuState meets the ABI; v20-v23 are scratch inside one translated
  instruction, which ARM64EC code never interrupts. Not run against Windows (NOT verified).

## Instructions

| Family | Instructions | How |
|---|---|---|
| VEX forms of SSE and SSE2 | every instruction sse.md lists, at 128 bits and (packed) at 256 | the SSE code once per half; the destination starts as the first source, through v22 when the destination is the second source |
| Masks at 256 bits | vmovmskps, vmovmskpd, vpmovmskb | each half's mask, the upper one shifted above |
| Lanes | vinsertf128, vinserti128, vextractf128, vextracti128, vperm2f128, vperm2i128 | moves of q registers between v0-v15, CpuState and memory; zeroing per imm bits 3 and 7 |
| Broadcasts | vbroadcastss, vbroadcastsd, vbroadcastf128, vpbroadcastb, w, d, q | dup of element 0 |
| FMA3 | vfmadd, vfmsub, vfnmadd, vfnmsub in 132, 213, 231, ps, pd, ss, sd | scalar: fmadd, fnmsub, fmsub, fnmadd (one rounding); packed: fmla or fmls into a copy of the addend, negated first for fmsub and fnmsub (negation is exact) |

- **Lane crossing:** the per-half lowering is right only for instructions that work within 128-bit lanes. vshufpd takes imm bits 3:2
  for the upper lane (a first run failed 12 shuffle cases without it). The shifts by an XMM count use the same low 128 bits in both
  halves. The width-changing conversions at 256 bits (vcvtps2pd, vcvtpd2ps, vcvtdq2pd, vcvt(t)pd2dq) and vpermq, vpermd, vpermps are
  not translated.
- **Not SME.** Plain NEON pairs; no streaming mode was tried (NOT measured).
- **The decoder** gives VEX semantics to these and prints them as LLVM does: 20000 random VEX encodings join the decoder corpus, 0
  print differently from LLVM or decode where LLVM rejects them (PROVEN, `x86_decoder_test`: `vex_instances.o: 25765 instructions,
  17900 with semantics (69.5%), 10690 rejected by LLVM, 0 failures`; the count includes what LLVM decodes between them).
- `translator_version` is 7 (6: the cpuid exit; 7: VEX blocks).

## Tests

- **`x86_avx.py`** writes 5788 cases, each family with a seed of its own: moves 500, transitions 300, arithmetic 900, logic 200,
  compare 500, integer 700, shuffle 400, lanes 400, fma 1200, convert 400, and 288 deterministic `fma edges` (every FMA3 form, ss and
  sd, on a NaN in each position, signaling and quiet, inf times 0 plus a QNaN, exact cancellation, results at the smallest normal,
  every RC, FZ and DAZ). A third of the register picks alias (destination = first source, = second source, or both sources equal).
- **`x86_avx_oracle.c`** runs them on x86; `x86_avx_check.h` compares every general register but RSP and R15, all 256 bits of every
  YMM register, the flags, MXCSR but DE and the buffer, and applies `testdata/x86_avx_silicon.txt` with Rosetta fingerprints, as
  x87.
- **Rosetta** executes AVX, AVX2 and FMA3 but does not report them: cpuid leaf 1 ECX is 0x0298220f (no OSXSAVE, AVX, FMA) unless
  `ROSETTA_ADVERTISE_AVX=1`, which changes only the cpuid answers (0x7ed8320f, leaf 7 EBX 0x128, XCR0 7). PROVEN by a probe run both
  ways.
- **Test first:** the oracle and all its families were committed together, failing (`61cd0c08`: 11576 of 11576 in this process,
  5788 of 5788 in a vCPU, all unsupported exits), then the implementation in one commit (`0532a99e`). Per-family test-first commits were
  not made (timebox). Before `0532a99e` passed: comis cleared upper halves and read a wrong register (33 compare failures), vshufpd
  used the low lane's imm bits (12); both fixed in it.
- **Results** (PROVEN, `ctest`): `translate_x86_avx` 11576 cases (both memory orderings), 0 failures; `hvf_avx` 5788 cases in a vCPU, 0
  failures; MXCSR.DE alone differs in 789 passing cases (DE is never set, as in sse.md).

### x86 silicon and Rosetta

5788 cases; the EPYC gave the same bytes on two runs (PROVEN, `cmp` in the workflow) and differs from Rosetta in 4
(`python3 -I docs/macos/avx-data/silicon_overrides.py ROSETTA.bin SILICON.bin`, PROVEN):

- **FMA3: 0 differences** in 1488 cases, the 288 edges included: fused single rounding, NaN choice and sign, FZ, DAZ and every flag of
  the scalar and packed forms agree with the EPYC to the bit, in this process and in a vCPU (PROVEN for this AMD core; Intel NOT
  verified).
- **4 cases differ, all one pattern:** a VEX add or mul with a signaling NaN in one source and a quiet NaN in the other, in the
  encoding the assembler chose (it swaps commutative sources to fit a two-byte VEX prefix, so the text's order is not the bytes'):

  | Case | Encoded sources (first, second) | Rosetta and the translator | EPYC |
  |---|---|---|---|
  | `vaddps xmm7, xmm9, xmm0` | ff812345 (SNaN), 7fc12345 (QNaN) | ffc12345 | 7fc12345 |
  | `vaddps ymm3, ymm9, ymm3` | 7fa00000 (SNaN), ffc00000 (QNaN) | 7fe00000 | ffc00000 |
  | `vmulps ymm12, ymm12, ymm1` | ff812345 (SNaN), 7fc12345 (QNaN) | ffc12345 | 7fc12345 |
  | `vaddps ymm14, ymm14, ymm0` | 7fc00000 (QNaN), 7fa00000 (SNaN) | 7fc00000 | 7fe00000 |

  In all four the EPYC returned the second source's NaN (quieted); Rosetta and the translator (FPCR.AH: the first source's NaN,
  quieted, PROVEN on the host by a probe of fadd and fmul) return the first. That contradicts the rule sse.md relies on ("the first
  source operand's NaN") for these VEX cases; whether legacy SSE behaves the same on the EPYC is open (the SSE oracle found no such
  case). The four lines carry `known`: the tests count them apart (`4 of them known to differ`) and fail if one starts to pass. Not
  fixed: the rule is not understood from four cases (open items).

## Performance check

`hvf_bench_run.py build-macos-hvf RESULTS 5 .../quiet/thermal fsaxpy,bytecount` with `native/hvf/testdata/avx_workloads.c`
(intrinsics, so the instructions do not depend on the vectorizer; raw data in `docs/macos/avx-data/perf/`):

- **fsaxpy:** y = a * x + y over 1024 singles, 16 passes: vfmadd231ps on YMM registers. The native build uses vfmaq_f32.
- **bytecount:** bytes equal to each of 16 values in 4096 bytes: vpcmpeqb, vpmovmskb and a scalar bit count (built with
  `-mno-popcnt`, which the translator does not run). The native build counts with NEON 16 bytes at a time: not the same algorithm.

5 rounds, the median of each process's warm runs per call, ratio to native. Load average 19.97 at the start (1, 5, 15 minutes:
19.97 13.83 11.22) and 15.00 at the end, thermal state 1 at both ends: a busy machine, standard deviations 10-30%.

| Workload | Native | Process, loop | Process, call | vCPU, loop | vCPU, call |
|---|---|---|---|---|---|
| fsaxpy | 2742 ns (sd 446) | 4128 ns (1.50, sd 897) | 4118 ns (1.50, sd 629) | 5099 ns (1.86, sd 740) | 5921 ns (2.16, sd 1645) |
| bytecount | 5946 ns (sd 1460) | 6198 ns (1.04) | 5560 ns (0.94) | 4831 ns (0.81) | 6121 ns (1.03) |

- **Checksums** are identical in all five configurations for both workloads (PROVEN: fsaxpy 17189222219625267200, bytecount
  2071036322561519860), so the translated FMA3 rounds as the native fmla does.
- **What dominates fsaxpy** (INFERRED from the generated code, not profiled): per 256-bit instruction two loads and a store of upper
  halves to CpuState and one FMA per half, against one native fmla per 128 bits. Keeping upper halves in v24-v31 within a block would
  remove most of that; not done.
- bytecount below native measures the native algorithm and the noise (sd above 1 µs), not a translator gain.

## Open items

- **AVX is not reported by cpuid** until the rest of AVX1 is translated (the list under cpuid and xgetbv); then AVX2, then FMA.
- **The four NaN cases** (silicon and Rosetta): more targeted cases on the EPYC, legacy SSE and VEX, both source orders, to find the
  rule; an Intel core to compare.
- **Not translated:** VEX forms of SSE3-SSE4.2, vblendvps/pd, vpblendvb, vmaskmov and vpmaskmov (masked moves must not fault on
  masked-out lanes), vpermilps, vpermq, vpermd, vpermps, variable shifts, gathers (0 in the corpus), F16C (0 in the corpus),
  vcmp predicates 8-31, width-changing conversions at 256 bits, vldmxcsr's reserved bits (as ldmxcsr), BMI1 and BMI2.
- **The x86-64 baseline bits** include MMX, FXSR and CX8, whose instructions are not translated yet; software built for the Windows
  11 24H2 baseline may also use SSE4.2 and POPCNT without asking.
- **`IsProcessorFeaturePresent` and KUSER_SHARED_DATA's XState** must agree with cpuid once AVX is reported; they live in native/nt,
  not touched here.
- **xsave, xrstor, xsaveopt** are not translated; CONTEXT has no YMM part yet.
- **Alignment:** vmovaps and the other aligned forms do not raise #GP on a misaligned address.
- **New A64 encodings** (fmla, fmls, fneg, the four-operand FMA forms) are proven by the oracle, not by `a64_emitter_test`.
- **The Linux build** was not run (NOT verified).
