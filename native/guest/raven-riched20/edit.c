/* SPDX-License-Identifier: MIT */
#include "re.h"

INT re_text_length(const re_editor *editor)
{
	return editor->doc.length - 1;
}

void re_undo_clear(re_undo_list *list)
{
	for (INT i = 0; i < list->count; i++) {
		re_doc_free_piece(&list->items[i].removed);
		if (list->items[i].before) {
			re_doc_clear(list->items[i].before);
			re_free(list->items[i].before);
		}
	}
	re_free(list->items);
	memset(list, 0, sizeof(*list));
}

static re_undo_item *push_undo(re_undo_list *list)
{
	if (list->count == list->capacity) {
		INT capacity = list->capacity ? list->capacity * 2 : 16;
		re_undo_item *grown = re_realloc(list->items, capacity * sizeof(re_undo_item));
		if (!grown)
			return NULL;
		list->items = grown;
		list->capacity = capacity;
	}
	re_undo_item *item = &list->items[list->count++];
	memset(item, 0, sizeof(*item));
	return item;
}

// The limit counts steps: the oldest whole steps give way.
static void trim_undo(re_editor *editor)
{
	re_undo_list *list = &editor->undo;
	for (;;) {
		INT steps = 0, previous = -1;
		for (INT i = 0; i < list->count; i++) {
			if (list->items[i].group != previous) {
				steps++;
				previous = list->items[i].group;
			}
		}
		if (steps <= editor->undo_limit || !list->count)
			return;
		INT group = list->items[0].group, gone = 0;
		while (gone < list->count && list->items[gone].group == group) {
			re_doc_free_piece(&list->items[gone].removed);
			if (list->items[gone].before) {
				re_doc_clear(list->items[gone].before);
				re_free(list->items[gone].before);
			}
			gone++;
		}
		memmove(list->items, list->items + gone, (list->count - gone) * sizeof(re_undo_item));
		list->count -= gone;
	}
}

void re_undo_begin_step(re_editor *editor)
{
	editor->undo_group++;
	editor->undo_open = FALSE;
}

static re_undo_list *record_list(re_editor *editor)
{
	if (editor->in_undo == 1)
		return &editor->redo;
	return &editor->undo;
}

static BOOL record_allowed(re_editor *editor)
{
	return editor->in_undo || (editor->undo_limit && !editor->undo_suspended);
}

// Stores the whole model, for changes that touch formats alone.
void re_undo_format(re_editor *editor)
{
	if (!record_allowed(editor))
		return;
	if (!editor->in_undo)
		re_undo_clear(&editor->redo);
	re_undo_item *item = push_undo(record_list(editor));
	if (!item)
		return;
	item->kind = UNDO_FORMAT;
	item->before = re_doc_clone(&editor->doc);
	item->group = editor->undo_group;
	item->sel_start = editor->sel_start;
	item->sel_end = editor->sel_end;
	if (!editor->in_undo)
		trim_undo(editor);
}

BOOL re_replace(re_editor *editor, INT at, INT remove, const WCHAR *text, INT length, const CHARFORMAT2W *format, BOOL undoable)
{
	re_doc *doc = &editor->doc;
	INT limit = doc->length - 1;
	if (at > limit)
		at = limit;
	if (at + remove > limit)
		remove = limit - at;
	if (undoable && record_allowed(editor)) {
		if (!editor->in_undo)
			re_undo_clear(&editor->redo);
		re_undo_item *item = push_undo(record_list(editor));
		if (item) {
			item->kind = UNDO_EDIT;
			item->at = at;
			item->inserted = length;
			item->group = editor->undo_group;
			item->sel_start = editor->sel_start;
			item->sel_end = editor->sel_end;
			re_doc_save_piece(doc, at, remove, &item->removed);
		}
		if (!editor->in_undo)
			trim_undo(editor);
	}
	re_ole_text_replaced(editor, at, remove, length);
	if (!re_doc_replace(doc, at, remove, text, length, format))
		return FALSE;
	return TRUE;
}

