#include "hlsl_effect.h"

#include "dxbc.h"
#include "hlsl_codegen.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>

namespace raven::shader {

namespace {

[[noreturn]] void fail(uint32_t line, const std::string &message) { throw GenerateError{line, message}; }

bool same_word(const std::string &a, const std::string &b)
{
	return a.size() == b.size() &&
		std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

uint32_t align16(uint32_t value) { return (value + 15) & ~15u; }

// ECompilerAssignmentType.
enum Assignment : uint32_t {
	constant_assignment = 1, variable_assignment = 2, index_assignment = 3, variable_index_assignment = 4, inline_shader = 7, inline_shader5 = 8
};

// EScalarType.
enum EffectScalar : uint32_t { scalar_float = 1, scalar_int = 2, scalar_uint = 3, scalar_bool = 4 };

// The blocks a state belongs to.
enum class Block : uint8_t { pass, rasterizer, depth_stencil, blend, sampler };

// What a state takes: an object of some kind, or numbers.
enum class Value : uint8_t { rasterizer, depth_stencil, blend, render_target, depth_target, texture, shader, uint, uint8, boolean, floating };

struct Named {
	const char *name;
	uint32_t value;
};

const Named no_values[] = {{nullptr, 0}};
const Named booleans[] = {{"false", 0}, {"true", 1}, {nullptr, 0}};
const Named write_masks[] = {{"ZERO", 0}, {"ALL", 1}, {nullptr, 0}};
const Named fills[] = {{"WIREFRAME", 2}, {"SOLID", 3}, {nullptr, 0}};
const Named culls[] = {{"NONE", 1}, {"FRONT", 2}, {"BACK", 3}, {nullptr, 0}};
const Named comparisons[] = {{"NEVER", 1}, {"LESS", 2}, {"EQUAL", 3}, {"LESS_EQUAL", 4}, {"GREATER", 5}, {"NOT_EQUAL", 6},
	{"GREATER_EQUAL", 7}, {"ALWAYS", 8}, {nullptr, 0}};
const Named stencil_ops[] = {{"KEEP", 1}, {"ZERO", 2}, {"REPLACE", 3}, {"INCR_SAT", 4}, {"DECR_SAT", 5}, {"INVERT", 6}, {"INCR", 7},
	{"DECR", 8}, {nullptr, 0}};
const Named blends[] = {{"ZERO", 1}, {"ONE", 2}, {"SRC_COLOR", 3}, {"INV_SRC_COLOR", 4}, {"SRC_ALPHA", 5}, {"INV_SRC_ALPHA", 6},
	{"DEST_ALPHA", 7}, {"INV_DEST_ALPHA", 8}, {"DEST_COLOR", 9}, {"INV_DEST_COLOR", 10}, {"SRC_ALPHA_SAT", 11}, {"BLEND_FACTOR", 14},
	{"INV_BLEND_FACTOR", 15}, {"SRC1_COLOR", 16}, {"INV_SRC1_COLOR", 17}, {"SRC1_ALPHA", 18}, {"INV_SRC1_ALPHA", 19}, {nullptr, 0}};
const Named blend_ops[] = {{"ADD", 1}, {"SUBTRACT", 2}, {"REV_SUBTRACT", 3}, {"MIN", 4}, {"MAX", 5}, {nullptr, 0}};
const Named addresses[] = {{"WRAP", 1}, {"MIRROR", 2}, {"CLAMP", 3}, {"BORDER", 4}, {"MIRROR_ONCE", 5}, {nullptr, 0}};
const Named filters[] = {{"MIN_MAG_MIP_POINT", 0x00}, {"MIN_MAG_POINT_MIP_LINEAR", 0x01}, {"MIN_POINT_MAG_LINEAR_MIP_POINT", 0x04},
	{"MIN_POINT_MAG_MIP_LINEAR", 0x05}, {"MIN_LINEAR_MAG_MIP_POINT", 0x10}, {"MIN_LINEAR_MAG_POINT_MIP_LINEAR", 0x11},
	{"MIN_MAG_LINEAR_MIP_POINT", 0x14}, {"MIN_MAG_MIP_LINEAR", 0x15}, {"ANISOTROPIC", 0x55}, {"COMPARISON_MIN_MAG_MIP_POINT", 0x80},
	{"COMPARISON_MIN_MAG_POINT_MIP_LINEAR", 0x81}, {"COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT", 0x84},
	{"COMPARISON_MIN_POINT_MAG_MIP_LINEAR", 0x85}, {"COMPARISON_MIN_LINEAR_MAG_MIP_POINT", 0x90},
	{"COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR", 0x91}, {"COMPARISON_MIN_MAG_LINEAR_MIP_POINT", 0x94}, {"COMPARISON_MIN_MAG_MIP_LINEAR", 0x95},
	{"COMPARISON_ANISOTROPIC", 0xd5}, {nullptr, 0}};

struct State {
	const char *name;
	Block block;
	Value value;
	uint32_t columns, indices;
	const Named *names;
};

// Every state an effect can assign, at the index the effect runtime knows it by.
const State states[] = {
	{"RasterizerState", Block::pass, Value::rasterizer, 1, 1, no_values},
	{"DepthStencilState", Block::pass, Value::depth_stencil, 1, 1, no_values},
	{"BlendState", Block::pass, Value::blend, 1, 1, no_values},
	{"RenderTargetView", Block::pass, Value::render_target, 1, 8, no_values},
	{"DepthStencilView", Block::pass, Value::depth_target, 1, 8, no_values},
	{"GenerateMips", Block::pass, Value::texture, 1, 1, no_values},
	{"VertexShader", Block::pass, Value::shader, 1, 1, no_values},
	{"PixelShader", Block::pass, Value::shader, 1, 1, no_values},
	{"GeometryShader", Block::pass, Value::shader, 1, 1, no_values},
	{"DS_StencilRef", Block::pass, Value::uint, 1, 1, no_values},
	{"AB_BlendFactor", Block::pass, Value::floating, 4, 1, no_values},
	{"AB_SampleMask", Block::pass, Value::uint, 1, 1, no_values},
	{"FillMode", Block::rasterizer, Value::uint, 1, 1, fills},
	{"CullMode", Block::rasterizer, Value::uint, 1, 1, culls},
	{"FrontCounterClockwise", Block::rasterizer, Value::boolean, 1, 1, booleans},
	{"DepthBias", Block::rasterizer, Value::uint, 1, 1, no_values},
	{"DepthBiasClamp", Block::rasterizer, Value::floating, 1, 1, no_values},
	{"SlopeScaledDepthBias", Block::rasterizer, Value::floating, 1, 1, no_values},
	{"DepthClipEnable", Block::rasterizer, Value::boolean, 1, 1, booleans},
	{"ScissorEnable", Block::rasterizer, Value::boolean, 1, 1, booleans},
	{"MultisampleEnable", Block::rasterizer, Value::boolean, 1, 1, booleans},
	{"AntialiasedLineEnable", Block::rasterizer, Value::boolean, 1, 1, booleans},
	{"DepthEnable", Block::depth_stencil, Value::boolean, 1, 1, booleans},
	{"DepthWriteMask", Block::depth_stencil, Value::uint, 1, 1, write_masks},
	{"DepthFunc", Block::depth_stencil, Value::uint, 1, 1, comparisons},
	{"StencilEnable", Block::depth_stencil, Value::boolean, 1, 1, booleans},
	{"StencilReadMask", Block::depth_stencil, Value::uint8, 1, 1, no_values},
	{"StencilWriteMask", Block::depth_stencil, Value::uint8, 1, 1, no_values},
	{"FrontFaceStencilFail", Block::depth_stencil, Value::uint, 1, 1, stencil_ops},
	{"FrontFaceStencilDepthFail", Block::depth_stencil, Value::uint, 1, 1, stencil_ops},
	{"FrontFaceStencilPass", Block::depth_stencil, Value::uint, 1, 1, stencil_ops},
	{"FrontFaceStencilFunc", Block::depth_stencil, Value::uint, 1, 1, comparisons},
	{"BackFaceStencilFail", Block::depth_stencil, Value::uint, 1, 1, stencil_ops},
	{"BackFaceStencilDepthFail", Block::depth_stencil, Value::uint, 1, 1, stencil_ops},
	{"BackFaceStencilPass", Block::depth_stencil, Value::uint, 1, 1, stencil_ops},
	{"BackFaceStencilFunc", Block::depth_stencil, Value::uint, 1, 1, comparisons},
	{"AlphaToCoverageEnable", Block::blend, Value::boolean, 1, 1, booleans},
	{"BlendEnable", Block::blend, Value::boolean, 1, 8, booleans},
	{"SrcBlend", Block::blend, Value::uint, 1, 8, blends},
	{"DestBlend", Block::blend, Value::uint, 1, 8, blends},
	{"BlendOp", Block::blend, Value::uint, 1, 8, blend_ops},
	{"SrcBlendAlpha", Block::blend, Value::uint, 1, 8, blends},
	{"DestBlendAlpha", Block::blend, Value::uint, 1, 8, blends},
	{"BlendOpAlpha", Block::blend, Value::uint, 1, 8, blend_ops},
	{"RenderTargetWriteMask", Block::blend, Value::uint8, 1, 8, no_values},
	{"Filter", Block::sampler, Value::uint, 1, 1, filters},
	{"AddressU", Block::sampler, Value::uint, 1, 1, addresses},
	{"AddressV", Block::sampler, Value::uint, 1, 1, addresses},
	{"AddressW", Block::sampler, Value::uint, 1, 1, addresses},
	{"MipLODBias", Block::sampler, Value::floating, 1, 1, no_values},
	{"MaxAnisotropy", Block::sampler, Value::uint, 1, 1, no_values},
	{"ComparisonFunc", Block::sampler, Value::uint, 1, 1, comparisons},
	{"BorderColor", Block::sampler, Value::floating, 4, 1, no_values},
	{"MinLOD", Block::sampler, Value::floating, 1, 1, no_values},
	{"MaxLOD", Block::sampler, Value::floating, 1, 1, no_values},
	{"Texture", Block::sampler, Value::texture, 1, 1, no_values},
	{"HullShader", Block::pass, Value::shader, 1, 1, no_values},
	{"DomainShader", Block::pass, Value::shader, 1, 1, no_values},
	{"ComputeShader", Block::pass, Value::shader, 1, 1, no_values},
};

bool is_shader(ObjectKind kind) { return kind >= ObjectKind::vertex_shader && kind <= ObjectKind::compute_shader; }

// Bytes written at offsets from the start of the effect's unstructured part, each string and type once.
class Unstructured {
public:
	// Offset zero stands for no string or no value.
	Unstructured() { out.u32(0); }

	uint32_t text(const std::string &value)
	{
		if (auto found = strings.find(value); found != strings.end())
			return found->second;
		const uint32_t at = uint32_t(out.size());
		out.text(value);
		strings[value] = at;
		return at;
	}

	uint32_t text_or_none(const std::string &value) { return value.empty() ? 0 : text(value); }

	// Words, shared between identical runs such as the same type described twice.
	uint32_t words(const std::vector<uint32_t> &values, bool shared)
	{
		out.align(4);
		if (shared)
			if (auto found = runs.find(values); found != runs.end())
				return found->second;
		const uint32_t at = uint32_t(out.size());
		for (const auto value : values)
			out.u32(value);
		if (shared)
			runs[values] = at;
		return at;
	}

	// A block of bytes after its size, as shaders are stored.
	uint32_t block(const std::vector<uint8_t> &data)
	{
		out.align(4);
		const uint32_t at = uint32_t(out.size());
		out.u32(uint32_t(data.size()));
		out.bytes(data);
		return at;
	}

	std::vector<uint8_t> take()
	{
		out.align(4);
		return out.take();
	}

private:
	Writer out;
	std::map<std::string, uint32_t> strings;
	std::map<std::vector<uint32_t>, uint32_t> runs;
};

class Effect {
public:
	Effect(const Program &program, const std::string &target, uint32_t flags, const EffectShaderCompiler &compile)
		: program(program), flags(flags), compile(compile)
	{
		if (target == "fx_4_0")
			tag = 0xfeff1001;
		else if (target == "fx_4_1")
			tag = 0xfeff1011;
		else if (target == "fx_5_0") {
			tag = 0xfeff2001;
			five = true;
		} else
			fail(0, "target '" + target + "' is not an effect target");
		for (const auto &variable : program.globals)
			declared[variable.name] = &variable;
		for (const auto &buffer : program.buffers)
			for (const auto &member : buffer.members)
				declared[member.name] = &member;
	}

	std::vector<uint8_t> run()
	{
		buffers();
		objects();
		techniques();
		Writer header;
		header.u32(tag);
		header.u32(buffer_count);
		header.u32(numeric_count);
		header.u32(object_count);
		for (int i = 0; i < 3; ++i)
			header.u32(0);
		header.u32(technique_count);
		const auto data = unstructured.take();
		header.u32(uint32_t(data.size()));
		header.u32(string_count);
		header.u32(resource_count);
		header.u32(depth_stencil_count);
		header.u32(blend_count);
		header.u32(rasterizer_count);
		header.u32(sampler_count);
		header.u32(render_target_count);
		header.u32(depth_target_count);
		header.u32(shader_count);
		header.u32(inline_count);
		if (five) {
			header.u32(group_count);
			header.u32(uav_count);
			for (int i = 0; i < 3; ++i)
				header.u32(0);
		}
		header.bytes(data);
		header.bytes(structured.take());
		return header.take();
	}

private:
	const Program &program;
	uint32_t flags;
	const EffectShaderCompiler &compile;
	uint32_t tag = 0;
	bool five = false;
	std::map<std::string, const Variable *> declared;
	Unstructured unstructured;
	Writer structured;
	uint32_t buffer_count = 0, numeric_count = 0, object_count = 0, technique_count = 0, string_count = 0, resource_count = 0,
		depth_stencil_count = 0, blend_count = 0, rasterizer_count = 0, sampler_count = 0, render_target_count = 0, depth_target_count = 0,
		shader_count = 0, inline_count = 0, group_count = 0, uav_count = 0;

	static uint32_t elements_of(const Type &type)
	{
		uint32_t count = 1;
		for (const auto size : type.array)
			count *= size;
		return type.array.empty() ? 0 : count;
	}

	// ---- types ----

	static uint32_t packed_size(const ReflectedType &type)
	{
		uint32_t one = 0;
		if (type.type_class == 5) {
			for (const auto &[name, member] : type.members)
				one += packed_size(member);
		} else
			one = uint32_t(type.rows) * type.columns * 4;
		return one * std::max<uint32_t>(1, type.elements);
	}

	uint32_t numeric_type(const ReflectedType &type)
	{
		std::vector<uint32_t> words{unstructured.text(type.name), type.type_class == 5 ? 3u : 1u, type.elements, type.size,
			align16(type.element_size), packed_size(type)};
		if (type.type_class == 5) {
			words.push_back(uint32_t(type.members.size()));
			for (size_t i = 0; i < type.members.size(); ++i) {
				words.push_back(unstructured.text(type.members[i].first));
				words.push_back(0);
				words.push_back(type.member_offsets[i]);
				words.push_back(numeric_type(type.members[i].second));
			}
			// The base class and the interfaces a structure implements, which effect structures do not have.
			if (five) {
				words.push_back(0);
				words.push_back(0);
			}
		} else {
			const uint32_t layout = type.type_class == 0 ? 1 : type.type_class == 1 ? 2 : 3;
			const uint32_t scalar = type.type == 1 ? scalar_bool : type.type == 2 ? scalar_int : type.type == 19 ? scalar_uint : scalar_float;
			words.push_back(layout | scalar << 3 | uint32_t(type.rows) << 8 | uint32_t(type.columns) << 11 | (type.type_class == 3 ? 1u << 14 : 0));
		}
		return unstructured.words(words, true);
	}

	// EObjectType, with the shader model 5 kinds of shaders in fx_5_0.
	uint32_t object_type(const Variable &variable) const
	{
		const ObjectKind kind = variable.type.object;
		switch (kind) {
		case ObjectKind::string: return 1;
		case ObjectKind::blend_state: return 2;
		case ObjectKind::depth_stencil_state: return 3;
		case ObjectKind::rasterizer_state: return 4;
		case ObjectKind::pixel_shader: return 5;
		case ObjectKind::vertex_shader: return 6;
		case ObjectKind::geometry_shader:
			// A geometry shader with stream output declarations: fx_5_0 holds up to four of them.
			if (variable.initializer && variable.initializer->kind == Expr::Kind::call && variable.initializer->text.starts_with("ConstructGSWithSO"))
				return five ? 27 : 8;
			return 7;
		case ObjectKind::compute_shader: return 28;
		case ObjectKind::hull_shader: return 29;
		case ObjectKind::domain_shader: return 30;
		case ObjectKind::texture1d: return 10;
		case ObjectKind::texture1d_array: return 11;
		case ObjectKind::texture2d: return 12;
		case ObjectKind::texture2d_array: return 13;
		case ObjectKind::texture2dms: return 14;
		case ObjectKind::texture2dms_array: return 15;
		case ObjectKind::texture3d: return 16;
		case ObjectKind::texture_cube: return 17;
		case ObjectKind::render_target_view: return 19;
		case ObjectKind::depth_stencil_view: return 20;
		case ObjectKind::sampler:
		case ObjectKind::sampler_comparison: return 21;
		case ObjectKind::buffer: return 22;
		case ObjectKind::texture_cube_array: return 23;
		case ObjectKind::rw_texture1d: return 31;
		case ObjectKind::rw_texture1d_array: return 32;
		case ObjectKind::rw_texture2d: return 33;
		case ObjectKind::rw_texture2d_array: return 34;
		case ObjectKind::rw_texture3d: return 35;
		case ObjectKind::rw_buffer: return 36;
		case ObjectKind::byte_address_buffer: return 37;
		case ObjectKind::rw_byte_address_buffer: return 38;
		case ObjectKind::structured_buffer: return 39;
		case ObjectKind::rw_structured_buffer: return 40;
		default: return 0;
		}
	}

	static std::string object_name(const Type &type)
	{
		Type plain = type;
		plain.array.clear();
		switch (type.object) {
		case ObjectKind::string: return "string";
		case ObjectKind::blend_state: return "BlendState";
		case ObjectKind::depth_stencil_state: return "DepthStencilState";
		case ObjectKind::rasterizer_state: return "RasterizerState";
		case ObjectKind::vertex_shader: return "VertexShader";
		case ObjectKind::pixel_shader: return "PixelShader";
		case ObjectKind::geometry_shader: return "GeometryShader";
		case ObjectKind::hull_shader: return "HullShader";
		case ObjectKind::domain_shader: return "DomainShader";
		case ObjectKind::compute_shader: return "ComputeShader";
		case ObjectKind::render_target_view: return "RenderTargetView";
		case ObjectKind::depth_stencil_view: return "DepthStencilView";
		case ObjectKind::sampler: return "SamplerState";
		case ObjectKind::sampler_comparison: return "SamplerComparisonState";
		case ObjectKind::texture1d: return "Texture1D";
		case ObjectKind::texture1d_array: return "Texture1DArray";
		case ObjectKind::texture2d: return "Texture2D";
		case ObjectKind::texture2d_array: return "Texture2DArray";
		case ObjectKind::texture2dms: return "Texture2DMS";
		case ObjectKind::texture2dms_array: return "Texture2DMSArray";
		case ObjectKind::texture3d: return "Texture3D";
		case ObjectKind::texture_cube: return "TextureCube";
		case ObjectKind::texture_cube_array: return "TextureCubeArray";
		default: return plain.name();
		}
	}

	uint32_t object_type_info(const Variable &variable)
	{
		const uint32_t kind = object_type(variable);
		if (!kind)
			fail(variable.line, "'" + variable.name + "' has a type effects cannot hold");
		return unstructured.words({unstructured.text(object_name(variable.type)), 2, elements_of(variable.type), 0, 0, 0, kind}, true);
	}

	// ---- annotations ----

	void annotations(const std::vector<Variable> &list)
	{
		structured.u32(uint32_t(list.size()));
		for (const auto &annotation : list) {
			if (!annotation.initializer)
				fail(annotation.line, "annotation '" + annotation.name + "' needs a value");
			const uint32_t count = std::max<uint32_t>(1, elements_of(annotation.type));
			if (annotation.type.shape == Type::Shape::object) {
				if (annotation.type.object != ObjectKind::string)
					fail(annotation.line, "annotations are numbers or strings");
				structured.u32(unstructured.text(annotation.name));
				structured.u32(object_type_info(annotation));
				std::vector<const Expr *> texts;
				if (annotation.initializer->kind == Expr::Kind::initializer_list)
					for (const auto &item : annotation.initializer->operands)
						texts.push_back(item.get());
				else
					texts.push_back(annotation.initializer.get());
				if (texts.size() != count)
					fail(annotation.line, "annotation '" + annotation.name + "' needs " + std::to_string(count) + " strings");
				for (const auto *text : texts) {
					if (text->kind != Expr::Kind::string)
						fail(annotation.line, "annotation '" + annotation.name + "' needs a string");
					structured.u32(unstructured.text(text->text));
				}
				continue;
			}
			structured.u32(unstructured.text(annotation.name));
			structured.u32(numeric_type(effect_type(program, annotation.type, flags)));
			const auto scalars = effect_constant(program, *annotation.initializer, annotation.type, flags);
			std::vector<uint32_t> bits;
			for (const auto &[value, kind] : scalars)
				bits.push_back(value);
			structured.u32(unstructured.words(bits, false));
		}
	}

	// ---- constant buffers ----

	void buffers()
	{
		for (const auto &buffer : effect_layout(program, flags)) {
			if (buffer.name == "$Globals" && buffer.variables.empty())
				continue;
			int explicit_register = -1;
			const ConstantBuffer *source = nullptr;
			for (const auto &declared_buffer : program.buffers)
				if (declared_buffer.name == buffer.name)
					source = &declared_buffer;
			if (source)
				explicit_register = source->register_index;
			structured.u32(unstructured.text(buffer.name));
			structured.u32(buffer.size);
			structured.u32(buffer.kind == 1 ? 1 : 0);
			structured.u32(uint32_t(buffer.variables.size()));
			structured.u32(uint32_t(explicit_register));
			annotations({});
			for (const auto &variable : buffer.variables) {
				const auto found = declared.find(variable.name);
				const Variable *source_variable = found == declared.end() ? nullptr : found->second;
				structured.u32(unstructured.text(variable.name));
				structured.u32(numeric_type(variable.type));
				structured.u32(source_variable ? unstructured.text_or_none(source_variable->semantic) : 0);
				const bool bound = source_variable && source_variable->pack_offset >= 0;
				structured.u32(variable.offset);
				std::vector<uint32_t> bits(variable.default_value.size() / 4);
				std::memcpy(bits.data(), variable.default_value.data(), bits.size() * 4);
				structured.u32(bits.empty() ? 0 : unstructured.words(bits, false));
				structured.u32(bound ? 4 : 0);
				static const std::vector<Variable> none;
				annotations(source_variable ? source_variable->annotations : none);
				++numeric_count;
			}
			++buffer_count;
		}
	}

	// ---- objects ----

	void objects()
	{
		for (const auto &variable : program.globals) {
			if (variable.type.shape != Type::Shape::object || variable.is_static)
				continue;
			const uint32_t count = std::max<uint32_t>(1, elements_of(variable.type));
			structured.u32(unstructured.text(variable.name));
			structured.u32(object_type_info(variable));
			structured.u32(unstructured.text_or_none(variable.semantic));
			structured.u32(variable.register_index >= 0 ? uint32_t(variable.register_index) : ~0u);
			const ObjectKind kind = variable.type.object;
			if (kind == ObjectKind::blend_state || kind == ObjectKind::depth_stencil_state || kind == ObjectKind::rasterizer_state ||
				kind == ObjectKind::sampler || kind == ObjectKind::sampler_comparison) {
				const Block block = kind == ObjectKind::blend_state ? Block::blend : kind == ObjectKind::depth_stencil_state ? Block::depth_stencil :
					kind == ObjectKind::rasterizer_state ? Block::rasterizer : Block::sampler;
				if (!variable.element_states.empty() && variable.element_states.size() != count)
					fail(variable.line, "'" + variable.name + "' needs " + std::to_string(count) + " state blocks");
				for (uint32_t i = 0; i < count; ++i) {
					const auto &states = variable.element_states.empty() ? variable.states : variable.element_states[i];
					structured.u32(uint32_t(states.size()));
					for (const auto &state : states)
						assignment(block, state.name, state.index, *state.value, state.line);
				}
				(block == Block::blend ? blend_count : block == Block::depth_stencil ? depth_stencil_count : block == Block::rasterizer ?
					rasterizer_count : sampler_count) += count;
			} else if (is_shader(kind)) {
				std::vector<const Expr *> values;
				if (variable.initializer && variable.initializer->kind == Expr::Kind::initializer_list)
					for (const auto &item : variable.initializer->operands)
						values.push_back(item.get());
				else
					values.push_back(variable.initializer.get());
				if (values.size() != count)
					fail(variable.line, "'" + variable.name + "' needs " + std::to_string(count) + " shaders");
				// Shader model 5 kinds carry SBinaryShaderData5, the others the bytecode's offset and, for stream output, one declaration.
				const uint32_t type = object_type(variable);
				for (const auto *value : values) {
					const auto shader = shader_of(value, variable.line);
					if (type >= 25)
						shader_data5(shader, structured);
					else {
						structured.u32(shader.code);
						if (type == 8)
							structured.u32(shader.outputs.empty() ? 0 : shader.outputs[0]);
					}
				}
				shader_count += count;
			} else if (kind == ObjectKind::string) {
				if (!variable.initializer)
					fail(variable.line, "string '" + variable.name + "' needs a value");
				std::vector<const Expr *> texts;
				if (variable.initializer->kind == Expr::Kind::initializer_list)
					for (const auto &item : variable.initializer->operands)
						texts.push_back(item.get());
				else
					texts.push_back(variable.initializer.get());
				if (texts.size() != count)
					fail(variable.line, "'" + variable.name + "' needs " + std::to_string(count) + " strings");
				for (const auto *text : texts) {
					if (text->kind != Expr::Kind::string)
						fail(variable.line, "'" + variable.name + "' needs a string");
					structured.u32(unstructured.text(text->text));
				}
				string_count += count;
			} else if (kind == ObjectKind::render_target_view)
				render_target_count += count;
			else if (kind == ObjectKind::depth_stencil_view)
				depth_target_count += count;
			else if (kind >= ObjectKind::rw_texture1d && kind <= ObjectKind::rw_byte_address_buffer)
				uav_count += count;
			else
				resource_count += count;
			annotations(variable.annotations);
			++object_count;
		}
	}

	// ---- shaders ----

	struct Shader {
		// The block holding the bytecode, or zero for none; the stream output declarations.
		uint32_t code = 0;
		std::vector<uint32_t> outputs;
		uint32_t rasterized = 0;
	};

	// A shader value: CompileShader(target, entry(...)), ConstructGSWithSO(shader, declarations...), NULL, or no initializer.
	Shader shader_of(const Expr *value, uint32_t line)
	{
		Shader shader;
		if (!value || (value->kind == Expr::Kind::name && same_word(value->text, "NULL")) ||
			(value->kind == Expr::Kind::literal && value->number == 0))
			return shader;
		if (value->kind == Expr::Kind::call && (value->text == "ConstructGSWithSO" || value->text == "ConstructGSWithSO5")) {
			if (value->operands.empty())
				fail(line, "ConstructGSWithSO needs a shader");
			shader = shader_of(value->operands[0].get(), line);
			for (size_t i = 1; i < value->operands.size(); ++i) {
				const auto &argument = *value->operands[i];
				if (argument.kind == Expr::Kind::string)
					shader.outputs.push_back(unstructured.text(argument.text));
				else if (argument.kind == Expr::Kind::name && same_word(argument.text, "NULL") && i + 1 < value->operands.size())
					shader.outputs.push_back(0);
				else if (i + 1 == value->operands.size())
					shader.rasterized = uint32_t(effect_constant(program, argument, Type::make_scalar(Scalar::unsigned_integer), flags)[0].first);
				else
					fail(line, "a stream output declaration is a string");
			}
			if (shader.outputs.size() > 4)
				fail(line, "a geometry shader streams to at most four outputs");
			return shader;
		}
		if (value->kind != Expr::Kind::call || value->text != "CompileShader" || value->operands.size() != 2 ||
			value->operands[0]->kind != Expr::Kind::name || value->operands[1]->kind != Expr::Kind::call)
			fail(line, "a shader is set with CompileShader(target, entry())");
		const auto &call = *value->operands[1];
		std::vector<const Expr *> uniforms;
		for (const auto &argument : call.operands)
			uniforms.push_back(argument.get());
		shader.code = unstructured.block(compile(call.text, value->operands[0]->text, uniforms, line));
		return shader;
	}

	// SBinaryShaderData5.
	static void shader_data5(const Shader &shader, Writer &out)
	{
		out.u32(shader.code);
		for (size_t i = 0; i < 4; ++i)
			out.u32(i < shader.outputs.size() ? shader.outputs[i] : 0);
		out.u32(uint32_t(shader.outputs.size()));
		out.u32(shader.rasterized);
		out.u32(0);
		out.u32(0);
	}

	// ---- assignments ----

	static std::optional<uint32_t> state_index(Block block, const std::string &name)
	{
		for (uint32_t i = 0; i < std::size(states); ++i)
			if (states[i].block == block && same_word(states[i].name, name))
				return i;
		return std::nullopt;
	}

	const Variable *global(const Expr &value) const
	{
		if (value.kind != Expr::Kind::name)
			return nullptr;
		const auto found = declared.find(value.text);
		return found == declared.end() ? nullptr : found->second;
	}

	void assignment(Block block, const std::string &name, std::optional<uint32_t> index, const Expr &value, uint32_t line)
	{
		const auto found = state_index(block, name);
		if (!found)
			fail(line, "unknown state '" + name + "'");
		const State &state = states[*found];
		if (index && *index >= state.indices)
			fail(line, "state '" + name + "' has no index " + std::to_string(*index));
		structured.u32(*found);
		structured.u32(index.value_or(0));
		const bool object = state.value <= Value::shader;
		const bool null = (value.kind == Expr::Kind::name && same_word(value.text, "NULL")) || (value.kind == Expr::Kind::literal && value.number == 0);
		if (object && null) {
			structured.u32(constant_assignment);
			structured.u32(unstructured.words({1, scalar_int, 0}, false));
			return;
		}
		if (state.value == Value::shader && value.kind == Expr::Kind::call) {
			const Shader shader = shader_of(&value, line);
			++shader_count;
			++inline_count;
			if (five) {
				Writer data;
				shader_data5(shader, data);
				const auto bytes = data.take();
				std::vector<uint32_t> words(bytes.size() / 4);
				std::memcpy(words.data(), bytes.data(), bytes.size());
				structured.u32(inline_shader5);
				structured.u32(unstructured.words(words, false));
			} else {
				structured.u32(inline_shader);
				structured.u32(unstructured.words({shader.code, shader.outputs.empty() ? 0 : shader.outputs[0]}, false));
			}
			return;
		}
		if (const Variable *variable = global(value)) {
			structured.u32(variable_assignment);
			structured.u32(unstructured.text(variable->name));
			return;
		}
		// An element chosen by an integer variable the program sets; any other expression would need the effect expression machine.
		if (value.kind == Expr::Kind::index && value.operands.size() == 2 && global(*value.operands[0]) && global(*value.operands[1])) {
			const Variable &index = *global(*value.operands[1]);
			if (index.type.shape != Type::Shape::scalar || (index.type.scalar != Scalar::integer && index.type.scalar != Scalar::unsigned_integer))
				fail(line, "'" + index.name + "' is not an integer to index '" + value.operands[0]->text + "' with");
			structured.u32(variable_index_assignment);
			structured.u32(unstructured.words({unstructured.text(value.operands[0]->text), unstructured.text(index.name)}, false));
			return;
		}
		if (value.kind == Expr::Kind::index && value.operands.size() == 2 && global(*value.operands[0])) {
			const uint32_t element = uint32_t(effect_constant(program, *value.operands[1], Type::make_scalar(Scalar::unsigned_integer), flags)[0].first);
			structured.u32(index_assignment);
			structured.u32(unstructured.words({unstructured.text(value.operands[0]->text), element}, false));
			return;
		}
		if (object)
			fail(line, "state '" + name + "' takes an object");
		// A named value such as SRC_ALPHA, or TRUE and FALSE.
		std::vector<uint32_t> constants;
		if (value.kind == Expr::Kind::name) {
			const Named *names = state.value == Value::boolean ? booleans : state.names;
			for (const Named *item = names; item->name; ++item)
				if (same_word(item->name, value.text)) {
					constants = {1, state.value == Value::boolean ? scalar_bool : scalar_uint, item->value};
					break;
				}
			if (constants.empty())
				fail(line, "'" + value.text + "' is not a value of state '" + name + "'");
		} else {
			const auto scalars = effect_constant(program, value, Type{}, flags);
			if (scalars.size() != state.columns)
				fail(line, "state '" + name + "' takes " + std::to_string(state.columns) + " values");
			constants.push_back(uint32_t(scalars.size()));
			for (const auto &[bits, kind] : scalars) {
				constants.push_back(kind == Scalar::boolean ? scalar_bool : kind == Scalar::integer ? scalar_int :
					kind == Scalar::unsigned_integer ? scalar_uint : scalar_float);
				constants.push_back(bits);
			}
		}
		structured.u32(constant_assignment);
		structured.u32(unstructured.words(constants, false));
	}

	// The assignments of a pass, with each Set call standing for the states it sets.
	std::vector<std::tuple<std::string, std::optional<uint32_t>, const Expr *, uint32_t>> pass_states(const Pass &pass)
	{
		std::vector<std::tuple<std::string, std::optional<uint32_t>, const Expr *, uint32_t>> list;
		for (const auto &state : pass.states) {
			const Expr &value = *state.value;
			if (value.kind != Expr::Kind::call || state.index || !value.text.starts_with("Set")) {
				list.emplace_back(state.name, state.index, &value, state.line);
				continue;
			}
			const std::string what = value.text.substr(3);
			const auto argument = [&](size_t i) { return value.operands[i].get(); };
			const auto expect = [&](size_t count) {
				if (value.operands.size() != count)
					fail(state.line, value.text + " takes " + std::to_string(count) + " arguments");
			};
			if (what.ends_with("Shader")) {
				expect(1);
				list.emplace_back(what, std::nullopt, argument(0), state.line);
			} else if (what == "BlendState") {
				expect(3);
				list.emplace_back("BlendState", std::nullopt, argument(0), state.line);
				list.emplace_back("AB_BlendFactor", std::nullopt, argument(1), state.line);
				list.emplace_back("AB_SampleMask", std::nullopt, argument(2), state.line);
			} else if (what == "DepthStencilState") {
				expect(2);
				list.emplace_back("DepthStencilState", std::nullopt, argument(0), state.line);
				list.emplace_back("DS_StencilRef", std::nullopt, argument(1), state.line);
			} else if (what == "RasterizerState") {
				expect(1);
				list.emplace_back("RasterizerState", std::nullopt, argument(0), state.line);
			} else if (what == "RenderTargets") {
				if (value.operands.empty())
					fail(state.line, "SetRenderTargets needs a render target");
				for (size_t i = 0; i + 1 < value.operands.size(); ++i)
					list.emplace_back("RenderTargetView", uint32_t(i), argument(i), state.line);
				list.emplace_back("DepthStencilView", std::nullopt, argument(value.operands.size() - 1), state.line);
			} else
				fail(state.line, "unknown pass call '" + value.text + "'");
		}
		return list;
	}

	// ---- groups, techniques and passes ----

	void technique(const Technique &item)
	{
		if (item.version == 11 && !five)
			fail(item.line, "technique11 needs fx_5_0");
		structured.u32(unstructured.text_or_none(item.name));
		structured.u32(uint32_t(item.passes.size()));
		annotations(item.annotations);
		for (const auto &pass : item.passes) {
			const auto list = pass_states(pass);
			structured.u32(unstructured.text_or_none(pass.name));
			structured.u32(uint32_t(list.size()));
			annotations(pass.annotations);
			for (const auto &[name, index, value, line] : list)
				assignment(Block::pass, name, index, *value, line);
		}
		++technique_count;
	}

	void techniques()
	{
		if (!five) {
			if (!program.groups.empty())
				fail(program.groups[0].line, "groups need fx_5_0");
			for (const auto &item : program.techniques)
				technique(item);
			return;
		}
		// Techniques outside every group belong to the group without a name.
		if (!program.techniques.empty()) {
			structured.u32(0);
			structured.u32(uint32_t(program.techniques.size()));
			annotations({});
			for (const auto &item : program.techniques)
				technique(item);
			++group_count;
		}
		for (const auto &group : program.groups) {
			structured.u32(unstructured.text(group.name));
			structured.u32(uint32_t(group.techniques.size()));
			annotations(group.annotations);
			for (const auto &item : group.techniques)
				technique(item);
			++group_count;
		}
	}
};

}

std::vector<uint8_t> effect_binary(const Program &program, const std::string &target, uint32_t flags, const EffectShaderCompiler &compile)
{
	return Effect(program, target, flags, compile).run();
}

}
