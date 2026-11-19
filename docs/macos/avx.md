# AVX, AVX2 and FMA3 in the translator

`native/translate/` translates VEX-encoded instructions: the VEX forms of SSE to SSE4.2 at 128 and 256 bits, vzeroupper and
vzeroall, the 128-bit lane moves, the broadcasts, vtestps and vtestpd, vpermilps and vpermilpd, vblendv, vmaskmov, the AVX
compare predicates, FMA3 and AVX2 (gathers included); xsave and xrstor run in the runtime. cpuid reports AVX, FMA and AVX2. This
note records what x64 code contains of AVX, how CRT code chooses AVX
paths, what cpuid and xgetbv report, the YMM state, the tests against Rosetta and an x86 CPU, a short performance check and the open
items. SSE is in [sse.md](sse.md), x87 in [x87.md](x87.md).

Labels are those of [translation.md](translation.md): **PROVEN** (a command produced the result), **INFERRED** (derived; the missing
check is named), **NOT verified**. MacBook Air M5 (Mac17,3), macOS 27.2.1, llvm-mingw 20261104. The x86 reference is an AMD EPYC
(GitHub Linux x86_64 runners, the private raven-macos-bench repository: a 9V45 for the first families, a 7763 and a 9V74 for the
later ones; `testdata/x86_avx_silicon.txt` names the runs; Intel Xeon runs are recorded where they differ);
Rosetta runs the same cases on every build, and where the two disagree the tests take the x86 CPU's result.

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
- **F16C and gathers: 0** in every image (measured, not guessed). The gathers were implemented later all the same, for AVX2's cpuid
  bit (see Instructions); F16C is not implemented (open items).

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

- **A feature is reported only when the translator runs every instruction it adds.** `cpuid_check.h` holds, per feature, at least one
  clang-assembled sample per opcode group the feature adds (every FMA3 form: 96), and fails if a reported bit has a sample
  `translates()` refuses. The tests print the samples refused today, which is the work left before each bit (PROVEN,
  `translate_cpuid`):

  | Feature | Samples translated | Refused today |
  |---|---|---|
  | SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT | 6 of 6, 7 of 7, 22 of 22, 4 of 4, 1 of 1 | none: reported |
  | AVX, with XSAVE (xsave, xrstor) | 53 of 53, 2 of 2 | none: reported |
  | FMA | 96 of 96 | none: reported |
  | AVX2 | 31 of 31 | none: reported |
  | BMI1, BMI2, LZCNT | 15 of 15, 20 of 20, 3 of 3 | none: reported |
  | F16C, MOVBE, CMPXCHG16B | 0 of each | all |
- **The x86-64 baseline is always reported** (psABI x86-64-v1: FPU, CX8, CMOV, MMX, FXSR, SSE, SSE2; long mode and syscall in
  0x80000001), because x64 code may assume it without asking. MMX, fxsave and cmpxchg8b are not translated yet (open items).
- **Bits that describe the same state agree:** OSXSAVE and XSAVE come with AVX; then XCR0 is 7 and leaf 0xd subleaf 0 lists 7 with
  832 bytes, subleaf 2 places the YMM state at 576 (256 bytes). Without AVX, OSXSAVE is clear and xgetbv faults (`FaultKind::instruction`)
  as x86 raises #UD; xgetbv of an XCR other than 0 faults as #GP. Both give the same `FaultKind::instruction`, as hlt and ud2 do: a
  later Windows exception mapping cannot tell #UD (STATUS_ILLEGAL_INSTRUCTION) from #GP (STATUS_PRIVILEGED_INSTRUCTION) by it and must
  decode the instruction and read OSXSAVE and ECX (open item). FMA, F16C and AVX2 are never reported without AVX; no AVX-512 bit
  and no XCR0 bit above 2 is ever set.
- Vendor "GenuineIntel" (as Rosetta), family 6, highest basic leaf 0xd, extended 0x80000008, brand "Raven x86-64 translator".

