#include "hlsl_codegen.h"
#include "hlsl_compile.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace raven::shader {

namespace {

using sm4::Instruction;
using sm4::Operand;

[[noreturn]] void fail(uint32_t line, std::string message)
{
	throw GenerateError{line, std::move(message)};
}

bool is_float(Scalar kind)
{
	return kind == Scalar::floating || kind == Scalar::half || kind == Scalar::doubled;
}

bool is_signed(Scalar kind)
{
	return kind == Scalar::integer;
}

// The order in which mixed operands promote: bool, int, uint, then the floating kinds.
int rank(Scalar kind)
{
	switch (kind) {
	case Scalar::boolean: return 0;
	case Scalar::integer: return 1;
	case Scalar::unsigned_integer: return 2;
	case Scalar::half: return 3;
	case Scalar::floating: return 4;
	case Scalar::doubled: return 5;
	default: return -1;
	}
}

uint32_t float_bits(float value)
{
	return std::bit_cast<uint32_t>(value);
}

float bits_float(uint32_t bits)
{
	return std::bit_cast<float>(bits);
}

uint32_t element_count(const Type &type)
{
	uint32_t count = 1;
	for (auto size : type.array)
		count *= size;
	return count;
}

Type element_of(const Type &type)
{
	Type element = type;
	if (!element.array.empty())
		element.array.erase(element.array.begin());
	return element;
}

// How many scalar components a value of the type holds, flattened in declaration order (matrices by rows).
uint32_t flat_size(const Type &type)
{
	uint32_t base = 0;
	switch (type.shape) {
	case Type::Shape::scalar:
	case Type::Shape::vector:
	case Type::Shape::matrix:
		base = uint32_t(type.rows) * type.columns;
		if (type.shape == Type::Shape::scalar)
			base = 1;
		break;
	case Type::Shape::structure:
		for (const auto &member : type.structure->members)
			base += flat_size(member.type);
		break;
	default:
		break;
	}
	return base * element_count(type);
}

// One scalar component of a value: a register component, or an immediate value.
struct Loc {
	Operand base;
	uint8_t component = 0;
	bool immediate = false;
	uint32_t bits = 0;
};

bool same_register(const Operand &left, const Operand &right)
{
	if (left.type != right.type || left.dimension != right.dimension)
		return false;
	for (uint32_t i = 0; i < left.dimension; ++i)
		if (left.index[i] != right.index[i] || left.relative[i] != right.relative[i])
			return false;
	return true;
}

Loc constant_loc(uint32_t bits)
{
	Loc loc;
	loc.immediate = true;
	loc.bits = bits;
	loc.base = Operand::values({}, 1);
	return loc;
}

// The value of an expression or the storage of a variable.
// An element of a buffer, texture or group shared array: read with a load, written with a store.
struct Access {
	enum class Kind : uint8_t { typed, structured, shared } kind = Kind::typed;
	// The resource, or the group shared variable.
	int target = -1;
	uint32_t element = 0;
	bool uav = false;
	// Typed: the coordinates. Structured and shared: the element index.
	std::vector<Loc> address;
	// Structured and shared: the byte offset of every scalar of the value inside the buffer element.
	std::vector<uint32_t> offsets;
	// Typed textures read with ld take a mip level after the coordinates.
	bool mip = false;
	// A group shared array before its index is known: indexing it sets the address.
	bool outer_array = false;
	// Structured and shared: a byte offset known only at run time, from a dynamic index into an array inside the element.
	std::optional<Loc> shift;
};

struct Ref {
	Type type;
	std::vector<Loc> locs;
	// A texture, buffer or sampler: the index of its resource and, for an array of them, the element.
	int resource = -1;
	uint32_t resource_element = 0;
	bool writable = false;
	// Registers between consecutive elements of an array whose elements sit at regular register steps; 0 when they do not.
	uint32_t element_registers = 0;
	// For a stream output object of a geometry shader: the stream number.
	int stream = -1;
	std::shared_ptr<Access> access;
	// A static const array held in the immediate constant buffer from this register, for dynamic indexing.
	int icb = -1;

	bool assignable() const { return access ? access->uav || access->kind == Access::Kind::shared : writable; }
};

struct Resource {
	std::string name;
	ObjectKind kind = ObjectKind::none;
	Type element;
	// 't', 's' or 'u'.
	char register_class = 't';
	int explicit_register = -1;
	int bound = -1;
	uint32_t count = 1;
	bool used = false;
	uint32_t stride = 0;
	bool comparison = false;
	uint32_t samples = 0;
	bool counter = false;
	// The texture register of a tbuffer, which the tbuffer itself reflects.
	bool texture_buffer = false;
};

struct BufferVariable {
	std::string name;
	Type type;
	uint32_t offset = 0, size = 0;
	bool used = false;
	std::vector<uint32_t> defaults;
};

struct Buffer {
	std::string name;
	bool texture = false;
	int explicit_register = -1;
	int bound = -1;
	bool used = false;
	uint32_t size = 0;
	std::vector<BufferVariable> variables;
	// Registers read with a dynamic index.
	bool dynamic = false;
	// A tbuffer: the resource its registers are loaded from.
	int resource = -1;
};

struct Loop {
	// The step of a for loop, generated again before each continue.
	const Expr *step = nullptr;
	// The condition of a do loop, tested again before each continue.
	const Expr *until = nullptr;
	bool is_switch = false;
};

struct Frame {
	const Function *function = nullptr;
	Ref result;
	bool entry = false;
	// A function returning from inside flow control runs in a block that a return leaves, with this flag set for the loops around.
	std::optional<uint32_t> returned;
	size_t loop_depth = 0;
	// A method: the object it was called on.
	std::optional<Ref> self;
};


}

class Generator {
public:
	Generator(const Program &program, uint32_t flags) : program(program), flags(flags) {}

	Generated run(const std::string &entry, const std::string &target);

	// Set on a second attempt after code needed a constant where a loop counter stood: every loop that can be unrolled is.
	bool unroll_all = false;

private:
	const Program &program;
	uint32_t flags;
	Generated out;
	Stage stage = Stage::pixel;
	bool sm5 = false;
	std::vector<Instruction> code;
	uint32_t temps = 0;
	// Indexable temps: register count and components of each.
	std::vector<std::pair<uint32_t, uint8_t>> indexables;
	std::vector<std::array<uint32_t, 4>> icb;
	std::vector<Resource> resources;
	std::vector<Buffer> buffers;
	// Group shared variables: name, bytes per element and element count.
	struct Shared {
		std::string name;
		uint32_t stride = 0, count = 0;
	};
	std::vector<Shared> shared;
	std::map<std::string, Ref> globals;
	// Uniform parameters of the entry point, which live in the $Params constant buffer.
	std::deque<Variable> parameter_uniforms;
	// The constant buffer and variable a global name stands for.
	std::map<std::string, std::pair<int, int>> buffer_names;
	std::vector<std::map<std::string, Ref>> scopes;
	std::vector<Frame> frames;
	std::vector<Loop> loops;
	std::set<const Function *> active;
	std::string warnings;
	// What the entry point writes: a stage output register component and the temp component holding its value.
	std::vector<std::pair<Loc, Loc>> output_copies;
	bool uses_discard = false;

	// ---- registers and instructions ----

	uint32_t new_register() { return temps++; }

	// After an unconditional break, continue or return, code up to the end of the enclosing block cannot run and is dropped.
	bool unreachable = false;
	uint32_t dead_depth = 0;
	Instruction dropped;

	Instruction &emit(uint16_t opcode, std::vector<Operand> operands, uint32_t controls = 0)
	{
		const bool opens = opcode == sm4::op_if || opcode == sm4::loop || opcode == sm4::op_switch;
		const bool closes = opcode == sm4::endif || opcode == sm4::endloop || opcode == sm4::endswitch;
		const bool splits = opcode == sm4::op_else || opcode == sm4::op_case || opcode == sm4::op_default;
		if (unreachable) {
			if (opens)
				++dead_depth;
			else if (dead_depth && closes)
				--dead_depth;
			else if (!dead_depth && (closes || splits))
				unreachable = false;
			if (unreachable) {
				dropped = Instruction{};
				return dropped;
			}
		}
		if (opcode == sm4::op_break || opcode == sm4::op_continue || opcode == sm4::ret)
			unreachable = true;
		Instruction instruction;
		instruction.opcode = opcode;
		instruction.operands = std::move(operands);
		instruction.controls = controls;
		code.push_back(std::move(instruction));
		return code.back();
	}

	static Operand temp_dest(uint32_t reg, uint8_t mask)
	{
		Operand operand = Operand::reg(sm4::temp, reg);
		operand.mask = mask;
		return operand;
	}

	// The operand writing the given components of the register a loc names.
	static Operand dest_of(const Loc &loc, uint8_t mask)
	{
		Operand operand = loc.base;
		operand.components = 4;
		operand.mode = Operand::Mode::mask;
		operand.mask = mask;
		operand.negate = operand.absolute = false;
		return operand;
	}

	static bool one_register(const std::vector<Loc> &locs)
	{
		if (locs.empty())
			return true;
		if (std::all_of(locs.begin(), locs.end(), [](const Loc &loc) { return loc.immediate; }))
			return true;
		for (const auto &loc : locs)
			if (loc.immediate || !same_register(loc.base, locs[0].base))
				return false;
		return true;
	}

	// The source operand that gives destination component dest[k] the value of locs[k]; the locs share a register or are immediates.
	static Operand source_for(const std::vector<Loc> &locs, const std::vector<uint8_t> &dest)
	{
		if (locs.empty())
			return Operand::values({}, 1);
		if (locs[0].immediate) {
			std::array<uint32_t, 4> bits{};
			for (int i = 0; i < 4; ++i)
				bits[i] = locs[0].bits;
			for (size_t k = 0; k < locs.size(); ++k)
				bits[dest[k]] = locs[k].bits;
			if (std::all_of(locs.begin(), locs.end(), [&](const Loc &loc) { return loc.bits == locs[0].bits; }) && dest.size() == 1)
				return Operand::values(bits, 1);
			return Operand::values(bits, 4);
		}
		if (scalar_register(locs[0].base.type))
			return scalar_source(locs[0]);
		Operand operand = locs[0].base;
		operand.components = 4;
		operand.mode = Operand::Mode::swizzle;
		for (int i = 0; i < 4; ++i)
			operand.swizzle[i] = locs[0].component;
		for (size_t k = 0; k < locs.size(); ++k)
			operand.swizzle[dest[k]] = locs[k].component;
		return operand;
	}

	static std::vector<uint8_t> first_components(size_t count)
	{
		std::vector<uint8_t> dest(count);
		for (size_t i = 0; i < count; ++i)
			dest[i] = uint8_t(i);
		return dest;
	}

	static Operand source_first(const std::vector<Loc> &locs) { return source_for(locs, first_components(locs.size())); }

	// Registers with a single component: the flattened thread index, the primitive and instance ids, the input coverage.
	static bool scalar_register(uint8_t type)
	{
		return type == sm4::thread_id_in_group_flattened || type == sm4::input_primitive_id || type == sm4::gs_instance_id ||
			type == sm4::output_control_point_id || type == sm4::input_fork_instance_id || type == sm4::input_join_instance_id ||
			type == sm4::input_coverage || type == sm4::output_depth || type == sm4::output_coverage ||
			type == sm4::output_depth_greater_equal || type == sm4::output_depth_less_equal;
	}

	// A scalar operand: one component selected.
	static Operand scalar_source(const Loc &loc)
	{
		if (loc.immediate)
			return Operand::values({loc.bits, loc.bits, loc.bits, loc.bits}, 1);
		if (scalar_register(loc.base.type)) {
			Operand operand = loc.base;
			operand.components = 1;
			return operand;
		}
		Operand operand = loc.base;
		operand.components = 4;
		operand.mode = Operand::Mode::select;
		operand.swizzle[0] = loc.component;
		return operand;
	}

	std::vector<Loc> temp_locs(uint32_t reg, uint32_t count)
	{
		std::vector<Loc> locs;
		for (uint32_t i = 0; i < count; ++i) {
			Loc loc;
			loc.base = Operand::reg(sm4::temp, reg);
			loc.component = uint8_t(i);
			locs.push_back(loc);
		}
		return locs;
	}

	// Temp components for any number of scalars, four to a register.
	std::vector<Loc> scratch(uint32_t count)
	{
		std::vector<Loc> locs;
		while (locs.size() < count) {
			const auto part = temp_locs(new_register(), std::min<uint32_t>(4, count - uint32_t(locs.size())));
			locs.insert(locs.end(), part.begin(), part.end());
		}
		return locs;
	}

	// The type of one row of a matrix or the whole of a vector or scalar.
	static uint32_t row_count(const Type &type) { return type.shape == Type::Shape::matrix ? type.rows : 1; }
	static uint32_t row_width(const Type &type) { return type.shape == Type::Shape::matrix ? type.columns : type.components(); }

	static std::vector<Loc> row(const Ref &value, uint32_t index)
	{
		const uint32_t width = row_width(value.type);
		return std::vector<Loc>(value.locs.begin() + index * width, value.locs.begin() + (index + 1) * width);
	}

	// Locs that can be read by one operand: copied to a temp register when they are spread over several.
	std::vector<Loc> readable(const std::vector<Loc> &locs)
	{
		if (one_register(locs))
			return locs;
		const uint32_t reg = new_register();
		auto result = temp_locs(reg, uint32_t(locs.size()));
		write(result, locs);
		return result;
	}

	// Writes values into register components, one move per destination register.
	void write(const std::vector<Loc> &dest, const std::vector<Loc> &values)
	{
		// A write split into several moves must not read a component an earlier move of it changed.
		if (!one_register(values))
			for (const auto &target : dest)
				for (const auto &value : values)
					if (!value.immediate && same_register(target.base, value.base)) {
						const auto copy = scratch(uint32_t(values.size()));
						write(copy, values);
						write(dest, copy);
						return;
					}
		std::vector<bool> done(dest.size());
		for (size_t i = 0; i < dest.size(); ++i) {
			if (done[i])
				continue;
			std::vector<size_t> group;
			for (size_t j = i; j < dest.size(); ++j)
				if (!done[j] && same_register(dest[j].base, dest[i].base)) {
					group.push_back(j);
					done[j] = true;
				}
			// The sources of one move must share a register; split the group where they do not.
			while (!group.empty()) {
				std::vector<size_t> part{group[0]};
				std::vector<size_t> rest;
				for (size_t k = 1; k < group.size(); ++k) {
					std::vector<Loc> trial;
					for (auto index : part)
						trial.push_back(values[index]);
					trial.push_back(values[group[k]]);
					bool component_taken = false;
					for (auto index : part)
						if (dest[index].component == dest[group[k]].component)
							component_taken = true;
					if (one_register(trial) && !component_taken && (values[group[k]].immediate == values[group[0]].immediate))
						part.push_back(group[k]);
					else
						rest.push_back(group[k]);
				}
				uint8_t mask = 0;
				std::vector<Loc> sources;
				std::vector<uint8_t> components;
				for (auto index : part) {
					mask |= uint8_t(1 << dest[index].component);
					sources.push_back(values[index]);
					components.push_back(dest[index].component);
				}
				emit(sm4::mov, {dest_of(dest[part[0]], mask), source_for(sources, components)});
				group = rest;
			}
		}
	}

	// ---- types and values ----

	static Type scalar_type(Scalar kind) { return Type::make_scalar(kind); }

	static Type with_kind(Type type, Scalar kind)
	{
		type.scalar = kind;
		return type;
	}

	Ref constant(const Type &type, const std::vector<uint32_t> &bits)
	{
		Ref ref;
		ref.type = type;
		for (auto value : bits)
			ref.locs.push_back(constant_loc(value));
		return ref;
	}

	Ref float_constant(float value) { return constant(scalar_type(Scalar::floating), {float_bits(value)}); }
	Ref int_constant(int32_t value) { return constant(scalar_type(Scalar::integer), {uint32_t(value)}); }
	Ref uint_constant(uint32_t value) { return constant(scalar_type(Scalar::unsigned_integer), {value}); }

	static bool all_immediate(const Ref &value)
	{
		return !value.locs.empty() && std::all_of(value.locs.begin(), value.locs.end(), [](const Loc &loc) { return loc.immediate; });
	}

	Ref new_value(const Type &type)
	{
		Ref ref;
		ref.type = type;
		ref.writable = true;
		for (uint32_t r = 0; r < row_count(type); ++r) {
			const auto locs = temp_locs(new_register(), row_width(type));
			ref.locs.insert(ref.locs.end(), locs.begin(), locs.end());
		}
		return ref;
	}

	static bool holds_array(const Type &type)
	{
		if (!type.array.empty())
			return true;
		if (type.shape == Type::Shape::structure)
			for (const auto &member : type.structure->members)
				if (holds_array(member.type))
					return true;
		return false;
	}

	// Storage for a variable of any type: arrays, and structures holding them, in an indexable temp so that any index can reach them
	// (only x# registers take a register index).
	Ref storage(const Type &type)
	{
		Ref ref;
		ref.type = type;
		ref.writable = true;
		if (type.shape == Type::Shape::object)
			fail(0, "objects cannot be local variables here");
		if (holds_array(type)) {
			const Type element = type.array.empty() ? type : element_of(type);
			const uint32_t per = registers_of(element);
			const uint32_t index = uint32_t(indexables.size());
			const uint32_t count = type.array.empty() ? per : element_count(type) / element_count(element) * per;
			indexables.push_back({count, 4});
			uint32_t next = 0;
			layout(type, ref.locs, next, [index](uint32_t reg) {
				Operand operand = Operand::reg(sm4::indexable_temp, index);
				operand.dimension = 2;
				operand.index[1] = reg;
				return operand;
			});
			ref.element_registers = per;
			return ref;
		}
		uint32_t next = 0;
		const uint32_t base = temps;
		layout(type, ref.locs, next, [base](uint32_t reg) { return Operand::reg(sm4::temp, base + reg); });
		temps += next;
		return ref;
	}

	// The registers a temp of the type takes: one per vector, matrix row or member.
	static uint32_t registers_of(const Type &type)
	{
		uint32_t base = 0;
		switch (type.shape) {
		case Type::Shape::scalar:
		case Type::Shape::vector:
			base = 1;
			break;
		case Type::Shape::matrix:
			base = type.rows;
			break;
		case Type::Shape::structure:
			for (const auto &member : type.structure->members)
				base += registers_of(member.type);
			break;
		default:
			break;
		}
		return base * element_count(type);
	}

	static void layout(const Type &type, std::vector<Loc> &locs, uint32_t &next, const std::function<Operand(uint32_t)> &at)
	{
		if (!type.array.empty()) {
			const Type element = element_of(type);
			for (uint32_t i = 0; i < type.array[0]; ++i)
				layout(element, locs, next, at);
			return;
		}
		switch (type.shape) {
		case Type::Shape::scalar:
		case Type::Shape::vector:
		case Type::Shape::matrix:
			for (uint32_t r = 0; r < row_count(type); ++r) {
				const uint32_t reg = next++;
				for (uint32_t c = 0; c < row_width(type); ++c) {
					Loc loc;
					loc.base = at(reg);
					loc.component = uint8_t(c);
					locs.push_back(loc);
				}
			}
			break;
		case Type::Shape::structure:
			for (const auto &member : type.structure->members)
				layout(member.type, locs, next, at);
			break;
		default:
			break;
		}
	}

	// ---- conversions ----

	static uint32_t fold(uint32_t bits, Scalar from, Scalar to)
	{
		if (to == Scalar::boolean) {
			if (is_float(from))
				return bits_float(bits) != 0.0f ? ~0u : 0;
			return bits ? ~0u : 0;
		}
		if (is_float(to)) {
			if (is_float(from))
				return bits;
			if (from == Scalar::boolean)
				return bits ? float_bits(1.0f) : 0;
			if (from == Scalar::integer)
				return float_bits(float(int32_t(bits)));
			return float_bits(float(bits));
		}
		if (is_float(from)) {
			const float value = bits_float(bits);
			if (to == Scalar::integer)
				return uint32_t(int32_t(std::isfinite(value) ? std::clamp(value, -2147483648.0f, 2147483520.0f) : 0));
			return std::isfinite(value) && value > 0 ? uint32_t(std::min(value, 4294967040.0f)) : 0;
		}
		if (from == Scalar::boolean)
			return bits ? 1 : 0;
		return bits;
	}

	// The scalar kinds of a value changed, component by component.
	Ref convert_kind(const Ref &value, Scalar to)
	{
		const Scalar from = value.type.scalar;
		Ref result = value;
		result.type.scalar = to;
		result.writable = false;
		const bool same_class = from == to || (is_float(from) && is_float(to)) ||
			((from == Scalar::integer || from == Scalar::unsigned_integer) && (to == Scalar::integer || to == Scalar::unsigned_integer));
		if (same_class)
			return result;
		if (all_immediate(value)) {
			for (auto &loc : result.locs)
				loc.bits = fold(loc.bits, from, to);
			return result;
		}
		result = new_value(result.type);
		for (uint32_t r = 0; r < row_count(value.type); ++r) {
			const auto source = readable(row(value, r));
			const auto dest = row(result, r);
			const Operand target = dest_of(dest[0], uint8_t((1 << dest.size()) - 1));
			const Operand input = source_first(source);
			if (to == Scalar::boolean) {
				const Operand zero = Operand::values({}, 4);
				emit(is_float(from) ? sm4::ne : sm4::ine, {target, input, zero});
			} else if (is_float(to)) {
				if (from == Scalar::boolean)
					emit(sm4::op_and, {target, input, Operand::values({float_bits(1), float_bits(1), float_bits(1), float_bits(1)}, 4)});
				else
					emit(from == Scalar::integer ? sm4::itof : sm4::utof, {target, input});
			} else if (from == Scalar::boolean) {
				emit(sm4::op_and, {target, input, Operand::values({1, 1, 1, 1}, 4)});
			} else {
				emit(to == Scalar::integer ? sm4::ftoi : sm4::ftou, {target, input});
			}
		}
		return result;
	}

	// The value with the shape of the target: a scalar spread to every component, a vector or matrix cut to the first ones.
	Ref convert_shape(const Ref &value, const Type &to, uint32_t line)
	{
		Ref result = value;
		result.writable = false;
		Type shape = to;
		shape.scalar = value.type.scalar;
		result.type = shape;
		const uint32_t from_count = flat_size(value.type), to_count = flat_size(to);
		if (from_count == to_count) {
			// A matrix read as a vector of the same size or the reverse keeps the components in row order.
			return result;
		}
		if (from_count == 1) {
			result.locs.assign(to_count, value.locs[0]);
			return result;
		}
		if (value.type.shape == Type::Shape::matrix && to.shape == Type::Shape::matrix) {
			if (to.rows > value.type.rows || to.columns > value.type.columns)
				fail(line, "cannot convert from '" + value.type.name() + "' to '" + to.name() + "'");
			result.locs.clear();
			for (uint32_t r = 0; r < to.rows; ++r)
				for (uint32_t c = 0; c < to.columns; ++c)
					result.locs.push_back(value.locs[r * value.type.columns + c]);
			return result;
		}
		if (to_count < from_count && to.numeric() && value.type.numeric()) {
			if (value.type.shape == Type::Shape::matrix && to.shape == Type::Shape::vector)
				fail(line, "cannot convert from '" + value.type.name() + "' to '" + to.name() + "'");
			if (to.shape != Type::Shape::scalar)
				warnings += "implicit truncation of vector type\n";
			result.locs.resize(to_count);
			return result;
		}
		fail(line, "cannot convert from '" + value.type.name() + "' to '" + to.name() + "'");
	}

	Ref convert(const Ref &value, const Type &to, uint32_t line)
	{
		if (to.shape == Type::Shape::structure || value.type.shape == Type::Shape::structure || !to.array.empty() ||
			!value.type.array.empty()) {
			if (flat_size(to) != flat_size(value.type) || !(to == value.type || to.shape == value.type.shape))
				fail(line, "cannot convert from '" + value.type.name() + "' to '" + to.name() + "'");
			Ref result = value;
			result.type = to;
			return result;
		}
		if (to.shape == Type::Shape::object || value.type.shape == Type::Shape::object) {
			if (to.shape != value.type.shape)
				fail(line, "cannot convert from '" + value.type.name() + "' to '" + to.name() + "'");
			return value;
		}
		if (value.type.shape == Type::Shape::void_type)
			fail(line, "void value used in an expression");
		return convert_shape(convert_kind(value, to.scalar), to, line);
	}

