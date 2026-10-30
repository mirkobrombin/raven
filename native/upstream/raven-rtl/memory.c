/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <stddef.h>

/* What the compiler may call in a component without a C runtime. */
void *memset(void *target, int value, size_t size)
{
	void *start = target;
	__asm__ volatile("rep stosb" : "+D"(target), "+c"(size) : "a"(value) : "memory");
	return start;
}

void *memcpy(void *target, const void *source, size_t size)
{
	void *start = target;
	__asm__ volatile("rep movsb" : "+D"(target), "+S"(source), "+c"(size) : : "memory");
	return start;
}

void *memmove(void *target, const void *source, size_t size)
{
	unsigned char *to = target;
	const unsigned char *from = source;
	if (to <= from || to >= from + size)
		return memcpy(target, source, size);
	while (size--)
		to[size] = from[size];
	return target;
}
