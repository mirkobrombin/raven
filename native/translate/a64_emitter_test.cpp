#include "a64_emitter.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

using namespace raven::translate::a64;
namespace simd = raven::translate::a64::simd;

struct Case {
	const char *text;
	std::function<void(Assembler &)> emit;
};

// Each case is an instruction as the assembler spells it and the emitter call that must encode the same word. The build assembles the
// texts with the host's assembler and disassembles them with llvm-objdump; the test compares that with what the emitter wrote.
static std::vector<Case> cases()
{
	return {
		{"movz x0, #0x1234", [](Assembler &a) { a.mov_imm(0, 0x1234); }},
		{"movz x9, #0x5678, lsl #16\nmovk x9, #0x1234, lsl #32", [](Assembler &a) { a.mov_imm(9, 0x123456780000ull); }},
		{"movn x3, #0", [](Assembler &a) { a.mov_imm(3, ~0ull); }},
		{"movn x3, #0x1233", [](Assembler &a) { a.mov_imm(3, ~0x1233ull); }},
		{"movz w5, #0x49", [](Assembler &a) { a.mov_imm(5, 0x49, false); }},
		{"movz x16, #0x2058\nmovk x16, #0x4000, lsl #16\nmovk x16, #0x1, lsl #32", [](Assembler &a) { a.mov_imm(16, 0x140002058ull); }},
		{"mov x0, #0xff00ff00ff00ff00", [](Assembler &a) { a.mov_imm(0, 0xff00ff00ff00ff00ull); }},
		{"mov x8, x0", [](Assembler &a) { a.mov(8, 0); }},
		{"mov w8, w1", [](Assembler &a) { a.mov(8, 1, false); }},
		{"mov x29, sp", [](Assembler &a) { a.mov_sp(29, sp); }},
		{"add x1, x2, #0x123", [](Assembler &a) { a.add_imm(1, 2, 0x123); }},
		{"adds w1, w2, #0x5", [](Assembler &a) { a.add_imm(1, 2, 5, false, false, true); }},
		{"sub sp, sp, #0x10, lsl #12", [](Assembler &a) { a.sub_imm(sp, sp, 0x10, true); }},
		{"subs w9, w10, #0x3e9", [](Assembler &a) { a.sub_imm(9, 10, 0x3e9, false, false, true); }},
		{"add x16, x25, x26, lsl #3", [](Assembler &a) { a.add(16, 25, 26, Shift::lsl, 3); }},
		{"adds w8, w8, w9", [](Assembler &a) { a.add(8, 8, 9, Shift::lsl, 0, false, true); }},
		{"subs x9, x10, x11, asr #2", [](Assembler &a) { a.sub(9, 10, 11, Shift::asr, 2, true, true); }},
		{"sub w9, wzr, w10", [](Assembler &a) { a.sub(9, zr, 10, Shift::lsl, 0, false); }},
		{"adcs x1, x2, x3", [](Assembler &a) { a.adc(1, 2, 3, true, true); }},
		{"sbc w1, w2, w3", [](Assembler &a) { a.sbc(1, 2, 3, false); }},
		{"ands w9, w10, w11", [](Assembler &a) { a.and_(9, 10, 11, false, true); }},
		{"orr x9, x10, x11, lsl #4", [](Assembler &a) { a.orr(9, 10, 11, Shift::lsl, 4); }},
		{"eor w9, w10, w11, lsr #1", [](Assembler &a) { a.eor(9, 10, 11, Shift::lsr, 1, false); }},
		{"orn x9, xzr, x10", [](Assembler &a) { a.orn(9, zr, 10); }},
		{"and x9, x10, #0xff", [](Assembler &a) { a.and_imm(9, 10, 0xff); }},
		{"ands w9, w10, #0x1", [](Assembler &a) { a.and_imm(9, 10, 1, false, true); }},
		{"and x9, x10, #0xfffffffffffffff0", [](Assembler &a) { a.and_imm(9, 10, ~0xfull); }},
		{"orr w9, w10, #0x80000000", [](Assembler &a) { a.orr_imm(9, 10, 0x80000000u, false); }},
		{"eor x9, x10, #0x5555555555555555", [](Assembler &a) { a.eor_imm(9, 10, 0x5555555555555555ull); }},
		{"and w9, w10, #0xff00ff", [](Assembler &a) { a.and_imm(9, 10, 0x00ff00ff, false); }},
		{"eor x9, x10, #0x1", [](Assembler &a) { a.eor_imm(9, 10, 1); }},
		{"ubfx x9, x8, #8, #8", [](Assembler &a) { a.ubfx(9, 8, 8, 8); }},
		{"sbfx w9, w8, #0, #16", [](Assembler &a) { a.sbfx(9, 8, 0, 16, false); }},
		{"bfi x8, x9, #0, #8", [](Assembler &a) { a.bfi(8, 9, 0, 8); }},
		{"bfi x8, x9, #8, #8", [](Assembler &a) { a.bfi(8, 9, 8, 8); }},
		{"bfi x8, x9, #0, #16", [](Assembler &a) { a.bfi(8, 9, 0, 16); }},
		{"lsl w9, w10, #24", [](Assembler &a) { a.lsl_imm(9, 10, 24, false); }},
		{"lsr x9, x10, #63", [](Assembler &a) { a.lsr_imm(9, 10, 63); }},
		{"asr w9, w10, #31", [](Assembler &a) { a.asr_imm(9, 10, 31, false); }},
		{"lsl x9, x10, x11", [](Assembler &a) { a.lslv(9, 10, 11); }},
		{"lsr w9, w10, w11", [](Assembler &a) { a.lsrv(9, 10, 11, false); }},
		{"asr x9, x10, x11", [](Assembler &a) { a.asrv(9, 10, 11); }},
		{"mul x9, x10, x11", [](Assembler &a) { a.mul(9, 10, 11); }},
		{"madd w9, w10, w11, w12", [](Assembler &a) { a.madd(9, 10, 11, 12, false); }},
		{"smulh x9, x10, x11", [](Assembler &a) { a.smulh(9, 10, 11); }},
		{"umulh x9, x10, x11", [](Assembler &a) { a.umulh(9, 10, 11); }},
		{"smull x9, w10, w11", [](Assembler &a) { a.smull(9, 10, 11); }},
		{"csel x9, x10, x11, ne", [](Assembler &a) { a.csel(9, 10, 11, Cond::ne); }},
		{"csel w9, w10, w11, hi", [](Assembler &a) { a.csel(9, 10, 11, Cond::hi, false); }},
		{"cset w9, eq", [](Assembler &a) { a.cset(9, Cond::eq); }},
		{"cset w9, lo", [](Assembler &a) { a.cset(9, Cond::cc); }},
		{"mrs x9, NZCV", [](Assembler &a) { a.mrs_nzcv(9); }},
		{"msr NZCV, x9", [](Assembler &a) { a.msr_nzcv(9); }},
		{"mrs x9, FPCR", [](Assembler &a) { a.mrs_fpcr(9); }},
		{"msr FPCR, x10", [](Assembler &a) { a.msr_fpcr(10); }},
		{"mrs x11, FPSR", [](Assembler &a) { a.mrs_fpsr(11); }},
		{"msr FPSR, x12", [](Assembler &a) { a.msr_fpsr(12); }},
		{"cfinv", [](Assembler &a) { a.cfinv(); }},
		{"setf8 w9", [](Assembler &a) { a.setf8(9); }},
		{"setf16 w9", [](Assembler &a) { a.setf16(9); }},
		{"rmif x9, #28, #2", [](Assembler &a) { a.rmif(9, 28, 2); }},
		{"ldr x9, [x23, #0x80]", [](Assembler &a) { a.ldr(9, 23, 0x80, 8); }},
		{"ldr w9, [x16]", [](Assembler &a) { a.ldr(9, 16, 0, 4); }},
		{"ldrh w9, [x16, #2]", [](Assembler &a) { a.ldr(9, 16, 2, 2); }},
		{"ldrb w9, [x16, #4095]", [](Assembler &a) { a.ldr(9, 16, 4095, 1); }},
		{"ldur x9, [x28, #-8]", [](Assembler &a) { a.ldr(9, 28, -8, 8); }},
		{"ldur w9, [x16, #3]", [](Assembler &a) { a.ldr(9, 16, 3, 4); }},
		{"str x9, [x23, #0x40]", [](Assembler &a) { a.str(9, 23, 0x40, 8); }},
		{"strb w9, [x16]", [](Assembler &a) { a.str(9, 16, 0, 1); }},
		{"sturh w9, [x16, #-2]", [](Assembler &a) { a.str(9, 16, -2, 2); }},
		{"ldrsb x9, [x16]", [](Assembler &a) { a.ldrs(9, 16, 0, 1, true); }},
		{"ldrsh w9, [x16, #2]", [](Assembler &a) { a.ldrs(9, 16, 2, 2, false); }},
		{"ldrsw x9, [x16, #4]", [](Assembler &a) { a.ldrs(9, 16, 4, 4, true); }},
		{"ldr x16, [x28], #8", [](Assembler &a) { a.ldr_post(16, 28, 8); }},
		{"str x9, [x28, #-8]!", [](Assembler &a) { a.str_pre(9, 28, -8); }},
		{"ldp x9, x10, [x17]", [](Assembler &a) { a.ldp(9, 10, 17, 0); }},
		{"stp x19, x20, [sp, #16]", [](Assembler &a) { a.stp(19, 20, sp, 16); }},
		{"stp x29, x30, [sp, #-16]!", [](Assembler &a) { a.stp_pre(29, 30, sp, -16); }},
		{"ldp x29, x30, [sp], #16", [](Assembler &a) { a.ldp_post(29, 30, sp, 16); }},
		{"stp q8, q9, [sp, #32]", [](Assembler &a) { a.stp_q(8, 9, sp, 32); }},
		{"ldp q8, q9, [sp, #32]", [](Assembler &a) { a.ldp_q(8, 9, sp, 32); }},
		{"ldr q1, [x16, #0x30]", [](Assembler &a) { a.ldr_v(1, 16, 0x30, 16); }},
		{"ldur q2, [x16, #-0x10]", [](Assembler &a) { a.ldr_v(2, 16, -16, 16); }},
		{"ldur q3, [x16, #0x3]", [](Assembler &a) { a.ldr_v(3, 16, 3, 16); }},
		{"str q4, [x7]", [](Assembler &a) { a.str_v(4, 7, 0, 16); }},
		{"ldr d5, [x16, #0x8]", [](Assembler &a) { a.ldr_v(5, 16, 8, 8); }},
		{"str d6, [x16, #0x18]", [](Assembler &a) { a.str_v(6, 16, 0x18, 8); }},
		{"ldr s7, [x16, #0x4]", [](Assembler &a) { a.ldr_v(7, 16, 4, 4); }},
		{"stur s8, [x16, #0x2]", [](Assembler &a) { a.str_v(8, 16, 2, 4); }},
		{"ldr h9, [x16]", [](Assembler &a) { a.ldr_v(9, 16, 0, 2); }},
		{"str b10, [x16, #0x1]", [](Assembler &a) { a.str_v(10, 16, 1, 1); }},
		{"and v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::and_16b, 1, 2, 3); }},
		{"bic v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::bic_16b, 1, 2, 3); }},
		{"orr v15.16b, v16.16b, v17.16b", [](Assembler &a) { a.simd(simd::orr_16b, 15, 16, 17); }},
		{"eor v1.16b, v2.16b, v31.16b", [](Assembler &a) { a.simd(simd::eor_16b, 1, 2, 31); }},
		{"fmov s1, w9", [](Assembler &a) { a.simd(simd::fmov_s_w, 1, 9); }},
		{"fmov w9, s1", [](Assembler &a) { a.simd(simd::fmov_w_s, 9, 1); }},
		{"fmov d1, x9", [](Assembler &a) { a.simd(simd::fmov_d_x, 1, 9); }},
		{"fmov x9, d1", [](Assembler &a) { a.simd(simd::fmov_x_d, 9, 1); }},
		{"fmov d3, d4", [](Assembler &a) { a.simd(simd::fmov_d_d, 3, 4); }},
		{"movi v16.2d, #0", [](Assembler &a) { a.simd(simd::movi_zero_2d, 16, 0); }},
		{"mov v1.s[0], v2.s[3]", [](Assembler &a) { a.ins(1, 0, 2, 3, 4); }},
		{"mov v1.d[1], v16.d[0]", [](Assembler &a) { a.ins(1, 1, 16, 0, 8); }},
		{"mov v1.b[15], v2.b[3]", [](Assembler &a) { a.ins(1, 15, 2, 3, 1); }},
		{"mov v1.h[2], v2.h[7]", [](Assembler &a) { a.ins(1, 2, 2, 7, 2); }},
		{"umov w9, v16.b[8]", [](Assembler &a) { a.umov(9, 16, 8, 1); }},
		{"umov w9, v16.s[1]", [](Assembler &a) { a.umov(9, 16, 1, 4); }},
		{"umov x9, v16.d[1]", [](Assembler &a) { a.umov(9, 16, 1, 8); }},
		{"dup v16.4s, v2.s[0]", [](Assembler &a) { a.dup(16, 2, 4); }},
		{"dup v16.2d, v2.d[0]", [](Assembler &a) { a.dup(16, 2, 8); }},
		{"ushr v16.16b, v2.16b, #7", [](Assembler &a) { a.shift_imm(simd::ushr, 16, 2, 8, 7); }},
		{"usra v16.8h, v16.8h, #7", [](Assembler &a) { a.shift_imm(simd::usra, 16, 16, 16, 7); }},
		{"usra v16.2d, v16.2d, #28", [](Assembler &a) { a.shift_imm(simd::usra, 16, 16, 64, 28); }},
		{"sshr v16.4s, v2.4s, #31", [](Assembler &a) { a.shift_imm(simd::sshr, 16, 2, 32, 31); }},
		{"shl v16.2d, v2.2d, #63", [](Assembler &a) { a.shift_imm(simd::shl, 16, 2, 64, 63); }},
		{"shl v16.8h, v2.8h, #1", [](Assembler &a) { a.shift_imm(simd::shl, 16, 2, 16, 1); }},
		{"ext v1.16b, v2.16b, v3.16b, #5", [](Assembler &a) { a.ext(1, 2, 3, 5); }},
		{"fadd v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::fadd_4s, 1, 2, 3); }},
		{"fadd v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::fadd_4s | simd::sz, 1, 2, 3); }},
		{"fsub v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::fsub_4s, 1, 2, 3); }},
		{"fsub v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::fsub_4s | simd::sz, 1, 2, 3); }},
		{"fmul v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::fmul_4s, 1, 2, 3); }},
		{"fmul v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::fmul_4s | simd::sz, 1, 2, 3); }},
		{"fdiv v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::fdiv_4s, 1, 2, 3); }},
		{"fdiv v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::fdiv_4s | simd::sz, 1, 2, 3); }},
		{"fmax v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::fmax_4s, 1, 2, 3); }},
		{"fmax v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::fmax_4s | simd::sz, 1, 2, 3); }},
		{"fmin v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::fmin_4s, 1, 2, 3); }},
		{"fmin v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::fmin_4s | simd::sz, 1, 2, 3); }},
		{"fadd s1, s2, s3", [](Assembler &a) { a.simd(simd::fadd_s, 1, 2, 3); }},
		{"fadd d1, d2, d3", [](Assembler &a) { a.simd(simd::fadd_s | simd::sz, 1, 2, 3); }},
		{"fsub s1, s2, s3", [](Assembler &a) { a.simd(simd::fsub_s, 1, 2, 3); }},
		{"fmul d1, d2, d3", [](Assembler &a) { a.simd(simd::fmul_s | simd::sz, 1, 2, 3); }},
		{"fdiv s1, s2, s3", [](Assembler &a) { a.simd(simd::fdiv_s, 1, 2, 3); }},
		{"fmax d1, d2, d3", [](Assembler &a) { a.simd(simd::fmax_s | simd::sz, 1, 2, 3); }},
		{"fmin s1, s2, s3", [](Assembler &a) { a.simd(simd::fmin_s, 1, 2, 3); }},
		{"fsqrt v1.4s, v2.4s", [](Assembler &a) { a.simd(simd::fsqrt_4s, 1, 2); }},
		{"fsqrt v1.2d, v2.2d", [](Assembler &a) { a.simd(simd::fsqrt_4s | simd::sz, 1, 2); }},
		{"fsqrt s1, s2", [](Assembler &a) { a.simd(simd::fsqrt_s, 1, 2); }},
		{"fsqrt d1, d2", [](Assembler &a) { a.simd(simd::fsqrt_s | simd::sz, 1, 2); }},
		{"fcmeq v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::fcmeq_4s, 1, 2, 3); }},
		{"fcmeq v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::fcmeq_4s | simd::sz, 1, 2, 3); }},
		{"fcmge v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::fcmge_4s, 1, 2, 3); }},
		{"fcmgt v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::fcmgt_4s | simd::sz, 1, 2, 3); }},
		{"fcmeq s1, s2, s3", [](Assembler &a) { a.simd(simd::fcmeq_s, 1, 2, 3); }},
		{"fcmge d1, d2, d3", [](Assembler &a) { a.simd(simd::fcmge_s | simd::sz, 1, 2, 3); }},
		{"fcmgt s1, s2, s3", [](Assembler &a) { a.simd(simd::fcmgt_s, 1, 2, 3); }},
		{"mvn v1.16b, v2.16b", [](Assembler &a) { a.simd(simd::not_16b, 1, 2); }},
		{"fcmp s2, s3", [](Assembler &a) { a.simd(simd::fcmp_s, 0, 2, 3); }},
		{"fcmpe d2, d3", [](Assembler &a) { a.simd(simd::fcmp_s | simd::sz | simd::signal_any, 0, 2, 3); }},
		{"scvtf s1, w9", [](Assembler &a) { a.simd(simd::scvtf_s_w, 1, 9); }},
		{"scvtf d1, x9", [](Assembler &a) { a.simd(simd::scvtf_s_w | simd::sf | simd::sz, 1, 9); }},
		{"scvtf s1, x9", [](Assembler &a) { a.simd(simd::scvtf_s_w | simd::sf, 1, 9); }},
		{"fcvtzs w9, s1", [](Assembler &a) { a.simd(simd::fcvtzs_w_s, 9, 1); }},
		{"fcvtzs x9, d1", [](Assembler &a) { a.simd(simd::fcvtzs_w_s | simd::sf | simd::sz, 9, 1); }},
		{"fcvtzs wzr, d1", [](Assembler &a) { a.simd(simd::fcvtzs_w_s | simd::sz, 31, 1); }},
		{"fcvt d1, s2", [](Assembler &a) { a.simd(simd::fcvt_d_s, 1, 2); }},
		{"fcvt s1, d2", [](Assembler &a) { a.simd(simd::fcvt_s_d, 1, 2); }},
		{"frintx s1, s2", [](Assembler &a) { a.simd(simd::frintx_s, 1, 2); }},
		{"frintx d1, d2", [](Assembler &a) { a.simd(simd::frintx_s | simd::sz, 1, 2); }},
		{"frintx v1.4s, v2.4s", [](Assembler &a) { a.simd(simd::frintx_4s, 1, 2); }},
		{"frintx v1.2d, v2.2d", [](Assembler &a) { a.simd(simd::frintx_4s | simd::sz, 1, 2); }},
		{"fcvtzs v1.4s, v2.4s", [](Assembler &a) { a.simd(simd::fcvtzs_4s, 1, 2); }},
		{"fcvtzs v1.2d, v2.2d", [](Assembler &a) { a.simd(simd::fcvtzs_4s | simd::sz, 1, 2); }},
		{"scvtf v1.4s, v2.4s", [](Assembler &a) { a.simd(simd::scvtf_4s, 1, 2); }},
		{"scvtf v1.2d, v2.2d", [](Assembler &a) { a.simd(simd::scvtf_4s | simd::sz, 1, 2); }},
		{"fcvtl v1.2d, v2.2s", [](Assembler &a) { a.simd(simd::fcvtl_2d, 1, 2); }},
		{"fcvtn v1.2s, v2.2d", [](Assembler &a) { a.simd(simd::fcvtn_2s, 1, 2); }},
		{"sshll v1.2d, v2.2s, #0", [](Assembler &a) { a.simd(simd::sxtl_2d, 1, 2); }},
		{"dup v16.4s, w9", [](Assembler &a) { a.simd(simd::dup_4s_w, 16, 9); }},
		{"add v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::add_16b, 1, 2, 3); }},
		{"add v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::add_16b | simd::size_d, 1, 2, 3); }},
		{"sub v1.8h, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::sub_16b | simd::size_h, 1, 2, 3); }},
		{"sqadd v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::sqadd_16b, 1, 2, 3); }},
		{"uqadd v1.8h, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::uqadd_16b | simd::size_h, 1, 2, 3); }},
		{"sqsub v1.8h, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::sqsub_16b | simd::size_h, 1, 2, 3); }},
		{"uqsub v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::uqsub_16b, 1, 2, 3); }},
		{"cmeq v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::cmeq_16b | simd::size_s, 1, 2, 3); }},
		{"cmgt v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::cmgt_16b, 1, 2, 3); }},
		{"mul v1.8h, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::mul_16b | simd::size_h, 1, 2, 3); }},
		{"smax v1.8h, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::smax_16b | simd::size_h, 1, 2, 3); }},
		{"umax v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::umax_16b, 1, 2, 3); }},
		{"smin v1.8h, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::smin_16b | simd::size_h, 1, 2, 3); }},
		{"umin v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::umin_16b, 1, 2, 3); }},
		{"urhadd v1.8h, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::urhadd_16b | simd::size_h, 1, 2, 3); }},
		{"uabd v1.16b, v2.16b, v3.16b", [](Assembler &a) { a.simd(simd::uabd_16b, 1, 2, 3); }},
		{"sshl v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::sshl_16b | simd::size_s, 1, 2, 3); }},
		{"ushl v1.2d, v2.2d, v3.2d", [](Assembler &a) { a.simd(simd::ushl_16b | simd::size_d, 1, 2, 3); }},
		{"addp v1.4s, v2.4s, v3.4s", [](Assembler &a) { a.simd(simd::addp_16b | simd::size_s, 1, 2, 3); }},
		{"uzp2 v1.8h, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::uzp2_16b | simd::size_h, 1, 2, 3); }},
		{"smull v1.4s, v2.4h, v3.4h", [](Assembler &a) { a.simd(simd::smull_8h | simd::size_h, 1, 2, 3); }},
		{"smull2 v1.4s, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::smull_8h | simd::size_h | simd::q, 1, 2, 3); }},
		{"umull v1.2d, v2.2s, v3.2s", [](Assembler &a) { a.simd(simd::umull_8h | simd::size_s, 1, 2, 3); }},
		{"umull2 v1.4s, v2.8h, v3.8h", [](Assembler &a) { a.simd(simd::umull_8h | simd::size_h | simd::q, 1, 2, 3); }},
		{"uaddlp v1.8h, v2.16b", [](Assembler &a) { a.simd(simd::uaddlp_16b, 1, 2); }},
		{"uaddlp v1.2d, v2.4s", [](Assembler &a) { a.simd(simd::uaddlp_16b | simd::size_s, 1, 2); }},
		{"xtn v1.2s, v2.2d", [](Assembler &a) { a.simd(simd::xtn_8b | simd::size_s, 1, 2); }},
		{"dup v1.16b, w9", [](Assembler &a) { a.simd(simd::dup_w | 1u << 16, 1, 9); }},
		{"dup v1.8h, w9", [](Assembler &a) { a.simd(simd::dup_w | 2u << 16, 1, 9); }},
		{"dup v1.4s, w9", [](Assembler &a) { a.simd(simd::dup_w | 4u << 16, 1, 9); }},
		{"dup v1.2d, x9", [](Assembler &a) { a.simd(simd::dup_w | 8u << 16, 1, 9); }},
		{"ldapr x9, [x16]", [](Assembler &a) { a.ldapr(9, 16, 8); }},
		{"ldapr w9, [x16]", [](Assembler &a) { a.ldapr(9, 16, 4); }},
		{"ldaprh w9, [x16]", [](Assembler &a) { a.ldapr(9, 16, 2); }},
		{"ldaprb w9, [x16]", [](Assembler &a) { a.ldapr(9, 16, 1); }},
		{"stlr x9, [x16]", [](Assembler &a) { a.stlr(9, 16, 8); }},
		{"stlr w9, [x16]", [](Assembler &a) { a.stlr(9, 16, 4); }},
		{"stlrb w9, [x16]", [](Assembler &a) { a.stlr(9, 16, 1); }},
		{"dmb ishld", [](Assembler &a) { a.dmb(Barrier::ishld); }},
		{"dmb ishst", [](Assembler &a) { a.dmb(Barrier::ishst); }},
		{"dmb ish", [](Assembler &a) { a.dmb(Barrier::ish); }},
		{"casal x9, x10, [x16]", [](Assembler &a) { a.casal(9, 10, 16, 8); }},
		{"casalb w9, w10, [x16]", [](Assembler &a) { a.casal(9, 10, 16, 1); }},
		{"ldaddal w9, w10, [x16]", [](Assembler &a) { a.ldaddal(9, 10, 16, 4); }},
		{"swpalh w9, w10, [x16]", [](Assembler &a) { a.swpal(9, 10, 16, 2); }},
		{"br x16", [](Assembler &a) { a.br(16); }},
		{"blr x16", [](Assembler &a) { a.blr(16); }},
		{"ret", [](Assembler &a) { a.ret(); }},
		{"brk #0x3c", [](Assembler &a) { a.brk(0x3c); }},
		{"nop", [](Assembler &a) { a.nop(); }},
		{"b #8\nnop", [](Assembler &a) {
			const auto skip = a.label();
			a.b(skip);
			a.nop();
			a.bind(skip);
		}},
		{"b.ne #8\nnop", [](Assembler &a) {
			const auto skip = a.label();
			a.b(Cond::ne, skip);
			a.nop();
			a.bind(skip);
		}},
		{"nop\nb.lo #-4", [](Assembler &a) {
			const auto back = a.label();
			a.bind(back);
			a.nop();
			a.b(Cond::cc, back);
		}},
		{"cbnz w9, #8\nnop", [](Assembler &a) {
			const auto skip = a.label();
			a.cbnz(9, skip, false);
			a.nop();
			a.bind(skip);
		}},
		{"cbz x9, #8\nnop", [](Assembler &a) {
			const auto skip = a.label();
			a.cbz(9, skip);
			a.nop();
			a.bind(skip);
		}},
		{"tbz w9, #0, #8\nnop", [](Assembler &a) {
			const auto skip = a.label();
			a.tbz(9, 0, skip);
			a.nop();
			a.bind(skip);
		}},
		{"tbnz x9, #63, #8\nnop", [](Assembler &a) {
			const auto skip = a.label();
			a.tbnz(9, 63, skip);
			a.nop();
			a.bind(skip);
		}},
		{"adr x10, #8\nnop", [](Assembler &a) {
			const auto skip = a.label();
			a.adr(10, skip);
			a.nop();
			a.bind(skip);
		}},
	};
}

int main(int argc, char **argv)
{
	if (argc == 3 && std::string(argv[1]) == "--assembly") {
		std::ofstream out(argv[2]);
		out << ".text\n";
		// Relative branches are written as instruction-relative offsets; the assembler takes them as "." plus the offset.
		const std::regex relative(R"(#(-?[0-9]+)$)");
		for (const Case &item : cases()) {
			std::string text = item.text;
			size_t start = 0;
			while (start < text.size()) {
				size_t end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				std::string line = text.substr(start, end - start);
				const bool branch = line.rfind("b #", 0) == 0 || line.rfind("b.", 0) == 0 || line.rfind("cb", 0) == 0 ||
					line.rfind("tb", 0) == 0 || line.rfind("adr ", 0) == 0;
				if (branch)
					line = std::regex_replace(line, relative, ".+$1");
				out << line << "\n";
				start = end + 1;
			}
		}
		return 0;
	}
	if (argc != 2) {
		std::cerr << "usage: a64_emitter_test DISASSEMBLY | --assembly OUTPUT\n";
		return 2;
	}
	std::ifstream disassembly(argv[1]);
	std::vector<uint32_t> expected;
	// llvm-objdump prints each AArch64 instruction as one 32-bit word.
	const std::regex line_pattern(R"(^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s)");
	std::string line;
	while (std::getline(disassembly, line)) {
		std::smatch match;
		if (std::regex_search(line, match, line_pattern))
			expected.push_back(static_cast<uint32_t>(std::stoul(match[1], nullptr, 16)));
	}
	size_t at = 0, failures = 0, checked = 0;
	for (const Case &item : cases()) {
		Assembler assembler;
		item.emit(assembler);
		assembler.finish();
		for (size_t i = 0; i < assembler.words.size(); ++i, ++at) {
			++checked;
			if (at >= expected.size() || expected[at] != assembler.words[i]) {
				++failures;
				std::fprintf(stderr, "%s: word %zu is %08x, the assembler wrote %08x\n", item.text, i, assembler.words[i],
					at < expected.size() ? expected[at] : 0u);
			}
		}
	}
	if (at != expected.size()) {
		std::fprintf(stderr, "%zu words emitted, %zu assembled\n", at, expected.size());
		++failures;
	}
	std::printf("%zu words checked, %zu failures\n", checked, failures);
	return failures == 0 ? 0 : 1;
}
