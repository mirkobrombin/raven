#include "vcpu.h"

#include "vm.h"

#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace raven::hvf {

// One page of EL0 memory per vCPU that the dispatcher reads before entering code. kernel.S knows the offsets.
struct Vcpu::Mailbox {
	uint64_t state;
	uint64_t code;
	uint64_t trampoline;
	uint32_t invalidate_instruction_cache;
	uint32_t reserved;
};

namespace {

constexpr uint64_t stack_size = 64 << 10;

// ESR exception classes.
constexpr uint64_t hvc_class = 0x16, lower_data_abort_class = 0x24;
// ISS of a data abort: WnR, S1PTW (the fault came from a stage-1 table walk), FnV, and the DFSC. A store to a page hv_vm_protect made
// read-only arrives as a translation fault at level 3 (DFSC 0b000111, syndrome 0x93880047 here), not as a permission fault
// (0b0011xx), so both count.
constexpr uint64_t write_not_read = 1 << 6, table_walk = 1 << 7, far_not_valid = 1 << 10;
// ISV: the syndrome describes the access, a single register's load or store without writeback: SAS (log2 of its size), SRT (its
// register, 31 being xzr).
constexpr uint64_t syndrome_valid = 1 << 24;

bool stage2_store_fault(uint64_t syndrome)
{
	const uint64_t status = syndrome & 0x3c;
	return syndrome >> 26 == lower_data_abort_class && syndrome & write_not_read && !(syndrome & (table_walk | far_not_valid)) &&
		(status == 0x04 || status == 0x0c);
}

void check(hv_return_t result, const char *what)
{
	if (result != HV_SUCCESS)
		throw std::runtime_error(std::string(what) + " failed: " + std::to_string(static_cast<uint32_t>(result)));
}

// TCR_EL1: 39-bit halves (T0SZ = T1SZ = 25), 4 KiB granules, walks write-back and inner shareable, 36-bit IPA.
constexpr uint64_t tcr = 25 | (1 << 8) | (1 << 10) | (3 << 12) | (0 << 14) | (25 << 16) | (1 << 24) | (1 << 26) | (3ull << 28) |
	(2ull << 30) | (1ull << 32);
// SCTLR_EL1: RES1 bits, the MMU, data and instruction caches, and EL0 reads of CTR_EL0.
constexpr uint64_t sctlr = 0x30d00800ull | 1 | (1 << 2) | (1 << 12) | (1 << 15);
// CPACR_EL1.FPEN: FP and SIMD at EL0 and EL1, which the enter and exit trampolines use for v8-v15.
constexpr uint64_t cpacr = 3 << 20;
// EL0t and EL1h with D, A, I and F masked.
constexpr uint64_t el0t = 0x3c0, el1h = 0x3c5;
constexpr uint64_t actlr_tso = 1 << 1;

}

Vcpu::Vcpu(Vm &vm, bool tso) : vm(vm)
{
	static_assert(offsetof(Mailbox, invalidate_instruction_cache) == 24);
	check(hv_vcpu_create(&vcpu, &exit, nullptr), "hv_vcpu_create");
	try {
		mailbox_address = vm.map_anywhere(sizeof(Mailbox), Access::read_write);
		mailbox = reinterpret_cast<Mailbox *>(vm.host(mailbox_address));
		stack_top = vm.map_anywhere(stack_size, Access::read_write) + stack_size;
		check(hv_vcpu_set_vtimer_mask(vcpu, true), "hv_vcpu_set_vtimer_mask");
		const std::pair<hv_sys_reg_t, uint64_t> registers[] = {
			{HV_SYS_REG_MAIR_EL1, 0xff},
			{HV_SYS_REG_TCR_EL1, tcr},
			{HV_SYS_REG_TTBR0_EL1, vm.user_tables()},
			{HV_SYS_REG_TTBR1_EL1, vm.kernel_tables()},
			{HV_SYS_REG_VBAR_EL1, vm.vectors()},
			// One kernel stack for every vCPU: the kernel never touches sp yet. A handler that does needs one stack per vCPU.
			{HV_SYS_REG_SP_EL1, vm.kernel_stack()},
			{HV_SYS_REG_SP_EL0, stack_top},
			{HV_SYS_REG_CPACR_EL1, cpacr},
			{HV_SYS_REG_SCTLR_EL1, sctlr},
			{HV_SYS_REG_ACTLR_EL1, tso ? actlr_tso : 0},
			// The first run starts in the kernel's return path, which drops to the dispatcher at EL0.
			{HV_SYS_REG_ELR_EL1, vm.dispatcher()},
			{HV_SYS_REG_SPSR_EL1, el0t},
		};
		for (const auto &[reg, value] : registers)
			check(hv_vcpu_set_sys_reg(vcpu, reg, value), "hv_vcpu_set_sys_reg");
		check(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, el1h), "hv_vcpu_set_reg");
		check(hv_vcpu_set_reg(vcpu, HV_REG_PC, vm.resume()), "hv_vcpu_set_reg");
		check(hv_vcpu_set_reg(vcpu, HV_REG_X19, mailbox_address), "hv_vcpu_set_reg");
	} catch (...) {
		hv_vcpu_destroy(vcpu);
		throw;
	}
}

