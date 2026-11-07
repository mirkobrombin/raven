/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The WebBrowser control's way to the runtime's page engine: raven-webengine.dll from the system directory, loaded with the first
 * navigation. Without it, as for 32-bit programs, the control does without pages. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "webengine.h"

static struct {
	engine_navigate_function navigate;
	engine_message_function message;
	engine_window_function resize, close;
} engine;

static BOOL load_engine(void)
{
	static LONG tried;
	if (InterlockedExchange(&tried, 1))
		return engine.navigate != NULL;
	WCHAR path[MAX_PATH];
	const UINT length = GetSystemDirectoryW(path, MAX_PATH - 24);
	if (!length || length >= MAX_PATH - 24)
		return FALSE;
	lstrcatW(path, L"\\raven-webengine.dll");
	HMODULE library = LoadLibraryW(path);
	if (!library)
		return FALSE;
	engine.message = (engine_message_function)GetProcAddress(library, "engine_message");
	engine.resize = (engine_window_function)GetProcAddress(library, "engine_resize");
	engine.close = (engine_window_function)GetProcAddress(library, "engine_close");
	engine_navigate_function navigate = (engine_navigate_function)GetProcAddress(library, "engine_navigate");
	if (!engine.message || !engine.resize || !engine.close || !navigate)
		return FALSE;
	engine.navigate = navigate;
	return TRUE;
}

BOOL engine_navigate(HWND window, const WCHAR *address)
{
	return load_engine() && engine.navigate(window, address);
}

BOOL engine_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
	return engine.navigate && engine.message(window, message, wparam, lparam, result);
}

void engine_resize(HWND window)
{
	if (engine.navigate)
		engine.resize(window);
}

void engine_close(HWND window)
{
	if (engine.navigate)
		engine.close(window);
}
