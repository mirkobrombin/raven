/* SPDX-License-Identifier: MIT */
#include "re.h"
#include <commctrl.h>

static HINSTANCE module;

HINSTANCE re_instance(void)
{
	return module;
}

LRESULT re_send_notify(re_editor *editor, UINT code, NMHDR *header)
{
	if (editor->host)
		return ITextHost_TxNotify(editor->host, code, header);
	HWND parent = editor->parent;
	if (!editor->hwnd || !parent)
		return 0;
	header->hwndFrom = editor->hwnd;
	header->idFrom = GetWindowLongPtrW(editor->hwnd, GWLP_ID);
	header->code = code;
	return SendMessageW(parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

static void selection_range(const re_editor *editor, INT *start, INT *end)
{
	*start = editor->sel_start < editor->sel_end ? editor->sel_start : editor->sel_end;
	*end = editor->sel_start < editor->sel_end ? editor->sel_end : editor->sel_start;
}

static UINT editor_code_page(const re_editor *editor)
{
	(void)editor;
	return CP_ACP;
}

// Turns any line ending into the one mark the model holds; a plain single-line control stops at the first.
static WCHAR *normalize_text(const re_editor *editor, const WCHAR *text, INT length, INT *out_length)
{
	WCHAR *result = re_alloc((length + 1) * sizeof(WCHAR));
	if (!result)
		return NULL;
	INT written = 0;
	for (INT i = 0; i < length; i++) {
		WCHAR c = text[i];
		if (c == RE_CR || c == RE_LF) {
			if (!(editor->style & ES_MULTILINE) && !editor->host)
				break;
			if (c == RE_CR && i + 1 < length && text[i + 1] == RE_LF)
				i++;
			else if (c == RE_CR && i + 2 < length && text[i + 1] == RE_CR && text[i + 2] == RE_LF) {
				// The soft break some programs write reads as a space.
				i += 2;
				result[written++] = ' ';
				continue;
			}
			c = RE_CR;
		}
		result[written++] = c;
	}
	result[written] = 0;
	*out_length = written;
	return result;
}

static INT room_left(const re_editor *editor, INT replaced)
{
	// A negative limit is a huge unsigned one.
	if (editor->limit < 0)
		return 0x7fffffff;
	INT left = editor->limit - (re_text_length(editor) - replaced);
	return left > 0 ? left : 0;
}

static void after_edit(re_editor *editor)
{
	re_detect_urls(editor);
	re_refresh(editor);
	re_scroll_to_caret(editor);
	re_notify_change(editor);
	re_notify_selection(editor);
}

static void set_all_text(re_editor *editor, const WCHAR *text, INT length, const CHARFORMAT2W *format)
{
	CHARFORMAT2W look;
	look = format ? *format : editor->default_format;
	WCHAR *clean = NULL;
	INT clean_length = 0;
	if (text)
		clean = normalize_text(editor, text, length, &clean_length);
	re_ole_text_reset(editor);
	re_doc_clear(&editor->doc);
	re_doc_init(&editor->doc, &look);
	if (clean && clean_length) {
		re_doc_replace(&editor->doc, 0, 0, clean, clean_length, &look);
	}
	re_free(clean);
	re_undo_clear(&editor->undo);
	re_undo_clear(&editor->redo);
	re_undo_begin_step(editor);
	editor->sel_start = editor->sel_end = 0;
	editor->has_insert_format = FALSE;
	editor->scroll_x = editor->scroll_y = 0;
}

static BOOL looks_like_rtf(const char *text, INT length)
{
	return (length >= 5 && !memcmp(text, "{\\rtf", 5)) || (length >= 6 && !memcmp(text, "{\\urtf", 6));
}

typedef struct
{
	const char *data;
	INT left;
} memory_stream;

static DWORD CALLBACK memory_reader(DWORD_PTR cookie, BYTE *buffer, LONG size, LONG *read)
{
	memory_stream *stream = (memory_stream *)cookie;
	LONG take = stream->left < size ? stream->left : size;
	memcpy(buffer, stream->data, take);
	stream->data += take;
	stream->left -= take;
	*read = take;
	return 0;
}

static BOOL stream_rtf(re_editor *editor, const char *text, INT length, WPARAM flags)
{
	memory_stream source = {text, length};
	EDITSTREAM stream = {(DWORD_PTR)&source, 0, memory_reader};
	re_stream_in(editor, SF_RTF | flags, &stream);
	return stream.dwError == 0;
}

// The host hands an ANSI window WM_SETTEXT, EM_REPLACESEL, WM_GETTEXT and the creation title as UTF-16, whichever function sent them; the
// messages it does not know about keep their narrow text.
static BOOL wide_is_rtf(const WCHAR *text)
{
	return !wcsncmp(text, L"{\\rtf", 5) || !wcsncmp(text, L"{\\urtf", 6);
}

static LRESULT on_set_text(re_editor *editor, const WCHAR *text)
{
	// Text the ANSI function sent is a document if it says so. On a wide window a wide string is only ever text; a document there comes as narrow bytes.
	const char *narrow = (const char *)text;
	if (text && editor->ansi && wide_is_rtf(text)) {
		INT size;
		char *bytes = re_wide_to_ansi(text, -1, CP_ACP, &size);
		if (bytes) {
			stream_rtf(editor, bytes, size, 0);
			re_free(bytes);
			re_refresh(editor);
			return 1;
		}
	} else if (text && !editor->ansi && looks_like_rtf(narrow, lstrlenA(narrow))) {
		stream_rtf(editor, narrow, lstrlenA(narrow), 0);
		re_refresh(editor);
		return 1;
	}
	if (text && lstrlenW(text) > editor->limit)
		editor->limit = lstrlenW(text);
	BOOL moved = editor->sel_start || editor->sel_end;
	set_all_text(editor, text, text ? lstrlenW(text) : 0, NULL);
	editor->modified = FALSE;
	re_detect_urls(editor);
	re_refresh(editor);
	re_notify_change(editor);
	if (moved)
		re_notify_selection(editor);
	return 1;
}

static LRESULT copy_out(const re_editor *editor, const WCHAR *text, INT length, void *out, INT capacity)
{
	if (!out || capacity <= 0)
		return 0;
	if (editor->ansi) {
		INT got = WideCharToMultiByte(editor_code_page(editor), 0, text, length, out, capacity - 1, NULL, NULL);
		((char *)out)[got] = 0;
		return got;
	}
	INT take = length < capacity - 1 ? length : capacity - 1;
	memcpy(out, text, take * sizeof(WCHAR));
	((WCHAR *)out)[take] = 0;
	return take;
}

// Plain text has no way to hold an object, so a space stands in for its mark.
static void objects_to_spaces(WCHAR *text, INT length)
{
	for (INT i = 0; i < length; i++)
		if (text[i] == RE_OBJECT_MARK)
			text[i] = ' ';
}

static LRESULT on_get_text(re_editor *editor, INT start, INT end, void *out, INT capacity, BOOL crlf)
{
	INT text = re_text_length(editor);
	if (end < 0 || end > text)
		end = text;
	if (start > end)
		start = end;
	INT size = crlf ? re_count_crlf(editor, start, end) : end - start;
	WCHAR *buffer = re_alloc((size + 1) * sizeof(WCHAR));
	if (!buffer)
		return 0;
	INT got = re_get_text_crlf(editor, start, end, buffer, size, crlf);
	objects_to_spaces(buffer, got);
	LRESULT result = 0;
	if (out && capacity > 0) {
		result = got < capacity - 1 ? got : capacity - 1;
		memcpy(out, buffer, result * sizeof(WCHAR));
		((WCHAR *)out)[result] = 0;
	}
	re_free(buffer);
	return result;
}

static LRESULT text_length_ex(re_editor *editor, const GETTEXTLENGTHEX *ex)
{
	if (!ex)
		return E_INVALIDARG;
	if ((ex->flags & GTL_NUMCHARS) && (ex->flags & GTL_NUMBYTES))
		return E_INVALIDARG;
	if ((ex->flags & GTL_PRECISE) && (ex->flags & GTL_CLOSE))
		return E_INVALIDARG;
	INT text = re_text_length(editor);
	INT count = (ex->flags & GTL_USECRLF) ? re_count_crlf(editor, 0, text) : text;
	if ((ex->flags & (GTL_NUMBYTES | GTL_PRECISE | GTL_CLOSE)) && !(ex->flags & GTL_NUMCHARS)) {
		if (ex->codepage == 1200)
			return count * 2;
		WCHAR *buffer = re_alloc((count + 1) * sizeof(WCHAR));
		if (!buffer)
			return 0;
		INT got = re_get_text_crlf(editor, 0, text, buffer, count, (ex->flags & GTL_USECRLF) != 0);
		INT bytes = WideCharToMultiByte(ex->codepage, 0, buffer, got, NULL, 0, NULL, NULL);
		re_free(buffer);
		return bytes;
	}
	return count;
}

static LRESULT on_get_text_ex(re_editor *editor, const GETTEXTEX *ex, void *out)
{
	if (!ex || !out || !ex->cb)
		return 0;
	INT start = 0, end = re_text_length(editor);
	if (ex->flags & GT_SELECTION) {
		selection_range(editor, &start, &end);
		if (end > re_text_length(editor))
			end = re_text_length(editor);
	}
	BOOL crlf = (ex->flags & GT_USECRLF) != 0;
	INT size = crlf ? re_count_crlf(editor, start, end) : end - start;
	WCHAR *buffer = re_alloc((size + 1) * sizeof(WCHAR));
	if (!buffer)
		return 0;
	INT got = re_get_text_crlf(editor, start, end, buffer, size, crlf);
	if (crlf)
		objects_to_spaces(buffer, got);
	LRESULT result;
	if (ex->codepage == 1200) {
		INT capacity = ex->cb / sizeof(WCHAR);
		INT take = got < capacity - 1 ? got : capacity - 1;
		// A line ending is copied whole or not at all.
		if (take && take < got && buffer[take - 1] == RE_CR && buffer[take] == RE_LF)
			take--;
		memcpy(out, buffer, take * sizeof(WCHAR));
		((WCHAR *)out)[take] = 0;
		result = take;
	} else {
		BOOL used = FALSE;
		INT made = WideCharToMultiByte(ex->codepage, 0, buffer, got, out, ex->cb - 1, ex->lpDefaultChar, ex->lpUsedDefChar ? &used : NULL);
		((char *)out)[made] = 0;
		if (ex->lpUsedDefChar)
			*ex->lpUsedDefChar = used;
		result = made;
	}
	re_free(buffer);
	return result;
}

static void replace_selection(re_editor *editor, const WCHAR *text, INT length, BOOL undoable)
{
	INT start, end;
	selection_range(editor, &start, &end);
	if (end > re_text_length(editor))
		end = re_text_length(editor);
	INT clean_length = 0;
	WCHAR *clean = normalize_text(editor, text, length, &clean_length);
	if (!clean)
		return;
	INT room = room_left(editor, end - start);
	if (clean_length > room)
		clean_length = room;
	CHARFORMAT2W format;
	DWORD differing;
	re_get_selection_format(editor, &format, &differing);
	if (start != end)
		re_doc_char_format(&editor->doc, start, &format);
	format.dwMask = CFM_ALL2;
	re_undo_begin_step(editor);
	re_replace(editor, start, end - start, clean, clean_length, &format, undoable);
	re_undo_begin_step(editor);
	editor->sel_start = editor->sel_end = start + clean_length;
	editor->has_insert_format = FALSE;
	editor->modified = TRUE;
	re_free(clean);
	if (!undoable) {
		re_undo_clear(&editor->undo);
		re_undo_clear(&editor->redo);
	}
}

static LRESULT on_set_text_ex(re_editor *editor, const SETTEXTEX *ex, const void *text)
{
	if (!ex)
		return 0;
	INT length = 0;
	WCHAR *wide = NULL;
	const char *narrow = text;
	if (text && looks_like_rtf(narrow, lstrlenA(narrow))) {
		if (ex->flags & ST_SELECTION) {
			INT start, end;
			selection_range(editor, &start, &end);
			if (end > re_text_length(editor))
				end = re_text_length(editor);
			if (start != end) {
				CHARFORMAT2W format;
				re_doc_char_format(&editor->doc, start, &format);
				re_undo_begin_step(editor);
				re_replace(editor, start, end - start, NULL, 0, &format, TRUE);
			}
			editor->sel_start = editor->sel_end = start;
			stream_rtf(editor, narrow, lstrlenA(narrow), SFF_SELECTION);
			re_refresh(editor);
			re_scroll_to_caret(editor);
		} else {
			stream_rtf(editor, narrow, lstrlenA(narrow), 0);
		}
		re_refresh(editor);
		return 1;
	}
	if (text) {
		if (ex->codepage == 1200) {
			length = lstrlenW(text);
		} else {
			// A UTF-8 mark decides the code page, whatever the caller named.
			BOOL marked = !memcmp(narrow, "\xef\xbb\xbf", lstrlenA(narrow) < 3 ? 0 : 3) && lstrlenA(narrow) >= 3;
			wide = re_ansi_to_wide(marked ? narrow + 3 : narrow, -1, marked ? CP_UTF8 : ex->codepage, &length);
			text = wide;
		}
	}
	if (ex->flags & ST_SELECTION) {
		replace_selection(editor, text ? text : L"", length, (ex->flags & ST_KEEPUNDO) != 0);
		re_free(wide);
		after_edit(editor);
		return length;
	}
	if (ex->flags & ST_KEEPUNDO) {
		INT clean_length;
		WCHAR *clean = normalize_text(editor, text ? text : L"", length, &clean_length);
		if (clean) {
			CHARFORMAT2W format;
			re_doc_char_format(&editor->doc, 0, &format);
			re_undo_begin_step(editor);
			re_replace(editor, 0, re_text_length(editor), clean, clean_length, &format, TRUE);
			re_undo_begin_step(editor);
			re_free(clean);
		}
		editor->sel_start = editor->sel_end = 0;
	} else {
		set_all_text(editor, text, length, NULL);
	}
	re_free(wide);
	editor->modified = (ex->flags & ST_KEEPUNDO) != 0;
	re_detect_urls(editor);
	re_refresh(editor);
	re_notify_change(editor);
	return 1;
}

static BOOL word_char(WCHAR c)
{
	WORD type = 0;
	GetStringTypeW(CT_CTYPE1, &c, 1, &type);
	return (type & (C1_ALPHA | C1_DIGIT)) != 0 || c == '_';
}

static INT class_of(WCHAR c)
{
	if (c == ' ' || c == RE_TAB || c == RE_CR || c == RE_LF)
		return 0;
	return word_char(c) ? 1 : 2;
}

// Word movement stops at the start of the next word; a mark is a stop of its own.
static INT next_word(const re_editor *editor, INT at)
{
	const WCHAR *text = editor->doc.text;
	INT end = editor->doc.length - 1;
	if (editor->word_break)
		return editor->word_break((WCHAR *)text, at, end * sizeof(WCHAR), WB_MOVEWORDRIGHT);
	if (at >= end)
		return end;
	INT type = class_of(text[at]);
	if (text[at] == RE_CR)
		return at + 1;
	while (at < end && type && class_of(text[at]) == type)
		at++;
	while (at < end && text[at] != RE_CR && !class_of(text[at]))
		at++;
	return at;
}

static INT previous_word(const re_editor *editor, INT at)
{
	const WCHAR *text = editor->doc.text;
	if (editor->word_break)
		return editor->word_break((WCHAR *)text, at, (editor->doc.length - 1) * sizeof(WCHAR), WB_MOVEWORDLEFT);
	if (at <= 0)
		return 0;
	if (text[at - 1] == RE_CR)
		return at - 1;
	while (at > 0 && text[at - 1] != RE_CR && !class_of(text[at - 1]))
		at--;
	if (at <= 0)
		return 0;
	if (text[at - 1] == RE_CR)
		return at;
	INT type = class_of(text[at - 1]);
	while (at > 0 && class_of(text[at - 1]) == type)
		at--;
	return at;
}

static void move_caret(re_editor *editor, INT to, BOOL extend, BOOL at_line_start)
{
	editor->caret_at_line_start = at_line_start;
	re_set_selection(editor, extend ? editor->sel_start : to, to);
	re_scroll_to_caret(editor);
	re_update_caret(editor);
}

static void move_vertically(re_editor *editor, INT lines, BOOL extend)
{
	const re_layout *layout = &editor->layout;
	if (!layout->count)
		return;
	INT index = re_line_at(layout, editor->sel_end, editor->caret_at_line_start);
	POINT here;
	INT height;
	re_position(editor, editor->sel_end, editor->caret_at_line_start, &here, &height);
	INT target = index + lines;
	if (target < 0) {
		move_caret(editor, 0, extend, FALSE);
		return;
	}
	if (target >= layout->count) {
		move_caret(editor, editor->doc.length - 1, extend, FALSE);
		return;
	}
	RECT view;
	re_view_rect(editor, &view);
	INT y = view.top + layout->lines[target].y - editor->scroll_y;
	BOOL start_of_line;
	INT to = re_char_from_point(editor, here.x, y, &start_of_line);
	move_caret(editor, to, extend, start_of_line);
}

static void on_key(re_editor *editor, UINT key)
{
	BOOL shift = GetKeyState(VK_SHIFT) < 0, control = GetKeyState(VK_CONTROL) < 0;
	const re_layout *layout = &editor->layout;
	INT start, end;
	selection_range(editor, &start, &end);
	switch (key) {
	case VK_LEFT:
		if (start != end && !shift)
			move_caret(editor, start, FALSE, FALSE);
		else if (control)
			move_caret(editor, previous_word(editor, editor->sel_end), shift, TRUE);
		else if (editor->sel_end > 0)
			move_caret(editor, editor->sel_end - 1, shift, TRUE);
		break;
	case VK_RIGHT:
		if (start != end && !shift)
			move_caret(editor, end, FALSE, FALSE);
		else if (control)
			move_caret(editor, next_word(editor, editor->sel_end), shift, TRUE);
		else if (editor->sel_end < editor->doc.length - 1)
			move_caret(editor, editor->sel_end + 1, shift, TRUE);
		else if (shift)
			move_caret(editor, editor->doc.length, TRUE, FALSE);
		break;
	case VK_UP:
		move_vertically(editor, -1, shift);
		break;
	case VK_DOWN:
		move_vertically(editor, 1, shift);
		break;
	case VK_HOME:
		if (control) {
			move_caret(editor, 0, shift, FALSE);
		} else if (layout->count) {
			move_caret(editor, layout->lines[re_line_at(layout, editor->sel_end, editor->caret_at_line_start)].start, shift, TRUE);
		}
		break;
	case VK_END:
		if (control) {
			move_caret(editor, editor->doc.length - 1, shift, FALSE);
		} else if (layout->count) {
			const re_line *line = &layout->lines[re_line_at(layout, editor->sel_end, editor->caret_at_line_start)];
			INT to = line->start + line->length;
			if (editor->doc.text[to - 1] == RE_CR)
				to--;
			move_caret(editor, to, shift, FALSE);
		}
		break;
	case VK_PRIOR:
	case VK_NEXT: {
		RECT view;
		re_view_rect(editor, &view);
		INT height = layout->count ? layout->lines[0].height : 1;
		INT lines = (view.bottom - view.top) / (height ? height : 1);
		if (lines < 1)
			lines = 1;
		move_vertically(editor, key == VK_NEXT ? lines : -lines, shift);
		break;
	}
	case VK_DELETE:
	case VK_BACK:
		if (editor->read_only) {
			if (editor->host == NULL)
				MessageBeep(MB_ICONERROR);
			break;
		}
		if (start == end) {
			if (key == VK_DELETE) {
				INT to = control ? next_word(editor, start) : start + 1;
				if (start >= editor->doc.length - 1)
					break;
				editor->sel_start = start;
				editor->sel_end = to;
			} else {
				if (start <= 0)
					break;
				editor->sel_start = control ? previous_word(editor, start) : start - 1;
				editor->sel_end = start;
			}
		}
		// Rubbing out while typing stays in the typing's undo step.
		if (key == VK_BACK && editor->undo_open) {
			re_delete_selection(editor);
		} else {
			re_undo_begin_step(editor);
			re_delete_selection(editor);
			re_undo_begin_step(editor);
		}
		after_edit(editor);
		break;
	case 'A':
		if (control)
			re_set_selection(editor, 0, editor->doc.length);
		break;
	case 'C':
		if (control)
			re_message(editor, WM_COPY, 0, 0, NULL);
		break;
	case 'X':
		if (control)
			re_message(editor, WM_CUT, 0, 0, NULL);
		break;
	case 'V':
		if (control)
			re_message(editor, WM_PASTE, 0, 0, NULL);
		break;
	case 'Z':
		if (control)
			re_message(editor, EM_UNDO, 0, 0, NULL);
		break;
	case 'Y':
		if (control)
			re_message(editor, EM_REDO, 0, 0, NULL);
		break;
	case VK_INSERT:
		if (control)
			re_message(editor, WM_COPY, 0, 0, NULL);
		else if (shift)
			re_message(editor, WM_PASTE, 0, 0, NULL);
		break;
	default:
		break;
	}
}

static void on_char(re_editor *editor, WCHAR c)
{
	if (editor->read_only)
		return;
	if (c < ' ' && c != RE_TAB && c != RE_CR && c != RE_LF)
		return;
	if (c == RE_LF && !(editor->style & ES_MULTILINE))
		return;
	if ((c == RE_CR || c == RE_LF) && !(editor->style & ES_MULTILINE) && !editor->host)
		return;
	if (c == RE_TAB && !(editor->style & ES_MULTILINE) && !editor->host)
		return;
	if (c == RE_LF)
		c = RE_CR;
	INT start, end;
	selection_range(editor, &start, &end);
	if (end > re_text_length(editor))
		end = re_text_length(editor);
	if (room_left(editor, end - start) < 1) {
		if (!editor->host)
			MessageBeep(MB_ICONERROR);
		return;
	}
	if (!editor->undo_open || start != end) {
		re_undo_begin_step(editor);
		editor->undo_open = TRUE;
	}
	re_insert_typed(editor, &c, 1);
	editor->caret_at_line_start = TRUE;
	if (c == RE_CR)
		re_undo_begin_step(editor);
	after_edit(editor);
}

static HGLOBAL text_to_global(const WCHAR *text, INT length)
{
	HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, (length + 1) * sizeof(WCHAR));
	if (!global)
		return NULL;
	WCHAR *data = GlobalLock(global);
	memcpy(data, text, length * sizeof(WCHAR));
	data[length] = 0;
	GlobalUnlock(global);
	return global;
}

typedef struct
{
	char *data;
	INT used, capacity;
} grow_buffer;

static DWORD CALLBACK buffer_writer(DWORD_PTR cookie, BYTE *data, LONG size, LONG *written)
{
	grow_buffer *buffer = (grow_buffer *)cookie;
	if (buffer->used + size > buffer->capacity) {
		INT capacity = (buffer->used + size) * 2;
		char *grown = re_realloc(buffer->data, capacity);
		if (!grown)
			return 1;
		buffer->data = grown;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->used, data, size);
	buffer->used += size;
	*written = size;
	return 0;
}

void re_copy_selected(re_editor *editor)
{
	INT start, end;
	selection_range(editor, &start, &end);
	if (!OpenClipboard(editor->hwnd))
		return;
	EmptyClipboard();
	end = end > re_text_length(editor) ? re_text_length(editor) : end;
	start = start > end ? end : start;
	INT size = re_count_crlf(editor, start, end);
	WCHAR *plain = re_alloc((size + 1) * sizeof(WCHAR));
	if (plain) {
		INT got = re_get_text_crlf(editor, start, end, plain, size, TRUE);
		objects_to_spaces(plain, got);
		HGLOBAL global = text_to_global(plain, got);
		if (global)
			SetClipboardData(CF_UNICODETEXT, global);
		INT narrow_length = 0;
		char *narrow = re_wide_to_ansi(plain, got, CP_ACP, &narrow_length);
		HGLOBAL narrow_global = narrow ? GlobalAlloc(GMEM_MOVEABLE, narrow_length + 1) : NULL;
		if (narrow_global) {
			char *data = GlobalLock(narrow_global);
			memcpy(data, narrow, narrow_length);
			data[narrow_length] = 0;
			GlobalUnlock(narrow_global);
			SetClipboardData(CF_TEXT, narrow_global);
		}
		re_free(narrow);
		re_free(plain);
	}
	if (editor->text_mode & TM_PLAINTEXT) {
		CloseClipboard();
		return;
	}
	grow_buffer buffer = {NULL, 0, 0};
	EDITSTREAM stream = {(DWORD_PTR)&buffer, 0, buffer_writer};
	re_stream_out(editor, SF_RTF | SFF_SELECTION, &stream);
	if (buffer.used) {
		HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, buffer.used + 1);
		if (global) {
			char *data = GlobalLock(global);
			memcpy(data, buffer.data, buffer.used);
			data[buffer.used] = 0;
			GlobalUnlock(global);
			SetClipboardData(RegisterClipboardFormatW(CF_RTFW), global);
		}
	}
	re_free(buffer.data);
	CloseClipboard();
}

