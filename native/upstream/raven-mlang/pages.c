/* SPDX-License-Identifier: MIT */
#include "ml.h"
#include <wchar.h>

/* The code page table and text conversion. The system converts everything it knows; Japanese encodings it has no table of
 * are spoken here by rearranging Shift-JIS, whose rows and cells are the same characters. */

#define C_MAILNEWS MIMECONTF_MAILNEWS
#define C_BROWSER MIMECONTF_BROWSER
#define C_MINIMAL MIMECONTF_MINIMAL
#define C_IMPORT MIMECONTF_IMPORT
#define C_SAVABLE_MAILNEWS MIMECONTF_SAVABLE_MAILNEWS
#define C_SAVABLE_BROWSER MIMECONTF_SAVABLE_BROWSER
#define C_EXPORT MIMECONTF_EXPORT
#define C_VALID MIMECONTF_VALID
#define C_VALID_NLS MIMECONTF_VALID_NLS
#define C_MIME_IE4 MIMECONTF_MIME_IE4
#define C_MIME_LATEST MIMECONTF_MIME_LATEST

const ml_page ml_pages[] = {
#include "pages.inc"
};

const UINT ml_page_count = ARRAYSIZE(ml_pages);

const ml_page *ml_page_of(UINT code_page)
{
	UINT index;
	for (index = 0; index < ml_page_count; index++)
		if (ml_pages[index].code_page == code_page)
			return &ml_pages[index];
	return NULL;
}

static BOOL is_jis(UINT code_page)
{
	return code_page == 50220 || code_page == 50221 || code_page == 50222;
}

static BOOL is_euc_japanese(UINT code_page)
{
	return code_page == 51932 || code_page == 20932;
}

BOOL ml_page_convertible(UINT code_page)
{
	return code_page == CP_UNICODE || code_page == CP_UNICODE_SWAPPED || code_page == CP_UTF8 || code_page == 932 ||
		is_jis(code_page) || is_euc_japanese(code_page) || code_page == 50932 || IsValidCodePage(code_page);
}

/* Shift-JIS and the row and cell of the JIS grid */

static void grid_from_shift(BYTE lead, BYTE trail, BYTE *row, BYTE *cell)
{
	UINT high = lead < 0xa0 ? lead - 0x71 : lead - 0xb1;
	UINT odd = trail < 0x9f;
	*row = (BYTE)(high * 2 + (odd ? 1 : 2));
	*cell = (BYTE)(odd ? trail - (trail > 0x7f ? 0x20 : 0x1f) : trail - 0x7e);
}

static void shift_from_grid(BYTE row, BYTE cell, BYTE *lead, BYTE *trail)
{
	UINT high = (row - 1) / 2;
	*lead = (BYTE)(high + (high < 0x2f ? 0x71 : 0xb1));
	*trail = (BYTE)(row & 1 ? cell + (cell > 0x5f ? 0x20 : 0x1f) : cell + 0x7e);
}

static BOOL shift_lead(BYTE value)
{
	return (value >= 0x81 && value <= 0x9f) || (value >= 0xe0 && value <= 0xfc);
}

/* Halfwidth katakana (0xa1-0xdf) has no place in the seven bit form: it goes to its fullwidth cell, voicing marks left apart. */
static void grid_from_half_kana(BYTE value, BYTE *row, BYTE *cell)
{
	static const BYTE cells[63] = {
		0x23, 0x56, 0x57, 0x22, 0x26, 0x72, 0x21, 0x23, 0x25, 0x27, 0x29, 0x63, 0x65, 0x67, 0x43, 0x3c,
		0x22, 0x24, 0x26, 0x28, 0x2a, 0x2b, 0x2d, 0x2f, 0x31, 0x33, 0x35, 0x37, 0x39, 0x3b, 0x3d, 0x3f,
		0x41, 0x44, 0x46, 0x48, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x52, 0x55, 0x58, 0x5b, 0x5e, 0x5f,
		0x60, 0x61, 0x62, 0x64, 0x66, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6f, 0x73, 0x2b, 0x2c,
	};
	BYTE index = value - 0xa1;
	*row = index == 0 || index == 1 || index == 2 || index == 3 || index == 0x0f || index >= 0x3d ? 0x21 : 0x25;
	*cell = cells[index];
}

