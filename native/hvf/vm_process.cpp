#include "vm_process.h"

#include "runtime.h"
#include "test_process.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace raven::hvf {

VmProcess::VmProcess(Vm &vm) : vm(vm)
{
	vm.write_fault = [this](uint64_t address) { return code_write(address); };
}

VmProcess::~VmProcess()
{
	vm.write_fault = nullptr;
}

void VmProcess::attach(translate::Runtime &runtime)
{
	std::lock_guard guard(lock);
	if (std::find(attached.begin(), attached.end(), &runtime) == attached.end())
		attached.push_back(&runtime);
}

void VmProcess::detach(translate::Runtime &runtime)
{
	std::lock_guard guard(lock);
	std::erase(attached, &runtime);
}

void VmProcess::drop_translations(uint64_t address)
{
	std::vector<translate::Runtime *> runtimes;
	{
		std::lock_guard guard(lock);
		runtimes = attached;
	}
	// Outside the lock: code_written takes each runtime's lock and calls watch_code.
	for (translate::Runtime *runtime : runtimes)
		runtime->code_written(address);
}

void VmProcess::watch_code(uint64_t address, size_t size, bool watch)
{
	std::lock_guard guard(lock);
	if (watch && attached.empty())
		throw std::logic_error("translating code of a VmProcess no runtime is attached to");
	for (uint64_t at = address & ~(Vm::page_size - 1); at < address + size; at += Vm::page_size) {
		unsigned &count = watched[at];
		if (watch && count++ == 0)
			vm.protect(at, false);
		else if (!watch && count && --count == 0) {
			vm.protect(at, true);
			watched.erase(at);
		} else if (!count)
			watched.erase(at);
	}
}

bool VmProcess::code_write(uint64_t address)
{
	const uint64_t page = address & ~(Vm::page_size - 1);
	if (!vm.host(address))
		return false;
	{
		std::lock_guard guard(lock);
		// Another store's fault on this page was handled first and unwatched it: this store just runs again.
		if (!watched.count(page))
			return true;
	}
	drop_translations(address);
	if (after_code_written)
		after_code_written(address);
	// code_written unwatched the page through watch_code. If a vCPU translated it again meanwhile, it is watched again and the retried
	// store faults again, dropping that translation too: making the page writable here would let the store miss it.
	return true;
}

uint64_t VmProcess::load(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open " + path.string());
	const std::vector<uint8_t> file{std::istreambuf_iterator<char>(input), {}};
	const PEImage pe(file);
	if (!pe.is_64 || pe.machine != 0x8664)
		throw std::runtime_error(path.string() + " is not an x64 image");
	// The VM's address space is the guest's own, so the image always gets its preferred base and needs no relocation.
	uint8_t *bytes = vm.map(pe.image_base, pe.image_size, Access::read_write);
	images.push_back({pe.image_base, pe.entry, translate::lay_out_image(pe, file, bytes, pe.image_base)});
	return pe.image_base;
}

uint64_t VmProcess::entry(uint64_t base) const
{
	for (const Image &image : images)
		if (image.base == base)
			return image.base + image.entry;
	throw std::runtime_error("no image at that base");
}

void VmProcess::prepare_thread(translate::Runtime &runtime)
{
	prepare_thread(runtime.state());
}

void VmProcess::prepare_thread(translate::CpuState &state)
{
	constexpr size_t stack_size = 1 << 20;
	const uint64_t stack = vm.map_anywhere(stack_size, Access::read_write);
	const uint64_t teb = vm.map_anywhere(Vm::page_size, Access::read_write);
	// NT_TIB: stack base and limit, and the TEB's own address, which gs:[0x30] reads.
	const uint64_t top = stack + stack_size;
	vm.write(teb + 0x8, &top, 8);
	vm.write(teb + 0x10, &stack, 8);
	vm.write(teb + 0x30, &teb, 8);
	state.gpr[translate::rsp] = top;
	state.gs_base = teb;
}

std::span<const uint8_t> VmProcess::code(uint64_t address) const
{
	for (const Image &image : images)
		for (const auto &[start, end] : image.code_ranges)
			if (address >= start && address < end)
				return {vm.host(address), static_cast<size_t>(end - address)};
	return {};
}

void VmProcess::write(uint64_t address, const void *bytes, size_t size)
{
	// Through the alias, past stage 2: a write the runtime makes to watched code drops its translations as a guest store would.
	vm.write(address, bytes, size);
	for (uint64_t page = address & ~(Vm::page_size - 1); page < address + size; page += Vm::page_size) {
		bool code;
		{
			std::lock_guard guard(lock);
			code = watched.count(page) != 0;
		}
		if (code)
			drop_translations(page);
	}
}

VmCodeCache::VmCodeCache(Vm &vm, size_t code_capacity, size_t data_capacity)
	: vm(vm), code_base(vm.map_anywhere(code_capacity, Access::read_execute)), code_alias(vm.host(code_base)),
	  code_size(code_capacity), data_base(vm.map_anywhere(data_capacity, Access::read_write)), data_alias(vm.host(data_base)),
	  data_size(data_capacity)
{
}

uint8_t *VmCodeCache::alias(const uint8_t *at) const
{
	return code_alias + (reinterpret_cast<uint64_t>(at) - code_base);
}

uint8_t *VmCodeCache::allocate(size_t size)
{
	const size_t start = (next + 15) & ~size_t(15);
	if (start + size > code_size)
		return nullptr;
	next = start + size;
	return reinterpret_cast<uint8_t *>(code_base + start);
}

void VmCodeCache::write(uint8_t *at, const void *bytes, size_t size)
{
	std::memcpy(alias(at), bytes, size);
	vm.code_written(alias(at), size);
}

void VmCodeCache::read(const uint8_t *at, void *bytes, size_t size) const
{
	std::memcpy(bytes, alias(at), size);
}

translate::CodeAllocator::Data VmCodeCache::allocate_data(size_t size)
{
	size = (size + 15) & ~size_t(15);
	size_t start;
	if (const auto reused = data_free.find(size); reused != data_free.end()) {
		start = reused->second;
		data_free.erase(reused);
		std::memset(data_alias + start, 0, size);
	} else {
		if (size > data_size - data_next)
			throw std::runtime_error("the VM's runtime data is full");
		start = data_next;
		data_next += size;
	}
	data_used[start] = size;
	return {data_alias + start, data_base + start};
}

void VmCodeCache::free_data(Data data)
{
	const auto used = data_used.find(data.address - data_base);
	if (used == data_used.end())
		return;
	data_free.emplace(used->second, used->first);
	data_used.erase(used);
}

bool VmCodeCache::contains(const uint8_t *address) const
{
	const uint64_t at = reinterpret_cast<uint64_t>(address);
	return at >= code_base && at < code_base + code_size;
}

}
