/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

/* The tree view control. Nodes form a linked tree under a hidden root; a layout pass flattens the expanded part into a row array, rebuilt lazily
 * whenever the shape changes. Text is stored wide, A messages convert at the edge and notifications follow the format the parent asked for. */

#define DEFAULT_INDENT 19
#define MINIMUM_INDENT 19
#define LABEL_PADDING 3
#define TEXT_LIMIT 1024
#define EDIT_LIMIT MAX_PATH
#define GLYPH_BOX 9
#define EDIT_TIMER 1
#define SEARCH_RESET_MS 1000
#define LIVE_BUCKETS 16384
#define NO_EXPANDED_IMAGE ((WORD)I_IMAGENONE)
#define CALLBACK_TEXT_W ((WCHAR *)LPSTR_TEXTCALLBACKW)
#define NOTIFY_CODE(state, name) ((state)->unicode ? name##W : name##A)

#ifndef TVE_ACTIONMASK
#define TVE_ACTIONMASK 0x0003
#endif

typedef struct {
	NMHDR hdr;
	UINT changed;
	HTREEITEM item;
	UINT new_state, old_state;
	LPARAM lparam;
} item_change;

enum { GESTURE_NONE, GESTURE_LEFT, GESTURE_RIGHT };

/* Programs reach through an HTREEITEM for the first three links and, in version 6, for the label width, so the head of this struct is an ABI. */
typedef struct tree_node {
	struct tree_node *parent, *next, *first_child;
	struct tree_node *prev, *last_child;
	WCHAR *text;
	UINT state;
	UINT accessibility_id;
	WORD pad;
	WORD width;
	struct tree_state *owner;
	struct tree_node *live_next;
	LPARAM lparam;
	int image, selected_image, children, integral;
	UINT state_ex;
	int expanded_image;
	BOOL callback_text, automatic_children, measured;
	int level, row_index, row_top;
	UINT stamp;
} tree_node;

typedef struct tree_state {
	HWND window, parent, edit, tooltips;
	WNDPROC edit_proc;
	tree_node root;
	tree_node **rows;
	int row_count, row_capacity, total_rows, content_width;
	int top, scroll_x;
	UINT stamp;
	BOOL dirty, in_layout, redraw, unicode, focused;
	tree_node *selected, *drop, *hot, *mark, *tip, *edited, *press;
	tree_node *editable;
	BOOL mark_after;
	HFONT font, own_font, bold_font, hot_font;
	HIMAGELIST normal_list, state_list, own_state_list;
	int normal_cx, normal_cy, state_cx, state_cy;
	int indent, height, custom_height, count;
	int scroll_time;
	COLORREF background, foreground, line_color, mark_color;
	DWORD extended_style, last_style;
	int gesture, wheel_rest;
	POINT press_at;
	WCHAR search[64];
	int search_length;
	DWORD search_time;
	UINT next_accessibility_id;
	HTHEME theme;
	int busy;
	BOOL dead;
} tree_state;

typedef struct {
	int image, selected_image, children;
	UINT state;
	const WCHAR *text;
	char raw[TEXT_LIMIT * sizeof(WCHAR)];
	WCHAR wide[TEXT_LIMIT];
} item_view;

static tree_node *live_nodes[LIVE_BUCKETS];
static CRITICAL_SECTION live_lock;
static BOOL live_lock_ready;

static tree_state *state_of(HWND window)
{
	return (tree_state *)GetWindowLongPtrW(window, 0);
}

static DWORD style_of(const tree_state *state)
{
	return (DWORD)GetWindowLongPtrW(state->window, GWL_STYLE);
}

