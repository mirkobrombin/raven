/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: Windows Runtime classes registered in the registry, as the runtime finds in-process classes on Windows.
 *
 * A class is looked up under HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\WindowsRuntime\ActivatableClassId\<class>, whose DllPath value names
 * the library that serves it; the library is loaded and its DllGetActivationFactory asked for the factory of the class. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>

typedef HRESULT (WINAPI *factory_function)(HSTRING, IActivationFactory **);

static const IID activation_factory = {0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* A class nobody registered is named in Raven's trace, so the classes a program misses can be found. */
static void trace_unregistered(HSTRING name)
{
	static void (WINAPI *raven_trace)(const char *);
	static BOOL looked;
	char line[200] = "winrt: no class ";
	size_t at = 16;
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	if (!looked) {
		raven_trace = (void (WINAPI *)(const char *))GetProcAddress(LoadLibraryW(L"ravenhost.dll"), "RavenTrace");
		looked = TRUE;
	}
	if (!raven_trace)
		return;
	for (UINT32 index = 0; index < length && at < ARRAYSIZE(line) - 1; ++index)
		line[at++] = text[index] < 0x80 ? (char)text[index] : '?';
	line[at] = 0;
	raven_trace(line);
}

/* The library registered for a class, with environment variables expanded; FALSE when the class is not registered. */
static BOOL class_library(HSTRING name, WCHAR *library, DWORD capacity)
{
	static const WCHAR root[] = L"SOFTWARE\\Microsoft\\WindowsRuntime\\ActivatableClassId\\";
	WCHAR key[MAX_PATH], raw[MAX_PATH];
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	if (!length || length >= MAX_PATH - ARRAYSIZE(root))
		return FALSE;
	lstrcpyW(key, root);
	lstrcpynW(key + ARRAYSIZE(root) - 1, text, (int)length + 1);
	DWORD type = 0, size = sizeof raw;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DllPath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, raw, &size))
		return FALSE;
	if (type == REG_EXPAND_SZ)
		return ExpandEnvironmentStringsW(raw, library, capacity) != 0;
	lstrcpynW(library, raw, (int)capacity);
	return TRUE;
}

HRESULT WINAPI RoGetActivationFactory(HSTRING name, REFIID iid, void **out)
{
	WCHAR library[MAX_PATH];
	IActivationFactory *factory;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!name)
		return E_INVALIDARG;
	if (!class_library(name, library, MAX_PATH)) {
		trace_unregistered(name);
		return REGDB_E_CLASSNOTREG;
	}
	/* The library stays loaded for the objects it makes, as the runtime keeps it until the apartment ends. */
	HMODULE module = LoadLibraryExW(library, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module)
		return CO_E_DLLNOTFOUND;
	factory_function get = (factory_function)GetProcAddress(module, "DllGetActivationFactory");
	if (!get)
		return CO_E_ERRORINDLL;
	HRESULT result = get(name, &factory);
	if (FAILED(result))
		return result;
	result = IActivationFactory_QueryInterface(factory, iid, out);
	IActivationFactory_Release(factory);
	return result;
}

HRESULT WINAPI RoActivateInstance(HSTRING name, IInspectable **out)
{
	IActivationFactory *factory;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	HRESULT result = RoGetActivationFactory(name, &activation_factory, (void **)&factory);
	if (FAILED(result))
		return result;
	result = IActivationFactory_ActivateInstance(factory, out);
	IActivationFactory_Release(factory);
	return result;
}
