#include "hlsl_effect.h"

#include "hlsl_codegen.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>

namespace raven::shader {

namespace {

[[noreturn]] void fail(uint32_t line, const std::string &message) { throw GenerateError{line, message}; }

// Upper case without underscores, the form names of states and their values are compared in: InvSrcAlpha, INV_SRC_ALPHA.
std::string folded(const std::string &text)
{
	std::string result;
	for (const char c : text)
		if (c != '_')
			result += char(std::toupper(uint8_t(c)));
	return result;
}

// D3DXPARAMETER_TYPE and D3DXPARAMETER_CLASS.
enum ParameterType : uint32_t {
	type_void = 0, type_bool = 1, type_int = 2, type_float = 3, type_string = 4, type_texture = 5, type_texture1d = 6, type_texture2d = 7,
	type_texture3d = 8, type_texture_cube = 9, type_sampler = 10, type_sampler1d = 11, type_sampler2d = 12, type_sampler3d = 13,
	type_sampler_cube = 14, type_pixel_shader = 15, type_vertex_shader = 16
};
enum ParameterClass : uint32_t { class_scalar = 0, class_vector = 1, class_matrix_rows = 2, class_matrix_columns = 3, class_object = 4, class_struct = 5 };

struct Named {
	const char *name;
	uint32_t value;
};

const Named no_values[] = {{nullptr, 0}};
const Named depth_buffers[] = {{"FALSE", 0}, {"TRUE", 1}, {"USEW", 2}, {nullptr, 0}};
const Named fills[] = {{"POINT", 1}, {"WIREFRAME", 2}, {"SOLID", 3}, {nullptr, 0}};
const Named shades[] = {{"FLAT", 1}, {"GOURAUD", 2}, {"PHONG", 3}, {nullptr, 0}};
const Named blends[] = {{"ZERO", 1}, {"ONE", 2}, {"SRCCOLOR", 3}, {"INVSRCCOLOR", 4}, {"SRCALPHA", 5}, {"INVSRCALPHA", 6}, {"DESTALPHA", 7},
	{"INVDESTALPHA", 8}, {"DESTCOLOR", 9}, {"INVDESTCOLOR", 10}, {"SRCALPHASAT", 11}, {"BOTHSRCALPHA", 12}, {"BOTHINVSRCALPHA", 13},
	{"BLENDFACTOR", 14}, {"INVBLENDFACTOR", 15}, {"SRCCOLOR2", 16}, {"INVSRCCOLOR2", 17}, {nullptr, 0}};
const Named culls[] = {{"NONE", 1}, {"CW", 2}, {"CCW", 3}, {nullptr, 0}};
const Named comparisons[] = {{"NEVER", 1}, {"LESS", 2}, {"EQUAL", 3}, {"LESSEQUAL", 4}, {"GREATER", 5}, {"NOTEQUAL", 6}, {"GREATEREQUAL", 7},
	{"ALWAYS", 8}, {nullptr, 0}};
const Named fogs[] = {{"NONE", 0}, {"EXP", 1}, {"EXP2", 2}, {"LINEAR", 3}, {nullptr, 0}};
const Named stencil_ops[] = {{"KEEP", 1}, {"ZERO", 2}, {"REPLACE", 3}, {"INCRSAT", 4}, {"DECRSAT", 5}, {"INVERT", 6}, {"INCR", 7}, {"DECR", 8},
	{nullptr, 0}};
const Named wraps[] = {{"U", 1}, {"V", 2}, {"W", 4}, {"COORD0", 1}, {"COORD1", 2}, {"COORD2", 4}, {"COORD3", 8}, {nullptr, 0}};
const Named material_sources[] = {{"MATERIAL", 0}, {"COLOR1", 1}, {"COLOR2", 2}, {nullptr, 0}};
const Named vertex_blends[] = {{"DISABLE", 0}, {"1WEIGHTS", 1}, {"2WEIGHTS", 2}, {"3WEIGHTS", 3}, {"TWEENING", 255}, {"0WEIGHTS", 256},
	{nullptr, 0}};
const Named patch_edges[] = {{"DISCRETE", 0}, {"CONTINUOUS", 1}, {nullptr, 0}};
const Named color_writes[] = {{"RED", 1}, {"GREEN", 2}, {"BLUE", 4}, {"ALPHA", 8}, {nullptr, 0}};
const Named blend_ops[] = {{"ADD", 1}, {"SUBTRACT", 2}, {"REVSUBTRACT", 3}, {"MIN", 4}, {"MAX", 5}, {nullptr, 0}};
const Named degrees[] = {{"LINEAR", 1}, {"QUADRATIC", 2}, {"CUBIC", 3}, {"QUINTIC", 5}, {nullptr, 0}};
const Named texture_ops[] = {{"DISABLE", 1}, {"SELECTARG1", 2}, {"SELECTARG2", 3}, {"MODULATE", 4}, {"MODULATE2X", 5}, {"MODULATE4X", 6},
	{"ADD", 7}, {"ADDSIGNED", 8}, {"ADDSIGNED2X", 9}, {"SUBTRACT", 10}, {"ADDSMOOTH", 11}, {"BLENDDIFFUSEALPHA", 12},
	{"BLENDTEXTUREALPHA", 13}, {"BLENDFACTORALPHA", 14}, {"BLENDTEXTUREALPHAPM", 15}, {"BLENDCURRENTALPHA", 16}, {"PREMODULATE", 17},
	{"MODULATEALPHAADDCOLOR", 18}, {"MODULATECOLORADDALPHA", 19}, {"MODULATEINVALPHAADDCOLOR", 20}, {"MODULATEINVCOLORADDALPHA", 21},
	{"BUMPENVMAP", 22}, {"BUMPENVMAPLUMINANCE", 23}, {"DOTPRODUCT3", 24}, {"MULTIPLYADD", 25}, {"LERP", 26}, {nullptr, 0}};
const Named texture_arguments[] = {{"DIFFUSE", 0}, {"CURRENT", 1}, {"TEXTURE", 2}, {"TFACTOR", 3}, {"SPECULAR", 4}, {"TEMP", 5},
	{"CONSTANT", 6}, {"COMPLEMENT", 0x10}, {"ALPHAREPLICATE", 0x20}, {nullptr, 0}};
const Named transform_flags[] = {{"DISABLE", 0}, {"COUNT1", 1}, {"COUNT2", 2}, {"COUNT3", 3}, {"COUNT4", 4}, {"PROJECTED", 256}, {nullptr, 0}};
const Named light_types[] = {{"POINT", 1}, {"SPOT", 2}, {"DIRECTIONAL", 3}, {nullptr, 0}};
const Named addresses[] = {{"WRAP", 1}, {"MIRROR", 2}, {"CLAMP", 3}, {"BORDER", 4}, {"MIRRORONCE", 5}, {nullptr, 0}};
const Named filters[] = {{"NONE", 0}, {"POINT", 1}, {"LINEAR", 2}, {"ANISOTROPIC", 3}, {"PYRAMIDALQUAD", 6}, {"GAUSSIANQUAD", 7},
	{nullptr, 0}};

// What a state takes.
enum class Value : uint8_t { boolean, integer, floating, color, floats, shader_vertex, shader_pixel, texture, sampler };

struct State {
	const char *name;
	Value value;
	const Named *names;
	// Values of floats: 0 for any count.
	uint32_t count = 0;
};

// Every state a Direct3D 9 effect can assign, at the index the effect runtime knows it by: render states, texture stage states, the
// fixed function transforms, material and lights, shaders and their constants, then the texture and sampler states.
const State states[] = {
	{"ZEnable", Value::integer, depth_buffers}, {"FillMode", Value::integer, fills}, {"ShadeMode", Value::integer, shades},
	{"ZWriteEnable", Value::boolean, no_values}, {"AlphaTestEnable", Value::boolean, no_values}, {"LastPixel", Value::boolean, no_values},
	{"SrcBlend", Value::integer, blends}, {"DestBlend", Value::integer, blends}, {"CullMode", Value::integer, culls},
	{"ZFunc", Value::integer, comparisons}, {"AlphaRef", Value::integer, no_values}, {"AlphaFunc", Value::integer, comparisons},
	{"DitherEnable", Value::boolean, no_values}, {"AlphaBlendEnable", Value::boolean, no_values}, {"FogEnable", Value::boolean, no_values},
	{"SpecularEnable", Value::boolean, no_values}, {"FogColor", Value::color, no_values}, {"FogTableMode", Value::integer, fogs},
	{"FogStart", Value::floating, no_values}, {"FogEnd", Value::floating, no_values}, {"FogDensity", Value::floating, no_values},
	{"RangeFogEnable", Value::boolean, no_values}, {"StencilEnable", Value::boolean, no_values}, {"StencilFail", Value::integer, stencil_ops},
	{"StencilZFail", Value::integer, stencil_ops}, {"StencilPass", Value::integer, stencil_ops}, {"StencilFunc", Value::integer, comparisons},
	{"StencilRef", Value::integer, no_values}, {"StencilMask", Value::integer, no_values}, {"StencilWriteMask", Value::integer, no_values},
	{"TextureFactor", Value::color, no_values}, {"Wrap0", Value::integer, wraps}, {"Wrap1", Value::integer, wraps},
	{"Wrap2", Value::integer, wraps}, {"Wrap3", Value::integer, wraps}, {"Wrap4", Value::integer, wraps}, {"Wrap5", Value::integer, wraps},
	{"Wrap6", Value::integer, wraps}, {"Wrap7", Value::integer, wraps}, {"Wrap8", Value::integer, wraps}, {"Wrap9", Value::integer, wraps},
	{"Wrap10", Value::integer, wraps}, {"Wrap11", Value::integer, wraps}, {"Wrap12", Value::integer, wraps},
	{"Wrap13", Value::integer, wraps}, {"Wrap14", Value::integer, wraps}, {"Wrap15", Value::integer, wraps},
	{"Clipping", Value::boolean, no_values}, {"Lighting", Value::boolean, no_values}, {"Ambient", Value::color, no_values},
	{"FogVertexMode", Value::integer, fogs}, {"ColorVertex", Value::boolean, no_values}, {"LocalViewer", Value::boolean, no_values},
	{"NormalizeNormals", Value::boolean, no_values}, {"DiffuseMaterialSource", Value::integer, material_sources},
	{"SpecularMaterialSource", Value::integer, material_sources}, {"AmbientMaterialSource", Value::integer, material_sources},
	{"EmissiveMaterialSource", Value::integer, material_sources}, {"VertexBlend", Value::integer, vertex_blends},
	{"ClipPlaneEnable", Value::integer, no_values}, {"PointSize", Value::floating, no_values}, {"PointSize_Min", Value::floating, no_values},
	{"PointSize_Max", Value::floating, no_values}, {"PointSpriteEnable", Value::boolean, no_values},
	{"PointScaleEnable", Value::boolean, no_values}, {"PointScale_A", Value::floating, no_values}, {"PointScale_B", Value::floating, no_values},
	{"PointScale_C", Value::floating, no_values}, {"MultiSampleAntialias", Value::boolean, no_values},
	{"MultiSampleMask", Value::integer, no_values}, {"PatchEdgeStyle", Value::integer, patch_edges},
	{"DebugMonitorToken", Value::integer, no_values}, {"IndexedVertexBlendEnable", Value::boolean, no_values},
	{"ColorWriteEnable", Value::integer, color_writes}, {"TweenFactor", Value::floating, no_values}, {"BlendOp", Value::integer, blend_ops},
	{"PositionDegree", Value::integer, degrees}, {"NormalDegree", Value::integer, degrees}, {"ScissorTestEnable", Value::boolean, no_values},
	{"SlopeScaleDepthBias", Value::floating, no_values}, {"AntialiasedLineEnable", Value::boolean, no_values},
	{"MinTessellationLevel", Value::floating, no_values}, {"MaxTessellationLevel", Value::floating, no_values},
	{"AdaptiveTess_X", Value::floating, no_values}, {"AdaptiveTess_Y", Value::floating, no_values},
	{"AdaptiveTess_Z", Value::floating, no_values}, {"AdaptiveTess_W", Value::floating, no_values},
	{"EnableAdaptiveTessellation", Value::boolean, no_values}, {"TwoSidedStencilMode", Value::boolean, no_values},
	{"CCW_StencilFail", Value::integer, stencil_ops}, {"CCW_StencilZFail", Value::integer, stencil_ops},
	{"CCW_StencilPass", Value::integer, stencil_ops}, {"CCW_StencilFunc", Value::integer, comparisons},
	{"ColorWriteEnable1", Value::integer, color_writes}, {"ColorWriteEnable2", Value::integer, color_writes},
	{"ColorWriteEnable3", Value::integer, color_writes}, {"BlendFactor", Value::color, no_values},
	{"SRGBWriteEnable", Value::boolean, no_values}, {"DepthBias", Value::floating, no_values},
	{"SeparateAlphaBlendEnable", Value::boolean, no_values}, {"SrcBlendAlpha", Value::integer, blends},
	{"DestBlendAlpha", Value::integer, blends}, {"BlendOpAlpha", Value::integer, blend_ops},
	{"ColorOp", Value::integer, texture_ops}, {"ColorArg0", Value::integer, texture_arguments},
	{"ColorArg1", Value::integer, texture_arguments}, {"ColorArg2", Value::integer, texture_arguments}, {"AlphaOp", Value::integer, texture_ops},
	{"AlphaArg0", Value::integer, texture_arguments}, {"AlphaArg1", Value::integer, texture_arguments},
	{"AlphaArg2", Value::integer, texture_arguments}, {"ResultArg", Value::integer, texture_arguments},
	{"BumpEnvMat00", Value::floating, no_values}, {"BumpEnvMat01", Value::floating, no_values}, {"BumpEnvMat10", Value::floating, no_values},
	{"BumpEnvMat11", Value::floating, no_values}, {"TexCoordIndex", Value::integer, no_values}, {"BumpEnvLScale", Value::floating, no_values},
	{"BumpEnvLOffset", Value::floating, no_values}, {"TextureTransformFlags", Value::integer, transform_flags},
	{"Constant", Value::color, no_values}, {"NPatchMode", Value::floating, no_values}, {"FVF", Value::integer, no_values},
	{"ProjectionTransform", Value::floats, no_values, 16}, {"ViewTransform", Value::floats, no_values, 16},
	{"WorldTransform", Value::floats, no_values, 16}, {"TextureTransform", Value::floats, no_values, 16},
	{"MaterialDiffuse", Value::floats, no_values, 4}, {"MaterialAmbient", Value::floats, no_values, 4},
	{"MaterialSpecular", Value::floats, no_values, 4}, {"MaterialEmissive", Value::floats, no_values, 4},
	{"MaterialPower", Value::floating, no_values}, {"LightType", Value::integer, light_types}, {"LightDiffuse", Value::floats, no_values, 4},
	{"LightSpecular", Value::floats, no_values, 4}, {"LightAmbient", Value::floats, no_values, 4},
	{"LightPosition", Value::floats, no_values, 3}, {"LightDirection", Value::floats, no_values, 3}, {"LightRange", Value::floating, no_values},
	{"LightFalloff", Value::floating, no_values}, {"LightAttenuation0", Value::floating, no_values},
	{"LightAttenuation1", Value::floating, no_values}, {"LightAttenuation2", Value::floating, no_values},
	{"LightTheta", Value::floating, no_values}, {"LightPhi", Value::floating, no_values}, {"LightEnable", Value::boolean, no_values},
	{"VertexShader", Value::shader_vertex, no_values}, {"PixelShader", Value::shader_pixel, no_values},
	{"VertexShaderConstantF", Value::floats, no_values}, {"VertexShaderConstantB", Value::floats, no_values},
	{"VertexShaderConstantI", Value::floats, no_values}, {"VertexShaderConstant", Value::floats, no_values},
	{"VertexShaderConstant1", Value::floats, no_values}, {"VertexShaderConstant2", Value::floats, no_values},
	{"VertexShaderConstant3", Value::floats, no_values}, {"VertexShaderConstant4", Value::floats, no_values},
	{"PixelShaderConstantF", Value::floats, no_values}, {"PixelShaderConstantB", Value::floats, no_values},
	{"PixelShaderConstantI", Value::floats, no_values}, {"PixelShaderConstant", Value::floats, no_values},
	{"PixelShaderConstant1", Value::floats, no_values}, {"PixelShaderConstant2", Value::floats, no_values},
	{"PixelShaderConstant3", Value::floats, no_values}, {"PixelShaderConstant4", Value::floats, no_values},
	{"Texture", Value::texture, no_values}, {"AddressU", Value::integer, addresses}, {"AddressV", Value::integer, addresses},
	{"AddressW", Value::integer, addresses}, {"BorderColor", Value::color, no_values}, {"MagFilter", Value::integer, filters},
	{"MinFilter", Value::integer, filters}, {"MipFilter", Value::integer, filters}, {"MipMapLodBias", Value::floating, no_values},
	{"MaxMipLevel", Value::integer, no_values}, {"MaxAnisotropy", Value::integer, no_values}, {"SRGBTexture", Value::boolean, no_values},
	{"ElementIndex", Value::integer, no_values}, {"DMAPOffset", Value::integer, no_values}, {"Sampler", Value::sampler, no_values},
};

constexpr uint32_t first_sampler_state = 164;

std::optional<uint32_t> state_index(const std::string &name)
{
	const std::string wanted = folded(name);
	for (uint32_t i = 0; i < std::size(states); ++i)
		if (folded(states[i].name) == wanted)
			return i;
	return std::nullopt;
}

class Effect9 {
public:
	Effect9(const Program &program, uint32_t flags, const EffectShaderCompiler &compile) : program(program), flags(flags), compile(compile)
	{
		// Offset 0 holds an empty name, for every name and semantic an effect leaves out.
		data.push_back(0);
	}

