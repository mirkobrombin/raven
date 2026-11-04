/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* raven-cefhost.exe: the renderer, GPU and utility processes of the WebBrowser control's Chromium Embedded Framework browser.
 *
 * The program protocols the control serves, such as a program's own pluggable protocol, reach every process as the --raven-schemes
 * switch, so each one registers them as standard schemes the way the browser process did. */
#include <windows.h>
#include "include/cef_api_hash.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_command_line_capi.h"
#include "include/capi/cef_scheme_capi.h"

static void CEF_CALLBACK no_add_ref(cef_base_ref_counted_t *self)
{
}

static int CEF_CALLBACK no_release(cef_base_ref_counted_t *self)
{
	return 0;
}

static int CEF_CALLBACK one_ref(cef_base_ref_counted_t *self)
{
	return 1;
}

/* The value of --raven-schemes=a,b on the process's command line, or NULL. */
static const WCHAR *schemes_switch(void)
{
	static const WCHAR name[] = L"--raven-schemes=";
	const WCHAR *line = GetCommandLineW(), *found = NULL;
	for (const WCHAR *at = line; *at; at++)
		if (!wcsncmp(at, name, ARRAYSIZE(name) - 1)) {
			found = at + ARRAYSIZE(name) - 1;
			break;
		}
	return found;
}

static void CEF_CALLBACK register_schemes(cef_app_t *self, cef_scheme_registrar_t *registrar)
{
	const WCHAR *list = schemes_switch();
	while (list && *list && *list != L' ' && *list != L'"') {
		const WCHAR *end = list;
		while (*end && *end != L',' && *end != L' ' && *end != L'"')
			end++;
		cef_string_t name = {0};
		cef_string_utf16_set((const char16_t *)list, end - list, &name, 1);
		registrar->add_custom_scheme(registrar, &name, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
		cef_string_utf16_clear(&name);
		list = *end == L',' ? end + 1 : end;
	}
}

static cef_app_t app = {
	.base = {sizeof(cef_app_t), no_add_ref, no_release, one_ref, one_ref},
	.on_register_custom_schemes = register_schemes,
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show)
{
	cef_api_hash(CEF_API_VERSION, 0);
	cef_main_args_t main_args = {instance};
	return cef_execute_process(&main_args, &app, NULL);
}
