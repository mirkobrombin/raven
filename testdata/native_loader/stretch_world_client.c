#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// StretchDIBits draws its destination rectangle through the DC's world transform, as every other drawing call does: a program that
// scales its page for printing or zoom puts the image where it scaled the rest.
int raven_entry(void)
{
	BITMAPINFO info={0};
	DWORD *pixels=NULL;
	const DWORD red[4]={0xff0000,0xff0000,0xff0000,0xff0000};
	XFORM scale={2,0,0,3,10,4};
	HDC dc=CreateCompatibleDC(0);
	if (!dc) return 1;
	info.bmiHeader.biSize=sizeof(info.bmiHeader);
	info.bmiHeader.biWidth=40;
	info.bmiHeader.biHeight=-40;
	info.bmiHeader.biPlanes=1;
	info.bmiHeader.biBitCount=32;
	HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,(void **)&pixels,NULL,0);
	if (!bitmap || !pixels) return 2;
	SelectObject(dc,bitmap);
	info.bmiHeader.biWidth=2;
	info.bmiHeader.biHeight=-2;
	if (!SetGraphicsMode(dc,GM_ADVANCED) || !SetWorldTransform(dc,&scale)) return 3;
	// Logical (1,1)-(5,4) lands on device (12,7)-(20,16).
	if (!StretchDIBits(dc,1,1,4,3,0,0,2,2,red,&info,DIB_RGB_COLORS,SRCCOPY)) return 4;
	GdiFlush();
	if ((pixels[7*40+12]&0xffffff)!=0xff0000 || (pixels[15*40+19]&0xffffff)!=0xff0000) return 5;
	if (pixels[6*40+12] || pixels[7*40+11] || pixels[16*40+19] || pixels[15*40+20]) return 6;
	if (pixels[1*40+1]) return 7;
	return 0;
}
