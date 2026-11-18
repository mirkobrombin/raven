# SSE and SSE2 in the translator

`native/translate/` translates SSE and SSE2 on XMM registers. This note records the guest state, the coverage measured on Notepad++
x64, the x86 semantics that differ from Arm and how each is handled, the tests and their results, a short performance check and the
open items. x87 comes next, then AVX; neither is here.

Labels are those of [translation.md](translation.md): **PROVEN** (a command produced the result), **INFERRED** (derived; the missing
check is named), **NOT verified**. MacBook Air M5 (Mac17,3), macOS 27.2.1, Xcode 27.2, llvm-mingw 20261104. The x86 reference is
Rosetta, not an x86 CPU: where only Rosetta says what x86 does, agreement with x86 silicon is INFERRED.

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
- **What counts as translated:** `translates()`, the translator's own check, accepts the instruction. That is static.
  - The evidence that the code is right is the oracle, per family, below.
  - Notepad++ itself was not run under the translator (NOT verified).

| When | SSE and SSE2 instances | With semantics in the decoder | Translated |
|---|---|---|---|
| Before (211decc0) | 40832 | 515: `movq` between general and XMM registers | 0 by default; 515 (1.3%) with `vector_registers`, which only `hvf_arm64ec_test` set |
| After moves, masks and logic (b0ee6212) | 40832 | 40831 | 36794 (90.1%) |
| After conversions (050c8f4d) | 40832 | 40831 | 39983 (97.9%) |
| After packs and shuffles (23520cae) | 40832 | 40831 | 40831 (99.998%): only `pinsrw` (1) is not |

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
script over the oracle's inputs; it is not in the repository). All pass in this process (LDAPR/STLR and barriers) and in a vCPU.

- **NaN propagation.** x86 returns the first source operand's NaN, quieted, whichever operand is signaling. Arm without AH prefers a
  signaling NaN in either operand. An invalid operation gives x86's QNaN indefinite (sign set: 0xffc00000) where Arm gives a positive
  default NaN.
  - Decision: FPCR.AH = 1 (FEAT_AFP), which gives both.
  - PROVEN in this process by a probe, AH 0 then 1:
    - `add(q1, s2)` = 7fc00002, then 7fc00001;
    - `mul(0, inf)` = 7fc00000, then ffc00000.
  - PROVEN by the oracle: 175 elements with two NaNs and 385 with one, in add, sub, mul and div.
  - That x86 silicon agrees: INFERRED from the SDM's rule (first source operand) and from Rosetta.
- **minps, maxps, minss, maxss and the pd and sd forms.**
  - x86 returns the second operand when either operand is a NaN, signaling ones unchanged, and when both are zeros of either sign.
    Arm without AH returns the NaN.
  - With AH, fmin and fmax return the second operand in both cases.
  - Decision: the destination is the first operand and the source the second.
  - PROVEN: 286 elements with a NaN operand in either position (160 of them signaling) and 31 with +0 and -0 in either order.
- **Conversions to integers.** Out of range or NaN, x86 gives the integer indefinite: 0x80000000, or 0x8000000000000000 for 64 bits.
  fcvtzs saturates instead and gives 0 for NaN.
  - **Scalar:** fcvtzs, then a fix-up with fcmp and csel. It keeps NZCV with mrs and msr when a later instruction reads the flags.
    For 32 bits from a double, the fix-up compares with 2^31, because 2147483647.5 truncates validly to the value saturation also
    gives.
  - **cvtps2dq and cvttps2dq:** fcvtzs, then fcmgt against 2^31 and a select, since fcvtzs already gives 0x80000000 below -2^31.
  - **cvtpd2dq and cvttpd2dq:** element by element as the scalar case, so FPSR gets IOC for every out-of-range element as MXCSR
    gets IE.
  - **Rounding forms:** frintx rounds as RMode says and reports inexact, then fcvtzs converts an integer.
  - PROVEN: 78 conversions of a NaN and 552 out of the int32 range (333, 79, 87 and 53 under RC 0 to 3), with the flags matching.
- **Rounding mode.** RC goes to RMode with the two directed modes swapped.
  - PROVEN: 725, 709 and 636 cases run with RC 1, 2 and 3, the rest with 0.
  - The integer to float conversions: 245, 48, 53 and 44 cases under RC 0 to 3.
  - Cases also change MXCSR with ldmxcsr in the middle of a block.
- **FZ and DAZ.**
  - x86's FZ flushes results only, after rounding; DAZ flushes inputs. Arm's FZ without AH flushes both.
  - With AH, FZ flushes outputs and FIZ inputs: FZ goes to FZ, DAZ to FIZ.
  - PROVEN, with modest counts: 35 cases with FZ in which x86 reports underflow, and 65 elements that are denormal inputs under DAZ.
    More targeted cases would make this stronger.
