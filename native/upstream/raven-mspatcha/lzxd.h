#ifndef RAVEN_MSPATCHA_LZXD_H
#define RAVEN_MSPATCHA_LZXD_H

#include <stddef.h>
#include <stdint.h>

enum lzxd_status
{
	lzxd_done,
	lzxd_corrupt,
	lzxd_cancelled,
	lzxd_no_memory,
};

/* Called after each block with the bytes made so far; returning 0 stops the decoding. */
typedef int (*lzxd_progress)(void *context, size_t done, size_t total);

/* The window is buffer: its first reference bytes are the old file the stream refers back to, and the size bytes after them receive
   the new one. large selects the 32 MiB window of PATCH_OPTION_USE_LZX_LARGE instead of 8 MiB. */
enum lzxd_status lzxd_decode(const unsigned char *input, size_t input_size, unsigned char *buffer, size_t reference, size_t size, int large,
	lzxd_progress progress, void *context);

/* The largest old file a stream can refer to. */
size_t lzxd_window_limit(int large);

#endif