	std::vector<uint8_t> build()
	{
		if (!program.groups.empty())
			fail(program.groups[0].line, "groups need fx_5_0");
		std::vector<const Variable *> parameters;
		for (const auto &variable : program.globals)
			if (!variable.is_static)
				parameters.push_back(&variable);
		for (const auto &buffer : program.buffers)
			for (const auto &member : buffer.members)
				parameters.push_back(&member);
		for (size_t i = 0; i < parameters.size(); ++i)
			parameter_index[parameters[i]->name] = uint32_t(i);
		uint32_t shaders = 0;
		std::vector<uint32_t> body;
		for (const auto *variable : parameters) {
			if (variable->type.shape == Type::Shape::object &&
				(variable->type.object == ObjectKind::vertex_shader || variable->type.object == ObjectKind::pixel_shader))
				++shaders;
			parameter(*variable, body);
		}
		for (uint32_t t = 0; t < program.techniques.size(); ++t) {
			const auto &technique = program.techniques[t];
			if (technique.version == 11)
				fail(technique.line, "technique11 needs fx_5_0");
			body.push_back(name(technique.name));
			body.push_back(uint32_t(technique.annotations.size()));
			body.push_back(uint32_t(technique.passes.size()));
			annotations(technique.annotations, body);
			for (uint32_t p = 0; p < technique.passes.size(); ++p) {
				const auto &pass = technique.passes[p];
				++shaders;
				body.push_back(name(pass.name, true));
				body.push_back(uint32_t(pass.annotations.size()));
				body.push_back(uint32_t(pass.states.size()));
				annotations(pass.annotations, body);
				for (uint32_t s = 0; s < pass.states.size(); ++s)
					state(pass.states[s], {t, p, 0, s}, body);
			}
		}
		std::vector<uint32_t> words{uint32_t(parameters.size()), uint32_t(program.techniques.size()), shaders, next_object};
		words.insert(words.end(), body.begin(), body.end());
		words.push_back(uint32_t(objects.size()));
		words.push_back(uint32_t(resources.size()));
		for (const auto &[id, bytes] : objects) {
			words.push_back(id);
			sized(bytes, words);
		}
		for (const auto &resource : resources) {
			words.insert(words.end(), resource.where.begin(), resource.where.end());
			words.push_back(resource.usage);
			sized(resource.bytes, words);
		}
		std::vector<uint8_t> out(8 + (data.size() + words.size()) * 4);
		const uint32_t header[2] = {0xfeff0901, uint32_t(data.size() * 4)};
		std::memcpy(out.data(), header, 8);
		std::memcpy(out.data() + 8, data.data(), data.size() * 4);
		std::memcpy(out.data() + 8 + data.size() * 4, words.data(), words.size() * 4);
		return out;
	}

private:
	const Program &program;
	uint32_t flags;
	const EffectShaderCompiler &compile;
	// The unstructured block, addressed by byte offsets from its start.
	std::vector<uint32_t> data;
	std::map<std::string, uint32_t> parameter_index;
	// Object 0 stands for none.
	uint32_t next_object = 1;
	// Strings and shader variables' programs, by object.
	std::vector<std::pair<uint32_t, std::vector<uint8_t>>> objects;
	struct Resource {
		// Technique (or ~0 for a sampler parameter), pass (or parameter), element and state.
		std::array<uint32_t, 4> where;
		// 0 a shader compiled for the state, 1 the name of the parameter the state takes.
		uint32_t usage;
		std::vector<uint8_t> bytes;
	};
	std::vector<Resource> resources;