**What it reports today: the baseline, SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT, AVX (with XSAVE and OSXSAVE), FMA, AVX2, BMI1,
BMI2 and LZCNT; no F16C** (PROVEN, `translate_cpuid` and `hvf_cpuid`, 0 failures: leaf 1 ECX 0x1c981201, EDX 0x07808101; leaf 7
EBX 0x128; leaf 0x80000001 ECX 0x20; xgetbv(0) 7). BMI1 `0d5ba385`, BMI2 `2dcaf9ea`, LZCNT `c6c4344a`. Each was switched on in a
commit of its own once its samples translated and its oracle families passed in
this process and in a vCPU against an x86 CPU: SSE4.2 `d1070f63`, AVX `714848e2`, FMA `26122295`, AVX2 `7ff6b2f9`. AVX2 was
reported one commit too early: a review then found vbroadcasti128 undecoded; it joined the samples (`80216673`) and was translated
(`86b6adcd`), so from `86b6adcd` on the AVX2 report holds for every sample. SSE4.2 was switched on with its `strings` family
held to an Intel Xeon Platinum 8370C (run 44500206597, which agreed with Rosetta in all 1500 cases); an AMD EPYC 7763 confirmed it
later (run 44501136642, the same agreement). The model requires each SSE level to come with
the ones before it, AVX with SSE4.2, and XSAVE's samples to translate whenever XSAVE is reported, which it is with AVX: so AVX also
needed xsave and xrstor (below). The model with every feature on is checked for the same consistency (`every feature on, model`, 0
failures).

**A known gap under SSSE3: its MMX-register forms.** The 16 SSSE3 instructions on MMX registers (pshufb, phaddw, phaddd, phaddsw,
pmaddubsw, phsubw, phsubd, phsubsw, psignb, psignw, psignd, pmulhrsw, pabsb, pabsw, pabsd, palignr with `mm` operands) are not
translated and end their block as unsupported, though cpuid reports SSSE3: MMX state is not implemented (it aliases the x87
registers, x87.md). Inline or hand-written assembly can contain them with any compiler. The intrinsics do not produce them here:
this llvm-mingw clang compiles `_mm_abs_pi8` on an `__m64` to `pabsb xmm0, xmm0` between two movq (PROVEN, `clang -O2 -mssse3` and
llvm-objdump), as LLVM lowers the MMX intrinsics to SSE. GCC was not tried (no x86 GCC on this machine: NOT verified). MSVC does
not offer the MMX intrinsics for x64 (INFERRED from Microsoft's documentation of them, not checked in this session).

## xsave and xrstor

`native/translate/xsave.h`: they end a block, as cpuid does, and the runtime runs them on CpuState in the standard format with XCR0
7: the legacy area as FXSAVE lays it out (CpuState's `X87State` is that image's first 160 bytes; MXCSR at 24, MXCSR_MASK 0xffff at
28), XSTATE_BV at 512, the upper halves of YMM0-YMM15 at 576. The requested-feature bitmap is EDX:EAX and XCR0. xsave writes the
requested components and sets their XSTATE_BV bits (the SDM allows a component in its initial state to be reported in use; whether
these CPUs track it was NOT verified), and leaves the rest of the header. xrstor loads a requested component when its XSTATE_BV
bit is set and gives it its initial state otherwise, and loads MXCSR whenever SSE or AVX is requested (INFERRED from the SDM's
description; not run on x86). Both fault (`FaultKind::instruction`) for an address not aligned to 64 bytes, and xrstor for
XSTATE_BV bits outside XCR0, a nonzero XCOMP_BV or the next 8 bytes, and an MXCSR with a reserved bit; while OSXSAVE is clear
(no AVX) they fault as #UD. PROVEN by `translate_xsave` (layout, round trip, init state, the four faults; test `3be0f83c` first
failing, then `7e83388c`). Not compared with an x86 CPU (NOT verified); xsaveopt, xsavec, xsaves and fxsave are not translated.

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
  - comis and ucomis write no vector register and keep them (PROVEN by the `compare` family, not `transitions`).
