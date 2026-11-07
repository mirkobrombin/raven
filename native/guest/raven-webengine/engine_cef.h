/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* What the parts of the page engine share: the functions taken from the Chromium Embedded Framework's library and the helpers around
 * its strings and references. Only built with the engine. */
#ifndef RAVEN_IEFRAME_ENGINE_CEF_H
#define RAVEN_IEFRAME_ENGINE_CEF_H

#include "webengine.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_render_process_handler_capi.h"
#include "include/capi/cef_scheme_capi.h"
#include "include/capi/cef_v8_capi.h"
#include "include/capi/cef_values_capi.h"

/* A page's call to window.external: lParam is the call, wParam 0; or the release of an object a page held, lParam the object, wParam 1. */
#define ENGINE_DISPATCH (WM_APP + 0x56)

struct engine_functions {
	const char *(*api_hash)(int, int);
	int (*initialize)(const cef_main_args_t *, const cef_settings_t *, cef_app_t *, void *);
	int (*create_browser)(const cef_window_info_t *, cef_client_t *, const cef_string_t *, const cef_browser_settings_t *, cef_dictionary_value_t *,
		cef_request_context_t *);
	int (*string_set)(const char16_t *, size_t, cef_string_utf16_t *, int);
	void (*string_clear)(cef_string_utf16_t *);
	void (*userfree_free)(cef_string_userfree_utf16_t);
	int (*register_factory)(const cef_string_t *, const cef_string_t *, cef_scheme_handler_factory_t *);
	cef_dictionary_value_t *(*dictionary)(void);
	cef_v8_value_t *(*undefined)(void);
	cef_v8_value_t *(*null)(void);
	cef_v8_value_t *(*boolean)(int);
	cef_v8_value_t *(*integer)(int32_t);
	cef_v8_value_t *(*unsigned_integer)(uint32_t);
	cef_v8_value_t *(*number)(double);
	cef_v8_value_t *(*string)(const cef_string_t *);
	cef_v8_value_t *(*object)(cef_v8_accessor_t *, cef_v8_interceptor_t *);
	cef_v8_value_t *(*function)(const cef_string_t *, cef_v8_handler_t *);
};

extern struct engine_functions cef;

__declspec(dllimport) void WINAPI RavenTrace(const char *text);

void engine_trace(const char *what, const WCHAR *text);
void engine_set_text(cef_string_t *out, const WCHAR *text);
WCHAR *engine_copy_text(const cef_string_t *text);
WCHAR *engine_take_userfree(cef_string_userfree_t text);
void engine_release(void *object);

/* The browser process's view of a browser's control; the extra information a browser is created with carries the control's window under
 * this key, for the pages, which have no window of their own to ask. */
#define ENGINE_WINDOW_KEY L"raven.window"

/* The pages' side: window.external for every page, answered by the object the control's site offers. */
extern cef_render_process_handler_t external_pages;
/* The control's side of the pages' calls, on the control's thread. */
void external_dispatch(WPARAM wparam, LPARAM lparam);

static inline void CEF_CALLBACK static_add_ref(cef_base_ref_counted_t *self)
{
}

static inline int CEF_CALLBACK static_release(cef_base_ref_counted_t *self)
{
	return 0;
}

static inline int CEF_CALLBACK static_one_ref(cef_base_ref_counted_t *self)
{
	return 1;
}

/* The base of an object that lives as long as the module and ignores references. */
#define STATIC_BASE(type) {sizeof(type), static_add_ref, static_release, static_one_ref, static_one_ref}

#endif
