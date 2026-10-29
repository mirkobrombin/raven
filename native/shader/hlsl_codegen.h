#pragma once

#include "hlsl_ast.h"
#include "sm4.h"

#include <cstdint>
#include <string>
#include <vector>

namespace raven::shader {

enum class Stage : uint8_t { pixel = 0, vertex = 1, geometry = 2, hull = 3, domain = 4, compute = 5 };

// One element of an input or output signature.
struct SignatureElement {
	std::string semantic;
	uint32_t semantic_index = 0;
	// D3D_NAME: 0 for a plain semantic, 1 position, 6 vertex id, 64 render target, 65 depth...
	uint32_t system_value = 0;
	// D3D_REGISTER_COMPONENT_TYPE: 1 uint, 2 int, 3 float.
	uint32_t component_type = 3;
	uint32_t reg = 0;
	uint8_t mask = 0, used = 0;
	uint32_t stream = 0;
};

// What reflection reports about a variable of a constant buffer.
struct ReflectedType {
	uint16_t type_class = 0, type = 0, rows = 0, columns = 0, elements = 0;
	std::string name;
	std::vector<std::pair<std::string, ReflectedType>> members;
	std::vector<uint32_t> member_offsets;
	// Bytes the value takes in a constant buffer, and one element's, as effects describe types.
	uint32_t size = 0, element_size = 0;
};

struct ReflectedVariable {
	std::string name;
	uint32_t offset = 0, size = 0;
	bool used = false;
	ReflectedType type;
	std::vector<uint8_t> default_value;
};

struct ReflectedBuffer {
	std::string name;
	uint32_t size = 0;
	// D3D_CBUFFER_TYPE: 0 cbuffer, 1 tbuffer.
	uint32_t kind = 0;
	std::vector<ReflectedVariable> variables;
};

// A bound resource: D3D_SHADER_INPUT_TYPE, D3D_RESOURCE_RETURN_TYPE and D3D_SRV_DIMENSION as reflection reports them.
struct ReflectedBinding {
	std::string name;
	uint32_t input_type = 0, return_type = 0, dimension = 0, samples = 0, bind_point = 0, bind_count = 1, flags = 0, stride = 0;
	// Shader model 5.1: the register space and the range the shader names it by.
	uint32_t space = 0, range = 0;
	// The constant buffer a cbuffer binding stands for.
	int buffer = -1;
};

struct Generated {
	Stage stage = Stage::pixel;
	uint32_t major = 4, minor = 0;
	// As given: "ps_2_b" and "vs_2_a" differ from their _0 targets in what they allow.
	std::string target;
	std::vector<sm4::Instruction> instructions;
	std::vector<SignatureElement> inputs, outputs, patch_constants;
	std::vector<ReflectedBuffer> buffers;
	std::vector<ReflectedBinding> bindings;
	uint32_t instruction_count = 0, temp_count = 0;
	std::string warnings;
};

struct GenerateError {
	uint32_t line;
	std::string message;
};

// The program of the entry point for a target such as "ps_4_0" or "cs_5_0", or a GenerateError. An effect gives the uniform parameters
// of the entry point their values.
Generated generate(const Program &program, const std::string &entry, const std::string &target, uint32_t flags,
	const std::vector<const Expr *> &uniform_arguments = {});

// Every constant buffer with every variable and its starting value, as an effect lists them.
std::vector<ReflectedBuffer> effect_layout(const Program &program, uint32_t flags);

// A type as reflection describes it.
ReflectedType effect_type(const Program &program, const Type &type, uint32_t flags);

// The scalars of a constant expression in the order written, converted to a type unless it is void, with their kinds; a GenerateError
// when the expression is not constant.
std::vector<std::pair<uint32_t, Scalar>> effect_constant(const Program &program, const Expr &expr, const Type &type, uint32_t flags);

}
