#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct seen { int italic, wrong; };

static int CALLBACK wide(const LOGFONTW *font,const TEXTMETRICW *metrics,DWORD type,LPARAM data)
{
	struct seen *seen=(struct seen *)data;
	(void)type;
	if (!font->lfItalic) return 1;
	++seen->italic;
	if (font->lfItalic!=255 || metrics->tmItalic!=255) ++seen->wrong;
	return 1;
}

static int CALLBACK narrow(const LOGFONTA *font,const TEXTMETRICA *metrics,DWORD type,LPARAM data)
{
	struct seen *seen=(struct seen *)data;
	(void)type;
	if (!font->lfItalic) return 1;
	++seen->italic;
	if (font->lfItalic!=255 || metrics->tmItalic!=255) ++seen->wrong;
	return 1;
}

// Windows reports an italic face with lfItalic and tmItalic set to 255; programs that compare against TRUE miss it otherwise.
int raven_entry(void)
{
	struct seen seen={0,0};
	LOGFONTW wanted={0};
	LOGFONTA wanted_ansi={0};
	HDC dc=CreateCompatibleDC(0);
	if (!dc) return 1;
	wanted.lfCharSet=DEFAULT_CHARSET;
	EnumFontFamiliesExW(dc,&wanted,(FONTENUMPROCW)wide,(LPARAM)&seen,0);
	if (!seen.italic) return 2;
	if (seen.wrong) return 3;
	seen.italic=0;
	wanted_ansi.lfCharSet=DEFAULT_CHARSET;
	EnumFontFamiliesExA(dc,&wanted_ansi,(FONTENUMPROCA)narrow,(LPARAM)&seen,0);
	if (!seen.italic) return 4;
	if (seen.wrong) return 5;
	return 0;
}
