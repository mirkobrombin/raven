// Glyph indices are the selected face's own, the ones its cmap table gives, as programs that read the table themselves (Qt) compute
// them: GetGlyphIndicesW, GetCharWidthI, GetCharABCWidthsI, GetGlyphOutlineW with GGO_GLYPH_INDEX, ExtTextOutW with ETO_GLYPH_INDEX and
// Uniscribe's shape, place, font properties and text calls all agree with it. GetGlyphOutlineW measures a glyph of a very large font,
// as programs create one to read design metrics.
#include <windows.h>
#include <usp10.h>

#define CMAP_TAG 0x70616d63 /* 'cmap' in the byte order GetFontData takes */

static unsigned char cmap[65536];
static DWORD *pixels;

static unsigned big16(const unsigned char *at) { return (unsigned)at[0] << 8 | at[1]; }
static unsigned big32(const unsigned char *at) { return big16(at) << 16 | big16(at + 2); }

// The glyph of a BMP character from the Windows Unicode subtable (platform 3, encoding 1, format 4), read as the TrueType specification
// lays it out; 0 when the table has no such subtable or the character is not mapped.
static unsigned cmap_glyph(DWORD size, unsigned character)
{
	if (size < 4)
		return 0;
	const unsigned tables = big16(cmap + 2);
	for (unsigned i = 0; i < tables && 4 + i * 8 + 8 <= size; i++) {
		const unsigned char *record = cmap + 4 + i * 8;
		const DWORD offset = big32(record + 4);
		if (big16(record) != 3 || big16(record + 2) != 1 || offset + 14 > size || big16(cmap + offset) != 4)
			continue;
		const unsigned char *table = cmap + offset;
		const unsigned segments = big16(table + 6) / 2;
		const unsigned char *ends = table + 14, *starts = ends + segments * 2 + 2, *deltas = starts + segments * 2, *ranges = deltas + segments * 2;
		if ((DWORD)(ranges + segments * 2 - cmap) > size)
			return 0;
		for (unsigned s = 0; s < segments; s++) {
			const unsigned end = big16(ends + s * 2), start = big16(starts + s * 2);
			if (character > end)
				continue;
			if (character < start)
				return 0;
			const unsigned delta = big16(deltas + s * 2), range = big16(ranges + s * 2);
			if (!range)
				return (character + delta) & 0xffff;
			const unsigned char *glyph = ranges + s * 2 + range + (character - start) * 2;
			if ((DWORD)(glyph + 2 - cmap) > size)
				return 0;
			const unsigned id = big16(glyph);
			return id ? (id + delta) & 0xffff : 0;
		}
	}
	return 0;
}

static HFONT arial(int height)
{
	LOGFONTW definition = {0};
	definition.lfHeight = height;
	definition.lfWeight = FW_NORMAL;
	lstrcpyW(definition.lfFaceName, L"Arial");
	return CreateFontIndirectW(&definition);
}

// The dark pixels in the bitmap after drawing black on white.
static int ink(void)
{
	int count = 0;
	for (int i = 0; i < 64 * 32; i++)
		if ((pixels[i] & 0xff) < 0x80)
			count++;
	return count;
}

static void clear(void)
{
	for (int i = 0; i < 64 * 32; i++)
		pixels[i] = 0x00ffffff;
}

