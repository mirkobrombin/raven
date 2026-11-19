#pragma once

#include "runtime.h"

#include <Hypervisor/Hypervisor.h>

#include <atomic>
#include <cstdint>
#include <functional>

namespace raven::hvf {

class Vm;

// A vCPU of the VM, running translated code at EL0 under the EL0 dispatcher. Hypervisor.framework binds a vCPU to the thread that
// created it: only that thread may construct, enter or destroy it.
class Vcpu final : public translate::CodeRunner {
public:
	// tso sets ACTLR_EL1.EnTSO, so plain loads and stores at EL0 keep x86's order.
	Vcpu(Vm &vm, bool tso);
	~Vcpu() override;
	Vcpu(const Vcpu &) = delete;
	Vcpu &operator=(const Vcpu &) = delete;
	// Test seam: replaces ID_AA64MMFR1_EL1 as the vCPU reports it, to simulate a CPU without FEAT_AFP. The constructor refuses a vCPU
	// whose AFP field (bits 47:44) is zero: translated SSE code needs FPCR.AH.
	static std::function<uint64_t(uint64_t)> id_aa64mmfr1_override;

	// Resumes the dispatcher, which enters code through trampoline with the CpuState at state; returns once translated code exits.
	uint32_t enter(uint64_t trampoline, uint64_t state, uint64_t code) override;
	// A store that faulted on watched code this many times in a row, each time after its translations were dropped, is performed by
	// the host instead (Vm::emulate_store), so it ends however often other vCPUs translate the page again.
	static constexpr unsigned store_retries = 8;
	// From another thread: forces an exit with hv_vcpus_exit and waits for it, unless the vCPU is not in the guest.
	void quiesce() override;
	bool restores_host_fpcr() const override { return false; }
	// On this vCPU's thread: stops every other vCPU of the VM (hv_vcpus_exit, then the gate keeps them out of the guest), runs work
	// and lets them go on.
	void exclusive(const std::function<void()> &work) override;

	hv_vcpu_t id() const { return vcpu; }
	// ACTLR_EL1 as the vCPU reads it back.
	uint64_t actlr() const;
	uint64_t reg(hv_reg_t reg) const;
	void set_reg(hv_reg_t reg, uint64_t value);
	// Called on this vCPU's thread whenever another thread made it exit with hv_vcpus_exit; it then resumes.
	std::function<void(Vcpu &)> on_cancel;

	struct Stats {
		uint64_t entries = 0;
		uint64_t cancels = 0;
		uint64_t instruction_cache_invalidations = 0;
		uint64_t write_faults = 0;
		uint64_t emulated_stores = 0;
	};
	const Stats &stats() const { return counters; }

private:
	struct Mailbox;

	Vm &vm;
	hv_vcpu_t vcpu = 0;
	hv_vcpu_exit_t *exit = nullptr;
	Mailbox *mailbox = nullptr;
	uint64_t mailbox_address = 0;
	uint64_t stack_top = 0;
	uint64_t seen_generation = 0;
	bool started = false;
	uint64_t faulting_pc = 0, faulting_address = 0;
	unsigned faults_in_a_row = 0;
	Stats counters;
	std::atomic<bool> in_guest{false};
	std::atomic<uint64_t> exits{0};
};

}
