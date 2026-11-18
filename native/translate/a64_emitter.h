#pragma once

#include <cstdint>
#include <optional>
#include <vector>

// Encodes the AArch64 instructions the translator emits. Reference: Arm Architecture Reference Manual for A-profile, chapter C6.
namespace raven::translate::a64 {

// Register numbers; 31 is the zero register or sp depending on the instruction, as in the encoding.
constexpr uint8_t zr = 31;
constexpr uint8_t sp = 31;

enum class Cond : uint8_t { eq, ne, cs, cc, mi, pl, vs, vc, hi, ls, ge, lt, gt, le, al, nv };
enum class Shift : uint8_t { lsl, lsr, asr, ror };
enum class Barrier : uint8_t { ishld = 0b1001, ishst = 0b1010, ish = 0b1011 };

inline Cond invert(Cond cond)
{
	return static_cast<Cond>(static_cast<uint8_t>(cond) ^ 1);
}

// Advanced SIMD and scalar floating-point instructions as words with their register fields zero, for Assembler::simd: Rd at 0, Rn at 5,
// Rm at 16. Vector forms are on 128 bits (q) unless the name says otherwise.
namespace simd {
constexpr uint32_t and_16b = 0x4e201c00, bic_16b = 0x4e601c00, orr_16b = 0x4ea01c00, eor_16b = 0x6e201c00;
// fmov between general and SIMD&FP registers, and of a whole d register (which clears the rest of the vector).
constexpr uint32_t fmov_s_w = 0x1e270000, fmov_w_s = 0x1e260000, fmov_d_x = 0x9e670000, fmov_x_d = 0x9e660000;
constexpr uint32_t fmov_d_d = 0x1e604000;
constexpr uint32_t movi_zero_2d = 0x6f00e400;
// Floating point on four singles; or sz (bit 22) in for two doubles, or the scalar single for one double.
constexpr uint32_t sz = 0x00400000;
constexpr uint32_t fadd_4s = 0x4e20d400, fsub_4s = 0x4ea0d400, fmul_4s = 0x6e20dc00, fdiv_4s = 0x6e20fc00, fmax_4s = 0x4e20f400,
	fmin_4s = 0x4ea0f400, fsqrt_4s = 0x6ea1f800;
constexpr uint32_t fcmeq_4s = 0x4e20e400, fcmge_4s = 0x6e20e400, fcmgt_4s = 0x6ea0e400, not_16b = 0x6e205800;
constexpr uint32_t fcmeq_s = 0x5e20e400, fcmge_s = 0x7e20e400, fcmgt_s = 0x7ea0e400;
// fcmp and, with signal_any (or in), fcmpe of Rn with Rm, Rd zero.
constexpr uint32_t fcmp_s = 0x1e202000, signal_any = 0x10;
// Integer vectors on 16 bytes; or in size_h, size_s or size_d for elements of 2, 4 or 8 bytes. The long multiplies take the low
// halves of their sources as written and the high halves with q (bit 30) in.
constexpr uint32_t size_h = 0x00400000, size_s = 0x00800000, size_d = 0x00c00000, q = 0x40000000;
constexpr uint32_t add_16b = 0x4e208400, sub_16b = 0x6e208400, sqadd_16b = 0x4e200c00, uqadd_16b = 0x6e200c00,
	sqsub_16b = 0x4e202c00, uqsub_16b = 0x6e202c00, cmeq_16b = 0x6e208c00, cmgt_16b = 0x4e203400, mul_16b = 0x4e209c00,
	smax_16b = 0x4e206400, umax_16b = 0x6e206400, smin_16b = 0x4e206c00, umin_16b = 0x6e206c00, urhadd_16b = 0x6e201400,
	uabd_16b = 0x6e207400, sshl_16b = 0x4e204400, ushl_16b = 0x6e204400, addp_16b = 0x4e20bc00, uzp2_16b = 0x4e005800,
	uaddlp_16b = 0x6e202800;
constexpr uint32_t smull_8h = 0x0e20c000, umull_8h = 0x2e20c000, xtn_8b = 0x0e212800;
// dup of a general register into every element of size bytes: or in size << 16.
constexpr uint32_t dup_w = 0x4e000c00;
// Conversions. The scalar ones between general and SIMD&FP registers take sf (bit 31) for an X register and sz for a double.
constexpr uint32_t sf = 0x80000000;
constexpr uint32_t scvtf_s_w = 0x1e220000, fcvtzs_w_s = 0x1e380000, fcvt_d_s = 0x1e22c000, fcvt_s_d = 0x1e624000,
	frintx_s = 0x1e274000;
constexpr uint32_t frintx_4s = 0x6e219800, fcvtzs_4s = 0x4ea1b800, scvtf_4s = 0x4e21d800, fcvtl_2d = 0x0e617800,
	fcvtn_2s = 0x0e616800, sxtl_2d = 0x0f20a400, dup_4s_w = 0x4e040c00;
constexpr uint32_t fadd_s = 0x1e202800, fsub_s = 0x1e203800, fmul_s = 0x1e200800, fdiv_s = 0x1e201800, fmax_s = 0x1e204800,
	fmin_s = 0x1e205800, fsqrt_s = 0x1e21c000;
// For Assembler::shift_imm.
constexpr uint32_t shl = 0x4f005400, ushr = 0x6f000400, sshr = 0x4f000400, usra = 0x6f001400;
}

// The N:immr:imms fields of a logical immediate, when value is one.
std::optional<uint32_t> logical_immediate(uint64_t value, bool is64);

class Assembler {
public:
	struct Label {
		uint32_t id;
	};