Vcpu::~Vcpu()
{
	hv_vcpu_destroy(vcpu);
}

void Vcpu::quiesce()
{
	if (!in_guest.load())
		return;
	// hv_vcpus_exit on a vCPU that is not running yet makes its next hv_vcpu_run return at once (Hypervisor/hv_vcpu.h), which covers
	// a thread between in_guest and hv_vcpu_run. The request is sent again while waiting, in case that is ever not so.
	const uint64_t seen = exits.load();
	for (unsigned round = 0; in_guest.load() && exits.load() == seen; ++round) {
		if (round % 1024 == 0)
			hv_vcpus_exit(&vcpu, 1);
		std::this_thread::yield();
	}
}

uint64_t Vcpu::actlr() const
{
	uint64_t value = 0;
	check(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ACTLR_EL1, &value), "hv_vcpu_get_sys_reg");
	return value;
}

uint64_t Vcpu::reg(hv_reg_t reg) const
{
	uint64_t value = 0;
	check(hv_vcpu_get_reg(vcpu, reg, &value), "hv_vcpu_get_reg");
	return value;
}

void Vcpu::set_reg(hv_reg_t reg, uint64_t value)
{
	check(hv_vcpu_set_reg(vcpu, reg, value), "hv_vcpu_set_reg");
}

uint32_t Vcpu::enter(uint64_t trampoline, uint64_t state, uint64_t code)
{
	mailbox->state = state;
	mailbox->code = code;
	mailbox->trampoline = trampoline;
	const uint64_t generation = vm.code_generation();
	if (generation != seen_generation || !started) {
		seen_generation = generation;
		if (vm.maintenance.guest_invalidate) {
			mailbox->invalidate_instruction_cache = 1;
			++counters.instruction_cache_invalidations;
		}
	}
	started = true;
	++counters.entries;
	for (;;) {
		in_guest.store(true);
		const hv_return_t ran = hv_vcpu_run(vcpu);
		exits.fetch_add(1);
		in_guest.store(false);
		check(ran, "hv_vcpu_run");
		if (exit->reason == HV_EXIT_REASON_CANCELED) {
			++counters.cancels;
			if (on_cancel)
				on_cancel(*this);
			continue;
		}
		if (exit->reason != HV_EXIT_REASON_EXCEPTION)
			throw std::runtime_error("vCPU exit " + std::to_string(static_cast<unsigned>(exit->reason)));
		const uint64_t syndrome = exit->exception.syndrome;
		if (syndrome >> 26 == hvc_class && (syndrome & 0xffff) == 0) {
			// Faults in a row are those of one execution of a store: a store that went through leaves no trace, so count from here.
			faults_in_a_row = 0;
			return static_cast<uint32_t>(reg(HV_REG_X0));
		}
		// A store to a page the host protected at stage 2: the PC still names the store, which runs again on resume.
		if (stage2_store_fault(syndrome) && vm.write_fault && vm.write_fault(exit->exception.virtual_address)) {
			++counters.write_faults;
			const uint64_t pc = reg(HV_REG_PC), address = exit->exception.virtual_address;
			faults_in_a_row = pc == faulting_pc && address == faulting_address ? faults_in_a_row + 1 : 1;
			faulting_pc = pc;
			faulting_address = address;
			// Other vCPUs translated the page again before every retry: do the store here and step over it. A store the syndrome does not
			// describe (STP, writeback, atomics) is retried without bound.
			if (faults_in_a_row >= store_retries && syndrome & syndrome_valid && vm.emulate_store) {
				const unsigned size = 1u << ((syndrome >> 22) & 3), source = (syndrome >> 16) & 31;
				const uint64_t value = source == 31 ? 0 : reg(static_cast<hv_reg_t>(HV_REG_X0 + source));
				vm.emulate_store(address, &value, size);
				set_reg(HV_REG_PC, pc + 4);
				faults_in_a_row = 0;
				++counters.emulated_stores;
			}
			continue;
		}
		faults_in_a_row = 0;
		uint64_t esr = 0, far = 0, elr = 0;
		hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr);
		hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &far);
		hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr);
		char message[192];
		std::snprintf(message, sizeof message, "guest exception: exit syndrome %#llx, ESR_EL1 %#llx, FAR_EL1 %#llx, ELR_EL1 %#llx",
			static_cast<unsigned long long>(syndrome), static_cast<unsigned long long>(esr), static_cast<unsigned long long>(far),
			static_cast<unsigned long long>(elr));
		throw std::runtime_error(message);
	}
}

}
