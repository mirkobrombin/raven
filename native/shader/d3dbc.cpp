#include "d3dbc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>

namespace raven::shader::d3dbc {

namespace {

struct Opcode {
	const char *name;
	// Destination and source parameters; definitions and declarations are read on their own.
	uint8_t dests, sources;
};

// D3DSHADER_INSTRUCTION_OPCODE_TYPE.
const std::map<uint32_t, Opcode> &opcodes()
{
	static const std::map<uint32_t, Opcode> table{{0, {"nop", 0, 0}}, {1, {"mov", 1, 1}}, {2, {"add", 1, 2}}, {3, {"sub", 1, 2}},
		{4, {"mad", 1, 3}}, {5, {"mul", 1, 2}}, {6, {"rcp", 1, 1}}, {7, {"rsq", 1, 1}}, {8, {"dp3", 1, 2}}, {9, {"dp4", 1, 2}},
		{10, {"min", 1, 2}}, {11, {"max", 1, 2}}, {12, {"slt", 1, 2}}, {13, {"sge", 1, 2}}, {14, {"exp", 1, 1}}, {15, {"log", 1, 1}},
		{16, {"lit", 1, 1}}, {17, {"dst", 1, 2}}, {18, {"lrp", 1, 3}}, {19, {"frc", 1, 1}}, {20, {"m4x4", 1, 2}}, {21, {"m4x3", 1, 2}},
		{22, {"m3x4", 1, 2}}, {23, {"m3x3", 1, 2}}, {24, {"m3x2", 1, 2}}, {25, {"call", 0, 1}}, {26, {"callnz", 0, 2}},
		{27, {"loop", 0, 2}}, {28, {"ret", 0, 0}}, {29, {"endloop", 0, 0}}, {30, {"label", 0, 1}}, {32, {"pow", 1, 2}},
		{33, {"crs", 1, 2}}, {34, {"sgn", 1, 3}}, {35, {"abs", 1, 1}}, {36, {"nrm", 1, 1}}, {37, {"sincos", 1, 3}}, {38, {"rep", 0, 1}},
		{39, {"endrep", 0, 0}}, {40, {"if", 0, 1}}, {41, {"if", 0, 2}}, {42, {"else", 0, 0}}, {43, {"endif", 0, 0}},
		{44, {"break", 0, 0}}, {45, {"break", 0, 2}}, {46, {"mova", 1, 1}}, {64, {"texcoord", 1, 0}}, {65, {"texkill", 1, 0}},
		{66, {"texld", 1, 2}}, {67, {"texbem", 1, 1}}, {68, {"texbeml", 1, 1}}, {69, {"texreg2ar", 1, 1}}, {70, {"texreg2gb", 1, 1}},
		{71, {"texm3x2pad", 1, 1}}, {72, {"texm3x2tex", 1, 1}}, {73, {"texm3x3pad", 1, 1}}, {74, {"texm3x3tex", 1, 1}},
		{76, {"texm3x3spec", 1, 2}}, {77, {"texm3x3vspec", 1, 1}}, {78, {"expp", 1, 1}}, {79, {"logp", 1, 1}}, {80, {"cnd", 1, 3}},
		{82, {"texreg2rgb", 1, 1}}, {83, {"texdp3tex", 1, 1}}, {84, {"texm3x2depth", 1, 1}}, {85, {"texdp3", 1, 1}},
		{86, {"texm3x3", 1, 1}}, {87, {"texdepth", 1, 0}}, {88, {"cmp", 1, 3}}, {89, {"bem", 1, 2}}, {90, {"dp2add", 1, 3}},
		{91, {"dsx", 1, 1}}, {92, {"dsy", 1, 1}}, {93, {"texldd", 1, 4}}, {94, {"setp", 1, 2}}, {95, {"texldl", 1, 2}},
		{96, {"break_pred", 0, 1}}};
	return table;
}

const char *const comparisons[] = {"", "_gt", "_eq", "_ge", "_lt", "_ne", "_le", ""};

// D3DDECLUSAGE.
const char *const usages[] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent", "binormal",
	"tessfactor", "positiont", "color", "fog", "depth", "sample"};

struct Reader {
	const std::vector<uint32_t> &tokens;
	size_t at = 1;
	uint32_t major = 0;
	bool pixel = false;

