#pragma once

#include "hlsl_lexer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace raven::shader {

// D3DCOMPILE flags that change the code: row-major matrices by default, strictness, and skipping optimization or validation.
enum CompileFlags : uint32_t {
	compile_debug = 1 << 0, compile_skip_validation = 1 << 1, compile_skip_optimization = 1 << 2, compile_pack_row_major = 1 << 3,
	compile_pack_column_major = 1 << 4, compile_avoid_flow_control = 1 << 9, compile_prefer_flow_control = 1 << 10,
	compile_enable_strictness = 1 << 11, compile_backwards_compatibility = 1 << 12
};

struct CompileRequest {
	std::string source, name, entry, target;
	std::vector<Macro> macros;
	IncludeReader include;
	uint32_t flags = 0;
};

struct CompileResult {
	// The DXBC container; empty when the compile failed.
	std::vector<uint8_t> code;
	// Errors and warnings in the compiler's "name(line,column): error Xnnnn: message" form, one per line.
	std::string messages;
	bool failed = false;
};

CompileResult compile(const CompileRequest &request);

// The source after preprocessing, as text, in `code`.
CompileResult preprocess_text(const CompileRequest &request);

}