- **comiss, comisd, ucomiss, ucomisd.**
  - x86 sets ZF, PF and CF: 111 unordered, 000 greater, 001 less, 100 equal. It clears OF, SF and AF.
  - After fcmp:
    - ZF is EQ or VS;
    - CF is LT, which unordered (V set) also satisfies;
    - C keeps the inverse of CF, the form block boundaries use;
    - the PF source is an even byte when unordered, and AF's source equals it.
  - comis signals on a quiet NaN as fcmpe does; ucomis only on a signaling one, as fcmp.
  - PROVEN: 158 elements with a NaN, and readers (setcc on every condition) in the same block.
- **cmpps predicates.**
  - lt, le, nlt and nle signal on a quiet NaN (fcmgt and fcmge); eq, neq, unord and ord only on a signaling one (fcmeq).
  - Unordered makes neq, nlt, nle and unord true.
  - PROVEN: 370 elements with a NaN.
- **Exception flags.** IE, ZE, OE, UE and PE match x86's exactly in every passing case: the comparison includes them.
  - DE is never set: Arm reports a denormal input (IDC) only when it flushes one, and with FIZ it would set the flag where x86 does
    not.
  - The tests count the cases where only DE differs: 366 of 8600, in MXCSR or in what stmxcsr stored.
- **Memory.**
  - Vector loads and stores have no LDAPR or STLR form. In LDAPR/STLR and barriers modes they are plain accesses with DMB ISHLD
    after a load and DMB ISHST before a store. Under hardware TSO (a vCPU) they are plain.
  - Every address form goes through the same path as general loads:
    - an index register;
    - displacements beyond one Arm offset (the mov and add path);
    - the stack;
    - RIP-relative constants.
  - PROVEN for one thread: the addressing family, 600 cases.
  - TSO across threads for vector accesses: NOT verified, since no litmus test uses XMM registers.

## Tests

- **`x86_sse.py`** writes 8600 cases (seed 20261120) in nine families:

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
- **vCPU per call:** every call is a host exit and a resume, and each now stores and loads sixteen q registers and converts MXCSR.
  That this is what makes the shorter calls cost more is INFERRED: no run compared the same calls before the change.
- No optimisation was done here; that belongs to the later generator work.

## Open items

- **FEAT_AFP is assumed.** The translator writes FPCR.AH without checking that the host has it. On a core without FEAT_AFP the bit
  is RES0 and the write is dropped silently. NaN propagation, the default NaN's sign and min and max with a NaN or a zero pair would
  then follow Arm. The M5 and its vCPUs have it (PROVEN above); older cores were not checked (NOT verified). Without it the
  translator would give wrong results instead of exiting as unsupported, against the rule the core keeps for everything else, which
  is why this item comes first.
  - The plan: the runtime reads `hw.optional.arm.FEAT_AFP`, and in a VM the vCPU's ID_AA64MMFR1_EL1.AFP, and refuses or falls back
    to software sequences for those instructions without it.
- **The host's FPSR** is replaced by the guest's flags on entry, and only FPCR is restored on exit. The host's cumulative flags are
  lost across a run. This matters only to host code that tests floating-point exceptions.
- **ldmxcsr** stores reserved bits instead of raising #GP.
- **DE** is never set.
- **Unmasked exceptions:** MXCSR's masks are ignored, so an unmasked exception does not raise #XM. Arm traps them only with FPCR's
  trap enables, which are left clear.
- **Alignment:** packed memory operands that are not 16-byte aligned do not raise #GP as on x86. The address is used as it is.
- **movntps, movntpd and movntdq** are plain stores; the non-temporal hint is dropped.
- **ARM64EC native code in a vCPU** runs with the guest's FPCR (AH, FIZ, the guest's RMode), since the ARM64EC transitions do not
  leave translated code. Its FPSR flags end up in MXCSR. Either the `arm64ec_*` routines switch FPCR or this is accepted as Windows'
  shared MXCSR and FPCR state: undecided.
- **The fault handler** of the in-process test runner (`TestProcess`) runs with the guest's FPCR.
- **Cost of every exit and entry:** sixteen q-register moves and the MXCSR conversions. The vCPU call column above shows it.
- **Not translated:**
  - pinsrw and pextrw, maskmovdqu, rcpps, rsqrtps, rcpss and rsqrtss;
  - the MMX forms;
  - SSE3, SSSE3 and SSE4 (123 instances in Notepad++);
  - sfence, lfence, mfence, prefetch and movnti.
- **Rosetta is the only x86 reference.** Running `x86_sse_oracle` on an x86 machine would turn the INFERRED agreement with silicon
  into PROVEN.
- **The Linux build** was not run (NOT verified). The new code uses `using enum` (C++20, GCC 11 or later: INFERRED from GCC's
  release notes).
- **`ec_client.c`** still says the translator moves doubles only with movq. That is no longer true, and the test is unchanged.