	static void sized(const std::vector<uint8_t> &bytes, std::vector<uint32_t> &words)
	{
		words.push_back(uint32_t(bytes.size()));
		const size_t at = words.size();
		words.resize(at + (bytes.size() + 3) / 4);
		std::memcpy(words.data() + at, bytes.data(), bytes.size());
	}

	uint32_t place(const std::vector<uint32_t> &words)
	{
		const uint32_t offset = uint32_t(data.size() * 4);
		data.insert(data.end(), words.begin(), words.end());
		return offset;
	}

	// A name or semantic: its length with the terminator, then its bytes; offset 0 for none, except a pass, whose name is read even
	// when it has none.
	uint32_t name(const std::string &text, bool always = false)
	{
		if (text.empty() && !always)
			return 0;
		std::vector<uint8_t> bytes(text.begin(), text.end());
		bytes.push_back(0);
		std::vector<uint32_t> words;
		sized(bytes, words);
		return place(words);
	}

	static std::vector<uint8_t> terminated(const std::string &text)
	{
		std::vector<uint8_t> bytes(text.begin(), text.end());
		bytes.push_back(0);
		return bytes;
	}

	uint32_t object_parameter_type(const Variable &variable) const
	{
		const std::string written = folded(variable.written_type);
		switch (variable.type.object) {
		case ObjectKind::string: return type_string;
		case ObjectKind::vertex_shader: return type_vertex_shader;
		case ObjectKind::pixel_shader: return type_pixel_shader;
		case ObjectKind::sampler:
			return written == "SAMPLER1D" ? type_sampler1d : written == "SAMPLER2D" ? type_sampler2d : written == "SAMPLER3D" ? type_sampler3d :
				written == "SAMPLERCUBE" ? type_sampler_cube : type_sampler;
		case ObjectKind::texture1d: return type_texture1d;
		case ObjectKind::texture2d: return written == "TEXTURE" ? type_texture : type_texture2d;
		case ObjectKind::texture3d: return type_texture3d;
		case ObjectKind::texture_cube: return type_texture_cube;
		default: fail(variable.line, "'" + variable.name + "' is not an object a Direct3D 9 effect holds");
		}
	}

