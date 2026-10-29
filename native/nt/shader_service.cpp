#include "kernel_context.h"

#include "shader/dxbc.h"
#include "shader/hlsl_compile.h"
#include "shader/sm4.h"

#include <cstring>
#include <map>
#include <mutex>

namespace raven {

namespace {

// What a request produced, kept until the component has read it.
struct ShaderResult {
	// 0 done, 1 failed with messages, 2 an include file is needed first, 3 the input is not a shader or lacks the part asked for.
	uint32_t status = 0;
	std::vector<uint8_t> code;
	std::string messages;
	std::string include;
	uint32_t include_system = 0;
};

std::mutex shader_mutex;
std::map<uint32_t, ShaderResult> shader_results;
uint32_t next_shader_result = 1;

// Requests are words and length-prefixed byte strings, as raven-d3dcompiler writes them.
class Reader {
public:
	explicit Reader(const std::vector<uint8_t> &bytes) : bytes(bytes) {}

	uint32_t u32()
	{
		if (bytes.size() - at < 4)
			throw std::runtime_error("short shader request");
		uint32_t value;
		std::memcpy(&value, bytes.data() + at, 4);
		at += 4;
		return value;
	}

	std::string text()
	{
		const uint32_t size = u32();
		if (bytes.size() - at < size)
			throw std::runtime_error("short shader request");
		std::string value(reinterpret_cast<const char *>(bytes.data() + at), size);
		at += size;
		return value;
	}

private:
	const std::vector<uint8_t> &bytes;
	size_t at = 0;
};

enum class Mode : uint32_t { compile = 0, preprocess = 1, part = 2, strip = 3, set_part = 4, disassemble = 5 };

bool tag_is(const shader::Chunk &chunk, const char *tag) { return std::memcmp(chunk.tag.data(), tag, 4) == 0; }

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// D3D_BLOB_PART: the signatures come back as containers of their own, the other parts as their bytes.
void blob_part(const std::vector<shader::Chunk> &chunks, uint32_t part, ShaderResult &result)
{
	std::vector<std::vector<const char *>> wanted{{"ISGN", "ISG1"}, {"OSGN", "OSG5", "OSG1"}, {"ISGN", "ISG1", "OSGN", "OSG5", "OSG1"},
		{"PCSG", "PSG1"}, {"ISGN", "ISG1", "OSGN", "OSG5", "OSG1", "PCSG", "PSG1"}, {"SDBG"}, {"Aon9"}, {"XNAP"}, {"XNAS"}, {"SPDB"},
		{"PRIV"}, {"RTS0"}, {"ILDN"}};
	if (part >= wanted.size()) {
		result.status = 3;
		return;
	}
	std::vector<shader::Chunk> picked;
	for (const auto &chunk : chunks)
		for (const auto tag : wanted[part])
			if (tag_is(chunk, tag))
				picked.push_back(chunk);
	if (picked.empty()) {
		result.status = 3;
		return;
	}
	if (part <= 4)
		result.code = shader::container(picked);
	else
		result.code = picked[0].data;
}

ShaderResult serve(const std::vector<uint8_t> &bytes)
{
	ShaderResult result;
	Reader in(bytes);
	const auto mode = Mode(in.u32());
	if (mode == Mode::compile || mode == Mode::preprocess) {
		shader::CompileRequest request;
		request.flags = in.u32();
		request.source = in.text();
		request.name = in.text();
		request.entry = in.text();
		request.target = in.text();
		for (uint32_t count = in.u32(); count; --count) {
			shader::Macro macro;
			macro.name = in.text();
			macro.value = in.text();
			request.macros.push_back(macro);
		}
		std::map<std::string, std::string> files;
		for (uint32_t count = in.u32(); count; --count) {
			auto name = in.text();
			files[name] = in.text();
		}
		// An include the component has not read yet stops the compile; the component opens it and asks again with it attached.
		request.include = [&](const std::string &name, bool system) -> std::optional<std::string> {
			if (const auto found = files.find(name); found != files.end())
				return found->second;
			if (result.include.empty()) {
				result.include = name;
				result.include_system = system ? 1 : 0;
			}
			return std::nullopt;
		};
		if (mode == Mode::preprocess) {
			const auto text = shader::preprocess_text(request);
			if (!result.include.empty())
				result.status = 2;
			else if (text.failed) {
				result.status = 1;
				result.messages = text.messages;
			} else
				result.code = text.code;
			return result;
		}
		auto compiled = shader::compile(request);
		if (!result.include.empty()) {
			result.status = 2;
			return result;
		}
		result.status = compiled.failed ? 1 : 0;
		result.code = std::move(compiled.code);
		result.messages = std::move(compiled.messages);
		return result;
	}
	const auto blob = bytes_of(in.text());
	auto chunks = shader::read_container(blob);
	if (chunks.empty()) {
		result.status = 3;
		return result;
	}
	switch (mode) {
	case Mode::part:
		blob_part(chunks, in.u32(), result);
		break;
	case Mode::strip: {
		// D3DCOMPILER_STRIP_FLAGS: reflection 1, debug information 2, test blobs 4, private data 8, root signature 16.
		const uint32_t flags = in.u32();
		std::erase_if(chunks, [flags](const shader::Chunk &chunk) {
			return ((flags & 1) && (tag_is(chunk, "RDEF") || tag_is(chunk, "STAT"))) ||
				((flags & 2) && (tag_is(chunk, "SDBG") || tag_is(chunk, "SPDB") || tag_is(chunk, "ILDB") || tag_is(chunk, "ILDN"))) ||
				((flags & 8) && tag_is(chunk, "PRIV")) || ((flags & 16) && tag_is(chunk, "RTS0"));
		});
		result.code = shader::container(chunks);
		break;
	}
	case Mode::set_part: {
		// Only private data may be set, as on Windows.
		const uint32_t part = in.u32();
		const auto data = bytes_of(in.text());
		if (part != 10) {
			result.status = 3;
			break;
		}
		std::erase_if(chunks, [](const shader::Chunk &chunk) { return tag_is(chunk, "PRIV"); });
		chunks.push_back({{'P', 'R', 'I', 'V'}, data});
		result.code = shader::container(chunks);
		break;
	}
	case Mode::disassemble: {
		for (const auto &chunk : chunks)
			if (tag_is(chunk, "SHDR") || tag_is(chunk, "SHEX")) {
				std::vector<uint32_t> tokens(chunk.data.size() / 4);
				std::memcpy(tokens.data(), chunk.data.data(), tokens.size() * 4);
				result.code = bytes_of(shader::sm4::disassemble(tokens));
				return result;
			}
		result.status = 3;
		break;
	}
	default:
		result.status = 3;
		break;
	}
	return result;
}

}

void register_shader_service(ApiRegistry &registry, KernelContext &)
{
	// Runs one request and returns the handle its result is read through, or 0 when the request is malformed.
	registry.add("RavenShaderRequest", 2, [](ApiCall &call) {
		ShaderResult result;
		try {
			result = serve(call.read(call[0], call.u32(1)));
		} catch (const std::exception &) {
			call.ret(0);
			return;
		}
		std::lock_guard lock(shader_mutex);
		const uint32_t handle = next_shader_result++;
		shader_results[handle] = std::move(result);
		call.ret(handle);
	});
	// Part 0 is the code, 1 the messages, 2 the name of the include needed, 3 two words: the status and whether that include is a system one.
	// Returns the part's size and copies it when the buffer can hold it.
	registry.add("RavenShaderResult", 4, [](ApiCall &call) {
		std::lock_guard lock(shader_mutex);
		const auto found = shader_results.find(call.u32(0));
		if (found == shader_results.end()) {
			call.ret(0);
			return;
		}
		const auto &result = found->second;
		std::vector<uint8_t> part;
		switch (call.u32(1)) {
		case 0:
			part = result.code;
			break;
		case 1:
			part = bytes_of(result.messages);
			break;
		case 2:
			part = bytes_of(result.include);
			break;
		default:
			part.resize(8);
			std::memcpy(part.data(), &result.status, 4);
			std::memcpy(part.data() + 4, &result.include_system, 4);
			break;
		}
		if (call[2] && call.u32(3) >= part.size() && !part.empty())
			call.write(call[2], part);
		call.ret(uint32_t(part.size()));
	});
	registry.add("RavenShaderRelease", 1, [](ApiCall &call) {
		std::lock_guard lock(shader_mutex);
		shader_results.erase(call.u32(0));
	});
}

}
