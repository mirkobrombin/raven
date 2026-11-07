/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_SCRRUN_SCRRUN_H
#define RAVEN_SCRRUN_SCRRUN_H

#include "object.h"

/* The classes the library makes through COM. */
IDispatch *sr_dictionary_new(void);
IDispatch *sr_file_system_new(void);

/* For the file system objects. */
IDispatch *sr_file_new(const WCHAR *path);
IDispatch *sr_folder_new(const WCHAR *path);
IDispatch *sr_drive_new(const WCHAR *root);
IDispatch *sr_drives_new(void);
/* A text stream over an open handle; owned closes it when the stream does. */
IDispatch *sr_text_stream_new(HANDLE file, BOOL owned, LONG mode, BOOL unicode);
/* Opens a text file: mode 1 reads, 2 writes, 8 appends; format -1 Unicode, 0 ANSI, -2 the system's default. */
HRESULT sr_open_text(const WCHAR *path, LONG mode, BOOL create, LONG format, BOOL overwrite, VARIANT *result);
/* A full path, as GetAbsolutePathName gives it. */
WCHAR *sr_full_path(const WCHAR *path);
/* The last part of a path, without the separators that end it; the path's parent; a path and a name joined. */
WCHAR *sr_file_name(const WCHAR *path);
WCHAR *sr_parent_name(const WCHAR *path);
WCHAR *sr_join(const WCHAR *path, const WCHAR *name);

/* What the file system object and its File and Folder objects do; specs may hold wildcards in their last part. */
HRESULT sr_copy_files(const WCHAR *spec, const WCHAR *destination, BOOL overwrite);
HRESULT sr_move_files(const WCHAR *spec, const WCHAR *destination);
HRESULT sr_delete_files(const WCHAR *spec, BOOL force);
HRESULT sr_copy_folders(const WCHAR *spec, const WCHAR *destination, BOOL overwrite);
HRESULT sr_move_folders(const WCHAR *spec, const WCHAR *destination);
HRESULT sr_delete_folders(const WCHAR *spec, BOOL force);

#define SR_FOR_READING 1
#define SR_FOR_WRITING 2
#define SR_FOR_APPENDING 8

#endif
