#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The part of ICU's C interface this client uses, as Windows' icuuc.dll and icuin.dll export it. */
typedef int (*case_function)(WCHAR *, int, const WCHAR *, int, const char *, int *);
typedef void *(*casemap_open_function)(const char *, unsigned, int *);
typedef void (*close_function)(void *);
typedef int (*fold_function)(const void *, char *, int, const char *, int, int *);
typedef void *(*break_open_function)(int, const char *, const WCHAR *, int, int *);
typedef int (*break_step_function)(void *);
typedef void *(*collator_open_function)(const char *, int *);
typedef int (*collate_function)(const void *, const WCHAR *, int, const WCHAR *, int);
typedef void *(*regex_open_function)(const WCHAR *, int, unsigned, void *, int *);
typedef void (*regex_text_function)(void *, const WCHAR *, int, int *);
typedef int (*regex_find_function)(void *, int *);
typedef int (*regex_start_function)(void *, int, int *);
typedef const char *(*error_name_function)(int);

static int same(const WCHAR *left, const WCHAR *right, int count)
{
	for (int index = 0; index < count; ++index)
		if (left[index] != right[index])
			return 0;
	return 1;
}

/* ICU through the libraries Windows ships it as, which forward to icu.dll: case mapping and folding, word breaks, collation and
 * regular expressions. */
int raven_entry(void)
{
	WCHAR out[32];
	HMODULE common = LoadLibraryW(L"icuuc.dll"), i18n = LoadLibraryW(L"icuin.dll");
	if (!common || !i18n) return 20;
	case_function to_upper = (case_function)GetProcAddress(common, "u_strToUpper");
	casemap_open_function casemap_open = (casemap_open_function)GetProcAddress(common, "ucasemap_open");
	close_function casemap_close = (close_function)GetProcAddress(common, "ucasemap_close");
	fold_function fold = (fold_function)GetProcAddress(common, "ucasemap_utf8FoldCase");
	break_open_function break_open = (break_open_function)GetProcAddress(common, "ubrk_open");
	break_step_function break_first = (break_step_function)GetProcAddress(common, "ubrk_first");
	break_step_function break_next = (break_step_function)GetProcAddress(common, "ubrk_next");
	break_step_function break_status = (break_step_function)GetProcAddress(common, "ubrk_getRuleStatus");
	close_function break_close = (close_function)GetProcAddress(common, "ubrk_close");
	error_name_function error_name = (error_name_function)GetProcAddress(common, "u_errorName");
	collator_open_function collator_open = (collator_open_function)GetProcAddress(i18n, "ucol_open");
	collate_function collate = (collate_function)GetProcAddress(i18n, "ucol_strcoll");
	close_function collator_close = (close_function)GetProcAddress(i18n, "ucol_close");
	regex_open_function regex_open = (regex_open_function)GetProcAddress(i18n, "uregex_open");
	regex_text_function regex_text = (regex_text_function)GetProcAddress(i18n, "uregex_setText");
	regex_find_function regex_find = (regex_find_function)GetProcAddress(i18n, "uregex_findNext");
	regex_start_function regex_start = (regex_start_function)GetProcAddress(i18n, "uregex_start");
	close_function regex_close = (close_function)GetProcAddress(i18n, "uregex_close");
	if (!to_upper || !casemap_open || !casemap_close || !fold || !break_open || !break_first || !break_next || !break_status ||
		!break_close || !error_name || !collator_open || !collate || !collator_close || !regex_open || !regex_text || !regex_find ||
		!regex_start || !regex_close) return 21;
	if (lstrcmpA(error_name(0), "U_ZERO_ERROR")) return 22;
	int status = 0;
	static const WCHAR street[] = {'s', 't', 'r', 'a', 0xdf, 'e'}, upper[] = {'S', 'T', 'R', 'A', 'S', 'S', 'E'};
	if (to_upper(out, 32, street, 6, "", &status) != 7 || status > 0 || !same(out, upper, 7)) return 23;
	void *map = casemap_open("", 0, &status);
	char folded[16];
	if (!map || status > 0 || fold(map, folded, sizeof folded, "\xc3\x80" "B", -1, &status) != 3 || status > 0 ||
		lstrcmpA(folded, "\xc3\xa0" "b")) return 24;
	casemap_close(map);
	static const WCHAR words[] = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
	void *breaks = break_open(1, "en", words, 11, &status);
	if (!breaks || status > 0) return 25;
	if (break_first(breaks) != 0 || break_next(breaks) != 5 || break_status(breaks) < 200 || break_status(breaks) >= 300) return 26;
	if (break_next(breaks) != 6 || break_next(breaks) != 11) return 27;
	break_close(breaks);
	void *collator = collator_open("en_US", &status);
	static const WCHAR lower_a[] = {'a'}, upper_b[] = {'B'};
	if (!collator || status > 0 || collate(collator, lower_a, 1, upper_b, 1) != -1 || collate(collator, upper_b, 1, lower_a, 1) != 1) return 28;
	collator_close(collator);
	static const WCHAR pattern[] = {'w', '.', 'r'};
	void *regex = regex_open(pattern, 3, 0, NULL, &status);
	if (!regex || status > 0) return 29;
	regex_text(regex, words, 11, &status);
	if (!regex_find(regex, &status) || regex_start(regex, 0, &status) != 6 || status > 0) return 30;
	regex_close(regex);
	return 0;
}
