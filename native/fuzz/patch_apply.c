/* The PA19 patch reader and LZX DELTA decoder of mspatcha.dll (native/guest/raven-mspatcha), fed changed patches on the host under the
   address and undefined behaviour sanitizers. The seeds are the patches testdata/native_loader/mspatcha_fixtures.py writes, each with
   the old file it applies to and the new file it makes; each seed must first make its new file. Each round changes one patch: flipped
   bytes, special values, inserted or cut runs, in its header, at the start of its stream or anywhere, and then mostly mends its closing
   CRC so that the change reaches the decoder. Sometimes the old file is changed instead. The patch is applied whole, as a header test
   or as a test only, with a progress callback that may cancel. Any patch may be refused; nothing may be read or written outside what
   was allocated.

   python3 testdata/native_loader/mspatcha_fixtures.py DIRECTORY
   cc -fsanitize=address,undefined -fno-sanitize-recover=all -Inative/guest/raven-mspatcha native/fuzz/patch_apply.c
      native/guest/raven-mspatcha/pa19.c native/guest/raven-mspatcha/lzxd.c
   ./a.out DIRECTORY [rounds] [seed] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "patch.h"

/* A header may claim a new file of any size; failing to allocate it is an answer, not a fault. */
const char *__asan_default_options(void)
{
	return "allocator_may_return_null=1";
}

static uint64_t state = 0x9e3779b97f4a7c15ull;

static uint64_t next(void)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

struct file
{
	unsigned char *data;
	size_t size;
};

static struct file load(const char *directory, const char *name)
{
	char path[4096];
	struct file file = { NULL, 0 };
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	FILE *stream = fopen(path, "rb");
	if (!stream) {
		fprintf(stderr, "cannot open %s\n", path);
		exit(2);
	}
	fseek(stream, 0, SEEK_END);
	file.size = (size_t)ftell(stream);
	fseek(stream, 0, SEEK_SET);
	file.data = malloc(file.size + 1);
	if (fread(file.data, 1, file.size, stream) != file.size)
		exit(2);
	fclose(stream);
	return file;
}

/* A patch, the old file it is applied to (none when old is NULL) and the new file it must make (none for a header only patch). */
static const struct
{
	const char *patch, *old, *made;
} seeds[] = {
	{ "delta.pa19", "old.txt", "new.txt" },
	{ "plain.pa19", NULL, "plain.txt" },
	{ "calls.pa19", NULL, "calls.bin" },
	{ "two.pa19", "first.bin", "target-first.bin" },
	{ "two.pa19", "second.bin", "target-second.bin" },
	{ "two-header.pa19", "first.bin", NULL },
	{ "same.pa19", "old.txt", "old.txt" },
};

enum { seed_count = sizeof(seeds) / sizeof(seeds[0]) };

static struct file patches[seed_count], olds[seed_count], made[seed_count];

static int stop_after;

static int progress(void *context, size_t done, size_t total)
{
	(void)context;
	(void)done;
	(void)total;
	return !stop_after || --stop_after;
}

/* The closing CRC that makes the CRC-32 of the whole patch read all ones. */
static void mend(unsigned char *patch, size_t size)
{
	if (size < 4)
		return;
	uint32_t crc = patch_crc32(0, patch, size - 4) ^ 0xffffffffu;
	for (int index = 0; index < 4; index++)
		patch[size - 4 + index] = (unsigned char)(crc >> (8 * index));
}

