// The installer's own dialogs, as the package describes them in its Dialog, Control, ControlEvent, ControlCondition and EventMapping
// tables. The user interface sequence runs with them: a dialog stands for its action, a modal one until a control ends it, a modeless
// one, the progress dialog, while the installation runs behind it.
#include "msi_own.h"
#include <commctrl.h>
#include <richedit.h>

enum {
	CONTROL_VISIBLE = 0x1, CONTROL_ENABLED = 0x2, CONTROL_SUNKEN = 0x4, CONTROL_INDIRECT = 0x8,
	TEXT_TRANSPARENT = 0x10000, TEXT_NOPREFIX = 0x20000, TEXT_NOWRAP = 0x40000,
	EDIT_MULTILINE = 0x10000, EDIT_PASSWORD = 0x200000, COMBO_LIST = 0x20000, PUSH_LIKE = 0x20000, GROUP_BORDER = 0x1000000,
	DIALOG_VISIBLE = 0x1, DIALOG_MODAL = 0x2, DIALOG_MINIMIZE = 0x4, DIALOG_KEEP_MODELESS = 0x10,
};

typedef enum { KIND_OTHER, KIND_TEXT, KIND_PUSH, KIND_CHECK, KIND_EDIT, KIND_PATH, KIND_LINE, KIND_GROUP, KIND_BITMAP, KIND_SCROLL,
	KIND_PROGRESS, KIND_RADIO, KIND_COMBO, KIND_LIST, KIND_LINK } control_kind;

static const struct { const WCHAR *name; control_kind kind; } control_kinds[] = {
	{ L"Text", KIND_TEXT }, { L"PushButton", KIND_PUSH }, { L"CheckBox", KIND_CHECK }, { L"Edit", KIND_EDIT }, { L"MaskedEdit", KIND_EDIT },
	{ L"PathEdit", KIND_PATH }, { L"Line", KIND_LINE }, { L"GroupBox", KIND_GROUP }, { L"Bitmap", KIND_BITMAP },
	{ L"ScrollableText", KIND_SCROLL }, { L"ProgressBar", KIND_PROGRESS }, { L"RadioButtonGroup", KIND_RADIO },
	{ L"ComboBox", KIND_COMBO }, { L"ListBox", KIND_LIST }, { L"Hyperlink", KIND_LINK },
};

#define CONTROL_ID 100
#define RADIO_ID 0x4000

typedef struct {
	WCHAR *name, *property, *checked, *next;	// checked: what a check box puts in its property
	control_kind kind;
	INT attributes;
	RECT box;
	HWND window;
	HWND *buttons;		// a radio button group's buttons and the values they stand for
	WCHAR **values;
	UINT button_count;
	BITMAPINFO *image;	// a bitmap the dialog paints itself, so transparent text can stand on it
	const BYTE *bits;
	msi_blob *blob;
	BOOL hidden, progress, action_text, action_data;
} ui_control;

typedef struct ui_dialog ui_dialog;
typedef struct ui_state ui_state;

struct ui_dialog {
	msi_package *package;
	ui_state *state;
	WCHAR *name, *first, *default_control, *cancel_control, *next;
	INT attributes;
	HWND window;
	ui_control *controls;
	UINT control_count;
	msi_property *saved;	// the bound properties as the dialog found them, for Reset
	UINT saved_count;
	BOOL done, updating;
	UINT result;
};

typedef struct { WCHAR *name; HFONT font; COLORREF color; BOOL colored; } ui_style;

struct ui_state {
	const WCHAR *execute;
	ui_dialog *modeless;
	ui_style *styles;
	UINT style_count;
	HFONT font;
	BOOL reduced, cancelled, executing;
};

static int units(int value)
{
	// An installer unit is a twelfth of the height of 10-point MS Sans Serif, 16 pixels on a 96 DPI screen.
	return MulDiv(value, 4, 3);
}

// The leading decimal number of a text, as an event argument or the {80} limit of an edit gives it.
static INT leading_number(const WCHAR *text)
{
	INT value = 0, sign = 1;
	if (*text == '-')
		sign = -1, text++;
	while (*text >= '0' && *text <= '9')
		value = value * 10 + (*text++ - '0');
	return value * sign;
}

static msi_record *params_new(const WCHAR *a, const WCHAR *b)
{
	msi_record *params = record_new(b ? 2 : 1);
	if (params) {
		record_set_text(params, 1, a);
		if (b)
			record_set_text(params, 2, b);
	}
	return params;
}

typedef struct { WCHAR *text; msi_blob *blob; } first_field;

static UINT keep_first(msi_record *row, void *context)
{
	first_field *field = context;
	WCHAR scratch[16];
	if (row->count >= 1 && row->fields[1].type == FIELD_BLOB && row->fields[1].blob)
		field->blob = blob_ref(row->fields[1].blob);
	else {
		const WCHAR *text = record_text(row, 1, scratch);
		field->text = text ? msi_copy(text) : NULL;
	}
	return ERROR_NO_MORE_ITEMS;
}

static first_field query_first(msi_package *package, const WCHAR *query, const WCHAR *a, const WCHAR *b)
{
	first_field field = {0};
	msi_record *params = params_new(a, b);
	db_query_each(package->db, query, params, keep_first, &field);
	if (params)
		object_unref(&params->base);
	return field;
}

static BOOL table_has_rows(msi_package *package, const WCHAR *query)
{
	BOOL found = FALSE;
	first_field field = {0};
	if (db_query_each(package->db, query, NULL, keep_first, &field) == ERROR_NO_MORE_ITEMS)
		found = TRUE;
	msi_free(field.text);
	if (field.blob)
		blob_unref(field.blob);
	return found;
}

typedef struct { WCHAR *face; INT size, color, bits; BOOL found; } style_row;

static UINT read_style(msi_record *record, void *context)
{
	style_row *out = context;
	WCHAR scratch[16];
	const WCHAR *face = record_text(record, 1, scratch);
	out->face = msi_copy(face ? face : L"MS Sans Serif");
	out->size = record_get_int(record, 2);
	out->color = record_get_int(record, 3);
	out->bits = record_get_int(record, 4);
	out->found = TRUE;
	return ERROR_NO_MORE_ITEMS;
}

