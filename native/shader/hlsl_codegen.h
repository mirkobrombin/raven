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
	// The constant buffer a cbuffer binding stands for.
	int buffer = -1;
};

struct Generated {
	Stage stage = Stage::pixel;
	uint32_t major = 4, minor = 0;
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

// The program of the entry point for a target such as "ps_4_0" or "cs_5_0", or a GenerateError.
Generated generate(const Program &program, const std::string &entry, const std::string &target, uint32_t flags);

}