	// The words of a type description, D3DXPARAMETER_CLASS first in the numeric forms; names are written out first.
	std::vector<uint32_t> type_words(const ReflectedType &type, const std::string &variable_name, const std::string &semantic, uint32_t elements)
	{
		std::vector<std::vector<uint32_t>> members;
		for (const auto &[member_name, member] : type.members)
			members.push_back(type_words(member, member_name, "", member.elements));
		const uint32_t parameter_type = type.type_class == class_struct ? type_void : type.type == 1 ? type_bool : type.type == 2 || type.type == 19 ?
			type_int : type_float;
		std::vector<uint32_t> words{parameter_type, type.type_class, name(variable_name), name(semantic), elements};
		if (type.type_class == class_vector)
			words.insert(words.end(), {type.columns, type.rows});
		else if (type.type_class == class_struct) {
			words.push_back(uint32_t(members.size()));
			for (const auto &member : members)
				words.insert(words.end(), member.begin(), member.end());
		} else
			words.insert(words.end(), {type.rows, type.columns});
		return words;
	}

	static uint32_t element_total(const Type &type)
	{
		uint32_t count = 1;
		for (const auto size : type.array)
			count *= size;
		return type.array.empty() ? 0 : count;
	}

	// The scalars of a numeric value in the order the effect stores them: a column major matrix by columns.
	std::vector<uint32_t> numeric_value(const Type &type, const Expr *initializer)
	{
		const ReflectedType reflected = effect_type(program, type, flags);
		const uint32_t total = std::max<uint32_t>(1, element_total(type)) * flat_scalars(reflected);
		std::vector<uint32_t> values(total);
		if (!initializer)
			return values;
		const auto scalars = effect_constant(program, *initializer, type, flags);
		for (size_t i = 0; i < scalars.size() && i < values.size(); ++i)
			values[i] = scalars[i].second == Scalar::boolean ? (scalars[i].first ? 1 : 0) : scalars[i].first;
		if (reflected.type_class == class_matrix_columns) {
			const uint32_t rows = reflected.rows, columns = reflected.columns, per = rows * columns;
			for (uint32_t base = 0; base + per <= values.size(); base += per) {
				std::vector<uint32_t> moved(per);
				for (uint32_t r = 0; r < rows; ++r)
					for (uint32_t c = 0; c < columns; ++c)
						moved[c * rows + r] = values[base + r * columns + c];
				std::copy(moved.begin(), moved.end(), values.begin() + base);
			}
		}
		return values;
	}

