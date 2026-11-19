# SSE and SSE2 in the translator

`native/translate/` translates SSE and SSE2 on XMM registers. This note records the guest state, the coverage measured on Notepad++
x64, the x86 semantics that differ from Arm and how each is handled, the tests and their results, a short performance check and the
open items. x87 is in [x87.md](x87.md), AVX in [avx.md](avx.md).

Labels are those of [translation.md](translation.md): **PROVEN** (a command produced the result), **INFERRED** (derived; the missing
check is named), **NOT verified**. MacBook Air M5 (Mac17,3), macOS 27.2.1, Xcode 27.2, llvm-mingw 20261104. The x86 reference is
Rosetta, not an x86 CPU: a result the oracle shows is said to **agree with Rosetta** (PROVEN as such), and agreement with x86 silicon
is INFERRED, except for the conversion flags below: `native/translate/testdata/sse_flags_probe.c` ran on an AMD EPYC 9V45
(GitHub Linux x86_64 runner) and printed the same 360 lines as under Rosetta (PROVEN: `diff docs/macos/sse-data/flags_probe_rosetta.txt
docs/macos/sse-data/flags_probe_x86.txt` is empty).

## Guest state

- **XMM0-XMM15 are v0-v15** while translated code runs, Microsoft's ARM64EC map. v16-v19 are scratch.
- **Between runs they are `CpuState::xmm`.** The enter trampoline loads all sixteen and the exit trampoline stores them, both in this
  process and in a vCPU. This replaces `CpuState::vector_high` and `TranslatorOptions::vector_registers`, which are gone:
  - movq is no longer limited to a vCPU;
  - `translator_version` is 3 and the cache key no longer holds the option;
  - `Runtime::call` callers read and write `state().xmm` and `state().mxcsr`.
- **MXCSR is `CpuState::mxcsr`** (0x1f80 by default). While translated code runs, its control bits are FPCR and its flags are FPSR:

  | MXCSR | FPCR or FPSR |
  |---|---|
  | RC (bits 14:13): 00 nearest, 01 down, 10 up, 11 zero | RMode (23:22): 00, 10, 01, 11 (down and up swap) |
  | FZ (bit 15) | FZ |
  | DAZ (bit 6) | FIZ (FEAT_AFP) |
  | (always) | AH = 1 (FEAT_AFP) |
  | IE, ZE, OE, UE, PE (bits 0, 2, 3, 4, 5) | IOC, DZC, OFC, UFC, IXC (FPSR bits 0-4) |
  | DE (bit 1) | none: kept as it was (see the open items) |

- **What the trampolines do with it.**
  - The enter trampoline keeps the host's FPCR in `CpuState::host_fpcr`, then sets FPCR and FPSR from MXCSR.
  - The exit trampoline folds FPSR's flags back into MXCSR and restores the host's FPCR.
  - `ldmxcsr` stores the whole value in `CpuState::mxcsr` and sets FPCR and FPSR from it.
  - `stmxcsr` folds FPSR in first. Its MXCSR helpers are shared by the trampolines and the translator.
- **Native thunks** (`bind_native`, in this process) do four things:
  - store v0-v15 and the folded MXCSR in CpuState, so the native function sees the guest's registers;
  - run the function with the host's FPCR;
  - load XMM6-XMM15 back from CpuState: they are callee-saved for x64, while AAPCS64 keeps only the low halves of v8-v15, and not v6
    or v7;
  - set FPCR and FPSR from MXCSR again.

  v0 is the function's floating-point result, as XMM0 is.
- **ARM64EC native code** (in a vCPU, reached without leaving translated code) runs with FPCR.AH and FIZ clear and the guest's RMode
  and FZ. `arm64ec_entry` and `arm64ec_resume` clear AH and FIZ on the way into native code; `arm64ec_dispatch_ret` and
  `arm64ec_dispatch_call` set AH again and FIZ from MXCSR.DAZ on the way back.
  - What Microsoft says: the ARM64EC ABI page (learn.microsoft.com/en-us/windows/arm/arm64ec-abi, 2025-11-06) says nothing about
    FPCR, FPSR or MXCSR. It maps registers through the x64 `CONTEXT`, whose only floating-point control is MxCsr, so the rounding
    mode and FZ being one state is INFERRED from that, not stated. AH and FIZ are this translator's own: Arm code compiled for
    Windows assumes them clear, which is why they are cleared and RMode and FZ are not.
  - PROVEN by hvf_arm64ec_test: `fmax with a NaN in native code, as Arm without FPCR.AH` returns the NaN (before the change it
    returned 1.0: `FAIL: ...: 4607182418800017408, native 9221120237041090560`), and `maxsd with a NaN in x64 code after native
    code, as x86` returns 1.0.
