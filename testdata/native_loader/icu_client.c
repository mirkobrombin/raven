#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ICU's interface through the libraries Windows ships it as: every result here comes from the API documentation or from the Unicode rules. */
typedef int (__cdecl *case_function)(WCHAR *, int, const WCHAR *, int, const char *, int *);
typedef void *(__cdecl *casemap_open_function)(const char *, unsigned, int *);
typedef void (__cdecl *close_function)(void *);
typedef int (__cdecl *fold_function)(const void *, char *, int, const char *, int, int *);
typedef void *(__cdecl *break_open_function)(int, const char *, const WCHAR *, int, int *);
typedef void (__cdecl *break_text_function)(void *, void *, int *);
typedef int (__cdecl *break_step_function)(void *);
typedef void *(__cdecl *collator_open_function)(const char *, int *);
typedef int (__cdecl *collate_function)(const void *, const WCHAR *, int, const WCHAR *, int);
typedef void *(__cdecl *regex_open_function)(const WCHAR *, int, unsigned, void *, int *);
typedef void *(__cdecl *regex_open_text_function)(void *, unsigned, void *, int *);
typedef void *(__cdecl *regex_clone_function)(const void *, int *);
typedef void (__cdecl *regex_text_function)(void *, const WCHAR *, int, int *);
typedef signed char (__cdecl *regex_find_function)(void *, int *);
typedef signed char (__cdecl *regex_matches_function)(void *, int, int *);
typedef int (__cdecl *regex_edge_function)(void *, int, int *);
typedef void *(__cdecl *regex_replace_text_function)(void *, void *, void *, int *);
typedef const char *(__cdecl *error_name_function)(int);
typedef void *(__cdecl *text_utf8_function)(void *, const char *, __int64, int *);
typedef void *(__cdecl *text_units_function)(void *, const WCHAR *, __int64, int *);
typedef void *(__cdecl *text_close_function)(void *);
typedef __int64 (__cdecl *text_length_function)(void *);
typedef int (__cdecl *text_char_function)(void *, __int64);
typedef int (__cdecl *text_extract_function)(void *, __int64, __int64, WCHAR *, int, int *);
typedef int (__cdecl *property_function)(int, int);
typedef int (__cdecl *convert_function)(const char *, const char *, char *, int, const char *, int, int *);

static int same(const WCHAR *left, const WCHAR *right, int count)
{
	for (int index = 0; index < count; ++index)
		if (left[index] != right[index])
			return 0;
	return 1;
}

#define LOAD(library, type, name) type name = (type)GetProcAddress(library, #name); if (!name) return 21;

