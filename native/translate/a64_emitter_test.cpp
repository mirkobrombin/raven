#include "a64_emitter.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

using namespace raven::translate::a64;

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
