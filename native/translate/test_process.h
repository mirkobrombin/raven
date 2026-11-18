#pragma once

#include "guest_memory.h"

#include "pe/pe.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace raven::translate {

class Runtime;

// Copies the headers and sections of an x64 image to bytes, laid out as the loader lays it out; returns its code sections as
// [start, end) guest addresses for an image based at base.
std::vector<std::pair<uint64_t, uint64_t>> lay_out_image(const PEImage &pe, std::span<const uint8_t> file, uint8_t *bytes, uint64_t base);

// Test-only process model: x64 images mapped into this process at their guest addresses, imports bound to native functions of this
// process, and code pages write-protected so writes to them reach the runtime. It stands in for the loader until the process model is
// chosen; it is not how Raven will load programs.
class TestProcess final : public GuestMemory {
public:
	struct Native {
		const void *function;
		unsigned integer_arguments;
	};

	TestProcess();
	~TestProcess() override;
	TestProcess(const TestProcess &) = delete;
	TestProcess &operator=(const TestProcess &) = delete;

	// Maps the image at its preferred base, or elsewhere when the base is taken and the image may move; returns where.
	uint64_t load(const std::filesystem::path &path);
	// Binds imports by name to natives; every import of every loaded image must have one. Call after attaching the runtime.
	void bind_imports(const std::map<std::string, Native> &natives);
	void attach(Runtime &runtime);
	uint64_t entry(uint64_t base) const;
	// A guest stack with RSP at its top, 16-byte aligned less the 8 a call leaves, and a TEB whose address gs: reads.
	void prepare_thread();
	const std::vector<uint8_t> &file(uint64_t base) const;

	std::span<const uint8_t> code(uint64_t address) const override;
	bool is_native(uint64_t address) const override;
	void write(uint64_t address, const void *bytes, size_t size) override;
	void watch_code(uint64_t address, size_t size, bool watch) override;
	size_t watch_granule() const override;

	// A write fault the signal handler passes on: true when it was a watched code page, now writable again.
	bool code_write(uint64_t address);

private:
	struct Image {
		uint64_t base;
		uint64_t size;
		uint32_t entry;
		std::vector<uint8_t> file;
		std::vector<std::pair<uint64_t, uint64_t>> code_ranges;
	};
	std::vector<Image> images;
	std::map<uint64_t, Native> natives;
	std::map<uint64_t, unsigned> watched;
	Runtime *runtime = nullptr;
	uint8_t *stack = nullptr;
	size_t stack_size = 0;
	uint8_t *teb = nullptr;
};

}
