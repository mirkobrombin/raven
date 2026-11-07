/* SPDX-License-Identifier: MIT */
#include "brush.h"
#include "graphics.h"
#include "gmath.h"

extern GpStatus GDIPAPI GdipAddPathRectangle(GpPath *, REAL, REAL, REAL, REAL);

// Families are names checked against what GDI enumerates; metrics come from a font created at the family's em size.
// Text reaches the surface as a white-on-black glyph picture drawn by GDI, used as coverage for the brush.

typedef struct gp_family
{
	WCHAR name[LF_FACESIZE];
	UINT16 em, ascent, descent, line;
	INT styles;
	LONG refs;
} GpFontFamily;

typedef struct gp_font
{
	GpFontFamily *family;
	LOGFONTW log;
	REAL size;
	GpUnit unit;
	INT style;
	REAL height_scale;
} GpFont;

typedef struct gp_collection
{
	GpFontFamily **families;
	INT count, capacity;
	BOOL installed;
} GpFontCollection;

enum { FontStyleBold = 1, FontStyleItalic = 2, FontStyleUnderline = 4, FontStyleStrikeout = 8 };

extern GpStatus GDIPAPI GdipDeleteFontFamily(GpFontFamily *);

static GpFontCollection installed_collection;

static BOOL same_name(const WCHAR *a, const WCHAR *b)
{
	return !lstrcmpiW(a, b);
}

static GpFontFamily *new_family(const WCHAR *name, HDC dc)
{
	GpFontFamily *family = gp_alloc(sizeof(GpFontFamily));
	if (!family)
		return NULL;
	lstrcpynW(family->name, name, LF_FACESIZE);
	family->refs = 1;
	LOGFONTW log = {0};
	log.lfHeight = -2048;
	log.lfWeight = FW_NORMAL;
	log.lfCharSet = DEFAULT_CHARSET;
	lstrcpynW(log.lfFaceName, name, LF_FACESIZE);
	HFONT font = CreateFontIndirectW(&log);
	HGDIOBJ old = font ? SelectObject(dc, font) : NULL;
	OUTLINETEXTMETRICW metrics;
	TEXTMETRICW text;
	family->styles = FontStyleBold | FontStyleItalic | FontStyleUnderline | FontStyleStrikeout;
	if (font && GetOutlineTextMetricsW(dc, sizeof(metrics), &metrics)) {
		family->em = (UINT16)metrics.otmEMSquare;
		family->ascent = (UINT16)metrics.otmTextMetrics.tmAscent;
		family->descent = (UINT16)metrics.otmTextMetrics.tmDescent;
		family->line = (UINT16)(metrics.otmTextMetrics.tmHeight + metrics.otmTextMetrics.tmExternalLeading);
	} else if (font && GetTextMetricsW(dc, &text)) {
		family->em = 2048;
		family->ascent = (UINT16)text.tmAscent;
		family->descent = (UINT16)text.tmDescent;
		family->line = (UINT16)(text.tmHeight + text.tmExternalLeading);
	} else {
		family->em = 2048;
		family->ascent = 1638;
		family->descent = 410;
		family->line = 2355;
	}
	if (old)
		SelectObject(dc, old);
	if (font)
		DeleteObject(font);
	return family;
}

static BOOL collection_add(GpFontCollection *collection, const WCHAR *name, HDC dc)
{
	for (INT i = 0; i < collection->count; i++) {
		if (same_name(collection->families[i]->name, name))
			return TRUE;
	}
	if (collection->count == collection->capacity) {
		INT capacity = collection->capacity ? collection->capacity * 2 : 32;
		GpFontFamily **grown = gp_realloc(collection->families, capacity * sizeof(GpFontFamily *));
		if (!grown)
			return FALSE;
		collection->families = grown;
		collection->capacity = capacity;
	}
	GpFontFamily *family = new_family(name, dc);
	if (!family)
		return FALSE;
	collection->families[collection->count++] = family;
	return TRUE;
}

typedef struct
{
	GpFontCollection *collection;
	HDC dc;
} enum_args;

static int CALLBACK add_enumerated(const LOGFONTW *log, const TEXTMETRICW *metrics, DWORD type, LPARAM data)
{
	(void)metrics;
	enum_args *args = (enum_args *)data;
	if (type & TRUETYPE_FONTTYPE) {
		if (log->lfFaceName[0] != '@')
			collection_add(args->collection, log->lfFaceName, args->dc);
	}
	return 1;
}

static void fill_installed(void)
{
	if (installed_collection.installed)
		return;
	installed_collection.installed = TRUE;
	HDC dc = CreateCompatibleDC(NULL);
	if (!dc)
		return;
	LOGFONTW filter = {0};
	filter.lfCharSet = DEFAULT_CHARSET;
	enum_args args = {&installed_collection, dc};
	EnumFontFamiliesExW(dc, &filter, add_enumerated, (LPARAM)&args, 0);
	DeleteDC(dc);
}

GpStatus GDIPAPI GdipNewInstalledFontCollection(GpFontCollection **out)
{
	if (!out)
		return InvalidParameter;
	fill_installed();
	*out = &installed_collection;
	return Ok;
}

