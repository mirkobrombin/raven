#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

// A game's start, as Skillshot City makes it: the context is made current on a window that is not shown yet and a first frame is
// swapped there, then the window is shown without activation, the context is made current again and the window is maximized. Every
// later frame must reach the screen, opaque although its alpha is zero. The exit code names the first check that failed.

static void pump(void)
{
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

unsigned int raven_entry(void)
{
	HWND window = CreateWindowExA(0, "STATIC", "Hidden OpenGL start", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, NULL, NULL);
	if (!window || IsWindowVisible(window))
		return 1;
	HDC device = GetDC(window);
	PIXELFORMATDESCRIPTOR format = {0};
	format.nSize = sizeof(format);
	format.nVersion = 1;
	format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	format.iPixelType = PFD_TYPE_RGBA;
	format.cColorBits = 32;
	format.cDepthBits = 24;
	const int index = ChoosePixelFormat(device, &format);
	if (!index || !SetPixelFormat(device, index, &format))
		return 2;
	HGLRC context = wglCreateContext(device);
	if (!context || !wglMakeCurrent(device, context))
		return 3;
	// Windows swaps the buffers of a window that is not shown, and nothing appears.
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	if (!SwapBuffers(device))
		return 4;
	if (!wglMakeCurrent(NULL, NULL))
		return 5;
	ShowWindow(window, SW_SHOWNA);
	if (!wglMakeCurrent(device, context))
		return 6;
	ShowWindow(window, SW_MAXIMIZE);
	pump();
	RECT client;
	if (!GetClientRect(window, &client) || client.right < 320 || client.bottom < 240)
		return 7;
	glViewport(0, 0, client.right, client.bottom);
	// Its frames leave alpha at zero, as the game's do: Windows shows them opaque all the same.
	for (int frame = 0; frame < 10; ++frame) {
		glClearColor(1, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		if (!SwapBuffers(device))
			return 8;
		pump();
		Sleep(20);
	}
	OutputDebugStringA("OpenGL frame presented");
	Sleep(3000);
	if (!wglMakeCurrent(NULL, NULL) || !wglDeleteContext(context) || !DestroyWindow(window))
		return 9;
	return 0;
}
