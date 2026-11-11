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
	return 0;
}
