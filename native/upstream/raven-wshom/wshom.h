/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_WSHOM_WSHOM_H
#define RAVEN_WSHOM_WSHOM_H

/* The library's objects are built on the scripting runtime's dispatch objects, and its streams are the runtime's text streams. */
#include "../raven-scrrun/scrrun.h"

/* The classes the library makes through COM. */
IDispatch *wsh_shell_new(void);
IDispatch *wsh_network_new(void);

/* A read-only collection of values, by position or, when it has names, by name: what SpecialFolders and the network's lists give. */
IDispatch *wsh_collection_new(VARIANT *items, const WCHAR *const *names, ULONG count);

/* The registry as the shell's RegRead, RegWrite and RegDelete name it: a root (HKCU or HKEY_CURRENT_USER and the rest), keys, and a
 * value, or with a final backslash the key's default value. */
HRESULT wsh_reg_read(const WCHAR *name, VARIANT *result);
HRESULT wsh_reg_write(const WCHAR *name, VARIANT *value, const WCHAR *type);
HRESULT wsh_reg_delete(const WCHAR *name);

/* The environment of the process, or the one the system, the user or the session keeps in the registry. */
HRESULT wsh_environment_new(const WCHAR *kind, VARIANT *result);

/* A shortcut file, .lnk or .url, read when it is there and written by Save. */
HRESULT wsh_shortcut_new(const WCHAR *path, VARIANT *result);

/* A program started by Exec, with its standard streams on pipes. */
HRESULT wsh_exec(const WCHAR *command, VARIANT *result);

/* Keys typed into the window that has the focus, written as SendKeys writes them. */
HRESULT wsh_send_keys(const WCHAR *keys);

#endif