/* Shift-JIS bytes become EUC-JP or JIS; the targets are at most three times as long, plus the escapes. */
static SIZE_T shift_to_euc(const BYTE *source, SIZE_T size, BYTE *target)
{
	SIZE_T out = 0, index = 0;
	while (index < size)
	{
		BYTE value = source[index++];
		if (value >= 0xa1 && value <= 0xdf)
		{
			target[out++] = 0x8e;
			target[out++] = value;
		}
		else if (shift_lead(value) && index < size)
		{
			BYTE row, cell;
			grid_from_shift(value, source[index++], &row, &cell);
			target[out++] = row + 0xa0;
			target[out++] = cell + 0xa0;
		}
		else
			target[out++] = value;
	}
	return out;
}

static SIZE_T euc_to_shift(const BYTE *source, SIZE_T size, BYTE *target)
{
	SIZE_T out = 0, index = 0;
	while (index < size)
	{
		BYTE value = source[index++];
		if (value == 0x8e && index < size)
			target[out++] = source[index++];
		else if (value >= 0xa1 && index < size)
		{
			shift_from_grid(value - 0xa0, source[index++] - 0xa0, &target[out], &target[out + 1]);
			out += 2;
		}
		else
			target[out++] = value;
	}
	return out;
}

static const BYTE enter_double[3] = {0x1b, '$', 'B'};
static const BYTE leave_double[3] = {0x1b, '(', 'B'};

static SIZE_T shift_to_jis(const BYTE *source, SIZE_T size, BYTE *target)
{
	SIZE_T out = 0, index = 0;
	BOOL in_double = FALSE;
	while (index < size)
	{
		BYTE value = source[index++], row, cell;
		BOOL wide = TRUE;
		if (value >= 0xa1 && value <= 0xdf)
			grid_from_half_kana(value, &row, &cell);
		else if (shift_lead(value) && index < size)
			grid_from_shift(value, source[index++], &row, &cell);
		else
			wide = FALSE;
		if (wide != in_double)
		{
			memcpy(target + out, wide ? enter_double : leave_double, 3);
			out += 3;
			in_double = wide;
		}
		if (wide)
		{
			target[out++] = row;
			target[out++] = cell;
		}
		else
			target[out++] = value;
	}
	if (in_double)
	{
		memcpy(target + out, leave_double, 3);
		out += 3;
	}
	return out;
}

static SIZE_T jis_to_shift(const BYTE *source, SIZE_T size, BYTE *target)
{
	SIZE_T out = 0, index = 0;
	BOOL in_double = FALSE;
	while (index < size)
	{
		BYTE value = source[index];
		if (value == 0x1b && index + 2 < size)
		{
			BYTE first = source[index + 1];
			if (first == '$' || first == '(')
			{
				in_double = first == '$';
				index += 3;
				continue;
			}
		}
		index++;
		if (in_double && index < size)
		{
			shift_from_grid(value, source[index++], &target[out], &target[out + 1]);
			out += 2;
		}
		else
			target[out++] = value;
	}
	return out;
}

/* What the automatic Japanese page takes for a text: escapes mean JIS, and of the two high-bit forms only Shift-JIS leads with
 * bytes from 0x81 to 0x9f. */
static UINT detect_japanese(const BYTE *source, SIZE_T size)
{
	SIZE_T index;
	for (index = 0; index < size; index++)
	{
		if (source[index] == 0x1b)
			return 50220;
		if (source[index] >= 0x81 && source[index] <= 0x9f && source[index] != 0x8e)
			return 932;
	}
	for (index = 0; index < size; index++)
		if (source[index] >= 0xa1)
			return 51932;
	return 932;
}

