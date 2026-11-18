#include "vm_process.h"

#include <stdexcept>

namespace raven::hvf {

uint64_t VmProcess::load(const std::filesystem::path &)
{
	throw std::runtime_error("the VM process is not implemented");
}

uint64_t VmProcess::entry(uint64_t) const
{
	throw std::runtime_error("the VM process is not implemented");
}

void VmProcess::prepare_thread(translate::Runtime &)
{
	throw std::runtime_error("the VM process is not implemented");
}

std::span<const uint8_t> VmProcess::code(uint64_t) const
{
	return {};
}

void VmProcess::write(uint64_t, const void *, size_t)
{
	throw std::runtime_error("the VM process is not implemented");
}

VmCodeCache::VmCodeCache(Vm &vm, size_t, size_t) : vm(vm), code_base(0), code_alias(nullptr), code_size(0), data_base(0),
	data_alias(nullptr), data_size(0)
{
	throw std::runtime_error("the VM code cache is not implemented");
}

uint8_t *VmCodeCache::allocate(size_t)
{
	return nullptr;
}

void VmCodeCache::write(uint8_t *, const void *, size_t)
{
}

void VmCodeCache::read(const uint8_t *, void *, size_t) const
{
}

translate::CodeAllocator::Data VmCodeCache::allocate_data(size_t)
{
	return {};
}

bool VmCodeCache::contains(const uint8_t *) const
{
	return false;
}

}
