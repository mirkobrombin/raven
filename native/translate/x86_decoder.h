#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace raven::translate {

// The instructions the translator gives semantics to. Everything else that decodes is `other`: its length is right, so a block can end
// before it, but nothing translates it.
enum class Mnemonic : uint8_t {
	invalid,
	other,
	add,
	or_,
	adc,
	sbb,
	and_,
	sub,
	xor_,
	cmp,
	inc,
	dec,
	neg,
	not_,
	mul,
	imul,
	div,
	idiv,
	test,
	mov,
	movzx,
	movsx,
	movsxd,
	lea,
	xchg,
	cmpxchg,
	xadd,
	push,
	pop,
	rol,
	ror,
	rcl,
	rcr,
	shl,
	shr,
	sar,
	jmp,
	jcc,
	call,
	ret,
	setcc,
	cmovcc,
	cbw,
	cwd,
	leave,
	nop,
	int3,
	syscall,
	ud2,
	hlt,
	// Only between a general and an XMM register (66 REX.W 0F 6E and 7E), what calls pass doubles with.
	movq,
};

// xmm: an XMM register, reg 0-15.
enum class OperandKind : uint8_t { none, reg, mem, imm, rel, xmm };

// General registers 0-15 in encoding order (rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15); 16-19 are ah, ch, dh, bh.
constexpr uint8_t high_byte_base = 16;
constexpr uint8_t no_register = 0xff;

enum class Segment : uint8_t { none, es, cs, ss, ds, fs, gs };

struct Operand {
	OperandKind kind = OperandKind::none;
	// In bytes; 0 for the address of lea, which reads no memory.
	uint8_t size = 0;
	uint8_t reg = no_register;
	uint8_t base = no_register;
	uint8_t index = no_register;
	uint8_t scale = 1;
	bool rip_relative = false;
	// A SIB byte that names no index though ModRM alone could have said the same: LLVM prints its index as riz.
	bool redundant_sib = false;
	Segment segment = Segment::none;
	// The size of an immediate as encoded, before any sign extension.
	uint8_t encoded_size = 0;
	// The displacement of a memory operand, the value of an immediate (sign-extended to the operand size) or the target of a branch.
	int64_t value = 0;
};

struct Instruction {
	Mnemonic mnemonic = Mnemonic::invalid;
	uint8_t length = 0;
	// The condition of jcc, setcc and cmovcc in x86 encoding order (o, no, b, ae, e, ne, be, a, s, ns, p, np, l, ge, le, g).
	uint8_t condition = 0;
	uint8_t operand_size = 4;
	uint8_t address_size = 8;
	bool lock = false;
	bool rep = false;
	bool repne = false;
	uint8_t count = 0;
	std::array<Operand, 3> operands{};
	// The opcode: one byte, or 0x0f00 | byte, 0x0f3800 | byte, 0x0f3a00 | byte for the escape maps.
	uint32_t opcode = 0;
};

// Decodes the 64-bit mode instruction at the start of bytes, which sits at address. Never reads past bytes.
Instruction decode(std::span<const uint8_t> bytes, uint64_t address);

// The instruction in the Intel syntax LLVM's disassembler prints, for the mnemonics with semantics; empty for the others.
std::string format_intel(const Instruction &instruction);

}