static BYTE *heap_bytes(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
}

static HRESULT wide_from_system(UINT code_page, const char *source, int size, WCHAR *target, UINT room, UINT *written)
{
	int length;
	if (!size)
	{
		*written = 0;
		return S_OK;
	}
	length = MultiByteToWideChar(code_page, 0, source, size, NULL, 0);
	if (length <= 0)
		return E_FAIL;
	*written = length;
	if (!target || !room)
		return S_OK;
	if ((UINT)length > room)
	{
		WCHAR *whole = (WCHAR *)heap_bytes(length * sizeof(WCHAR));
		if (!whole)
			return E_OUTOFMEMORY;
		MultiByteToWideChar(code_page, 0, source, size, whole, length);
		memcpy(target, whole, room * sizeof(WCHAR));
		HeapFree(GetProcessHeap(), 0, whole);
		return S_FALSE;
	}
	MultiByteToWideChar(code_page, 0, source, size, target, length);
	return S_OK;
}

HRESULT ml_to_unicode(UINT code_page, const char *source, UINT *source_size, WCHAR *target, UINT *target_size)
{
	UINT size, room = target_size ? *target_size : 0, written = 0;
	HRESULT status = S_OK;
	if (!source)
	{
		if (source_size)
			*source_size = 0;
		if (target_size)
			*target_size = 0;
		return E_FAIL;
	}
	if (!source_size || *source_size == (UINT)-1)
		size = code_page == CP_UNICODE || code_page == CP_UNICODE_SWAPPED ? (UINT)(wcslen((const WCHAR *)source) * 2) : lstrlenA(source);
	else
		size = *source_size;
	if (code_page == 50932)
		code_page = detect_japanese((const BYTE *)source, size);
	if (code_page == CP_UNICODE || code_page == CP_UNICODE_SWAPPED)
	{
		UINT index;
		written = size / 2;
		if (target && room)
		{
			UINT count = min(written, room);
			memcpy(target, source, count * sizeof(WCHAR));
			if (code_page == CP_UNICODE_SWAPPED)
				for (index = 0; index < count; index++)
					target[index] = (WCHAR)(target[index] << 8 | target[index] >> 8);
			if (written > room)
				status = S_FALSE;
		}
	}
	else if (is_jis(code_page) || is_euc_japanese(code_page))
	{
		BYTE *shift = heap_bytes(size);
		SIZE_T length;
		if (!shift)
			return E_OUTOFMEMORY;
		length = is_jis(code_page) ? jis_to_shift((const BYTE *)source, size, shift) : euc_to_shift((const BYTE *)source, size, shift);
		status = wide_from_system(932, (const char *)shift, (int)length, target, room, &written);
		HeapFree(GetProcessHeap(), 0, shift);
	}
	else
		status = wide_from_system(code_page, source, size, target, room, &written);
	if (status == S_FALSE)
		status = E_FAIL, written = 0;
	else if (FAILED(status))
		written = 0;
	if (source_size)
		*source_size = status == S_OK ? size : 0;
	if (target_size)
		*target_size = written;
	return status;
}