- **FPCR in a vCPU** stays the guest's between runs (`CodeRunner::restores_host_fpcr`, false for a `Vcpu`): its dispatcher does no
  floating point, and the enter trampoline writes FPCR and FPSR only when they differ from what MXCSR asks for. In this process the
  exit trampoline gives the host its FPCR back.
- **FEAT_AFP is required.** A runtime without a runner refuses to start when `hw.optional.arm.FEAT_AFP` (on Linux, HWCAP2_AFP) is 0,
  and a vCPU refuses to be created when its ID_AA64MMFR1_EL1.AFP (bits 47:44) is 0, each with an error naming FEAT_AFP: without it
  FPCR.AH reads as zero and SSE would quietly follow Arm. `Runtime::host_has_afp` and `Vcpu::id_aa64mmfr1_override` let the tests
  simulate a CPU without it (PROVEN: runtime_test `no FEAT_AFP, no runtime: ok`, `hvf_sse` passes its refusal check).
- **FEAT_AFP is there in both runners.**
  - PROVEN: `sysctl hw.optional.arm.FEAT_AFP` prints 1.
  - In a vCPU, ID_AA64MMFR1_EL1.AFP reads 1 (`hv_vcpu_get_sys_reg`: `mmfr1 100011312000 afp=1`), and `HV_REG_FPCR` written as
    0x1000003 (FZ, AH, FIZ) reads back unchanged.
  - The oracle results below pass in a vCPU at EL0 as in this process.

## Coverage

The share of SSE and SSE2 instructions on XMM registers in `notepad++.exe` (x64, GPL) that the translator takes, as
`x86_decoder_test` counts them with the corpus built with `-DRAVEN_CORPUS_IMAGES=.../npp/app/notepad++.exe`.

- **What counts as SSE and SSE2:**
  - the legacy 0F map with XMM registers or MXCSR;
  - not VEX, EVEX, 0F 38 or 0F 3A;
  - not the SSE3 opcodes of that map;
  - not the forms with an MMX register.
- **Not in the denominator:** sfence, lfence, mfence, prefetch and movnti (not on XMM registers), the MMX forms and SSE3.
- **What counts as translated:** `translates()`, the translator's own check, accepts the instruction. That is static.
  - The evidence that the code is right is the oracle, per family, below.
  - Notepad++ itself was not run under the translator (NOT verified).

| When | SSE and SSE2 instances | With semantics in the decoder | Translated |
|---|---|---|---|
| Before (211decc0) | 40832 | 515: `movq` between general and XMM registers | 0 by default; 515 (1.3%) with `vector_registers`, which only `hvf_arm64ec_test` set |
| After moves, masks and logic (b0ee6212) | 40832 | 40831 | 36794 (90.1%) |
| After conversions (050c8f4d) | 40832 | 40831 | 39983 (97.9%) |
| After packs and shuffles (23520cae) | 40832 | 40831 | 40831 (99.998%) accepted statically: only `pinsrw` (1) is not; Notepad++ was not run |

The "Before" row is measured at 892a4aaa, where the decoder already knew SSE and `translates()` took only movq between general and XMM
registers (PROVEN there); that 211decc0 translated none by default and those 515 with the option is INFERRED from its `supported()`,
since the tally did not exist then. PROVEN at the end: `x86_decoder_test build-macos-hvf/translate/x86_corpus.txt` prints
`notepad++.exe: 40832 SSE and SSE2 instructions, 40831 with semantics (100.0%), 40831 translated (100.0%); without: pinsrw 1`, and
for the 8000 random instances of `x86_corpus.py`: `8000 SSE and SSE2 instructions, 8000 with semantics (100.0%), 8000 translated`.
The decoder prints every one of them as LLVM's disassembler does (0 failures over the corpus, Notepad++ included).

