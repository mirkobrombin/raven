// Two vCPUs run translated x64 that adds to one 32-bit word in guest memory a million times each, with lock add, sub, inc, dec and
// xadd, and counts the updates lost: the word must end at what the additions sum to. lock neg (a compare-and-swap loop) and lock not
// run 16 trials each on a word of 5, which an even number of them leaves at 5; a lost one shows with odds of a half per trial. The
// same loop with a plain inc is the control, which must lose updates, or the vCPUs did not run at the same time and a zero proves
// nothing.
//
// The bit forms (lock or and and, xor, bts and btr, btc) work on disjoint bits of one word, a bit per vCPU: each sets its bit, checks
// that it is set, clears it and checks that it is clear. Only that vCPU changes its bit, so with atomic operations no check can fail;
// a read-modify-write that is not atomic writes back the other vCPU's bit as it was, and that vCPU's check sees it. Plain or and and
// are the control.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace raven;

namespace {

// The loops' x64 code in guest memory; nothing runs it but the translator, which reads it through the host alias.
class Snippets final : public translate::GuestMemory {
public:
	explicit Snippets(hvf::Vm &vm) : vm(vm), base(vm.map_anywhere(size, hvf::Access::read_write)), alias(vm.host(base)) {}
	uint64_t add(const std::vector<uint8_t> &code)
	{
		const uint64_t address = base + used;
		std::memcpy(alias + used, code.data(), code.size());
		used += (code.size() + 15) & ~size_t(15);
		return address;
	}
	std::span<const uint8_t> code(uint64_t address) const override
	{
		if (address < base || address >= base + used)
			return {};
		return {alias + (address - base), static_cast<size_t>(base + used - address)};
	}
	bool is_native(uint64_t) const override { return false; }
	void write(uint64_t address, const void *from, size_t bytes) override { vm.write(address, from, bytes); }
	void read(uint64_t address, void *into, size_t bytes) const override { vm.read(address, into, bytes); }
	void watch_code(uint64_t, size_t, bool) override {}
	size_t watch_granule() const override { return hvf::Vm::page_size; }

private:
	static constexpr size_t size = 4 << 20;
	hvf::Vm &vm;
	uint64_t base;
	uint8_t *alias;
	size_t used = 0;
};

struct Loop {
	const char *text;
	// The instruction on [rdi], run ecx times.
	std::vector<uint8_t> body;
	// What one run adds to the word; 0 for neg, which must leave the word at 5.
	int64_t step;
	bool control = false;
};

// mov eax, 1; loop: BODY; dec ecx; jnz loop; ret.
std::vector<uint8_t> loop_code(const std::vector<uint8_t> &body)
{
	std::vector<uint8_t> code = {0xb8, 0x01, 0x00, 0x00, 0x00};
	code.insert(code.end(), body.begin(), body.end());
	code.insert(code.end(), {0xff, 0xc9, 0x75, static_cast<uint8_t>(-static_cast<int>(body.size() + 4)), 0xc3});
	return code;
}

}