int raven_entry(void)
{
	HMODULE common = LoadLibraryW(L"icuuc.dll"), i18n = LoadLibraryW(L"icuin.dll");
	if (!common || !i18n) return 20;
	LOAD(common, case_function, u_strToUpper)
	LOAD(common, casemap_open_function, ucasemap_open)
	LOAD(common, close_function, ucasemap_close)
	LOAD(common, fold_function, ucasemap_utf8FoldCase)
	LOAD(common, break_open_function, ubrk_open)
	LOAD(common, break_text_function, ubrk_setUText)
	LOAD(common, break_step_function, ubrk_first)
	LOAD(common, break_step_function, ubrk_next)
	LOAD(common, break_step_function, ubrk_getRuleStatus)
	LOAD(common, close_function, ubrk_close)
	LOAD(common, error_name_function, u_errorName)
	LOAD(common, text_utf8_function, utext_openUTF8)
	LOAD(common, text_units_function, utext_openUChars)
	LOAD(common, text_close_function, utext_close)
	LOAD(common, text_length_function, utext_nativeLength)
	LOAD(common, text_char_function, utext_char32At)
	LOAD(common, text_extract_function, utext_extract)
	LOAD(common, property_function, u_getIntPropertyValue)
	LOAD(common, convert_function, ucnv_convert)
	LOAD(i18n, collator_open_function, ucol_open)
	LOAD(i18n, collate_function, ucol_strcoll)
	LOAD(i18n, close_function, ucol_close)
	LOAD(i18n, regex_open_function, uregex_open)
	LOAD(i18n, regex_open_text_function, uregex_openUText)
	LOAD(i18n, regex_clone_function, uregex_clone)
	LOAD(i18n, regex_text_function, uregex_setText)
	LOAD(i18n, regex_find_function, uregex_findNext)
	LOAD(i18n, regex_matches_function, uregex_matches)
	LOAD(i18n, regex_edge_function, uregex_start)
	LOAD(i18n, regex_edge_function, uregex_end)
	LOAD(i18n, regex_replace_text_function, uregex_replaceAllUText)
	LOAD(i18n, close_function, uregex_close)

	if (lstrcmpA(u_errorName(0), "U_ZERO_ERROR")) return 22;
	int status = 0;
	WCHAR out[64];
	static const WCHAR street[] = {'s', 't', 'r', 'a', 0xdf, 'e'}, upper[] = {'S', 'T', 'R', 'A', 'S', 'S', 'E'};
	if (u_strToUpper(out, 64, street, 6, "", &status) != 7 || status > 0 || !same(out, upper, 7)) return 23;
	// Asking for the length alone is an overflow that still answers.
	status = 0;
	if (u_strToUpper(NULL, 0, street, 6, "", &status) != 7 || status != 15) return 24;

	status = 0;
	void *map = ucasemap_open(NULL, 0, &status);
	char folded[16];
	if (!map || status > 0) return 25;
	if (ucasemap_utf8FoldCase(map, NULL, 0, "\xc3\x80" "B", 3, &status) != 3 || status != 15) return 26;
	status = 0;
	if (ucasemap_utf8FoldCase(map, folded, sizeof folded, "\xc3\x80" "B", 3, &status) != 3 || status > 0 || lstrcmpA(folded, "\xc3\xa0" "b")) return 27;
	ucasemap_close(map);

	// Word boundaries and their status over UTF-8, in bytes.
	void *text = utext_openUTF8(NULL, "caff\xc3\xa8 can't 3.14 \xe6\x97\xa5\xe6\x9c\xac", -1, &status);
	if (!text || status > 0) return 28;
	void *breaks = ubrk_open(1, NULL, NULL, 0, &status);
	if (!breaks || status > 0) return 29;
	ubrk_setUText(breaks, text, &status);
	if (status > 0 || ubrk_first(breaks) != 0) return 30;
	if (ubrk_next(breaks) != 6 || ubrk_getRuleStatus(breaks) != 200) return 31;
	if (ubrk_next(breaks) != 7 || ubrk_getRuleStatus(breaks) != 0) return 32;
	if (ubrk_next(breaks) != 12 || ubrk_getRuleStatus(breaks) != 200) return 33;
	if (ubrk_next(breaks) != 13 || ubrk_next(breaks) != 17 || ubrk_getRuleStatus(breaks) != 100) return 34;
	if (ubrk_next(breaks) != 18 || ubrk_next(breaks) != 21 || ubrk_getRuleStatus(breaks) != 400) return 35;
	if (ubrk_next(breaks) != 24 || ubrk_next(breaks) != -1) return 36;
	ubrk_close(breaks);
	if (utext_nativeLength(text) != 24 || utext_char32At(text, 4) != 0xe8 || utext_char32At(text, 24) != -1) return 37;
	utext_close(text);

	// Grapheme clusters: a base with a mark, a CR LF, a surrogate pair, a flag.
	static const WCHAR clusters[] = {'e', 0x301, '\r', '\n', 0xd83d, 0xde00, 0xd83c, 0xdde9, 0xd83c, 0xddea, 'x'};
	void *characters = ubrk_open(0, NULL, clusters, 11, &status);
	if (!characters || status > 0 || ubrk_first(characters) != 0) return 38;
	if (ubrk_next(characters) != 2 || ubrk_next(characters) != 4 || ubrk_next(characters) != 6 || ubrk_next(characters) != 10 ||
		ubrk_next(characters) != 11 || ubrk_next(characters) != -1) return 39;
	ubrk_close(characters);

	if (u_getIntPropertyValue(0x65e5, 0x1004) != 5 || u_getIntPropertyValue(0xff21, 0x1004) != 3 || u_getIntPropertyValue('a', 0x1004) != 4) return 40;

	void *collator = ucol_open("en_US", &status);
	static const WCHAR lower_a[] = {'a'}, upper_b[] = {'B'};
	if (!collator || status > 0 || ucol_strcoll(collator, lower_a, 1, upper_b, 1) != -1 || ucol_strcoll(collator, upper_b, 1, lower_a, 1) != 1 ||
		ucol_strcoll(collator, lower_a, 1, lower_a, 1) != 0) return 41;
	ucol_close(collator);

	char converted[16];
	static const char latin[] = {(char)0xe8, 0};
	if (ucnv_convert("UTF-8", "ISO-8859-1", converted, sizeof converted, latin, 1, &status) != 2 || status > 0 ||
		(unsigned char)converted[0] != 0xc3 || (unsigned char)converted[1] != 0xa8) return 42;

	// Searching, with alternation, a class, a group and a word edge.
	static const WCHAR words[] = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', ' ', 'w', 'a', 'r'};
	static const WCHAR pattern[] = {'\\', 'b', 'w', '[', 'a', 'o', ']', 'r', '(', 'l', 'd', '|', ')', '\\', 'b'};
	void *regex = uregex_open(pattern, 15, 0, NULL, &status);
	if (!regex || status > 0) return 43;
	uregex_setText(regex, words, 15, &status);
	if (!uregex_findNext(regex, &status) || uregex_start(regex, 0, &status) != 6 || uregex_end(regex, 0, &status) != 11 || uregex_end(regex, 1, &status) != 11) return 44;
	if (!uregex_findNext(regex, &status) || uregex_start(regex, 0, &status) != 12 || uregex_end(regex, 0, &status) != 15) return 45;
	if (uregex_findNext(regex, &status) || status > 0) return 46;

	// A clone has the pattern and not the text, as documented.
	void *copy = uregex_clone(regex, &status);
	if (!copy || status > 0) return 47;
	uregex_findNext(copy, &status);
	if (status != 0x10302) return 48;
	status = 0;
	uregex_close(copy);
	uregex_close(regex);

	// A whole match, case-insensitive, with a counted loop, from a UTF-8 pattern.
	void *source = utext_openUTF8(NULL, "^[a-z]+-\\d{2,3}$", -1, &status);
	void *ignoring = uregex_openUText(source, 2, NULL, &status);
	utext_close(source);
	if (!ignoring || status > 0) return 49;
	static const WCHAR good[] = {'A', 'b', 'C', '-', '1', '2', '3'}, bad[] = {'A', 'b', 'C', '-', '1'};
	uregex_setText(ignoring, good, 7, &status);
	if (!uregex_matches(ignoring, -1, &status)) return 50;
	uregex_setText(ignoring, bad, 5, &status);
	if (uregex_matches(ignoring, -1, &status) || status > 0) return 51;
	uregex_close(ignoring);

	// A pattern that fails to compile reports it.
	static const WCHAR broken[] = {'(', 'a'};
	if (uregex_open(broken, 2, 0, NULL, &status) || status != 0x10306) return 52;
	status = 0;

	// Replacing with a group, and an empty match does not hang: x* against abc.
	static const WCHAR swap[] = {'(', 'a', ')', '(', 'b', ')'}, swap_with[] = {'$', '2', '$', '1'}, abab[] = {'a', 'b', '-', 'a', 'b'}, swapped[] = {'b', 'a', '-', 'b', 'a'};
	void *replacing = uregex_open(swap, 6, 0, NULL, &status);
	uregex_setText(replacing, abab, 5, &status);
	void *with = utext_openUChars(NULL, swap_with, 4, &status);
	void *result = uregex_replaceAllUText(replacing, with, NULL, &status);
	if (!result || status > 0 || utext_nativeLength(result) != 5) return 53;
	if (utext_extract(result, 0, 0x7fffffffffffffffLL, out, 64, &status) != 5 || status > 0 || !same(out, swapped, 5)) return 54;
	utext_close(result);
	utext_close(with);
	uregex_close(replacing);
	static const WCHAR star[] = {'x', '*'}, abc[] = {'a', 'b', 'c'};
	void *empty = uregex_open(star, 2, 0, NULL, &status);
	uregex_setText(empty, abc, 3, &status);
	int found = 0;
	while (uregex_findNext(empty, &status) && found < 10) found++;
	if (found != 4 || status > 0) return 55;
	uregex_close(empty);

	// A pattern that would run for ever stops with the time limit.
	static const WCHAR nested[] = {'(', 'a', '+', ')', '+', 'b'};
	WCHAR many[40];
	for (int i = 0; i < 40; i++) many[i] = 'a';
	void *slow = uregex_open(nested, 6, 0, NULL, &status);
	uregex_setText(slow, many, 40, &status);
	uregex_findNext(slow, &status);
	if (status != 0x10312) return 56;
	status = 0;
	uregex_close(slow);
	return 0;
}
