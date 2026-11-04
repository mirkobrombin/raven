#include "unwind.h"

#include <algorithm>
#include <cstring>

namespace raven {

namespace {

uint64_t load64(const ContextBytes &bytes, size_t at)
{
	uint64_t value;
	std::memcpy(&value, bytes.data() + at, 8);
	return value;
}

void store64(ContextBytes &bytes, size_t at, uint64_t value)
{
	std::memcpy(bytes.data() + at, &value, 8);
}

uint64_t read64(NativeProcess &process, uint64_t address)
{
	const auto bytes = process.read_memory(address, 8);
	uint64_t value;
	std::memcpy(&value, bytes.data(), 8);
	return value;
}

constexpr size_t xmm_base = x64_context::float_area + 160;

}

uint64_t context_register(const ContextBytes &context, size_t number)
{
	return load64(context, x64_context::general + number * 8);
}

void set_context_register(ContextBytes &context, size_t number, uint64_t value)
{
	store64(context, x64_context::general + number * 8, value);
}

uint64_t context_rip(const ContextBytes &context)
{
	return load64(context, x64_context::rip);
}

uint64_t context_rsp(const ContextBytes &context)
{
	return load64(context, x64_context::rsp);
}

void set_context_rip(ContextBytes &context, uint64_t value)
{
	store64(context, x64_context::rip, value);
}

void set_context_rsp(ContextBytes &context, uint64_t value)
{
	store64(context, x64_context::rsp, value);
}

ContextBytes context_from_process(const NativeContext &native)
{
	ContextBytes context(x64_context::size);
	const auto &r = native.registers;
	const uint32_t flags = x64_context::full_flags;
	std::memcpy(context.data() + x64_context::flags, &flags, 4);
	const uint64_t general[16] = {r.rax, r.rcx, r.rdx, r.rbx, r.rsp, r.rbp, r.rsi, r.rdi, r.r8, r.r9, r.r10, r.r11, r.r12, r.r13, r.r14, r.r15};
	std::memcpy(context.data() + x64_context::general, general, sizeof general);
	store64(context, x64_context::rip, r.rip);
	const uint32_t flags_register = uint32_t(r.eflags);
	std::memcpy(context.data() + x64_context::eflags, &flags_register, 4);
	const uint16_t segments[6] = {uint16_t(r.cs), uint16_t(r.ds), uint16_t(r.es), uint16_t(r.fs), uint16_t(r.gs), uint16_t(r.ss)};
	std::memcpy(context.data() + 0x38, segments, sizeof segments);
	// The FXSAVE image at the start of the XSAVE area has the layout of XMM_SAVE_AREA32.
	std::memcpy(context.data() + x64_context::float_area, native.xstate.data(), 512);
	std::memcpy(context.data() + x64_context::mxcsr, native.xstate.data() + 24, 4);
	// VectorRegister[26] mirrors the YMM-less XMM registers for consumers that read the vector area.
	return context;
}

void apply_context(NativeContext &native, const ContextBytes &context)
{
	auto &r = native.registers;
	uint64_t general[16];
	std::memcpy(general, context.data() + x64_context::general, sizeof general);
	r.rax = general[0]; r.rcx = general[1]; r.rdx = general[2]; r.rbx = general[3]; r.rsp = general[4]; r.rbp = general[5];
	r.rsi = general[6]; r.rdi = general[7]; r.r8 = general[8]; r.r9 = general[9]; r.r10 = general[10]; r.r11 = general[11];
	r.r12 = general[12]; r.r13 = general[13]; r.r14 = general[14]; r.r15 = general[15];
	r.rip = load64(context, x64_context::rip);
	uint32_t flags;
	std::memcpy(&flags, context.data() + x64_context::eflags, 4);
	r.eflags = (flags & 0xcd5) | 0x202;
	r.orig_rax = uint64_t(-1);
	std::memcpy(native.xstate.data(), context.data() + x64_context::float_area, 512);
}

const PEModule *NativeUnwinder::module_at(uint64_t address) const
{
	for (const auto &[name, module] : modules.all()) {
		const uint64_t base = module->loaded.base();
		if (address >= base && address - base < module->image.image_size)
			return module.get();
	}
	return nullptr;
}

bool NativeUnwinder::remove_table(uint64_t identifier)
{
	const auto found = std::find_if(tables.begin(), tables.end(), [&](const auto &table) { return table.identifier == identifier; });
	if (found == tables.end())
		return false;
	tables.erase(found);
	return true;
}

std::optional<FunctionEntry> NativeUnwinder::lookup_dynamic(uint64_t pc) const
{
	for (auto table = tables.rbegin(); table != tables.rend(); ++table) {
		if (pc < table->base || pc - table->base >= table->length)
			continue;
		uint64_t address = 0;
		if (table->callback) {
			if (!guest_call)
				return std::nullopt;
			address = guest_call(table->callback, pc, table->context);
		} else {
			const uint32_t rva = uint32_t(pc - table->base);
			size_t low = 0, high = table->count;
			while (low < high && !address) {
				const size_t middle = (low + high) / 2;
				const auto row = process.read_memory(table->entries + middle * 12, 8);
				uint32_t begin, end;
				std::memcpy(&begin, row.data(), 4);
				std::memcpy(&end, row.data() + 4, 4);
				if (rva < begin)
					high = middle;
				else if (rva >= end)
					low = middle + 1;
				else
					address = table->entries + middle * 12;
			}
		}
		if (!address)
			return std::nullopt;
		const auto row = process.read_memory(address, 12);
		FunctionEntry entry{address, 0, 0, 0, table->base, nullptr};
		std::memcpy(&entry.begin, row.data(), 4);
		std::memcpy(&entry.end, row.data() + 4, 4);
		std::memcpy(&entry.unwind, row.data() + 8, 4);
		return entry;
	}
	return std::nullopt;
}

std::optional<FunctionEntry> NativeUnwinder::lookup(uint64_t pc) const
{
	const auto *module = module_at(pc);
	if (!module)
		return lookup_dynamic(pc);
	if (module->runtime_api)
		return std::nullopt;
	const auto &table = module->image.directories[3];
	if (!table.address || table.size < 12)
		return std::nullopt;
	const uint64_t base = module->loaded.base();
	const uint32_t rva = uint32_t(pc - base);
	auto &mutable_module = const_cast<PEModule &>(*module);
	const auto data = mutable_module.loaded.data();
	if (size_t(table.address) + table.size > data.size())
		return std::nullopt;
	const auto *entries = data.data() + table.address;
	size_t low = 0, high = table.size / 12;
	while (low < high) {
		const size_t middle = (low + high) / 2;
		uint32_t row[3];
		std::memcpy(row, entries + middle * 12, 12);
		if (rva < row[0])
			high = middle;
		else if (rva >= row[1])
			low = middle + 1;
		else {
			// An entry whose unwind data has its low bit set stands for another entry of the table, the function's primary one.
			size_t at = table.address + middle * 12;
			for (int hops = 0; hops < 4 && (row[2] & 1); ++hops) {
				at = row[2] & ~uint32_t(1);
				if (at + 12 > data.size())
					return std::nullopt;
				std::memcpy(row, data.data() + at, 12);
			}
			return FunctionEntry{base + at, row[0], row[1], row[2], base, module};
		}
	}
	return std::nullopt;
}

void NativeUnwinder::unwind_leaf(ContextBytes &context) const
{
	const uint64_t rsp = context_rsp(context);
	set_context_rip(context, read64(process, rsp));
	set_context_rsp(context, rsp + 8);
}

// Emulates the rest of a function epilogue (add/lea rsp, pops, ret or tail jump) when pc sits inside one.
bool NativeUnwinder::epilogue(uint64_t pc, ContextBytes &context, ContextPointers *pointers) const
{
	std::vector<uint8_t> code;
	try {
		code = process.read_memory(pc, 16);
	} catch (const std::runtime_error &) {
		return false;
	}
	size_t at = 0;
	struct Step {
		enum { adjust, load_rsp, pop } kind;
		int64_t value;
		size_t reg;
	};
	std::vector<Step> steps;
	if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xc4) {
		steps.push_back({Step::adjust, int8_t(code[3]), 0});
		at = 4;
	} else if (code[0] == 0x48 && code[1] == 0x81 && code[2] == 0xc4) {
		int32_t value;
		std::memcpy(&value, &code[3], 4);
		steps.push_back({Step::adjust, value, 0});
		at = 7;
	}
	if (!at && (code[0] == 0x48 || code[0] == 0x4c) && code[1] == 0x8d && ((code[2] & 0xc0) == 0x40 || (code[2] & 0xc0) == 0x80) && ((code[2] >> 3) & 7) == 4) {
		// lea rsp,[reg+disp]
		const size_t reg = (code[2] & 7) | ((code[0] & 1) ? 8 : 0);
		const bool wide = (code[2] & 0xc0) == 0x80;
		int32_t displacement = wide ? 0 : int8_t(code[3]);
		if (wide)
			std::memcpy(&displacement, &code[3], 4);
		steps.push_back({Step::load_rsp, displacement, reg});
		at = wide ? 7 : 4;
	}
	for (;;) {
		if (at >= code.size())
			return false;
		uint8_t byte = code[at];
		size_t reg = 0;
		if (byte == 0x41 && at + 1 < code.size() && (code[at + 1] & 0xf8) == 0x58) {
			reg = 8 + (code[at + 1] & 7);
			steps.push_back({Step::pop, 0, reg});
			at += 2;
		} else if ((byte & 0xf8) == 0x58) {
			steps.push_back({Step::pop, 0, size_t(byte & 7)});
			++at;
		} else {
			break;
		}
	}
	const uint8_t terminator = code[at];
	const bool tail = terminator == 0xe9 || terminator == 0xeb || (terminator == 0x48 && at + 2 < code.size() && code[at + 1] == 0xff && (code[at + 2] & 0xf8) == 0xe0) ||
		(terminator == 0xff && at + 1 < code.size() && (code[at + 1] & 0xf8) == 0x20);
	const bool plain_return = terminator == 0xc3 || (terminator == 0xf3 && at + 1 < code.size() && code[at + 1] == 0xc3);
	if (!plain_return && !tail)
		return false;
	// An instruction stream that merely starts like an epilogue is only one when it reaches a return.
	if (steps.empty() && !plain_return)
		return false;
	for (const auto &step : steps) {
		if (step.kind == Step::adjust) {
			set_context_rsp(context, context_rsp(context) + uint64_t(step.value));
		} else if (step.kind == Step::load_rsp) {
			set_context_rsp(context, context_register(context, step.reg) + uint64_t(step.value));
		} else {
			const uint64_t rsp = context_rsp(context);
			set_context_register(context, step.reg, read64(process, rsp));
			if (pointers)
				(*pointers)[16 + step.reg] = rsp;
			set_context_rsp(context, rsp + 8);
		}
	}
	unwind_leaf(context);
	return true;
}

