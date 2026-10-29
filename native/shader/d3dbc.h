#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace raven::shader::d3dbc {

// Whether the tokens start like a shader model 1 to 3 program: a vs_ or ps_ version token.
bool is_program(const std::vector<uint32_t> &tokens);

// The program as assembly text, one instruction a line, with the constant table its comments carry listed first.
std::string disassemble(const std::vector<uint32_t> &tokens);

}