GpStatus GDIPAPI GdipNewPrivateFontCollection(GpFontCollection **out)
{
	if (!out)
		return InvalidParameter;
	return (*out = gp_alloc(sizeof(GpFontCollection))) ? Ok : OutOfMemory;
}

GpStatus GDIPAPI GdipDeletePrivateFontCollection(GpFontCollection **collection)
{
	if (!collection)
		return InvalidParameter;
	if (*collection && !(*collection)->installed) {
		for (INT i = 0; i < (*collection)->count; i++)
			GdipDeleteFontFamily((*collection)->families[i]);
		gp_free((*collection)->families);
		gp_free(*collection);
	}
	*collection = NULL;
	return Ok;
}

GpStatus GDIPAPI GdipGetFontCollectionFamilyCount(GpFontCollection *collection, INT *out)
{
	if (!collection || !out)
		return InvalidParameter;
	*out = collection->count;
	return Ok;
}

GpStatus GDIPAPI GdipGetFontCollectionFamilyList(GpFontCollection *collection, INT wanted, GpFontFamily **out, INT *found)
{
	extern GpStatus GDIPAPI GdipCloneFontFamily(GpFontFamily *, GpFontFamily **);
	if (!collection || !out || !found)
		return InvalidParameter;
	INT i;
	for (i = 0; i < wanted && i < collection->count; i++) {
		GpStatus status = GdipCloneFontFamily(collection->families[i], &out[i]);
		if (status != Ok)
			return status;
	}
	*found = i;
	return Ok;
}

// A family is one shared object: asking twice, or cloning, hands back the same pointer with one more reference.

// Private fonts register with GDI for the process and the collection records the family names they bring.
static const WCHAR *ttf_family_name(const BYTE *data, SIZE_T size, WCHAR *out)
{
	#define BE16(p) ((UINT)(p)[0] << 8 | (p)[1])
	#define BE32(p) ((UINT)(p)[0] << 24 | (UINT)(p)[1] << 16 | (UINT)(p)[2] << 8 | (p)[3])
	if (size < 12)
		return NULL;
	UINT tables = BE16(data + 4);
	for (UINT i = 0; i < tables && 12 + (i + 1) * 16 <= size; i++) {
		const BYTE *entry = data + 12 + i * 16;
		if (memcmp(entry, "name", 4))
			continue;
		UINT offset = BE32(entry + 8), length = BE32(entry + 12);
		if (offset + length > size || length < 6)
			return NULL;
		const BYTE *name = data + offset;
		UINT count = BE16(name + 2), strings = BE16(name + 4);
		for (UINT n = 0; n < count && 6 + (n + 1) * 12 <= length; n++) {
			const BYTE *row = name + 6 + n * 12;
			if (BE16(row + 6) != 1 || BE16(row) != 3)
				continue;
			UINT text = BE16(row + 8), at = BE16(row + 10);
			if (strings + at + text > length || text / 2 >= LF_FACESIZE)
				continue;
			for (UINT c = 0; c < text / 2; c++)
				out[c] = (WCHAR)BE16(name + strings + at + c * 2);
			out[text / 2] = 0;
			return out;
		}
	}
	return NULL;
}

GpStatus GDIPAPI GdipPrivateAddMemoryFont(GpFontCollection *collection, const void *data, INT size)
{
	if (!collection)
		return InvalidParameter;
	if (!data || size <= 0)
		return InvalidParameter;
	DWORD installed;
	if (!AddFontMemResourceEx((void *)data, size, NULL, &installed))
		return InvalidParameter;
	WCHAR name[LF_FACESIZE];
	if (!ttf_family_name(data, size, name))
		return GenericError;
	HDC dc = CreateCompatibleDC(NULL);
	BOOL added = dc && collection_add(collection, name, dc);
	if (dc)
		DeleteDC(dc);
	return added ? Ok : OutOfMemory;
}

GpStatus GDIPAPI GdipPrivateAddFontFile(GpFontCollection *collection, const WCHAR *name)
{
	if (!collection || !name)
		return InvalidParameter;
	HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FileNotFound;
	DWORD size = GetFileSize(file, NULL), read = 0;
	void *data = gp_alloc(size ? size : 1);
	GpStatus status = OutOfMemory;
	if (data && ReadFile(file, data, size, &read, NULL))
		status = GdipPrivateAddMemoryFont(collection, data, read);
	gp_free(data);
	CloseHandle(file);
	return status;
}

GpStatus GDIPAPI GdipCreateFontFamilyFromName(const WCHAR *name, GpFontCollection *collection, GpFontFamily **out)
{
	if (!name || !out)
		return InvalidParameter;
	if (!collection) {
		fill_installed();
		collection = &installed_collection;
	}
	for (INT i = 0; i < collection->count; i++) {
		if (same_name(collection->families[i]->name, name)) {
			InterlockedIncrement(&collection->families[i]->refs);
			*out = collection->families[i];
			return Ok;
		}
	}
	return FontFamilyNotFound;
}

GpStatus GDIPAPI GdipCloneFontFamily(GpFontFamily *family, GpFontFamily **out)
{
	if (!family || !out)
		return InvalidParameter;
	InterlockedIncrement(&family->refs);
	*out = family;
	return Ok;
}

GpStatus GDIPAPI GdipDeleteFontFamily(GpFontFamily *family)
{
	if (!family)
		return InvalidParameter;
	if (!InterlockedDecrement(&family->refs))
		gp_free(family);
	return Ok;
}