	uint32_t next() { return at < tokens.size() ? tokens[at++] : 0; }
};

uint32_t register_type(uint32_t token) { return (token >> 28 & 7) | (token >> 8 & 0x18); }

std::string register_name(const Reader &reader, uint32_t token)
{
	const uint32_t type = register_type(token), number = token & 0x7ff;
	const auto numbered = [number](const char *prefix) { return prefix + std::to_string(number); };
	switch (type) {
	case 0: return numbered("r");
	case 1: return numbered("v");
	case 2: return numbered("c");
	case 3: return numbered(reader.pixel ? "t" : "a");
	case 4: return number == 0 ? "oPos" : number == 1 ? "oFog" : "oPts";
	case 5: return numbered("oD");
	case 6: return numbered(reader.major >= 3 ? "o" : "oT");
	case 7: return numbered("i");
	case 8: return numbered("oC");
	case 9: return "oDepth";
	case 10: return numbered("s");
	case 11: return "c" + std::to_string(number + 2048);
	case 12: return "c" + std::to_string(number + 4096);
	case 13: return "c" + std::to_string(number + 6144);
	case 14: return numbered("b");
	case 15: return "aL";
	case 17: return number == 0 ? "vPos" : "vFace";
	case 18: return numbered("l");
	case 19: return numbered("p");
	default: return "?" + std::to_string(type);
	}
}

std::string mask_text(uint32_t token)
{
	const uint32_t mask = token >> 16 & 15;
	if (mask == 15)
		return "";
	std::string text = ".";
	for (int i = 0; i < 4; ++i)
		if (mask & 1u << i)
			text += "xyzw"[i];
	return text;
}

std::string dest_text(Reader &reader, uint32_t token, std::string &suffix)
{
	const uint32_t modifier = token >> 20 & 15;
	suffix.clear();
	if (modifier & 1)
		suffix += "_sat";
	if (modifier & 2)
		suffix += "_pp";
	if (modifier & 4)
		suffix += "_centroid";
	std::string text = register_name(reader, token);
	// Shader model 3 outputs may be indexed by the loop register.
	if (token & 1u << 13 && reader.major >= 3)
		text += "[" + register_name(reader, reader.next()) + "]";
	return text + mask_text(token);
}

std::string source_text(Reader &reader, uint32_t token)
{
	std::string text = register_name(reader, token);
	if (token & 1u << 13) {
		std::string index;
		if (reader.major >= 2) {
			const uint32_t address = reader.next();
			index = register_name(reader, address) + "." + "xyzw"[address >> 16 & 3];
		} else
			index = "a0.x";
		const uint32_t number = token & 0x7ff;
		const char letter = text[0];
		text = std::string(1, letter) + "[" + index + (number ? " + " + std::to_string(number) : "") + "]";
	}
	const uint32_t swizzle = token >> 16 & 0xff;
	if (swizzle != 0xe4) {
		const char *letters = "xyzw";
		std::string part;
		for (int i = 0; i < 4; ++i)
			part += letters[swizzle >> (2 * i) & 3];
		// A replicated component is written once.
		text += "." + (part.find_first_not_of(part[0]) == std::string::npos ? part.substr(0, 1) : part);
	}
	switch (token >> 24 & 15) {
	case 1: return "-" + text;
	case 2: return text + "_bias";
	case 3: return "-" + text + "_bias";
	case 4: return text + "_bx2";
	case 5: return "-" + text + "_bx2";
	case 6: return "1 - " + text;
	case 7: return text + "_x2";
	case 8: return "-" + text + "_x2";
	case 9: return text + "_dz";
	case 10: return text + "_dw";
	case 11: return "|" + text + "|";
	case 12: return "-|" + text + "|";
	case 13: return "!" + text;
	default: return text;
	}
}

std::string number_text(uint32_t bits)
{
	float value;
	std::memcpy(&value, &bits, 4);
	char text[32];
	std::snprintf(text, sizeof(text), "%g", double(value));
	return text;
}

// The registers a constant table names, as comment lines.
std::string constant_table(const uint8_t *data, size_t size)
{
	const auto word = [&](size_t offset) -> uint32_t {
		uint32_t value = 0;
		if (offset + 4 <= size)
			std::memcpy(&value, data + offset, 4);
		return value;
	};
	const auto half = [&](size_t offset) -> uint32_t {
		uint16_t value = 0;
		if (offset + 2 <= size)
			std::memcpy(&value, data + offset, 2);
		return value;
	};
	const auto text_at = [&](size_t offset) {
		std::string text;
		for (size_t i = offset; i < size && data[i]; ++i)
			text += char(data[i]);
		return text;
	};
	std::string out = "//\n";
	if (const uint32_t creator = word(4))
		out += "// Generated by " + text_at(creator) + "\n//\n";
	const uint32_t count = word(12), info = word(16);
	if (!count)
		return out;
	out += "// Registers:\n//\n//   Name         Reg   Size\n//   ------------ ----- ----\n";
	static const char sets[] = {'b', 'i', 'c', 's'};
	for (uint32_t i = 0; i < count && info + i * 20 + 20 <= size; ++i) {
		const size_t at = info + i * 20;
		std::string name = text_at(word(at));
		const uint32_t set = half(at + 4), index = half(at + 6), registers = half(at + 8);
		char line[160];
		std::snprintf(line, sizeof(line), "//   %-12s %c%-4u %4u\n", name.c_str(), set < 4 ? sets[set] : '?', index, registers);
		out += line;
	}
	return out + "//\n";
}

}

