#include "hlsl_legacy.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace raven::shader {

float legacy_value(uint32_t bits, uint8_t kind)
{
	switch (kind) {
	case uint8_t(Scalar::boolean) + 1: return bits ? 1.0f : 0.0f;
	case uint8_t(Scalar::integer) + 1: return float(int32_t(bits));
	case uint8_t(Scalar::unsigned_integer) + 1: return float(bits);
	case 0:
		// Without a kind, all bits set is a true written by the generator itself.
		if (bits == ~0u)
			return 1.0f;
		break;
	default: break;
	}
	return std::bit_cast<float>(bits);
}

namespace {

// D3DSHADER_PARAM_REGISTER_TYPE.
enum Register : uint8_t {
	temp = 0, input = 1, constant = 2, address = 3, texture = 3, rastout = 4, attrout = 5, output = 6, constint = 7, colorout = 8,
	depthout = 9, sampler = 10, misc = 17
};

// D3DSHADER_INSTRUCTION_OPCODE_TYPE.
enum Op : uint16_t {
	nop = 0, mov = 1, add = 2, mad = 4, mul = 5, rcp = 6, rsq = 7, dp3 = 8, dp4 = 9, min = 10, max = 11, slt = 12, sge = 13, exp = 14,
	log = 15, lrp = 18, frc = 19, dcl = 31, abs = 35, sincos = 37, rep = 38, endrep = 39, ifc = 41, op_else = 42, endif = 43,
	op_break = 44, breakc = 45, mova = 46, defi = 48, texkill = 65, tex = 66, def = 81, cmp = 88, dp2add = 90, dsx = 91, dsy = 92,
	texldd = 93, texldl = 95
};

// D3DSHADER_COMPARISON.
enum Comparison : uint32_t { equal = 2, not_equal = 5 };

// D3DDECLUSAGE.
const std::map<std::string, uint32_t> &usages()
{
	static const std::map<std::string, uint32_t> names{{"POSITION", 0}, {"BLENDWEIGHT", 1}, {"BLENDINDICES", 2}, {"NORMAL", 3},
		{"PSIZE", 4}, {"TEXCOORD", 5}, {"TANGENT", 6}, {"BINORMAL", 7}, {"TESSFACTOR", 8}, {"POSITIONT", 9}, {"COLOR", 10}, {"FOG", 11},
		{"DEPTH", 12}, {"SAMPLE", 13}};
	return names;
}

std::string upper(std::string text)
{
	for (auto &c : text)
		c = char(std::toupper(uint8_t(c)));
	return text;
}

[[noreturn]] void fail(const std::string &message) { throw GenerateError{0, message}; }

// A source parameter: a register, its swizzle and modifier, and the address register component of a relative index.
struct Source {
	uint8_t type = temp;
	uint32_t number = 0;
	std::array<uint8_t, 4> swizzle{0, 1, 2, 3};
	// D3DSHADER_PARAM_SRCMOD_TYPE: 0 none, 1 negate, 0xb absolute, 0xc negated absolute.
	uint8_t modifier = 0;
	std::optional<uint8_t> relative;

	Source negated() const
	{
		Source result = *this;
		result.modifier = modifier == 0 ? 1 : modifier == 1 ? 0 : modifier == 0xb ? 0xc : 0xb;
		return result;
	}
	Source component(uint8_t c) const
	{
		Source result = *this;
		result.swizzle.fill(swizzle[c]);
		return result;
	}
};

struct Dest {
	uint8_t type = temp;
	uint32_t number = 0;
	uint8_t mask = 0xf;
	bool saturate = false, centroid = false;
};

uint32_t register_bits(uint8_t type, uint32_t number)
{
	return 0x80000000u | (number & 0x7ff) | uint32_t(type & 7) << 28 | uint32_t(type & 0x18) << 8;
}

uint32_t encode(const Dest &dest)
{
	return register_bits(dest.type, dest.number) | uint32_t(dest.mask) << 16 | uint32_t((dest.saturate ? 1 : 0) | (dest.centroid ? 4 : 0)) << 20;
}

Source register_source(uint8_t type, uint32_t number)
{
	Source source;
	source.type = type;
	source.number = number;
	return source;
}

Dest register_dest(uint8_t type, uint32_t number)
{
	Dest dest;
	dest.type = type;
	dest.number = number;
	return dest;
}

Source from(const Dest &dest)
{
	Source source;
	source.type = dest.type;
	source.number = dest.number;
	return source;
}

class Legacy {
public:
	Legacy(const Generated &in, uint32_t flags) : in(in), flags(flags)
	{
		pixel = in.stage == Stage::pixel;
		model3 = in.major == 3;
		extended = in.target.ends_with("_a") || in.target.ends_with("_b");
		if (!pixel && in.stage != Stage::vertex)
			fail("only vertex and pixel shaders have shader model " + std::to_string(in.major) + " targets");
		temp_limit = model3 ? 32 : in.target == "ps_2_a" ? 22 : in.target == "ps_2_b" ? 32 : in.target == "vs_2_a" ? 13 : 12;
		const_limit = pixel ? (model3 ? 224 : 32) : 256;
	}

	std::vector<uint32_t> run()
	{
		scan_declarations();
		place_constants();
		scratch_first = in.temp_count;
		prologue();
		for (at = 0; at < program.size(); ++at) {
			scratch_next = scratch_first;
			lower_apart(*program[at]);
		}
		const uint32_t temps = compact_temps();
		if (temps > temp_limit)
			fail("the program needs " + std::to_string(temps) + " temporary registers, more than " + in.target + " has");
		if (def_base + defs.size() > const_limit)
			fail("the program needs more constant registers than " + in.target + " has");
		std::vector<uint32_t> tokens;
		const uint32_t version = (pixel ? 0xffff0000u : 0xfffe0000u) | in.major << 8 | (extended ? 1 : 0);
		tokens.push_back(version);
		constant_table(tokens, version);
		declarations(tokens);
		tokens.insert(tokens.end(), body.begin(), body.end());
		tokens.push_back(0x0000ffff);
		return tokens;
	}

private:
	const Generated &in;
	uint32_t flags;
	bool pixel = false, model3 = false, extended = false;
	uint32_t temp_limit = 12, const_limit = 256;

	std::vector<const sm4::Instruction *> program;
	size_t at = 0;
	std::vector<uint32_t> body;

	// Inputs and outputs: an SM4 register to the D3D9 register that holds it.
	struct Mapped {
		uint8_t type = input;
		uint32_t number = 0;
	};
	std::map<uint32_t, Mapped> inputs, outputs;
	std::map<uint32_t, uint32_t> input_modes;
	struct Declared {
		uint32_t usage = 0, index = 0;
		uint8_t type = input;
		uint32_t number = 0;
		uint8_t mask = 0xf;
		bool centroid = false, plain = false;
	};
	std::vector<Declared> declared;
	// Texture dimensions by t register, and the D3DSAMPLER_TEXTURE_TYPE of each s register a sample reads.
	std::map<uint32_t, uint32_t> dimensions;
	std::map<uint32_t, uint32_t> sampler_types;
	std::vector<uint32_t> icb;

	// Constant registers: the used constant buffer registers, the immediate constant buffer, then the def constants.
	// The constant register each used register of a constant buffer moved to, by bind point and register: variables nothing reads
	// take none.
	std::map<std::pair<uint32_t, uint32_t>, uint32_t> constant_place;
	uint32_t icb_base = 0, def_base = 0;
	struct Def {
		std::array<float, 4> values{};
		uint32_t count = 0;
		bool fixed = false;
	};
	std::vector<Def> defs;
	bool loops = false, once = false;

	uint32_t scratch_first = 0, scratch_next = 0, scratch_high = 0;

	// ---- registers ----

	uint32_t scratch()
	{
		const uint32_t reg = scratch_next++;
		scratch_high = std::max(scratch_high, scratch_next);
		return reg;
	}

	// A temp that lives past the instruction being lowered.
	uint32_t persistent()
	{
		const uint32_t reg = scratch_first++;
		scratch_next = std::max(scratch_next, scratch_first);
		scratch_high = std::max(scratch_high, scratch_next);
		return reg;
	}

	Dest scratch_dest(uint8_t mask = 0xf)
	{
		Dest dest;
		dest.number = scratch();
		dest.mask = mask;
		return dest;
	}

