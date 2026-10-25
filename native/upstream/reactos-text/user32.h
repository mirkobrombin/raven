/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#pragma once
#include <windows.h>
#include <limits.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#define strlenW wcslen
#define strcpyW wcscpy
#define strncpyW wcsncpy
#define strchrW wcschr
#define strrchrW wcsrchr
#define DbgPrint(...) ((void)0)
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define FIXME(...) ((void)0)
#define WINE_DEFAULT_DEBUG_CHANNEL(name)
