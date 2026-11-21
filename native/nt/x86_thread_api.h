#pragma once

#include "environment.h"
#include "objects.h"
#include "threads.h"

#include <map>

namespace raven {

// Thread and wait services of a 32-bit process: stdcall arguments are read from the guest stack.
class NativeX86Threads {
public:
	NativeX86Threads(NativeProcess &process, NativeEnvironment &environment, NativeObjects &objects, NativeThreads &threads);
	bool dispatch(std::string_view name);
	static std::span<const std::pair<std::string_view, uint32_t>> exports();

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	NativeObjects &objects;
	NativeThreads &threads;
	struct Section {
		uint32_t owner = 0;
		uint32_t depth = 0;
	};
	// Critical sections are keyed by guest address: threads switch only inside API calls, so no host lock is needed.
	std::map<uint64_t, Section> sections;
	// The debug data every section points at: callers that force it write the section's name there.
	uint64_t section_debug = 0;
	// Writes the owner fields of the guest's section as Windows keeps them, which code that tests a section inline reads.
	void mirror_section(uint64_t address, const Section &section);
};

}
