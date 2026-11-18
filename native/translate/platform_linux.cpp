#include "platform.h"

#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

// Linux keeps code writable and executable at two addresses of one memfd, so no mapping is ever both; switching costs nothing.
namespace raven::translate::host {

CodeRegion map_code(size_t size)
{
	const int fd = memfd_create("raven-translated-code", MFD_CLOEXEC);
	if (fd < 0)
		throw std::runtime_error("cannot create the code memfd");
	if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
		close(fd);
		throw std::runtime_error("cannot size the code memfd");
	}
	void *execute = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
	void *write = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (execute == MAP_FAILED || write == MAP_FAILED)
		throw std::runtime_error("cannot map the code memfd");
	return {static_cast<uint8_t *>(execute), static_cast<uint8_t *>(write), size};
}

void unmap_code(CodeRegion &region)
{
	if (region.execute) {
		munmap(region.execute, region.size);
		munmap(region.write, region.size);
	}
	region = {};
}

void open_code_writes()
{
}

void close_code_writes()
{
}

void flush_instruction_cache(void *start, size_t size)
{
	auto *begin = static_cast<char *>(start);
	__builtin___clear_cache(begin, begin + size);
}

size_t page_size()
{
	return static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

bool hardware_tso()
{
	// Apple's Linux patch adds PR_SET_MEM_MODEL for Apple silicon; upstream kernels have no such switch.
	return false;
}

}