static BOOL play_step(re_editor *editor, re_undo_list *from, INT mode)
{
	if (!from->count)
		return FALSE;
	INT group = from->items[from->count - 1].group;
	INT saved_group = editor->undo_group;
	editor->in_undo = mode;
	editor->undo_group = ++editor->undo_group;
	INT sel_start = editor->sel_start, sel_end = editor->sel_end;
	while (from->count && from->items[from->count - 1].group == group) {
		re_undo_item item = from->items[--from->count];
		if (item.kind == UNDO_FORMAT) {
			re_undo_format(editor);
			re_doc_assign(&editor->doc, item.before);
			re_free(item.before);
		} else {
			CHARFORMAT2W format;
			re_doc_char_format(&editor->doc, item.at, &format);
			re_replace(editor, item.at, item.inserted, NULL, 0, &format, TRUE);
			if (mode == 1 && editor->redo.count)
				editor->redo.items[editor->redo.count - 1].inserted = item.removed.length;
			else if (mode == 2 && editor->undo.count)
				editor->undo.items[editor->undo.count - 1].inserted = item.removed.length;
			if (item.removed.length)
				re_doc_put_piece(&editor->doc, item.at, &item.removed);
			re_doc_free_piece(&item.removed);
		}
		sel_start = item.sel_start;
		sel_end = item.sel_end;
	}
	editor->in_undo = 0;
	editor->undo_group = saved_group;
	re_undo_begin_step(editor);
	editor->sel_start = sel_start;
	editor->sel_end = sel_end;
	re_editor_fix_selection(editor);
	return TRUE;
}

BOOL re_undo(re_editor *editor)
{
	if (editor->undo_suspended)
		return FALSE;
	return play_step(editor, &editor->undo, 1);
}

BOOL re_redo(re_editor *editor)
{
	if (editor->undo_suspended)
		return FALSE;
	return play_step(editor, &editor->redo, 2);
}

void re_editor_fix_selection(re_editor *editor)
{
	INT top = editor->doc.length;
	if (editor->sel_start < 0)
		editor->sel_start = 0;
	if (editor->sel_end < 0)
		editor->sel_end = 0;
	if (editor->sel_start > top)
		editor->sel_start = top;
	if (editor->sel_end > top)
		editor->sel_end = top;
}

INT re_count_crlf(const re_editor *editor, INT start, INT end)
{
	INT count = 0;
	for (INT i = start; i < end && i < editor->doc.length; i++) {
		count++;
		if (editor->doc.text[i] == RE_CR)
			count++;
	}
	return count;
}

// Copies characters to the buffer, whole pairs only; the terminator is left to the caller. Returns what was written.
INT re_get_text_crlf(const re_editor *editor, INT start, INT end, WCHAR *out, INT capacity, BOOL crlf)
{
	INT written = 0;
	for (INT i = start; i < end && i < editor->doc.length; i++) {
		WCHAR c = editor->doc.text[i];
		if (crlf && c == RE_CR) {
			if (written + 2 > capacity)
				break;
			out[written++] = RE_CR;
			out[written++] = RE_LF;
		} else {
			if (written >= capacity)
				break;
			out[written++] = c;
		}
	}
	return written;
}

void re_notify_selection(re_editor *editor)
{
	if (editor->notify_lock || !(editor->event_mask & ENM_SELCHANGE))
		return;
	SELCHANGE change;
	memset(&change, 0, sizeof(change));
	change.chrg.cpMin = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	change.chrg.cpMax = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
	change.seltyp = change.chrg.cpMin == change.chrg.cpMax ? SEL_EMPTY : SEL_TEXT;
	if (change.chrg.cpMax - change.chrg.cpMin > 1)
		change.seltyp |= SEL_MULTICHAR;
	re_send_notify(editor, EN_SELCHANGE, &change.nmhdr);
}