static size_t change(unsigned char *data, size_t size, size_t capacity)
{
	static const uint32_t special[] = { 0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000u, 0xffffffffu };
	const unsigned changes = 1 + (unsigned)(next() % 4);

	for (unsigned count = 0; count < changes && size; count++) {
		/* The header and the start of the first stream are where most of the meaning is. */
		size_t at = (size_t)(next() % size);
		if (next() % 2)
			at = (size_t)(next() % (size < 96 ? size : 96));
		switch (next() % 6) {
		case 0:
			data[at] ^= (unsigned char)(1u << (next() % 8));
			break;
		case 1:
			data[at] = (unsigned char)next();
			break;
		case 2: {
			const uint32_t value = special[next() % (sizeof(special) / sizeof(special[0]))];
			for (int index = 0; index < 4 && at + index < size; index++)
				data[at + index] = (unsigned char)(value >> (8 * index));
			break;
		}
		case 3:
			/* Numbers of the variable length kind made longer: a run of bytes loses the end mark. */
			for (size_t index = 0, run = 1 + (size_t)(next() % 10); index < run && at + index < size; index++)
				data[at + index] &= 0x7f;
			break;
		case 4:
			if (size + 16 <= capacity) {
				const size_t length = 1 + (size_t)(next() % 16);
				memmove(data + at + length, data + at, size - at);
				for (size_t index = 0; index < length; index++)
					data[at + index] = (unsigned char)next();
				size += length;
			}
			break;
		default: {
			const size_t cut = (size_t)(next() % (size - at < 64 ? size - at + 1 : 64));
			memmove(data + at, data + at + cut, size - at - cut);
			size -= cut;
		}
		}
	}
	return size;
}

static enum patch_status apply(const unsigned char *patch, size_t size, const struct file *old, unsigned options, int header_only,
	struct patch_result *result)
{
	return patch_apply(patch, size, old ? old->data : NULL, old ? old->size : 0, options, header_only, result, progress, NULL);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s DIRECTORY [rounds] [seed]\n", argv[0]);
		return 2;
	}
	const unsigned long rounds = argc > 2 ? strtoul(argv[2], NULL, 10) : 20000;
	if (argc > 3)
		state ^= strtoull(argv[3], NULL, 10) * 0x2545f4914f6cdd1dull;
	size_t largest = 0;
	for (int index = 0; index < seed_count; index++) {
		patches[index] = load(argv[1], seeds[index].patch);
		if (seeds[index].old)
			olds[index] = load(argv[1], seeds[index].old);
		if (seeds[index].made)
			made[index] = load(argv[1], seeds[index].made);
		if (patches[index].size > largest)
			largest = patches[index].size;
	}

	/* The seeds themselves make their new files. */
	for (int index = 0; index < seed_count; index++) {
		struct patch_result result;
		const enum patch_status status = apply(patches[index].data, patches[index].size, seeds[index].old ? &olds[index] : NULL, 0,
			!seeds[index].made, &result);
		if (status != patch_done || (seeds[index].made && (result.new_size != made[index].size ||
			memcmp(result.output, made[index].data, made[index].size)))) {
			printf("seed %s does not make its file (status %d)\n", seeds[index].patch, status);
			return 1;
		}
		free(result.output);
	}

	const size_t capacity = largest + 4096;
	unsigned char *patch = malloc(capacity);
	unsigned long made_count = 0;
	for (unsigned long round = 0; round < rounds; round++) {
		const int which = (int)(next() % seed_count);
		const struct file *old = seeds[which].old ? &olds[which] : NULL;
		struct file changed_old = { NULL, 0 };
		size_t size = patches[which].size;
		memcpy(patch, patches[which].data, size);
		if (old && next() % 8 == 0) {
			changed_old.data = malloc(old->size + 64);
			memcpy(changed_old.data, old->data, old->size);
			changed_old.size = change(changed_old.data, old->size, old->size + 64);
			old = &changed_old;
		} else {
			size = change(patch, size, capacity);
			if (next() % 16)
				mend(patch, size);
		}
		const unsigned options = (unsigned)(next() % 8 == 0 ? patch_fail_if_exact : next() % 8 == 0 ? patch_test_only : 0);
		stop_after = next() % 8 == 0 ? 1 + (int)(next() % 3) : 0;
		struct patch_result result;
		if (apply(patch, size, old, options, next() % 8 == 0, &result) == patch_done && result.output)
			made_count++;
		free(result.output);
		free(changed_old.data);
	}
	printf("%lu rounds, %lu patches applied whole\n", rounds, made_count);
	free(patch);
	return 0;
}
