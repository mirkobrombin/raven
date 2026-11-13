#include <windows.h>
#include <shellapi.h>

/* A library's icons counted in the order its resources list them: the groups named by a string come first, as 7z.dll's "0" does, and
   the icon at index 0 is that group's. Each index gives a large and a small icon. The library is icons.dll, or the one the command line
   names, as 7-Zip's test names 7z.dll, whose file types show the icons at these indexes. */
int raven_entry(void)
{
	int arguments = 0;
	LPWSTR *words = CommandLineToArgvW(GetCommandLineW(), &arguments);
	const WCHAR *library = words && arguments > 1 ? words[1] : L"icons.dll";
	const UINT count = ExtractIconExW(library, -1, NULL, NULL, 0);
	if (arguments > 1 ? count < 2 : count != 2)
		return 1;
	for (int index = 0; index < 2; ++index) {
		HICON large = NULL, small = NULL;
		ICONINFO info;
		BITMAP bitmap;
		if (ExtractIconExW(library, index, &large, &small, 1) != 1 || !large || !small)
			return 2 + index;
		if (!GetIconInfo(small, &info) || !GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof bitmap, &bitmap) ||
			bitmap.bmWidth != GetSystemMetrics(SM_CXSMICON))
			return 4;
		if (info.hbmColor)
			DeleteObject(info.hbmColor);
		DeleteObject(info.hbmMask);
		DestroyIcon(large);
		DestroyIcon(small);
	}
	/* The shell's default extraction gives both icons at the sizes asked for, large in the low word and small in the high one, as
	   paint.net asks for its setup's icons; an index past the last finds none. */
	typedef HRESULT (WINAPI *default_extract)(LPCWSTR, int, UINT, HICON *, HICON *, UINT);
	default_extract extract = (default_extract)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHDefExtractIconW");
	HICON large = NULL, small = NULL;
	ICONINFO info;
	BITMAP bitmap;
	if (!extract || extract(library, 0, 0, &large, &small, MAKELONG(48, 16)) != S_OK || !large || !small)
		return 5;
	if (!GetIconInfo(large, &info) || !GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof bitmap, &bitmap) || bitmap.bmWidth != 48)
		return 6;
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	DeleteObject(info.hbmMask);
	DestroyIcon(large);
	DestroyIcon(small);
	if (extract(library, 1000, 0, &large, &small, 0) != S_FALSE || large || small)
		return 7;
	/* A stock icon's location is a file and index that the shell's extraction finds, as .NET loads SystemIcons.Error from it. */
	SHSTOCKICONINFO stock = {sizeof stock};
	if (SHGetStockIconInfo(SIID_ERROR, SHGSI_ICONLOCATION, &stock) != S_OK || !stock.szPath[0])
		return 8;
	large = NULL;
	if (extract(stock.szPath, stock.iIcon, 0, &large, NULL, 32) != S_OK || !large)
		return 9;
	if (!GetIconInfo(large, &info) || !GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof bitmap, &bitmap) || bitmap.bmWidth != 32)
		return 10;
	DestroyIcon(large);
	return 0;
}
