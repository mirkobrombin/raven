#pragma once

// What CPUID and XGETBV must tell x64 code, checked the same way in this process (cpuid_test) and in a vCPU (hvf_cpuid_test). The
// rule: a feature bit is set only when the translator runs that feature's instructions, and the bits that describe the same thing
// agree with each other (OSXSAVE, XSAVE, XCR0, leaf 0xD and AVX; AVX under AVX2, FMA and F16C). No AVX-512 bit is ever set: x64
// code with an AVX-512 path checks for it (Notepad++ has one) and the translator has no EVEX.

#include "cpu_state.h"
#include "runtime.h"
#include "x86_decoder.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace raven::translate::cpuid_check {

struct Answer {
	ExitReason reason = ExitReason::none;
	uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
};

// mov eax, leaf; mov ecx, subleaf; cpuid (or xgetbv); ret.
inline std::vector<uint8_t> code(uint32_t leaf, uint32_t subleaf, bool xgetbv)
{
	std::vector<uint8_t> bytes = {0xb8};
	for (unsigned i = 0; i < 4; ++i)
		bytes.push_back(static_cast<uint8_t>(leaf >> (8 * i)));
	bytes.push_back(0xb9);
	for (unsigned i = 0; i < 4; ++i)
		bytes.push_back(static_cast<uint8_t>(subleaf >> (8 * i)));
	if (xgetbv)
		bytes.insert(bytes.end(), {0x0f, 0x01, 0xd0});
	else
		bytes.insert(bytes.end(), {0x0f, 0xa2});
	bytes.push_back(0xc3);
	return bytes;
}

// A feature bit and instructions that need it, as bytes: the bit may be set only if the translator takes every one.
struct Feature {
	const char *name;
	uint32_t leaf;
	// 0 eax, 1 ebx, 2 ecx, 3 edx.
	unsigned reg;
	unsigned bit;
	std::vector<std::vector<uint8_t>> samples;
};

inline const std::vector<Feature> &features()
{
	static const std::vector<Feature> table = {
		{"SSE3", 1, 2, 0, {{0xf2, 0x0f, 0xd0, 0xc1}, {0xf2, 0x0f, 0x7c, 0xc1}, {0xf2, 0x0f, 0x12, 0xc1}}},
		{"SSSE3", 1, 2, 9, {{0x66, 0x0f, 0x38, 0x00, 0xc1}, {0x66, 0x0f, 0x3a, 0x0f, 0xc1, 0x01}, {0x66, 0x0f, 0x38, 0x1c, 0xc1}}},
		{"FMA", 1, 2, 12, {{0xc4, 0xe2, 0x75, 0xb8, 0xc2}, {0xc4, 0xe2, 0xf1, 0xa9, 0xc2}}},
		{"CMPXCHG16B", 1, 2, 13, {{0x48, 0x0f, 0xc7, 0x0e}}},
		{"SSE4.1", 1, 2, 19, {{0x66, 0x0f, 0x38, 0x10, 0xc1}, {0x66, 0x0f, 0x38, 0x17, 0xc1}, {0x66, 0x0f, 0x3a, 0x08, 0xc1, 0x00}}},
		{"SSE4.2", 1, 2, 20, {{0x66, 0x0f, 0x3a, 0x63, 0xc1, 0x00}, {0xf2, 0x0f, 0x38, 0xf1, 0xc1}, {0x66, 0x0f, 0x38, 0x37, 0xc1}}},
		{"MOVBE", 1, 2, 22, {{0x0f, 0x38, 0xf0, 0x01}}},
		{"POPCNT", 1, 2, 23, {{0xf3, 0x0f, 0xb8, 0xc1}}},
		{"XSAVE", 1, 2, 26, {{0x0f, 0xae, 0x21}}},
		{"AVX", 1, 2, 28,
			{{0xc5, 0xf8, 0x77}, {0xc5, 0xfc, 0x77}, {0xc5, 0xf4, 0x58, 0xc2}, {0xc4, 0xe2, 0x71, 0x00, 0xc2},
				{0xc4, 0xe2, 0x7d, 0x18, 0x00}, {0xc4, 0xe3, 0x75, 0x06, 0xc2, 0x01}, {0xc4, 0xe3, 0x75, 0x4a, 0xc2, 0x30},
				{0xc4, 0xe2, 0x79, 0x17, 0xc1}}},
		{"F16C", 1, 2, 29, {{0xc4, 0xe2, 0x7d, 0x13, 0xc1}}},
		{"BMI1", 7, 1, 3, {{0xc4, 0xe2, 0x60, 0xf2, 0xc1}}},
		{"AVX2", 7, 1, 5,
			{{0xc5, 0xf5, 0xfe, 0xc2}, {0xc4, 0xe3, 0xfd, 0x00, 0xc1, 0x1b}, {0xc4, 0xe2, 0x7d, 0x58, 0xc1},
				{0xc4, 0xe2, 0x75, 0x47, 0xc2}}},
		{"BMI2", 7, 1, 8, {{0xc4, 0xe2, 0x71, 0xf7, 0xc3}}},
		{"LZCNT", 0x80000001, 2, 5, {{0xf3, 0x0f, 0xbd, 0xc1}}},
	};
	return table;
}

