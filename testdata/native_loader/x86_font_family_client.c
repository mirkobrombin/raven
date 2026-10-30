#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int CALLBACK found(const LOGFONTW *font,const TEXTMETRICW *metrics,DWORD type,LPARAM seen)
{
	(void)metrics;(void)type;
	const WCHAR *wanted=L"Microsoft Sans Serif";
	for (int i=0; wanted[i] || font->lfFaceName[i]; ++i)
		if (wanted[i]!=font->lfFaceName[i]) return 1;
	++*(int *)seen;
	return 0;
}

// A 32-bit program lists a family through its own callback, which is how GDI+ decides whether a family exists.
int raven_entry(void)
{
	int seen=0;
	HDC dc=CreateCompatibleDC(0);
	if (!dc) return 1;
	if (EnumFontFamiliesW(dc,L"Microsoft Sans Serif",(FONTENUMPROCW)found,(LPARAM)&seen)) return 2;
	if (seen!=1) return 3;
	return 0;
}
