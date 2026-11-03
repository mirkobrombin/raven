#include <windows.h>
#include <commctrl.h>
#include "icon_data.h"

__declspec(dllexport) volatile DWORD phase;

static int same(const BYTE *a, const BYTE *b, unsigned count)
{
	for (unsigned index = 0; index < count; ++index)
		if (a[index] != b[index])
			return 0;
	return 1;
}

static int verify(BYTE *data, DWORD size, int width, int height, int mono, const BYTE *mask_bits,
	unsigned mask_size, const BYTE *color_bits, unsigned color_size)
{
	HICON icon = CreateIconFromResourceEx(data, size, TRUE, 0x30000, 0, 0, 0);
	HICON again = CreateIconFromResourceEx(data, size, TRUE, 0x30000, width, height, 0);
	HICON copy = CopyIcon(icon);
	if (!icon || !again || !copy || icon == again || icon == copy)
		return 1;
	ICONINFO info;
	if (!GetIconInfo(icon, &info) || !info.fIcon || info.xHotspot != (DWORD)width / 2 ||
		info.yHotspot != (DWORD)height / 2 || !info.hbmMask || (!!info.hbmColor == mono))
		return 2;
	BITMAP bitmap;
	if (GetObjectW(info.hbmMask, sizeof(bitmap), &bitmap) != sizeof(bitmap) || bitmap.bmType ||
		bitmap.bmWidth != width || bitmap.bmHeight != height * (mono ? 2 : 1) || bitmap.bmPlanes != 1 ||
		bitmap.bmBitsPixel != 1 || bitmap.bmWidthBytes != 2 || bitmap.bmBits || GetObjectA(info.hbmMask, 0, NULL) != sizeof(bitmap))
		return 3;
	BYTE bits[64];
	if (GetBitmapBits(info.hbmMask, sizeof(bits), bits) != (LONG)mask_size || !same(bits, mask_bits, mask_size))
		return 4;
	if (!mono) {
		if (GetObjectA(info.hbmColor, sizeof(bitmap), &bitmap) != sizeof(bitmap) || bitmap.bmWidth != width ||
			bitmap.bmHeight != height || bitmap.bmPlanes != 1 || bitmap.bmBitsPixel != 32 ||
			bitmap.bmWidthBytes != width * 4 || bitmap.bmBits)
			return 5;
		if (GetBitmapBits(info.hbmColor, sizeof(bits), bits) != (LONG)color_size || !same(bits, color_bits, color_size))
			return 6;
	}
	SetLastError(71);
	if (!DestroyIcon(icon) || GetLastError() != 71 || DestroyIcon(icon) || GetLastError() != 71)
		return 7;
	if (GetIconInfo(icon, &(ICONINFO){0}) || GetLastError() != ERROR_INVALID_CURSOR_HANDLE)
		return 8;
	if (CopyIcon(icon) || GetLastError() != ERROR_INVALID_CURSOR_HANDLE)
		return 9;
	if (GetObjectW(info.hbmMask, sizeof(bitmap), &bitmap) != sizeof(bitmap) ||
		GetBitmapBits(info.hbmMask, sizeof(bits), bits) != (LONG)mask_size || !same(bits, mask_bits, mask_size))
		return 10;
	SetLastError(71);
	if (!DeleteObject(info.hbmMask) || GetLastError() != 71 || DeleteObject(info.hbmMask) || GetLastError() != 71)
		return 11;
	if (!mono && !DeleteObject(info.hbmColor))
		return 12;
	if (!GetIconInfo(copy, &info))
		return 13;
	if (!DeleteObject(info.hbmMask) || (!mono && !DeleteObject(info.hbmColor)) || !DestroyIcon(copy) || !DestroyIcon(again))
		return 14;
	HICON shared = CreateIconFromResourceEx(data, size, TRUE, 0x30000, 0, 0, LR_SHARED);
	if (!shared || !DestroyIcon(shared) || !GetIconInfo(shared, &info))
		return 15;
	if (!DeleteObject(info.hbmMask) || (!mono && !DeleteObject(info.hbmColor)))
		return 16;
	return 0;
}

