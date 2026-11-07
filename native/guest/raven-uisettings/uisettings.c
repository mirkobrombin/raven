/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven UI settings component: the Windows.UI.ViewManagement.UISettings class of the Windows Runtime, which programs ask for the
 * colours, sizes and timings of the desktop and the text scale the user chose.
 *
 * Every answer comes from the classic desktop the runtime draws: system colours and metrics, the caret and mouse timings, the default
 * blue accent of Windows in its shades, a text scale of one and the light theme. The settings never change, so the change events take
 * handlers and never raise them. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <winstring.h>
#include <activation.h>
#include <windows.ui.viewmanagement.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

typedef __x_ABI_CWindows_CUI_CViewManagement_CIUISettings settings1;
typedef __x_ABI_CWindows_CUI_CViewManagement_CIUISettings2 settings2;
typedef __x_ABI_CWindows_CUI_CViewManagement_CIUISettings3 settings3;
typedef __x_ABI_CWindows_CUI_CViewManagement_CIUISettings4 settings4;
typedef __x_ABI_CWindows_CUI_CViewManagement_CIUISettings5 settings5;
typedef __x_ABI_CWindows_CUI_CViewManagement_CIUISettings6 settings6;
typedef __x_ABI_CWindows_CUI_CColor color;
typedef __x_ABI_CWindows_CFoundation_CSize size;
typedef EventRegistrationToken token;

static const WCHAR class_name[] = L"Windows.UI.ViewManagement.UISettings";
static LONG objects, tokens;

/* One object answers all six interfaces, each through its own table at its own place in the object. */
struct settings {
	settings1 one;
	settings2 two;
	settings3 three;
	settings4 four;
	settings5 five;
	settings6 six;
	LONG refs;
};

#define SETTINGS(iface, member) ((struct settings *)((char *)(iface) - offsetof(struct settings, member)))

static HRESULT settings_query(struct settings *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID___x_ABI_CWindows_CUI_CViewManagement_CIUISettings))
		*out = &self->one;
	else if (IsEqualIID(iid, &IID___x_ABI_CWindows_CUI_CViewManagement_CIUISettings2))
		*out = &self->two;
	else if (IsEqualIID(iid, &IID___x_ABI_CWindows_CUI_CViewManagement_CIUISettings3))
		*out = &self->three;
	else if (IsEqualIID(iid, &IID___x_ABI_CWindows_CUI_CViewManagement_CIUISettings4))
		*out = &self->four;
	else if (IsEqualIID(iid, &IID___x_ABI_CWindows_CUI_CViewManagement_CIUISettings5))
		*out = &self->five;
	else if (IsEqualIID(iid, &IID___x_ABI_CWindows_CUI_CViewManagement_CIUISettings6))
		*out = &self->six;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG settings_release(struct settings *self)
{
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return (ULONG)refs;
}

static HRESULT inspectable_iids(ULONG *count, IID **iids)
{
	if (!count || !iids)
		return E_POINTER;
	*count = 0;
	*iids = NULL;
	return S_OK;
}

static HRESULT inspectable_name(HSTRING *name) { return WindowsCreateString(class_name, lstrlenW(class_name), name); }

static HRESULT inspectable_trust(TrustLevel *level)
{
	if (!level)
		return E_POINTER;
	*level = BaseTrust;
	return S_OK;
}

/* The IInspectable part of each table, over the object its interface sits in. */
#define INSPECTABLE(prefix, type, member) \
	static HRESULT WINAPI prefix##_query(type *iface, REFIID iid, void **out) { return settings_query(SETTINGS(iface, member), iid, out); } \
	static ULONG WINAPI prefix##_reference(type *iface) { return (ULONG)InterlockedIncrement(&SETTINGS(iface, member)->refs); } \
	static ULONG WINAPI prefix##_release(type *iface) { return settings_release(SETTINGS(iface, member)); } \
	static HRESULT WINAPI prefix##_iids(type *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); } \
	static HRESULT WINAPI prefix##_name(type *iface, HSTRING *name) { return inspectable_name(name); } \
	static HRESULT WINAPI prefix##_trust(type *iface, TrustLevel *level) { return inspectable_trust(level); }

