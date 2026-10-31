#include "x86_compat_api.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <sys/mman.h>

namespace raven {

namespace {

constexpr std::array<std::pair<std::string_view, uint32_t>, 28> compat_exports{{
	{"GetProcessHeap", 0}, {"HeapAlloc", 3}, {"HeapFree", 3}, {"HeapReAlloc", 4}, {"HeapSize", 3},
	{"CreateWindowExA", 12}, {"DispatchMessageA", 1}, {"GetMessageA", 4}, {"PeekMessageA", 5},
	{"RegisterClassA", 1}, {"RegisterClassExA", 1}, {"UnregisterClassA", 2}, {"GetClassInfoA", 3}, {"GetClassInfoExA", 3},
	{"GlobalHandle", 1}, {"GetVersionExA", 1}, {"WinExec", 2}, {"GetCurrentProcess", 0}, {"SetHandleCount", 1}, {"GetEnvironmentStrings", 0},
	{"FreeEnvironmentStringsA", 1}, {"UnhandledExceptionFilter", 1}, {"RtlUnwind", 4}, {"HeapCreate", 3}, {"HeapDestroy", 1},
	{"ExitWindowsEx", 2}, {"RealizePalette", 1},
}};

constexpr uint64_t process_heap_handle = 0x00150000, executable_heap_handle = 0x00160000, heap_region = 1 << 20, large_block = 256 << 10, header = 16;
constexpr size_t slot_bytes = 2048, slot_count = 4;

uint32_t read_word(const std::vector<uint8_t> &bytes, size_t offset)
{
	return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8 | uint32_t(bytes[offset + 2]) << 16 | uint32_t(bytes[offset + 3]) << 24;
}

void write_word(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
	for (size_t byte = 0; byte < 4; ++byte)
		bytes[offset + byte] = uint8_t(value >> (byte * 8));
}

}

NativeX86Compat::NativeX86Compat(NativeProcess &process, NativeEnvironment &environment, NativeWindows &windows)
	: process(process), environment(environment), windows(windows)
{
}

std::span<const std::pair<std::string_view, uint32_t>> NativeX86Compat::exports()
{
	return compat_exports;
}

uint32_t NativeX86Compat::block_size(uint64_t address) const
{
	return read_word(process.read_memory(address - header, 4), 0);
}

uint64_t NativeX86Compat::heap_alloc(uint32_t size, bool zero, bool executable)
{
	const uint32_t payload = (std::max<uint32_t>(size, 1) + 15) & ~uint32_t(15);
	const int protection = PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0);
	// The header holds the payload size and the arena the block goes back to.
	std::vector<uint8_t> prefix(header);
	write_word(prefix, 0, payload);
	write_word(prefix, 4, executable ? 1 : 0);
	if (payload > large_block) {
		const uint64_t length = (uint64_t(payload) + header + 4095) & ~uint64_t(4095);
		const auto base = process.map_memory(length, protection);
		process.write_memory(base, prefix);
		return base + header;
	}
	auto &arena = arenas[executable ? 1 : 0];
	auto &list = arena.free_blocks[payload];
	uint64_t address = 0;
	if (!list.empty()) {
		address = list.back();
		list.pop_back();
		if (zero)
			process.write_memory(address, std::vector<uint8_t>(payload));
		return address;
	}
	if (arena.region_left < payload + header) {
		arena.region = process.map_memory(heap_region, protection);
		arena.region_left = heap_region;
	}
	process.write_memory(arena.region, prefix);
	address = arena.region + header;
	arena.region += payload + header;
	arena.region_left -= payload + header;
	return address;
}

void NativeX86Compat::heap_free(uint64_t address)
{
	const uint32_t payload = block_size(address);
	if (payload > large_block) {
		process.unmap_memory(address - header);
		return;
	}
	const bool executable = read_word(process.read_memory(address - header + 4, 4), 0) == 1;
	arenas[executable ? 1 : 0].free_blocks[payload].push_back(address);
}

uint64_t NativeX86Compat::scratch_base()
{
	if (!scratch)
		scratch = process.map_memory(slot_bytes * slot_count, PROT_READ | PROT_WRITE);
	return scratch;
}

