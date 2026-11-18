#pragma once

#include "code_cache.h"
#include "cpu_state.h"
#include "guest_memory.h"
#include "vm.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
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
	explicit VmProcess(Vm &vm);
	~VmProcess() override;

	// Maps the image at its preferred base; returns it.
	uint64_t load(const std::filesystem::path &path);
	uint64_t entry(uint64_t base) const;
	// Runtimes whose translations a write to watched code invalidates: every runtime that translates this process's code must be
	// attached, from before its first translation until it goes, or a store could wait on a translation no write releases.
	void attach(translate::Runtime &runtime);
	void detach(translate::Runtime &runtime);
	// attach for the life of a scope.
	class Attachment {
	public:
		Attachment(VmProcess &process, translate::Runtime &runtime) : process(process), runtime(runtime) { process.attach(runtime); }
		~Attachment() { process.detach(runtime); }
		Attachment(const Attachment &) = delete;
		Attachment &operator=(const Attachment &) = delete;

	private:
		VmProcess &process;
		translate::Runtime &runtime;
	};
	// Test seam: runs on the storing vCPU's thread after a write fault's code_written and before the store is retried, where another
	// vCPU may translate the page again.
	std::function<void(uint64_t address)> after_code_written;
	// A guest stack with RSP at its top and a TEB whose address gs: reads, for the thread runtime runs.
	void prepare_thread(translate::Runtime &runtime);
	void prepare_thread(translate::CpuState &state);

	std::span<const uint8_t> code(uint64_t address) const override;
	bool is_native(uint64_t) const override { return false; }
	void write(uint64_t address, const void *bytes, size_t size) override;
	// A watched page loses write permission at stage 2; a guest store there reaches every attached runtime's code_written. Refuses to
	// watch with no runtime attached.
	void watch_code(uint64_t address, size_t size, bool watch) override;
	size_t watch_granule() const override { return Vm::page_size; }

private:
	struct Image {
		uint64_t base;
		uint32_t entry;
		std::vector<std::pair<uint64_t, uint64_t>> code_ranges;
	};
	Vm &vm;
	std::vector<Image> images;
	std::vector<translate::Runtime *> attached;
	std::mutex lock;
	std::map<uint64_t, unsigned> watched;

	bool code_write(uint64_t address);
	void drop_translations(uint64_t address);
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
