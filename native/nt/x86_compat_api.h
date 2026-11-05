#pragma once

#include <functional>
#include "desktop/windows.h"
#include "environment.h"

#include <map>
#include <vector>

namespace raven {

// Turns the guest-side heap paths on for this process, as RAVEN_GUEST_HEAP does, for a program known to need them: one that allocates
// so much that a stop per call makes it unusable.
void want_guest_heap();

// Services a 32-bit program needs beyond the shared handlers: the process heap, and the ANSI forms of the window functions, which
// widen their strings and forward to the wide handler.
class NativeX86Compat {
public:
	NativeX86Compat(NativeProcess &process, NativeEnvironment &environment, NativeWindows &windows);
	bool dispatch(std::string_view name);
	// A guest ANSI string as UTF-16 in scratch memory that stays valid for a few calls; atoms and null pass through.
	uint32_t widen(uint64_t address);
	static std::span<const std::pair<std::string_view, uint32_t>> exports();
	// Memory that GlobalAlloc and LocalAlloc hand out lives on the process heap in Windows, so the heap functions must take it too:
	// the size of such a block (UINT64_MAX when it is not one) and how to give it back.
	// The trap table, which the guest paths of the heap add their own way into the runtime to.
	void set_traps(std::map<uint64_t, std::pair<std::string_view, uint32_t>> *table) { traps = table; }
	void set_global_blocks(std::function<uint64_t(uint64_t)> size, std::function<bool(uint64_t)> release)
	{
		global_size = std::move(size);
		global_release = std::move(release);
	}

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	NativeWindows &windows;
	// Free blocks by payload size, and the bump region new blocks are cut from: one arena for the process heap and every heap made like
	// it, one whose memory runs code for the heaps made with HEAP_CREATE_ENABLE_EXECUTE.
	struct Arena {
		std::map<uint32_t, std::vector<uint64_t>> free_blocks;
		uint64_t region = 0, region_left = 0;
	};
	Arena arenas[2];
	std::map<uint64_t, std::pair<std::string_view, uint32_t>> *traps = nullptr;
	uint64_t guest_pages = 0;
	void guest_heap();
	uint64_t scratch = 0, environment_block = 0;
	size_t scratch_slot = 0;
	std::function<uint64_t(uint64_t)> global_size;
	std::function<bool(uint64_t)> global_release;
	bool global_block(uint64_t address) const { return global_size && global_size(address) != UINT64_MAX; }
	uint64_t heap_alloc(uint32_t size, bool zero, bool executable = false);
	void heap_free(uint64_t address);
	uint32_t block_size(uint64_t address) const;
	uint64_t scratch_base();
	bool forward(std::string_view wide_name);
};

}