// Copies a guest ANSI string into one of a few scratch slots as UTF-16 and returns its address; atoms and null pass through.
uint32_t NativeX86Compat::widen(uint64_t address)
{
	if (address <= 0xffff)
		return uint32_t(address);
	const uint64_t slot = scratch_base() + (scratch_slot++ % slot_count) * slot_bytes;
	std::vector<uint8_t> wide;
	for (size_t index = 0; index < slot_bytes / 2 - 1; ++index) {
		const uint8_t character = process.read_memory(address + index, 1)[0];
		wide.push_back(character);
		wide.push_back(0);
		if (!character)
			break;
	}
	if (wide.size() >= 2 && (wide[wide.size() - 2] || wide[wide.size() - 1])) {
		wide.push_back(0);
		wide.push_back(0);
	}
	process.write_memory(slot, wide);
	return uint32_t(slot);
}

bool NativeX86Compat::forward(std::string_view wide_name)
{
	return windows.dispatch(wide_name);
}

bool NativeX86Compat::dispatch(std::string_view name)
{
	const auto known = std::find_if(compat_exports.begin(), compat_exports.end(), [&](const auto &item) { return item.first == name; });
	if (known == compat_exports.end())
		return false;
	if (name == "GetProcessHeap") {
		process.api_arguments(0);
		process.return_api(process_heap_handle);
	} else if (name == "HeapAlloc") {
		const auto args = process.api_arguments(3);
		process.return_api(heap_alloc(uint32_t(args[2]), args[1] & 0x8, uint32_t(args[0]) == executable_heap_handle));
	} else if (name == "HeapFree") {
		const auto args = process.api_arguments(3);
		if (args[2] && global_block(args[2]))
			global_release(args[2]);
		else if (args[2])
			heap_free(args[2]);
		process.return_api(1);
	} else if (name == "HeapSize") {
		const auto args = process.api_arguments(3);
		process.return_api(args[2] && global_block(args[2]) ? uint32_t(global_size(args[2])) : args[2] ? block_size(args[2]) : UINT32_MAX);
	} else if (name == "HeapReAlloc") {
		const auto args = process.api_arguments(4);
		const bool global = global_block(args[2]);
		const uint32_t old_size = global ? uint32_t(global_size(args[2])) : block_size(args[2]), size = uint32_t(args[3]);
		const auto fresh = heap_alloc(size, args[1] & 0x8, uint32_t(args[0]) == executable_heap_handle);
		process.write_memory(fresh, process.read_memory(args[2], std::min(old_size, size)));
		if (global)
			global_release(args[2]);
		else
			heap_free(args[2]);
		process.return_api(fresh);
	} else if (name == "GlobalHandle") {
		process.return_api(process.api_arguments(1)[0]);
	} else if (name == "SetHandleCount") {
		process.return_api(process.api_arguments(1)[0]);
	} else if (name == "GetCurrentProcess") {
		process.api_arguments(0);
		process.return_api(UINT32_MAX);
	} else if (name == "ExitWindowsEx") {
		process.api_arguments(2);
		environment.set_last_error(5);
		process.return_api(0);
	} else if (name == "RealizePalette") {
		process.api_arguments(1);
		process.return_api(0);
	} else if (name == "WinExec") {
		process.api_arguments(2);
		process.return_api(2);
	} else if (name == "HeapCreate") {
		// Every heap shares the process heap's arena, except those whose blocks must run code.
		const auto args = process.api_arguments(3);
		process.return_api(args[0] & 0x40000 ? executable_heap_handle : process_heap_handle);
	} else if (name == "HeapDestroy" || name == "FreeEnvironmentStringsA") {
		process.api_arguments(1);
		process.return_api(1);
	} else if (name == "UnhandledExceptionFilter") {
		// The last chance for an exception nothing handled: it is named, and the process ends as its handler asks.
		const auto args = process.api_arguments(1);
		try {
			const auto record = read_word(process.read_memory(args[0], 4), 0);
			const auto bytes = process.read_memory(record, 32);
			std::cerr << "raven: unhandled exception 0x" << std::hex << read_word(bytes, 0) << " at 0x" << read_word(bytes, 12) << std::dec;
			// A C++ exception names the type thrown, as the compiler decorated it.
			if (read_word(bytes, 0) == 0xe06d7363 && read_word(bytes, 16) >= 3) {
				const auto info = read_word(process.read_memory(record + 28, 4), 0);
				const auto types = read_word(process.read_memory(info + 12, 4), 0);
				const auto type = read_word(process.read_memory(read_word(process.read_memory(types + 4, 4), 0) + 4, 4), 0);
				const auto name = process.read_memory(type + 8, 64);
				std::cerr << " of type " << std::string(reinterpret_cast<const char *>(name.data()), strnlen(reinterpret_cast<const char *>(name.data()), 64));
			}
			std::cerr << '\n';
		} catch (...) {
		}
		process.return_api(1);
	} else if (name == "RtlUnwind") {
		process.api_arguments(4);
		process.return_api(0);
	} else if (name == "GetEnvironmentStrings") {
		process.api_arguments(0);
		// An empty block: two terminators. The environment is not exposed to 32-bit programs.
		if (!environment_block) {
			environment_block = heap_alloc(16, true);
		}
		process.return_api(environment_block);
	} else if (name == "GetVersionExA") {
		const auto args = process.api_arguments(1);
		const auto size = read_word(process.read_memory(args[0], 4), 0);
		if (size != 148 && size != 156) {
			environment.set_last_error(122);
			process.return_api(0);
			return true;
		}
		std::vector<uint8_t> info(size);
		write_word(info, 0, size);
		write_word(info, 4, 10);
		write_word(info, 12, 19045);
		write_word(info, 16, 2);
		process.write_memory(args[0], info);
		process.return_api(1);
	} else if (name == "RegisterClassA" || name == "RegisterClassExA") {
		const bool extended = name == "RegisterClassExA";
		auto args = process.api_arguments(1);
		const size_t size = extended ? 48 : 40, shift = extended ? 4 : 0;
		auto definition = process.read_memory(args[0], size);
		write_word(definition, 32 + shift, widen(read_word(definition, 32 + shift)));
		write_word(definition, 36 + shift, widen(read_word(definition, 36 + shift)));
		scratch_base();
		// The rewritten structure lives in the last scratch slot's tail, clear of the strings.
		const uint64_t copy = scratch + slot_bytes * slot_count - 64;
		process.write_memory(copy, definition);
		process.set_api_argument(0, uint32_t(copy));
		return forward(extended ? "RegisterClassExW" : "RegisterClassW");
	} else if (name == "CreateWindowExA") {
		const auto args = process.api_arguments(12);
		process.set_api_argument(1, widen(args[1]));
		process.set_api_argument(2, widen(args[2]));
		return forward("CreateWindowExW");
	} else if (name == "PeekMessageA") {
		return forward("PeekMessageW");
	} else if (name == "GetMessageA") {
		return forward("GetMessageW");
	} else if (name == "DispatchMessageA") {
		return forward("DispatchMessageW");
	} else if (name == "GetClassInfoA" || name == "GetClassInfoExA") {
		// The wide handler fills the same layout; the class name it leaves there is the widened copy, so the program's own string goes
		// back in its place.
		const bool extended = name == "GetClassInfoExA";
		const auto args = process.api_arguments(3);
		const uint32_t wide = widen(args[1]);
		process.set_api_argument(1, wide);
		const bool handled = forward(extended ? "GetClassInfoExW" : "GetClassInfoW");
		const uint64_t field = args[2] + (extended ? 40 : 36);
		if (handled && args[2] && wide != args[1] && read_word(process.read_memory(field, 4), 0) == wide) {
			std::vector<uint8_t> original(4);
			write_word(original, 0, uint32_t(args[1]));
			process.write_memory(field, original);
		}
		return handled;
	} else if (name == "UnregisterClassA") {
		const auto args = process.api_arguments(2);
		process.set_api_argument(0, widen(args[0]));
		return forward("UnregisterClassW");
	} else {
		return false;
	}
	return true;
}

}