INSPECTABLE(one, settings1, one)
INSPECTABLE(two, settings2, two)
INSPECTABLE(three, settings3, three)
INSPECTABLE(four, settings4, four)
INSPECTABLE(five, settings5, five)
INSPECTABLE(six, settings6, six)

/* A handler is taken for a change that never comes: it gets a token of its own and is not kept. */
static HRESULT add_handler(void *handler, token *out)
{
	if (!handler || !out)
		return E_INVALIDARG;
	out->value = InterlockedIncrement(&tokens);
	return S_OK;
}

static HRESULT WINAPI remove_handler(void *iface, token value) { return S_OK; }

static HRESULT answer_size(size *out, int width, int height)
{
	if (!out)
		return E_POINTER;
	out->Width = (FLOAT)GetSystemMetrics(width);
	out->Height = (FLOAT)GetSystemMetrics(height);
	return S_OK;
}

static HRESULT answer_number(UINT32 *out, UINT32 value)
{
	if (!out)
		return E_POINTER;
	*out = value;
	return S_OK;
}

static HRESULT answer_flag(boolean *out, boolean value)
{
	if (!out)
		return E_POINTER;
	*out = value;
	return S_OK;
}

static UINT32 parameter(UINT action, UINT32 fallback)
{
	UINT32 value = 0;
	return SystemParametersInfoW(action, 0, &value, 0) ? value : fallback;
}

static color opaque(COLORREF value)
{
	color out = {255, GetRValue(value), GetGValue(value), GetBValue(value)};
	return out;
}

static HRESULT WINAPI one_hand(settings1 *iface, __x_ABI_CWindows_CUI_CViewManagement_CHandPreference *out)
{
	if (!out)
		return E_POINTER;
	*out = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? HandPreference_LeftHanded : HandPreference_RightHanded;
	return S_OK;
}

static HRESULT WINAPI one_cursor(settings1 *iface, size *out) { return answer_size(out, SM_CXCURSOR, SM_CYCURSOR); }
static HRESULT WINAPI one_scroll_bar(settings1 *iface, size *out) { return answer_size(out, SM_CXVSCROLL, SM_CYHSCROLL); }
static HRESULT WINAPI one_scroll_arrow(settings1 *iface, size *out) { return answer_size(out, SM_CXHSCROLL, SM_CYVSCROLL); }
static HRESULT WINAPI one_scroll_thumb(settings1 *iface, size *out) { return answer_size(out, SM_CXHTHUMB, SM_CYVTHUMB); }
static HRESULT WINAPI one_message(settings1 *iface, UINT32 *out) { return answer_number(out, parameter(SPI_GETMESSAGEDURATION, 5)); }
static HRESULT WINAPI one_animations(settings1 *iface, boolean *out) { return answer_flag(out, parameter(SPI_GETCLIENTAREAANIMATION, 1) != 0); }
static HRESULT WINAPI one_caret_browsing(settings1 *iface, boolean *out) { return answer_flag(out, FALSE); }
static HRESULT WINAPI one_caret_blink(settings1 *iface, UINT32 *out) { return answer_number(out, GetCaretBlinkTime()); }
static HRESULT WINAPI one_caret_width(settings1 *iface, UINT32 *out) { return answer_number(out, parameter(SPI_GETCARETWIDTH, 1)); }
static HRESULT WINAPI one_double_click(settings1 *iface, UINT32 *out) { return answer_number(out, GetDoubleClickTime()); }
static HRESULT WINAPI one_hover(settings1 *iface, UINT32 *out) { return answer_number(out, parameter(SPI_GETMOUSEHOVERTIME, 400)); }

/* The default accent of Windows, from the darkest shade to the lightest, as UIColorType numbers them from AccentDark3. */
static const COLORREF accent[] = {RGB(0x00, 0x26, 0x42), RGB(0x00, 0x42, 0x75), RGB(0x00, 0x5a, 0x9e), RGB(0x00, 0x78, 0xd7),
	RGB(0x42, 0x9c, 0xe3), RGB(0x76, 0xb9, 0xed), RGB(0xa6, 0xd8, 0xff)};