	// A def constant holding all the values, the swizzle that picks them for each component.
	Source def_values(const std::array<float, 4> &values, uint8_t used)
	{
		const auto slot_of = [](const Def &def, float value) -> int {
			for (uint32_t k = 0; k < def.count; ++k)
				if (std::bit_cast<uint32_t>(def.values[k]) == std::bit_cast<uint32_t>(value))
					return int(k);
			return -1;
		};
		std::vector<float> needed;
		for (int c = 0; c < 4; ++c)
			if (used & (1 << c)) {
				bool seen = false;
				for (float v : needed)
					seen |= std::bit_cast<uint32_t>(v) == std::bit_cast<uint32_t>(values[size_t(c)]);
				if (!seen)
					needed.push_back(values[size_t(c)]);
			}
		if (needed.empty())
			needed.push_back(values[0]);
		size_t chosen = defs.size();
		for (size_t i = 0; i < defs.size() && chosen == defs.size(); ++i) {
			if (defs[i].fixed)
				continue;
			uint32_t missing = 0;
			for (float v : needed)
				missing += slot_of(defs[i], v) < 0 ? 1 : 0;
			if (defs[i].count + missing <= 4)
				chosen = i;
		}
		if (chosen == defs.size())
			defs.emplace_back();
		Def &def = defs[chosen];
		for (float v : needed)
			if (slot_of(def, v) < 0)
				def.values[def.count++] = v;
		Source source;
		source.type = constant;
		source.number = def_base + uint32_t(chosen);
		for (int c = 0; c < 4; ++c) {
			const int slot = slot_of(def, (used & (1 << c)) ? values[size_t(c)] : needed[0]);
			source.swizzle[size_t(c)] = uint8_t(slot);
		}
		return source;
	}

	Source scalar(float value) { return def_values({value, value, value, value}, 1).component(0); }
	Source zero() { return scalar(0.0f); }
	Source one() { return scalar(1.0f); }

	// A def constant of exactly these four values, as an instruction that takes a whole register expects.
	Source def_vector(const std::array<float, 4> &values)
	{
		for (size_t i = 0; i < defs.size(); ++i)
			if (defs[i].fixed && defs[i].values == values)
				return register_source(constant, def_base + uint32_t(i));
		Def def;
		def.values = values;
		def.count = 4;
		def.fixed = true;
		defs.push_back(def);
		return register_source(constant, def_base + uint32_t(defs.size() - 1));
	}

	// ---- declarations ----

	void scan_declarations()
	{
		for (const auto &instruction : in.instructions) {
			switch (instruction.opcode) {
			case sm4::dcl_global_flags:
			case sm4::dcl_temps:
			case sm4::dcl_constant_buffer:
			case sm4::dcl_sampler:
			case sm4::dcl_input:
			case sm4::dcl_output:
			case sm4::dcl_output_siv:
				break;
			case sm4::dcl_input_ps:
			case sm4::dcl_input_ps_siv:
			case sm4::dcl_input_ps_sgv:
				input_modes[instruction.operands[0].index[0]] = instruction.controls;
				break;
			case sm4::dcl_input_siv:
			case sm4::dcl_input_sgv:
				fail("system value inputs such as SV_VertexID need shader model 4");
			case sm4::dcl_resource:
				dimensions[instruction.operands[0].index[0]] = instruction.controls & 0x1f;
				break;
			case sm4::customdata:
				icb = instruction.extra;
				break;
			case sm4::dcl_indexable_temp:
				fail("arrays indexed at run time need shader model 4");
			default:
				if (instruction.opcode >= sm4::dcl_resource && instruction.opcode <= sm4::dcl_global_flags)
					fail("this resource needs shader model 4");
				if (instruction.opcode >= sm4::dcl_thread_group && instruction.opcode <= sm4::dcl_resource_structured)
					fail("this resource needs shader model 4");
				program.push_back(&instruction);
				break;
			}
		}
		for (const auto &element : in.inputs)
			map_input(element);
		for (const auto &element : in.outputs)
			map_output(element);
	}

	static std::string semantic_of(const SignatureElement &element)
	{
		std::string name = upper(element.semantic);
		if (name == "SV_POSITION")
			return "POSITION";
		if (name == "SV_TARGET")
			return "COLOR";
		if (name == "SV_DEPTH")
			return "DEPTH";
		return name;
	}

	void map_input(const SignatureElement &element)
	{
		const std::string name = semantic_of(element);
		Mapped mapped;
		if (pixel && (name == "VPOS" || name == "POSITION" || name == "VFACE" || name == "SV_ISFRONTFACE")) {
			if (!model3)
				fail("'" + element.semantic + "' needs ps_3_0");
			// Read through a temp that the prologue fills: SV_Position is the pixel centre, SV_IsFrontFace a boolean.
			mapped.type = misc;
			mapped.number = name == "VPOS" || name == "POSITION" ? 0 : 1;
			inputs[element.reg] = mapped;
			return;
		}
		const auto usage = usages().find(name);
		if (usage == usages().end())
			fail("'" + element.semantic + "' is not a semantic of " + in.target);
		if (pixel && !model3) {
			if (name != "TEXCOORD" && name != "COLOR")
				fail("'" + element.semantic + "' is not an input of " + in.target);
			mapped.type = name == "TEXCOORD" ? texture : input;
			mapped.number = element.semantic_index;
		} else {
			mapped.type = input;
			mapped.number = element.reg;
		}
		if (inputs.count(element.reg) && inputs[element.reg].number != mapped.number)
			fail("inputs that share a register need shader model 4");
		inputs[element.reg] = mapped;
		Declared item;
		item.usage = usage->second;
		item.index = element.semantic_index;
		item.type = mapped.type;
		item.number = mapped.number;
		item.mask = pixel ? element.mask : 0xf;
		const uint32_t mode = input_modes.count(element.reg) ? input_modes[element.reg] : 0;
		item.centroid = pixel && (mode == 3 || mode == 5);
		item.plain = pixel && !model3;
		if (!pixel || element.used)
			declared.push_back(item);
	}

	void map_output(const SignatureElement &element)
	{
		const std::string name = semantic_of(element);
		Mapped mapped;
		if (pixel) {
			if (name == "COLOR")
				mapped = {colorout, element.semantic_index};
			else if (name == "DEPTH")
				mapped = {depthout, 0};
			else
				fail("'" + element.semantic + "' is not an output of " + in.target);
			outputs[element.reg] = mapped;
			return;
		}
		const auto usage = usages().find(name);
		if (usage == usages().end())
			fail("'" + element.semantic + "' is not a semantic of " + in.target);
		if (model3) {
			mapped = {output, element.reg};
			Declared item;
			item.usage = usage->second;
			item.index = element.semantic_index;
			item.type = output;
			item.number = element.reg;
			item.mask = element.mask;
			declared.push_back(item);
		} else if (name == "POSITION")
			mapped = {rastout, 0};
		else if (name == "FOG")
			mapped = {rastout, 1};
		else if (name == "PSIZE")
			mapped = {rastout, 2};
		else if (name == "COLOR")
			mapped = {attrout, element.semantic_index};
		else if (name == "TEXCOORD")
			mapped = {output, element.semantic_index};
		else
			fail("'" + element.semantic + "' is not an output of " + in.target);
		outputs[element.reg] = mapped;
	}

	void place_constants()
	{
		std::vector<const ReflectedBinding *> buffers;
		for (const auto &binding : in.bindings)
			if (binding.buffer >= 0) {
				if (binding.input_type != 0)
					fail("tbuffer needs shader model 4");
				buffers.push_back(&binding);
			}
		std::sort(buffers.begin(), buffers.end(), [](auto *a, auto *b) { return a->bind_point < b->bind_point; });
		uint32_t next = 0;
		for (const auto *binding : buffers) {
			std::vector<const ReflectedVariable *> used;
			for (const auto &variable : in.buffers[size_t(binding->buffer)].variables)
				if (variable.used)
					used.push_back(&variable);
			std::sort(used.begin(), used.end(), [](auto *a, auto *b) { return a->offset < b->offset; });
			for (const auto *variable : used)
				for (uint32_t reg = variable->offset / 16; reg < (variable->offset + variable->size + 15) / 16; ++reg)
					if (constant_place.emplace(std::pair{binding->bind_point, reg}, next).second)
						++next;
		}
		icb_base = next;
		def_base = icb_base + uint32_t(icb.size() / 4);
	}