GpStatus GDIPAPI GdipGetFamilyName(const GpFontFamily *family, WCHAR *out, LANGID language)
{
	(void)language;
	if (!family)
		return InvalidParameter;
	if (out)
		lstrcpynW(out, family->name, LF_FACESIZE);
	return Ok;
}

GpStatus GDIPAPI GdipGetCellAscent(const GpFontFamily *family, INT style, UINT16 *out)
{
	(void)style;
	if (!family || !out)
		return InvalidParameter;
	*out = family->ascent;
	return Ok;
}

GpStatus GDIPAPI GdipGetCellDescent(const GpFontFamily *family, INT style, UINT16 *out)
{
	(void)style;
	if (!family || !out)
		return InvalidParameter;
	*out = family->descent;
	return Ok;
}

GpStatus GDIPAPI GdipGetEmHeight(const GpFontFamily *family, INT style, UINT16 *out)
{
	(void)style;
	if (!family || !out)
		return InvalidParameter;
	*out = family->em;
	return Ok;
}

GpStatus GDIPAPI GdipGetLineSpacing(const GpFontFamily *family, INT style, UINT16 *out)
{
	(void)style;
	if (!family || !out)
		return InvalidParameter;
	*out = family->line;
	return Ok;
}

GpStatus GDIPAPI GdipIsStyleAvailable(const GpFontFamily *family, INT style, BOOL *out)
{
	(void)style;
	if (!family || !out)
		return InvalidParameter;
	*out = TRUE;
	return Ok;
}

static GpStatus generic_family(const WCHAR *const *names, GpFontFamily **out)
{
	if (!out)
		return InvalidParameter;
	for (; *names; names++) {
		if (GdipCreateFontFamilyFromName(*names, NULL, out) == Ok)
			return Ok;
	}
	fill_installed();
	if (installed_collection.count)
		return GdipCloneFontFamily(installed_collection.families[0], out);
	return FontFamilyNotFound;
}

GpStatus GDIPAPI GdipGetGenericFontFamilySansSerif(GpFontFamily **out)
{
	static const WCHAR *const names[] = {L"Microsoft Sans Serif", L"Arial", L"Liberation Sans", L"DejaVu Sans", L"Tahoma", NULL};
	return generic_family(names, out);
}

GpStatus GDIPAPI GdipGetGenericFontFamilySerif(GpFontFamily **out)
{
	static const WCHAR *const names[] = {L"Times New Roman", L"Liberation Serif", L"DejaVu Serif", NULL};
	return generic_family(names, out);
}

GpStatus GDIPAPI GdipGetGenericFontFamilyMonospace(GpFontFamily **out)
{
	static const WCHAR *const names[] = {L"Courier New", L"Liberation Mono", L"DejaVu Sans Mono", NULL};
	return generic_family(names, out);
}

GpStatus GDIPAPI GdipCreateFont(const GpFontFamily *family, REAL size, INT style, GpUnit unit, GpFont **out)
{
	if (!family || !out)
		return InvalidParameter;
	if (size <= 0.0f || unit == UnitDisplay)
		return InvalidParameter;
	GpFont *font = gp_alloc(sizeof(GpFont));
	if (!font)
		return OutOfMemory;
	font->family = (GpFontFamily *)family;
	InterlockedIncrement(&font->family->refs);
	font->size = size;
	font->style = style;
	font->unit = unit;
	font->log.lfHeight = -(LONG)(size + 0.5f);
	font->log.lfWeight = (style & FontStyleBold) ? FW_BOLD : FW_REGULAR;
	font->log.lfItalic = (style & FontStyleItalic) != 0;
	font->log.lfUnderline = (style & FontStyleUnderline) != 0;
	font->log.lfStrikeOut = (style & FontStyleStrikeout) != 0;
	font->log.lfCharSet = DEFAULT_CHARSET;
	lstrcpynW(font->log.lfFaceName, family->name, LF_FACESIZE);
	*out = font;
	return Ok;
}

// An unknown name falls back to the sans serif family rather than failing. A positive height names the character cell, which
// holds more than the em.
GpStatus GDIPAPI GdipCreateFontFromLogfontW(HDC dc, const LOGFONTW *log, GpFont **out)
{
	if (!dc || !log || !out)
		return InvalidParameter;
	GpFontFamily *family;
	GpStatus status = GdipCreateFontFamilyFromName(log->lfFaceName, NULL, &family);
	if (status != Ok && (status = GdipGetGenericFontFamilySansSerif(&family)) != Ok)
		return NotTrueTypeFont;
	INT style = (log->lfWeight >= FW_BOLD ? FontStyleBold : 0) | (log->lfItalic ? FontStyleItalic : 0) | (log->lfUnderline ? FontStyleUnderline : 0) | (log->lfStrikeOut ? FontStyleStrikeout : 0);
	REAL size = log->lfHeight < 0 ? (REAL)-log->lfHeight : (REAL)log->lfHeight * family->em / (family->ascent + family->descent);
	status = GdipCreateFont(family, size ? size : 12.0f, style, UnitWorld, out);
	if (status == Ok) {
		(*out)->log = *log;
		lstrcpynW((*out)->log.lfFaceName, family->name, LF_FACESIZE);
	}
	GdipDeleteFontFamily(family);
	return status;
}