	// The type both operands of an arithmetic or comparison operator take.
	Type common_type(const Type &left, const Type &right, uint32_t line)
	{
		if (!left.numeric() || !right.numeric())
			fail(line, "operator applied to non-numeric types '" + left.name() + "' and '" + right.name() + "'");
		const Scalar kind = rank(left.scalar) >= rank(right.scalar) ? left.scalar : right.scalar;
		if (left.shape == Type::Shape::scalar)
			return with_kind(right, kind);
		if (right.shape == Type::Shape::scalar)
			return with_kind(left, kind);
		if (left.shape == Type::Shape::matrix || right.shape == Type::Shape::matrix) {
			if (left.shape == Type::Shape::matrix && right.shape == Type::Shape::matrix)
				return Type::make_matrix(kind, std::min(left.rows, right.rows), std::min(left.columns, right.columns));
			// A vector meets a matrix of the same size.
			if (left.components() == right.components())
				return with_kind(left.shape == Type::Shape::matrix ? left : right, kind);
			fail(line, "mismatched operands '" + left.name() + "' and '" + right.name() + "'");
		}
		if (left.columns != right.columns)
			warnings += "implicit truncation of vector type\n";
		return Type::make_vector(kind, std::min(left.columns, right.columns));
	}

	// ---- component-wise instructions ----

	// One instruction per row of the result, reading each operand's matching row.
	Ref apply(uint16_t opcode, const Type &type, const std::vector<Ref> &operands, bool saturate = false, std::vector<bool> negate = {},
		std::vector<bool> absolute = {})
	{
		Ref result = new_value(type);
		for (uint32_t r = 0; r < row_count(type); ++r) {
			const auto dest = row(result, r);
			std::vector<Operand> list{dest_of(dest[0], uint8_t((1 << dest.size()) - 1))};
			for (size_t i = 0; i < operands.size(); ++i) {
				auto source = readable(row(operands[i], r));
				Operand operand = source_first(source);
				const bool minus = i < negate.size() && negate[i], magnitude = i < absolute.size() && absolute[i];
				if (operand.type == sm4::immediate32) {
					// Modifiers are folded into immediates.
					const bool floating = is_float(operands[i].type.scalar);
					for (auto &bits : operand.immediate) {
						if (magnitude)
							bits = floating ? bits & 0x7fffffffu : uint32_t(std::abs(int32_t(bits)));
						if (minus)
							bits = floating ? bits ^ 0x80000000u : uint32_t(-int64_t(int32_t(bits)));
					}
				} else {
					operand.negate = minus;
					operand.absolute = magnitude;
				}
				list.push_back(operand);
			}
			emit(opcode, list).saturate = saturate;
		}
		return result;
	}

	// An instruction with two destinations (imul, umul, udiv, sincos), one of them the null register.
	Ref apply_pair(uint16_t opcode, const Type &type, const std::vector<Ref> &operands, bool first)
	{
		Ref result = new_value(type);
		for (uint32_t r = 0; r < row_count(type); ++r) {
			const auto dest = row(result, r);
			const Operand target = dest_of(dest[0], uint8_t((1 << dest.size()) - 1));
			std::vector<Operand> list{first ? target : Operand::none(sm4::null_register), first ? Operand::none(sm4::null_register) : target};
			for (const auto &operand : operands)
				list.push_back(source_first(readable(row(operand, r))));
			emit(opcode, list);
		}
		return result;
	}

	// ---- constant buffers ----

	static uint32_t align16(uint32_t value) { return (value + 15) & ~15u; }

	bool column_major(const Type &type) const { return !type.row_major && !(flags & compile_pack_row_major); }

	// The bytes a value takes in a constant buffer: vectors never cross a register, and arrays, structures and matrices start one.
	uint32_t cb_size(const Type &type) const
	{
		if (!type.array.empty()) {
			const Type element = element_of(type);
			const uint32_t size = cb_size(element);
			return (type.array[0] - 1) * align16(size) + size;
		}
		switch (type.shape) {
		case Type::Shape::scalar:
			return 4;
		case Type::Shape::vector:
			return 4u * type.columns;
		case Type::Shape::matrix:
			return column_major(type) ? (type.columns - 1u) * 16 + type.rows * 4u : (type.rows - 1u) * 16 + type.columns * 4u;
		case Type::Shape::structure: {
			uint32_t offset = 0;
			for (const auto &member : type.structure->members)
				offset = cb_place(member.type, offset) + cb_size(member.type);
			return offset;
		}
		default:
			return 0;
		}
	}

	// Where a value goes when the buffer so far ends at `offset`.
	uint32_t cb_place(const Type &type, uint32_t offset) const
	{
		if (!type.array.empty() || type.shape == Type::Shape::matrix || type.shape == Type::Shape::structure)
			return align16(offset);
		if (offset / 16 != (offset + cb_size(type) - 1) / 16)
			return align16(offset);
		return offset;
	}

	void cb_layout(const Type &type, uint32_t offset, uint32_t buffer, std::vector<Loc> &locs) const
	{
		const auto at = [&](uint32_t byte) {
			Loc loc;
			loc.base = Operand::reg(sm4::constant_buffer, buffer);
			loc.base.dimension = 2;
			loc.base.index[1] = byte / 16;
			loc.component = uint8_t(byte % 16 / 4);
			return loc;
		};
		if (!type.array.empty()) {
			const Type element = element_of(type);
			const uint32_t stride = align16(cb_size(element));
			for (uint32_t i = 0; i < type.array[0]; ++i)
				cb_layout(element, offset + i * stride, buffer, locs);
			return;
		}
		switch (type.shape) {
		case Type::Shape::scalar:
		case Type::Shape::vector:
			for (uint32_t c = 0; c < type.components(); ++c)
				locs.push_back(at(offset + c * 4));
			break;
		case Type::Shape::matrix:
			for (uint32_t r = 0; r < type.rows; ++r)
				for (uint32_t c = 0; c < type.columns; ++c)
					locs.push_back(at(column_major(type) ? offset + c * 16 + r * 4 : offset + r * 16 + c * 4));
			break;
		case Type::Shape::structure: {
			uint32_t at_member = 0;
			for (const auto &member : type.structure->members) {
				at_member = cb_place(member.type, at_member);
				cb_layout(member.type, offset + at_member, buffer, locs);
				at_member += cb_size(member.type);
			}
			break;
		}
		default:
			break;
		}
	}

	// The byte offset of every scalar of a value in a structured buffer or group shared memory, where scalars are packed tightly
	// and matrices are stored by columns unless declared row_major.
	void tight_layout(const Type &type, uint32_t &offset, std::vector<uint32_t> &offsets) const
	{
		if (!type.array.empty()) {
			const Type element = element_of(type);
			for (uint32_t i = 0; i < type.array[0]; ++i)
				tight_layout(element, offset, offsets);
			return;
		}
		switch (type.shape) {
		case Type::Shape::scalar:
		case Type::Shape::vector:
			for (uint32_t c = 0; c < type.components(); ++c, offset += 4)
				offsets.push_back(offset);
			break;
		case Type::Shape::matrix:
			for (uint32_t r = 0; r < type.rows; ++r)
				for (uint32_t c = 0; c < type.columns; ++c)
					offsets.push_back(offset + 4 * (column_major(type) ? c * type.rows + r : r * type.columns + c));
			offset += 4u * type.rows * type.columns;
			break;
		case Type::Shape::structure:
			for (const auto &member : type.structure->members)
				tight_layout(member.type, offset, offsets);
			break;
		default:
			break;
		}
	}

	uint32_t tight_size(const Type &type) const
	{
		uint32_t size = 0;
		std::vector<uint32_t> offsets;
		tight_layout(type, size, offsets);
		return size;
	}

	// ---- resources ----

	static bool is_uav(ObjectKind kind) { return kind >= ObjectKind::rw_texture1d; }
	static bool is_sampler(ObjectKind kind) { return kind == ObjectKind::sampler || kind == ObjectKind::sampler_comparison; }
	static bool is_structured(ObjectKind kind) { return kind == ObjectKind::structured_buffer || kind == ObjectKind::rw_structured_buffer; }
	static bool is_raw(ObjectKind kind) { return kind == ObjectKind::byte_address_buffer || kind == ObjectKind::rw_byte_address_buffer; }

	// D3D10_SB_RESOURCE_DIMENSION of a declaration.
	static uint32_t resource_dimension(ObjectKind kind)
	{
		switch (kind) {
		case ObjectKind::buffer:
		case ObjectKind::rw_buffer: return 1;
		case ObjectKind::texture1d:
		case ObjectKind::rw_texture1d: return 2;
		case ObjectKind::texture2d:
		case ObjectKind::rw_texture2d: return 3;
		case ObjectKind::texture2dms: return 4;
		case ObjectKind::texture3d:
		case ObjectKind::rw_texture3d: return 5;
		case ObjectKind::texture_cube: return 6;
		case ObjectKind::texture1d_array:
		case ObjectKind::rw_texture1d_array: return 7;
		case ObjectKind::texture2d_array:
		case ObjectKind::rw_texture2d_array: return 8;
		case ObjectKind::texture2dms_array: return 9;
		case ObjectKind::texture_cube_array: return 10;
		case ObjectKind::byte_address_buffer:
		case ObjectKind::rw_byte_address_buffer: return 11;
		case ObjectKind::structured_buffer:
		case ObjectKind::rw_structured_buffer: return 12;
		default: return 0;
		}
	}

	// D3D_SRV_DIMENSION as reflection reports it.
	static uint32_t view_dimension(ObjectKind kind)
	{
		switch (resource_dimension(kind)) {
		case 1: return 1;
		case 2: return 2;
		case 3: return 4;
		case 4: return 6;
		case 5: return 8;
		case 6: return 9;
		case 7: return 3;
		case 8: return 5;
		case 9: return 7;
		case 10: return 10;
		case 11: return 11;
		default: return 1;
		}
	}

	// How many coordinates address the resource, and how many of them an offset moves.
	static uint32_t coordinate_count(ObjectKind kind)
	{
		switch (resource_dimension(kind)) {
		case 1:
		case 2:
		case 11:
		case 12: return 1;
		case 3:
		case 4:
		case 7: return 2;
		case 10: return 4;
		default: return 3;
		}
	}

	static uint32_t offset_count(ObjectKind kind)
	{
		switch (resource_dimension(kind)) {
		case 2:
		case 7: return 1;
		case 3:
		case 4:
		case 8:
		case 9: return 2;
		case 5: return 3;
		default: return 0;
		}
	}

	// D3D_RESOURCE_RETURN_TYPE of a texture element: unorm 1, snorm 2, sint 3, uint 4, float 5.
	static uint32_t return_type(const Type &element)
	{
		switch (element.scalar) {
		case Scalar::integer: return 3;
		case Scalar::unsigned_integer:
		case Scalar::boolean: return 4;
		default: return 5;
		}
	}

	Resource &resource_of(const Ref &ref, uint32_t line)
	{
		if (ref.resource < 0 || ref.type.shape != Type::Shape::object)
			fail(line, "a resource object is required");
		auto &resource = resources[size_t(ref.resource)];
		resource.used = true;
		return resource;
	}

	// The operand naming a resource: its slot is filled in once resources are bound, so it holds the resource number until then.
	static Operand resource_operand(const Ref &ref, uint8_t type)
	{
		Operand operand = Operand::reg(type, uint32_t(ref.resource) | ref.resource_element << 16);
		operand.mode = Operand::Mode::swizzle;
		return operand;
	}

	Operand sampler_operand(const Ref &ref, uint32_t line, uint8_t component = 0)
	{
		auto &sampler = resource_of(ref, line);
		if (!is_sampler(sampler.kind))
			fail(line, "a sampler is required");
		Operand operand = resource_operand(ref, sm4::sampler);
		operand.components = 0;
		if (component) {
			operand.components = 4;
			operand.mode = Operand::Mode::select;
			operand.swizzle[0] = uint8_t(component - 1);
		}
		return operand;
	}

	// ---- buffer, texture and group shared elements ----

	Operand access_operand(const Access &access) const
	{
		if (access.kind == Access::Kind::shared) {
			Operand operand = Operand::reg(sm4::group_shared, uint32_t(access.target));
			operand.mode = Operand::Mode::swizzle;
			return operand;
		}
		Ref holder;
		holder.resource = access.target;
		holder.resource_element = access.element;
		return resource_operand(holder, access.uav ? sm4::uav : sm4::resource);
	}

	// Splits scalars into runs of up to four consecutive dwords whose locs share a register and component slots.
	static std::vector<std::pair<size_t, size_t>> runs(const std::vector<uint32_t> &offsets, const std::vector<Loc> *destination)
	{
		std::vector<std::pair<size_t, size_t>> list;
		size_t start = 0;
		while (start < offsets.size()) {
			size_t end = start + 1;
			while (end < offsets.size() && end - start < 4 && offsets[end] == offsets[end - 1] + 4) {
				if (destination) {
					const auto &locs = *destination;
					bool taken = !same_register(locs[end].base, locs[start].base);
					for (size_t k = start; k < end; ++k)
						taken |= locs[k].component == locs[end].component;
					if (taken)
						break;
				}
				++end;
			}
			list.emplace_back(start, end);
			start = end;
		}
		return list;
	}

	Ref load(const Ref &ref)
	{
		if (!ref.access)
			return ref;
		const Access &access = *ref.access;
		Ref result = storage(ref.type);
		result.writable = false;
		const Operand target = access_operand(access);
		if (access.kind == Access::Kind::typed) {
			auto address = access.address;
			if (access.mip)
				address.push_back(constant_loc(0));
			address = readable(address);
			Operand resource = target;
			const uint8_t mask = uint8_t((1 << result.locs.size()) - 1);
			const uint16_t opcode = access.uav ? sm4::ld_uav_typed : sm4::ld;
			emit(opcode, {dest_of(result.locs[0], mask), source_first(address), resource});
			return result;
		}
		const std::vector<Loc> index{access.address[0]};
		for (const auto &[start, end] : runs(access.offsets, &result.locs)) {
			Operand source = target;
			uint8_t mask = 0;
			for (size_t k = start; k < end; ++k) {
				mask |= uint8_t(1 << result.locs[k].component);
				source.swizzle[result.locs[k].component] = uint8_t(k - start);
			}
			emit(sm4::ld_structured, {dest_of(result.locs[start], mask), scalar_source(index[0]), scalar_source(byte_offset(access, access.offsets[start])),
				source});
		}
		return result;
	}

	Loc byte_offset(const Access &access, uint32_t offset)
	{
		if (!access.shift)
			return constant_loc(offset);
		Ref shift;
		shift.type = scalar_type(Scalar::unsigned_integer);
		shift.locs = {*access.shift};
		return offset ? apply(sm4::iadd, shift.type, {shift, uint_constant(offset)}).locs[0] : *access.shift;
	}

	void store(const Ref &target, const Ref &value, uint32_t line)
	{
		const Access &access = *target.access;
		if (!access.uav && access.kind != Access::Kind::shared)
			fail(line, "a read-only resource cannot be written");
		if (access.kind == Access::Kind::typed) {
			const auto address = readable(access.address);
			const auto data = readable(value.locs);
			Operand dest = access_operand(access);
			dest.mode = Operand::Mode::mask;
			dest.mask = 0xf;
			std::vector<uint8_t> spread{0, 1, 2, 3};
			std::vector<Loc> four;
			for (int i = 0; i < 4; ++i)
				four.push_back(data[std::min<size_t>(size_t(i), data.size() - 1)]);
			emit(sm4::store_uav_typed, {dest, source_first(address), source_for(four, spread)});
			return;
		}
		const Loc index = access.address[0];
		for (const auto &[start, end] : runs(access.offsets, nullptr)) {
			std::vector<Loc> part(value.locs.begin() + long(start), value.locs.begin() + long(end));
			part = readable(part);
			Operand dest = access_operand(access);
			dest.mode = Operand::Mode::mask;
			dest.mask = uint8_t((1 << part.size()) - 1);
			emit(sm4::store_structured, {dest, scalar_source(index), scalar_source(byte_offset(access, access.offsets[start])), source_first(part)});
		}
	}

	void assign_to(const Ref &target, const Ref &value, uint32_t line)
	{
		if (target.access) {
			store(target, value, line);
			return;
		}
		if (!target.writable)
			fail(line, "l-value specifies const object");
		write(target.locs, value.locs);
	}

	// ---- expressions ----

	Ref zero(const Type &type) { return constant(type, std::vector<uint32_t>(flat_size(type), 0)); }

	Ref splat(const Type &type, uint32_t bits) { return constant(type, std::vector<uint32_t>(flat_size(type), bits)); }

	Ref copy(const Ref &value)
	{
		Ref result = storage(value.type);
		write(result.locs, load(value).locs);
		result.writable = false;
		return result;
	}

	static void flat_kinds(const Type &type, std::vector<Scalar> &kinds)
	{
		for (uint32_t i = 0; i < element_count(type); ++i) {
			if (type.shape == Type::Shape::structure)
				for (const auto &member : type.structure->members)
					flat_kinds(member.type, kinds);
			else
				kinds.insert(kinds.end(), type.components(), type.scalar);
		}
	}

	// One scalar converted between kinds.
	Loc convert_scalar(const Loc &loc, Scalar from, Scalar to)
	{
		Ref one;
		one.type = scalar_type(from);
		one.locs = {loc};
		return convert_kind(one, to).locs[0];
	}

	// Scalars of mixed kinds converted to the kinds of a type's components.
	Ref from_scalars(const std::vector<std::pair<Loc, Scalar>> &scalars, const Type &type, uint32_t line)
	{
		std::vector<Scalar> kinds;
		flat_kinds(type, kinds);
		if (scalars.size() != kinds.size())
			fail(line, "initializer does not match the type '" + type.name() + "'");
		Ref result;
		result.type = type;
		for (size_t k = 0; k < kinds.size(); ++k)
			result.locs.push_back(convert_scalar(scalars[k].first, scalars[k].second, kinds[k]));
		return result;
	}

	void scalars_of(const Ref &value, std::vector<std::pair<Loc, Scalar>> &scalars)
	{
		std::vector<Scalar> kinds;
		flat_kinds(value.type, kinds);
		const Ref loaded = load(value);
		for (size_t k = 0; k < kinds.size(); ++k)
			scalars.emplace_back(loaded.locs[k], kinds[k]);
	}

	void flatten(const Expr &expr, std::vector<std::pair<Loc, Scalar>> &scalars)
	{
		if (expr.kind == Expr::Kind::initializer_list) {
			for (const auto &item : expr.operands)
				flatten(*item, scalars);
			return;
		}
		scalars_of(value(expr), scalars);
	}

	Ref initial_value(const Expr &init, const Type &type)
	{
		if (init.kind != Expr::Kind::initializer_list)
			return convert(value(init), type, init.line);
		std::vector<std::pair<Loc, Scalar>> scalars;
		flatten(init, scalars);
		return from_scalars(scalars, type, init.line);
	}

	Ref literal(const Expr &expr)
	{
		const Scalar kind = expr.literal_type.scalar;
		if (kind == Scalar::boolean)
			return constant(scalar_type(Scalar::boolean), {expr.number != 0 ? ~0u : 0});
		if (is_float(kind))
			return float_constant(float(expr.number));
		if (kind == Scalar::unsigned_integer || expr.number > 2147483647.0)
			return uint_constant(uint32_t(int64_t(expr.number)));
		return int_constant(int32_t(int64_t(expr.number)));
	}

	Ref *find_local(const std::string &name)
	{
		for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
			if (auto found = scope->find(name); found != scope->end())
				return &found->second;
		return nullptr;
	}

	Ref lookup(const Expr &expr)
	{
		if (auto local = find_local(expr.text))
			return *local;
		if (auto global = globals.find(expr.text); global != globals.end()) {
			if (auto buffer = buffer_names.find(expr.text); buffer != buffer_names.end()) {
				buffers[size_t(buffer->second.first)].used = true;
				buffers[size_t(buffer->second.first)].variables[size_t(buffer->second.second)].used = true;
			}
			return global->second;
		}
		fail(expr.line, "undeclared identifier '" + expr.text + "'");
	}

	static std::optional<std::vector<uint8_t>> parse_swizzle(const std::string &text, uint32_t count)
	{
		if (text.empty() || text.size() > 4)
			return std::nullopt;
		for (const std::string set : {"xyzw", "rgba"}) {
			std::vector<uint8_t> list;
			for (const char letter : text) {
				const auto at = set.find(letter);
				if (at == std::string::npos || at >= count)
					break;
				list.push_back(uint8_t(at));
			}
			if (list.size() == text.size())
				return list;
		}
		return std::nullopt;
	}

	// _m00 names a matrix element from zero, _11 from one; several may follow each other.
	static std::optional<std::vector<uint32_t>> parse_matrix_swizzle(const std::string &text, const Type &type)
	{
		std::vector<uint32_t> list;
		size_t at = 0;
		while (at < text.size()) {
			if (text[at] != '_')
				return std::nullopt;
			const bool zero_based = at + 1 < text.size() && text[at + 1] == 'm';
			const size_t digits = at + (zero_based ? 2 : 1);
			if (digits + 2 > text.size() || !std::isdigit(uint8_t(text[digits])) || !std::isdigit(uint8_t(text[digits + 1])))
				return std::nullopt;
			const int row = text[digits] - '0' - (zero_based ? 0 : 1), column = text[digits + 1] - '0' - (zero_based ? 0 : 1);
			if (row < 0 || column < 0 || row >= type.rows || column >= type.columns)
				return std::nullopt;
			list.push_back(uint32_t(row * type.columns + column));
			at = digits + 2;
		}
		if (list.empty() || list.size() > 4)
			return std::nullopt;
		return list;
	}

	static bool unique(const std::vector<uint32_t> &list)
	{
		return std::set<uint32_t>(list.begin(), list.end()).size() == list.size();
	}

	// Components picked out of a value: a swizzle or a matrix element list.
	Ref pick(const Ref &base, const std::vector<uint32_t> &list)
	{
		Ref result;
		result.type = Type::make_vector(base.type.scalar, uint8_t(list.size()));
		if (base.access && base.access->kind != Access::Kind::typed) {
			auto access = std::make_shared<Access>(*base.access);
			access->offsets.clear();
			for (auto k : list)
				access->offsets.push_back(base.access->offsets[k]);
			result.access = access;
			return result;
		}
		const Ref source = load(base);
		for (auto k : list)
			result.locs.push_back(source.locs[k]);
		result.writable = base.writable && !base.access && unique(list);
		return result;
	}

	std::optional<Ref> member_of(const Ref &base, const std::string &name)
	{
		uint32_t start = 0;
		for (const auto &member : base.type.structure->members) {
			const uint32_t size = flat_size(member.type);
			if (member.name == name) {
				Ref result = base;
				result.type = member.type;
				result.icb = -1;
				if (base.access) {
					auto access = std::make_shared<Access>(*base.access);
					access->offsets.assign(base.access->offsets.begin() + start, base.access->offsets.begin() + start + size);
					result.access = access;
				} else
					result.locs.assign(base.locs.begin() + start, base.locs.begin() + start + size);
				return result;
			}
			start += size;
		}
		return std::nullopt;
	}

	Ref member(const Expr &expr)
	{
		const Ref base = eval(*expr.operands[0]);
		const auto &name = expr.text;
		if (base.type.shape == Type::Shape::structure && base.type.array.empty()) {
			if (auto found = member_of(base, name))
				return *found;
			fail(expr.line, "'" + name + "' is not a member of '" + base.type.name() + "'");
		}
		if (base.type.array.empty() && (base.type.shape == Type::Shape::scalar || base.type.shape == Type::Shape::vector)) {
			const auto list = parse_swizzle(name, base.type.components());
			if (!list)
				fail(expr.line, "invalid subscript '" + name + "'");
			return pick(base, std::vector<uint32_t>(list->begin(), list->end()));
		}
		if (base.type.array.empty() && base.type.shape == Type::Shape::matrix) {
			const auto list = parse_matrix_swizzle(name, base.type);
			if (!list)
				fail(expr.line, "invalid subscript '" + name + "'");
			return pick(base, *list);
		}
		fail(expr.line, "invalid subscript '" + name + "'");
	}

