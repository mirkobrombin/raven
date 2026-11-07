#ifndef RAVEN_MSPATCHA_PATCH_H
#define RAVEN_MSPATCHA_PATCH_H

#include <stddef.h>
#include <stdint.h>

#include "lzxd.h"

enum patch_status
{
	patch_done,
	patch_corrupt,
	patch_unsupported,
	patch_wrong_file,
	patch_not_necessary,
	patch_not_available,
	patch_decode_failure,
	patch_cancelled,
	patch_no_memory,
};

/* The apply options of patchapi.h this part acts on. */
enum {
	patch_fail_if_exact = 0x1,
	patch_test_only = 0x4,
};

struct patch_result
{
	/* From malloc, new_size bytes; NULL when only the header was tested. */
	unsigned char *output;
	size_t new_size;
	/* Seconds since 1970 the new file is stamped with, when has_stamp. */
	uint32_t stamp;
	int has_stamp;
};

/* Turns old (NULL for none) into the file a PA19 patch describes. With header_only, the patch's header and the match of old are all that
   is checked; new_size is known whenever the header could be read. */
enum patch_status patch_apply(const unsigned char *patch, size_t patch_size, const unsigned char *old, size_t old_size, unsigned options,
	int header_only, struct patch_result *result, lzxd_progress progress, void *context);

uint32_t patch_crc32(uint32_t crc, const unsigned char *data, size_t size);

#endif
