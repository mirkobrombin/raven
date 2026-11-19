#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <condition_variable>
#include <mutex>
#include <vector>

// The process's Hypervisor.framework VM: guest RAM, the guest kernel and the page tables EL0 runs under. Guest addresses are 39-bit
// virtual addresses of EL0 (TTBR0, 4 KiB pages); the kernel lives in the upper half (TTBR1), where all guest RAM is mapped once.
namespace raven::hvf {

// How EL0 may use a mapping. The kernel's own mapping is apart and never reachable from EL0.
enum class Access : uint8_t { read_write, read_execute };

// What makes code the host writes through its alias visible to the vCPUs' instruction fetch. The M5 reports CTR_EL0.IDC = 1 (no data
// cache clean needed), DIC = 0 (instruction cache invalidation needed) and a PIPT instruction cache, so invalidating by the host
// alias reaches the lines the guest fetched through its own address (hvf_code_write_test). Tests switch parts off as controls.
struct CodeMaintenance {
	// sys_dcache_flush on the host alias, which on macOS 27.2.1 is only DSB ISH (disassembled): with CTR_EL0.IDC = 1 there is no line
	// to clean.
	bool host_clean = false;
	// sys_icache_invalidate on the host alias: on macOS 27.2.1, DSB ISH, IC IVAU per 64-byte line, DSB ISH, ISB (disassembled); IC
	// IVAU is broadcast to the inner shareable domain.
	bool host_invalidate = true;
	// IC IALLUIS in the guest kernel before a vCPU returns to EL0 after code was written: for a CPU whose instruction cache is not
	// PIPT, where the host's alias would not name the guest's lines. Only the vCPU that next enters invalidates, so with several
	// vCPUs one that keeps running may fetch stale code until its next exit.
	bool guest_invalidate = false;
};

class Vm {
public:
	static constexpr uint64_t page_size = 4096;
	// The highest EL0 address plus one: T0SZ = 25.
	static constexpr uint64_t user_limit = uint64_t(1) << 39;

	// Creates the VM with ram_size bytes of guest RAM. Hypervisor.framework allows one VM per process.
	explicit Vm(size_t ram_size);
	~Vm();
	Vm(const Vm &) = delete;
	Vm &operator=(const Vm &) = delete;

	// Maps [address, address + size) for EL0 to fresh zeroed guest RAM; returns the host alias of address. The range must be free.
	uint8_t *map(uint64_t address, size_t size, Access access);
	// Changes how EL0 may use mapped pages: for parts of a mapping, such as an image's native code. No TLB maintenance, so only before
	// any vCPU has used them.
	void set_access(uint64_t address, size_t size, Access access);
	// The same at an address the VM picks; returns that address.
	uint64_t map_anywhere(size_t size, Access access);
	// The guest physical address (IPA) behind a mapped guest address.
	uint64_t physical(uint64_t address) const;
	// The host alias of a guest address, or nullptr when it is not mapped. Valid up to the end of the mapping holding it.
	uint8_t *host(uint64_t address) const;
	// Takes write permission away from the guest page holding address at stage 2 (writable false), or gives it back. A guest store to a
	// page without it exits to the host on the storing vCPU's thread, which calls write_fault.
	void protect(uint64_t address, bool writable);
	// Handles such a store at address; true when the store can be retried. Called for stage-2 store faults only (translation or
	// permission, not from a table walk).
	std::function<bool(uint64_t address)> write_fault;
	// Performs, for a vCPU, a store that kept faulting (Vcpu::store_retries): writes bytes at address as the guest store would have.
	std::function<void(uint64_t address, const void *bytes, size_t size)> emulate_store;
	// Copies between guest memory and the host; throws when the range is not mapped.
	void read(uint64_t address, void *into, size_t size) const;
	void write(uint64_t address, const void *from, size_t size);

	// Called after the host wrote code at alias: cleans or invalidates there as maintenance says and tells the vCPUs.
	void code_written(const uint8_t *alias, size_t size);
	uint64_t code_generation() const { return generation.load(std::memory_order_acquire); }

	// The gate every vCPU of the VM passes before it runs the guest, which Vcpu::exclusive closes to stop them all: while stopping,
	// none enters, and running counts those inside. vcpus lists every vCPU alive, for exclusive to make them exit.
	struct World {
		std::mutex lock;
		std::condition_variable changed;
		bool stopping = false;
		unsigned running = 0;
		std::vector<class Vcpu *> vcpus;
	};
	World world;
	CodeMaintenance maintenance;

	// What a vCPU starts with: the kernel's vectors and stack, the page table roots, the EL0 dispatcher.
	uint64_t vectors() const { return kernel_vectors; }
	// Where the kernel returns to EL0 after an exit, invalidating the instruction cache first when asked to.
	uint64_t resume() const { return kernel_resume; }
	uint64_t kernel_stack() const { return kernel_stack_top; }
	uint64_t user_tables() const { return ttbr0; }
	uint64_t kernel_tables() const { return ttbr1; }
	uint64_t dispatcher() const { return dispatcher_address; }

private:
	struct Mapping {
		uint64_t size;
		uint8_t *host;
	};

	uint8_t *ram = nullptr;
	size_t ram_size = 0;
	uint64_t next_physical = 0;
	uint64_t next_anywhere = 0;
	uint64_t ttbr0 = 0, ttbr1 = 0;
	uint64_t kernel_vectors = 0, kernel_resume = 0, kernel_stack_top = 0, dispatcher_address = 0;
	std::map<uint64_t, Mapping> mappings;
	mutable std::mutex lock;
	std::atomic<uint64_t> generation{0};

	uint64_t allocate_physical(size_t size);
	uint64_t *table_entry(uint64_t address);
};

}