void re_notify_change(re_editor *editor)
{
	if (editor->notify_lock)
		return;
	if (editor->host) {
		if (editor->event_mask & ENM_CHANGE)
			ITextHost_TxNotify(editor->host, EN_CHANGE, NULL);
		return;
	}
	if (!editor->hwnd)
		return;
	HWND parent = editor->parent;
	if (!parent)
		return;
	DWORD mask = editor->event_mask;
	if (mask & ENM_UPDATE && IsWindowVisible(editor->hwnd))
		SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(editor->hwnd, GWLP_ID), EN_UPDATE), (LPARAM)editor->hwnd);
	// The owner may change the text from the notification without causing another.
	editor->event_mask &= ~ENM_CHANGE;
	if (mask & ENM_CHANGE)
		SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(editor->hwnd, GWLP_ID), EN_CHANGE), (LPARAM)editor->hwnd);
	editor->event_mask = mask;
}

void re_refresh(re_editor *editor)
{
	re_layout_update(editor);
	re_update_scrollbars(editor);
	re_invalidate(editor);
	re_update_caret(editor);
}

void re_set_selection(re_editor *editor, INT anchor, INT caret)
{
	INT before_start = editor->sel_start, before_end = editor->sel_end;
	editor->sel_start = anchor;
	editor->sel_end = caret;
	re_editor_fix_selection(editor);
	if (editor->sel_start != before_start || editor->sel_end != before_end) {
		editor->has_insert_format = FALSE;
		re_invalidate(editor);
		re_notify_selection(editor);
	}
	re_update_caret(editor);
}

// EM_SETSEL's rules: a start below zero drops the selection, to its end or, with both below zero, to the end of the text; an end below zero means
// the whole rest, marks included; a start past the text collapses at its end.
void re_set_selection_user(re_editor *editor, INT start, INT end)
{
	INT all = editor->doc.length, text = all - 1;
	if (start < 0) {
		INT here = text;
		if (end >= 0) {
			here = editor->sel_start > editor->sel_end ? editor->sel_start : editor->sel_end;
			here = here > text ? text : here;
		}
		re_set_selection(editor, here, here);
		return;
	}
	if (end < 0)
		end = all;
	if (start > end) {
		INT swap = start;
		start = end;
		end = swap;
	}
	if (end > all)
		end = all;
	if (start > text)
		start = end = text;
	re_set_selection(editor, start, end);
}

void re_get_selection_format(re_editor *editor, CHARFORMAT2W *out, DWORD *differing)
{
	INT start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	INT end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
	*differing = 0;
	if (start == end) {
		if (editor->has_insert_format) {
			*out = editor->insert_format;
		} else {
			// The caret takes on the look of the character before it, within its paragraph.
			INT para = re_doc_para_at(&editor->doc, start);
			INT from = start > editor->doc.paras[para].start ? start - 1 : start;
			re_doc_char_format(&editor->doc, from, out);
		}
		out->dwMask = CFM_ALL2;
		return;
	}
	if (end > editor->doc.length - 1)
		end = editor->doc.length - 1 > start ? editor->doc.length - 1 : editor->doc.length;
	re_doc_char_format(&editor->doc, start, out);
	DWORD same = CFM_ALL2;
	for (INT i = re_doc_run_at(&editor->doc, start) + 1; i < editor->doc.run_count && editor->doc.runs[i].start < end; i++) {
		const CHARFORMAT2W *other = &editor->doc.runs[i].format;
		static const struct { DWORD mask, effect; } effects[] = {
			{CFM_BOLD, CFE_BOLD}, {CFM_ITALIC, CFE_ITALIC}, {CFM_UNDERLINE, CFE_UNDERLINE}, {CFM_STRIKEOUT, CFE_STRIKEOUT},
			{CFM_PROTECTED, CFE_PROTECTED}, {CFM_LINK, CFE_LINK}, {CFM_SUBSCRIPT, CFE_SUBSCRIPT}, {CFM_SUPERSCRIPT, CFE_SUPERSCRIPT},
			{CFM_COLOR, CFE_AUTOCOLOR}, {CFM_BACKCOLOR, CFE_AUTOBACKCOLOR},
		};
		for (size_t e = 0; e < ARRAYSIZE(effects); e++) {
			// A mixed effect reads as set, with its mask cleared.
			if ((out->dwEffects ^ other->dwEffects) & effects[e].effect) {
				same &= ~effects[e].mask;
				out->dwEffects |= effects[e].effect;
			}
		}
		if (out->crTextColor != other->crTextColor)
			same &= ~CFM_COLOR;
		if (out->crBackColor != other->crBackColor)
			same &= ~CFM_BACKCOLOR;
		if (out->yHeight != other->yHeight)
			same &= ~CFM_SIZE;
		if (out->yOffset != other->yOffset)
			same &= ~CFM_OFFSET;
		if (out->bCharSet != other->bCharSet)
			same &= ~CFM_CHARSET;
		if (out->wWeight != other->wWeight)
			same &= ~CFM_WEIGHT;
		if (lstrcmpW(out->szFaceName, other->szFaceName))
			same &= ~CFM_FACE;
		if (out->sSpacing != other->sSpacing)
			same &= ~CFM_SPACING;
		if (out->lcid != other->lcid)
			same &= ~CFM_LCID;
		if (out->sStyle != other->sStyle)
			same &= ~CFM_STYLE;
		if (out->wKerning != other->wKerning)
			same &= ~CFM_KERNING;
		if (out->bUnderlineType != other->bUnderlineType)
			same &= ~CFM_UNDERLINETYPE;
	}
	out->dwMask = same;
	*differing = CFM_ALL2 & ~same;
}