	// Components that a dynamic index selects: the first element's locs, indexed relative to a temp holding index times step.
	std::vector<Loc> relative(std::vector<Loc> first, const Ref &position, uint32_t step, uint32_t line, int dimension = -1)
	{
		const auto slot = [dimension](const Loc &loc) { return dimension < 0 ? loc.base.dimension - 1 : dimension; };
		const Ref index = convert(position, scalar_type(Scalar::integer), line);
		Ref offset = index;
		if (step > 1)
			offset = std::has_single_bit(step) ? apply(sm4::ishl, index.type, {index, int_constant(std::countr_zero(step))})
							   : apply_pair(sm4::imul, index.type, {index, int_constant(int32_t(step))}, false);
		const auto previous = first.empty() ? std::nullopt : first[0].base.relative[size_t(slot(first[0]))];
		if (previous) {
			Ref before;
			before.type = index.type;
			Loc loc;
			loc.base = Operand::reg(previous->type, previous->index);
			loc.component = previous->component;
			before.locs = {loc};
			offset = apply(sm4::iadd, index.type, {before, offset});
		}
		Loc at = offset.locs[0];
		if (at.immediate || at.base.type != sm4::temp || at.base.relative[0]) {
			const auto held = temp_locs(new_register(), 1);
			write(held, {at});
			at = held[0];
		}
		for (auto &loc : first)
			loc.base.relative[size_t(slot(loc))] = sm4::Relative{sm4::temp, at.base.index[0], at.component};
		return first;
	}

	// A value chosen among several by a dynamic index, with a conditional move for each.
	Ref select(const std::vector<Ref> &choices, const Ref &position, uint32_t line)
	{
		const Ref index = convert(position, scalar_type(Scalar::integer), line);
		Ref result = choices[0];
		for (size_t k = 1; k < choices.size(); ++k) {
			const Ref same = apply(sm4::ieq, scalar_type(Scalar::boolean), {index, int_constant(int32_t(k))});
			result = apply(sm4::movc, choices[k].type, {convert_shape(same, with_kind(choices[k].type, Scalar::boolean), line), choices[k], result});
		}
		return result;
	}

	Ref element_access(const Ref &object, const Ref &position, uint32_t line)
	{
		auto &resource = resource_of(object, line);
		if (is_sampler(resource.kind) || is_raw(resource.kind))
			fail(line, "this object cannot be indexed");
		auto access = std::make_shared<Access>();
		access->target = object.resource;
		access->element = object.resource_element;
		access->uav = is_uav(resource.kind);
		Ref result;
		result.type = resource.element;
		result.writable = access->uav;
		if (is_structured(resource.kind)) {
			access->kind = Access::Kind::structured;
			access->address = convert(position, scalar_type(Scalar::unsigned_integer), line).locs;
			uint32_t size = 0;
			tight_layout(resource.element, size, access->offsets);
		} else {
			if (resource.kind == ObjectKind::texture2dms || resource.kind == ObjectKind::texture2dms_array)
				fail(line, "multisampled textures are read with Load");
			const uint32_t count = coordinate_count(resource.kind);
			access->address = convert(position, Type::make_vector(Scalar::unsigned_integer, uint8_t(count)), line).locs;
			access->mip = !access->uav && resource.kind != ObjectKind::buffer;
		}
		result.access = access;
		return result;
	}

	uint32_t constant_index(const Ref &position, uint32_t line)
	{
		const Ref index = convert(position, scalar_type(Scalar::integer), line);
		return index.locs[0].bits;
	}

	Ref index(const Expr &expr)
	{
		const Ref base = eval(*expr.operands[0]);
		const Ref position = value(*expr.operands[1]);
		const uint32_t line = expr.line;
		if (!position.type.numeric() || position.type.components() > 4)
			fail(line, "the index must be numeric");
		if (base.type.shape == Type::Shape::object && base.type.array.empty())
			return element_access(base, position, line);
		if (!base.type.array.empty()) {
			const Type element = element_of(base.type);
			const uint32_t per = flat_size(element);
			Ref result = base;
			result.type = element;
			if (all_immediate(position)) {
				const uint32_t at = constant_index(position, line);
				if (at >= base.type.array[0])
					fail(line, "array index out of bounds");
				if (base.type.shape == Type::Shape::object) {
					result.resource_element += at;
					return result;
				}
				if (base.access && base.access->outer_array) {
					auto access = std::make_shared<Access>(*base.access);
					access->outer_array = false;
					access->address = {constant_loc(at)};
					access->offsets.resize(per);
					result.access = access;
				} else if (base.access) {
					auto access = std::make_shared<Access>(*base.access);
					access->offsets.assign(base.access->offsets.begin() + at * per, base.access->offsets.begin() + (at + 1) * per);
					result.access = access;
				} else
					result.locs.assign(base.locs.begin() + at * per, base.locs.begin() + (at + 1) * per);
				if (base.icb >= 0)
					result.icb = base.icb + int(at * registers_of(element));
				return result;
			}
			if (base.type.shape == Type::Shape::object)
				fail(line, "an array of resources needs a constant index");
			if (base.access && base.access->outer_array) {
				auto access = std::make_shared<Access>(*base.access);
				access->outer_array = false;
				access->address = convert(position, scalar_type(Scalar::unsigned_integer), line).locs;
				access->offsets.resize(per);
				result.access = access;
				return result;
			}
			if (base.access) {
				if (base.access->kind == Access::Kind::typed)
					fail(line, "this element cannot be indexed dynamically");
				// Elements of an array inside a buffer element sit at a regular byte stride, added to the offsets at run time.
				const uint32_t stride = base.type.array[0] > 1 ? base.access->offsets[per] - base.access->offsets[0] : 0;
				const Type uint_type = scalar_type(Scalar::unsigned_integer);
				Ref shift = apply_pair(sm4::imul, uint_type, {convert(position, uint_type, line), uint_constant(stride)}, false);
				if (base.access->shift) {
					Ref before;
					before.type = uint_type;
					before.locs = {*base.access->shift};
					shift = apply(sm4::iadd, uint_type, {before, shift});
				}
				auto access = std::make_shared<Access>(*base.access);
				access->offsets.resize(per);
				access->shift = shift.locs[0];
				result.access = access;
				return result;
			}
			std::vector<Loc> first;
			if (base.icb >= 0) {
				uint32_t next = 0;
				const uint32_t origin = uint32_t(base.icb);
				layout(element, first, next, [origin](uint32_t reg) {
					Operand operand = Operand::reg(sm4::immediate_constant_buffer, origin + reg);
					return operand;
				});
			} else if (all_immediate(base)) {
				fail(line, "a constant array needs a constant index");
			} else
				first.assign(base.locs.begin(), base.locs.begin() + per);
			if (first.empty())
				fail(line, "invalid index");
			uint32_t step = registers_of(element);
			// The vertices of a geometry shader input: v[vertex][register].
			if ((first[0].base.type == sm4::input || first[0].base.type == sm4::input_control_point || first[0].base.type == sm4::output_control_point) &&
				first[0].base.dimension == 2) {
				result.locs = relative(first, position, 1, line, 0);
				return result;
			}
			if (first[0].base.type == sm4::constant_buffer) {
				step = align16(cb_size(element)) / 16;
				buffers[first[0].base.index[0]].dynamic = true;
			}
			result.locs = relative(first, position, step, line);
			result.icb = -1;
			return result;
		}
		const Ref source = load(base);
		if (source.type.shape == Type::Shape::vector || source.type.shape == Type::Shape::matrix) {
			const bool matrix = source.type.shape == Type::Shape::matrix;
			const uint32_t count = matrix ? source.type.rows : source.type.columns;
			const uint32_t width = matrix ? source.type.columns : 1;
			const Type part = matrix ? Type::make_vector(source.type.scalar, source.type.columns) : scalar_type(source.type.scalar);
			if (all_immediate(position)) {
				const uint32_t at = constant_index(position, line);
				if (at >= count)
					fail(line, "index out of bounds");
				Ref result;
				result.type = part;
				result.locs.assign(source.locs.begin() + at * width, source.locs.begin() + (at + 1) * width);
				result.writable = base.writable && !base.access;
				return result;
			}
			std::vector<Ref> choices;
			for (uint32_t k = 0; k < count; ++k) {
				Ref choice;
				choice.type = part;
				choice.locs.assign(source.locs.begin() + k * width, source.locs.begin() + (k + 1) * width);
				choices.push_back(choice);
			}
			return select(choices, position, line);
		}
		fail(line, "this value cannot be indexed");
	}

	static std::optional<uint32_t> fold_binary(const std::string &op, Scalar kind, uint32_t x, uint32_t y)
	{
		const auto truth = [](bool value) { return value ? ~0u : 0u; };
		if (op == "&&")
			return truth(x && y);
		if (op == "||")
			return truth(x || y);
		if (is_float(kind)) {
			const float a = bits_float(x), b = bits_float(y);
			if (op == "+") return float_bits(a + b);
			if (op == "-") return float_bits(a - b);
			if (op == "*") return float_bits(a * b);
			if (op == "/") return float_bits(a / b);
			if (op == "%") return float_bits(std::fmod(a, b));
			if (op == "==") return truth(a == b);
			if (op == "!=") return truth(a != b);
			if (op == "<") return truth(a < b);
			if (op == ">") return truth(a > b);
			if (op == "<=") return truth(a <= b);
			if (op == ">=") return truth(a >= b);
			return std::nullopt;
		}
		const bool sign = is_signed(kind);
		const int32_t a = int32_t(x), b = int32_t(y);
		if (op == "+") return x + y;
		if (op == "-") return x - y;
		if (op == "*") return x * y;
		if (op == "/" || op == "%") {
			if (!y || (sign && a == INT32_MIN && b == -1))
				return std::nullopt;
			if (op == "/")
				return sign ? uint32_t(a / b) : x / y;
			return sign ? uint32_t(a % b) : x % y;
		}
		if (op == "<<") return x << (y & 31);
		if (op == ">>") return sign ? uint32_t(a >> (y & 31)) : x >> (y & 31);
		if (op == "&") return x & y;
		if (op == "|") return x | y;
		if (op == "^") return x ^ y;
		if (op == "==") return truth(x == y);
		if (op == "!=") return truth(x != y);
		if (op == "<") return truth(sign ? a < b : x < y);
		if (op == ">") return truth(sign ? a > b : x > y);
		if (op == "<=") return truth(sign ? a <= b : x <= y);
		if (op == ">=") return truth(sign ? a >= b : x >= y);
		return std::nullopt;
	}

	// Signed division and remainder through unsigned division of the magnitudes.
	Ref signed_divide(const Ref &a, const Ref &b, const Type &type, bool remainder)
	{
		const Type mask = with_kind(type, Scalar::boolean);
		const Ref left = apply(sm4::imax, type, {a, a}, false, {false, true});
		const Ref right = apply(sm4::imax, type, {b, b}, false, {false, true});
		const Ref result = apply_pair(sm4::udiv, type, {left, right}, !remainder);
		const Ref negative = remainder ? apply(sm4::ilt, mask, {a, zero(type)}) : apply(sm4::ilt, mask, {apply(sm4::op_xor, type, {a, b}), zero(type)});
		return apply(sm4::movc, type, {negative, apply(sm4::ineg, type, {result}), result});
	}

	Ref float_remainder(const Ref &a, const Ref &b, const Type &type)
	{
		const Ref quotient = apply(sm4::div, type, {a, b});
		const Ref fraction = apply(sm4::frc, type, {quotient}, false, {}, {true});
		const Ref magnitude = apply(sm4::mul, type, {fraction, b}, false, {}, {false, true});
		const Ref positive = apply(sm4::ge, with_kind(type, Scalar::boolean), {a, zero(type)});
		return apply(sm4::movc, type, {positive, magnitude, magnitude}, false, {false, false, true});
	}

	Ref binary_op(const std::string &op, Ref left, Ref right, uint32_t line)
	{
		left = load(left);
		right = load(right);
		Type type = common_type(left.type, right.type, line);
		const bool compare = op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
		const bool logical = op == "&&" || op == "||";
		const bool bitwise = op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>";
		if (logical)
			type.scalar = Scalar::boolean;
		else if (bitwise) {
			if (is_float(type.scalar))
				fail(line, "int or unsigned int type required");
			if (type.scalar == Scalar::boolean && (op == "<<" || op == ">>"))
				type.scalar = Scalar::integer;
		} else if (!compare && type.scalar == Scalar::boolean)
			type.scalar = Scalar::integer;
		const Ref a = convert(left, type, line), b = convert(right, type, line);
		const Type result_type = compare ? with_kind(type, Scalar::boolean) : type;
		if (all_immediate(a) && all_immediate(b)) {
			std::vector<uint32_t> bits;
			for (size_t k = 0; k < a.locs.size(); ++k) {
				const auto folded = fold_binary(op, type.scalar, a.locs[k].bits, b.locs[k].bits);
				if (!folded)
					break;
				bits.push_back(*folded);
			}
			if (bits.size() == a.locs.size())
				return constant(result_type, bits);
		}
		// A constant false on either side of && (true of ||) decides it, so that a branch it guards is left out.
		if (logical)
			for (const Ref *side : {&a, &b})
				if (all_immediate(*side) && std::all_of(side->locs.begin(), side->locs.end(), [&](const Loc &loc) { return (loc.bits != 0) == (op == "||"); }))
					return constant(type, std::vector<uint32_t>(a.locs.size(), op == "||" ? ~0u : 0u));
		const Scalar kind = type.scalar;
		const bool fp = is_float(kind), uns = kind == Scalar::unsigned_integer;
		if (logical)
			return apply(op == "&&" ? sm4::op_and : sm4::op_or, type, {a, b});
		if (compare) {
			if (op == "==")
				return apply(fp ? sm4::eq : sm4::ieq, result_type, {a, b});
			if (op == "!=")
				return apply(fp ? sm4::ne : sm4::ine, result_type, {a, b});
			const uint16_t less = fp ? sm4::lt : uns ? sm4::ult : sm4::ilt;
			const uint16_t at_least = fp ? sm4::ge : uns ? sm4::uge : sm4::ige;
			if (op == "<")
				return apply(less, result_type, {a, b});
			if (op == ">")
				return apply(less, result_type, {b, a});
			if (op == ">=")
				return apply(at_least, result_type, {a, b});
			return apply(at_least, result_type, {b, a});
		}
		if (op == "+")
			return apply(fp ? sm4::add : sm4::iadd, type, {a, b});
		if (op == "-")
			return apply(fp ? sm4::add : sm4::iadd, type, {a, b}, false, {false, true});
		if (op == "*")
			return fp ? apply(sm4::mul, type, {a, b}) : apply_pair(sm4::imul, type, {a, b}, false);
		if (op == "/") {
			if (fp)
				return apply(sm4::div, type, {a, b});
			return uns ? apply_pair(sm4::udiv, type, {a, b}, true) : signed_divide(a, b, type, false);
		}
		if (op == "%") {
			if (fp)
				return float_remainder(a, b, type);
			return uns ? apply_pair(sm4::udiv, type, {a, b}, false) : signed_divide(a, b, type, true);
		}
		if (op == "&")
			return apply(sm4::op_and, type, {a, b});
		if (op == "|")
			return apply(sm4::op_or, type, {a, b});
		if (op == "^")
			return apply(sm4::op_xor, type, {a, b});
		if (op == "<<")
			return apply(sm4::ishl, type, {a, b});
		if (op == ">>")
			return apply(uns ? sm4::ushr : sm4::ishr, type, {a, b});
		fail(line, "unknown operator '" + op + "'");
	}

	Ref increment(const Expr &operand, bool up, bool postfix, uint32_t line)
	{
		const Ref target = eval(operand);
		if (!target.assignable())
			fail(line, "l-value specifies const object");
		const Ref old = postfix ? copy(target) : load(target);
		const Ref updated = convert(binary_op(up ? "+" : "-", old, int_constant(1), line), target.type, line);
		assign_to(target, updated, line);
		return postfix ? old : updated;
	}

	Ref unary(const Expr &expr)
	{
		const auto &op = expr.text;
		if (op == "++" || op == "--")
			return increment(*expr.operands[0], op == "++", false, expr.line);
		Ref operand = value(*expr.operands[0]);
		if (!operand.type.numeric())
			fail(expr.line, "operator '" + op + "' needs a numeric operand");
		if (op == "+")
			return operand;
		if (op == "-") {
			const Type type = operand.type.scalar == Scalar::boolean ? with_kind(operand.type, Scalar::integer) : operand.type;
			operand = convert(operand, type, expr.line);
			if (all_immediate(operand)) {
				Ref result = operand;
				for (auto &loc : result.locs)
					loc.bits = is_float(type.scalar) ? loc.bits ^ 0x80000000u : uint32_t(-int64_t(int32_t(loc.bits)));
				return result;
			}
			return is_float(type.scalar) ? apply(sm4::mov, type, {operand}, false, {true}) : apply(sm4::ineg, type, {operand});
		}
		if (op == "!" || op == "~") {
			if (op == "~" && is_float(operand.type.scalar))
				fail(expr.line, "int or unsigned int type required");
			const Type type = op == "!" ? with_kind(operand.type, Scalar::boolean) : operand.type;
			operand = convert(operand, type, expr.line);
			if (all_immediate(operand)) {
				Ref result = operand;
				for (auto &loc : result.locs)
					loc.bits = ~loc.bits;
				return result;
			}
			return apply(sm4::op_not, type, {operand});
		}
		fail(expr.line, "unknown operator '" + op + "'");
	}

	Ref assignment(const Expr &expr)
	{
		const Ref target = eval(*expr.operands[0]);
		if (!target.assignable())
			fail(expr.line, "l-value specifies const object");
		Ref source = value(*expr.operands[1]);
		if (expr.text != "=")
			source = binary_op(expr.text.substr(0, expr.text.size() - 1), target, source, expr.line);
		source = convert(source, target.type, expr.line);
		assign_to(target, source, expr.line);
		return target.access ? source : target;
	}

	Ref ternary(const Expr &expr)
	{
		const Ref condition = value(*expr.operands[0]);
		Ref yes = value(*expr.operands[1]), no = value(*expr.operands[2]);
		if (!yes.type.numeric() || !no.type.numeric()) {
			if (!(yes.type == no.type))
				fail(expr.line, "the branches of ?: have different types");
			// Structures choose scalar by scalar.
			if (!condition.type.numeric() || condition.type.components() != 1)
				fail(expr.line, "the condition must be a scalar");
			const Ref chosen = convert(condition, scalar_type(Scalar::boolean), expr.line);
			Ref result = storage(yes.type);
			for (size_t k = 0; k < yes.locs.size(); ++k) {
				Ref a, b;
				a.type = b.type = scalar_type(Scalar::floating);
				a.locs = {yes.locs[k]};
				b.locs = {no.locs[k]};
				write({result.locs[k]}, apply(sm4::movc, a.type, {chosen, a, b}).locs);
			}
			result.writable = false;
			return result;
		}
		const Type type = yes.type == no.type ? yes.type : common_type(yes.type, no.type, expr.line);
		yes = convert(yes, type, expr.line);
		no = convert(no, type, expr.line);
		const Ref choice = convert(condition, with_kind(type, Scalar::boolean), expr.line);
		if (all_immediate(choice)) {
			Ref result = yes;
			for (size_t k = 0; k < result.locs.size(); ++k)
				if (!choice.locs[k].bits)
					result.locs[k] = no.locs[k];
			result.writable = false;
			return result;
		}
		return apply(sm4::movc, type, {choice, yes, no});
	}

	Ref cast(const Type &to, Ref value, uint32_t line)
	{
		value = load(value);
		if (to == value.type)
			return value;
		if (to.numeric() && value.type.numeric()) {
			const Ref converted = convert_kind(value, to.scalar);
			const uint32_t from_count = flat_size(value.type), to_count = flat_size(to);
			if (from_count == 1 || from_count == to_count || (to.shape == Type::Shape::matrix && value.type.shape == Type::Shape::matrix))
				return convert_shape(converted, to, line);
			if (to_count > from_count)
				fail(line, "cannot convert from '" + value.type.name() + "' to '" + to.name() + "'");
			Ref result = converted;
			result.type = to;
			result.locs.resize(to_count);
			result.writable = false;
			return result;
		}
		if (to.shape == Type::Shape::object || value.type.shape == Type::Shape::object)
			fail(line, "cannot convert from '" + value.type.name() + "' to '" + to.name() + "'");
		std::vector<std::pair<Loc, Scalar>> scalars;
		scalars_of(value, scalars);
		std::vector<Scalar> kinds;
		flat_kinds(to, kinds);
		if (scalars.size() == 1)
			scalars.assign(kinds.size(), scalars[0]);
		if (scalars.size() < kinds.size())
			fail(line, "cannot convert from '" + value.type.name() + "' to '" + to.name() + "'");
		scalars.resize(kinds.size());
		return from_scalars(scalars, to, line);
	}

	Ref construct(const Expr &expr)
	{
		const Type &to = expr.cast_type;
		Ref result;
		result.type = to;
		for (const auto &operand : expr.operands) {
			const Ref part = value(*operand);
			if (!part.type.numeric())
				fail(expr.line, "constructors take numeric values");
			const Ref converted = convert_kind(part, to.scalar);
			result.locs.insert(result.locs.end(), converted.locs.begin(), converted.locs.end());
		}
		if (result.locs.size() == 1 && to.components() > 1)
			result.locs.assign(to.components(), result.locs[0]);
		if (result.locs.size() != to.components())
			fail(expr.line, "incorrect number of arguments to numeric-type constructor");
		return result;
	}

	Ref eval(const Expr &expr)
	{
		switch (expr.kind) {
		case Expr::Kind::literal:
			return literal(expr);
		case Expr::Kind::name:
			return lookup(expr);
		case Expr::Kind::unary:
			return unary(expr);
		case Expr::Kind::binary:
			if (expr.text == ",") {
				eval(*expr.operands[0]);
				return eval(*expr.operands[1]);
			}
			return binary_op(expr.text, value(*expr.operands[0]), value(*expr.operands[1]), expr.line);
		case Expr::Kind::assign:
			return assignment(expr);
		case Expr::Kind::ternary:
			return ternary(expr);
		case Expr::Kind::call:
			return call(expr);
		case Expr::Kind::member:
			return member(expr);
		case Expr::Kind::index:
			return index(expr);
		case Expr::Kind::cast:
			return cast(expr.cast_type, value(*expr.operands[0]), expr.line);
		case Expr::Kind::construct:
			return construct(expr);
		case Expr::Kind::initializer_list:
			fail(expr.line, "unexpected initializer list");
		case Expr::Kind::postfix:
			return increment(*expr.operands[0], expr.text == "++", true, expr.line);
		}
		fail(expr.line, "unknown expression");
	}

	Ref value(const Expr &expr) { return load(eval(expr)); }

	// ---- intrinsic functions ----

	static Type float_of(const Type &type) { return is_float(type.scalar) ? type : with_kind(type, Scalar::floating); }

	Ref floats(const Ref &value, uint32_t line) { return convert(value, float_of(value.type), line); }

	Type joint(const std::vector<Ref> &arguments, uint32_t line)
	{
		Type type = arguments[0].type;
		for (size_t i = 1; i < arguments.size(); ++i)
			type = common_type(type, arguments[i].type, line);
		return type;
	}

	Ref fconst(const Type &type, float value) { return splat(type, float_bits(value)); }
	Ref fadd(const Ref &a, const Ref &b) { return apply(sm4::add, a.type, {a, b}); }
	Ref fsub(const Ref &a, const Ref &b) { return apply(sm4::add, a.type, {a, b}, false, {false, true}); }
	Ref fmul(const Ref &a, const Ref &b) { return apply(sm4::mul, a.type, {a, b}); }
	Ref fmad(const Ref &a, const Ref &b, const Ref &c) { return apply(sm4::mad, a.type, {a, b, c}); }
	Ref fop(uint16_t opcode, const Ref &a, bool absolute = false) { return apply(opcode, a.type, {a}, false, {}, {absolute}); }
	Ref fmovc(const Ref &condition, const Ref &a, const Ref &b) { return apply(sm4::movc, a.type, {condition, a, b}); }
	Ref fless(const Ref &a, const Ref &b) { return apply(sm4::lt, with_kind(a.type, Scalar::boolean), {a, b}); }

	Ref reciprocal(const Ref &a)
	{
		if (sm5)
			return fop(sm4::rcp, a);
		return apply(sm4::div, a.type, {fconst(a.type, 1), a});
	}

