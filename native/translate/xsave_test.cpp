#include "runtime.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace raven::translate;

namespace {

class Snippets final : public GuestMemory {
public:
	std::vector<uint8_t> bytes = std::vector<uint8_t>(1 << 16);
	size_t used = 0;
	uint64_t add(const std::vector<uint8_t> &code)
	{
		const uint64_t address = reinterpret_cast<uint64_t>(bytes.data()) + used;
		std::memcpy(bytes.data() + used, code.data(), code.size());
		used += (code.size() + 15) & ~size_t(15);
		return address;
	}
	std::span<const uint8_t> code(uint64_t address) const override
	{
		const uint64_t start = reinterpret_cast<uint64_t>(bytes.data());
		if (address < start || address >= start + used)
			return {};
		return {reinterpret_cast<const uint8_t *>(address), static_cast<size_t>(start + used - address)};
	}
	bool is_native(uint64_t) const override { return false; }
	void write(uint64_t address, const void *from, size_t size) override { std::memcpy(reinterpret_cast<void *>(address), from, size); }
	void read(uint64_t address, void *into, size_t size) const override
	{
		std::memcpy(into, reinterpret_cast<const void *>(address), size);
	}
	void watch_code(uint64_t, size_t, bool) override {}
	size_t watch_granule() const override { return 4096; }
};

template <typename T> T at(const uint8_t *image, size_t offset)
{
	T value;
	std::memcpy(&value, image + offset, sizeof value);
	return value;
}

}