static BOOL range_protected(const re_editor *editor, INT start, INT end)
{
	for (INT i = re_doc_run_at(&editor->doc, start); i < editor->doc.run_count && editor->doc.runs[i].start < end; i++) {
		if (editor->doc.runs[i].format.dwEffects & CFE_PROTECTED)
			return TRUE;
	}
	return FALSE;
}

// EN_PROTECTED lets the owner refuse a change that touches protected text.
static BOOL may_change(re_editor *editor, INT start, INT end, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (start == end || !(editor->event_mask & ENM_PROTECTED) || !range_protected(editor, start, end))
		return TRUE;
	ENPROTECTED protect;
	memset(&protect, 0, sizeof(protect));
	protect.msg = message;
	protect.wParam = wparam;
	protect.lParam = lparam;
	protect.chrg.cpMin = start;
	protect.chrg.cpMax = end;
	re_send_notify(editor, EN_PROTECTED, &protect.nmhdr);
	return FALSE;
}

void re_apply_format(re_editor *editor, DWORD flags, const CHARFORMAT2W *format)
{
	if (flags & SCF_ALL) {
		re_undo_format(editor);
		re_doc_set_format(&editor->doc, 0, editor->doc.length, format);
		editor->has_insert_format = FALSE;
		return;
	}
	INT start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	INT end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
	if (start == end || (editor->doc.length == 1 && (flags & SCF_SELECTION))) {
		// Nothing selected: what is typed next changes, and an empty document's mark goes along.
		CHARFORMAT2W look;
		DWORD differing;
		re_get_selection_format(editor, &look, &differing);
		re_format_merge(&look, format);
		editor->insert_format = look;
		editor->has_insert_format = TRUE;
		if (editor->doc.length == 1)
			re_doc_set_format(&editor->doc, 0, 1, format);
		return;
	}
	re_undo_format(editor);
	re_doc_set_format(&editor->doc, start, end, format);
}

void re_delete_selection(re_editor *editor)
{
	INT start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	INT end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
	if (end > editor->doc.length - 1)
		end = editor->doc.length - 1;
	if (start >= end)
		return;
	if (!may_change(editor, start, end, WM_CLEAR, 0, 0))
		return;
	CHARFORMAT2W format;
	re_doc_char_format(&editor->doc, start, &format);
	re_replace(editor, start, end - start, NULL, 0, &format, TRUE);
	editor->sel_start = editor->sel_end = start;
	editor->modified = TRUE;
}

