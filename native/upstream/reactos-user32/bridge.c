/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <user32.h>

/* The upstream header turns these public calls into kernel calls, so the bridge reaches the real ones under their import names. */
/* An imported function is named by its import symbol: plain on 64 bits, decorated with the argument bytes on 32. */
#ifdef _M_IX86
#define RAVEN_IMPORT(name, bytes) __asm__("_" name "@" #bytes)
#else
#define RAVEN_IMPORT(name, bytes) __asm__(name)
#endif

extern BOOL WINAPI raven_real_InvalidateRect(HWND, CONST RECT *, BOOL) RAVEN_IMPORT("InvalidateRect", 12);
extern BOOL WINAPI raven_real_InvalidateRgn(HWND, HRGN, BOOL) RAVEN_IMPORT("InvalidateRgn", 12);
extern BOOL WINAPI raven_real_RedrawWindow(HWND, CONST RECT *, HRGN, UINT) RAVEN_IMPORT("RedrawWindow", 16);
extern HDC WINAPI raven_real_GetDC(HWND) RAVEN_IMPORT("GetDC", 4);
extern HDC WINAPI raven_real_GetDCEx(HWND, HRGN, DWORD) RAVEN_IMPORT("GetDCEx", 12);
extern HDC WINAPI raven_real_BeginPaint(HWND, LPPAINTSTRUCT) RAVEN_IMPORT("BeginPaint", 8);
extern BOOL WINAPI raven_real_EndPaint(HWND, CONST PAINTSTRUCT *) RAVEN_IMPORT("EndPaint", 8);
extern HWND WINAPI raven_real_SetFocus(HWND) RAVEN_IMPORT("SetFocus", 4);
extern HWND WINAPI raven_real_SetCapture(HWND) RAVEN_IMPORT("SetCapture", 4);
extern HWND WINAPI raven_real_SetActiveWindow(HWND) RAVEN_IMPORT("SetActiveWindow", 4);
extern HWND WINAPI raven_real_GetForegroundWindow(VOID) RAVEN_IMPORT("GetForegroundWindow", 0);
extern BOOL WINAPI raven_real_SetWindowPos(HWND, HWND, int, int, int, int, UINT) RAVEN_IMPORT("SetWindowPos", 28);
extern BOOL WINAPI raven_real_MoveWindow(HWND, int, int, int, int, BOOL) RAVEN_IMPORT("MoveWindow", 24);
extern BOOL WINAPI raven_real_ShowWindow(HWND, int) RAVEN_IMPORT("ShowWindow", 8);
extern HWND WINAPI raven_real_SetParent(HWND, HWND) RAVEN_IMPORT("SetParent", 8);
extern BOOL WINAPI raven_real_DestroyWindow(HWND) RAVEN_IMPORT("DestroyWindow", 4);
extern BOOL WINAPI raven_real_DestroyMenu(HMENU) RAVEN_IMPORT("DestroyMenu", 4);
extern BOOL WINAPI raven_real_DeleteMenu(HMENU, UINT, UINT) RAVEN_IMPORT("DeleteMenu", 12);
extern UINT_PTR WINAPI raven_real_SetTimer(HWND, UINT_PTR, UINT, TIMERPROC) RAVEN_IMPORT("SetTimer", 16);
extern BOOL WINAPI raven_real_KillTimer(HWND, UINT_PTR) RAVEN_IMPORT("KillTimer", 8);
extern UINT_PTR WINAPI raven_real_SetSystemTimer(HWND, UINT_PTR, UINT, TIMERPROC) RAVEN_IMPORT("SetSystemTimer", 16);
extern BOOL WINAPI raven_real_CreateCaret(HWND, HBITMAP, int, int) RAVEN_IMPORT("CreateCaret", 16);
extern BOOL WINAPI raven_real_ShowCaret(HWND) RAVEN_IMPORT("ShowCaret", 4);
extern BOOL WINAPI raven_real_HideCaret(HWND) RAVEN_IMPORT("HideCaret", 4);
extern BOOL WINAPI raven_real_IsClipboardFormatAvailable(UINT) RAVEN_IMPORT("IsClipboardFormatAvailable", 4);
extern BOOL WINAPI raven_real_EmptyClipboard(VOID) RAVEN_IMPORT("EmptyClipboard", 0);
extern BOOL WINAPI raven_real_CloseClipboard(VOID) RAVEN_IMPORT("CloseClipboard", 0);

/* Process-wide state and kernel services the upstream control and dialog sources expect, rebuilt on public calls. */

