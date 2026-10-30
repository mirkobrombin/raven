/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <stddef.h>

/* The component links without a C runtime; the compiler still emits calls to these. */

void *memcpy(void *destination, const void *source, size_t count)
{
	unsigned char *to = destination;
	const unsigned char *from = source;
	while (count--)
		*to++ = *from++;
	return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
	unsigned char *to = destination;
	const unsigned char *from = source;
	if (to <= from)
		return memcpy(destination, source, count);
	while (count--)
		to[count] = from[count];
	return destination;
}

void *memset(void *destination, int value, size_t count)
{
	unsigned char *to = destination;
	while (count--)
		*to++ = (unsigned char)value;
	return destination;
}
