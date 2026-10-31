/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_URI_H
#define RAVEN_URI_H

#define COBJMACROS
#include <windows.h>
#include <shlwapi.h>
#include <urlmon.h>
#include <wchar.h>

/* Set on text this module composed from valid parts: a DOS path's rules were already applied. */
#define URI_INTERNAL_COMBINED 0x80000000u

/* The pieces of one parsed URI. Strings are owned; NULL means the piece is absent. */
typedef struct
{
	WCHAR *raw;
	DWORD flags;
	URL_SCHEME scheme_type;
	WCHAR *scheme;
	WCHAR *user;
	WCHAR *password;
	WCHAR *host;
	Uri_HOST_TYPE host_type;
	DWORD port;
	BOOL has_port;
	/* The port as written, when it stays visible in the text. */
	WCHAR *port_text;
	WCHAR *path;
	WCHAR *query;
	WCHAR *fragment;
	/* Filled by uri_compose: the complete forms and what can be read from them. */
	WCHAR *absolute;
	WCHAR *display;
	BOOL has_authority;
	BOOL show_user_info;
	BOOL is_opaque;
	BOOL host_in_brackets;
	BOOL host_after_scheme;
	/* Composed by a combine: an empty path counts, a default port stays out of the authority. */
	BOOL combined;
} uri_data;

HRESULT uri_parse(const WCHAR *text, DWORD flags, uri_data *uri);
void uri_clear(uri_data *uri);
/* NULL when out of memory. */
WCHAR *uri_text_copy(const WCHAR *text);
WCHAR *uri_text_range(const WCHAR *start, const WCHAR *end);
void *uri_alloc(SIZE_T size);
void uri_free(void *memory);

/* Maps flags with its defaults, or fails when opposites are combined. */
BOOL uri_flags_valid(DWORD flags);
DWORD uri_effective_flags(DWORD flags);
URL_SCHEME uri_scheme_of(const WCHAR *name);
/* Registered part of a DNS name, as a new string, or NULL if the name has none of its own. */
WCHAR *uri_domain_of(const WCHAR *host);
WCHAR *uri_host_as_ascii(const WCHAR *host);
WCHAR *uri_host_as_unicode(const WCHAR *host);

#endif