// xsave and xrstor in the standard format, with XCR0 7 (x87, SSE, AVX): the legacy area as FXSAVE lays it out, the header's
// XSTATE_BV at 512 and the upper halves of YMM0-YMM15 at 576. The requested-feature bitmap is EDX:EAX and XCR0.
int main()
{
	Snippets memory;
	JitCodeCache cache(4 << 20);
	Runtime runtime(memory, cache, TranslatorOptions{});
	std::vector<uint8_t> stack(1 << 14);
	alignas(64) static uint8_t image[1024 + 64];
	const uint64_t xsave = memory.add({0x0f, 0xae, 0x21, 0xc3}), xrstor = memory.add({0x0f, 0xae, 0x29, 0xc3});
	size_t failures = 0;
	auto check = [&](bool good, const char *what) {
		if (!good) {
			std::fprintf(stderr, "FAIL: %s\n", what);
			++failures;
		} else
			std::printf("%s\n", what);
	};
	CpuState &state = runtime.state();
	auto run = [&](uint64_t code, uint32_t features, uint8_t *buffer) {
		state.gpr[rsp] = reinterpret_cast<uint64_t>(stack.data() + stack.size());
		state.gpr[rax] = features;
		state.gpr[rdx] = 0;
		state.gpr[rcx] = reinterpret_cast<uint64_t>(buffer);
		return runtime.call(code).reason;
	};
	auto fill_state = [&]() {
		for (unsigned r = 0; r < 16; ++r) {
			state.xmm[r][0] = 0x1111111111111111ull * (r + 1);
			state.xmm[r][1] = 0x0101010101010101ull * (r + 16);
			state.ymm_high[r][0] = 0x2222222222222222ull ^ r;
			state.ymm_high[r][1] = 0x3333333333333333ull ^ r;
		}
		state.mxcsr = 0x1fa0;
		state.x87.fcw = 0x37a;
		state.x87.fsw = 0x3800;
		state.x87.ftw = 0x81;
		for (unsigned i = 0; i < 8; ++i) {
			state.x87.st[i][0] = 0x8000000000000000ull | i;
			state.x87.st[i][1] = 0x3fff + i;
		}
	};

	fill_state();
	std::memset(image, 0xcc, sizeof image);
	std::memset(image + 512, 0, 64);
	check(run(xsave, 7, image) == ExitReason::returned, "xsave with EDX:EAX 7 returns");
	bool legacy = at<uint16_t>(image, 0) == 0x37a && at<uint16_t>(image, 2) == 0x3800 && image[4] == 0x81 &&
		at<uint32_t>(image, 24) == 0x1fa0 && at<uint32_t>(image, 28) == 0xffff;
	for (unsigned i = 0; i < 8; ++i)
		legacy &= at<uint64_t>(image, 32 + 16 * i) == (0x8000000000000000ull | i) && at<uint16_t>(image, 40 + 16 * i) == 0x3fff + i;
	check(legacy, "xsave: FCW, FSW, the abridged tag word, MXCSR, MXCSR_MASK 0xffff and ST0-ST7 where FXSAVE puts them");
	bool vectors = true;
	for (unsigned r = 0; r < 16; ++r)
		vectors &= at<uint64_t>(image, 160 + 16 * r) == state.xmm[r][0] && at<uint64_t>(image, 168 + 16 * r) == state.xmm[r][1] &&
			at<uint64_t>(image, 576 + 16 * r) == state.ymm_high[r][0] && at<uint64_t>(image, 584 + 16 * r) == state.ymm_high[r][1];
	check(vectors, "xsave: XMM0-XMM15 at 160, the upper halves at 576");
	check(at<uint64_t>(image, 512) == 7 && at<uint64_t>(image, 520) == 0, "xsave: XSTATE_BV 7, XCOMP_BV left as it was");
	check(image[464] == 0xcc && image[1024] == 0xcc, "xsave: the bytes it does not own left as they were");

	std::memset(image, 0xcc, sizeof image);
	std::memset(image + 512, 0, 64);
	check(run(xsave, 2, image) == ExitReason::returned && image[0] == 0xcc && at<uint32_t>(image, 24) == 0x1fa0 &&
			at<uint64_t>(image, 160) == state.xmm[0][0] && image[576] == 0xcc && at<uint64_t>(image, 512) == 2,
		"xsave with EAX 2: SSE and MXCSR alone, XSTATE_BV 2");

	run(xsave, 7, image);
	const CpuState saved = state;
	std::memset(state.xmm, 0, sizeof state.xmm);
	std::memset(state.ymm_high, 0, sizeof state.ymm_high);
	state.mxcsr = 0x1f80;
	state.x87 = X87State{};
	check(run(xrstor, 7, image) == ExitReason::returned && std::memcmp(state.xmm, saved.xmm, sizeof state.xmm) == 0 &&
			std::memcmp(state.ymm_high, saved.ymm_high, sizeof state.ymm_high) == 0 && state.mxcsr == 0x1fa0 &&
			state.x87.fcw == 0x37a && state.x87.fsw == 0x3800 && state.x87.ftw == 0x81 && state.x87.st[3][0] == saved.x87.st[3][0] &&
			state.x87.st[3][1] == saved.x87.st[3][1],
		"xrstor puts back what xsave saved");

	fill_state();
	uint64_t bv = 3;
	std::memcpy(image + 512, &bv, 8);
	check(run(xrstor, 7, image) == ExitReason::returned && state.ymm_high[5][0] == 0 && state.ymm_high[5][1] == 0 &&
			state.xmm[5][0] == saved.xmm[5][0],
		"xrstor: a component requested but clear in XSTATE_BV gets its initial state (the upper halves zero)");
	bv = 7;
	std::memcpy(image + 512, &bv, 8);

	fill_state();
	check(run(xrstor, 1, image) == ExitReason::returned && state.xmm[5][0] == 0x6666666666666666ull && state.mxcsr == 0x1fa0,
		"xrstor with EAX 1: the x87 state alone");

	check(run(xsave, 7, image + 16) == ExitReason::fault, "xsave of an address not aligned to 64 bytes faults (#GP)");
	check(run(xrstor, 7, image + 16) == ExitReason::fault, "xrstor of an address not aligned to 64 bytes faults (#GP)");
	uint64_t compacted = 1;
	std::memcpy(image + 520, &compacted, 8);
	check(run(xrstor, 7, image) == ExitReason::fault, "xrstor with XCOMP_BV set faults (#GP)");
	compacted = 0;
	std::memcpy(image + 520, &compacted, 8);
	bv = 8;
	std::memcpy(image + 512, &bv, 8);
	check(run(xrstor, 7, image) == ExitReason::fault, "xrstor with an XSTATE_BV bit outside XCR0 faults (#GP)");
	bv = 7;
	std::memcpy(image + 512, &bv, 8);
	const uint32_t reserved = 0x10000;
	std::memcpy(image + 24, &reserved, 4);
	check(run(xrstor, 7, image) == ExitReason::fault, "xrstor of an MXCSR with a reserved bit set faults (#GP)");
	std::printf("%zu failures\n", failures);
	return failures == 0 ? 0 : 1;
}
