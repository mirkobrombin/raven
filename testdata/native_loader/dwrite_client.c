/* Exercises DirectWrite through its public interfaces: the system fonts, a face, glyph metrics, an outline and a rendered glyph run. */
#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite_2.h>

static int outline_segments, outline_figures;

static HRESULT STDMETHODCALLTYPE sink_query(IUnknown *self, REFIID iid, void **out) { *out = self; return S_OK; }
static ULONG STDMETHODCALLTYPE sink_ref(IUnknown *self) { return 1; }
static void STDMETHODCALLTYPE sink_fill(ID2D1SimplifiedGeometrySink *self, D2D1_FILL_MODE mode) {}
static void STDMETHODCALLTYPE sink_flags(ID2D1SimplifiedGeometrySink *self, D2D1_PATH_SEGMENT flags) {}
static void STDMETHODCALLTYPE sink_begin(ID2D1SimplifiedGeometrySink *self, D2D1_POINT_2F start, D2D1_FIGURE_BEGIN begin) { ++outline_figures; }
static void STDMETHODCALLTYPE sink_lines(ID2D1SimplifiedGeometrySink *self, const D2D1_POINT_2F *points, UINT32 count) { outline_segments += (int)count; }
static void STDMETHODCALLTYPE sink_beziers(ID2D1SimplifiedGeometrySink *self, const D2D1_BEZIER_SEGMENT *segments, UINT32 count) { outline_segments += (int)count; }
static void STDMETHODCALLTYPE sink_end(ID2D1SimplifiedGeometrySink *self, D2D1_FIGURE_END end) {}
static HRESULT STDMETHODCALLTYPE sink_close(ID2D1SimplifiedGeometrySink *self) { return S_OK; }

static ID2D1SimplifiedGeometrySinkVtbl sink_vtbl = {
	{sink_query, sink_ref, sink_ref}, sink_fill, sink_flags, sink_begin, sink_lines, sink_beziers, sink_end, sink_close,
};

