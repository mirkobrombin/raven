#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace raven::translate {

// How x86's total store order is kept on a host that orders memory more weakly.
enum class MemoryOrdering : uint8_t {
	// The CPU runs in TSO mode (a vCPU with ACTLR_EL1.TSOEN): plain loads and stores.
	hardware,
	// Loads are LDAPR and stores STLR (FEAT_LRCPC): load-load, load-store and store-store order kept, store-load free, as TSO allows.
	rcpc,
	// Plain accesses with DMB ISHLD after each load and DMB ISHST before each store, for hosts without FEAT_LRCPC.
	barriers,
};

struct TranslatorOptions {
	MemoryOrdering ordering = MemoryOrdering::rcpc;
	// Whether accesses through RSP are ordered too. Another thread may read a stack it was given a pointer into, so the default keeps
	// them ordered; false is the faster mode Rosetta-like translators offer per application.
	bool order_stack = true;
	uint32_t max_instructions = 64;
};

struct TranslationStats {
	uint64_t blocks = 0;
	uint64_t guest_instructions = 0;
	uint64_t host_words = 0;
	uint64_t barriers = 0;
	uint64_t ordered_loads = 0;
	uint64_t ordered_stores = 0;
	uint64_t plain_accesses = 0;
	uint64_t alignment_checks = 0;
};

// A branch the runtime may chain: word is the index of the b that leaves the block for target.
struct BlockExit {
	uint32_t word;
	uint64_t target;
};

struct TranslatedBlock {
	uint64_t guest_start = 0;
	uint32_t guest_size = 0;
	uint32_t instructions = 0;
	std::vector<uint32_t> code;
	std::vector<BlockExit> exits;
};

struct Instruction;
// Whether the translator has code for the instruction; a block ends before one it has not.
bool translates(const Instruction &instruction);

// Translates the x64 basic block at address, whose bytes start code. The code reaches the runtime only through CpuState (x23) and the
// exit protocol, so it can be copied anywhere: into the code cache now, or into an ahead-of-time cache file.
TranslatedBlock translate_block(std::span<const uint8_t> code, uint64_t address, const TranslatorOptions &options,
	TranslationStats &stats);

// The fixed code the runtime places once in the code cache.
// enter(CpuState *state, const void *code): saves the host's callee-saved registers, loads the guest's and branches to code.
// It also keeps the host's FPCR, sets FPCR and FPSR from CpuState::mxcsr and loads XMM0-XMM15 into v0-v15.
std::vector<uint32_t> enter_trampoline();
// Reached with the exit reason in w9: stores the guest's registers and returns the reason from enter.
// It also stores v0-v15 and FPSR's exception flags in CpuState and gives the host its FPCR back.
std::vector<uint32_t> exit_trampoline();
// Code a guest call reaches when it calls native function: the x64 arguments go to the AAPCS64 argument registers, the result to RAX,
// and the guest's return address is taken as a ret would take it. Microsoft's ARM64EC names this direction an entry thunk.
std::vector<uint32_t> native_thunk(uint64_t function, unsigned integer_arguments);
// ARM64EC transitions, which translated code and native code reach without leaving the guest. x64 code calling native function goes
// to its entry thunk as Windows enters it; x64 code returning to native code at address goes on there.
std::vector<uint32_t> arm64ec_entry(uint64_t function, uint64_t thunk);
std::vector<uint32_t> arm64ec_resume(uint64_t address);
// The routines native code reaches through the __os_arm64x_* slots. dispatch_ret: an entry thunk returns to the x64 address in x30
// with RAX in x8. dispatch_call: an exit thunk calls the x64 function in x9 with the native return address in x30. check_icall: x11
// is an indirect call's target and x10 its exit thunk; x11 is left alone when the target is native (in one of the ranges), otherwise
// it becomes the exit thunk and x9 the target. Only x9, x11 and x16 change. unsupported: a brk, for the slots not implemented.
std::vector<uint32_t> arm64ec_dispatch_ret();
std::vector<uint32_t> arm64ec_dispatch_call();
std::vector<uint32_t> arm64ec_check_icall(std::span<const std::pair<uint64_t, uint64_t>> native);
std::vector<uint32_t> arm64ec_unsupported();
// A lookup-table target that leaves translated code with ExitReason::returned: the address Runtime::call returns to.
std::vector<uint32_t> return_stub();

}
