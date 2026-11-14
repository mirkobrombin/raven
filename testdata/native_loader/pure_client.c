#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <uxtheme.h>

/* Services that depend on their arguments alone, whether the runtime or the program answers them. */

LPWSTR WINAPI PathFindFileNameW(LPCWSTR path);

static LRESULT CALLBACK echo(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return (LRESULT)window + message * 3 + (LRESULT)wparam * 5 + lparam * 7;
}

static int rect_is(const RECT *rect, LONG left, LONG top, LONG right, LONG bottom)
{
	return rect->left == left && rect->top == top && rect->right == right && rect->bottom == bottom;
}

int raven_entry(void)
{
	static const int cases[][4] = {{5, 3, 2, 8}, {-5, 3, 2, -8}, {5, -3, 2, -8}, {5, 3, -2, -8}, {-5, -3, -2, -8}, {96, 10, 72, 13},
		{-1, 1, 2, -1}, {1, 1, 0, -1}, {0x7fffffff, 2, 1, -1}, {0x7fffffff, 2, 2, 0x7fffffff}, {-0x7fffffff - 1, 1, 1, -0x7fffffff - 1},
		{0, 5, 7, 0}, {7, 1, 3, 2}, {8, 1, 3, 3}};
	for (unsigned index = 0; index < sizeof cases / sizeof cases[0]; ++index)
		if (MulDiv(cases[index][0], cases[index][1], cases[index][2]) != cases[index][3])
			return 1 + (int)index;
	RECT first, second, result;
	if (!SetRect(&first, 1, 2, 30, 40) || !rect_is(&first, 1, 2, 30, 40))
		return 20;
	if (!OffsetRect(&first, -1, 5) || !rect_is(&first, 0, 7, 29, 45))
		return 21;
	SetRect(&second, 10, 0, 50, 20);
	if (!IntersectRect(&result, &first, &second) || !rect_is(&result, 10, 7, 29, 20))
		return 22;
	SetRect(&second, 29, 0, 50, 20);
	if (IntersectRect(&result, &first, &second) || !rect_is(&result, 0, 0, 0, 0))
		return 23;
	SetRect(&second, 5, 5, 5, 60);
	if (IntersectRect(&result, &first, &second) || !rect_is(&result, 0, 0, 0, 0))
		return 24;
	SetRect(&second, 5, 5, 15, 60);
	if (!IntersectRect(&first, &first, &second) || !rect_is(&first, 5, 7, 15, 45))
		return 25;
	/* The other arithmetic on rectangles: empty ones, copies, sides moved apart, and the rectangle that holds two, where an empty one
	   counts for nothing and the result may be one of the two. */
	SetRect(&second, 5, 5, 5, 60);
	if (IsRectEmpty(&first) || !IsRectEmpty(&second))
		return 200;
	SetRect(&second, 5, 60, 15, 60);
	if (!IsRectEmpty(&second))
		return 201;
	SetRect(&second, 9, 9, 3, 12);
	if (!IsRectEmpty(&second))
		return 202;
	if (!CopyRect(&result, &first) || !rect_is(&result, 5, 7, 15, 45))
		return 203;
	if (!InflateRect(&result, 2, -3) || !rect_is(&result, 3, 10, 17, 42))
		return 204;
	if (!SetRectEmpty(&result) || !rect_is(&result, 0, 0, 0, 0) || !IsRectEmpty(&result))
		return 205;
	SetRect(&second, -4, 20, 9, 70);
	if (!UnionRect(&result, &first, &second) || !rect_is(&result, -4, 7, 15, 70))
		return 206;
	SetRect(&second, 100, 100, 100, 200);
	if (!UnionRect(&result, &first, &second) || !rect_is(&result, 5, 7, 15, 45))
		return 207;
	if (!UnionRect(&result, &second, &first) || !rect_is(&result, 5, 7, 15, 45))
		return 208;
	SetRect(&result, 1, 1, 2, 2);
	if (UnionRect(&result, &second, &second) || !rect_is(&result, 0, 0, 0, 0))
		return 209;
	SetRect(&second, 0, 0, 8, 8);
	if (!UnionRect(&second, &second, &first) || !rect_is(&second, 0, 0, 15, 45))
		return 210;
	static const struct { const WCHAR *path; unsigned skip; } names[] = {{L"C:\\dir\\file.txt", 7}, {L"a/b", 2}, {L"C:file", 2},
		{L"file", 0}, {L"dir\\", 0}, {L"ab:c", 3}, {L"\\\\server\\share\\x", 15}, {L"", 0}, {L"C:", 0}, {L"C:\\", 0}};
	for (unsigned index = 0; index < sizeof names / sizeof names[0]; ++index)
		if (PathFindFileNameW(names[index].path) != names[index].path + names[index].skip)
			return 30 + (int)index;
	if (GetWindowTheme(NULL) || GetWindowTheme((HWND)0x12345))
		return 40;
	if (CallWindowProcW(echo, (HWND)11, 13, 17, 19) != 11 + 13 * 3 + 17 * 5 + 19 * 7 || CallWindowProcA(echo, (HWND)1, 2, 3, 4) != 1 + 6 + 15 + 28)
		return 41;
	SetLastError(0);
	if (CallWindowProcW(NULL, (HWND)1, 2, 3, 4) || GetLastError() != ERROR_INVALID_PARAMETER)
		return 42;
	HDC screen = GetDC(NULL);
	if (GetSystemMetrics(SM_CXVSCROLL) != 17 || GetSystemMetrics(SM_CXICON) != 32 || GetSystemMetrics(SM_CMOUSEBUTTONS) != 3 ||
		GetSystemMetrics(200) != 0 || GetSystemMetrics(SM_CXSCREEN) != GetDeviceCaps(screen, HORZRES) || GetSystemMetrics(-1) != 0)
		return 43;
	ReleaseDC(NULL, screen);
	LOGBRUSH brush;
	if (GetObjectW(GetSysColorBrush(COLOR_WINDOW), sizeof brush, &brush) != sizeof brush || brush.lbColor != GetSysColor(COLOR_WINDOW) ||
		GetSysColor(500) != 0)
		return 44;
	return 0;
}