static void copy_selection(re_editor *editor)
{
	INT start, end;
	selection_range(editor, &start, &end);
	if (start < end && !editor->password)
		re_copy_selected(editor);
}

static BOOL paste_clipboard(re_editor *editor)
{
	if (editor->read_only || !OpenClipboard(editor->hwnd))
		return FALSE;
	BOOL done = FALSE;
	UINT rtf = RegisterClipboardFormatW(CF_RTFW);
	HANDLE data;
	if (!(editor->text_mode & TM_PLAINTEXT) && (data = GetClipboardData(rtf))) {
		const char *bytes = GlobalLock(data);
		if (bytes) {
			re_undo_begin_step(editor);
			stream_rtf(editor, bytes, lstrlenA(bytes), SFF_SELECTION);
			GlobalUnlock(data);
			done = TRUE;
		}
	}
	if (!done && (data = GetClipboardData(CF_UNICODETEXT))) {
		const WCHAR *text = GlobalLock(data);
		if (text) {
			replace_selection(editor, text, lstrlenW(text), TRUE);
			GlobalUnlock(data);
			done = TRUE;
		}
	}
	CloseClipboard();
	if (done) {
		editor->modified = TRUE;
		after_edit(editor);
	}
	return done;
}

static BOOL same_text(const WCHAR *left, const WCHAR *right, INT length, BOOL match_case)
{
	for (INT i = 0; i < length; i++) {
		WCHAR a = left[i], b = right[i];
		if (a == b)
			continue;
		if (match_case || (WCHAR)(ULONG_PTR)CharLowerW((WCHAR *)(ULONG_PTR)a) != (WCHAR)(ULONG_PTR)CharLowerW((WCHAR *)(ULONG_PTR)b))
			return FALSE;
	}
	return TRUE;
}