	Ref sine(const Ref &a, bool cosine)
	{
		Ref result = new_value(a.type);
		for (uint32_t r = 0; r < row_count(a.type); ++r) {
			const auto dest = row(result, r);
			const Operand target = dest_of(dest[0], uint8_t((1 << dest.size()) - 1));
			const Operand none = Operand::none(sm4::null_register);
			emit(sm4::sincos, {cosine ? none : target, cosine ? target : none, source_first(readable(row(a, r)))});
		}
		return result;
	}

	// Writes a scalar into dest from the dot product of two lists of the same length.
	void dot_into(const Loc &dest, const std::vector<Loc> &a, const std::vector<Loc> &b, Scalar kind)
	{
		const Operand target = dest_of(dest, uint8_t(1 << dest.component));
		const auto left = readable(a), right = readable(b);
		if (is_float(kind)) {
			static const uint16_t opcodes[] = {sm4::mul, sm4::mul, sm4::dp2, sm4::dp3, sm4::dp4};
			emit(opcodes[a.size()], {target, source_first(left), source_first(right)});
			return;
		}
		emit(sm4::imul, {Operand::none(sm4::null_register), target, scalar_source(left[0]), scalar_source(right[0])});
		for (size_t k = 1; k < a.size(); ++k)
			emit(sm4::imad, {target, scalar_source(left[k]), scalar_source(right[k]), scalar_source(dest)});
	}

	Ref dot(const Ref &a, const Ref &b, uint32_t line)
	{
		Type type = common_type(a.type, b.type, line);
		if (type.scalar == Scalar::boolean)
			type.scalar = Scalar::integer;
		const Ref left = convert(a, type, line), right = convert(b, type, line);
		Ref result = new_value(scalar_type(type.scalar));
		dot_into(result.locs[0], left.locs, right.locs, type.scalar);
		return result;
	}

	static std::vector<Loc> column(const Ref &matrix, uint32_t index)
	{
		std::vector<Loc> locs;
		for (uint32_t r = 0; r < matrix.type.rows; ++r)
			locs.push_back(matrix.locs[r * matrix.type.columns + index]);
		return locs;
	}

	Ref multiply(Ref a, Ref b, uint32_t line)
	{
		if (!a.type.numeric() || !b.type.numeric())
			fail(line, "mul needs numeric arguments");
		if (a.type.shape == Type::Shape::scalar || b.type.shape == Type::Shape::scalar)
			return binary_op("*", a, b, line);
		Scalar kind = rank(a.type.scalar) >= rank(b.type.scalar) ? a.type.scalar : b.type.scalar;
		if (kind == Scalar::boolean)
			kind = Scalar::integer;
		a = convert_kind(a, kind);
		b = convert_kind(b, kind);
		const bool left_matrix = a.type.shape == Type::Shape::matrix, right_matrix = b.type.shape == Type::Shape::matrix;
		if (!left_matrix && !right_matrix) {
			const uint32_t n = std::min(a.type.columns, b.type.columns);
			a.locs.resize(n);
			b.locs.resize(n);
			a.type = b.type = Type::make_vector(kind, uint8_t(n));
			return dot(a, b, line);
		}
		if (!left_matrix) {
			const uint32_t n = std::min<uint32_t>(a.type.columns, b.type.rows);
			Ref result = new_value(Type::make_vector(kind, b.type.columns));
			for (uint32_t j = 0; j < b.type.columns; ++j) {
				auto part = column(b, j);
				part.resize(n);
				dot_into(result.locs[j], std::vector<Loc>(a.locs.begin(), a.locs.begin() + n), part, kind);
			}
			return result;
		}
		if (!right_matrix) {
			const uint32_t n = std::min<uint32_t>(a.type.columns, b.type.columns);
			Ref result = new_value(Type::make_vector(kind, a.type.rows));
			for (uint32_t i = 0; i < a.type.rows; ++i) {
				auto part = row(a, i);
				part.resize(n);
				dot_into(result.locs[i], part, std::vector<Loc>(b.locs.begin(), b.locs.begin() + n), kind);
			}
			return result;
		}
		if (a.type.columns != b.type.rows)
			fail(line, "mul of '" + a.type.name() + "' and '" + b.type.name() + "' does not match");
		Ref result = new_value(Type::make_matrix(kind, a.type.rows, b.type.columns));
		for (uint32_t i = 0; i < a.type.rows; ++i)
			for (uint32_t j = 0; j < b.type.columns; ++j)
				dot_into(result.locs[i * b.type.columns + j], row(a, i), column(b, j), kind);
		return result;
	}

	Ref transpose(const Ref &value)
	{
		Ref result;
		result.type = value.type;
		if (value.type.shape != Type::Shape::matrix)
			return value;
		result.type.rows = value.type.columns;
		result.type.columns = value.type.rows;
		for (uint32_t c = 0; c < value.type.columns; ++c)
			for (uint32_t r = 0; r < value.type.rows; ++r)
				result.locs.push_back(value.locs[r * value.type.columns + c]);
		return result;
	}

	Ref scalar_at(const Ref &value, uint32_t index)
	{
		Ref result;
		result.type = scalar_type(value.type.scalar);
		result.locs = {value.locs[index]};
		return result;
	}

	// The determinant by expansion along the first row.
	Ref determinant(const Ref &matrix, uint32_t line)
	{
		const uint32_t n = matrix.type.rows;
		if (n != matrix.type.columns)
			fail(line, "determinant needs a square matrix");
		if (n == 1)
			return scalar_at(matrix, 0);
		Ref total;
		for (uint32_t c = 0; c < n; ++c) {
			Ref minor;
			minor.type = Type::make_matrix(matrix.type.scalar, uint8_t(n - 1), uint8_t(n - 1));
			for (uint32_t r = 1; r < n; ++r)
				for (uint32_t k = 0; k < n; ++k)
					if (k != c)
						minor.locs.push_back(matrix.locs[r * n + k]);
			const Ref term = fmul(scalar_at(matrix, c), n - 1 == 1 ? scalar_at(minor, 0) : determinant(minor, line));
			total = c == 0 ? term : (c % 2 ? fsub(total, term) : fadd(total, term));
		}
		return total;
	}

	Ref reduce(const Ref &value, uint16_t opcode, uint32_t line)
	{
		const Ref mask = convert(value, with_kind(value.type, Scalar::boolean), line);
		Ref result = scalar_at(mask, 0);
		for (uint32_t k = 1; k < mask.locs.size(); ++k)
			result = apply(opcode, result.type, {result, scalar_at(mask, k)});
		return result;
	}

	// The polynomial of arctangent on [0, 1] (Abramowitz and Stegun 4.4.49).
	Ref atan_unit(const Ref &t)
	{
		const Ref square = fmul(t, t);
		Ref sum = fconst(t.type, 0.0208351f);
		for (const float coefficient : {-0.0851330f, 0.1801410f, -0.3302995f, 0.9998660f})
			sum = fmad(sum, square, fconst(t.type, coefficient));
		return fmul(sum, t);
	}

	Ref arctangent2(const Ref &y, const Ref &x)
	{
		const Type type = x.type;
		const Ref ax = fop(sm4::mov, x, true), ay = fop(sm4::mov, y, true);
		const Ref t = apply(sm4::div, type, {apply(sm4::min, type, {ax, ay}), apply(sm4::max, type, {ax, ay})});
		Ref angle = atan_unit(t);
		angle = fmovc(fless(ax, ay), fsub(fconst(type, 1.5707963f), angle), angle);
		angle = fmovc(fless(x, zero(type)), fsub(fconst(type, 3.1415927f), angle), angle);
		return fmovc(fless(y, zero(type)), apply(sm4::mov, type, {angle}, false, {true}), angle);
	}

	Ref arctangent(const Ref &x)
	{
		const Type type = x.type;
		const Ref ax = fop(sm4::mov, x, true);
		const Ref big = fless(fconst(type, 1), ax);
		const Ref t = fmovc(big, reciprocal(ax), ax);
		Ref angle = atan_unit(t);
		angle = fmovc(big, fsub(fconst(type, 1.5707963f), angle), angle);
		return fmovc(fless(x, zero(type)), apply(sm4::mov, type, {angle}, false, {true}), angle);
	}

	// Arccosine on [-1, 1] (Abramowitz and Stegun 4.4.45), folded for negative values.
	Ref arccosine(const Ref &x)
	{
		const Type type = x.type;
		const Ref ax = fop(sm4::mov, x, true);
		Ref sum = fconst(type, -0.0187293f);
		for (const float coefficient : {0.0742610f, -0.2121144f, 1.5707288f})
			sum = fmad(sum, ax, fconst(type, coefficient));
		const Ref root = fop(sm4::sqrt, fsub(fconst(type, 1), ax));
		const Ref angle = fmul(sum, root);
		return fmovc(fless(x, zero(type)), fsub(fconst(type, 3.1415927f), angle), angle);
	}

	Ref exponential(const Ref &x, float scale) { return fop(sm4::exp, fmul(x, fconst(x.type, scale))); }

	void need_sm5(const std::string &name, uint32_t line)
	{
		if (!sm5)
			fail(line, "'" + name + "' needs shader model 5");
	}

	Ref intrinsic(const Expr &expr)
	{
		const auto &name = expr.text;
		const uint32_t line = expr.line;
		const size_t count = expr.operands.size();
		const auto expect = [&](size_t wanted) {
			if (count != wanted)
				fail(line, "'" + name + "' takes " + std::to_string(wanted) + " arguments");
		};
		// Intrinsics with out arguments read their arguments themselves.
		if (name == "sincos") {
			expect(3);
			const Ref x = floats(value(*expr.operands[0]), line);
			const Ref s = eval(*expr.operands[1]), c = eval(*expr.operands[2]);
			assign_to(s, convert(sine(x, false), s.type, line), line);
			assign_to(c, convert(sine(x, true), c.type, line), line);
			return {};
		}
		if (name == "modf") {
			expect(2);
			const Ref x = floats(value(*expr.operands[0]), line);
			const Ref whole = fop(sm4::round_z, x);
			const Ref target = eval(*expr.operands[1]);
			assign_to(target, convert(whole, target.type, line), line);
			return fsub(x, whole);
		}
		if (name.starts_with("Interlocked")) {
			if (expr.operands.empty())
				fail(line, "'" + name + "' needs a destination");
			std::vector<const Expr *> rest;
			for (size_t i = 1; i < count; ++i)
				rest.push_back(expr.operands[i].get());
			return interlocked(name, eval(*expr.operands[0]), rest, line);
		}
		if (name == "GroupMemoryBarrierWithGroupSync" || name == "GroupMemoryBarrier" || name == "DeviceMemoryBarrier" ||
			name == "DeviceMemoryBarrierWithGroupSync" || name == "AllMemoryBarrier" || name == "AllMemoryBarrierWithGroupSync") {
			if (stage != Stage::compute && name.ends_with("Sync"))
				fail(line, "'" + name + "' is only valid in compute shaders");
			// Thread group sync 1, group shared memory 2, unordered access memory of the group 4 or of the device 8.
			uint32_t controls = name.ends_with("Sync") ? 1 : 0;
			if (name.starts_with("Group"))
				controls |= 2;
			else if (name.starts_with("Device"))
				controls |= 8;
			else
				controls |= 2 | 8;
			if (stage != Stage::compute)
				controls &= ~2u;
			emit(sm4::sync, {}, controls);
			return {};
		}
		std::vector<Ref> args;
		for (const auto &operand : expr.operands)
			args.push_back(value(*operand));
		for (const auto &arg : args)
			if (!arg.type.numeric())
				fail(line, "'" + name + "' needs numeric arguments");
		const auto unary_float = [&](uint16_t opcode) {
			expect(1);
			return fop(opcode, floats(args[0], line));
		};
		if (name == "mul") {
			expect(2);
			return multiply(args[0], args[1], line);
		}
		if (name == "dot") {
			expect(2);
			if (args[0].type.shape == Type::Shape::matrix || args[1].type.shape == Type::Shape::matrix)
				fail(line, "dot needs vectors");
			const size_t left = args[0].locs.size(), right = args[1].locs.size();
			const uint32_t n = uint32_t(left == 1 ? right : right == 1 ? left : std::min(left, right));
			const Ref a = convert_shape(args[0], Type::make_vector(args[0].type.scalar, uint8_t(n)), line);
			const Ref b = convert_shape(args[1], Type::make_vector(args[1].type.scalar, uint8_t(n)), line);
			return dot(a, b, line);
		}
		if (name == "abs") {
			expect(1);
			const Ref x = args[0];
			if (is_float(x.type.scalar))
				return fop(sm4::mov, x, true);
			if (x.type.scalar == Scalar::integer)
				return apply(sm4::imax, x.type, {x, x}, false, {false, true});
			return x;
		}
		if (name == "saturate") {
			expect(1);
			return apply(sm4::mov, float_of(args[0].type), {floats(args[0], line)}, true);
		}
		if (name == "min" || name == "max") {
			expect(2);
			Type type = joint(args, line);
			if (type.scalar == Scalar::boolean)
				type.scalar = Scalar::integer;
			const bool minimum = name == "min";
			const uint16_t opcode = is_float(type.scalar) ? (minimum ? sm4::min : sm4::max)
				: type.scalar == Scalar::integer ? (minimum ? sm4::imin : sm4::imax) : (minimum ? sm4::umin : sm4::umax);
			return apply(opcode, type, {convert(args[0], type, line), convert(args[1], type, line)});
		}
		if (name == "clamp") {
			expect(3);
			Type type = joint(args, line);
			if (type.scalar == Scalar::boolean)
				type.scalar = Scalar::integer;
			const bool fp = is_float(type.scalar), sign = type.scalar == Scalar::integer;
			const Ref low = apply(fp ? sm4::max : sign ? sm4::imax : sm4::umax, type, {convert(args[0], type, line), convert(args[1], type, line)});
			return apply(fp ? sm4::min : sign ? sm4::imin : sm4::umin, type, {low, convert(args[2], type, line)});
		}
		if (name == "lerp") {
			expect(3);
			const Type type = float_of(joint(args, line));
			const Ref a = convert(args[0], type, line), b = convert(args[1], type, line), t = convert(args[2], type, line);
			return fmad(t, fsub(b, a), a);
		}
		if (name == "mad") {
			expect(3);
			Type type = joint(args, line);
			if (type.scalar == Scalar::boolean)
				type.scalar = Scalar::integer;
			const uint16_t opcode = is_float(type.scalar) ? sm4::mad : type.scalar == Scalar::integer ? sm4::imad : sm4::umad;
			return apply(opcode, type, {convert(args[0], type, line), convert(args[1], type, line), convert(args[2], type, line)});
		}
		if (name == "cross") {
			expect(2);
			const Type type = Type::make_vector(Scalar::floating, 3);
			const Ref a = convert(args[0], type, line), b = convert(args[1], type, line);
			const auto swizzled = [](const Ref &v, uint32_t x, uint32_t y, uint32_t z) {
				Ref result;
				result.type = v.type;
				result.locs = {v.locs[x], v.locs[y], v.locs[z]};
				return result;
			};
			const Ref product = fmul(swizzled(a, 2, 0, 1), swizzled(b, 1, 2, 0));
			return apply(sm4::mad, type, {swizzled(a, 1, 2, 0), swizzled(b, 2, 0, 1), product}, false, {false, false, true});
		}
		if (name == "length") {
			expect(1);
			const Ref v = floats(args[0], line);
			if (v.locs.size() == 1)
				return fop(sm4::mov, v, true);
			return fop(sm4::sqrt, dot(v, v, line));
		}
		if (name == "distance") {
			expect(2);
			const Type type = float_of(joint(args, line));
			const Ref d = fsub(convert(args[0], type, line), convert(args[1], type, line));
			if (d.locs.size() == 1)
				return fop(sm4::mov, d, true);
			return fop(sm4::sqrt, dot(d, d, line));
		}
		if (name == "normalize") {
			expect(1);
			const Ref v = floats(args[0], line);
			return fmul(v, convert_shape(fop(sm4::rsq, dot(v, v, line)), v.type, line));
		}
		if (name == "sqrt")
			return unary_float(sm4::sqrt);
		if (name == "rsqrt")
			return unary_float(sm4::rsq);
		if (name == "rcp") {
			expect(1);
			return reciprocal(floats(args[0], line));
		}
		if (name == "exp2")
			return unary_float(sm4::exp);
		if (name == "log2")
			return unary_float(sm4::log);
		if (name == "frac")
			return unary_float(sm4::frc);
		if (name == "floor")
			return unary_float(sm4::round_ni);
		if (name == "ceil")
			return unary_float(sm4::round_pi);
		if (name == "round")
			return unary_float(sm4::round_ne);
		if (name == "trunc")
			return unary_float(sm4::round_z);
		if (name == "ddx" || name == "ddy")
			return unary_float(name == "ddx" ? sm4::deriv_rtx : sm4::deriv_rty);
		if (name == "ddx_coarse" || name == "ddx_fine" || name == "ddy_coarse" || name == "ddy_fine") {
			need_sm5(name, line);
			const uint16_t opcode = name == "ddx_coarse" ? sm4::deriv_rtx_coarse : name == "ddx_fine" ? sm4::deriv_rtx_fine
				: name == "ddy_coarse" ? sm4::deriv_rty_coarse : sm4::deriv_rty_fine;
			return unary_float(opcode);
		}
		if (name == "fwidth") {
			expect(1);
			const Ref x = floats(args[0], line);
			return apply(sm4::add, x.type, {fop(sm4::deriv_rtx, x), fop(sm4::deriv_rty, x)}, false, {}, {true, true});
		}
		if (name == "exp") {
			expect(1);
			return exponential(floats(args[0], line), 1.44269504f);
		}
		if (name == "log" || name == "log10") {
			expect(1);
			const Ref x = floats(args[0], line);
			return fmul(fop(sm4::log, x), fconst(x.type, name == "log" ? 0.693147181f : 0.301029996f));
		}
		if (name == "pow") {
			expect(2);
			const Type type = float_of(joint(args, line));
			const Ref x = convert(args[0], type, line), y = convert(args[1], type, line);
			return fop(sm4::exp, fmul(fop(sm4::log, x), y));
		}
		if (name == "sin" || name == "cos") {
			expect(1);
			return sine(floats(args[0], line), name == "cos");
		}
		if (name == "tan") {
			expect(1);
			const Ref x = floats(args[0], line);
			return apply(sm4::div, x.type, {sine(x, false), sine(x, true)});
		}
		if (name == "sinh" || name == "cosh" || name == "tanh") {
			expect(1);
			const Ref x = floats(args[0], line);
			const Ref up = exponential(x, 1.44269504f), down = exponential(x, -1.44269504f);
			if (name == "tanh")
				return apply(sm4::div, x.type, {fsub(up, down), fadd(up, down)});
			return fmul(name == "sinh" ? fsub(up, down) : fadd(up, down), fconst(x.type, 0.5f));
		}
		if (name == "acos") {
			expect(1);
			return arccosine(floats(args[0], line));
		}
		if (name == "asin") {
			expect(1);
			const Ref x = floats(args[0], line);
			return fsub(fconst(x.type, 1.5707963f), arccosine(x));
		}
		if (name == "atan") {
			expect(1);
			return arctangent(floats(args[0], line));
		}
		if (name == "atan2") {
			expect(2);
			const Type type = float_of(joint(args, line));
			return arctangent2(convert(args[0], type, line), convert(args[1], type, line));
		}
		if (name == "degrees" || name == "radians") {
			expect(1);
			const Ref x = floats(args[0], line);
			return fmul(x, fconst(x.type, name == "degrees" ? 57.29577951f : 0.01745329252f));
		}
		if (name == "fmod") {
			expect(2);
			const Type type = float_of(joint(args, line));
			return float_remainder(convert(args[0], type, line), convert(args[1], type, line), type);
		}
		if (name == "ldexp") {
			expect(2);
			const Type type = float_of(joint(args, line));
			return fmul(convert(args[0], type, line), fop(sm4::exp, convert(args[1], type, line)));
		}
		if (name == "step") {
			expect(2);
			const Type type = float_of(joint(args, line));
			const Ref at_least = apply(sm4::ge, with_kind(type, Scalar::boolean), {convert(args[1], type, line), convert(args[0], type, line)});
			return apply(sm4::op_and, type, {at_least, fconst(type, 1)});
		}
		if (name == "smoothstep") {
			expect(3);
			const Type type = float_of(joint(args, line));
			const Ref a = convert(args[0], type, line), b = convert(args[1], type, line), x = convert(args[2], type, line);
			const Ref t = apply(sm4::mov, type, {apply(sm4::div, type, {fsub(x, a), fsub(b, a)})}, true);
			return fmul(fmul(t, t), fmad(t, fconst(type, -2), fconst(type, 3)));
		}
		if (name == "sign") {
			expect(1);
			const Ref x = args[0].type.scalar == Scalar::boolean ? convert(args[0], with_kind(args[0].type, Scalar::integer), line) : args[0];
			const Type mask = with_kind(x.type, Scalar::boolean), result = with_kind(x.type, Scalar::integer);
			const bool fp = is_float(x.type.scalar);
			if (x.type.scalar == Scalar::unsigned_integer)
				return apply(sm4::op_and, result, {apply(sm4::ine, mask, {x, zero(x.type)}), splat(result, 1)});
			const Ref positive = apply(fp ? sm4::lt : sm4::ilt, mask, {zero(x.type), x});
			const Ref negative = apply(fp ? sm4::lt : sm4::ilt, mask, {x, zero(x.type)});
			return apply(sm4::iadd, result, {negative, positive}, false, {false, true});
		}
		if (name == "reflect") {
			expect(2);
			const Type type = float_of(joint(args, line));
			const Ref i = convert(args[0], type, line), n = convert(args[1], type, line);
			const Ref d = convert_shape(fmul(dot(n, i, line), fconst(scalar_type(Scalar::floating), -2)), type, line);
			return fmad(d, n, i);
		}
		if (name == "refract") {
			expect(3);
			const Type type = float_of(joint({args[0], args[1]}, line));
			const Ref i = convert(args[0], type, line), n = convert(args[1], type, line);
			const Ref eta = convert(args[2], scalar_type(Scalar::floating), line);
			const Ref cosine = dot(n, i, line);
			const Ref one = fconst(eta.type, 1);
			const Ref k = fsub(one, fmul(fmul(eta, eta), fsub(one, fmul(cosine, cosine))));
			const Ref scale = fadd(fmul(eta, cosine), fop(sm4::sqrt, k));
			const Ref refracted = fsub(fmul(convert_shape(eta, type, line), i), fmul(convert_shape(scale, type, line), n));
			return fmovc(convert_shape(fless(k, zero(k.type)), with_kind(type, Scalar::boolean), line), zero(type), refracted);
		}
		if (name == "faceforward") {
			expect(3);
			const Type type = float_of(joint(args, line));
			const Ref n = convert(args[0], type, line);
			const Ref facing = fless(dot(convert(args[1], type, line), convert(args[2], type, line), line), zero(scalar_type(Scalar::floating)));
			return apply(sm4::movc, type, {convert_shape(facing, with_kind(type, Scalar::boolean), line), n, n}, false, {false, false, true});
		}
		if (name == "any" || name == "all") {
			expect(1);
			return reduce(args[0], name == "any" ? sm4::op_or : sm4::op_and, line);
		}
		if (name == "isnan" || name == "isinf" || name == "isfinite") {
			expect(1);
			const Ref x = floats(args[0], line);
			const Type mask = with_kind(x.type, Scalar::boolean);
			if (name == "isnan")
				return apply(sm4::ne, mask, {x, x});
			const Ref infinity = splat(x.type, 0x7f800000u);
			if (name == "isinf")
				return apply(sm4::eq, mask, {x, infinity}, false, {}, {true});
			return apply(sm4::lt, mask, {x, infinity}, false, {}, {true});
		}
		if (name == "asfloat" || name == "asint" || name == "asuint") {
			expect(1);
			Ref result = args[0];
			result.type.scalar = name == "asfloat" ? Scalar::floating : name == "asint" ? Scalar::integer : Scalar::unsigned_integer;
			result.writable = false;
			return result;
		}
		if (name == "transpose") {
			expect(1);
			return transpose(args[0]);
		}
		if (name == "determinant") {
			expect(1);
			if (args[0].type.shape != Type::Shape::matrix)
				return floats(args[0], line);
			return determinant(floats(args[0], line), line);
		}
		if (name == "clip") {
			expect(1);
			const Ref x = floats(args[0], line);
			const Ref below = reduce(fless(x, zero(x.type)), sm4::op_or, line);
			emit(sm4::discard, {scalar_source(below.locs[0])}, 1 << 7);
			uses_discard = true;
			return {};
		}
		if (name == "countbits" || name == "reversebits" || name == "firstbitlow" || name == "firstbithigh") {
			need_sm5(name, line);
			expect(1);
			const Type type = with_kind(args[0].type, args[0].type.scalar == Scalar::integer ? Scalar::integer : Scalar::unsigned_integer);
			const Ref x = convert(args[0], type, line);
			if (name != "firstbithigh")
				return apply(name == "countbits" ? sm4::countbits : name == "reversebits" ? sm4::bfrev : sm4::firstbit_lo, type, {x});
			// firstbit_hi counts from the top bit; firstbithigh counts from the bottom, and both give ~0 for no bit. One
			// component at a time, which every translator handles.
			Ref result = new_value(type);
			for (uint32_t k = 0; k < x.locs.size(); ++k) {
				const Ref top = apply(type.scalar == Scalar::integer ? sm4::firstbit_shi : sm4::firstbit_hi, scalar_type(type.scalar), {scalar_at(x, k)});
				const Ref found = apply(sm4::ine, scalar_type(Scalar::boolean), {top, splat(top.type, ~0u)});
				const Ref bottom = apply(sm4::iadd, top.type, {splat(top.type, 31), top}, false, {false, true});
				write({result.locs[k]}, apply(sm4::movc, top.type, {found, bottom, top}).locs);
			}
			return result;
		}
		if (name == "f32tof16" || name == "f16tof32") {
			need_sm5(name, line);
			expect(1);
			if (name == "f32tof16")
				return apply(sm4::f32tof16, with_kind(args[0].type, Scalar::unsigned_integer), {floats(args[0], line)});
			return apply(sm4::f16tof32, with_kind(args[0].type, Scalar::floating),
				{convert(args[0], with_kind(args[0].type, Scalar::unsigned_integer), line)});
		}
		fail(line, "unknown function '" + name + "'");
	}

