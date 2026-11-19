// CPUID and XGETBV as translated x64 code sees them in a vCPU: the same rules as in this process (cpuid_check.h).
#include "cpuid_check.h"
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <cstdio>
#include <cstring>
#include <exception>

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
	static constexpr size_t size = 1 << 20;
	hvf::Vm &vm;
	uint64_t base;
	uint8_t *alias;
	size_t used = 0;
};

}

int main()
{
	size_t failures = 0;
	try {
		hvf::Vm vm(64 << 20);
		Snippets memory(vm);
		const uint64_t stack = vm.map_anywhere(64 << 10, hvf::Access::read_write);
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		hvf::VmCodeCache cache(vm, 16 << 20, 1 << 20);
		hvf::Vcpu vcpu(vm, true);
		translate::Runtime runtime(memory, cache, options, &vcpu);
		failures = translate::cpuid_check::check(
			[&](const std::vector<uint8_t> &code) {
				const uint64_t address = memory.add(code);
				translate::CpuState &state = runtime.state();
				std::memset(state.gpr, 0, sizeof state.gpr);
				state.gpr[translate::rsp] = stack + (64 << 10);
				const translate::RunResult result = runtime.call(address);
				return translate::cpuid_check::Answer{result.reason, static_cast<uint32_t>(state.gpr[translate::rax]),
					static_cast<uint32_t>(state.gpr[translate::rbx]), static_cast<uint32_t>(state.gpr[translate::rcx]),
					static_cast<uint32_t>(state.gpr[translate::rdx])};
			},
			"vCPU");
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	std::printf("%zu failures\n", failures);
	return failures == 0 ? 0 : 1;
}
