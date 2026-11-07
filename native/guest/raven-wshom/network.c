/* SPDX-License-Identifier: MIT */
#include "wshom.h"
#include <winnetwk.h>

/* WScript.Network: the names of the computer, the user and the domain, the network drives and printers connected, and connecting or
 * disconnecting them. The network calls are looked up when used, so the library starts where mpr.dll or winspool.drv is missing. */

enum {
	NET_COMPUTER = 1, NET_USER, NET_DOMAIN, NET_ORGANIZATION, NET_SITE, NET_ENUMDRIVES, NET_ENUMPRINTERS, NET_MAPDRIVE, NET_REMOVEDRIVE,
	NET_ADDPRINTER, NET_ADDWINDOWSPRINTER, NET_REMOVEPRINTER, NET_DEFAULTPRINTER,
};

static const sr_member members[] = {
	{ L"ComputerName", NET_COMPUTER, 0 }, { L"UserName", NET_USER, 0 }, { L"UserDomain", NET_DOMAIN, 0 },
	{ L"Organization", NET_ORGANIZATION, 0 }, { L"Site", NET_SITE, 0 }, { L"EnumNetworkDrives", NET_ENUMDRIVES, 0 },
	{ L"EnumPrinterConnections", NET_ENUMPRINTERS, 0 }, { L"MapNetworkDrive", NET_MAPDRIVE, 5 }, { L"RemoveNetworkDrive", NET_REMOVEDRIVE, 3 },
	{ L"AddPrinterConnection", NET_ADDPRINTER, 5 }, { L"AddWindowsPrinterConnection", NET_ADDWINDOWSPRINTER, 3 },
	{ L"RemovePrinterConnection", NET_REMOVEPRINTER, 3 }, { L"SetDefaultPrinter", NET_DEFAULTPRINTER, 1 }, { NULL },
};

static FARPROC network_call(const WCHAR *library, const char *name)
{
	HMODULE module = GetModuleHandleW(library);
	if (!module)
		module = LoadLibraryW(library);
	return module ? GetProcAddress(module, name) : NULL;
}

static HRESULT win32(DWORD error) { return error ? HRESULT_FROM_WIN32(error) : S_OK; }

/* The connected resources of one type as the pairs the collection lists: the local name, then the remote one. */
static HRESULT connections(DWORD type, VARIANT *result)
{
	typedef DWORD (WINAPI *open_fn)(DWORD, DWORD, DWORD, LPNETRESOURCEW, LPHANDLE);
	typedef DWORD (WINAPI *enum_fn)(HANDLE, LPDWORD, LPVOID, LPDWORD);
	typedef DWORD (WINAPI *close_fn)(HANDLE);
	open_fn open = (open_fn)(void *)network_call(L"mpr.dll", "WNetOpenEnumW");
	enum_fn next = (enum_fn)(void *)network_call(L"mpr.dll", "WNetEnumResourceW");
	close_fn close = (close_fn)(void *)network_call(L"mpr.dll", "WNetCloseEnum");
	VARIANT *items = NULL;
	ULONG count = 0, room = 0;
	HANDLE handle;
	if (open && next && close && !open(RESOURCE_CONNECTED, type, 0, NULL, &handle)) {
		DWORD size = 16384;
		NETRESOURCEW *resources = sr_alloc(size);
		for (;;) {
			DWORD found = (DWORD)-1, buffer = size;
			if (next(handle, &found, resources, &buffer))
				break;
			for (DWORD i = 0; i < found; i++) {
				if (count + 2 > room) {
					room = room ? room * 2 : 16;
					items = sr_grow(items, room * sizeof(VARIANT));
				}
				V_VT(&items[count]) = VT_BSTR;
				V_BSTR(&items[count++]) = SysAllocString(resources[i].lpLocalName ? resources[i].lpLocalName : L"");
				V_VT(&items[count]) = VT_BSTR;
				V_BSTR(&items[count++]) = SysAllocString(resources[i].lpRemoteName ? resources[i].lpRemoteName : L"");
			}
		}
		sr_free(resources);
		close(handle);
	}
	IDispatch *collection = wsh_collection_new(items, NULL, count);
	for (ULONG i = 0; i < count; i++)
		VariantClear(&items[i]);
	sr_free(items);
	return sr_return_object(result, collection);
}