static void *allocate(SIZE_T size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void release(void *block)
{
	if (block)
		HeapFree(GetProcessHeap(), 0, block);
}

static WCHAR *copy_wide(const WCHAR *text)
{
	int length = lstrlenW(text) + 1;
	WCHAR *copy = allocate(length * sizeof(WCHAR));
	if (copy)
		CopyMemory(copy, text, length * sizeof(WCHAR));
	return copy;
}

static WCHAR *widen(const char *text)
{
	int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
	WCHAR *copy = allocate(length * sizeof(WCHAR));
	if (copy)
		MultiByteToWideChar(CP_ACP, 0, text, -1, copy, length);
	return copy;
}

/* A handle can be probed on any tree, and a stale or invented one must fail rather than fault, so liveness is tracked process wide. */
static UINT live_bucket(const tree_node *node)
{
	ULONG_PTR value = (ULONG_PTR)node;
	return (UINT)((value >> 4) ^ (value >> 18)) % LIVE_BUCKETS;
}

static void live_add(tree_node *node)
{
	UINT bucket = live_bucket(node);
	EnterCriticalSection(&live_lock);
	node->live_next = live_nodes[bucket];
	live_nodes[bucket] = node;
	LeaveCriticalSection(&live_lock);
}

static void live_remove(tree_node *node)
{
	tree_node **link = &live_nodes[live_bucket(node)];
	EnterCriticalSection(&live_lock);
	while (*link && *link != node)
		link = &(*link)->live_next;
	if (*link)
		*link = node->live_next;
	LeaveCriticalSection(&live_lock);
}

static BOOL is_live(const tree_node *node)
{
	const tree_node *walk;
	EnterCriticalSection(&live_lock);
	for (walk = live_nodes[live_bucket(node)]; walk && walk != node; walk = walk->live_next)
		;
	LeaveCriticalSection(&live_lock);
	return walk != NULL;
}

/* Any live node, even from another tree. */
static tree_node *any_node(HTREEITEM handle)
{
	tree_node *node = (tree_node *)handle;
	return node && is_live(node) ? node : NULL;
}

static tree_node *own_node(const tree_state *state, HTREEITEM handle)
{
	tree_node *node = any_node(handle);
	return node && node->owner == state ? node : NULL;
}

static LRESULT notify(tree_state *state, NMHDR *header, UINT code)
{
	header->hwndFrom = state->window;
	header->idFrom = GetDlgCtrlID(state->window);
	header->code = code;
	return SendMessageW(state->parent, WM_NOTIFY, header->idFrom, (LPARAM)header);
}

static void fill_notify_item(TVITEMW *item, const tree_node *node, UINT mask)
{
	item->mask = node ? mask : 0;
	if (!node)
		return;
	item->hItem = (HTREEITEM)node;
	item->state = node->state;
	item->stateMask = TVIS_STATEIMAGEMASK | TVIS_OVERLAYMASK | 0xff;
	item->iImage = node->image;
	item->iSelectedImage = node->selected_image;
	item->cChildren = node->automatic_children ? node->first_child != NULL : node->children;
	item->lParam = node->lparam;
}

static LRESULT notify_tree(tree_state *state, UINT code, UINT action, const tree_node *old_node, UINT old_mask, const tree_node *new_node, UINT new_mask)
{
	NMTREEVIEWW info;
	ZeroMemory(&info, sizeof(info));
	info.action = action;
	fill_notify_item(&info.itemOld, old_node, old_mask);
	fill_notify_item(&info.itemNew, new_node, new_mask);
	GetCursorPos(&info.ptDrag);
	ScreenToClient(state->window, &info.ptDrag);
	return notify(state, &info.hdr, code);
}

/* Reads a node as the parent wants it seen, asking for whatever it keeps itself. */
static void fetch_view(tree_state *state, tree_node *node, UINT wanted, item_view *view)
{
	NMTVDISPINFOW info;
	UINT ask = 0;

	view->image = node->image;
	view->selected_image = node->selected_image;
	view->children = node->automatic_children ? node->first_child != NULL : node->children;
	view->state = node->state;
	view->text = node->text ? node->text : L"";
	if ((wanted & TVIF_TEXT) && node->callback_text)
		ask |= TVIF_TEXT;
	if ((wanted & TVIF_IMAGE) && node->image == I_IMAGECALLBACK)
		ask |= TVIF_IMAGE;
	if ((wanted & TVIF_SELECTEDIMAGE) && node->selected_image == I_IMAGECALLBACK)
		ask |= TVIF_SELECTEDIMAGE;
	if ((wanted & TVIF_CHILDREN) && !node->automatic_children && node->children == I_CHILDRENCALLBACK)
		ask |= TVIF_CHILDREN;
	if (!ask)
		return;

	ZeroMemory(&info, sizeof(info));
	view->raw[0] = view->raw[1] = 0;
	info.item.mask = ask;
	info.item.hItem = (HTREEITEM)node;
	info.item.state = node->state;
	info.item.stateMask = 0;
	info.item.lParam = node->lparam;
	info.item.pszText = (WCHAR *)view->raw;
	info.item.cchTextMax = TEXT_LIMIT;
	info.item.iImage = node->image;
	info.item.iSelectedImage = node->selected_image;
	info.item.cChildren = 0;
	notify(state, &info.hdr, NOTIFY_CODE(state, TVN_GETDISPINFO));
	if (!is_live(node))
		return;

	if (ask & TVIF_IMAGE)
		view->image = info.item.iImage;
	if (ask & TVIF_SELECTEDIMAGE)
		view->selected_image = info.item.iSelectedImage;
	if (ask & TVIF_CHILDREN)
		view->children = info.item.cChildren;
	if (info.item.mask & TVIF_STATE)
		view->state = (view->state & ~info.item.stateMask) | (info.item.state & info.item.stateMask);
	if (ask & TVIF_TEXT) {
		const void *answer = info.item.pszText;
		if (!answer || answer == (const void *)LPSTR_TEXTCALLBACKW)
			view->text = L"";
		else if (info.hdr.code == TVN_GETDISPINFOW)
			view->text = answer;
		else {
			/* The notify set the code; only a handler that rewrote it speaks another format. */
			view->wide[0] = 0;
			MultiByteToWideChar(CP_ACP, 0, answer, -1, view->wide, TEXT_LIMIT);
			view->wide[TEXT_LIMIT - 1] = 0;
			view->text = view->wide;
		}
	}
	if (info.item.mask & TVIF_DI_SETITEM) {
		if (ask & TVIF_IMAGE)
			node->image = view->image;
		if (ask & TVIF_SELECTEDIMAGE)
			node->selected_image = view->selected_image;
		if (ask & TVIF_CHILDREN)
			node->children = view->children;
		if (ask & TVIF_TEXT) {
			release(node->text);
			node->text = copy_wide(view->text);
			node->callback_text = FALSE;
			node->measured = FALSE;
		}
	}
}

static BOOL has_children(tree_state *state, tree_node *node)
{
	item_view view;
	if (node->automatic_children)
		return node->first_child != NULL;
	if (node->children != I_CHILDRENCALLBACK)
		return node->children != 0;
	fetch_view(state, node, TVIF_CHILDREN, &view);
	return view.children != 0;
}

static void invalidate(tree_state *state)
{
	if (state->redraw)
		InvalidateRect(state->window, NULL, FALSE);
}

static void reshape(tree_state *state)
{
	state->dirty = TRUE;
	invalidate(state);
}

static HFONT label_font(const tree_state *state, const tree_node *node)
{
	if (node && node == state->hot && (style_of(state) & TVS_TRACKSELECT) && state->hot_font)
		return state->hot_font;
	if (node && (node->state & TVIS_BOLD) && state->bold_font)
		return state->bold_font;
	return state->font;
}

static void measure_node(tree_state *state, tree_node *node, HDC dc)
{
	item_view view;
	SIZE size = { 0, 0 };
	HGDIOBJ old;
	const WCHAR *text = node->text ? node->text : L"";

	if (node->measured)
		return;
	if (node->callback_text) {
		fetch_view(state, node, TVIF_TEXT, &view);
		if (!is_live(node))
			return;
		text = view.text;
	}
	old = SelectObject(dc, label_font(state, node));
	GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
	SelectObject(dc, old);
	node->width = (WORD)(size.cx + 2 * LABEL_PADDING);
	node->measured = TRUE;
}

static int indent_columns(const tree_state *state, const tree_node *node)
{
	return node->level + ((style_of(state) & TVS_LINESATROOT) ? 1 : 0);
}

/* Where the state icon begins; the expander column ends here. */
static int content_left(const tree_state *state, const tree_node *node)
{
	return indent_columns(state, node) * state->indent - state->scroll_x;
}

static int label_left(const tree_state *state, const tree_node *node)
{
	return content_left(state, node) + state->state_cx + state->normal_cx;
}

static int row_y(const tree_state *state, const tree_node *node)
{
	return (node->row_top - state->top) * state->height;
}

static int page_rows(const tree_state *state)
{
	RECT client;
	GetClientRect(state->window, &client);
	return state->height > 0 ? client.bottom / state->height : 0;
}

static void update_scrollbars(tree_state *state)
{
	SCROLLINFO info;
	RECT client;
	int page, limit;

	GetClientRect(state->window, &client);
	page = page_rows(state);
	limit = state->total_rows - (page > 0 ? page : 1);
	if (limit < 0)
		limit = 0;
	if (state->top > limit)
		state->top = limit;
	if (state->top < 0)
		state->top = 0;
	if (state->scroll_x > state->content_width - client.right)
		state->scroll_x = state->content_width > client.right ? state->content_width - client.right : 0;
	if (style_of(state) & TVS_NOSCROLL)
		return;

	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	info.nMax = state->total_rows > 0 ? state->total_rows - 1 : 0;
	info.nPage = page;
	info.nPos = state->top;
	ShowScrollBar(state->window, SB_VERT, state->total_rows > page);
	SetScrollInfo(state->window, SB_VERT, &info, state->redraw);

	info.nMax = state->content_width > 0 ? state->content_width - 1 : 0;
	info.nPage = client.right;
	info.nPos = state->scroll_x;
	if (style_of(state) & TVS_NOHSCROLL) {
		ShowScrollBar(state->window, SB_HORZ, FALSE);
		return;
	}
	ShowScrollBar(state->window, SB_HORZ, state->content_width > client.right);
	SetScrollInfo(state->window, SB_HORZ, &info, state->redraw);
}

static void flatten_rows(tree_state *state)
{
	tree_node *node = state->root.first_child;
	HDC dc = GetDC(state->window);
	int level = 0;

	state->stamp++;
	state->row_count = 0;
	state->total_rows = 0;
	state->content_width = 0;
	while (node) {
		int right;
		if (state->row_count == state->row_capacity) {
			int capacity = state->row_capacity ? state->row_capacity * 2 : 64;
			tree_node **rows = allocate(capacity * sizeof(*rows));
			if (!rows)
				break;
			if (state->rows)
				CopyMemory(rows, state->rows, state->row_count * sizeof(*rows));
			release(state->rows);
			state->rows = rows;
			state->row_capacity = capacity;
		}
		node->stamp = state->stamp;
		node->level = level;
		node->row_index = state->row_count;
		node->row_top = state->total_rows;
		state->rows[state->row_count++] = node;
		state->total_rows += node->integral;
		if (!node->callback_text)
			measure_node(state, node, dc);
		right = indent_columns(state, node) * state->indent + state->state_cx + state->normal_cx + node->width;
		if (right > state->content_width)
			state->content_width = right;

		if ((node->state & TVIS_EXPANDED) && node->first_child) {
			node = node->first_child;
			level++;
			continue;
		}
		while (node && !node->next) {
			node = node->parent;
			level--;
			if (node == &state->root)
				node = NULL;
		}
		if (node)
			node = node->next;
	}
	ReleaseDC(state->window, dc);
}

/* Showing a scrollbar resizes the client and marks the layout dirty again, hence the second pass. */
static void ensure_layout(tree_state *state)
{
	int pass;
	if (!state->dirty || state->in_layout)
		return;
	state->in_layout = TRUE;
	for (pass = 0; pass < 2 && state->dirty; pass++) {
		state->dirty = FALSE;
		flatten_rows(state);
		update_scrollbars(state);
	}
	state->dirty = FALSE;
	state->in_layout = FALSE;
}

static BOOL is_shown(tree_state *state, const tree_node *node)
{
	ensure_layout(state);
	return node->owner == state && node->stamp == state->stamp;
}

static tree_node *first_visible(tree_state *state)
{
	int low = 0, high;
	ensure_layout(state);
	if (!state->row_count)
		return NULL;
	high = state->row_count - 1;
	while (low < high) {
		int middle = (low + high + 1) / 2;
		if (state->rows[middle]->row_top <= state->top)
			low = middle;
		else
			high = middle - 1;
	}
	return state->rows[low];
}

static void move_edit(tree_state *state);

static void set_top(tree_state *state, int top)
{
	ensure_layout(state);
	state->top = top;
	update_scrollbars(state);
	move_edit(state);
	invalidate(state);
}

static BOOL scroll_into_view(tree_state *state, tree_node *node)
{
	int page, old_top = state->top;
	if (!is_shown(state, node))
		return FALSE;
	page = page_rows(state);
	if (node->row_top < state->top)
		state->top = node->row_top;
	else if (page > 0 && node->row_top + node->integral > state->top + page)
		state->top = node->row_top + node->integral - page;
	if (state->top == old_top)
		return FALSE;
	set_top(state, state->top);
	return TRUE;
}

static BOOL is_ancestor_or_self(const tree_node *ancestor, const tree_node *node)
{
	for (; node; node = node->parent)
		if (node == ancestor)
			return TRUE;
	return FALSE;
}

static BOOL select_node(tree_state *state, tree_node *node, UINT action, BOOL single_expand);

static void after_collapse(tree_state *state, tree_node *node)
{
	if (state->selected && state->selected != node && is_ancestor_or_self(node, state->selected))
		select_node(state, node, TVC_UNKNOWN, FALSE);
}

#define EXPAND_MASK (TVIF_HANDLE | TVIF_SELECTEDIMAGE | TVIF_IMAGE | TVIF_PARAM | TVIF_STATE)

/* What a person triggers tells the parent every time, even when nothing ends up changing. */
static BOOL user_expand(tree_state *state, tree_node *node)
{
	if (!has_children(state, node))
		return FALSE;
	if (notify_tree(state, NOTIFY_CODE(state, TVN_ITEMEXPANDING), TVE_EXPAND, NULL, 0, node, EXPAND_MASK) || !is_live(node))
		return FALSE;
	if (!node->first_child)
		return FALSE;
	node->state |= TVIS_EXPANDED;
	reshape(state);
	notify_tree(state, NOTIFY_CODE(state, TVN_ITEMEXPANDED), TVE_EXPAND, NULL, 0, node, EXPAND_MASK);
	if (!is_live(node))
		return FALSE;
	node->state |= TVIS_EXPANDEDONCE;
	return TRUE;
}

static BOOL user_collapse(tree_state *state, tree_node *node)
{
	if (!has_children(state, node))
		return FALSE;
	if (notify_tree(state, NOTIFY_CODE(state, TVN_ITEMEXPANDING), TVE_COLLAPSE, NULL, 0, node, EXPAND_MASK) || !is_live(node))
		return FALSE;
	if (!(node->state & TVIS_EXPANDED))
		return FALSE;
	node->state &= ~TVIS_EXPANDED;
	reshape(state);
	notify_tree(state, NOTIFY_CODE(state, TVN_ITEMEXPANDED), TVE_COLLAPSE, NULL, 0, node, EXPAND_MASK);
	if (!is_live(node))
		return FALSE;
	after_collapse(state, node);
	return TRUE;
}

static void reveal_ancestors(tree_state *state, tree_node *node)
{
	if (!node || node == &state->root)
		return;
	reveal_ancestors(state, node->parent);
	if (!(node->state & TVIS_EXPANDED))
		user_expand(state, node);
}

static void change_selected_flag(tree_state *state, tree_node *node, BOOL selected)
{
	item_change change;
	UINT wanted = selected ? node->state | TVIS_SELECTED : node->state & ~TVIS_SELECTED;

	ZeroMemory(&change, sizeof(change));
	change.changed = TVIF_STATE;
	change.item = (HTREEITEM)node;
	change.new_state = wanted;
	change.old_state = node->state;
	change.lparam = node->lparam;
	if (notify(state, &change.hdr, TVN_ITEMCHANGINGW) || !is_live(node))
		return;
	node->state = wanted;
	notify(state, &change.hdr, TVN_ITEMCHANGEDW);
}

static void apply_single_expand(tree_state *state, tree_node *old_node, tree_node *node)
{
	LRESULT answer = notify_tree(state, TVN_SINGLEEXPAND, TVC_UNKNOWN, old_node, TVIF_HANDLE | TVIF_STATE | TVIF_PARAM, node, TVIF_HANDLE | TVIF_STATE | TVIF_PARAM);
	tree_node *walk;

	if (node && !(answer & TVNRET_SKIPNEW) && !(node->state & TVIS_EXPANDED) && node->first_child)
		user_expand(state, node);
	if (answer & TVNRET_SKIPOLD)
		return;
	for (walk = old_node; walk && walk != &state->root && is_live(walk); walk = walk->parent) {
		if (node && is_ancestor_or_self(walk, node))
			break;
		if (walk->state & TVIS_EXPANDED)
			user_collapse(state, walk);
	}
}

static BOOL select_node(tree_state *state, tree_node *node, UINT action, BOOL single_expand)
{
	tree_node *old_node = state->selected;
	UINT mask = TVIF_HANDLE | TVIF_STATE | TVIF_PARAM;

	if (node == old_node)
		return TRUE;
	if (notify_tree(state, NOTIFY_CODE(state, TVN_SELCHANGING), action, old_node, mask, node, mask))
		return FALSE;
	if ((node && !is_live(node)) || (old_node && !is_live(old_node)))
		return FALSE;

	/* The caret moves even if the parent refuses the new item its selected state. */
	state->selected = node;
	if (node)
		change_selected_flag(state, node, TRUE);
	if (old_node && is_live(old_node))
		change_selected_flag(state, old_node, FALSE);
	if (node && is_live(node)) {
		reveal_ancestors(state, node->parent);
		scroll_into_view(state, node);
	}
	invalidate(state);
	notify_tree(state, NOTIFY_CODE(state, TVN_SELCHANGED), action, old_node, mask, node, mask);
	if (single_expand && (style_of(state) & TVS_SINGLEEXPAND) && (!node || is_live(node)))
		apply_single_expand(state, old_node && is_live(old_node) ? old_node : NULL, node);
	return TRUE;
}

static void unlink_node(tree_node *node)
{
	if (node->prev)
		node->prev->next = node->next;
	else
		node->parent->first_child = node->next;
	if (node->next)
		node->next->prev = node->prev;
	else
		node->parent->last_child = node->prev;
}

static void end_edit(tree_state *state, BOOL cancel, BOOL quiet);

static void delete_subtree(tree_state *state, tree_node *node)
{
	while (node->first_child)
		delete_subtree(state, node->first_child);
	notify_tree(state, NOTIFY_CODE(state, TVN_DELETEITEM), 0, node, TVIF_HANDLE | TVIF_PARAM, NULL, 0);
	unlink_node(node);
	live_remove(node);
	if (state->selected == node)
		state->selected = NULL;
	if (state->drop == node)
		state->drop = NULL;
	if (state->hot == node)
		state->hot = NULL;
	if (state->mark == node)
		state->mark = NULL;
	if (state->tip == node)
		state->tip = NULL;
	if (state->press == node)
		state->press = NULL;
	if (state->editable == node) {
		state->editable = NULL;
		KillTimer(state->window, EDIT_TIMER);
	}
	state->count--;
	release(node->text);
	release(node);
}

static void delete_all(tree_state *state)
{
	if (state->edit)
		end_edit(state, TRUE, TRUE);
	state->selected = NULL;
	while (state->root.first_child)
		delete_subtree(state, state->root.first_child);
	state->top = 0;
	reshape(state);
}

static void delete_children(tree_state *state, tree_node *node)
{
	if (state->selected && state->selected != node && is_ancestor_or_self(node, state->selected))
		select_node(state, node, TVC_UNKNOWN, FALSE);
	if (state->edited && is_ancestor_or_self(node, state->edited))
		end_edit(state, TRUE, TRUE);
	while (node->first_child)
		delete_subtree(state, node->first_child);
}

static BOOL delete_item(tree_state *state, tree_node *node)
{
	tree_node *parent = node->parent;

	if (state->edited && is_ancestor_or_self(node, state->edited))
		end_edit(state, TRUE, TRUE);
	if (state->selected && is_ancestor_or_self(node, state->selected)) {
		tree_node *heir = node->next ? node->next : node->prev;
		if (!heir && parent != &state->root)
			heir = parent;
		select_node(state, heir, TVC_UNKNOWN, FALSE);
		if (!is_live(node))
			return TRUE;
		parent = node->parent;
	}
	delete_subtree(state, node);
	if (!parent->first_child && parent != &state->root)
		parent->state &= ~(TVIS_EXPANDED | TVIS_EXPANDEDONCE);
	reshape(state);
	return TRUE;
}

static tree_node *find_sorted_place(tree_state *state, tree_node *parent, const WCHAR *text)
{
	tree_node *walk, *after = NULL;
	item_view *view = allocate(sizeof(*view));
	if (!view)
		return parent->last_child;
	for (walk = parent->first_child; walk; walk = walk->next) {
		fetch_view(state, walk, TVIF_TEXT, view);
		if (lstrcmpiW(view->text, text) > 0)
			break;
		after = walk;
	}
	release(view);
	return after;
}

static void link_after(tree_node *parent, tree_node *after, tree_node *node)
{
	node->parent = parent;
	node->prev = after;
	node->next = after ? after->next : parent->first_child;
	if (node->next)
		node->next->prev = node;
	else
		parent->last_child = node;
	if (after)
		after->next = node;
	else
		parent->first_child = node;
}

static BOOL set_text(tree_node *node, const void *text, BOOL wide)
{
	WCHAR *copy = NULL;
	BOOL callback = !text || text == (const void *)LPSTR_TEXTCALLBACKW;
	if (!callback) {
		copy = wide ? copy_wide(text) : widen(text);
		if (!copy)
			return FALSE;
	}
	release(node->text);
	node->text = copy;
	node->callback_text = callback;
	node->measured = FALSE;
	return TRUE;
}

static tree_node *insert_item(tree_state *state, const TVINSERTSTRUCTW *request, BOOL wide)
{
	const TVITEMEXW *item = &request->itemex;
	tree_node *parent, *after, *node;
	HTREEITEM place = request->hInsertAfter;

	if (!request->hParent || request->hParent == TVI_ROOT)
		parent = &state->root;
	else if (!(parent = own_node(state, request->hParent)))
		return NULL;

	node = allocate(sizeof(*node));
	if (!node)
		return NULL;
	node->owner = state;
	node->integral = 1;
	node->automatic_children = TRUE;
	node->expanded_image = NO_EXPANDED_IMAGE;
	node->accessibility_id = ++state->next_accessibility_id;
	if (style_of(state) & TVS_CHECKBOXES)
		node->state = INDEXTOSTATEIMAGEMASK(1);
	if ((item->mask & TVIF_TEXT) && !set_text(node, item->pszText, wide)) {
		release(node);
		return NULL;
	}
	if (item->mask & TVIF_IMAGE)
		node->image = item->iImage;
	if (item->mask & TVIF_SELECTEDIMAGE)
		node->selected_image = item->iSelectedImage;
	if (item->mask & TVIF_PARAM)
		node->lparam = item->lParam;
	if (item->mask & TVIF_STATE)
		node->state = (node->state & ~item->stateMask) | (item->state & item->stateMask);
	if (item->mask & TVIF_CHILDREN) {
		node->children = item->cChildren;
		node->automatic_children = FALSE;
	}
	if (item->mask & TVIF_INTEGRAL)
		node->integral = item->iIntegral > 0 ? item->iIntegral : 1;
	if (item->mask & TVIF_STATEEX)
		node->state_ex = item->uStateEx;
	if (item->mask & TVIF_EXPANDEDIMAGE)
		node->expanded_image = item->iExpandedImage;

	if (place == TVI_FIRST)
		after = NULL;
	else if (place == TVI_SORT)
		after = node->callback_text ? parent->last_child : find_sorted_place(state, parent, node->text ? node->text : L"");
	else {
		after = own_node(state, place);
		if (!after || after->parent != parent)
			after = parent->last_child;
	}
	link_after(parent, after, node);
	live_add(node);
	state->count++;
	reshape(state);
	return node;
}

static void copy_text_out(const WCHAR *text, void *target, int capacity, BOOL wide)
{
	if (!target || capacity <= 0)
		return;
	if (wide) {
		lstrcpynW(target, text, capacity);
		return;
	}
	if (!WideCharToMultiByte(CP_ACP, 0, text, -1, target, capacity, NULL, NULL))
		((char *)target)[capacity - 1] = 0;
}

static LRESULT get_item(tree_state *state, TVITEMEXW *item, BOOL wide)
{
	tree_node *node = any_node(item->hItem);
	item_view *view;

	if (!node)
		return FALSE;
	view = allocate(sizeof(*view));
	if (!view)
		return FALSE;
	fetch_view(state, node, item->mask, view);
	if (!is_live(node)) {
		release(view);
		return FALSE;
	}
	if (item->mask & TVIF_TEXT)
		copy_text_out(view->text, item->pszText, item->cchTextMax, wide);
	if (item->mask & TVIF_IMAGE)
		item->iImage = view->image;
	if (item->mask & TVIF_SELECTEDIMAGE)
		item->iSelectedImage = view->selected_image;
	if (item->mask & TVIF_CHILDREN)
		item->cChildren = view->children;
	if (item->mask & TVIF_PARAM)
		item->lParam = node->lparam;
	if (item->mask & TVIF_INTEGRAL)
		item->iIntegral = node->integral;
	if (item->mask & TVIF_STATEEX)
		item->uStateEx = node->state_ex;
	if (item->mask & TVIF_EXPANDEDIMAGE)
		item->iExpandedImage = node->expanded_image;
	/* The state comes back whether it was asked for or not. */
	item->state = view->state;
	release(view);
	return TRUE;
}

static LRESULT set_item(tree_state *state, const TVITEMEXW *item, BOOL wide)
{
	tree_node *node = own_node(state, item->hItem);

	if (!node)
		return FALSE;
	if ((item->mask & TVIF_TEXT) && !set_text(node, item->pszText, wide))
		return FALSE;
	if (item->mask & TVIF_IMAGE)
		node->image = item->iImage;
	if (item->mask & TVIF_SELECTEDIMAGE)
		node->selected_image = item->iSelectedImage;
	if (item->mask & TVIF_PARAM)
		node->lparam = item->lParam;
	if (item->mask & TVIF_CHILDREN) {
		node->children = item->cChildren;
		node->automatic_children = FALSE;
	}
	if (item->mask & TVIF_INTEGRAL)
		node->integral = item->iIntegral > 0 ? item->iIntegral : 1;
	if (item->mask & TVIF_STATEEX)
		node->state_ex = item->uStateEx;
	if (item->mask & TVIF_EXPANDEDIMAGE)
		node->expanded_image = item->iExpandedImage;
	if (item->mask & TVIF_STATE) {
		UINT old_bold = node->state & TVIS_BOLD;
		node->state = (node->state & ~item->stateMask) | (item->state & item->stateMask);
		if ((node->state & TVIS_BOLD) != old_bold)
			node->measured = FALSE;
	}
	reshape(state);
	return TRUE;
}

static LRESULT expand_message(tree_state *state, UINT action, tree_node *node)
{
	UINT code = action & TVE_ACTIONMASK;

	if (code == TVE_TOGGLE)
		code = (node->state & TVIS_EXPANDED) ? TVE_COLLAPSE : TVE_EXPAND;
	if (code == TVE_COLLAPSE) {
		if (!(node->state & TVIS_EXPANDED))
			return FALSE;
		node->state &= ~(TVIS_EXPANDED | TVIS_EXPANDPARTIAL);
		if (action & TVE_COLLAPSERESET) {
			delete_children(state, node);
			if (!is_live(node))
				return TRUE;
			node->state &= ~TVIS_EXPANDEDONCE;
		}
		reshape(state);
		after_collapse(state, node);
		return TRUE;
	}
	if (code != TVE_EXPAND || !has_children(state, node) || (node->state & TVIS_EXPANDED))
		return FALSE;
	if (!(node->state & TVIS_EXPANDEDONCE)) {
		if (notify_tree(state, NOTIFY_CODE(state, TVN_ITEMEXPANDING), TVE_EXPAND, NULL, 0, node, EXPAND_MASK) || !is_live(node))
			return FALSE;
	}
	if (!node->first_child)
		return FALSE;
	node->state |= TVIS_EXPANDED;
	if (action & TVE_EXPANDPARTIAL)
		node->state |= TVIS_EXPANDPARTIAL;
	reshape(state);
	if (!(node->state & TVIS_EXPANDEDONCE)) {
		notify_tree(state, NOTIFY_CODE(state, TVN_ITEMEXPANDED), TVE_EXPAND, NULL, 0, node, EXPAND_MASK);
		if (is_live(node))
			node->state |= TVIS_EXPANDEDONCE;
	}
	return TRUE;
}

static tree_node *next_row(tree_state *state, const tree_node *node, int delta)
{
	int index;
	if (!is_shown(state, node))
		return NULL;
	index = node->row_index + delta;
	return index >= 0 && index < state->row_count ? state->rows[index] : NULL;
}

static LRESULT next_item_message(tree_state *state, UINT relation, HTREEITEM handle)
{
	tree_node *node = own_node(state, handle);

	switch (relation) {
	case TVGN_ROOT:
		return (LRESULT)state->root.first_child;
	case TVGN_CARET:
		return (LRESULT)state->selected;
	case TVGN_DROPHILITE:
		return (LRESULT)state->drop;
	case TVGN_FIRSTVISIBLE:
		return (LRESULT)first_visible(state);
	case TVGN_LASTVISIBLE:
		ensure_layout(state);
		return (LRESULT)(state->row_count ? state->rows[state->row_count - 1] : NULL);
	case TVGN_CHILD:
		if (!handle || handle == TVI_ROOT)
			return (LRESULT)state->root.first_child;
		return node ? (LRESULT)node->first_child : 0;
	}
	if (!node)
		return 0;
	switch (relation) {
	case TVGN_NEXT:
		return (LRESULT)node->next;
	case TVGN_PREVIOUS:
		return (LRESULT)node->prev;
	case TVGN_PARENT:
		return node->parent == &state->root ? 0 : (LRESULT)node->parent;
	case TVGN_NEXTVISIBLE:
		return (LRESULT)next_row(state, node, 1);
	case TVGN_PREVIOUSVISIBLE:
		return (LRESULT)next_row(state, node, -1);
	}
	return 0;
}

static LRESULT select_message(tree_state *state, WPARAM relation, HTREEITEM handle)
{
	UINT code = (UINT)relation & ~TVSI_NOSINGLEEXPAND;
	tree_node *node = own_node(state, handle);

	if (handle && !node)
		return FALSE;
	switch (code) {
	case TVGN_CARET:
		return select_node(state, node, TVC_UNKNOWN, !(relation & TVSI_NOSINGLEEXPAND));
	case TVGN_DROPHILITE:
		if (state->drop)
			state->drop->state &= ~TVIS_DROPHILITED;
		state->drop = node;
		if (node)
			node->state |= TVIS_DROPHILITED;
		invalidate(state);
		return TRUE;
	case TVGN_FIRSTVISIBLE:
		if (!node)
			return FALSE;
		reveal_ancestors(state, node->parent);
		if (!is_live(node) || !is_shown(state, node))
			return FALSE;
		set_top(state, node->row_top);
		return TRUE;
	}
	return FALSE;
}

static void full_row_rect(const tree_state *state, const tree_node *node, RECT *rect)
{
	RECT client;
	GetClientRect(state->window, &client);
	rect->left = 0;
	rect->right = client.right;
	rect->top = row_y(state, node);
	rect->bottom = rect->top + node->integral * state->height;
}

static BOOL label_rect(tree_state *state, tree_node *node, RECT *rect)
{
	if (!is_shown(state, node))
		return FALSE;
	if (!node->measured) {
		HDC dc = GetDC(state->window);
		measure_node(state, node, dc);
		ReleaseDC(state->window, dc);
		if (!is_live(node))
			return FALSE;
	}
	full_row_rect(state, node, rect);
	rect->left = label_left(state, node);
	rect->right = rect->left + node->width;
	return TRUE;
}

static BOOL has_expander_column(const tree_state *state, const tree_node *node)
{
	return indent_columns(state, node) > 0;
}

static BOOL shows_button(tree_state *state, tree_node *node)
{
	return (style_of(state) & TVS_HASBUTTONS) && has_expander_column(state, node) && has_children(state, node);
}

static tree_node *hit_test(tree_state *state, POINT at, UINT *flags)
{
	RECT client, label;
	tree_node *node;
	int row, low, high, left;

	ensure_layout(state);
	GetClientRect(state->window, &client);
	*flags = 0;
	if (at.x < 0)
		*flags |= TVHT_TOLEFT;
	else if (at.x >= client.right)
		*flags |= TVHT_TORIGHT;
	if (at.y < 0)
		*flags |= TVHT_ABOVE;
	else if (at.y >= client.bottom)
		*flags |= TVHT_BELOW;
	if (*flags)
		return NULL;
	if (!state->row_count || state->height <= 0) {
		*flags = TVHT_NOWHERE;
		return NULL;
	}

	row = at.y / state->height + state->top;
	low = 0;
	high = state->row_count - 1;
	while (low < high) {
		int middle = (low + high + 1) / 2;
		if (state->rows[middle]->row_top <= row)
			low = middle;
		else
			high = middle - 1;
	}
	node = state->rows[low];
	if (row < node->row_top || row >= node->row_top + node->integral) {
		*flags = TVHT_NOWHERE;
		return NULL;
	}
	if (!label_rect(state, node, &label)) {
		*flags = TVHT_NOWHERE;
		return NULL;
	}

	left = content_left(state, node);
	if (at.x >= label.right)
		*flags = TVHT_ONITEMRIGHT;
	else if (at.x >= label.left)
		*flags = TVHT_ONITEMLABEL;
	else if (at.x >= left + state->state_cx)
		*flags = TVHT_ONITEMICON;
	else if (at.x >= left)
		*flags = TVHT_ONITEMSTATEICON;
	else if (at.x >= left - state->indent && shows_button(state, node))
		*flags = TVHT_ONITEMBUTTON;
	else
		*flags = TVHT_ONITEMINDENT;
	return node;
}

static void refresh_item_height(tree_state *state)
{
	if (state->custom_height > 0) {
		state->height = state->custom_height;
	} else {
		TEXTMETRICW metrics;
		HDC dc = GetDC(state->window);
		HGDIOBJ old = SelectObject(dc, state->font);
		int height;
		GetTextMetricsW(dc, &metrics);
		SelectObject(dc, old);
		ReleaseDC(state->window, dc);
		height = metrics.tmHeight + 3;
		if (state->normal_cy > height)
			height = state->normal_cy;
		if (state->state_cy > height)
			height = state->state_cy;
		if (!(style_of(state) & TVS_NONEVENHEIGHT))
			height &= ~1;
		state->height = height;
	}
	reshape(state);
}

static void destroy_derived_fonts(tree_state *state)
{
	if (state->bold_font)
		DeleteObject(state->bold_font);
	if (state->hot_font)
		DeleteObject(state->hot_font);
	state->bold_font = state->hot_font = NULL;
}

static void set_font(tree_state *state, HFONT font)
{
	LOGFONTW description;
	tree_node *walk;

	destroy_derived_fonts(state);
	state->font = font ? font : state->own_font;
	if (GetObjectW(state->font, sizeof(description), &description)) {
		LONG weight = description.lfWeight;
		description.lfWeight = FW_BOLD;
		state->bold_font = CreateFontIndirectW(&description);
		description.lfWeight = weight;
		description.lfUnderline = TRUE;
		state->hot_font = CreateFontIndirectW(&description);
	}
	for (walk = state->root.first_child; walk;) {
		walk->measured = FALSE;
		if (walk->first_child) {
			walk = walk->first_child;
			continue;
		}
		while (walk && !walk->next) {
			walk = walk->parent;
			if (walk == &state->root)
				walk = NULL;
		}
		if (walk)
			walk = walk->next;
	}
	refresh_item_height(state);
}

static void draw_checkbox_cell(tree_state *state, HDC dc, const RECT *cell, UINT frame)
{
	RECT box = *cell;
	HBRUSH brush = GetSysColorBrush(COLOR_WINDOW);
	HTHEME theme = OpenThemeData(state->window, L"BUTTON");

	FillRect(dc, cell, brush);
	InflateRect(&box, -1, -1);
	if (theme) {
		DrawThemeBackground(theme, dc, BP_CHECKBOX, (frame & DFCS_CHECKED) ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL, &box, NULL);
		CloseThemeData(theme);
	} else {
		DrawFrameControl(dc, &box, DFC_BUTTON, DFCS_BUTTONCHECK | frame);
	}
}

/* Index 0 of a state list stays empty, 1 is unchecked and 2 is checked. */
static void create_checkbox_list(tree_state *state)
{
	int size = GetSystemMetrics(SM_CXSMICON), index;
	HDC screen = GetDC(state->window), dc = CreateCompatibleDC(screen);
	HBITMAP bitmap = CreateCompatibleBitmap(screen, size, size);
	HIMAGELIST list = ImageList_Create(size, size, ILC_COLOR32, 3, 1);
	static const UINT frames[] = { 0, 0, DFCS_CHECKED };

	ReleaseDC(state->window, screen);
	for (index = 0; index < 3 && list; index++) {
		RECT cell = { 0, 0, size, size };
		HGDIOBJ old = SelectObject(dc, bitmap);
		if (index)
			draw_checkbox_cell(state, dc, &cell, frames[index]);
		else
			FillRect(dc, &cell, GetSysColorBrush(COLOR_WINDOW));
		SelectObject(dc, old);
		ImageList_Add(list, bitmap, NULL);
	}
	DeleteObject(bitmap);
	DeleteDC(dc);
	state->own_state_list = list;
	state->state_list = list;
	state->state_cx = state->state_cy = 0;
	if (list)
		ImageList_GetIconSize(list, &state->state_cx, &state->state_cy);
}

static void reset_all_checkboxes(tree_state *state)
{
	tree_node *walk;
	for (walk = state->root.first_child; walk;) {
		walk->state = (walk->state & ~TVIS_STATEIMAGEMASK) | INDEXTOSTATEIMAGEMASK(1);
		if (walk->first_child) {
			walk = walk->first_child;
			continue;
		}
		while (walk && !walk->next) {
			walk = walk->parent;
			if (walk == &state->root)
				walk = NULL;
		}
		if (walk)
			walk = walk->next;
	}
}

static void ensure_checkboxes(tree_state *state)
{
	if (!(style_of(state) & TVS_CHECKBOXES) || state->state_list)
		return;
	create_checkbox_list(state);
	reset_all_checkboxes(state);
	refresh_item_height(state);
}

static void cycle_checkbox(tree_state *state, tree_node *node)
{
	NMTVSTATEIMAGECHANGING change;
	int count = state->state_list ? ImageList_GetImageCount(state->state_list) : 0;
	int index = (node->state & TVIS_STATEIMAGEMASK) >> 12, next = index + 1;

	if (next >= count)
		next = 1;
	ZeroMemory(&change, sizeof(change));
	change.hti = (HTREEITEM)node;
	change.iOldStateImageIndex = index;
	change.iNewStateImageIndex = next;
	if (notify(state, &change.hdr, NM_TVSTATEIMAGECHANGING) || !is_live(node))
		return;
	node->state = (node->state & ~TVIS_STATEIMAGEMASK) | INDEXTOSTATEIMAGEMASK(next);
	invalidate(state);
}

static COLORREF window_color(const tree_state *state)
{
	if (!IsWindowEnabled(state->window))
		return GetSysColor(COLOR_BTNFACE);
	return state->background == CLR_NONE ? GetSysColor(COLOR_WINDOW) : state->background;
}

static void fill_color(HDC dc, const RECT *rect, COLORREF color)
{
	COLORREF old = SetBkColor(dc, color);
	ExtTextOutW(dc, 0, 0, ETO_OPAQUE, rect, NULL, 0, NULL);
	SetBkColor(dc, old);
}

static void dotted_line(HDC dc, int x1, int y1, int x2, int y2)
{
	MoveToEx(dc, x1, y1, NULL);
	LineTo(dc, x2, y2);
}

static void draw_lines(tree_state *state, HDC dc, tree_node *node, const RECT *row)
{
	int middle = (row->top + row->bottom) / 2, x = content_left(state, node) - state->indent / 2;
	const tree_node *walk;
	int column;

	if (!has_expander_column(state, node))
		return;
	dotted_line(dc, x, middle, x + state->indent / 2, middle);
	if (node->next || node->prev || node->parent != &state->root || (style_of(state) & TVS_LINESATROOT)) {
		int top = node->prev || node->parent != &state->root ? row->top : middle;
		dotted_line(dc, x, top, x, node->next ? row->bottom : middle + 1);
	}
	column = x;
	for (walk = node->parent; walk && walk != &state->root; walk = walk->parent) {
		column -= state->indent;
		if (walk->next && column >= -state->indent)
			dotted_line(dc, column, row->top, column, row->bottom);
		if (!((style_of(state) & TVS_LINESATROOT)) && walk->parent == &state->root)
			break;
	}
}

static void draw_button(tree_state *state, HDC dc, tree_node *node, const RECT *row)
{
	int x = content_left(state, node) - state->indent / 2, y = (row->top + row->bottom) / 2;
	BOOL open = (node->state & TVIS_EXPANDED) != 0;
	RECT box;

	box.left = x - GLYPH_BOX / 2;
	box.top = y - GLYPH_BOX / 2;
	box.right = box.left + GLYPH_BOX;
	box.bottom = box.top + GLYPH_BOX;
	if (state->theme && state->background == CLR_NONE && IsThemePartDefined(state->theme, TVP_GLYPH, 0)) {
		SIZE size;
		if (SUCCEEDED(GetThemePartSize(state->theme, dc, TVP_GLYPH, open ? GLPS_OPENED : GLPS_CLOSED, NULL, TS_DRAW, &size)) && size.cx > 0) {
			box.left = x - size.cx / 2;
			box.top = y - size.cy / 2;
			box.right = box.left + size.cx;
			box.bottom = box.top + size.cy;
		}
		fill_color(dc, &box, window_color(state));
		DrawThemeBackground(state->theme, dc, TVP_GLYPH, open ? GLPS_OPENED : GLPS_CLOSED, &box, NULL);
		return;
	}
	{
		RECT dash;
		COLORREF frame = GetSysColor(COLOR_BTNSHADOW), ink = state->foreground == CLR_NONE ? GetSysColor(COLOR_WINDOWTEXT) : state->foreground;
		fill_color(dc, &box, frame);
		InflateRect(&box, -1, -1);
		fill_color(dc, &box, window_color(state));
		SetRect(&dash, box.left + 1, y, box.right - 1, y + 1);
		fill_color(dc, &dash, ink);
		if (!open) {
			SetRect(&dash, x, box.top + 1, x + 1, box.bottom - 1);
			fill_color(dc, &dash, ink);
		}
	}
}

static LRESULT custom_draw(tree_state *state, NMTVCUSTOMDRAW *draw, DWORD stage)
{
	draw->nmcd.dwDrawStage = stage;
	return notify(state, &draw->nmcd.hdr, NM_CUSTOMDRAW);
}

static void draw_item(tree_state *state, HDC dc, tree_node *node, const RECT *clip, BOOL item_notify)
{
	NMTVCUSTOMDRAW draw;
	item_view *view;
	RECT row, text_rect;
	BOOL full_row = (style_of(state) & TVS_FULLROWSELECT) && !(style_of(state) & TVS_HASLINES);
	BOOL marked, dropped, active;
	COLORREF saved_text, saved_back, text, back;
	HGDIOBJ saved_font;
	int left, x, y, index;
	LRESULT answer = CDRF_DODEFAULT;

	if (!label_rect(state, node, &text_rect))
		return;
	full_row_rect(state, node, &row);
	if (row.bottom <= clip->top || row.top >= clip->bottom)
		return;
	view = allocate(sizeof(*view));
	if (!view)
		return;
	fetch_view(state, node, TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE, view);
	if (!is_live(node)) {
		release(view);
		return;
	}

	dropped = (node->state & TVIS_DROPHILITED) != 0;
	active = state->focused || (style_of(state) & TVS_SHOWSELALWAYS);
	marked = dropped || ((view->state & TVIS_SELECTED) && active);
	if (marked && (state->focused || dropped)) {
		text = GetSysColor(COLOR_HIGHLIGHTTEXT);
		back = GetSysColor(COLOR_HIGHLIGHT);
	} else if (marked) {
		text = GetSysColor(COLOR_BTNTEXT);
		back = GetSysColor(COLOR_BTNFACE);
	} else {
		text = state->foreground == CLR_NONE ? GetSysColor(COLOR_WINDOWTEXT) : state->foreground;
		back = window_color(state);
	}
	if (!IsWindowEnabled(state->window))
		text = GetSysColor(COLOR_GRAYTEXT);
	if (node == state->hot && (style_of(state) & TVS_TRACKSELECT) && !marked)
		text = GetSysColor(COLOR_HOTLIGHT);

	saved_font = SelectObject(dc, label_font(state, node));
	saved_text = SetTextColor(dc, text);
	saved_back = SetBkColor(dc, back);
	if (item_notify) {
		ZeroMemory(&draw, sizeof(draw));
		draw.nmcd.hdc = dc;
		draw.nmcd.rc = row;
		draw.nmcd.dwItemSpec = (DWORD_PTR)node;
		draw.nmcd.lItemlParam = node->lparam;
		if (view->state & TVIS_SELECTED)
			draw.nmcd.uItemState |= CDIS_SELECTED;
		if (state->focused && node == state->selected)
			draw.nmcd.uItemState |= CDIS_FOCUS;
		if (node == state->hot)
			draw.nmcd.uItemState |= CDIS_HOT;
		if (!IsWindowEnabled(state->window))
			draw.nmcd.uItemState |= CDIS_DISABLED;
		draw.clrText = text;
		draw.clrTextBk = back;
		draw.iLevel = node->level;
		answer = custom_draw(state, &draw, CDDS_ITEMPREPAINT);
		if (!is_live(node)) {
			release(view);
			return;
		}
		text = draw.clrText;
		back = draw.clrTextBk;
	}

	if (!(answer & CDRF_SKIPDEFAULT)) {
		HPEN pen = CreatePen(PS_DOT, 1, state->line_color == CLR_DEFAULT ? GetSysColor(COLOR_GRAYTEXT) : state->line_color);
		HGDIOBJ old_pen = SelectObject(dc, pen);
		int old_mode = SetBkMode(dc, TRANSPARENT);

		if (style_of(state) & TVS_HASLINES)
			draw_lines(state, dc, node, &row);
		SetBkMode(dc, old_mode);
		SelectObject(dc, old_pen);
		DeleteObject(pen);
		if (shows_button(state, node))
			draw_button(state, dc, node, &row);

		left = content_left(state, node);
		y = (row.top + row.bottom) / 2;
		index = (view->state & TVIS_STATEIMAGEMASK) >> 12;
		if (state->state_list && index > 0)
			ImageList_Draw(state->state_list, index, dc, left, y - state->state_cy / 2, ILD_NORMAL);
		x = left + state->state_cx;
		if (state->normal_list) {
			int image = (view->state & TVIS_SELECTED) ? view->selected_image : view->image;
			UINT flags = ILD_TRANSPARENT | (view->state & TVIS_OVERLAYMASK);
			if ((node->state & TVIS_EXPANDED) && node->expanded_image != NO_EXPANDED_IMAGE && node->expanded_image >= 0)
				image = node->expanded_image;
			if (node->state & TVIS_CUT)
				flags |= ILD_BLEND50;
			if (image >= 0)
				ImageList_Draw(state->normal_list, image, dc, x, y - state->normal_cy / 2, flags);
		}

		if (marked && full_row)
			fill_color(dc, &row, back);
		else if (marked || back != window_color(state))
			fill_color(dc, &text_rect, back);
		{
			RECT inner = text_rect;
			inner.left += LABEL_PADDING;
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, text);
			DrawTextW(dc, view->text, -1, &inner, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
		if (state->focused && node == state->selected && !(SendMessageW(state->window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
			SetTextColor(dc, RGB(0, 0, 0));
			SetBkColor(dc, RGB(255, 255, 255));
			DrawFocusRect(dc, full_row ? &row : &text_rect);
		}
		if (node == state->mark) {
			RECT bar = text_rect;
			COLORREF color = state->mark_color == CLR_DEFAULT ? GetSysColor(COLOR_WINDOWTEXT) : state->mark_color;
			bar.top = state->mark_after ? text_rect.bottom - 2 : text_rect.top;
			bar.bottom = bar.top + 2;
			fill_color(dc, &bar, color);
		}
	}

	/* Colors and font go back before the post paint stage sees the DC. */
	SelectObject(dc, saved_font);
	SetTextColor(dc, saved_text);
	SetBkColor(dc, saved_back);
	if (item_notify && (answer & CDRF_NOTIFYPOSTPAINT))
		custom_draw(state, &draw, CDDS_ITEMPOSTPAINT);
	release(view);
}

static void paint(tree_state *state, HDC dc, const RECT *clip)
{
	NMTVCUSTOMDRAW draw;
	LRESULT answer;
	RECT client;
	tree_node *node;
	int index;
	HGDIOBJ old_font;

	GetClientRect(state->window, &client);
	fill_color(dc, clip, window_color(state));
	old_font = SelectObject(dc, state->font);
	ZeroMemory(&draw, sizeof(draw));
	draw.nmcd.hdc = dc;
	draw.nmcd.rc = client;
	answer = custom_draw(state, &draw, CDDS_PREPAINT);
	if (!(answer & CDRF_SKIPDEFAULT)) {
		node = first_visible(state);
		for (index = node ? node->row_index : state->row_count; index < state->row_count; index++) {
			node = state->rows[index];
			if (row_y(state, node) >= clip->bottom)
				break;
			draw_item(state, dc, node, clip, (answer & CDRF_NOTIFYITEMDRAW) != 0);
			if (state->dead || state->dirty)
				break;
		}
	}
	if (!state->dead && (answer & CDRF_NOTIFYPOSTPAINT)) {
		ZeroMemory(&draw, sizeof(draw));
		draw.nmcd.hdc = dc;
		draw.nmcd.rc = client;
		custom_draw(state, &draw, CDDS_POSTPAINT);
	}
	SelectObject(dc, old_font);
}

static void paint_window(tree_state *state)
{
	PAINTSTRUCT painting;
	HDC dc, buffer;
	HBITMAP bitmap;
	RECT client;

	ensure_checkboxes(state);
	ensure_layout(state);
	dc = BeginPaint(state->window, &painting);
	GetClientRect(state->window, &client);
	buffer = CreateCompatibleDC(dc);
	bitmap = buffer ? CreateCompatibleBitmap(dc, client.right, client.bottom) : NULL;
	if (bitmap) {
		HGDIOBJ old = SelectObject(buffer, bitmap);
		paint(state, buffer, &painting.rcPaint);
		if (!state->dead)
			BitBlt(dc, painting.rcPaint.left, painting.rcPaint.top, painting.rcPaint.right - painting.rcPaint.left, painting.rcPaint.bottom - painting.rcPaint.top, buffer, painting.rcPaint.left, painting.rcPaint.top, SRCCOPY);
		SelectObject(buffer, old);
		DeleteObject(bitmap);
	} else {
		paint(state, dc, &painting.rcPaint);
	}
	if (buffer)
		DeleteDC(buffer);
	EndPaint(state->window, &painting);
}

static void resize_edit(tree_state *state)
{
	WCHAR text[EDIT_LIMIT];
	SIZE size = { 0, 0 };
	RECT rect, client;
	HDC dc;
	HGDIOBJ old;
	int width;

	if (!state->edit || !state->edited || !label_rect(state, state->edited, &rect))
		return;
	GetWindowTextW(state->edit, text, EDIT_LIMIT);
	GetClientRect(state->window, &client);
	dc = GetDC(state->edit);
	old = SelectObject(dc, label_font(state, state->edited));
	GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
	SelectObject(dc, old);
	ReleaseDC(state->edit, dc);
	width = size.cx + 2 * LABEL_PADDING + 8;
	if (rect.left + width > client.right)
		width = client.right - rect.left;
	if (width < 16)
		width = 16;
	SetWindowPos(state->edit, HWND_TOP, rect.left, rect.top, width, rect.bottom - rect.top, SWP_NOACTIVATE);
}

static void move_edit(tree_state *state)
{
	if (state->edit)
		resize_edit(state);
}

static LRESULT CALLBACK edit_subclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	tree_state *state = (tree_state *)GetWindowLongPtrW(window, GWLP_USERDATA);
	WNDPROC original = state->edit_proc;

	switch (message) {
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
	case WM_KEYDOWN:
		if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
			SendMessageW(state->window, TVM_ENDEDITLABELNOW, wparam == VK_ESCAPE, 0);
			return 0;
		}
		break;
	case WM_CHAR:
		if (wparam == VK_RETURN || wparam == VK_ESCAPE)
			return 0;
		break;
	}
	return CallWindowProcW(original, window, message, wparam, lparam);
}

/* Quiet ends are for an item about to vanish: nobody is asked about it. */
static void end_edit(tree_state *state, BOOL cancel, BOOL quiet)
{
	HWND edit = state->edit;
	tree_node *node = state->edited;
	WCHAR text[EDIT_LIMIT];

	if (!edit)
		return;
	state->edit = NULL;
	state->edited = NULL;
	GetWindowTextW(edit, text, EDIT_LIMIT);
	if (!quiet && node) {
		NMTVDISPINFOW info;
		char raw[EDIT_LIMIT * sizeof(WCHAR)];
		LRESULT accepted;

		ZeroMemory(&info, sizeof(info));
		info.item.mask = TVIF_HANDLE | TVIF_PARAM | (cancel ? 0 : TVIF_TEXT);
		info.item.hItem = (HTREEITEM)node;
		info.item.state = node->state;
		info.item.lParam = node->lparam;
		info.item.cchTextMax = cancel ? 0 : EDIT_LIMIT;
		if (!cancel) {
			if (state->unicode)
				CopyMemory(raw, text, sizeof(text));
			else if (!WideCharToMultiByte(CP_ACP, 0, text, -1, raw, EDIT_LIMIT, NULL, NULL))
				raw[EDIT_LIMIT - 1] = 0;
			info.item.pszText = (WCHAR *)raw;
		}
		accepted = notify(state, &info.hdr, NOTIFY_CODE(state, TVN_ENDLABELEDIT));
		if (!cancel && accepted && is_live(node) && info.item.pszText) {
			if (node->callback_text) {
				info.item.mask = TVIF_TEXT;
				notify(state, &info.hdr, NOTIFY_CODE(state, TVN_SETDISPINFO));
			} else {
				WCHAR *copy;
				if (state->unicode) {
					lstrcpynW(text, info.item.pszText, EDIT_LIMIT);
				} else {
					MultiByteToWideChar(CP_ACP, 0, (const char *)info.item.pszText, -1, text, EDIT_LIMIT);
					text[EDIT_LIMIT - 1] = 0;
				}
				copy = copy_wide(text);
				if (copy) {
					release(node->text);
					node->text = copy;
					node->measured = FALSE;
					reshape(state);
				}
			}
		}
	}
	if (state->dead)
		return;
	if (GetFocus() == edit)
		SetFocus(state->window);
	SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)state->edit_proc);
	DestroyWindow(edit);
	invalidate(state);
}

