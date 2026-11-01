#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnls.h>

static int same(const WCHAR *left, const WCHAR *right, int length)
{
	for (int index = 0; index < length; index++)
		if (left[index] != right[index])
			return 0;
	return 1;
}

static int check(NORM_FORM form, const WCHAR *in, const WCHAR *expected)
{
	WCHAR out[32];
	int wanted = lstrlenW(expected);
	int length = NormalizeString(form, in, -1, out, 32);
	return length == wanted + 1 && same(out, expected, length);
}

int raven_entry(void)
{
	// e + combining acute, in each direction.
	if (!check(NormalizationC, L"e\x0301", L"\x00e9") || !check(NormalizationD, L"\x00e9", L"e\x0301"))
		return 1;
	// Marks reorder by class: dot below (220) goes ahead of dot above (230), then the first composes.
	if (!check(NormalizationC, L"s\x0307\x0323", L"\x1e69"))
		return 2;
	if (!check(NormalizationD, L"\x1e69", L"s\x0323\x0307"))
		return 3;
	// A blocked mark does not join: the second acute stays apart.
	if (!check(NormalizationC, L"a\x0301\x0301", L"\x00e1\x0301"))
		return 4;
	// Hangul by arithmetic, both ways.
	if (!check(NormalizationD, L"\xd55c", L"\x1112\x1161\x11ab") || !check(NormalizationC, L"\x1112\x1161\x11ab", L"\xd55c"))
		return 5;
	// Compatibility forms only change under KC and KD.
	if (!check(NormalizationKC, L"\xfb01", L"fi") || !check(NormalizationC, L"\xfb01", L"\xfb01") || !check(NormalizationKD, L"\x2460", L"1"))
		return 6;
	// A composition exclusion stays decomposed, and a singleton does not come back.
	if (!check(NormalizationC, L"\x0958", L"\x0915\x093c") || !check(NormalizationC, L"\x212b", L"\x00c5"))
		return 7;
	// Beyond the basic plane: U+1D15E has an excluded canonical decomposition.
	if (!check(NormalizationC, L"\xd834\xdd5e", L"\xd834\xdd57\xd834\xdd65"))
		return 8;

	// Asking for the size, a counted source without terminator, a short buffer, broken text, a bad form.
	WCHAR out[8];
	if (NormalizeString(NormalizationC, L"e\x0301", -1, NULL, 0) < 2)
		return 9;
	if (NormalizeString(NormalizationC, L"e\x0301z", 2, out, 8) != 1 || out[0] != 0xe9)
		return 10;
	SetLastError(0);
	if (NormalizeString(NormalizationD, L"\x00e9\x00e9", 2, out, 1) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return 11;
	SetLastError(0);
	if (NormalizeString(NormalizationC, L"a\xd800z", -1, out, 8) > 0 || GetLastError() != ERROR_NO_UNICODE_TRANSLATION)
		return 12;
	SetLastError(0);
	if (NormalizeString((NORM_FORM)3, L"a", -1, out, 8) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 13;

	if (!IsNormalizedString(NormalizationC, L"\x00e9", -1) || IsNormalizedString(NormalizationC, L"e\x0301", -1))
		return 14;
	if (!IsNormalizedString(NormalizationD, L"e\x0301", -1) || IsNormalizedString(NormalizationD, L"\x00e9", -1))
		return 15;
	return 0;
}
