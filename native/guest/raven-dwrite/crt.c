/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The few C library routines that the compiler calls on its own: this component links against no runtime. */
#include <stddef.h>

#define PLAIN __attribute__((optimize("no-tree-loop-distribute-patterns")))

PLAIN void *memcpy(void *to, const void *from, size_t count)
{
	void *start = to;
	__asm__ volatile("rep movsb" : "+D"(to), "+S"(from), "+c"(count) : : "memory");
	return start;
}

PLAIN void *memmove(void *to, const void *from, size_t count)
{
	void *start = to;
	if ((const unsigned char *)to <= (const unsigned char *)from || (const unsigned char *)to >= (const unsigned char *)from + count) {
		__asm__ volatile("rep movsb" : "+D"(to), "+S"(from), "+c"(count) : : "memory");
	} else if (count) {
		to = (unsigned char *)to + count - 1;
		from = (const unsigned char *)from + count - 1;
		__asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(to), "+S"(from), "+c"(count) : : "memory");
	}
	return start;
}

PLAIN void *memset(void *to, int value, size_t count)
{
	void *start = to;
	__asm__ volatile("rep stosb" : "+D"(to), "+c"(count) : "a"(value) : "memory");
	return start;
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