GpStatus GDIPAPI GdipCreateFontFromLogfontA(HDC dc, const LOGFONTA *log, GpFont **out)
{
	if (!dc || !log || !out)
		return InvalidParameter;
	LOGFONTW wide;
	memcpy(&wide, log, FIELD_OFFSET(LOGFONTW, lfFaceName));
	if (!MultiByteToWideChar(CP_ACP, 0, log->lfFaceName, -1, wide.lfFaceName, LF_FACESIZE))
		return NotTrueTypeFont;
	return GdipCreateFontFromLogfontW(dc, &wide, out);
}

GpStatus GDIPAPI GdipCreateFontFromDC(HDC dc, GpFont **out)
{
	if (!dc || !out)
		return InvalidParameter;
	HFONT handle = GetCurrentObject(dc, OBJ_FONT);
	LOGFONTW log;
	if (!handle || !GetObjectW(handle, sizeof(log), &log))
		return GenericError;
	return GdipCreateFontFromLogfontW(dc, &log, out);
}

GpStatus GDIPAPI GdipCloneFont(GpFont *font, GpFont **out)
{
	if (!font || !out)
		return InvalidParameter;
	GpFont *copy = gp_dup(font, sizeof(GpFont));
	if (!copy)
		return OutOfMemory;
	InterlockedIncrement(&copy->family->refs);
	*out = copy;
	return Ok;
}

GpStatus GDIPAPI GdipDeleteFont(GpFont *font)
{
	if (!font)
		return InvalidParameter;
	GdipDeleteFontFamily(font->family);
	gp_free(font);
	return Ok;
}

GpStatus GDIPAPI GdipGetFamily(GpFont *font, GpFontFamily **out)
{
	if (!font || !out)
		return InvalidParameter;
	return GdipCloneFontFamily(font->family, out);
}

GpStatus GDIPAPI GdipGetFontSize(GpFont *font, REAL *out)
{
	if (!font || !out)
		return InvalidParameter;
	*out = font->size;
	return Ok;
}

GpStatus GDIPAPI GdipGetFontStyle(GpFont *font, INT *out)
{
	if (!font || !out)
		return InvalidParameter;
	*out = font->style;
	return Ok;
}

GpStatus GDIPAPI GdipGetFontUnit(GpFont *font, GpUnit *out)
{
	if (!font || !out)
		return InvalidParameter;
	*out = font->unit;
	return Ok;
}

static REAL font_pixels(const GpFont *font, const GpGraphics *graphics)
{
	// A page counted in pixels reads the horizontal resolution.
	REAL dpi = !graphics ? 96.0f : graphics->unit == UnitDisplay || graphics->unit == UnitPixel ? graphics->dpi_x : graphics->dpi_y;
	switch (font->unit) {
	case UnitPoint: return font->size * dpi / 72.0f;
	case UnitInch: return font->size * dpi;
	case UnitDocument: return font->size * dpi / 300.0f;
	case UnitMillimeter: return font->size * dpi / 25.4f;
	// Sizes in world units or pixels follow the page; the others are fixed physical lengths.
	default:
		if (graphics && graphics->unit != UnitDisplay)
			return font->size * gp_unit_scale(graphics, graphics->unit, dpi) * graphics->scale;
		return font->size;
	}
}

// The logical font as a device would need it: scaled and turned by the graphics' world.
static void device_log(const GpFont *font, const GpGraphics *graphics, LOGFONTW *out)
{
	const GpMatrix *world = &graphics->world;
	REAL sy = gp_sqrtf(world->m[2] * world->m[2] + world->m[3] * world->m[3]);
	REAL pixels = font_pixels(font, graphics);
	INT angle = (INT)gp_floor(-(REAL)atan2(world->m[1], world->m[0]) * 1800.0f / (REAL)GP_PI + 0.5f);
	if (angle < 0)
		angle += 3600;
	*out = font->log;
	out->lfHeight = -(LONG)gp_floor(pixels * sy + 0.5f);
	out->lfWidth = 0;
	out->lfEscapement = out->lfOrientation = angle;
}

GpStatus GDIPAPI GdipGetLogFontW(GpFont *font, const GpGraphics *graphics, LOGFONTW *out)
{
	if (!font || !graphics || !out)
		return InvalidParameter;
	device_log(font, graphics, out);
	return Ok;
}

GpStatus GDIPAPI GdipGetLogFontA(GpFont *font, const GpGraphics *graphics, LOGFONTA *out)
{
	if (!font || !graphics || !out)
		return InvalidParameter;
	LOGFONTW wide;
	device_log(font, graphics, &wide);
	memcpy(out, &wide, FIELD_OFFSET(LOGFONTW, lfFaceName));
	WideCharToMultiByte(CP_ACP, 0, wide.lfFaceName, -1, out->lfFaceName, LF_FACESIZE, NULL, NULL);
	return Ok;
}