	// The address an atomic operation takes: typed coordinates, or the element index and byte offset.
	std::vector<Loc> atomic_address(const Access &access)
	{
		if (access.kind == Access::Kind::typed)
			return readable(access.address);
		return readable({access.address[0], byte_offset(access, access.offsets[0])});
	}

	Operand atomic_target(const Access &access)
	{
		Operand operand = access_operand(access);
		operand.components = 0;
		return operand;
	}

	// Interlocked operations on a buffer element or group shared value, with the original value returned through the last argument.
	Ref interlocked(const std::string &name, const Ref &target, const std::vector<const Expr *> &rest, uint32_t line)
	{
		if (!target.access || !target.assignable() || target.type.components() != 1)
			fail(line, "'" + name + "' needs a scalar element of a writable buffer or of group shared memory");
		if (target.access->kind != Access::Kind::shared)
			resources[size_t(target.access->target)].used = true;
		const Type type = scalar_type(target.type.scalar == Scalar::integer ? Scalar::integer : Scalar::unsigned_integer);
		const bool sign = type.scalar == Scalar::integer;
		const auto address = atomic_address(*target.access);
		const Operand unit = atomic_target(*target.access);
		const auto argument = [&](size_t index) { return readable(convert(value(*rest[index]), type, line).locs)[0]; };
		// The plain form, the form returning the original value, how many values it takes, and whether the original must be returned.
		struct Form {
			const char *name;
			uint16_t plain, returning;
			size_t values;
			bool must_return;
		};
		const Form forms[] = {{"InterlockedAdd", sm4::atomic_iadd, sm4::imm_atomic_iadd, 1, false},
			{"InterlockedAnd", sm4::atomic_and, sm4::imm_atomic_and, 1, false}, {"InterlockedOr", sm4::atomic_or, sm4::imm_atomic_or, 1, false},
			{"InterlockedXor", sm4::atomic_xor, sm4::imm_atomic_xor, 1, false},
			{"InterlockedMin", sign ? sm4::atomic_imin : sm4::atomic_umin, sign ? sm4::imm_atomic_imin : sm4::imm_atomic_umin, 1, false},
			{"InterlockedMax", sign ? sm4::atomic_imax : sm4::atomic_umax, sign ? sm4::imm_atomic_imax : sm4::imm_atomic_umax, 1, false},
			{"InterlockedExchange", sm4::imm_atomic_exch, sm4::imm_atomic_exch, 1, true},
			{"InterlockedCompareExchange", sm4::imm_atomic_cmp_exch, sm4::imm_atomic_cmp_exch, 2, true},
			{"InterlockedCompareStore", sm4::atomic_cmp_store, sm4::atomic_cmp_store, 2, false}};
		for (const auto &form : forms) {
			if (name != form.name)
				continue;
			const bool returning = rest.size() == form.values + 1;
			if (rest.size() != form.values + (returning || form.must_return ? 1 : 0) || (returning && form.returning == form.plain && !form.must_return))
				fail(line, "wrong number of arguments to '" + name + "'");
			std::vector<Loc> values;
			for (size_t i = 0; i < form.values; ++i)
				values.push_back(argument(i));
			std::vector<Operand> list{unit, source_first(address)};
			for (const auto &loc : values)
				list.push_back(scalar_source(loc));
			if (!returning) {
				emit(form.plain, list);
				return {};
			}
			const Ref original = eval(*rest[form.values]);
			Ref result = new_value(type);
			list.insert(list.begin(), dest_of(result.locs[0], 1));
			emit(form.returning, list);
			assign_to(original, convert(result, original.type, line), line);
			return {};
		}
		fail(line, "unknown function '" + name + "'");
	}

	// ---- resource methods ----

	std::array<int8_t, 3> sample_offset(const Expr &expr, ObjectKind kind)
	{
		const uint32_t n = offset_count(kind);
		if (!n)
			fail(expr.line, "this texture takes no offset");
		const Ref offset = convert(value(expr), Type::make_vector(Scalar::integer, uint8_t(n)), expr.line);
		if (!all_immediate(offset))
			fail(expr.line, "texture offsets must be literal values");
		std::array<int8_t, 3> result{};
		for (uint32_t k = 0; k < n; ++k) {
			const int32_t at = int32_t(offset.locs[k].bits);
			if (at < -8 || at > 7)
				fail(expr.line, "texture offsets must be in the range -8 to 7");
			result[k] = int8_t(at);
		}
		return result;
	}

	static uint32_t gradient_count(ObjectKind kind)
	{
		if (kind == ObjectKind::texture_cube || kind == ObjectKind::texture_cube_array)
			return 3;
		return offset_count(kind);
	}

	static uint32_t size_count(ObjectKind kind)
	{
		switch (kind) {
		case ObjectKind::texture1d:
		case ObjectKind::rw_texture1d: return 1;
		case ObjectKind::texture2d_array:
		case ObjectKind::texture3d:
		case ObjectKind::texture_cube_array:
		case ObjectKind::texture2dms_array:
		case ObjectKind::rw_texture2d_array:
		case ObjectKind::rw_texture3d: return 3;
		default: return 2;
		}
	}

	Ref method(const Expr &expr)
	{
		const auto &name = expr.text;
		const uint32_t line = expr.line;
		const Ref object = eval(*expr.operands[0]);
		if (object.stream < 0 && object.type.shape == Type::Shape::structure && object.type.array.empty()) {
			const auto candidates = methods(*object.type.structure, name);
			if (candidates.empty())
				fail(line, "'" + name + "' is not a method of '" + object.type.name() + "'");
			return call_function(expr, candidates, &object);
		}
		const size_t count = expr.operands.size() - 1;
		const auto argument = [&](size_t index) -> const Expr & { return *expr.operands[index + 1]; };
		const auto expect = [&](size_t low, size_t high) {
			if (count < low || count > high)
				fail(line, "wrong number of arguments to '" + name + "'");
		};
		if (object.stream >= 0)
			return stream_method(expr, object);
		auto &resource = resource_of(object, line);
		const ObjectKind kind = resource.kind;
		const bool uav = is_uav(kind);
		Operand texture = resource_operand(object, uav ? sm4::uav : sm4::resource);
		const bool texture_kind = !uav && !is_sampler(kind) && kind != ObjectKind::buffer && !is_structured(kind) && !is_raw(kind);
		const bool multisampled = kind == ObjectKind::texture2dms || kind == ObjectKind::texture2dms_array;

		if (name.starts_with("Sample") || name.starts_with("Gather") || name.starts_with("CalculateLevelOfDetail")) {
			if (!texture_kind || multisampled)
				fail(line, "this object cannot be sampled");
			expect(2, 6);
			const Ref sampler = eval(argument(0));
			const uint32_t coords = coordinate_count(kind);
			const auto address = readable(convert(value(argument(1)), Type::make_vector(Scalar::floating, uint8_t(coords)), line).locs);
			const auto scalar_argument = [&](size_t index) {
				return readable(convert(value(argument(index)), scalar_type(Scalar::floating), line).locs)[0];
			};
			const auto vector_argument = [&](size_t index, uint32_t n) {
				return readable(convert(value(argument(index)), Type::make_vector(Scalar::floating, uint8_t(n)), line).locs);
			};
			Type result_type = resource.element;
			uint16_t opcode = sm4::sample;
			std::vector<Operand> extra;
			size_t next = 2;
			uint8_t channel = 0;
			if (name == "Sample") {
			} else if (name == "SampleBias") {
				opcode = sm4::sample_b;
				extra.push_back(scalar_source(scalar_argument(next++)));
			} else if (name == "SampleLevel") {
				opcode = sm4::sample_l;
				extra.push_back(scalar_source(scalar_argument(next++)));
			} else if (name == "SampleGrad") {
				opcode = sm4::sample_d;
				extra.push_back(source_first(vector_argument(next++, gradient_count(kind))));
				extra.push_back(source_first(vector_argument(next++, gradient_count(kind))));
			} else if (name == "SampleCmp" || name == "SampleCmpLevelZero") {
				opcode = name == "SampleCmp" ? sm4::sample_c : sm4::sample_c_lz;
				extra.push_back(scalar_source(scalar_argument(next++)));
				result_type = scalar_type(Scalar::floating);
				texture.swizzle = {0, 0, 0, 0};
			} else if (name == "CalculateLevelOfDetail" || name == "CalculateLevelOfDetailUnclamped") {
				opcode = sm4::lod;
				result_type = scalar_type(Scalar::floating);
				const uint8_t which = name == "CalculateLevelOfDetail" ? 0 : 1;
				texture.swizzle = {which, which, which, which};
			} else if (name.starts_with("Gather")) {
				std::string rest = name.substr(6);
				const bool compare = rest.starts_with("Cmp");
				if (compare)
					rest = rest.substr(3);
				static const char *const channels[] = {"Red", "Green", "Blue", "Alpha"};
				for (uint8_t c = 0; c < 4; ++c)
					if (rest == channels[c])
						channel = uint8_t(c + 1);
				if (!rest.empty() && !channel)
					fail(line, "unknown method '" + name + "'");
				if ((channel || compare) && !sm5)
					fail(line, "'" + name + "' needs shader model 5");
				if (!sm5 && !(out.major == 4 && out.minor >= 1))
					fail(line, "Gather needs shader model 4.1");
				opcode = compare ? sm4::gather4_c : sm4::gather4;
				if (compare)
					extra.push_back(scalar_source(scalar_argument(next++)));
				result_type = Type::make_vector(resource.element.scalar, 4);
				if (!channel)
					channel = 1;
			} else
				fail(line, "unknown method '" + name + "'");
			std::optional<std::array<int8_t, 3>> offset;
			if (next < count && opcode != sm4::lod)
				offset = sample_offset(argument(next++), kind);
			Ref result = new_value(result_type);
			std::vector<Operand> list{dest_of(result.locs[0], uint8_t((1 << result.locs.size()) - 1)), source_first(address), texture,
				sampler_operand(sampler, line, channel)};
			list.insert(list.end(), extra.begin(), extra.end());
			emit(opcode, list).offset = offset;
			return result;
		}

		if (name == "Load" || name == "Load2" || name == "Load3" || name == "Load4") {
			if (is_raw(kind)) {
				expect(1, 1);
				const uint32_t n = name == "Load" ? 1 : uint32_t(name.back() - '0');
				const auto address = readable(convert(value(argument(0)), scalar_type(Scalar::unsigned_integer), line).locs);
				Ref result = new_value(Type::make_vector(Scalar::unsigned_integer, uint8_t(n)));
				emit(sm4::ld_raw, {dest_of(result.locs[0], uint8_t((1 << n) - 1)), scalar_source(address[0]), texture});
				return result;
			}
			if (name != "Load")
				fail(line, "unknown method '" + name + "'");
			if (is_structured(kind) || uav || kind == ObjectKind::buffer) {
				expect(1, 1);
				return load(element_access(object, value(argument(0)), line));
			}
			if (is_sampler(kind))
				fail(line, "unknown method '" + name + "'");
			const uint32_t coords = coordinate_count(kind) + (multisampled ? 0 : 1);
			expect(multisampled ? 2 : 1, multisampled ? 3 : 2);
			const auto address = readable(convert(value(argument(0)), Type::make_vector(Scalar::integer, uint8_t(coords)), line).locs);
			Ref result = new_value(resource.element);
			std::vector<Operand> list{dest_of(result.locs[0], uint8_t((1 << result.locs.size()) - 1)), source_first(address), texture};
			size_t next = 1;
			if (multisampled)
				list.push_back(scalar_source(readable(convert(value(argument(next++)), scalar_type(Scalar::integer), line).locs)[0]));
			std::optional<std::array<int8_t, 3>> offset;
			if (next < count)
				offset = sample_offset(argument(next), kind);
			emit(multisampled ? sm4::ld_ms : sm4::ld, list).offset = offset;
			return result;
		}

		if (name == "Store" || name == "Store2" || name == "Store3" || name == "Store4") {
			if (kind != ObjectKind::rw_byte_address_buffer)
				fail(line, "unknown method '" + name + "'");
			expect(2, 2);
			const uint32_t n = name == "Store" ? 1 : uint32_t(name.back() - '0');
			const auto address = readable(convert(value(argument(0)), scalar_type(Scalar::unsigned_integer), line).locs);
			const auto data = readable(convert(value(argument(1)), Type::make_vector(Scalar::unsigned_integer, uint8_t(n)), line).locs);
			Operand dest = texture;
			dest.mode = Operand::Mode::mask;
			dest.mask = uint8_t((1 << n) - 1);
			emit(sm4::store_raw, {dest, scalar_source(address[0]), source_first(data)});
			return {};
		}

		if (name.starts_with("Interlocked")) {
			if (kind != ObjectKind::rw_byte_address_buffer)
				fail(line, "unknown method '" + name + "'");
			if (!count)
				fail(line, "'" + name + "' needs an address");
			Ref target;
			target.type = scalar_type(Scalar::unsigned_integer);
			target.access = std::make_shared<Access>();
			target.access->target = object.resource;
			target.access->element = object.resource_element;
			target.access->uav = true;
			target.access->address = convert(value(argument(0)), scalar_type(Scalar::unsigned_integer), line).locs;
			std::vector<const Expr *> rest;
			for (size_t i = 1; i < count; ++i)
				rest.push_back(&argument(i));
			return interlocked(name, target, rest, line);
		}

		if (name == "IncrementCounter" || name == "DecrementCounter") {
			if (kind != ObjectKind::rw_structured_buffer)
				fail(line, "unknown method '" + name + "'");
			expect(0, 0);
			resource.counter = true;
			Ref result = new_value(scalar_type(Scalar::unsigned_integer));
			Operand unit = texture;
			unit.components = 0;
			emit(name == "IncrementCounter" ? sm4::imm_atomic_alloc : sm4::imm_atomic_consume, {dest_of(result.locs[0], 1), unit});
			return result;
		}

		if (name == "GetDimensions") {
			std::vector<Ref> outputs;
			const auto assign_component = [&](const Ref &result, uint32_t component, const Expr &target) {
				const Ref destination = eval(target);
				assign_to(destination, convert(scalar_at(result, component), destination.type, line), line);
			};
			if (is_structured(kind) || is_raw(kind) || kind == ObjectKind::buffer || kind == ObjectKind::rw_buffer) {
				need_sm5("GetDimensions", line);
				expect(is_structured(kind) ? 2 : 1, is_structured(kind) ? 2 : 1);
				Ref result = new_value(scalar_type(Scalar::unsigned_integer));
				Operand unit = texture;
				emit(sm4::bufinfo, {dest_of(result.locs[0], 1), unit});
				assign_component(result, 0, argument(0));
				if (is_structured(kind)) {
					const Ref destination = eval(argument(1));
					assign_to(destination, convert(uint_constant(tight_size(resource.element)), destination.type, line), line);
				}
				return {};
			}
			if (is_sampler(kind))
				fail(line, "unknown method '" + name + "'");
			const uint32_t sizes = size_count(kind);
			const bool levels = !uav && !multisampled && count == sizes + 2;
			const size_t expected = multisampled ? sizes + 1 : levels ? sizes + 2 : sizes;
			if (count != expected)
				fail(line, "wrong number of arguments to 'GetDimensions'");
			const Loc mip = levels ? readable(convert(value(argument(0)), scalar_type(Scalar::unsigned_integer), line).locs)[0] : constant_loc(0);
			Ref result = new_value(Type::make_vector(Scalar::unsigned_integer, 4));
			// The return type in the controls: 2 asks for unsigned integers.
			emit(sm4::resinfo, {dest_of(result.locs[0], 0xf), scalar_source(mip), texture}, 2);
			const size_t first = levels ? 1 : 0;
			for (uint32_t k = 0; k < sizes; ++k)
				assign_component(result, k, argument(first + k));
			if (levels)
				assign_component(result, 3, argument(first + sizes));
			if (multisampled) {
				Ref samples = new_value(scalar_type(Scalar::unsigned_integer));
				emit(sm4::sample_info, {dest_of(samples.locs[0], 1), texture}, 1);
				assign_component(samples, 0, argument(sizes));
			}
			return {};
		}
		fail(line, "unknown method '" + name + "'");
	}

	Ref stream_method(const Expr &expr, const Ref &stream)
	{
		const uint32_t line = expr.line;
		if (expr.text == "RestartStrip") {
			if (expr.operands.size() != 1)
				fail(line, "RestartStrip takes no arguments");
			if (sm5)
				emit(sm4::cut_stream, {Operand::reg(sm4::stream, uint32_t(stream.stream))});
			else
				emit(sm4::cut, {});
			return {};
		}
		if (expr.text != "Append" || expr.operands.size() != 2)
			fail(line, "unknown method '" + expr.text + "'");
		const Ref vertex = convert(value(*expr.operands[1]), stream.type, line);
		emit_stream_vertex(vertex, line);
		if (sm5)
			emit(sm4::emit_stream, {Operand::reg(sm4::stream, uint32_t(stream.stream))});
		else
			emit(sm4::emit, {});
		return {};
	}

	// ---- functions ----

	static const Expr *root_of(const Expr *expr)
	{
		while (expr && (expr->kind == Expr::Kind::member || expr->kind == Expr::Kind::index))
			expr = expr->operands[0].get();
		return expr && expr->kind == Expr::Kind::name ? expr : nullptr;
	}

	// Names a function body may change: assignment targets, and arguments of calls that can write their arguments.
	void assigned_names(const Expr &expr, std::set<std::string> &names) const
	{
		const auto add = [&](const Expr &target) {
			if (const auto root = root_of(&target))
				names.insert(root->text);
		};
		if (expr.kind == Expr::Kind::assign || expr.kind == Expr::Kind::postfix || (expr.kind == Expr::Kind::unary && (expr.text == "++" || expr.text == "--")))
			add(*expr.operands[0]);
		if (expr.kind == Expr::Kind::call) {
			const bool writes = expr.text == "sincos" || expr.text == "modf" || expr.text.starts_with("Interlocked") || expr.text == "GetDimensions";
			const size_t first = expr.number == 1 ? 1 : 0;
			for (size_t i = first; i < expr.operands.size(); ++i) {
				// A function of the program writes only what reaches an out parameter of some overload.
				bool out = writes;
				for (const auto &function : program.functions)
					out |= function.name == expr.text && i - first < function.parameters.size() && function.parameters[i - first].is_out;
				if (out)
					add(*expr.operands[i]);
			}
			// A method may change the members of its object.
			if (expr.number == 1)
				for (const auto &function : program.functions)
					if (function.owner && function.name == expr.text)
						add(*expr.operands[0]);
		}
		for (const auto &operand : expr.operands)
			if (operand)
				assigned_names(*operand, names);
	}

	void assigned_names(const Stmt &stmt, std::set<std::string> &names) const
	{
		if (stmt.value)
			assigned_names(*stmt.value, names);
		if (stmt.step)
			assigned_names(*stmt.step, names);
		for (const auto &item : stmt.body)
			assigned_names(*item, names);
		for (const auto &variable : stmt.variables)
			if (variable.initializer)
				assigned_names(*variable.initializer, names);
		if (stmt.then_branch)
			assigned_names(*stmt.then_branch, names);
		if (stmt.else_branch)
			assigned_names(*stmt.else_branch, names);
		for (const auto &[label, body] : stmt.cases)
			for (const auto &item : body)
				assigned_names(*item, names);
	}

	static size_t count_returns(const Stmt &stmt)
	{
		size_t total = stmt.kind == Stmt::Kind::return_statement ? 1 : 0;
		for (const auto &item : stmt.body)
			total += count_returns(*item);
		if (stmt.then_branch)
			total += count_returns(*stmt.then_branch);
		if (stmt.else_branch)
			total += count_returns(*stmt.else_branch);
		for (const auto &[label, body] : stmt.cases)
			for (const auto &item : body)
				total += count_returns(*item);
		return total;
	}

	// A function needs its body in a loop when it returns anywhere but at its very end.
	static bool needs_wrap(const Stmt &body)
	{
		const size_t returns = count_returns(body);
		if (!returns)
			return false;
		return returns > 1 || body.body.empty() || body.body.back()->kind != Stmt::Kind::return_statement;
	}

	static int match_score(const Type &parameter, const Type &argument)
	{
		if (parameter == argument)
			return 3;
		if (parameter.shape == Type::Shape::object || argument.shape == Type::Shape::object)
			return parameter.shape == argument.shape && parameter.object == argument.object ? 2 : -1;
		if (parameter.numeric() && argument.numeric()) {
			if (parameter.shape == argument.shape && parameter.rows == argument.rows && parameter.columns == argument.columns)
				return 2;
			if (argument.components() == 1 || parameter.components() <= argument.components())
				return 1;
			return -1;
		}
		return flat_size(parameter) == flat_size(argument) && parameter.shape == argument.shape ? 1 : -1;
	}

	// The methods of a structure and of those it derives from, the nearest first.
	std::vector<const Function *> methods(const StructType &structure, const std::string &name) const
	{
		std::vector<const Function *> found;
		for (const StructType *owner = &structure; owner; owner = owner->base.get())
			for (const auto &function : program.functions)
				if (function.owner == owner && function.name == name)
					found.push_back(&function);
		return found;
	}