	static uint32_t flat_scalars(const ReflectedType &type)
	{
		if (type.type_class == class_struct) {
			uint32_t total = 0;
			for (const auto &[member_name, member] : type.members)
				total += std::max<uint32_t>(1, member.elements) * flat_scalars(member);
			return total;
		}
		return type.rows * type.columns;
	}

	// A variable's type and value: numbers as they start, an object by its object number, a sampler by its states.
	std::pair<uint32_t, uint32_t> described(const Variable &variable, std::optional<uint32_t> parameter)
	{
		const uint32_t elements = element_total(variable.type);
		if (variable.type.shape != Type::Shape::object) {
			const auto words = type_words(effect_type(program, variable.type, flags), variable.name, variable.semantic, elements);
			const uint32_t type_offset = place(words);
			return {type_offset, place(numeric_value(variable.type, variable.initializer.get()))};
		}
		const uint32_t parameter_type = object_parameter_type(variable);
		const uint32_t type_offset = place({parameter_type, class_object, name(variable.name), name(variable.semantic), elements});
		const uint32_t count = std::max<uint32_t>(1, elements);
		if (parameter_type >= type_sampler && parameter_type <= type_sampler_cube) {
			if (!variable.element_states.empty() && variable.element_states.size() != count)
				fail(variable.line, "'" + variable.name + "' needs " + std::to_string(count) + " sampler states");
			std::vector<uint32_t> words;
			for (uint32_t e = 0; e < count; ++e) {
				const auto &list = variable.element_states.empty() ? variable.states : variable.element_states[e];
				std::vector<uint32_t> block{uint32_t(list.size())};
				for (uint32_t s = 0; s < list.size(); ++s) {
					const auto index = state_index(list[s].name);
					if (!index || (*index < first_sampler_state || *index == std::size(states) - 1))
						fail(list[s].line, "unknown sampler state '" + list[s].name + "'");
					state(list[s], {~0u, parameter.value_or(0), elements ? e : ~0u, s}, block);
				}
				words.insert(words.end(), block.begin(), block.end());
			}
			return {type_offset, place(words)};
		}
		std::vector<const Expr *> values;
		if (variable.initializer && variable.initializer->kind == Expr::Kind::initializer_list)
			for (const auto &item : variable.initializer->operands)
				values.push_back(item.get());
		else
			values.push_back(variable.initializer.get());
		if (elements && values.size() == 1 && !values[0])
			values.assign(count, nullptr);
		if (values.size() != count)
			fail(variable.line, "'" + variable.name + "' needs " + std::to_string(count) + " values");
		std::vector<uint32_t> ids;
		for (const auto *value : values) {
			const uint32_t id = next_object++;
			ids.push_back(id);
			if (parameter_type == type_string) {
				if (value && value->kind != Expr::Kind::string)
					fail(variable.line, "'" + variable.name + "' takes a string");
				objects.emplace_back(id, terminated(value ? value->text : ""));
			} else if (parameter_type == type_vertex_shader || parameter_type == type_pixel_shader) {
				if (value && !(value->kind == Expr::Kind::name && folded(value->text) == "NULL"))
					objects.emplace_back(id, shader(*value, parameter_type, variable.line));
			}
		}
		return {type_offset, place(ids)};
	}