// Font height in the unit given by dpi: the family's line spacing at this em size.
GpStatus GDIPAPI GdipGetFontHeightGivenDPI(const GpFont *font, REAL dpi, REAL *out)
{
	if (!font || !out)
		return InvalidParameter;
	REAL pixels = font->size;
	switch (font->unit) {
	case UnitPoint: pixels = font->size * dpi / 72.0f; break;
	case UnitInch: pixels = font->size * dpi; break;
	case UnitDocument: pixels = font->size * dpi / 300.0f; break;
	case UnitMillimeter: pixels = font->size * dpi / 25.4f; break;
	default: break;
	}
	*out = pixels * font->family->line / font->family->em;
	return Ok;
}

GpStatus GDIPAPI GdipGetFontHeight(const GpFont *font, const GpGraphics *graphics, REAL *out)
{
	if (!font || !out)
		return InvalidParameter;
	if (!graphics)
		return GdipGetFontHeightGivenDPI(font, 96.0f, out);
	REAL dpi = graphics->dpi_y, pixels;
	GpStatus status = GdipGetFontHeightGivenDPI(font, dpi, &pixels);
	if (status == Ok)
		*out = pixels / (gp_unit_scale(graphics, graphics->unit, dpi) * graphics->scale);
	return status;
}

// Text is laid out in pixels of the font before the world transform; only painting takes the world's scale along.
static HFONT make_gdi_font(const GpFont *font, const GpGraphics *graphics, REAL scale)
{
	LOGFONTW log = font->log;
	log.lfHeight = -(LONG)(font_pixels(font, graphics) * scale + 0.5f);
	return CreateFontIndirectW(&log);
}

typedef struct
{
	INT start, length;
	REAL width;
} text_line;

// Breaks text into lines inside the width, at spaces when wrapping and at newlines always.
static INT lay_out(HDC dc, const WCHAR *text, INT length, REAL width, const GpStringFormat *format, text_line **out, REAL *widest)
{
	INT capacity = 16, lines = 0, at = 0;
	text_line *list = gp_alloc(capacity * sizeof(text_line));
	BOOL wrap = !format || !(format->flags & StringFormatFlagsNoWrap);
	*widest = 0.0f;
	while (list && at <= length) {
		INT end = at;
		while (end < length && text[end] != '\n')
			end++;
		INT line_end = end;
		SIZE size = {0, 0};
		if (wrap && width > 0.0f && end > at) {
			INT fit = 0;
			GetTextExtentExPointW(dc, text + at, end - at, (INT)width, &fit, NULL, &size);
			if (fit < end - at) {
				INT brk = at + fit;
				while (brk > at && text[brk] != ' ')
					brk--;
				line_end = brk > at ? brk : at + (fit ? fit : 1);
			}
		}
		// Without wrapping, a clipped line simply ends where the width does.
		if (!wrap && width > 0.0f && end > at) {
			INT fit = 0;
			GetTextExtentExPointW(dc, text + at, end - at, (INT)(width + 0.01f), &fit, NULL, &size);
			if (fit < end - at)
				line_end = end = at + (fit ? fit : 1);
		}
		INT shown = line_end - at;
		while (shown > 0 && (text[at + shown - 1] == '\r' || (text[at + shown - 1] == ' ' && !(format && (format->flags & StringFormatFlagsMeasureTrailingSpaces)))))
			shown--;
		if (shown)
			GetTextExtentPoint32W(dc, text + at, shown, &size);
		else
			size.cx = 0;
		if (lines == capacity) {
			capacity *= 2;
			text_line *grown = gp_realloc(list, capacity * sizeof(text_line));
			if (!grown)
				break;
			list = grown;
		}
		list[lines].start = at;
		list[lines].length = shown;
		list[lines].width = (REAL)size.cx;
		if (list[lines].width > *widest)
			*widest = list[lines].width;
		lines++;
		// An unwrapped format keeps to the first line.
		if (!wrap)
			break;
		at = line_end;
		while (at < length && text[at] == ' ' && line_end != end)
			at++;
		if (at < length && text[at] == '\n')
			at++;
		if (at >= length)
			break;
	}
	*out = list;
	return lines;
}

static INT text_length(const WCHAR *text, INT length)
{
	return length == -1 ? lstrlenW(text) : length;
}

static REAL aligned(StringAlignment alignment, REAL space, REAL used)
{
	return alignment == StringAlignmentCenter ? (space - used) / 2.0f : alignment == StringAlignmentFar ? space - used : 0.0f;
}

static HDC metrics_dc(const GpGraphics *graphics, BOOL *own)
{
	*own = FALSE;
	if (graphics && graphics->dc)
		return graphics->dc;
	*own = TRUE;
	return CreateCompatibleDC(NULL);
}

// Pixels per page unit along each axis, which is what measured text reports in: the world transform does not change a
// measurement. The margin ordinary formats leave on either side of the text is a sixth of the em, in pixels.
static void text_scales(const GpGraphics *graphics, REAL *sx, REAL *sy)
{
	REAL page = graphics->unit == UnitDisplay ? 1.0f : graphics->scale;
	*sx = *sy = gp_unit_scale(graphics, graphics->unit, graphics->dpi_y) * page;
}

// A rectangle's width in layout pixels: zero means no limit, and one narrower than its margins still limits to one glyph.
static REAL layout_width(REAL width, REAL sx, REAL margin)
{
	if (width <= 0.0f)
		return 0.0f;
	REAL inner = width * sx - margin * 2.0f;
	return inner < 1.0f ? 1.0f : inner;
}

