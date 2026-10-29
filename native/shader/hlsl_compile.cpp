#include "hlsl_compile.h"

#include "dxbc.h"
#include "hlsl_codegen.h"
#include "hlsl_parser.h"

#include <functional>
#include <map>

namespace raven::shader {

namespace {

// Strings written after the fixed part of a chunk, each once, at offsets from the chunk start.
class Strings {
public:
	explicit Strings(size_t start) : at(start) {}

	uint32_t add(const std::string &text)
	{
		if (auto found = offsets.find(text); found != offsets.end())
			return found->second;
		const uint32_t offset = uint32_t(at);
		offsets[text] = offset;
		order.push_back(text);
		at += text.size() + 1;
		return offset;
	}

	void write(Writer &out) const
	{
		for (const auto &text : order)
			out.text(text);
		out.align(4);
	}

private:
	size_t at;
	std::map<std::string, uint32_t> offsets;
	std::vector<std::string> order;
};

std::vector<uint8_t> signature(const std::vector<SignatureElement> &elements, bool streams)
{
	const size_t entry = streams ? 28 : 24;
	Strings names(8 + entry * elements.size());
	std::vector<uint32_t> name_offsets;
	for (const auto &element : elements)
		name_offsets.push_back(names.add(element.semantic));
	Writer out;
	out.u32(uint32_t(elements.size()));
	out.u32(8);
	for (size_t i = 0; i < elements.size(); ++i) {
		const auto &element = elements[i];
		if (streams)
			out.u32(element.stream);
		out.u32(name_offsets[i]);
		out.u32(element.semantic_index);
		out.u32(element.system_value);
		out.u32(element.component_type);
		out.u32(element.reg);
		out.u32(uint32_t(element.mask) | uint32_t(element.used) << 8);
	}
	names.write(out);
	return out.take();
}

uint32_t program_type(Stage stage)
{
	switch (stage) {
	case Stage::pixel: return 0xffff;
	case Stage::vertex: return 0xfffe;
	case Stage::geometry: return 0x4753;
	case Stage::hull: return 0x4853;
	case Stage::domain: return 0x4453;
	default: return 0x4353;
	}
}

// Resource definitions: constant buffers with their variables and types, the bound resources and the creator.
std::vector<uint8_t> resource_definitions(const Generated &generated, uint32_t flags)
{
	const bool sm5 = generated.major >= 5;
	const size_t header = sm5 ? 60 : 28, binding_size = 32, buffer_size = 24, variable_size = sm5 ? 40 : 24;
	// Fixed parts first: header, bindings, buffers, then every buffer's variables; types and strings follow.
	size_t at = header;
	const size_t bindings_at = at;
	at += binding_size * generated.bindings.size();
	const size_t buffers_at = at;
	at += buffer_size * generated.buffers.size();
	std::vector<size_t> variables_at;
	for (const auto &buffer : generated.buffers) {
		variables_at.push_back(at);
		at += variable_size * buffer.variables.size();
	}
	// Types are laid out depth first: a structure's members (12 bytes each) and their types come after it.
	Writer types;
	std::vector<std::pair<size_t, std::string>> type_names;
	std::function<uint32_t(const ReflectedType &)> put_type = [&](const ReflectedType &type) -> uint32_t {
		const size_t offset = at + types.size();
		types.u32(uint32_t(type.type_class) | uint32_t(type.type) << 16);
		types.u32(uint32_t(type.rows) | uint32_t(type.columns) << 16);
		types.u32(uint32_t(type.elements) | uint32_t(type.members.size()) << 16);
		const size_t members_field = types.size();
		types.u32(0);
		if (sm5) {
			for (int i = 0; i < 4; ++i)
				types.u32(0);
			type_names.emplace_back(types.size(), type.name);
			types.u32(0);
		}
		if (!type.members.empty()) {
			const size_t members_at = at + types.size();
			types.patch(members_field, uint32_t(members_at));
			std::vector<size_t> fields;
			for (size_t i = 0; i < type.members.size(); ++i) {
				type_names.emplace_back(types.size(), type.members[i].first);
				types.u32(0);
				fields.push_back(types.size());
				types.u32(0);
				types.u32(type.member_offsets[i]);
			}
			for (size_t i = 0; i < type.members.size(); ++i)
				types.patch(fields[i], put_type(type.members[i].second));
		}
		return uint32_t(offset);
	};
	std::vector<std::vector<uint32_t>> type_offsets;
	for (const auto &buffer : generated.buffers) {
		type_offsets.emplace_back();
		for (const auto &variable : buffer.variables)
			type_offsets.back().push_back(put_type(variable.type));
	}
	at += types.size();
	Strings strings(at);
	const uint32_t creator = strings.add("Raven HLSL Compiler");
	for (auto &[field, name] : type_names)
		types.patch(field, strings.add(name));

	Writer out;
	out.u32(uint32_t(generated.buffers.size()));
	out.u32(generated.buffers.empty() ? 0 : uint32_t(buffers_at));
	out.u32(uint32_t(generated.bindings.size()));
	out.u32(generated.bindings.empty() ? 0 : uint32_t(bindings_at));
	out.u32(generated.minor | generated.major << 4 | program_type(generated.stage) << 16);
	out.u32(flags);
	out.u32(creator);
	if (sm5) {
		out.bytes(std::span(reinterpret_cast<const uint8_t *>("RD11"), 4));
		for (const uint32_t value : {60u, 24u, 32u, 40u, 36u, 12u, 0u})
			out.u32(value);
	}
	for (const auto &binding : generated.bindings) {
		out.u32(strings.add(binding.name));
		out.u32(binding.input_type);
		out.u32(binding.return_type);
		out.u32(binding.dimension);
		out.u32(binding.samples);
		out.u32(binding.bind_point);
		out.u32(binding.bind_count);
		out.u32(binding.flags);
	}
	for (size_t i = 0; i < generated.buffers.size(); ++i) {
		const auto &buffer = generated.buffers[i];
		out.u32(strings.add(buffer.name));
		out.u32(uint32_t(buffer.variables.size()));
		out.u32(uint32_t(variables_at[i]));
		out.u32(buffer.size);
		out.u32(0);
		out.u32(buffer.kind);
	}
	for (size_t i = 0; i < generated.buffers.size(); ++i)
		for (size_t k = 0; k < generated.buffers[i].variables.size(); ++k) {
			const auto &variable = generated.buffers[i].variables[k];
			out.u32(strings.add(variable.name));
			out.u32(variable.offset);
			out.u32(variable.size);
			out.u32(variable.used ? 2 : 0);
			out.u32(type_offsets[i][k]);
			out.u32(0);
			if (sm5) {
				out.u32(~0u);
				out.u32(0);
				out.u32(~0u);
				out.u32(0);
			}
		}
	auto type_bytes = types.take();
	out.bytes(type_bytes);
	strings.write(out);
	return out.take();
}

std::vector<uint8_t> statistics(const Generated &generated)
{
	Writer out;
	const size_t count = generated.major >= 5 ? 37 : 29;
	std::vector<uint32_t> values(count);
	values[0] = generated.instruction_count;
	values[1] = generated.temp_count;
	values[3] = uint32_t(generated.instructions.size() - generated.instruction_count);
	for (const auto value : values)
		out.u32(value);
	return out.take();
}

std::string message(const std::string &name, uint32_t line, const std::string &text)
{
	return (name.empty() ? std::string("memory") : name) + "(" + std::to_string(line) + ",1): error X3000: " + text + "\n";
}

}

CompileResult compile(const CompileRequest &request)
{
	CompileResult result;
	const auto preprocessed = preprocess(request.source, request.name, request.macros, request.include);
	if (preprocessed.failed) {
		result.messages = preprocessed.errors;
		result.failed = true;
		return result;
	}
	auto parsed = parse(preprocessed.tokens, request.name);
	if (parsed.failed) {
		result.messages = parsed.errors;
		result.failed = true;
		return result;
	}
	Generated generated;
	try {
		generated = generate(parsed.program, request.entry, request.target, request.flags);
	} catch (const GenerateError &error) {
		result.messages = message(request.name, error.line, error.message);
		result.failed = true;
		return result;
	}
	result.messages = generated.warnings;
	const uint32_t type = uint32_t(generated.stage);
	const auto program = sm4::encode(type << 16 | generated.major << 4 | generated.minor, generated.instructions);
	std::vector<uint8_t> tokens(program.size() * 4);
	for (size_t i = 0; i < program.size(); ++i)
		for (int b = 0; b < 4; ++b)
			tokens[i * 4 + size_t(b)] = uint8_t(program[i] >> (8 * b));
	const bool streams = generated.stage == Stage::geometry && generated.major >= 5;
	std::vector<Chunk> chunks;
	chunks.push_back({{'R', 'D', 'E', 'F'}, resource_definitions(generated, request.flags)});
	chunks.push_back({{'I', 'S', 'G', 'N'}, signature(generated.inputs, false)});
	chunks.push_back({streams ? std::array<char, 4>{'O', 'S', 'G', '5'} : std::array<char, 4>{'O', 'S', 'G', 'N'}, signature(generated.outputs, streams)});
	chunks.push_back({generated.major >= 5 ? std::array<char, 4>{'S', 'H', 'E', 'X'} : std::array<char, 4>{'S', 'H', 'D', 'R'}, std::move(tokens)});
	chunks.push_back({{'S', 'T', 'A', 'T'}, statistics(generated)});
	result.code = container(chunks);
	return result;
}

CompileResult preprocess_text(const CompileRequest &request)
{
	CompileResult result;
	const auto preprocessed = preprocess(request.source, request.name, request.macros, request.include);
	if (preprocessed.failed) {
		result.messages = preprocessed.errors;
		result.failed = true;
		return result;
	}
	std::string text;
	uint32_t line = preprocessed.tokens.empty() ? 0 : preprocessed.tokens[0].line;
	for (const auto &token : preprocessed.tokens) {
		if (token.kind == TokenKind::end)
			break;
		if (token.line != line) {
			text += '\n';
			line = token.line;
		} else if (!text.empty() && text.back() != '\n')
			text += ' ';
		text += token.text;
	}
	text += '\n';
	result.code.assign(text.begin(), text.end());
	return result;
}

}