static int verify_scaled(BYTE *data, DWORD size, int mono, const BYTE *mask, const BYTE *color)
{
	HICON icon = CreateIconFromResourceEx(data,size,TRUE,0x30000,2,1,0);
	HICON copy = CopyIcon(icon);
	if (!icon || !copy || !DestroyIcon(icon))
		return 1;
	ICONINFO info;
	BITMAP bitmap;
	BYTE pixels[8];
	if (!GetIconInfo(copy,&info) || info.xHotspot != 1 || info.yHotspot || (!!info.hbmColor == mono) ||
		GetObjectW(info.hbmMask,sizeof(bitmap),&bitmap) != sizeof(bitmap) || bitmap.bmWidth != 2 ||
		bitmap.bmHeight != (mono ? 2 : 1) || bitmap.bmWidthBytes != 2 || bitmap.bmBitsPixel != 1 ||
		GetBitmapBits(info.hbmMask,sizeof(pixels),pixels) != (mono ? 4 : 2) || !same(pixels,mask,mono ? 4 : 2))
		return 2;
	if (!mono && (GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap) != sizeof(bitmap) || bitmap.bmWidth != 2 ||
		bitmap.bmHeight != 1 || bitmap.bmWidthBytes != 8 || bitmap.bmBitsPixel != 32 ||
		GetBitmapBits(info.hbmColor,sizeof(pixels),pixels) != 8 || !same(pixels,color,8)))
		return 3;
	if (!DeleteObject(info.hbmMask) || (!mono && !DeleteObject(info.hbmColor)) || !DestroyIcon(copy))
		return 4;
	return 0;
}

