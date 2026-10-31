/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "oa.h"

/* A BSTR points just past a pointer sized header whose last four bytes are the byte count, and
 * always ends in a wide zero. */
#define BSTR_HEADER sizeof(DWORD_PTR)

static inline DWORD *bstr_count(BSTR text)
{
	return (DWORD *)text - 1;
}

/* Freed strings wait a while in buckets by size. Old programs read a string just after freeing
 * it, or free it twice, and that only ever worked because of this. */
#define CACHE_BUCKETS 16
#define CACHE_DEPTH 6
#define CACHE_STEP 16

static struct
{
	void *block[CACHE_DEPTH];
	unsigned count;
} cache[CACHE_BUCKETS];
static CRITICAL_SECTION cache_lock;
static BOOL cache_off;

static void *cache_take(SIZE_T total)
{
	SIZE_T bucket = (total + CACHE_STEP - 1) / CACHE_STEP - 1;
	void *found = NULL;
	if (cache_off || bucket >= CACHE_BUCKETS)
		return NULL;
	EnterCriticalSection(&cache_lock);
	/* A larger block fits just as well. */
	for (; bucket < CACHE_BUCKETS && bucket * CACHE_STEP < total + 2 * CACHE_STEP * CACHE_BUCKETS; bucket++)
	{
		if (cache[bucket].count)
		{
			found = cache[bucket].block[--cache[bucket].count];
			break;
		}
	}
	LeaveCriticalSection(&cache_lock);
	return found;
}

static BOOL cache_put(void *block)
{
	IMalloc *allocator;
	SIZE_T size, bucket;
	BOOL kept = FALSE;
	unsigned index;
	/* The block's real size decides, since its length field may be anything. */
	if (cache_off || FAILED(CoGetMalloc(1, &allocator)))
		return FALSE;
	size = IMalloc_GetSize(allocator, block);
	IMalloc_Release(allocator);
	if (size < CACHE_STEP || size == (SIZE_T)-1)
		return FALSE;
	bucket = size / CACHE_STEP - 1;
	if (bucket >= CACHE_BUCKETS)
		return FALSE;
	EnterCriticalSection(&cache_lock);
	for (index = 0; index < cache[bucket].count; index++)
		if (cache[bucket].block[index] == block)
			kept = TRUE;
	if (!kept && cache[bucket].count < CACHE_DEPTH)
	{
		cache[bucket].block[cache[bucket].count++] = block;
		kept = TRUE;
	}
	LeaveCriticalSection(&cache_lock);
	return kept;
}

static BSTR bstr_alloc(SIZE_T bytes)
{
	BYTE *block;
	BSTR text;
	SIZE_T total = BSTR_HEADER + bytes + sizeof(WCHAR);
	if (bytes > 0x7ffffff0u || total > 0x7fffffffu)
		return NULL;
	/* Whole steps, so a freed block returns to the bucket it would be taken from. */
	total = (total + CACHE_STEP - 1) & ~(SIZE_T)(CACHE_STEP - 1);
	block = cache_take(total);
	if (!block)
		block = CoTaskMemAlloc(total);
	if (!block)
		return NULL;
	text = (BSTR)(block + BSTR_HEADER);
	*bstr_count(text) = (DWORD)bytes;
	((BYTE *)text)[bytes] = 0;
	((BYTE *)text)[bytes + 1] = 0;
	/* An odd length still ends in a whole wide zero, and a reused block is not clean. */
	if (bytes & 1)
		((BYTE *)text)[bytes + 2] = 0;
	return text;
}

UINT WINAPI SysStringLen(BSTR text)
{
	return text ? *bstr_count(text) / sizeof(WCHAR) : 0;
}

UINT WINAPI SysStringByteLen(BSTR text)
{
	return text ? *bstr_count(text) : 0;
}

void WINAPI SysFreeString(BSTR text)
{
	if (text && !cache_put((BYTE *)text - BSTR_HEADER))
		CoTaskMemFree((BYTE *)text - BSTR_HEADER);
}

BSTR WINAPI SysAllocStringLen(const OLECHAR *text, UINT length)
{
	BSTR out;
	if (length >= 0x7ffffff0u / sizeof(WCHAR))
		return NULL;
	out = bstr_alloc((SIZE_T)length * sizeof(WCHAR));
	if (out)
	{
		if (text)
			memcpy(out, text, length * sizeof(WCHAR));
		else
			memset(out, 0, length * sizeof(WCHAR));
	}
	return out;
}

BSTR WINAPI SysAllocString(LPCOLESTR text)
{
	return text ? SysAllocStringLen(text, lstrlenW(text)) : NULL;
}

BSTR WINAPI SysAllocStringByteLen(LPCSTR text, UINT bytes)
{
	BSTR out;
	if (bytes >= 0x7ffffff0u)
		return NULL;
	out = bstr_alloc(bytes);
	if (out)
	{
		if (text)
			memcpy(out, text, bytes);
		else
			memset(out, 0, bytes);
	}
	return out;
}

/* The old text may be the source of the new one, so it goes only after the copy. */
INT WINAPI SysReAllocStringLen(BSTR *where, const OLECHAR *text, UINT length)
{
	BSTR fresh;
	if (length >= 0x7ffffff0u / sizeof(WCHAR))
		return FALSE;
	if (*where && length * sizeof(WCHAR) <= SysStringByteLen(*where))
	{
		/* Shrinking stays in the same memory, and the text may already live there. */
		if (text)
			memmove(*where, text, length * sizeof(WCHAR));
		*bstr_count(*where) = length * sizeof(WCHAR);
		(*where)[length] = 0;
		return TRUE;
	}
	fresh = SysAllocStringLen(text, length);
	if (!fresh)
		return FALSE;
	SysFreeString(*where);
	*where = fresh;
	return TRUE;
}

INT WINAPI SysReAllocString(LPBSTR where, LPCOLESTR text)
{
	if (!where)
		return FALSE;
	if (!text)
	{
		SysFreeString(*where);
		*where = NULL;
		return TRUE;
	}
	return SysReAllocStringLen(where, text, lstrlenW(text));
}

ULONG WINAPI OaBuildVersion(void)
{
	return MAKELONG(0x1b98, 0x000a);
}

void WINAPI SetOaNoCache(void)
{
	cache_off = TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		InitializeCriticalSection(&cache_lock);
		oa_font_startup();
		oa_typelib_startup();
	}
	return TRUE;
}