What Notepad++ x64 has beyond SSE2 (PROVEN, `llvm-objdump -d` of `notepad++.exe`):

- VEX (AVX), 847 instances on XMM or YMM registers: the AVX task.
- SSSE3 and SSE4, 123:
  - pshufb 31, pcmpestri 24, pmaxsd 18, ptest 14;
  - pcmpistri 10, pcmpestrm 10, pblendvb 5, roundsd 3, pcmpeqq 3;
  - roundss 2, pinsrb 1, pextrq 1, pcmpgtq 1.
- Not on XMM registers: sfence 2 and prefetchw 2. Both still end a block as unsupported.

### Instruction families

| Family | Instructions | Arm code |
|---|---|---|
| Moves | movaps, movups, movapd, movupd, movdqa, movdqu, movntps, movntpd, movntdq, movss, movsd, movd, movq, movlps, movhps, movlpd, movhpd, movhlps, movlhps | orr, ldr and str of q, d and s, ins, fmov; a scalar load clears the rest of the register as x86's does |
| Masks | movmskps, movmskpd, pmovmskb | ushr, then usra to gather the bits, then two umov |
| Logic | andps, andnps, orps, xorps, the pd forms, pand, pandn, por, pxor | and, bic, orr, eor; xor of a register with itself is movi #0 |
| Arithmetic | add, sub, mul, div, sqrt, min and max in ps, pd, ss and sd | one vector or scalar instruction; scalar results go to element 0 with ins, as x86 keeps the destination's other elements |
| MXCSR | ldmxcsr, stmxcsr | CpuState, FPCR and FPSR as above |
| Compares | cmpps, cmppd, cmpss and cmpsd with predicates 0-7, comiss, comisd, ucomiss, ucomisd | fcmeq, fcmge, fcmgt, not; fcmp or fcmpe, then NZCV and the PF source |
| Conversions | cvtsi2ss, cvtsi2sd, cvt(t)ss2si, cvt(t)sd2si (32 and 64 bit), cvtss2sd, cvtsd2ss, cvtdq2ps, cvtdq2pd, cvt(t)ps2dq, cvt(t)pd2dq, cvtps2pd, cvtpd2ps | scvtf, fcvt, fcvtl, fcvtn, frintx and fcvtzs with a fix-up for the integer indefinite |
| Integer | padd and psub (b, w, d, q; signed and unsigned saturating), pmullw, pmulhw, pmulhuw, pmuludq, pmaddwd, pcmpeq and pcmpgt (b, w, d), pavgb, pavgw, pminub, pmaxub, pminsw, pmaxsw, psadbw | add, sub, sqadd, uqadd, sqsub, uqsub, mul, smull or umull and uzp2, xtn and umull, smull and addp, cmeq, cmgt, urhadd, umin, umax, smin, smax, uabd and uaddlp |
| Shifts | psll, psrl, psra (w, d, q) by an immediate or by an XMM register or memory; psrldq, pslldq | shl, ushr, sshr; ushl or sshl by a count limited to the width; ext with a zero register |
| Shuffles | packsswb, packssdw, packuswb, punpckl and punpckh (bw, wd, dq, qdq), unpcklps, unpcklpd, unpckhps, unpckhpd, pshufd, pshuflw, pshufhw, shufps, shufpd | sqxtn, sqxtun and their 2 forms, zip1, zip2, ins per element |

Every new encoding is checked against the host assembler in `a64_emitter_test`, 249 words (PROVEN).

## Where x86 and Arm differ