	// ---- operands ----

	Source source(const sm4::Operand &operand, uint8_t used)
	{
		Source result;
		if (operand.type == sm4::immediate32) {
			std::array<float, 4> values{};
			for (int c = 0; c < 4; ++c) {
				const int k = operand.components == 1 ? 0 : c;
				values[size_t(c)] = legacy_value(operand.immediate[size_t(k)], operand.kinds[size_t(k)]);
			}
			result = def_values(values, used);
		} else {
			switch (operand.type) {
			case sm4::temp:
				result.type = temp;
				result.number = operand.index[0];
				break;
			case sm4::input: {
				const auto found = inputs.find(operand.index[0]);
				if (found == inputs.end())
					fail("this input needs shader model 4");
				result.type = found->second.type;
				result.number = found->second.number;
				if (prologue_temps.count(operand.index[0])) {
					result.type = temp;
					result.number = prologue_temps[operand.index[0]];
				}
				break;
			}
			case sm4::constant_buffer: {
				const auto found = constant_place.find({operand.index[0], operand.index[1]});
				if (found == constant_place.end())
					fail("a constant buffer register no variable holds is read");
				result.type = constant;
				result.number = found->second;
				if (operand.relative[1])
					result.relative = address_from(*operand.relative[1]);
				break;
			}
			case sm4::immediate_constant_buffer:
				result.type = constant;
				result.number = icb_base + operand.index[0];
				if (operand.relative[0])
					result.relative = address_from(*operand.relative[0]);
				break;
			default:
				fail("this register needs shader model 4");
			}
			if (operand.mode == sm4::Operand::Mode::swizzle && operand.components == 4)
				result.swizzle = operand.swizzle;
			else if (operand.mode == sm4::Operand::Mode::select || operand.components == 1)
				result.swizzle.fill(operand.components == 1 ? 0 : operand.swizzle[0]);
		}
		if (operand.absolute) {
			if (model3)
				result.modifier = 0xb;
			else {
				const Dest held = scratch_dest(used ? used : 0xf);
				emit(abs, held, {result});
				result = from(held);
			}
		}
		if (operand.negate)
			result = result.negated();
		return result;
	}

	// A relative index: the address register, loaded from the temp component that holds the index.
	uint8_t address_from(const sm4::Relative &relative)
	{
		if (pixel)
			fail("constants indexed at run time in a pixel shader need shader model 4");
		Source index;
		index.type = relative.type == sm4::temp ? temp : input;
		index.number = relative.index;
		index.swizzle.fill(relative.component);
		Dest a0;
		a0.type = address;
		a0.mask = 1;
		emit(mova, a0, {index});
		return 0;
	}

	std::optional<Dest> dest(const sm4::Operand &operand, bool saturate = false)
	{
		Dest result;
		switch (operand.type) {
		case sm4::null_register: return std::nullopt;
		case sm4::temp:
			result.type = temp;
			result.number = operand.index[0];
			break;
		case sm4::output: {
			const auto found = outputs.find(operand.index[0]);
			if (found == outputs.end())
				fail("this output needs shader model 4");
			result.type = found->second.type;
			result.number = found->second.number;
			break;
		}
		case sm4::output_depth:
			result.type = depthout;
			break;
		default:
			fail("this register cannot be written in shader model " + std::to_string(in.major));
		}
		result.mask = operand.components == 0 ? 0xf : operand.mask;
		if (operand.components == 1)
			result.mask = 0xf;
		result.saturate = saturate;
		return result;
	}

	// ---- emission ----

	// Instructions under a condition that shader model 2 cannot branch on: each write of a program register blends with the old value.
	std::optional<std::pair<uint32_t, uint8_t>> predicate;
	// Set while the predicate itself and the bookkeeping around it are written.
	bool suspended = false;

	// The components of program temps written so far. A temp's first write keeps no earlier value for the lanes a condition leaves
	// out, as there is none, so it needs no blending.
	std::set<uint32_t> written;

	bool program_temp(const Dest &dest) const { return dest.type == temp && dest.number < in.temp_count; }

	bool fresh(const Dest &dest) const
	{
		for (uint32_t c = 0; c < 4; ++c)
			if ((dest.mask & (1 << c)) && written.count(dest.number * 4 + c))
				return false;
		return true;
	}

	bool predicated(const Dest &dest) const { return predicate && !suspended && program_temp(dest) && !fresh(dest); }

	Source predicate_source() const
	{
		Source source;
		source.number = predicate->first;
		source.swizzle.fill(predicate->second);
		return source;
	}

	void raw(uint16_t opcode, uint32_t controls, const std::vector<uint32_t> &params)
	{
		body.push_back(uint32_t(opcode) | controls << 16 | uint32_t(params.size()) << 24);
		body.insert(body.end(), params.begin(), params.end());
	}

	void emit(uint16_t opcode, std::optional<Dest> target, std::vector<Source> sources, uint32_t controls = 0)
	{
		limit_reads(sources);
		if (target && target->saturate && !pixel && !model3) {
			// vs_2_0 has no saturate modifier: the value is clamped after.
			Dest plain = *target;
			plain.saturate = false;
			const Dest held = scratch_dest(target->mask);
			emit(opcode, held, sources, controls);
			emit(max, held, {from(held), zero()});
			emit(min, plain, {from(held), one()});
			return;
		}
		if (target && predicate && !suspended && target->type != temp)
			fail("outputs written under a condition need shader model 3");
		if (target && predicated(*target)) {
			const Dest held = scratch_dest(target->mask);
			Dest unsaturated = held;
			unsaturated.saturate = target->saturate;
			put(opcode, unsaturated, sources, controls);
			Dest plain = *target;
			plain.saturate = false;
			// cmp chooses without arithmetic, so that a value that is not finite on either side stays out of the other.
			if (pixel)
				put(cmp, plain, {predicate_source().negated(), from(*target), from(held)}, 0);
			else
				put(lrp, plain, {predicate_source(), from(held), from(*target)}, 0);
			return;
		}
		put(opcode, target, sources, controls);
	}

	void put(uint16_t opcode, std::optional<Dest> target, const std::vector<Source> &sources, uint32_t controls)
	{
		std::vector<uint32_t> params;
		if (target)
			params.push_back(encode(*target));
		if (target && program_temp(*target))
			for (uint32_t c = 0; c < 4; ++c)
				if (target->mask & (1 << c))
					written.insert(target->number * 4 + c);
		for (const auto &source : sources) {
			uint32_t token = register_bits(source.type, source.number) | uint32_t(source.modifier) << 24;
			for (int c = 0; c < 4; ++c)
				token |= uint32_t(source.swizzle[size_t(c)] & 3) << (16 + 2 * c);
			if (source.relative)
				token |= 1u << 13;
			params.push_back(token);
			if (source.relative) {
				uint32_t index = register_bits(address, 0);
				for (int c = 0; c < 4; ++c)
					index |= uint32_t(*source.relative) << (16 + 2 * c);
				params.push_back(index);
			}
		}
		raw(opcode, controls, params);
	}

	// D3D9 reads only so many different constant and input registers in one instruction: the rest go through temps first.
	void limit_reads(std::vector<Source> &sources)
	{
		const auto limit_of = [&](uint8_t type) -> uint32_t {
			if (type == constant)
				return pixel ? 2 : 1;
			if (type == input || (pixel && type == texture))
				return 1;
			return 99;
		};
		std::map<uint8_t, std::set<uint32_t>> seen;
		for (auto &source : sources) {
			if (source.type == temp || source.type == misc)
				continue;
			auto &set = seen[source.type];
			if (set.count(source.number) || set.size() < limit_of(source.type)) {
				set.insert(source.number);
				continue;
			}
			const Dest held = scratch_dest();
			Source plain = source;
			plain.swizzle = {0, 1, 2, 3};
			plain.modifier = 0;
			put(mov, held, {plain}, 0);
			Source moved = from(held);
			moved.swizzle = source.swizzle;
			moved.modifier = source.modifier;
			source = moved;
		}
	}

	// ---- helpers over 0 and 1 booleans ----

