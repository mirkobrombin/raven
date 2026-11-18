#include "x86_decoder.h"

namespace raven::translate {

Instruction decode(std::span<const uint8_t>, uint64_t)
{
	return {};
}

std::string format_intel(const Instruction &)
{
	return {};
}

}
