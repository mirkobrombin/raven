# Integer instructions the translator refuses in real code

Which instructions `translates()` refuses in code that can run, grouped by mnemonic and operand form, and how many functions
contain each group: the code paths each gap blocks, and the groups translated since. Labels are those of
[translation.md](translation.md): **PROVEN** (a command produced the result), **INFERRED** (derived; the missing check is named),
**NOT verified**. The census is of the translator at `130f2bb8` (translator_version 23); [What is translated now](#what-is-translated-now)
is at `e71538cf` (translator_version 26). MacBook Air M5, macOS 27.2.1, llvm-mingw 20261104.

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
  traps (PROVEN by the fault cases). The flags are undefined and left as they were, so a #DE shows those of the instruction before
  (PROVEN: the fault cases compare them).
- **`int 0x29`** (`__fastfail`) stops with `FaultKind::fast_fail`, rip at the `int`, the code in ECX. Other `int` vectors stay
  refused. `int3` (`ExitReason::breakpoint`) and `ud2` (`FaultKind::instruction`) were already translated; the fault cases check both.
- **Bit test:** CF is the bit, set alone (`rmif`), so ZF stays: the SDM leaves ZF unaffected, and the EPYC 7763 does too (PROVEN, the
  bit test family compares ZF with random input flags). OF, SF, AF and PF are undefined. On a register both index forms are taken
  modulo the width. On memory an imm8 index is too, and a register index is signed and addresses a bit string: the byte at the
  operand's address plus the index shifted right by 3 (arithmetic), bit index modulo 8. That byte holds the same bit as the word x86
  reads (INFERRED: the bit string is little-endian); `lock` makes it an LSE byte atomic (`ldsetalb`, `ldclralb`, `ldeoralb`). The
  results with indices far beyond the operand and negative, at every width, are PROVEN by the bit test family.
- **Locked RMW:** one LSE atomic with acquire and release, as `xadd` and `cmpxchg` already were (`ldaddal`, `ldclral` with the
  complement for `and`, `ldsetal`, `ldeoral`, `ldeoral` with all ones for `not`), then the flags from the old value and the operand
  as without `lock`; `neg` is a `casal` loop. `inc` and `dec` keep CF (PROVEN). TSO is kept as it is for `xadd`, the atomics having
  acquire and release semantics (INFERRED from the Arm ARM; no litmus test runs on the new forms).

### Tests

The integer oracle (`x86_semantics.py`) now has families and the AVX checker's fingerprint scheme (`x86_semantics_check.h`,
`73fd8bcd`): `testdata/x86_silicon.txt` has a `corpus` line per family run on x86 silicon and the overrides in the AVX file's format,
written by `docs/macos/integer-data/silicon_overrides.py`; a family without a silicon run fails. The old override (registers only)
became a case line of the `random` family. The directed families set their registers per case, edge values first (0, 1, 2, -1, the
signed minimum and maximum and their neighbours, powers of two), with random input flags, and compare only the flags x86 defines
after the case (all of them where an instruction must leave a flag alone). Every case runs in process with LDAPR/STLR and with
barriers (`translate_x86_semantics`) and in a vCPU with hardware TSO (`hvf_integer`, a new test target), with these results
(PROVEN, the three test runs at `e71538cf`):

