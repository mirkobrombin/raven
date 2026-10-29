#pragma once

#include "hlsl_codegen.h"

#include <cstdint>
#include <vector>

namespace raven::shader {

// The float a shader model 2 or 3 program holds for the bits of a value of the given kind (an Operand's kinds entry: 0 unknown, else
// the Scalar plus one): those targets have no integers, and their booleans are 0 and 1.
float legacy_value(uint32_t bits, uint8_t kind);

// The D3D9 bytecode of a program generated for a vs_2_0 to ps_3_0 target, with its constant table; a GenerateError when the program
// uses what the target cannot express.
std::vector<uint32_t> legacy_program(const Generated &generated, uint32_t flags);

}
