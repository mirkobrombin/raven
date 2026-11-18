#include "vm.h"

#include <stdexcept>

namespace raven::hvf {

Vm::Vm(size_t)
{
	throw std::runtime_error("the VM is not implemented");
}

Vm::~Vm() = default;

uint8_t *Vm::map(uint64_t, size_t, Access)
{
	throw std::runtime_error("the VM is not implemented");
}

uint64_t Vm::map_anywhere(size_t, Access)
{
	throw std::runtime_error("the VM is not implemented");
}

uint8_t *Vm::host(uint64_t) const
{
	return nullptr;
}

void Vm::read(uint64_t, void *, size_t) const
{
	throw std::runtime_error("the VM is not implemented");
}

void Vm::write(uint64_t, const void *, size_t)
{
	throw std::runtime_error("the VM is not implemented");
}

void Vm::code_written(const uint8_t *, size_t)
{
}

}
