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
	// SSE and SSE2 on XMM registers, from movq to the end (sse_first to sse_last). movq and movd move between XMM and general
	// registers or memory; movq also between XMM registers and the low quadword. A memory operand has the size the instruction
	// accesses: 16 for a whole register, 8, 4 or 2 for a part.
	movq,
	movd,
	movups, movupd, movss, movsd, movlps, movlpd, movhlps, movhps, movhpd, movlhps, unpcklps, unpcklpd, unpckhps, unpckhpd, movaps,
	movapd, movntps, movntpd, cvtsi2ss, cvtsi2sd, cvttss2si, cvttsd2si, cvtss2si, cvtsd2si, ucomiss, ucomisd, comiss, comisd,
	movmskps, movmskpd, sqrtps, sqrtpd, sqrtss, sqrtsd, andps, andpd, andnps, andnpd, orps, orpd, xorps, xorpd, addps, addpd, addss,
	addsd, mulps, mulpd, mulss, mulsd, cvtps2pd, cvtpd2ps, cvtss2sd, cvtsd2ss, cvtdq2ps, cvtps2dq, cvttps2dq, subps, subpd, subss,
	subsd, minps, minpd, minss, minsd, divps, divpd, divss, divsd, maxps, maxpd, maxss, maxsd, punpcklbw, punpcklwd, punpckldq,
	packsswb, pcmpgtb, pcmpgtw, pcmpgtd, packuswb, punpckhbw, punpckhwd, punpckhdq, packssdw, punpcklqdq, punpckhqdq, movdqa, movdqu,
	pshufd, pshufhw, pshuflw, psrlw, psraw, psllw, psrld, psrad, pslld, psrlq, psrldq, psllq, pslldq, pcmpeqb, pcmpeqw, pcmpeqd,
	ldmxcsr, stmxcsr, cmpps, cmppd, cmpss, cmpsd, shufps, shufpd, paddq, pmullw, pmovmskb, psubusb, psubusw, pminub, pand, paddusb,
	paddusw, pmaxub, pandn, pavgb, pavgw, pmulhuw, pmulhw, cvttpd2dq, cvtdq2pd, cvtpd2dq, movntdq, psubsb, psubsw, pminsw, por,
	paddsb, paddsw, pmaxsw, pxor, pmuludq, pmaddwd, psadbw, psubb, psubw, psubd, psubq, paddb, paddw, paddd,
};

constexpr Mnemonic sse_first = Mnemonic::movq;
constexpr Mnemonic sse_last = Mnemonic::paddd;

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