static REAL text_margin(const GpFont *font, const GpGraphics *graphics, const GpStringFormat *format)
{
	return format && format->typographic ? 0.0f : font_pixels(font, graphics) / 6.0f;
}

GpStatus GDIPAPI GdipMeasureString(GpGraphics *graphics, const WCHAR *text, INT length, const GpFont *font, const GpRectF *rect, const GpStringFormat *format, GpRectF *out, INT *characters, INT *line_count)
{
	if (!graphics || !text || !font || !rect || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	length = text_length(text, length);
	BOOL own;
	HDC dc = metrics_dc(graphics, &own);
	HFONT handle = make_gdi_font(font, graphics, 1.0f);
	HGDIOBJ old = handle ? SelectObject(dc, handle) : NULL;
	TEXTMETRICW metrics;
	GetTextMetricsW(dc, &metrics);
	text_line *lines = NULL;
	REAL widest = 0.0f, sx, sy;
	text_scales(graphics, &sx, &sy);
	REAL margin = text_margin(font, graphics, format);
	REAL inner = layout_width(rect->Width, sx, margin);
	INT count = lay_out(dc, text, length, inner, format, &lines, &widest);
	REAL line_height = (REAL)(metrics.tmHeight + metrics.tmExternalLeading);
	BOOL clipped = rect->Height > 0.0f && (!format || !(format->flags & StringFormatFlagsNoClip));
	if (clipped) {
		INT fits = (INT)(rect->Height * sy / line_height);
		if (fits < 1)
			fits = 1;
		if (count > fits)
			count = fits;
	}
	REAL height = count * line_height / sy, width = (widest + margin * 2.0f) / sx;
	if (clipped && height > rect->Height)
		height = rect->Height;
	StringAlignment horizontal = format ? format->alignment : StringAlignmentNear, vertical = format ? format->line_alignment : StringAlignmentNear;
	out->X = rect->X + aligned(horizontal, rect->Width, width);
	out->Y = rect->Y + aligned(vertical, rect->Height, height);
	out->Width = width;
	out->Height = height;
	if (characters) {
		BOOL wrapped = !format || !(format->flags & StringFormatFlagsNoWrap);
		INT used = 0;
		if (count && lines) {
			used = lines[count - 1].start + lines[count - 1].length;
			while (wrapped && used < length && (text[used] == ' ' || text[used] == '\r' || text[used] == '\n'))
				used++;
		}
		*characters = used;
	}
	if (line_count)
		*line_count = count;
	gp_free(lines);
	if (old)
		SelectObject(dc, old);
	if (handle)
		DeleteObject(handle);
	if (own)
		DeleteDC(dc);
	return Ok;
}

GpStatus GDIPAPI GdipDrawString(GpGraphics *graphics, const WCHAR *text, INT length, const GpFont *font, const GpRectF *rect, const GpStringFormat *format, const GpBrush *brush)
{
	if (!graphics || !text || !font || !brush || !rect)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	length = text_length(text, length);
	HDC dc = CreateCompatibleDC(NULL);
	if (!dc)
		return GenericError;
	REAL world_scale = gp_sqrtf(graphics->world.m[2] * graphics->world.m[2] + graphics->world.m[3] * graphics->world.m[3]);
	HFONT handle = make_gdi_font(font, graphics, world_scale);
	HGDIOBJ old = handle ? SelectObject(dc, handle) : NULL;
	TEXTMETRICW metrics;
	GetTextMetricsW(dc, &metrics);
	text_line *lines = NULL;
	REAL widest = 0.0f;
	GpMatrix to_device;
	gp_world_to_device(graphics, &to_device);
	GpPointF origin = {rect->X, rect->Y};
	gp_matrix_apply(&to_device, &origin, 1);
	REAL box_width = rect->Width * gp_fabs(to_device.m[0]), box_height = rect->Height * gp_fabs(to_device.m[3]);
	REAL margin = text_margin(font, graphics, format) * world_scale;
	INT count = lay_out(dc, text, length, layout_width(box_width, 1.0f, margin), format, &lines, &widest);
	REAL line_height = (REAL)(metrics.tmHeight + metrics.tmExternalLeading);
	REAL total = count * line_height;
	StringAlignment horizontal = format ? format->alignment : StringAlignmentNear, vertical = format ? format->line_alignment : StringAlignmentNear;
	REAL top = origin.Y + aligned(vertical, box_height, total);
	INT pad = (INT)line_height;
	INT width = (INT)gp_ceil(widest) + pad * 2, height = (INT)gp_ceil(total) + pad * 2;
	GpStatus status = Ok;
	if (lines && width > 0 && height > 0) {
		BITMAPINFO header = {{sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB}};
		void *bits;
		HBITMAP bitmap = CreateDIBSection(dc, &header, DIB_RGB_COLORS, &bits, NULL, 0);
		if (!bitmap) {
			status = GenericError;
		} else {
			HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(255, 255, 255));
			SetTextAlign(dc, TA_LEFT | TA_TOP);
			REAL left_edge = origin.X - pad;
			for (INT i = 0; i < count; i++) {
				REAL used = lines[i].width + margin * 2.0f;
				REAL x = aligned(horizontal, box_width, used) + margin;
				TextOutW(dc, (INT)(x + pad), pad + (INT)(i * line_height), text + lines[i].start, lines[i].length);
			}
			GdiFlush();
			RECT limit, area = {(LONG)left_edge, (LONG)top - pad, (LONG)left_edge + width, (LONG)top - pad + height};
			if (gp_clip_bounds(graphics, &limit) && IntersectRect(&area, &area, &limit)) {
				gp_surface surface;
				if ((status = gp_surface_begin(graphics, &surface, &area)) == Ok) {
					BYTE *mask = gp_clip_mask(graphics, &area);
					GpMatrix inverse;
					BOOL has_inverse = gp_matrix_invert(&to_device, &inverse);
					INT area_width = area.right - area.left;
					INT shift_x = area.left - (LONG)left_edge, shift_y = area.top - ((LONG)top - pad);
					for (INT y = 0; y < area.bottom - area.top; y++) {
						const DWORD *row = (const DWORD *)bits + (SIZE_T)(y + shift_y) * width;
						for (INT x = 0; x < area_width; x++) {
							DWORD glyph = row[x + shift_x];
							UINT coverage = (((glyph >> 16) & 255) + ((glyph >> 8) & 255) + (glyph & 255)) / 3;
							if (!coverage || (mask && !mask[(SIZE_T)y * area_width + x]))
								continue;
							ARGB color = gp_brush_sample(brush, has_inverse ? &inverse : NULL, area.left + x + 0.5f, area.top + y + 0.5f, graphics->origin_x, graphics->origin_y);
							gp_surface_blend(&surface, area.left + x, area.top + y, color, (BYTE)coverage);
						}
					}
					gp_free(mask);
					gp_surface_end(&surface);
				}
			}
			SelectObject(dc, old_bitmap);
			DeleteObject(bitmap);
		}
	}
	gp_free(lines);
	if (old)
		SelectObject(dc, old);
	if (handle)
		DeleteObject(handle);
	DeleteDC(dc);
	return status;
}

