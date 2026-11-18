#pragma once

#include "peparse/peparse.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raven {

struct PEResource;

struct PESection {
	std::string name;
	uint32_t address;
	uint32_t virtual_size;
	uint32_t file_offset;
	uint32_t file_size;
	uint32_t characteristics;
};

struct PEDirectory {
	uint32_t address;
	uint32_t size;
};

struct PEImport {
	std::string module;
	std::string name;
	uint16_t ordinal;
	bool by_ordinal;
	uint32_t iat_address;
};

struct PEExport {
	uint32_t ordinal;
	uint32_t address;
	std::vector<std::string> names;
	std::string forwarder;
};

struct PEDelayImport {
	std::string module;
	uint32_t descriptor_address;
	uint32_t module_handle_address;
	uint32_t iat_address;
	uint32_t lookup_address;
	uint32_t bound_address;
	uint32_t unload_address;
	uint32_t timestamp;
	std::vector<PEImport> imports;
};

// The caller owns the file bytes for the lifetime of this view. The file is read by the Rust reader in native/peparse; every read
// checks its offsets against the bytes and throws std::runtime_error with the reason a file is refused.
class PEImage {
public:
	explicit PEImage(std::span<const uint8_t> bytes);
	std::span<const uint8_t> file_span(uint32_t rva, size_t size) const;
	std::vector<PEImport> imports() const;
	// A .NET image with no native code (COMIMAGE_FLAGS_ILONLY): it runs from the runtime's entry, and its one native import, of that
	// entry, is the runtime's to check rather than the loader's to bind.
	bool il_only() const;
	std::vector<PEDelayImport> delay_imports() const;
	std::vector<PEExport> exports() const;
	std::vector<PEResource> resources() const;

	bool is_64;
	uint16_t machine;
	uint16_t characteristics;
	uint64_t image_base;
	uint64_t stack_reserve;
	// IMAGE_SUBSYSTEM_*: 2 for a windowed program, 3 for a console one.
	uint16_t subsystem;
	uint64_t heap_reserve;
	uint64_t heap_commit;
	uint32_t image_size;
	uint32_t headers_size;
	uint32_t entry;
	uint32_t section_alignment;
	uint32_t file_alignment;
	std::array<PEDirectory, 16> directories{};
	std::vector<PESection> sections;

private:
	std::span<const uint8_t> bytes;
	raven_pe_headers parsed;
	std::string string_at(uint32_t rva) const;
	// The same string seen where the file holds it, for reading a table of names without a copy of each.
	std::string_view text_at(uint32_t rva) const;
};

// A .NET program built for any CPU (a PE32 file with only IL code, neither 32BITREQUIRED nor 32BITPREFERRED) runs as a 64-bit process,
// as on 64-bit Windows: its headers are rewritten in place to the PE32+ form the loader of such a process expects. Its import data stays
// as it was, for the runtime to check. True when the file was rewritten.
bool widen_il_only_image(std::vector<uint8_t> &file);

}
