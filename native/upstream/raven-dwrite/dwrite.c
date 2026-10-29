/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven DirectWrite component: the factory, the system font collection and the font faces that programs such as Chromium use.
 *
 * The interfaces are COM objects written here. Fonts are found and measured by the host (ravenhost.dll, which answers with
 * FreeType): this file keeps the families, matches faces, serves font files through the file loader interfaces and composes glyph
 * run textures from the glyph bitmaps the host renders. Every interface starts from a table of stubs that name the method they
 * stand for (dwrite_tables.h), so a call that nothing implements shows up by name in the trace. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

/* ---- the host services ---- */
__declspec(dllimport) DWORD WINAPI RavenFontScan(void *buffer, DWORD capacity);
__declspec(dllimport) DWORD WINAPI RavenFontOpen(const void *data, DWORD size, DWORD face_index);
__declspec(dllimport) void WINAPI RavenFontClose(DWORD font);
__declspec(dllimport) DWORD WINAPI RavenFontInfo(DWORD font, INT32 *info);
__declspec(dllimport) DWORD WINAPI RavenFontGlyphs(DWORD font, const UINT32 *codepoints, DWORD count, UINT16 *glyphs);
__declspec(dllimport) DWORD WINAPI RavenFontMetrics(DWORD font, DWORD glyph, INT32 *out);
__declspec(dllimport) DWORD WINAPI RavenFontRender(DWORD font, DWORD glyph, INT32 em16, const INT32 *matrix, DWORD mode, void *buffer, DWORD capacity, INT32 *bounds, INT32 shift_x16, INT32 shift_y16);
__declspec(dllimport) DWORD WINAPI RavenFontOutline(DWORD font, DWORD glyph, void *buffer, DWORD capacity);
__declspec(dllimport) DWORD WINAPI RavenFontTable(DWORD font, DWORD tag, DWORD offset, void *buffer, DWORD capacity);
__declspec(dllimport) DWORD WINAPI RavenFontName(DWORD font, DWORD name_id, char *buffer, DWORD capacity);
__declspec(dllimport) void WINAPI RavenTrace(const char *text);

/* ---- what the host answers about a font, kept: each glyph of each line of text would otherwise cross into the runtime, tens of
 * thousands of times a second while a program redraws its text. Font handles are never reused, so a handle names the same font for
 * the life of the process. Each cache is a table indexed by a hash, and a newer answer takes the place of an older one. ---- */
static SRWLOCK font_cache_lock = SRWLOCK_INIT;

static UINT32 font_hash(UINT32 a, UINT32 b, UINT32 c)
{
	UINT32 hash = a * 0x9e3779b1u ^ b * 0x85ebca77u ^ c * 0xc2b2ae3du;
	return hash ^ (hash >> 15);
}

typedef struct { DWORD font, glyph; INT32 metrics[7]; } MetricsEntry;
#define METRICS_SLOTS 8192
static MetricsEntry *metrics_cache;

/* Design metrics do not depend on the size: they are kept for good. */
static DWORD cached_metrics(DWORD font, DWORD glyph, INT32 *out)
{
	MetricsEntry *entry;
	DWORD found = 0;
	AcquireSRWLockShared(&font_cache_lock);
	entry = metrics_cache ? &metrics_cache[font_hash(font, glyph, 0) & (METRICS_SLOTS - 1)] : NULL;
	if (entry && entry->font == font && entry->glyph == glyph) {
		memcpy(out, entry->metrics, sizeof(entry->metrics));
		found = 1;
	}
	ReleaseSRWLockShared(&font_cache_lock);
	if (found)
		return 1;
	if (!RavenFontMetrics(font, glyph, out))
		return 0;
	AcquireSRWLockExclusive(&font_cache_lock);
	if (!metrics_cache)
		metrics_cache = ALLOC(METRICS_SLOTS * sizeof(MetricsEntry));
	if (metrics_cache) {
		entry = &metrics_cache[font_hash(font, glyph, 0) & (METRICS_SLOTS - 1)];
		entry->font = font;
		entry->glyph = glyph;
		memcpy(entry->metrics, out, sizeof(entry->metrics));
	}
	ReleaseSRWLockExclusive(&font_cache_lock);
	return 1;
}

typedef struct { DWORD font; UINT32 codepoint; UINT16 glyph; } GlyphEntry;
#define GLYPH_SLOTS 8192
static GlyphEntry *glyph_cache;

/* The glyphs of characters: only the ones not seen before go to the runtime, in one call. */
static DWORD cached_glyphs(DWORD font, const UINT32 *codepoints, DWORD count, UINT16 *glyphs)
{
	UINT32 *missing;
	UINT16 *found;
	DWORD index, misses = 0, result = 1;
	BOOL *known = ALLOC(count * sizeof(BOOL));
	if (!known)
		return RavenFontGlyphs(font, codepoints, count, glyphs);
	AcquireSRWLockShared(&font_cache_lock);
	for (index = 0; index < count; ++index) {
		const GlyphEntry *entry = glyph_cache ? &glyph_cache[font_hash(font, codepoints[index], 1) & (GLYPH_SLOTS - 1)] : NULL;
		if (font && entry && entry->font == font && entry->codepoint == codepoints[index]) {
			glyphs[index] = entry->glyph;
			known[index] = TRUE;
		} else
			++misses;
	}
	ReleaseSRWLockShared(&font_cache_lock);
	if (!misses) {
		FREE(known);
		return 1;
	}
	missing = ALLOC(misses * sizeof(UINT32));
	found = ALLOC(misses * sizeof(UINT16));
	if (!missing || !found) {
		FREE(missing);
		FREE(found);
		FREE(known);
		return RavenFontGlyphs(font, codepoints, count, glyphs);
	}
	for (index = 0, misses = 0; index < count; ++index)
		if (!known[index])
			missing[misses++] = codepoints[index];
	if (!RavenFontGlyphs(font, missing, misses, found))
		result = 0;
	else {
		AcquireSRWLockExclusive(&font_cache_lock);
		if (!glyph_cache)
			glyph_cache = ALLOC(GLYPH_SLOTS * sizeof(GlyphEntry));
		for (index = 0, misses = 0; index < count; ++index) {
			if (known[index])
				continue;
			glyphs[index] = found[misses++];
			if (glyph_cache) {
				GlyphEntry *entry = &glyph_cache[font_hash(font, codepoints[index], 1) & (GLYPH_SLOTS - 1)];
				entry->font = font;
				entry->codepoint = codepoints[index];
				entry->glyph = glyphs[index];
			}
		}
		ReleaseSRWLockExclusive(&font_cache_lock);
	}
	FREE(missing);
	FREE(found);
	FREE(known);
	return result;
}

typedef struct {
	DWORD font, glyph, mode, size;
	INT32 em16, shift_x16, shift_y16, matrix[4], bounds[4];
	BOOL has_matrix, used;
	BYTE *pixels;
} RenderEntry;
#define RENDER_SLOTS 4096
#define RENDER_LARGEST 16384
static RenderEntry *render_cache;

/* A rendered glyph for its size, transform, mode and fraction of a pixel, with its bounds; bitmaps up to 16 KiB are kept. The answer is
 * the runtime's: the size of the bitmap and its bounds always, its pixels when the buffer is large enough. */
static DWORD cached_render(DWORD font, DWORD glyph, INT32 em16, const INT32 *matrix, DWORD mode, void *buffer, DWORD capacity, INT32 *bounds,
	INT32 shift_x16, INT32 shift_y16)
{
	const UINT32 hash = font_hash(font, glyph, (UINT32)em16 ^ (UINT32)shift_x16 * 31u ^ (UINT32)shift_y16 * 131u ^ mode * 7919u);
	RenderEntry *entry;
	DWORD size = 0;
	BOOL hit = FALSE;
	AcquireSRWLockShared(&font_cache_lock);
	entry = render_cache ? &render_cache[hash & (RENDER_SLOTS - 1)] : NULL;
	if (entry && entry->used && entry->font == font && entry->glyph == glyph && entry->em16 == em16 && entry->mode == mode &&
		entry->shift_x16 == shift_x16 && entry->shift_y16 == shift_y16 && entry->has_matrix == (matrix != NULL) &&
		(!matrix || !memcmp(entry->matrix, matrix, sizeof(entry->matrix))) && (entry->pixels || !entry->size || !buffer || capacity < entry->size)) {
		size = entry->size;
		memcpy(bounds, entry->bounds, sizeof(entry->bounds));
		if (buffer && capacity >= size && size)
			memcpy(buffer, entry->pixels, size);
		hit = TRUE;
	}
	ReleaseSRWLockShared(&font_cache_lock);
	if (hit)
		return size;
	size = RavenFontRender(font, glyph, em16, matrix, mode, buffer, capacity, bounds, shift_x16, shift_y16);
	if (size > RENDER_LARGEST || (size && (!buffer || capacity < size)))
		return size;
	AcquireSRWLockExclusive(&font_cache_lock);
	if (!render_cache)
		render_cache = ALLOC(RENDER_SLOTS * sizeof(RenderEntry));
	if (render_cache) {
		BYTE *pixels = size ? ALLOC(size) : NULL;
		entry = &render_cache[hash & (RENDER_SLOTS - 1)];
		if (!size || pixels) {
			FREE(entry->pixels);
			entry->pixels = pixels;
			if (pixels)
				memcpy(pixels, buffer, size);
			entry->font = font;
			entry->glyph = glyph;
			entry->mode = mode;
			entry->size = size;
			entry->em16 = em16;
			entry->shift_x16 = shift_x16;
			entry->shift_y16 = shift_y16;
			entry->has_matrix = matrix != NULL;
			if (matrix)
				memcpy(entry->matrix, matrix, sizeof(entry->matrix));
			memcpy(entry->bounds, bounds, sizeof(entry->bounds));
			entry->used = TRUE;
		}
	}
	ReleaseSRWLockExclusive(&font_cache_lock);
	return size;
}

/* A refused interface goes to the trace by its GUID, so a program that needs one more interface shows which. */
static HRESULT no_interface(REFIID iid)
{
	static const char digits[] = "0123456789abcdef";
	char text[64] = "dwrite: no interface ";
	size_t at = 21, index;
	const BYTE *bytes = (const BYTE *)iid;
	static const BYTE order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	for (index = 0; index < 16; ++index) {
		if (index == 4 || index == 6 || index == 8 || index == 10)
			text[at++] = '-';
		text[at++] = digits[bytes[order[index]] >> 4];
		text[at++] = digits[bytes[order[index]] & 15];
	}
	text[at] = 0;
	RavenTrace(text);
	return E_NOINTERFACE;
}

static HRESULT unimplemented(const char *interface_name, const char *method)
{
	char text[160];
	size_t at = 0;
	const char *part[] = {"dwrite: ", interface_name, "::", method, " is not implemented"};
	size_t index, length;
	for (index = 0; index < 5; ++index)
		for (length = 0; part[index][length] && at < sizeof(text) - 1; ++length)
			text[at++] = part[index][length];
	text[at] = 0;
	RavenTrace(text);
	return E_NOTIMPL;
}

#include "dwrite_tables.h"

/* ---- small helpers ---- */
static size_t wlen(const WCHAR *text)
{
	size_t length = 0;
	while (text[length])
		++length;
	return length;
}

static WCHAR fold(WCHAR c)
{
	return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c;
}

static BOOL wequal_fold(const WCHAR *a, const WCHAR *b)
{
	while (*a && *b) {
		if (fold(*a) != fold(*b))
			return FALSE;
		++a;
		++b;
	}
	return *a == *b;
}

static BOOL same_guid(const GUID *a, const GUID *b)
{
	return memcmp(a, b, sizeof(GUID)) == 0;
}

/* UTF-8 text of the host as UTF-16 in a new block. */
static WCHAR *widen(const char *text)
{
	size_t length = 0, at = 0;
	WCHAR *result;
	while (text[length])
		++length;
	result = ALLOC((length + 1) * sizeof(WCHAR));
	if (!result)
		return NULL;
	while (at < length) {
		unsigned char c = (unsigned char)text[at];
		UINT32 code;
		size_t extra = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
		code = extra == 0 ? c : extra == 1 ? c & 0x1f : extra == 2 ? c & 0x0f : c & 0x07;
		++at;
		while (extra-- && at < length)
			code = code << 6 | ((unsigned char)text[at++] & 0x3f);
		{
			size_t out = wlen(result);
			if (code >= 0x10000) {
				code -= 0x10000;
				result[out++] = (WCHAR)(0xd800 + (code >> 10));
				result[out++] = (WCHAR)(0xdc00 + (code & 0x3ff));
			} else {
				result[out++] = (WCHAR)code;
			}
			result[out] = 0;
		}
	}
	return result;
}

static WCHAR *wdup(const WCHAR *text)
{
	size_t length = wlen(text);
	WCHAR *copy = ALLOC((length + 1) * sizeof(WCHAR));
	if (copy)
		memcpy(copy, text, length * sizeof(WCHAR));
	return copy;
}

/* ---- the fonts of the system ---- */
/* A system font is named by its path; a font a program supplies through its own loader has no path and keeps its opened face and file.
   The family and face are the weight-stretch-style names fonts are grouped by; win32 is the family GDI knows the font by, and the
   typographic names group every weight and width of a design. */
typedef struct {
	WCHAR *path, *family, *face, *full, *postscript, *win32, *typographic, *typographic_face;
	DWORD index, weight, style, stretch, flags;
	struct FaceData *data;
	IDWriteFontFile *file;
} FontEntry;

typedef struct {
	WCHAR *name;
	FontEntry **entries;
	UINT32 count, capacity;
} FamilyEntry;

static FontEntry *font_entries;
static UINT32 font_count;
static FamilyEntry *families;
static UINT32 family_count;
static INIT_ONCE system_once = INIT_ONCE_STATIC_INIT;

static FamilyEntry *find_family(const WCHAR *name)
{
	UINT32 index;
	for (index = 0; index < family_count; ++index)
		if (wequal_fold(families[index].name, name))
			return &families[index];
	return NULL;
}

static FamilyEntry *add_family(const WCHAR *name)
{
	FamilyEntry *grown = families ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, families, (family_count + 1) * sizeof(FamilyEntry))
		: ALLOC(sizeof(FamilyEntry));
	if (!grown)
		return NULL;
	families = grown;
	families[family_count].name = wdup(name);
	families[family_count].entries = NULL;
	families[family_count].count = families[family_count].capacity = 0;
	return &families[family_count++];
}

static void family_add(FamilyEntry *family, FontEntry *entry)
{
	if (family->count == family->capacity) {
		UINT32 capacity = family->capacity ? family->capacity * 2 : 4;
		FontEntry **grown = family->entries ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, family->entries, capacity * sizeof(FontEntry *))
			: ALLOC(capacity * sizeof(FontEntry *));
		if (!grown)
			return;
		family->entries = grown;
		family->capacity = capacity;
	}
	family->entries[family->count++] = entry;
}

/* The names Windows programs ask for, and the families of the host that answer for them. */
static const WCHAR *aliases[][2] = {
	{L"Arial", L"Liberation Sans"}, {L"Helvetica", L"Liberation Sans"}, {L"Times New Roman", L"Liberation Serif"}, {L"Times", L"Liberation Serif"},
	{L"Courier New", L"Liberation Mono"}, {L"Courier", L"Liberation Mono"}, {L"Segoe UI", L"Noto Sans"}, {L"Tahoma", L"DejaVu Sans"},
	{L"Verdana", L"DejaVu Sans"}, {L"Microsoft Sans Serif", L"Liberation Sans"}, {L"MS Shell Dlg", L"DejaVu Sans"}, {L"MS Shell Dlg 2", L"DejaVu Sans"},
	{L"Calibri", L"Noto Sans"}, {L"Consolas", L"DejaVu Sans Mono"}, {L"Lucida Console", L"DejaVu Sans Mono"}, {L"Georgia", L"DejaVu Serif"},
	{L"Trebuchet MS", L"Noto Sans"}, {L"Segoe UI Emoji", L"Noto Sans"}, {L"Segoe UI Symbol", L"DejaVu Sans"}, {L"Cambria", L"DejaVu Serif"},
	{L"Palatino Linotype", L"DejaVu Serif"}, {L"Symbol", L"DejaVu Sans"}, {L"MS Gothic", L"DejaVu Sans Mono"},
};

/* The free families that stand for one another when the host lacks one, so an alias still lands on a font of the same kind. */
static const WCHAR *stand_ins[][2] = {
	{L"Noto Sans", L"Liberation Sans"}, {L"Liberation Sans", L"DejaVu Sans"}, {L"DejaVu Sans", L"Liberation Sans"},
	{L"Liberation Serif", L"DejaVu Serif"}, {L"DejaVu Serif", L"Liberation Serif"},
	{L"Liberation Mono", L"DejaVu Sans Mono"}, {L"DejaVu Sans Mono", L"Liberation Mono"},
};

/* The family an alias names, or the first stand-in for it the host has. */
static FamilyEntry *alias_target(const WCHAR *name)
{
	int hop;
	for (hop = 0; hop < 3 && name; ++hop) {
		FamilyEntry *found = find_family(name);
		size_t index;
		const WCHAR *next = NULL;
		if (found)
			return found;
		for (index = 0; index < sizeof(stand_ins) / sizeof(stand_ins[0]); ++index)
			if (wequal_fold(stand_ins[index][0], name))
				next = stand_ins[index][1];
		name = next;
	}
	return NULL;
}

static BOOL CALLBACK build_system(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	DWORD size = RavenFontScan(NULL, 0), at = 0;
	BYTE *records;
	UINT32 index;
	if (size) {
		records = ALLOC(size);
		if (records && RavenFontScan(records, size) == size) {
			UINT32 count = 0;
			DWORD walk;
			for (walk = 0; walk + 4 <= size;) {
				DWORD record = *(DWORD *)(records + walk);
				if (record < 24 || walk + record > size)
					break;
				++count;
				walk += record;
			}
			font_entries = ALLOC(count * sizeof(FontEntry));
			for (walk = 0; walk + 4 <= size && font_count < count;) {
				DWORD record = *(DWORD *)(records + walk);
				const char *strings = (const char *)records + walk + 24;
				FontEntry *entry = &font_entries[font_count];
				if (record < 24 || walk + record > size)
					break;
				entry->index = *(DWORD *)(records + walk + 4);
				entry->weight = *(DWORD *)(records + walk + 8);
				entry->style = *(DWORD *)(records + walk + 12);
				entry->stretch = *(DWORD *)(records + walk + 16);
				entry->flags = *(DWORD *)(records + walk + 20);
				{
					const char *text = strings, *end = (const char *)records + walk + record;
					WCHAR **fields[8] = {&entry->path, &entry->family, &entry->face, &entry->full, &entry->postscript, &entry->win32,
						&entry->typographic, &entry->typographic_face};
					int field;
					for (field = 0; field < 8 && text < end; ++field) {
						*fields[field] = widen(text);
						while (*text)
							++text;
						++text;
					}
				}
				{
					/* Windows reports the font path with backslashes under the Z: drive, which maps the host root. */
					WCHAR *path = ALLOC((wlen(entry->path) + 4) * sizeof(WCHAR));
					size_t position;
					path[0] = 'Z';
					path[1] = ':';
					memcpy(path + 2, entry->path, (wlen(entry->path) + 1) * sizeof(WCHAR));
					for (position = 0; path[position]; ++position)
						if (path[position] == '/')
							path[position] = '\\';
					FREE(entry->path);
					entry->path = path;
				}
				if (entry->family && entry->family[0]) {
					FamilyEntry *family = find_family(entry->family);
					if (!family)
						family = add_family(entry->family);
					if (family)
						family_add(family, entry);
				}
				++font_count;
				walk += record;
			}
			(void)at;
		}
		if (records)
			FREE(records);
	}
	/* Families are listed in name order, as DirectWrite does. */
	{
		UINT32 outer, inner;
		for (outer = 1; outer < family_count; ++outer)
			for (inner = outer; inner > 0; --inner) {
				WCHAR *a = families[inner - 1].name, *b = families[inner].name;
				size_t position = 0;
				while (a[position] && fold(a[position]) == fold(b[position]))
					++position;
				if (fold(a[position]) > fold(b[position])) {
					FamilyEntry swap = families[inner - 1];
					families[inner - 1] = families[inner];
					families[inner] = swap;
				} else {
					break;
				}
			}
	}
	for (index = 0; index < sizeof(aliases) / sizeof(aliases[0]); ++index) {
		if (alias_target(aliases[index][1]) && !find_family(aliases[index][0])) {
			/* Adding a family can move the list, so the target is looked up again afterwards. */
			FamilyEntry *alias = add_family(aliases[index][0]);
			FamilyEntry *target = alias_target(aliases[index][1]);
			UINT32 entry;
			if (alias && target)
				for (entry = 0; entry < target->count; ++entry)
					family_add(alias, target->entries[entry]);
		}
	}
	return TRUE;
}

static void ensure_system(void)
{
	InitOnceExecuteOnce(&system_once, build_system, NULL, NULL);
}