- **A memory operand that faults changes nothing.** x86 faults are restartable: an instruction that raises #PF has written no
  register and, for a store, no byte, even when its 32 bytes cross into an unmapped page (INFERRED from the SDM's description of faults
  as reported before the instruction executes; no split access was faulted on the EPYC). So every load of a 256-bit instruction (both
  halves of a memory operand, both FMA3 forms) comes before its first write to the destination (the halves' results wait in v20-v23);
  and a 32-byte store first probes its first and last byte with `staddb wzr` (an atomic add of zero, which
  needs write access and changes nothing even with another thread writing), then stores. PROVEN by `hvf_avx_fault` in a vCPU, with
  the operand straddling an unmapped page: `vaddps ymm1, ymm1, [rax]` and `vfmadd231ps ymm1, ymm2, [rax]` with the low half
  unmapped leave every upper half as it was, `vmovups [rax], ymm1` with the upper half unmapped writes no byte. Before the fix
  (test `9fb748f7`) all three failed: `an upper half changed before the fault`, `memory was written before the fault`. Not covered:
  the low halves (host registers, not stored on a guest fault, so not observable this way), this process (a host fault ends it), and
  a 16-byte SSE store that crosses a page (an Arm store may then be partly performed; not probed).
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
| Broadcasts | vbroadcastss, vbroadcastsd, vbroadcastf128, vbroadcasti128, vpbroadcastb, w, d, q | dup of element 0 |
| FMA3 | vfmadd, vfmsub, vfnmadd, vfnmsub in 132, 213, 231, ps, pd, ss, sd | scalar: fmadd, fnmsub, fmsub, fnmadd (one rounding); packed: fmla or fmls into a copy of the addend, negated first for fmsub and fnmsub (negation is exact) |
| FMA3, alternating | vfmaddsub, vfmsubadd in 132, 213, 231, ps, pd | fmla into a copy of the addend negated in the subtracting elements (even for vfmaddsub, odd for vfmsubadd) |
| VEX forms of SSSE3 to SSE4.2 | maps 0F 38 and 0F 3A: every instruction sse.md lists, at 128 bits; vroundps, vroundpd, vblendps, vblendpd, vdpps and vptest also at 256; vpinsrw, vpextrw | the SSE code per half; vblendps and vblendpd take the upper half's selectors from the high imm8 bits; vpcmpestri and the others in the host as their legacy forms |
| Tests | vptest, vtestps, vtestpd at 128 and 256 bits | and and bic of both halves, or-ed; vtestps and vtestpd keep the sign bits (cmlt); ZF and CF, the other flags cleared |
| Permutes | vpermilps, vpermilpd by imm8 or by a selector vector | ins per element (imm8), tbl on byte indices built from the selector's bits |
| Blends by a register | vblendvps, vblendvpd (128 and 256), vpblendvb (128) | the SSE code with the selector named by imm8 bits 7:4; its upper half from CpuState |
| Compare predicates | vcmpps, vcmppd, vcmpss, vcmpsd with predicates 0-31 | predicate 8 inverts the unordered result, 16 swaps which predicates signal: quiet lt and le compare NaN-free copies; signaling eq and unord add an fcmge whose result is unused |
| Masked moves | vmaskmovps, vmaskmovpd (load and store), vmaskmovdqu | a branch per element around its access, so a masked-off element is never touched; a store probes the bytes it will write first |
| Conversions at 256 bits | vcvtps2pd, vcvtdq2pd ymm from 16 bytes; vcvtpd2ps, vcvtpd2dq, vcvttpd2dq xmm from 32 | the SSE conversion on each half |
| Approximations | rcpps, rsqrtps, rcpss, rsqrtss and their VEX forms | exact fdiv (and fsqrt), denormal inputs to signed infinities, tiny reciprocals to signed zeros, FPSR put back |
| AVX2, within lanes | the 256-bit integer forms of maps 0F 38 and 0F 3A, vpblendd (as blendps) | the SSE code per half; vmpsadbw takes the upper half's offsets from imm8 bits 5:3 |
| AVX2, widening | vpmovsx, vpmovzx into a YMM register | the SSE extension on each half of the source |
| AVX2, across lanes | vpermq, vpermpd (imm8), vpermd, vpermps (indices) | ins of quadwords; tbl over a table of two registers (v20, v21) with byte indices from the dword indices |
| AVX2, variable shifts | vpsllvd, vpsllvq, vpsrlvd, vpsrlvq, vpsravd | ushl or sshl by the (negated) count; a count of the width or more gives 0 (masked) or the sign (vpsravd's count capped at 31) |
| AVX2, masked moves | vpmaskmovd, vpmaskmovq | as vmaskmovps and vmaskmovpd |
| AVX2, gathers | vpgatherdd, dq, qd, qq, vgatherdps, dpd, qps, qpd (VSIB) | one element at a time in order: test the mask's sign, load, write the element and clear its mask bit; then the whole mask cleared |

- **Lane crossing:** the per-half lowering is right only for instructions that work within 128-bit lanes. vshufpd takes imm bits 3:2
  for the upper lane (a first run failed 12 shuffle cases without it). The shifts by an XMM count use the same low 128 bits in both
  halves. The width-changing conversions, the AVX2 extensions and permutes and vmpsadbw have their own lowering (above).
- **Not SME.** Plain NEON pairs; no streaming mode was tried (NOT measured).
- **The decoder** gives VEX semantics to these and prints them as LLVM does: 20000 random VEX encodings join the decoder corpus, 0
  print differently from LLVM or decode where LLVM rejects them (PROVEN, `x86_decoder_test`: `vex_instances.o: 25765 instructions,
  17900 with semantics (69.5%), 10690 rejected by LLVM, 0 failures`; the count includes what LLVM decodes between them).
- `translator_version` is 22 (6: the cpuid exit; 7: VEX blocks; 8: loads before writes in 256-bit instructions; 9: SSE3 to SSE4.2;
  10: the string compares; 11: the VEX forms of SSSE3 to SSE4.2; 12: the rest of AVX but the approximations; 13: the
  approximations; 14: vfmaddsub and vfmsubadd; 15: xsave and xrstor; 16: AVX2 but the gathers; 17: the gathers; 18: review
  fixes: a 128-bit vpgatherdq or vgatherdpd gathered 4 elements instead of 2 (reading mask and index bits x86 ignores), store
  probes left out an fs or gs base, vroundss and vroundsd with VEX.L set, a 256-bit vpmovsx or vpmovzx with VEX.vvvv set; 19:
  vbroadcasti128; 20: vpcmpestri and vpcmpestrm with VEX.W1 took their lengths from EAX and EDX, not RAX and RDX; 21: legacy
  maskmovdqu; 22: BMI1, BMI2 and LZCNT).

## Tests

- **This round's families**, each committed failing first, then implemented: `strings` 1500 (`ac2cc892`), `vex sse4` 1500 (the
  VEX forms of SSSE3 to SSE4.2, `397a4560`), `avx ops` 1500 (vtestps, vtestpd, vpermilps, vpermilpd, vblendv, vcmp with predicates
  0-31, the 256-bit conversions, vmaskmovps, vmaskmovpd and vmaskmovdqu, `cce54f83`; rdi cleared after vmaskmovdqu in `7afb535c`,
  since it held an address of the buffer that differs between machines), `approximations` 600 (`000ae951`, compared within 2^-11:
  x86 promises 1.5 * 2^-12 and its CPUs differ in the bits), `fma addsub` 600 (`2cebbc23`), `avx2` 1500 (`119658b2`; the
  variable shifts get counts around the element width), `gathers` 800 (`496b2a01`; every form and width, every scale, indices
  inside the buffer, some negative, random mask bits). PROVEN at the end (`ctest`): `translate_x86_avx` 32976 cases (16488 in each
  memory ordering), 0 failures; `hvf_avx` 16488 cases in a vCPU, 0 failures. Then `avx2 broadcast` 100 (vbroadcasti128,
  `d13dd50b`): 33176 and 16588 cases, 0 failures. The review fixes of version 18 came without a failing test first: the 128-bit
  gather bug did not show in the oracle (its stray loads stayed inside the buffer and their results were cleared).
- **Masked moves and faults** (`hvf_avx_fault`, in a vCPU, test `342adee2` first, then `69374b4f`): vmaskmovps with the elements on an
  unmapped page masked off neither faults nor moves them, and moves the others (load and store); with an element on the unmapped
  page selected it faults with no register or byte changed. The same for vpmaskmovd (`054880ed`, added after its implementation)
  and vpgatherdd (`496b2a01` first): with the unmapped elements masked off it gathers the others, keeps the masked-off ones and
  clears the mask; with one selected it faults with nothing changed. PROVEN, 12 scenarios, 0 failures.
- **Gathers that fault part way** (`3e2a9c56`): every gather form at both widths faults at element 1 and at its last element; the
  test checks that the elements before it are gathered and their mask elements cleared, and that the rest of the destination and
  mask are as they were. The rule is the SDM's (a gather may complete some elements before a fault and records them by clearing
  their mask elements, so it can restart): INFERRED from the SDM, not run on an x86 CPU (the bench has no faulting gather with a
  signal handler). Only the upper halves can be checked: a guest exception ends the run without storing v0-v15, so elements
  gathered into a low half stay in the vCPU (NOT verified there). PROVEN that the test catches the class: with the per-element
  mask clear removed it fails 3 scenarios (dd, dq and qq at 256 bits, last element), with it 0. Legacy maskmovdqu has the same
  masked-off and selected scenarios (`8d0f9610`, failing before `987b0d96`).
