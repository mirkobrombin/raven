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
	// hlt or ud2; rip is that instruction.
	fault,
};

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
}

}
