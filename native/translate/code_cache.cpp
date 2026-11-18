#include "code_cache.h"

#include <cstring>

namespace raven::translate {

JitCodeCache::JitCodeCache(size_t capacity) : region(host::map_code(capacity))
{
}

JitCodeCache::~JitCodeCache()
{
	host::unmap_code(region);
}

uint8_t *JitCodeCache::allocate(size_t size)
{
	const size_t start = (next + 15) & ~size_t(15);
	if (start + size > region.size)
		return nullptr;
	next = start + size;
	return region.execute + start;
}

void JitCodeCache::write(uint8_t *at, const void *bytes, size_t size)
{
	host::open_code_writes();
	std::memcpy(region.write + (at - region.execute), bytes, size);
	host::close_code_writes();
	host::flush_instruction_cache(at, size);
}

void JitCodeCache::reset()
{
	next = 0;
}

}
