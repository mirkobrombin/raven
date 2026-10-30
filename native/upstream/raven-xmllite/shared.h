/* What the reader and the writer share: memory through the caller's IMalloc when there is one, and the encodings they know by name. */

#ifndef RAVEN_XMLLITE_SHARED_H
#define RAVEN_XMLLITE_SHARED_H

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <xmllite.h>

static inline void *memory_get(IMalloc *allocator, size_t size)
{
	return allocator ? IMalloc_Alloc(allocator, size) : HeapAlloc(GetProcessHeap(), 0, size);
}

static inline void memory_put(IMalloc *allocator, void *memory)
{
	if (!memory)
		return;
	if (allocator)
		IMalloc_Free(allocator, memory);
	else
		HeapFree(GetProcessHeap(), 0, memory);
}

/* 0 for a name no table entry has. */
UINT code_page_of_name(const WCHAR *name);
const WCHAR *name_of_code_page(UINT code_page);
WCHAR *copy_text(IMalloc *allocator, const WCHAR *text, UINT length);

#endif