static const ui_style *style_find(ui_state *state, msi_package *package, const WCHAR *name)
{
	for (UINT i = 0; i < state->style_count; i++)
		if (same_text(state->styles[i].name, name))
			return &state->styles[i];
	style_row row = {0};
	msi_record *params = params_new(name, NULL);
	db_query_each(package->db, L"SELECT `FaceName`, `Size`, `Color`, `StyleBits` FROM `TextStyle` WHERE `TextStyle` = ?", params, read_style, &row);
	if (params)
		object_unref(&params->base);
	if (!row.found)
		return NULL;
	const INT bits = row.bits == MSI_NULL_INTEGER ? 0 : row.bits;
	HFONT font = CreateFontW(-MulDiv(row.size == MSI_NULL_INTEGER ? 8 : row.size, 96, 72), 0, 0, 0, bits & 1 ? FW_BOLD : FW_NORMAL,
		(bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, row.face);
	msi_free(row.face);
	state->styles = msi_grow(state->styles, (state->style_count + 1) * sizeof(ui_style));
	ui_style *style = &state->styles[state->style_count++];
	style->name = msi_copy(name);
	style->font = font;
	style->colored = row.color != MSI_NULL_INTEGER;
	style->color = style->colored ? (COLORREF)row.color : 0;
	return style;
}

// A control's text may start with {\Style} or {&Style}: the text style it is drawn in. The rest is formatted text.
static WCHAR *control_text(ui_dialog *dialog, const WCHAR *text, const ui_style **style)
{
	*style = NULL;
	while (text && text[0] == '{' && (text[1] == '\\' || text[1] == '&')) {
		const WCHAR *end = text + 2;
		while (*end && *end != '}')
			end++;
		if (!*end)
			break;
		WCHAR *name = msi_copy_n(text + 2, end - text - 2);
		*style = style_find(dialog->state, dialog->package, name);
		msi_free(name);
		text = end + 1;
	}
	return format_text(dialog->package, text ? text : L"", NULL);
}

// Indirect controls name, in their Property column, the property that holds the name of the one they show.
static WCHAR *bound_property(ui_dialog *dialog, const ui_control *control)
{
	if (!control->property || !control->property[0])
		return NULL;
	if (control->attributes & CONTROL_INDIRECT)
		return package_get(dialog->package, control->property);
	return msi_copy(control->property);
}

static WCHAR *bound_value(ui_dialog *dialog, const ui_control *control)
{
	WCHAR *name = bound_property(dialog, control), *value = name ? package_get(dialog->package, name) : NULL;
	msi_free(name);
	return value;
}

static void bound_set(ui_dialog *dialog, const ui_control *control, const WCHAR *value)
{
	WCHAR *name = bound_property(dialog, control);
	if (name)
		package_set(dialog->package, name, value && value[0] ? value : NULL);
	msi_free(name);
}

static WCHAR *window_text(HWND window)
{
	int length = GetWindowTextLengthW(window);
	WCHAR *text = msi_alloc((length + 1) * sizeof(WCHAR));
	GetWindowTextW(window, text, length + 1);
	return text;
}

// A transparent text shows the dialog through it, so a new text needs the dialog's background painted again under it.
static void text_set(ui_dialog *dialog, ui_control *control, const WCHAR *text)
{
	SetWindowTextW(control->window, text);
	if (control->attributes & TEXT_TRANSPARENT)
		RedrawWindow(dialog->window, &control->box, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// A hyperlink's text marks its links with <a> tags; without a link control the words show as plain text.
static void link_text(WCHAR *text)
{
	WCHAR *to = text;
	for (const WCHAR *at = text; *at;) {
		if (*at == '<' && (at[1] == 'a' || at[1] == 'A' || at[1] == '/')) {
			while (*at && *at != '>')
				at++;
			if (*at)
				at++;
		} else
			*to++ = *at++;
	}
	*to = 0;
}

// The controls show what their properties hold now.
static void controls_refresh(ui_dialog *dialog)
{
	dialog->updating = TRUE;
	for (UINT i = 0; i < dialog->control_count; i++) {
		ui_control *control = &dialog->controls[i];
		WCHAR *value = bound_value(dialog, control);
		switch (control->kind) {
		case KIND_CHECK:
			SendMessageW(control->window, BM_SETCHECK, value && value[0] ? BST_CHECKED : BST_UNCHECKED, 0);
			break;
		case KIND_EDIT:
		case KIND_PATH:
			if (control->property) {
				WCHAR *shown = window_text(control->window);
				if (lstrcmpW(shown, value ? value : L""))
					SetWindowTextW(control->window, value ? value : L"");
				msi_free(shown);
			}
			break;
		case KIND_RADIO:
			for (UINT b = 0; b < control->button_count; b++)
				SendMessageW(control->buttons[b], BM_SETCHECK, value && same_text(value, control->values[b]) ? BST_CHECKED : BST_UNCHECKED, 0);
			break;
		case KIND_COMBO:
		case KIND_LIST: {
			const UINT find = control->kind == KIND_COMBO ? CB_GETCOUNT : LB_GETCOUNT;
			const LRESULT count = SendMessageW(control->window, find, 0, 0);
			LRESULT selected = -1;
			for (LRESULT item = 0; item < count && value; item++) {
				const WCHAR *stands = (const WCHAR *)SendMessageW(control->window, control->kind == KIND_COMBO ? CB_GETITEMDATA : LB_GETITEMDATA, item, 0);
				if (stands && same_text(stands, value))
					selected = item;
			}
			SendMessageW(control->window, control->kind == KIND_COMBO ? CB_SETCURSEL : LB_SETCURSEL, selected, 0);
			if (selected < 0 && control->kind == KIND_COMBO && value)
				SetWindowTextW(control->window, value);
			break;
		}
		default:
			break;
		}
		msi_free(value);
	}
	dialog->updating = FALSE;
}

static ui_control *control_find(ui_dialog *dialog, const WCHAR *name)
{
	for (UINT i = 0; name && i < dialog->control_count; i++)
		if (same_text(dialog->controls[i].name, name))
			return &dialog->controls[i];
	return NULL;
}

static void control_show(ui_dialog *dialog, ui_control *control, BOOL shown)
{
	control->hidden = !shown;
	if (control->window)
		ShowWindow(control->window, shown ? SW_SHOWNA : SW_HIDE);
	for (UINT b = 0; b < control->button_count; b++)
		ShowWindow(control->buttons[b], shown ? SW_SHOWNA : SW_HIDE);
	if (control->kind == KIND_BITMAP)
		InvalidateRect(dialog->window, &control->box, TRUE);
}

static void control_enable(ui_control *control, BOOL enabled)
{
	if (control->window)
		EnableWindow(control->window, enabled);
	for (UINT b = 0; b < control->button_count; b++)
		EnableWindow(control->buttons[b], enabled);
}

static UINT apply_condition(msi_record *row, void *context)
{
	ui_dialog *dialog = context;
	WCHAR a[16], b[16], c[16];
	ui_control *control = control_find(dialog, record_text(row, 1, a));
	const WCHAR *action = record_text(row, 2, b), *condition = record_text(row, 3, c);
	if (!control || !action || !condition_true(dialog->package, condition))
		return ERROR_SUCCESS;
	if (same_text(action, L"Hide"))
		control_show(dialog, control, FALSE);
	else if (same_text(action, L"Show"))
		control_show(dialog, control, TRUE);
	else if (same_text(action, L"Disable"))
		control_enable(control, FALSE);
	else if (same_text(action, L"Enable"))
		control_enable(control, TRUE);
	else if (same_text(action, L"Default")) {
		msi_free(dialog->default_control);
		dialog->default_control = msi_copy(control->name);
	}
	return ERROR_SUCCESS;
}

// ControlCondition rows hide, show, enable and disable controls as the properties change.
static void conditions_apply(ui_dialog *dialog)
{
	msi_record *params = params_new(dialog->name, NULL);
	db_query_each(dialog->package->db, L"SELECT `Control_`, `Action`, `Condition` FROM `ControlCondition` WHERE `Dialog_` = ?", params,
		apply_condition, dialog);
	if (params)
		object_unref(&params->base);
	for (UINT i = 0; i < dialog->control_count; i++)
		if (dialog->controls[i].kind == KIND_PUSH)
			SendMessageW(dialog->controls[i].window, BM_SETSTYLE,
				same_text(dialog->controls[i].name, dialog->default_control) ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
}

static UINT dialog_run(ui_state *state, msi_package *package, const WCHAR *name, ui_dialog *parent);

static void features_set(msi_package *package, const WCHAR *argument, INSTALLSTATE state)
{
	for (UINT i = 0; i < package->feature_count; i++)
		if (same_text(argument, L"ALL") || same_text(argument, package->features[i].key))
			MsiSetFeatureStateW(package->session, package->features[i].key, state);
}

typedef struct { ui_dialog *dialog; WCHAR *spawn; } event_run;

// One published event. NewDialog and SpawnDialog wait for the others, as the dialog changes only once they have all run.
static UINT publish(msi_record *row, void *context)
{
	event_run *run = context;
	ui_dialog *dialog = run->dialog;
	msi_package *package = dialog->package;
	WCHAR a[16], b[16], c[16];
	const WCHAR *event = record_text(row, 1, a), *raw = record_text(row, 2, b), *condition = record_text(row, 3, c);
	if (!event || !condition_true(package, condition))
		return ERROR_SUCCESS;
	WCHAR *argument = format_text(package, raw ? raw : L"", NULL);
	package_log(L"Dialog %s: event %s, %s", dialog->name, event, argument);
	if (event[0] == '[') {
		WCHAR *property = msi_copy_n(event + 1, lstrlenW(event) - 2);
		package_set(package, property, lstrcmpW(argument, L"{}") ? argument : NULL);
		msi_free(property);
	} else if (same_text(event, L"EndDialog")) {
		dialog->done = TRUE;
		dialog->result = same_text(argument, L"Exit") ? ERROR_INSTALL_USEREXIT : ERROR_SUCCESS;
	} else if (same_text(event, L"NewDialog")) {
		msi_free(dialog->next);
		dialog->next = msi_copy(argument);
		dialog->done = TRUE;
		dialog->result = ERROR_SUCCESS;
	} else if (same_text(event, L"SpawnDialog")) {
		msi_free(run->spawn);
		run->spawn = msi_copy(argument);
	} else if (same_text(event, L"DoAction"))
		action_run(package, argument);
	else if (same_text(event, L"SetTargetPath")) {
		WCHAR *path = package_get(package, argument);
		if (path && path[0])
			MsiSetTargetPathW(package->session, argument, path);
		msi_free(path);
	} else if (same_text(event, L"AddLocal"))
		features_set(package, argument, INSTALLSTATE_LOCAL);
	else if (same_text(event, L"AddSource"))
		features_set(package, argument, INSTALLSTATE_SOURCE);
	else if (same_text(event, L"Remove"))
		features_set(package, argument, INSTALLSTATE_ABSENT);
	else if (same_text(event, L"SetInstallLevel"))
		MsiSetInstallLevel(package->session, leading_number(argument));
	else if (same_text(event, L"ValidateProductID")) {
		WCHAR *key = package_get(package, L"PIDKEY");
		if (key)
			package_set(package, L"ProductID", key);
		msi_free(key);
	} else if (same_text(event, L"Reset")) {
		for (UINT i = 0; i < dialog->saved_count; i++)
			package_set(package, dialog->saved[i].name, dialog->saved[i].value);
	} else
		package_log(L"Dialog %s: event %s has no effect here", dialog->name, event);
	msi_free(argument);
	return ERROR_SUCCESS;
}

static void control_publish(ui_dialog *dialog, const ui_control *control)
{
	event_run run = { dialog, NULL };
	msi_record *params = params_new(dialog->name, control->name);
	db_query_each(dialog->package->db, L"SELECT `Event`, `Argument`, `Condition`, `Ordering` FROM `ControlEvent` "
		L"WHERE `Dialog_` = ? AND `Control_` = ? ORDER BY `Ordering`", params, publish, &run);
	if (params)
		object_unref(&params->base);
	if (run.spawn && !dialog->done) {
		const UINT result = dialog_run(dialog->state, dialog->package, run.spawn, dialog);
		if (result == ERROR_INSTALL_USEREXIT) {
			dialog->done = TRUE;
			dialog->result = result;
		}
	}
	msi_free(run.spawn);
	if (dialog->done && dialog == dialog->state->modeless && dialog->result == ERROR_INSTALL_USEREXIT)
		dialog->state->cancelled = TRUE;
	if (!dialog->done) {
		controls_refresh(dialog);
		conditions_apply(dialog);
	}
}

// What the user changed goes into the control's property; then the conditions see it.
static void control_changed(ui_dialog *dialog, ui_control *control, HWND from)
{
	if (dialog->updating)
		return;
	switch (control->kind) {
	case KIND_CHECK:
		bound_set(dialog, control, SendMessageW(control->window, BM_GETCHECK, 0, 0) == BST_CHECKED ? control->checked : NULL);
		break;
	case KIND_RADIO:
		for (UINT b = 0; b < control->button_count; b++)
			if (control->buttons[b] == from)
				bound_set(dialog, control, control->values[b]);
		break;
	case KIND_EDIT:
	case KIND_PATH: {
		WCHAR *text = window_text(control->window);
		bound_set(dialog, control, text);
		msi_free(text);
		break;
	}
	case KIND_COMBO:
	case KIND_LIST: {
		const BOOL combo = control->kind == KIND_COMBO;
		const LRESULT item = SendMessageW(control->window, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0);
		if (item >= 0)
			bound_set(dialog, control, (const WCHAR *)SendMessageW(control->window, combo ? CB_GETITEMDATA : LB_GETITEMDATA, item, 0));
		else if (combo) {
			WCHAR *text = window_text(control->window);
			bound_set(dialog, control, text);
			msi_free(text);
		}
		break;
	}
	default:
		return;
	}
	conditions_apply(dialog);
}

// Edits hand over what was typed before any button acts on it, even where the edit control sent no notification.
static void controls_collect(ui_dialog *dialog)
{
	for (UINT i = 0; i < dialog->control_count; i++)
		if ((dialog->controls[i].kind == KIND_EDIT || dialog->controls[i].kind == KIND_PATH) && dialog->controls[i].property) {
			WCHAR *text = window_text(dialog->controls[i].window);
			bound_set(dialog, &dialog->controls[i], text);
			msi_free(text);
		}
}

static void control_pressed(ui_dialog *dialog, const WCHAR *name)
{
	ui_control *control = control_find(dialog, name);
	package_log(L"Dialog %s: %s pressed%s", dialog->name, name ? name : L"(none)", control ? L"" : L", no such control");
	if (!control || (control->window && !IsWindowEnabled(control->window)) || control->hidden)
		return;
	controls_collect(dialog);
	control_publish(dialog, control);
}

static void paint_bitmaps(ui_dialog *dialog, HDC dc)
{
	for (UINT i = 0; i < dialog->control_count; i++) {
		const ui_control *control = &dialog->controls[i];
		if (control->kind != KIND_BITMAP || !control->image || control->hidden)
			continue;
		const BITMAPINFOHEADER *header = &control->image->bmiHeader;
		SetStretchBltMode(dc, HALFTONE);
		StretchDIBits(dc, control->box.left, control->box.top, control->box.right - control->box.left, control->box.bottom - control->box.top,
			0, 0, header->biWidth, header->biHeight < 0 ? -header->biHeight : header->biHeight, control->bits, control->image, DIB_RGB_COLORS,
			SRCCOPY);
	}
}

static LRESULT CALLBACK dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	ui_dialog *dialog = (ui_dialog *)GetWindowLongPtrW(window, GWLP_USERDATA);
	if (!dialog)
		return DefWindowProcW(window, message, wparam, lparam);
	switch (message) {
	case WM_ERASEBKGND: {
		RECT client;
		GetClientRect(window, &client);
		FillRect((HDC)wparam, &client, GetSysColorBrush(COLOR_BTNFACE));
		paint_bitmaps(dialog, (HDC)wparam);
		return 1;
	}
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN: {
		HDC dc = (HDC)wparam;
		for (UINT i = 0; i < dialog->control_count; i++) {
			ui_control *control = &dialog->controls[i];
			if (control->window != (HWND)lparam)
				continue;
			const ui_style *style = (const ui_style *)GetPropW(control->window, L"RavenMsiStyle");
			if (style && style->colored)
				SetTextColor(dc, style->color);
			if (control->attributes & TEXT_TRANSPARENT && (control->kind == KIND_TEXT || control->kind == KIND_LINK)) {
				SetBkMode(dc, TRANSPARENT);
				return (LRESULT)GetStockObject(NULL_BRUSH);
			}
			SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
			return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
		}
		break;
	}
	case WM_COMMAND: {
		const UINT id = LOWORD(wparam), code = HIWORD(wparam);
		if (id == IDOK) {
			control_pressed(dialog, dialog->default_control);
			return 0;
		}
		if (id == IDCANCEL) {
			control_pressed(dialog, dialog->cancel_control);
			return 0;
		}
		if (id >= RADIO_ID) {
			const UINT index = (id - RADIO_ID) / 64;
			if (index < dialog->control_count && code == BN_CLICKED)
				control_changed(dialog, &dialog->controls[index], (HWND)lparam);
			return 0;
		}
		if (id < CONTROL_ID || id - CONTROL_ID >= dialog->control_count)
			break;
		ui_control *control = &dialog->controls[id - CONTROL_ID];
		if (control->kind == KIND_PUSH && code == BN_CLICKED)
			control_pressed(dialog, control->name);
		else if (control->kind == KIND_CHECK && code == BN_CLICKED) {
			control_changed(dialog, control, (HWND)lparam);
			control_publish(dialog, control);
		} else if ((control->kind == KIND_EDIT || control->kind == KIND_PATH) && code == EN_CHANGE)
			control_changed(dialog, control, (HWND)lparam);
		else if ((control->kind == KIND_COMBO && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)) ||
			(control->kind == KIND_LIST && code == LBN_SELCHANGE))
			control_changed(dialog, control, (HWND)lparam);
		return 0;
	}
	case WM_CLOSE:
		control_pressed(dialog, dialog->cancel_control);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static const WCHAR dialog_class[] = L"RavenMsiDialog";

static void class_register(void)
{
	static BOOL registered;
	if (registered)
		return;
	WNDCLASSW type = { .lpfnWndProc = dialog_proc, .hInstance = GetModuleHandleW(L"msi.dll"), .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
		.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1), .lpszClassName = dialog_class };
	registered = RegisterClassW(&type) != 0;
}

static UINT add_control(msi_record *row, void *context)
{
	ui_dialog *dialog = context;
	WCHAR s[6][16];
	const WCHAR *name = record_text(row, 1, s[0]), *type = record_text(row, 2, s[1]), *property = record_text(row, 8, s[2]);
	const WCHAR *text = record_text(row, 9, s[3]), *next = record_text(row, 10, s[4]);
	if (!name || !type)
		return ERROR_SUCCESS;
	dialog->controls = msi_grow(dialog->controls, (dialog->control_count + 1) * sizeof(ui_control));
	ui_control *control = &dialog->controls[dialog->control_count++];
	*control = (ui_control){0};
	control->name = msi_copy(name);
	control->kind = KIND_OTHER;
	for (UINT i = 0; i < ARRAYSIZE(control_kinds); i++)
		if (same_text(control_kinds[i].name, type))
			control->kind = control_kinds[i].kind;
	if (control->kind == KIND_OTHER)
		package_log(L"Dialog %s: control %s of type %s is not shown", dialog->name, name, type);
	const INT x = record_get_int(row, 3), y = record_get_int(row, 4), width = record_get_int(row, 5), height = record_get_int(row, 6);
	control->box = (RECT){ units(x), units(y), units(x + width), units(y + height) };
	control->attributes = record_get_int(row, 7) == MSI_NULL_INTEGER ? 0 : record_get_int(row, 7);
	control->property = property && property[0] ? msi_copy(property) : NULL;
	control->next = next && next[0] ? msi_copy(next) : NULL;
	// The text is kept in the checked slot until the windows exist; CheckBox values come from their own table.
	control->checked = text ? msi_copy(text) : NULL;
	return ERROR_SUCCESS;
}

static UINT add_item(msi_record *row, void *context)
{
	HWND *target = context;
	WCHAR a[16], b[16];
	const WCHAR *value = record_text(row, 1, a), *text = record_text(row, 2, b);
	const BOOL combo = GetPropW(*target, L"RavenMsiCombo") != NULL;
	const LRESULT item = SendMessageW(*target, combo ? CB_ADDSTRING : LB_ADDSTRING, 0, (LPARAM)(text && text[0] ? text : value ? value : L""));
	if (item >= 0)
		SendMessageW(*target, combo ? CB_SETITEMDATA : LB_SETITEMDATA, item, (LPARAM)msi_copy(value ? value : L""));
	return ERROR_SUCCESS;
}

typedef struct { ui_dialog *dialog; ui_control *control; UINT index; } radio_rows;

static UINT add_radio(msi_record *row, void *context)
{
	radio_rows *rows = context;
	ui_control *control = rows->control;
	WCHAR a[16], b[16];
	const WCHAR *value = record_text(row, 1, a), *raw = record_text(row, 6, b);
	if (control->button_count >= 64)
		return ERROR_SUCCESS;
	const INT x = record_get_int(row, 2), y = record_get_int(row, 3), width = record_get_int(row, 4), height = record_get_int(row, 5);
	const ui_style *style;
	WCHAR *text = control_text(rows->dialog, raw, &style);
	DWORD kind = BS_AUTORADIOBUTTON | (control->attributes & PUSH_LIKE ? BS_PUSHLIKE : 0);
	HWND button = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_CLIPSIBLINGS | kind | (control->button_count ? 0 : WS_GROUP | WS_TABSTOP),
		control->box.left + units(x), control->box.top + units(y), units(width), units(height), rows->dialog->window,
		(HMENU)(UINT_PTR)(RADIO_ID + rows->index * 64 + control->button_count), NULL, NULL);
	msi_free(text);
	SendMessageW(button, WM_SETFONT, (WPARAM)(style ? style->font : rows->dialog->state->font), FALSE);
	control->buttons = msi_grow(control->buttons, (control->button_count + 1) * sizeof(HWND));
	control->values = msi_grow(control->values, (control->button_count + 1) * sizeof(WCHAR *));
	control->buttons[control->button_count] = button;
	control->values[control->button_count++] = msi_copy(value ? value : L"");
	return ERROR_SUCCESS;
}

// A Bitmap control's picture is a .bmp file in the Binary table, named by the control's text.
static void bitmap_load(ui_dialog *dialog, ui_control *control, const WCHAR *name)
{
	first_field field = query_first(dialog->package, L"SELECT `Data` FROM `Binary` WHERE `Name` = ?", name, NULL);
	msi_free(field.text);
	if (!field.blob)
		return;
	const BITMAPFILEHEADER *file = (const BITMAPFILEHEADER *)field.blob->data;
	if (field.blob->size < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) || file->bfType != 0x4d42 || file->bfOffBits >= field.blob->size) {
		package_log(L"Dialog %s: the picture %s is not a bitmap file", dialog->name, name);
		blob_unref(field.blob);
		return;
	}
	control->blob = field.blob;
	control->image = (BITMAPINFO *)(field.blob->data + sizeof(BITMAPFILEHEADER));
	control->bits = field.blob->data + file->bfOffBits;
}

typedef struct { const char *at; LONG left; } rich_source;

static DWORD CALLBACK rich_feed(DWORD_PTR cookie, BYTE *buffer, LONG size, LONG *done)
{
	rich_source *in = (rich_source *)cookie;
	LONG take = in->left < size ? in->left : size;
	CopyMemory(buffer, in->at, take);
	in->at += take;
	in->left -= take;
	*done = take;
	return 0;
}

// ScrollableText holds rich text, as the license agreement of most packages is.
static void rich_text(HWND window, const WCHAR *text)
{
	char *ansi = msi_to_ansi(text);
	rich_source from = { ansi, lstrlenA(ansi) };
	EDITSTREAM stream = { (DWORD_PTR)&from, 0, rich_feed };
	SendMessageW(window, EM_STREAMIN, text[0] == '{' ? SF_RTF : SF_TEXT, (LPARAM)&stream);
	msi_free(ansi);
}

static void control_create(ui_dialog *dialog, UINT index)
{
	ui_control *control = &dialog->controls[index];
	WCHAR *raw = control->checked;
	control->checked = NULL;
	const ui_style *style = NULL;
	WCHAR *text = control->kind == KIND_SCROLL || control->kind == KIND_BITMAP ? msi_copy(raw ? raw : L"") : control_text(dialog, raw, &style);
	msi_free(raw);
	const WCHAR *type = NULL;
	DWORD kind = 0, extra = 0;
	const int x = control->box.left, y = control->box.top, width = control->box.right - x;
	int height = control->box.bottom - y;
	switch (control->kind) {
	case KIND_LINK:
		link_text(text);
		// fall through
	case KIND_TEXT:
		type = WC_STATICW;
		kind = (control->attributes & TEXT_NOPREFIX ? SS_NOPREFIX : 0) | (control->attributes & TEXT_NOWRAP ? SS_LEFTNOWORDWRAP : SS_LEFT);
		break;
	case KIND_PUSH:
		type = WC_BUTTONW;
		kind = WS_TABSTOP | (same_text(control->name, dialog->default_control) ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
		break;
	case KIND_CHECK: {
		type = WC_BUTTONW;
		kind = WS_TABSTOP | BS_AUTOCHECKBOX | (control->attributes & PUSH_LIKE ? BS_PUSHLIKE : 0);
		first_field value = control->property ? query_first(dialog->package, L"SELECT `Value` FROM `CheckBox` WHERE `Property` = ?", control->property, NULL)
			: (first_field){0};
		control->checked = value.text ? value.text : msi_copy(L"1");
		if (value.blob)
			blob_unref(value.blob);
		break;
	}
	case KIND_EDIT:
	case KIND_PATH:
		type = WC_EDITW;
		kind = WS_TABSTOP | ES_AUTOHSCROLL | (control->attributes & EDIT_PASSWORD ? ES_PASSWORD : 0) |
			(control->attributes & EDIT_MULTILINE ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL : 0);
		extra = WS_EX_CLIENTEDGE;
		break;
	case KIND_LINE:
		type = WC_STATICW;
		kind = SS_ETCHEDHORZ;
		break;
	case KIND_GROUP:
		type = WC_BUTTONW;
		kind = BS_GROUPBOX;
		break;
	case KIND_SCROLL:
		LoadLibraryW(L"riched20.dll");
		type = RICHEDIT_CLASSW;
		kind = WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL;
		extra = WS_EX_CLIENTEDGE;
		break;
	case KIND_PROGRESS:
		LoadLibraryW(L"comctl32.dll");
		type = PROGRESS_CLASSW;
		kind = PBS_SMOOTH;
		break;
	case KIND_COMBO:
		type = WC_COMBOBOXW;
		kind = WS_TABSTOP | WS_VSCROLL | (control->attributes & COMBO_LIST ? CBS_DROPDOWNLIST : CBS_DROPDOWN | CBS_AUTOHSCROLL);
		height += units(100);
		break;
	case KIND_LIST:
		type = WC_LISTBOXW;
		kind = WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY;
		extra = WS_EX_CLIENTEDGE;
		break;
	case KIND_BITMAP:
		bitmap_load(dialog, control, text);
		break;
	case KIND_RADIO:
		if (control->attributes & GROUP_BORDER)
			type = WC_BUTTONW, kind = BS_GROUPBOX;
		break;
	default:
		break;
	}
	if (control->attributes & CONTROL_SUNKEN)
		extra |= WS_EX_CLIENTEDGE;
	if (type) {
		control->window = CreateWindowExW(extra, type, control->kind == KIND_SCROLL ? L"" : text, WS_CHILD | WS_CLIPSIBLINGS | kind, x, y,
			width, height, dialog->window, (HMENU)(UINT_PTR)(CONTROL_ID + index), NULL, NULL);
		SendMessageW(control->window, WM_SETFONT, (WPARAM)(style ? style->font : dialog->state->font), FALSE);
		if (style)
			SetPropW(control->window, L"RavenMsiStyle", (HANDLE)style);
	}
	if (control->kind == KIND_SCROLL && control->window)
		rich_text(control->window, text);
	else if (control->kind == KIND_PROGRESS && control->window)
		SendMessageW(control->window, PBM_SETRANGE32, 0, 1000);
	else if (control->kind == KIND_EDIT && control->window && text[0] == '{')
		SendMessageW(control->window, EM_LIMITTEXT, leading_number(text + 1), 0);
	else if ((control->kind == KIND_COMBO || control->kind == KIND_LIST) && control->window && control->property) {
		if (control->kind == KIND_COMBO)
			SetPropW(control->window, L"RavenMsiCombo", (HANDLE)1);
		msi_record *params = params_new(control->property, NULL);
		db_query_each(dialog->package->db, control->kind == KIND_COMBO ? L"SELECT `Value`, `Text` FROM `ComboBox` WHERE `Property` = ? ORDER BY `Order`"
			: L"SELECT `Value`, `Text` FROM `ListBox` WHERE `Property` = ? ORDER BY `Order`", params, add_item, &control->window);
		if (params)
			object_unref(&params->base);
	} else if (control->kind == KIND_RADIO && control->property) {
		radio_rows rows = { dialog, control, index };
		msi_record *params = params_new(control->property, NULL);
		db_query_each(dialog->package->db, L"SELECT `Value`, `X`, `Y`, `Width`, `Height`, `Text`, `Order` FROM `RadioButton` "
			L"WHERE `Property` = ? ORDER BY `Order`", params, add_radio, &rows);
		if (params)
			object_unref(&params->base);
	}
	msi_free(text);
	control_show(dialog, control, (control->attributes & CONTROL_VISIBLE) != 0);
	control_enable(control, (control->attributes & CONTROL_ENABLED) != 0);
}

static UINT map_event(msi_record *row, void *context)
{
	ui_dialog *dialog = context;
	WCHAR a[16], b[16], c[16];
	ui_control *control = control_find(dialog, record_text(row, 1, a));
	const WCHAR *event = record_text(row, 2, b), *attribute = record_text(row, 3, c);
	if (!control || !event)
		return ERROR_SUCCESS;
	if (same_text(event, L"SetProgress"))
		control->progress = TRUE;
	else if (same_text(event, L"ActionText"))
		control->action_text = TRUE;
	else if (same_text(event, L"ActionData"))
		control->action_data = TRUE;
	else
		package_log(L"Dialog %s: %s does not follow %s (%s)", dialog->name, control->name, event, attribute ? attribute : L"");
	return ERROR_SUCCESS;
}

static void dialog_destroy(ui_dialog *dialog)
{
	// What the window says while it goes, a focus leaving an edit for instance, no longer reaches the dialog.
	if (dialog->window)
		SetWindowLongPtrW(dialog->window, GWLP_USERDATA, 0);
	for (UINT i = 0; i < dialog->control_count; i++) {
		ui_control *control = &dialog->controls[i];
		if (control->window && (control->kind == KIND_COMBO || control->kind == KIND_LIST)) {
			const BOOL combo = control->kind == KIND_COMBO;
			const LRESULT count = SendMessageW(control->window, combo ? CB_GETCOUNT : LB_GETCOUNT, 0, 0);
			for (LRESULT item = 0; item < count; item++)
				msi_free((WCHAR *)SendMessageW(control->window, combo ? CB_GETITEMDATA : LB_GETITEMDATA, item, 0));
			RemovePropW(control->window, L"RavenMsiCombo");
		}
		if (control->window)
			RemovePropW(control->window, L"RavenMsiStyle");
		msi_free(control->name);
		msi_free(control->property);
		msi_free(control->checked);
		msi_free(control->next);
		for (UINT b = 0; b < control->button_count; b++)
			msi_free(control->values[b]);
		msi_free(control->buttons);
		msi_free(control->values);
		if (control->blob)
			blob_unref(control->blob);
	}
	for (UINT i = 0; i < dialog->saved_count; i++) {
		msi_free(dialog->saved[i].name);
		msi_free(dialog->saved[i].value);
	}
	msi_free(dialog->saved);
	if (dialog->window)
		DestroyWindow(dialog->window);
	msi_free(dialog->controls);
	msi_free(dialog->name);
	msi_free(dialog->first);
	msi_free(dialog->default_control);
	msi_free(dialog->cancel_control);
	msi_free(dialog->next);
	msi_free(dialog);
}

typedef struct { INT h, v, width, height, attributes; WCHAR *title, *first, *default_control, *cancel_control; BOOL found; } dialog_row;

static UINT read_dialog(msi_record *row, void *context)
{
	dialog_row *out = context;
	WCHAR s[4][16];
	const WCHAR *title = record_text(row, 6, s[0]), *first = record_text(row, 7, s[1]), *fallback = record_text(row, 8, s[2]),
		*cancel = record_text(row, 9, s[3]);
	out->h = record_get_int(row, 1);
	out->v = record_get_int(row, 2);
	out->width = record_get_int(row, 3);
	out->height = record_get_int(row, 4);
	out->attributes = record_get_int(row, 5) == MSI_NULL_INTEGER ? 0 : record_get_int(row, 5);
	out->title = msi_copy(title ? title : L"");
	out->first = first ? msi_copy(first) : NULL;
	out->default_control = fallback ? msi_copy(fallback) : NULL;
	out->cancel_control = cancel ? msi_copy(cancel) : NULL;
	out->found = TRUE;
	return ERROR_NO_MORE_ITEMS;
}

static BOOL dialog_exists(msi_package *package, const WCHAR *name)
{
	first_field field = query_first(package, L"SELECT `Dialog` FROM `Dialog` WHERE `Dialog` = ?", name, NULL);
	const BOOL found = field.text != NULL;
	msi_free(field.text);
	return found;
}

// The controls go in their tab order, from the first along Control_Next; the rest after them, below in the z-order.
static void controls_order(ui_dialog *dialog)
{
	ui_control *ordered = msi_alloc(dialog->control_count * sizeof(ui_control) + 1);
	BOOL *taken = msi_alloc(dialog->control_count * sizeof(BOOL) + 1);
	ZeroMemory(taken, dialog->control_count * sizeof(BOOL));
	UINT count = 0;
	const WCHAR *at = dialog->first;
	while (at && count < dialog->control_count) {
		UINT found = dialog->control_count;
		for (UINT i = 0; i < dialog->control_count; i++)
			if (!taken[i] && same_text(dialog->controls[i].name, at))
				found = i;
		if (found == dialog->control_count)
			break;
		taken[found] = TRUE;
		ordered[count++] = dialog->controls[found];
		at = dialog->controls[found].next;
	}
	for (UINT i = 0; i < dialog->control_count; i++)
		if (!taken[i])
			ordered[count++] = dialog->controls[i];
	CopyMemory(dialog->controls, ordered, dialog->control_count * sizeof(ui_control));
	msi_free(ordered);
	msi_free(taken);
}

static void properties_save(ui_dialog *dialog)
{
	for (UINT i = 0; i < dialog->control_count; i++) {
		WCHAR *name = bound_property(dialog, &dialog->controls[i]);
		if (!name)
			continue;
		dialog->saved = msi_grow(dialog->saved, (dialog->saved_count + 1) * sizeof(msi_property));
		dialog->saved[dialog->saved_count].value = package_get(dialog->package, name);
		dialog->saved[dialog->saved_count++].name = name;
	}
}

static ui_dialog *dialog_create(ui_state *state, msi_package *package, const WCHAR *name, ui_dialog *parent)
{
	dialog_row row = {0};
	msi_record *params = params_new(name, NULL);
	db_query_each(package->db, L"SELECT `HCentering`, `VCentering`, `Width`, `Height`, `Attributes`, `Title`, `Control_First`, `Control_Default`, "
		L"`Control_Cancel` FROM `Dialog` WHERE `Dialog` = ?", params, read_dialog, &row);
	if (!row.found) {
		if (params)
			object_unref(&params->base);
		return NULL;
	}
	class_register();
	ui_dialog *dialog = msi_alloc(sizeof(ui_dialog));
	*dialog = (ui_dialog){ .package = package, .state = state, .name = msi_copy(name), .first = row.first, .default_control = row.default_control,
		.cancel_control = row.cancel_control, .attributes = row.attributes };
	// A modeless dialog the new one replaces goes once the new one is up, so that a window of the installer stays active throughout.
	ui_dialog *replaced = state->modeless && !parent && !(row.attributes & DIALOG_KEEP_MODELESS) ? state->modeless : NULL;
	if (replaced)
		state->modeless = NULL;
	db_query_each(package->db, L"SELECT `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Property`, `Text`, `Control_Next` "
		L"FROM `Control` WHERE `Dialog_` = ?", params, add_control, dialog);
	controls_order(dialog);
	WCHAR *title = format_text(package, row.title, NULL);
	msi_free(row.title);
	RECT frame = { 0, 0, units(row.width), units(row.height) };
	const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | (row.attributes & DIALOG_MINIMIZE ? WS_MINIMIZEBOX : 0);
	AdjustWindowRectEx(&frame, style, FALSE, WS_EX_DLGMODALFRAME);
	const int width = frame.right - frame.left, height = frame.bottom - frame.top;
	const int screen_width = GetSystemMetrics(SM_CXSCREEN), screen_height = GetSystemMetrics(SM_CYSCREEN);
	const int x = MulDiv(screen_width - width, row.h == MSI_NULL_INTEGER ? 50 : row.h, 100);
	const int y = MulDiv(screen_height - height, row.v == MSI_NULL_INTEGER ? 50 : row.v, 100);
	HWND owner = parent ? parent->window : state->modeless ? state->modeless->window : NULL;
	dialog->window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, dialog_class, title, style, x, y, width, height, owner, NULL, NULL, NULL);
	msi_free(title);
	if (!dialog->window) {
		if (params)
			object_unref(&params->base);
		dialog_destroy(dialog);
		if (replaced)
			dialog_destroy(replaced);
		return NULL;
	}
	SetWindowLongPtrW(dialog->window, GWLP_USERDATA, (LONG_PTR)dialog);
	for (UINT i = 0; i < dialog->control_count; i++)
		control_create(dialog, i);
	db_query_each(package->db, L"SELECT `Control_`, `Event`, `Attribute` FROM `EventMapping` WHERE `Dialog_` = ?", params, map_event, dialog);
	if (params)
		object_unref(&params->base);
	properties_save(dialog);
	controls_refresh(dialog);
	conditions_apply(dialog);
	package_log(L"Dialog %s shown with %u controls", name, dialog->control_count);
	if (row.attributes & DIALOG_VISIBLE) {
		ShowWindow(dialog->window, SW_SHOW);
		UpdateWindow(dialog->window);
		SetForegroundWindow(dialog->window);
	}
	if (replaced)
		dialog_destroy(replaced);
	for (UINT i = 0; i < dialog->control_count; i++)
		if (dialog->controls[i].window && same_text(dialog->controls[i].name, dialog->first)) {
			SetFocus(dialog->controls[i].window);
			break;
		}
	if (parent)
		EnableWindow(parent->window, FALSE);
	return dialog;
}

static void pump(void)
{
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

// A dialog and those its NewDialog events lead to, until one ends the wizard. A modeless dialog stays up and returns at once.
static UINT dialog_run(ui_state *state, msi_package *package, const WCHAR *name, ui_dialog *parent)
{
	WCHAR *current = msi_copy(name);
	UINT result = ERROR_SUCCESS;
	while (current) {
		ui_dialog *dialog = dialog_create(state, package, current, parent);
		msi_free(current);
		current = NULL;
		if (!dialog)
			return ERROR_FUNCTION_FAILED;
		if (!(dialog->attributes & DIALOG_MODAL) && !parent) {
			state->modeless = dialog;
			pump();
			return ERROR_SUCCESS;
		}
		MSG message;
		package_log(L"Dialog %s waits for the user", dialog->name);
		while (!dialog->done && GetMessageW(&message, NULL, 0, 0) > 0) {
			if (message.message == WM_KEYDOWN && (message.wParam == VK_RETURN || message.wParam == VK_ESCAPE) &&
				(message.hwnd == dialog->window || IsChild(dialog->window, message.hwnd))) {
				WCHAR type[16];
				GetClassNameW(message.hwnd, type, ARRAYSIZE(type));
				const BOOL multiline = (GetWindowLongW(message.hwnd, GWL_STYLE) & ES_MULTILINE) && same_text(type, L"Edit");
				if (message.wParam == VK_ESCAPE || !multiline) {
					control_pressed(dialog, message.wParam == VK_RETURN ? dialog->default_control : dialog->cancel_control);
					continue;
				}
			}
			if (!IsDialogMessageW(dialog->window, &message)) {
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
		}
		result = dialog->result;
		current = dialog->next;
		dialog->next = NULL;
		if (parent)
			EnableWindow(parent->window, TRUE);
		dialog_destroy(dialog);
	}
	if (parent)
		SetForegroundWindow(parent->window);
	return result;
}

BOOL ui_wanted(msi_package *package, const WCHAR *sequence)
{
	const INSTALLUILEVEL level = ui_internal_level() & 7;
	if (level != INSTALLUILEVEL_FULL && level != INSTALLUILEVEL_REDUCED)
		return FALSE;
	WCHAR query[96];
	wsprintfW(query, L"SELECT `Action` FROM `%s`", sequence);
	return table_has_rows(package, L"SELECT `Dialog` FROM `Dialog`") && table_has_rows(package, query);
}

UINT ui_run(msi_package *package, const WCHAR *sequence, const WCHAR *execute)
{
	ui_state state = { .execute = execute, .reduced = (ui_internal_level() & 7) == INSTALLUILEVEL_REDUCED };
	NONCLIENTMETRICSW metrics = { sizeof metrics };
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
	WCHAR *fallback = package_get(package, L"DefaultUIFont");
	package->ui = &state;
	const ui_style *style = fallback ? style_find(&state, package, fallback) : NULL;
	state.font = style ? style->font : CreateFontIndirectW(&metrics.lfMessageFont);
	msi_free(fallback);
	package_log(L"User interface: %s, then %s", sequence, execute);
	UINT status = sequence_run(package, sequence);
	if (state.modeless)
		dialog_destroy(state.modeless);
	if (!style)
		DeleteObject(state.font);
	for (UINT i = 0; i < state.style_count; i++) {
		DeleteObject(state.styles[i].font);
		msi_free(state.styles[i].name);
	}
	msi_free(state.styles);
	package->ui = NULL;
	return status;
}

// In the user interface sequence a dialog stands for its action, and ExecuteAction runs the installation itself.
BOOL ui_action(msi_package *package, const WCHAR *name, UINT *status)
{
	ui_state *state = package->ui;
	if (!state)
		return FALSE;
	if (state->executing)
		return FALSE;
	if (same_text(name, L"ExecuteAction")) {
		state->executing = TRUE;
		*status = sequence_run(package, state->execute);
		state->executing = FALSE;
		if (!*status && state->cancelled)
			*status = ERROR_INSTALL_USEREXIT;
		return TRUE;
	}
	if (!dialog_exists(package, name))
		return FALSE;
	first_field modal = query_first(package, L"SELECT `Attributes` FROM `Dialog` WHERE `Dialog` = ?", name, NULL);
	const BOOL wizard = modal.text && (leading_number(modal.text) & DIALOG_MODAL);
	msi_free(modal.text);
	if (state->reduced && wizard) {
		package_log(L"Dialog %s skipped at the reduced level", name);
		*status = ERROR_SUCCESS;
		return TRUE;
	}
	*status = dialog_run(state, package, name, NULL);
	return TRUE;
}

void ui_action_started(msi_package *package, const WCHAR *name)
{
	ui_state *state = package->ui;
	if (!state || !state->modeless)
		return;
	first_field description = query_first(package, L"SELECT `Description` FROM `ActionText` WHERE `Action` = ?", name, NULL);
	WCHAR *text = description.text ? format_text(package, description.text, NULL) : msi_copy(name);
	msi_free(description.text);
	ui_dialog *dialog = state->modeless;
	for (UINT i = 0; i < dialog->control_count; i++)
		if (dialog->controls[i].action_text && dialog->controls[i].window)
			text_set(dialog, &dialog->controls[i], text);
	msi_free(text);
	pump();
}

BOOL ui_progress(msi_package *package, UINT done, UINT total)
{
	ui_state *state = package->ui;
	if (!state || !state->modeless)
		return TRUE;
	ui_dialog *dialog = state->modeless;
	for (UINT i = 0; i < dialog->control_count; i++)
		if (dialog->controls[i].progress && dialog->controls[i].window)
			SendMessageW(dialog->controls[i].window, PBM_SETPOS, total ? MulDiv(done, 1000, total) : 0, 0);
	pump();
	return !state->cancelled;
}