	// target = condition ? when_true : when_false, for a condition of 0 or 1.
	void select(const Dest &target, const Source &condition, const Source &when_true, const Source &when_false)
	{
		if (pixel)
			emit(cmp, target, {condition.negated(), when_false, when_true});
		else
			emit(lrp, target, {condition, when_true, when_false});
	}

	// target = 1 where the comparison holds, 0 elsewhere.
	void compare(uint16_t opcode, const Dest &target, const Source &left, const Source &right)
	{
		const bool less = opcode == sm4::lt || opcode == sm4::ilt || opcode == sm4::ult;
		const bool at_least = opcode == sm4::ge || opcode == sm4::ige || opcode == sm4::uge;
		const bool equal_test = opcode == sm4::eq || opcode == sm4::ieq;
		if (!pixel) {
			if (less || at_least) {
				emit(less ? slt : sge, target, {left, right});
				return;
			}
			const Dest a = scratch_dest(target.mask), b = scratch_dest(target.mask);
			emit(equal_test ? sge : slt, a, {left, right});
			emit(equal_test ? sge : slt, b, {right, left});
			emit(equal_test ? mul : add, target, {from(a), from(b)});
			return;
		}
		const Dest difference = scratch_dest(target.mask);
		emit(add, difference, {left, right.negated()});
		if (less || at_least) {
			emit(cmp, target, {from(difference), less ? zero() : one(), less ? one() : zero()});
			return;
		}
		const Dest square = scratch_dest(target.mask);
		emit(mul, square, {from(difference), from(difference)});
		emit(cmp, target, {from(square).negated(), equal_test ? one() : zero(), equal_test ? zero() : one()});
	}

	void floor_of(const Dest &target, const Source &value)
	{
		const Dest fraction = scratch_dest(target.mask);
		emit(frc, fraction, {value});
		emit(add, target, {value, from(fraction).negated()});
	}

	void ceil_of(const Dest &target, const Source &value)
	{
		const Dest fraction = scratch_dest(target.mask);
		emit(frc, fraction, {value.negated()});
		emit(add, target, {value, from(fraction)});
	}

	// Towards zero, as a float to integer conversion rounds.
	void truncate(const Dest &target, const Source &value)
	{
		const Dest low = scratch_dest(target.mask), high = scratch_dest(target.mask);
		floor_of(low, value);
		ceil_of(high, value);
		if (pixel)
			emit(cmp, target, {value, from(low), from(high)});
		else {
			const Dest negative = scratch_dest(target.mask);
			emit(slt, negative, {value, zero()});
			emit(lrp, target, {from(negative), from(high), from(low)});
		}
	}

	// rcp, rsq, exp and log read one component: one instruction a destination component.
	void per_component(uint16_t opcode, const Dest &target, const Source &value)
	{
		for (uint8_t c = 0; c < 4; ++c)
			if (target.mask & (1 << c)) {
				Dest part = target;
				part.mask = uint8_t(1 << c);
				emit(opcode, part, {value.component(c)});
			}
	}

	std::optional<uint32_t> immediate_bits(const sm4::Operand &operand, uint8_t c) const
	{
		if (operand.type != sm4::immediate32)
			return std::nullopt;
		return operand.immediate[operand.components == 1 ? 0 : c];
	}

	// ---- inputs that need a conversion ----

	std::map<uint32_t, uint32_t> prologue_temps;

	void prologue()
	{
		for (const auto &element : in.inputs) {
			if (!pixel || !element.used)
				continue;
			const std::string name = semantic_of(element);
			if (name != "POSITION" && name != "SV_ISFRONTFACE")
				continue;
			const uint32_t reg = persistent();
			Dest target;
			target.number = reg;
			Source misc_register;
			misc_register.type = misc;
			misc_register.number = name == "POSITION" ? 0 : 1;
			if (name == "POSITION") {
				// SV_Position names the pixel centre, VPOS its corner.
				target.mask = 3;
				emit(add, target, {misc_register, scalar(0.5f)});
				target.mask = 0xc;
				emit(mov, target, {def_values({0, 0, 0, 1}, 0xc)});
			} else
				emit(cmp, target, {misc_register.component(0), one(), zero()});
			prologue_temps[element.reg] = reg;
		}
	}

	// ---- shader model 2 conditions ----

	struct Frame {
		bool block = false;
		// The predicate before the frame, the branch condition, and the lanes that left through a break inside it.
		std::pair<uint32_t, uint8_t> saved, condition, broken;
		bool always = false;
	};
	std::vector<Frame> frames;
	uint32_t slot_register = 0;
	uint8_t slot_next = 4;

	// Slots of closed frames, which the next frames take again.
	std::vector<std::pair<uint32_t, uint8_t>> free_slots;

	std::pair<uint32_t, uint8_t> slot()
	{
		if (!free_slots.empty()) {
			const auto at = free_slots.back();
			free_slots.pop_back();
			return at;
		}
		if (slot_next == 4) {
			slot_register = persistent();
			slot_next = 0;
		}
		return {slot_register, slot_next++};
	}

	static Source slot_source(std::pair<uint32_t, uint8_t> at)
	{
		Source source;
		source.number = at.first;
		source.swizzle.fill(at.second);
		return source;
	}

	static Dest slot_dest(std::pair<uint32_t, uint8_t> at)
	{
		Dest dest;
		dest.number = at.first;
		dest.mask = uint8_t(1 << at.second);
		return dest;
	}

	Source current_predicate() { return predicate ? predicate_source() : one(); }

	// Sets the predicate without the write itself being predicated.
	void set_predicate(const std::function<void(const Dest &)> &write)
	{
		const auto held = predicate ? *predicate : slot();
		unpredicated([&] { write(slot_dest(held)); });
		predicate = held;
	}

	void unpredicated(const std::function<void()> &work)
	{
		const bool was = suspended;
		suspended = true;
		work();
		suspended = was;
	}

	void flat_if(const Source &test, bool nonzero)
	{
		Frame frame;
		frame.saved = slot();
		frame.condition = slot();
		frame.broken = slot();
		frame.always = !predicate;
		unpredicated([&] {
			emit(mov, slot_dest(frame.saved), {frame.always ? one() : predicate_source()});
			if (nonzero)
				emit(mov, slot_dest(frame.condition), {test});
			else
				emit(add, slot_dest(frame.condition), {one(), test.negated()});
			emit(mov, slot_dest(frame.broken), {zero()});
		});
		frames.push_back(frame);
		set_predicate([&](const Dest &p) { emit(mul, p, {slot_source(frame.saved), slot_source(frame.condition)}); });
	}

	void flat_else()
	{
		const Frame &frame = frames.back();
		const Dest inverse = scratch_dest(1);
		unpredicated([&] { emit(add, inverse, {one(), slot_source(frame.condition).negated()}); });
		set_predicate([&](const Dest &p) { emit(mul, p, {slot_source(frame.saved), from(inverse).component(0)}); });
	}

	void flat_end()
	{
		const Frame frame = frames.back();
		frames.pop_back();
		if (frame.block)
			set_predicate([&](const Dest &p) { emit(mov, p, {slot_source(frame.saved)}); });
		else {
			set_predicate([&](const Dest &p) { emit(add, p, {slot_source(frame.saved), slot_source(frame.broken).negated()}); });
			if (!frames.empty())
				unpredicated([&] {
					emit(add, slot_dest(frames.back().broken), {slot_source(frames.back().broken), slot_source(frame.broken)});
				});
		}
		if (frames.empty() && frame.always)
			predicate.reset();
		free_slots.push_back(frame.saved);
		free_slots.push_back(frame.broken);
		if (!frame.block)
			free_slots.push_back(frame.condition);
	}

	void flat_block()
	{
		Frame frame;
		frame.block = true;
		frame.saved = slot();
		frame.broken = slot();
		frame.always = !predicate;
		unpredicated([&] {
			emit(mov, slot_dest(frame.saved), {frame.always ? one() : predicate_source()});
			emit(mov, slot_dest(frame.broken), {zero()});
		});
		frames.push_back(frame);
		if (!predicate)
			set_predicate([&](const Dest &p) { emit(mov, p, {one()}); });
	}