static HWND begin_edit(tree_state *state, tree_node *node)
{
	NMTVDISPINFOW info;
	item_view *view;
	HWND edit;
	char raw[TEXT_LIMIT * sizeof(WCHAR)];
	LRESULT refused;

	if (state->edit)
		end_edit(state, FALSE, FALSE);
	if (!is_live(node) || !is_shown(state, node))
		return NULL;
	scroll_into_view(state, node);
	view = allocate(sizeof(*view));
	if (!view)
		return NULL;
	fetch_view(state, node, TVIF_TEXT, view);
	if (!is_live(node)) {
		release(view);
		return NULL;
	}
	edit = CreateWindowExW(WS_EX_LEFT, WC_EDITW, view->text, WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_LEFT | WS_CLIPSIBLINGS, 0, 0, 16, 16, state->window, NULL, (HINSTANCE)GetWindowLongPtrW(state->window, GWLP_HINSTANCE), NULL);
	if (!edit) {
		release(view);
		return NULL;
	}
	SendMessageW(edit, WM_SETFONT, (WPARAM)label_font(state, node), FALSE);
	SendMessageW(edit, EM_LIMITTEXT, EDIT_LIMIT - 1, 0);
	SetWindowLongPtrW(edit, GWLP_USERDATA, (LONG_PTR)state);
	state->edit_proc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
	state->edit = edit;
	state->edited = node;

	ZeroMemory(&info, sizeof(info));
	info.item.mask = TVIF_HANDLE | TVIF_STATE | TVIF_PARAM | TVIF_TEXT;
	info.item.hItem = (HTREEITEM)node;
	info.item.state = node->state;
	info.item.lParam = node->lparam;
	info.item.cchTextMax = TEXT_LIMIT;
	if (state->unicode)
		lstrcpynW((WCHAR *)raw, view->text, TEXT_LIMIT);
	else if (!WideCharToMultiByte(CP_ACP, 0, view->text, -1, raw, TEXT_LIMIT * sizeof(WCHAR), NULL, NULL))
		raw[0] = 0;
	info.item.pszText = (WCHAR *)raw;
	release(view);
	refused = notify(state, &info.hdr, NOTIFY_CODE(state, TVN_BEGINLABELEDIT));
	if (state->dead)
		return NULL;
	if (refused || !is_live(node)) {
		end_edit(state, TRUE, TRUE);
		return NULL;
	}
	resize_edit(state);
	SendMessageW(edit, EM_SETSEL, 0, -1);
	ShowWindow(edit, SW_SHOW);
	SetFocus(edit);
	return edit;
}