HRESULT ml_from_unicode(UINT code_page, const WCHAR *source, UINT *source_size, char *target, UINT *target_size)
{
	UINT size, room = target_size ? *target_size : 0;
	SIZE_T written = 0;
	BYTE *bytes = NULL;
	const BYTE *result = NULL;
	HRESULT status = S_OK;
	if (!source)
	{
		if (source_size)
			*source_size = 0;
		if (target_size)
			*target_size = 0;
		return E_FAIL;
	}
	size = !source_size || *source_size == (UINT)-1 ? (UINT)wcslen(source) : *source_size;
	if (!target_size)
		target = NULL;
	if (code_page == CP_UNICODE || code_page == CP_UNICODE_SWAPPED)
	{
		written = size * sizeof(WCHAR);
		if (code_page == CP_UNICODE)
			result = (const BYTE *)source;
		else
		{
			UINT index;
			if (!(bytes = heap_bytes(written)))
				return E_OUTOFMEMORY;
			for (index = 0; index < size; index++)
			{
				bytes[index * 2] = (BYTE)(source[index] >> 8);
				bytes[index * 2 + 1] = (BYTE)source[index];
			}
			result = bytes;
		}
	}
	else if (size)
	{
		UINT system_page = is_jis(code_page) || is_euc_japanese(code_page) || code_page == 50932 ? 932 : code_page;
		int length = WideCharToMultiByte(system_page, 0, source, size, NULL, 0, NULL, NULL);
		if (length <= 0)
			status = E_FAIL;
		else if (!(bytes = heap_bytes((SIZE_T)length * 3 + 8)))
			return E_OUTOFMEMORY;
		else
		{
			WideCharToMultiByte(system_page, 0, source, size, (char *)bytes, length, NULL, NULL);
			if (system_page == 932 && code_page != 932)
			{
				/* The rearrangement grows, so it runs into a block of its own. */
				BYTE *other = heap_bytes((SIZE_T)length * 8 + 8);
				if (!other)
				{
					HeapFree(GetProcessHeap(), 0, bytes);
					return E_OUTOFMEMORY;
				}
				written = is_euc_japanese(code_page) ? shift_to_euc(bytes, length, other) : shift_to_jis(bytes, length, other);
				HeapFree(GetProcessHeap(), 0, bytes);
				bytes = other;
			}
			else
				written = length;
			result = bytes;
		}
	}
	if (SUCCEEDED(status))
	{
		if (target && room)
		{
			memcpy(target, result, min(written, (SIZE_T)room));
			if (written > room)
				status = E_FAIL;
		}
		if (target_size)
			*target_size = status == S_OK ? (UINT)written : 0;
		if (source_size)
			*source_size = status == S_OK ? size : 0;
	}
	else
	{
		if (target_size)
			*target_size = 0;
		if (source_size)
			*source_size = 0;
	}
	if (bytes)
		HeapFree(GetProcessHeap(), 0, bytes);
	return status;
}

/* Between two byte encodings the text passes through Unicode. */
HRESULT ml_convert(UINT from, UINT to, const BYTE *source, UINT *source_size, BYTE *target, UINT *target_size)
{
	UINT wide_size = 0, wide_room;
	WCHAR *wide;
	HRESULT status;
	if (from == CP_UNICODE)
	{
		UINT count = source_size && *source_size != (UINT)-1 ? *source_size / 2 : (UINT)-1;
		status = ml_from_unicode(to, (const WCHAR *)source, &count, (char *)target, target_size);
		if (source_size)
			*source_size = count == (UINT)-1 ? 0 : count * 2;
		return status;
	}
	if (to == CP_UNICODE)
	{
		UINT room = target_size ? *target_size / 2 : 0;
		status = ml_to_unicode(from, (const char *)source, source_size, (WCHAR *)target, target_size ? &room : NULL);
		if (target_size)
			*target_size = room * 2;
		return status;
	}
	status = ml_to_unicode(from, (const char *)source, source_size, NULL, &wide_size);
	if (FAILED(status))
		return status;
	if (!(wide = (WCHAR *)heap_bytes((wide_size + 1) * sizeof(WCHAR))))
		return E_OUTOFMEMORY;
	wide_room = wide_size;
	status = ml_to_unicode(from, (const char *)source, source_size, wide, &wide_room);
	if (SUCCEEDED(status))
		status = ml_from_unicode(to, wide, &wide_room, (char *)target, target_size);
	HeapFree(GetProcessHeap(), 0, wide);
	return status;
}