static LRESULT find_text(re_editor *editor, DWORD flags, const CHARRANGE *range, const WCHAR *what, CHARRANGE *found)
{
	INT start = range->cpMin, end = range->cpMax;
	INT text = re_text_length(editor);
	INT length = lstrlenW(what);
	if (found) {
		found->cpMin = -1;
		found->cpMax = -1;
	}
	BOOL down = (flags & FR_DOWN) != 0;
	if (end < 0 || end > text)
		end = down ? text : (end < 0 ? 0 : text);
	if (start < 0 || start > text)
		start = start < 0 ? text : text;
	if (!length)
		return -1;
	if (down && start > end)
		return -1;
	const WCHAR *body = editor->doc.text;
	if (!down && end > start)
		end = 0;
	INT last = down ? end - length : start - length;
	INT first = down ? start : end;
	INT step = down ? 1 : -1;
	for (INT at = down ? first : last; down ? at <= last : at >= first; at += step) {
		if (!same_text(body + at, what, length, (flags & FR_MATCHCASE) != 0))
			continue;
		if (flags & FR_WHOLEWORD) {
			if (at > 0 && word_char(body[at - 1]))
				continue;
			if (at + length < text && word_char(body[at + length]))
				continue;
		}
		if (found) {
			found->cpMin = at;
			found->cpMax = at + length;
		}
		return at;
	}
	return -1;
}

static LRESULT find_word_break(re_editor *editor, UINT code, INT at)
{
	if (at < 0)
		return 0;
	INT length = re_text_length(editor);
	if (at > length)
		at = length;
	const WCHAR *text = editor->doc.text;
	switch (code) {
	case WB_ISDELIMITER:
		return at < length ? !class_of(text[at]) && text[at] != RE_CR : TRUE;
	case WB_CLASSIFY:
		return at < length ? (class_of(text[at]) == 0 ? WBF_ISWHITE | WBF_BREAKLINE : class_of(text[at]) == 2 ? 1 | WBF_BREAKAFTER : 0) : 0;
	case WB_LEFT:
	case WB_LEFTBREAK:
		return previous_word(editor, at);
	case WB_RIGHT:
	case WB_RIGHTBREAK:
		return next_word(editor, at);
	case WB_MOVEWORDLEFT:
		return previous_word(editor, at);
	case WB_MOVEWORDRIGHT:
		return next_word(editor, at);
	default:
		return 0;
	}
}

static void on_mouse_button(re_editor *editor, UINT message, WPARAM wparam, LPARAM lparam)
{
	INT x = (short)LOWORD(lparam), y = (short)HIWORD(lparam);
	BOOL start_of_line;
	INT at = re_char_from_point(editor, x, y, &start_of_line);
	if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
		if (editor->hwnd) {
			SetFocus(editor->hwnd);
			SetCapture(editor->hwnd);
		}
		editor->dragging = TRUE;
		if (message == WM_LBUTTONDBLCLK) {
			INT from = at;
			const WCHAR *text = editor->doc.text;
			INT type = at < re_text_length(editor) ? class_of(text[at]) : 0;
			while (from > 0 && text[from - 1] != RE_CR && class_of(text[from - 1]) == type)
				from--;
			INT to = at;
			while (to < re_text_length(editor) && text[to] != RE_CR && class_of(text[to]) == type)
				to++;
			re_set_selection(editor, from, to);
			return;
		}
		editor->caret_at_line_start = start_of_line;
		re_set_selection(editor, (wparam & MK_SHIFT) ? editor->sel_start : at, at);
	} else if (message == WM_MOUSEMOVE) {
		if (editor->dragging && (wparam & MK_LBUTTON)) {
			editor->caret_at_line_start = start_of_line;
			re_set_selection(editor, editor->sel_start, at);
			re_scroll_to_caret(editor);
		}
	} else if (message == WM_LBUTTONUP) {
		if (editor->dragging && editor->hwnd)
			ReleaseCapture();
		editor->dragging = FALSE;
	}
}