	void parameter(const Variable &variable, std::vector<uint32_t> &body)
	{
		const auto [type_offset, value_offset] = described(variable, parameter_index.at(variable.name));
		body.push_back(type_offset);
		body.push_back(value_offset);
		body.push_back(std::find(variable.modifiers.begin(), variable.modifiers.end(), "shared") != variable.modifiers.end() ? 1 : 0);
		body.push_back(uint32_t(variable.annotations.size()));
		annotations(variable.annotations, body);
	}

	void annotations(const std::vector<Variable> &list, std::vector<uint32_t> &body)
	{
		for (const auto &annotation : list) {
			const auto [type_offset, value_offset] = described(annotation, std::nullopt);
			body.push_back(type_offset);
			body.push_back(value_offset);
		}
	}

	// A program from compile vs_2_0 f(...); shader model 1 targets are built for shader model 2, which every device that runs them takes.
	std::vector<uint8_t> shader(const Expr &value, uint32_t parameter_type, uint32_t line)
	{
		if (value.kind != Expr::Kind::call || value.text != "CompileShader" || value.operands.size() != 2 ||
			value.operands[0]->kind != Expr::Kind::name || value.operands[1]->kind != Expr::Kind::call)
			fail(line, "a shader is set with compile target entry()");
		std::string target = value.operands[0]->text;
		for (auto &c : target)
			c = char(std::tolower(uint8_t(c)));
		const bool vertex = target.starts_with("vs_");
		if ((parameter_type == type_vertex_shader) != vertex || !(vertex || target.starts_with("ps_")))
			fail(line, "'" + target + "' does not build a " + std::string(parameter_type == type_vertex_shader ? "vertex" : "pixel") + " shader");
		if (target.size() >= 4 && target[3] == '1')
			target = target.substr(0, 3) + "2_0";
		const auto &call = *value.operands[1];
		std::vector<const Expr *> uniforms;
		for (const auto &argument : call.operands)
			uniforms.push_back(argument.get());
		return compile(call.text, target, uniforms, line);
	}

