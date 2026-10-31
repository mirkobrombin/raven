/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_RE_H
#define RAVEN_RE_H

#include <stddef.h>
#include <windows.h>
#include <richedit.h>
#include <richole.h>
#include <imm.h>
#include "host.h"
#include <tom.h>

#define RE_CR 0x0d
#define RE_LF 0x0a
#define RE_TAB 0x09
#define RE_VT 0x0b
#define RE_OBJECT_MARK 0xfffc
#ifndef CF_RTFW
#define CF_RTFW L"Rich Text Format"
#endif

// A stretch of the text sharing one character format; it reaches the start of the next run.
typedef struct
{
	INT start;
	CHARFORMAT2W format;
} re_run;

// A paragraph starts here and ends after its mark, the next paragraph's start.
typedef struct
{
	INT start;
	PARAFORMAT2 format;
} re_para;

// The text. Paragraph marks are single CRs and the last character of the document is always the final mark.
typedef struct
{
	WCHAR *text;
	INT length, capacity;
	re_run *runs;
	INT run_count, run_capacity;
	re_para *paras;
	INT para_count, para_capacity;
} re_doc;

// What a stretch of removed text looked like, enough to put it back.
typedef struct
{
	INT length;
	WCHAR *text;
	re_run *runs;
	INT run_count;
	re_para *paras;
	INT para_count;
} re_piece;

typedef enum
{
	UNDO_EDIT,		// text replaced
	UNDO_FORMAT		// the whole model before a formatting change
} re_undo_kind;

typedef struct
{
	re_undo_kind kind;
	INT at, inserted;
	re_piece removed;
	re_doc *before;
	INT sel_start, sel_end;
	INT group;
	UNDONAMEID name;
} re_undo_item;

typedef struct
{
	re_undo_item *items;
	INT count, capacity;
} re_undo_list;

typedef struct
{
	INT start, length;	// characters, the break or mark included
	INT y, height, ascent, width, x;
	INT para;
	INT visible;		// characters that are drawn: without the trailing mark and spaces
} re_line;

// One rendering of the document on a device, in pixels of that device.
typedef struct
{
	re_line *lines;
	INT count, capacity;
	INT total_height, widest;
} re_layout;

typedef struct re_editor re_editor;

struct re_editor
{
	re_doc doc;
	re_layout layout;
	HWND hwnd;
	ITextHost *host;		// text services: everything about the window goes through it
	ITextHost2 *host2;
	BOOL host_window;		// the window owns this editor, as opposed to a program's own host
	BOOL in_place;			// a program's host has activated it in its window
	BOOL ansi;			// the window speaks the code page
	BOOL version1;			// created through the 1.0 entry point
	DWORD style, ex_style;
	DWORD options;			// ECO_*
	DWORD event_mask;
	DWORD text_mode;		// TM_*
	INT limit;			// most characters
	INT sel_start, sel_end;		// sel_start is the anchor; sel_end has the caret
	BOOL caret_at_line_start;
	INT goal_x;
	WCHAR password;
	BOOL modified;
	BOOL read_only;
	BOOL focused;
	BOOL hide_selection;
	BOOL auto_url;
	UINT auto_url_type;
	BOOL dragging;
	BOOL updating;
	BOOL wrap;			// lines break at the width
	BOOL dialog_mode;
	BOOL multiline;
	BOOL has_caret;
	BOOL hide_caret;
	BOOL changed_since_notify;
	COLORREF back_color;
	BOOL back_system;
	INT scroll_x, scroll_y;
	INT zoom_num, zoom_den;
	RECT format_rect;		// EM_SETRECT
	BOOL has_format_rect;
	INT margin_left, margin_right;
	UINT border_x, border_y;
	HFONT set_font;
	HDC target_dc;
	INT target_width;
	EDITWORDBREAKPROCW word_break;
	EDITWORDBREAKPROCEX word_break_ex;
	IRichEditOleCallback *callback;
	re_undo_list undo, redo;
	INT undo_limit;
	INT undo_limit_kept;		// to come back to when a disabled history is enabled again
	BOOL undo_suspended;
	INT undo_group;
	BOOL undo_open;			// consecutive typing joins one step
	BOOL in_undo;
	UNDONAMEID undo_name;
	UNDONAMEID redo_name;
	IUnknown *ole;			// IRichEditOle, once asked for
	CHARFORMAT2W insert_format;	// what the next typed text looks like
	BOOL has_insert_format;
	INT notify_lock;
	LONG refs;
	IUnknown *outer;
	BOOL client_valid;
	RECT client;
	BOOL scroll_v_shown, scroll_h_shown;
	DWORD scroll_bars;
	HWND parent;
	BOOL enter_typed;
	CHARFORMAT2W default_format;
	INT scroll_v_sent[3], scroll_h_sent[3];
	BOOL in_scrollbars;
	POINT last_url;
	INT link_start, link_end;
	WCHAR *auto_url_scheme;
};

// memory.c
void *re_alloc(SIZE_T size);
void *re_calloc(SIZE_T size);
void *re_realloc(void *block, SIZE_T size);
void re_free(void *block);
WCHAR *re_ansi_to_wide(const char *text, INT length, UINT code_page, INT *out_length);
char *re_wide_to_ansi(const WCHAR *text, INT length, UINT code_page, INT *out_length);

