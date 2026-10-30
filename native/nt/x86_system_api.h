#pragma once

#include "environment.h"
#include "memory.h"
#include "objects.h"
#include "threads.h"

#include <map>
#include <string>
#include <vector>

namespace raven {

// System services a 32-bit program needs that are tied to the width of its pointers or to the cooperative scheduler: thread local
// storage, slim locks and condition variables, semaphores, multi-object waits, and the answers for services Raven does not model.
class NativeX86System {
public:
	struct Export {
		std::string_view module, name;
		uint32_t words;
	};
	NativeX86System(NativeProcess &process, NativeEnvironment &environment, NativeObjects &objects, NativeThreads &threads, NativeMemory &memory);
	static std::span<const Export> exports();
	bool dispatch(std::string_view name);

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	NativeObjects &objects;
	NativeThreads &threads;
	NativeMemory &memory;
	struct Srw {
		uint32_t owner = 0, shared = 0;
	};
	std::map<uint64_t, Srw> locks;
	std::map<uint64_t, std::vector<std::shared_ptr<bool>>> sleepers;
	std::map<uint32_t, std::map<uint32_t, uint32_t>> tls_values;
	// Slot 0 is taken before the program runs, as the system libraries of a Windows process take it: programs (older Rust runtimes
	// among them) take an index of 0 for "not allocated".
	std::vector<bool> tls_used = [] { std::vector<bool> used(1088, false); used[0] = true; return used; }();
	uint32_t unique_counter = 1;
	uint32_t wait(const std::vector<uint64_t> &handles, bool all, uint32_t milliseconds);
	void lock_exclusive(uint64_t address);
	void lock_shared(uint64_t address);
	void write32(uint64_t address, uint32_t value);
	uint32_t read32(uint64_t address);
};

}