	// The number a value names: TRUE, a value of the state such as InvSrcAlpha, or several joined with |.
	std::optional<uint32_t> named(const Expr &value, const Named *names) const
	{
		if (value.kind == Expr::Kind::binary && value.text == "|") {
			const auto left = named(*value.operands[0], names), right = named(*value.operands[1], names);
			if (left && right)
				return *left | *right;
			return std::nullopt;
		}
		if (value.kind != Expr::Kind::name)
			return std::nullopt;
		std::string wanted = folded(value.text);
		// D3DBLEND_SRCALPHA and SRCALPHA name the same value.
		if (wanted.starts_with("D3D"))
			for (const Named *item = names; item->name; ++item)
				if (wanted.ends_with(folded(item->name)) && wanted.size() > std::strlen(item->name))
					wanted = folded(item->name);
		for (const Named *item = names; item->name; ++item)
			if (folded(item->name) == wanted)
				return item->value;
		if (wanted == "TRUE")
			return 1;
		if (wanted == "FALSE")
			return 0;
		return std::nullopt;
	}

	// The parameter a state value names: <name>, (name) or name.
	const Variable *referenced(const Expr &value) const
	{
		if (value.kind != Expr::Kind::name)
			return nullptr;
		const auto found = parameter_index.find(value.text);
		if (found == parameter_index.end())
			return nullptr;
		for (const auto &variable : program.globals)
			if (variable.name == value.text)
				return &variable;
		for (const auto &buffer : program.buffers)
			for (const auto &member : buffer.members)
				if (member.name == value.text)
					return &member;
		return nullptr;
	}

