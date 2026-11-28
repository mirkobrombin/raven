// Integer workloads measured on every engine: translated by Raven's runtime (x64 PE), by FEX (x86-64 Linux ELF) and native (arm64).
// One call does a fixed amount of work and returns a checksum. Exactly one WORKLOAD_* names the function the build exports as
// workload(); the PE build calls it from raven_entry.
#include <stdint.h>

#define BUFFER_SIZE 4096

static uint8_t buffer[BUFFER_SIZE];
static uint32_t histogram[256];

static void fill(void)
{
	uint32_t state = 0x12345678u;
	for (unsigned i = 0; i < BUFFER_SIZE; ++i) {
		state = state * 1103515245u + 12345u;
		buffer[i] = (uint8_t)(state >> 16);
	}
}

// CRC-32 (IEEE, reflected) bit by bit over the buffer: shifts, masks and a data-dependent xor.
static uint64_t crc32_buffer(void)
{
	uint32_t crc = 0xffffffffu;
	for (unsigned i = 0; i < BUFFER_SIZE; ++i) {
		crc ^= buffer[i];
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

// FNV-1a 64 over the buffer, eight times: a multiply chain.
static uint64_t fnv_buffer(void)
{
	uint64_t hash = 0xcbf29ce484222325ull;
	for (int round = 0; round < 8; ++round)
		for (unsigned i = 0; i < BUFFER_SIZE; ++i)
			hash = (hash ^ buffer[i]) * 0x100000001b3ull;
	return hash;
}

// A histogram of xorshift output: a load and a store to memory per step, where memory ordering costs show.
static uint64_t histogram_steps(void)
{
	uint32_t x = 2463534242u;
	for (unsigned i = 0; i < 32768; ++i) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		histogram[x & 255] += 1;
	}
	uint64_t sum = 0;
	for (unsigned i = 0; i < 256; ++i) {
		sum = sum * 31 + histogram[i];
		histogram[i] = 0;
	}
	return sum;
}

uint64_t workload(void)
{
	static int filled;
	if (!filled) {
		fill();
		filled = 1;
	}
#if defined(WORKLOAD_CRC32)
	return crc32_buffer();
#elif defined(WORKLOAD_FNV)
	return fnv_buffer();
#elif defined(WORKLOAD_HISTOGRAM)
	return histogram_steps();
#else
#error "define WORKLOAD_CRC32, WORKLOAD_FNV or WORKLOAD_HISTOGRAM"
#endif
}

#ifdef _WIN32
uint64_t raven_entry(void)
{
	return workload();
}
#endif