	// A break, or with a test a conditional one: the lanes it takes stop running to the end of the block.
	void flat_break(const std::optional<Source> &test, bool nonzero)
	{
		if (frames.empty())
			fail("break outside a loop");
		const Frame &frame = frames.back();
		const Source p = current_predicate();
		if (!test) {
			unpredicated([&] { emit(add, slot_dest(frame.broken), {slot_source(frame.broken), p}); });
			set_predicate([&](const Dest &target) { emit(mov, target, {zero()}); });
			return;
		}
		const Dest taken = scratch_dest(1);
		unpredicated([&] {
			if (nonzero)
				emit(mul, taken, {p, *test});
			else {
				const Dest inverse = scratch_dest(1);
				emit(add, inverse, {one(), test->negated()});
				emit(mul, taken, {p, from(inverse).component(0)});
			}
			emit(add, slot_dest(frame.broken), {slot_source(frame.broken), from(taken).component(0)});
		});
		// The lanes that broke stop: the predicate loses them.
		set_predicate([&](const Dest &target) { emit(add, target, {p, from(taken).component(0).negated()}); });
	}

	// The loop the generator puts around unrolled passes, which a break leaves: it runs once.
	bool runs_once(size_t start) const
	{
		int depth = 0;
		for (size_t i = start; i < program.size(); ++i) {
			const auto opcode = program[i]->opcode;
			if (opcode == sm4::loop)
				++depth;
			else if (opcode == sm4::endloop && --depth == 0)
				return i > start && program[i - 1]->opcode == sm4::op_break;
		}
		return false;
	}

	// ---- instructions ----

	static size_t destination_count(uint16_t opcode)
	{
		return opcode == sm4::imul || opcode == sm4::umul || opcode == sm4::udiv || opcode == sm4::sincos ? 2 : 1;
	}

	// An instruction that becomes several reads a source after it has written part of its destination: when the two are one register,
	// the result goes to a temp of its own first.
	void lower_apart(const sm4::Instruction &instruction)
	{
		static const std::set<uint16_t> single{sm4::mov, sm4::add, sm4::iadd, sm4::mul, sm4::mad, sm4::imad, sm4::umad, sm4::min, sm4::max,
			sm4::imin, sm4::imax, sm4::umin, sm4::umax, sm4::frc, sm4::dp3, sm4::dp4, sm4::itof, sm4::utof, sm4::ineg};
		const auto &ops = instruction.operands;
		const size_t count = std::min(destination_count(instruction.opcode), ops.size());
		if (single.count(instruction.opcode) || instruction.opcode == sm4::sample || instruction.opcode == sm4::sample_b ||
			instruction.opcode == sm4::sample_l || instruction.opcode == sm4::sample_d) {
			lower(instruction);
			return;
		}
		const auto reads = [&](uint32_t reg) {
			for (size_t i = count; i < ops.size(); ++i) {
				if (ops[i].type == sm4::temp && ops[i].index[0] == reg)
					return true;
				for (const auto &relative : ops[i].relative)
					if (relative && relative->type == sm4::temp && relative->index == reg)
						return true;
			}
			return false;
		};
		sm4::Instruction copy = instruction;
		std::vector<std::pair<sm4::Operand, uint32_t>> finals;
		for (size_t i = 0; i < count; ++i)
			if (ops[i].type == sm4::temp && reads(ops[i].index[0])) {
				const uint32_t held = scratch();
				finals.emplace_back(ops[i], held);
				copy.operands[i].index[0] = held;
			}
		if (finals.empty()) {
			lower(instruction);
			return;
		}
		copy.saturate = false;
		lower(copy);
		for (const auto &[operand, held] : finals) {
			Source value;
			value.number = held;
			emit(mov, dest(operand, instruction.saturate), {value});
		}
	}

	void lower(const sm4::Instruction &instruction)
	{
		const auto &ops = instruction.operands;
		const auto target = [&](size_t i) { return dest(ops[i], instruction.saturate); };
		const auto mask_of = [&](size_t i) -> uint8_t { return ops[i].type == sm4::null_register ? 0 : (ops[i].components == 0 ? 0xf : ops[i].mask); };
		const auto src = [&](size_t i, uint8_t used) { return source(ops[i], used); };
		const uint16_t opcode = instruction.opcode;
		const bool test_nonzero = (instruction.controls & (1 << 7)) != 0;
		switch (opcode) {
		case sm4::mov:
		case sm4::itof:
		case sm4::utof: {
			const auto d = target(0);
			emit(mov, d, {src(1, mask_of(0))});
			return;
		}
		case sm4::add:
		case sm4::iadd:
			emit(add, target(0), {src(1, mask_of(0)), src(2, mask_of(0))});
			return;
		case sm4::mul:
			emit(mul, target(0), {src(1, mask_of(0)), src(2, mask_of(0))});
			return;
		case sm4::imul:
		case sm4::umul:
			if (const auto d = target(1))
				emit(mul, d, {src(2, mask_of(1)), src(3, mask_of(1))});
			return;
		case sm4::mad:
		case sm4::imad:
		case sm4::umad:
			emit(mad, target(0), {src(1, mask_of(0)), src(2, mask_of(0)), src(3, mask_of(0))});
			return;
		case sm4::min:
		case sm4::imin:
		case sm4::umin:
			emit(min, target(0), {src(1, mask_of(0)), src(2, mask_of(0))});
			return;
		case sm4::max:
		case sm4::imax:
		case sm4::umax:
			emit(max, target(0), {src(1, mask_of(0)), src(2, mask_of(0))});
			return;
		case sm4::ineg:
			emit(mov, target(0), {src(1, mask_of(0)).negated()});
			return;
		case sm4::frc:
			emit(frc, target(0), {src(1, mask_of(0))});
			return;
		case sm4::dp3:
		case sm4::dp4:
			emit(opcode == sm4::dp3 ? dp3 : dp4, target(0), {src(1, 0xf), src(2, 0xf)});
			return;
		case sm4::dp2: {
			const auto d = *target(0);
			if (pixel) {
				emit(dp2add, d, {src(1, 3), src(2, 3), zero()});
				return;
			}
			const Dest product = scratch_dest(3);
			emit(mul, product, {src(1, 3), src(2, 3)});
			emit(add, d, {from(product).component(0), from(product).component(1)});
			return;
		}
		case sm4::rcp:
		case sm4::rsq:
		case sm4::exp:
		case sm4::log:
			per_component(opcode == sm4::rcp ? rcp : opcode == sm4::rsq ? rsq : opcode == sm4::exp ? exp : log, *target(0), src(1, mask_of(0)));
			return;
		case sm4::sqrt: {
			const Dest d = *target(0);
			const Dest inverse = scratch_dest(d.mask);
			per_component(rsq, inverse, src(1, d.mask));
			per_component(rcp, d, from(inverse));
			return;
		}
		case sm4::div: {
			const Dest d = *target(0);
			const Dest inverse = scratch_dest(d.mask);
			per_component(rcp, inverse, src(2, d.mask));
			emit(mul, d, {src(1, d.mask), from(inverse)});
			return;
		}
		case sm4::udiv: {
			const uint8_t mask = uint8_t(mask_of(0) | mask_of(1));
			const Source a = src(2, mask), b = src(3, mask);
			const Dest inverse = scratch_dest(mask), part = scratch_dest(mask), quotient = scratch_dest(mask);
			per_component(rcp, inverse, b);
			// (a + 0.5) / b lies strictly between the quotient and the next integer.
			emit(add, part, {a, scalar(0.5f)});
			emit(mul, part, {from(part), from(inverse)});
			floor_of(quotient, from(part));
			if (const auto q = target(0))
				emit(mov, q, {from(quotient)});
			if (const auto r = target(1))
				emit(mad, r, {from(quotient).negated(), b, a});
			return;
		}
		case sm4::ftoi:
		case sm4::ftou:
		case sm4::round_z:
			truncate(*target(0), src(1, mask_of(0)));
			return;
		case sm4::round_ni:
			floor_of(*target(0), src(1, mask_of(0)));
			return;
		case sm4::round_pi:
			ceil_of(*target(0), src(1, mask_of(0)));
			return;
		case sm4::round_ne: {
			const Dest d = *target(0);
			const Dest shifted = scratch_dest(d.mask);
			emit(add, shifted, {src(1, d.mask), scalar(0.5f)});
			floor_of(d, from(shifted));
			return;
		}
		case sm4::ishl:
		case sm4::ishr:
		case sm4::ushr: {
			const Dest d = *target(0);
			std::array<float, 4> factors{};
			for (uint8_t c = 0; c < 4; ++c) {
				const auto bits = immediate_bits(ops[2], c);
				if (!bits)
					fail("shifts by an amount found at run time need shader model 4");
				factors[c] = std::ldexp(1.0f, opcode == sm4::ishl ? int(*bits & 31) : -int(*bits & 31));
			}
			if (opcode == sm4::ishl) {
				emit(mul, d, {src(1, d.mask), def_values(factors, d.mask)});
				return;
			}
			const Dest scaled = scratch_dest(d.mask);
			emit(mul, scaled, {src(1, d.mask), def_values(factors, d.mask)});
			floor_of(d, from(scaled));
			return;
		}
		case sm4::lt:
		case sm4::ge:
		case sm4::eq:
		case sm4::ne:
		case sm4::ilt:
		case sm4::ige:
		case sm4::ieq:
		case sm4::ine:
		case sm4::ult:
		case sm4::uge:
			compare(opcode, *target(0), src(1, mask_of(0)), src(2, mask_of(0)));
			return;
		case sm4::movc:
			select(*target(0), src(1, mask_of(0)), src(2, mask_of(0)), src(3, mask_of(0)));
			return;
		case sm4::op_and:
		case sm4::op_or:
		case sm4::op_xor:
			logical(instruction);
			return;
		case sm4::op_not:
			emit(add, target(0), {one(), src(1, mask_of(0)).negated()});
			return;
		case sm4::sincos:
			sine_cosine(instruction);
			return;
		case sm4::deriv_rtx:
		case sm4::deriv_rty:
		case sm4::deriv_rtx_coarse:
		case sm4::deriv_rtx_fine:
		case sm4::deriv_rty_coarse:
		case sm4::deriv_rty_fine: {
			if (!pixel || !(model3 || in.target == "ps_2_a"))
				fail("ddx and ddy need ps_2_a or ps_3_0");
			const bool x = opcode == sm4::deriv_rtx || opcode == sm4::deriv_rtx_coarse || opcode == sm4::deriv_rtx_fine;
			emit(x ? dsx : dsy, target(0), {src(1, mask_of(0))});
			return;
		}
		case sm4::sample:
		case sm4::sample_b:
		case sm4::sample_l:
		case sm4::sample_d:
			sample(instruction);
			return;
		case sm4::discard:
			kill(src(0, 1), test_nonzero);
			return;
		case sm4::op_if:
			if (model3)
				emit(ifc, std::nullopt, {src(0, 1), zero()}, test_nonzero ? not_equal : equal);
			else
				flat_if(src(0, 1), test_nonzero);
			return;
		case sm4::op_else:
			if (model3)
				emit(op_else, std::nullopt, {});
			else
				flat_else();
			return;
		case sm4::endif:
			if (model3)
				emit(endif, std::nullopt, {});
			else
				flat_end();
			return;
		case sm4::loop:
			if (model3 && runs_once(at)) {
				once = true;
				emit(rep, std::nullopt, {register_source(constint, 1)});
			} else if (model3) {
				loops = true;
				emit(rep, std::nullopt, {register_source(constint, 0)});
			} else if (runs_once(at))
				flat_block();
			else
				fail("loops that cannot be unrolled need shader model 3");
			return;
		case sm4::endloop:
		case sm4::endswitch:
			if (model3)
				emit(endrep, std::nullopt, {});
			else
				flat_end();
			return;
		case sm4::op_switch:
			// Only the blocks the generator puts around what returns or continues from inside flow control, and around a switch, which run
			// once.
			if (at + 1 >= program.size() || program[at + 1]->opcode != sm4::op_default)
				fail("switch statements need shader model 4");
			++at;
			if (model3) {
				once = true;
				emit(rep, std::nullopt, {register_source(constint, 1)});
			} else
				flat_block();
			return;
		case sm4::op_break:
			if (model3)
				emit(op_break, std::nullopt, {});
			else
				flat_break(std::nullopt, true);
			return;
		case sm4::breakc:
			if (model3)
				emit(breakc, std::nullopt, {src(0, 1), zero()}, test_nonzero ? not_equal : equal);
			else
				flat_break(src(0, 1), test_nonzero);
			return;
		case sm4::ret:
			if (at + 1 != program.size())
				fail("a return inside flow control needs shader model 4");
			return;
		case sm4::op_continue:
		case sm4::continuec:
			fail("continue needs shader model 4");
		default:
			fail("this operation needs shader model 4");
		}
	}