	void state(const StateAssignment &assignment, std::array<uint32_t, 4> where, std::vector<uint32_t> &body)
	{
		const uint32_t line = assignment.line;
		const auto found = state_index(assignment.name);
		if (!found)
			fail(line, "unknown state '" + assignment.name + "'");
		const State &kind = states[*found];
		const Expr &value = *assignment.value;
		body.push_back(*found);
		body.push_back(assignment.index.value_or(0));
		const auto object_state = [&](uint32_t parameter_type) {
			body.push_back(place({parameter_type, class_object, 0, 0, 0}));
			const uint32_t id = next_object++;
			body.push_back(place({id}));
			if (value.kind == Expr::Kind::name && folded(value.text) == "NULL")
				return;
			if (const Variable *source = referenced(value)) {
				resources.push_back({where, 1, terminated(source->name)});
				return;
			}
			if (parameter_type == type_vertex_shader || parameter_type == type_pixel_shader) {
				resources.push_back({where, 0, shader(value, parameter_type, line)});
				return;
			}
			fail(line, "state '" + assignment.name + "' takes a parameter");
		};
		switch (kind.value) {
		case Value::shader_vertex: object_state(type_vertex_shader); return;
		case Value::shader_pixel: object_state(type_pixel_shader); return;
		case Value::texture: object_state(type_texture); return;
		case Value::sampler: {
			// A pass sets a sampler parameter's states on a sampler: the parameter is named, and its own states stay with it.
			const Variable *source = referenced(value);
			if (!source || source->type.object != ObjectKind::sampler)
				fail(line, "state 'Sampler' takes a sampler parameter");
			body.push_back(place({type_sampler, class_object, 0, 0, 0}));
			body.push_back(place({0}));
			resources.push_back({where, 1, terminated(source->name)});
			return;
		}
		default: break;
		}
		// A numeric parameter named as the value is read when the pass runs.
		if (const Variable *source = referenced(value); source && source->type.shape != Type::Shape::object && !named(value, kind.names)) {
			const auto words = type_words(effect_type(program, source->type, flags), "", "", element_total(source->type));
			body.push_back(place(words));
			body.push_back(place(numeric_value(source->type, nullptr)));
			resources.push_back({where, 1, terminated(source->name)});
			return;
		}
		uint32_t parameter_type = type_int, columns = 1;
		std::vector<uint32_t> values;
		if (const auto number = named(value, kind.names); number && kind.value != Value::floats && kind.value != Value::floating) {
			parameter_type = kind.value == Value::boolean ? type_bool : type_int;
			values = {*number};
		} else {
			const auto scalars = effect_constant(program, value, Type{}, flags);
			const auto as_float = [](const std::pair<uint32_t, Scalar> &scalar) {
				if (scalar.second == Scalar::floating)
					return std::bit_cast<float>(scalar.first);
				if (scalar.second == Scalar::boolean)
					return scalar.first ? 1.0f : 0.0f;
				return scalar.second == Scalar::unsigned_integer ? float(scalar.first) : float(int32_t(scalar.first));
			};
			switch (kind.value) {
			case Value::boolean:
			case Value::integer:
				if (scalars.size() != 1)
					fail(line, "state '" + assignment.name + "' takes one value");
				parameter_type = kind.value == Value::boolean ? type_bool : type_int;
				values = {scalars[0].second == Scalar::floating ? uint32_t(int32_t(as_float(scalars[0]))) :
					scalars[0].second == Scalar::boolean ? (scalars[0].first ? 1u : 0u) : scalars[0].first};
				break;
			case Value::floating:
				if (scalars.size() != 1)
					fail(line, "state '" + assignment.name + "' takes one value");
				parameter_type = type_float;
				values = {std::bit_cast<uint32_t>(as_float(scalars[0]))};
				break;
			case Value::color: {
				// A colour is packed as Direct3D 9 takes it, A8R8G8B8; a single number is taken as already packed.
				if (scalars.size() == 1) {
					values = {scalars[0].second == Scalar::floating ? uint32_t(as_float(scalars[0])) : scalars[0].first};
					break;
				}
				if (scalars.size() != 3 && scalars.size() != 4)
					fail(line, "state '" + assignment.name + "' takes a colour");
				const auto channel = [&](size_t i) {
					const float v = i < scalars.size() ? as_float(scalars[i]) : 1.0f;
					return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
				};
				values = {channel(3) << 24 | channel(0) << 16 | channel(1) << 8 | channel(2)};
				break;
			}
			default:
				if (kind.count && scalars.size() != kind.count)
					fail(line, "state '" + assignment.name + "' takes " + std::to_string(kind.count) + " values");
				parameter_type = type_float;
				for (const auto &scalar : scalars)
					values.push_back(std::bit_cast<uint32_t>(as_float(scalar)));
				columns = uint32_t(values.size());
				break;
			}
		}
		if (columns == 16)
			body.push_back(place({parameter_type, class_matrix_rows, 0, 0, 0, 4, 4}));
		else if (columns > 1)
			body.push_back(place({parameter_type, class_vector, 0, 0, 0, columns, 1}));
		else
			body.push_back(place({parameter_type, class_scalar, 0, 0, 0, 1, 1}));
		body.push_back(place(values));
	}
};

}

std::vector<uint8_t> effect9_binary(const Program &program, uint32_t flags, const EffectShaderCompiler &compile)
{
	return Effect9(program, flags, compile).build();
}

}