- **Variable shift counts** (`d07cc9cb`): the `shift counts` family gives every variable shift, d and q, register and memory
  counts of 63, 64, 128, 255, 256, 0x80000000 and 0xffffffff, with the low half or (quadwords) only the high half set. PROVEN that it
  catches the class: changing the quadword limit from 6 bits to 32 fails 50 of its 400 cases, the earlier families none.
- **Later families** (after the FMA3 edges, so the earlier cases keep their numbers): sse3 500 (legacy and VEX at 128 and 256
  bits), ssse3 500, sse41 1000 (every SSE4.1 instruction, ptest followed by a setcc), sse42 400 (pcmpgtq, crc32 of every width),
  popcnt 300 (16, 32 and 64 bits, then a setcc). Each was committed failing (`0186a0ac`, `88266640`, `ad08e950`, `ecc31d0b`,
  `ca050087`: every case unsupported), then implemented. PROVEN at the end: `translate_x86_avx` 16976 cases (8488 in each
  ordering), 0 failures; `hvf_avx` 8488 cases, 0 failures.
- **Legacy alignment.** x86 raises #GP for a legacy 16-byte operand that is not 16-byte aligned; Rosetta does not check. The first
  sse3 cases had unaligned movsldup and movshdup operands: the EPYC's oracle crashed (`Segmentation fault`) where Rosetta ran them;
  the generator now aligns them (`f4878ad2`). The translator does not raise #GP either (open item).
