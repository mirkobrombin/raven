/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_URLMON_PRIVATE_H
#define RAVEN_URLMON_PRIVATE_H

#include "uri.h"
#include <wininet.h>
#include <shellapi.h>

#ifndef Uri_DISPLAY_NO_FRAGMENT
#define Uri_DISPLAY_NO_FRAGMENT 0x8000000
#endif

extern const CLSID CLSID_CUri;
extern const IID IID_IAsyncBindCtx;
extern const IID IID_IBindStatusCallbackHolder;
extern HINSTANCE urlmon_instance;

void urlmon_lock_module(void);
void urlmon_unlock_module(void);

/* The parsed form inside an IUri of this module; NULL for any other. */
const uri_data *urlmon_uri_data(IUri *uri);
HRESULT urlmon_uri_property(const uri_data *data, Uri_PROPERTY property, WCHAR **value);
HRESULT urlmon_create_empty_uri(REFIID iid, void **object);
HRESULT urlmon_combine(const uri_data *base, const uri_data *relative, DWORD flags, IUri **result);
BOOL urlmon_is_known(const uri_data *data);
void urlmon_uri_settle_raw(IUri *uri);
HRESULT urlmon_uri_set_authority(IUri *uri, const WCHAR *user, const WCHAR *password, const WCHAR *host, const WCHAR *path);
HRESULT urlmon_create_uri_combined(const WCHAR *text, DWORD flags, IUri **uri);
BOOL urlmon_uris_match(const uri_data *left, const uri_data *right);

/* A protocol registered for this process or in the registry, by scheme; adds a reference. */
IInternetProtocolInfo *urlmon_protocol_info(const WCHAR *scheme);
IInternetProtocolInfo *urlmon_url_protocol_info(const WCHAR *url);
/* Who serves a URL: registered in this process, built in here, or named by the registry. */
HRESULT urlmon_protocol_factory(const WCHAR *url, BOOL *from_session, IClassFactory **factory, CLSID *clsid);
BOOL urlmon_builtin_protocol(const WCHAR *scheme, IClassFactory **factory, CLSID *clsid);
IClassFactory *urlmon_mime_filter(const WCHAR *mime, CLSID *clsid);
HRESULT urlmon_create_bind_protocol(IInternetProtocol **protocol);
HRESULT urlmon_bind(IMoniker *moniker, IUri *uri, IBindCtx *context, BOOL to_object, REFIID iid, void **result);
IBindStatusCallback *urlmon_context_callback(IBindCtx *context);
WCHAR *urlmon_user_agent(void);
void urlmon_session_startup(void);
void urlmon_session_shutdown(void);
BOOL urlmon_shell_execute(SHELLEXECUTEINFOW *info);
HRESULT urlmon_create_file_protocol(IUnknown *outer, REFIID iid, void **object);
HRESULT urlmon_create_mk_protocol(IUnknown *outer, REFIID iid, void **object);
HRESULT urlmon_create_http_protocol(IUnknown *outer, REFIID iid, void **object);
HRESULT urlmon_create_https_protocol(IUnknown *outer, REFIID iid, void **object);
HRESULT urlmon_create_ftp_protocol(IUnknown *outer, REFIID iid, void **object);
HRESULT urlmon_create_moniker_object(IUnknown *outer, REFIID iid, void **object);
HRESULT urlmon_create_security_manager(IUnknown *outer, REFIID iid, void **object);
HRESULT urlmon_create_zone_manager(IUnknown *outer, REFIID iid, void **object);

#endif