HINSTANCE User32Instance;
static SERVERINFO server_info;
PSERVERINFO gpsi = &server_info;
USERAPIHOOK guah;
BOOLEAN gfServerProcess;
Imm32ApiTable gImmApiEntries;

static const WCHAR function_property[] = {'R','a','v','e','n','F','n','i','d',0};
static const WCHAR dialog_property[] = {'R','a','v','e','n','D','l','g',0};

/* Control and dialog code reads these fields through a window pointer; the values are sampled per call. */
PWND FASTCALL ValidateHwnd(HWND window)
{
	static WND ring[8];
	static unsigned next;
	WND *record;
	RECT rectangle;
	POINT origin = {0, 0};
	if (!IsWindow(window))
		return NULL;
	record = &ring[next++ & 7];
	ZeroMemory(record, sizeof *record);
	record->fnid = (WORD)(ULONG_PTR)GetPropW(window, function_property);
	record->style = (DWORD)GetWindowLongW(window, GWL_STYLE);
	record->ExStyle = (DWORD)GetWindowLongW(window, GWL_EXSTYLE);
	record->cbwndExtra = (int)GetClassLongW(window, GCL_CBWNDEXTRA);
	record->DialogPointer = GetPropW(window, dialog_property);
	/* Every window follows the Windows 4.0 behavior; dialog windows are recognized by their class procedure. */
	record->state2 = WNDS2_WIN40COMPAT;
	GetWindowRect(window, &record->rcWindow);
	GetClientRect(window, &rectangle);
	ClientToScreen(window, &origin);
	OffsetRect(&rectangle, origin.x, origin.y);
	record->rcClient = rectangle;
	return record;
}

BOOL NTAPI NtUserSetWindowFNID(HWND window, WORD function)
{
	return SetPropW(window, function_property, (HANDLE)(ULONG_PTR)function);
}

DWORD NTAPI NtUserAlterWindowStyle(HWND window, DWORD index, LONG value)
{
	return (DWORD)SetWindowLongW(window, (int)index, value);
}

BOOL NTAPI NtUserInvalidateRect(HWND window, CONST RECT *rectangle, BOOL erase)
{
	return raven_real_InvalidateRect(window, rectangle, erase);
}

BOOL NTAPI NtUserInvalidateRgn(HWND window, HRGN region, BOOL erase)
{
	return raven_real_InvalidateRgn(window, region, erase);
}

BOOL NTAPI NtUserRedrawWindow(HWND window, CONST RECT *rectangle, HRGN region, UINT flags)
{
	return raven_real_RedrawWindow(window, rectangle, region, flags);
}

HDC NTAPI NtUserGetDC(HWND window)
{
	return raven_real_GetDC(window);
}

HDC NTAPI NtUserGetDCEx(HWND window, HANDLE region, ULONG flags)
{
	return raven_real_GetDCEx(window, region, flags);
}

HDC NTAPI NtUserBeginPaint(HWND window, PAINTSTRUCT *paint)
{
	return raven_real_BeginPaint(window, paint);
}

BOOL NTAPI NtUserEndPaint(HWND window, CONST PAINTSTRUCT *paint)
{
	return raven_real_EndPaint(window, paint);
}

HWND NTAPI NtUserSetFocus(HWND window)
{
	return raven_real_SetFocus(window);
}

HWND NTAPI NtUserSetCapture(HWND window)
{
	return raven_real_SetCapture(window);
}

HWND NTAPI NtUserSetActiveWindow(HWND window)
{
	return raven_real_SetActiveWindow(window);
}

HWND NTAPI NtUserGetForegroundWindow(VOID)
{
	return raven_real_GetForegroundWindow();
}

/* The upstream header turns ShowScrollBar into a kernel call, so the real function is reached under its import name. */
extern BOOL WINAPI raven_show_scroll_bar(HWND window, int bar, BOOL show) RAVEN_IMPORT("ShowScrollBar", 12);

DWORD NTAPI NtUserShowScrollBar(HWND window, int bar, DWORD show)
{
	return raven_show_scroll_bar(window, bar, show);
}

BOOL NTAPI NtUserSetWindowPos(HWND window, HWND after, int x, int y, int width, int height, UINT flags)
{
	return raven_real_SetWindowPos(window, after, x, y, width, height, flags);
}

BOOL NTAPI NtUserMoveWindow(HWND window, int x, int y, int width, int height, BOOL repaint)
{
	return raven_real_MoveWindow(window, x, y, width, height, repaint);
}