int main()
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	constexpr uint32_t iterations = 1000000;
	const std::vector<Loop> loops = {
		{"lock inc dword ptr [rdi]", {0xf0, 0xff, 0x07}, 1},
		{"lock dec dword ptr [rdi]", {0xf0, 0xff, 0x0f}, -1},
		{"lock add dword ptr [rdi], 3", {0xf0, 0x83, 0x07, 0x03}, 3},
		{"lock sub dword ptr [rdi], eax", {0xf0, 0x29, 0x07}, -1},
		{"mov eax, 1; lock xadd dword ptr [rdi], eax", {0xb8, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x0f, 0xc1, 0x07}, 1},
		{"inc dword ptr [rdi] (no lock, the control)", {0xff, 0x07}, 1, true},
	};
	// The bit loops on one word, a bit per vCPU: SET; xchg [rsp - 8], r9; test [rdi], eax; jnz +3; inc r8; CLEAR; xchg; test
	// [rdi], eax; jz +3; inc r8; dec ecx; jnz; ret. EAX holds the vCPU's bit, EDX its complement, ESI its index; R8 counts the failed
	// checks. The xchg (a full barrier) makes the test read the word as the other vCPU sees it, not the vCPU's own store.
	struct BitLoop {
		const char *text;
		std::vector<uint8_t> set, clear;
		bool control = false;
	};
	const std::vector<BitLoop> bit_loops = {
		{"lock or and lock and", {0xf0, 0x09, 0x07}, {0xf0, 0x21, 0x17}},
		{"lock xor twice", {0xf0, 0x31, 0x07}, {0xf0, 0x31, 0x07}},
		{"lock bts and lock btr", {0xf0, 0x0f, 0xab, 0x37}, {0xf0, 0x0f, 0xb3, 0x37}},
		{"lock btc twice", {0xf0, 0x0f, 0xbb, 0x37}, {0xf0, 0x0f, 0xbb, 0x37}},
		{"or and and (no lock, the control)", {0x09, 0x07}, {0x21, 0x17}, true},
	};
	auto bit_code = [](const BitLoop &loop) {
		const std::vector<uint8_t> fence = {0x4c, 0x87, 0x4c, 0x24, 0xf8};
		std::vector<uint8_t> code = loop.set;
		code.insert(code.end(), fence.begin(), fence.end());
		code.insert(code.end(), {0x85, 0x07, 0x75, 0x03, 0x49, 0xff, 0xc0});
		code.insert(code.end(), loop.clear.begin(), loop.clear.end());
		code.insert(code.end(), fence.begin(), fence.end());
		code.insert(code.end(), {0x85, 0x07, 0x74, 0x03, 0x49, 0xff, 0xc0, 0xff, 0xc9});
		code.insert(code.end(), {0x75, static_cast<uint8_t>(-static_cast<int>(code.size() + 2)), 0xc3});
		return code;
	};
	int failures = 0;
	try {
		hvf::Vm vm(uint64_t(1) << 30);
		Snippets memory(vm);
		const uint64_t shared = vm.map_anywhere(hvf::Vm::page_size, hvf::Access::read_write);
		auto *word = reinterpret_cast<volatile int32_t *>(vm.host(shared));
		// Runs entry on both vCPUs from one start, each with RCX iterations and its registers from setup; returns R8 of each.
		auto run_pair = [&](uint64_t entry0, uint64_t entry1, uint64_t count, auto setup) {
			std::barrier start(2);
			uint64_t r8[2] = {};
			auto thread = [&](unsigned role) {
				try {
					hvf::Vcpu vcpu(vm, true);
					hvf::VmCodeCache cache(vm, 1 << 20, 1 << 18);
					translate::TranslatorOptions options;
					options.ordering = translate::MemoryOrdering::hardware;
					translate::Runtime runtime(memory, cache, options, &vcpu);
					const uint64_t stack = vm.map_anywhere(64 << 10, hvf::Access::read_write);
					translate::CpuState &state = runtime.state();
					state.gpr[translate::rsp] = stack + (64 << 10);
					state.gpr[translate::rdi] = shared;
					state.gpr[translate::rcx] = count;
					state.gpr[translate::r8] = 0;
					setup(role, state);
					const uint64_t entry = role ? entry1 : entry0;
					runtime.translate_ahead(entry);
					start.arrive_and_wait();
					if (runtime.call(entry).reason != translate::ExitReason::returned)
						throw std::runtime_error("the loop did not return");
					r8[role] = state.gpr[translate::r8];
				} catch (const std::exception &error) {
					std::fprintf(stderr, "FAIL: %s\n", error.what());
					std::_Exit(1);
				}
			};
			std::thread first(thread, 0u), second(thread, 1u);
			first.join();
			second.join();
			return r8[0] + r8[1];
		};
		auto run_both = [&](uint64_t entry, uint64_t count, auto setup) { return run_pair(entry, entry, count, setup); };
		// Split locks: operands across a 16-byte boundary of the page (offsets from shared), which the translator cannot do with one
		// LSE atomic. Fewer iterations: each goes through the runtime.
		constexpr uint32_t split_iterations = 20000;
		struct Split {
			const char *text;
			std::vector<uint8_t> body;
			unsigned offset, size;
		};
		const std::vector<Split> splits = {
			{"lock inc word ptr [rdi] at 15", {0xf0, 0x66, 0xff, 0x07}, 15, 2},
			{"lock inc dword ptr [rdi] at 14", {0xf0, 0xff, 0x07}, 14, 4},
			{"lock inc qword ptr [rdi] at 12", {0xf0, 0x48, 0xff, 0x07}, 12, 8},
			{"mov eax, 1; lock xadd dword ptr [rdi], eax at 13", {0xb8, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x0f, 0xc1, 0x07}, 13, 4},
			// An increment by compare and swap: mov eax, [rdi]; retry: lea edx, [rax + 1]; lock cmpxchg [rdi], edx; jne retry.
			{"lock cmpxchg dword ptr [rdi], edx (an increment loop) at 14", {0x8b, 0x07, 0x8d, 0x50, 0x01, 0xf0, 0x0f, 0xb1, 0x17, 0x75, 0xf7},
				14, 4},
			// A spinlock on the split word guarding a plain counter at [rsi]: retry: xchg [rdi], eax; test eax, eax; jnz retry;
			// inc dword ptr [rsi]; mov dword ptr [rdi], 0; mov eax, 1.
			{"xchg dword ptr [rdi], eax (a spinlock around inc dword ptr [rsi]) at 14", {0x87, 0x07, 0x85, 0xc0, 0x75, 0xfa, 0xff,
				0x06, 0xc7, 0x07, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00}, 14, 4},
		};
		auto *page = reinterpret_cast<volatile uint8_t *>(vm.host(shared));
		for (const Split &split : splits) {
			const uint64_t entry = memory.add(loop_code(split.body));
			for (unsigned i = 0; i < 64; ++i)
				page[i] = 0;
			const auto began = std::chrono::steady_clock::now();
			run_both(entry, split_iterations, [&](unsigned, translate::CpuState &state) {
				state.gpr[translate::rdi] = shared + split.offset;
				state.gpr[translate::rsi] = shared + 32;
			});
			// The time per split operation, both vCPUs together, setup and translation included: a rough cost, not a benchmark.
			const double per = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count() /
				(2.0 * split_iterations);
			std::printf("%s: %.2f us per operation (two vCPUs, rough)\n", split.text, per);
			uint64_t value = 0;
			const bool spinlock = split.body[0] == 0x87;
			std::memcpy(&value, const_cast<uint8_t *>(page) + (spinlock ? 32 : split.offset), spinlock ? 4 : split.size);
			const int64_t lost = int64_t(2 * split_iterations) - int64_t(value);
			std::printf("%s: %lld of %u updates lost%s\n", split.text, static_cast<long long>(lost), 2 * split_iterations,
				lost ? " (FAIL)" : "");
			failures += lost != 0;
		}
		{
			// One vCPU adds 1 << 48 to the aligned quadword at 8 (bytes 14 and 15 its top 16 bits) with lock add; the other lock incs
			// the split doubleword at 14. Both add 1 to the 16 bits at 14 each time, below 65536 in all.
			const uint64_t aligned = memory.add(loop_code({0x48, 0xc7, 0xc2, 0x00, 0x00, 0x00, 0x00, 0x48, 0xba, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x01, 0x00, 0xf0, 0x48, 0x01, 0x17}));
			const uint64_t split = memory.add(loop_code({0xf0, 0xff, 0x07}));
			for (unsigned i = 0; i < 64; ++i)
				page[i] = 0;
			run_pair(aligned, split, split_iterations, [&](unsigned role, translate::CpuState &state) {
				state.gpr[translate::rdi] = shared + (role ? 14 : 8);
			});
			uint16_t value = 0;
			std::memcpy(&value, const_cast<uint8_t *>(page) + 14, 2);
			const int64_t lost = int64_t(2 * split_iterations) - int64_t(value);
			std::printf("lock add qword ptr [8], 1 << 48 against lock inc dword ptr [14]: %lld of %u updates lost%s\n",
				static_cast<long long>(lost), 2 * split_iterations, lost ? " (FAIL)" : "");
			failures += lost != 0;
		}
		for (const BitLoop &loop : bit_loops) {
			const uint64_t entry = memory.add(bit_code(loop));
			*word = 0;
			const uint64_t failed = run_both(entry, iterations, [](unsigned role, translate::CpuState &state) {
				state.gpr[translate::rax] = 1u << role;
				state.gpr[translate::rdx] = ~(1u << role) & 0xffffffffu;
				state.gpr[translate::rsi] = role;
			});
			const bool ok = loop.control ? failed > 0 : failed == 0;
			std::printf("%s: %llu of %u checks failed%s\n", loop.text, static_cast<unsigned long long>(failed), 4 * iterations,
				ok ? "" : loop.control ? " (FAIL: the control failed none, so the vCPUs did not overlap)" : " (FAIL)");
			failures += !ok;
		}
		// lock neg and lock not, 16 trials each.
		for (const auto &[text, body] : {std::pair<const char *, std::vector<uint8_t>>{"lock neg dword ptr [rdi]", {0xf0, 0xf7, 0x1f}},
				 {"lock not dword ptr [rdi]", {0xf0, 0xf7, 0x17}}}) {
			const uint64_t entry = memory.add(loop_code(body));
			unsigned wrong = 0;
			for (int trial = 0; trial < 16; ++trial) {
				*word = 5;
				run_both(entry, 200000, [](unsigned, translate::CpuState &) {});
				wrong += *word != 5;
			}
			std::printf("%s: %u of 16 trials of 400000 left the word other than 5%s\n", text, wrong, wrong ? " (FAIL)" : "");
			failures += wrong != 0;
		}
		for (const Loop &loop : loops) {
			if (!loop.step)
				continue;
			const uint64_t entry = memory.add(loop_code(loop.body));
			*word = loop.step ? 0 : 5;
			std::barrier start(2);
			auto thread = [&](unsigned) {
				try {
					hvf::Vcpu vcpu(vm, true);
					hvf::VmCodeCache cache(vm, 1 << 20, 1 << 18);
					translate::TranslatorOptions options;
					options.ordering = translate::MemoryOrdering::hardware;
					translate::Runtime runtime(memory, cache, options, &vcpu);
					const uint64_t stack = vm.map_anywhere(64 << 10, hvf::Access::read_write);
					translate::CpuState &state = runtime.state();
					state.gpr[translate::rsp] = stack + (64 << 10);
					state.gpr[translate::rdi] = shared;
					state.gpr[translate::rcx] = iterations;
					// Translate before the start, so both loops begin together.
					runtime.translate_ahead(entry);
					start.arrive_and_wait();
					if (runtime.call(entry).reason != translate::ExitReason::returned)
						throw std::runtime_error("the loop did not return");
				} catch (const std::exception &error) {
					std::fprintf(stderr, "FAIL: %s\n", error.what());
					std::_Exit(1);
				}
			};
			std::thread first(thread, 0u), second(thread, 1u);
			first.join();
			second.join();
			if (!loop.step) {
				const bool ok = *word == 5;
				std::printf("%s: the word is %d after %u negations%s\n", loop.text, *word, 2 * iterations, ok ? "" : " (FAIL)");
				failures += !ok;
				continue;
			}
			const int64_t expected = static_cast<int32_t>(static_cast<uint32_t>(2 * iterations * loop.step));
			const int64_t lost = (expected - *word) / loop.step;
			const bool ok = loop.control ? lost > 0 : lost == 0;
			std::printf("%s: %lld of %u updates lost%s\n", loop.text, static_cast<long long>(lost), 2 * iterations,
				ok ? "" : loop.control ? " (FAIL: the control lost none, so the vCPUs did not overlap)" : " (FAIL)");
			failures += !ok;
		}
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	return failures == 0 ? 0 : 1;
}
