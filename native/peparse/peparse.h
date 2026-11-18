#pragma once

// The PE reader in Rust (native/peparse): every function returns NULL on success or the reason the file was refused, a string that
// lives as long as the program. The headers raven_pe_parse fills are handed back with the same bytes to every later read.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAVEN_PE_MAX_SECTIONS 96

struct raven_pe_directory {
	uint32_t address;
	uint32_t size;
};

struct raven_pe_section {
	uint8_t name[8];
	uint8_t name_size;
	uint32_t address;
	uint32_t virtual_size;
	uint32_t file_offset;
	uint32_t file_size;
	uint32_t characteristics;
};

struct raven_pe_headers {
	bool is_64;
	uint16_t machine;
	uint16_t characteristics;
	uint64_t image_base;
	uint64_t stack_reserve;
	uint16_t subsystem;
	uint64_t heap_reserve;
	uint64_t heap_commit;
	uint32_t image_size;
	uint32_t headers_size;
	uint32_t entry;
	uint32_t section_alignment;
	uint32_t file_alignment;
	struct raven_pe_directory directories[16];
	uint32_t section_count;
	struct raven_pe_section sections[RAVEN_PE_MAX_SECTIONS];
};

struct raven_pe_text {
	const uint8_t *data;
	size_t size;
};

struct raven_pe_import {
	struct raven_pe_text module;
	// Empty for an import by ordinal.
	struct raven_pe_text name;
	uint16_t ordinal;
	bool by_ordinal;
	uint32_t iat_address;
};

struct raven_pe_delay_descriptor {
	struct raven_pe_text module;
	uint32_t descriptor_address;
	uint32_t module_handle_address;
	uint32_t iat_address;
	uint32_t lookup_address;
	uint32_t bound_address;
	uint32_t unload_address;
	uint32_t timestamp;
};

struct raven_pe_export {
	uint32_t ordinal;
	uint32_t address;
	// Empty unless the export forwards to another library's.
	struct raven_pe_text forwarder;
};

// A resource type or name: `id` when `name.data` is NULL, otherwise `name.size` bytes of little-endian UTF-16.
struct raven_pe_key {
	uint32_t id;
	struct raven_pe_text name;
};

struct raven_pe_resource {
	struct raven_pe_key type;
	struct raven_pe_key name;
	uint32_t language;
	uint32_t entry_address;
	uint32_t data_address;
	uint32_t size;
	uint32_t code_page;
};

// A caller's function returns false to stop the read, which then returns raven_pe_stopped().
typedef bool (*raven_pe_import_found)(void *context, const struct raven_pe_import *import);
typedef bool (*raven_pe_descriptor_found)(void *context, const struct raven_pe_delay_descriptor *descriptor);
typedef bool (*raven_pe_export_found)(void *context, const struct raven_pe_export *symbol);
// The names come in the order of the name table, each with the index of the export it names.
typedef bool (*raven_pe_name_found)(void *context, uint32_t index, struct raven_pe_text name);
typedef bool (*raven_pe_resource_found)(void *context, const struct raven_pe_resource *resource);
typedef void (*raven_pe_range_read)(void *context, uint64_t offset, uint64_t size);

const char *raven_pe_parse(const uint8_t *data, size_t size, struct raven_pe_headers *headers);
// The offset in the file of the `length` bytes at the image address `rva`.
const char *raven_pe_file_span(const uint8_t *data, size_t size, const struct raven_pe_headers *headers, uint32_t rva,
	uint64_t length, size_t *offset);
// The zero-terminated string at `rva`, without its terminator.
const char *raven_pe_text_at(const uint8_t *data, size_t size, const struct raven_pe_headers *headers, uint32_t rva,
	struct raven_pe_text *text);
const char *raven_pe_il_only(const uint8_t *data, size_t size, const struct raven_pe_headers *headers, bool *il_only);
const char *raven_pe_imports(const uint8_t *data, size_t size, const struct raven_pe_headers *headers, void *context,
	raven_pe_import_found each);
// Each descriptor comes before its imports.
const char *raven_pe_delay_imports(const uint8_t *data, size_t size, const struct raven_pe_headers *headers, void *context,
	raven_pe_descriptor_found descriptor, raven_pe_import_found each);
// The exports in ordinal order, then their names.
const char *raven_pe_exports(const uint8_t *data, size_t size, const struct raven_pe_headers *headers, void *context,
	raven_pe_export_found each, raven_pe_name_found named);
// `record_size` is what each resource costs the caller before its names, for the limit on what a file may make it hold.
const char *raven_pe_resources(const uint8_t *data, size_t size, const struct raven_pe_headers *headers, uint64_t record_size,
	void *context, raven_pe_resource_found each);
// The resource tree of a directory whose bytes are given: directory offsets are relative to its root, payload addresses
// image-relative. `read`, which may be NULL, receives every range of the directory the walk read.
const char *raven_pe_read_resources(struct raven_pe_directory directory, const uint8_t *data, size_t size, uint32_t image_size,
	uint64_t record_size, void *context, raven_pe_resource_found each, raven_pe_range_read read);
bool raven_pe_widen_il_only(uint8_t *data, size_t size);
const char *raven_pe_stopped(void);

#ifdef __cplusplus
}
static_assert(sizeof(raven_pe_headers) == 3272 && sizeof(raven_pe_section) == 32, "the layout the Rust side declares");
#endif