typedef struct {
	tree_state *state;
	PFNTVCOMPARE compare;
	LPARAM sort_param;
	item_view *left, *right;
} sort_context;

static int compare_nodes(sort_context *context, tree_node *a, tree_node *b)
{
	if (context->compare)
		return context->compare(a->lparam, b->lparam, context->sort_param);
	fetch_view(context->state, a, TVIF_TEXT, context->left);
	fetch_view(context->state, b, TVIF_TEXT, context->right);
	return lstrcmpiW(context->left->text, context->right->text);
}

/* Merge sort, because equal labels must keep their order. */
static void merge_sort(sort_context *context, tree_node **items, tree_node **scratch, int count)
{
	int middle = count / 2, left = 0, right = middle, out = 0;
	if (count < 2)
		return;
	merge_sort(context, items, scratch, middle);
	merge_sort(context, items + middle, scratch, count - middle);
	while (left < middle && right < count) {
		if (compare_nodes(context, items[left], items[right]) <= 0)
			scratch[out++] = items[left++];
		else
			scratch[out++] = items[right++];
	}
	while (left < middle)
		scratch[out++] = items[left++];
	while (right < count)
		scratch[out++] = items[right++];
	CopyMemory(items, scratch, count * sizeof(*items));
}

static LRESULT sort_children(tree_state *state, HTREEITEM handle, PFNTVCOMPARE compare, LPARAM sort_param)
{
	tree_node *parent, *walk;
	tree_node **items;
	sort_context context;
	int count = 0, index;

	if (!handle || handle == TVI_ROOT)
		parent = &state->root;
	else if (!(parent = own_node(state, handle)))
		return FALSE;
	for (walk = parent->first_child; walk; walk = walk->next)
		count++;
	if (!count)
		return FALSE;
	items = allocate(2 * count * sizeof(*items));
	context.left = allocate(sizeof(item_view));
	context.right = allocate(sizeof(item_view));
	if (!items || !context.left || !context.right) {
		release(items);
		release(context.left);
		release(context.right);
		return FALSE;
	}
	context.state = state;
	context.compare = compare;
	context.sort_param = sort_param;
	for (index = 0, walk = parent->first_child; walk; walk = walk->next)
		items[index++] = walk;
	merge_sort(&context, items, items + count, count);
	parent->first_child = items[0];
	parent->last_child = items[count - 1];
	for (index = 0; index < count; index++) {
		items[index]->prev = index ? items[index - 1] : NULL;
		items[index]->next = index + 1 < count ? items[index + 1] : NULL;
	}
	release(items);
	release(context.left);
	release(context.right);
	reshape(state);
	return TRUE;
}