	void logical(const sm4::Instruction &instruction)
	{
		const auto &ops = instruction.operands;
		const Dest d = *dest(ops[0]);
		const uint16_t opcode = instruction.opcode;
		// Booleans hold 0 or 1 here: masking one with true or with 1 leaves it as it is.
		for (size_t side = 1; side <= 2; ++side) {
			const auto bits = immediate_bits(ops[side], 0);
			if (!bits)
				continue;
			const size_t other = 3 - side;
			const bool all_same = ops[side].components == 1 ||
				std::all_of(ops[side].immediate.begin(), ops[side].immediate.end(), [&](uint32_t b) { return b == *bits; });
			if (!all_same)
				break;
			const bool truth = *bits == ~0u || *bits == 1 || *bits == std::bit_cast<uint32_t>(1.0f);
			if (*bits != 0 && !truth)
				break;
			if (opcode == sm4::op_and)
				emit(mov, d, {truth ? source(ops[other], d.mask) : zero()});
			else if (opcode == sm4::op_or)
				emit(mov, d, {truth ? one() : source(ops[other], d.mask)});
			else if (truth)
				emit(add, d, {one(), source(ops[other], d.mask).negated()});
			else
				emit(mov, d, {source(ops[other], d.mask)});
			return;
		}
		if (ops[1].type == sm4::immediate32 || ops[2].type == sm4::immediate32)
			fail("bitwise operations need shader model 4");
		const Source a = source(ops[1], d.mask), b = source(ops[2], d.mask);
		if (opcode == sm4::op_and)
			emit(mul, d, {a, b});
		else if (opcode == sm4::op_or)
			emit(max, d, {a, b});
		else {
			const Dest difference = scratch_dest(d.mask);
			emit(add, difference, {a, b.negated()});
			emit(mul, d, {from(difference), from(difference)});
		}
	}

	void sine_cosine(const sm4::Instruction &instruction)
	{
		const auto &ops = instruction.operands;
		const auto sine = dest(ops[0]), cosine = dest(ops[1]);
		const uint8_t mask = uint8_t((sine ? sine->mask : 0) | (cosine ? cosine->mask : 0));
		const Source value = source(ops[2], mask);
		constexpr float pi = 3.14159265358979f;
		for (uint8_t c = 0; c < 4; ++c) {
			if (!(mask & (1 << c)))
				continue;
			// The argument brought into [-pi, pi], which sincos expects.
			const Dest reduced = scratch_dest(1);
			emit(mad, reduced, {value.component(c), scalar(0.5f / pi), scalar(0.5f)});
			emit(frc, reduced, {from(reduced).component(0)});
			emit(mad, reduced, {from(reduced).component(0), scalar(2 * pi), scalar(-pi)});
			const Dest both = scratch_dest(3);
			if (model3)
				emit(sincos, both, {from(reduced).component(0)});
			else
				emit(sincos, both, {from(reduced).component(0), def_vector({-1.5500992e-6f, -2.1701389e-5f, 0.0026041667f, 0.00026041668f}),
					def_vector({-0.020833334f, -0.125f, 1.0f, 0.5f})});
			if (sine && (sine->mask & (1 << c))) {
				Dest part = *sine;
				part.mask = uint8_t(1 << c);
				emit(mov, part, {from(both).component(1)});
			}
			if (cosine && (cosine->mask & (1 << c))) {
				Dest part = *cosine;
				part.mask = uint8_t(1 << c);
				emit(mov, part, {from(both).component(0)});
			}
		}
	}

