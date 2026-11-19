#include "cpuid.h"

namespace raven::translate {

namespace {

constexpr uint32_t highest_basic = 0xd, highest_extended = 0x80000008;

uint32_t text(const char *four)
{
	return uint32_t(uint8_t(four[0])) | uint32_t(uint8_t(four[1])) << 8 | uint32_t(uint8_t(four[2])) << 16 |
		uint32_t(uint8_t(four[3])) << 24;
}

}

CpuidAnswer cpuid(uint32_t leaf, uint32_t subleaf, const GuestFeatures &features)
{
	const bool xsave = features.avx;
	const uint64_t xcr0 = 3 | (features.avx ? 4 : 0);
	switch (leaf) {
	// "GenuineIntel", as Rosetta answers: x64 code that tunes by vendor knows it.
	case 0: return {highest_basic, text("Genu"), text("ntel"), text("ineI")};
	case 1: {
		// Family 6, model 0, stepping 0; no brand index, CLFLUSH size or APIC ID, one logical processor.
		uint32_t ecx = 0;
		ecx |= features.sse3 ? 1u << 0 : 0;
		ecx |= features.ssse3 ? 1u << 9 : 0;
		ecx |= features.fma ? 1u << 12 : 0;
		ecx |= features.sse41 ? 1u << 19 : 0;
		ecx |= features.sse42 ? 1u << 20 : 0;
		ecx |= features.popcnt ? 1u << 23 : 0;
		ecx |= xsave ? 1u << 26 | 1u << 27 : 0;
		ecx |= features.avx ? 1u << 28 : 0;
		ecx |= features.f16c ? 1u << 29 : 0;
		// FPU, CX8, CMOV, MMX, FXSR, SSE, SSE2.
		const uint32_t edx = 1u << 0 | 1u << 8 | 1u << 15 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26;
		return {0x600, 1u << 16, ecx, edx};
	}
	case 7: {
		if (subleaf != 0)
			return {0, 0, 0, 0};
		uint32_t ebx = 0;
		ebx |= features.bmi1 ? 1u << 3 : 0;
		ebx |= features.avx2 ? 1u << 5 : 0;
		ebx |= features.bmi2 ? 1u << 8 : 0;
		return {0, ebx, 0, 0};
	}
	case 0xd:
		if (!xsave)
			return {0, 0, 0, 0};
		// The legacy area and header (576 bytes), then the upper halves of YMM0-YMM15 at 576 (256 bytes).
		if (subleaf == 0) {
			const uint32_t size = 576 + (features.avx ? 256 : 0);
			return {static_cast<uint32_t>(xcr0), size, size, static_cast<uint32_t>(xcr0 >> 32)};
		}
		if (subleaf == 2 && features.avx)
			return {256, 576, 0, 0};
		return {0, 0, 0, 0};
	case 0x80000000: return {highest_extended, 0, 0, 0};
	// LAHF, LZCNT and the rest in ECX are not run; EDX: syscall and long mode.
	case 0x80000001: return {0, 0, 0, 1u << 11 | 1u << 29};
	case 0x80000002: return {text("Rave"), text("n x8"), text("6-64"), text(" tra")};
	case 0x80000003: return {text("nsla"), text("tor\0"), 0, 0};
	case 0x80000004: return {0, 0, 0, 0};
	// 48-bit virtual and physical addresses.
	case 0x80000008: return {0x3030, 0, 0, 0};
	default: return {0, 0, 0, 0};
	}
}

std::optional<uint64_t> xgetbv(uint32_t index, const GuestFeatures &features)
{
	if (!features.avx || index != 0)
		return std::nullopt;
	return 7;
}

}
