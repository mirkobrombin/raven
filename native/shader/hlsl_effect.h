#pragma once

#include "hlsl_ast.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace raven::shader {

// Compiles one shader an effect names, CompileShader(target, entry(uniform values...)), into its bytecode; throws a GenerateError.
using EffectShaderCompiler =
	std::function<std::vector<uint8_t>(const std::string &entry, const std::string &target, const std::vector<const Expr *> &uniforms, uint32_t line)>;

// The binary of an effect for fx_4_0, fx_4_1 or fx_5_0, the layout the Direct3D 10 and 11 effect runtimes load: its constant buffers
// and variables with their starting values, its objects and state blocks, and its groups, techniques and passes with the shaders they
// compile. Throws a GenerateError.
std::vector<uint8_t> effect_binary(const Program &program, const std::string &target, uint32_t flags, const EffectShaderCompiler &compile);

}
