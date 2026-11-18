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
	// Ordered accesses: ldapr (FEAT_LRCPC) and stlr, on a base register alone.
	void ldapr(uint8_t rt, uint8_t rn, unsigned size);
	void stlr(uint8_t rt, uint8_t rn, unsigned size);
	void dmb(Barrier barrier);
	// FEAT_LSE atomics with acquire and release semantics.
	void casal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size);
	void ldaddal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size);
	void swpal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size);

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
};

}
