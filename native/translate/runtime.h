#pragma once

#include "code_cache.h"
#include "cpu_state.h"
#include "guest_memory.h"
#include "translator.h"

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace raven::translate {

struct RunResult {
	ExitReason reason = ExitReason::none;
	// RAX when the guest returned, the code a native function stopped it with, or the guest address of the exit otherwise.
	uint64_t value = 0;
};

struct RuntimeStats {
	uint64_t runs = 0;
	uint64_t lookup_misses = 0;
	uint64_t chains = 0;
	uint64_t invalidated_blocks = 0;
	uint64_t cache_loaded_blocks = 0;
	// Nanoseconds spent translating, and spent placing blocks loaded from an ahead-of-time cache.
	uint64_t translate_ns = 0;
	uint64_t cache_load_ns = 0;
};

// Runs x64 code by translating it block by block: dispatch, the lookup table for indirect branches, chaining of direct branches,
// native functions behind thunks, and invalidation when guest code is written. One guest thread; nothing here is thread-safe yet.
class Runtime {
public:
	Runtime(GuestMemory &memory, CodeAllocator &cache, TranslatorOptions options = {});
	~Runtime();
	Runtime(const Runtime &) = delete;
	Runtime &operator=(const Runtime &) = delete;

	CpuState &state() { return cpu; }
	const TranslationStats &translation_stats() const { return translation; }
	const RuntimeStats &stats() const { return counters; }
	// The runtime whose guest code is running on this thread, for native functions that need it.
	static Runtime *current();

	// Makes a guest call to address run function, taking integer_arguments x64 integer arguments; GuestMemory::is_native must agree.
	void bind_native(uint64_t address, const void *function, unsigned integer_arguments);
	// Calls the guest function at address from native code, with up to four integer arguments, on the guest stack the state's RSP names.
	RunResult call(uint64_t address, std::initializer_list<uint64_t> arguments = {});
	// From a native function: ends the current call with ExitReason::stopped once the function returns.
	void request_stop(uint64_t code);

	// A guest write reached guest code at address: drops every translation of the granule holding it.
	void code_written(uint64_t address);

	// The ahead-of-time cache: every block translated so far, keyed by key (the image hash, its base and the options).
	void save_cache(const std::filesystem::path &path, uint64_t key) const;
	// Places the blocks of a cache whose key matches; returns how many.
	size_t load_cache(const std::filesystem::path &path, uint64_t key);
	// The key of a cache for an image: its bytes, where it was mapped, the translator's version and the options that change code.
	static uint64_t cache_key(std::span<const uint8_t> image, uint64_t base, const TranslatorOptions &options);

private:
	struct Block {
		uint64_t guest_start;
		uint32_t guest_size;
		uint8_t *host;
		std::vector<BlockExit> exits;
		std::vector<uint32_t> code;
		// Branches in other blocks chained to this one: their execute addresses and the original word to restore.
		std::vector<std::pair<uint8_t *, uint32_t>> incoming;
	};

	GuestMemory &memory;
	CodeAllocator &cache;
	TranslatorOptions options;
	CpuState cpu;
	TranslationStats translation;
	RuntimeStats counters;
	std::vector<LookupEntry> lookup;
	std::unordered_map<uint64_t, std::unique_ptr<Block>> blocks;
	std::map<uint64_t, std::vector<uint64_t>> blocks_by_granule;
	std::unordered_map<uint64_t, uint8_t *> natives;
	uint8_t *enter = nullptr;
	uint8_t *exit = nullptr;
	uint8_t *return_address = nullptr;
	uint64_t return_guest = 0;
	unsigned depth = 0;

	void place_fixed_code();
	uint8_t *place(const std::vector<uint32_t> &code);
	uint8_t *host_code(uint64_t guest);
	Block *translate(uint64_t guest);
	Block *install(TranslatedBlock &&translated);
	void remember(uint64_t guest, uint8_t *host);
	void chain(uint8_t *site, uint64_t target);
	void invalidate(Block &block);
	RunResult run();
};

}