	void sample(const sm4::Instruction &instruction)
	{
		const auto &ops = instruction.operands;
		const uint16_t opcode = instruction.opcode;
		if (instruction.offset)
			fail("texture offsets need shader model 4");
		const uint32_t texture_register = ops[2].index[0];
		const uint32_t sampler_register = ops[3].index[0];
		const uint32_t dimension = dimensions.count(texture_register) ? dimensions[texture_register] : 3;
		// D3DSAMPLER_TEXTURE_TYPE: 2D (which 1D textures use too), cube or volume.
		const uint32_t type = dimension == 6 ? 3 : dimension == 5 ? 4 : dimension == 2 || dimension == 3 ? 2 : 0;
		if (!type)
			fail("this texture needs shader model 4");
		sampler_types[sampler_register] = type;
		if (!pixel && opcode != sm4::sample_l)
			fail("a vertex shader samples with a level, as tex2Dlod does");
		if (!pixel && !model3)
			fail("vertex shaders sample textures from vs_3_0");
		if (opcode == sm4::sample_l && !model3)
			fail("sampling at a level needs shader model 3");
		if (opcode == sm4::sample_d && !(model3 || in.target == "ps_2_a"))
			fail("sampling with gradients needs ps_2_a or ps_3_0");
		Source coordinates = source(ops[1], 0xf);
		if (opcode == sm4::sample_b || opcode == sm4::sample_l) {
			// The bias or level goes in w.
			const Dest held = scratch_dest(7);
			emit(mov, held, {coordinates});
			Dest w = held;
			w.mask = 8;
			emit(mov, w, {source(ops[4], 1).component(0)});
			coordinates = from(held);
		} else if (!model3 && (coordinates.type != temp && coordinates.type != texture)) {
			const Dest held = scratch_dest();
			emit(mov, held, {coordinates});
			coordinates = from(held);
		}
		Source state;
		state.type = sampler;
		state.number = sampler_register;
		const Dest result = scratch_dest();
		if (opcode == sm4::sample_d)
			emit(texldd, result, {coordinates, state, source(ops[4], 0xf), source(ops[5], 0xf)});
		else if (opcode == sm4::sample_l)
			emit(texldl, result, {coordinates, state});
		else
			emit(tex, result, {coordinates, state}, opcode == sm4::sample_b ? 2 : 0);
		// The resource operand's swizzle says which result component each destination component takes.
		Source value = from(result);
		value.swizzle = ops[2].swizzle;
		emit(mov, dest(ops[0], instruction.saturate), {value});
	}

	// texkill drops the pixel when a component is below zero.
	void kill(const Source &test, bool nonzero)
	{
		const Dest value = scratch_dest();
		Source condition = test;
		if (!nonzero) {
			const Dest inverse = scratch_dest(1);
			emit(add, inverse, {one(), test.negated()});
			condition = from(inverse).component(0);
		}
		if (predicate) {
			const Dest both = scratch_dest(1);
			unpredicated([&] { emit(mul, both, {predicate_source(), condition}); });
			condition = from(both).component(0);
		}
		unpredicated([&] {
			emit(mov, value, {condition.negated()});
			put(texkill, value, {}, 0);
		});
	}

	// ---- program parts ----

	// Whether each component of the destination takes the same component of every source, so that moving the destination to
	// another component moves what the sources supply with it.
	static bool componentwise(uint32_t opcode)
	{
		switch (opcode) {
		case mov:
		case add:
		case mad:
		case mul:
		case min:
		case max:
		case slt:
		case sge:
		case lrp:
		case frc:
		case abs:
		case cmp:
		case dsx:
		case dsy:
			return true;
		default:
			return false;
		}
	}

	// The components of a source an instruction reads.
	static uint8_t read_components(uint32_t opcode, size_t parameter, uint32_t token, uint8_t written)
	{
		uint8_t swizzle[4];
		for (int c = 0; c < 4; ++c)
			swizzle[c] = uint8_t(token >> (16 + 2 * c) & 3);
		uint8_t count = 4;
		if (componentwise(opcode)) {
			uint8_t read = 0;
			for (int c = 0; c < 4; ++c)
				if (written & (1 << c))
					read |= uint8_t(1 << swizzle[c]);
			return read;
		}
		if (opcode == dp3)
			count = 3;
		else if (opcode == dp2add && parameter < 3)
			count = 2;
		uint8_t read = 0;
		for (int c = 0; c < count; ++c)
			read |= uint8_t(1 << swizzle[c]);
		return read;
	}