/* ---- font data: one opened face of a font file ---- */
typedef struct FaceData {
	LONG refs;
	DWORD host;
	BYTE *bytes;
	DWORD size;
	WCHAR *path;
	DWORD index;
	INT32 info[20];
	DWORD face_type;
} FaceData;

static void face_data_release(FaceData *data)
{
	if (data && InterlockedDecrement(&data->refs) == 0) {
		RavenFontClose(data->host);
		if (data->bytes)
			FREE(data->bytes);
		if (data->path)
			FREE(data->path);
		FREE(data);
	}
}

/* Takes the bytes of a font file and opens one face of it with the host; the data owns the bytes. */
static FaceData *face_data_create(BYTE *bytes, DWORD size, DWORD index, const WCHAR *path)
{
	FaceData *data = ALLOC(sizeof(FaceData));
	if (!data)
		return NULL;
	data->refs = 1;
	data->bytes = bytes;
	data->size = size;
	data->index = index;
	data->path = path ? wdup(path) : NULL;
	data->host = RavenFontOpen(bytes, size, index);
	if (!data->host || !RavenFontInfo(data->host, data->info)) {
		data->refs = 1;
		face_data_release(data);
		return NULL;
	}
	/* A file that starts with OTTO holds CFF outlines; everything else here is TrueType. */
	data->face_type = size >= 4 && bytes[0] == 'O' && bytes[1] == 'T' && bytes[2] == 'T' && bytes[3] == 'O' ? DWRITE_FONT_FACE_TYPE_CFF : DWRITE_FONT_FACE_TYPE_TRUETYPE;
	return data;
}

static BYTE *read_file(const WCHAR *path, DWORD *size)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	BYTE *bytes = NULL;
	DWORD length, done = 0;
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	length = GetFileSize(file, NULL);
	if (length != INVALID_FILE_SIZE && length) {
		bytes = ALLOC(length);
		while (bytes && done < length) {
			DWORD read = 0;
			if (!ReadFile(file, bytes + done, length - done, &read, NULL) || !read) {
				FREE(bytes);
				bytes = NULL;
				break;
			}
			done += read;
		}
	}
	CloseHandle(file);
	*size = length;
	return bytes;
}

/* Faces of system files are opened once and shared. */
static FaceData *face_data_for_entry(const FontEntry *entry)
{
	static struct {
		const FontEntry *entry;
		FaceData *data;
	} cache[256];
	static SRWLOCK lock = SRWLOCK_INIT;
	UINT32 index;
	FaceData *result = NULL;
	if (entry->data) {
		InterlockedIncrement(&entry->data->refs);
		return entry->data;
	}
	AcquireSRWLockExclusive(&lock);
	for (index = 0; index < 256 && cache[index].entry; ++index)
		if (cache[index].entry == entry) {
			InterlockedIncrement(&cache[index].data->refs);
			ReleaseSRWLockExclusive(&lock);
			return cache[index].data;
		}
	{
		DWORD size = 0;
		BYTE *bytes = read_file(entry->path, &size);
		if (bytes)
			result = face_data_create(bytes, size, entry->index, entry->path);
		if (result && index < 256) {
			cache[index].entry = entry;
			cache[index].data = result;
			InterlockedIncrement(&result->refs);
		}
	}
	ReleaseSRWLockExclusive(&lock);
	return result;
}

/* ---- localized strings: one string in one locale ---- */
typedef struct {
	const IDWriteLocalizedStringsVtbl *vtbl;
	LONG refs;
	WCHAR *text;
} Strings;

