#include "vm.h"

#include <Hypervisor/Hypervisor.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>

#include <cstring>
#include <stdexcept>
#include <string>

// kernel.S
extern "C" char raven_hvf_vectors[], raven_hvf_vectors_end[], raven_hvf_resume[];
extern "C" char raven_hvf_dispatcher[], raven_hvf_dispatcher_end[];

namespace raven::hvf {

namespace {

// Guest RAM sits at IPA 1 GiB, one stage-1 level-1 block, which is also how the kernel maps all of it in the upper half.
constexpr uint64_t ram_ipa = uint64_t(1) << 30;
constexpr uint64_t ram_limit = uint64_t(1) << 30;
constexpr uint64_t kernel_base = 0xffffff8000000000ull;
constexpr uint64_t kernel_stack_size = 16 << 10;
// EL0 addresses the VM picks itself: the dispatcher at the top of EL0's range, everything else from 256 GiB up, with an unmapped
// gap after each mapping so that running off its end faults.
constexpr uint64_t dispatcher_page = Vm::user_limit - Vm::page_size;
constexpr uint64_t anywhere_base = uint64_t(1) << 38;
constexpr uint64_t anywhere_gap = 64 << 10;

// Stage-1 descriptors, 4 KiB granule. Memory is attribute 0 of MAIR_EL1 (normal, write-back) and inner shareable.
constexpr uint64_t table = 3, page = 3, block = 1;
constexpr uint64_t accessed = 1 << 10, inner_shareable = 3 << 8, el0 = 1 << 6, read_only = 1 << 7;
constexpr uint64_t privileged_never = uint64_t(1) << 53, user_never = uint64_t(1) << 54;

void check(hv_return_t result, const char *what)
{
	if (result != HV_SUCCESS)
		throw std::runtime_error(std::string(what) + " failed: " + std::to_string(static_cast<uint32_t>(result)));
}

uint64_t page_bits(Access access)
{
	const uint64_t common = page | accessed | inner_shareable | el0 | privileged_never;
	return access == Access::read_execute ? common | read_only : common | user_never;
}

uint64_t round_up(uint64_t value)
{
	return (value + Vm::page_size - 1) & ~(Vm::page_size - 1);
}

}

Vm::Vm(size_t size) : ram_size(round_up(size))
{
	if (ram_size > ram_limit)
		throw std::invalid_argument("guest RAM is limited to 1 GiB");
	hv_vm_config_t config = hv_vm_config_create();
	// Stage 2 in 4 KiB pages, the guest's own page size, so that the host can later protect single guest pages.
	const hv_return_t granule = hv_vm_config_set_ipa_granule(config, HV_IPA_GRANULE_4KB);
	const hv_return_t created = granule == HV_SUCCESS ? hv_vm_create(config) : granule;
	os_release(config);
	check(created, "hv_vm_create");
	void *memory = mmap(nullptr, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (memory == MAP_FAILED) {
		hv_vm_destroy();
		throw std::runtime_error("cannot map guest RAM");
	}
	ram = static_cast<uint8_t *>(memory);
	try {
		check(hv_vm_map(ram, ram_ipa, ram_size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC), "hv_vm_map");
		next_physical = ram_ipa;
		next_anywhere = anywhere_base;
		ttbr0 = allocate_physical(page_size);
		ttbr1 = allocate_physical(page_size);
		reinterpret_cast<uint64_t *>(ram + (ttbr1 - ram_ipa))[(kernel_base + ram_ipa) >> 30 & 511] =
			ram_ipa | block | accessed | inner_shareable | user_never;
		const size_t vectors_size = static_cast<size_t>(raven_hvf_vectors_end - raven_hvf_vectors);
		const uint64_t vectors_ipa = allocate_physical(vectors_size);
		std::memcpy(ram + (vectors_ipa - ram_ipa), raven_hvf_vectors, vectors_size);
		sys_icache_invalidate(ram + (vectors_ipa - ram_ipa), vectors_size);
		kernel_vectors = kernel_base + vectors_ipa;
		kernel_resume = kernel_vectors + static_cast<uint64_t>(raven_hvf_resume - raven_hvf_vectors);
		kernel_stack_top = kernel_base + allocate_physical(kernel_stack_size) + kernel_stack_size;
		const size_t dispatcher_size = static_cast<size_t>(raven_hvf_dispatcher_end - raven_hvf_dispatcher);
		uint8_t *dispatcher_alias = map(dispatcher_page, dispatcher_size, Access::read_execute);
		std::memcpy(dispatcher_alias, raven_hvf_dispatcher, dispatcher_size);
		code_written(dispatcher_alias, dispatcher_size);
		dispatcher_address = dispatcher_page;
	} catch (...) {
		hv_vm_destroy();
		munmap(ram, ram_size);
		throw;
	}
}

Vm::~Vm()
{
	hv_vm_unmap(ram_ipa, ram_size);
	hv_vm_destroy();
	munmap(ram, ram_size);
}

uint64_t Vm::allocate_physical(size_t size)
{
	const uint64_t at = next_physical;
	if (round_up(size) > ram_ipa + ram_size - at)
		throw std::runtime_error("guest RAM is full");
	next_physical += round_up(size);
	return at;
}

uint64_t *Vm::table_entry(uint64_t address)
{
	uint64_t ipa = ttbr0;
	for (unsigned shift : {30u, 21u}) {
		uint64_t &entry = reinterpret_cast<uint64_t *>(ram + (ipa - ram_ipa))[address >> shift & 511];
		if (!entry)
			entry = allocate_physical(page_size) | table;
		ipa = entry & 0x0000fffffffff000ull;
	}
	return &reinterpret_cast<uint64_t *>(ram + (ipa - ram_ipa))[address >> 12 & 511];
}

uint8_t *Vm::map(uint64_t address, size_t size, Access access)
{
	const uint64_t length = round_up(size);
	if (address & (page_size - 1) || !length || address >= user_limit || length > user_limit - address)
		throw std::invalid_argument("not a range of EL0 pages");
	std::lock_guard guard(lock);
	const auto after = mappings.lower_bound(address);
	if ((after != mappings.end() && after->first < address + length) ||
		(after != mappings.begin() && std::prev(after)->first + std::prev(after)->second.size > address))
		throw std::runtime_error("guest address range already mapped");
	const uint64_t ipa = allocate_physical(length);
	uint64_t offset = 0;
	try {
		for (; offset < length; offset += page_size)
			*table_entry(address + offset) = (ipa + offset) | page_bits(access);
	} catch (...) {
		// No room for a table: take back the pages already entered. The RAM and the tables stay allocated, as all guest RAM does.
		for (uint64_t entered = 0; entered < offset; entered += page_size)
			*table_entry(address + entered) = 0;
		throw;
	}
	uint8_t *alias = ram + (ipa - ram_ipa);
	mappings[address] = {length, alias};
	return alias;
}

uint64_t Vm::map_anywhere(size_t size, Access access)
{
	uint64_t address;
	{
		std::lock_guard guard(lock);
		address = next_anywhere;
		next_anywhere += round_up(size) + anywhere_gap;
	}
	map(address, size, access);
	return address;
}

uint8_t *Vm::host(uint64_t address) const
{
	std::lock_guard guard(lock);
	auto found = mappings.upper_bound(address);
	if (found == mappings.begin())
		return nullptr;
	--found;
	if (address - found->first >= found->second.size)
		return nullptr;
	return found->second.host + (address - found->first);
}

void Vm::protect(uint64_t address, bool writable)
{
	const uint8_t *alias = host(address & ~(page_size - 1));
	if (!alias)
		throw std::invalid_argument("protecting an unmapped guest page");
	const uint64_t ipa = ram_ipa + static_cast<uint64_t>(alias - ram);
	check(hv_vm_protect(ipa, page_size, writable ? HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC : HV_MEMORY_READ | HV_MEMORY_EXEC),
		"hv_vm_protect");
}

void Vm::read(uint64_t address, void *into, size_t size) const
{
	const uint8_t *alias = host(address);
	if (!alias || (size && host(address + size - 1) != alias + size - 1))
		throw std::runtime_error("guest read outside one mapping");
	std::memcpy(into, alias, size);
}

void Vm::write(uint64_t address, const void *from, size_t size)
{
	uint8_t *alias = host(address);
	if (!alias || (size && host(address + size - 1) != alias + size - 1))
		throw std::runtime_error("guest write outside one mapping");
	std::memcpy(alias, from, size);
}

void Vm::code_written(const uint8_t *alias, size_t size)
{
	// libkern's calls, which know the CPU's line sizes and errata; EL0 cannot read CTR_EL0 on macOS. vm.h says what they do here.
	auto *bytes = const_cast<uint8_t *>(alias);
	if (maintenance.host_clean)
		sys_dcache_flush(bytes, size);
	if (maintenance.host_invalidate)
		sys_icache_invalidate(bytes, size);
	generation.fetch_add(1, std::memory_order_acq_rel);
}

}