// A mouse message over linked text goes to the owner as EN_LINK.
static BOOL link_notify(re_editor *editor, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (!(editor->event_mask & ENM_LINK))
		return FALSE;
	INT x = (short)LOWORD(lparam), y = (short)HIWORD(lparam);
	if (message == WM_SETCURSOR) {
		POINT point;
		GetCursorPos(&point);
		if (editor->hwnd)
			ScreenToClient(editor->hwnd, &point);
		x = point.x;
		y = point.y;
	}
	RECT view;
	re_view_rect(editor, &view);
	if (x < view.left)
		return FALSE;
	INT at = re_char_from_point(editor, x, y, NULL);
	if (at >= re_text_length(editor))
		return FALSE;
	const re_doc *doc = &editor->doc;
	INT run = re_doc_run_at(doc, at);
	if (!(doc->runs[run].format.dwEffects & CFE_LINK))
		return FALSE;
	POINT position;
	re_position(editor, at, FALSE, &position, NULL);
	ENLINK link;
	memset(&link, 0, sizeof(link));
	link.msg = message;
	link.wParam = message == WM_SETCURSOR ? 0 : wparam;
	link.lParam = MAKELPARAM(x, y);
	link.chrg.cpMin = doc->runs[run].start;
	link.chrg.cpMax = run + 1 < doc->run_count ? doc->runs[run + 1].start : doc->length;
	re_send_notify(editor, EN_LINK, &link.nmhdr);
	return TRUE;
}

// The steps widen with the size: every point up to 12, then even sizes to 28, a few fixed stops to 80 and tens above that. A sum between two stops goes
// the way the change was going.
static INT stepped_font_size(INT size, INT points)
{
	static const SHORT stops[] = {28, 36, 48, 72, 80};
	INT wanted = size + points;
	if (wanted < 1)
		return 1;
	if (wanted > 12 && wanted < 28 && wanted % 2)
		wanted += points > 0 ? 1 : -1;
	for (size_t i = 0; i + 1 < ARRAYSIZE(stops); i++)
		if (wanted > stops[i] && wanted < stops[i + 1])
			wanted = points > 0 ? stops[i + 1] : stops[i];
	if (wanted > 80 && wanted % 10)
		wanted = wanted / 10 * 10 + (points > 0 ? 10 : 0);
	return wanted > 1638 ? 1638 : wanted;
}

static LRESULT scroll_lines(re_editor *editor, INT lines)
{
	const re_layout *layout = &editor->layout;
	if (!layout->count)
		return 0;
	INT top = 0;
	while (top + 1 < layout->count && layout->lines[top + 1].y <= editor->scroll_y)
		top++;
	INT target = top + lines;
	if (target < 0)
		target = 0;
	if (target >= layout->count)
		target = layout->count - 1;
	INT before = editor->scroll_y;
	re_scroll_to(editor, editor->scroll_x, layout->lines[target].y);
	return editor->scroll_y != before;
}

static void on_scroll_bar(re_editor *editor, UINT message, WPARAM wparam)
{
	const re_layout *layout = &editor->layout;
	RECT view;
	re_view_rect(editor, &view);
	INT line = layout->count ? layout->lines[0].height : 1;
	BOOL vertical = message == WM_VSCROLL;
	INT page = vertical ? view.bottom - view.top : view.right - view.left;
	INT position = vertical ? editor->scroll_y : editor->scroll_x;
	switch (LOWORD(wparam)) {
	case SB_LINEUP: position -= line; break;
	case SB_LINEDOWN: position += line; break;
	case SB_PAGEUP: position -= page; break;
	case SB_PAGEDOWN: position += page; break;
	case SB_TOP: position = 0; break;
	case SB_BOTTOM: position = 0x3fffffff; break;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: {
		SCROLLINFO info = {sizeof(info), SIF_TRACKPOS};
		if (editor->hwnd && GetScrollInfo(editor->hwnd, vertical ? SB_VERT : SB_HORZ, &info))
			position = info.nTrackPos;
		break;
	}
	default:
		return;
	}
	if (vertical)
		re_scroll_to(editor, editor->scroll_x, position);
	else
		re_scroll_to(editor, position, editor->scroll_y);
}

static WORD style_alignment(DWORD style)
{
	return style & ES_CENTER ? PFA_CENTER : style & ES_RIGHT ? PFA_RIGHT : PFA_LEFT;
}

static void set_window_style_state(re_editor *editor, DWORD style, DWORD ex_style)
{
	editor->style = style;
	editor->ex_style = ex_style;
	editor->read_only = (style & ES_READONLY) != 0;
	editor->hide_selection = !(style & ES_NOHIDESEL);
	editor->multiline = (style & ES_MULTILINE) != 0;
	editor->wrap = (style & ES_MULTILINE) && !(style & (ES_AUTOHSCROLL | WS_HSCROLL));
	editor->doc.paras[0].format.wAlignment = style_alignment(style);
	editor->scroll_bars = style & (WS_VSCROLL | WS_HSCROLL | ES_DISABLENOSCROLL);
	editor->scroll_v_shown = (style & WS_VSCROLL) != 0;
	editor->scroll_h_shown = (style & WS_HSCROLL) != 0;
	if (style & ES_PASSWORD)
		editor->password = '*';
	editor->options = 0;
	if (style & (ES_AUTOHSCROLL | WS_HSCROLL))
		editor->options |= ECO_AUTOHSCROLL;
	if (style & (ES_AUTOVSCROLL | WS_VSCROLL))
		editor->options |= ECO_AUTOVSCROLL;
	if (style & ES_NOHIDESEL)
		editor->options |= ECO_NOHIDESEL;
	if (style & ES_READONLY)
		editor->options |= ECO_READONLY;
	if (style & ES_SAVESEL)
		editor->options |= ECO_SAVESEL;
	if (style & ES_SELECTIONBAR)
		editor->options |= ECO_SELECTIONBAR;
	if (style & ES_VERTICAL)
		editor->options |= ECO_VERTICAL;
	if (style & ES_WANTRETURN)
		editor->options |= ECO_WANTRETURN;
}

static LRESULT apply_options(re_editor *editor, UINT operation, DWORD value)
{
	const DWORD known = ECO_AUTOWORDSELECTION | ECO_AUTOVSCROLL | ECO_AUTOHSCROLL | ECO_NOHIDESEL | ECO_READONLY | ECO_WANTRETURN | ECO_SAVESEL | ECO_SELECTIONBAR | ECO_VERTICAL;
	value &= known;
	DWORD before = editor->options;
	switch (operation) {
	case ECOOP_SET: editor->options = value; break;
	case ECOOP_OR: editor->options |= value; break;
	case ECOOP_AND: editor->options &= value; break;
	case ECOOP_XOR: editor->options ^= value; break;
	default: return editor->options;
	}
	editor->read_only = (editor->options & ECO_READONLY) != 0;
	editor->hide_selection = !(editor->options & ECO_NOHIDESEL);
	// The selection bar comes out of the left of the format rectangle.
	if (editor->has_format_rect && (editor->options ^ before) & ECO_SELECTIONBAR)
		editor->format_rect.left += editor->options & ECO_SELECTIONBAR ? 8 : -8;
	DWORD mirrored = 0;
	if (editor->options & ECO_AUTOHSCROLL) mirrored |= ES_AUTOHSCROLL;
	if (editor->options & ECO_AUTOVSCROLL) mirrored |= ES_AUTOVSCROLL;
	if (editor->options & ECO_NOHIDESEL) mirrored |= ES_NOHIDESEL;
	if (editor->options & ECO_READONLY) mirrored |= ES_READONLY;
	if (editor->options & ECO_WANTRETURN) mirrored |= ES_WANTRETURN;
	if (editor->options & ECO_SAVESEL) mirrored |= ES_SAVESEL;
	if (editor->options & ECO_SELECTIONBAR) mirrored |= ES_SELECTIONBAR;
	if (editor->options & ECO_VERTICAL) mirrored |= ES_VERTICAL;
	if (editor->hwnd) {
		const DWORD mask = ES_AUTOHSCROLL | ES_AUTOVSCROLL | ES_NOHIDESEL | ES_READONLY | ES_WANTRETURN | ES_SAVESEL | ES_SELECTIONBAR | ES_VERTICAL;
		DWORD style = GetWindowLongW(editor->hwnd, GWL_STYLE);
		SetWindowLongW(editor->hwnd, GWL_STYLE, (style & ~mask) | mirrored);
		editor->style = (editor->style & ~mask) | mirrored;
	}
	re_invalidate(editor);
	return editor->options;
}

static LRESULT selection_type(re_editor *editor)
{
	INT start, end;
	selection_range(editor, &start, &end);
	if (start == end)
		return SEL_EMPTY;
	INT objects = 0;
	for (INT i = start; i < end && i < editor->doc.length; i++)
		if (editor->doc.text[i] == RE_OBJECT_MARK)
			objects++;
	INT characters = end - start - objects;
	LRESULT type = objects > 1 ? SEL_OBJECT | SEL_MULTIOBJECT : objects ? SEL_OBJECT : 0;
	if (characters)
		type |= SEL_TEXT;
	if (characters > 1)
		type |= SEL_MULTICHAR;
	return type;
}

