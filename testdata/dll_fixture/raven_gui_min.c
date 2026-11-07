// GUI fixture: a minimal PE32 GUI program that drives user32/gdi32 without
// any CRT or COM bootstrap.
//
// It registers a window class, creates a window, runs a GetMessage/Dispatch
// loop, and on the first WM_PAINT paints (FillRect + TextOutW) then quits.
// All Win32 calls land in the real user32/gdi32, which issue real NtUser*/NtGdi*
// syscalls into Raven's win32k bridge - proving the generic real-DLL GUI path.
//
// Build (PE32, no CRT):
//   i686-w64-mingw32-gcc -nostdlib -e _raven_entry \
//     -o raven_gui_min.exe raven_gui_min.c -luser32 -lgdi32 -lkernel32
//
// No msvcrt, no ole32. Own entry point (_raven_entry), no C runtime.

#include <windows.h>

static const wchar_t kClass[] = L"RavenUI";
static int g_painted = 0;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        r.left = 0; r.top = 0; r.right = 400; r.bottom = 300;
        FillRect(dc, &r, (HBRUSH)(COLOR_WINDOW + 1));
        TextOutW(dc, 24, 24, L"Raven UI: real gdi32", 20);
        EndPaint(hwnd, &ps);
        if (!g_painted) {
            g_painted = 1;
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void raven_entry(void)
{
    WNDCLASSW wc;
    int i;
    char *z = (char *)&wc;
    for (i = 0; i < (int)sizeof(wc); i++) z[i] = 0;   // memset, no CRT
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = kClass;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, kClass, L"Raven UI",
        WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, 0, 0, wc.hInstance, 0);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);   // force the first WM_PAINT

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ExitProcess(0);
}
