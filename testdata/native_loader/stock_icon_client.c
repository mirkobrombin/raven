#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

/* SHGetStockIconInfo fills a structure laid out for the caller's pointer width and nothing past it, gives the system icon
 * at the large or small size, and refuses a structure of any other size. */

static int icon_size(HICON icon)
{
	ICONINFO info;
	BITMAP bitmap;
	if (!GetIconInfo(icon, &info) || !GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap))
		return 0;
	return bitmap.bmWidth;
}

int raven_entry(void)
{
	static const WCHAR path[] = L"C:\\Windows\\System32\\user32.dll";
	struct {
		SHSTOCKICONINFO info;
		volatile DWORD guard;
	} slot = {{sizeof(SHSTOCKICONINFO)}, 0x52415645};
	if (SHGetStockIconInfo(SIID_WARNING, 0, &slot.info) != S_OK || slot.guard != 0x52415645 || slot.info.hIcon)
		return 1;
	if (slot.info.iSysImageIndex != -1 || slot.info.iIcon != -32515)
		return 2;
	for (unsigned index = 0; index < sizeof(path) / sizeof(path[0]); ++index)
		if (slot.info.szPath[index] != path[index])
			return 3;
	if (SHGetStockIconInfo(SIID_ERROR, SHGSI_ICON, &slot.info) != S_OK || slot.guard != 0x52415645 || slot.info.iIcon != -32513 ||
		icon_size(slot.info.hIcon) != 32 || !DestroyIcon(slot.info.hIcon))
		return 4;
	if (SHGetStockIconInfo(SIID_HELP, SHGSI_ICON | SHGSI_SMALLICON, &slot.info) != S_OK || slot.info.iIcon != -32514 ||
		icon_size(slot.info.hIcon) != 16 || !DestroyIcon(slot.info.hIcon))
		return 5;
	if (SHGetStockIconInfo(SIID_FOLDER, 0, &slot.info) != S_OK || slot.info.iIcon != -32512)
		return 6;
	slot.info.cbSize = sizeof(SHSTOCKICONINFO) ^ 0x8;
	if (SHGetStockIconInfo(SIID_WARNING, SHGSI_ICON, &slot.info) != E_INVALIDARG || SHGetStockIconInfo(SIID_WARNING, 0, NULL) != E_INVALIDARG)
		return 7;
	return 0;
}
