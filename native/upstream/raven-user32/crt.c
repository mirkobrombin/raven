/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <stddef.h>

/* The component links without a C runtime; the compiler still emits calls to these. */

void *memcpy(void *destination, const void *source, size_t count)
{
	void *start = destination;
	__asm__ volatile("rep movsb" : "+D"(destination), "+S"(source), "+c"(count) : : "memory");
	return start;
}

void *memmove(void *destination, const void *source, size_t count)
{
	void *start = destination;
	if ((const unsigned char *)destination <= (const unsigned char *)source || (const unsigned char *)destination >= (const unsigned char *)source + count) {
		__asm__ volatile("rep movsb" : "+D"(destination), "+S"(source), "+c"(count) : : "memory");
	} else if (count) {
		destination = (unsigned char *)destination + count - 1;
		source = (const unsigned char *)source + count - 1;
		__asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(destination), "+S"(source), "+c"(count) : : "memory");
	}
	return start;
}

void *memset(void *destination, int value, size_t count)
{
	void *start = destination;
	__asm__ volatile("rep stosb" : "+D"(destination), "+c"(count) : "a"(value) : "memory");
	return start;
}