int raven_entry(void)
{
	IDWriteFactory *factory = NULL;
	IDWriteFactory2 *factory2 = NULL;
	IDWriteFontCollection *collection = NULL;
	IDWriteFontFamily *family = NULL;
	IDWriteFont *font = NULL;
	IDWriteFontFace *face = NULL;
	IDWriteLocalizedStrings *names = NULL;
	IDWriteGlyphRunAnalysis *analysis = NULL;
	DWRITE_FONT_METRICS metrics;
	DWRITE_GLYPH_METRICS glyph_metrics[2];
	DWRITE_GLYPH_RUN run;
	UINT32 index, codepoints[2] = {'A', 'g'}, count;
	UINT16 glyphs[2];
	WINBOOL exists;
	RECT bounds;
	static BYTE texture[64 * 64 * 3];
	FLOAT advances[2] = {12.0f, 10.0f};
	WCHAR name[64];
	int lit = 0, position;
	ID2D1SimplifiedGeometrySink sink = {&sink_vtbl};

	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&factory)) || !factory)
		return 1;
	if (FAILED(IDWriteFactory_QueryInterface(factory, &IID_IDWriteFactory2, (void **)&factory2)))
		return 2;
	IDWriteFactory2_Release(factory2);
	if (FAILED(IDWriteFactory_GetSystemFontCollection(factory, &collection, FALSE)) || IDWriteFontCollection_GetFontFamilyCount(collection) == 0)
		return 3;
	if (FAILED(IDWriteFontCollection_FindFamilyName(collection, L"Arial", &index, &exists)) || !exists)
		return 4;
	if (FAILED(IDWriteFontCollection_GetFontFamily(collection, index, &family)))
		return 5;
	if (FAILED(IDWriteFontFamily_GetFamilyNames(family, &names)) || FAILED(IDWriteLocalizedStrings_GetString(names, 0, name, 64)) || name[0] != 'A' || name[1] != 'r')
		return 6;
	if (FAILED(IDWriteFontFamily_GetFirstMatchingFont(family, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
		return 7;
	if (IDWriteFont_GetWeight(font) != DWRITE_FONT_WEIGHT_REGULAR)
		return 8;
	if (FAILED(IDWriteFont_CreateFontFace(font, &face)))
		return 9;
	IDWriteFontFace_GetMetrics(face, &metrics);
	if (metrics.designUnitsPerEm < 256 || metrics.ascent == 0 || metrics.descent == 0)
		return 10;
	if (FAILED(IDWriteFontFace_GetGlyphIndices(face, codepoints, 2, glyphs)) || glyphs[0] == 0 || glyphs[1] == 0 || glyphs[0] == glyphs[1])
		return 11;
	if (FAILED(IDWriteFontFace_GetDesignGlyphMetrics(face, glyphs, 2, glyph_metrics, FALSE)) || glyph_metrics[0].advanceWidth == 0 ||
		glyph_metrics[0].advanceWidth > metrics.designUnitsPerEm * 2)
		return 12;
	{
		const void *table;
		UINT32 size;
		void *context;
		if (FAILED(IDWriteFontFace_TryGetFontTable(face, DWRITE_MAKE_OPENTYPE_TAG('c', 'm', 'a', 'p'), &table, &size, &context, &exists)) || !exists || size < 4)
			return 13;
		IDWriteFontFace_ReleaseFontTable(face, context);
	}
	if (FAILED(IDWriteFontFace_GetGlyphRunOutline(face, 20.0f, glyphs, advances, NULL, 2, FALSE, FALSE, &sink)) || outline_figures < 2 || outline_segments < 8)
		return 14;
	run.fontFace = face;
	run.fontEmSize = 20.0f;
	run.glyphCount = 2;
	run.glyphIndices = glyphs;
	run.glyphAdvances = advances;
	run.glyphOffsets = NULL;
	run.isSideways = FALSE;
	run.bidiLevel = 0;
	if (FAILED(IDWriteFactory_CreateGlyphRunAnalysis(factory, &run, 1.0f, NULL, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL, 4.0f, 30.0f, &analysis)))
		return 15;
	if (FAILED(IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds)) ||
		bounds.right - bounds.left < 8 || bounds.bottom - bounds.top < 8 || bounds.right - bounds.left > 40 || bounds.bottom - bounds.top > 40 || bounds.top > 30 || bounds.bottom < 30)
		return 16;
	if (FAILED(IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis, DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, texture, sizeof(texture))))
		return 17;
	for (position = 0; position < (bounds.right - bounds.left) * (bounds.bottom - bounds.top) * 3; ++position)
		if (texture[position])
			++lit;
	if (lit < 40)
		return 18;
	{
		/* A text format and an empty layout made from it answer with the settings they were given. */
		IDWriteTextFormat *format;
		IDWriteTextLayout *layout;
		if (FAILED(IDWriteFactory_CreateTextFormat(factory, L"Arial", NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STRETCH_NORMAL,
			14.0f, L"en-us", &format)))
			return 19;
		if (IDWriteTextFormat_GetFontSize(format) != 14.0f || IDWriteTextFormat_GetFontWeight(format) != DWRITE_FONT_WEIGHT_BOLD ||
			IDWriteTextFormat_GetFontStyle(format) != DWRITE_FONT_STYLE_ITALIC)
			return 20;
		if (FAILED(IDWriteFactory_CreateTextLayout(factory, L"", 0, format, 100.0f, 20.0f, &layout)) ||
			IDWriteTextFormat_GetFontSize((IDWriteTextFormat *)layout) != 14.0f || IDWriteTextFormat_GetFontWeight((IDWriteTextFormat *)layout) != DWRITE_FONT_WEIGHT_BOLD)
			return 21;
		IDWriteTextLayout_Release(layout);
		IDWriteTextFormat_Release(format);
	}
	IDWriteGlyphRunAnalysis_Release(analysis);
	IDWriteFontFace_Release(face);
	IDWriteFont_Release(font);
	IDWriteLocalizedStrings_Release(names);
	IDWriteFontFamily_Release(family);
	count = IDWriteFactory_Release(factory);
	(void)count;
	return 0;
}
