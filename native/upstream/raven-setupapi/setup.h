/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_SETUP_H
#define RAVEN_SETUP_H

#define COBJMACROS
#include <windows.h>
#include <setupapi.h>
#include <wchar.h>

void *setup_alloc(SIZE_T size);
void *setup_realloc(void *memory, SIZE_T size);
void setup_free(void *memory);
WCHAR *setup_copy_wide(const WCHAR *text);
WCHAR *setup_to_wide(const char *text);
char *setup_to_ansi(const WCHAR *text);
WCHAR *setup_join_path(const WCHAR *left, const WCHAR *right);
BOOL setup_make_directories(const WCHAR *path);
/* Copies to a caller's counted buffer the way every text query of this API does. */
BOOL setup_give_wide(const WCHAR *text, WCHAR *buffer, DWORD size, DWORD *required);
BOOL setup_wide_to_ansi_result(BOOL result, const WCHAR *text, char *buffer, DWORD size, DWORD *required);

/* INF internals other parts need. */
BOOL setup_set_dir_id(HINF handle, DWORD id, const WCHAR *path);
const WCHAR *setup_user_dir_id(HINF handle, DWORD id);
const WCHAR *setup_inf_directory(HINF handle);
const WCHAR *setup_inf_path(HINF handle);
BOOL setup_inf_is_win4(HINF handle);
/* The directory of an id as a new string, or NULL when it names none. */
WCHAR *setup_dir_id_path(HINF handle, int id);
/* The directory a line of an INF designates: id in field first, subdirectory in the next. */
WCHAR *setup_directory_from_line(HINF handle, INFCONTEXT *context, DWORD first);
/* The target directory of a copy section, per [DestinationDirs]. */
WCHAR *setup_section_target(HINF handle, const WCHAR *section);
BOOL setup_source_location(HINF handle, const WCHAR *file, UINT *id, WCHAR **subdir);
WCHAR *setup_source_info(HINF handle, UINT id, UINT what);

/* File copying with the SP_COPY_ styles; decompresses unless told otherwise. */
BOOL setup_copy_styled(const WCHAR *source, const WCHAR *target, DWORD style, BOOL *in_use);
DWORD setup_decompress(const WCHAR *source, const WCHAR *target, UINT type);
/* Extracts one named file of a cabinet; NULL name takes the first. */
DWORD setup_extract_from_cabinet(const WCHAR *cabinet, const WCHAR *name, const WCHAR *target);

/* What the mingw headers leave out. */
#define SETUP_SVC_START 0x00000800
#ifndef MAX_CLASS_NAME_LEN
#define MAX_CLASS_NAME_LEN 32
#endif
BOOL WINAPI SetupGetInfSections(HINF handle, PWSTR buffer, DWORD size, DWORD *required);
PCWSTR WINAPI pSetupGetField(INFCONTEXT *context, DWORD index);

#endif
