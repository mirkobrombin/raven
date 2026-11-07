/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven Direct3D 10.1 component: d3d10_1.dll, the library through which programs create a Direct3D 10.1 device.
 *
 * The device itself comes from the d3d10core.dll Raven is given (the DXVK build in RAVEN_D3D_DIRECTORY), whose D3D10CoreCreateDevice
 * makes a device for a DXGI adapter. This library picks the adapter as Windows does for a null one, the first of a new factory, and
 * hands back the 10.1 interface of the device. Only hardware devices exist: there is no reference rasterizer behind them, and a request
 * for WARP, the software rasterizer programs fall back to when told to avoid the GPU, is served by the hardware device. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10_1.h>
#include <d3d10effect.h>

typedef HRESULT (WINAPI *core_create_device)(IDXGIFactory *, IDXGIAdapter *, UINT, D3D_FEATURE_LEVEL, ID3D10Device **);
typedef HRESULT (WINAPI *create_factory)(REFIID, void **);

static const IID device1_iid = {0x9b7e4c8f, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}};
static const IID factory_iid = {0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};

static FARPROC function(const char *library, const char *name)
{
	HMODULE module = LoadLibraryA(library);
	return module ? GetProcAddress(module, name) : NULL;
}

__declspec(dllexport) HRESULT WINAPI D3D10CreateDevice1(IDXGIAdapter *adapter, D3D10_DRIVER_TYPE type, HMODULE software, UINT flags, D3D10_FEATURE_LEVEL1 level,
	UINT version, ID3D10Device1 **result)
{
	core_create_device create = (core_create_device)(void *)function("d3d10core.dll", "D3D10CoreCreateDevice");
	create_factory make_factory = (create_factory)(void *)function("dxgi.dll", "CreateDXGIFactory");
	IDXGIFactory *factory = NULL;
	IDXGIAdapter *chosen = adapter;
	ID3D10Device *device = NULL;
	HRESULT status;
	if (result)
		*result = NULL;
	if (type != D3D10_DRIVER_TYPE_HARDWARE && type != D3D10_DRIVER_TYPE_WARP)
		return DXGI_ERROR_UNSUPPORTED;
	if (!create || !make_factory)
		return DXGI_ERROR_UNSUPPORTED;
	if (adapter)
		status = IDXGIAdapter_GetParent(adapter, &factory_iid, (void **)&factory);
	else {
		status = make_factory(&factory_iid, (void **)&factory);
		if (SUCCEEDED(status))
			status = IDXGIFactory_EnumAdapters(factory, 0, &chosen);
	}
	if (SUCCEEDED(status))
		status = create(factory, chosen, flags, (D3D_FEATURE_LEVEL)level, &device);
	if (chosen && chosen != adapter)
		IDXGIAdapter_Release(chosen);
	if (factory)
		IDXGIFactory_Release(factory);
	if (FAILED(status))
		return status;
	/* Without a place for the device the call only asks whether one can be made. */
	if (!result) {
		ID3D10Device_Release(device);
		return S_FALSE;
	}
	status = ID3D10Device_QueryInterface(device, &device1_iid, (void **)result);
	ID3D10Device_Release(device);
	return status;
}

/* State blocks record the pipeline state of a device; the devices made here have no way to capture it from outside. */
__declspec(dllexport) HRESULT WINAPI D3D10CreateStateBlock(ID3D10Device *device, D3D10_STATE_BLOCK_MASK *mask, ID3D10StateBlock **block)
{
	if (block)
		*block = NULL;
	return E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	return TRUE;
}