static HIMAGELIST create_drag_image(tree_state *state, tree_node *node)
{
	RECT label;
	HDC screen, dc;
	HBITMAP bitmap;
	HGDIOBJ old_bitmap, old_font;
	HIMAGELIST list;
	item_view *view;
	RECT area;
	int width;

	if (!label_rect(state, node, &label))
		return NULL;
	view = allocate(sizeof(*view));
	if (!view)
		return NULL;
	fetch_view(state, node, TVIF_TEXT | TVIF_IMAGE, view);
	width = state->normal_cx + (label.right - label.left);
	screen = GetDC(state->window);
	dc = CreateCompatibleDC(screen);
	bitmap = CreateCompatibleBitmap(screen, width, state->height);
	ReleaseDC(state->window, screen);
	old_bitmap = SelectObject(dc, bitmap);
	old_font = SelectObject(dc, label_font(state, node));
	SetRect(&area, 0, 0, width, state->height);
	fill_color(dc, &area, window_color(state));
	if (state->normal_list && view->image >= 0)
		ImageList_Draw(state->normal_list, view->image, dc, 0, 0, ILD_TRANSPARENT);
	area.left = state->normal_cx + LABEL_PADDING;
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
	DrawTextW(dc, view->text, -1, &area, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(dc, old_font);
	SelectObject(dc, old_bitmap);
	DeleteDC(dc);
	list = ImageList_Create(width, state->height, ILC_COLOR32, 1, 1);
	if (list)
		ImageList_Add(list, bitmap, NULL);
	DeleteObject(bitmap);
	release(view);
	return list;
}

static void create_own_tooltips(tree_state *state)
{
	TTTOOLINFOW tool;

	state->tooltips = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, state->window, NULL, (HINSTANCE)GetWindowLongPtrW(state->window, GWLP_HINSTANCE), NULL);
	if (!state->tooltips)
		return;
	ZeroMemory(&tool, sizeof(tool));
	tool.cbSize = sizeof(tool);
	tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
	tool.hwnd = state->window;
	tool.uId = (UINT_PTR)state->window;
	tool.lpszText = LPSTR_TEXTCALLBACKW;
	SendMessageW(state->tooltips, TTM_ADDTOOLW, 0, (LPARAM)&tool);
}