	// A call of a function, or of a method on self, whose operands then start after the object.
	Ref call_function(const Expr &expr, const std::vector<const Function *> &candidates, const Ref *self = nullptr)
	{
		const uint32_t line = expr.line;
		std::vector<Ref> places;
		for (size_t i = self && expr.number == 1 ? 1 : 0; i < expr.operands.size(); ++i)
			places.push_back(eval(*expr.operands[i]));
		const Function *best = nullptr;
		int best_score = -1;
		for (const auto *candidate : candidates) {
			if (candidate->parameters.size() < places.size())
				continue;
			int score = 0;
			for (size_t i = 0; i < candidate->parameters.size() && score >= 0; ++i) {
				if (i >= places.size()) {
					if (!candidate->parameters[i].default_value)
						score = -1;
					continue;
				}
				const int one = match_score(candidate->parameters[i].type, places[i].type);
				score = one < 0 ? -1 : score + one;
			}
			// A definition wins over a prototype that matches as well.
			if (score > best_score || (score == best_score && best && !best->body && candidate->body)) {
				best = candidate;
				best_score = score;
			}
		}
		if (!best)
			fail(line, "no overload of '" + expr.text + "' takes these arguments");
		const Function &function = *best;
		if (!function.body)
			fail(line, "'" + function.name + "' is declared but never defined");
		if (active.count(&function))
			fail(line, "recursive calls are not allowed");
		std::set<std::string> assigned;
		assigned_names(*function.body, assigned);
		std::map<std::string, Ref> scope;
		std::vector<std::pair<Ref, Ref>> writebacks;
		for (size_t i = 0; i < function.parameters.size(); ++i) {
			const auto &parameter = function.parameters[i];
			const Ref argument = i < places.size() ? places[i] : value(*parameter.default_value);
			// Objects and geometry shader streams are passed as themselves.
			if (parameter.type.shape == Type::Shape::object || argument.stream >= 0) {
				scope[parameter.name] = argument;
				continue;
			}
			if (parameter.is_out) {
				if (!argument.assignable())
					fail(line, "an out parameter needs an l-value");
				Ref local = storage(parameter.type);
				if (parameter.is_in)
					write(local.locs, convert(load(argument), parameter.type, line).locs);
				writebacks.emplace_back(argument, local);
				scope[parameter.name] = local;
				continue;
			}
			Ref passed = convert(load(argument), parameter.type, line);
			if (assigned.count(parameter.name)) {
				Ref local = storage(parameter.type);
				write(local.locs, passed.locs);
				scope[parameter.name] = local;
			} else {
				passed.writable = false;
				scope[parameter.name] = passed;
			}
		}
		// A method names the members of its object directly, and the object itself as this.
		if (self) {
			for (const auto &member : self->type.structure->members)
				if (!scope.count(member.name))
					scope[member.name] = *member_of(*self, member.name);
			scope["this"] = *self;
		}
		Frame frame;
		frame.function = &function;
		frame.loop_depth = loops.size();
		if (self)
			frame.self = *self;
		if (function.result.shape != Type::Shape::void_type)
			frame.result = storage(function.result);
		auto saved = std::move(scopes);
		scopes = {scope};
		active.insert(&function);
		frames.push_back(frame);
		body_of(function);
		frames.pop_back();
		active.erase(&function);
		scopes = std::move(saved);
		for (const auto &[target, local] : writebacks)
			assign_to(target, convert(local, target.type, line), line);
		Ref result = frame.result;
		result.writable = false;
		return result;
	}

	// The body of the function on top of the frames, in a block that returns leave when it returns from inside flow control.
	void body_of(const Function &function)
	{
		if (!needs_wrap(*function.body) || frames.back().entry) {
			statement(*function.body);
			return;
		}
		const uint32_t flag = new_register();
		frames.back().returned = flag;
		frames.back().loop_depth = loops.size();
		write(temp_locs(flag, 1), {constant_loc(0)});
		// A switch with only a default case, which every return breaks out of.
		emit(sm4::op_switch, {Operand::values({}, 1)});
		emit(sm4::op_default, {});
		Loop wrapper;
		wrapper.is_switch = true;
		loops.push_back(wrapper);
		statement(*function.body);
		loops.pop_back();
		emit(sm4::op_break, {});
		emit(sm4::endswitch, {});
	}

	Ref call(const Expr &expr)
	{
		if (expr.number == 1)
			return method(expr);
		// Inside a method, the other methods of the object come before free functions.
		if (!frames.empty() && frames.back().self) {
			const Ref self = *frames.back().self;
			const auto candidates = methods(*self.type.structure, expr.text);
			if (!candidates.empty())
				return call_function(expr, candidates, &self);
		}
		std::vector<const Function *> candidates;
		for (const auto &function : program.functions)
			if (function.name == expr.text && !function.owner)
				candidates.push_back(&function);
		if (!candidates.empty())
			return call_function(expr, candidates);
		return intrinsic(expr);
	}

	// ---- statements ----

	static constexpr uint32_t test_nonzero = 1 << 7;

	Loc condition(const Expr &expr)
	{
		const Ref value = this->value(expr);
		if (!value.type.numeric() || value.locs.size() != 1)
			fail(expr.line, "the condition must be a scalar");
		const Ref truth = convert(value, scalar_type(Scalar::boolean), expr.line);
		if (truth.locs[0].immediate)
			return truth.locs[0];
		return readable(truth.locs)[0];
	}

	// The loops that belong to callers or to the loop around a function body, which the function's own break cannot leave.
	size_t own_loops() const
	{
		if (frames.empty())
			return 0;
		return frames.back().loop_depth + (frames.back().returned ? 1 : 0);
	}

	// After a loop or switch inside a function that returned from within it, the enclosing loops are left as well.
	void after_breakable()
	{
		if (frames.empty() || !frames.back().returned || loops.size() <= frames.back().loop_depth)
			return;
		// A conditional break written as if and break, which is also accepted inside a switch.
		emit(sm4::op_if, {scalar_source(temp_locs(*frames.back().returned, 1)[0])}, test_nonzero);
		emit(sm4::op_break, {});
		emit(sm4::endif, {});
	}

	void scoped(const Stmt &stmt)
	{
		scopes.emplace_back();
		statement(stmt);
		scopes.pop_back();
	}

	// Whether a loop body breaks out of or continues the loop itself, past any loop or switch nested in it.
	static void jumps(const Stmt &stmt, bool in_switch, bool &breaks, bool &continues)
	{
		switch (stmt.kind) {
		case Stmt::Kind::break_statement:
			breaks |= !in_switch;
			return;
		case Stmt::Kind::continue_statement:
			continues = true;
			return;
		case Stmt::Kind::for_statement:
		case Stmt::Kind::while_statement:
		case Stmt::Kind::do_statement:
			return;
		case Stmt::Kind::switch_statement:
			for (const auto &[label, body] : stmt.cases)
				for (const auto &item : body)
					jumps(*item, true, breaks, continues);
			return;
		default:
			for (const auto &item : stmt.body)
				jumps(*item, in_switch, breaks, continues);
			if (stmt.then_branch)
				jumps(*stmt.then_branch, in_switch, breaks, continues);
			if (stmt.else_branch)
				jumps(*stmt.else_branch, in_switch, breaks, continues);
			return;
		}
	}

	// The counter after one step of an unrolled loop, when the step is a constant change of it.
	std::optional<uint32_t> stepped(const Expr &step, const std::string &name, uint32_t current, const Type &type)
	{
		const auto counter = [&](const Expr &expr) { return expr.kind == Expr::Kind::name && expr.text == name; };
		if ((step.kind == Expr::Kind::unary || step.kind == Expr::Kind::postfix) && step.operands.size() == 1 && counter(*step.operands[0])) {
			if (step.text == "++")
				return current + 1;
			if (step.text == "--")
				return current - 1;
			return std::nullopt;
		}
		if (step.kind != Expr::Kind::assign || step.operands.size() != 2 || !counter(*step.operands[0]))
			return std::nullopt;
		const Ref amount = convert(value(*step.operands[1]), type, step.line);
		if (amount.locs.size() != 1 || !amount.locs[0].immediate)
			return std::nullopt;
		const uint32_t by = amount.locs[0].bits;
		const bool is_signed = type.scalar == Scalar::integer;
		const std::string &op = step.text;
		if (op == "=")
			return by;
		if (op == "+=")
			return current + by;
		if (op == "-=")
			return current - by;
		if (op == "*=")
			return current * by;
		if ((op == "/=" || op == "%=") && by) {
			if (is_signed)
				return uint32_t(op == "/=" ? int32_t(current) / int32_t(by) : int32_t(current) % int32_t(by));
			return op == "/=" ? current / by : current % by;
		}
		if (op == "<<=")
			return current << (by & 31);
		if (op == ">>=")
			return is_signed ? uint32_t(int32_t(current) >> (by & 31)) : current >> (by & 31);
		if (op == "&=")
			return current & by;
		if (op == "|=")
			return current | by;
		if (op == "^=")
			return current ^ by;
		return std::nullopt;
	}

	// [unroll], or a loop whose body needs constants: a for loop over one integer counter with constant bounds becomes a copy of its body
	// per pass, the counter a constant in each. Returns false, having emitted nothing, for a loop that does not fit.
	bool unroll(const Stmt &stmt)
	{
		const Stmt &init = *stmt.body[0];
		std::string name;
		const Expr *start = nullptr;
		Type type;
		Ref *existing = nullptr;
		if (init.kind == Stmt::Kind::declaration && init.variables.size() == 1 && init.variables[0].initializer) {
			name = init.variables[0].name;
			start = init.variables[0].initializer.get();
			type = init.variables[0].type;
		} else if (init.kind == Stmt::Kind::expression && init.value && init.value->kind == Expr::Kind::assign && init.value->text == "=" &&
			init.value->operands[0]->kind == Expr::Kind::name && (existing = find_local(init.value->operands[0]->text)) && existing->writable &&
			!existing->access) {
			name = init.value->operands[0]->text;
			start = init.value->operands[1].get();
			type = existing->type;
		} else
			return false;
		if (type.shape != Type::Shape::scalar || !type.array.empty() || (type.scalar != Scalar::integer && type.scalar != Scalar::unsigned_integer) ||
			!stmt.value || !stmt.step)
			return false;
		std::set<std::string> assigned;
		assigned_names(*stmt.then_branch, assigned);
		bool breaks = false, continues = false;
		jumps(*stmt.then_branch, false, breaks, continues);
		if (assigned.count(name) || continues)
			return false;
		// The passes, found without emitting anything: a bound that is not a constant leaves the loop as it is.
		const size_t mark = code.size();
		const uint32_t temps_mark = temps;
		std::vector<uint32_t> passes;
		std::optional<uint32_t> current;
		scopes.emplace_back();
		const Ref first = convert(value(*start), type, stmt.line);
		if (first.locs.size() == 1 && first.locs[0].immediate)
			current = first.locs[0].bits;
		while (current) {
			scopes.back()[name] = constant(type, {*current});
			const Loc test = condition(*stmt.value);
			if (!test.immediate || passes.size() > 1024) {
				current.reset();
				break;
			}
			if (!test.bits)
				break;
			passes.push_back(*current);
			current = stepped(*stmt.step, name, *current, type);
		}
		scopes.pop_back();
		if (!current || code.size() != mark) {
			code.resize(mark);
			temps = temps_mark;
			return false;
		}
		// A break leaves the copies through a loop around them that runs once.
		Loop once;
		if (breaks) {
			emit(sm4::loop, {});
			loops.push_back(once);
		}
		for (const auto pass : passes) {
			scopes.emplace_back();
			scopes.back()[name] = constant(type, {pass});
			scoped(*stmt.then_branch);
			scopes.pop_back();
		}
		if (breaks) {
			emit(sm4::op_break, {});
			loops.pop_back();
			emit(sm4::endloop, {});
			after_breakable();
		}
		// The counter keeps its last value after the loop.
		const Ref last = constant(type, {*current});
		if (existing)
			write(existing->locs, last.locs);
		else {
			Ref place = storage(type);
			write(place.locs, last.locs);
			scopes.back()[name] = place;
		}
		return true;
	}

	void declare(const Variable &variable)
	{
		Type type = variable.type;
		if (!type.array.empty() && type.array[0] == 0) {
			if (!variable.initializer || variable.initializer->kind != Expr::Kind::initializer_list)
				fail(variable.line, "an array without a size needs an initializer list");
			std::vector<std::pair<Loc, Scalar>> scalars;
			flatten(*variable.initializer, scalars);
			Type element = element_of(type);
			const uint32_t per = flat_size(element);
			if (!per || scalars.size() % per)
				fail(variable.line, "the initializer does not fill whole elements");
			type.array[0] = uint32_t(scalars.size() / per);
			Ref value = from_scalars(scalars, type, variable.line);
			bind_variable(variable, type, &value);
			return;
		}
		if (type.shape == Type::Shape::object) {
			if (!variable.initializer)
				fail(variable.line, "a local resource needs an initializer");
			const Ref object = eval(*variable.initializer);
			if (object.type.shape != Type::Shape::object || object.type.object != type.object)
				fail(variable.line, "cannot convert from '" + object.type.name() + "' to '" + type.name() + "'");
			scopes.back()[variable.name] = object;
			return;
		}
		if (variable.initializer) {
			const Ref value = initial_value(*variable.initializer, type);
			bind_variable(variable, type, &value);
		} else
			bind_variable(variable, type, nullptr);
	}

	// A constant array placed in the immediate constant buffer so that it can also be indexed dynamically.
	int place_icb(const Ref &value)
	{
		const int base = int(icb.size());
		std::vector<Loc> slots;
		uint32_t next = 0;
		layout(value.type, slots, next, [](uint32_t reg) { return Operand::reg(sm4::immediate_constant_buffer, reg); });
		icb.resize(icb.size() + next);
		for (size_t k = 0; k < slots.size(); ++k)
			icb[size_t(base) + slots[k].base.index[0]][slots[k].component] = value.locs[k].bits;
		return base;
	}

	void bind_variable(const Variable &variable, const Type &type, const Ref *value)
	{
		if (variable.is_const && value && all_immediate(*value)) {
			Ref constant = *value;
			constant.type = type;
			constant.writable = false;
			if (!type.array.empty())
				constant.icb = place_icb(constant);
			scopes.back()[variable.name] = constant;
			return;
		}
		Ref place = storage(type);
		if (value)
			write(place.locs, value->locs);
		place.writable = !variable.is_const;
		scopes.back()[variable.name] = place;
	}

	void statement(const Stmt &stmt)
	{
		switch (stmt.kind) {
		case Stmt::Kind::empty:
			return;
		case Stmt::Kind::block:
			scopes.emplace_back();
			for (const auto &item : stmt.body)
				statement(*item);
			scopes.pop_back();
			return;
		case Stmt::Kind::declaration:
			for (const auto &variable : stmt.variables)
				declare(variable);
			return;
		case Stmt::Kind::expression:
			if (stmt.value)
				eval(*stmt.value);
			return;
		case Stmt::Kind::if_statement: {
			const Loc test = condition(*stmt.value);
			if (test.immediate) {
				if (test.bits)
					scoped(*stmt.then_branch);
				else if (stmt.else_branch)
					scoped(*stmt.else_branch);
				return;
			}
			emit(sm4::op_if, {scalar_source(test)}, test_nonzero);
			scoped(*stmt.then_branch);
			if (stmt.else_branch) {
				emit(sm4::op_else, {});
				scoped(*stmt.else_branch);
			}
			emit(sm4::endif, {});
			return;
		}
		case Stmt::Kind::while_statement:
		case Stmt::Kind::for_statement:
		case Stmt::Kind::do_statement: {
			const auto marked = [&](const char *word) { return std::find(stmt.attributes.begin(), stmt.attributes.end(), word) != stmt.attributes.end(); };
			if (stmt.kind == Stmt::Kind::for_statement && (marked("unroll") || (unroll_all && !marked("loop"))) && unroll(stmt))
				return;
			// A for loop's counter outlives the loop, as in older HLSL.
			if (stmt.kind == Stmt::Kind::for_statement)
				statement(*stmt.body[0]);
			scopes.emplace_back();
			emit(sm4::loop, {});
			Loop loop;
			loop.step = stmt.step.get();
			if (stmt.kind == Stmt::Kind::do_statement)
				loop.until = stmt.value.get();
			loops.push_back(loop);
			if (stmt.kind != Stmt::Kind::do_statement && stmt.value) {
				const Loc test = condition(*stmt.value);
				if (!test.immediate)
					emit(sm4::breakc, {scalar_source(test)}, 0);
				else if (!test.bits)
					emit(sm4::op_break, {});
			}
			scoped(*stmt.then_branch);
			if (stmt.step)
				eval(*stmt.step);
			if (stmt.kind == Stmt::Kind::do_statement) {
				const Loc test = condition(*stmt.value);
				if (!test.immediate)
					emit(sm4::breakc, {scalar_source(test)}, 0);
				else if (!test.bits)
					emit(sm4::op_break, {});
			}
			loops.pop_back();
			emit(sm4::endloop, {});
			scopes.pop_back();
			after_breakable();
			return;
		}
		case Stmt::Kind::switch_statement: {
			const Ref selector = convert(value(*stmt.value), scalar_type(Scalar::integer), stmt.line);
			if (!selector.type.numeric() || selector.locs.size() != 1)
				fail(stmt.line, "switch needs a scalar");
			emit(sm4::op_switch, {scalar_source(readable(selector.locs)[0])});
			Loop loop;
			loop.is_switch = true;
			loops.push_back(loop);
			for (const auto &[label, body] : stmt.cases) {
				if (label)
					emit(sm4::op_case, {Operand::values({uint32_t(*label), 0, 0, 0}, 1)});
				else
					emit(sm4::op_default, {});
				scopes.emplace_back();
				for (const auto &item : body)
					statement(*item);
				scopes.pop_back();
			}
			loops.pop_back();
			emit(sm4::endswitch, {});
			after_breakable();
			return;
		}
		case Stmt::Kind::break_statement:
			if (loops.size() <= own_loops())
				fail(stmt.line, "break outside of a loop or switch");
			emit(sm4::op_break, {});
			return;
		case Stmt::Kind::continue_statement: {
			const Loop *target = nullptr;
			for (size_t at = loops.size(); at > own_loops(); --at)
				if (!loops[at - 1].is_switch) {
					target = &loops[at - 1];
					break;
				}
			if (!target)
				fail(stmt.line, "continue outside of a loop");
			const Loop copy = *target;
			if (copy.step)
				eval(*copy.step);
			if (copy.until) {
				const Loc test = condition(*copy.until);
				if (!test.immediate)
					emit(sm4::breakc, {scalar_source(test)}, 0);
				else if (!test.bits)
					emit(sm4::op_break, {});
			}
			emit(sm4::op_continue, {});
			return;
		}
		case Stmt::Kind::discard_statement:
			if (stage != Stage::pixel)
				fail(stmt.line, "discard is only valid in pixel shaders");
			uses_discard = true;
			emit(sm4::discard, {Operand::values({~0u, ~0u, ~0u, ~0u}, 1)}, test_nonzero);
			return;
		case Stmt::Kind::return_statement: {
			if (stmt.value) {
				if (frames.back().result.type.shape == Type::Shape::void_type)
					fail(stmt.line, "void functions cannot return a value");
				// The value may call functions, whose frames can move this one: it is looked up again afterwards.
				const Ref returned = value(*stmt.value);
				const Ref result = convert(returned, frames.back().result.type, stmt.line);
				write(frames.back().result.locs, result.locs);
			}
			Frame &frame = frames.back();
			if (!stmt.value && frame.result.type.shape != Type::Shape::void_type)
				fail(stmt.line, "the function must return a value");
			if (frame.entry) {
				emit_outputs();
				emit(sm4::ret, {});
			} else if (frame.returned) {
				if (loops.size() > frame.loop_depth + 1)
					write(temp_locs(*frame.returned, 1), {constant_loc(~0u)});
				emit(sm4::op_break, {});
			}
			return;
		}
		}
	}

	// ---- globals ----

	void add_resource(const Variable &variable)
	{
		Resource resource;
		resource.name = variable.name;
		resource.kind = variable.type.object;
		if (variable.type.element)
			resource.element = *variable.type.element;
		resource.register_class = is_sampler(resource.kind) ? 's' : is_uav(resource.kind) ? 'u' : 't';
		if (variable.register_class == resource.register_class)
			resource.explicit_register = variable.register_index;
		resource.count = element_count(variable.type);
		resource.comparison = resource.kind == ObjectKind::sampler_comparison;
		if (is_structured(resource.kind))
			resource.stride = tight_size(resource.element);
		Ref ref;
		ref.type = variable.type;
		ref.resource = int(resources.size());
		resources.push_back(resource);
		globals[variable.name] = ref;
	}

	void add_shared(const Variable &variable)
	{
		if (stage != Stage::compute)
			fail(variable.line, "groupshared is only valid in compute shaders");
		const Type element = variable.type.array.empty() ? variable.type : element_of(variable.type);
		Shared item;
		item.name = variable.name;
		item.stride = tight_size(element);
		item.count = variable.type.array.empty() ? 1 : variable.type.array[0];
		Ref ref;
		ref.type = variable.type;
		ref.access = std::make_shared<Access>();
		ref.access->kind = Access::Kind::shared;
		ref.access->target = int(shared.size());
		ref.access->outer_array = !variable.type.array.empty();
		ref.access->address = {constant_loc(0)};
		uint32_t size = 0;
		tight_layout(element, size, ref.access->offsets);
		shared.push_back(item);
		globals[variable.name] = ref;
	}

	void collect_globals(const Function &entry)
	{
		Buffer globals_buffer;
		globals_buffer.name = "$Globals";
		buffers.push_back(globals_buffer);
		std::vector<std::pair<size_t, const Variable *>> members;
		for (const auto &variable : program.globals) {
			if (variable.type.shape == Type::Shape::object)
				add_resource(variable);
			else if (variable.is_groupshared)
				add_shared(variable);
			else if (!variable.is_static)
				members.emplace_back(0, &variable);
		}
		for (const auto &declared : program.buffers) {
			Buffer buffer;
			buffer.name = declared.name;
			buffer.texture = declared.texture_buffer;
			if (buffer.texture) {
				Resource view;
				view.name = declared.name;
				view.kind = ObjectKind::buffer;
				view.element = Type::make_vector(Scalar::floating, 4);
				view.explicit_register = declared.register_index;
				view.texture_buffer = true;
				buffer.resource = int(resources.size());
				resources.push_back(view);
			} else
				buffer.explicit_register = declared.register_index;
			buffers.push_back(buffer);
			for (const auto &variable : declared.members) {
				if (variable.type.shape == Type::Shape::object)
					add_resource(variable);
				else
					members.emplace_back(buffers.size() - 1, &variable);
			}
		}
		for (const auto &parameter : entry.parameters) {
			if (!parameter.is_uniform)
				continue;
			if (parameter_uniforms.empty()) {
				Buffer params;
				params.name = "$Params";
				buffers.push_back(params);
			}
			Variable &variable = parameter_uniforms.emplace_back();
			variable.name = parameter.name;
			variable.type = parameter.type;
			variable.line = entry.line;
			if (variable.type.shape == Type::Shape::object)
				fail(entry.line, "a uniform entry point parameter cannot be an object");
			members.emplace_back(buffers.size() - 1, &variable);
		}
		std::vector<uint32_t> ends(buffers.size());
		for (const auto &[index, variable] : members) {
			auto &buffer = buffers[index];
			const Type &type = variable->type;
			if (!type.array.empty() && type.array[0] == 0)
				fail(variable->line, "a uniform array needs a size");
			const uint32_t offset = variable->pack_offset >= 0 ? uint32_t(variable->pack_offset) * 4 : cb_place(type, ends[index]);
			BufferVariable entry;
			entry.name = variable->name;
			entry.type = type;
			entry.offset = offset;
			entry.size = cb_size(type);
			ends[index] = std::max(ends[index], offset + entry.size);
			Ref ref;
			ref.type = type;
			cb_layout(type, offset, uint32_t(index), ref.locs);
			globals[variable->name] = ref;
			buffer_names[variable->name] = {int(index), int(buffer.variables.size())};
			buffer.variables.push_back(entry);
		}
		for (size_t i = 0; i < buffers.size(); ++i)
			buffers[i].size = align16(ends[i]);
	}

	// Static globals take their values at the start of the entry point.
	void initialize_statics()
	{
		scopes = {{}};
		for (const auto &variable : program.globals)
			if (variable.is_static && !variable.is_groupshared && variable.type.shape != Type::Shape::object)
				declare(variable);
		for (auto &[name, ref] : scopes[0])
			globals[name] = ref;
		scopes.clear();
	}

	// ---- stage inputs and outputs ----

	// The interpolation mode of a pixel shader input (D3D10_SB_INTERPOLATION_MODE).
	static uint32_t interpolation(const Type &type, const std::vector<std::string> &modifiers, uint32_t system_value)
	{
		const auto has = [&](const char *word) { return std::find(modifiers.begin(), modifiers.end(), word) != modifiers.end(); };
		if (system_value == 1)
			return 4;
		if ((system_value >= 6 && system_value <= 10) || !is_float(type.scalar) || has("nointerpolation"))
			return 1;
		const bool perspective = !has("noperspective");
		if (has("centroid"))
			return perspective ? 3 : 5;
		if (has("sample"))
			return perspective ? 6 : 7;
		return perspective ? 2 : 4;
	}

	static std::pair<std::string, uint32_t> split_semantic(const std::string &semantic)
	{
		size_t end = semantic.size();
		while (end > 0 && std::isdigit(uint8_t(semantic[end - 1])))
			--end;
		return {semantic.substr(0, end), end < semantic.size() ? uint32_t(std::stoul(semantic.substr(end))) : 0};
	}

