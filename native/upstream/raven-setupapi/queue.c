/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include "setup.h"
#include <string.h>

/* A queue holds deletions and renames as plain lists and copies grouped by source media: the
 * callback hears about one media once, however many files come from it. */

typedef struct file_op {
	struct file_op *next;
	WCHAR *root;
	WCHAR *path;
	WCHAR *file;
	WCHAR *target_dir;
	WCHAR *target_file;
	DWORD style;
} file_op;

typedef struct media_group {
	struct media_group *next;
	WCHAR *root;
	WCHAR *description;
	WCHAR *tag;
	file_op *copies;
	file_op **copies_tail;
} media_group;

#define QUEUE_MAGIC 0x51465352

typedef struct file_queue {
	DWORD magic;
	media_group *media;
	media_group **media_tail;
	file_op *deletes;
	file_op **deletes_tail;
	file_op *renames;
	file_op **renames_tail;
	DWORD delete_count;
	DWORD rename_count;
	DWORD copy_count;
	DWORD flags;
} file_queue;

static file_queue *queue_of(HSPFILEQ handle)
{
	file_queue *queue = handle;

	if (!queue || handle == INVALID_HANDLE_VALUE || queue->magic != QUEUE_MAGIC) {
		SetLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	return queue;
}

HSPFILEQ WINAPI SetupOpenFileQueue(void)
{
	file_queue *queue = setup_alloc(sizeof(*queue));

	if (!queue) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	queue->magic = QUEUE_MAGIC;
	queue->media_tail = &queue->media;
	queue->deletes_tail = &queue->deletes;
	queue->renames_tail = &queue->renames;
	return queue;
}

static void free_ops(file_op *op)
{
	while (op) {
		file_op *next = op->next;
		setup_free(op->root);
		setup_free(op->path);
		setup_free(op->file);
		setup_free(op->target_dir);
		setup_free(op->target_file);
		setup_free(op);
		op = next;
	}
}

BOOL WINAPI SetupCloseFileQueue(HSPFILEQ handle)
{
	file_queue *queue = queue_of(handle);

	if (!queue)
		return FALSE;
	while (queue->media) {
		media_group *next = queue->media->next;
		free_ops(queue->media->copies);
		setup_free(queue->media->root);
		setup_free(queue->media->description);
		setup_free(queue->media->tag);
		setup_free(queue->media);
		queue->media = next;
	}
	free_ops(queue->deletes);
	free_ops(queue->renames);
	queue->magic = 0;
	setup_free(queue);
	return TRUE;
}

static BOOL same_text(const WCHAR *left, const WCHAR *right)
{
	if (!left || !right)
		return left == right;
	return !lstrcmpiW(left, right);
}

/* A description and tag distinguish media only as far as both name one, which keeps files without
 * either on the media that came first from the same root. */
static media_group *media_for(file_queue *queue, const WCHAR *root, const WCHAR *description, const WCHAR *tag)
{
	media_group *media;

	for (media = queue->media; media; media = media->next) {
		if (same_text(media->root, root) && same_text(media->description, description) && same_text(media->tag, tag))
			return media;
	}
	media = setup_alloc(sizeof(*media));
	if (!media)
		return NULL;
	media->root = setup_copy_wide(root);
	media->description = setup_copy_wide(description);
	media->tag = setup_copy_wide(tag);
	media->copies_tail = &media->copies;
	*queue->media_tail = media;
	queue->media_tail = &media->next;
	return media;
}

BOOL WINAPI SetupQueueCopyIndirectW(PSP_FILE_COPY_PARAMS_W params)
{
	file_queue *queue;
	media_group *media;
	file_op *op;

	if (!params || params->cbSize != sizeof(*params) || !params->SourceFilename) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	queue = queue_of(params->QueueHandle);
	if (!queue)
		return FALSE;
	media = media_for(queue, params->SourceRootPath, params->SourceDescription, params->SourceTagfile);
	op = setup_alloc(sizeof(*op));
	if (!media || !op) {
		setup_free(op);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	op->root = setup_copy_wide(params->SourceRootPath);
	op->path = setup_copy_wide(params->SourcePath);
	op->file = setup_copy_wide(params->SourceFilename);
	op->target_dir = setup_copy_wide(params->TargetDirectory);
	op->target_file = setup_copy_wide(params->TargetFilename ? params->TargetFilename : params->SourceFilename);
	op->style = params->CopyStyle;
	*media->copies_tail = op;
	media->copies_tail = &op->next;
	queue->copy_count++;
	return TRUE;
}

BOOL WINAPI SetupQueueCopyIndirectA(PSP_FILE_COPY_PARAMS_A params)
{
	SP_FILE_COPY_PARAMS_W wide;
	BOOL result;

	if (!params || params->cbSize != sizeof(*params)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	memset(&wide, 0, sizeof(wide));
	wide.cbSize = sizeof(wide);
	wide.QueueHandle = params->QueueHandle;
	wide.SourceRootPath = setup_to_wide(params->SourceRootPath);
	wide.SourcePath = setup_to_wide(params->SourcePath);
	wide.SourceFilename = setup_to_wide(params->SourceFilename);
	wide.SourceDescription = setup_to_wide(params->SourceDescription);
	wide.SourceTagfile = setup_to_wide(params->SourceTagfile);
	wide.TargetDirectory = setup_to_wide(params->TargetDirectory);
	wide.TargetFilename = setup_to_wide(params->TargetFilename);
	wide.CopyStyle = params->CopyStyle;
	wide.LayoutInf = params->LayoutInf;
	result = SetupQueueCopyIndirectW(&wide);
	setup_free((void *)wide.SourceRootPath);
	setup_free((void *)wide.SourcePath);
	setup_free((void *)wide.SourceFilename);
	setup_free((void *)wide.SourceDescription);
	setup_free((void *)wide.SourceTagfile);
	setup_free((void *)wide.TargetDirectory);
	setup_free((void *)wide.TargetFilename);
	return result;
}

BOOL WINAPI SetupQueueCopyW(HSPFILEQ handle, PCWSTR root, PCWSTR path, PCWSTR file, PCWSTR description, PCWSTR tag, PCWSTR target_dir, PCWSTR target_file, DWORD style)
{
	SP_FILE_COPY_PARAMS_W params;

	memset(&params, 0, sizeof(params));
	params.cbSize = sizeof(params);
	params.QueueHandle = handle;
	params.SourceRootPath = root;
	params.SourcePath = path;
	params.SourceFilename = file;
	params.SourceDescription = description;
	params.SourceTagfile = tag;
	params.TargetDirectory = target_dir;
	params.TargetFilename = target_file;
	params.CopyStyle = style;
	return SetupQueueCopyIndirectW(&params);
}

BOOL WINAPI SetupQueueCopyA(HSPFILEQ handle, PCSTR root, PCSTR path, PCSTR file, PCSTR description, PCSTR tag, PCSTR target_dir, PCSTR target_file, DWORD style)
{
	SP_FILE_COPY_PARAMS_A params;

	memset(&params, 0, sizeof(params));
	params.cbSize = sizeof(params);
	params.QueueHandle = handle;
	params.SourceRootPath = root;
	params.SourcePath = path;
	params.SourceFilename = file;
	params.SourceDescription = description;
	params.SourceTagfile = tag;
	params.TargetDirectory = target_dir;
	params.TargetFilename = target_file;
	params.CopyStyle = style;
	return SetupQueueCopyIndirectA(&params);
}

/* A copy described by an INF: the file's disk, path and tag come from its layout sections. */
static BOOL queue_from_inf(file_queue *queue, HSPFILEQ handle, const WCHAR *root, HINF hinf, HINF layout, const WCHAR *file, const WCHAR *target_dir, const WCHAR *target_file, DWORD style)
{
	SP_FILE_COPY_PARAMS_W params;
	WCHAR *description = NULL, *tag = NULL, *disk_path = NULL, *subdir = NULL, *joined = NULL, *full_root = NULL;
	UINT id = 0;
	BOOL result;

	if (setup_source_location(layout ? layout : hinf, file, &id, &subdir)) {
		description = setup_source_info(layout ? layout : hinf, id, SRCINFO_DESCRIPTION);
		tag = setup_source_info(layout ? layout : hinf, id, SRCINFO_TAGFILE);
		disk_path = setup_source_info(layout ? layout : hinf, id, SRCINFO_PATH);
		if (description && !*description) {
			setup_free(description);
			description = NULL;
		}
		if (tag && !*tag) {
			setup_free(tag);
			tag = NULL;
		}
	}
	if (disk_path && *disk_path)
		full_root = setup_join_path(root, disk_path);
	if (subdir && *subdir)
		joined = setup_copy_wide(subdir);
	memset(&params, 0, sizeof(params));
	params.cbSize = sizeof(params);
	params.QueueHandle = handle;
	params.SourceRootPath = full_root ? full_root : root;
	params.SourcePath = joined;
	params.SourceFilename = file;
	params.SourceDescription = description;
	params.SourceTagfile = tag;
	params.TargetDirectory = target_dir;
	params.TargetFilename = target_file;
	params.CopyStyle = style;
	result = SetupQueueCopyIndirectW(&params);
	setup_free(description);
	setup_free(tag);
	setup_free(disk_path);
	setup_free(subdir);
	setup_free(joined);
	setup_free(full_root);
	return result;
}

BOOL WINAPI SetupQueueDefaultCopyW(HSPFILEQ handle, HINF hinf, PCWSTR root, PCWSTR source_file, PCWSTR target_file, DWORD style)
{
	file_queue *queue = queue_of(handle);
	WCHAR *target;
	BOOL result;

	if (!queue)
		return FALSE;
	if (!hinf || hinf == INVALID_HANDLE_VALUE) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	target = setup_section_target(hinf, NULL);
	if (!target)
		return FALSE;
	result = queue_from_inf(queue, handle, root, hinf, NULL, source_file, target, target_file, style);
	setup_free(target);
	return result;
}

BOOL WINAPI SetupQueueDefaultCopyA(HSPFILEQ handle, HINF hinf, PCSTR root, PCSTR source_file, PCSTR target_file, DWORD style)
{
	WCHAR *wide_root = setup_to_wide(root), *wide_source = setup_to_wide(source_file), *wide_target = setup_to_wide(target_file);
	BOOL result = SetupQueueDefaultCopyW(handle, hinf, wide_root, wide_source, wide_target, style);

	setup_free(wide_root);
	setup_free(wide_source);
	setup_free(wide_target);
	return result;
}

BOOL WINAPI SetupQueueCopySectionW(HSPFILEQ handle, PCWSTR root, HINF hinf, HINF layout, PCWSTR section, DWORD style)
{
	file_queue *queue = queue_of(handle);
	WCHAR *target_dir;
	INFCONTEXT context;
	BOOL result = TRUE;

	if (!queue)
		return FALSE;
	if (!hinf || hinf == INVALID_HANDLE_VALUE || !section) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	/* The "@file" form of a CopyFiles directive names one file rather than a section. */
	target_dir = setup_section_target(hinf, section);
	if (!target_dir)
		return FALSE;
	if (section[0] == '@') {
		result = queue_from_inf(queue, handle, root, hinf, layout, section + 1, target_dir, NULL, style);
		setup_free(target_dir);
		return result;
	}
	if (!SetupFindFirstLineW(hinf, section, NULL, &context)) {
		setup_free(target_dir);
		SetLastError(ERROR_LINE_NOT_FOUND);
		return FALSE;
	}
	do {
		const WCHAR *target_file = pSetupGetField(&context, 1), *source_file = SetupGetFieldCount(&context) > 1 ? pSetupGetField(&context, 2) : NULL;
		INT line_style = 0;

		if (!target_file || !*target_file)
			continue;
		if (!source_file || !*source_file)
			source_file = target_file;
		if (SetupGetFieldCount(&context) >= 4)
			SetupGetIntField(&context, 4, &line_style);
		if (!queue_from_inf(queue, handle, root, hinf, layout, source_file, target_dir, target_file, style | (DWORD)line_style))
			result = FALSE;
	} while (result && SetupFindNextLine(&context, &context));
	setup_free(target_dir);
	if (result)
		SetLastError(0);
	return result;
}

BOOL WINAPI SetupQueueCopySectionA(HSPFILEQ handle, PCSTR root, HINF hinf, HINF layout, PCSTR section, DWORD style)
{
	WCHAR *wide_root = setup_to_wide(root), *wide_section = setup_to_wide(section);
	BOOL result = SetupQueueCopySectionW(handle, wide_root, hinf, layout, wide_section, style);

	setup_free(wide_root);
	setup_free(wide_section);
	return result;
}

BOOL WINAPI SetupQueueDeleteW(HSPFILEQ handle, PCWSTR path, PCWSTR file)
{
	file_queue *queue = queue_of(handle);
	file_op *op;

	if (!queue)
		return FALSE;
	op = setup_alloc(sizeof(*op));
	if (!op) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	op->target_dir = setup_copy_wide(path);
	op->target_file = setup_copy_wide(file);
	*queue->deletes_tail = op;
	queue->deletes_tail = &op->next;
	queue->delete_count++;
	return TRUE;
}

BOOL WINAPI SetupQueueDeleteA(HSPFILEQ handle, PCSTR path, PCSTR file)
{
	WCHAR *wide_path = setup_to_wide(path), *wide_file = setup_to_wide(file);
	BOOL result = SetupQueueDeleteW(handle, wide_path, wide_file);

	setup_free(wide_path);
	setup_free(wide_file);
	return result;
}

BOOL WINAPI SetupQueueRenameW(HSPFILEQ handle, PCWSTR source_path, PCWSTR source_file, PCWSTR target_path, PCWSTR target_file)
{
	file_queue *queue = queue_of(handle);
	file_op *op;

	if (!queue)
		return FALSE;
	op = setup_alloc(sizeof(*op));
	if (!op) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	op->path = setup_copy_wide(source_path);
	op->file = setup_copy_wide(source_file);
	op->target_dir = setup_copy_wide(target_path ? target_path : source_path);
	op->target_file = setup_copy_wide(target_file);
	*queue->renames_tail = op;
	queue->renames_tail = &op->next;
	queue->rename_count++;
	return TRUE;
}

BOOL WINAPI SetupQueueRenameA(HSPFILEQ handle, PCSTR source_path, PCSTR source_file, PCSTR target_path, PCSTR target_file)
{
	WCHAR *a = setup_to_wide(source_path), *b = setup_to_wide(source_file), *c = setup_to_wide(target_path), *d = setup_to_wide(target_file);
	BOOL result = SetupQueueRenameW(handle, a, b, c, d);

	setup_free(a);
	setup_free(b);
	setup_free(c);
	setup_free(d);
	return result;
}

BOOL WINAPI SetupQueueDeleteSectionW(HSPFILEQ handle, HINF hinf, HINF layout, PCWSTR section)
{
	WCHAR *target_dir = setup_section_target(hinf, section);
	INFCONTEXT context;
	BOOL result = TRUE;

	if (!target_dir)
		return FALSE;
	if (SetupFindFirstLineW(hinf, section, NULL, &context)) {
		do {
			const WCHAR *file = pSetupGetField(&context, 1);
			if (file && *file && !SetupQueueDeleteW(handle, target_dir, file))
				result = FALSE;
		} while (SetupFindNextLine(&context, &context));
	} else {
		result = FALSE;
	}
	setup_free(target_dir);
	return result;
}

BOOL WINAPI SetupQueueDeleteSectionA(HSPFILEQ handle, HINF hinf, HINF layout, PCSTR section)
{
	WCHAR *wide = setup_to_wide(section);
	BOOL result = SetupQueueDeleteSectionW(handle, hinf, layout, wide);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupQueueRenameSectionW(HSPFILEQ handle, HINF hinf, HINF layout, PCWSTR section)
{
	WCHAR *target_dir = setup_section_target(hinf, section);
	INFCONTEXT context;
	BOOL result = TRUE;

	if (!target_dir)
		return FALSE;
	if (SetupFindFirstLineW(hinf, section, NULL, &context)) {
		do {
			const WCHAR *target = pSetupGetField(&context, 1), *source = pSetupGetField(&context, 2);
			if (target && source && !SetupQueueRenameW(handle, target_dir, source, NULL, target))
				result = FALSE;
		} while (SetupFindNextLine(&context, &context));
	} else {
		result = FALSE;
	}
	setup_free(target_dir);
	return result;
}

BOOL WINAPI SetupQueueRenameSectionA(HSPFILEQ handle, HINF hinf, HINF layout, PCSTR section)
{
	WCHAR *wide = setup_to_wide(section);
	BOOL result = SetupQueueRenameSectionW(handle, hinf, layout, wide);

	setup_free(wide);
	return result;
}

BOOL WINAPI SetupGetFileQueueCount(HSPFILEQ handle, UINT sub_queue, PUINT count)
{
	file_queue *queue = queue_of(handle);

	if (!queue)
		return FALSE;
	switch (sub_queue) {
	case FILEOP_COPY:
		*count = queue->copy_count;
		return TRUE;
	case FILEOP_RENAME:
		*count = queue->rename_count;
		return TRUE;
	case FILEOP_DELETE:
		*count = queue->delete_count;
		return TRUE;
	}
	SetLastError(ERROR_INVALID_PARAMETER);
	return FALSE;
}

BOOL WINAPI SetupGetFileQueueFlags(HSPFILEQ handle, PDWORD flags)
{
	file_queue *queue = queue_of(handle);

	if (!queue)
		return FALSE;
	*flags = queue->flags;
	return TRUE;
}

BOOL WINAPI SetupSetFileQueueFlags(HSPFILEQ handle, DWORD mask, DWORD flags)
{
	file_queue *queue = queue_of(handle);

	if (!queue)
		return FALSE;
	queue->flags = (queue->flags & ~mask) | (flags & mask);
	return TRUE;
}

BOOL WINAPI SetupSetFileQueueAlternatePlatformW(HSPFILEQ handle, PSP_ALTPLATFORM_INFO platform, PCWSTR catalog)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

BOOL WINAPI SetupSetFileQueueAlternatePlatformA(HSPFILEQ handle, PSP_ALTPLATFORM_INFO platform, PCSTR catalog)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

/* The commit. */

static WCHAR *op_source_path(const WCHAR *root, const WCHAR *path, const WCHAR *file)
{
	WCHAR *directory = setup_join_path(root, path), *full;

	if (!directory)
		return NULL;
	full = setup_join_path(directory, file);
	setup_free(directory);
	return full;
}

static WCHAR *op_target_path(const file_op *op)
{
	return setup_join_path(op->target_dir, op->target_file);
}

static BOOL path_exists(const WCHAR *path)
{
	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL is_cabinet_name(const WCHAR *name)
{
	const WCHAR *dot = name ? wcsrchr(name, '.') : NULL;

	return dot && !lstrcmpiW(dot, L".cab");
}

static BOOL has_trailing_slash(const WCHAR *text)
{
	SIZE_T length = text ? lstrlenW(text) : 0;

	return length && (text[length - 1] == '\\' || text[length - 1] == '/');
}

/* The source directory a callback sees has no separator at its end, whatever the caller queued. */
static WCHAR *media_directory(const WCHAR *root, const WCHAR *path)
{
	WCHAR *joined = setup_join_path(root, path);

	if (joined && has_trailing_slash(joined) && lstrlenW(joined) > 3)
		joined[lstrlenW(joined) - 1] = 0;
	return joined;
}

/* Announces the media and settles the directory its files come from. Returns FALSE on abort; sets
 * *skip when the callback declines the media's first file. */
static BOOL ask_media(media_group *media, file_op *first, PSP_FILE_CALLBACK_W callback, void *context, WCHAR **root, WCHAR **first_directory, BOOL *skip)
{
	WCHAR *current = media_directory(media->root, first->path);
	WCHAR path[MAX_PATH];
	BOOL ask = TRUE;

	*skip = FALSE;
	setup_free(*first_directory);
	*first_directory = NULL;
	for (;;) {
		SOURCE_MEDIA_W notice;
		UINT answer;

		if (!ask) {
			*first_directory = current;
			return TRUE;
		}
		memset(&notice, 0, sizeof(notice));
		notice.Reserved = NULL;
		notice.Tagfile = media->tag;
		notice.Description = media->description;
		notice.SourcePath = current;
		notice.SourceFile = first->file;
		notice.Flags = first->style & (SP_COPY_WARNIFSKIP | SP_COPY_NOSKIP | SP_FLAG_CABINETCONTINUATION | SP_COPY_NOBROWSE);
		path[0] = 0;
		answer = callback(context, SPFILENOTIFY_NEEDMEDIA, (UINT_PTR)&notice, (UINT_PTR)path);
		switch (answer) {
		case FILEOP_ABORT:
			setup_free(current);
			return FALSE;
		case FILEOP_SKIP:
			setup_free(current);
			*skip = TRUE;
			return TRUE;
		case FILEOP_NEWPATH: {
			WCHAR *candidate;
			BOOL found;

			/* The answer names the first file's directory, with its subdirectory. The rest of
			 * the media keeps its own subdirectories, so that element comes off the new root,
			 * but only when it is spelled exactly as the file gave it. */
			setup_free(*root);
			*root = setup_copy_wide(path);
			if (*root && first->path && *first->path) {
				SIZE_T length = lstrlenW(*root), sub = lstrlenW(first->path);
				if (length > sub && (*root)[length - sub - 1] == '\\' && !lstrcmpW(*root + length - sub, first->path))
					(*root)[length - sub - 1] = 0;
			}
			setup_free(current);
			current = setup_copy_wide(path);
			/* A tag naming a cabinet is what must be found; otherwise the first file itself. */
			candidate = setup_join_path(path, is_cabinet_name(media->tag) ? media->tag : first->file);
			found = candidate && path_exists(candidate);
			setup_free(candidate);
			ask = !found;
			break;
		}
		default:
			ask = FALSE;
			break;
		}
	}
}

static UINT notify_copy(PSP_FILE_CALLBACK_W callback, void *context, UINT message, const WCHAR *source, const WCHAR *target, DWORD error, DWORD flags, UINT_PTR extra)
{
	FILEPATHS_W paths;

	memset(&paths, 0, sizeof(paths));
	paths.Source = source;
	paths.Target = target;
	paths.Win32Error = error;
	paths.Flags = flags;
	return callback(context, message, (UINT_PTR)&paths, extra);
}

/* Copies one file, with the callback able to redirect the source. FALSE aborts the commit. */
static BOOL commit_copy(file_op *op, media_group *media, WCHAR **root, const WCHAR *path, const WCHAR *cabinet, PSP_FILE_CALLBACK_W callback, void *context)
{
	WCHAR *source = cabinet ? setup_copy_wide(op->file) : op_source_path(*root, path, op->file);
	WCHAR *target = op_target_path(op);
	BOOL result = TRUE;
	UINT answer;

	if (!source || !target) {
		setup_free(source);
		setup_free(target);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	setup_make_directories(op->target_dir);
	answer = notify_copy(callback, context, SPFILENOTIFY_STARTCOPY, source, target, 0, FILEOP_COPY, 0);
	if (answer == FILEOP_ABORT) {
		result = FALSE;
		goto done;
	}
	if (answer == FILEOP_SKIP)
		goto done;
	for (;;) {
		DWORD error = 0;
		BOOL in_use = FALSE;

		if (cabinet)
			error = setup_extract_from_cabinet(cabinet, op->file, target);
		else if (!setup_copy_styled(source, target, op->style, &in_use))
			error = GetLastError();
		if (!error) {
			notify_copy(callback, context, SPFILENOTIFY_ENDCOPY, source, target, 0, FILEOP_COPY, 0);
			break;
		}
		if (error == ERROR_SUCCESS)
			break;
		if (cabinet || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
			answer = notify_copy(callback, context, SPFILENOTIFY_COPYERROR, source, target, error, FILEOP_COPY, 0);
			if (answer == FILEOP_ABORT) {
				SetLastError(error);
				result = FALSE;
			} else if (answer != FILEOP_RETRY) {
				notify_copy(callback, context, SPFILENOTIFY_ENDCOPY, source, target, error, FILEOP_COPY, 0);
				break;
			}
			if (!result)
				break;
			continue;
		}
		{
			WCHAR redirect[MAX_PATH];

			redirect[0] = 0;
			answer = notify_copy(callback, context, SPFILENOTIFY_COPYERROR, source, target, error, FILEOP_COPY, (UINT_PTR)redirect);
			if (answer == FILEOP_ABORT) {
				SetLastError(error);
				result = FALSE;
				break;
			}
			if (answer == FILEOP_NEWPATH) {
				WCHAR *moved = setup_join_path(redirect, op->file);
				if (moved) {
					setup_free(source);
					source = moved;
				}
				/* Later files of the media follow the redirect too. */
				setup_free(*root);
				*root = setup_copy_wide(redirect);
				continue;
			}
			if (answer == FILEOP_RETRY)
				continue;
			notify_copy(callback, context, SPFILENOTIFY_ENDCOPY, source, target, error, FILEOP_COPY, 0);
			break;
		}
	}
done:
	setup_free(source);
	setup_free(target);
	return result;
}

static BOOL commit_media(media_group *media, PSP_FILE_CALLBACK_W callback, void *context)
{
	WCHAR *root = setup_copy_wide(media->root), *cabinet = NULL, *first_directory = NULL;
	file_op *op, *first = media->copies;
	BOOL skip = FALSE, result = TRUE;

	/* Declining the media declines the file it was asked for; the next file asks again. */
	for (;;) {
		if (!first) {
			setup_free(root);
			return TRUE;
		}
		if (!ask_media(media, first, callback, context, &root, &first_directory, &skip)) {
			setup_free(root);
			return FALSE;
		}
		if (!skip)
			break;
		first = first->next;
	}
	if (is_cabinet_name(media->tag)) {
		/* The first file's subdirectory says where the cabinet sits. It serves the whole media. */
		WCHAR *candidate = setup_join_path(first_directory, media->tag);
		if (candidate && path_exists(candidate))
			cabinet = candidate;
		else
			setup_free(candidate);
	}
	for (op = first; op; op = op->next) {
		BOOL copied;

		/* The first file comes from where the media answer said, without its subdirectory. */
		if (op == first) {
			WCHAR *directory = first_directory;
			copied = commit_copy(op, media, &directory, NULL, cabinet, callback, context);
			if (directory != first_directory) {
				setup_free(root);
				root = directory;
			} else {
				setup_free(first_directory);
			}
			first_directory = NULL;
		} else {
			copied = commit_copy(op, media, &root, op->path, cabinet, callback, context);
		}
		if (!copied) {
			result = FALSE;
			break;
		}
	}
	setup_free(first_directory);
	setup_free(root);
	setup_free(cabinet);
	return result;
}

static BOOL commit_deletes(file_queue *queue, PSP_FILE_CALLBACK_W callback, void *context)
{
	file_op *op;

	if (!queue->deletes)
		return TRUE;
	if (callback(context, SPFILENOTIFY_STARTSUBQUEUE, FILEOP_DELETE, queue->delete_count) == FILEOP_ABORT)
		return FALSE;
	for (op = queue->deletes; op; op = op->next) {
		WCHAR *target = op_target_path(op);
		UINT answer = notify_copy(callback, context, SPFILENOTIFY_STARTDELETE, target, target, 0, FILEOP_DELETE, 0);

		if (answer == FILEOP_ABORT) {
			setup_free(target);
			return FALSE;
		}
		if (answer == FILEOP_DOIT) {
			DWORD error = 0;
			if (!DeleteFileW(target))
				error = GetLastError();
			if (error && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
				notify_copy(callback, context, SPFILENOTIFY_DELETEERROR, target, target, error, FILEOP_DELETE, 0);
			notify_copy(callback, context, SPFILENOTIFY_ENDDELETE, target, target, error, FILEOP_DELETE, 0);
		}
		setup_free(target);
	}
	callback(context, SPFILENOTIFY_ENDSUBQUEUE, FILEOP_DELETE, 0);
	return TRUE;
}

static BOOL commit_renames(file_queue *queue, PSP_FILE_CALLBACK_W callback, void *context)
{
	file_op *op;

	if (!queue->renames)
		return TRUE;
	if (callback(context, SPFILENOTIFY_STARTSUBQUEUE, FILEOP_RENAME, queue->rename_count) == FILEOP_ABORT)
		return FALSE;
	for (op = queue->renames; op; op = op->next) {
		WCHAR *source = setup_join_path(op->path, op->file), *target = op_target_path(op);
		UINT answer = notify_copy(callback, context, SPFILENOTIFY_STARTRENAME, source, target, 0, FILEOP_RENAME, 0);
		BOOL aborted = answer == FILEOP_ABORT;

		if (answer == FILEOP_DOIT) {
			DWORD error = 0;
			if (!MoveFileW(source, target))
				error = GetLastError();
			if (error)
				notify_copy(callback, context, SPFILENOTIFY_RENAMEERROR, source, target, error, FILEOP_RENAME, 0);
			notify_copy(callback, context, SPFILENOTIFY_ENDRENAME, source, target, error, FILEOP_RENAME, 0);
		}
		setup_free(source);
		setup_free(target);
		if (aborted)
			return FALSE;
	}
	callback(context, SPFILENOTIFY_ENDSUBQUEUE, FILEOP_RENAME, 0);
	return TRUE;
}

BOOL WINAPI SetupCommitFileQueueW(HWND owner, HSPFILEQ handle, PSP_FILE_CALLBACK_W callback, PVOID context)
{
	file_queue *queue = queue_of(handle);
	media_group *media;
	BOOL result = TRUE;

	if (!queue)
		return FALSE;
	if (!callback(context, SPFILENOTIFY_STARTQUEUE, (UINT_PTR)owner, 0)) {
		SetLastError(ERROR_CANCELLED);
		return FALSE;
	}
	if (!commit_deletes(queue, callback, context) || !commit_renames(queue, callback, context))
		result = FALSE;
	if (result && queue->copy_count) {
		if (callback(context, SPFILENOTIFY_STARTSUBQUEUE, FILEOP_COPY, queue->copy_count) == FILEOP_ABORT)
			result = FALSE;
		for (media = queue->media; result && media; media = media->next) {
			if (!commit_media(media, callback, context))
				result = FALSE;
		}
		if (result)
			callback(context, SPFILENOTIFY_ENDSUBQUEUE, FILEOP_COPY, 0);
	}
	{
		DWORD error = GetLastError();
		callback(context, SPFILENOTIFY_ENDQUEUE, result, 0);
		if (result)
			SetLastError(ERROR_SUCCESS);
		else
			SetLastError(error);
	}
	return result;
}

/* The ANSI commit: the callback sees ANSI copies of everything the wide engine reports. */
struct ansi_callback {
	PSP_FILE_CALLBACK_A callback;
	void *context;
};

static UINT WINAPI to_ansi_callback(void *context, UINT message, UINT_PTR param1, UINT_PTR param2)
{
	struct ansi_callback *ansi = context;
	UINT answer;

	switch (message) {
	case SPFILENOTIFY_NEEDMEDIA: {
		const SOURCE_MEDIA_W *wide = (const SOURCE_MEDIA_W *)param1;
		SOURCE_MEDIA_A media;
		char *tag = setup_to_ansi(wide->Tagfile), *description = setup_to_ansi(wide->Description);
		char *source_path = setup_to_ansi(wide->SourcePath), *source_file = setup_to_ansi(wide->SourceFile);
		char path[MAX_PATH];

		memset(&media, 0, sizeof(media));
		media.Tagfile = tag;
		media.Description = description;
		media.SourcePath = source_path;
		media.SourceFile = source_file;
		media.Flags = wide->Flags;
		path[0] = 0;
		answer = ansi->callback(ansi->context, message, (UINT_PTR)&media, (UINT_PTR)path);
		if (answer == FILEOP_NEWPATH)
			MultiByteToWideChar(CP_ACP, 0, path, -1, (WCHAR *)param2, MAX_PATH);
		setup_free(tag);
		setup_free(description);
		setup_free(source_path);
		setup_free(source_file);
		return answer;
	}
	case SPFILENOTIFY_STARTCOPY:
	case SPFILENOTIFY_ENDCOPY:
	case SPFILENOTIFY_COPYERROR:
	case SPFILENOTIFY_STARTDELETE:
	case SPFILENOTIFY_ENDDELETE:
	case SPFILENOTIFY_DELETEERROR:
	case SPFILENOTIFY_STARTRENAME:
	case SPFILENOTIFY_ENDRENAME:
	case SPFILENOTIFY_RENAMEERROR: {
		const FILEPATHS_W *wide = (const FILEPATHS_W *)param1;
		FILEPATHS_A paths;
		char *source = setup_to_ansi(wide->Source), *target = setup_to_ansi(wide->Target);
		char path[MAX_PATH];

		memset(&paths, 0, sizeof(paths));
		paths.Source = source;
		paths.Target = target;
		paths.Win32Error = wide->Win32Error;
		paths.Flags = wide->Flags;
		path[0] = 0;
		answer = ansi->callback(ansi->context, message, (UINT_PTR)&paths, param2 ? (UINT_PTR)path : 0);
		if (answer == FILEOP_NEWPATH && param2)
			MultiByteToWideChar(CP_ACP, 0, path, -1, (WCHAR *)param2, MAX_PATH);
		setup_free(source);
		setup_free(target);
		return answer;
	}
	}
	return ansi->callback(ansi->context, message, param1, param2);
}

BOOL WINAPI SetupCommitFileQueueA(HWND owner, HSPFILEQ handle, PSP_FILE_CALLBACK_A callback, PVOID context)
{
	struct ansi_callback ansi = { callback, context };

	return SetupCommitFileQueueW(owner, handle, to_ansi_callback, &ansi);
}

/* The default callback: no dialogs in this runtime, so errors skip the file and go on. */

#define DEFAULT_CONTEXT_MAGIC 0x43515053

struct default_context {
	DWORD magic;
	HWND owner;
	DWORD unused1[4];
	DWORD_PTR unused2[7];
	HWND progress;
	UINT message;
	DWORD_PTR unused3[5];
};

PVOID WINAPI SetupInitDefaultQueueCallbackEx(HWND owner, HWND progress, UINT message, DWORD reserved1, PVOID reserved2)
{
	struct default_context *context = setup_alloc(sizeof(*context));

	if (!context) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	context->magic = DEFAULT_CONTEXT_MAGIC;
	context->owner = owner;
	context->progress = progress;
	context->message = message;
	return context;
}

PVOID WINAPI SetupInitDefaultQueueCallback(HWND owner)
{
	return SetupInitDefaultQueueCallbackEx(owner, NULL, 0, 0, NULL);
}

void WINAPI SetupTermDefaultQueueCallback(PVOID context)
{
	setup_free(context);
}

static UINT default_action(UINT message)
{
	switch (message) {
	case SPFILENOTIFY_STARTQUEUE:
	case SPFILENOTIFY_STARTSUBQUEUE:
		return TRUE;
	case SPFILENOTIFY_ENDQUEUE:
	case SPFILENOTIFY_ENDSUBQUEUE:
	case SPFILENOTIFY_ENDDELETE:
	case SPFILENOTIFY_ENDRENAME:
	case SPFILENOTIFY_ENDCOPY:
		return 0;
	case SPFILENOTIFY_STARTDELETE:
	case SPFILENOTIFY_STARTRENAME:
	case SPFILENOTIFY_STARTCOPY:
	case SPFILENOTIFY_NEEDMEDIA:
		return FILEOP_DOIT;
	case SPFILENOTIFY_DELETEERROR:
	case SPFILENOTIFY_RENAMEERROR:
	case SPFILENOTIFY_COPYERROR:
		return FILEOP_SKIP;
	case SPFILENOTIFY_QUEUESCAN:
	case SPFILENOTIFY_QUEUESCAN_EX:
		return 0;
	}
	return 0;
}

UINT WINAPI SetupDefaultQueueCallbackW(PVOID context, UINT message, UINT_PTR param1, UINT_PTR param2)
{
	if (message == SPFILENOTIFY_NEEDMEDIA && param2) {
		const SOURCE_MEDIA_W *media = (const SOURCE_MEDIA_W *)param1;
		if (media->SourcePath)
			lstrcpynW((WCHAR *)param2, media->SourcePath, MAX_PATH);
	}
	return default_action(message);
}

UINT WINAPI SetupDefaultQueueCallbackA(PVOID context, UINT message, UINT_PTR param1, UINT_PTR param2)
{
	if (message == SPFILENOTIFY_NEEDMEDIA && param2) {
		const SOURCE_MEDIA_A *media = (const SOURCE_MEDIA_A *)param1;
		if (media->SourcePath)
			lstrcpynA((char *)param2, media->SourcePath, MAX_PATH);
	}
	return default_action(message);
}

/* Scanning: which queued copies would be needed. */

BOOL WINAPI SetupScanFileQueueW(HSPFILEQ handle, DWORD flags, HWND window, PSP_FILE_CALLBACK_W callback, PVOID context, PDWORD result)
{
	file_queue *queue = queue_of(handle);
	media_group *media;
	file_op *op;

	if (!queue)
		return FALSE;
	if ((flags & (SPQ_SCAN_USE_CALLBACK | SPQ_SCAN_USE_CALLBACKEX)) == (SPQ_SCAN_USE_CALLBACK | SPQ_SCAN_USE_CALLBACKEX) ||
			(flags & (SPQ_SCAN_USE_CALLBACK | SPQ_SCAN_USE_CALLBACKEX)) == 0 && !(flags & (SPQ_SCAN_FILE_PRESENCE | SPQ_SCAN_FILE_VALIDITY))) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*result = 0;
	for (media = queue->media; media; media = media->next) {
		for (op = media->copies; op; op = op->next) {
			WCHAR *target = op_target_path(op);
			if (flags & SPQ_SCAN_USE_CALLBACK) {
				if (callback(context, SPFILENOTIFY_QUEUESCAN, (UINT_PTR)target, 0)) {
					*result = 2;
					setup_free(target);
					return FALSE;
				}
			} else if (flags & SPQ_SCAN_USE_CALLBACKEX) {
				WCHAR *source = op_source_path(media->root, op->path, op->file);
				UINT answer = notify_copy(callback, context, SPFILENOTIFY_QUEUESCAN_EX, source, target, 0, 0, 0);
				setup_free(source);
				if (answer) {
					*result = 2;
					setup_free(target);
					return FALSE;
				}
			} else if (!path_exists(target)) {
				*result = 0;
				setup_free(target);
				SetLastError(ERROR_FILE_NOT_FOUND);
				return FALSE;
			}
			setup_free(target);
		}
	}
	if (!(flags & (SPQ_SCAN_USE_CALLBACK | SPQ_SCAN_USE_CALLBACKEX)))
		*result = 1;
	return TRUE;
}

struct ansi_scan {
	PSP_FILE_CALLBACK_A callback;
	void *context;
};

static UINT WINAPI scan_to_ansi(void *context, UINT message, UINT_PTR param1, UINT_PTR param2)
{
	struct ansi_scan *ansi = context;

	if (message == SPFILENOTIFY_QUEUESCAN) {
		char *target = setup_to_ansi((const WCHAR *)param1);
		UINT answer = ansi->callback(ansi->context, message, (UINT_PTR)target, param2);
		setup_free(target);
		return answer;
	}
	return to_ansi_callback(context, message, param1, param2);
}

BOOL WINAPI SetupScanFileQueueA(HSPFILEQ handle, DWORD flags, HWND window, PSP_FILE_CALLBACK_A callback, PVOID context, PDWORD result)
{
	struct ansi_scan ansi = { callback, context };

	return SetupScanFileQueueW(handle, flags, window, scan_to_ansi, &ansi, result);
}

BOOL WINAPI SetupPrepareQueueForRestoreW(HSPFILEQ handle, PCWSTR backup_path, DWORD flags)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

BOOL WINAPI SetupPrepareQueueForRestoreA(HSPFILEQ handle, PCSTR backup_path, DWORD flags)
{
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}
