#pragma once

#include <cstdint>
#include <optional>

namespace raven::translate {

// What cpuid and xgetbv tell x64 code. A feature is reported only when the translator runs every instruction it adds, and bits that
// describe the same state agree: OSXSAVE and XSAVE come with AVX, and then XCR0 is 7 (x87, SSE, AVX) and leaf 0xd lists the same
// components; otherwise OSXSAVE is clear and xgetbv raises #UD, as on x86. Beyond the x86-64 baseline (psABI x86-64-v1: FPU, CX8,
// CMOV, MMX, FXSR, SSE, SSE2, long mode, syscall), which x64 code may assume without asking, nothing is reported yet:
// docs/macos/avx.md has the policy and what each feature still lacks. cpuid_check.h tests both rules.
struct GuestFeatures {
	bool sse3 = false;
	bool ssse3 = false;
	bool sse41 = false;
	bool sse42 = false;
	bool popcnt = false;
	bool avx = false;
	bool avx2 = false;
	bool fma = false;
	bool f16c = false;
	bool bmi1 = false;
	bool bmi2 = false;
};

// What the translator runs today: every sample of cpuid_check.h translates, and the feature's oracle families pass in this process and
// in a vCPU against the x86 CPU.
constexpr GuestFeatures guest_features{
	.sse3 = true, .ssse3 = true, .sse41 = true, .sse42 = true, .popcnt = true, .avx = true, .avx2 = true, .fma = true,
	.bmi1 = true, .bmi2 = true};

struct CpuidAnswer {
	uint32_t eax, ebx, ecx, edx;
};

CpuidAnswer cpuid(uint32_t leaf, uint32_t subleaf, const GuestFeatures &features = guest_features);
// XCR index, or nothing where x86 raises #UD (OSXSAVE clear) or #GP (an XCR other than 0).
std::optional<uint64_t> xgetbv(uint32_t index, const GuestFeatures &features = guest_features);

}
