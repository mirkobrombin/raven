#pragma once

#include "code_cache.h"
#include "guest_memory.h"
#include "vm.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <utility>
#include <vector>

namespace raven::translate {
class Runtime;
}

namespace raven::hvf {

// Test-only process model inside the VM, as TestProcess is in this process: x64 images mapped at their preferred base in guest
// memory, a stack and a TEB per thread. Images keep no imports, since native thunks cannot run in a vCPU yet.
class VmProcess final : public translate::GuestMemory {
public:
	explicit VmProcess(Vm &vm) : vm(vm) {}

	// Maps the image at its preferred base; returns it.
	uint64_t load(const std::filesystem::path &path);
	uint64_t entry(uint64_t base) const;
	// A guest stack with RSP at its top and a TEB whose address gs: reads, for the thread runtime runs.
	void prepare_thread(translate::Runtime &runtime);

	std::span<const uint8_t> code(uint64_t address) const override;
	bool is_native(uint64_t) const override { return false; }
	void write(uint64_t address, const void *bytes, size_t size) override;
	// Not watched yet: the translated code would need the image's pages read-only to EL0 and the kernel to pass the fault on.
	void watch_code(uint64_t, size_t, bool) override {}
	size_t watch_granule() const override { return Vm::page_size; }

private:
	struct Image {
		uint64_t base;
		uint32_t entry;
		std::vector<std::pair<uint64_t, uint64_t>> code_ranges;
	};
	Vm &vm;
	std::vector<Image> images;
};

// Translated code in guest memory: EL0 runs it at its guest address, the host writes it through its alias.
class VmCodeCache final : public translate::CodeAllocator {
public:
	VmCodeCache(Vm &vm, size_t code_capacity, size_t data_capacity);

	uint8_t *allocate(size_t size) override;
	void write(uint8_t *at, const void *bytes, size_t size) override;
	void read(const uint8_t *at, void *bytes, size_t size) const override;
	Data allocate_data(size_t size) override;
	void free_data(Data data) override;
	void reset() override { next = 0; }
	size_t used() const override { return next; }
	size_t capacity() const override { return code_size; }
	bool contains(const uint8_t *address) const override;

private:
	Vm &vm;
	uint64_t code_base;
	uint8_t *code_alias;
	size_t code_size;
	size_t next = 0;
	uint64_t data_base;
	uint8_t *data_alias;
	size_t data_size;
	size_t data_next = 0;
	// Offsets of the blocks in use and their sizes, and of those given back by size: a runtime asks for the same two sizes each time.
	std::map<size_t, size_t> data_used;
	std::multimap<size_t, size_t> data_free;

	uint8_t *alias(const uint8_t *at) const;
};

}
