#include "a64_emitter.h"

#include <stdexcept>

namespace raven::translate::a64 {

namespace {

uint32_t sf(bool is64)
{
	return is64 ? 1u << 31 : 0;
}

uint32_t reg(uint8_t value, unsigned at)
{
	return uint32_t(value & 31) << at;
}

unsigned size_log2(unsigned size)
{
	switch (size) {
	case 1: return 0;
	case 2: return 1;
	case 4: return 2;
	case 8: return 3;
	default: throw std::logic_error("access size must be 1, 2, 4 or 8");
	}
}

}

std::optional<uint32_t> logical_immediate(uint64_t value, bool is64)
{
	if (!is64)
		value = (value & 0xffffffffu) | (value << 32);
	if (value == 0 || value == ~0ull)
		return std::nullopt;
	// The smallest element size whose repetition gives value.
	unsigned size = 64;
	while (size > 2) {
		const unsigned half = size / 2;
		const uint64_t mask = (1ull << half) - 1;
		if ((value & mask) != ((value >> half) & mask))
			break;
		size = half;
	}
	const uint64_t mask = size == 64 ? ~0ull : (1ull << size) - 1;
	const uint64_t element = value & mask;
	// The element must be a rotation of a run of ones.
	unsigned rotation = 0;
	uint64_t run = element;
	for (; rotation < size; ++rotation) {
		// A run starting at bit 0 that does not wrap: ones then zeros.
		if ((run & 1) && ((run + 1) & run) == 0)
			break;
		run = ((run >> 1) | ((run & 1) << (size - 1))) & mask;
	}
	if (rotation == size)
		return std::nullopt;
	unsigned ones = 0;
	while (ones < size && ((run >> ones) & 1))
		++ones;
	// run is element rotated right by rotation; element is run rotated left, which ROR by (size - rotation) % size gives.
	const unsigned immr = (size - rotation) % size;
	const unsigned n = size == 64 ? 1 : 0;
	const unsigned imms = ((~(size * 2 - 1) & 0x3f) | (ones - 1)) & 0x3f;
	if (!is64 && n)
		return std::nullopt;
	return (n << 12) | (immr << 6) | imms;
}

Assembler::Label Assembler::label()
{
	bound.push_back(-1);
	return Label{static_cast<uint32_t>(bound.size() - 1)};
}

void Assembler::bind(Label label)
{
	bound[label.id] = static_cast<int64_t>(words.size());
}

void Assembler::branch_to(Label target, uint32_t word, FixupKind kind)
{
	fixups.push_back({words.size(), target.id, kind});
	words.push_back(word);
}

void Assembler::finish()
{
	for (const Fixup &fixup : fixups) {
		if (bound[fixup.label] < 0)
			throw std::logic_error("branch to an unbound label");
		const int64_t offset = bound[fixup.label] - static_cast<int64_t>(fixup.at);
		uint32_t &word = words[fixup.at];
		switch (fixup.kind) {
		case FixupKind::b26: word |= static_cast<uint32_t>(offset) & 0x3ffffff; break;
		case FixupKind::b19: word |= (static_cast<uint32_t>(offset) & 0x7ffff) << 5; break;
		case FixupKind::b14: word |= (static_cast<uint32_t>(offset) & 0x3fff) << 5; break;
		case FixupKind::adr: {
			const int64_t bytes = offset * 4;
			word |= (static_cast<uint32_t>(bytes) & 3) << 29;
			word |= ((static_cast<uint32_t>(bytes) >> 2) & 0x7ffff) << 5;
			break;
		}
		}
	}
	fixups.clear();
}

void Assembler::mov_imm(uint8_t rd, uint64_t value, bool is64)
{
	if (!is64)
		value &= 0xffffffffu;
	const unsigned halves = is64 ? 4 : 2;
	unsigned zeros = 0, ones = 0;
	for (unsigned i = 0; i < halves; ++i) {
		const uint16_t half = static_cast<uint16_t>(value >> (16 * i));
		zeros += half == 0;
		ones += half == 0xffff;
	}
	if (const auto logical = logical_immediate(value, is64); logical && zeros < halves - 1 && ones < halves - 1) {
		emit(sf(is64) | 0x32000000u | (*logical << 10) | reg(zr, 5) | reg(rd, 0));
		return;
	}
	const bool inverted = ones > zeros;
	if (value == (inverted ? (is64 ? ~0ull : 0xffffffffull) : 0)) {
		emit(sf(is64) | (inverted ? 0x12800000u : 0x52800000u) | reg(rd, 0));
		return;
	}
	bool first = true;
	for (unsigned i = 0; i < halves; ++i) {
		const uint16_t half = static_cast<uint16_t>(value >> (16 * i));
		if (half == (inverted ? 0xffff : 0) && !(i == halves - 1 && first))
			continue;
		if (first) {
			const uint16_t field = inverted ? static_cast<uint16_t>(~half) : half;
			emit(sf(is64) | (inverted ? 0x12800000u : 0x52800000u) | (i << 21) | (uint32_t(field) << 5) | reg(rd, 0));
			first = false;
		} else
			emit(sf(is64) | 0x72800000u | (i << 21) | (uint32_t(half) << 5) | reg(rd, 0));
	}
}

void Assembler::mov(uint8_t rd, uint8_t rm, bool is64)
{
	orr(rd, zr, rm, Shift::lsl, 0, is64);
}

void Assembler::mov_sp(uint8_t rd, uint8_t rn)
{
	add_imm(rd, rn, 0);
}

void Assembler::add_imm(uint8_t rd, uint8_t rn, uint32_t imm12, bool shift12, bool is64, bool flags)
{
	emit(sf(is64) | (flags ? 0x31000000u : 0x11000000u) | (shift12 ? 1u << 22 : 0) | ((imm12 & 0xfff) << 10) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::sub_imm(uint8_t rd, uint8_t rn, uint32_t imm12, bool shift12, bool is64, bool flags)
{
	emit(sf(is64) | (flags ? 0x71000000u : 0x51000000u) | (shift12 ? 1u << 22 : 0) | ((imm12 & 0xfff) << 10) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::add(uint8_t rd, uint8_t rn, uint8_t rm, Shift shift, uint8_t amount, bool is64, bool flags)
{
	emit(sf(is64) | (flags ? 0x2b000000u : 0x0b000000u) | (uint32_t(shift) << 22) | reg(rm, 16) | (uint32_t(amount & 63) << 10) |
		reg(rn, 5) | reg(rd, 0));
}

void Assembler::sub(uint8_t rd, uint8_t rn, uint8_t rm, Shift shift, uint8_t amount, bool is64, bool flags)
{
	emit(sf(is64) | (flags ? 0x6b000000u : 0x4b000000u) | (uint32_t(shift) << 22) | reg(rm, 16) | (uint32_t(amount & 63) << 10) |
		reg(rn, 5) | reg(rd, 0));
}

void Assembler::adc(uint8_t rd, uint8_t rn, uint8_t rm, bool is64, bool flags)
{
	emit(sf(is64) | (flags ? 0x3a000000u : 0x1a000000u) | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::sbc(uint8_t rd, uint8_t rn, uint8_t rm, bool is64, bool flags)
{
	emit(sf(is64) | (flags ? 0x7a000000u : 0x5a000000u) | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::and_(uint8_t rd, uint8_t rn, uint8_t rm, bool is64, bool flags)
{
	emit(sf(is64) | (flags ? 0x6a000000u : 0x0a000000u) | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::orr(uint8_t rd, uint8_t rn, uint8_t rm, Shift shift, uint8_t amount, bool is64)
{
	emit(sf(is64) | 0x2a000000u | (uint32_t(shift) << 22) | reg(rm, 16) | (uint32_t(amount & 63) << 10) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::eor(uint8_t rd, uint8_t rn, uint8_t rm, Shift shift, uint8_t amount, bool is64)
{
	emit(sf(is64) | 0x4a000000u | (uint32_t(shift) << 22) | reg(rm, 16) | (uint32_t(amount & 63) << 10) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::orn(uint8_t rd, uint8_t rn, uint8_t rm, bool is64)
{
	emit(sf(is64) | 0x2a200000u | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

bool Assembler::and_imm(uint8_t rd, uint8_t rn, uint64_t value, bool is64, bool flags)
{
	const auto fields = logical_immediate(value, is64);
	if (!fields)
		return false;
	emit(sf(is64) | (flags ? 0x72000000u : 0x12000000u) | (*fields << 10) | reg(rn, 5) | reg(rd, 0));
	return true;
}

bool Assembler::orr_imm(uint8_t rd, uint8_t rn, uint64_t value, bool is64)
{
	const auto fields = logical_immediate(value, is64);
	if (!fields)
		return false;
	emit(sf(is64) | 0x32000000u | (*fields << 10) | reg(rn, 5) | reg(rd, 0));
	return true;
}

bool Assembler::eor_imm(uint8_t rd, uint8_t rn, uint64_t value, bool is64)
{
	const auto fields = logical_immediate(value, is64);
	if (!fields)
		return false;
	emit(sf(is64) | 0x52000000u | (*fields << 10) | reg(rn, 5) | reg(rd, 0));
	return true;
}

void Assembler::ubfm(uint8_t rd, uint8_t rn, uint8_t immr, uint8_t imms, bool is64)
{
	emit(sf(is64) | (is64 ? 1u << 22 : 0) | 0x53000000u | (uint32_t(immr & 63) << 16) | (uint32_t(imms & 63) << 10) | reg(rn, 5) |
		reg(rd, 0));
}

void Assembler::sbfm(uint8_t rd, uint8_t rn, uint8_t immr, uint8_t imms, bool is64)
{
	emit(sf(is64) | (is64 ? 1u << 22 : 0) | 0x13000000u | (uint32_t(immr & 63) << 16) | (uint32_t(imms & 63) << 10) | reg(rn, 5) |
		reg(rd, 0));
}

void Assembler::bfm(uint8_t rd, uint8_t rn, uint8_t immr, uint8_t imms, bool is64)
{
	emit(sf(is64) | (is64 ? 1u << 22 : 0) | 0x33000000u | (uint32_t(immr & 63) << 16) | (uint32_t(imms & 63) << 10) | reg(rn, 5) |
		reg(rd, 0));
}

void Assembler::bfi(uint8_t rd, uint8_t rn, uint8_t lsb, uint8_t width, bool is64)
{
	const uint8_t bits = is64 ? 64 : 32;
	bfm(rd, rn, static_cast<uint8_t>((bits - lsb) % bits), static_cast<uint8_t>(width - 1), is64);
}

void Assembler::lsl_imm(uint8_t rd, uint8_t rn, uint8_t amount, bool is64)
{
	const uint8_t bits = is64 ? 64 : 32;
	ubfm(rd, rn, static_cast<uint8_t>((bits - amount) % bits), static_cast<uint8_t>(bits - 1 - amount), is64);
}

void Assembler::lsr_imm(uint8_t rd, uint8_t rn, uint8_t amount, bool is64)
{
	ubfm(rd, rn, amount, is64 ? 63 : 31, is64);
}

void Assembler::asr_imm(uint8_t rd, uint8_t rn, uint8_t amount, bool is64)
{
	sbfm(rd, rn, amount, is64 ? 63 : 31, is64);
}

void Assembler::lslv(uint8_t rd, uint8_t rn, uint8_t rm, bool is64)
{
	emit(sf(is64) | 0x1ac02000u | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::lsrv(uint8_t rd, uint8_t rn, uint8_t rm, bool is64)
{
	emit(sf(is64) | 0x1ac02400u | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::asrv(uint8_t rd, uint8_t rn, uint8_t rm, bool is64)
{
	emit(sf(is64) | 0x1ac02800u | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::madd(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra, bool is64)
{
	emit(sf(is64) | 0x1b000000u | reg(rm, 16) | reg(ra, 10) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::smulh(uint8_t rd, uint8_t rn, uint8_t rm)
{
	emit(0x9b407c00u | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::umulh(uint8_t rd, uint8_t rn, uint8_t rm)
{
	emit(0x9bc07c00u | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::smull(uint8_t rd, uint8_t rn, uint8_t rm)
{
	emit(0x9b207c00u | reg(rm, 16) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::csel(uint8_t rd, uint8_t rn, uint8_t rm, Cond cond, bool is64)
{
	emit(sf(is64) | 0x1a800000u | reg(rm, 16) | (uint32_t(cond) << 12) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::csinc(uint8_t rd, uint8_t rn, uint8_t rm, Cond cond, bool is64)
{
	emit(sf(is64) | 0x1a800400u | reg(rm, 16) | (uint32_t(cond) << 12) | reg(rn, 5) | reg(rd, 0));
}

void Assembler::mrs_nzcv(uint8_t rt)
{
	emit(0xd53b4200u | reg(rt, 0));
}

void Assembler::msr_nzcv(uint8_t rt)
{
	emit(0xd51b4200u | reg(rt, 0));
}

void Assembler::mrs_fpcr(uint8_t rt)
{
	emit(0xd53b4400u | reg(rt, 0));
}

void Assembler::msr_fpcr(uint8_t rt)
{
	emit(0xd51b4400u | reg(rt, 0));
}

void Assembler::mrs_fpsr(uint8_t rt)
{
	emit(0xd53b4420u | reg(rt, 0));
}

void Assembler::msr_fpsr(uint8_t rt)
{
	emit(0xd51b4420u | reg(rt, 0));
}

void Assembler::cfinv()
{
	emit(0xd500401fu);
}

void Assembler::setf8(uint8_t rn)
{
	emit(0x3a00080du | reg(rn, 5));
}

void Assembler::setf16(uint8_t rn)
{
	emit(0x3a00480du | reg(rn, 5));
}

void Assembler::rmif(uint8_t rn, uint8_t shift, uint8_t mask)
{
	emit(0xba000400u | (uint32_t(shift & 63) << 15) | reg(rn, 5) | (mask & 15));
}

void Assembler::memory_offset(uint8_t rt, uint8_t rn, int64_t offset, unsigned size, uint32_t opc, bool is_signed_load, bool is64)
{
	const unsigned scale = size_log2(size);
	uint32_t opc_field = opc;
	if (is_signed_load)
		opc_field = is64 ? 2 : 3;
	const uint32_t base = (uint32_t(scale) << 30) | 0x38000000u | (opc_field << 22);
	if (offset >= 0 && (offset & int64_t((1u << scale) - 1)) == 0 && (offset >> scale) < 4096)
		emit(base | 0x01000000u | (uint32_t(offset >> scale) << 10) | reg(rn, 5) | reg(rt, 0));
	else if (offset >= -256 && offset < 256)
		emit(base | ((uint32_t(offset) & 0x1ff) << 12) | reg(rn, 5) | reg(rt, 0));
	else
		throw std::logic_error("memory offset out of range");
}

void Assembler::ldr(uint8_t rt, uint8_t rn, int64_t offset, unsigned size)
{
	memory_offset(rt, rn, offset, size, 1, false, false);
}

void Assembler::str(uint8_t rt, uint8_t rn, int64_t offset, unsigned size)
{
	memory_offset(rt, rn, offset, size, 0, false, false);
}

void Assembler::ldrs(uint8_t rt, uint8_t rn, int64_t offset, unsigned size, bool is64)
{
	memory_offset(rt, rn, offset, size, 0, true, is64);
}

void Assembler::ldr_post(uint8_t rt, uint8_t rn, int16_t offset)
{
	emit(0xf8400400u | ((uint32_t(offset) & 0x1ff) << 12) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::str_pre(uint8_t rt, uint8_t rn, int16_t offset)
{
	emit(0xf8000c00u | ((uint32_t(offset) & 0x1ff) << 12) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::ldp(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset)
{
	emit(0xa9400000u | ((uint32_t(offset / 8) & 0x7f) << 15) | reg(rt2, 10) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::stp(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset)
{
	emit(0xa9000000u | ((uint32_t(offset / 8) & 0x7f) << 15) | reg(rt2, 10) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::ldp_post(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset)
{
	emit(0xa8c00000u | ((uint32_t(offset / 8) & 0x7f) << 15) | reg(rt2, 10) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::stp_pre(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset)
{
	emit(0xa9800000u | ((uint32_t(offset / 8) & 0x7f) << 15) | reg(rt2, 10) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::ldp_q(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset)
{
	emit(0xad400000u | ((uint32_t(offset / 16) & 0x7f) << 15) | reg(rt2, 10) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::stp_q(uint8_t rt, uint8_t rt2, uint8_t rn, int16_t offset)
{
	emit(0xad000000u | ((uint32_t(offset / 16) & 0x7f) << 15) | reg(rt2, 10) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::ldapr(uint8_t rt, uint8_t rn, unsigned size)
{
	emit((uint32_t(size_log2(size)) << 30) | 0x38bfc000u | reg(rn, 5) | reg(rt, 0));
}

void Assembler::stlr(uint8_t rt, uint8_t rn, unsigned size)
{
	emit((uint32_t(size_log2(size)) << 30) | 0x089ffc00u | reg(rn, 5) | reg(rt, 0));
}

void Assembler::dmb(Barrier barrier)
{
	emit(0xd50330bfu | (uint32_t(barrier) << 8));
}

void Assembler::casal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size)
{
	emit((uint32_t(size_log2(size)) << 30) | 0x08e0fc00u | reg(rs, 16) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::ldaddal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size)
{
	emit((uint32_t(size_log2(size)) << 30) | 0x38e00000u | reg(rs, 16) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::swpal(uint8_t rs, uint8_t rt, uint8_t rn, unsigned size)
{
	emit((uint32_t(size_log2(size)) << 30) | 0x38e08000u | reg(rs, 16) | reg(rn, 5) | reg(rt, 0));
}

void Assembler::b(Label target)
{
	branch_to(target, 0x14000000u, FixupKind::b26);
}

void Assembler::b(Cond cond, Label target)
{
	branch_to(target, 0x54000000u | uint32_t(cond), FixupKind::b19);
}

void Assembler::cbz(uint8_t rt, Label target, bool is64)
{
	branch_to(target, sf(is64) | 0x34000000u | reg(rt, 0), FixupKind::b19);
}

void Assembler::cbnz(uint8_t rt, Label target, bool is64)
{
	branch_to(target, sf(is64) | 0x35000000u | reg(rt, 0), FixupKind::b19);
}

void Assembler::tbz(uint8_t rt, uint8_t bit, Label target)
{
	branch_to(target, (uint32_t(bit >> 5) << 31) | 0x36000000u | (uint32_t(bit & 31) << 19) | reg(rt, 0), FixupKind::b14);
}

void Assembler::tbnz(uint8_t rt, uint8_t bit, Label target)
{
	branch_to(target, (uint32_t(bit >> 5) << 31) | 0x37000000u | (uint32_t(bit & 31) << 19) | reg(rt, 0), FixupKind::b14);
}

void Assembler::br(uint8_t rn)
{
	emit(0xd61f0000u | reg(rn, 5));
}

void Assembler::blr(uint8_t rn)
{
	emit(0xd63f0000u | reg(rn, 5));
}

void Assembler::ret(uint8_t rn)
{
	emit(0xd65f0000u | reg(rn, 5));
}

void Assembler::adr(uint8_t rd, Label target)
{
	branch_to(target, 0x10000000u | reg(rd, 0), FixupKind::adr);
}

void Assembler::brk(uint16_t imm)
{
	emit(0xd4200000u | (uint32_t(imm) << 5));
}

void Assembler::nop()
{
	emit(0xd503201fu);
}

uint32_t Assembler::encode_b(int64_t word_offset)
{
	return 0x14000000u | (static_cast<uint32_t>(word_offset) & 0x3ffffff);
}

}
