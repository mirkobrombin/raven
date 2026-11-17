/* SPDX-License-Identifier: MIT */
#include "winmm_private.h"

void timer_init(void);
void wave_init(void);
void mixer_init(void);
void sound_init(void);

FARPROC user32_function(const char *name)
{
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (!user32)
		user32 = LoadLibraryW(L"user32.dll");
	return user32 ? GetProcAddress(user32, name) : NULL;
}

void ascii_from_wide(char *target, size_t size, const WCHAR *source)
{
	if (!size)
		return;
	size_t index = 0;
	for (; index + 1 < size && source[index]; index++)
		target[index] = source[index] < 0x80 ? (char)source[index] : '?';
	target[index] = 0;
}

MMRESULT notify_init(notify *notify, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
	notify->kind = flags & CALLBACK_TYPEMASK;
	notify->target = callback;
	notify->instance = instance;
	switch (notify->kind) {
	case CALLBACK_NULL:
		return MMSYSERR_NOERROR;
	case CALLBACK_WINDOW: {
		/* No window posts nothing, as Windows lets a program pass none. */
		if (!callback)
			return MMSYSERR_NOERROR;
		BOOL(WINAPI *is_window)(HWND) = (void *)user32_function("IsWindow");
		return is_window && is_window((HWND)callback) ? MMSYSERR_NOERROR : MMSYSERR_INVALPARAM;
	}
	case CALLBACK_THREAD:
	case CALLBACK_FUNCTION:
	case CALLBACK_EVENT:
		return callback ? MMSYSERR_NOERROR : MMSYSERR_INVALPARAM;
	default:
		return MMSYSERR_INVALFLAG;
	}
}

void notify_send(const notify *notify, HANDLE handle, UINT message, DWORD_PTR first, DWORD_PTR second)
{
	switch (notify->kind) {
	case CALLBACK_WINDOW:
		if (notify->target) {
			BOOL(WINAPI *post)(HWND, UINT, WPARAM, LPARAM) = (void *)user32_function("PostMessageW");
			if (post)
				post((HWND)notify->target, message, (WPARAM)handle, (LPARAM)first);
		}
		break;
	case CALLBACK_THREAD: {
		BOOL(WINAPI *post)(DWORD, UINT, WPARAM, LPARAM) = (void *)user32_function("PostThreadMessageW");
		if (post)
			post((DWORD)notify->target, message, (WPARAM)handle, (LPARAM)first);
		break;
	}
	case CALLBACK_FUNCTION:
		((LPDRVCALLBACK)notify->target)((HDRVR)handle, message, notify->instance, first, second);
		break;
	case CALLBACK_EVENT:
		SetEvent((HANDLE)notify->target);
		break;
	}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
		timer_init();
		wave_init();
		mixer_init();
		sound_init();
	}
	return TRUE;
}