Each decision below rests on cases of the oracle test, which compares every bit of every XMM register, the general registers, the
flags, MXCSR (without DE) and memory. The counts say how many of the cases or elements in `sse_oracle.bin` fall in each class (a
script over the oracle's inputs; it is not in the repository). All pass in this process (LDAPR/STLR and barriers) and in a vCPU,
which proves agreement with Rosetta; agreement with x86 silicon is INFERRED where the line says so.

- **NaN propagation.** x86 returns the first source operand's NaN, quieted, whichever operand is signaling. Arm without AH prefers a
  signaling NaN in either operand. An invalid operation gives x86's QNaN indefinite (sign set: 0xffc00000) where Arm gives a positive
  default NaN.
  - Decision: FPCR.AH = 1 (FEAT_AFP), which gives both.
  - PROVEN in this process by a probe, AH 0 then 1:
    - `add(q1, s2)` = 7fc00002, then 7fc00001;
    - `mul(0, inf)` = 7fc00000, then ffc00000.
  - Agrees with Rosetta: 175 elements with two NaNs and 385 with one, in add, sub, mul and div.
  - That x86 silicon agrees: INFERRED from the SDM's rule (first source operand) and from Rosetta.
- **minps, maxps, minss, maxss and the pd and sd forms.**
  - x86 returns the second operand when either operand is a NaN, signaling ones unchanged, and when both are zeros of either sign.
    Arm without AH returns the NaN.
  - With AH, fmin and fmax return the second operand in both cases.
  - Decision: the destination is the first operand and the source the second.
  - Agrees with Rosetta: 286 elements with a NaN operand in either position (160 of them signaling) and 31 with +0 and -0 in either order.
- **Conversions to integers.** Out of range or NaN, x86 gives the integer indefinite: 0x80000000, or 0x8000000000000000 for 64 bits.
  fcvtzs saturates instead and gives 0 for NaN.
  - **Scalar:** fcvtzs, then a fix-up with fcmp and csel. It keeps NZCV with mrs and msr when a later instruction reads the flags.
    For 32 bits from a double, the fix-up compares with 2^31, because 2147483647.5 truncates validly to the value saturation also
    gives.
  - **cvtps2dq and cvttps2dq:** fcvtzs, then fcmgt against 2^31 and a select, since fcvtzs already gives 0x80000000 below -2^31.
  - **cvtpd2dq and cvttpd2dq:** element by element as the scalar case, so FPSR gets IOC for every out-of-range element as MXCSR
    gets IE.
  - **Rounding forms:** frintx rounds as RMode says and reports inexact, then fcvtzs converts an integer.
  - **Flags when rounding makes a value out of range** (2147483647.5 to nearest): frintx reports inexact and fcvtzs then invalid, but
    x86 raises IE alone. FPSR is cleared before frintx and merged back without IXC when IOC was raised. Agrees with Rosetta (the
    boundaries family: 20 values, 10 forms, 4 rounding modes, 800 cases; before the fix 14 of them failed with `mxcsr 0x1fa1, x86 left
    0x1f81`); x86 silicon does the same, PROVEN by the probe on an AMD EPYC 9V45: `rc=0 cvtsd2si r32 41dfffffffe00000 -> 80000000 flags 01`
    (IE alone), identical to Rosetta on all 360 lines (`docs/macos/sse-data/flags_probe_x86.txt`).
  - Agrees with Rosetta: 78 conversions of a NaN and 552 out of the int32 range (333, 79, 87 and 53 under RC 0 to 3), with the
    flags matching.
- **Rounding mode.** RC goes to RMode with the two directed modes swapped.
  - Agrees with Rosetta: 725, 709 and 636 cases run with RC 1, 2 and 3, the rest with 0.
  - The integer to float conversions: 245, 48, 53 and 44 cases under RC 0 to 3.
  - Cases also change MXCSR with ldmxcsr in the middle of a block.
- **FZ and DAZ.**
  - x86's FZ flushes results only, after rounding; DAZ flushes inputs. Arm's FZ without AH flushes both.
  - With AH, FZ flushes outputs and FIZ inputs: FZ goes to FZ, DAZ to FIZ.
  - Agrees with Rosetta, with modest counts: 35 cases with FZ in which x86 reports underflow, and 65 elements that are denormal inputs under DAZ.
    More targeted cases would make this stronger.
- **comiss, comisd, ucomiss, ucomisd.**
  - x86 sets ZF, PF and CF: 111 unordered, 000 greater, 001 less, 100 equal. It clears OF, SF and AF.
  - After fcmp:
    - ZF is EQ or VS;
    - CF is LT, which unordered (V set) also satisfies;
    - C keeps the inverse of CF, the form block boundaries use;
    - the PF source is an even byte when unordered, and AF's source equals it.
  - comis signals on a quiet NaN as fcmpe does; ucomis only on a signaling one, as fcmp.
  - Agrees with Rosetta: 158 elements with a NaN, and readers (setcc on every condition) in the same block.
- **cmpps predicates.**
  - lt, le, nlt and nle signal on a quiet NaN (fcmgt and fcmge); eq, neq, unord and ord only on a signaling one (fcmeq).
  - Unordered makes neq, nlt, nle and unord true.
  - Agrees with Rosetta: 370 elements with a NaN.
- **Exception flags.** IE, ZE, OE, UE and PE match x86's exactly in every passing case: the comparison includes them.
  - DE is never set: Arm reports a denormal input (IDC) only when it flushes one, and with FIZ it would set the flag where x86 does
    not.
  - The tests count the cases where only DE differs: 366 of 9400, in MXCSR or in what stmxcsr stored.
- **Memory.**
  - Vector loads and stores have no LDAPR or STLR form. In LDAPR/STLR and barriers modes they are plain accesses with DMB ISHLD
    after a load and DMB ISHST before a store. Under hardware TSO (a vCPU) they are plain.
  - Every address form goes through the same path as general loads:
    - an index register;
    - displacements beyond one Arm offset (the mov and add path);
    - the stack;
    - RIP-relative constants.
  - Agrees with Rosetta for one thread: the addressing family, 600 cases.
  - TSO across threads for vector accesses: NOT verified, since no litmus test uses XMM registers.

## Tests

- **`x86_sse.py`** writes 9400 cases (seed 20261120) in ten families:

  | Family | Cases |
  |---|---|
  | moves | 600 |
  | logic | 300 |
  | arithmetic | 1500 |
  | compare | 1200 |
  | convert | 1500 |
  | integer | 1500 |
  | shuffle | 800 |
  | addressing | 600 |
  | edges | 600 |
  | boundaries (deterministic: conversions to integers on their limits, every RC) | 800 |

  - Inputs come from pools of edge values: ±0, ±denormals, ±inf, QNaN and SNaN with payloads, ±2^31, 2^31-1, ±2^63, ties, limits
    of the integer conversions. Fixed-seed random values fill the rest.
  - MXCSR goes through every RC value, with FZ, DAZ and set flags in part of the cases.
  - The edges family puts signed-zero and NaN pairs, in both orders, into every element.
- **`x86_sse_oracle.c`** runs them under Rosetta. `run_case` loads and stores all sixteen XMM registers and MXCSR.
- **`x86_sse_check.h`** reads the results and compares them; `x86_sse_test` (`translate_x86_sse`, in this process, LDAPR/STLR and
  barriers) and `hvf_sse_test` (`hvf_sse`, a vCPU with hardware TSO) both use it.
- **`runtime_test`** has three cases:
  - XMM1 and XMM9 across exits after which the runner writes every vector register;
  - XMM6 across a native function that writes every vector register;
  - a native function that sees the host's FPCR and the guest's XMM1 and MXCSR in CpuState, after which the guest's MXCSR
    (rounding up) and all 128 bits of XMM8 are back.
- **The decoder test** requires semantics for 8000 random SSE and SSE2 instances and checks how they print against LLVM's.

PROVEN: `ctest --test-dir build-macos-hvf`, 17 of 17, three times in a row at the end of this work, and `ctest --test-dir
build-macos-translate` (native/translate's own build), 5 of 5.

### Failing runs, before each implementation

Each family's test was committed failing, then its implementation.

| Test commit | Before the implementation | After |
|---|---|---|
| `20aad9e4` (XMM state) | `FAIL: XMM1 - XMM9 across an exit gave returned 0x3f7fffdd`; `FAIL: XMM6 across a native function gave returned 0` | 9a2e272e |
| `56cba84a` (decoder) | `sse_instances.o: 8000 instructions, 204 with semantics (2.5%), 0 rejected by LLVM, 7796 failures` | 892a4aaa |
| `d0e576ec` (moves, logic) | moves 585 of 600, logic 300 of 300, in this process and in a vCPU (unsupported exits; the 15 passing were movq between general and XMM registers) | b0ee6212 |
| `97f3d070` (arithmetic) | 1500 of 1500 | a9907af2 |
| `be826cab` (compares) | 1200 of 1200 | 4fd76409 |
| `782acc77` (conversions) | 1500 of 1500 | 050c8f4d |
| `bd896677` and `dfd29cc9` (integer) | 1500 of 1500 | 82ab2697 |
| `66bca999` (shuffles) | 800 of 800 | 23520cae |
| `5e28f925` (conversion boundaries) | 14 of 800, in this process and in a vCPU: `cvtsd2si eax, xmm1: mxcsr 0x1fa1, x86 left 0x1f81` | 62c925db |
| `9a39879e` (FEAT_AFP) | does not build: `no member named 'host_has_afp' in 'raven::translate::Runtime'` (new API) | 44bd2976 |
| `378541fb` (ARM64EC FPCR) | `FAIL: fmax with a NaN in native code, as Arm without FPCR.AH: 4607182418800017408, native 9221120237041090560` | 071d0c4c |

- **`bd896677` broke the build.** Its generator failed on a memory shift count (`ValueError: invalid literal for int() with base 10:
  'word'`). `dfd29cc9` fixes it; history was not rewritten.
- **Arithmetic, first attempt.** It passed Rm to the unary fsqrt, which made an undefined encoding: SIGILL in this process (exit
  132), and in a vCPU `FAIL: guest exception: exit syndrome 0x5a000001, ESR_EL1 0x2000000` (EC 0, unknown instruction). The second
  attempt left only DE: `maxps xmm13, xmm8: mxcsr 0x1f80, x86 left 0x1f82`, in 173 cases. The comparison now leaves DE out and
  counts it.
- **Written after their implementation.** `6de5cf31` (native functions and FPCR), `3c5bb3af` (address forms and barriers) and
  `3fe2a0a6` (edge pairs) all passed at once. For `6de5cf31`, a mutation makes the case fail: without the host FPCR before the call
  and the reload of XMM6-XMM15 after it, the run prints:
  - `FAIL: XMM6 across a native function gave returned 0xffffffffffffffff`;
  - `FAIL: the native function ran with FPCR 4194306`;
  - `FAIL: XMM8 changed across the native function`.

## Oracles on x86 silicon

Both oracles ran on an AMD EPYC 9V45 (GitHub Linux x86_64 runner, raven-macos-bench run 44411803673): the same generated cases,
assembled for ELF instead of Mach-O, so 856 of the 10000 integer cases have other encodings of the same instructions (the assembler
picks another `xchg` form). Compared field by field as the tests compare them, with `docs/macos/sse-data/compare_oracles.py`:

| Oracle | Cases | Rosetta and the EPYC differ | Where |
|---|---|---|---|
| SSE and SSE2 | 9400 | 1 | `cmpordss xmm15, xmm14`: MXCSR.DE set by Rosetta (0x3f82), not by the EPYC (0x3f80); DE is not compared |
| integer | 10000 | 1 | `lock cmpxchg dword` that succeeds: Rosetta clears the upper half of RAX, the EPYC keeps all of it |

- PROVEN: the translator passes both x86 results, in this process and, for SSE, in a vCPU (`x86_semantics_test X86.bin`: `10000 cases,
  0 failures`; `x86_sse_test`: `18800 cases, 0 failures`; `hvf_sse_test`: `9400 cases, 0 failures`), after the cmpxchg fix.
- The cmpxchg case was a real defect copied from Rosetta: the translator wrote the accumulator on success too. Test first: the integer
  test now applies `native/translate/testdata/x86_silicon.txt` (cases where silicon and Rosetta disagree, with the value silicon left)
  and failed with `register 0 is 0xebc6a17c, x86 left 0x7f5a3510ebc6a17c`; the fix writes RAX only when the compare fails.
- So "agrees with Rosetta" above also holds against this x86 CPU for every case of both oracles (PROVEN for this AMD core). An Intel
  core was not run (NOT verified); Intel's documented pseudocode also writes the accumulator only on failure (INFERRED).

## Performance check

`hvf_bench_run.py build-macos-hvf RESULTS 5 .../quiet/thermal saxpy,dot,copy`: the SSE workloads of
`native/hvf/testdata/sse_workloads.c`.

- **saxpy:** 1024 singles, 16 times; mulps and addps after clang's vectorizer.
- **dot:** 1024 doubles, 8 times, in order; mulsd and addsd.
- **copy:** 4096 bytes, 16 times, with movdqu and pxor.

The source turns FP contraction off, since SSE2 has no fused multiply-add. Without that, the native arm64 build fuses `a * x + y` and
its checksum differs from x86's; with it, the native and both translated checksums match (PROVEN, the checksums printed by
`hvf_native_saxpy` and `hvf_bench`).

5 rounds, the median of the warm runs per call, and the ratio to native. load average 13.0 to 9.2 (1, 5 and 15 minutes at the start:
13.02 15.83 17.49), thermal state 1 at both ends: a busy machine.

| Workload | Native | Process, loop | Process, call | vCPU, loop | vCPU, call |
|---|---|---|---|---|---|
| saxpy | 757 ns | 1619 ns (2.14) | 1685 ns (2.23) | 1631 ns (2.15) | 2601 ns (3.43) |
| dot | 4900 ns | 10703 ns (2.18) | 9974 ns (2.04) | 10131 ns (2.07) | 11004 ns (2.25) |
| copy | 1395 ns | 1260 ns (0.90) | 1364 ns (0.98) | 1245 ns (0.89) | 2106 ns (1.51) |

- **copy below native:** that measures how the native build compiled the loop, not a translator gain.
- **vCPU per call:** every call is a host exit and a resume, and each stores and loads sixteen q registers and converts MXCSR. This
  table was measured at 803c9f96, before FPCR was left alone in a vCPU (below).
- No optimisation was done here; that belongs to the later generator work.

### The integer workloads before and after

The cost the SSE state adds to every exit and entry, on code without SSE: `hvf_bench` of 211decc0 (built from `git archive` in a
scratch directory) and of this branch, interleaved run by run on one machine, the median of 5 warm runs of each process.

- **The first pass** (all four workloads, loop and call, 3 rounds, at d0d37cda) ran with the load average at 37 falling to 18, which
  swamps most of it: crc32, fnv and histogram moved between 0.77 and 1.51 times the old time in both directions, run to run.
- **entry, one call per run, which is exit and entry and little else**, measured again on its own, 10 to 15 rounds, min and median of
  the rounds:

  | Build | vCPU, call | Process, call |
  |---|---|---|
  | 211decc0 | 1078 ns, median 2529 | 398 ns, median 466 |
  | the SSE state without the FPCR and FPSR work (an experiment, not committed) | | 433 ns, median 492 |
  | 803c9f96 (FPCR and FPSR set on every entry and exit) | 1218 ns, median 2575 | |
  | d0d37cda (FPCR and FPSR set only when they differ; FPCR kept in a vCPU) | 1089 ns, median 2461 | 429 ns, median 553 |

  Load average 7 to 45 across these runs, thermal state 1: the minima are the steadier numbers.
- **What that says (INFERRED from those minima):**
  - In a vCPU the regression came from writing FPCR on every exit and entry (+13% on the minimum). With FPCR left as the guest's it is
    gone (+1%).
  - In this process the host needs its FPCR back after each run, so the write stays, and the sixteen q registers cost about 35 ns per
    call. That is +8% on the minimum for a workload made of exits; the integer workloads with real work per call do not show it
    above the noise.
- **What is not done:** moving only the vector registers a block uses, and keeping FPCR in this process (host code there would run
  with AH set). Both belong to the generator work; this process is the test runner, the vCPU the process model.

## SSE3, SSSE3, SSE4.1 and SSE4.2

These are translated too, in their legacy forms, with semantics checked by the AVX oracle's `sse3`, `ssse3`, `sse41`, `sse42` and
`popcnt` families against an AMD EPYC (avx.md has the counts): addsub as one fadd of a source whose even elements fneg negated,
hadd and hsub with faddp and uzp, pshufb as tbl on indices masked to 0x8f, pmulhrsw as smull and rshrn by 15 (exact at -32768
squared, where sqrdmulh saturates), round with frint (and FPCR's RMode set around frintx when a fixed mode must still raise PE),
dpps with masked inputs and two faddp, mpsadbw with ext, dup and uabal, phminposuw with uminv, ptest clearing PF, AF, SF and OF
as x86 does (Rosetta keeps them), crc32 as Arm's crc32c and popcnt as cnt and addv. SSE4.2's string compares (pcmpestri,
pcmpestrm, pcmpistri, pcmpistrm) run in the host, as cpuid does (`string_compare.h`): the block ends before one and the runtime
computes it on CpuState, every data format, aggregation, polarity and output selection, the implicit lengths of the i forms (the
first null element) and the explicit ones of the e forms (EAX and EDX, or RAX and RDX with REX.W or VEX.W1: their absolute value,
saturated to the element count), with CF, ZF, SF and OF and AF
and PF cleared. The `strings` family (1500 cases, imm8 values over all 128 and lengths around 0, 8 and 16, with registers and
memory) passes against an AMD EPYC 7763 and an Intel Xeon Platinum 8370C, which both agree with Rosetta in every case (PROVEN,
avx.md). rcpps, rsqrtps, rcpss and rsqrtss divide exactly, which is within x86's 1.5 * 2^-12; a denormal input counts as zero and
a reciprocal below the smallest normal becomes a signed zero, as on x86, and FPSR is put back since x86 raises nothing (the
`approximations` family compares within 2^-11, avx.md). cpuid reports SSE3, SSSE3, SSE4.1, SSE4.2 and POPCNT (PROVEN,
`translate_cpuid`). SSE2's maskmovdqu is translated too, a branch per byte around its store so that a masked-off byte
is never touched (PROVEN: the `maskmovdqu` family, 200 cases against an Intel Xeon 8573C, and `hvf_avx_fault`, which failed before
it with the masked-off bytes on an unmapped page).

## Open items

- **Without FEAT_AFP** the runtime refuses to run (see the guest state). Software sequences for those instructions would let it run
  on older cores: not done, and no core without it was tried (NOT verified).
- **The host's FPSR** is replaced by the guest's flags on entry, and only FPCR is restored on exit. The host's cumulative flags are
  lost across a run. This matters only to host code that tests floating-point exceptions.
- **ldmxcsr** stores reserved bits instead of raising #GP.
- **DE** is never set.
- **Unmasked exceptions:** MXCSR's masks are ignored, so an unmasked exception does not raise #XM. Arm traps them only with FPCR's
  trap enables, which are left clear.
- **Alignment:** packed memory operands that are not 16-byte aligned do not raise #GP as on x86. The address is used as it is.
- **movntps, movntpd and movntdq** are plain stores; the non-temporal hint is dropped.
- **ARM64EC native code's FPSR flags** end up in MXCSR, as they would with one shared state; a native change of FPCR.RMode does not
  reach `CpuState::mxcsr` until the guest's next ldmxcsr, so stmxcsr can report the old RC.
- **Signal handlers in this process** (the `TestProcess` fault handler, and any host handler a signal reaches while translated code
  runs) run with the guest's FPCR: AH, FIZ and the guest's rounding mode. The kernel restores FPCR from the signal frame on return
  (INFERRED from the arm64 signal frame's NEON state), so only the handler's own floating point is affected. Setting
  `CpuState::host_fpcr` at handler entry would fix it; not done.
- **Cost of every exit and entry:** sixteen q-register moves and the MXCSR conversions; in this process also two FPCR writes (see the
  integer workloads above).
- **Not translated:**
  - the MMX forms, the 16 SSSE3 ones included, which end their block though cpuid reports SSSE3 (MMX state aliases x87 and is
    not implemented; avx.md says who can emit them);
  - sfence, lfence, mfence, prefetch and movnti.
- **Rosetta is the only x86 reference.** Running `x86_sse_oracle` on an x86 machine would turn the INFERRED agreement with silicon
  into PROVEN.
- **The Linux build** was not run (NOT verified). The new code uses `using enum` (C++20, GCC 11 or later: INFERRED from GCC's
  release notes).
