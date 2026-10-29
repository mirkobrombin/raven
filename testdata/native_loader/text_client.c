#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include "text_reference.h"

static const WCHAR *faces[]={L"Liberation Sans",L"Liberation Mono",L"Liberation Serif"};
static const WCHAR *samples[]={L"Raven Ag09",L"iiiiiiii",L"WWWWWWWW",L"AVATAR",L"Hello, world!",L"0123456789",L"Quick fox",L"\x03a9\x00e9"};
static const int sizes[]={8,9,10,11,12,13,14,16,18,20,24,32,48};
__declspec(dllexport) volatile BYTE text_pixels[390*512*64*4];
__declspec(dllexport) volatile unsigned text_frames;

static int length(const WCHAR *text)
{
	int count=0; while(text[count]) ++count; return count;
}

static BOOL snapshot(const BYTE *pixels)
{
	if(text_frames>=390) return FALSE;
	unsigned long long hash=14695981039346656037ULL;
	for(unsigned i=0;i<512*64*4;++i) {
		hash=(hash^pixels[i])*1099511628211ULL;
		text_pixels[text_frames*512*64*4+i]=pixels[i];
	}
	if(hash!=text_hashes[text_frames]) return FALSE;
	++text_frames; return TRUE;
}

int raven_entry(void)
{
	HDC dc=CreateCompatibleDC(NULL); if(!dc) return 1;
	BITMAPINFO info={0}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=512;
	info.bmiHeader.biHeight=-64; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
	void *storage=NULL; HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&storage,NULL,0);
	if(!bitmap || !storage) return 2;
	HGDIOBJ previous_bitmap=SelectObject(dc,bitmap); if(!previous_bitmap) return 3;
	BYTE *pixels=storage;
	for(unsigned family=0;family<3;++family) for(unsigned size=0;size<13;++size) {
		LOGFONTW definition={0}; definition.lfHeight=-sizes[size]; definition.lfWeight=400; definition.lfQuality=NONANTIALIASED_QUALITY;
		for(unsigned i=0;i<=((unsigned)length(faces[family]));++i) definition.lfFaceName[i]=faces[family][i];
		HFONT font=CreateFontIndirectW(&definition); if(!font) return 4;
		HGDIOBJ previous_font=SelectObject(dc,font); TEXTMETRICW metrics;
		if(!previous_font || !GetTextMetricsW(dc,&metrics)) return 5;
		const LONG *expected=text_metrics[family*13+size];
		if(metrics.tmHeight!=expected[0] || metrics.tmAscent!=expected[1] || metrics.tmDescent!=expected[2] ||
			metrics.tmInternalLeading!=expected[3] || metrics.tmExternalLeading!=expected[4] ||
			metrics.tmAveCharWidth!=expected[5] || metrics.tmMaxCharWidth!=expected[6]) return 6;
		if(metrics.tmWeight!=400 || metrics.tmOverhang || metrics.tmDigitizedAspectX!=96 || metrics.tmDigitizedAspectY!=96 ||
			metrics.tmFirstChar!=32 || metrics.tmLastChar!=(family==1 ? 65059 : 65532) || metrics.tmDefaultChar!=31 || metrics.tmBreakChar!=32 ||
			metrics.tmItalic || metrics.tmUnderlined || metrics.tmStruckOut || metrics.tmPitchAndFamily!=(family==0 ? 39 : family==1 ? 54 : 23) || metrics.tmCharSet) return 7;
		if(SetTextColor(dc,RGB(0,0,0))==CLR_INVALID || SetBkColor(dc,RGB(255,255,255))==CLR_INVALID || !SetBkMode(dc,OPAQUE)) return 8;
		for(unsigned sample=0;sample<8;++sample) {
			SIZE extent; int count=length(samples[sample]);
			if(!GetTextExtentPoint32W(dc,samples[sample],count,&extent) || extent.cx!=text_extents[family*13+size][sample] || extent.cy!=metrics.tmHeight) return 9;
			for(unsigned i=0;i<512*64;++i) { pixels[i*4]=255; pixels[i*4+1]=255; pixels[i*4+2]=255; pixels[i*4+3]=255; }
			if(!TextOutW(dc,0,0,samples[sample],count) || !snapshot(pixels)) return 10;
		}
		for(unsigned mode=1;mode<=2;++mode) {
			for(unsigned i=0;i<512*64;++i) { pixels[i*4]=17; pixels[i*4+1]=203; pixels[i*4+2]=91; pixels[i*4+3]=119; }
			if(!SetBkMode(dc,(int)mode) || SetBkColor(dc,RGB(220,75,33))==CLR_INVALID || SetTextColor(dc,RGB(41,111,231))==CLR_INVALID ||
				!TextOutW(dc,7,5,samples[0],length(samples[0])) || !snapshot(pixels)) return 11;
		}
		SelectObject(dc,previous_font); if(!DeleteObject(font)) return 12;
	}
#ifdef RAVEN_TEXT_BOUNDARY
	LOGFONTW definition={0}; definition.lfHeight=-16; definition.lfWeight=400; definition.lfQuality=NONANTIALIASED_QUALITY;
	for(unsigned i=0;i<16;++i) definition.lfFaceName[i]=faces[0][i];
	HFONT font=CreateFontIndirectW(&definition); HGDIOBJ previous=SelectObject(dc,font); if(!font || !previous) return 13;
	SIZE extent={123,456}; WCHAR missing=0xffff; SetLastError(0);
	if(!GetTextExtentPoint32W(dc,&missing,1,&extent) || extent.cx<=0 || extent.cy<=0) return 14;
	SetLastError(0); if(SetBkMode(dc,3) || GetLastError()!=ERROR_INVALID_PARAMETER) return 15;
	SetLastError(0); if(SetTextColor(dc,0x01000004)!=CLR_INVALID || GetLastError()!=ERROR_CALL_NOT_IMPLEMENTED) return 16;
	SetLastError(0); if(!TextOutW(dc,0,0,samples[0],4) || TextOutW(dc,0,0,samples[0],-1)) return 17;
	WCHAR combining[]={L'e',0x0301}; SetLastError(0);
	if(!TextOutW(dc,0,0,combining,2)) return 19;
	SelectObject(dc,previous); DeleteObject(font);
	/* Uniscribe describes the one script it itemizes and keeps the table; US English digits are not substituted. */
	HMODULE usp=LoadLibraryW(L"usp10.dll"); if(!usp) return 20;
	typedef HRESULT (WINAPI *properties_function)(const void ***,int *);
	typedef HRESULT (WINAPI *digits_function)(LCID,DWORD *);
	properties_function properties=(properties_function)GetProcAddress(usp,"ScriptGetProperties");
	digits_function digits=(digits_function)GetProcAddress(usp,"ScriptRecordDigitSubstitution");
	const void **table=NULL,**again=NULL; int scripts=0; DWORD substitute[3]={7,7,7};
	if(!properties || !digits || properties(&table,&scripts)!=S_OK || !table || !table[0] || scripts<1) return 21;
	if(properties(&again,NULL)!=S_OK || again!=table || ((const BYTE *)table[0])[2]&1) return 22;
	if(digits(0x409,substitute)!=S_OK || substitute[0]!=(9|9<<16) || substitute[1]!=1 || digits(0x409,NULL)!=E_INVALIDARG) return 23;
#endif
	SelectObject(dc,previous_bitmap); if(!DeleteObject(bitmap) || !DeleteDC(dc) || text_frames!=390) return 18;
	return 0;
}