bool is_program(const std::vector<uint32_t> &tokens)
{
	if (tokens.empty())
		return false;
	const uint32_t kind = tokens[0] >> 16, major = tokens[0] >> 8 & 0xff;
	return (kind == 0xffff || kind == 0xfffe) && major >= 1 && major <= 3;
}

std::string disassemble(const std::vector<uint32_t> &tokens)
{
	if (!is_program(tokens))
		return "";
	Reader reader{tokens};
	reader.pixel = tokens[0] >> 16 == 0xffff;
	reader.major = tokens[0] >> 8 & 0xff;
	const uint32_t minor = tokens[0] & 0xff;
	std::string header, body;
	body += std::string("    ") + (reader.pixel ? "ps_" : "vs_") + std::to_string(reader.major) + "_" +
		(minor == 0xff ? std::string("sw") : std::to_string(minor)) + "\n";
	int depth = 0;
	while (reader.at < tokens.size()) {
		const size_t start = reader.at;
		const uint32_t token = reader.next();
		const uint32_t opcode = token & 0xffff;
		if (opcode == 0xffff)
			break;
		if (opcode == 0xfffe) {
			const uint32_t length = token >> 16 & 0x7fff;
			const size_t end = std::min(tokens.size(), start + 1 + length);
			if (length >= 1 && tokens[start + 1] == 0x42415443) // "CTAB"
				header += constant_table(reinterpret_cast<const uint8_t *>(tokens.data() + start + 2), (end - start - 2) * 4);
			reader.at = end;
			continue;
		}
		// Shader model 2 and later give the length; shader model 1 is read by the parameters the opcode takes.
		const size_t end = reader.major >= 2 ? start + 1 + (token >> 24 & 15) : 0;
		std::string line, suffix;
		if (opcode == 42 || opcode == 43 || opcode == 29 || opcode == 39)
			--depth;
		const std::string indent(4 + 2 * size_t(std::max(depth, 0)), ' ');
		if (token & 1u << 28) {
			const uint32_t predicate = reader.next();
			line = "(" + source_text(reader, predicate) + ") ";
		}
		if (opcode == 31) {
			const uint32_t usage = reader.next(), dest = reader.next();
			std::string name;
			if (register_type(dest) == 10) {
				const uint32_t texture = usage >> 27 & 15;
				name = texture == 3 ? "_cube" : texture == 4 ? "_volume" : "_2d";
			} else if (reader.major >= 3 || !reader.pixel) {
				const uint32_t kind = usage & 31, index = usage >> 16 & 15;
				if (register_type(dest) != 17 && kind < std::size(usages))
					name = std::string("_") + usages[kind] + (index ? std::to_string(index) : "");
			}
			line += "dcl" + name;
			const std::string target = dest_text(reader, dest, suffix);
			line += suffix + " " + target;
		} else if (opcode == 81 || opcode == 48 || opcode == 47) {
			const uint32_t dest = reader.next();
			line += opcode == 81 ? "def " : opcode == 48 ? "defi " : "defb ";
			line += register_name(reader, dest);
			const int count = opcode == 47 ? 1 : 4;
			for (int i = 0; i < count; ++i) {
				const uint32_t value = reader.next();
				line += ", " + (opcode == 81 ? number_text(value) : opcode == 48 ? std::to_string(int32_t(value)) : value ? "true" : "false");
			}
		} else {
			const auto found = opcodes().find(opcode);
			if (found == opcodes().end()) {
				line += "; unknown opcode " + std::to_string(opcode);
			} else {
				std::string name = found->second.name;
				if (opcode == 41 || opcode == 45 || opcode == 94)
					name += comparisons[token >> 16 & 7];
				if (opcode == 66 && reader.major >= 2)
					name += (token >> 16 & 3) == 1 ? "p" : (token >> 16 & 3) == 2 ? "b" : "";
				if (opcode == 66 && reader.major < 2)
					name = reader.major == 1 && minor >= 4 ? "texld" : "tex";
				line += name;
				std::string operands;
				uint8_t dests = found->second.dests, sources = found->second.sources;
				// sincos takes only its source on shader model 3.
				if (opcode == 37 && reader.major >= 3)
					sources = 1;
				for (uint8_t i = 0; i < dests; ++i) {
					std::string modifier;
					operands += (operands.empty() ? " " : ", ") + dest_text(reader, reader.next(), modifier);
					line += modifier;
				}
				while (sources-- && (end == 0 || reader.at < end))
					operands += (operands.empty() ? " " : ", ") + source_text(reader, reader.next());
				line += operands;
			}
		}
		if (end)
			reader.at = std::max(reader.at, end);
		body += indent + line + "\n";
		if (opcode == 40 || opcode == 41 || opcode == 42 || opcode == 27 || opcode == 38)
			++depth;
	}
	return header + body;
}

}