static HRESULT STDMETHODCALLTYPE strings_query(IDWriteLocalizedStrings *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteLocalizedStrings)) {
		InterlockedIncrement(&((Strings *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE strings_add_ref(IDWriteLocalizedStrings *self) { return (ULONG)InterlockedIncrement(&((Strings *)self)->refs); }
static ULONG STDMETHODCALLTYPE strings_release(IDWriteLocalizedStrings *self)
{
	Strings *strings = (Strings *)self;
	LONG left = InterlockedDecrement(&strings->refs);
	if (!left) {
		FREE(strings->text);
		FREE(strings);
	}
	return (ULONG)left;
}
static UINT32 STDMETHODCALLTYPE strings_count(IDWriteLocalizedStrings *self) { return 1; }
static HRESULT STDMETHODCALLTYPE strings_find(IDWriteLocalizedStrings *self, const WCHAR *locale, UINT32 *index, WINBOOL *exists)
{
	BOOL found = locale && wequal_fold(locale, L"en-us");
	if (index)
		*index = found ? 0 : UINT32_MAX;
	if (exists)
		*exists = found;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE strings_locale_length(IDWriteLocalizedStrings *self, UINT32 index, UINT32 *length)
{
	if (index != 0)
		return E_INVALIDARG;
	*length = 5;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE strings_locale(IDWriteLocalizedStrings *self, UINT32 index, WCHAR *name, UINT32 size)
{
	if (index != 0)
		return E_INVALIDARG;
	if (size < 6)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(name, L"en-us", 6 * sizeof(WCHAR));
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE strings_length(IDWriteLocalizedStrings *self, UINT32 index, UINT32 *length)
{
	if (index != 0)
		return E_INVALIDARG;
	*length = (UINT32)wlen(((Strings *)self)->text);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE strings_get(IDWriteLocalizedStrings *self, UINT32 index, WCHAR *buffer, UINT32 size)
{
	const WCHAR *text = ((Strings *)self)->text;
	size_t length = wlen(text);
	if (index != 0)
		return E_INVALIDARG;
	if (size < length + 1)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(buffer, text, (length + 1) * sizeof(WCHAR));
	return S_OK;
}

static void *Strings_table_[IDWriteLocalizedStrings_methods];
static const IDWriteLocalizedStringsVtbl *strings_vtbl;

static IDWriteLocalizedStrings *make_strings(const WCHAR *text)
{
	Strings *strings;
	if (!strings_vtbl) {
		memcpy(Strings_table_, IDWriteLocalizedStrings_table, sizeof(Strings_table_));
		Strings_table_[IDWriteLocalizedStrings_QueryInterface] = (void *)strings_query;
		Strings_table_[IDWriteLocalizedStrings_AddRef] = (void *)strings_add_ref;
		Strings_table_[IDWriteLocalizedStrings_Release] = (void *)strings_release;
		Strings_table_[IDWriteLocalizedStrings_GetCount] = (void *)strings_count;
		Strings_table_[IDWriteLocalizedStrings_FindLocaleName] = (void *)strings_find;
		Strings_table_[IDWriteLocalizedStrings_GetLocaleNameLength] = (void *)strings_locale_length;
		Strings_table_[IDWriteLocalizedStrings_GetLocaleName] = (void *)strings_locale;
		Strings_table_[IDWriteLocalizedStrings_GetStringLength] = (void *)strings_length;
		Strings_table_[IDWriteLocalizedStrings_GetString] = (void *)strings_get;
		strings_vtbl = (const IDWriteLocalizedStringsVtbl *)Strings_table_;
	}
	strings = ALLOC(sizeof(Strings));
	if (!strings)
		return NULL;
	strings->vtbl = strings_vtbl;
	strings->refs = 1;
	strings->text = wdup(text);
	return (IDWriteLocalizedStrings *)strings;
}

/* ---- font file loader and stream: the way font bytes are handed over ---- */
typedef struct {
	const IDWriteFontFileStreamVtbl *vtbl;
	LONG refs;
	BYTE *bytes;
	UINT64 size;
} Stream;

static HRESULT STDMETHODCALLTYPE stream_query(IDWriteFontFileStream *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFileStream)) {
		InterlockedIncrement(&((Stream *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE stream_add_ref(IDWriteFontFileStream *self) { return (ULONG)InterlockedIncrement(&((Stream *)self)->refs); }
static ULONG STDMETHODCALLTYPE stream_release(IDWriteFontFileStream *self)
{
	Stream *stream = (Stream *)self;
	LONG left = InterlockedDecrement(&stream->refs);
	if (!left) {
		FREE(stream->bytes);
		FREE(stream);
	}
	return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE stream_read(IDWriteFontFileStream *self, const void **fragment, UINT64 offset, UINT64 size, void **context)
{
	Stream *stream = (Stream *)self;
	*context = NULL;
	if (offset > stream->size || size > stream->size - offset) {
		*fragment = NULL;
		return E_FAIL;
	}
	*fragment = stream->bytes + offset;
	return S_OK;
}
static void STDMETHODCALLTYPE stream_release_fragment(IDWriteFontFileStream *self, void *context) {}
static HRESULT STDMETHODCALLTYPE stream_size(IDWriteFontFileStream *self, UINT64 *size)
{
	*size = ((Stream *)self)->size;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE stream_time(IDWriteFontFileStream *self, UINT64 *time)
{
	*time = 0;
	return S_OK;
}

static void *Stream_table_[IDWriteFontFileStream_methods];

static IDWriteFontFileStream *make_stream(BYTE *bytes, UINT64 size)
{
	static const IDWriteFontFileStreamVtbl *vtbl;
	Stream *stream;
	if (!vtbl) {
		memcpy(Stream_table_, IDWriteFontFileStream_table, sizeof(Stream_table_));
		Stream_table_[IDWriteFontFileStream_QueryInterface] = (void *)stream_query;
		Stream_table_[IDWriteFontFileStream_AddRef] = (void *)stream_add_ref;
		Stream_table_[IDWriteFontFileStream_Release] = (void *)stream_release;
		Stream_table_[IDWriteFontFileStream_ReadFileFragment] = (void *)stream_read;
		Stream_table_[IDWriteFontFileStream_ReleaseFileFragment] = (void *)stream_release_fragment;
		Stream_table_[IDWriteFontFileStream_GetFileSize] = (void *)stream_size;
		Stream_table_[IDWriteFontFileStream_GetLastWriteTime] = (void *)stream_time;
		vtbl = (const IDWriteFontFileStreamVtbl *)Stream_table_;
	}
	stream = ALLOC(sizeof(Stream));
	if (!stream)
		return NULL;
	stream->vtbl = vtbl;
	stream->refs = 1;
	stream->bytes = bytes;
	stream->size = size;
	return (IDWriteFontFileStream *)stream;
}

/* The loader of the files of the system: the key is the wide path. It is a local file loader, so a program can learn the path back. */
typedef struct {
	const IDWriteFontFileLoaderVtbl *vtbl;
} SystemLoader;

static HRESULT STDMETHODCALLTYPE loader_query(IDWriteFontFileLoader *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFileLoader) || same_guid(iid, &IID_IDWriteLocalFontFileLoader)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE loader_add_ref(IDWriteFontFileLoader *self) { return 2; }
static ULONG STDMETHODCALLTYPE loader_release(IDWriteFontFileLoader *self) { return 1; }
static HRESULT STDMETHODCALLTYPE loader_create_stream(IDWriteFontFileLoader *self, const void *key, UINT32 key_size, IDWriteFontFileStream **stream)
{
	DWORD size = 0;
	BYTE *bytes;
	WCHAR *path;
	*stream = NULL;
	if (!key_size || key_size % sizeof(WCHAR))
		return E_INVALIDARG;
	path = ALLOC(key_size + sizeof(WCHAR));
	if (!path)
		return E_OUTOFMEMORY;
	memcpy(path, key, key_size);
	bytes = read_file(path, &size);
	FREE(path);
	if (!bytes)
		return DWRITE_E_FILENOTFOUND;
	*stream = make_stream(bytes, size);
	return *stream ? S_OK : E_OUTOFMEMORY;
}

/* The characters of the path in a key, without the terminator the key may carry. */
static UINT32 key_path_length(const void *key, UINT32 key_size)
{
	const WCHAR *path = key;
	UINT32 length = key_size / sizeof(WCHAR);
	while (length && !path[length - 1])
		--length;
	return length;
}
static HRESULT STDMETHODCALLTYPE loader_path_length(IDWriteLocalFontFileLoader *self, const void *key, UINT32 key_size, UINT32 *length)
{
	if (!key || !length)
		return E_INVALIDARG;
	*length = key_path_length(key, key_size);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE loader_path(IDWriteLocalFontFileLoader *self, const void *key, UINT32 key_size, WCHAR *path, UINT32 capacity)
{
	UINT32 length;
	if (!key || !path)
		return E_INVALIDARG;
	length = key_path_length(key, key_size);
	if (capacity <= length)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(path, key, length * sizeof(WCHAR));
	path[length] = 0;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE loader_write_time(IDWriteLocalFontFileLoader *self, const void *key, UINT32 key_size, FILETIME *time)
{
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	UINT32 length;
	WCHAR *path;
	BOOL found;
	if (!key || !time)
		return E_INVALIDARG;
	length = key_path_length(key, key_size);
	path = ALLOC((length + 1) * sizeof(WCHAR));
	if (!path)
		return E_OUTOFMEMORY;
	memcpy(path, key, length * sizeof(WCHAR));
	found = GetFileAttributesExW(path, GetFileExInfoStandard, &attributes);
	FREE(path);
	if (!found)
		return DWRITE_E_FILENOTFOUND;
	*time = attributes.ftLastWriteTime;
	return S_OK;
}

static void *Loader_table_[7];
static SystemLoader system_loader;

static IDWriteFontFileLoader *get_system_loader(void)
{
	if (!system_loader.vtbl) {
		/* IUnknown, CreateStreamFromKey, then the three path methods of IDWriteLocalFontFileLoader. */
		Loader_table_[0] = (void *)loader_query;
		Loader_table_[1] = (void *)loader_add_ref;
		Loader_table_[2] = (void *)loader_release;
		Loader_table_[3] = (void *)loader_create_stream;
		Loader_table_[4] = (void *)loader_path_length;
		Loader_table_[5] = (void *)loader_path;
		Loader_table_[6] = (void *)loader_write_time;
		system_loader.vtbl = (const IDWriteFontFileLoaderVtbl *)Loader_table_;
	}
	return (IDWriteFontFileLoader *)&system_loader;
}

/* ---- font files ---- */
typedef struct {
	const IDWriteFontFileVtbl *vtbl;
	LONG refs;
	BYTE *key;
	UINT32 key_size;
	IDWriteFontFileLoader *loader;
} FontFile;

static HRESULT STDMETHODCALLTYPE file_query(IDWriteFontFile *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFile)) {
		InterlockedIncrement(&((FontFile *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE file_add_ref(IDWriteFontFile *self) { return (ULONG)InterlockedIncrement(&((FontFile *)self)->refs); }
static ULONG STDMETHODCALLTYPE file_release(IDWriteFontFile *self)
{
	FontFile *file = (FontFile *)self;
	LONG left = InterlockedDecrement(&file->refs);
	if (!left) {
		if (file->loader)
			IDWriteFontFileLoader_Release(file->loader);
		FREE(file->key);
		FREE(file);
	}
	return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE file_key(IDWriteFontFile *self, const void **key, UINT32 *size)
{
	FontFile *file = (FontFile *)self;
	*key = file->key;
	*size = file->key_size;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE file_loader(IDWriteFontFile *self, IDWriteFontFileLoader **loader)
{
	FontFile *file = (FontFile *)self;
	IDWriteFontFileLoader_AddRef(file->loader);
	*loader = file->loader;
	return S_OK;
}

/* The bytes of the file behind a font file object, read through its loader. */
static BYTE *file_bytes(FontFile *file, DWORD *size)
{
	IDWriteFontFileStream *stream = NULL;
	UINT64 length = 0;
	const void *fragment = NULL;
	void *context = NULL;
	BYTE *copy = NULL;
	if (FAILED(IDWriteFontFileLoader_CreateStreamFromKey(file->loader, file->key, file->key_size, &stream)) || !stream)
		return NULL;
	if (SUCCEEDED(IDWriteFontFileStream_GetFileSize(stream, &length)) && length && length < 0x7fffffff &&
		SUCCEEDED(IDWriteFontFileStream_ReadFileFragment(stream, &fragment, 0, length, &context))) {
		copy = ALLOC((size_t)length);
		if (copy) {
			memcpy(copy, fragment, (size_t)length);
			*size = (DWORD)length;
		}
		IDWriteFontFileStream_ReleaseFileFragment(stream, context);
	}
	IDWriteFontFileStream_Release(stream);
	return copy;
}

static HRESULT STDMETHODCALLTYPE file_analyze(IDWriteFontFile *self, WINBOOL *supported, DWRITE_FONT_FILE_TYPE *file_type, DWRITE_FONT_FACE_TYPE *face_type, UINT32 *faces)
{
	DWORD size = 0;
	BYTE *bytes = file_bytes((FontFile *)self, &size);
	FaceData *data;
	DWORD kind;
	if (!bytes) {
		*supported = FALSE;
		*file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
		*face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
		*faces = 0;
		return S_OK;
	}
	kind = size >= 4 && bytes[0] == 't' && bytes[1] == 't' && bytes[2] == 'c' && bytes[3] == 'f' ? 3 : size >= 4 && bytes[0] == 'O' && bytes[1] == 'T' ? 1 : 2;
	data = face_data_create(bytes, size, 0, NULL);
	if (!data) {
		*supported = FALSE;
		*file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
		*face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
		*faces = 0;
		return S_OK;
	}
	*supported = TRUE;
	*file_type = (DWRITE_FONT_FILE_TYPE)kind;
	*face_type = kind == 3 ? DWRITE_FONT_FACE_TYPE_OPENTYPE_COLLECTION : (DWRITE_FONT_FACE_TYPE)data->face_type;
	*faces = (UINT32)data->info[13];
	face_data_release(data);
	return S_OK;
}

static void *FontFile_table_[IDWriteFontFile_methods];

static IDWriteFontFile *make_file(const void *key, UINT32 key_size, IDWriteFontFileLoader *loader)
{
	static const IDWriteFontFileVtbl *vtbl;
	FontFile *file;
	if (!vtbl) {
		memcpy(FontFile_table_, IDWriteFontFile_table, sizeof(FontFile_table_));
		FontFile_table_[IDWriteFontFile_QueryInterface] = (void *)file_query;
		FontFile_table_[IDWriteFontFile_AddRef] = (void *)file_add_ref;
		FontFile_table_[IDWriteFontFile_Release] = (void *)file_release;
		FontFile_table_[IDWriteFontFile_GetReferenceKey] = (void *)file_key;
		FontFile_table_[IDWriteFontFile_GetLoader] = (void *)file_loader;
		FontFile_table_[IDWriteFontFile_Analyze] = (void *)file_analyze;
		vtbl = (const IDWriteFontFileVtbl *)FontFile_table_;
	}
	file = ALLOC(sizeof(FontFile));
	if (!file)
		return NULL;
	file->vtbl = vtbl;
	file->refs = 1;
	file->key = ALLOC(key_size ? key_size : 1);
	if (!file->key) {
		FREE(file);
		return NULL;
	}
	memcpy(file->key, key, key_size);
	file->key_size = key_size;
	file->loader = loader;
	IDWriteFontFileLoader_AddRef(loader);
	return (IDWriteFontFile *)file;
}

/* ---- font metrics shared by faces and fonts ---- */
static void fill_metrics(const INT32 *info, DWRITE_FONT_METRICS *metrics)
{
	metrics->designUnitsPerEm = (UINT16)info[0];
	metrics->ascent = (UINT16)info[1];
	metrics->descent = (UINT16)info[2];
	metrics->lineGap = (INT16)info[3];
	metrics->capHeight = (UINT16)info[4];
	metrics->xHeight = (UINT16)info[5];
	metrics->underlinePosition = (INT16)info[6];
	metrics->underlineThickness = (UINT16)info[7];
	metrics->strikethroughPosition = (INT16)info[8];
	metrics->strikethroughThickness = (UINT16)info[9];
}

static void fill_metrics1(const INT32 *info, DWRITE_FONT_METRICS1 *metrics)
{
	fill_metrics(info, (DWRITE_FONT_METRICS *)metrics);
	metrics->glyphBoxLeft = (INT16)info[14];
	metrics->glyphBoxTop = (INT16)info[17];
	metrics->glyphBoxRight = (INT16)info[16];
	metrics->glyphBoxBottom = (INT16)info[15];
	metrics->subscriptPositionX = 0;
	metrics->subscriptPositionY = (INT16)(info[0] / 7);
	metrics->subscriptSizeX = (INT16)(info[0] * 6 / 10);
	metrics->subscriptSizeY = (INT16)(info[0] * 6 / 10);
	metrics->superscriptPositionX = 0;
	metrics->superscriptPositionY = (INT16)(info[0] * 3 / 5);
	metrics->superscriptSizeX = (INT16)(info[0] * 6 / 10);
	metrics->superscriptSizeY = (INT16)(info[0] * 6 / 10);
	metrics->hasTypographicMetrics = FALSE;
}

/* A metric rounded to whole pixels at an em size, kept in design units: the GDI compatible measures. */
static INT32 pixel_round(INT32 value, float em, INT32 units_per_em)
{
	float scaled = (float)value * em / (float)units_per_em;
	INT32 whole = (INT32)(scaled + (scaled < 0 ? -0.5f : 0.5f));
	return (INT32)((float)whole * (float)units_per_em / em + (whole < 0 ? -0.5f : 0.5f));
}

/* ---- font faces ---- */
/* IDWriteFontFace1 extends IDWriteFontFace, so one table serves both interfaces and one pointer names both. */
typedef struct {
	const IDWriteFontFace1Vtbl *vtbl;
	LONG refs;
	FaceData *data;
	IDWriteFontFile *file;
	DWRITE_FONT_SIMULATIONS simulations;
} Face;

#define FACE_OF(self) ((Face *)(self))
#define FACE_OF1(self) ((Face *)(self))

static HRESULT STDMETHODCALLTYPE face_query(IDWriteFontFace *self, REFIID iid, void **out)
{
	Face *face = FACE_OF(self);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFace) || same_guid(iid, &IID_IDWriteFontFace1))
		*out = face;
	else
		return no_interface(iid);
	InterlockedIncrement(&face->refs);
	return S_OK;
}
static ULONG STDMETHODCALLTYPE face_add_ref(IDWriteFontFace *self) { return (ULONG)InterlockedIncrement(&FACE_OF(self)->refs); }
static ULONG STDMETHODCALLTYPE face_release(IDWriteFontFace *self)
{
	Face *face = FACE_OF(self);
	LONG left = InterlockedDecrement(&face->refs);
	if (!left) {
		face_data_release(face->data);
		if (face->file)
			IDWriteFontFile_Release(face->file);
		FREE(face);
	}
	return (ULONG)left;
}
static DWRITE_FONT_FACE_TYPE STDMETHODCALLTYPE face_type(IDWriteFontFace *self) { return (DWRITE_FONT_FACE_TYPE)FACE_OF(self)->data->face_type; }
static HRESULT STDMETHODCALLTYPE face_files(IDWriteFontFace *self, UINT32 *count, IDWriteFontFile **files)
{
	Face *face = FACE_OF(self);
	if (!count)
		return E_INVALIDARG;
	if (!files) {
		*count = 1;
		return S_OK;
	}
	if (*count < 1)
		return E_INVALIDARG;
	if (!face->file) {
		/* A face made from a system path still names its file: the key is the path and the loader is the system one. */
		if (!face->data->path)
			return E_FAIL;
		face->file = make_file(face->data->path, (UINT32)((wlen(face->data->path) + 1) * sizeof(WCHAR)), get_system_loader());
		if (!face->file)
			return E_OUTOFMEMORY;
	}
	IDWriteFontFile_AddRef(face->file);
	files[0] = face->file;
	*count = 1;
	return S_OK;
}
static UINT32 STDMETHODCALLTYPE face_index(IDWriteFontFace *self) { return FACE_OF(self)->data->index; }
static DWRITE_FONT_SIMULATIONS STDMETHODCALLTYPE face_simulations(IDWriteFontFace *self) { return FACE_OF(self)->simulations; }
static WINBOOL STDMETHODCALLTYPE face_symbol(IDWriteFontFace *self) { return (FACE_OF(self)->data->info[11] & 32) != 0; }
static void STDMETHODCALLTYPE face_metrics(IDWriteFontFace *self, DWRITE_FONT_METRICS *metrics) { fill_metrics(FACE_OF(self)->data->info, metrics); }
static UINT16 STDMETHODCALLTYPE face_glyph_count(IDWriteFontFace *self) { return (UINT16)FACE_OF(self)->data->info[10]; }
static HRESULT STDMETHODCALLTYPE face_design_metrics(IDWriteFontFace *self, const UINT16 *glyphs, UINT32 count, DWRITE_GLYPH_METRICS *metrics, WINBOOL sideways)
{
	Face *face = FACE_OF(self);
	UINT32 index;
	for (index = 0; index < count; ++index) {
		INT32 raw[7] = {0};
		if (!cached_metrics(face->data->host, glyphs[index], raw)) {
			memset(&metrics[index], 0, sizeof(metrics[index]));
			continue;
		}
		metrics[index].leftSideBearing = raw[0];
		metrics[index].advanceWidth = (UINT32)raw[1];
		metrics[index].rightSideBearing = raw[2];
		metrics[index].topSideBearing = raw[3];
		metrics[index].advanceHeight = (UINT32)raw[4];
		metrics[index].bottomSideBearing = raw[5];
		metrics[index].verticalOriginY = raw[6];
	}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE face_glyph_indices(IDWriteFontFace *self, const UINT32 *codepoints, UINT32 count, UINT16 *glyphs)
{
	if (!count)
		return S_OK;
	return cached_glyphs(FACE_OF(self)->data->host, codepoints, count, glyphs) ? S_OK : E_FAIL;
}
static UINT32 swap_tag(UINT32 tag)
{
	return (tag & 0xff) << 24 | (tag & 0xff00) << 8 | (tag >> 8 & 0xff00) | tag >> 24;
}
static HRESULT STDMETHODCALLTYPE face_table(IDWriteFontFace *self, UINT32 tag, const void **data, UINT32 *size, void **context, WINBOOL *exists)
{
	Face *face = FACE_OF(self);
	DWORD length = RavenFontTable(face->data->host, swap_tag(tag), 0, NULL, 0);
	BYTE *copy;
	*data = NULL;
	*size = 0;
	*context = NULL;
	*exists = FALSE;
	if (length == 0xffffffffU || length == 0)
		return S_OK;
	copy = ALLOC(length);
	if (!copy)
		return E_OUTOFMEMORY;
	RavenFontTable(face->data->host, swap_tag(tag), 0, copy, length);
	*data = copy;
	*size = length;
	*context = copy;
	*exists = TRUE;
	return S_OK;
}
static void STDMETHODCALLTYPE face_release_table(IDWriteFontFace *self, void *context)
{
	if (context)
		FREE(context);
}

/* Glyph outlines go to the geometry sink with y downward from the baseline, in units of the em size. */
static HRESULT STDMETHODCALLTYPE face_outline(IDWriteFontFace *self, FLOAT em, const UINT16 *glyphs, const FLOAT *advances, const DWRITE_GLYPH_OFFSET *offsets,
	UINT32 count, WINBOOL sideways, WINBOOL rtl, IDWriteGeometrySink *sink)
{
	Face *face = FACE_OF(self);
	float scale = em / (float)face->data->info[0], pen = 0;
	UINT32 index;
	if (!sink)
		return E_INVALIDARG;
	ID2D1SimplifiedGeometrySink_SetFillMode(sink, D2D1_FILL_MODE_WINDING);
	for (index = 0; index < count; ++index) {
		DWORD size = RavenFontOutline(face->data->host, glyphs[index], NULL, 0);
		float shift_x = pen + (offsets ? offsets[index].advanceOffset : 0), shift_y = offsets ? -offsets[index].ascenderOffset : 0;
		if (size) {
			BYTE *records = ALLOC(size);
			if (records && RavenFontOutline(face->data->host, glyphs[index], records, size) == size) {
				DWORD at;
				D2D1_POINT_2F current = {0, 0};
				BOOL open = FALSE;
				for (at = 0; at + 28 <= size; at += 28) {
					const INT32 *record = (const INT32 *)(records + at);
					float x1 = shift_x + (float)record[1] * scale, y1 = shift_y - (float)record[2] * scale;
					float x2 = shift_x + (float)record[3] * scale, y2 = shift_y - (float)record[4] * scale;
					float x3 = shift_x + (float)record[5] * scale, y3 = shift_y - (float)record[6] * scale;
					if (record[0] == 0) {
						D2D1_POINT_2F start = {x1, y1};
						ID2D1SimplifiedGeometrySink_BeginFigure(sink, start, D2D1_FIGURE_BEGIN_FILLED);
						current = start;
						open = TRUE;
					} else if (record[0] == 1) {
						D2D1_POINT_2F to = {x1, y1};
						ID2D1SimplifiedGeometrySink_AddLines(sink, &to, 1);
						current = to;
					} else if (record[0] == 2) {
						/* A quadratic curve is sent as the cubic with the same shape. */
						D2D1_BEZIER_SEGMENT segment;
						segment.point1.x = current.x + 2.0f / 3.0f * (x1 - current.x);
						segment.point1.y = current.y + 2.0f / 3.0f * (y1 - current.y);
						segment.point2.x = x2 + 2.0f / 3.0f * (x1 - x2);
						segment.point2.y = y2 + 2.0f / 3.0f * (y1 - y2);
						segment.point3.x = x2;
						segment.point3.y = y2;
						ID2D1SimplifiedGeometrySink_AddBeziers(sink, &segment, 1);
						current = segment.point3;
					} else if (record[0] == 3) {
						D2D1_BEZIER_SEGMENT segment = {{x1, y1}, {x2, y2}, {x3, y3}};
						ID2D1SimplifiedGeometrySink_AddBeziers(sink, &segment, 1);
						current = segment.point3;
					} else if (open) {
						ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
						open = FALSE;
					}
				}
				if (open)
					ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
			}
			if (records)
				FREE(records);
		}
		if (advances)
			pen += advances[index];
		else {
			INT32 raw[7] = {0};
			if (cached_metrics(face->data->host, glyphs[index], raw))
				pen += (float)raw[1] * scale;
		}
	}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE face_recommended(IDWriteFontFace *self, FLOAT em, FLOAT ppd, DWRITE_MEASURING_MODE mode, IDWriteRenderingParams *params, DWRITE_RENDERING_MODE *rendering)
{
	*rendering = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE face_gdi_metrics(IDWriteFontFace *self, FLOAT em, FLOAT ppd, const DWRITE_MATRIX *transform, DWRITE_FONT_METRICS *metrics)
{
	const INT32 *info = FACE_OF(self)->data->info;
	INT32 rounded[20];
	float pixels = em * ppd;
	int index;
	memcpy(rounded, info, sizeof(rounded));
	if (pixels > 0)
		for (index = 1; index <= 9; ++index)
			rounded[index] = pixel_round(info[index], pixels, info[0]);
	fill_metrics(rounded, metrics);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE face_gdi_glyph_metrics(IDWriteFontFace *self, FLOAT em, FLOAT ppd, const DWRITE_MATRIX *transform, WINBOOL natural,
	const UINT16 *glyphs, UINT32 count, DWRITE_GLYPH_METRICS *metrics, WINBOOL sideways)
{
	Face *face = FACE_OF(self);
	float pixels = em * ppd;
	UINT32 index;
	face_design_metrics(self, glyphs, count, metrics, sideways);
	if (pixels > 0)
		for (index = 0; index < count; ++index)
			metrics[index].advanceWidth = (UINT32)pixel_round((INT32)metrics[index].advanceWidth, pixels, face->data->info[0]);
	return S_OK;
}

/* IDWriteFontFace1 */
static void STDMETHODCALLTYPE face1_metrics(IDWriteFontFace1 *self, DWRITE_FONT_METRICS1 *metrics) { fill_metrics1(FACE_OF1(self)->data->info, metrics); }
static HRESULT STDMETHODCALLTYPE face1_gdi_metrics(IDWriteFontFace1 *self, FLOAT em, FLOAT ppd, const DWRITE_MATRIX *transform, DWRITE_FONT_METRICS1 *metrics)
{
	DWRITE_FONT_METRICS plain;
	HRESULT result = face_gdi_metrics((IDWriteFontFace *)FACE_OF1(self), em, ppd, transform, &plain);
	fill_metrics1(FACE_OF1(self)->data->info, metrics);
	memcpy(metrics, &plain, sizeof(plain));
	return result;
}
static void STDMETHODCALLTYPE face1_caret(IDWriteFontFace1 *self, DWRITE_CARET_METRICS *metrics)
{
	metrics->slopeRise = (INT16)((FACE_OF1(self)->data->info[11] & 2) ? 5 : 1);
	metrics->slopeRun = (INT16)((FACE_OF1(self)->data->info[11] & 2) ? 1 : 0);
	metrics->offset = 0;
}
static HRESULT STDMETHODCALLTYPE face1_ranges(IDWriteFontFace1 *self, UINT32 max, DWRITE_UNICODE_RANGE *ranges, UINT32 *count)
{
	*count = 1;
	if (max < 1)
		return E_NOT_SUFFICIENT_BUFFER;
	ranges[0].first = 0;
	ranges[0].last = 0x10ffff;
	return S_OK;
}
static WINBOOL STDMETHODCALLTYPE face1_mono(IDWriteFontFace1 *self) { return (FACE_OF1(self)->data->info[11] & 8) != 0; }
static HRESULT STDMETHODCALLTYPE face1_design_advances(IDWriteFontFace1 *self, UINT32 count, const UINT16 *glyphs, INT32 *advances, WINBOOL sideways)
{
	Face *face = FACE_OF1(self);
	UINT32 index;
	for (index = 0; index < count; ++index) {
		INT32 raw[7] = {0};
		cached_metrics(face->data->host, glyphs[index], raw);
		advances[index] = raw[1];
	}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE face1_gdi_advances(IDWriteFontFace1 *self, FLOAT em, FLOAT ppd, const DWRITE_MATRIX *transform, WINBOOL natural, WINBOOL sideways,
	UINT32 count, const UINT16 *glyphs, INT32 *advances)
{
	Face *face = FACE_OF1(self);
	UINT32 index;
	float pixels = em * ppd;
	face1_design_advances(self, count, glyphs, advances, sideways);
	if (pixels > 0)
		for (index = 0; index < count; ++index)
			advances[index] = pixel_round(advances[index], pixels, face->data->info[0]);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE face1_kerning(IDWriteFontFace1 *self, UINT32 count, const UINT16 *glyphs, INT32 *adjustments)
{
	UINT32 index;
	for (index = 0; index < count; ++index)
		adjustments[index] = 0;
	return S_OK;
}
static WINBOOL STDMETHODCALLTYPE face1_has_kerning(IDWriteFontFace1 *self) { return FALSE; }
static HRESULT STDMETHODCALLTYPE face1_recommended(IDWriteFontFace1 *self, FLOAT em, FLOAT dpi_x, FLOAT dpi_y, const DWRITE_MATRIX *transform, WINBOOL sideways,
	DWRITE_OUTLINE_THRESHOLD threshold, DWRITE_MEASURING_MODE measuring, DWRITE_RENDERING_MODE *rendering)
{
	*rendering = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE face1_vertical_variants(IDWriteFontFace1 *self, UINT32 count, const UINT16 *nominal, UINT16 *vertical)
{
	memcpy(vertical, nominal, count * sizeof(UINT16));
	return S_OK;
}
static WINBOOL STDMETHODCALLTYPE face1_has_vertical(IDWriteFontFace1 *self) { return FALSE; }

static void *Face1_table_[IDWriteFontFace1_methods];
static const IDWriteFontFace1Vtbl *face_vtbl;

static void init_face_tables(void)
{
	if (face_vtbl)
		return;
	memcpy(Face1_table_, IDWriteFontFace1_table, sizeof(Face1_table_));
	Face1_table_[IDWriteFontFace_QueryInterface] = (void *)face_query;
	Face1_table_[IDWriteFontFace_AddRef] = (void *)face_add_ref;
	Face1_table_[IDWriteFontFace_Release] = (void *)face_release;
	Face1_table_[IDWriteFontFace_GetType] = (void *)face_type;
	Face1_table_[IDWriteFontFace_GetFiles] = (void *)face_files;
	Face1_table_[IDWriteFontFace_GetIndex] = (void *)face_index;
	Face1_table_[IDWriteFontFace_GetSimulations] = (void *)face_simulations;
	Face1_table_[IDWriteFontFace_IsSymbolFont] = (void *)face_symbol;
	Face1_table_[IDWriteFontFace_GetMetrics] = (void *)face_metrics;
	Face1_table_[IDWriteFontFace_GetGlyphCount] = (void *)face_glyph_count;
	Face1_table_[IDWriteFontFace_GetDesignGlyphMetrics] = (void *)face_design_metrics;
	Face1_table_[IDWriteFontFace_GetGlyphIndices] = (void *)face_glyph_indices;
	Face1_table_[IDWriteFontFace_TryGetFontTable] = (void *)face_table;
	Face1_table_[IDWriteFontFace_ReleaseFontTable] = (void *)face_release_table;
	Face1_table_[IDWriteFontFace_GetGlyphRunOutline] = (void *)face_outline;
	Face1_table_[IDWriteFontFace_GetRecommendedRenderingMode] = (void *)face_recommended;
	Face1_table_[IDWriteFontFace_GetGdiCompatibleMetrics] = (void *)face_gdi_metrics;
	Face1_table_[IDWriteFontFace_GetGdiCompatibleGlyphMetrics] = (void *)face_gdi_glyph_metrics;
	Face1_table_[IDWriteFontFace1_IDWriteFontFace1_GetMetrics] = (void *)face1_metrics;
	Face1_table_[IDWriteFontFace1_IDWriteFontFace1_GetGdiCompatibleMetrics] = (void *)face1_gdi_metrics;
	Face1_table_[IDWriteFontFace1_GetCaretMetrics] = (void *)face1_caret;
	Face1_table_[IDWriteFontFace1_GetUnicodeRanges] = (void *)face1_ranges;
	Face1_table_[IDWriteFontFace1_IsMonospacedFont] = (void *)face1_mono;
	Face1_table_[IDWriteFontFace1_GetDesignGlyphAdvances] = (void *)face1_design_advances;
	Face1_table_[IDWriteFontFace1_GetGdiCompatibleGlyphAdvances] = (void *)face1_gdi_advances;
	Face1_table_[IDWriteFontFace1_GetKerningPairAdjustments] = (void *)face1_kerning;
	Face1_table_[IDWriteFontFace1_HasKerningPairs] = (void *)face1_has_kerning;
	Face1_table_[IDWriteFontFace1_IDWriteFontFace1_GetRecommendedRenderingMode] = (void *)face1_recommended;
	Face1_table_[IDWriteFontFace1_GetVerticalGlyphVariants] = (void *)face1_vertical_variants;
	Face1_table_[IDWriteFontFace1_HasVerticalGlyphVariants] = (void *)face1_has_vertical;
	face_vtbl = (const IDWriteFontFace1Vtbl *)Face1_table_;
}

static IDWriteFontFace *make_face(FaceData *data, IDWriteFontFile *file, DWRITE_FONT_SIMULATIONS simulations)
{
	Face *face;
	init_face_tables();
	face = ALLOC(sizeof(Face));
	if (!face)
		return NULL;
	face->vtbl = face_vtbl;
	face->refs = 1;
	face->data = data;
	face->file = file;
	if (file)
		IDWriteFontFile_AddRef(file);
	face->simulations = simulations;
	return (IDWriteFontFace *)face;
}

/* ---- fonts and families ---- */
typedef struct {
	const IDWriteFont1Vtbl *vtbl;
	LONG refs;
	FontEntry *entry;
	FamilyEntry *family;
} Font;

typedef struct {
	const IDWriteFontFamilyVtbl *vtbl;
	LONG refs;
	FamilyEntry *family;
} Family;

typedef struct {
	const IDWriteFontCollectionVtbl *vtbl;
} Collection;

static Collection system_collection;
static IDWriteFontCollection *get_collection(void);
static IDWriteFont *make_font(FontEntry *entry, FamilyEntry *family);
static IDWriteFontFamily *make_family(FamilyEntry *family);
static IDWriteFontFaceReference *make_reference(FontEntry *entry);
static IDWriteFontSet *get_system_set(void);

#define FONT_OF(self) ((Font *)(self))

static HRESULT STDMETHODCALLTYPE font_query(IDWriteFont *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFont) || same_guid(iid, &IID_IDWriteFont1) || same_guid(iid, &IID_IDWriteFont2) ||
		same_guid(iid, &IID_IDWriteFont3)) {
		InterlockedIncrement(&FONT_OF(self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE font_add_ref(IDWriteFont *self) { return (ULONG)InterlockedIncrement(&FONT_OF(self)->refs); }
static ULONG STDMETHODCALLTYPE font_release(IDWriteFont *self)
{
	LONG left = InterlockedDecrement(&FONT_OF(self)->refs);
	if (!left)
		FREE(FONT_OF(self));
	return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE font_family(IDWriteFont *self, IDWriteFontFamily **family)
{
	*family = make_family(FONT_OF(self)->family);
	return *family ? S_OK : E_OUTOFMEMORY;
}
static DWRITE_FONT_WEIGHT STDMETHODCALLTYPE font_weight(IDWriteFont *self) { return (DWRITE_FONT_WEIGHT)FONT_OF(self)->entry->weight; }
static DWRITE_FONT_STRETCH STDMETHODCALLTYPE font_stretch(IDWriteFont *self) { return (DWRITE_FONT_STRETCH)FONT_OF(self)->entry->stretch; }
static DWRITE_FONT_STYLE STDMETHODCALLTYPE font_style(IDWriteFont *self) { return (DWRITE_FONT_STYLE)FONT_OF(self)->entry->style; }
static WINBOOL STDMETHODCALLTYPE font_symbol(IDWriteFont *self) { return (FONT_OF(self)->entry->flags & 1) != 0; }
static HRESULT STDMETHODCALLTYPE font_face_names(IDWriteFont *self, IDWriteLocalizedStrings **names)
{
	*names = make_strings(FONT_OF(self)->entry->face ? FONT_OF(self)->entry->face : L"Regular");
	return *names ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE font_info_strings(IDWriteFont *self, DWRITE_INFORMATIONAL_STRING_ID id, IDWriteLocalizedStrings **strings, WINBOOL *exists)
{
	FontEntry *entry = FONT_OF(self)->entry;
	/* The OpenType name ids that the informational strings stand for; the names the host listed answer the common ones directly. */
	static const int name_ids[] = {-1, 0, 5, 7, 8, 9, 12, 10, 11, 13, 14, 1, 2, 16, 17, 19, 4, 6, 20};
	const WCHAR *text = NULL;
	*strings = NULL;
	*exists = FALSE;
	if (id == DWRITE_INFORMATIONAL_STRING_FULL_NAME)
		text = entry->full;
	else if (id == DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME)
		text = entry->postscript;
	else if (id == DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES)
		text = entry->win32 && entry->win32[0] ? entry->win32 : entry->family;
	if (!text && (int)id > 0 && (size_t)id < sizeof(name_ids) / sizeof(name_ids[0])) {
		FaceData *data = face_data_for_entry(entry);
		if (data) {
			char buffer[512];
			DWORD length = RavenFontName(data->host, (DWORD)name_ids[id], buffer, sizeof(buffer));
			if (length && length < sizeof(buffer)) {
				WCHAR *wide = widen(buffer);
				if (wide) {
					*strings = make_strings(wide);
					*exists = *strings != NULL;
					FREE(wide);
				}
			}
			face_data_release(data);
		}
		return S_OK;
	}
	if (text && text[0]) {
		*strings = make_strings(text);
		*exists = *strings != NULL;
	}
	return S_OK;
}
static DWRITE_FONT_SIMULATIONS STDMETHODCALLTYPE font_simulations(IDWriteFont *self) { return DWRITE_FONT_SIMULATIONS_NONE; }
static void STDMETHODCALLTYPE font_metrics(IDWriteFont *self, DWRITE_FONT_METRICS *metrics)
{
	FaceData *data = face_data_for_entry(FONT_OF(self)->entry);
	memset(metrics, 0, sizeof(*metrics));
	if (data) {
		fill_metrics(data->info, metrics);
		face_data_release(data);
	}
}
static HRESULT STDMETHODCALLTYPE font_has_character(IDWriteFont *self, UINT32 value, WINBOOL *exists)
{
	FaceData *data = face_data_for_entry(FONT_OF(self)->entry);
	UINT16 glyph = 0;
	*exists = FALSE;
	if (!data)
		return E_FAIL;
	cached_glyphs(data->host, &value, 1, &glyph);
	*exists = glyph != 0;
	face_data_release(data);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE font_create_face(IDWriteFont *self, IDWriteFontFace **face)
{
	FaceData *data = face_data_for_entry(FONT_OF(self)->entry);
	*face = NULL;
	if (!data)
		return DWRITE_E_FILENOTFOUND;
	*face = make_face(data, FONT_OF(self)->entry->file, DWRITE_FONT_SIMULATIONS_NONE);
	if (!*face) {
		face_data_release(data);
		return E_OUTOFMEMORY;
	}
	return S_OK;
}
static void STDMETHODCALLTYPE font1_metrics(IDWriteFont1 *self, DWRITE_FONT_METRICS1 *metrics)
{
	FaceData *data = face_data_for_entry(FONT_OF(self)->entry);
	memset(metrics, 0, sizeof(*metrics));
	if (data) {
		fill_metrics1(data->info, metrics);
		face_data_release(data);
	}
}
static void STDMETHODCALLTYPE font1_panose(IDWriteFont1 *self, DWRITE_PANOSE *panose) { memset(panose, 0, sizeof(*panose)); }
static HRESULT STDMETHODCALLTYPE font1_ranges(IDWriteFont1 *self, UINT32 max, DWRITE_UNICODE_RANGE *ranges, UINT32 *count)
{
	*count = 1;
	if (max < 1)
		return E_NOT_SUFFICIENT_BUFFER;
	ranges[0].first = 0;
	ranges[0].last = 0x10ffff;
	return S_OK;
}
static WINBOOL STDMETHODCALLTYPE font1_mono(IDWriteFont1 *self) { return (FONT_OF(self)->entry->flags & 2) != 0; }

/* IDWriteFont2 and IDWriteFont3: the fonts are local files without color glyph tables. */
static WINBOOL STDMETHODCALLTYPE font2_color(IDWriteFont *self) { return FALSE; }
static WINBOOL STDMETHODCALLTYPE font3_equals(IDWriteFont *self, IDWriteFont *other);
static HRESULT STDMETHODCALLTYPE font3_reference(IDWriteFont *self, IDWriteFontFaceReference **reference)
{
	if (!reference)
		return E_POINTER;
	*reference = make_reference(FONT_OF(self)->entry);
	return *reference ? S_OK : E_OUTOFMEMORY;
}
static WINBOOL STDMETHODCALLTYPE font3_has_character(IDWriteFont *self, UINT32 value)
{
	WINBOOL exists = FALSE;
	return SUCCEEDED(font_has_character(self, value, &exists)) && exists;
}
static DWRITE_LOCALITY STDMETHODCALLTYPE font3_locality(IDWriteFont *self) { return DWRITE_LOCALITY_LOCAL; }

static void *Font1_table_[IDWriteFont3_methods];
static const IDWriteFont1Vtbl *font_vtbl;

static IDWriteFont *make_font(FontEntry *entry, FamilyEntry *family)
{
	Font *font;
	if (!font_vtbl) {
		memcpy(Font1_table_, IDWriteFont3_table, sizeof(Font1_table_));
		Font1_table_[IDWriteFont_QueryInterface] = (void *)font_query;
		Font1_table_[IDWriteFont_AddRef] = (void *)font_add_ref;
		Font1_table_[IDWriteFont_Release] = (void *)font_release;
		Font1_table_[IDWriteFont_GetFontFamily] = (void *)font_family;
		Font1_table_[IDWriteFont_GetWeight] = (void *)font_weight;
		Font1_table_[IDWriteFont_GetStretch] = (void *)font_stretch;
		Font1_table_[IDWriteFont_GetStyle] = (void *)font_style;
		Font1_table_[IDWriteFont_IsSymbolFont] = (void *)font_symbol;
		Font1_table_[IDWriteFont_GetFaceNames] = (void *)font_face_names;
		Font1_table_[IDWriteFont_GetInformationalStrings] = (void *)font_info_strings;
		Font1_table_[IDWriteFont_GetSimulations] = (void *)font_simulations;
		Font1_table_[IDWriteFont_GetMetrics] = (void *)font_metrics;
		Font1_table_[IDWriteFont_HasCharacter] = (void *)font_has_character;
		Font1_table_[IDWriteFont_CreateFontFace] = (void *)font_create_face;
		Font1_table_[IDWriteFont1_IDWriteFont1_GetMetrics] = (void *)font1_metrics;
		Font1_table_[IDWriteFont1_GetPanose] = (void *)font1_panose;
		Font1_table_[IDWriteFont1_GetUnicodeRanges] = (void *)font1_ranges;
		Font1_table_[IDWriteFont1_IsMonospacedFont] = (void *)font1_mono;
		Font1_table_[IDWriteFont3_IsColorFont] = (void *)font2_color;
		Font1_table_[IDWriteFont3_Equals] = (void *)font3_equals;
		Font1_table_[IDWriteFont3_GetFontFaceReference] = (void *)font3_reference;
		Font1_table_[IDWriteFont3_IDWriteFont3_HasCharacter] = (void *)font3_has_character;
		Font1_table_[IDWriteFont3_GetLocality] = (void *)font3_locality;
		font_vtbl = (const IDWriteFont1Vtbl *)Font1_table_;
	}
	font = ALLOC(sizeof(Font));
	if (!font)
		return NULL;
	font->vtbl = font_vtbl;
	font->refs = 1;
	font->entry = entry;
	font->family = family;
	return (IDWriteFont *)font;
}

/* How far a face is from the wanted weight, width and slant: a mismatch of slant counts most, then width, then weight. */
static unsigned match_distance(const FontEntry *entry, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch, DWRITE_FONT_STYLE style)
{
	unsigned style_cost = entry->style == (DWORD)style ? 0 : (entry->style != 0 && style != 0) ? 1000 : 100000;
	unsigned stretch_cost = (unsigned)(entry->stretch > (DWORD)stretch ? entry->stretch - stretch : stretch - entry->stretch) * 1000;
	unsigned weight_cost = (unsigned)(entry->weight > (DWORD)weight ? entry->weight - weight : weight - entry->weight);
	return style_cost + stretch_cost / 10 + weight_cost;
}

static FontEntry *best_match(FamilyEntry *family, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch, DWRITE_FONT_STYLE style)
{
	FontEntry *best = NULL;
	unsigned best_distance = 0xffffffffU;
	UINT32 index;
	for (index = 0; index < family->count; ++index) {
		unsigned distance = match_distance(family->entries[index], weight, stretch, style);
		if (distance < best_distance) {
			best_distance = distance;
			best = family->entries[index];
		}
	}
	return best;
}

static WINBOOL STDMETHODCALLTYPE font3_equals(IDWriteFont *self, IDWriteFont *other)
{
	return other && FONT_OF(other)->vtbl == font_vtbl && FONT_OF(other)->entry == FONT_OF(self)->entry;
}

#define FAMILY_OF(self) ((Family *)(self))

static HRESULT STDMETHODCALLTYPE family_query(IDWriteFontFamily *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFamily) || same_guid(iid, &IID_IDWriteFontList) ||
		same_guid(iid, &IID_IDWriteFontFamily1)) {
		InterlockedIncrement(&FAMILY_OF(self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE family_add_ref(IDWriteFontFamily *self) { return (ULONG)InterlockedIncrement(&FAMILY_OF(self)->refs); }
static ULONG STDMETHODCALLTYPE family_release(IDWriteFontFamily *self)
{
	LONG left = InterlockedDecrement(&FAMILY_OF(self)->refs);
	if (!left)
		FREE(FAMILY_OF(self));
	return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE family_collection(IDWriteFontFamily *self, IDWriteFontCollection **collection)
{
	*collection = get_collection();
	return S_OK;
}
static UINT32 STDMETHODCALLTYPE family_count_fonts(IDWriteFontFamily *self) { return FAMILY_OF(self)->family->count; }
static HRESULT STDMETHODCALLTYPE family_font(IDWriteFontFamily *self, UINT32 index, IDWriteFont **font)
{
	Family *family = FAMILY_OF(self);
	*font = NULL;
	if (index >= family->family->count)
		return E_INVALIDARG;
	*font = make_font(family->family->entries[index], family->family);
	return *font ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE family_names(IDWriteFontFamily *self, IDWriteLocalizedStrings **names)
{
	*names = make_strings(FAMILY_OF(self)->family->name);
	return *names ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE family_first_match(IDWriteFontFamily *self, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch, DWRITE_FONT_STYLE style, IDWriteFont **font)
{
	Family *family = FAMILY_OF(self);
	FontEntry *entry = best_match(family->family, weight, stretch, style);
	*font = NULL;
	if (!entry)
		return E_FAIL;
	*font = make_font(entry, family->family);
	return *font ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE family_matching(IDWriteFontFamily *self, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch, DWRITE_FONT_STYLE style, IDWriteFontList **fonts)
{
	/* The family answers as the list: its fonts in the family's own order. */
	InterlockedIncrement(&FAMILY_OF(self)->refs);
	*fonts = (IDWriteFontList *)self;
	return S_OK;
}

/* IDWriteFontFamily1: every font is local. */
static DWRITE_LOCALITY STDMETHODCALLTYPE family1_locality(IDWriteFontFamily *self, UINT32 index) { return DWRITE_LOCALITY_LOCAL; }
static HRESULT STDMETHODCALLTYPE family1_reference(IDWriteFontFamily *self, UINT32 index, IDWriteFontFaceReference **reference)
{
	Family *family = FAMILY_OF(self);
	if (!reference)
		return E_POINTER;
	*reference = NULL;
	if (index >= family->family->count)
		return E_INVALIDARG;
	*reference = make_reference(family->family->entries[index]);
	return *reference ? S_OK : E_OUTOFMEMORY;
}

static void *Family_table_[IDWriteFontFamily1_methods];
static const IDWriteFontFamilyVtbl *family_vtbl;

static IDWriteFontFamily *make_family(FamilyEntry *entry)
{
	Family *family;
	if (!family_vtbl) {
		memcpy(Family_table_, IDWriteFontFamily1_table, sizeof(Family_table_));
		Family_table_[IDWriteFontFamily_QueryInterface] = (void *)family_query;
		Family_table_[IDWriteFontFamily_AddRef] = (void *)family_add_ref;
		Family_table_[IDWriteFontFamily_Release] = (void *)family_release;
		Family_table_[IDWriteFontFamily_GetFontCollection] = (void *)family_collection;
		Family_table_[IDWriteFontFamily_GetFontCount] = (void *)family_count_fonts;
		Family_table_[IDWriteFontFamily_GetFont] = (void *)family_font;
		Family_table_[IDWriteFontFamily_GetFamilyNames] = (void *)family_names;
		Family_table_[IDWriteFontFamily_GetFirstMatchingFont] = (void *)family_first_match;
		Family_table_[IDWriteFontFamily_GetMatchingFonts] = (void *)family_matching;
		Family_table_[IDWriteFontFamily1_GetFontLocality] = (void *)family1_locality;
		Family_table_[IDWriteFontFamily1_IDWriteFontFamily1_GetFont] = (void *)family_font;
		Family_table_[IDWriteFontFamily1_GetFontFaceReference] = (void *)family1_reference;
		family_vtbl = (const IDWriteFontFamilyVtbl *)Family_table_;
	}
	family = ALLOC(sizeof(Family));
	if (!family)
		return NULL;
	family->vtbl = family_vtbl;
	family->refs = 1;
	family->family = entry;
	return (IDWriteFontFamily *)family;
}

/* ---- the system font collection ---- */
static HRESULT STDMETHODCALLTYPE collection_query(IDWriteFontCollection *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontCollection) || same_guid(iid, &IID_IDWriteFontCollection1)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE collection_add_ref(IDWriteFontCollection *self) { return 2; }
static ULONG STDMETHODCALLTYPE collection_release(IDWriteFontCollection *self) { return 1; }
static UINT32 STDMETHODCALLTYPE collection_count(IDWriteFontCollection *self)
{
	ensure_system();
	return family_count;
}
static HRESULT STDMETHODCALLTYPE collection_family(IDWriteFontCollection *self, UINT32 index, IDWriteFontFamily **family)
{
	ensure_system();
	*family = NULL;
	if (index >= family_count)
		return E_INVALIDARG;
	*family = make_family(&families[index]);
	return *family ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE collection_find(IDWriteFontCollection *self, const WCHAR *name, UINT32 *index, WINBOOL *exists)
{
	UINT32 position;
	ensure_system();
	*index = UINT32_MAX;
	*exists = FALSE;
	for (position = 0; position < family_count; ++position)
		if (wequal_fold(families[position].name, name)) {
			*index = position;
			*exists = TRUE;
			break;
		}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE collection_from_face(IDWriteFontCollection *self, IDWriteFontFace *face, IDWriteFont **font)
{
	Face *source = (Face *)face;
	UINT32 position, entry;
	ensure_system();
	*font = NULL;
	for (position = 0; position < family_count && source->data->path; ++position)
		for (entry = 0; entry < families[position].count; ++entry) {
			FontEntry *candidate = families[position].entries[entry];
			if (candidate->index == source->data->index && wequal_fold(candidate->path, source->data->path)) {
				*font = make_font(candidate, &families[position]);
				return *font ? S_OK : E_OUTOFMEMORY;
			}
		}
	return DWRITE_E_NOFONT;
}

static HRESULT STDMETHODCALLTYPE collection_set(IDWriteFontCollection *self, IDWriteFontSet **set)
{
	if (!set)
		return E_POINTER;
	*set = get_system_set();
	return *set ? S_OK : E_OUTOFMEMORY;
}

static void *Collection_table_[IDWriteFontCollection1_methods];

static IDWriteFontCollection *get_collection(void)
{
	if (!system_collection.vtbl) {
		memcpy(Collection_table_, IDWriteFontCollection1_table, sizeof(Collection_table_));
		Collection_table_[IDWriteFontCollection_QueryInterface] = (void *)collection_query;
		Collection_table_[IDWriteFontCollection_AddRef] = (void *)collection_add_ref;
		Collection_table_[IDWriteFontCollection_Release] = (void *)collection_release;
		Collection_table_[IDWriteFontCollection_GetFontFamilyCount] = (void *)collection_count;
		Collection_table_[IDWriteFontCollection_GetFontFamily] = (void *)collection_family;
		Collection_table_[IDWriteFontCollection_FindFamilyName] = (void *)collection_find;
		Collection_table_[IDWriteFontCollection_GetFontFromFontFace] = (void *)collection_from_face;
		Collection_table_[IDWriteFontCollection1_GetFontSet] = (void *)collection_set;
		Collection_table_[IDWriteFontCollection1_IDWriteFontCollection1_GetFontFamily] = (void *)collection_family;
		system_collection.vtbl = (const IDWriteFontCollectionVtbl *)Collection_table_;
	}
	return (IDWriteFontCollection *)&system_collection;
}

/* ---- font sets ---- */
/* A set is a list of system fonts, each with the family it is listed under: a family that stands in for a missing one lists the
   fonts of the family it stands in for under its own name. The system set holds every font of the system collection, in family order. */
typedef struct {
	FontEntry *entry;
	FamilyEntry *family;
} SetFont;

typedef struct {
	const IDWriteFontSetVtbl *vtbl;
	LONG refs;
	SetFont *fonts;
	UINT32 count;
} FontSet;

typedef struct {
	const IDWriteFontFaceReferenceVtbl *vtbl;
	LONG refs;
	FontEntry *entry;
} Reference;

typedef struct {
	const IDWriteStringListVtbl *vtbl;
	LONG refs;
	WCHAR **values;
	UINT32 count;
} StringList;

static FamilyEntry *family_of_entry(const FontEntry *entry)
{
	UINT32 position, index;
	for (position = 0; position < family_count; ++position)
		for (index = 0; index < families[position].count; ++index)
			if (families[position].entries[index] == entry)
				return &families[position];
	return NULL;
}

/* ---- string lists ---- */
static HRESULT STDMETHODCALLTYPE list_query(IDWriteStringList *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteStringList)) {
		InterlockedIncrement(&((StringList *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE list_add_ref(IDWriteStringList *self) { return (ULONG)InterlockedIncrement(&((StringList *)self)->refs); }
static ULONG STDMETHODCALLTYPE list_release(IDWriteStringList *self)
{
	StringList *list = (StringList *)self;
	LONG left = InterlockedDecrement(&list->refs);
	UINT32 index;
	if (!left) {
		for (index = 0; index < list->count; ++index)
			FREE(list->values[index]);
		if (list->values)
			FREE(list->values);
		FREE(list);
	}
	return (ULONG)left;
}
static UINT32 STDMETHODCALLTYPE list_count(IDWriteStringList *self) { return ((StringList *)self)->count; }
static HRESULT STDMETHODCALLTYPE list_locale_length(IDWriteStringList *self, UINT32 index, UINT32 *length)
{
	if (index >= ((StringList *)self)->count)
		return E_INVALIDARG;
	*length = 5;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE list_locale(IDWriteStringList *self, UINT32 index, WCHAR *name, UINT32 size)
{
	if (index >= ((StringList *)self)->count)
		return E_INVALIDARG;
	if (size < 6)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(name, L"en-us", 6 * sizeof(WCHAR));
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE list_length(IDWriteStringList *self, UINT32 index, UINT32 *length)
{
	if (index >= ((StringList *)self)->count)
		return E_INVALIDARG;
	*length = (UINT32)wlen(((StringList *)self)->values[index]);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE list_string(IDWriteStringList *self, UINT32 index, WCHAR *buffer, UINT32 size)
{
	StringList *list = (StringList *)self;
	size_t length;
	if (index >= list->count)
		return E_INVALIDARG;
	length = wlen(list->values[index]);
	if (size < length + 1)
		return E_NOT_SUFFICIENT_BUFFER;
	memcpy(buffer, list->values[index], (length + 1) * sizeof(WCHAR));
	return S_OK;
}

static void *List_table_[IDWriteStringList_methods];

static StringList *make_list(void)
{
	StringList *list;
	if (!List_table_[0]) {
		memcpy(List_table_, IDWriteStringList_table, sizeof(List_table_));
		List_table_[IDWriteStringList_QueryInterface] = (void *)list_query;
		List_table_[IDWriteStringList_AddRef] = (void *)list_add_ref;
		List_table_[IDWriteStringList_Release] = (void *)list_release;
		List_table_[IDWriteStringList_GetCount] = (void *)list_count;
		List_table_[IDWriteStringList_GetLocaleNameLength] = (void *)list_locale_length;
		List_table_[IDWriteStringList_GetLocaleName] = (void *)list_locale;
		List_table_[IDWriteStringList_GetStringLength] = (void *)list_length;
		List_table_[IDWriteStringList_GetString] = (void *)list_string;
	}
	list = ALLOC(sizeof(StringList));
	if (!list)
		return NULL;
	list->vtbl = (const IDWriteStringListVtbl *)List_table_;
	list->refs = 1;
	return list;
}

/* ---- font face references ---- */
static HRESULT STDMETHODCALLTYPE reference_query(IDWriteFontFaceReference *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFaceReference)) {
		InterlockedIncrement(&((Reference *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE reference_add_ref(IDWriteFontFaceReference *self) { return (ULONG)InterlockedIncrement(&((Reference *)self)->refs); }
static ULONG STDMETHODCALLTYPE reference_release(IDWriteFontFaceReference *self)
{
	LONG left = InterlockedDecrement(&((Reference *)self)->refs);
	if (!left)
		FREE(self);
	return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE reference_face(IDWriteFontFaceReference *self, IDWriteFontFace3 **face)
{
	FontEntry *entry = ((Reference *)self)->entry;
	IDWriteFont *font = make_font(entry, family_of_entry(entry));
	HRESULT result;
	if (!face)
		return E_POINTER;
	*face = NULL;
	if (!font)
		return E_OUTOFMEMORY;
	result = IDWriteFont_CreateFontFace(font, (IDWriteFontFace **)face);
	IDWriteFont_Release(font);
	return result;
}
static HRESULT STDMETHODCALLTYPE reference_face_simulated(IDWriteFontFaceReference *self, DWRITE_FONT_SIMULATIONS simulations, IDWriteFontFace3 **face)
{
	return reference_face(self, face);
}
static WINBOOL STDMETHODCALLTYPE reference_equals(IDWriteFontFaceReference *self, IDWriteFontFaceReference *other)
{
	return other && ((Reference *)other)->vtbl == ((Reference *)self)->vtbl && ((Reference *)other)->entry == ((Reference *)self)->entry;
}
static UINT32 STDMETHODCALLTYPE reference_index(IDWriteFontFaceReference *self) { return ((Reference *)self)->entry->index; }
static DWRITE_FONT_SIMULATIONS STDMETHODCALLTYPE reference_simulations(IDWriteFontFaceReference *self) { return DWRITE_FONT_SIMULATIONS_NONE; }
static HRESULT STDMETHODCALLTYPE reference_file(IDWriteFontFaceReference *self, IDWriteFontFile **file)
{
	FontEntry *entry = ((Reference *)self)->entry;
	if (!file)
		return E_POINTER;
	*file = NULL;
	if (entry->file) {
		IDWriteFontFile_AddRef(entry->file);
		*file = entry->file;
		return S_OK;
	}
	if (!entry->path)
		return E_FAIL;
	*file = make_file(entry->path, (UINT32)((wlen(entry->path) + 1) * sizeof(WCHAR)), get_system_loader());
	return *file ? S_OK : E_OUTOFMEMORY;
}
static UINT64 STDMETHODCALLTYPE reference_file_size(IDWriteFontFaceReference *self)
{
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	FontEntry *entry = ((Reference *)self)->entry;
	if (!entry->path || !GetFileAttributesExW(entry->path, GetFileExInfoStandard, &attributes))
		return 0;
	return ((UINT64)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
}
static HRESULT STDMETHODCALLTYPE reference_file_time(IDWriteFontFaceReference *self, FILETIME *time)
{
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	FontEntry *entry = ((Reference *)self)->entry;
	if (!time)
		return E_POINTER;
	memset(time, 0, sizeof(*time));
	if (entry->path && GetFileAttributesExW(entry->path, GetFileExInfoStandard, &attributes))
		*time = attributes.ftLastWriteTime;
	return S_OK;
}
static DWRITE_LOCALITY STDMETHODCALLTYPE reference_locality(IDWriteFontFaceReference *self) { return DWRITE_LOCALITY_LOCAL; }
/* Every font is already local, so a download request has nothing to fetch. */
static HRESULT STDMETHODCALLTYPE reference_download(IDWriteFontFaceReference *self) { return S_OK; }
static HRESULT STDMETHODCALLTYPE reference_download_characters(IDWriteFontFaceReference *self, const WCHAR *characters, UINT32 count) { return S_OK; }
static HRESULT STDMETHODCALLTYPE reference_download_glyphs(IDWriteFontFaceReference *self, const UINT16 *glyphs, UINT32 count) { return S_OK; }
static HRESULT STDMETHODCALLTYPE reference_download_fragment(IDWriteFontFaceReference *self, UINT64 offset, UINT64 size) { return S_OK; }

static void *Reference_table_[IDWriteFontFaceReference_methods];

static IDWriteFontFaceReference *make_reference(FontEntry *entry)
{
	Reference *reference;
	if (!Reference_table_[0]) {
		memcpy(Reference_table_, IDWriteFontFaceReference_table, sizeof(Reference_table_));
		Reference_table_[IDWriteFontFaceReference_QueryInterface] = (void *)reference_query;
		Reference_table_[IDWriteFontFaceReference_AddRef] = (void *)reference_add_ref;
		Reference_table_[IDWriteFontFaceReference_Release] = (void *)reference_release;
		Reference_table_[IDWriteFontFaceReference_CreateFontFace] = (void *)reference_face;
		Reference_table_[IDWriteFontFaceReference_CreateFontFaceWithSimulations] = (void *)reference_face_simulated;
		Reference_table_[IDWriteFontFaceReference_Equals] = (void *)reference_equals;
		Reference_table_[IDWriteFontFaceReference_GetFontFaceIndex] = (void *)reference_index;
		Reference_table_[IDWriteFontFaceReference_GetSimulations] = (void *)reference_simulations;
		Reference_table_[IDWriteFontFaceReference_GetFontFile] = (void *)reference_file;
		Reference_table_[IDWriteFontFaceReference_GetLocalFileSize] = (void *)reference_file_size;
		Reference_table_[IDWriteFontFaceReference_GetFileSize] = (void *)reference_file_size;
		Reference_table_[IDWriteFontFaceReference_GetFileTime] = (void *)reference_file_time;
		Reference_table_[IDWriteFontFaceReference_GetLocality] = (void *)reference_locality;
		Reference_table_[IDWriteFontFaceReference_EnqueueFontDownloadRequest] = (void *)reference_download;
		Reference_table_[IDWriteFontFaceReference_EnqueueCharacterDownloadRequest] = (void *)reference_download_characters;
		Reference_table_[IDWriteFontFaceReference_EnqueueGlyphDownloadRequest] = (void *)reference_download_glyphs;
		Reference_table_[IDWriteFontFaceReference_EnqueueFileFragmentDownloadRequest] = (void *)reference_download_fragment;
	}
	reference = ALLOC(sizeof(Reference));
	if (!reference)
		return NULL;
	reference->vtbl = (const IDWriteFontFaceReferenceVtbl *)Reference_table_;
	reference->refs = 1;
	reference->entry = entry;
	return (IDWriteFontFaceReference *)reference;
}

/* ---- the sets ---- */
/* The text of one property of a font, written into the buffer when it is a number; NULL when the font has no such property. */
static const WCHAR *property_text(const SetFont *font, DWRITE_FONT_PROPERTY_ID id, WCHAR buffer[16])
{
	const FontEntry *entry = font->entry;
	DWORD number;
	int at = 15;
	switch (id) {
	case DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME:
		return font->family ? font->family->name : entry->family;
	case DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME:
		return entry->typographic && entry->typographic[0] ? entry->typographic : entry->family;
	case DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME:
		return entry->win32 && entry->win32[0] ? entry->win32 : entry->family;
	case DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FACE_NAME:
		return entry->face ? entry->face : L"Regular";
	case DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FACE_NAME:
		return entry->typographic_face && entry->typographic_face[0] ? entry->typographic_face : entry->face ? entry->face : L"Regular";
	case DWRITE_FONT_PROPERTY_ID_FULL_NAME:
		return entry->full;
	case DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME:
		return entry->postscript;
	case DWRITE_FONT_PROPERTY_ID_WEIGHT:
		number = entry->weight;
		break;
	case DWRITE_FONT_PROPERTY_ID_STRETCH:
		number = entry->stretch;
		break;
	case DWRITE_FONT_PROPERTY_ID_STYLE:
		number = entry->style;
		break;
	default:
		return NULL;
	}
	buffer[at] = 0;
	do {
		buffer[--at] = (WCHAR)(L'0' + number % 10);
		number /= 10;
	} while (number && at);
	return buffer + at;
}

static BOOL font_has_property(const SetFont *font, const DWRITE_FONT_PROPERTY *property)
{
	WCHAR buffer[16];
	const WCHAR *text = property_text(font, property->propertyId, buffer);
	return text && property->propertyValue && wequal_fold(text, property->propertyValue);
}

static IDWriteFontSet *make_set(const SetFont *fonts, UINT32 count);

static HRESULT STDMETHODCALLTYPE set_query(IDWriteFontSet *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontSet) || same_guid(iid, &IID_IDWriteFontSet1) || same_guid(iid, &IID_IDWriteFontSet2) ||
		same_guid(iid, &IID_IDWriteFontSet3)) {
		InterlockedIncrement(&((FontSet *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE set_add_ref(IDWriteFontSet *self) { return (ULONG)InterlockedIncrement(&((FontSet *)self)->refs); }
static ULONG STDMETHODCALLTYPE set_release(IDWriteFontSet *self)
{
	FontSet *set = (FontSet *)self;
	LONG left = InterlockedDecrement(&set->refs);
	if (!left) {
		if (set->fonts)
			FREE(set->fonts);
		FREE(set);
	}
	return (ULONG)left;
}
static UINT32 STDMETHODCALLTYPE set_count(IDWriteFontSet *self) { return ((FontSet *)self)->count; }
static HRESULT STDMETHODCALLTYPE set_reference(IDWriteFontSet *self, UINT32 index, IDWriteFontFaceReference **reference)
{
	FontSet *set = (FontSet *)self;
	if (!reference)
		return E_POINTER;
	*reference = NULL;
	if (index >= set->count)
		return E_INVALIDARG;
	*reference = make_reference(set->fonts[index].entry);
	return *reference ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE set_find_reference(IDWriteFontSet *self, IDWriteFontFaceReference *reference, UINT32 *index, WINBOOL *exists)
{
	FontSet *set = (FontSet *)self;
	UINT32 position;
	*index = UINT32_MAX;
	*exists = FALSE;
	if (!reference || ((Reference *)reference)->vtbl != (const IDWriteFontFaceReferenceVtbl *)Reference_table_)
		return S_OK;
	for (position = 0; position < set->count; ++position)
		if (set->fonts[position].entry == ((Reference *)reference)->entry) {
			*index = position;
			*exists = TRUE;
			break;
		}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE set_find_face(IDWriteFontSet *self, IDWriteFontFace *face, UINT32 *index, WINBOOL *exists)
{
	FontSet *set = (FontSet *)self;
	Face *source = (Face *)face;
	UINT32 position;
	*index = UINT32_MAX;
	*exists = FALSE;
	if (!face)
		return E_INVALIDARG;
	for (position = 0; position < set->count; ++position) {
		FontEntry *entry = set->fonts[position].entry;
		if (entry->data ? entry->data == source->data :
			source->data->path && entry->index == source->data->index && wequal_fold(entry->path, source->data->path)) {
			*index = position;
			*exists = TRUE;
			break;
		}
	}
	return S_OK;
}
/* The distinct values of a property over the set, in the order the fonts come. */
static HRESULT STDMETHODCALLTYPE set_values(IDWriteFontSet *self, DWRITE_FONT_PROPERTY_ID id, IDWriteStringList **values)
{
	FontSet *set = (FontSet *)self;
	StringList *list;
	UINT32 position, seen;
	if (!values)
		return E_POINTER;
	*values = NULL;
	list = make_list();
	if (!list)
		return E_OUTOFMEMORY;
	list->values = ALLOC((set->count ? set->count : 1) * sizeof(WCHAR *));
	if (!list->values) {
		list_release((IDWriteStringList *)list);
		return E_OUTOFMEMORY;
	}
	for (position = 0; position < set->count; ++position) {
		WCHAR buffer[16];
		const WCHAR *text = property_text(&set->fonts[position], id, buffer);
		if (!text || !text[0])
			continue;
		for (seen = 0; seen < list->count && !wequal_fold(list->values[seen], text); ++seen)
			;
		if (seen == list->count && (list->values[list->count] = wdup(text)))
			++list->count;
	}
	*values = (IDWriteStringList *)list;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE set_values_locale(IDWriteFontSet *self, DWRITE_FONT_PROPERTY_ID id, const WCHAR *locales, IDWriteStringList **values)
{
	return set_values(self, id, values);
}
static HRESULT STDMETHODCALLTYPE set_font_values(IDWriteFontSet *self, UINT32 index, DWRITE_FONT_PROPERTY_ID id, WINBOOL *exists, IDWriteLocalizedStrings **values)
{
	FontSet *set = (FontSet *)self;
	WCHAR buffer[16];
	const WCHAR *text;
	if (!exists || !values)
		return E_POINTER;
	*exists = FALSE;
	*values = NULL;
	if (index >= set->count)
		return E_INVALIDARG;
	text = property_text(&set->fonts[index], id, buffer);
	if (!text || !text[0])
		return S_OK;
	*values = make_strings(text);
	if (!*values)
		return E_OUTOFMEMORY;
	*exists = TRUE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE set_occurrences(IDWriteFontSet *self, const DWRITE_FONT_PROPERTY *property, UINT32 *count)
{
	FontSet *set = (FontSet *)self;
	UINT32 position;
	if (!property || !count)
		return E_INVALIDARG;
	*count = 0;
	for (position = 0; position < set->count; ++position)
		if (font_has_property(&set->fonts[position], property))
			++*count;
	return S_OK;
}
/* The fonts of a family, nearest to the wanted weight, width and slant first. */
static HRESULT STDMETHODCALLTYPE set_matching_family(IDWriteFontSet *self, const WCHAR *name, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STRETCH stretch,
	DWRITE_FONT_STYLE style, IDWriteFontSet **out)
{
	FontSet *set = (FontSet *)self;
	SetFont *found;
	UINT32 position, count = 0, sorted;
	if (!name || !out)
		return E_INVALIDARG;
	*out = NULL;
	found = ALLOC((set->count ? set->count : 1) * sizeof(SetFont));
	if (!found)
		return E_OUTOFMEMORY;
	for (position = 0; position < set->count; ++position) {
		/* The name may be any of the font's family names or its full name. */
		static const DWRITE_FONT_PROPERTY_ID names[] = {DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME,
			DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME, DWRITE_FONT_PROPERTY_ID_FULL_NAME};
		WCHAR buffer[16];
		size_t which;
		for (which = 0; which < sizeof(names) / sizeof(names[0]); ++which) {
			const WCHAR *family = property_text(&set->fonts[position], names[which], buffer);
			if (family && wequal_fold(family, name)) {
				found[count++] = set->fonts[position];
				break;
			}
		}
	}
	for (sorted = 1; sorted < count; ++sorted) {
		SetFont font = found[sorted];
		unsigned distance = match_distance(font.entry, weight, stretch, style);
		UINT32 at = sorted;
		while (at && match_distance(found[at - 1].entry, weight, stretch, style) > distance) {
			found[at] = found[at - 1];
			--at;
		}
		found[at] = font;
	}
	*out = make_set(found, count);
	FREE(found);
	return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE set_matching(IDWriteFontSet *self, const DWRITE_FONT_PROPERTY *properties, UINT32 property_count, IDWriteFontSet **out)
{
	FontSet *set = (FontSet *)self;
	SetFont *found;
	UINT32 position, property, count = 0;
	if (!out || (!properties && property_count))
		return E_INVALIDARG;
	*out = NULL;
	found = ALLOC((set->count ? set->count : 1) * sizeof(SetFont));
	if (!found)
		return E_OUTOFMEMORY;
	for (position = 0; position < set->count; ++position) {
		for (property = 0; property < property_count && font_has_property(&set->fonts[position], &properties[property]); ++property)
			;
		if (property == property_count)
			found[count++] = set->fonts[position];
	}
	*out = make_set(found, count);
	FREE(found);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* IDWriteFontSet1 to IDWriteFontSet3: the fonts are installed for the machine and stay valid while the program runs. */
static DWRITE_LOCALITY STDMETHODCALLTYPE set1_locality(IDWriteFontSet *self, UINT32 index) { return DWRITE_LOCALITY_LOCAL; }
static HANDLE STDMETHODCALLTYPE set2_expiration(IDWriteFontSet *self)
{
	static HANDLE never;
	if (!never) {
		HANDLE made = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (made && InterlockedCompareExchangePointer(&never, made, NULL))
			CloseHandle(made);
	}
	return never;
}
static DWRITE_FONT_SOURCE_TYPE STDMETHODCALLTYPE set3_source_type(IDWriteFontSet *self, UINT32 index)
{
	return index < ((FontSet *)self)->count ? DWRITE_FONT_SOURCE_TYPE_PER_MACHINE : DWRITE_FONT_SOURCE_TYPE_UNKNOWN;
}
static UINT32 STDMETHODCALLTYPE set3_source_name_length(IDWriteFontSet *self, UINT32 index) { return 0; }
static HRESULT STDMETHODCALLTYPE set3_source_name(IDWriteFontSet *self, UINT32 index, WCHAR *buffer, UINT32 size)
{
	if (!buffer || !size)
		return E_NOT_SUFFICIENT_BUFFER;
	buffer[0] = 0;
	return S_OK;
}

static void *Set_table_[IDWriteFontSet3_methods];

static IDWriteFontSet *make_set(const SetFont *fonts, UINT32 count)
{
	FontSet *set;
	if (!Set_table_[0]) {
		memcpy(Set_table_, IDWriteFontSet3_table, sizeof(Set_table_));
		Set_table_[IDWriteFontSet_QueryInterface] = (void *)set_query;
		Set_table_[IDWriteFontSet_AddRef] = (void *)set_add_ref;
		Set_table_[IDWriteFontSet_Release] = (void *)set_release;
		Set_table_[IDWriteFontSet_GetFontCount] = (void *)set_count;
		Set_table_[IDWriteFontSet_GetFontFaceReference] = (void *)set_reference;
		Set_table_[IDWriteFontSet_FindFontFaceReference] = (void *)set_find_reference;
		Set_table_[IDWriteFontSet_FindFontFace] = (void *)set_find_face;
		Set_table_[IDWriteFontSet_GetPropertyValues__] = (void *)set_values;
		Set_table_[IDWriteFontSet_GetPropertyValues_] = (void *)set_values_locale;
		Set_table_[IDWriteFontSet_GetPropertyValues] = (void *)set_font_values;
		Set_table_[IDWriteFontSet_GetPropertyOccurrenceCount] = (void *)set_occurrences;
		Set_table_[IDWriteFontSet_GetMatchingFonts_] = (void *)set_matching_family;
		Set_table_[IDWriteFontSet_GetMatchingFonts] = (void *)set_matching;
		Set_table_[IDWriteFontSet3_GetFontLocality] = (void *)set1_locality;
		Set_table_[IDWriteFontSet3_GetExpirationEvent] = (void *)set2_expiration;
		Set_table_[IDWriteFontSet3_GetFontSourceType] = (void *)set3_source_type;
		Set_table_[IDWriteFontSet3_GetFontSourceNameLength] = (void *)set3_source_name_length;
		Set_table_[IDWriteFontSet3_GetFontSourceName] = (void *)set3_source_name;
	}
	set = ALLOC(sizeof(FontSet));
	if (!set)
		return NULL;
	set->vtbl = (const IDWriteFontSetVtbl *)Set_table_;
	set->refs = 1;
	set->fonts = ALLOC((count ? count : 1) * sizeof(SetFont));
	if (!set->fonts) {
		FREE(set);
		return NULL;
	}
	if (count)
		memcpy(set->fonts, fonts, count * sizeof(SetFont));
	set->count = count;
	return (IDWriteFontSet *)set;
}

static IDWriteFontSet *system_set;

/* The system set is made once and never released; a caller's Release only takes back its own reference. */
static IDWriteFontSet *get_system_set(void)
{
	if (!system_set) {
		SetFont *fonts;
		UINT32 position, index, count = 0, total = 0;
		IDWriteFontSet *made;
		ensure_system();
		for (position = 0; position < family_count; ++position)
			total += families[position].count;
		fonts = ALLOC((total ? total : 1) * sizeof(SetFont));
		if (!fonts)
			return NULL;
		for (position = 0; position < family_count; ++position)
			for (index = 0; index < families[position].count; ++index) {
				fonts[count].entry = families[position].entries[index];
				fonts[count++].family = &families[position];
			}
		made = make_set(fonts, count);
		FREE(fonts);
		if (made && InterlockedCompareExchangePointer((void **)&system_set, made, NULL))
			set_release(made);
		if (!system_set)
			return NULL;
	}
	set_add_ref(system_set);
	return system_set;
}

/* ---- collections a program builds from its own loader ---- */
/* The families of the files the loader's enumerator lists. A file named by a path is the system font at that path; the collection
   for one loader and key is built once and kept, since a program asks for the same one again and the families point into it. */
typedef struct {
	const IDWriteFontCollectionVtbl *vtbl;
	IDWriteFontCollectionLoader *loader;
	BYTE *key;
	UINT32 key_size;
	FamilyEntry *families;
	UINT32 count;
} CustomCollection;

static CustomCollection **custom_collections;
static UINT32 custom_count;
static CRITICAL_SECTION custom_lock;
static INIT_ONCE custom_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK custom_init(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	InitializeCriticalSection(&custom_lock);
	return TRUE;
}

#define CUSTOM_OF(self) ((CustomCollection *)(self))

static HRESULT STDMETHODCALLTYPE custom_query(IDWriteFontCollection *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontCollection)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE custom_add_ref(IDWriteFontCollection *self) { return 2; }
static ULONG STDMETHODCALLTYPE custom_release(IDWriteFontCollection *self) { return 1; }
static UINT32 STDMETHODCALLTYPE custom_family_count(IDWriteFontCollection *self) { return CUSTOM_OF(self)->count; }
static HRESULT STDMETHODCALLTYPE custom_family(IDWriteFontCollection *self, UINT32 index, IDWriteFontFamily **family)
{
	*family = NULL;
	if (index >= CUSTOM_OF(self)->count)
		return E_INVALIDARG;
	*family = make_family(&CUSTOM_OF(self)->families[index]);
	return *family ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE custom_find(IDWriteFontCollection *self, const WCHAR *name, UINT32 *index, WINBOOL *exists)
{
	UINT32 position;
	*index = UINT32_MAX;
	*exists = FALSE;
	for (position = 0; position < CUSTOM_OF(self)->count; ++position)
		if (wequal_fold(CUSTOM_OF(self)->families[position].name, name)) {
			*index = position;
			*exists = TRUE;
			break;
		}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE custom_from_face(IDWriteFontCollection *self, IDWriteFontFace *face, IDWriteFont **font)
{
	CustomCollection *collection = CUSTOM_OF(self);
	Face *source = (Face *)face;
	UINT32 position, entry;
	*font = NULL;
	for (position = 0; position < collection->count; ++position)
		for (entry = 0; entry < collection->families[position].count; ++entry) {
			FontEntry *candidate = collection->families[position].entries[entry];
			if (candidate->data ? candidate->data == source->data :
				source->data->path && candidate->index == source->data->index && wequal_fold(candidate->path, source->data->path)) {
				*font = make_font(candidate, &collection->families[position]);
				return *font ? S_OK : E_OUTOFMEMORY;
			}
		}
	return DWRITE_E_NOFONT;
}

static void *Custom_table_[IDWriteFontCollection_methods];

static const IDWriteFontCollectionVtbl *custom_vtbl(void)
{
	if (!Custom_table_[0]) {
		memcpy(Custom_table_, IDWriteFontCollection_table, sizeof(Custom_table_));
		Custom_table_[IDWriteFontCollection_QueryInterface] = (void *)custom_query;
		Custom_table_[IDWriteFontCollection_AddRef] = (void *)custom_add_ref;
		Custom_table_[IDWriteFontCollection_Release] = (void *)custom_release;
		Custom_table_[IDWriteFontCollection_GetFontFamilyCount] = (void *)custom_family_count;
		Custom_table_[IDWriteFontCollection_GetFontFamily] = (void *)custom_family;
		Custom_table_[IDWriteFontCollection_FindFamilyName] = (void *)custom_find;
		Custom_table_[IDWriteFontCollection_GetFontFromFontFace] = (void *)custom_from_face;
	}
	return (const IDWriteFontCollectionVtbl *)Custom_table_;
}

/* Files the entry under the family of its name, which is made when the collection does not hold it yet. */
static void custom_add_entry(CustomCollection *collection, FontEntry *entry)
{
	FamilyEntry *family = NULL;
	UINT32 position;
	for (position = 0; position < collection->count; ++position)
		if (wequal_fold(collection->families[position].name, entry->family))
			family = &collection->families[position];
	if (!family) {
		FamilyEntry *grown = collection->families ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, collection->families, (collection->count + 1) * sizeof(FamilyEntry))
			: ALLOC(sizeof(FamilyEntry));
		if (!grown)
			return;
		collection->families = grown;
		family = &collection->families[collection->count++];
		family->name = wdup(entry->family);
		family->entries = NULL;
		family->count = family->capacity = 0;
	}
	family_add(family, entry);
}

/* Puts every system face stored in the file at this path into the collection's families. */
static void custom_add_path(CustomCollection *collection, const WCHAR *path)
{
	UINT32 index;
	ensure_system();
	for (index = 0; index < font_count; ++index) {
		FontEntry *entry = &font_entries[index];
		if (entry->family && entry->family[0] && wequal_fold(entry->path, path))
			custom_add_entry(collection, entry);
	}
}

/* An English name from the face's name table, the typographic one first when the font has it. */
static WCHAR *face_name(FaceData *data, DWORD preferred, DWORD fallback)
{
	char buffer[512];
	DWORD length = RavenFontName(data->host, preferred, buffer, sizeof(buffer));
	if (!length || length >= sizeof(buffer))
		length = RavenFontName(data->host, fallback, buffer, sizeof(buffer));
	return length && length < sizeof(buffer) ? widen(buffer) : NULL;
}

/* Opens every face of a file that the program's own loader serves and files each one under its family. Each face owns a copy of the
   bytes; the entries live as long as the collection, which is never released. */
static void custom_add_file(CustomCollection *collection, IDWriteFontFile *file)
{
	FontFile source = {0};
	DWORD size = 0, index, faces = 1;
	BYTE *bytes;
	const void *key = NULL;
	if (FAILED(IDWriteFontFile_GetReferenceKey(file, &key, &source.key_size)) || FAILED(IDWriteFontFile_GetLoader(file, &source.loader)))
		return;
	source.key = (BYTE *)key;
	bytes = file_bytes(&source, &size);
	IDWriteFontFileLoader_Release(source.loader);
	for (index = 0; bytes && index < faces; ++index) {
		BYTE *copy = ALLOC(size);
		FaceData *data;
		FontEntry *entry;
		if (!copy)
			break;
		memcpy(copy, bytes, size);
		data = face_data_create(copy, size, index, NULL);
		if (!data)
			continue;
		if (!index && data->info[13] > 1)
			faces = (DWORD)data->info[13];
		entry = ALLOC(sizeof(FontEntry));
		if (!entry) {
			face_data_release(data);
			continue;
		}
		entry->family = face_name(data, 21, 1);
		entry->face = face_name(data, 22, 2);
		entry->full = face_name(data, 4, 4);
		entry->postscript = face_name(data, 6, 6);
		entry->win32 = face_name(data, 1, 1);
		entry->typographic = face_name(data, 16, 1);
		entry->typographic_face = face_name(data, 17, 2);
		entry->index = index;
		entry->weight = (DWORD)(data->info[12] ? data->info[12] : 400);
		entry->style = data->info[11] & 2 ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
		entry->stretch = (DWORD)(data->info[19] >= 1 && data->info[19] <= 9 ? data->info[19] : 5);
		entry->flags = data->info[11] & 32 ? 1 : 0;
		entry->data = data;
		entry->file = file;
		IDWriteFontFile_AddRef(file);
		if (entry->family && entry->family[0])
			custom_add_entry(collection, entry);
	}
	if (bytes)
		FREE(bytes);
}

static HRESULT STDMETHODCALLTYPE factory_custom_collection(IDWriteFactory2 *self, IDWriteFontCollectionLoader *loader, const void *key, UINT32 key_size,
	IDWriteFontCollection **out)
{
	IDWriteFontFileEnumerator *enumerator = NULL;
	CustomCollection *collection;
	UINT32 index;
	WINBOOL more = FALSE;
	HRESULT result;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!loader || (!key && key_size))
		return E_INVALIDARG;
	InitOnceExecuteOnce(&custom_once, custom_init, NULL, NULL);
	EnterCriticalSection(&custom_lock);
	for (index = 0; index < custom_count; ++index) {
		collection = custom_collections[index];
		if (collection->loader == loader && collection->key_size == key_size && (!key_size || !memcmp(collection->key, key, key_size))) {
			LeaveCriticalSection(&custom_lock);
			*out = (IDWriteFontCollection *)collection;
			return S_OK;
		}
	}
	LeaveCriticalSection(&custom_lock);
	result = IDWriteFontCollectionLoader_CreateEnumeratorFromKey(loader, (IDWriteFactory *)self, key, key_size, &enumerator);
	if (FAILED(result))
		return result;
	if (!enumerator)
		return E_FAIL;
	collection = ALLOC(sizeof(CustomCollection));
	if (!collection) {
		IDWriteFontFileEnumerator_Release(enumerator);
		return E_OUTOFMEMORY;
	}
	collection->vtbl = custom_vtbl();
	collection->loader = loader;
	IDWriteFontCollectionLoader_AddRef(loader);
	collection->key_size = key_size;
	collection->key = key_size ? ALLOC(key_size) : NULL;
	if (key_size && collection->key)
		memcpy(collection->key, key, key_size);
	while (SUCCEEDED(IDWriteFontFileEnumerator_MoveNext(enumerator, &more)) && more) {
		IDWriteFontFile *file = NULL;
		if (FAILED(IDWriteFontFileEnumerator_GetCurrentFontFile(enumerator, &file)) || !file)
			continue;
		if (((FontFile *)file)->vtbl == (const IDWriteFontFileVtbl *)FontFile_table_ && ((FontFile *)file)->loader == get_system_loader())
			custom_add_path(collection, (const WCHAR *)((FontFile *)file)->key);
		else
			custom_add_file(collection, file);
		IDWriteFontFile_Release(file);
	}
	IDWriteFontFileEnumerator_Release(enumerator);
	EnterCriticalSection(&custom_lock);
	{
		CustomCollection **grown = custom_collections ? HeapReAlloc(GetProcessHeap(), 0, custom_collections, (custom_count + 1) * sizeof(CustomCollection *))
			: ALLOC(sizeof(CustomCollection *));
		if (grown) {
			custom_collections = grown;
			custom_collections[custom_count++] = collection;
		}
	}
	LeaveCriticalSection(&custom_lock);
	*out = (IDWriteFontCollection *)collection;
	return S_OK;
}

/* ---- rendering parameters ---- */
typedef struct {
	const IDWriteRenderingParams2Vtbl *vtbl;
	LONG refs;
	FLOAT gamma, contrast, grayscale_contrast, cleartype;
	DWRITE_PIXEL_GEOMETRY geometry;
	DWRITE_RENDERING_MODE mode;
	DWRITE_GRID_FIT_MODE grid_fit;
} Params;

#define PARAMS_OF(self) ((Params *)(self))

static HRESULT STDMETHODCALLTYPE params_query(IDWriteRenderingParams *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteRenderingParams) || same_guid(iid, &IID_IDWriteRenderingParams1) ||
		same_guid(iid, &IID_IDWriteRenderingParams2)) {
		InterlockedIncrement(&PARAMS_OF(self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE params_add_ref(IDWriteRenderingParams *self) { return (ULONG)InterlockedIncrement(&PARAMS_OF(self)->refs); }
static ULONG STDMETHODCALLTYPE params_release(IDWriteRenderingParams *self)
{
	LONG left = InterlockedDecrement(&PARAMS_OF(self)->refs);
	if (!left)
		FREE(PARAMS_OF(self));
	return (ULONG)left;
}
static FLOAT STDMETHODCALLTYPE params_gamma(IDWriteRenderingParams *self) { return PARAMS_OF(self)->gamma; }
static FLOAT STDMETHODCALLTYPE params_contrast(IDWriteRenderingParams *self) { return PARAMS_OF(self)->contrast; }
static FLOAT STDMETHODCALLTYPE params_cleartype(IDWriteRenderingParams *self) { return PARAMS_OF(self)->cleartype; }
static DWRITE_PIXEL_GEOMETRY STDMETHODCALLTYPE params_geometry(IDWriteRenderingParams *self) { return PARAMS_OF(self)->geometry; }
static DWRITE_RENDERING_MODE STDMETHODCALLTYPE params_mode(IDWriteRenderingParams *self) { return PARAMS_OF(self)->mode; }
static FLOAT STDMETHODCALLTYPE params_grayscale(IDWriteRenderingParams1 *self) { return PARAMS_OF(self)->grayscale_contrast; }
static DWRITE_GRID_FIT_MODE STDMETHODCALLTYPE params_grid(IDWriteRenderingParams2 *self) { return PARAMS_OF(self)->grid_fit; }

static void *Params_table_[IDWriteRenderingParams2_methods];
static const IDWriteRenderingParams2Vtbl *params_vtbl;

static Params *make_params(FLOAT gamma, FLOAT contrast, FLOAT grayscale, FLOAT cleartype, DWRITE_PIXEL_GEOMETRY geometry, DWRITE_RENDERING_MODE mode, DWRITE_GRID_FIT_MODE grid)
{
	Params *params;
	if (!params_vtbl) {
		memcpy(Params_table_, IDWriteRenderingParams2_table, sizeof(Params_table_));
		Params_table_[IDWriteRenderingParams_QueryInterface] = (void *)params_query;
		Params_table_[IDWriteRenderingParams_AddRef] = (void *)params_add_ref;
		Params_table_[IDWriteRenderingParams_Release] = (void *)params_release;
		Params_table_[IDWriteRenderingParams_GetGamma] = (void *)params_gamma;
		Params_table_[IDWriteRenderingParams_GetEnhancedContrast] = (void *)params_contrast;
		Params_table_[IDWriteRenderingParams_GetClearTypeLevel] = (void *)params_cleartype;
		Params_table_[IDWriteRenderingParams_GetPixelGeometry] = (void *)params_geometry;
		Params_table_[IDWriteRenderingParams_GetRenderingMode] = (void *)params_mode;
		Params_table_[IDWriteRenderingParams1_GetGrayscaleEnhancedContrast] = (void *)params_grayscale;
		Params_table_[IDWriteRenderingParams2_GetGridFitMode] = (void *)params_grid;
		params_vtbl = (const IDWriteRenderingParams2Vtbl *)Params_table_;
	}
	params = ALLOC(sizeof(Params));
	if (!params)
		return NULL;
	params->vtbl = params_vtbl;
	params->refs = 1;
	params->gamma = gamma;
	params->contrast = contrast;
	params->grayscale_contrast = grayscale;
	params->cleartype = cleartype;
	params->geometry = geometry;
	params->mode = mode;
	params->grid_fit = grid;
	return params;
}

/* ---- glyph run analysis: glyph bitmaps from the host, laid out along the run ---- */
typedef struct {
	const IDWriteGlyphRunAnalysisVtbl *vtbl;
	LONG refs;
	FaceData *data;
	float em;
	UINT32 count;
	UINT16 *glyphs;
	float *positions; /* two per glyph: the origin of the glyph on the device, y down */
	INT32 matrix[4];
	BOOL has_matrix;
	DWRITE_RENDERING_MODE mode;
	INT32 bounds[2][4]; /* per texture type: aliased, ClearType */
	BOOL measured[2];
} Analysis;

#define ANALYSIS_OF(self) ((Analysis *)(self))

static HRESULT STDMETHODCALLTYPE analysis_query(IDWriteGlyphRunAnalysis *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteGlyphRunAnalysis)) {
		InterlockedIncrement(&ANALYSIS_OF(self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE analysis_add_ref(IDWriteGlyphRunAnalysis *self) { return (ULONG)InterlockedIncrement(&ANALYSIS_OF(self)->refs); }
static ULONG STDMETHODCALLTYPE analysis_release(IDWriteGlyphRunAnalysis *self)
{
	Analysis *analysis = ANALYSIS_OF(self);
	LONG left = InterlockedDecrement(&analysis->refs);
	if (!left) {
		face_data_release(analysis->data);
		FREE(analysis->glyphs);
		FREE(analysis->positions);
		FREE(analysis);
	}
	return (ULONG)left;
}

/* The render mode the host takes for a texture type and the rendering mode of the run: black and white only when the run asked for it. */
static DWORD host_mode(const Analysis *analysis, DWRITE_TEXTURE_TYPE type)
{
	if (type == DWRITE_TEXTURE_CLEARTYPE_3x1)
		return 2;
	return analysis->mode == DWRITE_RENDERING_MODE_ALIASED ? 0 : 1;
}

static INT32 fixed(float value)
{
	return (INT32)(value * 65536.0f + (value < 0 ? -0.5f : 0.5f));
}

/* Renders one glyph at its place; returns the bytes of its bitmap and fills the bounds relative to the device origin. */
static DWORD render_glyph(Analysis *analysis, UINT32 index, DWRITE_TEXTURE_TYPE type, void *buffer, DWORD capacity, INT32 *bounds)
{
	float x = analysis->positions[index * 2], y = analysis->positions[index * 2 + 1];
	INT32 whole_x = (INT32)(x < 0 ? x - 0.999f : x), whole_y = (INT32)(y < 0 ? y - 0.999f : y);
	DWORD size = cached_render(analysis->data->host, analysis->glyphs[index], fixed(analysis->em), analysis->has_matrix ? analysis->matrix : NULL,
		host_mode(analysis, type), buffer, capacity, bounds, fixed(x - (float)whole_x), fixed(y - (float)whole_y));
	if (size || bounds[2] > bounds[0] || bounds[3] > bounds[1]) {
		bounds[0] += whole_x;
		bounds[2] += whole_x;
		bounds[1] += whole_y;
		bounds[3] += whole_y;
	}
	return size;
}

static HRESULT measure(Analysis *analysis, DWRITE_TEXTURE_TYPE type, INT32 *bounds)
{
	UINT32 index;
	BOOL first = TRUE;
	int slot = type == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 1 : 0;
	if (analysis->measured[slot]) {
		memcpy(bounds, analysis->bounds[slot], sizeof(INT32) * 4);
		return S_OK;
	}
	memset(bounds, 0, sizeof(INT32) * 4);
	for (index = 0; index < analysis->count; ++index) {
		INT32 one[4] = {0, 0, 0, 0};
		render_glyph(analysis, index, type, NULL, 0, one);
		if (one[2] <= one[0] || one[3] <= one[1])
			continue;
		if (first) {
			memcpy(bounds, one, sizeof(one));
			first = FALSE;
		} else {
			if (one[0] < bounds[0]) bounds[0] = one[0];
			if (one[1] < bounds[1]) bounds[1] = one[1];
			if (one[2] > bounds[2]) bounds[2] = one[2];
			if (one[3] > bounds[3]) bounds[3] = one[3];
		}
	}
	memcpy(analysis->bounds[slot], bounds, sizeof(INT32) * 4);
	analysis->measured[slot] = TRUE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_bounds(IDWriteGlyphRunAnalysis *self, DWRITE_TEXTURE_TYPE type, RECT *rect)
{
	INT32 bounds[4];
	if (type != DWRITE_TEXTURE_ALIASED_1x1 && type != DWRITE_TEXTURE_CLEARTYPE_3x1)
		return E_INVALIDARG;
	measure(ANALYSIS_OF(self), type, bounds);
	rect->left = bounds[0];
	rect->top = bounds[1];
	rect->right = bounds[2];
	rect->bottom = bounds[3];
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_texture(IDWriteGlyphRunAnalysis *self, DWRITE_TEXTURE_TYPE type, const RECT *rect, BYTE *alpha, UINT32 size)
{
	Analysis *analysis = ANALYSIS_OF(self);
	UINT32 bytes_per_pixel = type == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 3 : 1, index;
	INT32 width = rect->right - rect->left, height = rect->bottom - rect->top;
	if (type != DWRITE_TEXTURE_ALIASED_1x1 && type != DWRITE_TEXTURE_CLEARTYPE_3x1)
		return E_INVALIDARG;
	if (width < 0 || height < 0 || (UINT64)width * height * bytes_per_pixel > size)
		return E_INVALIDARG;
	memset(alpha, 0, (size_t)width * height * bytes_per_pixel);
	for (index = 0; index < analysis->count; ++index) {
		INT32 bounds[4] = {0, 0, 0, 0};
		DWORD needed = render_glyph(analysis, index, type, NULL, 0, bounds);
		BYTE *bitmap;
		INT32 row, column, glyph_width = bounds[2] - bounds[0], glyph_height = bounds[3] - bounds[1];
		if (!needed || glyph_width <= 0 || glyph_height <= 0)
			continue;
		bitmap = ALLOC(needed);
		if (!bitmap)
			return E_OUTOFMEMORY;
		render_glyph(analysis, index, type, bitmap, needed, bounds);
		for (row = 0; row < glyph_height; ++row) {
			INT32 y = bounds[1] + row - rect->top;
			if (y < 0 || y >= height)
				continue;
			for (column = 0; column < glyph_width; ++column) {
				INT32 x = bounds[0] + column - rect->left, sub;
				if (x < 0 || x >= width)
					continue;
				for (sub = 0; sub < (INT32)bytes_per_pixel; ++sub) {
					BYTE *target = alpha + ((size_t)y * width + x) * bytes_per_pixel + sub, value = bitmap[((size_t)row * glyph_width + column) * bytes_per_pixel + sub];
					if (value > *target)
						*target = value;
				}
			}
		}
		FREE(bitmap);
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE analysis_blend(IDWriteGlyphRunAnalysis *self, IDWriteRenderingParams *params, FLOAT *gamma, FLOAT *contrast, FLOAT *level)
{
	*gamma = params ? IDWriteRenderingParams_GetGamma(params) : 1.0f;
	*contrast = params ? IDWriteRenderingParams_GetEnhancedContrast(params) : 0.5f;
	*level = params ? IDWriteRenderingParams_GetClearTypeLevel(params) : 1.0f;
	return S_OK;
}

static void *Analysis_table_[IDWriteGlyphRunAnalysis_methods];
static const IDWriteGlyphRunAnalysisVtbl *analysis_vtbl;

static HRESULT create_analysis(const DWRITE_GLYPH_RUN *run, const DWRITE_MATRIX *transform, DWRITE_RENDERING_MODE mode, float origin_x, float origin_y, IDWriteGlyphRunAnalysis **result)
{
	Analysis *analysis;
	Face *face;
	UINT32 index;
	float pen = 0;
	*result = NULL;
	if (!run || !run->fontFace || !run->fontEmSize || (run->glyphCount && !run->glyphIndices))
		return E_INVALIDARG;
	face = (Face *)run->fontFace;
	if (!analysis_vtbl) {
		memcpy(Analysis_table_, IDWriteGlyphRunAnalysis_table, sizeof(Analysis_table_));
		Analysis_table_[IDWriteGlyphRunAnalysis_QueryInterface] = (void *)analysis_query;
		Analysis_table_[IDWriteGlyphRunAnalysis_AddRef] = (void *)analysis_add_ref;
		Analysis_table_[IDWriteGlyphRunAnalysis_Release] = (void *)analysis_release;
		Analysis_table_[IDWriteGlyphRunAnalysis_GetAlphaTextureBounds] = (void *)analysis_bounds;
		Analysis_table_[IDWriteGlyphRunAnalysis_CreateAlphaTexture] = (void *)analysis_texture;
		Analysis_table_[IDWriteGlyphRunAnalysis_GetAlphaBlendParams] = (void *)analysis_blend;
		analysis_vtbl = (const IDWriteGlyphRunAnalysisVtbl *)Analysis_table_;
	}
	analysis = ALLOC(sizeof(Analysis));
	if (!analysis)
		return E_OUTOFMEMORY;
	analysis->vtbl = analysis_vtbl;
	analysis->refs = 1;
	InterlockedIncrement(&face->data->refs);
	analysis->data = face->data;
	analysis->em = run->fontEmSize;
	analysis->count = run->glyphCount;
	analysis->mode = mode;
	analysis->glyphs = ALLOC(run->glyphCount * sizeof(UINT16) + 2);
	analysis->positions = ALLOC(run->glyphCount * 2 * sizeof(float) + 8);
	if (!analysis->glyphs || !analysis->positions) {
		analysis->refs = 1;
		analysis_release((IDWriteGlyphRunAnalysis *)analysis);
		return E_OUTOFMEMORY;
	}
	memcpy(analysis->glyphs, run->glyphIndices, run->glyphCount * sizeof(UINT16));
	if (transform && !(transform->m11 == 1 && transform->m12 == 0 && transform->m21 == 0 && transform->m22 == 1)) {
		analysis->has_matrix = TRUE;
		analysis->matrix[0] = fixed(transform->m11);
		analysis->matrix[1] = fixed(transform->m12);
		analysis->matrix[2] = fixed(transform->m21);
		analysis->matrix[3] = fixed(transform->m22);
	}
	for (index = 0; index < run->glyphCount; ++index) {
		float dx = run->glyphOffsets ? run->glyphOffsets[index].advanceOffset : 0, dy = run->glyphOffsets ? -run->glyphOffsets[index].ascenderOffset : 0;
		float x = pen + dx, y = dy;
		/* The transform moves the pen as well as the glyph. */
		if (transform) {
			float tx = x * transform->m11 + y * transform->m21 + transform->dx, ty = x * transform->m12 + y * transform->m22 + transform->dy;
			x = tx;
			y = ty;
		}
		analysis->positions[index * 2] = origin_x + x;
		analysis->positions[index * 2 + 1] = origin_y + y;
		if (run->glyphAdvances)
			pen += run->glyphAdvances[index];
		else {
			INT32 raw[7] = {0};
			if (cached_metrics(face->data->host, run->glyphIndices[index], raw))
				pen += (float)raw[1] * run->fontEmSize / (float)face->data->info[0];
		}
	}
	*result = (IDWriteGlyphRunAnalysis *)analysis;
	return S_OK;
}

/* ---- GDI interop and font fallback ---- */
static HRESULT STDMETHODCALLTYPE interop_create_target(IDWriteGdiInterop *self, HDC dc, UINT32 width, UINT32 height, IDWriteBitmapRenderTarget **result);
static const WCHAR *default_family = L"Arial";

static HRESULT STDMETHODCALLTYPE interop_query(IDWriteGdiInterop *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteGdiInterop) || same_guid(iid, &IID_IDWriteGdiInterop1)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE interop_add_ref(IDWriteGdiInterop *self) { return 2; }
static ULONG STDMETHODCALLTYPE interop_release(IDWriteGdiInterop *self) { return 1; }
static HRESULT STDMETHODCALLTYPE interop_create_font(IDWriteGdiInterop *self, const LOGFONTW *logfont, IDWriteFont **font)
{
	FamilyEntry *family;
	FontEntry *entry;
	ensure_system();
	*font = NULL;
	family = logfont->lfFaceName[0] ? find_family(logfont->lfFaceName) : NULL;
	if (!family)
		family = find_family(default_family);
	if (!family && family_count)
		family = &families[0];
	if (!family)
		return DWRITE_E_NOFONT;
	entry = best_match(family, (DWRITE_FONT_WEIGHT)(logfont->lfWeight ? logfont->lfWeight : 400), DWRITE_FONT_STRETCH_NORMAL,
		logfont->lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL);
	if (!entry)
		return DWRITE_E_NOFONT;
	*font = make_font(entry, family);
	return *font ? S_OK : E_OUTOFMEMORY;
}
static void fill_logfont(const FontEntry *entry, const WCHAR *family, LOGFONTW *logfont)
{
	size_t length = wlen(family);
	memset(logfont, 0, sizeof(*logfont));
	logfont->lfWeight = (LONG)entry->weight;
	logfont->lfItalic = entry->style != 0;
	logfont->lfCharSet = DEFAULT_CHARSET;
	logfont->lfQuality = CLEARTYPE_QUALITY;
	if (length > 31)
		length = 31;
	memcpy(logfont->lfFaceName, family, length * sizeof(WCHAR));
}
static HRESULT STDMETHODCALLTYPE interop_font_to_logfont(IDWriteGdiInterop *self, IDWriteFont *font, LOGFONTW *logfont, WINBOOL *system)
{
	Font *source = FONT_OF(font);
	fill_logfont(source->entry, source->family->name, logfont);
	if (system)
		*system = TRUE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE interop_face_to_logfont(IDWriteGdiInterop *self, IDWriteFontFace *face, LOGFONTW *logfont)
{
	IDWriteFont *font = NULL;
	HRESULT result = collection_from_face(get_collection(), face, &font);
	if (SUCCEEDED(result)) {
		result = interop_font_to_logfont(self, font, logfont, NULL);
		IDWriteFont_Release(font);
	}
	return result;
}

/* IDWriteGdiInterop1: the font a LOGFONT names in a given collection, and the scripts and code pages a font claims. */
static HRESULT STDMETHODCALLTYPE interop_create_font_in(IDWriteGdiInterop *self, const LOGFONTW *logfont, IDWriteFontCollection *collection,
	IDWriteFont **font)
{
	IDWriteFontFamily *family = NULL;
	UINT32 index = 0;
	WINBOOL exists = FALSE;
	HRESULT result;
	if (!logfont || !font)
		return E_INVALIDARG;
	*font = NULL;
	if (!collection)
		return interop_create_font(self, logfont, font);
	result = IDWriteFontCollection_FindFamilyName(collection, logfont->lfFaceName, &index, &exists);
	if (FAILED(result))
		return result;
	if (!exists)
		return DWRITE_E_NOFONT;
	result = IDWriteFontCollection_GetFontFamily(collection, index, &family);
	if (FAILED(result))
		return result;
	result = IDWriteFontFamily_GetFirstMatchingFont(family, (DWRITE_FONT_WEIGHT)(logfont->lfWeight ? logfont->lfWeight : 400),
		DWRITE_FONT_STRETCH_NORMAL, logfont->lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, font);
	IDWriteFontFamily_Release(family);
	return result;
}

static UINT32 big_endian32(const BYTE *bytes) { return ((UINT32)bytes[0] << 24) | ((UINT32)bytes[1] << 16) | ((UINT32)bytes[2] << 8) | bytes[3]; }

/* The two GetFontSignature slots take a font or a font face; either object answers here, whichever order the slots come in. */
static HRESULT STDMETHODCALLTYPE interop_signature(IDWriteGdiInterop *self, IUnknown *object, FONTSIGNATURE *signature)
{
	IDWriteFont *font = NULL;
	IDWriteFontFace *face = NULL;
	const void *table = NULL;
	UINT32 size = 0;
	void *context = NULL;
	WINBOOL exists = FALSE;
	HRESULT result;
	if (!object || !signature)
		return E_INVALIDARG;
	memset(signature, 0, sizeof(*signature));
	if (SUCCEEDED(IUnknown_QueryInterface(object, &IID_IDWriteFont, (void **)&font))) {
		result = IDWriteFont_CreateFontFace(font, &face);
		IDWriteFont_Release(font);
		if (FAILED(result))
			return result;
	} else if (FAILED(IUnknown_QueryInterface(object, &IID_IDWriteFontFace, (void **)&face))) {
		return E_INVALIDARG;
	}
	result = IDWriteFontFace_TryGetFontTable(face, DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'), &table, &size, &context, &exists);
	if (SUCCEEDED(result) && exists && size >= 58) {
		const BYTE *bytes = table;
		UINT32 index;
		for (index = 0; index < 4; ++index)
			signature->fsUsb[index] = big_endian32(bytes + 42 + index * 4);
		if (size >= 86 && ((bytes[0] << 8) | bytes[1]) >= 1) {
			signature->fsCsb[0] = big_endian32(bytes + 78);
			signature->fsCsb[1] = big_endian32(bytes + 82);
		}
	}
	if (exists)
		IDWriteFontFace_ReleaseFontTable(face, context);
	IDWriteFontFace_Release(face);
	/* A font that claims no script still draws Latin text. */
	if (!signature->fsCsb[0] && !signature->fsCsb[1]) {
		signature->fsUsb[0] |= 3;
		signature->fsCsb[0] = 1;
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE interop_matching_fonts(IDWriteGdiInterop *self, const LOGFONTW *logfont, IUnknown *set, IUnknown **subset)
{
	if (subset)
		*subset = NULL;
	return unimplemented("IDWriteGdiInterop1", "GetMatchingFontsByLOGFONT");
}

/* The IDWriteGdiInterop1 slots after those of IDWriteGdiInterop. */
enum { Interop1_CreateFontFromLOGFONT = 8, Interop1_GetFontSignature_face = 9, Interop1_GetFontSignature = 10,
	Interop1_GetMatchingFontsByLOGFONT = 11, Interop1_methods = 12 };

static void *Interop_table_[Interop1_methods];
static IDWriteGdiInterop interop_object;

static IDWriteGdiInterop *get_interop(void)
{
	if (!interop_object.lpVtbl) {
		memcpy(Interop_table_, IDWriteGdiInterop_table, IDWriteGdiInterop_methods * sizeof(void *));
		Interop_table_[IDWriteGdiInterop_QueryInterface] = (void *)interop_query;
		Interop_table_[IDWriteGdiInterop_AddRef] = (void *)interop_add_ref;
		Interop_table_[IDWriteGdiInterop_Release] = (void *)interop_release;
		Interop_table_[IDWriteGdiInterop_CreateFontFromLOGFONT] = (void *)interop_create_font;
		Interop_table_[IDWriteGdiInterop_ConvertFontToLOGFONT] = (void *)interop_font_to_logfont;
		Interop_table_[IDWriteGdiInterop_ConvertFontFaceToLOGFONT] = (void *)interop_face_to_logfont;
		Interop_table_[IDWriteGdiInterop_CreateBitmapRenderTarget] = (void *)interop_create_target;
		Interop_table_[Interop1_CreateFontFromLOGFONT] = (void *)interop_create_font_in;
		Interop_table_[Interop1_GetFontSignature_face] = (void *)interop_signature;
		Interop_table_[Interop1_GetFontSignature] = (void *)interop_signature;
		Interop_table_[Interop1_GetMatchingFontsByLOGFONT] = (void *)interop_matching_fonts;
		interop_object.lpVtbl = (IDWriteGdiInteropVtbl *)Interop_table_;
	}
	return &interop_object;
}

/* Fallback: the base family when it holds the character, otherwise the first of a fixed list of families that does. */
static const WCHAR *fallback_order[] = {L"Noto Sans", L"DejaVu Sans", L"Liberation Sans", L"Noto Sans CJK JP", L"Noto Sans Symbols", L"Noto Sans Symbols2", L"Noto Color Emoji"};

static BOOL entry_has(FontEntry *entry, UINT32 codepoint)
{
	FaceData *data = face_data_for_entry(entry);
	UINT16 glyph = 0;
	if (!data)
		return FALSE;
	cached_glyphs(data->host, &codepoint, 1, &glyph);
	face_data_release(data);
	return glyph != 0;
}

static UINT32 next_codepoint(const WCHAR *text, UINT32 length, UINT32 *units)
{
	WCHAR first = text[0];
	if (first >= 0xd800 && first < 0xdc00 && length > 1 && text[1] >= 0xdc00 && text[1] < 0xe000) {
		*units = 2;
		return 0x10000 + (((UINT32)first - 0xd800) << 10) + ((UINT32)text[1] - 0xdc00);
	}
	*units = 1;
	return first;
}

static HRESULT STDMETHODCALLTYPE fallback_query(IDWriteFontFallback *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFallback)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE fallback_add_ref(IDWriteFontFallback *self) { return 2; }
static ULONG STDMETHODCALLTYPE fallback_release(IDWriteFontFallback *self) { return 1; }
static HRESULT STDMETHODCALLTYPE fallback_map(IDWriteFontFallback *self, IDWriteTextAnalysisSource *source, UINT32 position, UINT32 length, IDWriteFontCollection *collection,
	const WCHAR *base_name, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, UINT32 *mapped_length, IDWriteFont **mapped_font, FLOAT *scale)
{
	const WCHAR *text = NULL;
	UINT32 available = 0, units, codepoint, covered = 0, index;
	FamilyEntry *chosen = NULL;
	FontEntry *entry = NULL;
	FamilyEntry *base = base_name ? find_family(base_name) : NULL;
	ensure_system();
	*mapped_font = NULL;
	*mapped_length = length;
	*scale = 1.0f;
	if (FAILED(IDWriteTextAnalysisSource_GetTextAtPosition(source, position, &text, &available)) || !text || !available)
		return S_OK;
	if (available > length)
		available = length;
	codepoint = next_codepoint(text, available, &units);
	if (base) {
		FontEntry *candidate = best_match(base, weight, stretch, style);
		if (candidate && entry_has(candidate, codepoint)) {
			chosen = base;
			entry = candidate;
		}
	}
	for (index = 0; !entry && index < sizeof(fallback_order) / sizeof(fallback_order[0]); ++index) {
		FamilyEntry *family = find_family(fallback_order[index]);
		FontEntry *candidate = family ? best_match(family, weight, stretch, style) : NULL;
		if (candidate && entry_has(candidate, codepoint)) {
			chosen = family;
			entry = candidate;
		}
	}
	if (!entry) {
		/* Nothing holds the character: the run of such characters has no font. */
		*mapped_length = units;
		return S_OK;
	}
	/* The mapped run goes on while the next characters are in the same font. */
	covered = units;
	while (covered < available) {
		UINT32 step, next = next_codepoint(text + covered, available - covered, &step);
		if (!entry_has(entry, next))
			break;
		covered += step;
	}
	*mapped_length = covered;
	*mapped_font = make_font(entry, chosen);
	return *mapped_font ? S_OK : E_OUTOFMEMORY;
}

static void *Fallback_table_[IDWriteFontFallback_methods];
static IDWriteFontFallback fallback_object;

static IDWriteFontFallback *get_fallback(void)
{
	if (!fallback_object.lpVtbl) {
		memcpy(Fallback_table_, IDWriteFontFallback_table, sizeof(Fallback_table_));
		Fallback_table_[IDWriteFontFallback_QueryInterface] = (void *)fallback_query;
		Fallback_table_[IDWriteFontFallback_AddRef] = (void *)fallback_add_ref;
		Fallback_table_[IDWriteFontFallback_Release] = (void *)fallback_release;
		Fallback_table_[IDWriteFontFallback_MapCharacters] = (void *)fallback_map;
		fallback_object.lpVtbl = (IDWriteFontFallbackVtbl *)Fallback_table_;
	}
	return &fallback_object;
}

/* A fallback a program builds: its own mappings of character ranges to families come first, the system fallback answers the rest. */
typedef struct {
	DWRITE_UNICODE_RANGE *ranges;
	UINT32 range_count;
	WCHAR **names;
	UINT32 name_count;
	IDWriteFontCollection *collection;
	FLOAT scale;
} FallbackMapping;

typedef struct {
	const IDWriteFontFallbackVtbl *vtbl;
	LONG refs;
	FallbackMapping *mappings;
	UINT32 count;
} BuiltFallback;

typedef struct {
	const IDWriteFontFallbackBuilderVtbl *vtbl;
	LONG refs;
	FallbackMapping *mappings;
	UINT32 count;
} FallbackBuilder;

static BOOL mapping_covers(const FallbackMapping *mapping, UINT32 codepoint)
{
	UINT32 index;
	for (index = 0; index < mapping->range_count; ++index)
		if (codepoint >= mapping->ranges[index].first && codepoint <= mapping->ranges[index].last)
			return TRUE;
	return FALSE;
}

/* The font of the named family in the mapping's collection that holds the character, or NULL. */
static IDWriteFont *mapping_font(const FallbackMapping *mapping, const WCHAR *name, UINT32 codepoint, DWRITE_FONT_WEIGHT weight,
	DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch)
{
	IDWriteFontCollection *collection = mapping->collection ? mapping->collection : get_collection();
	IDWriteFontFamily *family = NULL;
	IDWriteFont *font = NULL;
	UINT32 index = 0;
	WINBOOL exists = FALSE;
	if (FAILED(IDWriteFontCollection_FindFamilyName(collection, name, &index, &exists)) || !exists)
		return NULL;
	if (FAILED(IDWriteFontCollection_GetFontFamily(collection, index, &family)))
		return NULL;
	if (SUCCEEDED(IDWriteFontFamily_GetFirstMatchingFont(family, weight, stretch, style, &font))) {
		exists = FALSE;
		if (FAILED(IDWriteFont_HasCharacter(font, codepoint, &exists)) || !exists) {
			IDWriteFont_Release(font);
			font = NULL;
		}
	}
	IDWriteFontFamily_Release(family);
	return font;
}

static void mapping_clear(FallbackMapping *mapping)
{
	UINT32 name;
	if (mapping->ranges)
		FREE(mapping->ranges);
	for (name = 0; name < mapping->name_count; ++name)
		FREE(mapping->names[name]);
	if (mapping->names)
		FREE(mapping->names);
	if (mapping->collection)
		IDWriteFontCollection_Release(mapping->collection);
	memset(mapping, 0, sizeof(*mapping));
}

static void mappings_free(FallbackMapping *mappings, UINT32 count)
{
	UINT32 index;
	for (index = 0; index < count; ++index)
		mapping_clear(&mappings[index]);
	if (mappings)
		FREE(mappings);
}

static HRESULT STDMETHODCALLTYPE built_query(IDWriteFontFallback *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFallback)) {
		InterlockedIncrement(&((BuiltFallback *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE built_add_ref(IDWriteFontFallback *self) { return (ULONG)InterlockedIncrement(&((BuiltFallback *)self)->refs); }
static ULONG STDMETHODCALLTYPE built_release(IDWriteFontFallback *self)
{
	BuiltFallback *fallback = (BuiltFallback *)self;
	LONG left = InterlockedDecrement(&fallback->refs);
	if (!left) {
		mappings_free(fallback->mappings, fallback->count);
		FREE(fallback);
	}
	return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE built_map(IDWriteFontFallback *self, IDWriteTextAnalysisSource *source, UINT32 position, UINT32 length, IDWriteFontCollection *collection,
	const WCHAR *base_name, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch, UINT32 *mapped_length, IDWriteFont **mapped_font, FLOAT *scale)
{
	BuiltFallback *fallback = (BuiltFallback *)self;
	const WCHAR *text = NULL;
	UINT32 available = 0, units, codepoint, index, name;
	if (!source || !mapped_length || !mapped_font || !scale)
		return E_INVALIDARG;
	if (SUCCEEDED(IDWriteTextAnalysisSource_GetTextAtPosition(source, position, &text, &available)) && text && available) {
		if (available > length)
			available = length;
		codepoint = next_codepoint(text, available, &units);
		for (index = 0; index < fallback->count; ++index) {
			const FallbackMapping *mapping = &fallback->mappings[index];
			if (!mapping_covers(mapping, codepoint))
				continue;
			for (name = 0; name < mapping->name_count; ++name) {
				IDWriteFont *font = mapping_font(mapping, mapping->names[name], codepoint, weight, style, stretch);
				UINT32 covered = units;
				if (!font)
					continue;
				/* The run goes on while the next characters stay in the mapping and in the font. */
				while (covered < available) {
					UINT32 step, next = next_codepoint(text + covered, available - covered, &step);
					WINBOOL holds = FALSE;
					if (!mapping_covers(mapping, next) || FAILED(IDWriteFont_HasCharacter(font, next, &holds)) || !holds)
						break;
					covered += step;
				}
				*mapped_length = covered;
				*mapped_font = font;
				*scale = mapping->scale;
				return S_OK;
			}
		}
	}
	return fallback_map(get_fallback(), source, position, length, collection, base_name, weight, style, stretch, mapped_length, mapped_font, scale);
}

static void *Built_table_[IDWriteFontFallback_methods];

static HRESULT STDMETHODCALLTYPE builder_query(IDWriteFontFallbackBuilder *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFontFallbackBuilder)) {
		InterlockedIncrement(&((FallbackBuilder *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE builder_add_ref(IDWriteFontFallbackBuilder *self) { return (ULONG)InterlockedIncrement(&((FallbackBuilder *)self)->refs); }
static ULONG STDMETHODCALLTYPE builder_release(IDWriteFontFallbackBuilder *self)
{
	FallbackBuilder *builder = (FallbackBuilder *)self;
	LONG left = InterlockedDecrement(&builder->refs);
	if (!left) {
		mappings_free(builder->mappings, builder->count);
		FREE(builder);
	}
	return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE builder_add_mapping(IDWriteFontFallbackBuilder *self, const DWRITE_UNICODE_RANGE *ranges, UINT32 range_count,
	const WCHAR **names, UINT32 name_count, IDWriteFontCollection *collection, const WCHAR *locale, const WCHAR *base_name, FLOAT scale)
{
	FallbackBuilder *builder = (FallbackBuilder *)self;
	FallbackMapping *grown, *mapping;
	UINT32 index;
	if (!ranges || !range_count || !names || !name_count || scale <= 0.0f)
		return E_INVALIDARG;
	grown = builder->mappings ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, builder->mappings, (builder->count + 1) * sizeof(FallbackMapping))
		: ALLOC(sizeof(FallbackMapping));
	if (!grown)
		return E_OUTOFMEMORY;
	builder->mappings = grown;
	mapping = &grown[builder->count];
	mapping->ranges = ALLOC(range_count * sizeof(DWRITE_UNICODE_RANGE));
	mapping->names = ALLOC(name_count * sizeof(WCHAR *));
	if (!mapping->ranges || !mapping->names) {
		mapping_clear(mapping);
		return E_OUTOFMEMORY;
	}
	memcpy(mapping->ranges, ranges, range_count * sizeof(DWRITE_UNICODE_RANGE));
	mapping->range_count = range_count;
	for (index = 0; index < name_count; ++index)
		if (names[index] && (mapping->names[mapping->name_count] = wdup(names[index])))
			++mapping->name_count;
	mapping->collection = collection;
	if (collection)
		IDWriteFontCollection_AddRef(collection);
	mapping->scale = scale;
	++builder->count;
	return S_OK;
}
/* The system fallback is what a built fallback falls back to anyway; another built one hands over its mappings. */
static HRESULT STDMETHODCALLTYPE builder_add_mappings(IDWriteFontFallbackBuilder *self, IDWriteFontFallback *fallback)
{
	BuiltFallback *source = (BuiltFallback *)fallback;
	UINT32 index;
	HRESULT result = S_OK;
	if (!fallback)
		return E_INVALIDARG;
	if (fallback == get_fallback() || source->vtbl != (const IDWriteFontFallbackVtbl *)Built_table_)
		return S_OK;
	for (index = 0; index < source->count && SUCCEEDED(result); ++index) {
		const FallbackMapping *mapping = &source->mappings[index];
		result = builder_add_mapping(self, mapping->ranges, mapping->range_count, (const WCHAR **)mapping->names, mapping->name_count,
			mapping->collection, NULL, NULL, mapping->scale);
	}
	return result;
}
static HRESULT STDMETHODCALLTYPE builder_create(IDWriteFontFallbackBuilder *self, IDWriteFontFallback **out)
{
	FallbackBuilder *builder = (FallbackBuilder *)self;
	BuiltFallback *fallback;
	IDWriteFontFallbackBuilder *copy = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!Built_table_[0]) {
		memcpy(Built_table_, IDWriteFontFallback_table, sizeof(Built_table_));
		Built_table_[IDWriteFontFallback_QueryInterface] = (void *)built_query;
		Built_table_[IDWriteFontFallback_AddRef] = (void *)built_add_ref;
		Built_table_[IDWriteFontFallback_Release] = (void *)built_release;
		Built_table_[IDWriteFontFallback_MapCharacters] = (void *)built_map;
	}
	fallback = ALLOC(sizeof(BuiltFallback));
	if (!fallback)
		return E_OUTOFMEMORY;
	fallback->vtbl = (const IDWriteFontFallbackVtbl *)Built_table_;
	fallback->refs = 1;
	/* The fallback keeps its own copy, so the builder can go on adding mappings for another one. */
	if (builder->count) {
		FallbackBuilder scratch = {builder->vtbl, 1, NULL, 0};
		UINT32 index;
		copy = (IDWriteFontFallbackBuilder *)&scratch;
		for (index = 0; index < builder->count; ++index) {
			const FallbackMapping *mapping = &builder->mappings[index];
			if (FAILED(builder_add_mapping(copy, mapping->ranges, mapping->range_count, (const WCHAR **)mapping->names, mapping->name_count,
				mapping->collection, NULL, NULL, mapping->scale))) {
				mappings_free(scratch.mappings, scratch.count);
				FREE(fallback);
				return E_OUTOFMEMORY;
			}
		}
		fallback->mappings = scratch.mappings;
		fallback->count = scratch.count;
	}
	*out = (IDWriteFontFallback *)fallback;
	return S_OK;
}

static void *Builder_table_[6];

static HRESULT STDMETHODCALLTYPE factory2_fallback_builder(IDWriteFactory2 *self, IDWriteFontFallbackBuilder **out)
{
	FallbackBuilder *builder;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!Builder_table_[0]) {
		Builder_table_[0] = (void *)builder_query;
		Builder_table_[1] = (void *)builder_add_ref;
		Builder_table_[2] = (void *)builder_release;
		Builder_table_[3] = (void *)builder_add_mapping;
		Builder_table_[4] = (void *)builder_add_mappings;
		Builder_table_[5] = (void *)builder_create;
	}
	builder = ALLOC(sizeof(FallbackBuilder));
	if (!builder)
		return E_OUTOFMEMORY;
	builder->vtbl = (const IDWriteFontFallbackBuilderVtbl *)Builder_table_;
	builder->refs = 1;
	*out = (IDWriteFontFallbackBuilder *)builder;
	return S_OK;
}

#include "text.inc"
#include "analyzer.inc"

/* ---- the factory ---- */
typedef struct {
	const IDWriteFactory2Vtbl *vtbl;
	LONG refs;
} Factory;

static Factory factory_object;

static HRESULT STDMETHODCALLTYPE factory_query(IDWriteFactory2 *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteFactory) || same_guid(iid, &IID_IDWriteFactory1) || same_guid(iid, &IID_IDWriteFactory2) ||
		same_guid(iid, &IID_IDWriteFactory3)) {
		InterlockedIncrement(&factory_object.refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE factory_add_ref(IDWriteFactory2 *self) { return (ULONG)InterlockedIncrement(&factory_object.refs); }
static ULONG STDMETHODCALLTYPE factory_release(IDWriteFactory2 *self) { return (ULONG)InterlockedDecrement(&factory_object.refs); }
static HRESULT STDMETHODCALLTYPE factory_system_collection(IDWriteFactory2 *self, IDWriteFontCollection **collection, WINBOOL update)
{
	*collection = get_collection();
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory3_system_set(IDWriteFactory2 *self, IDWriteFontSet **set)
{
	if (!set)
		return E_POINTER;
	*set = get_system_set();
	return *set ? S_OK : E_OUTOFMEMORY;
}
/* Every font is local, so the collection with downloadable fonts is the system collection too. */
static HRESULT STDMETHODCALLTYPE factory3_system_collection(IDWriteFactory2 *self, WINBOOL downloadable, IDWriteFontCollection1 **collection, WINBOOL update)
{
	if (!collection)
		return E_POINTER;
	*collection = (IDWriteFontCollection1 *)get_collection();
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory_register_loader(IDWriteFactory2 *self, IDWriteFontFileLoader *loader) { return S_OK; }
static HRESULT STDMETHODCALLTYPE factory_file_reference(IDWriteFactory2 *self, const WCHAR *path, const FILETIME *time, IDWriteFontFile **file)
{
	size_t length;
	*file = NULL;
	if (!path)
		return E_INVALIDARG;
	length = wlen(path);
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
		return DWRITE_E_FILENOTFOUND;
	*file = make_file(path, (UINT32)((length + 1) * sizeof(WCHAR)), get_system_loader());
	return *file ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE factory_custom_file_reference(IDWriteFactory2 *self, const void *key, UINT32 key_size, IDWriteFontFileLoader *loader, IDWriteFontFile **file)
{
	*file = NULL;
	if (!loader || (!key && key_size))
		return E_INVALIDARG;
	*file = make_file(key, key_size, loader);
	return *file ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE factory_create_face(IDWriteFactory2 *self, DWRITE_FONT_FACE_TYPE type, UINT32 count, IDWriteFontFile *const *files, UINT32 index,
	DWRITE_FONT_SIMULATIONS simulations, IDWriteFontFace **face)
{
	DWORD size = 0;
	BYTE *bytes;
	FaceData *data;
	*face = NULL;
	if (count != 1 || !files || !files[0])
		return E_INVALIDARG;
	bytes = file_bytes((FontFile *)files[0], &size);
	if (!bytes)
		return DWRITE_E_FILENOTFOUND;
	data = face_data_create(bytes, size, index, NULL);
	if (!data)
		return DWRITE_E_FILEFORMAT;
	*face = make_face(data, files[0], simulations);
	if (!*face) {
		face_data_release(data);
		return E_OUTOFMEMORY;
	}
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory_rendering_params(IDWriteFactory2 *self, IDWriteRenderingParams **params)
{
	*params = (IDWriteRenderingParams *)make_params(1.8f, 0.5f, 1.0f, 1.0f, DWRITE_PIXEL_GEOMETRY_RGB, DWRITE_RENDERING_MODE_DEFAULT, DWRITE_GRID_FIT_MODE_DEFAULT);
	return *params ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE factory_monitor_params(IDWriteFactory2 *self, HMONITOR monitor, IDWriteRenderingParams **params)
{
	return factory_rendering_params(self, params);
}
static HRESULT STDMETHODCALLTYPE factory_custom_params(IDWriteFactory2 *self, FLOAT gamma, FLOAT contrast, FLOAT cleartype, DWRITE_PIXEL_GEOMETRY geometry,
	DWRITE_RENDERING_MODE mode, IDWriteRenderingParams **params)
{
	*params = (IDWriteRenderingParams *)make_params(gamma, contrast, contrast, cleartype, geometry, mode, DWRITE_GRID_FIT_MODE_DEFAULT);
	return *params ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE factory_gdi_interop(IDWriteFactory2 *self, IDWriteGdiInterop **interop)
{
	*interop = get_interop();
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory_glyph_analysis(IDWriteFactory2 *self, const DWRITE_GLYPH_RUN *run, FLOAT ppd, const DWRITE_MATRIX *transform,
	DWRITE_RENDERING_MODE mode, DWRITE_MEASURING_MODE measuring, FLOAT origin_x, FLOAT origin_y, IDWriteGlyphRunAnalysis **analysis)
{
	return create_analysis(run, transform, mode, origin_x, origin_y, analysis);
}
/* No end-user defined characters are installed; the collection of them is there and holds no family. */
static HRESULT STDMETHODCALLTYPE factory1_eudc(IDWriteFactory2 *self, IDWriteFontCollection **collection, WINBOOL update)
{
	static CustomCollection empty;
	if (!collection)
		return E_POINTER;
	if (!empty.vtbl)
		empty.vtbl = custom_vtbl();
	*collection = (IDWriteFontCollection *)&empty;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory1_custom_params(IDWriteFactory2 *self, FLOAT gamma, FLOAT contrast, FLOAT grayscale, FLOAT cleartype, DWRITE_PIXEL_GEOMETRY geometry,
	DWRITE_RENDERING_MODE mode, IDWriteRenderingParams1 **params)
{
	*params = (IDWriteRenderingParams1 *)make_params(gamma, contrast, grayscale, cleartype, geometry, mode, DWRITE_GRID_FIT_MODE_DEFAULT);
	return *params ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE factory2_fallback(IDWriteFactory2 *self, IDWriteFontFallback **fallback)
{
	*fallback = get_fallback();
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE factory2_color_run(IDWriteFactory2 *self, FLOAT x, FLOAT y, const DWRITE_GLYPH_RUN *run, const DWRITE_GLYPH_RUN_DESCRIPTION *description,
	DWRITE_MEASURING_MODE measuring, const DWRITE_MATRIX *transform, UINT32 palette, IDWriteColorGlyphRunEnumerator **layers)
{
	*layers = NULL;
	return DWRITE_E_NOCOLOR;
}
static HRESULT STDMETHODCALLTYPE factory2_custom_params(IDWriteFactory2 *self, FLOAT gamma, FLOAT contrast, FLOAT grayscale, FLOAT cleartype, DWRITE_PIXEL_GEOMETRY geometry,
	DWRITE_RENDERING_MODE mode, DWRITE_GRID_FIT_MODE grid, IDWriteRenderingParams2 **params)
{
	*params = (IDWriteRenderingParams2 *)make_params(gamma, contrast, grayscale, cleartype, geometry, mode, grid);
	return *params ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE factory2_glyph_analysis(IDWriteFactory2 *self, const DWRITE_GLYPH_RUN *run, const DWRITE_MATRIX *transform, DWRITE_RENDERING_MODE mode,
	DWRITE_MEASURING_MODE measuring, DWRITE_GRID_FIT_MODE grid, DWRITE_TEXT_ANTIALIAS_MODE antialias, FLOAT origin_x, FLOAT origin_y, IDWriteGlyphRunAnalysis **analysis)
{
	return create_analysis(run, transform, mode, origin_x, origin_y, analysis);
}

/* ---- number substitution: digits are always shaped as written, so the object only carries its reference count ---- */
typedef struct {
	const IDWriteNumberSubstitutionVtbl *vtbl;
	LONG refs;
} Substitution;

static HRESULT STDMETHODCALLTYPE substitution_query(IDWriteNumberSubstitution *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IDWriteNumberSubstitution)) {
		InterlockedIncrement(&((Substitution *)self)->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return no_interface(iid);
}
static ULONG STDMETHODCALLTYPE substitution_add_ref(IDWriteNumberSubstitution *self) { return (ULONG)InterlockedIncrement(&((Substitution *)self)->refs); }
static ULONG STDMETHODCALLTYPE substitution_release(IDWriteNumberSubstitution *self)
{
	LONG left = InterlockedDecrement(&((Substitution *)self)->refs);
	if (!left)
		FREE(self);
	return (ULONG)left;
}

static const IDWriteNumberSubstitutionVtbl substitution_vtbl = { substitution_query, substitution_add_ref, substitution_release };

static HRESULT STDMETHODCALLTYPE factory_number_substitution(IDWriteFactory2 *self, DWRITE_NUMBER_SUBSTITUTION_METHOD method, const WCHAR *locale,
	WINBOOL ignore_overrides, IDWriteNumberSubstitution **substitution)
{
	Substitution *object;
	if (!substitution)
		return E_POINTER;
	*substitution = NULL;
	if ((UINT32)method > DWRITE_NUMBER_SUBSTITUTION_METHOD_TRADITIONAL || !locale)
		return E_INVALIDARG;
	object = ALLOC(sizeof(Substitution));
	if (!object)
		return E_OUTOFMEMORY;
	object->vtbl = &substitution_vtbl;
	object->refs = 1;
	*substitution = (IDWriteNumberSubstitution *)object;
	return S_OK;
}

/* The table is the IDWriteFactory3 one: its first methods are those of the earlier factory interfaces, the rest report themselves unimplemented. */
static void *Factory_table_[IDWriteFactory3_methods];

static HRESULT create_factory(REFIID iid, IUnknown **out)
{
	if (!factory_object.vtbl) {
		memcpy(Factory_table_, IDWriteFactory3_table, sizeof(Factory_table_));
		Factory_table_[IDWriteFactory_QueryInterface] = (void *)factory_query;
		Factory_table_[IDWriteFactory_AddRef] = (void *)factory_add_ref;
		Factory_table_[IDWriteFactory_Release] = (void *)factory_release;
		Factory_table_[IDWriteFactory_GetSystemFontCollection] = (void *)factory_system_collection;
		Factory_table_[IDWriteFactory_CreateCustomFontCollection] = (void *)factory_custom_collection;
		Factory_table_[IDWriteFactory_RegisterFontCollectionLoader] = (void *)factory_register_loader;
		Factory_table_[IDWriteFactory_CreateTextAnalyzer] = (void *)factory_text_analyzer;
		Factory_table_[IDWriteFactory_UnregisterFontCollectionLoader] = (void *)factory_register_loader;
		Factory_table_[IDWriteFactory_CreateFontFileReference] = (void *)factory_file_reference;
		Factory_table_[IDWriteFactory_CreateCustomFontFileReference] = (void *)factory_custom_file_reference;
		Factory_table_[IDWriteFactory_CreateFontFace] = (void *)factory_create_face;
		Factory_table_[IDWriteFactory_CreateRenderingParams] = (void *)factory_rendering_params;
		Factory_table_[IDWriteFactory_CreateMonitorRenderingParams] = (void *)factory_monitor_params;
		Factory_table_[IDWriteFactory_CreateCustomRenderingParams] = (void *)factory_custom_params;
		Factory_table_[IDWriteFactory_RegisterFontFileLoader] = (void *)factory_register_loader;
		Factory_table_[IDWriteFactory_UnregisterFontFileLoader] = (void *)factory_register_loader;
		Factory_table_[IDWriteFactory_GetGdiInterop] = (void *)factory_gdi_interop;
		Factory_table_[IDWriteFactory_CreateGlyphRunAnalysis] = (void *)factory_glyph_analysis;
		Factory_table_[IDWriteFactory_CreateTextFormat] = (void *)factory_create_format;
		Factory_table_[IDWriteFactory_CreateTextLayout] = (void *)factory_create_layout;
		Factory_table_[IDWriteFactory_CreateGdiCompatibleTextLayout] = (void *)factory_create_gdi_layout;
		Factory_table_[IDWriteFactory_CreateEllipsisTrimmingSign] = (void *)factory_ellipsis_sign;
		Factory_table_[IDWriteFactory_CreateNumberSubstitution] = (void *)factory_number_substitution;
		Factory_table_[IDWriteFactory1_GetEudcFontCollection] = (void *)factory1_eudc;
		Factory_table_[IDWriteFactory1_IDWriteFactory1_CreateCustomRenderingParams] = (void *)factory1_custom_params;
		Factory_table_[IDWriteFactory2_GetSystemFontFallback] = (void *)factory2_fallback;
		Factory_table_[IDWriteFactory2_CreateFontFallbackBuilder] = (void *)factory2_fallback_builder;
		Factory_table_[IDWriteFactory3_GetSystemFontSet] = (void *)factory3_system_set;
		Factory_table_[IDWriteFactory3_IDWriteFactory3_GetSystemFontCollection] = (void *)factory3_system_collection;
		Factory_table_[IDWriteFactory2_TranslateColorGlyphRun] = (void *)factory2_color_run;
		Factory_table_[IDWriteFactory2_IDWriteFactory2_CreateCustomRenderingParams] = (void *)factory2_custom_params;
		Factory_table_[IDWriteFactory2_IDWriteFactory2_CreateGlyphRunAnalysis] = (void *)factory2_glyph_analysis;
		factory_object.vtbl = (const IDWriteFactory2Vtbl *)Factory_table_;
	}
	return factory_query((IDWriteFactory2 *)&factory_object, iid, (void **)out);
}

HRESULT WINAPI DWriteCreateFactory(DWRITE_FACTORY_TYPE type, REFIID iid, IUnknown **factory)
{
	if (!factory)
		return E_POINTER;
	if (type != DWRITE_FACTORY_TYPE_SHARED && type != DWRITE_FACTORY_TYPE_ISOLATED) {
		*factory = NULL;
		return E_INVALIDARG;
	}
	return create_factory(iid, factory);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