/* MapNetworkDrive and AddPrinterConnection: a local name, the remote one, whether to remember it, and the user and password to log on. */
static HRESULT connect_resource(DWORD type, DISPPARAMS *params, WORD flags)
{
	typedef DWORD (WINAPI *add_fn)(LPNETRESOURCEW, LPCWSTR, LPCWSTR, DWORD);
	add_fn add = (add_fn)(void *)network_call(L"mpr.dll", "WNetAddConnection2W");
	BSTR local = NULL, remote = NULL, user = NULL, password = NULL;
	BOOL remember = FALSE;
	HRESULT hr;
	if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &local)) && SUCCEEDED(hr = sr_text(sr_arg(params, flags, 1), &remote)) &&
		SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 2), FALSE, &remember))) {
		if (sr_arg(params, flags, 3))
			hr = sr_text(sr_arg(params, flags, 3), &user);
		if (SUCCEEDED(hr) && sr_arg(params, flags, 4))
			hr = sr_text(sr_arg(params, flags, 4), &password);
	}
	if (SUCCEEDED(hr)) {
		NETRESOURCEW resource = { 0 };
		resource.dwType = type;
		resource.lpLocalName = local;
		resource.lpRemoteName = remote;
		hr = add ? win32(add(&resource, password, user, remember ? CONNECT_UPDATE_PROFILE : 0)) : win32(ERROR_NOT_SUPPORTED);
	}
	SysFreeString(local);
	SysFreeString(remote);
	SysFreeString(user);
	SysFreeString(password);
	return hr;
}

/* RemoveNetworkDrive and RemovePrinterConnection: the name, whether to force it, whether to forget it. */
static HRESULT disconnect_resource(DISPPARAMS *params, WORD flags)
{
	typedef DWORD (WINAPI *cancel_fn)(LPCWSTR, DWORD, BOOL);
	cancel_fn cancel = (cancel_fn)(void *)network_call(L"mpr.dll", "WNetCancelConnection2W");
	BSTR name = NULL;
	BOOL force = FALSE, forget = FALSE;
	HRESULT hr;
	if (SUCCEEDED(hr = sr_text(sr_arg(params, flags, 0), &name)) && SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 1), FALSE, &force)) &&
		SUCCEEDED(hr = sr_bool_or(sr_arg(params, flags, 2), FALSE, &forget)))
		hr = cancel ? win32(cancel(name, forget ? CONNECT_UPDATE_PROFILE : 0, force)) : win32(ERROR_NOT_SUPPORTED);
	SysFreeString(name);
	return hr;
}

static HRESULT printer_call(const char *name, DISPPARAMS *params, WORD flags)
{
	typedef BOOL (WINAPI *printer_fn)(LPWSTR);
	printer_fn call = (printer_fn)(void *)network_call(L"winspool.drv", name);
	BSTR printer = NULL;
	HRESULT hr = sr_text(sr_arg(params, flags, 0), &printer);
	if (SUCCEEDED(hr))
		hr = !call ? win32(ERROR_NOT_SUPPORTED) : call(printer) ? S_OK : win32(GetLastError());
	SysFreeString(printer);
	return hr;
}

static HRESULT network_invoke(sr_object *self, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result)
{
	WCHAR name[256];
	DWORD size = ARRAYSIZE(name);
	if (sr_is_put(flags))
		return DISP_E_MEMBERNOTFOUND;
	switch (id) {
	case NET_COMPUTER:
		if (!GetComputerNameW(name, &size))
			return win32(GetLastError());
		return sr_return_text(result, name);
	case NET_USER:
		if (!GetUserNameW(name, &size))
			return win32(GetLastError());
		return sr_return_text(result, name);
	case NET_DOMAIN:
		/* The domain the user logged on to; a computer outside a domain is its own. */
		if (!GetEnvironmentVariableW(L"USERDOMAIN", name, ARRAYSIZE(name)) && !GetComputerNameW(name, &size))
			return win32(GetLastError());
		return sr_return_text(result, name);
	case NET_ORGANIZATION:
	case NET_SITE:
		return sr_return_text(result, L"");
	case NET_ENUMDRIVES:
		return connections(RESOURCETYPE_DISK, result);
	case NET_ENUMPRINTERS:
		return connections(RESOURCETYPE_PRINT, result);
	case NET_MAPDRIVE:
		return connect_resource(RESOURCETYPE_DISK, params, flags);
	case NET_ADDPRINTER:
		return connect_resource(RESOURCETYPE_PRINT, params, flags);
	case NET_REMOVEDRIVE:
		return disconnect_resource(params, flags);
	case NET_REMOVEPRINTER: {
		/* A printer connected by its \\server\name goes through the spooler, one connected to a port through the network. */
		BSTR printer = NULL;
		HRESULT hr = sr_text(sr_arg(params, flags, 0), &printer);
		BOOL spooled = SUCCEEDED(hr) && printer[0] == L'\\';
		SysFreeString(printer);
		if (FAILED(hr))
			return hr;
		return spooled ? printer_call("DeletePrinterConnectionW", params, flags) : disconnect_resource(params, flags);
	}
	case NET_ADDWINDOWSPRINTER:
		return printer_call("AddPrinterConnectionW", params, flags);
	case NET_DEFAULTPRINTER:
		return printer_call("SetDefaultPrinterW", params, flags);
	}
	return DISP_E_MEMBERNOTFOUND;
}

static const sr_class network_class = { members, network_invoke, NULL };

IDispatch *wsh_network_new(void) { return sr_dispatch(sr_object_new(&network_class, sizeof(sr_object))); }