LRESULT re_message(re_editor *editor, UINT message, WPARAM wparam, LPARAM lparam, BOOL *handled)
{
	if (handled)
		*handled = TRUE;
	re_doc *doc = &editor->doc;
	switch (message) {
	case WM_SETTEXT:
		return on_set_text(editor, (const void *)lparam);
	case WM_GETTEXT:
		return on_get_text(editor, 0, -1, (void *)lparam, (INT)wparam, TRUE);
	case WM_GETTEXTLENGTH: {
		GETTEXTLENGTHEX ex = {GTL_PRECISE | GTL_USECRLF | (editor->ansi ? GTL_NUMBYTES : GTL_NUMCHARS), editor->ansi ? CP_ACP : 1200};
		if (!editor->ansi)
			ex.flags = GTL_PRECISE | GTL_USECRLF;
		return text_length_ex(editor, &ex);
	}
	case WM_SETFONT: {
		HFONT font = (HFONT)wparam;
		CHARFORMAT2W look;
		re_default_char_format(&look, font ? font : GetStockObject(SYSTEM_FONT));
		look.dwEffects = (look.dwEffects & ~(CFE_AUTOBACKCOLOR)) | CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR;
		look.dwMask = CFM_SIZE | CFM_FACE | CFM_CHARSET | CFM_WEIGHT | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
		re_doc_set_format(doc, 0, doc->length, &look);
		re_format_merge(&editor->default_format, &look);
		editor->set_font = font;
		editor->has_insert_format = FALSE;
		re_layout_update(editor);
		re_update_scrollbars(editor);
		if (LOWORD(lparam))
			re_invalidate(editor);
		re_update_caret(editor);
		return 0;
	}
	case WM_GETFONT:
		return (LRESULT)editor->set_font;
	case WM_COPY:
		copy_selection(editor);
		return 0;
	case WM_CUT:
		copy_selection(editor);
		if (!editor->read_only) {
			INT from, to, all = editor->doc.length;
			selection_range(editor, &from, &to);
			re_undo_begin_step(editor);
			re_delete_selection(editor);
			re_undo_begin_step(editor);
			// Nothing left: the paragraph is a new one.
			if (!from && to == all) {
				re_default_para_format(&editor->doc.paras[0].format);
				editor->doc.paras[0].format.wAlignment = style_alignment(editor->style);
			}
			after_edit(editor);
		}
		return 0;
	case WM_PASTE:
		paste_clipboard(editor);
		return 0;
	case EM_PASTESPECIAL:
		paste_clipboard(editor);
		return 0;
	case EM_CANPASTE: {
		if (editor->read_only)
			return 0;
		UINT rtf = RegisterClipboardFormatW(CF_RTFW);
		if (wparam)
			return wparam == CF_TEXT || wparam == CF_UNICODETEXT || wparam == CF_OEMTEXT || wparam == rtf ? IsClipboardFormatAvailable((UINT)wparam) : 0;
		return IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT) || IsClipboardFormatAvailable(rtf);
	}
	case WM_CLEAR:
		if (!editor->read_only) {
			re_undo_begin_step(editor);
			re_delete_selection(editor);
			re_undo_begin_step(editor);
			after_edit(editor);
		}
		return 0;
	case WM_GETDLGCODE: {
		LRESULT code = DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
		MSG *msg = (MSG *)lparam;
		if (editor->style & ES_MULTILINE)
			code |= DLGC_WANTMESSAGE;
		if (!(editor->style & ES_SAVESEL))
			code |= DLGC_HASSETSEL;
		if (msg && msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN && (editor->style & ES_WANTRETURN))
			code |= DLGC_WANTMESSAGE;
		if (msg && msg->message == WM_KEYDOWN && msg->wParam == VK_TAB && editor->style & ES_MULTILINE && GetKeyState(VK_CONTROL) < 0)
			code |= DLGC_WANTMESSAGE;
		if (msg && msg->message == WM_KEYDOWN && !(editor->style & ES_MULTILINE) && (msg->wParam == VK_RETURN || msg->wParam == VK_TAB))
			code &= ~DLGC_WANTMESSAGE;
		// A message to look at means the control sits in a dialog.
		if (msg)
			editor->dialog_mode = TRUE;
		return code;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	case WM_PRINTCLIENT: {
		PAINTSTRUCT paint;
		HDC dc = message == WM_PAINT && !wparam ? BeginPaint(editor->hwnd, &paint) : (HDC)wparam;
		if (dc) {
			if (!editor->layout.count)
				re_layout_update(editor);
			RECT all;
			if (message == WM_PAINT && !wparam)
				all = paint.rcPaint;
			else
				GetClientRect(editor->hwnd, &all);
			re_paint(editor, dc, &all);
		}
		if (message == WM_PAINT && !wparam)
			EndPaint(editor->hwnd, &paint);
		return 0;
	}
	case WM_SIZE:
		re_layout_update(editor);
		re_update_scrollbars(editor);
		re_scroll_to(editor, editor->scroll_x, editor->scroll_y);
		re_invalidate(editor);
		return 0;
	case WM_SETREDRAW:
		// Redrawing is off while the window counts as not visible.
		if (editor->hwnd) {
			LONG shown = GetWindowLongW(editor->hwnd, GWL_STYLE);
			SetWindowLongW(editor->hwnd, GWL_STYLE, wparam ? shown | WS_VISIBLE : shown & ~WS_VISIBLE);
			if (wparam)
				re_invalidate(editor);
		}
		return 0;
	case WM_SETFOCUS:
		editor->focused = TRUE;
		re_update_caret(editor);
		re_invalidate(editor);
		if (editor->hwnd && editor->parent)
			SendMessageW(editor->parent, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(editor->hwnd, GWLP_ID), EN_SETFOCUS), (LPARAM)editor->hwnd);
		return 0;
	case WM_KILLFOCUS:
		editor->undo_open = FALSE;
		editor->focused = FALSE;
		if (editor->has_caret) {
			DestroyCaret();
			editor->has_caret = FALSE;
		}
		re_invalidate(editor);
		if (editor->hwnd && editor->parent)
			SendMessageW(editor->parent, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(editor->hwnd, GWLP_ID), EN_KILLFOCUS), (LPARAM)editor->hwnd);
		return 0;
	case WM_ENABLE:
		re_invalidate(editor);
		return 0;
	case WM_STYLECHANGED:
		if (wparam == GWL_STYLE && editor->hwnd)
			editor->style = ((STYLESTRUCT *)lparam)->styleNew;
		return 0;
	case WM_CHAR:
	case WM_UNICHAR: {
		if (message == WM_UNICHAR && wparam == UNICODE_NOCHAR)
			return TRUE;
		WCHAR c = (WCHAR)wparam;
		if (editor->ansi && message == WM_CHAR && wparam < 256) {
			char narrow = (char)wparam;
			MultiByteToWideChar(CP_ACP, 0, &narrow, 1, &c, 1);
		}
		if (editor->event_mask & ENM_KEYEVENTS) {
			MSGFILTER filter;
			memset(&filter, 0, sizeof(filter));
			filter.msg = message;
			filter.wParam = wparam;
			filter.lParam = lparam;
			if (re_send_notify(editor, EN_MSGFILTER, &filter.nmhdr))
				return 0;
		}
		// The key's own press already made the line.
		if (c == RE_CR && editor->enter_typed) {
			editor->enter_typed = FALSE;
			return 0;
		}
		on_char(editor, c);
		return message == WM_UNICHAR;
	}
	case WM_KEYDOWN:
		if (editor->event_mask & ENM_KEYEVENTS) {
			MSGFILTER filter;
			memset(&filter, 0, sizeof(filter));
			filter.msg = message;
			filter.wParam = wparam;
			filter.lParam = lparam;
			if (re_send_notify(editor, EN_MSGFILTER, &filter.nmhdr))
				return 0;
		}
		if (wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP || wparam == VK_DOWN || wparam == VK_HOME || wparam == VK_END || wparam == VK_PRIOR || wparam == VK_NEXT)
			editor->undo_open = FALSE;
		if (wparam == VK_RETURN && (editor->style & ES_MULTILINE) && !editor->host && !(editor->dialog_mode && !(editor->style & ES_WANTRETURN))) {
			on_char(editor, RE_CR);
			editor->enter_typed = TRUE;
			return 0;
		}
		if (editor->dialog_mode && editor->parent) {
			if (wparam == VK_TAB && GetKeyState(VK_CONTROL) >= 0) {
				SendMessageW(editor->parent, WM_NEXTDLGCTL, GetKeyState(VK_SHIFT) < 0, 0);
				return 0;
			}
			if (wparam == VK_ESCAPE) {
				PostMessageW(editor->parent, WM_CLOSE, 0, 0);
				return 0;
			}
		}
		if (wparam == VK_RETURN && (!(editor->style & ES_MULTILINE) || editor->dialog_mode) && editor->hwnd) {
			// In a dialog the key belongs to the default button, unless the control asked for it.
			HWND parent = editor->parent;
			if (parent && editor->dialog_mode && !(editor->style & ES_WANTRETURN)) {
				LRESULT id = SendMessageW(parent, DM_GETDEFID, 0, 0);
				HWND button = HIWORD(id) == DC_HASDEFID ? GetDlgItem(parent, LOWORD(id)) : NULL;
				if (button) {
					SendMessageW(parent, WM_NEXTDLGCTL, (WPARAM)button, TRUE);
					PostMessageW(button, WM_KEYDOWN, VK_RETURN, lparam);
				}
			}
			return 0;
		}
		on_key(editor, (UINT)wparam);
		if (handled)
			*handled = TRUE;
		return 0;
	case WM_RBUTTONDBLCLK:
		link_notify(editor, message, wparam, lparam);
		if (handled)
			*handled = FALSE;
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_LBUTTONUP:
	case WM_MOUSEMOVE:
		if (message != WM_MOUSEMOVE || !editor->dragging)
			link_notify(editor, message, wparam, lparam);
		if (editor->event_mask & ENM_MOUSEEVENTS) {
			MSGFILTER filter;
			memset(&filter, 0, sizeof(filter));
			filter.msg = message;
			filter.wParam = wparam;
			filter.lParam = lparam;
			if (re_send_notify(editor, EN_MSGFILTER, &filter.nmhdr))
				return 0;
		}
		on_mouse_button(editor, message, wparam, lparam);
		return 0;
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
		link_notify(editor, message, wparam, lparam);
		if (editor->event_mask & ENM_MOUSEEVENTS) {
			MSGFILTER filter;
			memset(&filter, 0, sizeof(filter));
			filter.msg = message;
			filter.wParam = wparam;
			filter.lParam = lparam;
			if (re_send_notify(editor, EN_MSGFILTER, &filter.nmhdr))
				return 0;
		}
		if (handled)
			*handled = FALSE;
		return 0;
	case WM_SETCURSOR: {
		BOOL linked = (HWND)wparam == editor->hwnd && link_notify(editor, WM_SETCURSOR, wparam, lparam);
		if (LOWORD(lparam) != HTCLIENT) {
			if (handled)
				*handled = FALSE;
			return 0;
		}
		SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(linked ? 32649 : 32513)));
		return TRUE;
	}
	case WM_MOUSEWHEEL: {
		INT delta = (short)HIWORD(wparam);
		if (LOWORD(wparam) & MK_CONTROL) {
			INT num = editor->zoom_num ? editor->zoom_num : 100, den = editor->zoom_den ? editor->zoom_den : 100;
			INT percent = num * 100 / den + (delta > 0 ? 10 : -10);
			if (percent >= 10 && percent <= 500) {
				editor->zoom_num = percent;
				editor->zoom_den = 100;
				re_refresh(editor);
			}
			return 0;
		}
		scroll_lines(editor, -delta * 3 / WHEEL_DELTA);
		return 0;
	}
	case WM_VSCROLL:
	case WM_HSCROLL:
		on_scroll_bar(editor, message, wparam);
		// Dragging the thumb is the user's own doing and goes unreported.
		if (LOWORD(wparam) != SB_THUMBTRACK && LOWORD(wparam) != SB_THUMBPOSITION && editor->event_mask & ENM_SCROLL) {
			WORD code = message == WM_VSCROLL ? EN_VSCROLL : EN_HSCROLL;
			if (editor->host)
				ITextHost_TxNotify(editor->host, code, NULL);
			else if (editor->parent)
				SendMessageW(editor->parent, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(editor->hwnd, GWLP_ID), code), (LPARAM)editor->hwnd);
		}
		return 0;
	case WM_IME_STARTCOMPOSITION:
	case WM_IME_COMPOSITION:
	case WM_IME_ENDCOMPOSITION:
		if (handled)
			*handled = FALSE;
		return 0;
	case EM_GETSEL: {
		INT start, end;
		selection_range(editor, &start, &end);
		if (wparam)
			*(DWORD *)wparam = start;
		if (lparam)
			*(DWORD *)lparam = end;
		return MAKELONG(start > 0xffff ? 0xffff : start, end > 0xffff ? 0xffff : end);
	}
	case EM_EXGETSEL: {
		CHARRANGE *range = (CHARRANGE *)lparam;
		if (range)
			selection_range(editor, (INT *)&range->cpMin, (INT *)&range->cpMax);
		return 0;
	}
	case EM_SETSEL:
		re_set_selection_user(editor, (INT)wparam, (INT)lparam);
		if (!editor->hide_caret)
			re_scroll_to_caret(editor);
		return editor->sel_end;
	case EM_EXSETSEL: {
		const CHARRANGE *range = (const CHARRANGE *)lparam;
		if (!range)
			return 0;
		re_set_selection_user(editor, range->cpMin, range->cpMax);
		re_scroll_to_caret(editor);
		if (editor->host)
			re_invalidate(editor);
		return editor->sel_end;
	}
	case EM_HIDESELECTION:
		editor->hide_selection = (BOOL)wparam;
		re_invalidate(editor);
		return 0;
	case EM_SELECTIONTYPE:
		return selection_type(editor);
	case EM_GETSELTEXT: {
		INT start, end;
		selection_range(editor, &start, &end);
		if (end > re_text_length(editor))
			end = re_text_length(editor);
		if (start > end)
			start = end;
		INT shown = end - start;
		WCHAR *buffer = re_alloc((shown + 1) * sizeof(WCHAR));
		if (!buffer)
			return 0;
		memcpy(buffer, doc->text + start, shown * sizeof(WCHAR));
		objects_to_spaces(buffer, shown);
		LRESULT result = copy_out(editor, buffer, shown, (void *)lparam, 0x7fffffff / (editor->ansi ? 1 : 2));
		re_free(buffer);
		return result;
	}
	case EM_GETTEXTRANGE: {
		TEXTRANGEW *range = (TEXTRANGEW *)lparam;
		if (!range)
			return 0;
		INT start = range->chrg.cpMin, end = range->chrg.cpMax;
		if (start < 0)
			return 0;
		if (end < 0 && start)
			return 0;
		if (end < 0 || end > re_text_length(editor))
			end = re_text_length(editor);
		if (start >= end)
			return 0;
		INT shown = end - start;
		WCHAR *buffer = re_alloc((shown + 1) * sizeof(WCHAR));
		if (!buffer)
			return 0;
		memcpy(buffer, doc->text + start, shown * sizeof(WCHAR));
		objects_to_spaces(buffer, shown);
		LRESULT result = copy_out(editor, buffer, shown, range->lpstrText, 0x3fffffff);
		re_free(buffer);
		return result;
	}
	case EM_GETTEXTEX:
		return on_get_text_ex(editor, (const GETTEXTEX *)wparam, (void *)lparam);
	case EM_SETTEXTEX:
		return on_set_text_ex(editor, (const SETTEXTEX *)wparam, (const void *)lparam);
	case EM_GETTEXTLENGTHEX:
		return text_length_ex(editor, (const GETTEXTLENGTHEX *)wparam);
	case EM_REPLACESEL: {
		const WCHAR *text = lparam ? (const WCHAR *)lparam : L"";
		INT from, to;
		selection_range(editor, &from, &to);
		if (wide_is_rtf(text) && editor->ansi) {
			INT size;
			char *narrow = re_wide_to_ansi(text, -1, CP_ACP, &size);
			if (narrow) {
				stream_rtf(editor, narrow, size, SFF_SELECTION);
				re_free(narrow);
			}
		} else {
			replace_selection(editor, text, lstrlenW(text), (BOOL)wparam);
		}
		after_edit(editor);
		// What was handed in counts, before its line endings are made one.
		return wide_is_rtf(text) && editor->ansi ? editor->sel_end - from : lstrlenW(text);
	}
	case EM_GETLINECOUNT:
		return editor->layout.count ? editor->layout.count : 1;
	case EM_LINEFROMCHAR:
	case EM_EXLINEFROMCHAR: {
		INT at = (INT)(message == EM_EXLINEFROMCHAR ? lparam : wparam);
		if (at < 0) {
			INT start, end;
			selection_range(editor, &start, &end);
			at = start;
		}
		if (at > doc->length - 1)
			at = doc->length - 1;
		return re_line_at(&editor->layout, at, TRUE);
	}
	case EM_LINEINDEX: {
		INT line = (INT)wparam;
		if (line < 0)
			line = re_line_at(&editor->layout, editor->sel_end, editor->caret_at_line_start);
		if (line >= editor->layout.count)
			return -1;
		return editor->layout.lines[line].start;
	}
	case EM_LINELENGTH: {
		INT at = (INT)wparam;
		if (at < 0) {
			INT start, end;
			selection_range(editor, &start, &end);
			INT first = re_line_at(&editor->layout, start, TRUE), last = re_line_at(&editor->layout, end, TRUE);
			INT count = 0;
			if (first == last) {
				const re_line *line = &editor->layout.lines[first];
				count = line->length - (end - start);
				if (doc->text[line->start + line->length - 1] == RE_CR)
					count--;
			} else {
				count = start - editor->layout.lines[first].start;
				count += editor->layout.lines[last].start + editor->layout.lines[last].length - end;
				if (doc->text[editor->layout.lines[last].start + editor->layout.lines[last].length - 1] == RE_CR)
					count--;
			}
			return count > 0 ? count : 0;
		}
		if (at > doc->length - 1)
			return 0;
		const re_line *line = &editor->layout.lines[re_line_at(&editor->layout, at, TRUE)];
		INT count = line->length;
		if (doc->text[line->start + count - 1] == RE_CR)
			count--;
		return count;
	}
	case EM_GETLINE: {
		INT index = (INT)wparam;
		if (index < 0 || index >= editor->layout.count)
			return 0;
		const re_line *line = &editor->layout.lines[index];
		WORD capacity = *(WORD *)lparam;
		INT size = line->length;
		INT wanted = size;
		BOOL mark = doc->text[line->start + size - 1] == RE_CR;
		WCHAR *buffer = re_alloc((size + 2) * sizeof(WCHAR));
		if (!buffer)
			return 0;
		memcpy(buffer, doc->text + line->start, size * sizeof(WCHAR));
		// The final mark is not text; other marks read as they are stored.
		if (mark && line->start + size == doc->length && size > 1)
			wanted--;
		else if (mark)
			buffer[wanted - 1] = RE_CR;
		INT take = wanted < capacity ? wanted : capacity;
		LRESULT result;
		if (editor->ansi) {
			result = WideCharToMultiByte(CP_ACP, 0, buffer, take, (char *)lparam, capacity, NULL, NULL);
			if (result < capacity)
				((char *)lparam)[result] = 0;
		} else {
			memcpy((void *)lparam, buffer, take * sizeof(WCHAR));
			if (take < capacity)
				((WCHAR *)lparam)[take] = 0;
			result = take;
		}
		re_free(buffer);
		return result;
	}
	case EM_GETFIRSTVISIBLELINE: {
		const re_layout *layout = &editor->layout;
		INT top = 0;
		while (top + 1 < layout->count && layout->lines[top + 1].y <= editor->scroll_y)
			top++;
		return top;
	}
	case EM_LINESCROLL:
		if (editor->style & ES_MULTILINE) {
			scroll_lines(editor, (INT)lparam);
			INT step = re_zoomed(editor, 8);
			re_scroll_to(editor, editor->scroll_x + (INT)wparam * step, editor->scroll_y);
		}
		return editor->style & ES_MULTILINE ? 1 : 0;
	case EM_SCROLL: {
		INT before = editor->scroll_y;
		INT lines = 0;
		const re_layout *layout = &editor->layout;
		INT top = 0;
		while (top + 1 < layout->count && layout->lines[top + 1].y <= before)
			top++;
		RECT view;
		re_view_rect(editor, &view);
		switch (wparam) {
		case SB_LINEUP: scroll_lines(editor, -1); break;
		case SB_LINEDOWN: scroll_lines(editor, 1); break;
		case SB_PAGEUP: re_scroll_to(editor, editor->scroll_x, before - (view.bottom - view.top)); break;
		case SB_PAGEDOWN: re_scroll_to(editor, editor->scroll_x, before + (view.bottom - view.top)); break;
		default: return FALSE;
		}
		INT now = 0;
		while (now + 1 < layout->count && layout->lines[now + 1].y <= editor->scroll_y)
			now++;
		lines = now - top;
		return MAKELRESULT((WORD)lines, 1);
	}
	case EM_SCROLLCARET:
		re_scroll_to_caret(editor);
		return 0;
	case EM_GETSCROLLPOS: {
		POINT *point = (POINT *)lparam;
		if (point) {
			point->x = editor->scroll_x;
			point->y = editor->scroll_y;
		}
		return 1;
	}
	case EM_SETSCROLLPOS: {
		const POINT *point = (const POINT *)lparam;
		if (point)
			re_scroll_to(editor, point->x, point->y);
		return 1;
	}
	case EM_POSFROMCHAR: {
		// Two forms: a POINTL to fill with the character in wParam, or the 16-bit result of a character in wParam.
		INT at = (INT)(lparam && (ULONG_PTR)wparam > 0xffff ? lparam : wparam);
		POINTL *target = NULL;
		if ((ULONG_PTR)wparam > 0xffff) {
			target = (POINTL *)wparam;
			at = (INT)lparam;
		}
		if (at < 0)
			at = 0;
		if (at > re_text_length(editor) && !target && !(editor->style & ES_MULTILINE))
			return -1;
		POINT position;
		re_position(editor, at > doc->length - 1 ? doc->length - 1 : at, TRUE, &position, NULL);
		if (target) {
			target->x = position.x;
			target->y = position.y;
			return 0;
		}
		return MAKELONG((WORD)position.x, (WORD)position.y);
	}
	case EM_CHARFROMPOS: {
		const POINTL *point = (const POINTL *)lparam;
		if (!point)
			return 0;
		return re_char_from_point(editor, point->x, point->y, NULL);
	}
	case EM_GETMODIFY:
		return editor->modified ? -1 : 0;
	case EM_SETMODIFY:
		editor->modified = (BOOL)wparam;
		return 0;
	case EM_CANUNDO:
		return editor->undo.count != 0;
	case EM_CANREDO:
		return editor->redo.count != 0;
	case EM_UNDO:
	case WM_UNDO: {
		BOOL done = re_undo(editor);
		if (done) {
			editor->modified = TRUE;
			after_edit(editor);
		}
		return done;
	}
	case EM_REDO: {
		BOOL done = re_redo(editor);
		if (done) {
			editor->modified = TRUE;
			after_edit(editor);
		}
		return done;
	}
	case EM_EMPTYUNDOBUFFER:
		re_undo_clear(&editor->undo);
		re_undo_clear(&editor->redo);
		return 0;
	case EM_SETUNDOLIMIT: {
		// Negative numbers ask for the default.
		INT limit = (INT)wparam < 0 ? 100 : (INT)wparam > 1000 ? 1000 : (INT)wparam;
		editor->undo_limit = limit;
		while (editor->undo.count > limit) {
			re_undo_list *list = &editor->undo;
			re_doc_free_piece(&list->items[0].removed);
			if (list->items[0].before) {
				re_doc_clear(list->items[0].before);
				re_free(list->items[0].before);
			}
			memmove(list->items, list->items + 1, (list->count - 1) * sizeof(re_undo_item));
			list->count--;
		}
		if (!limit)
			re_undo_clear(&editor->redo);
		return limit;
	}
	case EM_STOPGROUPTYPING:
		re_undo_begin_step(editor);
		return 0;
	case EM_GETUNDONAME:
		return editor->undo.count ? UID_TYPING : UID_UNKNOWN;
	case EM_GETREDONAME:
		return editor->redo.count ? UID_TYPING : UID_UNKNOWN;
	case EM_LIMITTEXT:
	case EM_EXLIMITTEXT: {
		INT limit = message == EM_LIMITTEXT ? (INT)wparam : (INT)lparam;
		if (!limit)
			limit = 65536;
		editor->limit = limit;
		return 0;
	}
	case EM_GETLIMITTEXT:
		return editor->limit;
	case EM_SETREADONLY:
		editor->read_only = (BOOL)wparam;
		apply_options(editor, wparam ? ECOOP_OR : ECOOP_AND, wparam ? ECO_READONLY : ~(DWORD)ECO_READONLY);
		return 1;
	case EM_SETOPTIONS:
		return apply_options(editor, (UINT)wparam, (DWORD)lparam);
	case EM_GETOPTIONS:
		return editor->options;
	case EM_SETEVENTMASK: {
		DWORD previous = editor->event_mask;
		editor->event_mask = (DWORD)lparam;
		return previous;
	}
	case EM_GETEVENTMASK:
		return editor->event_mask;
	case EM_SETTEXTMODE: {
		DWORD mode = (DWORD)wparam;
		if (re_text_length(editor) || editor->undo.count || editor->redo.count)
			return E_UNEXPECTED;
		if ((mode & (TM_PLAINTEXT | TM_RICHTEXT)) == (TM_PLAINTEXT | TM_RICHTEXT)
			|| (mode & (TM_SINGLELEVELUNDO | TM_MULTILEVELUNDO)) == (TM_SINGLELEVELUNDO | TM_MULTILEVELUNDO)
			|| (mode & (TM_SINGLECODEPAGE | TM_MULTICODEPAGE)) == (TM_SINGLECODEPAGE | TM_MULTICODEPAGE))
			return E_INVALIDARG;
		if (mode & (TM_PLAINTEXT | TM_RICHTEXT))
			editor->text_mode = (editor->text_mode & ~(TM_PLAINTEXT | TM_RICHTEXT)) | (mode & (TM_PLAINTEXT | TM_RICHTEXT));
		if (mode & (TM_SINGLELEVELUNDO | TM_MULTILEVELUNDO))
			editor->text_mode = (editor->text_mode & ~(TM_SINGLELEVELUNDO | TM_MULTILEVELUNDO)) | (mode & (TM_SINGLELEVELUNDO | TM_MULTILEVELUNDO));
		if (mode & (TM_SINGLECODEPAGE | TM_MULTICODEPAGE))
			editor->text_mode = (editor->text_mode & ~(TM_SINGLECODEPAGE | TM_MULTICODEPAGE)) | (mode & (TM_SINGLECODEPAGE | TM_MULTICODEPAGE));
		return 0;
	}
	case EM_GETTEXTMODE:
		return editor->text_mode;
	case EM_SETCHARFORMAT: {
		CHARFORMAT2W format;
		if (!re_format_from_user(&format, (const void *)lparam, editor->ansi))
			return 0;
		DWORD flags = (DWORD)wparam;
		// In plain text one look holds for all the text.
		if (editor->text_mode & TM_PLAINTEXT) {
			if (flags & (SCF_WORD | SCF_SELECTION))
				return 0;
			flags = SCF_ALL;
		}
		INT from, to;
		selection_range(editor, &from, &to);
		if (flags & SCF_ALL) {
			re_apply_format(editor, SCF_ALL, &format);
			re_format_merge(&editor->default_format, &format);
			editor->modified = TRUE;
		} else if (flags & SCF_WORD) {
			if (from == to) {
				while (from > 0 && word_char(doc->text[from - 1]))
					from--;
				while (to < re_text_length(editor) && word_char(doc->text[to]))
					to++;
				re_undo_format(editor);
				re_doc_set_format(doc, from, to, &format);
			} else {
				re_apply_format(editor, SCF_SELECTION, &format);
			}
			editor->modified = editor->modified || from != to;
		} else if (flags & SCF_SELECTION) {
			re_apply_format(editor, SCF_SELECTION, &format);
			editor->modified = editor->modified || from != to;
		} else {
			// SCF_DEFAULT, and bits nothing is defined for: the default look of the control.
			re_format_merge(&editor->default_format, &format);
			re_doc_set_format(doc, 0, doc->length, &format);
			if (doc->length == 1)
				editor->has_insert_format = FALSE;
		}
		re_refresh(editor);
		return 1;
	}
	case EM_GETCHARFORMAT: {
		CHARFORMAT2W format;
		DWORD differing = 0;
		if (wparam == SCF_DEFAULT) {
			format = editor->default_format;
			format.dwMask = CFM_ALL2;
		} else {
			re_get_selection_format(editor, &format, &differing);
		}
		if (!lparam)
			return 0;
		if (!re_format_to_user(&format, (void *)lparam, editor->ansi, format.dwMask))
			return 0;
		return ((CHARFORMAT *)lparam)->dwMask;
	}
	case EM_SETFONTSIZE: {
		if (editor->text_mode & TM_PLAINTEXT)
			return 0;
		INT points = (INT)wparam;
		CHARFORMAT2W now;
		DWORD differing;
		re_get_selection_format(editor, &now, &differing);
		now.dwMask = CFM_SIZE;
		now.yHeight = stepped_font_size(now.yHeight / 20, points) * 20;
		BOOL modified = editor->modified;
		re_apply_format(editor, SCF_SELECTION, &now);
		editor->modified = modified;
		re_refresh(editor);
		return 1;
	}
	case EM_SETPARAFORMAT: {
		PARAFORMAT2 format;
		const PARAFORMAT *user = (const PARAFORMAT *)lparam;
		if (!user || (user->cbSize != sizeof(PARAFORMAT) && user->cbSize != sizeof(PARAFORMAT2)))
			return 0;
		memset(&format, 0, sizeof(format));
		memcpy(&format, user, user->cbSize);
		format.cbSize = user->cbSize;
		INT start, end;
		selection_range(editor, &start, &end);
		if (end > doc->length)
			end = doc->length;
		re_undo_format(editor);
		re_doc_set_para(doc, start, end, &format);
		re_refresh(editor);
		return user->dwMask;
	}
	case EM_GETPARAFORMAT: {
		PARAFORMAT *user = (PARAFORMAT *)lparam;
		if (!user || (user->cbSize != sizeof(PARAFORMAT) && user->cbSize != sizeof(PARAFORMAT2)))
			return 0;
		INT start, end;
		selection_range(editor, &start, &end);
		const PARAFORMAT2 *para = &doc->paras[re_doc_para_at(doc, start)].format;
		UINT size = user->cbSize;
		DWORD mask = size == sizeof(PARAFORMAT) ? PFM_ALL : PFM_ALL2;
		memcpy(user, para, size);
		user->cbSize = size;
		user->dwMask = mask;
		// Paragraphs that disagree drop that part from the mask.
		INT last = re_doc_para_at(doc, end > start ? end - 1 : start);
		for (INT i = re_doc_para_at(doc, start) + 1; i <= last; i++) {
			const PARAFORMAT2 *other = &doc->paras[i].format;
			if (other->wAlignment != para->wAlignment)
				user->dwMask &= ~PFM_ALIGNMENT;
			if (other->dxStartIndent != para->dxStartIndent)
				user->dwMask &= ~PFM_STARTINDENT;
			if (other->dxRightIndent != para->dxRightIndent)
				user->dwMask &= ~PFM_RIGHTINDENT;
			if (other->dxOffset != para->dxOffset)
				user->dwMask &= ~PFM_OFFSET;
			if (other->wNumbering != para->wNumbering)
				user->dwMask &= ~PFM_NUMBERING;
			if (other->cTabCount != para->cTabCount)
				user->dwMask &= ~PFM_TABSTOPS;
		}
		return user->dwMask;
	}
	case EM_SETBKGNDCOLOR: {
		COLORREF previous = editor->back_system ? GetSysColor(COLOR_WINDOW) : editor->back_color;
		editor->back_system = (BOOL)wparam;
		editor->back_color = (COLORREF)lparam;
		re_invalidate(editor);
		return previous;
	}
	case EM_SETPASSWORDCHAR:
		editor->password = (WCHAR)wparam;
		re_refresh(editor);
		return 0;
	case EM_GETPASSWORDCHAR:
		return editor->password;
	case EM_SETRECT:
	case EM_SETRECTNP: {
		if (!lparam) {
			editor->has_format_rect = FALSE;
		} else {
			RECT rect = *(const RECT *)lparam;
			// Given as is with the flag set, otherwise it stays within the client area.
			if (!wparam) {
				RECT client;
				GetClientRect(editor->hwnd, &client);
				rect.top = rect.top > 0 ? rect.top : 0;
				rect.left = rect.left > 0 ? rect.left : 0;
				if (rect.bottom > client.bottom)
					rect.bottom = client.bottom;
				if (rect.right > client.right)
					rect.right = client.right;
			}
			if (editor->style & WS_BORDER || editor->ex_style & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)) {
				rect.top -= 1;
				rect.left -= 1;
				rect.right += 1;
			}
			editor->format_rect = rect;
			editor->has_format_rect = TRUE;
		}
		re_refresh(editor);
		return 0;
	}
	case EM_GETRECT:
		if (lparam)
			re_view_rect(editor, (RECT *)lparam);
		return 0;
	case EM_SETMARGINS:
		if (wparam & EC_LEFTMARGIN)
			editor->margin_left = LOWORD(lparam) == EC_USEFONTINFO ? 0 : LOWORD(lparam);
		if (wparam & EC_RIGHTMARGIN)
			editor->margin_right = HIWORD(lparam) == EC_USEFONTINFO ? 0 : HIWORD(lparam);
		re_refresh(editor);
		return 0;
	case EM_GETMARGINS:
		return MAKELONG(editor->margin_left, editor->margin_right);
	case EM_GETZOOM:
		if (wparam)
			*(INT *)wparam = editor->zoom_num;
		if (lparam)
			*(INT *)lparam = editor->zoom_den;
		return 1;
	case EM_SETZOOM: {
		INT num = (INT)wparam, den = (INT)lparam;
		if ((num || den) && (num <= 0 || den <= 0 || num >= 64 * den || 64 * num <= den))
			return 0;
		if (num < 0 || den < 0)
			return 0;
		editor->zoom_num = num;
		editor->zoom_den = den;
		re_refresh(editor);
		return 1;
	}
	case EM_FORMATRANGE:
		return re_format_range(editor, (FORMATRANGE *)lparam, (BOOL)wparam);
	case EM_DISPLAYBAND:
		return 0;
	case EM_SETTARGETDEVICE:
		editor->target_dc = (HDC)wparam;
		editor->target_width = (INT)lparam;
		editor->wrap = !(lparam == 1);
		re_refresh(editor);
		return 1;
	case EM_FINDTEXT:
	case EM_FINDTEXTW: {
		const FINDTEXTW *find = (const FINDTEXTW *)lparam;
		if (!find)
			return -1;
		WCHAR *narrow = NULL;
		const WCHAR *what = find->lpstrText;
		if (message == EM_FINDTEXT && editor->ansi)
			what = narrow = re_ansi_to_wide((const char *)find->lpstrText, -1, CP_ACP, NULL);
		LRESULT result = what ? find_text(editor, (DWORD)wparam, &find->chrg, what, NULL) : -1;
		re_free(narrow);
		return result;
	}
	case EM_FINDTEXTEX:
	case EM_FINDTEXTEXW: {
		FINDTEXTEXW *find = (FINDTEXTEXW *)lparam;
		if (!find)
			return -1;
		WCHAR *narrow = NULL;
		const WCHAR *what = find->lpstrText;
		if (message == EM_FINDTEXTEX && editor->ansi)
			what = narrow = re_ansi_to_wide((const char *)find->lpstrText, -1, CP_ACP, NULL);
		LRESULT result = what ? find_text(editor, (DWORD)wparam, &find->chrg, what, &find->chrgText) : -1;
		re_free(narrow);
		return result;
	}
	case EM_FINDWORDBREAK:
		return find_word_break(editor, (UINT)wparam, (INT)lparam);
	case EM_SETWORDBREAKPROC: {
		EDITWORDBREAKPROCW previous = editor->word_break;
		editor->word_break = (EDITWORDBREAKPROCW)lparam;
		return (LRESULT)previous;
	}
	case EM_GETWORDBREAKPROC:
		return (LRESULT)editor->word_break;
	case EM_SETWORDBREAKPROCEX: {
		EDITWORDBREAKPROCEX previous = editor->word_break_ex;
		editor->word_break_ex = (EDITWORDBREAKPROCEX)lparam;
		return (LRESULT)previous;
	}
	case EM_GETWORDBREAKPROCEX:
		return (LRESULT)editor->word_break_ex;
	case EM_STREAMIN: {
		LRESULT result = re_stream_in(editor, wparam, (EDITSTREAM *)lparam);
		editor->modified = TRUE;
		if (wparam & SFF_SELECTION)
			editor->modified = TRUE;
		re_detect_urls(editor);
		re_refresh(editor);
		re_notify_change(editor);
		re_notify_selection(editor);
		return result;
	}
	case EM_STREAMOUT:
		return re_stream_out(editor, wparam, (EDITSTREAM *)lparam);
	case EM_AUTOURLDETECT: {
		if (wparam > 1)
			return E_INVALIDARG;
		editor->auto_url = wparam != 0;
		if (!wparam) {
			CHARFORMAT2W plain;
			memset(&plain, 0, sizeof(plain));
			plain.cbSize = sizeof(plain);
			plain.dwMask = CFM_LINK;
			re_doc_set_format(doc, 0, doc->length, &plain);
		}
		re_detect_urls(editor);
		re_refresh(editor);
		return 0;
	}
	case EM_GETAUTOURLDETECT:
		return editor->auto_url;
	case EM_GETOLEINTERFACE:
		if (!lparam)
			return 0;
		if (!editor->ole && FAILED(re_ole_create(editor, NULL, &editor->ole)))
			return 0;
		*(void **)lparam = NULL;
		return SUCCEEDED(IUnknown_QueryInterface(editor->ole, &IID_IRichEditOle, (void **)lparam));
	case EM_SETOLECALLBACK: {
		IRichEditOleCallback *callback = (IRichEditOleCallback *)lparam;
		if (callback)
			callback->lpVtbl->AddRef(callback);
		if (editor->callback)
			editor->callback->lpVtbl->Release(editor->callback);
		editor->callback = callback;
		return 1;
	}
	case EM_GETIMECOLOR:
	case EM_SETIMECOLOR:
	case EM_GETIMEOPTIONS:
	case EM_SETIMEOPTIONS:
	case EM_GETPUNCTUATION:
	case EM_SETPUNCTUATION:
	case EM_GETWORDWRAPMODE:
	case EM_SETWORDWRAPMODE:
	case EM_GETIMECOMPMODE:
	case EM_SETLANGOPTIONS:
	case EM_GETLANGOPTIONS:
	case EM_SETTYPOGRAPHYOPTIONS:
	case EM_GETTYPOGRAPHYOPTIONS:
		return 0;
	case EM_SHOWSCROLLBAR:
		if (editor->hwnd)
			ShowScrollBar(editor->hwnd, (INT)wparam, (BOOL)lparam);
		return 0;
	case EM_REQUESTRESIZE:
		if (editor->event_mask & ENM_REQUESTRESIZE) {
			REQRESIZE request;
			memset(&request, 0, sizeof(request));
			re_view_rect(editor, &request.rc);
			request.rc.bottom = request.rc.top + editor->layout.total_height;
			re_send_notify(editor, EN_REQUESTRESIZE, &request.nmhdr);
		}
		return 0;
	case EM_GETHANDLE:
		return 0;
	default:
		if (handled)
			*handled = FALSE;
		return 0;
	}
}

