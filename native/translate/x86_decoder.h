#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace raven::translate {

// The instructions the translator gives semantics to. Everything else that decodes is `other`: its length is right, so a block can end
// before it, but nothing translates it.
enum class Mnemonic : uint16_t {
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
	// The runtime answers these (cpuid.h): a block ends before them with ExitReason::cpuid.
	cpuid,
	xgetbv,
	// SSE4.2's crc32 and popcnt, on general registers.
	crc32,
	popcnt,
	// xsave and xrstor (0F AE /4 and /5, memory only); operand_size 8 for their REX.W forms, xsave64 and xrstor64.
	xsave,
	xrstor,
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
	paddsb, paddsw, pmaxsw, pxor, pmuludq, pmaddwd, psadbw, psubb, psubw, psubd, psubq, paddb, paddw, paddd, rcpps, rsqrtps, rcpss,
	rsqrtss, maskmovdqu,
	// SSE3, SSSE3, SSE4.1 and SSE4.2 on XMM registers (and pinsrw, pextrw of SSE2), from addsubps to pcmpistri.
	addsubps, addsubpd, haddps, haddpd, hsubps, hsubpd, movddup, movsldup, movshdup, lddqu, pinsrw, pextrw, pshufb, phaddw, phaddd,
	phaddsw, pmaddubsw, phsubw, phsubd, phsubsw, psignb, psignw, psignd, pmulhrsw, pabsb, pabsw, pabsd, palignr, pblendvb, blendvps,
	blendvpd, ptest, pmovsxbw, pmovsxbd, pmovsxbq, pmovsxwd, pmovsxwq, pmovsxdq, pmuldq, pcmpeqq, movntdqa, packusdw, pmovzxbw,
	pmovzxbd, pmovzxbq, pmovzxwd, pmovzxwq, pmovzxdq, pcmpgtq, pminsb, pminsd, pminuw, pminud, pmaxsb, pmaxsd, pmaxuw, pmaxud, pmulld,
	phminposuw, roundps, roundpd, roundss, roundsd, blendps, blendpd, pblendw, pextrb, pextrd, pextrq, extractps, pinsrb, insertps,
	pinsrd, pinsrq, dpps, dppd, mpsadbw, pcmpestrm, pcmpestri, pcmpistrm, pcmpistri,
	// x87, from fadd to fwait (x87_first to x87_last), named as LLVM prints them: fcompi and fucompi are fcomip and fucomip, fwait
	// prints as wait. A memory operand has the size the instruction accesses (2, 4, 8 or 10 bytes), 28 for an environment and 108
	// for a save area.
	fadd, fmul, fcom, fcomp, fsub, fsubr, fdiv, fdivr, fld, fst, fstp, fldenv, fldcw, fnstenv, fnstcw, fxch, fnop, fchs, fabs, ftst,
	fxam, fld1, fldl2t, fldl2e, fldpi, fldlg2, fldln2, fldz, f2xm1, fyl2x, fptan, fpatan, fxtract, fprem1, fdecstp, fincstp, fprem,
	fyl2xp1, fsqrt, fsincos, frndint, fscale, fsin, fcos, fiadd, fimul, ficom, ficomp, fisub, fisubr, fidiv, fidivr, fcmovb, fcmove,
	fcmovbe, fcmovu, fucompp, fild, fisttp, fist, fistp, fcmovnb, fcmovne, fcmovnbe, fcmovnu, fnclex, fninit, fucomi, fcomi, frstor,
	fnsave, fnstsw, ffree, fucom, fucomp, faddp, fmulp, fcompp, fsubrp, fsubp, fdivrp, fdivp, ffreep, fbld, fbstp, fucompi, fcompi,
	fwait,
	// AVX and AVX2 instructions without an SSE form, from vzeroupper to the last FMA3 one (avx_first to avx_last). The VEX forms of SSE
	// and SSE2 keep the SSE mnemonic with Instruction::vex set.
	vzeroupper, vzeroall, vbroadcastss, vbroadcastsd, vbroadcastf128, vbroadcasti128, vpbroadcastb, vpbroadcastw, vpbroadcastd,
	vpbroadcastq,
	vinsertf128, vextractf128, vinserti128, vextracti128, vperm2f128, vperm2i128,
	vtestps, vtestpd, vpermilps, vpermilpd, vmaskmovps, vmaskmovpd, vmaskmovdqu,
	// AVX2 without an SSE form.
	vpermq, vpermpd, vpermd, vpermps, vpblendd, vpsllvd, vpsllvq, vpsrlvd, vpsrlvq, vpsravd, vpmaskmovd, vpmaskmovq,
	// The gathers: index size (d, q), then element (d or ps, q or pd).
	vpgatherdd, vpgatherdq, vpgatherqd, vpgatherqq, vgatherdps, vgatherdpd, vgatherqps, vgatherqpd,
	// FMA3: operation, operand order, then ps, pd, ss, sd.
	vfmadd132ps, vfmadd132pd, vfmadd132ss, vfmadd132sd, vfmadd213ps, vfmadd213pd, vfmadd213ss, vfmadd213sd, vfmadd231ps, vfmadd231pd,
	vfmadd231ss, vfmadd231sd, vfmsub132ps, vfmsub132pd, vfmsub132ss, vfmsub132sd, vfmsub213ps, vfmsub213pd, vfmsub213ss, vfmsub213sd,
	vfmsub231ps, vfmsub231pd, vfmsub231ss, vfmsub231sd, vfnmadd132ps, vfnmadd132pd, vfnmadd132ss, vfnmadd132sd, vfnmadd213ps,
	vfnmadd213pd, vfnmadd213ss, vfnmadd213sd, vfnmadd231ps, vfnmadd231pd, vfnmadd231ss, vfnmadd231sd, vfnmsub132ps, vfnmsub132pd,
	vfnmsub132ss, vfnmsub132sd, vfnmsub213ps, vfnmsub213pd, vfnmsub213ss, vfnmsub213sd, vfnmsub231ps, vfnmsub231pd, vfnmsub231ss,
	vfnmsub231sd,
	// vfmaddsub and vfmsubadd: operation, operand order, then ps and pd.
	vfmaddsub132ps, vfmaddsub132pd, vfmaddsub213ps, vfmaddsub213pd, vfmaddsub231ps, vfmaddsub231pd, vfmsubadd132ps, vfmsubadd132pd,
	vfmsubadd213ps, vfmsubadd213pd, vfmsubadd231ps, vfmsubadd231pd,
};

