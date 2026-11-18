#pragma once

#include "platform.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace raven::translate {

// Where translated code goes. The translator never writes code memory itself: it hands finished words to an allocator, so the same
// translator emits into a JIT region of this process, into a VM's memory, or into a buffer that becomes an ahead-of-time cache.
class CodeAllocator {
public:
	virtual ~CodeAllocator() = default;
	// Execute address of size bytes, 16-byte aligned, or nullptr when full.
	virtual uint8_t *allocate(size_t size) = 0;
	// Copies bytes to the code at execute address at and makes them visible to instruction fetch.
	virtual void write(uint8_t *at, const void *bytes, size_t size) = 0;
	// Copies size bytes of the code at execute address at into bytes.
	virtual void read(const uint8_t *at, void *bytes, size_t size) const = 0;
	// Memory translated code reads and writes besides guest memory: the CpuState and the lookup table. host is where the runtime
	// reaches it, address where translated code does; the two are the same in this process and differ in a VM. Zeroed, 16-byte
	// aligned, and kept until the allocator goes, whatever reset does.
	struct Data {
		void *host;
		uint64_t address;
	};
	virtual Data allocate_data(size_t size) = 0;
	virtual void reset() = 0;
	virtual size_t used() const = 0;
	virtual size_t capacity() const = 0;
	// Whether a direct branch from one address in the cache can reach any other (AArch64 b reaches 128 MiB).
	virtual bool contains(const uint8_t *address) const = 0;
};

class JitCodeCache final : public CodeAllocator {
public:
	explicit JitCodeCache(size_t capacity);
	~JitCodeCache() override;
	JitCodeCache(const JitCodeCache &) = delete;
	JitCodeCache &operator=(const JitCodeCache &) = delete;
	uint8_t *allocate(size_t size) override;
	void write(uint8_t *at, const void *bytes, size_t size) override;
	void read(const uint8_t *at, void *bytes, size_t size) const override;
	Data allocate_data(size_t size) override;
	void reset() override;
	size_t used() const override { return next; }
	size_t capacity() const override { return region.size; }
	bool contains(const uint8_t *address) const override { return address >= region.execute && address < region.execute + region.size; }

private:
	host::CodeRegion region;
	size_t next = 0;
	std::vector<std::unique_ptr<uint64_t[]>> data;
};

}