GpStatus GDIPAPI GdipMeasureCharacterRanges(GpGraphics *graphics, const WCHAR *text, INT length, const GpFont *font, const GpRectF *rect, const GpStringFormat *format, INT count, GpRegion **regions)
{
	extern GpStatus GDIPAPI GdipCombineRegionRect(GpRegion *, const GpRectF *, CombineMode);
	if (!graphics || !text || !font || !rect || !format || !regions)
		return InvalidParameter;
	if (count < format->range_count)
		return InvalidParameter;
	length = text_length(text, length);
	BOOL own;
	HDC dc = metrics_dc(graphics, &own);
	HFONT handle = make_gdi_font(font, graphics, 1.0f);
	HGDIOBJ old = handle ? SelectObject(dc, handle) : NULL;
	TEXTMETRICW metrics;
	GetTextMetricsW(dc, &metrics);
	text_line *lines = NULL;
	REAL widest;
	REAL sx, sy, margin = text_margin(font, graphics, format);
	text_scales(graphics, &sx, &sy);
	INT line_count = lay_out(dc, text, length, layout_width(rect->Width, sx, margin), format, &lines, &widest);
	REAL line_height = (REAL)(metrics.tmHeight + metrics.tmExternalLeading);
	BOOL nowhere = !rect->Width && !rect->Height && !(format->flags & StringFormatFlagsNoClip);
	if (rect->Height > 0.0f && !(format->flags & StringFormatFlagsNoClip)) {
		INT fits = (INT)(rect->Height * sy / line_height);
		if (line_count > (fits < 1 ? 1 : fits))
			line_count = fits < 1 ? 1 : fits;
	}
	for (INT r = 0; r < format->range_count; r++) {
		extern GpStatus GDIPAPI GdipSetEmpty(GpRegion *);
		GdipSetEmpty(regions[r]);
		INT first = format->ranges[r].First, last = first + format->ranges[r].Length;
		for (INT i = 0; i < line_count && !nowhere; i++) {
			INT from = first > lines[i].start ? first : lines[i].start;
			INT to = last < lines[i].start + lines[i].length ? last : lines[i].start + lines[i].length;
			if (from >= to)
				continue;
			SIZE before = {0, 0}, span = {0, 0};
			if (from > lines[i].start)
				GetTextExtentPoint32W(dc, text + lines[i].start, from - lines[i].start, &before);
			GetTextExtentPoint32W(dc, text + from, to - from, &span);
			GpRectF box = {rect->X + aligned(format->alignment, rect->Width, (lines[i].width + margin * 2.0f) / sx) + (before.cx + margin) / sx,
				rect->Y + aligned(format->line_alignment, rect->Height, line_count * line_height / sy) + i * line_height / sy, span.cx / sx, line_height / sy};
			GdipCombineRegionRect(regions[r], &box, CombineModeUnion);
		}
	}
	gp_free(lines);
	if (old)
		SelectObject(dc, old);
	if (handle)
		DeleteObject(handle);
	if (own)
		DeleteDC(dc);
	return Ok;
}

enum { DriverCmapLookup = 1, DriverVertical = 2, DriverRealizedAdvance = 4 };

