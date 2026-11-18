#include "runtime.h"

#include <stdexcept>

namespace raven::translate {

Runtime::Runtime(GuestMemory &memory, CodeAllocator &cache, TranslatorOptions options) : memory(memory), cache(cache), options(options)
{
}

Runtime::~Runtime() = default;

Runtime *Runtime::current()
{
	return nullptr;
}

void Runtime::bind_native(uint64_t, const void *, unsigned)
{
}

RunResult Runtime::call(uint64_t, std::initializer_list<uint64_t>)
{
	throw std::runtime_error("the runtime is not written yet");
}

void Runtime::request_stop(uint64_t)
{
}

void Runtime::code_written(uint64_t)
{
}

void Runtime::save_cache(const std::filesystem::path &, uint64_t) const
{
}

size_t Runtime::load_cache(const std::filesystem::path &, uint64_t)
{
	return 0;
}

uint64_t Runtime::cache_key(std::span<const uint8_t>, uint64_t, const TranslatorOptions &)
{
	return 0;
}

}
