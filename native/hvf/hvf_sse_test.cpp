// The SSE oracle's cases (native/translate/x86_sse.py) translated and run at EL0 in a vCPU: they must leave what x86 left, as they do
// in this process (x86_sse_test). The code, the buffer and the stack are guest memory; translated code runs with hardware TSO.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"
#include "x86_sse_check.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

using namespace raven;

namespace {

// The cases' x64 code in guest memory; nothing runs it but the translator, which reads it through the host alias.
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
	void watch_code(uint64_t, size_t, bool) override {}
	size_t watch_granule() const override { return hvf::Vm::page_size; }

private:
	static constexpr size_t size = 4 << 20;
	hvf::Vm &vm;
	uint64_t base;
	uint8_t *alias;
	size_t used = 0;
};

}

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: hvf_sse_test ORACLE_RESULTS\n");
		return 2;
	}
	const std::vector<translate::sse_oracle::Case> cases = translate::sse_oracle::read_cases(argv[1]);
	if (cases.empty()) {
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	translate::sse_oracle::Report report;
	try {
		hvf::Vm vm(256 << 20);
		Snippets memory(vm);
		const uint64_t buffer = vm.map_anywhere(hvf::Vm::page_size, hvf::Access::read_write);
		uint8_t *buffer_alias = vm.host(buffer);
		const uint64_t stack = vm.map_anywhere(64 << 10, hvf::Access::read_write);
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		hvf::VmCodeCache cache(vm, 64 << 20, 1 << 20);
		hvf::Vcpu vcpu(vm, true);
		translate::Runtime runtime(memory, cache, options, &vcpu);
		for (const translate::sse_oracle::Case &item : cases) {
			const uint64_t address = memory.add(item.code);
			translate::CpuState &state = runtime.state();
			translate::sse_oracle::load(state, item.input);
			std::memcpy(buffer_alias, item.input.buffer, sizeof item.input.buffer);
			state.gpr[translate::rsp] = stack + (64 << 10);
			state.gpr[translate::r15] = buffer;
			const translate::RunResult result = runtime.call(address);
			std::string problem;
			if (result.reason != translate::ExitReason::returned)
				problem = "did not return (exit " + std::to_string(static_cast<unsigned>(result.reason)) + ")";
			else
				problem = translate::sse_oracle::compare(item, state, buffer_alias);
			report.add(item, problem);
		}
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	report.print("vCPU");
	std::printf("%zu cases, %zu failures\n", cases.size(), report.failures);
	return report.failures == 0 ? 0 : 1;
}
