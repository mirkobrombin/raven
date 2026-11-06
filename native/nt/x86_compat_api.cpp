#include <cstdlib>
#include "x86_compat_api.h"

#include "actctx.h"

#include <algorithm>
#include <array>
#include <atomic>
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

constexpr uint32_t guest_heap_classes = large_block / 16 + 1, guest_heap_pages = 1U << 20;

void code_bytes(std::vector<uint8_t> &code, std::initializer_list<uint8_t> bytes)
{
	code.insert(code.end(), bytes);
}

void code_word(std::vector<uint8_t> &code, uint32_t value)
{
	for (size_t byte = 0; byte < 4; ++byte)
		code.push_back(uint8_t(value >> (byte * 8)));
}

// A short forward jump whose target is set once the label is reached.
size_t code_jump(std::vector<uint8_t> &code, uint8_t opcode)
{
	code_bytes(code, {opcode, 0});
	return code.size();
}

void code_land(std::vector<uint8_t> &code, std::initializer_list<size_t> jumps)
{
	for (const auto after : jumps)
		code[after - 1] = uint8_t(code.size() - after);
}

// The two calls that a C runtime makes for every malloc and free answer from free lists in guest memory, as a trap costs far more than
// the allocation. The bitmap names the pages of the arena, so that a block from anywhere else, which has no header, goes to the runtime
// and so does a heap whose blocks run code. The lock is only tried: a thread that finds it held takes the runtime's path, whose own lists
// and bump region do not depend on it. Free lists link through the third word of the block header, not the payload, so that a write
// through a stale pointer after HeapFree, which the Windows heap survives, cannot redirect the next allocation.
std::vector<uint8_t> alloc_prelude(uint32_t lock, uint32_t lists, uint32_t executable)
{
	std::vector<uint8_t> code;
	code_bytes(code, {0x8b, 0x44, 0x24, 0x04, 0x3d}); // mov eax,[esp+4]; cmp eax,executable
	code_word(code, executable);
	const auto foreign = code_jump(code, 0x74);
	code_bytes(code, {0x8b, 0x44, 0x24, 0x0c, 0x8d, 0x48, 0xff, 0x81, 0xf9}); // mov eax,[esp+12]; lea ecx,[eax-1]; cmp ecx,limit
	code_word(code, uint32_t(large_block) - 1);
	const auto sized = code_jump(code, 0x77);
	code_bytes(code, {0x83, 0xc0, 0x0f, 0xc1, 0xe8, 0x04, 0xb9, 1, 0, 0, 0, 0x87, 0x0d}); // add eax,15; shr eax,4; mov ecx,1; xchg [lock],ecx
	code_word(code, lock);
	code_bytes(code, {0x85, 0xc9}); // test ecx,ecx
	const auto held = code_jump(code, 0x75);
	code_bytes(code, {0x89, 0xc1, 0x8b, 0x04, 0x8d}); // mov ecx,eax; mov eax,[lists+ecx*4]
	code_word(code, lists);
	code_bytes(code, {0x85, 0xc0}); // test eax,eax
	const auto empty = code_jump(code, 0x74);
	code_bytes(code, {0x8b, 0x50, 0xf8, 0x89, 0x14, 0x8d}); // mov edx,[eax-8]; mov [lists+ecx*4],edx
	code_word(code, lists);
	code_bytes(code, {0xc7, 0x05}); // mov dword [lock],0
	code_word(code, lock);
	code_word(code, 0);
	code_bytes(code, {0xf6, 0x44, 0x24, 0x08, 0x08}); // test byte [esp+8],HEAP_ZERO_MEMORY
	const auto zero = code_jump(code, 0x75);
	code_bytes(code, {0xc2, 0x0c, 0x00}); // ret 12
	code_land(code, {zero});
	code_bytes(code, {0x57, 0x89, 0xc7, 0x89, 0xc2, 0xc1, 0xe1, 0x02, 0x31, 0xc0, 0xf3, 0xab, 0x89, 0xd0, 0x5f, 0xc2, 0x0c, 0x00}); // rep stosd over the class, keeping edi
	code_land(code, {empty});
	code_bytes(code, {0xc7, 0x05}); // mov dword [lock],0
	code_word(code, lock);
	code_word(code, 0);
	code_land(code, {foreign, sized, held});
	return code;
}

