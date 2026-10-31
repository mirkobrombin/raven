/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_ADVPACK_H
#define RAVEN_ADVPACK_H

#include <windows.h>
#include <setupapi.h>
#include <shlobj.h>
#include <wchar.h>

/* Flags shared by the install entry points; the RSC_ and ALINF_ values agree. */
#define INSTALL_QUIET 0x4
#define INSTALL_DELAY_OCX 0x200

/* advpub.h declares only the unsuffixed ANSI names, so the wide layouts live here. */
typedef struct
{
	char *cab;
	char *inf;
	char *section;
	char source[MAX_PATH];
	DWORD flags;
} cab_info_a;

typedef struct
{
	WCHAR *cab;
	WCHAR *inf;
	WCHAR *section;
	WCHAR source[MAX_PATH];
	DWORD flags;
} cab_info_w;

typedef struct
{
	WCHAR guid[59];
	WCHAR name[128];
	WCHAR locale[10];
	WCHAR stub[MAX_PATH * 4];
	WCHAR version[32];
	WCHAR component[128];
	DWORD installed;
	BOOL rollback;
} per_user_w;

typedef struct
{
	char guid[59];
	char name[128];
	char locale[10];
	char stub[MAX_PATH * 4];
	char version[32];
	char component[128];
	DWORD installed;
	BOOL rollback;
} per_user_a;

void *adv_alloc(SIZE_T size);
void adv_free(void *memory);
WCHAR *adv_wide(const char *text);
WCHAR *adv_wide_list(const char *list);
WCHAR *adv_copy(const WCHAR *text);
WCHAR *adv_join(const WCHAR *left, const WCHAR *right);
/* The absolute form of a path, as a new string. */
WCHAR *adv_full_path(const WCHAR *path);
/* The directory holding a path, as a new string. */
WCHAR *adv_parent(const WCHAR *path);
/* An error of the setup API maps to its SPAPI_E_ value, any other to its Win32 HRESULT. */
HRESULT adv_error(DWORD error);
/* Runs a command line and waits for it. */
HRESULT adv_run_wait(const WCHAR *command, const WCHAR *directory, BOOL hidden);
/* Splits rundll32 style "a,b,c" text in place; quotes protect commas and go away. */
DWORD adv_split(WCHAR *text, WCHAR **fields, DWORD count);

HRESULT adv_extract_cab(const WCHAR *cab, const WCHAR *directory, const WCHAR *list);

#endif