void re_insert_typed(re_editor *editor, const WCHAR *text, INT length)
{
	INT start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	INT end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
	CHARFORMAT2W format;
	DWORD differing;
	if (end > editor->doc.length - 1)
		end = editor->doc.length - 1;
	re_get_selection_format(editor, &format, &differing);
	if (start != end && !editor->has_insert_format)
		re_doc_char_format(&editor->doc, start, &format);
	format.dwMask = CFM_ALL2;
	re_replace(editor, start, end - start, text, length, &format, TRUE);
	editor->sel_start = editor->sel_end = start + length;
	editor->modified = TRUE;
	editor->has_insert_format = FALSE;
}

static BOOL is_url_char(WCHAR c)
{
	return c > ' ' && c != RE_CR;
}

// Punctuation that belongs to the address only when more of it follows.
static BOOL is_url_neutral(WCHAR c)
{
	return c && wcschr(L"-!'()[]{}\",.;:?<>~`", c) != NULL;
}

static BOOL url_scheme_at(const WCHAR *text, INT left)
{
	static const WCHAR *const schemes[] = {L"http:", L"https:", L"ftp:", L"file:", L"mailto:", L"news:", L"nntp:", L"gopher:", L"telnet:", L"prospero:", L"wais:", L"notes:", L"www."};
	for (size_t i = 0; i < ARRAYSIZE(schemes); i++) {
		INT n = lstrlenW(schemes[i]), at = 0;
		while (at < n && at < left && (WCHAR)(ULONG_PTR)CharLowerW((WCHAR *)(ULONG_PTR)text[at]) == schemes[i][at])
			at++;
		if (at == n)
			return TRUE;
	}
	return FALSE;
}

// Marks what looks like a link: a known scheme at the start of a word, up to the next blank.
void re_detect_urls(re_editor *editor)
{
	if (!editor->auto_url)
		return;
	re_doc *doc = &editor->doc;
	CHARFORMAT2W link, plain;
	memset(&link, 0, sizeof(link));
	link.cbSize = sizeof(link);
	link.dwMask = CFM_LINK;
	link.dwEffects = CFE_LINK;
	plain = link;
	plain.dwEffects = 0;
	INT i = 0;
	while (i < doc->length) {
		INT gap = i;
		while (i < doc->length && !is_url_char(doc->text[i]))
			i++;
		if (gap != i)
			re_doc_set_format(doc, gap, i, &plain);
		INT start = i;
		while (i < doc->length && is_url_char(doc->text[i]))
			i++;
		if (start == i)
			continue;
		CHARFORMAT2W here;
		re_doc_char_format(doc, start, &here);
		INT from = start;
		while (from < i && is_url_neutral(doc->text[from]))
			from++;
		BOOL hit = url_scheme_at(doc->text + from, i - from);
		INT end = i;
		while (hit && end > from && is_url_neutral(doc->text[end - 1]))
			end--;
		re_doc_set_format(doc, start, i, &plain);
		if (hit)
			re_doc_set_format(doc, from, end, &link);
	}
}

BOOL re_editor_create(re_editor *editor)
{
	CHARFORMAT2W format;
	re_default_char_format(&format, NULL);
	if (!re_doc_init(&editor->doc, &format))
		return FALSE;
	editor->default_format = format;
	editor->limit = 32767;
	editor->undo_limit = editor->undo_limit_kept = 100;
	editor->zoom_num = editor->zoom_den = 0;
	editor->back_system = TRUE;
	editor->text_mode = TM_RICHTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE;
	editor->border_x = 1;
	editor->border_y = 1;
	editor->wrap = TRUE;
	editor->link_start = editor->link_end = -1;
	return TRUE;
}

void re_editor_destroy(re_editor *editor)
{
	if (editor->ole) {
		re_ole_detach(editor->ole);
		editor->ole = NULL;
	}
	if (editor->callback)
		editor->callback->lpVtbl->Release(editor->callback);
	re_undo_clear(&editor->undo);
	re_undo_clear(&editor->redo);
	re_layout_free(&editor->layout);
	re_doc_clear(&editor->doc);
}