| Family | Cases | Failures in process (each ordering) | Failures in a vCPU |
|---|---|---|---|
| random (the existing sequences) | 10000 | 0 | 0 |
| multiply | 1120 | 0 | 0 |
| divide (no #DE inputs: Python leaves them out) | 1303 | 0 | 0 |
| bit test | 1200 | 0 | 0 |
| locked | 864 | 0 | 0 |
| fault cases: #DE from `div` and `idiv` at each width, by zero and by overflow, a memory divisor, a #DE after a `cmp`, `int 0x29`, `int3`, `ud2` | 23 | 0 | 0 |

Before each feature commit the family failed whole: every case "did not return (exit 6)", the unsupported exit, and 21 of the 23
fault cases (all but `int3` and `ud2`) took the unsupported exit instead of the fault.

Atomicity (`hvf_locked`, a new test target, PROVEN in three runs): two vCPUs run a million iterations each of `lock inc`, `lock dec`,
`lock add` (imm8), `lock sub` (register) and `lock xadd` on one 32-bit word, and none loses an update; `lock neg` two million times
leaves a word of 5 at 5. The control, a plain `inc` on the same word, loses about half (996602 and 992116 of 2000000 in two runs),
so the vCPUs did overlap. Whether the `lock neg` check would catch a non-atomic `neg` is NOT verified (an even number of lost
negations cancels).

### x86 silicon against Rosetta

Every case ran on Rosetta (the build) and on GitHub's x86-64 runners through `raven-macos-bench`'s `integer-oracle` workflow, which
generates the cases there, assembles them with clang as the build does and runs the oracle twice (the two runs were identical every
time). The runs: AMD EPYC 9V74 (runs 44522174773 and 44522713416), AMD EPYC 7763 (44523646082 and 44524257886, the last holding every
family, dump sha256 `8b28a4ea...`). PROVEN by `silicon_overrides.py` on the dumps:

- **multiply, divide, bit test, locked:** silicon and Rosetta agree in every register, every memory byte and every defined flag. They
  differ only in flags x86 leaves undefined: 1039 multiply cases and 1231 divide cases (bit test and locked none), not compared.
- **random:** the one known difference stays, the RAX a 32-bit `lock cmpxchg` leaves (case 8316), now held to the EPYC; 2609 cases
  differ only in undefined flags.

No silicon-against-Rosetta difference in a defined result was found in groups 1 to 3.

### Reachable refused instructions, before and after

PROVEN (`integer_reach.py` on the same images, rerun after each feature commit):

| Corpus | Before (`564c0960`) | After group 1 | After group 2 | After group 3 |
|---|---|---|---|---|
| notepad++.exe | 3675 | 2454 | 755 | 638 |
| GUP.exe | 858 | 686 | 305 | 238 |
| NppConverter.dll | 203 | 165 | 73 | 41 |
| nppPluginList.dll | 69 | 60 | 41 | 21 |
| mimeTools.dll | 79 | 62 | 43 | 23 |
| NppExport.dll | 124 | 98 | 55 | 35 |
| **Notepad++, GUP and plugins** | **5008 in 1772 functions** | **3525 in 1131** | **1272 in 428** | **996 in 284** |
| libc++.dll | 474 | 311 | 276 | 136 |
| libomp.dll | 730 | 342 | 309 | 147 |
| libunwind.dll | 8 | 4 | 3 | 3 |
| libwinpthread-1.dll | 95 | 78 | 74 | 1 |
| **runtime DLLs** | **1307 in 587 functions** | **735 in 423** | **662 in 379** | **287 in 178** |

Linearly, Notepad++, GUP and the plugins now have 3488 refusals (7910 before), the runtime DLLs 323 (1494 before). What blocks the most
functions now: in Notepad++ and its companions `ror r64, cl` 106 in 57, `rep stosw` 66 in 53, `bsf r32, r32` 54 in 38 and `bsr r32,
r32` 33 in 19; in the runtime DLLs `pause` 93 in 79 and `bsr r64, r64` 56 in 41. These are groups 4 and 5, not done.

## Open items

- **Groups 4 and 5 are not done:** rotates (`rol`, `ror` by imm and by cl), shifts by cl at 8 and 16 bits, `rcl`/`rcr`, `adc` and
  `sbb` at 8 and 16 bits; `bsf`/`bsr`, `pause`, `rep stos` and `rep movs` (with DF, which CpuState does not hold yet).
- `lock adc` and `lock sbb` stay refused (none in the census).
- Locked operands and bit-test memory forms are tested aligned only. LSE atomics on an address that crosses 16 bytes fault on Arm
  (FEAT_LSE2 covers only accesses within 16 bytes), where x86 takes a split lock: NOT verified on the M5.
- A #DE or an `int 0x29` is a fault exit; delivering it to the guest as an exception is the runtime's caller's work (NOT done here).
- The new forms are tested as single instructions before `ret`: a reader of their CF later in the same block (jcc, setcc, cmov,
  adc, sbb after `bt` or `mul`) is covered only by the existing random sequences, which do not contain them (NOT verified).
- Atomicity is counted for `lock inc`, `dec`, `add`, `sub` and `xadd` only; that `lock and`, `or`, `xor`, `not` and the locked bit
  tests are atomic is INFERRED from the LSE instructions, which the oracle checks single-threaded.
- The runtime's division reads a memory divisor through `GuestMemory::read`, as xsave and the string compares do: a fault on that
  read is a host error, not a guest fault (NOT handled).
- The division slow path costs an exit to the runtime (a dividend wider than 64 bits, as in 128-bit arithmetic, and every 64-bit
  `idiv` by -1): its cost was not measured.

- The walk does not follow indirect calls: functions reached only through vtables or callbacks outside `.pdata` are missing, and a
  reachable count with them would be higher (NOT measured).
- Whether each group is on a path a user action runs was not measured: reachable here is static.
- The linear count's 2300 unreachable refusals in Notepad++ were not split into data, padding and indirectly reached code.