// doc.c
BOOL re_doc_init(re_doc *doc, const CHARFORMAT2W *format);
void re_doc_clear(re_doc *doc);
BOOL re_doc_replace(re_doc *doc, INT at, INT remove, const WCHAR *text, INT length, const CHARFORMAT2W *format);
void re_doc_char_format(const re_doc *doc, INT at, CHARFORMAT2W *out);
INT re_doc_run_at(const re_doc *doc, INT at);
INT re_doc_para_at(const re_doc *doc, INT at);
INT re_doc_para_end(const re_doc *doc, INT para);
BOOL re_doc_set_format(re_doc *doc, INT start, INT end, const CHARFORMAT2W *format);
void re_doc_set_para(re_doc *doc, INT start, INT end, const PARAFORMAT2 *format);
BOOL re_doc_save_piece(const re_doc *doc, INT at, INT length, re_piece *out);
void re_doc_free_piece(re_piece *piece);
BOOL re_doc_put_piece(re_doc *doc, INT at, const re_piece *piece);
re_doc *re_doc_clone(const re_doc *doc);
void re_doc_assign(re_doc *doc, re_doc *from);
void re_format_merge(CHARFORMAT2W *format, const CHARFORMAT2W *with);
void re_para_merge(PARAFORMAT2 *format, const PARAFORMAT2 *with);
BOOL re_format_from_user(CHARFORMAT2W *out, const void *user, BOOL ansi);
BOOL re_format_to_user(const CHARFORMAT2W *format, void *user, BOOL ansi, DWORD mask);
void re_default_char_format(CHARFORMAT2W *out, HFONT font);
void re_default_para_format(PARAFORMAT2 *out);

// edit.c
void re_editor_fix_selection(re_editor *editor);
INT re_text_length(const re_editor *editor);
BOOL re_replace(re_editor *editor, INT at, INT remove, const WCHAR *text, INT length, const CHARFORMAT2W *format, BOOL undoable);
void re_undo_begin_step(re_editor *editor);
void re_undo_clear(re_undo_list *list);
void re_undo_format(re_editor *editor);
BOOL re_undo(re_editor *editor);
BOOL re_redo(re_editor *editor);
void re_notify_change(re_editor *editor);
void re_notify_selection(re_editor *editor);
void re_refresh(re_editor *editor);
INT re_get_text_crlf(const re_editor *editor, INT start, INT end, WCHAR *out, INT capacity, BOOL crlf);
INT re_count_crlf(const re_editor *editor, INT start, INT end);
void re_set_selection(re_editor *editor, INT anchor, INT caret);
void re_set_selection_user(re_editor *editor, INT start, INT end);
void re_get_selection_format(re_editor *editor, CHARFORMAT2W *out, DWORD *differing);
void re_apply_format(re_editor *editor, DWORD flags, const CHARFORMAT2W *format);
void re_insert_typed(re_editor *editor, const WCHAR *text, INT length);
void re_delete_selection(re_editor *editor);
BOOL re_editor_create(re_editor *editor);
void re_editor_destroy(re_editor *editor);
void re_detect_urls(re_editor *editor);

// layout.c
void re_layout_free(re_layout *layout);
void re_layout_update(re_editor *editor);
void re_layout_document(re_editor *editor, HDC dc, re_layout *out, INT width, INT dpi_scale_num, INT dpi_scale_den);
INT re_line_at(const re_layout *layout, INT at, BOOL at_line_start);
void re_position(re_editor *editor, INT at, BOOL at_line_start, POINT *out, INT *height);
INT re_char_from_point(re_editor *editor, INT x, INT y, BOOL *at_line_start);
void re_paint(re_editor *editor, HDC dc, const RECT *update);
void re_view_rect(re_editor *editor, RECT *out);
void re_update_caret(re_editor *editor);
void re_scroll_to_caret(re_editor *editor);
void re_update_scrollbars(re_editor *editor);
void re_scroll_to(re_editor *editor, INT x, INT y);
INT re_zoomed(const re_editor *editor, INT value);
HFONT re_create_font(const CHARFORMAT2W *format, HDC dc, const re_editor *editor);
LRESULT re_format_range(re_editor *editor, FORMATRANGE *range, BOOL render);
void re_invalidate(re_editor *editor);

// window.c
LRESULT re_message(re_editor *editor, UINT message, WPARAM wparam, LPARAM lparam, BOOL *handled);
BOOL re_register_classes(HINSTANCE instance);
void re_unregister_classes(HINSTANCE instance);
LRESULT re_send_notify(re_editor *editor, UINT code, NMHDR *header);
HINSTANCE re_instance(void);
void re_copy_selected(re_editor *editor);

// rtf.c
LRESULT re_stream_in(re_editor *editor, WPARAM flags, EDITSTREAM *stream);
LRESULT re_stream_out(re_editor *editor, WPARAM flags, EDITSTREAM *stream);

// txtsrv.c
HRESULT re_ole_create(re_editor *editor, IUnknown *controlling, IUnknown **out);
BOOL re_ole_frozen(const re_editor *editor);
void re_ole_detach(IUnknown *ole);
void re_ole_text_replaced(re_editor *editor, INT at, INT removed, INT inserted);
void re_ole_text_reset(re_editor *editor);
BOOL re_ole_object_size(re_editor *editor, HDC dc, INT at, SIZE *out);
void re_ole_draw(re_editor *editor, HDC dc, INT at, INT left, INT bottom);

#endif
