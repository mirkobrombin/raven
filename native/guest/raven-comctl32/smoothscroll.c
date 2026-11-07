/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>

/* The ordinals with no public contract. Scrolling happens in one step: programs only see where it ends. */

typedef BOOL (CALLBACK *scroll_function)(HWND, INT, INT, LPRECT, LPRECT, HRGN, LPRECT, DWORD);

typedef struct {
	DWORD size;
	DWORD reserved;
	HWND window;
	DWORD dx, dy;
	LPRECT scroll_rect, clip_rect;
	HRGN update_region;
	LPRECT update_rect;
	DWORD flags, interval, dx_step, dy_step;
	scroll_function scroll;
} smooth_scroll;

BOOL WINAPI SmoothScrollWindow(const smooth_scroll *request)
{
	if (request->scroll)
		return request->scroll(request->window, (INT)request->dx, (INT)request->dy, request->scroll_rect, request->clip_rect,
			request->update_region, request->update_rect, request->flags);
	return ScrollWindowEx(request->window, (INT)request->dx, (INT)request->dy, request->scroll_rect, request->clip_rect,
		request->update_region, request->update_rect, request->flags) != ERROR;
}
