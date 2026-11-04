/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The page engine of the WebBrowser control: a Chromium Embedded Framework browser in the control's window, when the runtime brings
 * one. Everything the engine tells the control arrives as a message to the control's window, on the control's thread. */
#ifndef RAVEN_IEFRAME_ENGINE_H
#define RAVEN_IEFRAME_ENGINE_H

#include <windows.h>

/* wParam: TRUE for a redirect; lParam: the address, valid during the call. The answer is TRUE to cancel the navigation. */
#define ENGINE_BEFORE (WM_APP + 0x52)
/* lParam: the address the main frame reached, which the window's procedure frees with HeapFree. */
#define ENGINE_REACHED (WM_APP + 0x53)
/* A page asks for window.external: the answer is the IDispatch the control's site offers pages, with a reference for the caller, or 0. */
#define ENGINE_EXTERNAL (WM_APP + 0x57)

/* Sends the browser in the window to an address; FALSE when no engine draws pages, and the control completes the navigation itself. */
BOOL engine_navigate(HWND window, const WCHAR *address);
/* The engine's share of the window's messages; TRUE when it took the message. */
BOOL engine_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT *result);
void engine_resize(HWND window);
/* Closes the browser in the window, before the window goes. */
void engine_close(HWND window);

#endif