static LRESULT window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, BOOL ansi)
{
	re_editor *editor = (re_editor *)GetWindowLongPtrW(hwnd, 0);
	if (message == WM_NCCREATE) {
		editor = re_calloc(sizeof(*editor));
		if (!editor || !re_editor_create(editor)) {
			re_free(editor);
			return 0;
		}
		editor->hwnd = hwnd;
		editor->ansi = ansi;
		CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
		set_window_style_state(editor, create->style, create->dwExStyle);
		editor->parent = create->hwndParent;
		SetWindowLongPtrW(hwnd, 0, (LONG_PTR)editor);
		return ansi ? DefWindowProcA(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
	}
	if (!editor)
		return ansi ? DefWindowProcA(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
	if (message == WM_CREATE) {
		CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
		re_layout_update(editor);
		if (create->style & ES_DISABLENOSCROLL) {
			if (create->style & WS_VSCROLL)
				SetScrollRange(hwnd, SB_VERT, 0, 1, FALSE);
			if (create->style & WS_HSCROLL)
				SetScrollRange(hwnd, SB_HORZ, 0, 1, FALSE);
		}
		if (create->lpszName && create->lpszName[0])
			on_set_text(editor, create->lpszName);
		re_update_scrollbars(editor);
		return 0;
	}
	if (message == WM_NCDESTROY) {
		re_editor_destroy(editor);
		re_free(editor);
		SetWindowLongPtrW(hwnd, 0, 0);
		return ansi ? DefWindowProcA(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
	}
	if (message == WM_DESTROY && editor->has_caret) {
		DestroyCaret();
		editor->has_caret = FALSE;
	}
	BOOL handled;
	LRESULT result = re_message(editor, message, wparam, lparam, &handled);
	if (!handled)
		return ansi ? DefWindowProcA(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
	return result;
}

static LRESULT WINAPI wide_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	return window_proc(hwnd, message, wparam, lparam, FALSE);
}

LRESULT WINAPI RichEditANSIWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	return window_proc(hwnd, message, wparam, lparam, TRUE);
}

LRESULT WINAPI RichEdit10ANSIWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	return window_proc(hwnd, message, wparam, lparam, TRUE);
}

BOOL re_register_classes(HINSTANCE instance)
{
	WNDCLASSW wide;
	WNDCLASSA narrow;
	memset(&wide, 0, sizeof(wide));
	wide.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW | CS_GLOBALCLASS;
	wide.lpfnWndProc = wide_proc;
	wide.cbWndExtra = sizeof(void *);
	wide.hInstance = instance;
	wide.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32513));
	wide.lpszClassName = L"RichEdit20W";
	RegisterClassW(&wide);
	wide.lpszClassName = L"RichEdit50W";
	RegisterClassW(&wide);
	memset(&narrow, 0, sizeof(narrow));
	narrow.style = wide.style;
	narrow.lpfnWndProc = RichEditANSIWndProc;
	narrow.cbWndExtra = sizeof(void *);
	narrow.hInstance = instance;
	narrow.hCursor = wide.hCursor;
	narrow.lpszClassName = "RichEdit20A";
	RegisterClassA(&narrow);
	return TRUE;
}

void re_unregister_classes(HINSTANCE instance)
{
	UnregisterClassW(L"RichEdit20W", instance);
	UnregisterClassW(L"RichEdit50W", instance);
	UnregisterClassA("RichEdit20A", instance);
}

LRESULT WINAPI REExtendedRegisterClass(void)
{
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		module = instance;
		re_register_classes(instance);
	} else if (reason == DLL_PROCESS_DETACH) {
		re_unregister_classes(instance);
	}
	return TRUE;
}