BOOL NTAPI NtUserShowWindow(HWND window, LONG command)
{
	return raven_real_ShowWindow(window, command);
}

HWND NTAPI NtUserSetParent(HWND child, HWND parent)
{
	return raven_real_SetParent(child, parent);
}

BOOLEAN NTAPI NtUserDestroyWindow(HWND window)
{
	return raven_real_DestroyWindow(window);
}

BOOL NTAPI NtUserDestroyMenu(HMENU menu)
{
	return raven_real_DestroyMenu(menu);
}

BOOL NTAPI NtUserFlashWindowEx(PFLASHWINFO info)
{
	return TRUE;
}

UINT_PTR NTAPI NtUserSetTimer(HWND window, UINT_PTR identifier, UINT elapse, TIMERPROC procedure)
{
	return raven_real_SetTimer(window, identifier, elapse, procedure);
}

UINT_PTR NTAPI NtUserSetSystemTimer(HWND window, UINT_PTR identifier, UINT elapse, TIMERPROC procedure)
{
	return raven_real_SetSystemTimer(window, identifier, elapse, procedure);
}

BOOL NTAPI NtUserKillTimer(HWND window, UINT_PTR identifier)
{
	return raven_real_KillTimer(window, identifier);
}

BOOL NTAPI NtUserCreateCaret(HWND window, HBITMAP bitmap, int width, int height)
{
	return raven_real_CreateCaret(window, bitmap, width, height);
}

BOOL NTAPI NtUserShowCaret(HWND window)
{
	return raven_real_ShowCaret(window);
}

BOOL NTAPI NtUserHideCaret(HWND window)
{
	return raven_real_HideCaret(window);
}

BOOL NTAPI NtUserSystemParametersInfo(UINT action, UINT parameter, PVOID value, UINT flags)
{
	return SystemParametersInfoW(action, parameter, value, flags);
}

BOOL NTAPI NtUserGetComboBoxInfo(HWND window, PCOMBOBOXINFO info)
{
	return (BOOL)SendMessageW(window, CB_GETCOMBOBOXINFO, 0, (LPARAM)info);
}

BOOL NTAPI NtUserIsClipboardFormatAvailable(UINT format)
{
	return raven_real_IsClipboardFormatAvailable(format);
}

BOOL NTAPI NtUserEmptyClipboard(VOID)
{
	return raven_real_EmptyClipboard();
}

BOOL NTAPI NtUserCloseClipboard(VOID)
{
	return raven_real_CloseClipboard();
}

/* The thread-state queries only feed dialog activation heuristics; this process has one input queue. */
DWORD_PTR NTAPI NtUserGetThreadState(DWORD routine)
{
	return routine == THREADSTATE_FOREGROUNDTHREAD;
}

DWORD NTAPI NtUserSetThreadState(DWORD flags, DWORD mask)
{
	return 0;
}

DWORD_PTR NTAPI NtUserCallNoParam(DWORD routine)
{
	return 0;
}

DWORD_PTR NTAPI NtUserCallOneParam(DWORD_PTR parameter, DWORD routine)
{
	return 0;
}

/* The message box converts its text with this ntdll helper, which the provider does not export. */
BOOLEAN NTAPI RtlCreateUnicodeStringFromAsciiz(PUNICODE_STRING destination, PCSZ source)
{
	int count = MultiByteToWideChar(CP_ACP, 0, source, -1, NULL, 0);
	WCHAR *buffer;
	if (count <= 0)
		return FALSE;
	buffer = HeapAlloc(GetProcessHeap(), 0, count * sizeof(WCHAR));
	if (!buffer)
		return FALSE;
	MultiByteToWideChar(CP_ACP, 0, source, -1, buffer, count);
	destination->Buffer = buffer;
	destination->Length = (USHORT)((count - 1) * sizeof(WCHAR));
	destination->MaximumLength = (USHORT)(count * sizeof(WCHAR));
	return TRUE;
}

VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING destination, PCWSTR source)
{
	size_t length = source ? lstrlenW(source) * sizeof(WCHAR) : 0;
	destination->Buffer = (PWSTR)source;
	destination->Length = (USHORT)length;
	destination->MaximumLength = source ? (USHORT)(length + sizeof(WCHAR)) : 0;
}

VOID NTAPI RtlFreeUnicodeString(PUNICODE_STRING string)
{
	if (string->Buffer)
		HeapFree(GetProcessHeap(), 0, string->Buffer);
	string->Buffer = NULL;
	string->Length = string->MaximumLength = 0;
}

