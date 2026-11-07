/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven web component: the HTTP classes of the Windows Runtime (Windows.Web.Http, its Headers and Filters) and Windows.Foundation.Uri,
 * which HTTP requests are made with. Requests go through WinINet, which the base protocol filter of Windows also sits on: its cache,
 * cookies, proxy settings and certificate checks are the ones WinINet programs share.
 *
 * http.c: the client, the base protocol filter, methods, request and response messages, and the factories.
 * headers.c: the header collections and the header values they hand out.
 * content.c: the content of responses, read as a buffer, a string or a stream.
 * transport.c: one request through WinINet.
 * uri.c: Windows.Foundation.Uri, over urlmon's IUri. */
#ifndef RAVEN_WEB_H
#define RAVEN_WEB_H

#include "../raven-winrt/winrt.h"
#include <wininet.h>
#include "windows.web.http.h"

typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClass uri_class;
typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer buffer_class;
typedef __x_ABI_CWindows_CStorage_CStreams_CIInputStream input_stream;
typedef __x_ABI_CWindows_CWeb_CHttp_CIHttpContent http_content;
typedef __x_ABI_CWindows_CWeb_CHttp_CIHttpMethod http_method;
typedef __x_ABI_CWindows_CWeb_CHttp_CIHttpRequestMessage request_message;
typedef __x_ABI_CWindows_CWeb_CHttp_CIHttpResponseMessage response_message;
typedef __x_ABI_CWindows_CWeb_CHttp_CFilters_CIHttpFilter http_filter;
typedef __FIAsyncOperationWithProgress_2_Windows__CWeb__CHttp__CHttpResponseMessage_HttpProgress response_operation;

/* uri.c */
HRESULT uri_create(const WCHAR *text, UINT32 length, uri_class **out);
HRESULT uri_factory(REFIID iid, void **out);
/* The absolute URI of any Windows.Foundation.Uri. */
HRESULT uri_text(uri_class *uri, HSTRING *out);

/* headers.c */
enum headers_kind {
	HEADERS_REQUEST,
	HEADERS_RESPONSE,
	HEADERS_CONTENT
};

struct headers;
HRESULT headers_new(enum headers_kind kind, struct headers **out);
/* The collection's own interface (IHttpRequestHeaderCollection, ...), with a reference added. */
void *headers_interface(struct headers *headers);
void headers_release(struct headers *headers);
struct winrt_map *headers_map(struct headers *headers);
/* Whether a header describes the content (RFC 9110, 6.4 and 8): Content-*, Expires, Last-Modified, Allow. */
BOOL header_is_content(const WCHAR *name, UINT32 length);
HRESULT date_or_delta_statics(REFIID iid, void **out);

/* content.c */
struct exchange;
/* The content of a response: the body still to read from the exchange, which the content takes over. */
HRESULT content_new(struct exchange *exchange, struct headers *headers, http_content **out);
/* All the bytes of any IHttpContent, which the caller frees: the body of a request. */
HRESULT content_bytes(http_content *content, BYTE **bytes, UINT32 *length);

/* transport.c */
struct transport_settings {
	BOOL allow_redirect, decompress, use_cookies;
	DWORD cache_flags;
};
struct exchange *exchange_open(void);
/* Sends the request and reads the head of the response; the body stays in the exchange. */
HRESULT exchange_send(struct exchange *exchange, const struct transport_settings *settings, request_message *request,
	response_message **out);
/* Reads up to capacity bytes of the body; 0 at its end. */
HRESULT exchange_read(struct exchange *exchange, BYTE *into, UINT32 capacity, UINT32 *read);
/* The length the head gave, if any. */
BOOL exchange_length(struct exchange *exchange, UINT64 *length);
void exchange_close(struct exchange *exchange);

/* http.c */
HRESULT response_new(INT32 status, response_message **out);
/* A response's parts, set while its head is read. */
void response_set_head(response_message *response, HSTRING reason, INT32 version, request_message *request, struct headers *headers);
void response_set_content(response_message *response, http_content *content);
/* The request's method name, URI and headers. */
HRESULT request_parts(request_message *request, HSTRING *method, uri_class **uri, http_content **content);
/* Points a request of this component at the URI a redirection ended at. */
void request_set_uri(request_message *request, uri_class *uri);

/* The HRESULT of a WinINet or Win32 failure, as the Windows classes report them (WININET_E_* for WinINet's errors). */
static inline HRESULT web_error(DWORD error) { return error ? HRESULT_FROM_WIN32(error) : E_FAIL; }

#endif