int raven_entry(void)
{
	static const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
	HDC dc = CreateCompatibleDC(NULL);
	if (!dc)
		return 1;
	BITMAPINFO info = {{sizeof(BITMAPINFOHEADER), 64, -32, 1, 32, BI_RGB}};
	void *bits = NULL;
	HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bitmap || !bits)
		return 2;
	SelectObject(dc, bitmap);
	pixels = bits;
	HFONT font = arial(-16);
	if (!font || !SelectObject(dc, font))
		return 3;

	// The glyph the face's own table names for 'A', as Qt reads it.
	const DWORD size = GetFontData(dc, CMAP_TAG, 0, NULL, 0);
	if (size == GDI_ERROR || size > sizeof cmap || GetFontData(dc, CMAP_TAG, 0, cmap, size) != size)
		return 4;
	const WORD expected = (WORD)cmap_glyph(size, 'A'), space = (WORD)cmap_glyph(size, ' ');
	if (!expected || expected == 'A' || !space)
		return 5;

	WORD glyphs[3] = {0};
	if (GetGlyphIndicesW(dc, L"A A", 3, glyphs, 0) != 3 || glyphs[0] != expected || glyphs[1] != space || glyphs[2] != expected)
		return 10;
	// A character the face lacks is its missing glyph, or 0xffff when the caller asks for it to be marked.
	const WCHAR absent = 0xffff;
	if (GetGlyphIndicesW(dc, &absent, 1, glyphs, 0) != 1 || glyphs[0] != 0)
		return 11;
	if (GetGlyphIndicesW(dc, &absent, 1, glyphs, GGI_MARK_NONEXISTING_GLYPHS) != 1 || glyphs[0] != 0xffff)
		return 12;
	if (GetGlyphIndicesW(dc, NULL, 1, glyphs, 0) != GDI_ERROR || GetGlyphIndicesW(dc, L"A", 1, NULL, 0) != GDI_ERROR)
		return 13;
	if (GetGlyphIndicesA(dc, "A", 1, glyphs, 0) != 1 || glyphs[0] != expected)
		return 14;

	// The widths by glyph are the widths by character.
	INT by_character = 0, by_glyph = 0, by_range = 0;
	ABC abc = {0};
	WORD glyph = expected;
	if (!GetCharWidth32W(dc, 'A', 'A', &by_character) || by_character <= 0)
		return 20;
	if (!GetCharWidthI(dc, 0, 1, &glyph, &by_glyph) || by_glyph != by_character)
		return 21;
	if (!GetCharWidthI(dc, expected, 1, NULL, &by_range) || by_range != by_character)
		return 22;
	if (!GetCharABCWidthsI(dc, 0, 1, &glyph, &abc) || abc.abcA + (int)abc.abcB + abc.abcC != by_character)
		return 23;
	if (GetCharWidthI(dc, 0, 1, &glyph, NULL))
		return 24;

	// GetGlyphOutlineW names the same glyph by character or by index.
	GLYPHMETRICS character_metrics = {0}, glyph_metrics = {0};
	if (GetGlyphOutlineW(dc, 'A', GGO_METRICS, &character_metrics, 0, NULL, &identity) == GDI_ERROR)
		return 30;
	if (GetGlyphOutlineW(dc, expected, GGO_METRICS | GGO_GLYPH_INDEX, &glyph_metrics, 0, NULL, &identity) == GDI_ERROR)
		return 31;
	if (glyph_metrics.gmBlackBoxX != character_metrics.gmBlackBoxX || glyph_metrics.gmBlackBoxY != character_metrics.gmBlackBoxY ||
		glyph_metrics.gmCellIncX != character_metrics.gmCellIncX || glyph_metrics.gmCellIncX != by_character ||
		glyph_metrics.gmptGlyphOrigin.y != character_metrics.gmptGlyphOrigin.y || !glyph_metrics.gmBlackBoxX || glyph_metrics.gmBlackBoxY < 8)
		return 32;
	const DWORD needed = GetGlyphOutlineW(dc, expected, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX, &glyph_metrics, 0, NULL, &identity);
	if (needed == GDI_ERROR || needed < glyph_metrics.gmBlackBoxY * ((glyph_metrics.gmBlackBoxX + 3) & ~3U))
		return 33;
	if (GetGlyphOutlineW(dc, expected, GGO_METRICS | GGO_GLYPH_INDEX, NULL, 0, NULL, &identity) != GDI_ERROR)
		return 34;

	// ETO_GLYPH_INDEX draws the glyph the character draws.
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(0, 0, 0));
	clear();
	if (!ExtTextOutW(dc, 4, 4, 0, NULL, L"A", 1, NULL))
		return 40;
	GdiFlush();
	const int character_ink = ink();
	clear();
	if (!ExtTextOutW(dc, 4, 4, ETO_GLYPH_INDEX, NULL, (const WCHAR *)&glyph, 1, NULL))
		return 41;
	GdiFlush();
	const int glyph_ink = ink();
	if (character_ink < 10 || glyph_ink * 10 < character_ink * 9 || glyph_ink * 10 > character_ink * 11)
		return 42;

	// Uniscribe shapes to the same glyphs and places them with the same widths.
	SCRIPT_CACHE cache = NULL;
	SCRIPT_ANALYSIS analysis = {0};
	WORD shaped[4] = {0}, clusters[4] = {0};
	SCRIPT_VISATTR attributes[4];
	int count = 0, advances[4] = {0};
	GOFFSET offsets[4];
	ABC run = {0};
	if (ScriptShape(dc, &cache, L"A A", 3, 4, &analysis, shaped, clusters, attributes, &count) != S_OK || count != 3 ||
		shaped[0] != expected || shaped[1] != space || shaped[2] != expected)
		return 50;
	if (ScriptPlace(dc, &cache, shaped, count, attributes, &analysis, advances, offsets, &run) != S_OK || advances[0] != by_character ||
		advances[2] != by_character)
		return 51;
	SCRIPT_FONTPROPERTIES properties = {sizeof properties};
	if (ScriptGetFontProperties(dc, &cache, &properties) != S_OK || properties.wgBlank != space || properties.wgDefault != 0)
		return 52;
	clear();
	if (ScriptTextOut(dc, &cache, 4, 4, 0, NULL, &analysis, NULL, 0, shaped, 1, advances, NULL, offsets) != S_OK)
		return 53;
	GdiFlush();
	const int script_ink = ink();
	if (script_ink * 10 < character_ink * 9 || script_ink * 10 > character_ink * 11)
		return 54;

	// A surrogate pair is one glyph, which both of its units' clusters name, and positions go through the clusters.
	WORD pair_shaped[4] = {0}, pair_clusters[4] = {0};
	int pair_count = 0, pair_advances[4] = {0}, x = 0, position = 0, trailing = 0;
	if (ScriptShape(dc, &cache, L"A\xd83d\xde00" L"A", 4, 4, &analysis, pair_shaped, pair_clusters, attributes, &pair_count) != S_OK ||
		pair_count != 3 || pair_shaped[0] != expected || pair_shaped[2] != expected || pair_clusters[0] != 0 || pair_clusters[1] != 1 ||
		pair_clusters[2] != 1 || pair_clusters[3] != 2)
		return 55;
	if (ScriptPlace(dc, &cache, pair_shaped, pair_count, attributes, &analysis, pair_advances, offsets, &run) != S_OK || pair_advances[1] <= 0)
		return 56;
	if (ScriptCPtoX(3, FALSE, 4, pair_count, pair_clusters, attributes, pair_advances, &analysis, &x) != S_OK ||
		x != pair_advances[0] + pair_advances[1])
		return 57;
	if (ScriptXtoCP(pair_advances[0] + 1, 4, pair_count, pair_clusters, attributes, pair_advances, &analysis, &position, &trailing) != S_OK ||
		position != 1 || trailing != 0)
		return 58;

	// A character the face lacks is drawn as text output draws it, through the fallback faces, not as the face's missing glyph.
	if (!cmap_glyph(size, 0x2603)) {
		WORD lacking = 0, lacking_cluster = 0;
		int lacking_count = 0, lacking_advance = 0;
		clear();
		ExtTextOutW(dc, 4, 4, 0, NULL, L"\x2603", 1, NULL);
		GdiFlush();
		const int text_ink = ink();
		if (ScriptShape(dc, &cache, L"\x2603", 1, 1, &analysis, &lacking, &lacking_cluster, attributes, &lacking_count) != S_OK ||
			lacking_count != 1 || !lacking)
			return 59;
		if (ScriptPlace(dc, &cache, &lacking, 1, attributes, &analysis, &lacking_advance, offsets, &run) != S_OK)
			return 64;
		clear();
		if (ScriptTextOut(dc, &cache, 4, 4, 0, NULL, &analysis, NULL, 0, &lacking, 1, &lacking_advance, NULL, offsets) != S_OK)
			return 65;
		GdiFlush();
		if (ink() != text_ink)
			return 66;
	}
	ScriptFreeCache(&cache);

	// A font as large as the em square some programs create to read design metrics measures from the outline.
	HFONT large = arial(-65535);
	if (!large || !SelectObject(dc, large))
		return 60;
	GLYPHMETRICS large_metrics = {0};
	if (GetGlyphOutlineW(dc, expected, GGO_METRICS | GGO_GLYPH_INDEX, &large_metrics, 0, NULL, &identity) == GDI_ERROR)
		return 61;
	if (large_metrics.gmBlackBoxX < 20000 || large_metrics.gmBlackBoxY < 20000)
		return 62;
	INT large_width = 0;
	if (!GetCharWidthI(dc, 0, 1, &glyph, &large_width) || large_width < 20000)
		return 63;

	SelectObject(dc, GetStockObject(SYSTEM_FONT));
	DeleteObject(large);
	DeleteObject(font);
	DeleteObject(bitmap);
	DeleteDC(dc);
	return 0;
}