	static std::string upper(std::string text)
	{
		for (auto &letter : text)
			letter = char(std::toupper(uint8_t(letter)));
		return text;
	}

	// D3D_NAME of a system value semantic; zero for an ordinary one.
	static uint32_t system_value(const std::string &key, uint32_t line)
	{
		static const std::map<std::string, uint32_t> names{{"SV_POSITION", 1}, {"SV_CLIPDISTANCE", 2}, {"SV_CULLDISTANCE", 3},
			{"SV_RENDERTARGETARRAYINDEX", 4}, {"SV_VIEWPORTARRAYINDEX", 5}, {"SV_VERTEXID", 6}, {"SV_PRIMITIVEID", 7}, {"SV_INSTANCEID", 8},
			{"SV_ISFRONTFACE", 9}, {"SV_SAMPLEINDEX", 10}, {"SV_TARGET", 64}, {"SV_DEPTH", 65}, {"SV_COVERAGE", 66},
			{"SV_DEPTHGREATEREQUAL", 67}, {"SV_DEPTHLESSEQUAL", 68}};
		if (!key.starts_with("SV_"))
			return 0;
		const auto found = names.find(key);
		if (found == names.end())
			fail(line, "unknown system value semantic '" + key + "'");
		return found->second;
	}

	static uint32_t component_type(Scalar kind)
	{
		return is_float(kind) ? 3 : kind == Scalar::integer ? 2 : 1;
	}

	std::vector<uint32_t> input_modes;
	// Registers that compute and geometry shaders read without a signature: their operand type and the components read.
	std::map<uint8_t, uint8_t> special_inputs;
	uint32_t next_input = 0, next_output = 0;
	// Where an appended geometry shader vertex goes, scalar by scalar.
	std::vector<Loc> stream_layout;
	uint32_t vertices = 0, input_primitive = 0, output_topology = 0;
	// Tessellation: the control points in and out of a patch, the domain (1 isoline, 2 triangle, 3 quad), the hull shader's
	// partitioning, output primitive and largest factor, and the patch constant registers handed out so far.
	uint32_t control_points = 0, output_control_points = 0, tess_domain = 0, partitioning = 0, output_primitive = 0, next_patch = 0;
	float max_tess_factor = 64.0f;
	// A hull shader is generated as two phases: the control point function, kept here once done, then the patch constant function.
	bool patch_phase = false;
	std::vector<Instruction> control_code;
	std::map<uint8_t, uint8_t> control_specials;
	// A domain shader's inputs are patch constants, except the control points of its OutputPatch.
	bool control_point_input = false;

	Loc register_loc(uint8_t type, uint32_t reg, uint32_t component)
	{
		Loc loc;
		loc.base = Operand::reg(type, reg);
		if (type == sm4::input && stage == Stage::geometry) {
			loc.base.dimension = 2;
			loc.base.index[1] = reg;
			loc.base.index[0] = 0;
		}
		loc.component = uint8_t(component);
		return loc;
	}

	Loc special_loc(uint8_t type, uint32_t component)
	{
		Loc loc;
		loc.base = Operand::none(type);
		loc.base.components = 4;
		loc.component = uint8_t(component);
		return loc;
	}

	void input_value(const Type &type, const std::string &semantic, const std::vector<std::string> &modifiers, uint32_t line, std::vector<Loc> &locs)
	{
		if (type.shape == Type::Shape::structure && type.array.empty()) {
			for (const auto &member : type.structure->members) {
				auto combined = modifiers;
				combined.insert(combined.end(), member.modifiers.begin(), member.modifiers.end());
				input_value(member.type, member.semantic, combined, line, locs);
			}
			return;
		}
		if (semantic.empty())
			fail(line, "missing semantic on a stage input");
		const auto [name, index] = split_semantic(semantic);
		if (!type.array.empty()) {
			const Type element = element_of(type);
			for (uint32_t i = 0; i < type.array[0]; ++i)
				input_value(element, name + std::to_string(index + i * registers_of(element)), modifiers, line, locs);
			return;
		}
		if (!type.numeric())
			fail(line, "stage inputs must be numeric");
		const std::string key = upper(name);
		static const std::map<std::string, uint8_t> compute{{"SV_DISPATCHTHREADID", sm4::thread_id}, {"SV_GROUPID", sm4::thread_group_id},
			{"SV_GROUPTHREADID", sm4::thread_id_in_group}, {"SV_GROUPINDEX", sm4::thread_id_in_group_flattened}};
		uint8_t special = 0;
		if (stage == Stage::compute) {
			const auto found = compute.find(key);
			if (found == compute.end())
				fail(line, "invalid compute shader input semantic '" + semantic + "'");
			special = found->second;
		} else if ((stage == Stage::geometry || stage == Stage::hull || stage == Stage::domain) && key == "SV_PRIMITIVEID")
			special = sm4::input_primitive_id;
		else if (stage == Stage::hull && !patch_phase && key == "SV_OUTPUTCONTROLPOINTID")
			special = sm4::output_control_point_id;
		else if (stage == Stage::domain && key == "SV_DOMAINLOCATION") {
			if (type.components() != (tess_domain == 2 ? 3u : 2u))
				fail(line, "'SV_DomainLocation' has the wrong size for the domain");
			special = sm4::input_domain_point;
		}
		else if (stage == Stage::geometry && key == "SV_GSINSTANCEID")
			special = sm4::gs_instance_id;
		else if (stage == Stage::pixel && key == "SV_COVERAGE")
			special = sm4::input_coverage;
		if (special) {
			const uint32_t width = scalar_register(special) ? 1 : type.components();
			for (uint32_t c = 0; c < type.components(); ++c)
				locs.push_back(special_loc(special, std::min(c, width - 1)));
			special_inputs[special] |= uint8_t((1 << width) - 1);
			return;
		}
		if (stage == Stage::domain && !control_point_input) {
			patch_element(name, index, type, line, false, [&](const Loc &loc) { locs.push_back(loc); });
			return;
		}
		const uint32_t sv = system_value(key, line);
		if (sv >= 64 || (sv == 6 || sv == 8 ? stage != Stage::vertex : false))
			fail(line, "invalid input semantic '" + semantic + "'");
		for (uint32_t r = 0; r < row_count(type); ++r) {
			SignatureElement element;
			element.semantic = name;
			element.semantic_index = index + r;
			element.system_value = sv;
			element.component_type = component_type(type.scalar);
			element.reg = next_input++;
			element.mask = uint8_t((1 << row_width(type)) - 1);
			out.inputs.push_back(element);
			input_modes.push_back(interpolation(type, modifiers, sv));
			for (uint32_t c = 0; c < row_width(type); ++c)
				locs.push_back(register_loc(sm4::input, element.reg, c));
		}
	}

	// Binds stage outputs to the scalars of `sources` (copied when the entry point returns), or records where a stream vertex goes.
	void output_value(const Type &type, const std::string &semantic, uint32_t line, const std::vector<Loc> &sources, size_t &at, uint32_t stream)
	{
		if (type.shape == Type::Shape::structure && type.array.empty()) {
			for (const auto &member : type.structure->members)
				output_value(member.type, member.semantic, line, sources, at, stream);
			return;
		}
		if (semantic.empty())
			fail(line, "missing semantic on a stage output");
		auto [name, index] = split_semantic(semantic);
		if (!type.array.empty()) {
			const Type element = element_of(type);
			for (uint32_t i = 0; i < type.array[0]; ++i)
				output_value(element, name + std::to_string(index + i * registers_of(element)), line, sources, at, stream);
			return;
		}
		if (!type.numeric())
			fail(line, "stage outputs must be numeric");
		std::string key = upper(name);
		if (stage == Stage::pixel && key == "COLOR")
			key = "SV_TARGET";
		if (stage == Stage::pixel && key == "DEPTH")
			key = "SV_DEPTH";
		if (stage == Stage::vertex && key == "POSITION" && (flags & compile_backwards_compatibility))
			key = "SV_POSITION";
		if (key.starts_with("SV_") && key != name && upper(name) != key)
			name = key == "SV_TARGET" ? "SV_Target" : key == "SV_DEPTH" ? "SV_Depth" : "SV_Position";
		const auto bind = [&](const Loc &dest) {
			if (stream != ~0u)
				stream_layout.push_back(dest);
			else
				output_copies.emplace_back(dest, sources[at]);
			++at;
		};
		if (patch_phase) {
			patch_element(name, index, type, line, true, bind);
			return;
		}
		const uint32_t sv = system_value(key, line);
		if (stage == Stage::pixel) {
			if (sv == 64) {
				for (uint32_t r = 0; r < row_count(type); ++r) {
					SignatureElement element;
					element.semantic = name;
					element.semantic_index = index + r;
					element.system_value = 64;
					element.component_type = component_type(type.scalar);
					element.reg = index + r;
					element.mask = uint8_t((1 << row_width(type)) - 1);
					out.outputs.push_back(element);
					for (uint32_t c = 0; c < row_width(type); ++c)
						bind(register_loc(sm4::output, element.reg, c));
				}
				return;
			}
			if (sv == 65 || sv == 66 || sv == 67 || sv == 68) {
				if (type.components() != 1)
					fail(line, "'" + semantic + "' must be a scalar");
				if (sv != 65 && !sm5 && !(sv == 66 && out.minor >= 1))
					fail(line, "'" + semantic + "' needs a newer shader model");
				SignatureElement element;
				element.semantic = name;
				element.system_value = sv;
				element.component_type = sv == 66 ? 1 : 3;
				element.reg = ~0u;
				element.mask = 1;
				out.outputs.push_back(element);
				const uint8_t types[] = {sm4::output_depth, sm4::output_coverage, sm4::output_depth_greater_equal, sm4::output_depth_less_equal};
				Loc loc;
				loc.base = Operand::none(types[sv - 65]);
				loc.base.components = 1;
				bind(loc);
				return;
			}
			fail(line, "invalid pixel shader output semantic '" + semantic + "'");
		}
		if (sv >= 6 && sv != 7)
			fail(line, "invalid output semantic '" + semantic + "'");
		for (uint32_t r = 0; r < row_count(type); ++r) {
			SignatureElement element;
			element.semantic = name;
			element.semantic_index = index + r;
			element.system_value = sv;
			element.component_type = component_type(type.scalar);
			element.reg = next_output++;
			element.mask = uint8_t((1 << row_width(type)) - 1);
			element.stream = stream == ~0u ? 0 : stream;
			out.outputs.push_back(element);
			for (uint32_t c = 0; c < row_width(type); ++c)
				bind(register_loc(sm4::output, element.reg, c));
		}
	}

	// The D3D_NAME of a tessellation factor, or 0: an edge or inside factor of the domain, which has a fixed number of each.
	uint32_t tess_factor(const std::string &key, uint32_t index, uint32_t line) const
	{
		const bool edge = key == "SV_TESSFACTOR", inside = key == "SV_INSIDETESSFACTOR";
		if (!edge && !inside)
			return 0;
		static const uint32_t edges[] = {0, 2, 3, 4}, insides[] = {0, 0, 1, 2};
		if (!tess_domain || index >= (edge ? edges : insides)[tess_domain])
			fail(line, "the domain has no tessellation factor '" + key + std::to_string(index) + "'");
		if (tess_domain == 1)
			return index == 0 ? 16 : 15;
		return (tess_domain == 3 ? 11 : 13) + (inside ? 1 : 0);
	}

	// The name a declaration gives a tessellation factor register (D3D11_SB_NAME), from its D3D_NAME and index.
	static uint32_t tess_factor_name(uint32_t system_value, uint32_t index)
	{
		switch (system_value) {
		case 11: return 11 + index;
		case 12: return 15 + index;
		case 13: return 17 + index;
		case 14: return 20;
		case 15: return 21;
		default: return 22;
		}
	}

	// One patch constant: written by a hull shader's patch constant function, read by a domain shader, each row in a register.
	void patch_element(const std::string &name, uint32_t index, const Type &type, uint32_t line, bool output, const std::function<void(const Loc &)> &bind)
	{
		const std::string key = upper(name);
		const uint32_t sv = tess_factor(key, index, line);
		if (sv && (type.shape != Type::Shape::scalar || type.scalar != Scalar::floating))
			fail(line, "tessellation factors are floats");
		if (!sv && key.starts_with("SV_"))
			fail(line, "invalid patch constant semantic '" + name + "'");
		for (uint32_t r = 0; r < row_count(type); ++r) {
			SignatureElement element;
			element.semantic = name;
			element.semantic_index = index + r;
			element.system_value = sv;
			element.component_type = component_type(type.scalar);
			element.reg = next_patch++;
			element.mask = uint8_t((1 << row_width(type)) - 1);
			out.patch_constants.push_back(element);
			for (uint32_t c = 0; c < row_width(type); ++c)
				bind(register_loc(output ? sm4::output : sm4::input_patch_constant, element.reg, c));
		}
	}

	// The registers of a control point as the signature already lays them out, from register 0 in member order.
	void patch_layout(const Type &type, std::vector<Loc> &locs, uint32_t &next)
	{
		if (type.shape == Type::Shape::structure && type.array.empty()) {
			for (const auto &member : type.structure->members)
				patch_layout(member.type, locs, next);
			return;
		}
		if (!type.array.empty()) {
			for (uint32_t i = 0; i < type.array[0]; ++i)
				patch_layout(element_of(type), locs, next);
			return;
		}
		for (uint32_t r = 0; r < row_count(type); ++r) {
			const uint32_t reg = next++;
			for (uint32_t c = 0; c < row_width(type); ++c)
				locs.push_back(register_loc(sm4::input, reg, c));
		}
	}

	void emit_outputs()
	{
		std::vector<Loc> dest, values;
		for (const auto &[target, source] : output_copies) {
			if (target.base.type == sm4::output) {
				dest.push_back(target);
				values.push_back(source);
			} else
				emit(sm4::mov, {target.base, scalar_source(source)});
		}
		write(dest, values);
	}

	void emit_stream_vertex(const Ref &vertex, uint32_t line)
	{
		if (vertex.locs.size() != stream_layout.size())
			fail(line, "the appended vertex does not match the stream");
		write(stream_layout, vertex.locs);
	}

	static const std::vector<std::string> *attribute(const Function &function, const std::string &name)
	{
		for (const auto &[key, arguments] : function.attributes)
			if (upper(key) == upper(name))
				return &arguments;
		return nullptr;
	}

	// The tessellator settings of a hull or domain shader's attributes; a domain shader names only its domain.
	void tessellation(const Function &function)
	{
		const auto word = [&](const char *name, std::initializer_list<std::pair<const char *, uint32_t>> choices) -> uint32_t {
			const auto *arguments = attribute(function, name);
			if (!arguments || arguments->size() != 1)
				fail(function.line, std::string("the entry point needs [") + name + "(...)]");
			for (const auto &[text, value] : choices)
				if ((*arguments)[0] == text)
					return value;
			fail(function.line, std::string("invalid [") + name + "] value '" + (*arguments)[0] + "'");
		};
		tess_domain = word("domain", {{"isoline", 1}, {"tri", 2}, {"quad", 3}});
		if (stage == Stage::domain)
			return;
		partitioning = word("partitioning", {{"integer", 1}, {"pow2", 2}, {"fractional_odd", 3}, {"fractional_even", 4}});
		output_primitive = word("outputtopology", {{"point", 1}, {"line", 2}, {"triangle_cw", 3}, {"triangle_ccw", 4}});
		if (tess_domain == 1 && output_primitive > 2)
			fail(function.line, "an isoline domain puts out points or lines");
		const auto *points = attribute(function, "outputcontrolpoints");
		if (!points || points->size() != 1)
			fail(function.line, "a hull shader needs [outputcontrolpoints(n)]");
		output_control_points = uint32_t(std::stoul((*points)[0]));
		if (output_control_points > 32)
			fail(function.line, "a hull shader puts out at most 32 control points");
		if (const auto *largest = attribute(function, "maxtessfactor"); largest && largest->size() == 1)
			max_tess_factor = std::stof((*largest)[0]);
		if (max_tess_factor < 1.0f || max_tess_factor > 64.0f)
			fail(function.line, "[maxtessfactor] lies between 1 and 64");
	}

	void entry_point(const Function &function)
	{
		Frame frame;
		frame.function = &function;
		frame.entry = true;
		std::map<std::string, Ref> scope;
		std::set<std::string> assigned;
		assigned_names(*function.body, assigned);
		uint32_t streams = 0;
		for (const auto &parameter : function.parameters) {
			const auto has = [&](const char *word) { return std::find(parameter.modifiers.begin(), parameter.modifiers.end(), word) != parameter.modifiers.end(); };
			// Found through the globals, in $Params.
			if (parameter.is_uniform)
				continue;
			if (has("TriangleStream") || has("LineStream") || has("PointStream")) {
				if (stage != Stage::geometry)
					fail(function.line, "streams are only valid in geometry shaders");
				if (streams && !sm5)
					fail(function.line, "several streams need shader model 5");
				output_topology = has("TriangleStream") ? 5 : has("LineStream") ? 3 : 1;
				Ref stream;
				stream.type = parameter.type;
				stream.stream = int(streams);
				size_t at = 0;
				output_value(parameter.type, parameter.semantic, function.line, {}, at, streams);
				++streams;
				scope[parameter.name] = stream;
				continue;
			}
			if (has("InputPatch") || has("OutputPatch")) {
				if (stage != Stage::hull && stage != Stage::domain)
					fail(function.line, "patches are only valid in hull and domain shaders");
				const bool output_patch = has("OutputPatch");
				if (parameter.type.array.empty() || !parameter.type.array[0] || parameter.type.array[0] > 32)
					fail(function.line, "a patch holds 1 to 32 control points");
				const uint32_t count = parameter.type.array[0];
				const Type element = element_of(parameter.type);
				std::vector<Loc> first;
				uint8_t type = sm4::input_control_point;
				if (stage == Stage::domain) {
					if (!output_patch)
						fail(function.line, "a domain shader reads its control points from an OutputPatch");
					control_points = count;
					control_point_input = true;
					input_value(element, parameter.semantic, parameter.modifiers, function.line, first);
					control_point_input = false;
				} else if (!patch_phase) {
					if (output_patch)
						fail(function.line, "the control point function reads its control points from an InputPatch");
					control_points = count;
					input_value(element, parameter.semantic, parameter.modifiers, function.line, first);
					type = sm4::input;
				} else {
					if (count != (output_patch ? output_control_points : control_points))
						fail(function.line, "the patch does not hold as many control points as the hull shader");
					uint32_t next = 0;
					patch_layout(element, first, next);
					if (output_patch)
						type = sm4::output_control_point;
				}
				// Every control point's registers: v[point][register], vicp or vocp.
				Ref input;
				input.type = parameter.type;
				for (uint32_t v = 0; v < count; ++v)
					for (const auto &loc : first) {
						Loc point = loc;
						point.base = Operand::reg(type, 0);
						point.base.dimension = 2;
						point.base.index[0] = v;
						point.base.index[1] = loc.base.index[loc.base.dimension - 1];
						input.locs.push_back(point);
					}
				scope[parameter.name] = input;
				continue;
			}
			if (stage == Stage::geometry && parameter.is_in && !parameter.is_out && !parameter.type.array.empty()) {
				static const std::pair<const char *, std::pair<uint32_t, uint32_t>> primitives[] = {{"point", {1, 1}}, {"line", {2, 2}},
					{"triangle", {3, 3}}, {"lineadj", {4, 6}}, {"triangleadj", {6, 7}}};
				for (const auto &[word, shape] : primitives)
					if (has(word)) {
						vertices = shape.first;
						input_primitive = shape.second;
					}
				if (!vertices || parameter.type.array[0] != vertices)
					fail(function.line, "the input array does not match the primitive");
				std::vector<Loc> first;
				input_value(element_of(parameter.type), parameter.semantic, parameter.modifiers, function.line, first);
				Ref input;
				input.type = parameter.type;
				for (uint32_t v = 0; v < vertices; ++v)
					for (auto loc : first) {
						if (loc.base.type == sm4::input)
							loc.base.index[0] = v;
						input.locs.push_back(loc);
					}
				scope[parameter.name] = input;
				continue;
			}
			if (parameter.is_out) {
				Ref place = storage(parameter.type);
				if (parameter.is_in) {
					std::vector<Loc> locs;
					input_value(parameter.type, parameter.semantic, parameter.modifiers, function.line, locs);
					write(place.locs, locs);
				}
				size_t at = 0;
				output_value(parameter.type, parameter.semantic, function.line, place.locs, at, ~0u);
				scope[parameter.name] = place;
				continue;
			}
			Ref input;
			input.type = parameter.type;
			input_value(parameter.type, parameter.semantic, parameter.modifiers, function.line, input.locs);
			if (assigned.count(parameter.name)) {
				Ref place = storage(parameter.type);
				write(place.locs, input.locs);
				input = place;
			}
			scope[parameter.name] = input;
		}
		if (function.result.shape != Type::Shape::void_type) {
			if (stage == Stage::compute || stage == Stage::geometry)
				fail(function.line, "this entry point must return void");
			frame.result = storage(function.result);
			size_t at = 0;
			output_value(function.result, function.semantic, function.line, frame.result.locs, at, ~0u);
		}
		if (stage == Stage::geometry && (!vertices || !streams))
			fail(function.line, "a geometry shader needs an input primitive and an output stream");
		frames.push_back(frame);
		scopes = {scope};
		statement(*function.body);
		const auto &items = function.body->body;
		if (items.empty() || items.back()->kind != Stmt::Kind::return_statement) {
			emit_outputs();
			emit(sm4::ret, {});
		}
		frames.pop_back();
		scopes.clear();
	}

	// ---- binding, registers and declarations ----

	// Gives every used resource of a register class a slot: explicit registers first, the rest in declaration order.
	template<typename List>
	static void bind(List &list, const std::function<bool(const typename List::value_type &)> &member)
	{
		std::set<uint32_t> taken;
		for (auto &item : list)
			if (member(item) && item.used && item.explicit_register >= 0) {
				item.bound = item.explicit_register;
				for (uint32_t k = 0; k < count_of(item); ++k)
					taken.insert(uint32_t(item.bound) + k);
			}
		for (auto &item : list) {
			if (!member(item) || !item.used || item.bound >= 0)
				continue;
			uint32_t slot = 0;
			for (;; ++slot) {
				bool free = true;
				for (uint32_t k = 0; k < count_of(item) && free; ++k)
					free = !taken.count(slot + k);
				if (free)
					break;
			}
			item.bound = int(slot);
			for (uint32_t k = 0; k < count_of(item); ++k)
				taken.insert(slot + k);
		}
	}

	static uint32_t count_of(const Resource &resource) { return resource.count; }
	static uint32_t count_of(const Buffer &) { return 1; }

	void bind_all()
	{
		for (const char letter : {'t', 's', 'u'})
			bind(resources, [letter](const Resource &resource) { return resource.register_class == letter; });
		bind(buffers, [](const Buffer &buffer) { return !buffer.texture; });
		for (auto &buffer : buffers)
			if (buffer.texture && buffer.used)
				buffer.bound = resources[size_t(buffer.resource)].bound;
		const auto remap = [&](Operand &operand) {
			if (operand.type == sm4::resource || operand.type == sm4::sampler || operand.type == sm4::uav) {
				const auto &resource = resources[operand.index[0] & 0xffff];
				operand.index[0] = uint32_t(resource.bound) + (operand.index[0] >> 16);
			} else if (operand.type == sm4::constant_buffer)
				operand.index[0] = uint32_t(buffers[operand.index[0]].bound);
		};
		for (auto *list : {&code, &control_code})
			for (auto &instruction : *list)
				for (auto &operand : instruction.operands)
					remap(operand);
	}