// Positions name each glyph's baseline start. With realized advance the first one starts a run and the font spaces the rest.
GpStatus GDIPAPI GdipDrawDriverString(GpGraphics *graphics, const UINT16 *text, INT length, const GpFont *font, const GpBrush *brush, const GpPointF *positions, INT flags, const GpMatrix *matrix)
{
	if (!graphics || !text || !font || !brush || !positions)
		return InvalidParameter;
	if (length < 0)
		length = lstrlenW(text);
	if (!length)
		return Ok;
	REAL sx, sy;
	text_scales(graphics, &sx, &sy);
	GpMatrix saved = graphics->world;
	if (matrix)
		gp_matrix_mul(&graphics->world, matrix, &saved);
	REAL margin = font_pixels(font, graphics) / 6.0f / sx, em = font_pixels(font, graphics) / sy;
	GpStatus status = Ok;
	INT runs = (flags & DriverRealizedAdvance) ? 1 : length;
	for (INT i = 0; i < runs && status == Ok; i++) {
		GpRectF rect = {positions[i].X - margin, positions[i].Y - em, 0.0f, 0.0f};
		status = GdipDrawString(graphics, text + i, runs == 1 ? length : 1, font, &rect, NULL, brush);
	}
	graphics->world = saved;
	return status;
}

GpStatus GDIPAPI GdipMeasureDriverString(GpGraphics *graphics, const UINT16 *text, INT length, const GpFont *font, const GpPointF *positions, INT flags, const GpMatrix *matrix, GpRectF *out)
{
	(void)matrix;
	if (!graphics || !text || !font || !positions || !out)
		return InvalidParameter;
	if (length < 0)
		length = lstrlenW(text);
	out->X = out->Y = out->Width = out->Height = 0.0f;
	if (!length)
		return Ok;
	BOOL own;
	HDC dc = metrics_dc(graphics, &own);
	HFONT handle = make_gdi_font(font, graphics, 1.0f);
	HGDIOBJ old = handle ? SelectObject(dc, handle) : NULL;
	TEXTMETRICW metrics;
	GetTextMetricsW(dc, &metrics);
	REAL sx, sy;
	text_scales(graphics, &sx, &sy);
	REAL line = (REAL)(metrics.tmHeight + metrics.tmExternalLeading) / sy, em = font_pixels(font, graphics) / sy;
	REAL left = 0.0f, right = 0.0f, pen = 0.0f;
	for (INT i = 0; i < length; i++) {
		SIZE size = {0, 0};
		GetTextExtentPoint32W(dc, text + i, 1, &size);
		REAL start = (flags & DriverRealizedAdvance) ? positions[0].X + pen : positions[i].X, end = start + size.cx / sx;
		left = i && left < start ? left : start;
		right = i && right > end ? right : end;
		pen += size.cx / sx;
	}
	// A vertical run lies along the height, with the line's depth across it.
	if (flags & DriverVertical) {
		out->X = positions[0].Y - em;
		out->Y = left;
		out->Width = line;
		out->Height = right - left;
	} else {
		out->X = left;
		out->Y = positions[0].Y - em;
		out->Width = right - left;
		out->Height = line;
	}
	if (old)
		SelectObject(dc, old);
	if (handle)
		DeleteObject(handle);
	if (own)
		DeleteDC(dc);
	return Ok;
}

// A path from text is an honest approximation with this design: GDI hands out coverage, so the path carries the boxes
// of the glyph rows rather than outlines. Region and bounds consumers see the text's extent.
GpStatus GDIPAPI GdipAddPathString(GpPath *path, const WCHAR *text, INT length, const GpFontFamily *family, INT style, REAL size, const GpRectF *rect, const GpStringFormat *format)
{
	if (!path || !text || !family || !rect)
		return InvalidParameter;
	if (!length)
		return Ok;
	GpFont *font;
	GpStatus status = GdipCreateFont(family, size, style, UnitPixel, &font);
	if (status != Ok)
		return status;
	length = text_length(text, length);
	HDC dc = CreateCompatibleDC(NULL);
	HFONT handle = dc ? make_gdi_font(font, NULL, 1.0f) : NULL;
	HGDIOBJ old = handle ? SelectObject(dc, handle) : NULL;
	TEXTMETRICW metrics;
	GetTextMetricsW(dc, &metrics);
	text_line *lines = NULL;
	REAL widest;
	INT count = lay_out(dc, text, length, rect->Width, format, &lines, &widest);
	for (INT i = 0; i < count && status == Ok; i++) {
		if (lines[i].width > 0.0f)
			status = GdipAddPathRectangle(path, rect->X, rect->Y + i * (REAL)metrics.tmHeight, lines[i].width, (REAL)metrics.tmHeight);
	}
	gp_free(lines);
	if (old)
		SelectObject(dc, old);
	if (handle)
		DeleteObject(handle);
	if (dc)
		DeleteDC(dc);
	GdipDeleteFont(font);
	return status;
}

GpStatus GDIPAPI GdipAddPathStringI(GpPath *path, const WCHAR *text, INT length, const GpFontFamily *family, INT style, REAL size, const GpRect *rect, const GpStringFormat *format)
{
	if (!rect)
		return InvalidParameter;
	GpRectF box = {(REAL)rect->X, (REAL)rect->Y, (REAL)rect->Width, (REAL)rect->Height};
	return GdipAddPathString(path, text, length, family, style, size, &box, format);
}
