/* Exercises DirectWrite through its public interfaces: the system fonts, a face, glyph metrics, an outline and a rendered glyph run. */
#define INITGUID
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>

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
	{
		/* A face found under Arial goes by Arial in its Win32 family name, whichever host font stands for it. */
		IDWriteLocalizedStrings *win32 = NULL;
		if (FAILED(IDWriteFont_GetInformationalStrings(font, DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES, &win32, &exists)) || !exists ||
			FAILED(IDWriteLocalizedStrings_GetString(win32, 0, name, 64)) || lstrcmpW(name, L"Arial"))
			return 22;
		IDWriteLocalizedStrings_Release(win32);
	}
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
		{
			/* A format answers for version 1 and hands its settings to the layouts made from it. */
			IDWriteTextFormat1 *format1;
			IDWriteTextLayout1 *layout1;
			IDWriteFontFace2 *face2;
			IDWriteTextLayout *wide;
			DWRITE_TEXT_METRICS plain, spaced;
			FLOAT leading, trailing, minimum;
			DWRITE_TEXT_RANGE range = {0, 2};
			if (FAILED(IDWriteTextFormat_QueryInterface(format, &IID_IDWriteTextFormat1, (void **)&format1)))
				return 28;
			if (FAILED(IDWriteTextFormat1_SetLastLineWrapping(format1, FALSE)) || IDWriteTextFormat1_GetLastLineWrapping(format1))
				return 29;
			IDWriteTextFormat1_Release(format1);
			if (FAILED(IDWriteFactory_CreateTextLayout(factory, L"ab", 2, format, 400.0f, 40.0f, &wide)) ||
				FAILED(IDWriteTextLayout_GetMetrics(wide, &plain)))
				return 30;
			if (FAILED(IDWriteTextLayout_QueryInterface(wide, &IID_IDWriteTextLayout1, (void **)&layout1)))
				return 31;
			if (FAILED(IDWriteTextLayout1_SetCharacterSpacing(layout1, 1.0f, 2.0f, 0.0f, range)) ||
				FAILED(IDWriteTextLayout1_GetCharacterSpacing(layout1, 0, &leading, &trailing, &minimum, NULL)) ||
				leading != 1.0f || trailing != 2.0f || minimum != 0.0f)
				return 32;
			if (FAILED(IDWriteTextLayout_GetMetrics(wide, &spaced)) || spaced.width < plain.width + 5.0f)
				return 33;
			IDWriteTextLayout1_Release(layout1);
			IDWriteTextLayout_Release(wide);
			if (FAILED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace2, (void **)&face2)))
				return 34;
			if (IDWriteFontFace2_IsColorFont(face2) || IDWriteFontFace2_GetColorPaletteCount(face2))
				return 35;
			IDWriteFontFace2_Release(face2);
		}
		{
			/* No text still takes a line of the format's font, with no room or plenty. */
			DWRITE_TEXT_METRICS empty;
			IDWriteTextLayout *none;
			if (FAILED(IDWriteTextLayout_GetMetrics(layout, &empty)) || empty.height < 10.0f || empty.width != 0.0f)
				return 36;
			if (FAILED(IDWriteFactory_CreateTextLayout(factory, L"", 0, format, 0.0f, 0.0f, &none)) ||
				FAILED(IDWriteTextLayout_GetMetrics(none, &empty)) || empty.height < 10.0f)
				return 37;
			IDWriteTextLayout_Release(none);
		}
		IDWriteTextLayout_Release(layout);
		IDWriteTextFormat_Release(format);
	}
	{
		/* The analyzer answers for its later versions, and one of their methods places a rotated glyph. */
		IDWriteTextAnalyzer *analyzer;
		IDWriteTextAnalyzer1 *analyzer1;
		IDWriteTextAnalyzer2 *analyzer2;
		DWRITE_MATRIX turned;
		if (FAILED(IDWriteFactory_CreateTextAnalyzer(factory, &analyzer)))
			return 22;
		if (FAILED(IDWriteTextAnalyzer_QueryInterface(analyzer, &IID_IDWriteTextAnalyzer1, (void **)&analyzer1)))
			return 23;
		if (FAILED(IDWriteTextAnalyzer_QueryInterface(analyzer, &IID_IDWriteTextAnalyzer2, (void **)&analyzer2)))
			return 24;
		if (FAILED(IDWriteTextAnalyzer1_GetGlyphOrientationTransform(analyzer1, DWRITE_GLYPH_ORIENTATION_ANGLE_90_DEGREES, FALSE, &turned)) ||
			turned.m11 != 0.0f || turned.m12 != 1.0f || turned.m21 != -1.0f || turned.m22 != 0.0f)
			return 25;
		if (FAILED(IDWriteTextAnalyzer2_GetGlyphOrientationTransform(analyzer2, DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES, FALSE, 5.0f, 6.0f, &turned)) ||
			turned.m11 != 1.0f || turned.m22 != 1.0f)
			return 26;
		if (IDWriteTextAnalyzer_QueryInterface(analyzer, &IID_IDWriteFactory, (void **)&factory2) != E_NOINTERFACE)
			return 27;
		IDWriteTextAnalyzer2_Release(analyzer2);
		IDWriteTextAnalyzer1_Release(analyzer1);
		IDWriteTextAnalyzer_Release(analyzer);
	}
	{
		/* Tahoma is answered by Raven Sans, whose cell has Tahoma's height on 2048 units. */
		IDWriteFontFamily *dialog_family;
		IDWriteFont *dialog_font;
		IDWriteFontFace *dialog_face;
		DWRITE_FONT_METRICS dialog_metrics;
		if (FAILED(IDWriteFontCollection_FindFamilyName(collection, L"Tahoma", &index, &exists)) || !exists ||
			FAILED(IDWriteFontCollection_GetFontFamily(collection, index, &dialog_family)))
			return 38;
		if (FAILED(IDWriteFontFamily_GetFirstMatchingFont(dialog_family, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &dialog_font)) ||
			FAILED(IDWriteFont_CreateFontFace(dialog_font, &dialog_face)))
			return 39;
		IDWriteFontFace_GetMetrics(dialog_face, &dialog_metrics);
		if (dialog_metrics.designUnitsPerEm != 2048 || dialog_metrics.ascent != 2049 || dialog_metrics.descent != 423)
			return 40;
		IDWriteFontFace_Release(dialog_face);
		IDWriteFont_Release(dialog_font);
		IDWriteFontFamily_Release(dialog_family);
	}
	{
		/* The newest factory, as paint.net asks for it, and its system font set; the headers declare it for C++ alone, and its
		   GetSystemFontSet is the method after the fifty-five of IDWriteFactory6. */
		typedef HRESULT (STDMETHODCALLTYPE *set_function)(void *, WINBOOL, IDWriteFontSet **);
		IUnknown *factory7;
		IDWriteFontSet *set = NULL;
		if (FAILED(IDWriteFactory_QueryInterface(factory, &IID_IDWriteFactory7, (void **)&factory7)))
			return 41;
		if (FAILED((*(set_function **)factory7)[55](factory7, FALSE, &set)) || !set || IDWriteFontSet_GetFontCount(set) == 0)
			return 42;
		IDWriteFontSet_Release(set);
		IUnknown_Release(factory7);
	}
	{
		/* Rendering parameters of the newest kind, with a mode only that kind names, which the older interfaces read as its nearest. */
		IDWriteFactory3 *factory3;
		IDWriteRenderingParams3 *params3;
		IDWriteRenderingParams *params0;
		if (FAILED(IDWriteFactory_QueryInterface(factory, &IID_IDWriteFactory3, (void **)&factory3)) ||
			FAILED(IDWriteFactory3_CreateCustomRenderingParams(factory3, 2.2f, 1.0f, 1.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
				DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED, DWRITE_GRID_FIT_MODE_DEFAULT, &params3)))
			return 43;
		if (IDWriteRenderingParams3_GetRenderingMode1(params3) != DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED ||
			FAILED(IDWriteRenderingParams3_QueryInterface(params3, &IID_IDWriteRenderingParams, (void **)&params0)) ||
			IDWriteRenderingParams_GetRenderingMode(params0) != DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC)
			return 44;
		IDWriteRenderingParams_Release(params0);
		IDWriteRenderingParams3_Release(params3);
		IDWriteFactory3_Release(factory3);
	}
	{
		/* A text format of the newest kind: the line spacing as one structure and font axis values, the methods after the
		   thirty-six of IDWriteTextFormat1, which the headers declare for C++ alone. */
		typedef HRESULT (STDMETHODCALLTYPE *spacing_function)(void *, DWRITE_LINE_SPACING *);
		typedef HRESULT (STDMETHODCALLTYPE *axes_function)(void *, const DWRITE_FONT_AXIS_VALUE *, UINT32);
		typedef UINT32 (STDMETHODCALLTYPE *count_function)(void *);
		IDWriteTextFormat *format;
		void **format3;
		DWRITE_LINE_SPACING spacing = {DWRITE_LINE_SPACING_METHOD_UNIFORM, 20.0f, 16.0f, 0.5f, DWRITE_FONT_LINE_GAP_USAGE_ENABLED}, read = {0};
		DWRITE_FONT_AXIS_VALUE weight = {DWRITE_FONT_AXIS_TAG_WEIGHT, 600.0f};
		if (FAILED(IDWriteFactory_CreateTextFormat(factory, L"Arial", NULL, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &format)) ||
			FAILED(IDWriteTextFormat_QueryInterface(format, &IID_IDWriteTextFormat3, (void **)&format3)))
			return 45;
		if (FAILED(((spacing_function)(*(void ***)format3)[36])(format3, &spacing)) || FAILED(((spacing_function)(*(void ***)format3)[37])(format3, &read)) ||
			read.method != spacing.method || read.height != 20.0f || read.baseline != 16.0f || read.leadingBefore != 0.5f ||
			read.fontLineGapUsage != DWRITE_FONT_LINE_GAP_USAGE_ENABLED)
			return 46;
		if (FAILED(((axes_function)(*(void ***)format3)[38])(format3, &weight, 1)) || ((count_function)(*(void ***)format3)[39])(format3) != 1)
			return 47;
		{
			/* A layout of that format is of the newest kind too and keeps its settings: the spacing, the axis values, and the line
			   metrics with leading, the fourth method of IDWriteTextLayout3 after the eighty of IDWriteTextLayout2. */
			typedef HRESULT (STDMETHODCALLTYPE *lines_function)(void *, DWRITE_LINE_METRICS1 *, UINT32, UINT32 *);
			IDWriteTextLayout *layout;
			void **layout4;
			DWRITE_LINE_METRICS1 lines[2];
			UINT32 line_count = 0;
			if (FAILED(IDWriteFactory_CreateTextLayout(factory, L"Wide", 4, format, 200.0f, 100.0f, &layout)) ||
				FAILED(IDWriteTextLayout_QueryInterface(layout, &IID_IDWriteTextLayout4, (void **)&layout4)))
				return 48;
			if (FAILED(((spacing_function)(*(void ***)layout4)[82])(layout4, &read)) || read.leadingBefore != 0.5f ||
				((count_function)(*(void ***)layout4)[85])(layout4) != 1)
				return 49;
			if (FAILED(((lines_function)(*(void ***)layout4)[83])(layout4, lines, 2, &line_count)) || line_count != 1 || lines[0].length != 4)
				return 50;
			{
				/* The layout is a text format of the newest kind as well, which reads the layout's own settings. */
				void **layout_format;
				if (FAILED(IUnknown_QueryInterface((IUnknown *)layout4, &IID_IDWriteTextFormat3, (void **)&layout_format)) ||
					FAILED(((spacing_function)(*(void ***)layout_format)[37])(layout_format, &read)) || read.leadingBefore != 0.5f ||
					IDWriteTextFormat_GetFontSize((IDWriteTextFormat *)layout_format) != 12.0f)
					return 51;
				IUnknown_Release((IUnknown *)layout_format);
			}
			IUnknown_Release((IUnknown *)layout4);
			IDWriteTextLayout_Release(layout);
		}
		IUnknown_Release((IUnknown *)format3);
		IDWriteTextFormat_Release(format);
	}
	/* The family a font names answers as the newer family of Windows 10: its fonts for axis values as a list, and as a set, as
	   paint.net asks of the font its text tool starts with. */
	IDWriteFontFamily *named = NULL;
	IDWriteFontFamily2 *newer = NULL;
	IDWriteFontList2 *matching = NULL;
	IDWriteFontSet1 *of_family = NULL, *of_list = NULL;
	IDWriteFont *listed = NULL;
	if (FAILED(IDWriteFont_GetFontFamily(font, &named)) || FAILED(IDWriteFontFamily_QueryInterface(named, &IID_IDWriteFontFamily2, (void **)&newer)))
		return 52;
	if (FAILED(IDWriteFontFamily2_GetMatchingFonts(newer, (const DWRITE_FONT_AXIS_VALUE *)NULL, 0, &matching)) || !matching ||
		IDWriteFontList2_GetFontCount(matching) != IDWriteFontFamily_GetFontCount(named) || !IDWriteFontList2_GetFontCount(matching) ||
		FAILED(((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, IDWriteFont **))(*(void ***)matching)[5])(matching, 0, &listed)) || !listed)
		return 53;
	if (FAILED(IDWriteFontFamily2_GetFontSet(newer, &of_family)) || FAILED(IDWriteFontList2_GetFontSet(matching, &of_list)) ||
		IDWriteFontSet1_GetFontCount(of_family) != IDWriteFontFamily_GetFontCount(named) ||
		IDWriteFontSet1_GetFontCount(of_list) != IDWriteFontFamily_GetFontCount(named))
		return 54;
	IDWriteFont_Release(listed);
	IDWriteFontSet1_Release(of_list);
	IDWriteFontSet1_Release(of_family);
	IDWriteFontList2_Release(matching);
	IDWriteFontFamily2_Release(newer);
	IDWriteFontFamily_Release(named);
	IDWriteGlyphRunAnalysis_Release(analysis);
	IDWriteFontFace_Release(face);
	IDWriteFont_Release(font);
	IDWriteLocalizedStrings_Release(names);
	IDWriteFontFamily_Release(family);
	count = IDWriteFactory_Release(factory);
	(void)count;
	return 0;
}
