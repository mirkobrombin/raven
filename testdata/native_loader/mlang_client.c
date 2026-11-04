// mlang converts Japanese text between Shift-JIS, EUC-JP and Unicode.
#include <windows.h>

HRESULT WINAPI ConvertINetMultiByteToUnicode(LPDWORD, DWORD, LPCSTR, LPINT, LPWSTR, LPINT);
HRESULT WINAPI ConvertINetUnicodeToMultiByte(LPDWORD, DWORD, LPCWSTR, LPINT, LPSTR, LPINT);

int raven_entry(void)
{
	// A hiragana and a katakana whose Shift-JIS trail byte is a backslash.
	static const WCHAR expected[] = {0x3042, 0x30bd};
	WCHAR wide[8];
	char narrow[8];
	DWORD mode = 0;
	INT in = 4, out = 8;
	if (ConvertINetMultiByteToUnicode(&mode, 932, "\x82\xa0\x83\x5c", &in, wide, &out) != S_OK || out != 2 ||
		wide[0] != expected[0] || wide[1] != expected[1])
		return 1;
	mode = 0, in = 4, out = 8;
	if (ConvertINetMultiByteToUnicode(&mode, 51932, "\xa4\xa2\xa5\xbd", &in, wide, &out) != S_OK || out != 2 ||
		wide[0] != expected[0] || wide[1] != expected[1])
		return 2;
	mode = 0, in = 2, out = 8;
	if (ConvertINetUnicodeToMultiByte(&mode, 51932, expected, &in, narrow, &out) != S_OK || out != 4 ||
		narrow[0] != '\xa4' || narrow[1] != '\xa2' || narrow[2] != '\xa5' || narrow[3] != '\xbd')
		return 3;
	return 0;
}
