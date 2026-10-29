#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raven::shader::sm4 {

// Operand types of the tokenized program format.
enum OperandType : uint8_t {
	temp = 0, input = 1, output = 2, indexable_temp = 3, immediate32 = 4, sampler = 6, resource = 7, constant_buffer = 8,
	immediate_constant_buffer = 9, input_primitive_id = 11, output_depth = 12, null_register = 13, output_coverage = 15, stream = 16,
	output_control_point_id = 22, input_fork_instance_id = 23, input_join_instance_id = 24, input_control_point = 25,
	output_control_point = 26, input_patch_constant = 27, input_domain_point = 28, uav = 30, group_shared = 31, thread_id = 32,
	thread_group_id = 33, thread_id_in_group = 34, input_coverage = 35, thread_id_in_group_flattened = 36, gs_instance_id = 37,
	output_depth_greater_equal = 38, output_depth_less_equal = 39
};

enum Opcode : uint16_t {
	add = 0, op_and = 1, op_break = 2, breakc = 3, op_case = 6, op_continue = 7, continuec = 8, cut = 9, op_default = 10, deriv_rtx = 11,
	deriv_rty = 12, discard = 13, div = 14, dp2 = 15, dp3 = 16, dp4 = 17, op_else = 18, emit = 19, endif = 21, endloop = 22,
	endswitch = 23, eq = 24, exp = 25, frc = 26, ftoi = 27, ftou = 28, ge = 29, iadd = 30, op_if = 31, ieq = 32, ige = 33, ilt = 34,
	imad = 35, imax = 36, imin = 37, imul = 38, ine = 39, ineg = 40, ishl = 41, ishr = 42, itof = 43, ld = 45, ld_ms = 46, log = 47,
	loop = 48, lt = 49, mad = 50, min = 51, max = 52, customdata = 53, mov = 54, movc = 55, mul = 56, ne = 57, op_not = 59, op_or = 60, resinfo = 61,
	ret = 62, retc = 63, round_ne = 64, round_ni = 65, round_pi = 66, round_z = 67, rsq = 68, sample = 69, sample_c = 70, sample_c_lz = 71,
	sample_l = 72, sample_d = 73, sample_b = 74, sqrt = 75, op_switch = 76, sincos = 77, udiv = 78, ult = 79, uge = 80, umul = 81,
	umad = 82, umax = 83, umin = 84, ushr = 85, utof = 86, op_xor = 87, dcl_resource = 88, dcl_constant_buffer = 89, dcl_sampler = 90,
	dcl_gs_output_topology = 92, dcl_gs_input_primitive = 93, dcl_max_output_vertex_count = 94, dcl_input = 95, dcl_input_sgv = 96,
	dcl_input_siv = 97, dcl_input_ps = 98, dcl_input_ps_sgv = 99, dcl_input_ps_siv = 100, dcl_output = 101, dcl_output_sgv = 102,
	dcl_output_siv = 103, dcl_temps = 104, dcl_indexable_temp = 105, dcl_global_flags = 106, lod = 108, gather4 = 109,
	sample_info = 111, hs_decls = 113, hs_control_point_phase = 114, hs_fork_phase = 115, hs_join_phase = 116, emit_stream = 117,
	cut_stream = 118, bufinfo = 121, deriv_rtx_coarse = 122, deriv_rtx_fine = 123, deriv_rty_coarse = 124, deriv_rty_fine = 125,
	gather4_c = 126, gather4_po = 127, gather4_po_c = 128, rcp = 129, f32tof16 = 130, f16tof32 = 131, uaddc = 132, usubb = 133,
	countbits = 134, firstbit_hi = 135, firstbit_lo = 136, firstbit_shi = 137, ubfe = 138, ibfe = 139, bfi = 140, bfrev = 141,
	swapc = 142, dcl_stream = 143, dcl_input_control_point_count = 147, dcl_output_control_point_count = 148, dcl_tess_domain = 149,
	dcl_tess_partitioning = 150, dcl_tess_output_primitive = 151, dcl_hs_max_tessfactor = 152, dcl_hs_fork_phase_instance_count = 153,
	dcl_thread_group = 155, dcl_uav_typed = 156, dcl_uav_raw = 157, dcl_uav_structured = 158, dcl_tgsm_raw = 159,
	dcl_tgsm_structured = 160, dcl_resource_raw = 161, dcl_resource_structured = 162, ld_uav_typed = 163, store_uav_typed = 164,
	ld_raw = 165, store_raw = 166, ld_structured = 167, store_structured = 168, atomic_and = 169, atomic_or = 170, atomic_xor = 171,
	atomic_cmp_store = 172, atomic_iadd = 173, atomic_imax = 174, atomic_imin = 175, atomic_umax = 176, atomic_umin = 177,
	imm_atomic_alloc = 178, imm_atomic_consume = 179, imm_atomic_iadd = 180, imm_atomic_and = 181, imm_atomic_or = 182,
	imm_atomic_xor = 183, imm_atomic_exch = 184, imm_atomic_cmp_exch = 185, imm_atomic_imax = 186, imm_atomic_imin = 187,
	imm_atomic_umax = 188, imm_atomic_umin = 189, sync = 190, eval_sample_index = 204, eval_centroid = 205,
	dcl_gs_instance_count = 206
};

// A register used as an index of another operand: one component of a temp (or indexable temp) register.
struct Relative {
	uint8_t type = temp;
	uint32_t index = 0;
	uint8_t component = 0;
	bool operator==(const Relative &) const = default;
};

struct Operand {
	uint8_t type = temp;
	// 0, 1 or 4 components; a four component operand selects them by mask (destinations), swizzle or one component.
	uint8_t components = 4;
	enum class Mode : uint8_t { mask, swizzle, select } mode = Mode::mask;
	uint8_t mask = 0xf;
	std::array<uint8_t, 4> swizzle{0, 1, 2, 3};
	uint8_t dimension = 1;
	std::array<uint32_t, 3> index{};
	std::array<std::optional<Relative>, 3> relative{};
	bool negate = false, absolute = false;
	std::array<uint32_t, 4> immediate{};

	static Operand reg(uint8_t type, uint32_t number)
	{
		Operand operand;
		operand.type = type;
		operand.index[0] = number;
		return operand;
	}
	static Operand none(uint8_t type)
	{
		Operand operand;
		operand.type = type;
		operand.components = 0;
		operand.dimension = 0;
		return operand;
	}
	static Operand values(std::array<uint32_t, 4> bits, uint8_t count)
	{
		Operand operand;
		operand.type = immediate32;
		operand.components = count == 1 ? 1 : 4;
		operand.mode = Operand::Mode::swizzle;
		operand.dimension = 0;
		operand.immediate = bits;
		return operand;
	}
};

struct Instruction {
	uint16_t opcode = 0;
	// The opcode specific control bits (11 to 23): test sense, resource dimension, interpolation, access pattern.
	uint32_t controls = 0;
	bool saturate = false;
	std::vector<Operand> operands;
	// Tokens written after the operands: a declaration's counts, names or return types.
	std::vector<uint32_t> extra;
	// A sample offset (aoffimmi) carried in an extended opcode token.
	std::optional<std::array<int8_t, 3>> offset;
	// The resource dimension and return type of a sample or load, as shader model 5 extended opcode tokens.
	std::optional<std::pair<uint32_t, uint32_t>> resource_info;
};

// The program tokens: version, length and every instruction.
std::vector<uint32_t> encode(uint32_t version, const std::vector<Instruction> &instructions);

// The program as assembly text, one instruction a line.
std::string disassemble(const std::vector<uint32_t> &tokens);

}
