/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_PROPSYS_PS_H
#define RAVEN_PROPSYS_PS_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <propidl.h>
#include <propsys.h>
#include <propvarutil.h>
#include <propkey.h>

#ifndef STRSAFE_E_INSUFFICIENT_BUFFER
#define STRSAFE_E_INSUFFICIENT_BUFFER ((HRESULT)0x8007007A)
#endif

extern LONG ps_locks;

HRESULT ps_store_create(REFIID riid, void **out);
HRESULT ps_system_get(REFIID riid, void **out);
WCHAR *ps_copy_text(const WCHAR *text);
WCHAR *ps_widen(const char *text);
HRESULT ps_render(const PROPVARIANT *value, WCHAR **out);

#endif
