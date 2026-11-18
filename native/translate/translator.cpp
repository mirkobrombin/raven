#include "translator.h"

#include <stdexcept>

namespace raven::translate {

TranslatedBlock translate_block(std::span<const uint8_t>, uint64_t, const TranslatorOptions &, TranslationStats &)
{
	throw std::runtime_error("the translator is not written yet");
}

std::vector<uint32_t> enter_trampoline()
{
	return {};
}

std::vector<uint32_t> exit_trampoline()
{
	return {};
}

std::vector<uint32_t> native_thunk(uint64_t, unsigned)
{
	return {};
}

std::vector<uint32_t> return_stub()
{
	return {};
}

}