	std::vector<uint32_t> words;

	size_t size() const { return words.size(); }
	Label label();
	void bind(Label label);
	// Resolves branches to labels; every label used must be bound.
	void finish();

	void emit(uint32_t word) { words.push_back(word); }

	// Moves and arithmetic. is64 selects the X form.
	void mov_imm(uint8_t rd, uint64_t value, bool is64 = true);
	void mov(uint8_t rd, uint8_t rm, bool is64 = true);
	void mov_sp(uint8_t rd, uint8_t rn);
	void add_imm(uint8_t rd, uint8_t rn, uint32_t imm12, bool shift12 = false, bool is64 = true, bool flags = false);
	void sub_imm(uint8_t rd, uint8_t rn, uint32_t imm12, bool shift12 = false, bool is64 = true, bool flags = false);
	void add(uint8_t rd, uint8_t rn, uint8_t rm, Shift shift = Shift::lsl, uint8_t amount = 0, bool is64 = true, bool flags = false);
	void sub(uint8_t rd, uint8_t rn, uint8_t rm, Shift shift = Shift::lsl, uint8_t amount = 0, bool is64 = true, bool flags = false);
	void adc(uint8_t rd, uint8_t rn, uint8_t rm, bool is64 = true, bool flags = false);
	void sbc(uint8_t rd, uint8_t rn, uint8_t rm, bool is64 = true, bool flags = false);
	void and_(uint8_t rd, uint8_t rn, uint8_t rm, bool is64 = true, bool flags = false);
	void orr(uint8_t rd, uint8_t rn, uint8_t rm, Shift shift = Shift::lsl, uint8_t amount = 0, bool is64 = true);
	void eor(uint8_t rd, uint8_t rn, uint8_t rm, Shift shift = Shift::lsl, uint8_t amount = 0, bool is64 = true);
	void orn(uint8_t rd, uint8_t rn, uint8_t rm, bool is64 = true);
	// Logical operations with an immediate; false when the value has no logical-immediate encoding.
	bool and_imm(uint8_t rd, uint8_t rn, uint64_t value, bool is64 = true, bool flags = false);
	bool orr_imm(uint8_t rd, uint8_t rn, uint64_t value, bool is64 = true);
	bool eor_imm(uint8_t rd, uint8_t rn, uint64_t value, bool is64 = true);
	void ubfm(uint8_t rd, uint8_t rn, uint8_t immr, uint8_t imms, bool is64 = true);
	void sbfm(uint8_t rd, uint8_t rn, uint8_t immr, uint8_t imms, bool is64 = true);
	void bfm(uint8_t rd, uint8_t rn, uint8_t immr, uint8_t imms, bool is64 = true);
	void ubfx(uint8_t rd, uint8_t rn, uint8_t lsb, uint8_t width, bool is64 = true) { ubfm(rd, rn, lsb, uint8_t(lsb + width - 1), is64); }
	void sbfx(uint8_t rd, uint8_t rn, uint8_t lsb, uint8_t width, bool is64 = true) { sbfm(rd, rn, lsb, uint8_t(lsb + width - 1), is64); }
	void bfi(uint8_t rd, uint8_t rn, uint8_t lsb, uint8_t width, bool is64 = true);
	void lsl_imm(uint8_t rd, uint8_t rn, uint8_t amount, bool is64 = true);
	void lsr_imm(uint8_t rd, uint8_t rn, uint8_t amount, bool is64 = true);
	void asr_imm(uint8_t rd, uint8_t rn, uint8_t amount, bool is64 = true);
	void lslv(uint8_t rd, uint8_t rn, uint8_t rm, bool is64 = true);
	void lsrv(uint8_t rd, uint8_t rn, uint8_t rm, bool is64 = true);
	void asrv(uint8_t rd, uint8_t rn, uint8_t rm, bool is64 = true);
	void madd(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra, bool is64 = true);
	void mul(uint8_t rd, uint8_t rn, uint8_t rm, bool is64 = true) { madd(rd, rn, rm, zr, is64); }
	void smulh(uint8_t rd, uint8_t rn, uint8_t rm);
	void umulh(uint8_t rd, uint8_t rn, uint8_t rm);
	void smull(uint8_t rd, uint8_t rn, uint8_t rm);
	void csel(uint8_t rd, uint8_t rn, uint8_t rm, Cond cond, bool is64 = true);
	void csinc(uint8_t rd, uint8_t rn, uint8_t rm, Cond cond, bool is64 = true);
	void cset(uint8_t rd, Cond cond, bool is64 = false) { csinc(rd, zr, zr, invert(cond), is64); }

