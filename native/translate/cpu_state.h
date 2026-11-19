#pragma once

#include <cstddef>
#include <cstdint>

namespace raven::translate {

enum Gpr : uint8_t { rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15 };

// Why translated code handed control back to the runtime. The values are part of the code the translator emits.
enum class ExitReason : uint32_t {
	none,
	// rip names code with no translation in the lookup table.
	lookup_miss,
	// A direct branch to rip that the runtime may chain; exit_info is the execute address of the branch to patch.
	chain,
	// The guest returned to the address Runtime::call pushed.
	returned,
	// syscall; rip is the next instruction.
	syscall,
	// int3; rip is the int3, as Windows reports it.
	breakpoint,
	// An instruction the translator has no code for; rip is that instruction.
	unsupported,
	// A native function asked the guest to stop (ExitProcess); exit_info is its code.
	stopped,
	// hlt or ud2; rip is that instruction. Also an x87 instruction x86 would raise #MF on (an unmasked exception pending).
	fault,
	// An x87 instruction; rip is that instruction. The runtime runs it and the x87 instructions after it (x87.h).
	x87,
	// cpuid or xgetbv, or an SSE4.2 string compare; rip is that instruction. The runtime runs it (cpuid.h, string_compare.h) and goes
	// on after it.
	cpuid,
};

// What CpuState::exit_info holds after ExitReason::fault.
enum class FaultKind : uint64_t {
	// hlt or ud2 in translated code, or an xgetbv x86 raises #UD or #GP for (cpuid.h).
	instruction,
	// #MF: an x87 instruction found an unmasked exception pending; FSW and FCW in CpuState::x87 say which.
	x87,
};

// The x87 FPU, kept in CpuState only: the host's x87 core (x87.h) reads and writes it, translated code never does. The order is that
// of the FXSAVE image's first 32 bytes (with ftw the abridged tag byte, and mxcsr's place unused: CpuState::mxcsr holds it), but st
// holds the physical registers R0-R7, not ST(0)-ST(7): ST(i) is R((TOP + i) mod 8), TOP being FSW bits 13:11. Each register is the
// 64-bit significand, then sign and exponent in the low 16 bits of the second quadword.
//
// MMX, when it comes, aliases the same registers: MMn is the significand of Rn, and an MMX write sets the exponent field to 0xffff,
// TOP to 0 and every tag valid; emms clears the tags.
struct X87State {
	// Precision control 53 bits, round to nearest, every exception masked: what Windows x64 starts a program with.
	uint16_t fcw = 0x27f;
	uint16_t fsw = 0;
	// Bit n set: Rn holds a value (FXSAVE's abridged tag word).
	uint8_t ftw = 0;
	uint8_t reserved = 0;
	uint16_t fop = 0;
	uint64_t fip = 0;
	uint64_t fdp = 0;
	uint64_t unused = 0;
	alignas(16) uint64_t st[8][2] = {};
};
static_assert(sizeof(X87State) == 160, "the FXSAVE image's legacy area");

struct LookupEntry {
	uint64_t guest;
	uint64_t host;
};

constexpr unsigned lookup_bits = 12;

// The guest thread as the runtime sees it between runs of translated code. While translated code runs, the registers live in host
// registers (translator.cpp has the mapping) and only the fields below rip mean anything.
struct CpuState {
	uint64_t gpr[16] = {};
	uint64_t rip = 0;
	// NZCV as the host keeps it while translated code runs: N, Z and V are SF, ZF and OF, and C is the inverse of CF, as after SUBS.
	uint64_t nzcv = 0;
	// The last result and the xor of the last two operands of a flag-setting operation: PF is the parity of the low byte of the first,
	// AF bit 4 of the xor of both.
	uint64_t parity_source = 0;
	uint64_t adjust_source = 0;
	uint64_t gs_base = 0;
	uint64_t fs_base = 0;
	uint64_t exit_info = 0;
	// Read by translated code, so that code holds no absolute host address and moves as a blob (ahead-of-time cache).
	uint64_t exit_trampoline = 0;
	LookupEntry *lookup_table = nullptr;
	uint32_t exit_reason = 0;
	uint32_t stop_requested = 0;
	uint64_t host_frame = 0;
	// The code a native function stopped the guest with.
	uint64_t stop_code = 0;
	// The host's FPCR while translated code runs: the enter trampoline keeps it here and the exit trampoline and native thunks put it
	// back.
	uint64_t host_fpcr = 0;
	// MXCSR between runs. While translated code runs, its control bits are FPCR and its exception flags FPSR (translator.cpp maps
	// them); the exit trampoline folds FPSR back. DE stays as it was: Arm reports a denormal input only when it flushes one.
	uint32_t mxcsr = 0x1f80;
	uint32_t reserved = 0;
	// XMM0-XMM15 between runs, low quadword first. While translated code runs, XMMn is vn; the trampolines store and load all sixteen
	// on every exit and entry, and native thunks store them before the native function runs.
	alignas(16) uint64_t xmm[16][2] = {};
	// The upper halves of YMM0-YMM15 (bits 255:128), low quadword first. They stay here while translated code runs too: an
	// instruction on a YMM register loads and stores them around its NEON pair, a VEX.128 one clears them, a legacy SSE one leaves
	// them alone, so the trampolines and native thunks need not move them.
	alignas(16) uint64_t ymm_high[16][2] = {};
	X87State x87;
};

namespace state_offset {
constexpr size_t gpr = offsetof(CpuState, gpr);
constexpr size_t rip = offsetof(CpuState, rip);
constexpr size_t nzcv = offsetof(CpuState, nzcv);
constexpr size_t parity_source = offsetof(CpuState, parity_source);
constexpr size_t adjust_source = offsetof(CpuState, adjust_source);
constexpr size_t gs_base = offsetof(CpuState, gs_base);
constexpr size_t fs_base = offsetof(CpuState, fs_base);
constexpr size_t exit_info = offsetof(CpuState, exit_info);
constexpr size_t exit_trampoline = offsetof(CpuState, exit_trampoline);
constexpr size_t lookup_table = offsetof(CpuState, lookup_table);
constexpr size_t exit_reason = offsetof(CpuState, exit_reason);
constexpr size_t stop_requested = offsetof(CpuState, stop_requested);
constexpr size_t host_frame = offsetof(CpuState, host_frame);
constexpr size_t host_fpcr = offsetof(CpuState, host_fpcr);
constexpr size_t mxcsr = offsetof(CpuState, mxcsr);
constexpr size_t xmm = offsetof(CpuState, xmm);
static_assert(xmm % 16 == 0 && xmm + 224 <= 1008, "ldp and stp of q registers reach the XMM array");
constexpr size_t ymm_high = offsetof(CpuState, ymm_high);
static_assert(ymm_high % 16 == 0 && ymm_high + 240 <= 65520, "ldr and str of a q register reach every upper half");
}

}
