/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: what the file dialog window shares with the dialog objects and the classic calls that show it. */
#pragma once

#include <windows.h>
#include <shobjidl.h>

#define MAX_FILTERS 32
#define MAX_RESULTS 64
#define MAX_CHECKS 4

typedef struct {
	WCHAR name[64];
	WCHAR spec[256];
} Filter;

typedef struct {
	BOOL save;
	DWORD options; /* FOS_* values; the classic flags are translated by the callers */
	WCHAR title[128];
	WCHAR ok_label[32];
	WCHAR name_label[32];
	Filter filters[MAX_FILTERS];
	UINT filter_count;
	UINT filter_index; /* one based */
	WCHAR dir[MAX_PATH];
	WCHAR name[MAX_PATH];
	WCHAR defext[16];
	/* The chosen paths: result is the first, results all of them when several files may be chosen. */
	WCHAR result[MAX_PATH];
	WCHAR results[MAX_RESULTS][MAX_PATH];
	UINT result_count;
	BOOL has_result;
	struct { DWORD id; BOOL checked; WCHAR label[64]; } checks[MAX_CHECKS];
	UINT check_count;
} DialogState;

void copy_text(WCHAR *to, size_t capacity, const WCHAR *from);
void join_path(WCHAR *out, size_t capacity, const WCHAR *dir, const WCHAR *name);
BOOL is_absolute(const WCHAR *path);
const WCHAR *file_part(const WCHAR *path);
void parent_directory(WCHAR *dir);
BOOL match_spec(const WCHAR *spec, const WCHAR *name);

/* Shows the file dialog for the state, which receives the chosen paths; TRUE when the user accepted. */
BOOL run_dialog(HWND owner, DialogState *state);