NativeUnwinder::Result NativeUnwinder::unwind(uint32_t handler_type, uint64_t pc, const FunctionEntry &function, ContextBytes &context,
	ContextPointers *pointers) const
{
	Result result{};
	const uint64_t base = function.image_base;
	// Unwind data at an offset from the image base: in the loaded image of a module, or in the program's memory for code a generator
	// emitted and described through a function table.
	const auto load = [&](uint64_t rva, size_t size) {
		if (!function.module)
			return process.read_memory(base + rva, size);
		const auto image = const_cast<PEModule &>(*function.module).loaded.data();
		if (rva > image.size() || size > image.size() - rva)
			throw std::runtime_error("native unwind data lies outside its image");
		return std::vector<uint8_t>(image.begin() + rva, image.begin() + rva + size);
	};
	const uint64_t prolog_offset = pc - (base + function.begin);
	result.establisher = context_rsp(context);
	const auto original = context;
	const auto first = load(function.unwind, 4);
	const uint32_t frame_register = first[3] & 15, frame_offset = (first[3] >> 4) * 16u;
	const uint8_t prolog_size = first[1];
	bool in_prolog = prolog_offset < prolog_size;
	if (!in_prolog && epilogue(pc, context, pointers)) {
		result.establisher = frame_register ? context_register(original, frame_register) - frame_offset : context_rsp(original);
		return result;
	}
	uint64_t limit = in_prolog ? prolog_offset : UINT64_MAX;
	uint64_t setfp_offset = UINT64_MAX;
	uint64_t info_rva = function.unwind;
	for (int depth = 0; depth < 32; ++depth) {
		const auto header = load(info_rva, 4);
		// The codes, padded to an even count, then a handler and its data or the parent function's entry.
		const size_t padded = (size_t(header[2]) + 1) & ~size_t(1);
		const auto record = load(info_rva, 4 + padded * 2 + ((header[0] >> 3) & 7 ? 12 : 0));
		const uint8_t *info = record.data();
		const uint8_t version = info[0] & 7, flags = info[0] >> 3, count = info[2];
		(void)version;
		const uint32_t frame_reg = info[3] & 15, frame_off = (info[3] >> 4) * 16u;
		const uint8_t *codes = info + 4;
		for (size_t slot = 0; slot < count;) {
			const uint8_t offset = codes[slot * 2], op = codes[slot * 2 + 1] & 15, op_info = codes[slot * 2 + 1] >> 4;
			const bool applies = offset <= limit;
			size_t used = 1;
			const auto word = [&](size_t index) { uint16_t value; std::memcpy(&value, codes + (slot + index) * 2, 2); return value; };
			const auto dword = [&](size_t index) { return uint32_t(word(index)) | uint32_t(word(index + 1)) << 16; };
			switch (op) {
			case 0:
				if (applies) {
					const uint64_t rsp = context_rsp(context);
					set_context_register(context, op_info, read64(process, rsp));
					if (pointers)
						(*pointers)[16 + op_info] = rsp;
					set_context_rsp(context, rsp + 8);
				}
				break;
			case 1:
				used = op_info ? 3 : 2;
				if (applies)
					set_context_rsp(context, context_rsp(context) + (op_info ? dword(1) : uint64_t(word(1)) * 8));
				break;
			case 2:
				if (applies)
					set_context_rsp(context, context_rsp(context) + op_info * 8 + 8);
				break;
			case 3:
				if (applies) {
					set_context_rsp(context, context_register(context, frame_reg) - frame_off);
				}
				if (depth == 0)
					setfp_offset = offset;
				break;
			case 4:
				used = 2;
				if (applies) {
					const uint64_t slot = context_rsp(context) + uint64_t(word(1)) * 8;
					set_context_register(context, op_info, read64(process, slot));
					if (pointers)
						(*pointers)[16 + op_info] = slot;
				}
				break;
			case 5:
				used = 3;
				if (applies) {
					const uint64_t slot = context_rsp(context) + dword(1);
					set_context_register(context, op_info, read64(process, slot));
					if (pointers)
						(*pointers)[16 + op_info] = slot;
				}
				break;
			case 6:
				used = 2;
				break;
			case 7:
				used = 3;
				break;
			case 8:
				used = 2;
				if (applies) {
					const uint64_t slot = context_rsp(context) + uint64_t(word(1)) * 16;
					const auto bytes = process.read_memory(slot, 16);
					std::memcpy(context.data() + xmm_base + op_info * 16, bytes.data(), 16);
					if (pointers)
						(*pointers)[op_info] = slot;
				}
				break;
			case 9:
				used = 3;
				if (applies) {
					const uint64_t slot = context_rsp(context) + dword(1);
					const auto bytes = process.read_memory(slot, 16);
					std::memcpy(context.data() + xmm_base + op_info * 16, bytes.data(), 16);
					if (pointers)
						(*pointers)[op_info] = slot;
				}
				break;
			case 10:
				if (applies) {
					const uint64_t rsp = context_rsp(context) + (op_info ? 8 : 0);
					set_context_rip(context, read64(process, rsp));
					set_context_rsp(context, read64(process, rsp + 24));
				}
				break;
			default:
				throw std::runtime_error("native unwind data uses an unknown operation");
			}
			slot += used;
		}
		const uint8_t *tail = codes + padded * 2;
		if (flags & 4) {
			// Chained unwind data continues in the parent function's own record, whose prolog has always completed.
			uint32_t parent[3];
			std::memcpy(parent, tail, 12);
			// The parent may itself stand for another entry, as table entries with the low bit set do.
			for (int hops = 0; hops < 4 && (parent[2] & 1); ++hops) {
				const auto entry = load(parent[2] & ~uint32_t(1), 12);
				std::memcpy(parent, entry.data(), 12);
			}
			info_rva = parent[2];
			limit = UINT64_MAX;
			continue;
		}
		if (flags & handler_type) {
			uint32_t handler_rva;
			std::memcpy(&handler_rva, tail, 4);
			result.handler = base + handler_rva;
			result.handler_data = base + info_rva + 4 + padded * 2 + 4;
		}
		break;
	}
	// The establisher is the frame base the function maintains: its frame register once SET_FPREG ran, the stack pointer before that.
	// A chained part runs after the prolog of the part it chains to, so its frame register is always set.
	const bool chained = (first[0] >> 3) & 4;
	const bool established = frame_register && (chained || prolog_offset >= setfp_offset);
	result.establisher = established ? context_register(original, frame_register) - frame_offset : context_rsp(original);
	unwind_leaf(context);
	return result;
}

}