constexpr Mnemonic sse_first = Mnemonic::movq;
constexpr Mnemonic sse_last = Mnemonic::pcmpistri;
constexpr Mnemonic sse3_first = Mnemonic::addsubps;
constexpr Mnemonic x87_first = Mnemonic::fadd;
constexpr Mnemonic x87_last = Mnemonic::fwait;
constexpr Mnemonic avx_first = Mnemonic::vzeroupper;
constexpr Mnemonic avx_last = Mnemonic::vfmsubadd231pd;
constexpr Mnemonic fma_first = Mnemonic::vfmadd132ps;

// xmm: an XMM register, reg 0-15, or with size 32 the YMM register. st: the x87 register ST(reg), or ST(0) written as st when reg is no_register.
enum class OperandKind : uint8_t { none, reg, mem, imm, rel, xmm, st };

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
	// A gather's index (VSIB): an XMM (16) or YMM (32) register, index its number; 0 for a general register.
	uint8_t vector_index = 0;
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
	std::array<Operand, 4> operands{};
	// VEX-encoded: an SSE mnemonic names its VEX form (the operands then follow the VEX order: destination, the VEX.vvvv source, the
	// ModRM source, any immediate), and vector_size is 32 for a 256-bit form (VEX.L), 16 otherwise.
	bool vex = false;
	uint8_t vector_size = 16;
	// The opcode: one byte, or 0x0f00 | byte, 0x0f3800 | byte, 0x0f3a00 | byte for the escape maps.
	uint32_t opcode = 0;
};

// Decodes the 64-bit mode instruction at the start of bytes, which sits at address. Never reads past bytes.
Instruction decode(std::span<const uint8_t> bytes, uint64_t address);

// The instruction in the Intel syntax LLVM's disassembler prints, for the mnemonics with semantics; empty for the others.
std::string format_intel(const Instruction &instruction);

}
