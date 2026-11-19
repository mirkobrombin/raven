// Two vCPUs run translated x64 that adds to one 32-bit word in guest memory a million times each, with lock add, sub, inc, dec and
// xadd, and counts the updates lost: the word must end at what the additions sum to. lock neg (a compare-and-swap loop) runs as many
// times on a word of 5, which an even number of negations leaves at 5. The same loop with a plain inc is the control, which must lose
// updates, or the vCPUs did not run at the same time and a zero proves nothing.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <barrier>
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
	constexpr uint32_t iterations = 1000000;
	const std::vector<Loop> loops = {
		{"lock inc dword ptr [rdi]", {0xf0, 0xff, 0x07}, 1},
		{"lock dec dword ptr [rdi]", {0xf0, 0xff, 0x0f}, -1},
		{"lock add dword ptr [rdi], 3", {0xf0, 0x83, 0x07, 0x03}, 3},
		{"lock sub dword ptr [rdi], eax", {0xf0, 0x29, 0x07}, -1},
		{"mov eax, 1; lock xadd dword ptr [rdi], eax", {0xb8, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x0f, 0xc1, 0x07}, 1},
		{"lock neg dword ptr [rdi]", {0xf0, 0xf7, 0x1f}, 0},
		{"inc dword ptr [rdi] (no lock, the control)", {0xff, 0x07}, 1, true},
	};
	int failures = 0;
	try {
		hvf::Vm vm(256 << 20);
		Snippets memory(vm);
		const uint64_t shared = vm.map_anywhere(hvf::Vm::page_size, hvf::Access::read_write);
		auto *word = reinterpret_cast<volatile int32_t *>(vm.host(shared));
		for (const Loop &loop : loops) {
			const uint64_t entry = memory.add(loop_code(loop.body));
			*word = loop.step ? 0 : 5;
			std::barrier start(2);
			auto thread = [&](unsigned) {
				try {
					hvf::Vcpu vcpu(vm, true);
					hvf::VmCodeCache cache(vm, 4 << 20, 1 << 20);
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