	// Temp registers renumbered over the finished program so that registers whose lifetimes do not overlap share one; a register used
	// inside a loop that repeats lives through the whole loop. A register only ever used through one component takes a component of
	// its own, so that scalars share registers. Returns how many there are.
	uint32_t compact_temps()
	{
		struct Use {
			size_t index;
			uint32_t *token;
			// The destination's token, for a source of an instruction that works component by component.
			uint32_t *dest;
			bool written;
			uint8_t components;
		};
		std::vector<Use> uses;
		std::vector<std::pair<size_t, size_t>> loops_at;
		std::vector<size_t> open;
		for (size_t at = 0, index = 0; at < body.size(); ++index) {
			const uint32_t opcode = body[at] & 0xffff, length = body[at] >> 24 & 0xf;
			// A rep over i1 runs once, the block a break leaves: nothing in it lives on into another pass.
			if (opcode == rep)
				open.push_back((body[at + 1] & 0x7ff) == 1 ? SIZE_MAX : index);
			else if (opcode == endrep && !open.empty()) {
				if (open.back() != SIZE_MAX)
					loops_at.emplace_back(open.back(), index);
				open.pop_back();
			}
			const bool has_dest = length && opcode != ifc && opcode != breakc && opcode != rep && opcode != texkill;
			uint32_t *dest = has_dest ? &body[at + 1] : nullptr;
			const uint8_t written = dest ? uint8_t(*dest >> 16 & 0xf) : 0xf;
			for (uint32_t k = 1, parameter = 0; k <= length; ++k, ++parameter) {
				uint32_t &token = body[at + k];
				const uint32_t type = (token >> 28 & 7) | (token >> 8 & 0x18);
				const bool is_dest = has_dest && k == 1;
				if (type == temp) {
					Use use{index, &token, nullptr, is_dest, 0xf};
					if (is_dest)
						use.components = written;
					else if (opcode != texkill) {
						use.components = read_components(opcode, parameter, token, written);
						if (dest && componentwise(opcode))
							use.dest = dest;
					}
					uses.push_back(use);
				}
				// A relative source is followed by its address token.
				if (!is_dest && (token & 0x2000))
					++k;
			}
			at += 1 + length;
		}
		const uint32_t total = std::max(scratch_high, in.temp_count);
		std::vector<int64_t> first(total, -1), last(total, -1);
		std::vector<uint8_t> touched(total, 0);
		for (const auto &use : uses) {
			const uint32_t reg = *use.token & 0x7ff;
			if (first[reg] < 0)
				first[reg] = int64_t(use.index);
			last[reg] = int64_t(use.index);
			touched[reg] |= use.components;
		}
		for (bool grew = true; grew;) {
			grew = false;
			for (const auto &[start, end] : loops_at)
				for (uint32_t reg = 0; reg < total; ++reg)
					if (first[reg] >= 0 && first[reg] <= int64_t(end) && last[reg] >= int64_t(start) &&
						(first[reg] > int64_t(start) || last[reg] < int64_t(end))) {
						first[reg] = std::min(first[reg], int64_t(start));
						last[reg] = std::max(last[reg], int64_t(end));
						grew = true;
					}
		}
		std::vector<uint32_t> order;
		for (uint32_t reg = 0; reg < total; ++reg)
			if (first[reg] >= 0)
				order.push_back(reg);
		std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return first[a] < first[b]; });
		const auto single = [&](uint32_t reg) { return std::popcount(touched[reg]) == 1; };
		std::vector<std::array<int64_t, 4>> busy_until;
		std::vector<uint32_t> renamed(total, 0);
		std::vector<int> moved(total, -1);
		for (const uint32_t reg : order) {
			const auto free_at = [&](size_t slot, int c) { return busy_until[slot][size_t(c)] < first[reg]; };
			size_t slot = 0;
			int component = -1;
			if (single(reg)) {
				for (; slot < busy_until.size() && component < 0; ++slot)
					for (int c = 0; c < 4 && component < 0; ++c)
						if (free_at(slot, c))
							component = c;
				if (component >= 0)
					--slot;
			} else
				while (slot < busy_until.size() && !(free_at(slot, 0) && free_at(slot, 1) && free_at(slot, 2) && free_at(slot, 3)))
					++slot;
			if (slot == busy_until.size())
				busy_until.push_back({-1, -1, -1, -1});
			if (single(reg)) {
				if (component < 0)
					component = 0;
				busy_until[slot][size_t(component)] = last[reg];
				moved[reg] = component;
			} else
				busy_until[slot].fill(last[reg]);
			renamed[reg] = uint32_t(slot);
		}
		// A destination moving to another component takes the sources' swizzles along, before the sources move themselves.
		for (const auto &use : uses) {
			if (!use.written)
				continue;
			const uint32_t reg = *use.token & 0x7ff;
			if (moved[reg] < 0)
				continue;
			const int from_component = std::countr_zero(touched[reg]);
			const int to_component = moved[reg];
			for (const auto &other : uses)
				if (other.dest == use.token && other.token != use.token) {
					const uint32_t entry = *other.token >> (16 + 2 * from_component) & 3;
					*other.token = (*other.token & ~(3u << (16 + 2 * to_component))) | entry << (16 + 2 * to_component);
				}
		}
		for (const auto &use : uses) {
			uint32_t &token = *use.token;
			const uint32_t reg = token & 0x7ff;
			if (moved[reg] >= 0) {
				const uint32_t c = uint32_t(moved[reg]);
				if (use.written)
					token = (token & ~(0xfu << 16)) | (1u << c) << 16;
				else
					token = (token & ~(0xffu << 16)) | (c | c << 2 | c << 4 | c << 6) << 16;
			}
			token = (token & ~0x7ffu) | renamed[reg];
		}
		return uint32_t(busy_until.size());
	}

	void declarations(std::vector<uint32_t> &tokens)
	{
		const auto instruction = [&](uint16_t opcode, std::vector<uint32_t> params) {
			tokens.push_back(uint32_t(opcode) | uint32_t(params.size()) << 24);
			tokens.insert(tokens.end(), params.begin(), params.end());
		};
		for (size_t i = 0; i < icb.size() / 4; ++i) {
			Dest target;
			target.type = constant;
			target.number = icb_base + uint32_t(i);
			instruction(def, {encode(target), icb[i * 4], icb[i * 4 + 1], icb[i * 4 + 2], icb[i * 4 + 3]});
		}
		for (size_t i = 0; i < defs.size(); ++i) {
			Dest target;
			target.type = constant;
			target.number = def_base + uint32_t(i);
			const auto &v = defs[i].values;
			instruction(def, {encode(target), std::bit_cast<uint32_t>(v[0]), std::bit_cast<uint32_t>(v[1]), std::bit_cast<uint32_t>(v[2]),
				std::bit_cast<uint32_t>(v[3])});
		}
		if (loops)
			instruction(defi, {encode(register_dest(constint, 0)), 255, 0, 1, 0});
		if (once)
			instruction(defi, {encode(register_dest(constint, 1)), 1, 0, 0, 0});
		for (const auto &item : declared) {
			Dest target;
			target.type = item.type;
			target.number = item.number;
			target.mask = item.mask;
			target.centroid = item.centroid;
			instruction(dcl, {item.plain ? 0x80000000u : 0x80000000u | item.usage | item.index << 16, encode(target)});
		}
		for (const auto &element : in.inputs) {
			const auto mapped = inputs.find(element.reg);
			if (!pixel || !element.used || mapped == inputs.end() || mapped->second.type != misc)
				continue;
			Dest target;
			target.type = misc;
			target.number = mapped->second.number;
			target.mask = target.number == 0 ? 3 : 0xf;
			instruction(dcl, {0x80000000u, encode(target)});
		}
		for (const auto &[number, type] : sampler_types) {
			Dest target;
			target.type = sampler;
			target.number = number;
			instruction(dcl, {0x80000000u | type << 27, encode(target)});
		}
	}

	// The CTAB comment: D3DXSHADER_CONSTANTTABLE, its constants, their types and names.
	void constant_table(std::vector<uint32_t> &tokens, uint32_t version)
	{
		struct Entry {
			std::string name;
			uint16_t set = 2, index = 0, count = 0;
			ReflectedType type;
		};
		std::vector<Entry> entries;
		for (const auto &binding : in.bindings) {
			if (binding.buffer < 0)
				continue;
			const auto &buffer = in.buffers[size_t(binding.buffer)];
			for (const auto &variable : buffer.variables) {
				if (!variable.used)
					continue;
				Entry entry;
				entry.name = variable.name;
				entry.index = uint16_t(constant_place.at({binding.bind_point, variable.offset / 16}));
				entry.count = uint16_t((variable.size + 15) / 16);
				entry.type = variable.type;
				entries.push_back(entry);
			}
		}
		for (const auto &binding : in.bindings) {
			if (binding.input_type != 3 || !sampler_types.count(binding.bind_point))
				continue;
			Entry entry;
			entry.name = binding.name;
			entry.set = 3;
			entry.index = uint16_t(binding.bind_point);
			entry.count = uint16_t(binding.bind_count);
			entry.type.type_class = 4;
			const uint32_t type = sampler_types[binding.bind_point];
			entry.type.type = uint16_t(type == 3 ? 14 : type == 4 ? 13 : 12);
			entry.type.rows = entry.type.columns = 1;
			entries.push_back(entry);
		}
		std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });

		std::vector<uint8_t> data;
		const auto u16 = [&](size_t at, uint16_t value) {
			data[at] = uint8_t(value);
			data[at + 1] = uint8_t(value >> 8);
		};
		const auto u32 = [&](size_t at, uint32_t value) {
			for (int b = 0; b < 4; ++b)
				data[at + size_t(b)] = uint8_t(value >> (8 * b));
		};
		const auto reserve = [&](size_t bytes) {
			const size_t at = data.size();
			data.resize(at + bytes);
			return at;
		};
		const auto text = [&](const std::string &value) {
			const size_t at = data.size();
			data.insert(data.end(), value.begin(), value.end());
			data.push_back(0);
			return uint32_t(at);
		};
		const size_t header = reserve(28);
		const size_t infos = reserve(20 * entries.size());
		std::function<uint32_t(const ReflectedType &)> type_info = [&](const ReflectedType &type) -> uint32_t {
			std::vector<std::pair<uint32_t, uint32_t>> members;
			for (const auto &[name, member] : type.members)
				members.emplace_back(text(name), type_info(member));
			while (data.size() % 4)
				data.push_back(0);
			size_t member_info = 0;
			if (!members.empty()) {
				member_info = reserve(8 * members.size());
				for (size_t i = 0; i < members.size(); ++i) {
					u32(member_info + i * 8, members[i].first);
					u32(member_info + i * 8 + 4, members[i].second);
				}
			}
			const size_t at = reserve(16);
			u16(at, type.type_class);
			u16(at + 2, type.type);
			u16(at + 4, type.rows);
			u16(at + 6, type.columns);
			u16(at + 8, uint16_t(std::max<uint16_t>(1, type.elements)));
			u16(at + 10, uint16_t(members.size()));
			u32(at + 12, uint32_t(member_info));
			return uint32_t(at);
		};
		for (size_t i = 0; i < entries.size(); ++i) {
			const size_t at = infos + i * 20;
			u32(at, text(entries[i].name));
			while (data.size() % 4)
				data.push_back(0);
			u16(at + 4, entries[i].set);
			u16(at + 6, entries[i].index);
			u16(at + 8, entries[i].count);
			u32(at + 12, type_info(entries[i].type));
			u32(at + 16, 0);
		}
		u32(header, 28);
		u32(header + 4, text("Raven HLSL compiler"));
		u32(header + 8, version);
		u32(header + 12, uint32_t(entries.size()));
		u32(header + 16, uint32_t(infos));
		u32(header + 20, flags);
		u32(header + 24, text(in.target));
		while (data.size() % 4)
			data.push_back(0);
		const uint32_t length = uint32_t(data.size() / 4) + 1;
		tokens.push_back(0xfffe | length << 16);
		tokens.push_back(0x42415443);
		for (size_t i = 0; i < data.size(); i += 4)
			tokens.push_back(uint32_t(data[i]) | uint32_t(data[i + 1]) << 8 | uint32_t(data[i + 2]) << 16 | uint32_t(data[i + 3]) << 24);
	}
};

}

std::vector<uint32_t> legacy_program(const Generated &generated, uint32_t flags)
{
	return Legacy(generated, flags).run();
}

}
