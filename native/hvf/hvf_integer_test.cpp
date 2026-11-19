// The integer oracle's cases (native/translate/x86_semantics.py) translated and run at EL0 in a vCPU: they must leave what x86 left,
// held to an x86 CPU where Rosetta differs, as they do in this process (x86_semantics_test). The code, the buffer and the stack are
// guest memory; translated code runs with hardware TSO.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"
#include "x86_semantics_check.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
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

}

int main(int argc, char **argv)
{
	if (argc != 3) {
		std::fprintf(stderr, "usage: hvf_integer_test ORACLE_RESULTS SILICON_RESULTS\n");
		return 2;
	}
	std::vector<translate::integer_oracle::Case> cases = translate::integer_oracle::read_cases(argv[1]);
	if (cases.empty() || translate::integer_oracle::apply_silicon(cases, argv[2]) < 0) {
		std::fprintf(stderr, "cannot read %s or apply %s\n", argv[1], argv[2]);
		return 2;
	}
	translate::integer_oracle::Report report;
	report.first = cases.data();
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
		for (const translate::integer_oracle::Case &item : cases) {
			const uint64_t address = memory.add(item.code);
			translate::CpuState &state = runtime.state();
			translate::integer_oracle::load(state, item.input);
			std::memcpy(buffer_alias, item.input.buffer, sizeof item.input.buffer);
			state.gpr[translate::rsp] = stack + (64 << 10);
			state.gpr[translate::r15] = buffer;
			const translate::RunResult result = runtime.call(address);
			std::string problem;
			if (result.reason != translate::ExitReason::returned)
				problem = "did not return (exit " + std::to_string(static_cast<unsigned>(result.reason)) + ")";
			else
				problem = translate::integer_oracle::compare(item, state, buffer_alias);
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