/* No service session exists, so the message box never needs the kernel's hard error path. */
NTSTATUS NTAPI NtRaiseHardError(NTSTATUS error, ULONG count, ULONG mask, PULONG_PTR parameters, ULONG options, PULONG response)
{
	return (NTSTATUS)0xC0000002;
}

DWORD_PTR NTAPI NtUserCallHwnd(HWND window, DWORD routine)
{
	return 0;
}

BOOL NTAPI NtUserDeleteMenu(HMENU menu, UINT position, UINT flags)
{
	return raven_real_DeleteMenu(menu, position, flags);
}

DWORD NTAPI NtUserCallHwndParam(HWND window, DWORD_PTR parameter, DWORD routine)
{
	/* The dialog manager keeps its per-window state behind a pointer the window system would own. */
	if (routine == HWNDPARAM_ROUTINE_SETDIALOGPOINTER) {
		if (parameter)
			SetPropW(window, dialog_property, (HANDLE)parameter);
		else
			RemovePropW(window, dialog_property);
	}
	return 0;
}

DWORD_PTR NTAPI NtUserQueryWindow(HWND window, DWORD index)
{
	DWORD process = 0;
	if (index == QUERY_WINDOW_FOREGROUND)
		return raven_real_GetForegroundWindow() == GetAncestor(window, GA_ROOT);
	if (index == QUERY_WINDOW_UNIQUE_PROCESS_ID) {
		GetWindowThreadProcessId(window, &process);
		return process;
	}
	return 0;
}

/* The class window procedures ask the parent for a brush and fall back to the system colors. */
HBRUSH DefWndControlColor(HDC device, UINT type)
{
	if (type == CTLCOLOR_SCROLLBAR) {
		COLORREF background = GetSysColor(COLOR_3DHILIGHT);
		SetTextColor(device, GetSysColor(COLOR_3DFACE));
		SetBkColor(device, background);
		if (background == GetSysColor(COLOR_WINDOW))
			return gpsi->hbrGray;
		return GetSysColorBrush(COLOR_SCROLLBAR);
	}
	SetTextColor(device, GetSysColor(COLOR_WINDOWTEXT));
	if (type == CTLCOLOR_EDIT || type == CTLCOLOR_LISTBOX) {
		SetBkColor(device, GetSysColor(COLOR_WINDOW));
		return GetSysColorBrush(COLOR_WINDOW);
	}
	SetBkColor(device, GetSysColor(COLOR_3DFACE));
	return GetSysColorBrush(COLOR_3DFACE);
}

HBRUSH NTAPI NtUserGetControlColor(HWND parent, HWND window, HDC device, UINT message)
{
	HBRUSH brush;
	if (!parent)
		parent = GetParent(window);
	brush = parent ? (HBRUSH)SendMessageW(parent, message, (WPARAM)device, (LPARAM)window) : NULL;
	if (!brush || GetObjectType(brush) != OBJ_BRUSH)
		brush = DefWndControlColor(device, message - WM_CTLCOLORMSGBOX);
	return brush;
}

HBRUSH NTAPI NtUserGetControlBrush(HWND window, HDC device, UINT message)
{
	return NtUserGetControlColor(NULL, window, device, message);
}

/* Controls that share their parent's device context clip to their own rectangle. */
HRGN set_control_clipping(HDC device, const RECT *rectangle)
{
	RECT clip = *rectangle;
	HRGN region = CreateRectRgn(0, 0, 0, 0);
	if (GetClipRgn(device, region) != 1) {
		DeleteObject(region);
		region = 0;
	}
	DPtoLP(device, (POINT *)&clip, 2);
	if (GetLayout(device) & LAYOUT_RTL) {
		clip.left++;
		clip.right++;
	}
	IntersectClipRect(device, clip.left, clip.top, clip.right, clip.bottom);
	return region;
}

BOOL get_icon_size(HICON icon, SIZE *size)
{
	ICONINFO information;
	BITMAP bitmap;
	HBITMAP source;
	if (!GetIconInfo(icon, &information))
		return FALSE;
	source = information.hbmColor ? information.hbmColor : information.hbmMask;
	if (!GetObjectW(source, sizeof bitmap, &bitmap)) {
		if (information.hbmColor)
			DeleteObject(information.hbmColor);
		DeleteObject(information.hbmMask);
		return FALSE;
	}
	size->cx = bitmap.bmWidth;
	/* A monochrome icon stacks its AND and XOR masks in one bitmap. */
	size->cy = information.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
	if (information.hbmColor)
		DeleteObject(information.hbmColor);
	DeleteObject(information.hbmMask);
	return TRUE;
}