std::vector<uint8_t> free_prelude(uint32_t lock, uint32_t lists, uint32_t pages)
{
	std::vector<uint8_t> code;
	code_bytes(code, {0x8b, 0x44, 0x24, 0x0c, 0x89, 0xc1, 0xc1, 0xe9, 0x0c, 0x0f, 0xa3, 0x0d}); // mov eax,[esp+12]; mov ecx,eax; shr ecx,12; bt [pages],ecx
	code_word(code, pages);
	const auto foreign = code_jump(code, 0x73);
	code_bytes(code, {0x8b, 0x48, 0xf0, 0xc1, 0xe9, 0x04, 0x81, 0xf9}); // mov ecx,[eax-16]; shr ecx,4; cmp ecx,classes
	code_word(code, guest_heap_classes);
	const auto sized = code_jump(code, 0x73);
	code_bytes(code, {0xba, 1, 0, 0, 0, 0x87, 0x15}); // mov edx,1; xchg [lock],edx
	code_word(code, lock);
	code_bytes(code, {0x85, 0xd2}); // test edx,edx
	const auto held = code_jump(code, 0x75);
	code_bytes(code, {0x8b, 0x14, 0x8d}); // mov edx,[lists+ecx*4]
	code_word(code, lists);
	code_bytes(code, {0x89, 0x50, 0xf8, 0x89, 0x04, 0x8d}); // mov [eax-8],edx; mov [lists+ecx*4],eax
	code_word(code, lists);
	code_bytes(code, {0xc7, 0x05}); // mov dword [lock],0
	code_word(code, lock);
	code_word(code, 0);
	code_bytes(code, {0xb8, 1, 0, 0, 0, 0xc2, 0x0c, 0x00}); // mov eax,1; ret 12
	code_land(code, {foreign, sized, held});
	return code;
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
		if (guest_pages && !executable) {
			// One bit per page: mark exactly the region's pages, since a region need not start on a 32 KiB boundary and whole bytes
			// would claim up to seven pages of unrelated memory, whose blocks the guest's HeapFree would then take for its own.
			const uint64_t first = arena.region >> 12, count = heap_region >> 12;
			const uint64_t low = first >> 3, high = (first + count + 7) >> 3;
			auto bits = process.read_memory(guest_pages + low, size_t(high - low));
			for (uint64_t page = first; page < first + count; ++page)
				bits[size_t((page >> 3) - low)] |= uint8_t(1U << (page & 7));
			process.write_memory(guest_pages + low, bits);
		}
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

void NativeX86Compat::guest_heap()
{
	if (!traps || guest_pages)
		return;
	const size_t lists_size = guest_heap_classes * 4, pages_size = guest_heap_pages / 8;
	const auto lock = process.map_memory(4096 + ((lists_size + 4095) & ~size_t(4095)) + pages_size, PROT_READ | PROT_WRITE);
	const auto lists = lock + 4096;
	guest_pages = lists + ((lists_size + 4095) & ~size_t(4095));
	const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE | PROT_EXEC);
	uint64_t next = code;
	std::map<uint64_t, std::pair<std::string_view, uint32_t>> served;
	for (const auto &[stub, api] : *traps) {
		std::vector<uint8_t> body;
		if (api.first == "GetProcessHeap") {
			body = {0xb8};
			code_word(body, uint32_t(process_heap_handle));
			code_bytes(body, {0xc3});
		} else if (api.first == "HeapAlloc")
			body = alloc_prelude(uint32_t(lock), uint32_t(lists), uint32_t(executable_heap_handle));
		else if (api.first == "HeapFree")
			body = free_prelude(uint32_t(lock), uint32_t(lists), uint32_t(guest_pages));
		else
			continue;
		if (api.first != "GetProcessHeap") {
			served.emplace(next + body.size(), api);
			code_bytes(body, {0xcc, 0xc3});
		}
		process.write_memory(next, body);
		std::vector<uint8_t> jump{0xe9};
		code_word(jump, uint32_t(next - (stub + 5)));
		const auto page = stub & ~uint64_t(4095);
		process.protect_image(page, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
		process.write_memory(stub, jump);
		process.protect_image(page, 8192, PROT_READ | PROT_EXEC);
		next += (body.size() + 2 + 15) & ~size_t(15);
	}
	for (const auto &[address, api] : served) {
		process.add_api_trap(address, api.second);
		traps->emplace(address, api);
	}
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

static std::atomic<bool> guest_heap_wanted{false};

void want_guest_heap()
{
	guest_heap_wanted.store(true, std::memory_order_relaxed);
}

bool NativeX86Compat::dispatch(std::string_view name)
{
	const auto known = std::find_if(compat_exports.begin(), compat_exports.end(), [&](const auto &item) { return item.first == name; });
	if (known == compat_exports.end())
		return false;
	// The guest-side heap is opt-in: with it, a 32-bit program that starts several threads at once (Bitvise SSH Client given -host)
	// still crashes now and then on a corrupted block, and without it it does not.
	static const bool guest_side = std::getenv("RAVEN_GUEST_HEAP") != nullptr;
	if ((guest_side || guest_heap_wanted.load(std::memory_order_relaxed)) && (name.starts_with("Heap") || name == "GetProcessHeap"))
		guest_heap();
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
		// HEAP_REALLOC_IN_PLACE_ONLY, which _expand asks for: the caller keeps using the block, so it may only stay where it is.
		if (args[1] & 0x10) {
			if (!global && size && size <= old_size) {
				process.return_api(args[2]);
				return true;
			}
			environment.set_last_error(8);
			process.return_api(0);
			return true;
		}
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
		const auto version = activation_contexts().reported_version();
		write_word(info, 4, version.major);
		write_word(info, 8, version.minor);
		write_word(info, 12, version.build);
		write_word(info, 16, 2);
		if (version.service_pack) {
			const std::string pack = "Service Pack " + std::to_string(version.service_pack);
			std::copy(pack.begin(), pack.end(), info.begin() + 20);
			if (size == 156)
				info[148] = uint8_t(version.service_pack);
		}
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