int raven_entry(void)
{
	BYTE group[] = {0,0,1,0,3,0,
		16,16,0,0,1,0,32,0,1,0,0,0,16,0,
		32,32,0,0,1,0,8,0,1,0,0,0,8,0,
		32,32,0,0,1,0,32,0,1,0,0,0,32,0};
	if (LookupIconIdFromDirectoryEx(group, TRUE, 0, 0, 0) != 32)
		return 91;
	SetLastError(71);
	if (LookupIconIdFromDirectoryEx(group, TRUE, 0, 0, 0) != 32 || GetLastError() != 71 ||
		LookupIconIdFromDirectoryEx(group, TRUE, 24, 24, 0) != 16 ||
		LookupIconIdFromDirectoryEx(group, TRUE, 25, 25, 0) != 32 ||
		LookupIconIdFromDirectoryEx(group, TRUE, 24, 24, LR_MONOCHROME) != 8 ||
		LookupIconIdFromDirectoryEx(group, TRUE, 32, 32, LR_MONOCHROME) != 8 || GetLastError() != 71)
		return 90;
	const BYTE color_mask[] = {0x40, 0, 0xa0, 0};
	const BYTE mono_mask[] = {0x30, 0, 0xc0, 0, 0x50, 0, 0xa0, 0};
	const BYTE png_mask[] = {0x60, 0, 0xe0, 0};
	const BYTE color_bits[] = {0, 0, 255, 0, 0, 255, 0, 128, 255, 0, 0, 255,
		247, 99, 8, 1, 99, 8, 247, 254, 8, 247, 99, 0};
	const BYTE png_bits[] = {0, 0, 255, 255, 0, 255, 0, 128, 255, 0, 0, 0,
		64, 16, 9, 3, 3, 2, 1, 254, 255, 255, 255, 1};
	const BYTE black_bits[32] = {0};
	int result = verify(color_icon, sizeof(color_icon), 3, 2, 0, color_mask, sizeof(color_mask), color_bits, sizeof(color_bits));
	if (result)
		return result;
	phase = 1;
	result = verify(mono_icon, sizeof(mono_icon), 4, 2, 1, mono_mask, sizeof(mono_mask), NULL, 0);
	if (result)
		return 20 + result;
	phase = 2;
	result = verify(black_palette_icon, sizeof(black_palette_icon), 4, 2, 0, mono_mask, 4, black_bits, sizeof(black_bits));
	if (result)
		return 40 + result;
	phase = 3;
	result = verify(png_icon, sizeof(png_icon), 3, 2, 0, png_mask, sizeof(png_mask), png_bits, sizeof(png_bits));
	if (result)
		return 60 + result;
	phase = 4;
	const BYTE scaled_color[] = {0,0,0,0,8,0,0,0};
	const BYTE scaled_png[] = {0,0,0,0,255,0,0,0};
	const BYTE scaled_mask[] = {0,0,0,0};
	const BYTE scaled_png_mask[] = {0x40,0};
	const BYTE scaled_black[8] = {0};
	if (verify_scaled(color_icon,sizeof(color_icon),0,scaled_mask,scaled_color) ||
		verify_scaled(mono_icon,sizeof(mono_icon),1,scaled_mask,NULL) ||
		verify_scaled(png_icon,sizeof(png_icon),0,scaled_png_mask,scaled_png) ||
		verify_scaled(black_palette_icon,sizeof(black_palette_icon),0,scaled_mask,scaled_black))
		return 92;
	HMODULE module = LoadLibraryW(L"icons.dll");
	if (!module)
		return 81;
	BYTE *(*data)(void) = (BYTE *(*)(void))GetProcAddress(module, "icon_data");
	DWORD (*size)(void) = (DWORD (*)(void))GetProcAddress(module, "icon_size");
	if (!data || !size)
		return 82;
	HICON owned = CreateIconFromResourceEx(data(), size(), TRUE, 0x30000, 2, 1, 0);
	HICON copy = CopyIcon(owned);
	if (!owned || !copy || !FreeLibrary(module))
		return 83;
	for (unsigned index = 0; index < 2; ++index) {
		ICONINFO info;
		BYTE bits[8];
		HICON icon = index ? copy : owned;
		if (!GetIconInfo(icon, &info) || GetBitmapBits(info.hbmColor, sizeof(bits), bits) != sizeof(bits) ||
			!same(bits, scaled_color, sizeof(bits)) || !DeleteObject(info.hbmMask) || !DeleteObject(info.hbmColor) || !DestroyIcon(icon))
			return 84;
	}
	phase = 5;
	HMODULE controls = GetModuleHandleW(L"comctl32.dll");
	if (!controls || GetProcAddress(controls,(LPCSTR)381) != (FARPROC)LoadIconWithScaleDown)
		return 93;
	module = LoadLibraryW(L"icons.dll");
	HICON loaded = NULL, named = NULL;
	/* LoadIcon gives the display's icon size, whichever image the group lists first. */
	{
		HICON loaded = LoadIconW(module, MAKEINTRESOURCEW(201));
		ICONINFO info;
		BITMAP bitmap;
		if (!loaded || !GetIconInfo(loaded, &info) || !GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap))
			return 98;
		if (bitmap.bmWidth != GetSystemMetrics(SM_CXICON))
			return 99;
		if (info.hbmColor)
			DeleteObject(info.hbmColor);
		DeleteObject(info.hbmMask);
		DestroyIcon(loaded);
	}
	if (!module || LoadIconWithScaleDown(module,MAKEINTRESOURCEW(201),16,16,&loaded) != S_OK || !loaded ||
		LoadIconWithScaleDown(module,L"named",16,16,&named) != S_OK || !named || named == loaded || !FreeLibrary(module))
		return 94;
	for (unsigned index = 0; index < 2; ++index) {
		ICONINFO info;
		BITMAP bitmap;
		BYTE pixels[1024];
		HICON icon = index ? named : loaded;
		if (!GetIconInfo(icon,&info) || GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap) != sizeof(bitmap) ||
			bitmap.bmWidth != 16 || bitmap.bmHeight != 16 || GetBitmapBits(info.hbmColor,sizeof(pixels),pixels) != sizeof(pixels))
			return 95;
		for (unsigned pixel = 0; pixel < 256; ++pixel)
			if (pixels[pixel*4] != 32 || pixels[pixel*4+1] != 200 || pixels[pixel*4+2] != 16 || pixels[pixel*4+3] != 255)
				return 96;
		if (!DeleteObject(info.hbmMask) || !DeleteObject(info.hbmColor) || !DestroyIcon(icon))
			return 97;
	}
	phase = 6;
	return 0;
}