	// Temp registers renumbered so that registers whose lifetimes do not overlap share one; a register used inside a loop lives
	// through the whole loop, since the loop may run again.
	uint32_t compact_temps()
	{
		const uint32_t total = temps;
		std::vector<int64_t> first(total, -1), last(total, -1);
		const auto touch = [&](uint32_t reg, int64_t at) {
			if (first[reg] < 0)
				first[reg] = at;
			last[reg] = at;
		};
		std::vector<std::pair<int64_t, int64_t>> loop_ranges;
		std::vector<int64_t> open;
		for (size_t i = 0; i < code.size(); ++i) {
			for (const auto &operand : code[i].operands) {
				if (operand.type == sm4::temp)
					touch(operand.index[0], int64_t(i));
				for (const auto &relative : operand.relative)
					if (relative && relative->type == sm4::temp)
						touch(relative->index, int64_t(i));
			}
			if (code[i].opcode == sm4::loop)
				open.push_back(int64_t(i));
			else if (code[i].opcode == sm4::endloop) {
				loop_ranges.emplace_back(open.back(), int64_t(i));
				open.pop_back();
			}
		}
		for (bool changed = true; changed;) {
			changed = false;
			for (uint32_t reg = 0; reg < total; ++reg) {
				if (first[reg] < 0)
					continue;
				for (const auto &[start, end] : loop_ranges)
					if (first[reg] <= end && last[reg] >= start && (first[reg] > start || last[reg] < end)) {
						first[reg] = std::min(first[reg], start);
						last[reg] = std::max(last[reg], end);
						changed = true;
					}
			}
		}
		std::vector<uint32_t> order;
		for (uint32_t reg = 0; reg < total; ++reg)
			if (first[reg] >= 0)
				order.push_back(reg);
		std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return first[a] < first[b]; });
		std::vector<uint32_t> mapping(total, 0);
		std::vector<int64_t> busy_until;
		for (const auto reg : order) {
			uint32_t slot = 0;
			while (slot < busy_until.size() && busy_until[slot] >= first[reg])
				++slot;
			if (slot == busy_until.size())
				busy_until.push_back(0);
			busy_until[slot] = last[reg];
			mapping[reg] = slot;
		}
		for (auto &instruction : code)
			for (auto &operand : instruction.operands) {
				if (operand.type == sm4::temp)
					operand.index[0] = mapping[operand.index[0]];
				for (auto &relative : operand.relative)
					if (relative && relative->type == sm4::temp)
						relative->index = mapping[relative->index];
			}
		return uint32_t(busy_until.size());
	}

	// The components of each register of an input class (v, vicp, vocp, vpc) the program reads.
	std::map<uint32_t, uint8_t> reads_of(uint8_t type) const
	{
		std::map<uint32_t, uint8_t> reads;
		for (const auto &instruction : code)
			for (size_t i = 0; i < instruction.operands.size(); ++i) {
				const auto &operand = instruction.operands[i];
				if (operand.type != type)
					continue;
				const uint32_t reg = operand.index[operand.dimension - 1];
				uint8_t mask = 0;
				if (operand.mode == Operand::Mode::select)
					mask = uint8_t(1 << operand.swizzle[0]);
				else if (operand.mode == Operand::Mode::swizzle)
					for (auto component : operand.swizzle)
						mask |= uint8_t(1 << component);
				else
					mask = operand.mask;
				reads[reg] |= mask;
			}
		return reads;
	}

	static Operand declared(uint8_t type, uint32_t number)
	{
		Operand operand = Operand::reg(type, number);
		operand.components = 0;
		return operand;
	}

	static Instruction declaration(uint16_t opcode, std::vector<Operand> operands, uint32_t controls = 0, std::vector<uint32_t> extra = {})
	{
		Instruction instruction;
		instruction.opcode = opcode;
		instruction.operands = std::move(operands);
		instruction.controls = controls;
		instruction.extra = std::move(extra);
		return instruction;
	}

	// A control point register as a declaration names it: vicp[points][register] or vocp[points][register].
	static Operand control_point(uint8_t type, uint32_t points, uint32_t reg, uint8_t mask)
	{
		Operand operand = Operand::reg(type, 0);
		operand.dimension = 2;
		operand.index[0] = points;
		operand.index[1] = reg;
		operand.mask = mask;
		return operand;
	}

	// The declarations; a hull shader's also hold both phases with their code.
	std::vector<Instruction> declarations(uint32_t temp_count, uint32_t control_temps, const Function &function)
	{
		std::vector<Instruction> list;
		if (stage == Stage::hull) {
			list.push_back(declaration(sm4::hs_decls, {}));
			list.push_back(declaration(sm4::dcl_input_control_point_count, {}, control_points));
			list.push_back(declaration(sm4::dcl_output_control_point_count, {}, output_control_points));
			list.push_back(declaration(sm4::dcl_tess_domain, {}, tess_domain));
			list.push_back(declaration(sm4::dcl_tess_partitioning, {}, partitioning));
			list.push_back(declaration(sm4::dcl_tess_output_primitive, {}, output_primitive));
			list.push_back(declaration(sm4::dcl_hs_max_tessfactor, {}, 0, {std::bit_cast<uint32_t>(max_tess_factor)}));
		} else if (stage == Stage::domain) {
			list.push_back(declaration(sm4::dcl_input_control_point_count, {}, control_points));
			list.push_back(declaration(sm4::dcl_tess_domain, {}, tess_domain));
		}
		uint32_t global_flags = flags & compile_skip_optimization ? 0 : 1;
		if (!sm5 && stage == Stage::compute)
			global_flags |= 2;
		if (global_flags)
			list.push_back(declaration(sm4::dcl_global_flags, {}, global_flags));
		if (!icb.empty()) {
			Instruction data = declaration(sm4::customdata, {}, 3);
			for (const auto &row : icb)
				data.extra.insert(data.extra.end(), row.begin(), row.end());
			list.push_back(data);
		}
		std::vector<const Buffer *> used_buffers;
		for (const auto &buffer : buffers)
			if (buffer.used && !buffer.texture)
				used_buffers.push_back(&buffer);
		std::sort(used_buffers.begin(), used_buffers.end(), [](const Buffer *a, const Buffer *b) { return a->bound < b->bound; });
		for (const auto *buffer : used_buffers) {
			Operand operand = Operand::reg(sm4::constant_buffer, uint32_t(buffer->bound));
			operand.mode = Operand::Mode::swizzle;
			operand.dimension = 2;
			operand.index[1] = std::max<uint32_t>(1, buffer->size / 16);
			list.push_back(declaration(sm4::dcl_constant_buffer, {operand}, buffer->dynamic ? 1 : 0));
		}
		std::vector<const Resource *> used;
		for (const auto &resource : resources)
			if (resource.used)
				used.push_back(&resource);
		std::sort(used.begin(), used.end(), [](const Resource *a, const Resource *b) { return a->bound < b->bound; });
		for (const auto *resource : used)
			if (resource->register_class == 's')
				for (uint32_t k = 0; k < resource->count; ++k)
					list.push_back(declaration(sm4::dcl_sampler, {declared(sm4::sampler, uint32_t(resource->bound) + k)}, resource->comparison ? 1 : 0));
		for (const char letter : {'t', 'u'})
			for (const auto *resource : used) {
				if (resource->register_class != letter)
					continue;
				const uint8_t type = letter == 't' ? sm4::resource : sm4::uav;
				for (uint32_t k = 0; k < resource->count; ++k) {
					const Operand operand = declared(type, uint32_t(resource->bound) + k);
					const uint32_t rt = return_type(resource->element);
					if (is_raw(resource->kind))
						list.push_back(declaration(letter == 't' ? sm4::dcl_resource_raw : sm4::dcl_uav_raw, {operand}));
					else if (is_structured(resource->kind))
						list.push_back(declaration(letter == 't' ? sm4::dcl_resource_structured : sm4::dcl_uav_structured, {operand},
							resource->counter ? 1u << 12 : 0, {resource->stride}));
					else
						list.push_back(declaration(letter == 't' ? sm4::dcl_resource : sm4::dcl_uav_typed, {operand},
							resource_dimension(resource->kind) | resource->samples << 5, {rt | rt << 4 | rt << 8 | rt << 12}));
				}
			}
		if (stage == Stage::geometry) {
			list.push_back(declaration(sm4::dcl_gs_input_primitive, {}, input_primitive));
			if (sm5)
				list.push_back(declaration(sm4::dcl_stream, {declared(sm4::stream, 0)}));
			list.push_back(declaration(sm4::dcl_gs_output_topology, {}, output_topology));
			const auto *count = attribute(function, "maxvertexcount");
			if (!count || count->empty())
				fail(function.line, "a geometry shader needs [maxvertexcount(n)]");
			list.push_back(declaration(sm4::dcl_max_output_vertex_count, {}, 0, {uint32_t(std::stoul((*count)[0]))}));
			if (const auto *instances = attribute(function, "instance"); instances && !instances->empty())
				list.push_back(declaration(sm4::dcl_gs_instance_count, {}, 0, {uint32_t(std::stoul((*instances)[0]))}));
		}
		if (stage != Stage::hull) {
			io_declarations(list, temp_count, function);
			return list;
		}
		// The control point phase, then the patch constant function as a single fork phase.
		std::swap(code, control_code);
		std::swap(special_inputs, control_specials);
		patch_phase = false;
		list.push_back(declaration(sm4::hs_control_point_phase, {}));
		io_declarations(list, control_temps, function);
		list.insert(list.end(), code.begin(), code.end());
		std::swap(code, control_code);
		std::swap(special_inputs, control_specials);
		patch_phase = true;
		list.push_back(declaration(sm4::hs_fork_phase, {}));
		io_declarations(list, temp_count, function);
		list.insert(list.end(), code.begin(), code.end());
		return list;
	}

	// What a phase declares: the inputs it reads, its outputs and its registers.
	void io_declarations(std::vector<Instruction> &list, uint32_t temp_count, const Function &function)
	{
		const auto reads = reads_of(sm4::input);
		for (size_t i = 0; i < out.inputs.size() && !patch_phase; ++i) {
			auto &element = out.inputs[i];
			const auto found = reads.find(element.reg);
			element.used = found == reads.end() ? 0 : uint8_t(found->second & element.mask);
			if (!element.used)
				continue;
			Operand operand = Operand::reg(sm4::input, element.reg);
			operand.mask = element.used;
			if (stage == Stage::geometry || stage == Stage::hull) {
				operand.dimension = 2;
				operand.index[0] = stage == Stage::geometry ? vertices : control_points;
				operand.index[1] = element.reg;
			}
			const uint32_t sv = element.system_value;
			if (stage == Stage::pixel) {
				const uint32_t mode = input_modes[i];
				if (sv == 7 || sv == 9 || sv == 10)
					list.push_back(declaration(sm4::dcl_input_ps_sgv, {operand}, mode, {sv}));
				else if (sv)
					list.push_back(declaration(sm4::dcl_input_ps_siv, {operand}, mode, {sv}));
				else
					list.push_back(declaration(sm4::dcl_input_ps, {operand}, mode));
			} else if (sv == 6 || sv == 8)
				list.push_back(declaration(sm4::dcl_input_sgv, {operand}, 0, {sv}));
			else if (sv)
				list.push_back(declaration(sm4::dcl_input_siv, {operand}, 0, {sv}));
			else
				list.push_back(declaration(sm4::dcl_input, {operand}));
		}
		for (const auto &[type, mask] : special_inputs) {
			Operand operand = Operand::none(type);
			if (scalar_register(type))
				operand.components = 1;
			else {
				operand.components = 4;
				operand.mask = mask;
			}
			list.push_back(declaration(sm4::dcl_input, {operand}));
		}
		if (stage == Stage::domain || patch_phase) {
			// Control points are read through vicp and, in the patch constant phase, the hull shader's own through vocp.
			for (const auto &[reg, mask] : reads_of(sm4::input_control_point))
				for (auto &element : out.inputs)
					if (element.reg == reg) {
						element.used |= uint8_t(mask & element.mask);
						list.push_back(declaration(sm4::dcl_input, {control_point(sm4::input_control_point, control_points, reg, element.mask)}));
					}
			for (const auto &[reg, mask] : reads_of(sm4::output_control_point))
				for (const auto &element : out.outputs)
					if (element.reg == reg)
						list.push_back(declaration(sm4::dcl_input, {control_point(sm4::output_control_point, output_control_points, reg, element.mask)}));
			const auto patch = reads_of(sm4::input_patch_constant);
			for (auto &element : out.patch_constants) {
				const auto found = patch.find(element.reg);
				if (stage != Stage::domain || found == patch.end())
					continue;
				element.used = uint8_t(found->second & element.mask);
				Operand operand = Operand::reg(sm4::input_patch_constant, element.reg);
				operand.mask = element.mask;
				if (element.system_value)
					list.push_back(declaration(sm4::dcl_input_siv, {operand}, 0, {tess_factor_name(element.system_value, element.semantic_index)}));
				else
					list.push_back(declaration(sm4::dcl_input, {operand}));
			}
		}
		if (patch_phase)
			for (const auto &element : out.patch_constants) {
				Operand operand = Operand::reg(sm4::output, element.reg);
				operand.mask = element.mask;
				if (element.system_value)
					list.push_back(declaration(sm4::dcl_output_siv, {operand}, 0, {tess_factor_name(element.system_value, element.semantic_index)}));
				else
					list.push_back(declaration(sm4::dcl_output, {operand}));
			}
		for (const auto &element : patch_phase ? std::vector<SignatureElement>() : out.outputs) {
			if (element.reg == ~0u) {
				const uint8_t types[] = {sm4::output_depth, sm4::output_coverage, sm4::output_depth_greater_equal, sm4::output_depth_less_equal};
				Operand operand = Operand::none(types[element.system_value - 65]);
				operand.components = 1;
				list.push_back(declaration(sm4::dcl_output, {operand}));
				continue;
			}
			Operand operand = Operand::reg(sm4::output, element.reg);
			operand.mask = element.mask;
			if (element.system_value && element.system_value < 64)
				list.push_back(declaration(sm4::dcl_output_siv, {operand}, 0, {element.system_value}));
			else
				list.push_back(declaration(sm4::dcl_output, {operand}));
		}
		if (temp_count)
			list.push_back(declaration(sm4::dcl_temps, {}, 0, {temp_count}));
		// Indexable temps belong to the phase that uses them.
		std::set<uint32_t> indexed;
		for (const auto &instruction : code)
			for (const auto &operand : instruction.operands)
				if (operand.type == sm4::indexable_temp)
					indexed.insert(operand.index[0]);
		for (const auto i : indexed)
			list.push_back(declaration(sm4::dcl_indexable_temp, {}, 0, {i, indexables[i].first, indexables[i].second}));
		for (size_t i = 0; i < shared.size(); ++i)
			list.push_back(declaration(sm4::dcl_tgsm_structured, {declared(sm4::group_shared, uint32_t(i))}, 0, {shared[i].stride, shared[i].count}));
		if (stage == Stage::compute) {
			const auto *threads = attribute(function, "numthreads");
			if (!threads || threads->size() != 3)
				fail(function.line, "a compute shader needs [numthreads(x, y, z)]");
			std::vector<uint32_t> sizes;
			for (const auto &size : *threads)
				sizes.push_back(uint32_t(std::stoul(size)));
			list.push_back(declaration(sm4::dcl_thread_group, {}, 0, sizes));
		}
	}

	// ---- reflection ----

	ReflectedType reflect(const Type &type) const
	{
		ReflectedType result;
		Type base = type;
		base.array.clear();
		result.elements = uint16_t(type.array.empty() ? 0 : element_count(type));
		result.name = base.name();
		switch (base.shape) {
		case Type::Shape::scalar:
			result.type_class = 0;
			break;
		case Type::Shape::vector:
			result.type_class = 1;
			break;
		case Type::Shape::matrix:
			result.type_class = column_major(base) ? 3 : 2;
			break;
		default:
			result.type_class = 5;
			break;
		}
		static const uint16_t kinds[] = {1, 2, 19, 3, 3, 39, 0};
		result.type = base.shape == Type::Shape::structure ? 0 : kinds[int(base.scalar)];
		result.rows = base.shape == Type::Shape::matrix ? base.rows : 1;
		result.columns = base.shape == Type::Shape::matrix || base.shape == Type::Shape::vector ? base.columns : 1;
		if (base.shape == Type::Shape::structure) {
			uint32_t offset = 0, scalars = 0;
			for (const auto &member : base.structure->members) {
				offset = cb_place(member.type, offset);
				result.members.emplace_back(member.name, reflect(member.type));
				result.member_offsets.push_back(offset);
				offset += cb_size(member.type);
				scalars += flat_size(member.type) / std::max<uint32_t>(1, element_count(member.type));
			}
			result.columns = uint16_t(scalars);
		}
		return result;
	}

	void reflection()
	{
		for (const auto &buffer : buffers) {
			if (!buffer.used)
				continue;
			ReflectedBuffer reflected;
			reflected.name = buffer.name;
			reflected.size = buffer.size;
			reflected.kind = buffer.texture ? 1 : 0;
			for (const auto &variable : buffer.variables) {
				ReflectedVariable item;
				item.name = variable.name;
				item.offset = variable.offset;
				item.size = variable.size;
				item.used = variable.used;
				item.type = reflect(variable.type);
				reflected.variables.push_back(item);
			}
			out.buffers.push_back(reflected);
		}
		std::vector<ReflectedBinding> samplers, views, uavs, constants;
		for (const auto &resource : resources) {
			if (!resource.used || resource.texture_buffer)
				continue;
			ReflectedBinding binding;
			binding.name = resource.name;
			binding.bind_point = uint32_t(resource.bound);
			binding.bind_count = resource.count;
			if (resource.explicit_register >= 0)
				binding.flags |= 1;
			if (is_sampler(resource.kind)) {
				binding.input_type = 3;
				if (resource.comparison)
					binding.flags |= 2;
				samplers.push_back(binding);
				continue;
			}
			binding.dimension = view_dimension(resource.kind);
			if (is_structured(resource.kind)) {
				binding.input_type = is_uav(resource.kind) ? (resource.counter ? 11 : 6) : 5;
				binding.return_type = 6;
				binding.samples = resource.stride;
				binding.stride = resource.stride;
			} else if (is_raw(resource.kind)) {
				binding.input_type = is_uav(resource.kind) ? 8 : 7;
				binding.return_type = 6;
			} else {
				binding.input_type = is_uav(resource.kind) ? 4 : 2;
				binding.return_type = return_type(resource.element);
				binding.samples = resource.kind == ObjectKind::texture2dms || resource.kind == ObjectKind::texture2dms_array ? resource.samples : ~0u;
				binding.flags |= uint32_t(std::min<uint32_t>(resource.element.components(), 4) - 1) << 2;
			}
			(is_uav(resource.kind) ? uavs : views).push_back(binding);
		}
		int index = 0;
		for (const auto &buffer : buffers) {
			if (buffer.used) {
				ReflectedBinding binding;
				binding.name = buffer.name;
				binding.input_type = buffer.texture ? 1 : 0;
				binding.bind_point = uint32_t(buffer.bound);
				binding.flags = (buffer.texture ? resources[size_t(buffer.resource)].explicit_register : buffer.explicit_register) >= 0 ? 1 : 0;
				binding.buffer = index++;
				constants.push_back(binding);
			}
		}
		for (auto *list : {&samplers, &views, &uavs, &constants}) {
			std::sort(list->begin(), list->end(), [](const ReflectedBinding &a, const ReflectedBinding &b) { return a.bind_point < b.bind_point; });
			out.bindings.insert(out.bindings.end(), list->begin(), list->end());
		}
	}

	// A tbuffer is read through its texture register: an operand naming one of its registers becomes a temp loaded with ld just before.
	void load_texture_buffers()
	{
		for (auto &buffer : buffers)
			if (buffer.texture && buffer.used)
				resources[size_t(buffer.resource)].used = true;
		for (auto *list : {&code, &control_code}) {
			std::vector<Instruction> rewritten;
			rewritten.reserve(list->size());
			for (auto &instruction : *list) {
				for (auto &operand : instruction.operands) {
					if (operand.type != sm4::constant_buffer || !buffers[operand.index[0]].texture)
						continue;
					Operand address = Operand::values({operand.index[1], 0, 0, 0}, 1);
					if (const auto relative = operand.relative[1]) {
						Operand sum = Operand::reg(sm4::temp, new_register());
						sum.mask = 1;
						Operand base = Operand::reg(relative->type, relative->index);
						base.mode = Operand::Mode::select;
						base.swizzle[0] = relative->component;
						Instruction add;
						add.opcode = sm4::iadd;
						add.operands = {sum, base, Operand::values({operand.index[1], 0, 0, 0}, 1)};
						rewritten.push_back(add);
						address = sum;
						address.mode = Operand::Mode::select;
						address.swizzle[0] = 0;
					}
					Operand loaded = Operand::reg(sm4::temp, new_register());
					Operand view = Operand::reg(sm4::resource, uint32_t(buffers[operand.index[0]].resource));
					view.mode = Operand::Mode::swizzle;
					Instruction load;
					load.opcode = sm4::ld;
					load.operands = {loaded, address, view};
					rewritten.push_back(load);
					operand.type = sm4::temp;
					operand.dimension = 1;
					operand.index = {loaded.index[0], 0, 0};
					operand.relative = {};
				}
				rewritten.push_back(std::move(instruction));
			}
			*list = std::move(rewritten);
		}
	}

	void finish(const Function &function)
	{
		load_texture_buffers();
		bind_all();
		uint32_t control_temps = 0;
		if (stage == Stage::hull) {
			std::swap(code, control_code);
			control_temps = compact_temps();
			std::swap(code, control_code);
		}
		const uint32_t temp_count = compact_temps();
		out.instruction_count = uint32_t(code.size() + control_code.size());
		out.temp_count = std::max(temp_count, control_temps);
		auto list = declarations(temp_count, control_temps, function);
		if (stage != Stage::hull)
			list.insert(list.end(), code.begin(), code.end());
		out.instructions = std::move(list);
		reflection();
		out.warnings = warnings;
	}
};

Generated Generator::run(const std::string &entry, const std::string &target)
{
	static const std::map<std::string, Stage> stages{{"ps", Stage::pixel}, {"vs", Stage::vertex}, {"gs", Stage::geometry}, {"hs", Stage::hull},
		{"ds", Stage::domain}, {"cs", Stage::compute}};
	const auto found = target.size() >= 6 ? stages.find(target.substr(0, 2)) : stages.end();
	if (found == stages.end() || target[2] != '_' || target[4] != '_' || !std::isdigit(uint8_t(target[3])) || !std::isdigit(uint8_t(target[5])))
		fail(0, "invalid target '" + target + "'");
	stage = found->second;
	out.stage = stage;
	out.major = uint32_t(target[3] - '0');
	out.minor = uint32_t(target[5] - '0');
	if (out.major < 4 || out.major > 5 || out.minor > 1 || (out.major == 5 && out.minor != 0))
		fail(0, "target '" + target + "' is not a shader model 4 or 5 target");
	sm5 = out.major == 5;
	if ((stage == Stage::hull || stage == Stage::domain) && !sm5)
		fail(0, "hull and domain shaders need shader model 5");
	const Function *function = nullptr;
	for (const auto &candidate : program.functions)
		if (candidate.name == entry && candidate.body && !candidate.owner)
			function = &candidate;
	if (!function)
		fail(0, "entry point '" + entry + "' not found");
	collect_globals(*function);
	if (stage == Stage::hull || stage == Stage::domain)
		tessellation(*function);
	initialize_statics();
	entry_point(*function);
	if (stage == Stage::hull) {
		// The patch constant function runs once per patch, after the control points, with registers of its own.
		const auto *name = attribute(*function, "patchconstantfunc");
		const Function *constants = nullptr;
		for (const auto &candidate : program.functions)
			if (name && !name->empty() && candidate.name == (*name)[0] && candidate.body)
				constants = &candidate;
		if (!constants)
			fail(function->line, "a hull shader needs [patchconstantfunc(\"name\")] naming a function");
		control_code = std::move(code);
		code.clear();
		control_specials = std::move(special_inputs);
		special_inputs.clear();
		output_copies.clear();
		// The control point phase ended with its return; the new phase starts reachable.
		unreachable = false;
		dead_depth = 0;
		patch_phase = true;
		initialize_statics();
		entry_point(*constants);
	}
	finish(*function);
	return std::move(out);
}

Generated generate(const Program &program, const std::string &entry, const std::string &target, uint32_t flags)
{
	try {
		Generator generator(program, flags);
		return generator.run(entry, target);
	} catch (const GenerateError &error) {
		// Texture offsets, resource array indices and the like must be constants: loops whose counter reaches them run unrolled, as the
		// reference compiler does without being asked.
		static const char *const constants_needed[] = {"must be literal", "needs a constant index", "must be a constant"};
		if (std::none_of(std::begin(constants_needed), std::end(constants_needed), [&](const char *text) { return error.message.find(text) != std::string::npos; }))
			throw;
		Generator generator(program, flags);
		generator.unroll_all = true;
		return generator.run(entry, target);
	}
}

}