HWND *WIN_ListChildren(HWND parent)
{
	HWND *list;
	UINT count = 0, index = 0;
	HWND child;
	for (child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
		++count;
	list = HeapAlloc(GetProcessHeap(), 0, (count + 1) * sizeof(HWND));
	if (!list)
		return NULL;
	for (child = GetWindow(parent, GW_CHILD); child && index < count; child = GetWindow(child, GW_HWNDNEXT))
		list[index++] = child;
	list[index] = NULL;
	return list;
}

BOOL FASTCALL TestWindowProcess(PWND window)
{
	return TRUE;
}

VOID FASTCALL IntNotifyWinEvent(DWORD event, HWND window, LONG object, LONG child, DWORD flags)
{
}

PTHREADINFO GetW32ThreadInfo(VOID)
{
	return NULL;
}

/* No API hooking: user hooks are never installed, so every control and dialog call runs directly. */
BOOL FASTCALL BeginIfHookedUserApiHook(VOID)
{
	return FALSE;
}

BOOL FASTCALL EndUserApiHook(VOID)
{
	return FALSE;
}

BOOL FASTCALL IsInsideUserApiHook(VOID)
{
	return FALSE;
}

const char *SPY_GetMsgName(UINT message, HWND window)
{
	return "";
}

/* No input method exists: contexts cannot be created and every composition request reports that. */
static HIMC WINAPI no_context(HWND window) { return NULL; }
static BOOL WINAPI no_release(HWND window, HIMC context) { return TRUE; }
static BOOL WINAPI no_ime(HKL layout) { return FALSE; }
static LONG WINAPI no_string(HIMC context, DWORD index, LPVOID buffer, DWORD length) { return IMM_ERROR_NODATA; }
static BOOL WINAPI no_window(HIMC context, LPCOMPOSITIONFORM form) { return FALSE; }
static BOOL WINAPI no_get_window(HIMC context, LPCOMPOSITIONFORM form) { return FALSE; }
static BOOL WINAPI no_font(HIMC context, LPLOGFONTW font) { return FALSE; }
static PINPUTCONTEXT WINAPI no_lock(HIMC context) { return NULL; }
static BOOL WINAPI no_unlock(HIMC context) { return FALSE; }
static BOOL WINAPI no_notify(HIMC context, DWORD action, DWORD index, DWORD value) { return FALSE; }

BOOL WINAPI RavenInitControls(VOID);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		static const WORD gray_bits[8] = {0x5555, 0xaaaa, 0x5555, 0xaaaa, 0x5555, 0xaaaa, 0x5555, 0xaaaa};
		HBITMAP pattern = CreateBitmap(8, 8, 1, 1, gray_bits);
		User32Instance = instance;
		DisableThreadLibraryCalls(instance);
		if (pattern) {
			gpsi->hbrGray = CreatePatternBrush(pattern);
			DeleteObject(pattern);
		}
		gpsi->PUSIFlags = PUSIF_KEYBOARDCUES;
		gImmApiEntries.pImmGetContext = (FN_ImmGetContext)no_context;
		gImmApiEntries.pImmReleaseContext = (FN_ImmReleaseContext)no_release;
		gImmApiEntries.pImmIsIME = (FN_ImmIsIME)no_ime;
		gImmApiEntries.pImmGetCompositionStringW = (FN_ImmGetCompositionStringW)no_string;
		gImmApiEntries.pImmSetCompositionWindow = (FN_ImmSetCompositionWindow)no_window;
		gImmApiEntries.pImmGetCompositionWindow = (FN_ImmGetCompositionWindow)no_get_window;
		gImmApiEntries.pImmSetCompositionFontW = (FN_ImmSetCompositionFontW)no_font;
		gImmApiEntries.pImmLockIMC = (FN_ImmLockIMC)no_lock;
		gImmApiEntries.pImmUnlockIMC = (FN_ImmUnlockIMC)no_unlock;
		gImmApiEntries.pImmNotifyIME = (FN_ImmNotifyIME)no_notify;
		/* Registering here, ahead of every other library, lets later libraries subclass the built-in classes. */
		if (!RavenInitControls())
			return FALSE;
	}
	return TRUE;
}

/* The hook loader asks whether the thread runs inside a loader callout; this component never loads hooks. */
BOOLEAN NTAPI RtlIsThreadWithinLoaderCallout(VOID)
{
	return FALSE;
}
