/* SPDX-License-Identifier: MIT */
#include "scrrun.h"

/* The classes the library makes through COM, and the exports COM looks for. */

static const CLSID CLSID_Dictionary = { 0xee09b103, 0x97e0, 0x11cf, { 0x97, 0x8f, 0x00, 0xa0, 0x24, 0x63, 0xe0, 0x6f } };
static const CLSID CLSID_FileSystemObject = { 0x0d43fe01, 0xf093, 0x11cf, { 0x89, 0x40, 0x00, 0xa0, 0xc9, 0x05, 0x42, 0x28 } };

static const sr_coclass classes[] = {
	{ &CLSID_Dictionary, sr_dictionary_new },
	{ &CLSID_FileSystemObject, sr_file_system_new },
	{ NULL },
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) { return sr_class_object(classes, clsid, riid, out); }
HRESULT WINAPI DllCanUnloadNow(void) { return sr_can_unload() ? S_OK : S_FALSE; }

/* The classes are registered with the rest of the system's; there is nothing to write here. */
HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
