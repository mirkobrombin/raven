/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* raven-webengine.dll, the page engine of the runtime's web controls: a Chromium Embedded Framework browser in a control's window, when
 * the runtime brings one. Controls load the library when they first need it and do without pages when it is missing. Everything the
 * engine tells a control arrives as a message to the control's window, on the control's thread. */
#ifndef RAVEN_WEBENGINE_H
#define RAVEN_WEBENGINE_H

#include <windows.h>

#ifdef WEBENGINE_BUILD
#define WEBENGINE_API __declspec(dllexport)
#else
#define WEBENGINE_API
#endif

/* wParam: TRUE for a redirect; lParam: the address, valid during the call. The answer is TRUE to cancel the navigation. */
#define ENGINE_BEFORE (WM_APP + 0x52)
/* lParam: the address the main frame reached, which the window's procedure frees with HeapFree; wParam: TRUE when the page failed. */
#define ENGINE_REACHED (WM_APP + 0x53)
/* A page asks for window.external: the answer is the IDispatch the control's site offers pages, with a reference for the caller, or 0. */
#define ENGINE_EXTERNAL (WM_APP + 0x57)
/* A document is about to run its own scripts: the answer is a script it runs first, allocated with HeapAlloc for the engine to free,
 * or 0. While that script runs, the global function __ravenPost(text) sends text back to the window as ENGINE_POSTED. */
#define ENGINE_SCRIPTS (WM_APP + 0x58)
/* lParam: text a page passed to __ravenPost, which the window's procedure frees with HeapFree. Posted, so it arrives in turn. */
#define ENGINE_POSTED (WM_APP + 0x59)

/* Sends the browser in the window to an address; FALSE when no engine draws pages, and the control completes the navigation itself. */
WEBENGINE_API BOOL engine_navigate(HWND window, const WCHAR *address);
/* The engine's share of the window's messages; TRUE when it took the message. */
WEBENGINE_API BOOL engine_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result);
WEBENGINE_API void engine_resize(HWND window);
/* Closes the browser in the window, before the window goes. */
WEBENGINE_API void engine_close(HWND window);
/* Runs a script in the main frame of the window's page; FALSE when the window has no page yet. */
WEBENGINE_API BOOL engine_execute(HWND window, const WCHAR *script);

/* Serves http and https requests to the host from the files under the folder, for every window of the process; FALSE when the
 * mapping cannot be kept. */
WEBENGINE_API BOOL engine_map_folder(const WCHAR *host, const WCHAR *folder);

typedef BOOL(*engine_map_folder_function)(const WCHAR *host, const WCHAR *folder);
typedef BOOL(*engine_navigate_function)(HWND window, const WCHAR *address);
typedef BOOL(*engine_execute_function)(HWND window, const WCHAR *script);
typedef BOOL(*engine_message_function)(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result);
typedef void(*engine_window_function)(HWND window);

#endif