static void answer_tooltip(tree_state *state, NMHDR *header)
{
	static WCHAR text[TEXT_LIMIT];
	tree_node *node = state->tip;
	item_view *view;
	RECT label, client;

	text[0] = 0;
	if (node && (view = allocate(sizeof(*view)))) {
		GetClientRect(state->window, &client);
		if (style_of(state) & TVS_INFOTIP) {
			NMTVGETINFOTIPW info;
			char raw[TEXT_LIMIT * sizeof(WCHAR)];
			ZeroMemory(&info, sizeof(info));
			raw[0] = raw[1] = 0;
			info.pszText = (WCHAR *)raw;
			info.cchTextMax = TEXT_LIMIT;
			info.hItem = (HTREEITEM)node;
			info.lParam = node->lparam;
			notify(state, &info.hdr, NOTIFY_CODE(state, TVN_GETINFOTIP));
			if (state->unicode)
				lstrcpynW(text, (WCHAR *)raw, TEXT_LIMIT);
			else
				MultiByteToWideChar(CP_ACP, 0, raw, -1, text, TEXT_LIMIT);
		} else if (is_live(node) && label_rect(state, node, &label) && label.right > client.right) {
			fetch_view(state, node, TVIF_TEXT, view);
			lstrcpynW(text, view->text, TEXT_LIMIT);
		}
		release(view);
	}
	if (header->code == TTN_GETDISPINFOW) {
		NMTTDISPINFOW *info = (NMTTDISPINFOW *)header;
		info->lpszText = text;
		info->hinst = NULL;
	} else {
		static char narrow[TEXT_LIMIT];
		NMTTDISPINFOA *info = (NMTTDISPINFOA *)header;
		if (!WideCharToMultiByte(CP_ACP, 0, text, -1, narrow, TEXT_LIMIT, NULL, NULL))
			narrow[TEXT_LIMIT - 1] = 0;
		info->lpszText = narrow;
		info->hinst = NULL;
	}
}

static void track_hot(tree_state *state, tree_node *node)
{
	if (node != state->tip) {
		state->tip = node;
		if (state->tooltips)
			SendMessageW(state->tooltips, TTM_POP, 0, 0);
	}
	if (node == state->hot)
		return;
	state->hot = node;
	if (style_of(state) & TVS_TRACKSELECT)
		invalidate(state);
	if (node) {
		TRACKMOUSEEVENT track;
		track.cbSize = sizeof(track);
		track.dwFlags = TME_LEAVE;
		track.hwndTrack = state->window;
		track.dwHoverTime = 0;
		TrackMouseEvent(&track);
	}
}

static void send_simple(tree_state *state, UINT code, LRESULT *result)
{
	NMHDR header;
	LRESULT answer = notify(state, &header, code);
	if (result)
		*result = answer;
}

static void left_down(tree_state *state, POINT at)
{
	UINT flags;
	tree_node *node, *was_selected = state->selected;
	BOOL full_row = (style_of(state) & TVS_FULLROWSELECT) != 0;

	if (state->edit)
		end_edit(state, FALSE, FALSE);
	if (state->dead)
		return;
	KillTimer(state->window, EDIT_TIMER);
	state->editable = NULL;
	SetFocus(state->window);
	node = hit_test(state, at, &flags);
	if (!node) {
		state->gesture = GESTURE_LEFT;
		state->press = NULL;
		state->press_at = at;
		SetCapture(state->window);
		return;
	}
	if (flags & TVHT_ONITEMBUTTON) {
		if (node->state & TVIS_EXPANDED)
			user_collapse(state, node);
		else
			user_expand(state, node);
		return;
	}
	if ((flags & TVHT_ONITEMSTATEICON) && state->state_list) {
		cycle_checkbox(state, node);
		return;
	}
	if ((flags & TVHT_ONITEMINDENT) && !full_row) {
		state->press = NULL;
	} else if ((flags & TVHT_ONITEMRIGHT) && !full_row) {
		state->press = NULL;
	} else {
		state->press = node;
		if ((flags & TVHT_ONITEMLABEL) && node == was_selected && (style_of(state) & TVS_EDITLABELS))
			state->editable = node;
		if (node == was_selected && (style_of(state) & TVS_SINGLEEXPAND)) {
			if (node->state & TVIS_EXPANDED)
				user_collapse(state, node);
			else
				user_expand(state, node);
		} else {
			select_node(state, node, TVC_BYMOUSE, TRUE);
		}
		if (state->dead)
			return;
		if (!is_live(node))
			state->press = NULL;
	}
	state->gesture = GESTURE_LEFT;
	state->press_at = at;
	SetCapture(state->window);
}

static void begin_drag_if_moved(tree_state *state, POINT at)
{
	tree_node *node = state->press;
	int gesture = state->gesture;
	int dx = at.x - state->press_at.x, dy = at.y - state->press_at.y;

	if (dx < 0)
		dx = -dx;
	if (dy < 0)
		dy = -dy;
	if (!node || (dx <= GetSystemMetrics(SM_CXDRAG) && dy <= GetSystemMetrics(SM_CYDRAG)))
		return;
	state->gesture = GESTURE_NONE;
	state->press = NULL;
	state->editable = NULL;
	if (GetCapture() == state->window)
		ReleaseCapture();
	if (style_of(state) & TVS_DISABLEDRAGDROP)
		return;
	if (gesture == GESTURE_LEFT)
		notify_tree(state, NOTIFY_CODE(state, TVN_BEGINDRAG), 0, NULL, 0, node, TVIF_HANDLE | TVIF_STATE | TVIF_PARAM);
	else
		notify_tree(state, NOTIFY_CODE(state, TVN_BEGINRDRAG), 0, NULL, 0, node, TVIF_HANDLE | TVIF_STATE | TVIF_PARAM);
}

static void left_up(tree_state *state)
{
	if (state->gesture != GESTURE_LEFT)
		return;
	state->gesture = GESTURE_NONE;
	if (GetCapture() == state->window)
		ReleaseCapture();
	send_simple(state, NM_CLICK, NULL);
	if (state->dead)
		return;
	if (state->editable)
		SetTimer(state->window, EDIT_TIMER, GetDoubleClickTime(), NULL);
}

static void left_double(tree_state *state, POINT at)
{
	UINT flags;
	tree_node *node = hit_test(state, at, &flags);
	LRESULT answer;

	KillTimer(state->window, EDIT_TIMER);
	state->editable = NULL;
	send_simple(state, NM_DBLCLK, &answer);
	if (state->dead || answer || !node || !is_live(node))
		return;
	if (flags & (TVHT_ONITEMLABEL | TVHT_ONITEMICON | TVHT_ONITEMBUTTON)) {
		if (node->state & TVIS_EXPANDED)
			user_collapse(state, node);
		else
			user_expand(state, node);
	}
}

static void right_down(tree_state *state, POINT at)
{
	UINT flags;
	tree_node *node;

	if (state->edit)
		end_edit(state, FALSE, FALSE);
	if (state->dead)
		return;
	SetFocus(state->window);
	node = hit_test(state, at, &flags);
	state->press = node && (flags & TVHT_ONITEM) ? node : NULL;
	state->gesture = GESTURE_RIGHT;
	state->press_at = at;
	SetCapture(state->window);
}

static void right_up(tree_state *state, LPARAM lparam)
{
	LRESULT answer;
	POINT at;

	if (state->gesture != GESTURE_RIGHT)
		return;
	state->gesture = GESTURE_NONE;
	state->press = NULL;
	if (GetCapture() == state->window)
		ReleaseCapture();
	send_simple(state, NM_RCLICK, &answer);
	if (state->dead || answer)
		return;
	at.x = (short)LOWORD(lparam);
	at.y = (short)HIWORD(lparam);
	ClientToScreen(state->window, &at);
	SendMessageW(state->window, WM_CONTEXTMENU, (WPARAM)state->window, MAKELPARAM(at.x, at.y));
}

static void expand_all(tree_state *state, tree_node *node)
{
	tree_node *child;
	user_expand(state, node);
	if (!is_live(node))
		return;
	for (child = node->first_child; child; child = child->next)
		expand_all(state, child);
}

