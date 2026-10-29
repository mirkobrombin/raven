#include "sm4.h"

#include <map>

namespace raven::shader::sm4 {

namespace {

void put_operand(std::vector<uint32_t> &out, const Operand &operand);

void put_relative(std::vector<uint32_t> &out, const Relative &relative)
{
	Operand index = Operand::reg(relative.type, relative.index);
	index.mode = Operand::Mode::select;
	index.swizzle[0] = relative.component;
	put_operand(out, index);
}

void put_operand(std::vector<uint32_t> &out, const Operand &operand)
{
	const bool modified = operand.negate || operand.absolute;
	uint32_t token = operand.components == 0 ? 0 : operand.components == 1 ? 1 : 2;
	if (operand.components == 4) {
		switch (operand.mode) {
		case Operand::Mode::mask:
			token |= uint32_t(operand.mask & 0xf) << 4;
			break;
		case Operand::Mode::swizzle:
			token |= 1u << 2;
			for (int i = 0; i < 4; ++i)
				token |= uint32_t(operand.swizzle[i] & 3) << (4 + i * 2);
			break;
		case Operand::Mode::select:
			token |= 2u << 2 | uint32_t(operand.swizzle[0] & 3) << 4;
			break;
		}
	}
	token |= uint32_t(operand.type) << 12 | uint32_t(operand.dimension) << 20;
	for (uint32_t i = 0; i < operand.dimension; ++i)
		if (operand.relative[i])
			token |= (operand.index[i] ? 3u : 2u) << (22 + i * 3);
	if (modified)
		token |= 1u << 31;
	out.push_back(token);
	if (modified)
		out.push_back(1 | uint32_t(operand.absolute && operand.negate ? 3 : operand.absolute ? 2 : 1) << 6);
	if (operand.type == immediate32) {
		out.push_back(operand.immediate[0]);
		if (operand.components == 4)
			for (int i = 1; i < 4; ++i)
				out.push_back(operand.immediate[i]);
		return;
	}
	for (uint32_t i = 0; i < operand.dimension; ++i) {
		if (!operand.relative[i] || operand.index[i])
			out.push_back(operand.index[i]);
		if (operand.relative[i])
			put_relative(out, *operand.relative[i]);
	}
}

}

std::vector<uint32_t> encode(uint32_t version, const std::vector<Instruction> &instructions)
{
	std::vector<uint32_t> out{version, 0};
	for (const auto &instruction : instructions) {
		// The immediate constant buffer: a custom data block whose second token is its length.
		if (instruction.opcode == customdata) {
			out.push_back(customdata | instruction.controls << 11);
			out.push_back(uint32_t(instruction.extra.size() + 2));
			out.insert(out.end(), instruction.extra.begin(), instruction.extra.end());
			continue;
		}
		const size_t start = out.size();
		out.push_back(0);
		std::vector<uint32_t> extended;
		if (instruction.offset) {
			const auto &offset = *instruction.offset;
			extended.push_back(1 | uint32_t(offset[0] & 0xf) << 9 | uint32_t(offset[1] & 0xf) << 13 | uint32_t(offset[2] & 0xf) << 17);
		}
		if (instruction.resource_info) {
			extended.push_back(2 | (instruction.resource_info->first & 0x1f) << 6);
			extended.push_back(3 | (instruction.resource_info->second & 0xffff) << 6);
		}
		for (size_t i = 0; i < extended.size(); ++i)
			out.push_back(extended[i] | (i + 1 < extended.size() ? 1u << 31 : 0));
		for (const auto &operand : instruction.operands)
			put_operand(out, operand);
		out.insert(out.end(), instruction.extra.begin(), instruction.extra.end());
		const uint32_t length = uint32_t(out.size() - start);
		out[start] = uint32_t(instruction.opcode) | (instruction.controls & 0x1fff) << 11 | (instruction.saturate ? 1u << 13 : 0) |
			(length & 0x7f) << 24 | (extended.empty() ? 0 : 1u << 31);
	}
	out[1] = uint32_t(out.size());
	return out;
}

namespace {

const std::map<uint32_t, const char *> &mnemonics()
{
	static const std::map<uint32_t, const char *> table{
	{0, "add"},
	{1, "and"},
	{2, "break"},
	{3, "breakc"},
	{6, "case"},
	{7, "continue"},
	{8, "continuec"},
	{9, "cut"},
	{10, "default"},
	{11, "deriv_rtx"},
	{12, "deriv_rty"},
	{13, "discard"},
	{14, "div"},
	{15, "dp2"},
	{16, "dp3"},
	{17, "dp4"},
	{18, "else"},
	{19, "emit"},
	{21, "endif"},
	{22, "endloop"},
	{23, "endswitch"},
	{24, "eq"},
	{25, "exp"},
	{26, "frc"},
	{27, "ftoi"},
	{28, "ftou"},
	{29, "ge"},
	{30, "iadd"},
	{31, "if"},
	{32, "ieq"},
	{33, "ige"},
	{34, "ilt"},
	{35, "imad"},
	{36, "imax"},
	{37, "imin"},
	{38, "imul"},
	{39, "ine"},
	{40, "ineg"},
	{41, "ishl"},
	{42, "ishr"},
	{43, "itof"},
	{45, "ld"},
	{46, "ld_ms"},
	{47, "log"},
	{48, "loop"},
	{49, "lt"},
	{50, "mad"},
	{51, "min"},
	{52, "max"},
	{53, "customdata"},
	{54, "mov"},
	{55, "movc"},
	{56, "mul"},
	{57, "ne"},
	{59, "not"},
	{60, "or"},
	{61, "resinfo"},
	{62, "ret"},
	{63, "retc"},
	{64, "round_ne"},
	{65, "round_ni"},
	{66, "round_pi"},
	{67, "round_z"},
	{68, "rsq"},
	{69, "sample"},
	{70, "sample_c"},
	{71, "sample_c_lz"},
	{72, "sample_l"},
	{73, "sample_d"},
	{74, "sample_b"},
	{75, "sqrt"},
	{76, "switch"},
	{77, "sincos"},
	{78, "udiv"},
	{79, "ult"},
	{80, "uge"},
	{81, "umul"},
	{82, "umad"},
	{83, "umax"},
	{84, "umin"},
	{85, "ushr"},
	{86, "utof"},
	{87, "xor"},
	{88, "dcl_resource"},
	{89, "dcl_constant_buffer"},
	{90, "dcl_sampler"},
	{92, "dcl_gs_output_topology"},
	{93, "dcl_gs_input_primitive"},
	{94, "dcl_max_output_vertex_count"},
	{95, "dcl_input"},
	{96, "dcl_input_sgv"},
	{97, "dcl_input_siv"},
	{98, "dcl_input_ps"},
	{99, "dcl_input_ps_sgv"},
	{100, "dcl_input_ps_siv"},
	{101, "dcl_output"},
	{102, "dcl_output_sgv"},
	{103, "dcl_output_siv"},
	{104, "dcl_temps"},
	{105, "dcl_indexable_temp"},
	{106, "dcl_global_flags"},
	{108, "lod"},
	{109, "gather4"},
	{111, "sample_info"},
	{113, "hs_decls"},
	{114, "hs_control_point_phase"},
	{115, "hs_fork_phase"},
	{116, "hs_join_phase"},
	{117, "emit_stream"},
	{118, "cut_stream"},
	{121, "bufinfo"},
	{122, "deriv_rtx_coarse"},
	{123, "deriv_rtx_fine"},
	{124, "deriv_rty_coarse"},
	{125, "deriv_rty_fine"},
	{126, "gather4_c"},
	{127, "gather4_po"},
	{128, "gather4_po_c"},
	{129, "rcp"},
	{130, "f32tof16"},
	{131, "f16tof32"},
	{132, "uaddc"},
	{133, "usubb"},
	{134, "countbits"},
	{135, "firstbit_hi"},
	{136, "firstbit_lo"},
	{137, "firstbit_shi"},
	{138, "ubfe"},
	{139, "ibfe"},
	{140, "bfi"},
	{141, "bfrev"},
	{142, "swapc"},
	{143, "dcl_stream"},
	{147, "dcl_input_control_point_count"},
	{148, "dcl_output_control_point_count"},
	{149, "dcl_tess_domain"},
	{150, "dcl_tess_partitioning"},
	{151, "dcl_tess_output_primitive"},
	{152, "dcl_hs_max_tessfactor"},
	{153, "dcl_hs_fork_phase_instance_count"},
	{155, "dcl_thread_group"},
	{156, "dcl_uav_typed"},
	{157, "dcl_uav_raw"},
	{158, "dcl_uav_structured"},
	{159, "dcl_tgsm_raw"},
	{160, "dcl_tgsm_structured"},
	{161, "dcl_resource_raw"},
	{162, "dcl_resource_structured"},
	{163, "ld_uav_typed"},
	{164, "store_uav_typed"},
	{165, "ld_raw"},
	{166, "store_raw"},
	{167, "ld_structured"},
	{168, "store_structured"},
	{169, "atomic_and"},
	{170, "atomic_or"},
	{171, "atomic_xor"},
	{172, "atomic_cmp_store"},
	{173, "atomic_iadd"},
	{174, "atomic_imax"},
	{175, "atomic_imin"},
	{176, "atomic_umax"},
	{177, "atomic_umin"},
	{178, "imm_atomic_alloc"},
	{179, "imm_atomic_consume"},
	{180, "imm_atomic_iadd"},
	{181, "imm_atomic_and"},
	{182, "imm_atomic_or"},
	{183, "imm_atomic_xor"},
	{184, "imm_atomic_exch"},
	{185, "imm_atomic_cmp_exch"},
	{186, "imm_atomic_imax"},
	{187, "imm_atomic_imin"},
	{188, "imm_atomic_umax"},
	{189, "imm_atomic_umin"},
	{190, "sync"},
	{204, "eval_sample_index"},
	{205, "eval_centroid"},
	{206, "dcl_gs_instance_count"}};
	return table;
}

std::string hex(uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	std::string text = "0x";
	bool started = false;
	for (int shift = 28; shift >= 0; shift -= 4) {
		const uint32_t digit = value >> shift & 15;
		if (digit || started || !shift) {
			text += digits[digit];
			started = true;
		}
	}
	return text;
}

// Reads one operand at `at` and returns its text.
std::string operand_text(const std::vector<uint32_t> &tokens, size_t &at)
{
	static const std::map<uint32_t, const char *> registers{{temp, "r"}, {input, "v"}, {output, "o"}, {indexable_temp, "x"}, {sampler, "s"},
		{resource, "t"}, {constant_buffer, "cb"}, {immediate_constant_buffer, "icb"}, {input_primitive_id, "vPrim"}, {output_depth, "oDepth"},
		{null_register, "null"}, {output_coverage, "oMask"}, {stream, "m"}, {output_control_point_id, "vOutputControlPointID"},
		{input_fork_instance_id, "vForkInstanceID"}, {input_join_instance_id, "vJoinInstanceID"}, {input_control_point, "vicp"},
		{output_control_point, "vocp"}, {input_patch_constant, "vpc"}, {input_domain_point, "vDomain"}, {uav, "u"}, {group_shared, "g"},
		{thread_id, "vThreadID"}, {thread_group_id, "vThreadGroupID"}, {thread_id_in_group, "vThreadIDInGroup"}, {input_coverage, "vCoverage"},
		{thread_id_in_group_flattened, "vThreadIDInGroupFlattened"}, {gs_instance_id, "vGSInstanceID"},
		{output_depth_greater_equal, "oDepthGE"}, {output_depth_less_equal, "oDepthLE"}};
	if (at >= tokens.size())
		return "?";
	const uint32_t token = tokens[at++];
	const uint32_t components = token & 3, mode = token >> 2 & 3, select = token >> 4 & 0xff, type = token >> 12 & 0xff, dimension = token >> 20 & 3;
	std::string prefix, suffix;
	if (token >> 31 && at < tokens.size()) {
		const uint32_t modifier = tokens[at++] >> 6 & 0xff;
		prefix = modifier == 1 ? "-" : modifier == 2 ? "|" : modifier == 3 ? "-|" : "";
		suffix = modifier >= 2 ? "|" : "";
	}
	if (type == immediate32) {
		const uint32_t count = components == 2 ? 4 : 1;
		std::string text = "l(";
		for (uint32_t i = 0; i < count && at < tokens.size(); ++i)
			text += (i ? ", " : "") + hex(tokens[at++]);
		return prefix + text + ")" + suffix;
	}
	const auto found = registers.find(type);
	std::string text = found != registers.end() ? found->second : "?" + std::to_string(type);
	for (uint32_t i = 0; i < dimension; ++i) {
		const uint32_t representation = token >> (22 + 3 * i) & 7;
		std::string index;
		if (representation == 0 || representation == 3)
			index = std::to_string(at < tokens.size() ? tokens[at++] : 0);
		if (representation == 2 || representation == 3)
			index = operand_text(tokens, at) + (index.empty() ? "" : " + " + index);
		text += i == 0 && dimension == 1 && representation == 0 ? index : "[" + index + "]";
	}
	if (components == 2) {
		static const char letters[] = "xyzw";
		text += '.';
		if (mode == 0) {
			for (int c = 0; c < 4; ++c)
				if (select & (1 << c))
					text += letters[c];
		} else if (mode == 1) {
			for (int c = 0; c < 4; ++c)
				text += letters[select >> (2 * c) & 3];
		} else
			text += letters[select & 3];
	}
	return prefix + text + suffix;
}

}

std::string disassemble(const std::vector<uint32_t> &tokens)
{
	if (tokens.size() < 2)
		return {};
	static const char *const stages[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
	const uint32_t version = tokens[0];
	std::string text = std::string((version >> 16) < 6 ? stages[version >> 16] : "?") + "_" + std::to_string(version >> 4 & 15) + "_" +
		std::to_string(version & 15) + "\n";
	size_t at = 2;
	const size_t end = std::min<size_t>(tokens.size(), tokens[1]);
	while (at < end) {
		const uint32_t token = tokens[at];
		const uint32_t opcode = token & 0x7ff;
		if (opcode == customdata) {
			const uint32_t length = at + 1 < end ? tokens[at + 1] : 2;
			text += "dcl_immediateConstantBuffer { " + std::to_string(length > 2 ? (length - 2) / 4 : 0) + " vectors }\n";
			at += std::max<uint32_t>(length, 2);
			continue;
		}
		const uint32_t length = token >> 24 & 0x7f;
		const size_t next = at + std::max<uint32_t>(length, 1);
		const auto found = mnemonics().find(opcode);
		std::string line = found != mnemonics().end() ? found->second : "opcode" + std::to_string(opcode);
		const uint32_t controls = token >> 11 & 0x1fff;
		const bool conditional = opcode == breakc || opcode == continuec || opcode == discard || opcode == op_if || opcode == retc;
		if (conditional)
			line += controls & (1 << 7) ? "_nz" : "_z";
		else if (opcode < dcl_resource && (controls & 4) && opcode != sync)
			line += "_sat";
		size_t position = at + 1;
		if (token >> 31)
			while (position < next && tokens[position++] >> 31) {
			}
		const bool declaration = (opcode >= dcl_resource && opcode <= dcl_global_flags) || opcode == dcl_stream || (opcode >= dcl_input_control_point_count &&
			opcode <= dcl_resource_structured) || opcode == dcl_gs_instance_count || opcode == hs_decls;
		std::string operands;
		const bool no_operands = opcode == dcl_temps || opcode == dcl_indexable_temp || opcode == dcl_global_flags || opcode == dcl_thread_group ||
			opcode == dcl_max_output_vertex_count || opcode == dcl_gs_input_primitive || opcode == dcl_gs_output_topology || opcode == dcl_gs_instance_count;
		while (!no_operands && position < next) {
			if (declaration && (opcode == dcl_resource || opcode == dcl_uav_typed || opcode == dcl_input_sgv || opcode == dcl_input_siv ||
					opcode == dcl_input_ps_sgv || opcode == dcl_input_ps_siv || opcode == dcl_output_sgv || opcode == dcl_output_siv ||
					opcode == dcl_resource_structured || opcode == dcl_uav_structured || opcode == dcl_tgsm_raw || opcode == dcl_tgsm_structured) &&
				!operands.empty())
				break;
			operands += (operands.empty() ? " " : ", ") + operand_text(tokens, position);
		}
		for (; position < next; ++position)
			operands += (operands.empty() ? " " : ", ") + std::to_string(tokens[position]);
		if (declaration && controls)
			line += " (" + hex(controls) + ")";
		text += line + operands + "\n";
		at = next;
	}
	return text;
}

}
