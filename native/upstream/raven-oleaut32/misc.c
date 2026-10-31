/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "oa.h"

HCURSOR WINAPI OleIconToCursor(HINSTANCE instance, HICON icon)
{
	return (HCURSOR)CopyImage(icon, IMAGE_CURSOR, 0, 0, 0);
}

/* The property sheet frame is not provided; callers treat this as no pages to show. */
HRESULT WINAPI OleCreatePropertyFrameIndirect(OCPFIPARAMS *params)
{
	return E_NOTIMPL;
}

HRESULT WINAPI OleCreatePropertyFrame(HWND owner, UINT x, UINT y, LPCOLESTR caption, ULONG objects, IUnknown **units, ULONG pages, CLSID *page_ids, LCID lcid, DWORD reserved, void *reserved2)
{
	OCPFIPARAMS params;
	memset(&params, 0, sizeof(params));
	params.cbStructSize = sizeof(params);
	params.hWndOwner = owner;
	params.x = x;
	params.y = y;
	params.lpszCaption = caption;
	params.cObjects = objects;
	params.lplpUnk = units;
	params.cPages = pages;
	params.lpPages = page_ids;
	params.lcid = lcid;
	return OleCreatePropertyFrameIndirect(&params);
}

HRESULT WINAPI OleLoadPictureFile(VARIANT file, IDispatch **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (V_VT(&file) != VT_BSTR)
		return CTL_E_FILENOTFOUND;
	return OleLoadPicturePath(V_BSTR(&file), NULL, 0, 0, &IID_IDispatch, (void **)out);
}

HRESULT WINAPI OleSavePictureFile(IDispatch *picture, BSTR file)
{
	IPicture *object = NULL;
	IStream *stream = NULL;
	HANDLE target;
	HGLOBAL memory = NULL;
	LONG written = 0;
	HRESULT status;
	if (!picture || !file)
		return E_POINTER;
	status = IDispatch_QueryInterface(picture, &IID_IPicture, (void **)&object);
	if (FAILED(status))
		return status;
	status = CreateStreamOnHGlobal(NULL, TRUE, &stream);
	if (SUCCEEDED(status))
		status = IPicture_SaveAsFile(object, stream, TRUE, &written);
	if (SUCCEEDED(status))
		status = GetHGlobalFromStream(stream, &memory);
	if (SUCCEEDED(status))
	{
		target = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (target == INVALID_HANDLE_VALUE)
			status = CTL_E_FILENOTFOUND;
		else
		{
			DWORD put = 0;
			WriteFile(target, GlobalLock(memory), written, &put, NULL);
			GlobalUnlock(memory);
			CloseHandle(target);
		}
	}
	if (stream)
		IStream_Release(stream);
	IPicture_Release(object);
	return status;
}

static void register_class(const WCHAR *clsid, const WCHAR *name)
{
	WCHAR path[80];
	HKEY key, server;
	wsprintfW(path, L"CLSID\\%s", clsid);
	if (RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL))
		return;
	RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)name, (lstrlenW(name) + 1) * sizeof(WCHAR));
	if (!RegCreateKeyExW(key, L"InprocServer32", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &server, NULL))
	{
		static const WCHAR module[] = L"oleaut32.dll", both[] = L"Both";
		RegSetValueExW(server, NULL, 0, REG_SZ, (const BYTE *)module, sizeof(module));
		RegSetValueExW(server, L"ThreadingModel", 0, REG_SZ, (const BYTE *)both, sizeof(both));
		RegCloseKey(server);
	}
	RegCloseKey(key);
}

HRESULT WINAPI DllRegisterServer(void)
{
	ITypeLib *lib = NULL;
	register_class(L"{0BE35203-8F91-11CE-9DE3-00AA004BB851}", L"Standard Font");
	register_class(L"{0BE35204-8F91-11CE-9DE3-00AA004BB851}", L"Standard Picture");
	if (SUCCEEDED(LoadTypeLib(L"stdole2.tlb", &lib)))
	{
		RegisterTypeLib(lib, (OLECHAR *)L"stdole2.tlb", NULL);
		ITypeLib_Release(lib);
	}
	return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	return S_OK;
}

/* What the export list names but nothing has ever been seen calling. */
HRESULT WINAPI oleaut_missing(void)
{
	return E_NOTIMPL;
}