static void key_down(tree_state *state, WPARAM key, LPARAM lparam)
{
	NMTVKEYDOWN info;
	tree_node *current, *target = NULL;
	BOOL control = GetKeyState(VK_CONTROL) < 0;

	ZeroMemory(&info, sizeof(info));
	info.wVKey = (WORD)key;
	info.flags = (UINT)lparam;
	notify(state, &info.hdr, TVN_KEYDOWN);
	if (state->dead)
		return;
	ensure_layout(state);
	current = state->selected;

	if (control) {
		switch (key) {
		case VK_UP:
			set_top(state, state->top - 1);
			return;
		case VK_DOWN:
			set_top(state, state->top + 1);
			return;
		case VK_PRIOR:
			set_top(state, state->top - page_rows(state));
			return;
		case VK_NEXT:
			set_top(state, state->top + page_rows(state));
			return;
		case VK_HOME:
			set_top(state, 0);
			return;
		case VK_END:
			set_top(state, state->total_rows);
			return;
		}
	}

	switch (key) {
	case VK_RETURN:
		send_simple(state, NM_RETURN, NULL);
		return;
	case VK_UP:
		target = current ? next_row(state, current, -1) : first_visible(state);
		break;
	case VK_DOWN:
		target = current ? next_row(state, current, 1) : first_visible(state);
		break;
	case VK_HOME:
		target = state->row_count ? state->rows[0] : NULL;
		break;
	case VK_END:
		target = state->row_count ? state->rows[state->row_count - 1] : NULL;
		break;
	case VK_PRIOR:
	case VK_NEXT:
		if (current && is_shown(state, current)) {
			int index = current->row_index + (key == VK_NEXT ? page_rows(state) : -page_rows(state));
			if (index < 0)
				index = 0;
			if (index >= state->row_count)
				index = state->row_count - 1;
			target = state->rows[index];
		}
		break;
	case VK_LEFT:
		if (!current)
			break;
		if ((current->state & TVIS_EXPANDED) && current->first_child)
			user_collapse(state, current);
		else if (current->parent != &state->root)
			target = current->parent;
		break;
	case VK_BACK:
		if (current && current->parent != &state->root)
			target = current->parent;
		break;
	case VK_RIGHT:
		if (!current)
			break;
		if (!(current->state & TVIS_EXPANDED))
			user_expand(state, current);
		else
			target = current->first_child;
		break;
	case VK_ADD:
		if (current)
			user_expand(state, current);
		break;
	case VK_SUBTRACT:
		if (current)
			user_collapse(state, current);
		break;
	case VK_MULTIPLY:
		if (current)
			expand_all(state, current);
		break;
	case VK_SPACE:
		if (current && (style_of(state) & TVS_CHECKBOXES) && state->state_list)
			cycle_checkbox(state, current);
		return;
	default:
		return;
	}
	if (target)
		select_node(state, target, TVC_BYKEYBOARD, TRUE);
}

static void search_char(tree_state *state, WCHAR letter)
{
	DWORD now = GetTickCount();
	item_view *view;
	int index, start, step;

	if (letter < 32 || (letter == ' ' && (style_of(state) & TVS_CHECKBOXES)))
		return;
	if (now - state->search_time > SEARCH_RESET_MS)
		state->search_length = 0;
	state->search_time = now;
	if (state->search_length >= (int)(sizeof(state->search) / sizeof(WCHAR)) - 1)
		return;
	state->search[state->search_length++] = letter;
	state->search[state->search_length] = 0;
	ensure_layout(state);
	if (!state->row_count || !(view = allocate(sizeof(*view))))
		return;
	start = state->selected && is_shown(state, state->selected) ? state->selected->row_index : 0;
	if (state->search_length == 1)
		start++;
	for (step = 0; step < state->row_count; step++) {
		index = (start + step) % state->row_count;
		fetch_view(state, state->rows[index], TVIF_TEXT, view);
		if (state->dead || state->dirty)
			break;
		if (lstrlenW(view->text) >= state->search_length && CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, view->text, state->search_length, state->search, state->search_length) == CSTR_EQUAL) {
			tree_node *found = state->rows[index];
			release(view);
			select_node(state, found, TVC_BYKEYBOARD, TRUE);
			return;
		}
	}
	release(view);
}

static void scroll_message(tree_state *state, int bar, UINT code)
{
	SCROLLINFO info;
	RECT client;

	ensure_layout(state);
	GetClientRect(state->window, &client);
	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	info.fMask = SIF_TRACKPOS;
	GetScrollInfo(state->window, bar, &info);
	if (bar == SB_VERT) {
		int top = state->top, page = page_rows(state);
		switch (code) {
		case SB_LINEUP: top--; break;
		case SB_LINEDOWN: top++; break;
		case SB_PAGEUP: top -= page; break;
		case SB_PAGEDOWN: top += page; break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: top = info.nTrackPos; break;
		case SB_TOP: top = 0; break;
		case SB_BOTTOM: top = state->total_rows; break;
		default: return;
		}
		set_top(state, top);
	} else {
		int x = state->scroll_x;
		switch (code) {
		case SB_LINELEFT: x -= state->indent; break;
		case SB_LINERIGHT: x += state->indent; break;
		case SB_PAGELEFT: x -= client.right; break;
		case SB_PAGERIGHT: x += client.right; break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: x = info.nTrackPos; break;
		case SB_LEFT: x = 0; break;
		case SB_RIGHT: x = state->content_width; break;
		default: return;
		}
		state->scroll_x = x < 0 ? 0 : x;
		update_scrollbars(state);
		move_edit(state);
		invalidate(state);
	}
}

static void style_changed(tree_state *state, DWORD old_style, DWORD new_style)
{
	DWORD changed = old_style ^ new_style;

	if (changed & TVS_NOTOOLTIPS) {
		if (new_style & TVS_NOTOOLTIPS) {
			if (state->tooltips)
				DestroyWindow(state->tooltips);
			state->tooltips = NULL;
		} else {
			create_own_tooltips(state);
		}
	}
	if ((changed & TVS_CHECKBOXES) && (new_style & TVS_CHECKBOXES))
		ensure_checkboxes(state);
	if (changed & TVS_NONEVENHEIGHT)
		refresh_item_height(state);
	state->last_style = new_style;
	reshape(state);
}

static LRESULT set_image_list(tree_state *state, WPARAM which, HIMAGELIST list)
{
	HIMAGELIST old;

	if (which == TVSIL_NORMAL) {
		old = state->normal_list;
		state->normal_list = list;
		state->normal_cx = state->normal_cy = 0;
		if (list)
			ImageList_GetIconSize(list, &state->normal_cx, &state->normal_cy);
	} else if (which == TVSIL_STATE) {
		old = state->state_list;
		state->state_list = list;
		state->state_cx = state->state_cy = 0;
		if (list)
			ImageList_GetIconSize(list, &state->state_cx, &state->state_cy);
	} else {
		return 0;
	}
	refresh_item_height(state);
	return (LRESULT)old;
}

static LRESULT create_state(HWND window, const CREATESTRUCTW *create)
{
	tree_state *state = allocate(sizeof(*state));
	LOGFONTW description;

	if (!state)
		return FALSE;
	SetWindowLongPtrW(window, 0, (LONG_PTR)state);
	state->window = window;
	state->parent = create->hwndParent;
	state->root.state = TVIS_EXPANDED;
	state->root.level = -1;
	state->redraw = TRUE;
	state->indent = DEFAULT_INDENT;
	state->background = CLR_NONE;
	state->foreground = CLR_NONE;
	state->line_color = CLR_DEFAULT;
	state->mark_color = CLR_DEFAULT;
	state->scroll_time = 500;
	state->last_style = create->style;
	if (SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(description), &description, 0))
		state->own_font = CreateFontIndirectW(&description);
	if (!state->own_font)
		state->own_font = GetStockObject(DEFAULT_GUI_FONT);
	state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
	return TRUE;
}

static void destroy_resources(tree_state *state)
{
	if (state->edit)
		end_edit(state, TRUE, TRUE);
	delete_all(state);
	if (state->tooltips)
		DestroyWindow(state->tooltips);
	state->tooltips = NULL;
	if (state->own_state_list && state->state_list == state->own_state_list)
		ImageList_Destroy(state->own_state_list);
	state->own_state_list = state->state_list = NULL;
	if (state->theme)
		CloseThemeData(state->theme);
	state->theme = NULL;
	destroy_derived_fonts(state);
	if (state->own_font)
		DeleteObject(state->own_font);
	state->own_font = NULL;
	release(state->rows);
	state->rows = NULL;
	state->row_count = state->row_capacity = 0;
}

