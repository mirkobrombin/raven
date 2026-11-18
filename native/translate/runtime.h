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
#include <mutex>
#include <span>
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

// Enters translated code at code, with the CpuState translated code sees at state, through the enter trampoline at trampoline, and
// returns the exit reason. Every address is one translated code uses. Without a runner the runtime calls the trampoline in this
// process; a VM's runner resumes a vCPU instead.
class CodeRunner {
public:
	virtual ~CodeRunner() = default;
	virtual uint32_t enter(uint64_t trampoline, uint64_t state, uint64_t code) = 0;
	// Called from another host thread after the runtime unlinked and unpublished code: returns once code this runner is running has
	// passed a context synchronization event since the call, or at once when it runs none. Then it can no longer enter that code.
	virtual void quiesce() {}
};

// Runs x64 code by translating it block by block: dispatch, the lookup table for indirect branches, chaining of direct branches,
// native functions behind thunks, and invalidation when guest code is written. The translations, the lookup table and the chains are
// shared by every guest thread of the runtime; each thread has its own CpuState and runner. Guest threads run translated code
// concurrently; the host side (translation, chaining, invalidation) runs on the host thread of whichever guest thread needs it, one at
// a time under the runtime's lock.
class Runtime {
	// Runtime data that goes back to the allocator with its owner, also when the owner's constructor throws.
	struct OwnedData {
		CodeAllocator &cache;
		CodeAllocator::Data data;
		OwnedData(CodeAllocator &cache, size_t size) : cache(cache), data(cache.allocate_data(size)) {}
		~OwnedData() { cache.free_data(data); }
		OwnedData(const OwnedData &) = delete;
		OwnedData &operator=(const OwnedData &) = delete;
	};

public:
	// One guest thread: its CpuState, which translated code sees at state_address(), and the runner that enters its code (none: this
	// process's own thread). Use it from one host thread at a time.
	class Thread {
	public:
		Thread(const Thread &) = delete;
		Thread &operator=(const Thread &) = delete;
		CpuState &state() { return cpu; }
		uint64_t state_address() const { return state_memory.data.address; }
		// Calls the guest function at address from native code, with up to four integer arguments, on the guest stack the state's RSP
		// names.
		RunResult call(uint64_t address, std::initializer_list<uint64_t> arguments = {});
		// From a native function: ends the current call with ExitReason::stopped once the function returns.
		void request_stop(uint64_t code);

	private:
		friend class Runtime;
		Thread(Runtime &runtime, CodeRunner *runner);
		Runtime &runtime;
		CodeRunner *runner;
		OwnedData state_memory;
		CpuState &cpu;
		unsigned depth = 0;
		uint32_t enter_code(const uint8_t *code);
		RunResult run();
	};

	// The first guest thread runs on runner.
	Runtime(GuestMemory &memory, CodeAllocator &cache, TranslatorOptions options = {}, CodeRunner *runner = nullptr);
	~Runtime();
	Runtime(const Runtime &) = delete;
	Runtime &operator=(const Runtime &) = delete;

	// Another guest thread, running on runner, until remove_thread or the runtime's end. Only threads with a runner can be added, and
	// not once native functions are bound: in this process the runtime runs its first thread alone (the fault handler's state and the
	// natives' stops are not per thread).
	Thread &add_thread(CodeRunner *runner);
	// Before its runner goes. Not the first thread.
	void remove_thread(Thread &thread);
	Thread &main_thread() { return *first; }
	// The first thread's.
	CpuState &state() { return first->state(); }
	uint64_t state_address() const { return first->state_address(); }
	const TranslationStats &translation_stats() const { return translation; }
	const RuntimeStats &stats() const { return counters; }
	// The runtime whose guest code is running on this thread, for native functions that need it.
	static Runtime *current();

	// Makes a guest call to address run function, taking integer_arguments x64 integer arguments; GuestMemory::is_native must agree.
	// Only without a runner: the thunk calls function in this process.
	void bind_native(uint64_t address, const void *function, unsigned integer_arguments);
	// Thread::call on the guest thread running on this host thread, or on the first thread.
	RunResult call(uint64_t address, std::initializer_list<uint64_t> arguments = {});
	void request_stop(uint64_t code);

	// Translates the block at address now, as an ahead-of-time pass over an image does; returns its guest size, 0 when there is none.
	uint32_t translate_ahead(uint64_t address);
	// Where the translation of the block at address sits in the code cache, or nullptr.
	const void *translated_code(uint64_t address) const;

	// A guest write reached guest code at address: drops every translation of the granule holding it. On return no guest thread can
	// enter them: chains into them are undone, the lookup table no longer names them and every other guest thread's runner has been
	// quiesced. A guest thread inside one finishes that block, which has no backward branch of its own, and leaves it.
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
	OwnedData lookup_memory;
	std::span<LookupEntry> lookup;
	TranslationStats translation;
	RuntimeStats counters;
	std::unordered_map<uint64_t, std::unique_ptr<Block>> blocks;
	std::map<uint64_t, std::vector<uint64_t>> blocks_by_granule;
	std::unordered_map<uint64_t, uint8_t *> natives;
	uint8_t *enter = nullptr;
	uint8_t *exit = nullptr;
	uint8_t *return_address = nullptr;
	uint64_t return_guest = 0;
	std::vector<std::unique_ptr<Thread>> threads;
	// threads.front(), kept apart: add_thread may move the vector while another thread reads it.
	Thread *first = nullptr;
	mutable std::mutex lock;

	void place_fixed_code();
	uint8_t *place(const std::vector<uint32_t> &code);
	uint8_t *host_code(uint64_t guest);
	Block *translate(uint64_t guest);
	Block *install(TranslatedBlock &&translated);
	void remember(uint64_t guest, uint8_t *host);
	void chain(uint8_t *site, uint64_t target);
	void invalidate(Block &block);
	Thread &calling_thread();
};

}
