#include "vcpu.h"

#include <stdexcept>

namespace raven::hvf {

Vcpu::Vcpu(Vm &vm, bool) : vm(vm)
{
	throw std::runtime_error("the vCPU is not implemented");
}

Vcpu::~Vcpu() = default;

uint32_t Vcpu::enter(uint64_t, uint64_t, uint64_t)
{
	throw std::runtime_error("the vCPU is not implemented");
}

uint64_t Vcpu::actlr() const
{
	return 0;
}

uint64_t Vcpu::reg(hv_reg_t) const
{
	return 0;
}

void Vcpu::set_reg(hv_reg_t, uint64_t)
{
}

}