static LRESULT tree_message(tree_state *state, UINT message, WPARAM wparam, LPARAM lparam)
{
	HWND window = state->window;
	POINT at;

	at.x = (short)LOWORD(lparam);
	at.y = (short)HIWORD(lparam);
	switch (message) {
	case WM_CREATE:
		state->theme = OpenThemeData(window, L"TREEVIEW");
		set_font(state, NULL);
		if (!(state->last_style & TVS_NOTOOLTIPS))
			create_own_tooltips(state);
		return 0;
	case WM_DESTROY:
		destroy_resources(state);
		return 0;
	case WM_PAINT:
		if (wparam) {
			RECT client;
			ensure_checkboxes(state);
			ensure_layout(state);
			GetClientRect(window, &client);
			paint(state, (HDC)wparam, &client);
		} else {
			paint_window(state);
		}
		return 0;
	case WM_PRINTCLIENT: {
		RECT client;
		ensure_checkboxes(state);
		ensure_layout(state);
		GetClientRect(window, &client);
		paint(state, (HDC)wparam, &client);
		return 0;
	}
	case WM_ERASEBKGND:
		return TRUE;
	case WM_SIZE:
		state->dirty = TRUE;
		if (!state->in_layout)
			ensure_layout(state);
		invalidate(state);
		return 0;
	case WM_SETREDRAW:
		state->redraw = wparam != 0;
		if (state->redraw) {
			state->dirty = TRUE;
			ensure_layout(state);
			InvalidateRect(window, NULL, FALSE);
		}
		return 0;
	case WM_SETFONT:
		set_font(state, (HFONT)wparam);
		if (LOWORD(lparam))
			InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_GETFONT:
		return (LRESULT)state->font;
	case WM_GETDLGCODE:
		return DLGC_WANTCHARS | DLGC_WANTARROWS;
	case WM_NOTIFYFORMAT:
		if (lparam == NF_REQUERY)
			state->unicode = SendMessageW(state->parent, WM_NOTIFYFORMAT, (WPARAM)window, NF_QUERY) == NFR_UNICODE;
		return state->unicode ? NFR_UNICODE : NFR_ANSI;
	case WM_SETFOCUS:
		state->focused = TRUE;
		invalidate(state);
		send_simple(state, NM_SETFOCUS, NULL);
		return 0;
	case WM_KILLFOCUS:
		state->focused = FALSE;
		invalidate(state);
		send_simple(state, NM_KILLFOCUS, NULL);
		return 0;
	case WM_ENABLE:
	case WM_SYSCOLORCHANGE:
		InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_THEMECHANGED:
		if (state->theme)
			CloseThemeData(state->theme);
		state->theme = OpenThemeData(window, L"TREEVIEW");
		InvalidateRect(window, NULL, FALSE);
		return 0;
	case WM_STYLECHANGED:
		if (wparam == GWL_STYLE)
			style_changed(state, ((const STYLESTRUCT *)lparam)->styleOld, ((const STYLESTRUCT *)lparam)->styleNew);
		return 0;
	case WM_VSCROLL:
		scroll_message(state, SB_VERT, LOWORD(wparam));
		return 0;
	case WM_HSCROLL:
		scroll_message(state, SB_HORZ, LOWORD(wparam));
		return 0;
	case WM_MOUSEWHEEL: {
		UINT lines = 3;
		int notches;
		SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
		state->wheel_rest += (short)HIWORD(wparam);
		notches = state->wheel_rest / WHEEL_DELTA;
		state->wheel_rest -= notches * WHEEL_DELTA;
		if (notches) {
			ensure_layout(state);
			set_top(state, state->top - notches * (int)lines);
		}
		return 0;
	}
	case WM_KEYDOWN:
		key_down(state, wparam, lparam);
		return 0;
	case WM_CHAR:
		search_char(state, (WCHAR)wparam);
		return 0;
	case WM_LBUTTONDOWN:
		left_down(state, at);
		return 0;
	case WM_LBUTTONUP:
		left_up(state);
		return 0;
	case WM_LBUTTONDBLCLK:
		left_double(state, at);
		return 0;
	case WM_RBUTTONDOWN:
		right_down(state, at);
		return 0;
	case WM_RBUTTONUP:
		right_up(state, lparam);
		return 0;
	case WM_RBUTTONDBLCLK:
		send_simple(state, NM_RDBLCLK, NULL);
		return 0;
	case WM_MOUSEMOVE: {
		UINT flags;
		tree_node *node;
		if (state->gesture != GESTURE_NONE) {
			begin_drag_if_moved(state, at);
			return 0;
		}
		node = hit_test(state, at, &flags);
		track_hot(state, node && (flags & TVHT_ONITEM) ? node : NULL);
		return 0;
	}
	case WM_MOUSELEAVE:
		track_hot(state, NULL);
		return 0;
	case WM_CAPTURECHANGED:
		if ((HWND)lparam != window) {
			state->gesture = GESTURE_NONE;
			state->press = NULL;
		}
		return 0;
	case WM_SETCURSOR:
		if ((HWND)wparam == window && (style_of(state) & TVS_TRACKSELECT) && state->hot) {
			NMMOUSE mouse;
			ZeroMemory(&mouse, sizeof(mouse));
			if (!notify(state, &mouse.hdr, NM_SETCURSOR) && !state->dead)
				SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
			return TRUE;
		}
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_TIMER:
		if (wparam == EDIT_TIMER) {
			tree_node *node = state->editable;
			KillTimer(window, EDIT_TIMER);
			state->editable = NULL;
			if (node && node == state->selected)
				begin_edit(state, node);
		}
		return 0;
	case WM_COMMAND:
		if (!state->edit || (HWND)lparam != state->edit)
			return 0;
		if (HIWORD(wparam) == EN_KILLFOCUS)
			end_edit(state, FALSE, FALSE);
		else if (HIWORD(wparam) == EN_UPDATE)
			resize_edit(state);
		return 0;
	case WM_CTLCOLOREDIT:
		return SendMessageW(state->parent, message, wparam, lparam);
	case WM_NOTIFY:
		if (((NMHDR *)lparam)->code == TTN_GETDISPINFOW || ((NMHDR *)lparam)->code == TTN_GETDISPINFOA)
			answer_tooltip(state, (NMHDR *)lparam);
		return 0;

	case TVM_INSERTITEMA:
	case TVM_INSERTITEMW:
		return (LRESULT)insert_item(state, (const TVINSERTSTRUCTW *)lparam, message == TVM_INSERTITEMW);
	case TVM_DELETEITEM:
		if (!lparam || (HTREEITEM)lparam == TVI_ROOT) {
			delete_all(state);
			return TRUE;
		} else {
			tree_node *node = own_node(state, (HTREEITEM)lparam);
			return node ? delete_item(state, node) : FALSE;
		}
	case TVM_EXPAND: {
		tree_node *node = own_node(state, (HTREEITEM)lparam);
		return node ? expand_message(state, (UINT)wparam, node) : FALSE;
	}
	case TVM_GETITEMRECT: {
		RECT *rect = (RECT *)lparam;
		tree_node *node = rect ? own_node(state, *(HTREEITEM *)rect) : NULL;
		RECT result;
		if (!node || !is_shown(state, node))
			return FALSE;
		if (wparam) {
			if (!label_rect(state, node, &result))
				return FALSE;
		} else {
			full_row_rect(state, node, &result);
		}
		*rect = result;
		return TRUE;
	}
	case TVM_GETCOUNT:
		return state->count;
	case TVM_GETINDENT:
		return state->indent;
	case TVM_SETINDENT:
		state->indent = (int)wparam < MINIMUM_INDENT ? MINIMUM_INDENT : (int)wparam;
		reshape(state);
		return 0;
	case TVM_GETIMAGELIST:
		if (wparam == TVSIL_NORMAL)
			return (LRESULT)state->normal_list;
		return wparam == TVSIL_STATE ? (LRESULT)state->state_list : 0;
	case TVM_SETIMAGELIST:
		return set_image_list(state, wparam, (HIMAGELIST)lparam);
	case TVM_GETNEXTITEM:
		return next_item_message(state, (UINT)wparam, (HTREEITEM)lparam);
	case TVM_SELECTITEM:
		return select_message(state, wparam, (HTREEITEM)lparam);
	case TVM_GETITEMA:
	case TVM_GETITEMW:
		return get_item(state, (TVITEMEXW *)lparam, message == TVM_GETITEMW);
	case TVM_SETITEMA:
	case TVM_SETITEMW:
		return set_item(state, (const TVITEMEXW *)lparam, message == TVM_SETITEMW);
	case TVM_GETITEMSTATE: {
		tree_node *node = any_node((HTREEITEM)wparam);
		return node ? node->state & (UINT)lparam : 0;
	}
	case TVM_EDITLABELA:
	case TVM_EDITLABELW: {
		tree_node *node = own_node(state, (HTREEITEM)lparam);
		return node ? (LRESULT)begin_edit(state, node) : 0;
	}
	case TVM_GETEDITCONTROL:
		return (LRESULT)state->edit;
	case TVM_ENDEDITLABELNOW:
		if (!state->edit)
			return FALSE;
		end_edit(state, wparam != 0, FALSE);
		return TRUE;
	case TVM_GETVISIBLECOUNT:
		return page_rows(state);
	case TVM_HITTEST: {
		TVHITTESTINFO *info = (TVHITTESTINFO *)lparam;
		if (!info)
			return 0;
		info->hItem = (HTREEITEM)hit_test(state, info->pt, &info->flags);
		return (LRESULT)info->hItem;
	}
	case TVM_CREATEDRAGIMAGE: {
		tree_node *node = own_node(state, (HTREEITEM)lparam);
		return node ? (LRESULT)create_drag_image(state, node) : 0;
	}
	case TVM_SORTCHILDREN:
		return sort_children(state, (HTREEITEM)lparam, NULL, 0);
	case TVM_SORTCHILDRENCB: {
		const TVSORTCB *sort = (const TVSORTCB *)lparam;
		return sort && sort->lpfnCompare ? sort_children(state, sort->hParent, sort->lpfnCompare, sort->lParam) : FALSE;
	}
	case TVM_ENSUREVISIBLE: {
		tree_node *node = own_node(state, (HTREEITEM)lparam), *walk;
		BOOL expanded = FALSE;
		if (!node)
			return FALSE;
		for (walk = node->parent; walk && walk != &state->root; walk = walk->parent)
			if (!(walk->state & TVIS_EXPANDED))
				expanded = TRUE;
		reveal_ancestors(state, node->parent);
		if (!is_live(node))
			return FALSE;
		return scroll_into_view(state, node) && !expanded;
	}
	case TVM_GETISEARCHSTRINGW:
		if (lparam)
			lstrcpyW((WCHAR *)lparam, state->search);
		return state->search_length;
	case TVM_GETISEARCHSTRINGA:
		if (lparam)
			WideCharToMultiByte(CP_ACP, 0, state->search, -1, (char *)lparam, sizeof(state->search), NULL, NULL);
		return state->search_length;
	case TVM_GETTOOLTIPS:
		return (LRESULT)state->tooltips;
	case TVM_SETTOOLTIPS: {
		HWND old = state->tooltips;
		state->tooltips = (HWND)wparam;
		return (LRESULT)old;
	}
	case TVM_SETINSERTMARK: {
		tree_node *node = own_node(state, (HTREEITEM)lparam);
		if (lparam && !node)
			return FALSE;
		state->mark = node;
		state->mark_after = wparam != 0;
		invalidate(state);
		return TRUE;
	}
	case TVM_GETUNICODEFORMAT:
		return state->unicode;
	case TVM_SETUNICODEFORMAT: {
		BOOL old = state->unicode;
		state->unicode = wparam != 0;
		return old;
	}
	case TVM_GETITEMHEIGHT:
		return state->height;
	case TVM_SETITEMHEIGHT: {
		int old = state->height, height = (short)wparam;
		if (height == -1) {
			state->custom_height = 0;
		} else {
			if (height < 1)
				height = 1;
			if (!(style_of(state) & TVS_NONEVENHEIGHT))
				height &= ~1;
			state->custom_height = height > 0 ? height : 1;
		}
		refresh_item_height(state);
		return old;
	}
	case TVM_GETBKCOLOR:
		return state->background;
	case TVM_SETBKCOLOR: {
		COLORREF old = state->background;
		state->background = (COLORREF)lparam;
		InvalidateRect(window, NULL, FALSE);
		return old;
	}
	case TVM_GETTEXTCOLOR:
		return state->foreground;
	case TVM_SETTEXTCOLOR: {
		COLORREF old = state->foreground;
		state->foreground = (COLORREF)lparam;
		InvalidateRect(window, NULL, FALSE);
		return old;
	}
	case TVM_GETSCROLLTIME:
		return state->scroll_time;
	case TVM_SETSCROLLTIME: {
		int old = state->scroll_time;
		state->scroll_time = (int)wparam;
		return old;
	}
	case TVM_GETINSERTMARKCOLOR:
		return state->mark_color;
	case TVM_SETINSERTMARKCOLOR: {
		COLORREF old = state->mark_color;
		state->mark_color = (COLORREF)lparam;
		invalidate(state);
		return old;
	}
	case TVM_GETLINECOLOR:
		return state->line_color;
	case TVM_SETLINECOLOR: {
		COLORREF old = state->line_color;
		state->line_color = (COLORREF)lparam;
		invalidate(state);
		return old;
	}
	case TVM_MAPACCIDTOHTREEITEM: {
		tree_node *walk;
		for (walk = state->root.first_child; walk;) {
			if (walk->accessibility_id == (UINT)wparam)
				return (LRESULT)walk;
			if (walk->first_child) {
				walk = walk->first_child;
				continue;
			}
			while (walk && !walk->next) {
				walk = walk->parent;
				if (walk == &state->root)
					walk = NULL;
			}
			if (walk)
				walk = walk->next;
		}
		return 0;
	}
	case TVM_MAPHTREEITEMTOACCID: {
		tree_node *node = own_node(state, (HTREEITEM)wparam);
		return node ? node->accessibility_id : 0;
	}
	case TVM_GETEXTENDEDSTYLE:
		return state->extended_style;
	case TVM_SETEXTENDEDSTYLE: {
		DWORD old = state->extended_style;
		state->extended_style = (old & ~(DWORD)wparam) | ((DWORD)lparam & (DWORD)wparam);
		invalidate(state);
		return old;
	}
	case TVM_SETAUTOSCROLLINFO:
		return TRUE;
	case TVM_GETSELECTEDCOUNT: {
		tree_node *walk;
		int count = 0;
		for (walk = state->root.first_child; walk;) {
			if (walk->state & TVIS_SELECTED)
				count++;
			if (walk->first_child) {
				walk = walk->first_child;
				continue;
			}
			while (walk && !walk->next) {
				walk = walk->parent;
				if (walk == &state->root)
					walk = NULL;
			}
			if (walk)
				walk = walk->next;
		}
		return count;
	}
	case TVM_SHOWINFOTIP: {
		tree_node *node = own_node(state, (HTREEITEM)lparam);
		if (node) {
			state->tip = node;
			if (state->tooltips)
				SendMessageW(state->tooltips, TTM_POPUP, 0, 0);
		}
		return 0;
	}
	case TVM_GETITEMPARTRECT: {
		const TVGETITEMPARTRECTINFO *info = (const TVGETITEMPARTRECTINFO *)lparam;
		tree_node *node = info ? own_node(state, info->hti) : NULL;
		if (!node || info->partID != TVGIPR_BUTTON || !is_shown(state, node) || !shows_button(state, node))
			return FALSE;
		full_row_rect(state, node, info->prc);
		info->prc->right = content_left(state, node);
		info->prc->left = info->prc->right - state->indent;
		return TRUE;
	}
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

/* A notification can destroy the tree while a message is still running, so the state lives until the outermost message returns. */
static LRESULT CALLBACK tree_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	tree_state *state = state_of(window);
	LRESULT result;

	if (message == WM_NCCREATE)
		return create_state(window, (const CREATESTRUCTW *)lparam) ? DefWindowProcW(window, message, wparam, lparam) : FALSE;
	if (!state)
		return DefWindowProcW(window, message, wparam, lparam);
	state->busy++;
	if (message == WM_NCDESTROY) {
		SetWindowLongPtrW(window, 0, 0);
		state->dead = TRUE;
		result = DefWindowProcW(window, message, wparam, lparam);
	} else {
		result = tree_message(state, message, wparam, lparam);
	}
	if (!--state->busy && state->dead)
		release(state);
	return result;
}

void TREEVIEW_Register(void)
{
	WNDCLASSW info;

	if (!live_lock_ready) {
		InitializeCriticalSection(&live_lock);
		live_lock_ready = TRUE;
	}
	ZeroMemory(&info, sizeof(info));
	info.style = CS_GLOBALCLASS | CS_DBLCLKS;
	info.lpfnWndProc = tree_proc;
	info.cbWndExtra = sizeof(tree_state *);
	info.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	info.lpszClassName = WC_TREEVIEWW;
	RegisterClassW(&info);
}

void TREEVIEW_Unregister(void)
{
	UnregisterClassW(WC_TREEVIEWW, NULL);
}
