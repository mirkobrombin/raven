#pragma once

#include <cstdint>
#include <span>
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

// Translates the x64 basic block at address, whose bytes start code. The code reaches the runtime only through CpuState (x23) and the
// exit protocol, so it can be copied anywhere: into the code cache now, or into an ahead-of-time cache file.
TranslatedBlock translate_block(std::span<const uint8_t> code, uint64_t address, const TranslatorOptions &options,
	TranslationStats &stats);

// The fixed code the runtime places once in the code cache.
// enter(CpuState *state, const void *code): saves the host's callee-saved registers, loads the guest's and branches to code.
std::vector<uint32_t> enter_trampoline();
// Reached with the exit reason in w9: stores the guest's registers and returns the reason from enter.
std::vector<uint32_t> exit_trampoline();
// Code a guest call reaches when it calls native function: the x64 arguments go to the AAPCS64 argument registers, the result to RAX,
// and the guest's return address is taken as a ret would take it. Microsoft's ARM64EC names this direction an entry thunk.
std::vector<uint32_t> native_thunk(uint64_t function, unsigned integer_arguments);
// A lookup-table target that leaves translated code with ExitReason::returned: the address Runtime::call returns to.
std::vector<uint32_t> return_stub();

}