// Runs every check with run(code), which translates and runs the bytes from a fresh state and returns what they left in RAX, RBX,
// RCX and RDX with the exit reason. Prints each failure and the leaves it read; returns the number of failures.
template <class Run>
size_t check(Run run, const char *where)
{
	size_t failures = 0;
	auto fail = [&](const std::string &text) {
		std::fprintf(stderr, "FAIL (%s): %s\n", where, text.c_str());
		++failures;
	};
	auto cpuid = [&](uint32_t leaf, uint32_t subleaf) {
		const Answer answer = run(code(leaf, subleaf, false));
		if (answer.reason != ExitReason::returned) {
			char text[96];
			std::snprintf(text, sizeof text, "cpuid leaf %#x subleaf %u did not return (exit %u)", leaf, subleaf,
				static_cast<unsigned>(answer.reason));
			fail(text);
		}
		return answer;
	};
	const Answer leaf0 = cpuid(0, 0), leaf1 = cpuid(1, 0), leaf7 = cpuid(7, 0), leaf13 = cpuid(13, 0), extended = cpuid(0x80000000, 0),
		 ext1 = cpuid(0x80000001, 0);
	if (failures)
		return failures;
	std::printf("%s: leaf 0 %08x %08x %08x %08x\n", where, leaf0.eax, leaf0.ebx, leaf0.ecx, leaf0.edx);
	std::printf("%s: leaf 1 %08x %08x %08x %08x\n", where, leaf1.eax, leaf1.ebx, leaf1.ecx, leaf1.edx);
	std::printf("%s: leaf 7 %08x %08x %08x %08x\n", where, leaf7.eax, leaf7.ebx, leaf7.ecx, leaf7.edx);
	std::printf("%s: leaf 0xd %08x %08x %08x %08x\n", where, leaf13.eax, leaf13.ebx, leaf13.ecx, leaf13.edx);
	std::printf("%s: leaf 0x80000001 %08x %08x %08x %08x\n", where, ext1.eax, ext1.ebx, ext1.ecx, ext1.edx);
	if (leaf0.eax < 0xd)
		fail("the highest basic leaf is below 0xd, which leaf 7 and leaf 0xd need");
	if (extended.eax < 0x80000001)
		fail("the highest extended leaf is below 0x80000001");
	// The x86-64 baseline (psABI x86-64-v1): FPU, CX8, CMOV, MMX, FXSR, SSE, SSE2; long mode and syscall.
	for (const unsigned bit : {0u, 8u, 15u, 23u, 24u, 25u, 26u})
		if (!(leaf1.edx >> bit & 1))
			fail("leaf 1 EDX lacks baseline bit " + std::to_string(bit));
	if (!(ext1.edx >> 29 & 1) || !(ext1.edx >> 11 & 1))
		fail("leaf 0x80000001 EDX lacks long mode or syscall");

	const bool osxsave = leaf1.ecx >> 27 & 1, xsave = leaf1.ecx >> 26 & 1, avx = leaf1.ecx >> 28 & 1;
	const Answer xcr0 = run(code(0, 0, true));
	if (osxsave) {
		if (!xsave)
			fail("OSXSAVE without XSAVE");
		if (xcr0.reason != ExitReason::returned)
			fail("OSXSAVE is set but xgetbv does not return");
		else {
			const uint64_t value = xcr0.eax | uint64_t(xcr0.edx) << 32;
			if ((value & 3) != 3)
				fail("XCR0 lacks x87 or SSE");
			if (bool(value & 4) != avx)
				fail("XCR0.AVX and CPUID's AVX bit disagree");
			if (value & ~uint64_t(7))
				fail("XCR0 enables state beyond x87, SSE and AVX (AVX-512 or others)");
			if (leaf13.eax != static_cast<uint32_t>(value) || leaf13.edx != static_cast<uint32_t>(value >> 32))
				fail("leaf 0xd subleaf 0 does not list XCR0's components");
			const uint32_t size = 576 + (avx ? 256 : 0);
			if (leaf13.ebx != size || leaf13.ecx != size)
				fail("leaf 0xd subleaf 0 sizes are not those of XCR0's components");
			if (avx) {
				const Answer ymm = cpuid(13, 2);
				if (ymm.eax != 256 || ymm.ebx != 576)
					fail("leaf 0xd subleaf 2 does not place the AVX state at 576, 256 bytes");
			}
		}
	} else {
		if (avx)
			fail("AVX without OSXSAVE");
		if (xcr0.reason != ExitReason::fault)
			fail("xgetbv returns although OSXSAVE is clear (x86 raises #UD)");
	}
	if (run(code(0, 1, true)).reason != ExitReason::fault)
		fail("xgetbv of XCR 1 does not fault (no XGETBV_ECX1 is reported)");
	for (const auto &[name, bit] : {std::pair{"FMA", 12u}, std::pair{"F16C", 29u}})
		if ((leaf1.ecx >> bit & 1) && !avx)
			fail(std::string(name) + " without AVX");
	if ((leaf7.ebx >> 5 & 1) && !avx)
		fail("AVX2 without AVX");
	// AVX512F, DQ, IFMA, PF, ER, CD, BW, VL in EBX; VBMI, VBMI2, VNNI, BITALG, VPOPCNTDQ in ECX; 4VNNIW, 4FMAPS, VP2INTERSECT, FP16 in
	// EDX.
	if (leaf7.ebx & 0xdc230000u)
		fail("leaf 7 EBX reports AVX-512");
	if (leaf7.ecx & 0x00005842u)
		fail("leaf 7 ECX reports AVX-512");
	if (leaf7.edx & 0x0080010cu)
		fail("leaf 7 EDX reports AVX-512");

	for (const Feature &feature : features()) {
		const Answer &answer = feature.leaf == 1 ? leaf1 : feature.leaf == 7 ? leaf7 : ext1;
		const uint32_t value = feature.reg == 0 ? answer.eax : feature.reg == 1 ? answer.ebx : feature.reg == 2 ? answer.ecx : answer.edx;
		const bool reported = value >> feature.bit & 1;
		size_t translated = 0;
		for (const auto &bytes : feature.samples)
			translated += translates(decode(bytes, 0x1000)) ? 1 : 0;
		std::printf("%s: %s %s, %zu of %zu samples translated\n", where, feature.name, reported ? "reported" : "not reported", translated,
			feature.samples.size());
		if (reported && translated != feature.samples.size())
			fail(std::string(feature.name) + " is reported but the translator does not take all its samples");
	}
	return failures;
}

}
