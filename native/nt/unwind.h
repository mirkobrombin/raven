#pragma once

#include "cpu/native_cpu.h"
#include "pe/module_set.h"

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace raven {

// x64 CONTEXT (1232 bytes) as raw guest bytes, with the register slots the unwinder touches.
namespace x64_context {
inline constexpr size_t size = 1232, flags = 0x30, mxcsr = 0x34, eflags = 0x44, general = 0x78, rsp = 0x98, rip = 0xf8, float_area = 0x100;
inline constexpr uint32_t full_flags = 0x10000f;
}

using ContextBytes = std::vector<uint8_t>;
uint64_t context_register(const ContextBytes &context, size_t number);
void set_context_register(ContextBytes &context, size_t number, uint64_t value);
uint64_t context_rip(const ContextBytes &context);
uint64_t context_rsp(const ContextBytes &context);
void set_context_rip(ContextBytes &context, uint64_t value);
void set_context_rsp(ContextBytes &context, uint64_t value);
ContextBytes context_from_process(const NativeContext &native);
// Writes the registers a CONTEXT describes into a native context, keeping its other state.
void apply_context(NativeContext &native, const ContextBytes &context);
// The upper halves of the sixteen YMM registers, the AVX part of the processor state, which a CONTEXT keeps apart from its record in
// the extended area a CONTEXT_EX describes: an XSAVE header of 64 bytes whose first quadword is the mask of the features kept, then
// the 256 bytes of the halves.
namespace x64_context {
inline constexpr size_t extension = 24, extended_header = 64, avx_size = 256;
inline constexpr uint32_t xstate_flags = 0x100040;
inline constexpr uint64_t avx_mask = 4;
}
// Whether the processor state the host keeps for a thread has an AVX part.
bool native_has_avx(const NativeContext &native);
std::array<uint8_t, x64_context::avx_size> native_avx(const NativeContext &native);
void set_native_avx(NativeContext &native, const uint8_t *halves);
// Where the extended area of a CONTEXT in guest memory is: zero for one that asks for no extended state or has no room for the AVX part.
uint64_t context_extended_area(NativeProcess &process, uint64_t context);

struct FunctionEntry {
	uint64_t address = 0;
	uint32_t begin = 0, end = 0, unwind = 0;
	uint64_t image_base = 0;
	const PEModule *module = nullptr;
};

// Table-based x64 stack unwinding over the loaded modules' exception directories.
class NativeUnwinder {
public:
	NativeUnwinder(NativeProcess &process, const ModuleSet &modules) : process(process), modules(modules) {}
	std::optional<FunctionEntry> lookup(uint64_t pc) const;
	const PEModule *module_at(uint64_t address) const;
	struct Result {
		uint64_t handler = 0, handler_data = 0, establisher = 0;
	};
	// Where the callee saved each nonvolatile register it restores, as KNONVOLATILE_CONTEXT_POINTERS lists them: XMM0 to XMM15, then
	// RAX to R15 in CONTEXT order.
	using ContextPointers = std::array<uint64_t, 32>;
	// Moves the context from the callee's frame to its caller's; handler_type is UNW_FLAG_EHANDLER (1) or UNW_FLAG_UHANDLER (2).
	// The stack addresses the saved registers come from are recorded in pointers when it is given.
	Result unwind(uint32_t handler_type, uint64_t pc, const FunctionEntry &function, ContextBytes &context,
		ContextPointers *pointers = nullptr) const;
	// A function without unwind data is a leaf: its caller's return address is on top of the stack.
	void unwind_leaf(ContextBytes &context) const;

	// Function tables of code a generator emits outside any module: an array of RUNTIME_FUNCTION entries relative to a base
	// (RtlAddFunctionTable), or a range whose entries a callback in the program finds (RtlInstallFunctionTableCallback).
	struct DynamicTable {
		uint64_t identifier = 0, base = 0, length = 0;
		uint64_t entries = 0, count = 0;
		uint64_t callback = 0, context = 0;
	};
	void add_table(const DynamicTable &table) { tables.push_back(table); }
	bool remove_table(uint64_t identifier);
	// Calls a function of the program with two arguments and returns what it returns; the callbacks of function tables run this way.
	void set_guest_call(std::function<uint64_t(uint64_t, uint64_t, uint64_t)> call) { guest_call = std::move(call); }

private:
	NativeProcess &process;
	const ModuleSet &modules;
	std::vector<DynamicTable> tables;
	std::function<uint64_t(uint64_t, uint64_t, uint64_t)> guest_call;
	std::optional<FunctionEntry> lookup_dynamic(uint64_t pc) const;
	bool epilogue(uint64_t pc, ContextBytes &context, ContextPointers *pointers) const;
};

}
