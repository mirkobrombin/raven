# Integer instructions the translator refuses in real code

Which instructions `translates()` refuses in code that can run, grouped by mnemonic and operand form, and how many functions
contain each group: the code paths each gap blocks, and the groups translated since. Labels are those of
[translation.md](translation.md): **PROVEN** (a command produced the result), **INFERRED** (derived; the missing check is named),
**NOT verified**. The census is of the translator at `130f2bb8` (translator_version 23); [What is translated now](#what-is-translated-now)
is at `fabee6af` (translator_version 31). MacBook Air M5, macOS 27.2.1, llvm-mingw 20261104.

## How it is counted

`docs/macos/integer-data/integer_reach.py READOBJ DECODER_TEST CORPUS OUT IMAGE...`:

- **The instructions** are `x86_corpus.py`'s corpus of each image: llvm-objdump's text of its code sections, the decoder's
  reference corpus.
- **Reachable** is as in `x87_reach.py` and `avx_reach.py`: from the `.pdata` function starts, the entry point and the exports,
  through direct branches, direct calls and fall-through, stopping at ret, jmp, int3, ud2 and hlt. Indirect jumps and calls are not
  followed, so it is a lower bound of what can run, and code reached only through function pointers or jump tables outside `.pdata`
  counts as unreachable (INFERRED from the walk; how much that is was not measured).
- **Refused** is what `translates()` says no to: `x86_decoder_test` with `RAVEN_REFUSED` set lists every such instruction
  (`3db8cd36`). The script runs it on the whole corpus (linear) and on the reachable instructions alone.
- **The form** is the mnemonic with its prefixes (lock, rep) and the kind and size of each operand, from LLVM's text: `r32`, `m64`,
  `imm`, `cl`, a segment override. LLVM prints the implicit `es:[rdi]` of the string instructions, so `es:` there is not an
  override.
- **A function** runs from a `.pdata` start, a direct call target, the entry point or an export to the next one: leaf functions have
  no `.pdata` entry, so call targets count too. Funclets and tail-merged code may split or join functions: the function counts
  are approximate (INFERRED).

## Linear against reachable

PROVEN (the script on the corpus of these images):

| Image | Instructions (linear) | Reachable | Refused, linear | Refused, reachable |
|---|---|---|---|---|
| notepad++.exe | 1228590 | 992817 | 5975 | 3675 |
| GUP.exe | 282915 | 237994 | 1402 | 858 |
| NppConverter.dll | 35972 | 32642 | 228 | 203 |
| NppExport.dll | 24750 | 22656 | 128 | 124 |
| mimeTools.dll | 22231 | 20014 | 83 | 79 |
| nppPluginList.dll | 15306 | 13634 | 94 | 69 |
| **all six** | | | **7910** | **5008, in 1772 functions** |

Of Notepad++'s 5975 linear refusals, 3675 are reachable and 2300 are not: data decoded as code, padding, or code reached only
indirectly (which of the three was not split). `in` 109, `out` 97, `lodsd` 103 and `movabs` 73 are refused linearly and none of
them reachably (PROVEN); that they are data decoded as code is INFERRED (user-mode code has no use for port I/O). The linear sweep
overstates Notepad++'s gaps by more than a third (2300 of 5975).

## The gaps, ranked by the functions they block

Notepad++, GUP and the four plugins together (PROVEN: the counts; the size is an estimate):

| # | Group | Instructions | Functions | Size |
|---|---|---|---|---|
| 1 | `imul r64` (one operand, RDX:RAX) | 751 | 342 | medium: a 128-bit product (smulh), CF and OF |
| 2 | `bts r32, imm` | 546 | 211 | medium: bit test and set, CF from the bit |
| 3 | `bt r32, imm` | 489 | 207 | medium: CF from one bit, other flags undefined |
| 4 | `bt r64, r64` | 416 | 197 | medium: the bit index modulo 64 |
| 5 | `mul r64` (RDX:RAX) | 193 | 108 | medium: umulh, CF and OF |
| 6 | `lock inc m32` | 140 | 83 | medium: an atomic add that keeps the result for SF, ZF, OF (CF kept) |
| 7 | `bt r32, r32` | 134 | 76 | medium, as 4 |
| 8 | `mul r32` (EDX:EAX) | 177 | 71 | medium, as 5 |
| 9 | `btr r32, imm` | 168 | 71 | medium, as 2 |
| 10 | `bt r64, imm` | 92 | 61 | medium, as 3 |
| 11 | `imul r32` (one operand) | 114 | 57 | medium, as 1 |
| 12 | `ror r64, cl` | 106 | 57 | trivial: the shifts by cl extended, CF and OF of a rotate |
| 13 | `div r64` | 79 | 57 | large: #DE on a zero divisor or a quotient that overflows, which needs exception delivery |
| 14 | `rep stosw es:m16, r16` | 66 | 53 | large: a rep string op (count, direction flag, a fault part way) |
| 15 | `bts r32, r32` | 291 | 49 | medium, as 2 |

Then, with fewer functions each: `int 0x29` 40 in 40 (`__fastfail`, every one `int 0x29`: large, a system call into the kernel's
fail-fast path), `bsf r32, r32` 54 in 38 and `bsr` 33 in 19 (medium: ZF and the destination kept for a zero source), `lock or m32,
imm` 48 in 32 and `lock and m32, imm` 29 in 24 (medium, as 6), `div r32` 34 in 29, `idiv r64` 50 in 28 and `idiv r32` 19 in 15
(large, as 13), the 8- and 16-bit `sbb` and the 8- and 16-bit shifts by cl (trivial: the 32- and 64-bit forms extended), `rol` and
`ror r32, imm` (179 `ror` in only 6 functions: hashing code), the memory bit-string forms `bts m32, imm` 44 in 12 and `btr m64,
imm` 12 in 6 (medium; a register bit offset into memory, `bts m32, r32` 7 in 7, can address far beyond the operand: medium too),
and the other `rep stos` and `rep movs` (large). The full list is the script's output.

What this says (INFERRED from the table):

- **Five groups block most paths:** one-operand `imul` and `mul` (rows 1, 5, 8, 11: 578 function entries) and the register `bt`
  family (rows 2-4, 7, 9, 10, 15: 872). All are register-only with simple flags; none needs a new mechanism.
- **`lock` RMW forms beyond xadd and cmpxchg** (inc, dec, or, and, add on memory) are the next group, and the only atomic one.
- **The large items are few and localized:** division (#DE), the rep string ops and `int 0x29` need exception or system support,
  in tens of functions each.

## A second corpus: the llvm-mingw runtime

`libc++.dll`, `libomp.dll`, `libunwind.dll` and `libwinpthread-1.dll` (PROVEN, the same script): 1494 refused linearly, 1307
reachable, in 587 functions. Ranked by functions: `lock inc m32` 195 in 159, `pause` 93 in 79 (trivial: a hint, yield or nothing),
`div r32` 169 in 67, `lock dec m32` 78 in 59, `mul r64` 127 in 55, `div r64` 105 in 41, `bsr r64, r64` 56 in 41, `lock or m32, imm`
46 in 27, `div m32` 31 in 24, `bt r32, r32` 32 in 22, `idiv r32` 44 in 21, `imul r64` 25 in 18; and `xbegin` 22 in 9 in libomp
(RTM: reached only when cpuid reports RTM, which it does not; INFERRED from libomp's lock selection, its source not read here).
So the runtime adds `pause` and weighs the `lock` forms and division more than Notepad++ does.

## What is translated now

Groups 1 to 3 of the ranked list, each a failing oracle family first and then the translation:

| Group | Test commit | Feature commit | translator_version |
|---|---|---|---|
| 1. one-operand `mul`, `imul`, `div`, `idiv` (8 to 64 bits, register and memory), and `int 0x29` | `98ec8b75` | `b4390745` | 24 |
| 2. `bt`, `bts`, `btr`, `btc` (16 to 64 bits, register and imm8 index, register and memory, `lock`) | `418f5f5b` | `d2cf77f7` | 25 |
| 3. `lock add`, `sub`, `and`, `or`, `xor`, `inc`, `dec`, `not`, `neg` on memory (8 to 64 bits) | `03a2bc4f` | `e71538cf` | 26 |
| 4. `rol`, `ror`, `rcl`, `rcr` (imm8, by one, by CL), `shl`, `shr`, `sar` by CL at 8 and 16 bits, `adc` and `sbb` at 8 and 16 bits, `lock adc` and `lock sbb`; locked forms across 16 bytes | `979d9cdf` | `6d7009e8` | 27 |
| 5. `bsf`, `bsr` (16 to 64 bits), `pause`, `stos` and `movs` with and without `rep` (8 to 64 bits), `cld`, `std`, `pushfq`, `popfq` | `17e6d18d` | `b24035e2` | 28 |

How each works. The results are PROVEN by the oracle families and fault cases below; how the code gets them is INFERRED from reading
it, and claims about the ISA are marked:

- **Multiply:** `mul` and `imul` with one operand write AX (8 bits), DX:AX, EDX:EAX (both zero-extended) or RDX:RAX, reading the
  source first, as it may be RAX, RDX or AH. CF and OF are set when the high half is not zero (`mul`) or not the sign of the low half
  (`imul`); SF, ZF, AF and PF are undefined and not compared.
- **Divide:** inline when the dividend fits the host's divide (RDX zero for `div`, the sign of RAX for `idiv`, at 64 bits; always at
  8 to 32 bits, in a 64- or 32-bit register) and the divisor is neither 0 nor, for 64-bit `idiv`, -1; the quotient is checked to fit
  the width. Anything else leaves with `ExitReason::cpuid` at the instruction, and the runtime divides in 128 bits (`divide.cpp`) or
  stops with `ExitReason::fault` and `FaultKind::divide_error`, rip at the `div`, nothing changed. Translated code never computes a
  quotient Arm's `udiv` or `sdiv` would give for a #DE input (0 for a zero divisor, the minimum for minimum by -1), and the host never
  traps (PROVEN by the fault cases). The flags are undefined and left as they were, and div and idiv count as readers of every flag
  in the translator's liveness pass (`0df64172`), so a #DE shows those of the instruction before even when a later instruction of the
  block redefines them. Until `0df64172` it did not: in `cmp rax, rbx; div rcx; cmp rax, rax` the first cmp's flags were never
  computed and the #DE showed the input flags, 0x51 instead of 0x95 (PROVEN by the fault case added in `8b5d518a`, which failed
  in process in both orderings and in the vCPU before the fix).
- **`int 0x29`** (`__fastfail`) stops with `FaultKind::fast_fail`, rip at the `int`, the code in ECX. Other `int` vectors stay
  refused. `int3` (`ExitReason::breakpoint`) and `ud2` (`FaultKind::instruction`) were already translated; the fault cases check both.
- **Bit test:** CF is the bit, set alone (`rmif`), so ZF stays: the SDM leaves ZF unaffected, and the EPYC 7763 does too (PROVEN, the
  bit test family compares ZF with random input flags). OF, SF, AF and PF are undefined. On a register both index forms are taken
  modulo the width. On memory an imm8 index is too, and a register index is signed and addresses a bit string: the byte at the
  operand's address plus the index shifted right by 3 (arithmetic), bit index modulo 8. That byte holds the same bit as the word x86
  reads (INFERRED: the bit string is little-endian); `lock` makes it an LSE byte atomic (`ldsetalb`, `ldclralb`, `ldeoralb`). The
  results with indices far beyond the operand and negative, at every width, are PROVEN by the bit test family, but only in the vCPU:
  in process an address wrong in bits 56 to 63 alone still reaches the right byte, as macOS runs user code with top-byte ignore, and
  the test is blind to it (PROVEN: with the `asr` of the bit-string address made an `lsr`, `translate_x86_semantics` passes all 1200
  bit test cases, while `hvf_integer` fails 163 cases, for example #12488 `bt word ptr [r15 + rax], r9w` with a data abort at
  FAR_EL1 0x2000004000410027; since `6badd93b` a guest exception fails its case, with its text, and the run goes on).
- **Locked RMW:** one LSE atomic with acquire and release, as `xadd` and `cmpxchg` already were (`ldaddal`, `ldclral` with the
  complement for `and`, `ldsetal`, `ldeoral`, `ldeoral` with all ones for `not`), then the flags from the old value and the operand
  as without `lock`; `neg` is a `casal` loop. `inc` and `dec` keep CF (PROVEN). TSO is kept as it is for `xadd`, the atomics having
  acquire and release semantics (INFERRED from the Arm ARM; no litmus test runs on the new forms).

- **Rotates:** CF and OF only (OF as for a count of 1, which alone defines it); SF, ZF, AF and PF stay. A masked count of 0 changes
  nothing; `rol` and `ror` at 8 and 16 bits set CF even when the count is a multiple of the width (PROVEN, the rotate family). 8- and
  16-bit values are replicated across 32 bits and rotated with `rorv` or `extr`; `rcl` and `rcr` shift the value and CF in a 64-bit
  register, by the masked count modulo 9 and 17 at 8 and 16 bits (INFERRED from the code; the results PROVEN). A 32-bit register
  rotated by 0 still has its upper half cleared, as the shifts do, and as Rosetta and the EPYC 9V45 do (PROVEN).
- **Narrow shifts by CL:** CL modulo 32 may pass the width; the value shifts in a 64-bit register. Silicon defines CF for `sar` past
  the width (the sign) and the tests compare it; for `shl` and `shr` past the width CF is undefined (PROVEN agreement on the sar
  cases, the SDM for the rest).
- **Narrow `adc` and `sbb`:** computed in 32 bits with the carry, CF bit 8 or 16 of the result, OF from the signs. `lock adc` and
  `lock sbb` are a `casal` loop with NZCV kept across it.
- **Split locks:** an LSE atomic whose operand crosses a 16-byte boundary raises an alignment fault on the M5 (PROVEN: SIGBUS in
  process for `ldaddal` at offsets 13 to 15 of 16 for 4 bytes, 9 to 15 for 8, 15 for 2; inside 16 bytes unaligned ones work). Before
  `6d7009e8` such a `lock` operand crashed the in-process test (SIGBUS, exit 138) and stopped the vCPU test with an alignment data
  abort (ESR_EL1 0x92000021). Every locked form (`xchg`, `cmpxchg`, `xadd` and the locked RMW) checks, without touching NZCV,
  whether its operand crosses 16 bytes. From `6d7009e8` it then took a plain load and store between full barriers, which lost
  updates against another vCPU (PROVEN, `e6043284`: two vCPUs doing 20000 `lock inc dword` each at offset 14 lost 7952 of 40000, and
  every width and `xadd` and `cmpxchg` lost some; the `xchg` spinlock hung). Since `849e2ff6` it leaves to the runtime at the
  instruction (`ExitReason::cpuid`), which runs it (`locked.cpp`, flags as x86) while every other vCPU of the VM is stopped
  (`Vcpu::exclusive`: `hv_vcpus_exit` on each vCPU in the guest, and a gate in `Vcpu::enter` that keeps them out until it is done).
  `hvf_locked` then loses no update at 2, 4 and 8 bytes, for `xadd`, a `cmpxchg` increment loop, an `xchg` spinlock, and an aligned
  8-byte `lock add` on bytes 8 to 15 from one vCPU against a split 4-byte `lock inc` on bytes 14 to 17 from the other (PROVEN). In
  process the runtime runs its first thread alone and cannot add others without a runner (runtime.h), so the runtime runs the
  instruction with nothing to stop (INFERRED from that restriction; nothing in process can run two guest threads to test it). Host
  code of other vCPUs (the runtime's own accesses for x87, xsave or a division) is not stopped, which x86 does not order with a
  split lock either (INFERRED). Cost (measured once, rough: two vCPUs contending, setup included): 2.7 to 6.8 us per split operation
  against 0.02 us for the plain fallback before. The unaligned locked family checks both kinds, 64-byte aligned buffer, in process and
  in a vCPU (PROVEN). Exposed to the alignment fault
  before `6d7009e8`, and covered since: `xchg` with memory, `lock cmpxchg`, `lock xadd`, `lock add`, `sub`, `and`, `or`, `xor`,
  `inc`, `dec`, `not` and `neg` (`neg`'s `casal` loop) at 2, 4 and 8 bytes. Never exposed: the 1-byte forms, and every bit test, whose
  memory forms access one byte. Plain loads and stores are not exposed either (LDAPR and STLR take an out-of-line path when
  unaligned, plain ones fault on no alignment).

- **Bit scan:** a zero source sets ZF and leaves the destination whole, a 32-bit one's upper half included: the EPYC 9V45 does that,
  as Rosetta does (PROVEN, the bit scan family compares the whole register, zero sources among the cases). Else ZF clear and the
  index; CF, OF, SF, AF and PF undefined (156 cases differ there between the two, not compared).
- **`pause`** is `yield`.
- **DF** is now `CpuState::direction`, 0 at the start as Windows starts a thread. `cld` and `std` store it, `pushfq` builds RFLAGS
  from the lazy flags, DF and bits 1 and 9 (IF), `popfq` sets the six status flags and DF. Since `fabee6af` AC (bit 18) and ID (bit 21) are kept
  in `CpuState::rflags_system` and come back through `pushfq`, as the CPUID-availability test needs (PROVEN by the rflags family,
  which toggles each on the EPYC 7763 and Rosetta; before the fix all 24 cases failed, RBX without the bit). AC changes nothing for
  Windows user code, which runs without CR0.AM (INFERRED). TF set by `popfq` is ignored: x86 raises a single-step exception after the
  next instruction, the translator does not (NOT done, by decision). IF and the other system bits are not kept. Before this `cld`, `std`, `pushfq` and
  `popfq` were refused (not translated at all). `pushfq` and `popfq` agree with Rosetta and the EPYC in every case of the strings
  family (PROVEN, RAX holds what `pushfq` stored). The 16-bit `pushf` and `popf` (66) stay refused.
- **`rep stos` and `rep movs`:** one element per pass, forward or backward by DF, RDI, RSI and RCX updated after each, RCX 0 doing
  nothing. A fault part way leaves them as x86 does (PROVEN in a vCPU, `hvf_string_fault`, a new test target: `rep stosb` with RCX
  100 five bytes before an unmapped page faults with RCX 95 and RDI at the page end, the five bytes written; also `rep stosq`, a
  `rep movsb` whose source faults, backward `rep stosw` and `rep movsd`, and RCX 0 on an unmapped address, which returns). Overlapping
  `movs` forward and backward agree with silicon (PROVEN, strings family). The elements are plain accesses between barriers, as a
  fast string operation allows (INFERRED from the SDM's ordering rules; no litmus test). A long `rep` cannot be stopped part way by
  the runtime (NOT handled). Address-size (67) and segment-override forms and `repne` stay refused.

### Tests

The integer oracle (`x86_semantics.py`) now has families and the AVX checker's fingerprint scheme (`x86_semantics_check.h`,
`73fd8bcd`): `testdata/x86_silicon.txt` has a `corpus` line per family run on x86 silicon and the overrides in the AVX file's format,
written by `docs/macos/integer-data/silicon_overrides.py`. Since `3cd7ab4b` the `generated` hash also covers each case's defined
flags and inputs (marking a flag defined makes the tests refuse the file until the family runs on silicon again: PROVEN by marking
CF defined in the bit scan family, which the test then refused), and each line has `differently=N`, the cases the runner encoded
otherwise and so did not compare, which must be 0 unless the line says `explained`; it is 0 for every family. A family without a silicon run fails. The old override (registers only)
became a case line of the `random` family. The directed families set their registers per case, edge values first (0, 1, 2, -1, the
signed minimum and maximum and their neighbours, powers of two), with random input flags, and compare only the flags x86 defines
after the case (all of them where an instruction must leave a flag alone). Every case runs in process with LDAPR/STLR and with
barriers (`translate_x86_semantics`) and in a vCPU with hardware TSO (`hvf_integer`, a new test target), with these results
(PROVEN, the three test runs at the end of this work):

| Family | Cases | Failures in process (each ordering) | Failures in a vCPU |
|---|---|---|---|
| random (the existing sequences) | 10000 | 0 | 0 |
| multiply | 1120 | 0 | 0 |
| divide (no #DE inputs: Python leaves them out) | 1303 | 0 | 0 |
| bit test | 1200 | 0 | 0 |
| locked | 864 | 0 | 0 |
| rotate | 1041 | 0 | 0 |
| shift by cl | 312 | 0 | 0 |
| carry narrow (with lock adc, lock sbb) | 464 | 0 | 0 |
| flag readers (setc, seto, cmovc, jc in the same block after bt, mul, a rotate, a narrow shift or adc) | 144 | 0 | 0 |
| unaligned locked (inside 16 bytes and across) | 496 | 0 | 0 |
| bit scan | 156 | 0 | 0 |
| strings (stos, movs, cld, std, pushfq, popfq, pause) | 272 | 0 | 0 |
| rflags (AC and ID through popfq and pushfq) | 24 | 0 | 0 |
| shift by one (narrow shl, shr, sar by 1, every top-bit and carry combination) | 144 | 0 | 0 |
| fault cases: #DE from `div` and `idiv` at each width, by zero and by overflow, a memory divisor, a #DE after a `cmp`, `int 0x29`, `int3`, `ud2` | 23 | 0 | 0 |

Before each feature commit the family failed whole: every case "did not return (exit 6)", the unsupported exit, and 21 of the 23
fault cases (all but `int3` and `ud2`) took the unsupported exit instead of the fault.

Atomicity (`hvf_locked`, a new test target, PROVEN in three runs): two vCPUs run a million iterations each of `lock inc`, `lock dec`,
`lock add` (imm8), `lock sub` (register) and `lock xadd` on one 32-bit word, and none loses an update. The control, a plain `inc` on
the same word, loses about half (996602 and 992116 of 2000000 in two runs), so the vCPUs did overlap. Since `cb80feee`:

- `lock neg` and `lock not` run 16 trials of 200000 each per vCPU on a word of 5, and every trial ends at 5. A lost negation or
  complement shows in a trial with odds of a half (an even number cancels), so a translation that loses them passes with odds near
  2^-16 (INFERRED from that model).
- `lock or` with `lock and`, `lock xor` twice, `lock bts` with `lock btr` and `lock btc` twice work on one word, a bit per vCPU:
  each sets its bit, checks it is set, clears it and checks it is clear, an `xchg` (a full barrier) before each check so that the
  check reads the word and not the vCPU's own store. No check of 4000000 fails. The control, plain `or` and `and`, fails 374 and 589
  in two runs. With `lock or` translated as a plain load, or and store (a mutation, not committed), 6130 checks of `lock or` fail
  (PROVEN). Without the barrier the control failed none: a vCPU's load is served from its own store first.
- `lock adc` and `lock sbb` of 0 with CF set (by `cmp eax, 2` with EAX 1) count up and down on two vCPUs since `4ecd5faa`, with no
  update lost. With their `casal` replaced by a plain store (a mutation, not committed) they lose 994964 and 897969 of 2000000, while
  the `hvf_locked` before `4ecd5faa` passed with that mutation (PROVEN).
- The narrow shifts' OF: with the CF xor dropped from the narrow shift by CL (a mutation), the shift by cl family fails 2 of 312
  cases and the shift by one family, added in `1433a811`, 16 of 144 (PROVEN).

### x86 silicon against Rosetta

Every case ran on Rosetta (the build) and on GitHub's x86-64 runners through `raven-macos-bench`'s `integer-oracle` workflow, which
generates the cases there, assembles them with clang as the build does and runs the oracle twice (the two runs were identical every
time). The runs: AMD EPYC 9V74 (runs 44522174773 and 44522713416), AMD EPYC 7763 (44523646082 and 44524257886, the last holding every
family then, dump sha256 `8b28a4ea...`), AMD EPYC 9V45 (44526496471, with group 4, dump sha256 `c4477a00...`). PROVEN by
`silicon_overrides.py` on the dumps; group 5 on an AMD EPYC 9V45 too (44527495718, dump sha256 `11298321...`):

- **multiply, divide, bit test, locked:** silicon and Rosetta agree in every register, every memory byte and every defined flag. They
  differ only in flags x86 leaves undefined: 1039 multiply cases and 1231 divide cases (bit test and locked none), not compared.
- **rotate, shift by cl, carry narrow, flag readers, unaligned locked:** the same: no difference in a defined result; undefined flags
  differ in 7, 278, 0, 38 and 29 cases.
- **rflags, shift by one:** the same on the EPYC 7763 (run 44532999818): ID and AC toggle on both; undefined flags differ in 0 and
  144 cases.
- These counts come from the dumps, which are artifacts of the bench runs and not in this repository; the summary of the last
  (`silicon_overrides.py` on the build's Rosetta dump and the EPYC 7763 dump, sha256
  `f672c276756d52cb1383f944525af03854da94ba467584b202ad2939010def5c`, holding every family) is
  `docs/macos/integer-data/silicon_summary.txt`.
- **Case counts:** the silicon runs count the oracle's cases only (17372 at group 5, 17540 now); the vCPU test adds the 23 fault
  cases, which never run on x86 (a #DE would kill the oracle), hence its 17395 then and 17563 now.
- **random:** the one known difference stays, the RAX a 32-bit `lock cmpxchg` leaves (case 8316), now held to the EPYC; 2609 cases
  differ only in undefined flags.

No silicon-against-Rosetta difference in a defined result was found in groups 1 to 5.

### Reachable refused instructions, before and after

PROVEN (`integer_reach.py` on the same images, rerun after each feature commit):

| Corpus | Before (`564c0960`) | After group 1 | After group 2 | After group 3 | After group 4 | After group 5 |
|---|---|---|---|---|---|---|
| notepad++.exe | 3675 | 2454 | 755 | 638 | 169 | 34 |
| GUP.exe | 858 | 686 | 305 | 238 | 68 | 15 |
| NppConverter.dll | 203 | 165 | 73 | 41 | 16 | 1 |
| nppPluginList.dll | 69 | 60 | 41 | 21 | 6 | 0 |
| mimeTools.dll | 79 | 62 | 43 | 23 | 7 | 0 |
| NppExport.dll | 124 | 98 | 55 | 35 | 18 | 3 |
| **Notepad++, GUP and plugins** | **5008 in 1772 functions** | **3525 in 1131** | **1272 in 428** | **996 in 284** | **284 in 169** | **53 in 21** |
| libc++.dll | 474 | 311 | 276 | 136 | 99 | 24 |
| libomp.dll | 730 | 342 | 309 | 147 | 135 | 42 |
| libunwind.dll | 8 | 4 | 3 | 3 | 3 | 1 |
| libwinpthread-1.dll | 95 | 78 | 74 | 1 | 1 | 1 |
| **runtime DLLs** | **1307 in 587 functions** | **735 in 423** | **662 in 379** | **287 in 178** | **238 in 165** | **68 in 32** |

Linearly, Notepad++, GUP and the plugins now have 1291 refusals (7910 before), the runtime DLLs 68 (1494 before). What is left after group 5, the whole list (PROVEN, the script):

- Notepad++, GUP and the plugins, 53 instructions in 21 functions: `shrd r64, r64, cl` 10 in 4, `bswap r32` 6 in 4, `bswap r64` 6
  in 3, `prefetchw m8` 3 in 3, `insb` 3 in 2, `vpminuq` (AVX-512, four forms) 12 in 2, `cmpsb` 2 in 2, and one each of `insd`,
  `in r32, r16`, `out imm, r32`, `cli`, `or r8, imm`, `lahf`, `sahf`, `cmc`, `retf`, `mov m16, es` and `sfence`.
- The runtime DLLs, 68 in 32: `xbegin` 22 in 9, `xabort` 5 in 5 and `xend` 5 in 5 (RTM, never reached while cpuid reports no RTM:
  INFERRED), `tpause` 6 in 6, `shrd r64, r64, cl` 11 in 3, `shrd r32, r32, cl` 10 in 3, `jecxz` 2 in 1, and one each of `bswap r32`,
  `shld r64, r64, cl`, `shld r64, r64, imm`, `shrd r64, r64, imm`, `umonitor`, `umwait` and `mfence`.

## Open items

- DF is not in the context an exception would report, nor restored by anything but `popfq` and `cld` (NOT done).
- A split lock costs microseconds and stops every vCPU; code that takes many would be slow (not measured on real software).
- A #DE or an `int 0x29` is a fault exit; delivering it to the guest as an exception is the runtime's caller's work (NOT done here).
- **Precise exception state (design item).** A #DE now shows the flags before the div, but a faulting memory access does not:
  liveness does not count a load or store as a flag reader, so flags an earlier instruction defined and a later one redefines may
  never be computed when the access faults. That predates this work and is not fixed by making every access a reader (too costly);
  it belongs with a side table from host pc to guest state, which would also give the guest rip and the flags at a fault.
- The runtime's division reads a memory divisor through `GuestMemory::read`, as xsave and the string compares do. An unmapped
  divisor faults in translated code first, since `divide` reads its source before any exit (the review confirmed it). But
  `Vm::read` throws on an operand that straddles two mappings, so a divisor across a mapping boundary that reaches the runtime's
  re-read in `divide.cpp` is a host error, not a guest fault (NOT handled).
- The division slow path costs an exit to the runtime (a dividend wider than 64 bits, as in 128-bit arithmetic, and every 64-bit
  `idiv` by -1): its cost was not measured.

- The walk does not follow indirect calls: functions reached only through vtables or callbacks outside `.pdata` are missing, and a
  reachable count with them would be higher (NOT measured).
- Whether each group is on a path a user action runs was not measured: reachable here is static.
- The linear count's 2300 unreachable refusals in Notepad++ were not split into data, padding and indirectly reached code.
