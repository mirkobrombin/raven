#include "platform.h"

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

// Apple silicon allows writable and executable memory only as MAP_JIT, switched between the two per thread
// (https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon). A hardened-runtime build
// needs the com.apple.security.cs.allow-jit entitlement for it.
namespace raven::translate::host {

CodeRegion map_code(size_t size)
{
	void *memory = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (memory == MAP_FAILED)
		throw std::runtime_error("cannot map JIT memory");
	auto *bytes = static_cast<uint8_t *>(memory);
	return {bytes, bytes, size};
}

void unmap_code(CodeRegion &region)
{
	if (region.execute)
		munmap(region.execute, region.size);
	region = {};
}

void open_code_writes()
{
	pthread_jit_write_protect_np(0);
}

void close_code_writes()
{
	pthread_jit_write_protect_np(1);
}

void flush_instruction_cache(void *start, size_t size)
{
	sys_icache_invalidate(start, size);
}

size_t page_size()
{
	return static_cast<size_t>(getpagesize());
}

bool hardware_tso()
{
	// No public interface turns on TSO for a native macOS thread; Rosetta's own threads are the only ones that run with it.
	return false;
}

}
