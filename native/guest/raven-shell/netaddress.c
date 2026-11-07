/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* The network address control (msctls_netaddress) that InitNetworkAddressControl registers: an edit field that reads its text as a host
 * name, an IPv4 or an IPv6 address, each with an optional port, and answers the NCM_ messages of the public headers. Which forms it
 * accepts is a mask of NET_STRING_ types the program sets. */
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <windns.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <string.h>

static WNDPROC edit_procedure;
static const WCHAR allow_property[] = L"RavenNetAddressAllow";

static BOOL digit(WCHAR c) { return c >= '0' && c <= '9'; }

static int hex_value(WCHAR c)
{
	if (digit(c))
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* A decimal number of up to `limit` from text[from, to), or -1. */
static long number(const WCHAR *text, int from, int to, long limit)
{
	long value = 0;
	int at;
	if (from >= to || to - from > 5)
		return -1;
	for (at = from; at < to; ++at) {
		if (!digit(text[at]))
			return -1;
		value = value * 10 + (text[at] - '0');
	}
	return value <= limit ? value : -1;
}

static BOOL parse_ipv4(const WCHAR *text, int from, int to, BYTE address[4])
{
	int part = 0, start = from, at;
	for (at = from; at <= to; ++at)
		if (at == to || text[at] == '.') {
			const long value = number(text, start, at, 255);
			if (value < 0 || part > 3)
				return FALSE;
			address[part++] = (BYTE)value;
			start = at + 1;
		}
	return part == 4;
}

static BOOL parse_ipv6(const WCHAR *text, int from, int to, BYTE address[16])
{
	USHORT groups[8] = {0};
	int count = 0, gap = -1, at = from, index;
	if (to - from >= 2 && text[from] == ':' && text[from + 1] == ':') {
		gap = 0;
		at += 2;
	}
	while (at < to) {
		int value = 0, length = 0;
		while (at < to && hex_value(text[at]) >= 0 && length < 4)
			value = value * 16 + hex_value(text[at++]), ++length;
		if (!length || count == 8)
			return FALSE;
		groups[count++] = (USHORT)value;
		if (at == to)
			break;
		if (text[at] != ':')
			return FALSE;
		++at;
		if (at < to && text[at] == ':') {
			if (gap >= 0)
				return FALSE;
			gap = count;
			++at;
		} else if (at == to)
			return FALSE;
	}
	if (gap < 0 ? count != 8 : count > 7)
		return FALSE;
	memset(address, 0, 16);
	for (index = 0; index < count; ++index) {
		const int slot = gap >= 0 && index >= gap ? index + 8 - count : index;
		address[slot * 2] = (BYTE)(groups[index] >> 8);
		address[slot * 2 + 1] = (BYTE)groups[index];
	}
	return TRUE;
}

static BOOL host_name(const WCHAR *text, int from, int to)
{
	int at;
	if (from >= to || to - from >= DNS_MAX_NAME_BUFFER_LENGTH || text[from] == '.' || text[from] == '-')
		return FALSE;
	for (at = from; at < to; ++at) {
		const WCHAR c = text[at];
		if (!(digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_'))
			return FALSE;
	}
	return TRUE;
}

/* Reads the text as one of the forms the mask allows; fills the answer when the program passes one. */
static HRESULT parse_address(const WCHAR *text, DWORD allow, NC_ADDRESS *answer)
{
	NET_ADDRESS_INFO info;
	int from = 0, to = lstrlenW(text), host_end, at;
	long port = -1;
	BYTE address[16];
	while (from < to && (text[from] == ' ' || text[from] == '\t'))
		++from;
	while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t'))
		--to;
	memset(&info, 0, sizeof(info));
	if (from == to)
		return E_INVALIDARG;
	/* [IPv6]:port */
	if (text[from] == '[') {
		for (at = from; at < to && text[at] != ']'; ++at)
			;
		if (at == to || !parse_ipv6(text, from + 1, at, address))
			return E_INVALIDARG;
		if (at + 1 < to) {
			if (text[at + 1] != ':' || (port = number(text, at + 2, to, 65535)) < 0)
				return E_INVALIDARG;
		}
		if (!(allow & (port >= 0 ? (NET_STRING_IPV6_SERVICE | NET_STRING_IPV6_SERVICE_NO_SCOPE) : (NET_STRING_IPV6_ADDRESS | NET_STRING_IPV6_ADDRESS_NO_SCOPE))))
			return E_INVALIDARG;
		info.Format = NET_ADDRESS_IPV6;
		info.Ipv6Address.sin6_family = AF_INET6;
		memcpy(&info.Ipv6Address.sin6_addr, address, 16);
	} else if (parse_ipv6(text, from, to, address)) {
		if (!(allow & (NET_STRING_IPV6_ADDRESS | NET_STRING_IPV6_ADDRESS_NO_SCOPE)))
			return E_INVALIDARG;
		info.Format = NET_ADDRESS_IPV6;
		info.Ipv6Address.sin6_family = AF_INET6;
		memcpy(&info.Ipv6Address.sin6_addr, address, 16);
	} else {
		host_end = to;
		for (at = from; at < to; ++at)
			if (text[at] == ':') {
				host_end = at;
				if ((port = number(text, at + 1, to, 65535)) < 0)
					return E_INVALIDARG;
				break;
			}
		if (parse_ipv4(text, from, host_end, address)) {
			if (!(allow & (port >= 0 ? NET_STRING_IPV4_SERVICE : NET_STRING_IPV4_ADDRESS)))
				return E_INVALIDARG;
			info.Format = NET_ADDRESS_IPV4;
			info.Ipv4Address.sin_family = AF_INET;
			memcpy(&info.Ipv4Address.sin_addr, address, 4);
		} else if (host_name(text, from, host_end)) {
			if (!(allow & (port >= 0 ? NET_STRING_NAMED_SERVICE : NET_STRING_NAMED_ADDRESS)))
				return E_INVALIDARG;
			info.Format = NET_ADDRESS_DNS_NAME;
			memcpy(info.NamedAddress.Address, text + from, (host_end - from) * sizeof(WCHAR));
			if (port >= 0)
				memcpy(info.NamedAddress.Port, text + host_end + 1, (to - host_end - 1) * sizeof(WCHAR));
		} else
			return E_INVALIDARG;
	}
	if (port >= 0 && info.Format == NET_ADDRESS_IPV4)
		info.Ipv4Address.sin_port = (USHORT)(((port & 0xff) << 8) | (port >> 8));
	else if (port >= 0 && info.Format == NET_ADDRESS_IPV6)
		info.Ipv6Address.sin6_port = (USHORT)(((port & 0xff) << 8) | (port >> 8));
	if (answer) {
		if (answer->pAddrInfo)
			*answer->pAddrInfo = info;
		answer->PortNumber = port >= 0 ? (USHORT)port : 0;
		answer->PrefixLength = 0;
	}
	return S_OK;
}

static LRESULT CALLBACK address_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case NCM_GETADDRESS: {
		const DWORD allow = (DWORD)(ULONG_PTR)GetPropW(window, allow_property);
		const int length = GetWindowTextLengthW(window);
		WCHAR *text = HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
		HRESULT result;
		if (!text)
			return E_OUTOFMEMORY;
		GetWindowTextW(window, text, length + 1);
		result = parse_address(text, allow ? allow : NET_STRING_ANY_ADDRESS | NET_STRING_ANY_SERVICE, (NC_ADDRESS *)lparam);
		HeapFree(GetProcessHeap(), 0, text);
		return result;
	}
	case NCM_SETALLOWTYPE:
		SetPropW(window, allow_property, (HANDLE)(ULONG_PTR)wparam);
		return S_OK;
	case NCM_GETALLOWTYPE: {
		const DWORD allow = (DWORD)(ULONG_PTR)GetPropW(window, allow_property);
		return allow ? allow : NET_STRING_ANY_ADDRESS | NET_STRING_ANY_SERVICE;
	}
	case NCM_DISPLAYERRORTIP:
		MessageBeep(MB_ICONWARNING);
		return S_OK;
	case WM_NCDESTROY:
		RemovePropW(window, allow_property);
		break;
	}
	return CallWindowProcW(edit_procedure, window, message, wparam, lparam);
}

BOOL WINAPI InitNetworkAddressControl(void)
{
	WNDCLASSEXW type = {sizeof(type)};
	HMODULE module = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address_procedure, &module);
	if (GetClassInfoExW(module, WC_NETADDRESS, &type))
		return TRUE;
	if (!GetClassInfoExW(NULL, L"Edit", &type))
		return FALSE;
	edit_procedure = type.lpfnWndProc;
	type.lpfnWndProc = address_procedure;
	type.hInstance = module;
	type.lpszClassName = WC_NETADDRESS;
	type.style |= CS_GLOBALCLASS;
	return RegisterClassExW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}
