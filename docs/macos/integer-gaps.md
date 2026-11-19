# Integer instructions the translator refuses in real code

Which instructions `translates()` refuses in code that can run, grouped by mnemonic and operand form, and how many functions
contain each group: the code paths each gap blocks. Measured, not implemented. Labels are those of
[translation.md](translation.md): **PROVEN** (a command produced the result), **INFERRED** (derived; the missing check is named),
**NOT verified**. Translator at `130f2bb8` (translator_version 23), MacBook Air M5, macOS 27.2.1, llvm-mingw 20261104.

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

## Open items

- The walk does not follow indirect calls: functions reached only through vtables or callbacks outside `.pdata` are missing, and a
  reachable count with them would be higher (NOT measured).
- Whether each group is on a path a user action runs was not measured: reachable here is static.
- The linear count's 2300 unreachable refusals in Notepad++ were not split into data, padding and indirectly reached code.