/* The classic elements are the system colours; the newer ones are the accent and the text and control shades of the light theme. */
static HRESULT WINAPI one_element(settings1 *iface, __x_ABI_CWindows_CUI_CViewManagement_CUIElementType element, color *out)
{
	static const int system[] = {COLOR_ACTIVECAPTION, COLOR_BACKGROUND, COLOR_BTNFACE, COLOR_BTNTEXT, COLOR_CAPTIONTEXT, COLOR_GRAYTEXT,
		COLOR_HIGHLIGHT, COLOR_HIGHLIGHTTEXT, COLOR_HOTLIGHT, COLOR_INACTIVECAPTION, COLOR_INACTIVECAPTIONTEXT, COLOR_WINDOW, COLOR_WINDOWTEXT};
	if (!out)
		return E_POINTER;
	if ((UINT32)element < ARRAYSIZE(system)) {
		*out = opaque(GetSysColor(system[element]));
		return S_OK;
	}
	switch ((UINT32)element) {
	case 1000: *out = opaque(accent[3]); return S_OK;
	case 1001: *out = opaque(RGB(0, 0, 0)); return S_OK;
	case 1002: *out = opaque(RGB(0x33, 0x33, 0x33)); return S_OK;
	case 1003: *out = opaque(RGB(0x66, 0x66, 0x66)); return S_OK;
	case 1004: *out = opaque(RGB(0xff, 0xff, 0xff)); return S_OK;
	case 1005: *out = opaque(RGB(0x33, 0x33, 0x33)); return S_OK;
	case 1006: *out = opaque(RGB(0x66, 0x66, 0x66)); return S_OK;
	case 1007: *out = opaque(RGB(0x99, 0x99, 0x99)); return S_OK;
	case 1008: *out = opaque(RGB(0xcc, 0xcc, 0xcc)); return S_OK;
	case 1009: *out = opaque(RGB(0xf2, 0xf2, 0xf2)); return S_OK;
	case 1010: *out = opaque(RGB(0xff, 0xff, 0xff)); return S_OK;
	case 1011: *out = opaque(RGB(0xe6, 0xe6, 0xe6)); return S_OK;
	case 1012: *out = opaque(RGB(0x00, 0x00, 0x00)); return S_OK;
	case 1013: *out = opaque(RGB(0xff, 0xff, 0xff)); return S_OK;
	default:
		return E_INVALIDARG;
	}
}

static HRESULT WINAPI two_scale(settings2 *iface, DOUBLE *out)
{
	if (!out)
		return E_POINTER;
	*out = 1.0;
	return S_OK;
}

static HRESULT WINAPI two_add(settings2 *iface, void *handler, token *out) { return add_handler(handler, out); }

/* GetColorValue: the light theme's white background and black text, and the accent shades. */
static HRESULT WINAPI three_value(settings3 *iface, __x_ABI_CWindows_CUI_CViewManagement_CUIColorType type, color *out)
{
	if (!out)
		return E_POINTER;
	if (type == UIColorType_Background)
		*out = opaque(RGB(0xff, 0xff, 0xff));
	else if (type == UIColorType_Foreground)
		*out = opaque(RGB(0, 0, 0));
	else if (type >= UIColorType_AccentDark3 && type <= UIColorType_AccentLight3)
		*out = opaque(accent[type - UIColorType_AccentDark3]);
	else
		return E_INVALIDARG;
	return S_OK;
}

static HRESULT WINAPI three_add(settings3 *iface, void *handler, token *out) { return add_handler(handler, out); }
static HRESULT WINAPI four_effects(settings4 *iface, boolean *out) { return answer_flag(out, TRUE); }
static HRESULT WINAPI four_add(settings4 *iface, void *handler, token *out) { return add_handler(handler, out); }
static HRESULT WINAPI five_hide(settings5 *iface, boolean *out) { return answer_flag(out, TRUE); }
static HRESULT WINAPI five_add(settings5 *iface, void *handler, token *out) { return add_handler(handler, out); }
static HRESULT WINAPI six_add(settings6 *iface, void *handler, token *out) { return add_handler(handler, out); }

