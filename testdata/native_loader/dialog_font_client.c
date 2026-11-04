// The dialog faces Tahoma and MS Shell Dlg 2 keep their name and get a cell as tall as Tahoma's: 13 pixels at eight points.
#include <windows.h>

static int measure(HDC dc, const wchar_t *name, int weight, TEXTMETRICW *metrics, SIZE *extent, wchar_t *face)
{
	LOGFONTW definition = {0};
	definition.lfHeight = -11;
	definition.lfWeight = weight;
	lstrcpyW(definition.lfFaceName, name);
	HFONT font = CreateFontIndirectW(&definition);
	if (!font)
		return 0;
	HGDIOBJ old = SelectObject(dc, font);
	const int done = GetTextMetricsW(dc, metrics) && GetTextExtentPoint32W(dc, L"Release notes", 13, extent) && GetTextFaceW(dc, LF_FACESIZE, face);
	SelectObject(dc, old);
	DeleteObject(font);
	return done;
}

int raven_entry(void)
{
	HDC dc = CreateCompatibleDC(NULL);
	if (!dc)
		return 1;
	TEXTMETRICW regular, bold, shell, arial;
	SIZE regular_extent, bold_extent, shell_extent, arial_extent;
	wchar_t face[LF_FACESIZE];
	if (!measure(dc, L"Tahoma", FW_NORMAL, &regular, &regular_extent, face) || lstrcmpW(face, L"Tahoma"))
		return 2;
	if (regular.tmHeight != 13 || regular.tmAscent != 11 || regular.tmDescent != 2 || regular_extent.cy != 13)
		return 3;
	if (!measure(dc, L"Tahoma", FW_BOLD, &bold, &bold_extent, face) || bold.tmHeight != 13 || bold_extent.cx <= regular_extent.cx)
		return 4;
	if (!measure(dc, L"MS Shell Dlg 2", FW_NORMAL, &shell, &shell_extent, face) || shell.tmHeight != 13 || shell_extent.cx != regular_extent.cx)
		return 5;
	// Arial keeps its own face, whose cell is a pixel shorter.
	if (!measure(dc, L"Arial", FW_NORMAL, &arial, &arial_extent, face) || arial.tmHeight != 12)
		return 6;
	DeleteDC(dc);
	return 0;
}
