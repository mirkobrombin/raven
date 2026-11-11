#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <commctrl.h>

#define CHECK(code, condition) do { if (!(condition)) return code; } while (0)

static int same(const WCHAR *left, const WCHAR *right)
{
	unsigned i = 0;
	for (; left[i] || right[i]; ++i) if (left[i] != right[i]) return 0;
	return 1;
}

static void join(WCHAR *out, const WCHAR *left, const WCHAR *right)
{
	unsigned i = 0, j = 0;
	while (left[i]) { out[i] = left[i]; ++i; }
	while (right[j]) out[i++] = right[j++];
	out[i] = 0;
}

/* A 16 by 16 icon file of one colour. */
static BOOL write_icon(const WCHAR *path)
{
	static BYTE bytes[6 + 16 + 40 + 16 * 16 * 4 + 16 * 4];
	DWORD *pixels = (DWORD *)(bytes + 6 + 16 + 40), written = 0;
	BITMAPINFOHEADER *header = (BITMAPINFOHEADER *)(bytes + 22);
	unsigned i;
	bytes[2] = 1; bytes[4] = 1;
	bytes[6] = 16; bytes[7] = 16; bytes[10] = 1; bytes[12] = 32;
	*(DWORD *)(bytes + 14) = sizeof bytes - 22;
	*(DWORD *)(bytes + 18) = 22;
	header->biSize = 40; header->biWidth = 16; header->biHeight = 32; header->biPlanes = 1; header->biBitCount = 32;
	for (i = 0; i < 256; ++i)
		pixels[i] = 0xff2080c0;
	HANDLE file = CreateFileW(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	BOOL done = WriteFile(file,bytes,sizeof bytes,&written,0) && written == sizeof bytes;
	return CloseHandle(file) && done;
}

int raven_entry(void)
{
	WCHAR text[64];
	CHECK(1, GetNumberFormatW(LOCALE_USER_DEFAULT,0,L"1234567.891",0,text,64) == 13 && same(text,L"1,234,567.89"));
	CHECK(2, GetNumberFormatW(LOCALE_USER_DEFAULT,0,L"-0.005",0,text,64) && same(text,L"-0.01"));
	CHECK(3, GetNumberFormatW(LOCALE_USER_DEFAULT,0,L"999.999",0,0,0) == 9);
	NUMBERFMTW plain = {0,1,3,L".",L",",1};
	CHECK(4, GetNumberFormatW(LOCALE_USER_DEFAULT,0,L"300000",&plain,text,64) && same(text,L"300,000"));
	NUMBERFMTW indian = {1,0,32,L",",L".",0};
	CHECK(5, GetNumberFormatW(LOCALE_USER_DEFAULT,0,L"-12345678.25",&indian,text,64) && same(text,L"(1.23.45.678,3)"));
	CHECK(6, GetNumberFormatW(LOCALE_USER_DEFAULT,0,L"0.5",&indian,text,64) && same(text,L",5"));
	SetLastError(0);
	CHECK(7, !GetNumberFormatW(LOCALE_USER_DEFAULT,0,L"12a",0,text,64) && GetLastError() == ERROR_INVALID_PARAMETER);
	CHECK(8, !GetNumberFormatW(LOCALE_USER_DEFAULT,LOCALE_NOUSEROVERRIDE,L"1",&plain,text,64) && GetLastError() == ERROR_INVALID_FLAGS);
	CHECK(9, !GetNumberFormatW(LOCALE_USER_DEFAULT,0,L"123456",0,text,4) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);

	SHFILEINFOW info;
	CHECK(10, SHGetFileInfoW(L"C:\\nowhere\\notes.txt",FILE_ATTRIBUTE_NORMAL,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_DISPLAYNAME));
	CHECK(11, same(info.szTypeName,L"TXT File") && same(info.szDisplayName,L"notes.txt"));
	CHECK(12, SHGetFileInfoW(L"C:\\nowhere\\stuff",FILE_ATTRIBUTE_DIRECTORY,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_ATTRIBUTES));
	CHECK(13, same(info.szTypeName,L"File folder") && (info.dwAttributes & 0x20000000));
	CHECK(14, SHGetFileInfoW(L"README",FILE_ATTRIBUTE_NORMAL,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME) && same(info.szTypeName,L"File"));
	CHECK(15, !SHGetFileInfoW(L"C:\\nowhere\\missing.txt",0,&info,sizeof info,SHGFI_TYPENAME));
	CHECK(16, SHGetFileInfoW(L"x.bin",FILE_ATTRIBUTE_NORMAL,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON));
	if (info.hIcon) {
		ICONINFO icon;
		BITMAP bitmap;
		CHECK(17, GetIconInfo(info.hIcon,&icon) && GetObjectW(icon.hbmColor,sizeof bitmap,&bitmap) && bitmap.bmWidth == 16);
		DeleteObject(icon.hbmColor);
		DeleteObject(icon.hbmMask);
		CHECK(18, DestroyIcon(info.hIcon));
	}

	// A file manager asks for the system image list and, for each item, only the index of its picture there: a folder, a file and a
	// drive each have their own, in the small and the large list alike.
	HIMAGELIST small = (HIMAGELIST)SHGetFileInfoW(L"x.bin",FILE_ATTRIBUTE_NORMAL,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
	const int file_icon = info.iIcon;
	CHECK(33, small && ImageList_GetImageCount(small) > file_icon && file_icon >= 0);
	CHECK(34, (HIMAGELIST)SHGetFileInfoW(L"C:\\nowhere\\stuff",FILE_ATTRIBUTE_DIRECTORY,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON) == small);
	const int folder_icon = info.iIcon;
	CHECK(35, SHGetFileInfoW(L"C:\\",0,&info,sizeof info,SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME) && info.iIcon != folder_icon && info.iIcon != file_icon && folder_icon != file_icon);
	int width = 0, height = 0;
	HIMAGELIST large = (HIMAGELIST)SHGetFileInfoW(L"x.bin",FILE_ATTRIBUTE_NORMAL,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX);
	CHECK(36, large && large != small && info.iIcon == file_icon && ImageList_GetIconSize(large,&width,&height) && width == 32 && height == 32);
	// A type a program registered is named as its class names it.
	HKEY key;
	CHECK(37, RegCreateKeyExW(HKEY_CLASSES_ROOT,L".ravenshelltest",0,0,0,KEY_WRITE,0,&key,0) == ERROR_SUCCESS);
	RegSetValueExW(key,0,0,REG_SZ,(const BYTE *)L"Raven.ShellTest",sizeof L"Raven.ShellTest");
	RegCloseKey(key);
	CHECK(38, RegCreateKeyExW(HKEY_CLASSES_ROOT,L"Raven.ShellTest",0,0,0,KEY_WRITE,0,&key,0) == ERROR_SUCCESS);
	RegSetValueExW(key,0,0,REG_SZ,(const BYTE *)L"Shell Test Archive",sizeof L"Shell Test Archive");
	RegCloseKey(key);
	CHECK(39, SHGetFileInfoW(L"a.ravenshelltest",FILE_ATTRIBUTE_NORMAL,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME) && same(info.szTypeName,L"Shell Test Archive"));
	// Its DefaultIcon gives files of the type their own picture in the system image list, in both sizes at the same index.
	static WCHAR icon_path[260], icon_value[270];
	CHECK(40, GetTempPathW(260,icon_path));
	join(icon_value,icon_path,L"shell-test.ico");
	CHECK(41, write_icon(icon_value));
	join(icon_path,icon_value,L",0");
	CHECK(42, RegCreateKeyExW(HKEY_CLASSES_ROOT,L"Raven.ShellTest\\DefaultIcon",0,0,0,KEY_WRITE,0,&key,0) == ERROR_SUCCESS);
	RegSetValueExW(key,0,0,REG_SZ,(const BYTE *)icon_path,(lstrlenW(icon_path) + 1) * sizeof(WCHAR));
	RegCloseKey(key);
	const int before = ImageList_GetImageCount(small);
	CHECK(43, (HIMAGELIST)SHGetFileInfoW(L"a.ravenshelltest",FILE_ATTRIBUTE_NORMAL,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON) == small);
	const int typed = info.iIcon;
	CHECK(44, typed != file_icon && typed >= 0 && ImageList_GetImageCount(small) == before + 1 && ImageList_GetImageCount(large) == ImageList_GetImageCount(small));
	CHECK(45, SHGetFileInfoW(L"b.ravenshelltest",FILE_ATTRIBUTE_NORMAL,&info,sizeof info,SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX) && info.iIcon == typed &&
		ImageList_GetImageCount(small) == before + 1);
	RegDeleteKeyW(HKEY_CLASSES_ROOT,L"Raven.ShellTest\\DefaultIcon");
	DeleteFileW(icon_value);
	RegDeleteKeyW(HKEY_CLASSES_ROOT,L".ravenshelltest");
	RegDeleteKeyW(HKEY_CLASSES_ROOT,L"Raven.ShellTest");

	WCHAR temporary[260], root[260], path[260];
	CHECK(19, GetTempPathW(260,temporary));
	// A program that is not SYSTEM gets the same folder from GetTempPath2, which newer programs look up at run time.
	typedef DWORD (WINAPI *TempPath2W)(DWORD,LPWSTR);
	typedef DWORD (WINAPI *TempPath2A)(DWORD,LPSTR);
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	TempPath2W temp_path2w = (TempPath2W)GetProcAddress(kernel,"GetTempPath2W");
	TempPath2A temp_path2a = (TempPath2A)GetProcAddress(kernel,"GetTempPath2A");
	char narrow[260];
	CHECK(31, temp_path2w && temp_path2a && temp_path2w(260,path) == GetTempPathW(0,0) - 1 && same(path,temporary));
	CHECK(32, temp_path2a(260,narrow) == GetTempPathA(0,0) - 1 && narrow[0] == (char)temporary[0]);
	join(root,temporary,L"raven-change-notification");
	CreateDirectoryW(root,0);
	SetLastError(0);
	CHECK(20, FindFirstChangeNotificationW(root,FALSE,0) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER);
	join(path,root,L"\\missing");
	CHECK(21, FindFirstChangeNotificationW(path,FALSE,FILE_NOTIFY_CHANGE_FILE_NAME) == INVALID_HANDLE_VALUE);
	HANDLE change = FindFirstChangeNotificationW(root,FALSE,FILE_NOTIFY_CHANGE_FILE_NAME);
	CHECK(22, change != INVALID_HANDLE_VALUE && WaitForSingleObject(change,0) == WAIT_TIMEOUT);
	join(path,root,L"\\created.txt");
	HANDLE file = CreateFileW(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
	CHECK(23, file != INVALID_HANDLE_VALUE && CloseHandle(file));
	CHECK(24, WaitForSingleObject(change,5000) == WAIT_OBJECT_0);
	CHECK(25, FindNextChangeNotification(change) && WaitForSingleObject(change,0) == WAIT_TIMEOUT);
	CHECK(26, DeleteFileW(path) && WaitForMultipleObjects(1,&change,FALSE,5000) == WAIT_OBJECT_0);
	CHECK(27, FindCloseChangeNotification(change) && !FindCloseChangeNotification(change));
	CHECK(28, RemoveDirectoryW(root));

	CHECK(29, CoLockObjectExternal(0,TRUE,FALSE) == E_INVALIDARG);
	CHECK(30, CoLockObjectExternal((IUnknown *)&info,TRUE,FALSE) == S_OK && CoLockObjectExternal((IUnknown *)&info,FALSE,TRUE) == S_OK);
	return 0;
}
