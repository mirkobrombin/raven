/* Independent Direct3D 11 workload with two child windows of one window, each with a DXGI swap chain of its own. The swap chain of the
 * first child is made while that child is still hidden, as programs that build their swap chain before showing the window do; the second
 * child is visible from the start. The first child is cleared to red and the second to green; the first is then shown, and both keep
 * presenting so that a compositor capture finds a red and a green square side by side in the window. Each back buffer is read back once,
 * as the other window workloads do. C:\children.txt reports OK once both have presented after the first child was shown, or the step
 * that failed. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define SIDE 160

static void write_text(const wchar_t *path, const char *text)
{
	HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD n;
	WriteFile(f, text, lstrlenA(text), &n, NULL);
	CloseHandle(f);
}

static void fail(const char *step, HRESULT hr)
{
	char text[96];
	int length = 0;
	const char *prefix = "FAIL ";
	while (*prefix)
		text[length++] = *prefix++;
	while (*step && length < 60)
		text[length++] = *step++;
	text[length++] = ' ';
	for (int shift = 28; shift >= 0; shift -= 4)
		text[length++] = "0123456789abcdef"[((unsigned)hr >> shift) & 15];
	text[length] = 0;
	write_text(L"C:\\children.txt", text);
	ExitProcess(1);
}

static void pump(void)
{
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

static void show(ID3D11DeviceContext *context, IDXGISwapChain *swap, ID3D11RenderTargetView *view, const float *colour, const char *step)
{
	HRESULT hr;
	ID3D11DeviceContext_ClearRenderTargetView(context, view, colour);
	if (FAILED(hr = IDXGISwapChain_Present(swap, 0, 0)))
		fail(step, hr);
}

/* Reads the back buffer back, which waits for the GPU, and requires the colour its first pixel was cleared to. */
static void settle(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11RenderTargetView *view, unsigned expected, const char *step)
{
	ID3D11Resource *back = NULL;
	ID3D11Texture2D *staging = NULL;
	D3D11_TEXTURE2D_DESC td = {SIDE, SIDE, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr;
	ID3D11RenderTargetView_GetResource(view, &back);
	if (FAILED(hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &staging)))
		fail(step, hr);
	ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, back);
	if (FAILED(hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)))
		fail(step, hr);
	const unsigned pixel = *(const unsigned *)mapped.pData;
	ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
	ID3D11Texture2D_Release(staging);
	ID3D11Resource_Release(back);
	if (pixel != expected)
		fail(step, pixel);
}

void __stdcall entry(void)
{
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSEXW class_info = {sizeof class_info, 0, DefWindowProcW, 0, 0, instance, NULL, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), NULL, NULL, L"RavenChildren", NULL};
	RegisterClassExW(&class_info);
	RECT frame = {0, 0, 2 * SIDE + 60, SIDE + 40};
	AdjustWindowRect(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
	HWND window = CreateWindowExW(0, L"RavenChildren", L"Direct3D 11 children", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 100, 100,
		frame.right - frame.left, frame.bottom - frame.top, NULL, NULL, instance, NULL);
	HWND hidden = CreateWindowExW(0, L"RavenChildren", L"", WS_CHILD, 20, 20, SIDE, SIDE, window, NULL, instance, NULL);
	HWND visible = CreateWindowExW(0, L"RavenChildren", L"", WS_CHILD | WS_VISIBLE, SIDE + 40, 20, SIDE, SIDE, window, NULL, instance, NULL);
	if (!window || !hidden || !visible)
		fail("window", 0);
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);
	pump();

	DXGI_SWAP_CHAIN_DESC description = {{SIDE, SIDE, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED},
		{1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, hidden, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
	IDXGISwapChain *first = NULL, *second = NULL;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	D3D_FEATURE_LEVEL level = 0;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &description, &first, &device, &level, &context);
	if (FAILED(hr))
		fail("swap chain of the hidden child", hr);
	IDXGIFactory *factory = NULL;
	if (FAILED(hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory)))
		fail("factory", hr);
	description.OutputWindow = visible;
	if (FAILED(hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &description, &second)))
		fail("swap chain of the visible child", hr);
	ID3D11Texture2D *back = NULL;
	ID3D11RenderTargetView *red_view = NULL, *green_view = NULL;
	if (FAILED(hr = IDXGISwapChain_GetBuffer(first, 0, &IID_ID3D11Texture2D, (void **)&back)) ||
		FAILED(hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &red_view)))
		fail("first view", hr);
	ID3D11Texture2D_Release(back);
	if (FAILED(hr = IDXGISwapChain_GetBuffer(second, 0, &IID_ID3D11Texture2D, (void **)&back)) ||
		FAILED(hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &green_view)))
		fail("second view", hr);
	ID3D11Texture2D_Release(back);

	const float red[4] = {0.8f, 0.1f, 0.1f, 1.0f}, green[4] = {0.1f, 0.7f, 0.2f, 1.0f};
	show(context, first, red_view, red, "present while hidden");
	show(context, second, green_view, green, "present to the visible child");
	settle(device, context, red_view, 0xff1a1accu, "read back the hidden child");
	settle(device, context, green_view, 0xff33b21au, "read back the visible child");
	pump();
	ShowWindow(hidden, SW_SHOW);
	pump();
	for (int frame_number = 0; frame_number < 80; ++frame_number) {
		show(context, first, red_view, red, "present after showing");
		show(context, second, green_view, green, "present beside it");
		pump();
		if (frame_number == 10)
			write_text(L"C:\\children.txt", "OK");
		Sleep(100);
	}
	ExitProcess(0);
}
