/* The cabinet file format as the public Microsoft Cabinet Format specification lays it out, and the decoders' interface. */

#ifndef RAVEN_CABINET_FORMAT_H
#define RAVEN_CABINET_FORMAT_H

#include <stddef.h>
#include <stdint.h>

enum {
	header_size = 36,
	folder_entry_size = 8,
	file_entry_size = 16,
	block_header_size = 8,
	cabinet_signature = 0x4643534d,
	flag_has_previous = 1,
	flag_has_next = 2,
	flag_has_reserve = 4,
	folder_from_previous = 0xfffd,
	folder_to_next = 0xfffe,
	folder_both_ways = 0xffff,
	type_none = 0,
	type_mszip = 1,
	type_quantum = 2,
	type_lzx = 3,
	block_output_limit = 32768,
	/* A block never holds more than this much compressed data, the worst case of the three compressors plus their framing. */
	block_input_limit = 32768 + 6144,
	attribute_executable = 0x40,
	attribute_utf8_name = 0x80,
};

static inline uint32_t get_16(const unsigned char *data)
{
	return data[0] | (uint32_t)data[1] << 8;
}

static inline uint32_t get_32(const unsigned char *data)
{
	return get_16(data) | get_16(data + 2) << 16;
}

static inline void put_16(unsigned char *data, uint32_t value)
{
	data[0] = (unsigned char)value;
	data[1] = (unsigned char)(value >> 8);
}

static inline void put_32(unsigned char *data, uint32_t value)
{
	put_16(data, value & 0xffff);
	put_16(data + 2, value >> 16);
}

/* The specification's rolling sum; a block's stored value also covers the two size words of its header. */
uint32_t block_checksum(const unsigned char *data, size_t size, uint32_t seed);

typedef void *(*decoder_alloc)(size_t size, void *context);
typedef void (*decoder_free)(void *memory, void *context);

/* Both decoders keep their history across blocks of one folder. A decode call consumes one whole block and produces exactly `produce` bytes. */
struct lzx_decoder;
struct lzx_decoder *lzx_create(unsigned window_bits, decoder_alloc alloc, void *context);
void lzx_destroy(struct lzx_decoder *decoder, decoder_free release, void *context);
int lzx_decode(struct lzx_decoder *decoder, const unsigned char *input, size_t size, unsigned char *output, size_t produce);

struct quantum_decoder;
struct quantum_decoder *quantum_create(unsigned window_bits, decoder_alloc alloc, void *context);
void quantum_destroy(struct quantum_decoder *decoder, decoder_free release, void *context);
int quantum_decode(struct quantum_decoder *decoder, const unsigned char *input, size_t size, unsigned char *output, size_t produce);

#endif