static const __x_ABI_CWindows_CUI_CViewManagement_CIUISettingsVtbl one_table = {one_query, one_reference, one_release, one_iids, one_name,
	one_trust, one_hand, one_cursor, one_scroll_bar, one_scroll_arrow, one_scroll_thumb, one_message, one_animations, one_caret_browsing,
	one_caret_blink, one_caret_width, one_double_click, one_hover, one_element};
static const __x_ABI_CWindows_CUI_CViewManagement_CIUISettings2Vtbl two_table = {two_query, two_reference, two_release, two_iids, two_name,
	two_trust, two_scale, (void *)two_add, (void *)remove_handler};
static const __x_ABI_CWindows_CUI_CViewManagement_CIUISettings3Vtbl three_table = {three_query, three_reference, three_release, three_iids,
	three_name, three_trust, three_value, (void *)three_add, (void *)remove_handler};
static const __x_ABI_CWindows_CUI_CViewManagement_CIUISettings4Vtbl four_table = {four_query, four_reference, four_release, four_iids,
	four_name, four_trust, four_effects, (void *)four_add, (void *)remove_handler};
static const __x_ABI_CWindows_CUI_CViewManagement_CIUISettings5Vtbl five_table = {five_query, five_reference, five_release, five_iids,
	five_name, five_trust, five_hide, (void *)five_add, (void *)remove_handler};
static const __x_ABI_CWindows_CUI_CViewManagement_CIUISettings6Vtbl six_table = {six_query, six_reference, six_release, six_iids, six_name,
	six_trust, (void *)six_add, (void *)remove_handler, (void *)six_add, (void *)remove_handler};

/* The activation factory of the class: each activation makes a new settings object. */
static HRESULT WINAPI factory_query(IActivationFactory *iface, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject) ||
		IsEqualIID(iid, &IID_IActivationFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_reference(IActivationFactory *iface) { return 2; }
static ULONG WINAPI factory_release(IActivationFactory *iface) { return 1; }
static HRESULT WINAPI factory_iids(IActivationFactory *iface, ULONG *count, IID **iids) { return inspectable_iids(count, iids); }
static HRESULT WINAPI factory_name(IActivationFactory *iface, HSTRING *name) { return inspectable_name(name); }
static HRESULT WINAPI factory_trust(IActivationFactory *iface, TrustLevel *level) { return inspectable_trust(level); }

static HRESULT WINAPI factory_activate(IActivationFactory *iface, IInspectable **out)
{
	struct settings *self;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!(self = ALLOC(sizeof *self)))
		return E_OUTOFMEMORY;
	self->one.lpVtbl = (__x_ABI_CWindows_CUI_CViewManagement_CIUISettingsVtbl *)&one_table;
	self->two.lpVtbl = (__x_ABI_CWindows_CUI_CViewManagement_CIUISettings2Vtbl *)&two_table;
	self->three.lpVtbl = (__x_ABI_CWindows_CUI_CViewManagement_CIUISettings3Vtbl *)&three_table;
	self->four.lpVtbl = (__x_ABI_CWindows_CUI_CViewManagement_CIUISettings4Vtbl *)&four_table;
	self->five.lpVtbl = (__x_ABI_CWindows_CUI_CViewManagement_CIUISettings5Vtbl *)&five_table;
	self->six.lpVtbl = (__x_ABI_CWindows_CUI_CViewManagement_CIUISettings6Vtbl *)&six_table;
	self->refs = 1;
	InterlockedIncrement(&objects);
	*out = (IInspectable *)&self->one;
	return S_OK;
}

static IActivationFactoryVtbl factory_table = {factory_query, factory_reference, factory_release, factory_iids, factory_name, factory_trust,
	factory_activate};
static IActivationFactory factory = {&factory_table};

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
	UINT32 length = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	if (length != ARRAYSIZE(class_name) - 1 || CompareStringOrdinal(text, (int)length, class_name, -1, FALSE) != CSTR_EQUAL)
		return CLASS_E_CLASSNOTAVAILABLE;
	*out = &factory;
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
