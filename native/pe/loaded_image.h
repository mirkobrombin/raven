#pragma once

#include "pe.h"

namespace raven {

struct ImageMapping {
	uint64_t address;
	uint64_t size;
	uint64_t file_offset;
	int protection;
};

// Owns shared storage; guest mappings must stop using its descriptor before destruction.
class LoadedImage {
public:
	// An image mapped only to be read keeps its contents as they are wherever it lands: nothing in it runs.
	LoadedImage(const PEImage &image, uint64_t base, bool relocated = true);
	~LoadedImage();
	LoadedImage(LoadedImage &&other) noexcept;
	LoadedImage &operator=(LoadedImage &&other) noexcept;
	LoadedImage(const LoadedImage &) = delete;
	LoadedImage &operator=(const LoadedImage &) = delete;
	std::span<uint8_t> data();
	std::span<const uint8_t> data() const;
	std::vector<ImageMapping> mappings() const;
	int descriptor() const;
	uint64_t base() const;
	size_t relocation_count() const;

private:
	int fd = -1;
	uint8_t *storage = nullptr;
	size_t size = 0;
	uint64_t address = 0;
	size_t relocated = 0;
	std::vector<int> protections;
	void relocate(const PEImage &image);
	void release();
};

}