	// Flags.
	void mrs_nzcv(uint8_t rt);
	void msr_nzcv(uint8_t rt);
	void cfinv();
	// The floating-point control and status registers.
	void mrs_fpcr(uint8_t rt);
	void msr_fpcr(uint8_t rt);
	void mrs_fpsr(uint8_t rt);
	void msr_fpsr(uint8_t rt);
	// FEAT_FlagM: N, Z and V from an 8- or 16-bit value, C kept.
	void setf8(uint8_t rn);
	void setf16(uint8_t rn);
	// FEAT_FlagM: rotates rn right by shift and moves its low four bits into the NZCV bits mask selects.
	void rmif(uint8_t rn, uint8_t shift, uint8_t mask);

	// Memory. size is 1, 2, 4 or 8 bytes.
	void ldr(uint8_t rt, uint8_t rn, int64_t offset, unsigned size);
	void str(uint8_t rt, uint8_t rn, int64_t offset, unsigned size);
	void ldrs(uint8_t rt, uint8_t rn, int64_t offset, unsigned size, bool is64);
	void ldr_post(uint8_t rt, uint8_t rn, int16_t offset);
	void str_pre(uint8_t rt, uint8_t rn, int16_t offset);
	void ldp(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset);
	void stp(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset);
	void ldp_post(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset);
	void stp_pre(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset);
	void ldp_q(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset);
	void stp_q(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset);
	// SIMD&FP registers: b, h, s, d or q for a size of 1, 2, 4, 8 or 16.
	void ldr_v(uint8_t rt, uint8_t rn, int64_t offset, unsigned size);
	void str_v(uint8_t rt, uint8_t rn, int64_t offset, unsigned size);
	// Ordered accesses: ldapr (FEAT_LRCPC) and stlr, on a base register alone.
	void ldapr(uint8_t rt, uint8_t rn, unsigned size);
	void stlr(uint8_t rt, uint8_t rn, unsigned size);
	void dmb(Barrier barrier);
	// FEAT_LSE atomics with acquire and release semantics.
	void casal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size);
	void ldaddal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size);
	void swpal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size);

	// Advanced SIMD and floating point. simd: any word of a64::simd with its registers. ins (mov element): element to of rd from
	// element from of rn; umov: element index of rn into general register rd; dup: element 0 of rn into every element of rd; elements
	// of size bytes. shift_imm: a64::simd::shl, ushr, sshr or usra by shift on elements of element_bits. ext: bytes index.. of rn:rm.
	void simd(uint32_t opcode, uint8_t rd, uint8_t rn, uint8_t rm = 0) { emit(opcode | uint32_t(rm & 31) << 16 | uint32_t(rn & 31) << 5 | (rd & 31u)); }
	void ins(uint8_t rd, unsigned to, uint8_t rn, unsigned from, unsigned size);
	void umov(uint8_t rd, uint8_t rn, unsigned index, unsigned size);
	void dup(uint8_t rd, uint8_t rn, unsigned size);
	void shift_imm(uint32_t opcode, uint8_t rd, uint8_t rn, unsigned element_bits, unsigned shift);
	void ext(uint8_t rd, uint8_t rn, uint8_t rm, unsigned index);

	// Branches.
	void b(Label target);
	void b(Cond cond, Label target);
	void cbz(uint8_t rt, Label target, bool is64 = true);
	void cbnz(uint8_t rt, Label target, bool is64 = true);
	void tbz(uint8_t rt, uint8_t bit, Label target);
	void tbnz(uint8_t rt, uint8_t bit, Label target);
	void br(uint8_t rn);
	void blr(uint8_t rn);
	void ret(uint8_t rn = 30);
	void adr(uint8_t rd, Label target);
	void brk(uint16_t imm);
	void nop();

	// A b with a word offset, for patching a chain into code already emitted.
	static uint32_t encode_b(int64_t word_offset);
	static bool b_reaches(int64_t word_offset) { return word_offset >= -(1 << 25) && word_offset < (1 << 25); }

private:
	enum class FixupKind : uint8_t { b26, b19, b14, adr };
	struct Fixup {
		size_t at;
		uint32_t label;
		FixupKind kind;
	};
	std::vector<int64_t> bound;
	std::vector<Fixup> fixups;
	void branch_to(Label target, uint32_t word, FixupKind kind);
	void memory_offset(uint8_t rt, uint8_t rn, int64_t offset, unsigned size, uint32_t opc, bool is_signed_load, bool is64);
	void vector_offset(uint8_t rt, uint8_t rn, int64_t offset, unsigned size, bool load);
};

}