- **`x86_avx.py`** writes 5788 cases in its first families, each family with a seed of its own: moves 500, transitions 300, arithmetic 900, logic 200,
  compare 500, integer 700, shuffle 400, lanes 400, fma 1200, convert 400, and 288 deterministic `fma edges` (every FMA3 form, ss and
  sd, on a NaN in each position, signaling and quiet, inf times 0 plus a QNaN, exact cancellation, results at the smallest normal,
  every RC, FZ and DAZ). A third of the register picks alias (destination = first source, = second source, or both sources equal).
  clang swaps the sources of a commutative instruction to fit a two-byte VEX prefix, so the destination-equals-second-source path
  (through v22) is exercised mostly by the non-commutative ones (sub, div, the shuffles, the FMA3 forms) (INFERRED from the
  encodings).
- **Memory operands** are all `[r15 + displacement]`. The upper half of a 32-byte operand is read 16 bytes further on, a path no case
  takes with a RIP-relative, index-scaled or large displacement (NOT verified; the x87 oracle's `addressing` family is the model).
- **`x86_avx_oracle.c`** runs them on x86; `x86_avx_check.h` compares every general register but RSP and R15, all 256 bits of every
  YMM register, the flags, MXCSR but DE and the buffer, and applies `testdata/x86_avx_silicon.txt` with Rosetta fingerprints, as
  x87.
- **Rosetta** executes AVX, AVX2 and FMA3 but does not report them: cpuid leaf 1 ECX is 0x0298220f (no OSXSAVE, AVX, FMA) unless
  `ROSETTA_ADVERTISE_AVX=1`, which changes only the cpuid answers (0x7ed8320f, leaf 7 EBX 0x128, XCR0 7). PROVEN by a probe run both
  ways.
- **Test first:** the oracle and all its families were committed together, failing (`25269bd0`: 11576 of 11576 in this process,
  5788 of 5788 in a vCPU, all unsupported exits), then the implementation in one commit (`f1e280d1`). Per-family test-first commits were
  not made (timebox). Before `f1e280d1` passed, its first run failed 104 compare cases (comis cleared upper halves; then 33, comis read
  register 0) and 12 shuffle cases (vshufpd used the low lane's imm bits); all fixed in it.
- **Do not bisect into `3a4e7237`..`0f1f6e5e`:** there the decoder gives VEX instructions their SSE mnemonic and `supported()` does not
  yet refuse them, so the translator runs them as legacy SSE with the wrong operands (a null-address crash in the first AVX test run),
  without a `translator_version` change. `25269bd0` refuses them; `f1e280d1` translates them and sets the version to 7.
- **Results** (PROVEN, `ctest`): `translate_x86_avx` 11576 cases (both memory orderings), 0 failures; `hvf_avx` 5788 cases in a vCPU, 0
  failures; MXCSR.DE alone differs in 791 passing cases (DE is never set, as in sse.md). Historical: those were the first
  families; with every family today DE alone differs in 1178 passing cases (PROVEN, `hvf_avx`).

### x86 silicon and Rosetta

**This round** (runs 44501136642 and 44503053255 on an AMD EPYC 7763, 44502534716 on an EPYC 9V74, 44503360598 on an Intel Xeon
Platinum 8573C; PROVEN with `silicon_overrides.py`, the file keeps the overrides):

- `vex sse4`: 90 cases differ, every one a vptest: Rosetta keeps PF, AF, SF and OF, the EPYC clears them, as for ptest.
- `avx ops`: 157 differ, every one a vtestps or vtestpd, the same flags. vcmp with all 32 predicates, vpermil, vblendv, vmaskmov and
  the conversions agree with Rosetta.
- `approximations`: the EPYC 7763 differs from Rosetta in 358 of 600 cases (the result bits); the Xeon 8573C agrees with Rosetta in
  all 600 (so Rosetta's tables match that Intel core's: INFERRED from that agreement). The translator, which divides exactly, is
  within 2^-11 of the EPYC in every case.
- `fma addsub`: 8 cases differ, all in MXCSR, where Rosetta sets IE, UE or PE and the Xeon 8573C and the EPYC 9V74 do not; the
  translator matches them.
- `strings`: the EPYC 7763 agrees with Rosetta in all 1500, as the Xeon 8370C did.
- `avx2` (EPYC 9V74, run 44505099547): 4 cases differ, each a vpsllvq or vpsrlvq whose count has bits set above bit 31: Rosetta
  shifts by the low 32 bits, the EPYC by all 64 (so by 64 or more: 0), as the SDM says; the translator matches the EPYC. The 9V74
  also gave the same fma addsub MXCSR values as the Xeon. `gathers` (run 44505811968): all 800 agree with Rosetta. One dpps case
  (#7786, NaN lanes) differs
  between the Intel runs and the AMD ones; the AMD result is kept.

**The SSE3 to SSE4.2 families** (raven-macos-bench runs 44498540080, an AMD EPYC 7763, and 44499294363, an AMD EPYC 9V74; the file
names the latest): 2700 cases, 75 differ from Rosetta, every one a `ptest`: Rosetta leaves PF, AF, SF and OF as they were, the EPYC
clears them, as the SDM says. They are lines of `testdata/x86_avx_silicon.txt`, and the translator matches the EPYC (it clears them).
SSE3, SSSE3, the rest of SSE4.1, pcmpgtq, crc32 and popcnt agree with Rosetta in every case. The silicon file now has one `corpus`
line per family; a family without one is held to Rosetta only and the tests print `Rosetta only`; none is today.

**The first families:**

5788 cases. PROVEN (`python3 -I docs/macos/avx-data/silicon_overrides.py ROSETTA.bin SILICON.bin`): on an AMD EPYC 9V45 (run
44493373058) **0 cases differ from Rosetta** in a compared field; the EPYC gave the same bytes on two runs of the binary (`cmp` in the
workflow). So `testdata/x86_avx_silicon.txt` has no case line, only a `corpus` line with FNV-1a 64 fingerprints of the generated
cases (code and text, `generated=891c81824699c0fc`) and of Rosetta's results (`rosetta=95d7459814bec276`): the tests refuse the file
when either changes (PROVEN by editing one digit: `Rosetta's results are not those the silicon results were compared with`, exit 2).
The EPYC's dump is not committed (11 MB); its sha256 `e328a853...681945`, printed in the run's log, and its corpus fingerprints, equal
to Rosetta's, are in the file's header. And the translator passes against the EPYC in every case, in this process
and in a vCPU.

- **A false lead, recorded.** The first run (run 44490908127, an EPYC 9V74) assembled the cases with GNU as, which encodes 154 of them
  differently from clang (another VEX form, or the sources of a commutative instruction not swapped). Four of those differed in a NaN
  result and looked like "x86 returns the second source's NaN"; with the same bytes they agree. The script now compares only cases
  whose bytes match and prints how many it left out (0 for the clang run). The bench builds with clang since.
- **NaN choice with an SNaN and a QNaN source** (counted over the oracle's inputs, single register-register add, sub, mul and div):
  legacy SSE 89 lanes, VEX 3 lanes; in every one x86 returns the first source's NaN, quieted, as FPCR.AH does (PROVEN by those counts
  and a host probe of fadd and fmul with AH set). The VEX count is small (INFERRED to hold beyond it from the SSE count).
- **FMA3: 0 differences** in 1488 cases. The 288 edges cover only ss and sd; the packed fmsub and fnmsub forms, which negate the
  addend with fneg before fmla or fmls, had a NaN addend in 57 register-register lanes of the `fma` family (ps 35, pd 22), all equal to
  the EPYC's, so fneg's treatment of a NaN's sign under AH matches x86 there (PROVEN for those lanes; Intel NOT verified).
- **Rosetta** agrees with the EPYC on all of it.

## Performance check

`hvf_bench_run.py build-macos-hvf RESULTS 5 .../quiet/thermal fsaxpy,bytecount` with `native/hvf/testdata/avx_workloads.c`
(intrinsics, so the instructions do not depend on the vectorizer; raw data in `docs/macos/avx-data/perf/`):

- **fsaxpy:** y = a * x + y over 1024 singles, 16 passes: vfmadd231ps on YMM registers. The native build uses vfmaq_f32.
- **bytecount:** bytes equal to each of 16 values in 4096 bytes: vpcmpeqb, vpmovmskb and a scalar bit count (built with
  `-mno-popcnt`, which the translator does not run). The native build counts with NEON 16 bytes at a time: not the same algorithm.

PROVEN (that command; raw data `docs/macos/avx-data/perf/measurements.jsonl`, `summary.csv` and `state.json`): 5 rounds, the median of
each process's warm runs per call, ratio to native. Measured before `4f72c2d4` reordered loads and writes in 256-bit instructions, so
fsaxpy now has one more register move per instruction (NOT measured since). Load average 19.97 at the start (1, 5, 15 minutes:
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

## BMI1, BMI2 and LZCNT

`BlockTranslator::bmi` (`0f3788b3`), on general registers at 32 bits (W forms, the result zero-extended) and 64; the 16-bit tzcnt and
lzcnt (66 prefix) are not translated. The decoder gives them their own mnemonics (VEX map 0F 38 F2 to F7 and 0F 3A F0; F3 0F BC and
BD), not VEX forms of SSE.

- andn as bic; bextr as lsrv and a mask from lslv, a start or length of the width or more giving 0 or every bit (csel); blsi, blsmsk
  and blsr from neg or sub 1; tzcnt as rbit and clz, lzcnt as clz (the width for a zero source); bzhi as bic of a lslv mask, an index
  of the width or more keeping every bit; mulx as mul and umulh (umull at 32 bits), the low half written first so that the high half
  wins when both destinations are one register, as x86 does; rorx as extr; sarx, shlx and shrx as asrv, lslv and lsrv, which take the
  count modulo the width as x86 masks it to 5 or 6 bits.
- **Flags**, as the SDM defines them: ands of the result gives SF and ZF with CF and OF clear, then CF where the instruction sets it
  (blsi: the source is not zero; blsmsk, blsr, tzcnt, lzcnt: it is zero; bzhi: the index is the width or more) and ZF clear for
  blsmsk. PF and AF, undefined, are left as they were; mulx, pdep, pext, rorx and the shifts change no flag.
- **pdep and pext** loop once per set bit of the mask, lowest first, with cbz (no flag read or written): no Arm instruction does them
  (SVE2's BDEP and BEXT are not on this core). Cost (PROVEN, `hvf_bench_run.py ... bitdeposit`, `67391e2b`, raw data in
  `avx-data/perf/bitdeposit/`, 5 rounds, load average 2.24 4.67 11.74 at the start and 2.12 4.53 11.56 at the end, thermal state 0
  at both ends, one checksum in all 25 processes): 1024 pdep and 1024 pext through masks of 32 set bits on average take 40.3 µs per
  call in this process (1.08 of native, loop and call alike), 40.3 µs in a vCPU loop (1.08) and 43.0 µs in vCPU calls (1.148),
  against 37.4 µs for the same loop compiled natively. The native standard deviation (2.8 µs) is as large as the difference, so
  these say only that the translated loop is close to the native one. That is about 20 ns per instruction at 32 mask bits
  (INFERRED: the call divided by 2048). x86 does each in a few cycles on Intel since Haswell and AMD since Zen 3, and earlier Zen
  cores microcode them (INFERRED from published instruction tables, not checked in this session; NOT measured here).
- **Tests:** the `bmi` family (3000 cases, `83598012`, failing before `0f3788b3` with every case unsupported, for example
  `bmi: #17589 blsi eax, r12d: did not return (exit 6)`): every instruction at 32 and 64 bits, register and memory sources, and
  registers and memory set to zero, one bit, all ones, alternating bits, counts and bextr controls around 0, 32 and 64.
  `x86_avx_check.h` compares only the flags each instruction defines. On an AMD EPYC 7763 (run 44515104032) 492 cases differ
  from Rosetta, all in flags the SDM leaves undefined (tzcnt 176, bextr 168, lzcnt 148), which are not compared (PROVEN). Results
  (PROVEN, `ctest`): 3000 cases in each memory ordering in this process and 3000 in a vCPU, 0 failures.

## Notepad++ with these features

With AVX, FMA and AVX2 reported, the MSVC CRT's level check (8 cpuid and 2 xgetbv sites, above) can now see OSXSAVE, AVX, xgetbv(0)
= 7 and leaf 7's AVX2 bit; which level it stores, and so which routines it picks, was not read (proprietary code, CONTRIBUTING.md):
INFERRED that its AVX and AVX2 paths become reachable, and that its AVX-512 path does not (no AVX-512 bit). What the translator takes
of Notepad++'s VEX code, linearly over `.text` (PROVEN, `x86_decoder_test` on a corpus of `notepad++.exe` alone): before the AVX2
work, `960 VEX instructions, 895 translated (93.2%)` (`526afbb4`); after it `958 translated (99.8%); without: shlx 2`; with BMI2
now `960 VEX instructions, 960 translated (100.0%)` (PROVEN). The 2 shlx sit in a routine that the CRT's AVX2 level reaches
without testing BMI2 (open items); its 200 instructions now all translate. Over every instruction, linearly (data decoded as code
included), `1218553 instructions translated of 1224528`; the most frequent left are bt 1023, imul 821, bts 777,
adc 567, mul 320 and rol 281 (PROVEN, `563241f6`; which forms of them are refused, and which are reachable, was not counted). Its
legacy SSE and SSE2 are 40832 of 40832 translated, its
SSE4.2 string compares too. Running Notepad++ under the translator was not done: it needs the PE loaded with its imports by the
port (NOT verified).

## Open items

- **cpuid must agree with Windows:** native/nt's `IsProcessorFeaturePresent` (PF_SSE3_INSTRUCTIONS_AVAILABLE,
  PF_SSSE3_INSTRUCTIONS_AVAILABLE, PF_SSE4_1_INSTRUCTIONS_AVAILABLE, PF_SSE4_2_INSTRUCTIONS_AVAILABLE, PF_AVX_INSTRUCTIONS_AVAILABLE,
  ...) and KUSER_SHARED_DATA's ProcessorFeatures and XState (EnabledFeatures 7, the YMM area) should report what cpuid now reports.
  Not done here (native/nt is out of this branch's scope): an item for a design review.
- **The MMX forms of SSSE3** (under cpuid) end their block while SSSE3 is reported; MMX state is the open item.
- **BMI2 next to AVX2: closed.** Notepad++'s 2 shlx are in one function (0x1404377b0 to 0x140437b61, `.pdata`), a scalar double
  routine in VEX, reached only through a dispatcher (0x140436f70) whose level the CRT sets from FMA, OSXSAVE, AVX, xgetbv and AVX2
  alone, not BMI2 (PROVEN by reading that disassembly). With BMI2 translated, the routine's 200 instructions all translate (PROVEN,
  `x86_decoder_test` on that range: `200 instructions translated of 200`), and Notepad++'s VEX code is `960 VEX instructions, 960
  translated (100.0%)`.
- **The 16-bit tzcnt and lzcnt** are not translated. Notepad++ has 17 tzcnt and lzcnt, all 32-bit, all translated (PROVEN,
  llvm-objdump and `x86_decoder_test`).
- **A gather that faults after some elements** is not tested (see Tests); nor are 32-bit addresses or fs/gs with VSIB, nor a
  test that would have caught the 128-bit vpgatherdq count (index bits beyond the elements pointing at an unmapped page).
- **xrstor** now reads everything before writing the state; whether x86 leaves the state unchanged when the YMM area faults was
  not checked (NOT verified).
- **xsave and xrstor** are not compared with an x86 CPU; xsaveopt, xsavec, xsaves, fxsave and fxrstor are not translated; CONTEXT
  has no YMM part yet.
- **More NaN cases** for VEX (3 lanes with an SNaN and a QNaN) and packed FMA3 edges (ps, pd).
- **Addressing** of 32-byte operands beyond `[r15 + displacement]` (see Tests).
- **Not translated:** F16C (0 in the corpus), BMI1, BMI2, LZCNT, vldmxcsr's reserved bits (as ldmxcsr), legacy maskmovdqu.
- **The x86-64 baseline bits** include MMX, FXSR and CX8, whose instructions are not translated yet.
- **xgetbv's two faults** share `FaultKind::instruction` (see cpuid and xgetbv); so do xsave's #GP and #UD.
- **Alignment:** vmovaps and the other aligned forms, and legacy 16-byte operands, do not raise #GP on a misaligned address.
- **New A64 encodings** (fmla, fmls, fneg, the four-operand FMA forms, tbl, the compares against zero) are proven by the oracle, not
  by `a64_emitter_test`.
- **The Linux build** was not run (NOT verified).
