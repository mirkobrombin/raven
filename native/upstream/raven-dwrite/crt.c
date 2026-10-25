/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The few C library routines that the compiler calls on its own: this component links against no runtime. */
#include <stddef.h>

#define PLAIN __attribute__((optimize("no-tree-loop-distribute-patterns")))

PLAIN void *memcpy(void *to, const void *from, size_t count)
{
	unsigned char *target = to;
	const unsigned char *source = from;
	while (count--)
		*target++ = *source++;
	return to;
}

PLAIN void *memmove(void *to, const void *from, size_t count)
{
	unsigned char *target = to;
	const unsigned char *source = from;
	if (target < source)
		while (count--)
			*target++ = *source++;
	else
		while (count--)
			target[count] = source[count];
	return to;
}

PLAIN void *memset(void *to, int value, size_t count)
{
	unsigned char *target = to;
	while (count--)
		*target++ = (unsigned char)value;
	return to;
}

PLAIN int memcmp(const void *left, const void *right, size_t count)
{
	const unsigned char *a = left, *b = right;
	while (count--) {
		if (*a != *b)
			return *a < *b ? -1 : 1;
		++a;
		++b;
	}
	return 0;
}
