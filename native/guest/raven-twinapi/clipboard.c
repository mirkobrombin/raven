/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.ApplicationModel.DataTransfer: DataPackage and DataPackageView, the data of a copy, a drag or a share by format; Clipboard,
 * which puts a package on the Windows clipboard and reads it back; StandardDataFormats, HtmlFormatHelper and ClipboardContentOptions.
 *
 * - A package keeps a value or a DataProviderHandler for each format, and its properties are a map from names to values, Title,
 *   Description and the others among them, as DataPackagePropertySet is on Windows. A view is a snapshot of them; a value a provider
 *   gives is asked for each time a view is asked, on the operation's thread, waiting for a deferral it hands out.
 * - Clipboard.SetContent renders every format into the clipboard at once, a provider's too: text as CF_UNICODETEXT, HTML as
 *   "HTML Format" in UTF-8, RTF as "Rich Text Format", a web link as "UniformResourceLocatorW", an application link as
 *   "ApplicationLink", files and folders as CF_HDROP, a bitmap as CF_DIB when it is a BMP file and as "PNG" otherwise, and a format of the
 *   app's own as a clipboard format of that name, a string in UTF-16 and a stream or buffer as its bytes. Flush then has nothing left to
 *   do. While the clipboard still holds what this process put there, GetContent gives the package's own view; otherwise a view of what
 *   the clipboard has, each format read when it is asked for.
 * - ContentChanged comes from a message-only window of a thread of its own that listens for WM_CLIPBOARDUPDATE.
 * - There is no clipboard history and no roaming: history is reported disabled and empty. Enterprise data protection allows every
 *   access. A package's Destroyed, ShareCompleted and ShareCanceled handlers are kept; nothing shares or destroys a package here. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <roapi.h>
#include <shellapi.h>
#include <shlobj.h>

static const IID iid_clipboard_statics = {0xc627e291, 0x34e2, 0x4963, {0x8e, 0xed, 0x93, 0xcb, 0xb0, 0xea, 0x3d, 0x70}};
static const IID iid_clipboard_statics2 = {0xd2ac1b6a, 0xd29f, 0x554b, {0xb3, 0x03, 0xf0, 0x45, 0x23, 0x45, 0xfe, 0x02}};
static const IID iid_package = {0x61ebf5c7, 0xefea, 0x4346, {0x95, 0x54, 0x98, 0x1d, 0x7e, 0x19, 0x8f, 0xfe}};
static const IID iid_package2 = {0x041c1fe9, 0x2409, 0x45e1, {0xa5, 0x38, 0x4c, 0x53, 0xee, 0xee, 0x04, 0xa7}};
static const IID iid_package3 = {0x88f31f5d, 0x787b, 0x4d32, {0x96, 0x5a, 0xa9, 0x83, 0x81, 0x05, 0xa0, 0x56}};
static const IID iid_package4 = {0x13a24ec8, 0x9382, 0x536f, {0x85, 0x2a, 0x30, 0x45, 0xe1, 0xb2, 0x9a, 0x3b}};
static const IID iid_view = {0x7b840471, 0x5900, 0x4d85, {0xa9, 0x0b, 0x10, 0xcb, 0x85, 0xfe, 0x35, 0x52}};
static const IID iid_view2 = {0x40ecba95, 0x2450, 0x4c1d, {0xb6, 0xb4, 0xed, 0x45, 0x46, 0x3d, 0xee, 0x9c}};
static const IID iid_view3 = {0xd37771a8, 0xddad, 0x4288, {0x84, 0x28, 0xd1, 0xca, 0xe3, 0x94, 0x12, 0x8b}};
static const IID iid_view4 = {0xdfe96f1f, 0xe042, 0x4433, {0xa0, 0x9f, 0x26, 0xd6, 0xff, 0xda, 0x8b, 0x85}};
static const IID iid_set1 = {0xcd1c93eb, 0x4c4c, 0x443a, {0xa8, 0xd3, 0xf5, 0xc2, 0x41, 0xe9, 0x16, 0x89}};
static const IID iid_set2 = {0xeb505d4a, 0x9800, 0x46aa, {0xb1, 0x81, 0x7b, 0x6f, 0x0f, 0x2b, 0x91, 0x9a}};
static const IID iid_set3 = {0x9e87fd9b, 0x5205, 0x401b, {0x87, 0x4a, 0x45, 0x56, 0x53, 0xbd, 0x39, 0xe8}};
static const IID iid_set4 = {0x6390ebf5, 0x1739, 0x4c74, {0xb2, 0x2f, 0x86, 0x5f, 0xab, 0x5e, 0x85, 0x45}};
static const IID iid_set_view1 = {0xb94cec01, 0x0c1a, 0x4c57, {0xbe, 0x55, 0x75, 0xd0, 0x12, 0x89, 0x73, 0x5d}};
static const IID iid_set_view2 = {0x6054509b, 0x8ebe, 0x4feb, {0x9c, 0x1e, 0x75, 0xe6, 0x9d, 0xe5, 0x4b, 0x84}};
static const IID iid_set_view3 = {0xdb764ce5, 0xd174, 0x495c, {0x84, 0xfc, 0x1a, 0x51, 0xf6, 0xab, 0x45, 0xd7}};
static const IID iid_set_view4 = {0x4474c80d, 0xd16f, 0x40ae, {0x95, 0x80, 0x6f, 0x85, 0x62, 0xb9, 0x42, 0x35}};
static const IID iid_set_view5 = {0x6f0a9445, 0x3760, 0x50bb, {0x85, 0x23, 0xc4, 0x20, 0x2d, 0xed, 0x7d, 0x78}};
static const IID iid_formats_statics = {0x7ed681a1, 0xa880, 0x40c9, {0xb4, 0xed, 0x0b, 0xee, 0x1e, 0x15, 0xf5, 0x49}};
static const IID iid_formats_statics2 = {0x42a254f4, 0x9d76, 0x42e8, {0x86, 0x1b, 0x47, 0xc2, 0x5d, 0xd0, 0xcf, 0x71}};
static const IID iid_formats_statics3 = {0x3b57b069, 0x01d4, 0x474c, {0x8b, 0x5f, 0xbc, 0x8e, 0x27, 0xf3, 0x8b, 0x21}};
static const IID iid_html_statics = {0xe22e7749, 0xdd70, 0x446f, {0xae, 0xfc, 0x61, 0xce, 0xe5, 0x9f, 0x65, 0x5e}};
static const IID iid_completed_args = {0xe7af329d, 0x051d, 0x4fab, {0xb1, 0xa9, 0x47, 0xfd, 0x77, 0xf7, 0x0a, 0x41}};
static const IID iid_completed_args2 = {0x858fa073, 0x1e19, 0x4105, {0xb2, 0xf7, 0xc8, 0x47, 0x88, 0x08, 0xd5, 0x62}};
static const IID iid_request = {0xebbc7157, 0xd3c8, 0x47da, {0xac, 0xde, 0xf8, 0x23, 0x88, 0xd5, 0xf7, 0x16}};
static const IID iid_deferral = {0xc2cf2373, 0x2d26, 0x43d9, {0xb6, 0x9d, 0xdc, 0xb8, 0x6d, 0x03, 0xf6, 0xda}};
static const IID iid_content_options = {0xe888a98c, 0xad4b, 0x5447, {0xa0, 0x56, 0xab, 0x35, 0x56, 0x27, 0x6d, 0x2b}};
static const IID iid_history_result = {0xe6dfdee6, 0x0ee2, 0x52e3, {0x85, 0x2b, 0xf2, 0x95, 0xdb, 0x65, 0x93, 0x9a}};
static const IID iid_storage_item = {0x4207a996, 0xca2f, 0x42f7, {0xbd, 0xe8, 0x8b, 0x10, 0x45, 0x7a, 0x7f, 0x30}};
static const IID iid_storage_items_iterable = {0xbb8b8418, 0x65d1, 0x544b, {0xb0, 0x83, 0x6d, 0x17, 0x2f, 0x56, 0x8c, 0x73}};
static const IID iid_uri_factory = {0x44a9796f, 0x723e, 0x4fdf, {0xa2, 0x18, 0x03, 0x3e, 0x75, 0xb0, 0xc0, 0x84}};
static const IID iid_buffer = {0x905a0fe0, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const IID iid_stream_reference = {0x33ee3134, 0x1dd6, 0x4e3a, {0x80, 0x67, 0xd1, 0xc1, 0x62, 0xe8, 0x64, 0x2b}};
static const IID iid_uri = {0x9e365e57, 0x48b2, 0x4160, {0x95, 0x6f, 0xc7, 0x38, 0x51, 0x20, 0xbb, 0xfc}};
static const IID iid_random_access_stream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
/* IAsyncOperation of Object, String, Uri, RandomAccessStreamReference, IMapView<String, RandomAccessStreamReference>,
 * ProtectionPolicyEvaluationResult and ClipboardHistoryItemsResult. */
static const IID iid_object_operation = {0xabf53c57, 0xee50, 0x5342, {0xb5, 0x2a, 0x26, 0xe3, 0xb8, 0xcc, 0x02, 0x4f}};
static const IID iid_string_operation = {0x3e1fe603, 0xf897, 0x5263, {0xb3, 0x28, 0x08, 0x06, 0x42, 0x6b, 0x8a, 0x79}};
static const IID iid_uri_operation = {0x641cb9dd, 0xa28d, 0x59e2, {0xb8, 0xdb, 0xa2, 0x27, 0xed, 0xa6, 0xcf, 0x2e}};
static const IID iid_reference_operation = {0xd90442ca, 0x543c, 0x504b, {0x9e, 0xb9, 0x29, 0x4b, 0xca, 0xd8, 0xa2, 0x83}};
static const IID iid_resources_operation = {0xfc012d44, 0x2dcf, 0x5162, {0xbe, 0x9a, 0x76, 0x68, 0x67, 0x5a, 0xa5, 0x90}};
static const IID iid_policy_operation = {0xe8d81715, 0xc56c, 0x5a6b, {0xb7, 0x38, 0x5d, 0xf6, 0xc2, 0x77, 0x5b, 0x7b}};
static const IID iid_history_operation = {0x677b06b8, 0x0134, 0x5692, {0xb4, 0x87, 0x4c, 0x8e, 0x24, 0x08, 0xca, 0x01}};
/* IMap<String, RandomAccessStreamReference>, its view, pair, IIterable and IIterator. */
static const IID iid_resource_map = {0xe5d2ccfc, 0x825a, 0x5a8e, {0x82, 0xaa, 0x09, 0x5e, 0xd5, 0xdb, 0xd5, 0xd1}};
static const IID iid_resource_view = {0x0a4ce7a5, 0xdfe0, 0x5796, {0xa4, 0x38, 0xef, 0xfd, 0xfa, 0xa3, 0x1f, 0x1b}};
static const IID iid_resource_pair = {0xe5195792, 0xaeab, 0x56e8, {0xbd, 0x30, 0x13, 0x72, 0xc4, 0x34, 0x0b, 0xf6}};
static const IID iid_resource_iterable = {0xc9729ba7, 0x5e20, 0x569d, {0xa3, 0xd1, 0x97, 0xa4, 0xe6, 0x53, 0xe5, 0xbb}};
static const IID iid_resource_iterator = {0x9419af53, 0xacb8, 0x5328, {0x88, 0x53, 0x70, 0xba, 0x87, 0xeb, 0x6a, 0xd5}};
/* IReference<Windows.UI.Color>. */
static const IID iid_color_reference = {0xab8e5d11, 0xb0c1, 0x5a21, {0x95, 0xae, 0xf1, 0x6b, 0xf3, 0xa3, 0x76, 0x24}};
/* IVectorView, IIterable and IIterator of ClipboardHistoryItem. */
static const IID iid_history_view = {0xc91de16d, 0x13ff, 0x55b0, {0x89, 0x7d, 0x18, 0xa1, 0x31, 0xd5, 0x7a, 0xc9}};
static const IID iid_history_iterable = {0x5c3705f4, 0x8e46, 0x5ae5, {0x97, 0xbf, 0x88, 0x22, 0x0c, 0xcc, 0xff, 0xd9}};
static const IID iid_history_iterator = {0x63fac521, 0x1395, 0x5c95, {0xaa, 0xff, 0x97, 0x36, 0x37, 0x8a, 0x4f, 0x2f}};
static const struct winrt_items_kind history_list = {&iid_history_view, &iid_history_iterable, &iid_history_iterator, FALSE, NULL};

/* What Windows answers for a format the data does not have. */
#define NO_FORMAT ((HRESULT)0x80040064)
#define PROPERTY_STRING 12
#define WM_CLIPBOARD_UPDATE 0x031d

/* The formats of StandardDataFormats; Uri and WebLink are the same format. */
static const WCHAR text_format[] = L"Text", link_format[] = L"UniformResourceLocatorW", html_format[] = L"HTML Format",
	rtf_format[] = L"Rich Text Format", bitmap_format[] = L"Bitmap", items_format[] = L"Shell IDList Array",
	application_link_format[] = L"ApplicationLink", activity_format[] = L"UserActivityJsonArray";

typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClass uri_type;

static HSTRING name_of(const WCHAR *text)
{
	HSTRING made = NULL;
	WindowsCreateString(text, lstrlenW(text), &made);
	return made;
}

static BOOL is_format(HSTRING id, const WCHAR *name)
{
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(id, &length);
	return length == (UINT32)lstrlenW(name) && CompareStringOrdinal(text, (int)length, name, (int)length, FALSE) == CSTR_EQUAL;
}

static HRESULT text_box(HSTRING text, IInspectable **out) { return property_value_make(PROPERTY_STRING, &text, (void **)out); }

static HRESULT text_box_of(const WCHAR *text, UINT32 length, IInspectable **out)
{
	HSTRING string = NULL;
	HRESULT result = WindowsCreateString(text, length, &string);
	if (SUCCEEDED(result))
		result = text_box(string, out);
	WindowsDeleteString(string);
	return result;
}

static HRESULT make_uri(const WCHAR *text, UINT32 length, IInspectable **out)
{
	__x_ABI_CWindows_CFoundation_CIUriRuntimeClassFactory *factory = NULL;
	HSTRING class_name = name_of(L"Windows.Foundation.Uri"), address = NULL;
	*out = NULL;
	HRESULT result = RoGetActivationFactory(class_name, &iid_uri_factory, (void **)&factory);
	if (SUCCEEDED(result))
		result = WindowsCreateString(text, length, &address);
	if (SUCCEEDED(result))
		result = factory->lpVtbl->CreateUri(factory, address, (uri_type **)out);
	WindowsDeleteString(address);
	WindowsDeleteString(class_name);
	release(factory);
	return result;
}

/* ---- the properties: a map whose Title, Description and other members are keys ---- */

static HRESULT key_lookup(void *iface, const WCHAR *key, IInspectable **out)
{
	HSTRING name = name_of(key);
	const HRESULT result = map_lookup_any(iface, name, out);
	WindowsDeleteString(name);
	return result;
}

static HRESULT key_insert(void *iface, const WCHAR *key, IInspectable *value)
{
	HSTRING name = name_of(key);
	const HRESULT result = map_insert_any(iface, name, value);
	WindowsDeleteString(name);
	return result;
}

static HRESULT get_text_key(void *iface, const WCHAR *key, HSTRING *out)
{
	IInspectable *value = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (SUCCEEDED(key_lookup(iface, key, &value)) && value)
		property_value_string((IUnknown *)value, out);
	release(value);
	return S_OK;
}

static HRESULT put_text_key(void *iface, const WCHAR *key, HSTRING text)
{
	IInspectable *box = NULL;
	HRESULT result = text_box(text, &box);
	if (SUCCEEDED(result))
		result = key_insert(iface, key, box);
	release(box);
	return result;
}

static HRESULT get_object_key(void *iface, const WCHAR *key, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (FAILED(key_lookup(iface, key, (IInspectable **)out)))
		*out = NULL;
	return S_OK;
}

static HRESULT put_object_key(void *iface, const WCHAR *key, IInspectable *value) { return key_insert(iface, key, value); }

/* LogoBackgroundColor: a Windows.UI.Color, four bytes, kept boxed. */
static HRESULT get_color_key(void *iface, UINT32 *out)
{
	IInspectable *value = NULL;
	IUnknown *box = NULL;
	if (!out)
		return E_POINTER;
	*out = 0;
	if (SUCCEEDED(key_lookup(iface, L"LogoBackgroundColor", &value)) && value &&
		SUCCEEDED(value->lpVtbl->QueryInterface(value, &iid_color_reference, (void **)&box))) {
		((HRESULT (WINAPI *)(void *, UINT32 *))method(box, 6))(box, out);
		release(box);
	}
	release(value);
	return S_OK;
}

static HRESULT put_color_key(void *iface, UINT32 color)
{
	void *box = NULL;
	HRESULT result = winrt_box(&iid_color_reference, &color, sizeof color, &box);
	if (SUCCEEDED(result))
		result = key_insert(iface, L"LogoBackgroundColor", box);
	release(box);
	return result;
}

/* FileTypes: an IVector<String> in the set, made the first time it is asked for, and its view in a view of the set. */
static HRESULT WINAPI set_file_types(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (SUCCEEDED(key_lookup(iface, L"FileTypes", (IInspectable **)out)) && *out)
		return S_OK;
	HRESULT result = winrt_vector(&string_list, NULL, 0, out);
	if (SUCCEEDED(result) && FAILED(result = key_insert(iface, L"FileTypes", *out))) {
		release(*out);
		*out = NULL;
	}
	return result;
}

static HRESULT WINAPI view_file_types(void *iface, void **out)
{
	IInspectable *types = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (SUCCEEDED(key_lookup(iface, L"FileTypes", &types)) && types) {
		const HRESULT result = ((HRESULT (WINAPI *)(void *, void **))method(types, 8))(types, out);
		release(types);
		return result;
	}
	return winrt_items(&string_list, NULL, 0, FALSE, out);
}

#define TEXT_PROPERTY(getter, setter, key) \
	static HRESULT WINAPI getter(void *iface, HSTRING *out) { return get_text_key(iface, key, out); } \
	static HRESULT WINAPI setter(void *iface, HSTRING value) { return put_text_key(iface, key, value); }
#define OBJECT_PROPERTY(getter, setter, key) \
	static HRESULT WINAPI getter(void *iface, void **out) { return get_object_key(iface, key, out); } \
	static HRESULT WINAPI setter(void *iface, IInspectable *value) { return put_object_key(iface, key, value); }

TEXT_PROPERTY(title, set_title, L"Title")
TEXT_PROPERTY(description, set_description, L"Description")
OBJECT_PROPERTY(thumbnail, set_thumbnail, L"Thumbnail")
TEXT_PROPERTY(application_name, set_application_name, L"ApplicationName")
OBJECT_PROPERTY(listing_uri, set_listing_uri, L"ApplicationListingUri")
OBJECT_PROPERTY(source_web_link, set_source_web_link, L"ContentSourceWebLink")
OBJECT_PROPERTY(source_application_link, set_source_application_link, L"ContentSourceApplicationLink")
TEXT_PROPERTY(family_name, set_family_name, L"PackageFamilyName")
OBJECT_PROPERTY(logo, set_logo, L"Square30x30Logo")
TEXT_PROPERTY(enterprise_id, set_enterprise_id, L"EnterpriseId")
TEXT_PROPERTY(activity_json, set_activity_json, L"ContentSourceUserActivityJson")

static HRESULT WINAPI logo_color(void *iface, UINT32 *out) { return get_color_key(iface, out); }
static HRESULT WINAPI set_logo_color(void *iface, UINT32 value) { return put_color_key(iface, value); }

static HRESULT WINAPI from_roaming(void *iface, boolean *out) { return answer_flag(out, FALSE); }

TABLE(set1_table, 3, INSPECTABLE, (const void *)title, (const void *)set_title, (const void *)description, (const void *)set_description,
	(const void *)thumbnail, (const void *)set_thumbnail, (const void *)set_file_types, (const void *)application_name,
	(const void *)set_application_name, (const void *)listing_uri, (const void *)set_listing_uri);
TABLE(set2_table, 4, INSPECTABLE, (const void *)source_web_link, (const void *)set_source_web_link, (const void *)source_application_link,
	(const void *)set_source_application_link, (const void *)family_name, (const void *)set_family_name, (const void *)logo,
	(const void *)set_logo, (const void *)logo_color, (const void *)set_logo_color);
TABLE(set3_table, 5, INSPECTABLE, (const void *)enterprise_id, (const void *)set_enterprise_id);
TABLE(set4_table, 6, INSPECTABLE, (const void *)activity_json, (const void *)set_activity_json);
TABLE(set_view1_table, 2, INSPECTABLE, (const void *)title, (const void *)description, (const void *)thumbnail, (const void *)view_file_types,
	(const void *)application_name, (const void *)listing_uri);
TABLE(set_view2_table, 3, INSPECTABLE, (const void *)family_name, (const void *)source_web_link, (const void *)source_application_link,
	(const void *)logo, (const void *)logo_color);
TABLE(set_view3_table, 4, INSPECTABLE, (const void *)enterprise_id);
TABLE(set_view4_table, 5, INSPECTABLE, (const void *)activity_json);
TABLE(set_view5_table, 6, INSPECTABLE, (const void *)from_roaming);
static const struct map_face set_faces[] = {{&iid_set1, set1_table.slots}, {&iid_set2, set2_table.slots}, {&iid_set3, set3_table.slots},
	{&iid_set4, set4_table.slots}};
static const struct map_face set_view_faces[] = {{&iid_set_view1, set_view1_table.slots}, {&iid_set_view2, set_view2_table.slots},
	{&iid_set_view3, set_view3_table.slots}, {&iid_set_view4, set_view4_table.slots}, {&iid_set_view5, set_view5_table.slots}};
static struct map_types property_types = {&iid_property_view, &iid_property_iterable, &iid_property_iterator, &iid_property_pair,
	L"Windows.ApplicationModel.DataTransfer.DataPackagePropertySetView", L"Windows.Foundation.Collections.IKeyValuePair`2<String, Object>",
	L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, Object>>", &iid_property_map,
	L"Windows.ApplicationModel.DataTransfer.DataPackagePropertySet", set_faces, set_view_faces, ARRAYSIZE(set_faces),
	ARRAYSIZE(set_view_faces), "DataPackagePropertySet", "DataPackagePropertySetView"};
/* A package's ResourceMap: IMap<String, RandomAccessStreamReference>. */
static struct map_types resource_types = {&iid_resource_view, &iid_resource_iterable, &iid_resource_iterator, &iid_resource_pair,
	L"Windows.Foundation.Collections.IMapView`2<String, Windows.Storage.Streams.RandomAccessStreamReference>",
	L"Windows.Foundation.Collections.IKeyValuePair`2<String, Windows.Storage.Streams.RandomAccessStreamReference>",
	L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, "
	L"Windows.Storage.Streams.RandomAccessStreamReference>>", &iid_resource_map,
	L"Windows.Foundation.Collections.IMap`2<String, Windows.Storage.Streams.RandomAccessStreamReference>"};
static INIT_ONCE types_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK make_types(INIT_ONCE *once, void *parameter, void **context)
{
	map_types_init(&property_types);
	map_types_init(&resource_types);
	return TRUE;
}

static void types_ready(void) { InitOnceExecuteOnce(&types_once, make_types, NULL, NULL); }

/* The IDataPackagePropertySet of a set, and the IDataPackagePropertySetView of its snapshot. */
static HRESULT set_face(struct property_set *set, void **out)
{
	IUnknown *map = map_face(set);
	return map->lpVtbl->QueryInterface(map, &iid_set1, out);
}

static HRESULT set_view_face(struct property_set *set, void **out)
{
	IUnknown *view = NULL;
	*out = NULL;
	HRESULT result = property_set_view(set, (void **)&view);
	if (SUCCEEDED(result))
		result = view->lpVtbl->QueryInterface(view, &iid_set_view1, out);
	release(view);
	return result;
}

/* ---- packages ---- */

/* A format with its value, or with the DataProviderHandler that gives it. */
struct format {
	HSTRING id;
	IInspectable *value;
	IUnknown *provider;
};

static void formats_free(struct format *formats, UINT32 count)
{
	for (UINT32 at = 0; at < count; ++at) {
		WindowsDeleteString(formats[at].id);
		release(formats[at].value);
		release(formats[at].provider);
	}
	FREE(formats);
}

static struct format *formats_copy(const struct format *formats, UINT32 count)
{
	struct format *copy = ALLOC((count ? count : 1) * sizeof *copy);
	if (!copy)
		return NULL;
	for (UINT32 at = 0; at < count; ++at) {
		WindowsDuplicateString(formats[at].id, &copy[at].id);
		reference(copy[at].value = formats[at].value);
		reference(copy[at].provider = formats[at].provider);
	}
	return copy;
}

static const struct format *find_format(const struct format *formats, UINT32 count, HSTRING id)
{
	for (UINT32 at = 0; at < count; ++at) {
		INT32 order = 1;
		if (SUCCEEDED(WindowsCompareStringOrdinal(formats[at].id, id, &order)) && !order)
			return &formats[at];
	}
	return NULL;
}

struct package {
	struct object base;
	SRWLOCK lock;
	struct format *formats;
	UINT32 count, capacity;
	struct property_set *properties, *resources;
	INT32 operation;
	struct handlers completed, destroyed, share_completed, share_canceled;
};

static struct package *package_of(void *iface) { return (struct package *)owner(iface); }

static void destroy_package(struct object *base)
{
	struct package *self = (struct package *)base;
	formats_free(self->formats, self->count);
	if (self->properties)
		release(map_face(self->properties));
	if (self->resources)
		release(map_face(self->resources));
	handlers_clear(&self->completed);
	handlers_clear(&self->destroyed);
	handlers_clear(&self->share_completed);
	handlers_clear(&self->share_canceled);
}

/* Sets the value or the provider of a format, in place of what it had. */
static HRESULT set_format(struct package *self, HSTRING id, IInspectable *value, IUnknown *provider)
{
	HSTRING copy = NULL;
	if (!WindowsGetStringLen(id))
		return E_INVALIDARG;
	HRESULT result = WindowsDuplicateString(id, &copy);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&self->lock);
	struct format *found = (struct format *)find_format(self->formats, self->count, id);
	if (!found && self->count == self->capacity) {
		const UINT32 capacity = self->capacity ? self->capacity * 2 : 8;
		struct format *grown = self->formats ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, self->formats, capacity * sizeof *grown) :
			ALLOC(capacity * sizeof *grown);
		if (grown) {
			self->formats = grown;
			self->capacity = capacity;
		} else
			result = E_OUTOFMEMORY;
	}
	IInspectable *old_value = NULL;
	IUnknown *old_provider = NULL;
	if (SUCCEEDED(result)) {
		if (!found)
			found = &self->formats[self->count++];
		else
			WindowsDeleteString(found->id);
		old_value = found->value;
		old_provider = found->provider;
		found->id = copy;
		reference(found->value = value);
		reference(found->provider = provider);
		copy = NULL;
	}
	ReleaseSRWLockExclusive(&self->lock);
	WindowsDeleteString(copy);
	release(old_value);
	release(old_provider);
	return result;
}

static HRESULT set_named(struct package *self, const WCHAR *name, IInspectable *value)
{
	HSTRING id = name_of(name);
	const HRESULT result = value ? set_format(self, id, value, NULL) : E_INVALIDARG;
	WindowsDeleteString(id);
	return result;
}

static HRESULT set_named_text(struct package *self, const WCHAR *name, HSTRING text)
{
	IInspectable *box = NULL;
	HRESULT result = text_box(text, &box);
	if (SUCCEEDED(result))
		result = set_named(self, name, box);
	release(box);
	return result;
}

static HRESULT make_view(struct package *package, const struct format *formats, UINT32 count, DWORD sequence, void **out);

static HRESULT WINAPI package_view(void *iface, void **out)
{
	struct package *self = package_of(iface);
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = make_view(self, self->formats, self->count, 0, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT WINAPI package_properties(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	return set_face(package_of(iface)->properties, out);
}

static HRESULT WINAPI package_operation(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = package_of(iface)->operation;
	return S_OK;
}

static HRESULT WINAPI set_package_operation(void *iface, INT32 value) { package_of(iface)->operation = value; return S_OK; }

#define PACKAGE_EVENT(add, remove, field) \
	static HRESULT WINAPI add(void *iface, void *handler, INT64 *token) \
	{ \
		struct package *self = package_of(iface); \
		AcquireSRWLockExclusive(&self->lock); \
		const HRESULT result = handlers_add(&self->field, handler, token); \
		ReleaseSRWLockExclusive(&self->lock); \
		return result; \
	} \
	static HRESULT WINAPI remove(void *iface, INT64 token) \
	{ \
		struct package *self = package_of(iface); \
		AcquireSRWLockExclusive(&self->lock); \
		const HRESULT result = handlers_remove(&self->field, token); \
		ReleaseSRWLockExclusive(&self->lock); \
		return result; \
	}

PACKAGE_EVENT(completed_add, completed_remove, completed)
PACKAGE_EVENT(destroyed_add, destroyed_remove, destroyed)
PACKAGE_EVENT(share_completed_add, share_completed_remove, share_completed)
PACKAGE_EVENT(share_canceled_add, share_canceled_remove, share_canceled)

static HRESULT WINAPI set_data(void *iface, HSTRING id, IInspectable *value) { return set_format(package_of(iface), id, value, NULL); }

static HRESULT WINAPI set_provider(void *iface, HSTRING id, IUnknown *provider)
{
	return provider ? set_format(package_of(iface), id, NULL, provider) : E_INVALIDARG;
}

static HRESULT WINAPI set_text(void *iface, HSTRING text) { return set_named_text(package_of(iface), text_format, text); }
static HRESULT WINAPI set_uri(void *iface, IInspectable *uri) { return set_named(package_of(iface), link_format, uri); }
static HRESULT WINAPI set_html(void *iface, HSTRING html) { return set_named_text(package_of(iface), html_format, html); }
static HRESULT WINAPI set_rtf(void *iface, HSTRING rtf) { return set_named_text(package_of(iface), rtf_format, rtf); }
static HRESULT WINAPI set_bitmap(void *iface, IInspectable *bitmap) { return set_named(package_of(iface), bitmap_format, bitmap); }
static HRESULT WINAPI set_items(void *iface, IInspectable *items) { return set_named(package_of(iface), items_format, items); }
static HRESULT WINAPI set_items_with(void *iface, IInspectable *items, boolean read_only) { return set_items(iface, items); }
static HRESULT WINAPI set_application_link(void *iface, IInspectable *uri) { return set_named(package_of(iface), application_link_format, uri); }
static HRESULT WINAPI set_web_link(void *iface, IInspectable *uri) { return set_named(package_of(iface), link_format, uri); }

static HRESULT WINAPI package_resources(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = map_face(package_of(iface)->resources));
	return S_OK;
}

TABLE(package_table, 0, INSPECTABLE, (const void *)package_view, (const void *)package_properties, (const void *)package_operation,
	(const void *)set_package_operation, (const void *)completed_add, (const void *)completed_remove, (const void *)destroyed_add,
	(const void *)destroyed_remove, (const void *)set_data, (const void *)set_provider, (const void *)set_text, (const void *)set_uri,
	(const void *)set_html, (const void *)package_resources, (const void *)set_rtf, (const void *)set_bitmap, (const void *)set_items,
	(const void *)set_items_with);
TABLE(package2_table, 1, INSPECTABLE, (const void *)set_application_link, (const void *)set_web_link);
TABLE(package3_table, 2, INSPECTABLE, (const void *)share_completed_add, (const void *)share_completed_remove);
TABLE(package4_table, 3, INSPECTABLE, (const void *)share_canceled_add, (const void *)share_canceled_remove);
static const struct face_iid package_iids[] = {{&iid_package, 0}, {&iid_package2, 1}, {&iid_package3, 2}, {&iid_package4, 3}, {NULL, 0}};
static const struct kind package_kind = {"DataPackage", L"Windows.ApplicationModel.DataTransfer.DataPackage", package_iids, destroy_package};

static HRESULT package_make(struct package **out)
{
	static const void *const *const tables[] = {package_table.slots, package2_table.slots, package3_table.slots, package4_table.slots};
	types_ready();
	struct package *self = make(sizeof *self, &package_kind, tables, 4);
	if (!(*out = self))
		return E_OUTOFMEMORY;
	InitializeSRWLock(&self->lock);
	self->operation = 1;
	self->properties = typed_map_make(&property_types);
	self->resources = typed_map_make(&resource_types);
	if (!self->properties || !self->resources) {
		release(face_of(&self->base, 0));
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	return S_OK;
}

static HRESULT WINAPI package_activate(void *iface, IInspectable **out)
{
	struct package *self;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = package_make(&self);
	if (SUCCEEDED(result))
		*out = face_of(&self->base, 0);
	return result;
}

TABLE(package_factory_table, 0, INSPECTABLE, (const void *)package_activate);
static const struct face_iid package_class_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind package_class_kind = {"DataPackage", L"Windows.ApplicationModel.DataTransfer.DataPackage", package_class_iids, NULL};
struct object data_package_class = {{package_factory_table.slots}, &package_class_kind, 1};

/* ---- a provider's request, with its deferral ---- */

struct request {
	struct object base;
	HSTRING id;
	INT64 deadline;
	SRWLOCK lock;
	IInspectable *value;
	LONG deferrals;
	HANDLE completed;
};

struct deferral {
	struct object base;
	struct request *request;
	LONG done;
};

static void destroy_request(struct object *base)
{
	struct request *self = (struct request *)base;
	WindowsDeleteString(self->id);
	release(self->value);
	CloseHandle(self->completed);
}

static void destroy_deferral(struct object *base) { release(face_of(&((struct deferral *)base)->request->base, 0)); }

static HRESULT WINAPI deferral_complete(void *iface)
{
	struct deferral *self = (struct deferral *)owner(iface);
	if (!InterlockedExchange(&self->done, 1) && !InterlockedDecrement(&self->request->deferrals))
		SetEvent(self->request->completed);
	return S_OK;
}

TABLE(deferral_table, 0, INSPECTABLE, (const void *)deferral_complete);
static const struct face_iid deferral_iids[] = {{&iid_deferral, 0}, {NULL, 0}};
static const struct kind deferral_kind = {"DataProviderDeferral", L"Windows.ApplicationModel.DataTransfer.DataProviderDeferral",
	deferral_iids, destroy_deferral};

static HRESULT WINAPI request_id(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct request *)owner(iface))->id, out);
}

static HRESULT WINAPI request_deadline(void *iface, INT64 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct request *)owner(iface))->deadline;
	return S_OK;
}

static HRESULT WINAPI request_deferral(void *iface, void **out)
{
	static const void *const *const tables[] = {deferral_table.slots};
	struct request *request = (struct request *)owner(iface);
	if (!out)
		return E_POINTER;
	struct deferral *self = make(sizeof *self, &deferral_kind, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	self->request = request;
	reference(face_of(&request->base, 0));
	InterlockedIncrement(&request->deferrals);
	return S_OK;
}

static HRESULT WINAPI request_set_data(void *iface, IInspectable *value)
{
	struct request *self = (struct request *)owner(iface);
	reference(value);
	AcquireSRWLockExclusive(&self->lock);
	IInspectable *old = self->value;
	self->value = value;
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	return S_OK;
}

TABLE(request_table, 0, INSPECTABLE, (const void *)request_id, (const void *)request_deadline, (const void *)request_deferral,
	(const void *)request_set_data);
static const struct face_iid request_iids[] = {{&iid_request, 0}, {NULL, 0}};
static const struct kind request_kind = {"DataProviderRequest", L"Windows.ApplicationModel.DataTransfer.DataProviderRequest", request_iids,
	destroy_request};

/* How long a provider has to give its data, in 100 ns units. */
#define PROVIDER_TIME 300000000LL

/* Asks a provider for the value of a format: it may set it at once or later, through a deferral it completes in time. */
static HRESULT ask_provider(IUnknown *provider, HSTRING id, IInspectable **out)
{
	static const void *const *const tables[] = {request_table.slots};
	FILETIME now;
	*out = NULL;
	struct request *self = make(sizeof *self, &request_kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	InitializeSRWLock(&self->lock);
	GetSystemTimeAsFileTime(&now);
	self->deadline = (INT64)(((UINT64)now.dwHighDateTime << 32) | now.dwLowDateTime) + PROVIDER_TIME;
	self->deferrals = 1;
	HRESULT result = WindowsDuplicateString(id, &self->id);
	if (SUCCEEDED(result) && !(self->completed = CreateEventW(NULL, TRUE, FALSE, NULL)))
		result = HRESULT_FROM_WIN32(GetLastError());
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, void *))method(provider, 3))(provider, face_of(&self->base, 0));
	if (SUCCEEDED(result) && InterlockedDecrement(&self->deferrals) &&
		WaitForSingleObject(self->completed, (DWORD)(PROVIDER_TIME / 10000)) != WAIT_OBJECT_0)
		result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
	if (SUCCEEDED(result)) {
		AcquireSRWLockShared(&self->lock);
		reference(*out = self->value);
		ReleaseSRWLockShared(&self->lock);
		if (!*out)
			result = NO_FORMAT;
	}
	release(face_of(&self->base, 0));
	return result;
}

/* ---- the clipboard of the process ---- */

static SRWLOCK clipboard_lock = SRWLOCK_INIT;
/* The package this process put on the clipboard, while the clipboard's sequence is the one it had then. */
static struct package *clipboard_package;
static DWORD clipboard_sequence;
static struct handlers clipboard_handlers;
static HWND clipboard_window;
static INIT_ONCE window_once = INIT_ONCE_STATIC_INIT;

static UINT registered(const WCHAR *name) { return RegisterClipboardFormatW(name); }

/* The clipboard, opened for the window of this component: another program may hold it a moment. */
static BOOL open_clipboard(void)
{
	for (int tries = 0; tries < 20; ++tries) {
		if (OpenClipboard(clipboard_window))
			return TRUE;
		Sleep(10);
	}
	return FALSE;
}

static LRESULT CALLBACK clipboard_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_CLIPBOARD_UPDATE) {
		struct handlers copy = {0};
		AcquireSRWLockShared(&clipboard_lock);
		for (UINT32 at = 0; at < clipboard_handlers.count; ++at)
			handlers_add(&copy, clipboard_handlers.entries[at].handler, &(INT64){0});
		ReleaseSRWLockShared(&clipboard_lock);
		handlers_raise(&copy, NULL, NULL);
		handlers_clear(&copy);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static DWORD WINAPI clipboard_thread(void *ready)
{
	static const WCHAR class_name[] = L"RavenDataTransferClipboard";
	const WNDCLASSW kind = {0, clipboard_procedure, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, class_name};
	MSG message;
	RegisterClassW(&kind);
	clipboard_window = CreateWindowExW(0, class_name, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
	if (clipboard_window)
		AddClipboardFormatListener(clipboard_window);
	SetEvent(ready);
	if (!clipboard_window)
		return 1;
	while (GetMessageW(&message, NULL, 0, 0) > 0)
		DispatchMessageW(&message);
	return 0;
}

/* The component's clipboard window, on a thread of its own that keeps it for the life of the process. */
static BOOL CALLBACK start_window(INIT_ONCE *once, void *parameter, void **context)
{
	HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE thread = ready ? CreateThread(NULL, 0, clipboard_thread, ready, 0, NULL) : NULL;
	if (thread) {
		WaitForSingleObject(ready, INFINITE);
		CloseHandle(thread);
	}
	if (ready)
		CloseHandle(ready);
	return TRUE;
}

static void window_ready(void) { InitOnceExecuteOnce(&window_once, start_window, NULL, NULL); }

/* Puts bytes on the open clipboard as a format. */
static BOOL put_bytes(UINT format, const void *bytes, SIZE_T length)
{
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, length ? length : 1);
	void *data = memory ? GlobalLock(memory) : NULL;
	if (!data) {
		if (memory)
			GlobalFree(memory);
		return FALSE;
	}
	memcpy(data, bytes, length);
	GlobalUnlock(memory);
	if (SetClipboardData(format, memory))
		return TRUE;
	GlobalFree(memory);
	return FALSE;
}

static BOOL put_wide(UINT format, const WCHAR *text, UINT32 length)
{
	WCHAR *copy = ALLOC(((SIZE_T)length + 1) * sizeof(WCHAR));
	if (!copy)
		return FALSE;
	memcpy(copy, text, (SIZE_T)length * sizeof(WCHAR));
	const BOOL done = put_bytes(format, copy, ((SIZE_T)length + 1) * sizeof(WCHAR));
	FREE(copy);
	return done;
}

/* Text in a code page, with its terminating null. */
static BOOL put_narrow(UINT format, UINT code_page, const WCHAR *text, UINT32 length)
{
	const int size = WideCharToMultiByte(code_page, 0, text, (int)length, NULL, 0, NULL, NULL);
	char *bytes = ALLOC((SIZE_T)size + 1);
	if (!bytes)
		return FALSE;
	WideCharToMultiByte(code_page, 0, text, (int)length, bytes, size, NULL, NULL);
	const BOOL done = put_bytes(format, bytes, (SIZE_T)size + 1);
	FREE(bytes);
	return done;
}

/* An address on the clipboard: what an Uri's AbsoluteUri is. */
static BOOL put_uri(UINT format, IInspectable *value)
{
	uri_type *uri = NULL;
	HSTRING address = NULL;
	UINT32 length = 0;
	BOOL done = FALSE;
	if (value && SUCCEEDED(value->lpVtbl->QueryInterface(value, &iid_uri, (void **)&uri)) &&
		SUCCEEDED(uri->lpVtbl->get_AbsoluteUri(uri, &address))) {
		const WCHAR *text = WindowsGetStringRawBuffer(address, &length);
		done = put_wide(format, text, length);
	}
	WindowsDeleteString(address);
	release(uri);
	return done;
}

/* The bytes of a stream, a buffer or a stream reference. */
static HRESULT bytes_of(IInspectable *value, BYTE **bytes, UINT32 *length)
{
	IUnknown *found = NULL;
	*bytes = NULL;
	*length = 0;
	if (!value)
		return E_INVALIDARG;
	if (SUCCEEDED(value->lpVtbl->QueryInterface(value, &iid_buffer, (void **)&found))) {
		BYTE *data;
		HRESULT result = winrt_buffer_bytes((void *)found, &data, length);
		if (SUCCEEDED(result) && !(*bytes = ALLOC(*length ? *length : 1)))
			result = E_OUTOFMEMORY;
		if (SUCCEEDED(result))
			memcpy(*bytes, data, *length);
		release(found);
		return result;
	}
	if (SUCCEEDED(value->lpVtbl->QueryInterface(value, &iid_random_access_stream, (void **)&found))) {
		const HRESULT result = winrt_stream_read_all(found, bytes, length);
		release(found);
		return result;
	}
	if (SUCCEEDED(value->lpVtbl->QueryInterface(value, &iid_stream_reference, (void **)&found))) {
		void *operation = NULL, *stream = NULL;
		HRESULT result = ((HRESULT (WINAPI *)(void *, void **))method(found, 6))(found, &operation);
		if (SUCCEEDED(result))
			result = winrt_wait(operation, FALSE, &stream);
		if (SUCCEEDED(result))
			result = winrt_stream_read_all(stream, bytes, length);
		release(stream);
		release(operation);
		release(found);
		return result;
	}
	return TYPE_MISMATCH;
}

/* A bitmap: a BMP file goes on the clipboard as the DIB after its file header, any other image as "PNG". */
static BOOL put_bitmap(IInspectable *value)
{
	BYTE *bytes;
	UINT32 length;
	BOOL done = FALSE;
	if (FAILED(bytes_of(value, &bytes, &length)))
		return FALSE;
	if (length > sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) && bytes[0] == 'B' && bytes[1] == 'M')
		done = put_bytes(CF_DIB, bytes + sizeof(BITMAPFILEHEADER), length - sizeof(BITMAPFILEHEADER));
	else
		done = put_bytes(registered(L"PNG"), bytes, length);
	FREE(bytes);
	return done;
}

/* Files and folders as CF_HDROP: DROPFILES, then each path with its null, then one more. */
static BOOL put_items(IInspectable *value)
{
	IUnknown *iterable = NULL, *iterator = NULL;
	WCHAR *paths = NULL;
	SIZE_T length = 0, capacity = 0;
	BOOL done = FALSE, more = FALSE;
	if (!value || FAILED(value->lpVtbl->QueryInterface(value, &iid_storage_items_iterable, (void **)&iterable)))
		return FALSE;
	if (FAILED(((HRESULT (WINAPI *)(void *, void **))method(iterable, 6))(iterable, (void **)&iterator))) {
		release(iterable);
		return FALSE;
	}
	((HRESULT (WINAPI *)(void *, boolean *))method(iterator, 7))(iterator, (boolean *)&more);
	while (more & 0xff) {
		IUnknown *item = NULL;
		HSTRING path = NULL;
		UINT32 count = 0;
		if (SUCCEEDED(((HRESULT (WINAPI *)(void *, void **))method(iterator, 6))(iterator, (void **)&item)) &&
			SUCCEEDED(((HRESULT (WINAPI *)(void *, HSTRING *))method(item, 12))(item, &path))) {
			const WCHAR *text = WindowsGetStringRawBuffer(path, &count);
			if (length + count + 2 > capacity) {
				const SIZE_T grown = (length + count + 2) * 2;
				WCHAR *moved = paths ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, paths, grown * sizeof(WCHAR)) :
					ALLOC(grown * sizeof(WCHAR));
				if (moved) {
					paths = moved;
					capacity = grown;
				}
			}
			if (length + count + 2 <= capacity) {
				memcpy(paths + length, text, count * sizeof(WCHAR));
				length += count + 1;
			}
		}
		WindowsDeleteString(path);
		release(item);
		more = FALSE;
		((HRESULT (WINAPI *)(void *, boolean *))method(iterator, 8))(iterator, (boolean *)&more);
	}
	release(iterator);
	release(iterable);
	if (paths) {
		const SIZE_T size = sizeof(DROPFILES) + (length + 1) * sizeof(WCHAR);
		BYTE *drop = ALLOC(size);
		if (drop) {
			((DROPFILES *)drop)->pFiles = sizeof(DROPFILES);
			((DROPFILES *)drop)->fWide = TRUE;
			memcpy(drop + sizeof(DROPFILES), paths, length * sizeof(WCHAR));
			done = put_bytes(CF_HDROP, drop, size);
			FREE(drop);
		}
		FREE(paths);
	}
	return done;
}

/* One format of a package on the open clipboard. */
static void put_format(HSTRING id, IInspectable *value)
{
	HSTRING text = NULL;
	UINT32 length = 0;
	if (is_format(id, bitmap_format)) {
		put_bitmap(value);
		return;
	}
	if (is_format(id, items_format)) {
		put_items(value);
		return;
	}
	if (is_format(id, link_format) || is_format(id, application_link_format)) {
		put_uri(registered(WindowsGetStringRawBuffer(id, NULL)), value);
		return;
	}
	if (SUCCEEDED(property_value_string((IUnknown *)value, &text))) {
		const WCHAR *characters = WindowsGetStringRawBuffer(text, &length);
		if (is_format(id, text_format))
			put_wide(CF_UNICODETEXT, characters, length);
		else if (is_format(id, html_format))
			put_narrow(registered(html_format), CP_UTF8, characters, length);
		else if (is_format(id, rtf_format))
			put_narrow(registered(rtf_format), CP_ACP, characters, length);
		else
			put_wide(registered(WindowsGetStringRawBuffer(id, NULL)), characters, length);
		WindowsDeleteString(text);
		return;
	}
	BYTE *bytes;
	if (SUCCEEDED(bytes_of(value, &bytes, &length))) {
		put_bytes(registered(WindowsGetStringRawBuffer(id, NULL)), bytes, length);
		FREE(bytes);
	}
}

static HRESULT clipboard_put(struct package *package)
{
	struct format *formats = NULL;
	UINT32 count = 0;
	window_ready();
	if (package) {
		AcquireSRWLockShared(&package->lock);
		formats = formats_copy(package->formats, count = package->count);
		ReleaseSRWLockShared(&package->lock);
		if (!formats)
			return E_OUTOFMEMORY;
	}
	/* What providers give is asked for before the clipboard is opened, so that they may take their time. */
	for (UINT32 at = 0; at < count; ++at)
		if (formats[at].provider && !formats[at].value && FAILED(ask_provider(formats[at].provider, formats[at].id, &formats[at].value)))
			formats[at].value = NULL;
	if (!open_clipboard()) {
		formats_free(formats, count);
		return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
	}
	EmptyClipboard();
	for (UINT32 at = 0; at < count; ++at)
		if (formats[at].value)
			put_format(formats[at].id, formats[at].value);
	CloseClipboard();
	formats_free(formats, count);
	if (package)
		reference(face_of(&package->base, 0));
	AcquireSRWLockExclusive(&clipboard_lock);
	struct package *old = clipboard_package;
	clipboard_package = package;
	clipboard_sequence = GetClipboardSequenceNumber();
	ReleaseSRWLockExclusive(&clipboard_lock);
	if (old)
		release(face_of(&old->base, 0));
	return S_OK;
}

/* The formats the clipboard has, as the formats of a package name them. */
static HRESULT clipboard_formats(struct format **out, UINT32 *count)
{
	static const WCHAR *const names[] = {text_format, bitmap_format, items_format, html_format, rtf_format, link_format};
	WCHAR name[256];
	UINT32 capacity = 16;
	*count = 0;
	if (!(*out = ALLOC(capacity * sizeof **out)))
		return E_OUTOFMEMORY;
	if (!open_clipboard())
		return S_OK;
	for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format)) {
		const WCHAR *id = NULL;
		if (format == CF_UNICODETEXT || format == CF_TEXT || format == CF_OEMTEXT)
			id = names[0];
		else if (format == CF_DIB || format == CF_DIBV5 || format == CF_BITMAP)
			id = names[1];
		else if (format == CF_HDROP)
			id = names[2];
		else if (format >= 0xc000 && GetClipboardFormatNameW(format, name, ARRAYSIZE(name))) {
			id = CompareStringOrdinal(name, -1, L"PNG", -1, FALSE) == CSTR_EQUAL ? names[1] :
				CompareStringOrdinal(name, -1, L"UniformResourceLocator", -1, FALSE) == CSTR_EQUAL ? names[5] : name;
		}
		if (!id)
			continue;
		HSTRING text = name_of(id);
		if (!find_format(*out, *count, text)) {
			if (*count == capacity) {
				struct format *grown = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *out, capacity * 2 * sizeof **out);
				if (!grown) {
					WindowsDeleteString(text);
					break;
				}
				*out = grown;
				capacity *= 2;
			}
			(*out)[(*count)++].id = text;
		} else
			WindowsDeleteString(text);
	}
	CloseClipboard();
	return S_OK;
}

/* The bytes of a clipboard format: a copy the caller frees. */
static BOOL clipboard_bytes(UINT format, BYTE **bytes, SIZE_T *length)
{
	HANDLE memory = GetClipboardData(format);
	const void *data = memory ? GlobalLock(memory) : NULL;
	*bytes = NULL;
	*length = 0;
	if (!data)
		return FALSE;
	*length = GlobalSize(memory);
	if ((*bytes = ALLOC(*length ? *length : 1)))
		memcpy(*bytes, data, *length);
	GlobalUnlock(memory);
	return *bytes != NULL;
}

/* Text of a clipboard format up to its first null, in a code page or as UTF-16 when code_page is 0. */
static HRESULT clipboard_text(UINT format, UINT code_page, IInspectable **out)
{
	BYTE *bytes;
	SIZE_T length;
	HRESULT result = NO_FORMAT;
	if (!clipboard_bytes(format, &bytes, &length))
		return result;
	if (!code_page) {
		UINT32 count = 0;
		while (count < length / sizeof(WCHAR) && ((WCHAR *)bytes)[count])
			++count;
		result = text_box_of((WCHAR *)bytes, count, out);
	} else {
		int count = 0;
		while ((SIZE_T)count < length && bytes[count])
			++count;
		const int wide = MultiByteToWideChar(code_page, 0, (char *)bytes, count, NULL, 0);
		WCHAR *text = ALLOC(((SIZE_T)wide + 1) * sizeof(WCHAR));
		if (text) {
			MultiByteToWideChar(code_page, 0, (char *)bytes, count, text, wide);
			result = text_box_of(text, (UINT32)wide, out);
			FREE(text);
		} else
			result = E_OUTOFMEMORY;
	}
	FREE(bytes);
	return result;
}

/* A clipboard format as a stream of its bytes, or with reference as a RandomAccessStreamReference to one. */
static HRESULT clipboard_stream(const BYTE *bytes, SIZE_T length, BOOL as_reference, IInspectable **out)
{
	void *stream = NULL;
	HRESULT result = length > 0x7fffffff ? E_OUTOFMEMORY : winrt_memory_stream_of(bytes, (UINT32)length, &stream);
	if (SUCCEEDED(result) && as_reference)
		result = stream_reference_make(stream, (void **)out);
	else if (SUCCEEDED(result))
		reference(*out = stream);
	release(stream);
	return result;
}

/* A DIB as a BMP file: the file header, then the DIB, its pixels after the header and the colours. */
static HRESULT clipboard_bitmap(IInspectable **out)
{
	BYTE *bytes;
	SIZE_T length;
	HRESULT result = NO_FORMAT;
	if (clipboard_bytes(registered(L"PNG"), &bytes, &length)) {
		result = clipboard_stream(bytes, length, TRUE, out);
		FREE(bytes);
		return result;
	}
	if (!clipboard_bytes(CF_DIB, &bytes, &length) || length < sizeof(BITMAPINFOHEADER)) {
		FREE(bytes);
		return result;
	}
	const BITMAPINFOHEADER *header = (const BITMAPINFOHEADER *)bytes;
	SIZE_T colours = header->biClrUsed ? header->biClrUsed : header->biBitCount <= 8 ? (SIZE_T)1 << header->biBitCount : 0;
	SIZE_T masks = header->biSize == sizeof(BITMAPINFOHEADER) && header->biCompression == BI_BITFIELDS ? 12 : 0;
	BYTE *file = ALLOC(sizeof(BITMAPFILEHEADER) + length);
	if (file) {
		BITMAPFILEHEADER *top = (BITMAPFILEHEADER *)file;
		top->bfType = 0x4d42;
		top->bfSize = (DWORD)(sizeof(BITMAPFILEHEADER) + length);
		top->bfOffBits = (DWORD)(sizeof(BITMAPFILEHEADER) + header->biSize + masks + colours * sizeof(RGBQUAD));
		memcpy(file + sizeof(BITMAPFILEHEADER), bytes, length);
		result = clipboard_stream(file, sizeof(BITMAPFILEHEADER) + length, TRUE, out);
		FREE(file);
	} else
		result = E_OUTOFMEMORY;
	FREE(bytes);
	return result;
}

/* CF_HDROP as an IVectorView<IStorageItem> of its files and folders. */
static HRESULT clipboard_items(IInspectable **out)
{
	HDROP drop = GetClipboardData(CF_HDROP);
	if (!drop)
		return NO_FORMAT;
	const UINT count = DragQueryFileW(drop, 0xffffffff, NULL, 0);
	IInspectable **items = ALLOC((count ? count : 1) * sizeof *items);
	UINT made = 0;
	HRESULT result = items ? S_OK : E_OUTOFMEMORY;
	for (UINT at = 0; SUCCEEDED(result) && at < count; ++at) {
		WCHAR path[MAX_PATH];
		IUnknown *found = NULL;
		if (!DragQueryFileW(drop, at, path, MAX_PATH))
			continue;
		const DWORD attributes = GetFileAttributesW(path);
		if (attributes == INVALID_FILE_ATTRIBUTES)
			continue;
		if (SUCCEEDED(attributes & FILE_ATTRIBUTE_DIRECTORY ? make_folder(path, (void **)&found) : make_file(path, (void **)&found))) {
			found->lpVtbl->QueryInterface(found, &iid_storage_item, (void **)&items[made]);
			if (items[made])
				++made;
			release(found);
		}
	}
	if (SUCCEEDED(result))
		result = winrt_items(&item_list, items, made, FALSE, (void **)out);
	for (UINT at = 0; at < made; ++at)
		release(items[at]);
	FREE(items);
	return result;
}

/* The value of a format the clipboard has, read now; text_only asks a custom format for its text. */
static HRESULT clipboard_value(HSTRING id, BOOL text_only, IInspectable **out)
{
	*out = NULL;
	if (!open_clipboard())
		return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
	HRESULT result;
	if (is_format(id, text_format))
		result = IsClipboardFormatAvailable(CF_UNICODETEXT) ? clipboard_text(CF_UNICODETEXT, 0, out) : clipboard_text(CF_TEXT, CP_ACP, out);
	else if (is_format(id, html_format))
		result = clipboard_text(registered(html_format), CP_UTF8, out);
	else if (is_format(id, rtf_format))
		result = clipboard_text(registered(rtf_format), CP_ACP, out);
	else if (is_format(id, link_format)) {
		IInspectable *text = NULL;
		HSTRING address = NULL;
		result = IsClipboardFormatAvailable(registered(link_format)) ? clipboard_text(registered(link_format), 0, &text) :
			clipboard_text(registered(L"UniformResourceLocator"), CP_ACP, &text);
		if (SUCCEEDED(result) && SUCCEEDED(result = property_value_string((IUnknown *)text, &address))) {
			UINT32 length = 0;
			const WCHAR *characters = WindowsGetStringRawBuffer(address, &length);
			result = make_uri(characters, length, out);
		}
		WindowsDeleteString(address);
		release(text);
	} else if (is_format(id, bitmap_format))
		result = clipboard_bitmap(out);
	else if (is_format(id, items_format))
		result = clipboard_items(out);
	else if (text_only)
		result = clipboard_text(registered(WindowsGetStringRawBuffer(id, NULL)), 0, out);
	else {
		BYTE *bytes;
		SIZE_T length;
		result = NO_FORMAT;
		if (clipboard_bytes(registered(WindowsGetStringRawBuffer(id, NULL)), &bytes, &length)) {
			result = clipboard_stream(bytes, length, FALSE, out);
			FREE(bytes);
		}
	}
	CloseClipboard();
	return result;
}

/* ---- views ---- */

struct view {
	struct object base;
	/* The package the view is of, or NULL for a view of what the clipboard has. */
	struct package *package;
	struct format *formats;
	UINT32 count;
	DWORD sequence;
	IUnknown *properties;
	INT32 operation;
	SRWLOCK lock;
	HSTRING accepted;
};

static struct view *view_of(void *iface) { return (struct view *)owner(iface); }

static void destroy_view(struct object *base)
{
	struct view *self = (struct view *)base;
	formats_free(self->formats, self->count);
	release(self->properties);
	WindowsDeleteString(self->accepted);
	if (self->package)
		release(face_of(&self->package->base, 0));
}

static HRESULT WINAPI view_properties(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = view_of(iface)->properties);
	return S_OK;
}

static HRESULT WINAPI view_operation(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = view_of(iface)->operation;
	return S_OK;
}

/* OperationCompletedEventArgs: what the target did, and with which format. */
struct completed_args {
	struct object base;
	INT32 operation;
	HSTRING accepted;
};

static void destroy_completed_args(struct object *base) { WindowsDeleteString(((struct completed_args *)base)->accepted); }

static HRESULT WINAPI args_operation(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct completed_args *)owner(iface))->operation;
	return S_OK;
}

static HRESULT WINAPI args_accepted(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct completed_args *)owner(iface))->accepted, out);
}

TABLE(args_table, 0, INSPECTABLE, (const void *)args_operation);
TABLE(args2_table, 1, INSPECTABLE, (const void *)args_accepted);
static const struct face_iid args_iids[] = {{&iid_completed_args, 0}, {&iid_completed_args2, 1}, {NULL, 0}};
static const struct kind args_kind = {"OperationCompletedEventArgs", L"Windows.ApplicationModel.DataTransfer.OperationCompletedEventArgs",
	args_iids, destroy_completed_args};

/* The target reports what it did: the package's OperationCompleted handlers hear it. */
static HRESULT WINAPI view_report(void *iface, INT32 operation)
{
	static const void *const *const tables[] = {args_table.slots, args2_table.slots};
	struct view *self = view_of(iface);
	if (!self->package)
		return S_OK;
	struct completed_args *args = make(sizeof *args, &args_kind, tables, 2);
	if (!args)
		return E_OUTOFMEMORY;
	args->operation = operation;
	AcquireSRWLockShared(&self->lock);
	WindowsDuplicateString(self->accepted, &args->accepted);
	ReleaseSRWLockShared(&self->lock);
	struct handlers copy = {0};
	AcquireSRWLockShared(&self->package->lock);
	for (UINT32 at = 0; at < self->package->completed.count; ++at)
		handlers_add(&copy, self->package->completed.entries[at].handler, &(INT64){0});
	ReleaseSRWLockShared(&self->package->lock);
	handlers_raise(&copy, face_of(&self->package->base, 0), face_of(&args->base, 0));
	handlers_clear(&copy);
	release(face_of(&args->base, 0));
	return S_OK;
}

static HRESULT WINAPI view_formats(void *iface, void **out)
{
	struct view *self = view_of(iface);
	HSTRING *ids = ALLOC((self->count ? self->count : 1) * sizeof *ids);
	if (!ids)
		return E_OUTOFMEMORY;
	for (UINT32 at = 0; at < self->count; ++at)
		ids[at] = self->formats[at].id;
	const HRESULT result = winrt_items(&string_list, (IInspectable **)ids, self->count, FALSE, out);
	FREE(ids);
	return result;
}

static HRESULT WINAPI view_contains(void *iface, HSTRING id, boolean *out)
{
	struct view *self = view_of(iface);
	return answer_flag(out, find_format(self->formats, self->count, id) != NULL);
}

/* What a view is asked for: an object, a string, storage items or an address. */
enum want { WANT_OBJECT, WANT_TEXT, WANT_ITEMS };

struct asking {
	struct view *view;
	HSTRING id;
	enum want want;
};

static void free_asking(void *context)
{
	struct asking *asking = context;
	WindowsDeleteString(asking->id);
	release(face_of(&asking->view->base, 0));
	FREE(asking);
}

/* Items given as any IIterable<IStorageItem>, as the IVectorView<IStorageItem> GetStorageItemsAsync answers. */
static HRESULT items_view(IInspectable *value, IInspectable **out)
{
	IUnknown *iterable = NULL, *iterator = NULL;
	IInspectable **items = NULL;
	UINT32 count = 0, capacity = 0;
	boolean more = FALSE;
	HRESULT result = value->lpVtbl->QueryInterface(value, &iid_storage_items_iterable, (void **)&iterable);
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, void **))method(iterable, 6))(iterable, (void **)&iterator);
	if (SUCCEEDED(result))
		result = ((HRESULT (WINAPI *)(void *, boolean *))method(iterator, 7))(iterator, &more);
	while (SUCCEEDED(result) && more) {
		if (count == capacity) {
			capacity = capacity ? capacity * 2 : 8;
			IInspectable **grown = items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, items, capacity * sizeof *items) :
				ALLOC(capacity * sizeof *items);
			if (!grown) {
				result = E_OUTOFMEMORY;
				break;
			}
			items = grown;
		}
		if (SUCCEEDED(result = ((HRESULT (WINAPI *)(void *, void **))method(iterator, 6))(iterator, (void **)&items[count])))
			++count;
		if (SUCCEEDED(result))
			result = ((HRESULT (WINAPI *)(void *, boolean *))method(iterator, 8))(iterator, &more);
	}
	if (SUCCEEDED(result))
		result = winrt_items(&item_list, items, count, FALSE, (void **)out);
	for (UINT32 at = 0; at < count; ++at)
		release(items[at]);
	FREE(items);
	release(iterator);
	release(iterable);
	return result;
}

static HRESULT run_asking(struct winrt_async *operation, void *context, union winrt_value *result)
{
	struct asking *asking = context;
	struct view *view = asking->view;
	const struct format *format = find_format(view->formats, view->count, asking->id);
	IInspectable *value = NULL;
	HRESULT done;
	if (!format)
		return NO_FORMAT;
	if (format->value) {
		reference(value = format->value);
		done = S_OK;
	} else if (format->provider)
		done = ask_provider(format->provider, format->id, &value);
	else
		done = clipboard_value(asking->id, asking->want == WANT_TEXT, &value);
	if (SUCCEEDED(done)) {
		if (asking->want == WANT_TEXT)
			done = property_value_string((IUnknown *)value, &result->string);
		else if (asking->want == WANT_ITEMS && !format->value && !format->provider)
			reference(result->object = value);
		else if (asking->want == WANT_ITEMS)
			done = items_view(value, (IInspectable **)&result->object);
		else
			reference(result->object = value);
	}
	release(value);
	return done;
}

static HRESULT ask(void *iface, HSTRING id, enum want want, const IID *operation_iid, void **out)
{
	struct view *self = view_of(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct asking *asking = ALLOC(sizeof *asking);
	if (!asking)
		return E_OUTOFMEMORY;
	WindowsDuplicateString(id, &asking->id);
	asking->view = self;
	asking->want = want;
	reference(face_of(&self->base, 0));
	return winrt_async_start(operation_iid, FALSE, want == WANT_TEXT ? WINRT_RESULT_STRING : WINRT_RESULT_OBJECT, run_asking, asking,
		free_asking, out);
}

static HRESULT ask_named(void *iface, const WCHAR *name, enum want want, const IID *operation_iid, void **out)
{
	HSTRING id = name_of(name);
	const HRESULT result = ask(iface, id, want, operation_iid, out);
	WindowsDeleteString(id);
	return result;
}

static HRESULT WINAPI get_data(void *iface, HSTRING id, void **out) { return ask(iface, id, WANT_OBJECT, &iid_object_operation, out); }
static HRESULT WINAPI get_text(void *iface, void **out) { return ask_named(iface, text_format, WANT_TEXT, &iid_string_operation, out); }
static HRESULT WINAPI get_custom_text(void *iface, HSTRING id, void **out) { return ask(iface, id, WANT_TEXT, &iid_string_operation, out); }
static HRESULT WINAPI get_uri(void *iface, void **out) { return ask_named(iface, link_format, WANT_OBJECT, &iid_uri_operation, out); }
static HRESULT WINAPI get_html(void *iface, void **out) { return ask_named(iface, html_format, WANT_TEXT, &iid_string_operation, out); }
static HRESULT WINAPI get_rtf(void *iface, void **out) { return ask_named(iface, rtf_format, WANT_TEXT, &iid_string_operation, out); }
static HRESULT WINAPI get_bitmap(void *iface, void **out) { return ask_named(iface, bitmap_format, WANT_OBJECT, &iid_reference_operation, out); }
static HRESULT WINAPI get_items(void *iface, void **out) { return ask_named(iface, items_format, WANT_ITEMS, &iid_items_operation, out); }
static HRESULT WINAPI get_application_link(void *iface, void **out)
{
	return ask_named(iface, application_link_format, WANT_OBJECT, &iid_uri_operation, out);
}

/* The package's resources as they are now; a view of the clipboard has none. */
static HRESULT WINAPI get_resources(void *iface, void **out)
{
	struct view *self = view_of(iface);
	union winrt_value resources = {0};
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = self->package ? property_set_view(self->package->resources, &resources.object) :
		typed_map_view(&resource_types, NULL, 0, &resources.object);
	const HRESULT made = winrt_async_done(&iid_resources_operation, WINRT_RESULT_OBJECT, result, &resources, out);
	release(resources.object);
	return made;
}

/* Enterprise data protection allows every access. */
static HRESULT allowed(void **out)
{
	union winrt_value value = {0};
	if (!out)
		return E_POINTER;
	return winrt_async_done(&iid_policy_operation, WINRT_RESULT_UINT32, S_OK, &value, out);
}

static HRESULT WINAPI request_access(void *iface, void **out) { return allowed(out); }
static HRESULT WINAPI request_access_for(void *iface, HSTRING enterprise, void **out) { return allowed(out); }

static HRESULT WINAPI unlock_identity(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI set_accepted(void *iface, HSTRING id)
{
	struct view *self = view_of(iface);
	HSTRING copy = NULL;
	const HRESULT result = WindowsDuplicateString(id, &copy);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&self->lock);
	HSTRING old = self->accepted;
	self->accepted = copy;
	ReleaseSRWLockExclusive(&self->lock);
	WindowsDeleteString(old);
	return S_OK;
}

TABLE(view_table, 0, INSPECTABLE, (const void *)view_properties, (const void *)view_operation, (const void *)view_report,
	(const void *)view_formats, (const void *)view_contains, (const void *)get_data, (const void *)get_text, (const void *)get_custom_text,
	(const void *)get_uri, (const void *)get_html, (const void *)get_resources, (const void *)get_rtf, (const void *)get_bitmap,
	(const void *)get_items);
TABLE(view2_table, 1, INSPECTABLE, (const void *)get_application_link, (const void *)get_uri);
TABLE(view3_table, 2, INSPECTABLE, (const void *)request_access, (const void *)request_access_for, (const void *)unlock_identity);
TABLE(view4_table, 3, INSPECTABLE, (const void *)set_accepted);
static const struct face_iid view_iids[] = {{&iid_view, 0}, {&iid_view2, 1}, {&iid_view3, 2}, {&iid_view4, 3}, {NULL, 0}};
static const struct kind view_kind = {"DataPackageView", L"Windows.ApplicationModel.DataTransfer.DataPackageView", view_iids, destroy_view};

/* A view of a package's formats, or with no package of the clipboard's formats, which it then owns. */
static HRESULT make_view(struct package *package, const struct format *formats, UINT32 count, DWORD sequence, void **out)
{
	static const void *const *const tables[] = {view_table.slots, view2_table.slots, view3_table.slots, view4_table.slots};
	*out = NULL;
	types_ready();
	struct view *self = make(sizeof *self, &view_kind, tables, 4);
	if (!self)
		return E_OUTOFMEMORY;
	InitializeSRWLock(&self->lock);
	self->sequence = sequence;
	self->operation = package ? package->operation : 1;
	HRESULT result = S_OK;
	if (package) {
		self->package = package;
		reference(face_of(&package->base, 0));
		if (!(self->formats = formats_copy(formats, count)))
			result = E_OUTOFMEMORY;
		else
			self->count = count;
		if (SUCCEEDED(result))
			result = set_view_face(package->properties, (void **)&self->properties);
	} else {
		self->formats = (struct format *)formats;
		self->count = count;
		struct property_set *empty = typed_map_make(&property_types);
		result = empty ? set_view_face(empty, (void **)&self->properties) : E_OUTOFMEMORY;
		if (empty)
			release(map_face(empty));
	}
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

/* ---- Clipboard ---- */

static HRESULT WINAPI clipboard_content(void *iface, void **out)
{
	struct format *formats;
	UINT32 count;
	if (!out)
		return E_POINTER;
	*out = NULL;
	window_ready();
	AcquireSRWLockShared(&clipboard_lock);
	struct package *own = clipboard_package && clipboard_sequence == GetClipboardSequenceNumber() ? clipboard_package : NULL;
	if (own)
		reference(face_of(&own->base, 0));
	ReleaseSRWLockShared(&clipboard_lock);
	if (own) {
		const HRESULT result = package_view(face_of(&own->base, 0), out);
		release(face_of(&own->base, 0));
		return result;
	}
	HRESULT result = clipboard_formats(&formats, &count);
	if (SUCCEEDED(result) && FAILED(result = make_view(NULL, formats, count, GetClipboardSequenceNumber(), out)))
		formats_free(formats, count);
	return result;
}

static HRESULT WINAPI clipboard_set(void *iface, void *content)
{
	if (!content)
		return clipboard_put(NULL);
	IUnknown *found = NULL;
	if (FAILED(((IUnknown *)content)->lpVtbl->QueryInterface(content, &iid_package, (void **)&found)))
		return E_INVALIDARG;
	/* Only a package of this component can be read here. */
	const HRESULT result = method(found, 0) == (void *)object_query && owner(found)->kind == &package_kind ? clipboard_put(package_of(found)) :
		E_INVALIDARG;
	release(found);
	return result;
}

static HRESULT WINAPI clipboard_flush(void *iface) { return S_OK; }

static HRESULT WINAPI clipboard_clear(void *iface) { return clipboard_put(NULL); }

static HRESULT WINAPI clipboard_changed_add(void *iface, void *handler, INT64 *token)
{
	window_ready();
	AcquireSRWLockExclusive(&clipboard_lock);
	const HRESULT result = handlers_add(&clipboard_handlers, handler, token);
	ReleaseSRWLockExclusive(&clipboard_lock);
	return result;
}

static HRESULT WINAPI clipboard_changed_remove(void *iface, INT64 token)
{
	AcquireSRWLockExclusive(&clipboard_lock);
	const HRESULT result = handlers_remove(&clipboard_handlers, token);
	ReleaseSRWLockExclusive(&clipboard_lock);
	return result;
}

/* ClipboardHistoryItemsResult: history is disabled, with no items. */
struct history_result {
	struct object base;
};

static HRESULT WINAPI history_status(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 2;
	return S_OK;
}

static HRESULT WINAPI history_items(void *iface, void **out) { return winrt_items(&history_list, NULL, 0, FALSE, out); }

TABLE(history_table, 0, INSPECTABLE, (const void *)history_status, (const void *)history_items);
static const struct face_iid history_iids[] = {{&iid_history_result, 0}, {NULL, 0}};
static const struct kind history_kind = {"ClipboardHistoryItemsResult", L"Windows.ApplicationModel.DataTransfer.ClipboardHistoryItemsResult",
	history_iids, NULL};

static HRESULT WINAPI clipboard_history(void *iface, void **out)
{
	static const void *const *const tables[] = {history_table.slots};
	union winrt_value value = {0};
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct history_result *result = make(sizeof *result, &history_kind, tables, 1);
	if (!result)
		return E_OUTOFMEMORY;
	value.object = face_of(&result->base, 0);
	const HRESULT made = winrt_async_done(&iid_history_operation, WINRT_RESULT_OBJECT, S_OK, &value, out);
	release(value.object);
	return made;
}

static HRESULT WINAPI answer_false(void *iface, boolean *out) { return answer_flag(out, FALSE); }
static HRESULT WINAPI delete_history_item(void *iface, void *item, boolean *out) { return answer_flag(out, FALSE); }

/* SetHistoryItemAsContentStatus.ItemDeleted: no item is in a history that is off. */
static HRESULT WINAPI history_item_as_content(void *iface, void *item, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 2;
	return S_OK;
}

static HRESULT WINAPI clipboard_set_with(void *iface, void *content, void *options, boolean *out)
{
	const HRESULT result = clipboard_set(iface, content);
	return SUCCEEDED(result) ? answer_flag(out, TRUE) : result;
}

TABLE(clipboard_factory_table, 0, FACTORY);
TABLE(clipboard_statics_table, 1, INSPECTABLE, (const void *)clipboard_content, (const void *)clipboard_set, (const void *)clipboard_flush,
	(const void *)clipboard_clear, (const void *)clipboard_changed_add, (const void *)clipboard_changed_remove);
TABLE(clipboard_statics2_table, 2, INSPECTABLE, (const void *)clipboard_history, (const void *)answer_false,
	(const void *)delete_history_item, (const void *)history_item_as_content, (const void *)answer_false, (const void *)answer_false,
	(const void *)clipboard_set_with, (const void *)event_add, (const void *)event_remove, (const void *)event_add,
	(const void *)event_remove, (const void *)event_add, (const void *)event_remove);
static const struct face_iid clipboard_class_iids[] = {{&iid_factory, 0}, {&iid_clipboard_statics, 1}, {&iid_clipboard_statics2, 2}, {NULL, 0}};
static const struct kind clipboard_class_kind = {"Clipboard", L"Windows.ApplicationModel.DataTransfer.Clipboard", clipboard_class_iids, NULL};
struct object clipboard_class = {{clipboard_factory_table.slots, clipboard_statics_table.slots, clipboard_statics2_table.slots},
	&clipboard_class_kind, 1};

/* ---- ClipboardContentOptions ---- */

struct content_options {
	struct object base;
	boolean roamable, in_history;
	IUnknown *roaming, *history;
};

static void destroy_content_options(struct object *base)
{
	struct content_options *self = (struct content_options *)base;
	release(self->roaming);
	release(self->history);
}

static struct content_options *content_options_of(void *iface) { return (struct content_options *)owner(iface); }
static HRESULT WINAPI roamable(void *iface, boolean *out) { return answer_flag(out, content_options_of(iface)->roamable); }
static HRESULT WINAPI set_roamable(void *iface, boolean value) { content_options_of(iface)->roamable = value; return S_OK; }
static HRESULT WINAPI in_history(void *iface, boolean *out) { return answer_flag(out, content_options_of(iface)->in_history); }
static HRESULT WINAPI set_in_history(void *iface, boolean value) { content_options_of(iface)->in_history = value; return S_OK; }

static HRESULT WINAPI roaming_formats(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = content_options_of(iface)->roaming);
	return S_OK;
}

static HRESULT WINAPI history_formats(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = content_options_of(iface)->history);
	return S_OK;
}

TABLE(content_options_table, 0, INSPECTABLE, (const void *)roamable, (const void *)set_roamable, (const void *)in_history,
	(const void *)set_in_history, (const void *)roaming_formats, (const void *)history_formats);
static const struct face_iid content_options_iids[] = {{&iid_content_options, 0}, {NULL, 0}};
static const struct kind content_options_kind = {"ClipboardContentOptions", L"Windows.ApplicationModel.DataTransfer.ClipboardContentOptions",
	content_options_iids, destroy_content_options};

static HRESULT WINAPI content_options_activate(void *iface, IInspectable **out)
{
	static const void *const *const tables[] = {content_options_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	struct content_options *self = make(sizeof *self, &content_options_kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	self->roamable = self->in_history = TRUE;
	if (FAILED(winrt_vector(&string_list, NULL, 0, (void **)&self->roaming)) ||
		FAILED(winrt_vector(&string_list, NULL, 0, (void **)&self->history))) {
		release(face_of(&self->base, 0));
		return E_OUTOFMEMORY;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

TABLE(content_options_factory_table, 0, INSPECTABLE, (const void *)content_options_activate);
static const struct face_iid content_options_class_iids[] = {{&iid_factory, 0}, {NULL, 0}};
static const struct kind content_options_class_kind = {"ClipboardContentOptions",
	L"Windows.ApplicationModel.DataTransfer.ClipboardContentOptions", content_options_class_iids, NULL};
struct object clipboard_content_options_class = {{content_options_factory_table.slots}, &content_options_class_kind, 1};

/* ---- StandardDataFormats ---- */

static HRESULT answer_name(const WCHAR *name, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsCreateString(name, lstrlenW(name), out);
}

static HRESULT WINAPI format_text(void *iface, HSTRING *out) { return answer_name(text_format, out); }
static HRESULT WINAPI format_uri(void *iface, HSTRING *out) { return answer_name(link_format, out); }
static HRESULT WINAPI format_html(void *iface, HSTRING *out) { return answer_name(html_format, out); }
static HRESULT WINAPI format_rtf(void *iface, HSTRING *out) { return answer_name(rtf_format, out); }
static HRESULT WINAPI format_bitmap(void *iface, HSTRING *out) { return answer_name(bitmap_format, out); }
static HRESULT WINAPI format_items(void *iface, HSTRING *out) { return answer_name(items_format, out); }
static HRESULT WINAPI format_application_link(void *iface, HSTRING *out) { return answer_name(application_link_format, out); }
static HRESULT WINAPI format_activity(void *iface, HSTRING *out) { return answer_name(activity_format, out); }

TABLE(formats_factory_table, 0, FACTORY);
TABLE(formats_statics_table, 1, INSPECTABLE, (const void *)format_text, (const void *)format_uri, (const void *)format_html,
	(const void *)format_rtf, (const void *)format_bitmap, (const void *)format_items);
TABLE(formats_statics2_table, 2, INSPECTABLE, (const void *)format_uri, (const void *)format_application_link);
TABLE(formats_statics3_table, 3, INSPECTABLE, (const void *)format_activity);
static const struct face_iid formats_class_iids[] = {{&iid_factory, 0}, {&iid_formats_statics, 1}, {&iid_formats_statics2, 2},
	{&iid_formats_statics3, 3}, {NULL, 0}};
static const struct kind formats_class_kind = {"StandardDataFormats", L"Windows.ApplicationModel.DataTransfer.StandardDataFormats",
	formats_class_iids, NULL};
struct object standard_formats_class = {{formats_factory_table.slots, formats_statics_table.slots, formats_statics2_table.slots,
	formats_statics3_table.slots}, &formats_class_kind, 1};

/* ---- HtmlFormatHelper: the CF_HTML text of "HTML Format", whose offsets count bytes of its UTF-8 ---- */

static const char html_header[] = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\n";
static const char html_start[] = "<html><body><!--StartFragment-->", html_end[] = "<!--EndFragment--></body></html>";

static void put_offset(char *text, const char *name, UINT32 value)
{
	char *at = text;
	const size_t length = lstrlenA(name);
	for (; *at; ++at)
		if (!memcmp(at, name, length)) {
			at += length;
			for (int digit = 9; digit >= 0; --digit, value /= 10)
				at[digit] = (char)('0' + value % 10);
			return;
		}
}

static HRESULT WINAPI create_html_format(void *iface, HSTRING fragment, HSTRING *out)
{
	UINT32 length = 0, bytes = 0, made_length = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const WCHAR *text = WindowsGetStringRawBuffer(fragment, &length);
	char *utf8 = winrt_utf8(text, length, &bytes);
	if (!utf8)
		return E_OUTOFMEMORY;
	const UINT32 header = sizeof html_header - 1, start = sizeof html_start - 1, end = sizeof html_end - 1;
	const UINT32 total = header + start + bytes + end;
	char *whole = ALLOC((SIZE_T)total + 1);
	HRESULT result = E_OUTOFMEMORY;
	if (whole) {
		memcpy(whole, html_header, header);
		memcpy(whole + header, html_start, start);
		memcpy(whole + header + start, utf8, bytes);
		memcpy(whole + header + start + bytes, html_end, end);
		put_offset(whole, "StartHTML:", header);
		put_offset(whole, "EndHTML:", total);
		put_offset(whole, "StartFragment:", header + start);
		put_offset(whole, "EndFragment:", header + start + bytes);
		WCHAR *wide = winrt_utf16(whole, total, &made_length);
		result = wide ? WindowsCreateString(wide, made_length, out) : E_OUTOFMEMORY;
		FREE(wide);
		FREE(whole);
	}
	FREE(utf8);
	return result;
}

/* The number after a name in the header, or -1. */
static INT64 header_number(const char *text, UINT32 length, const char *name)
{
	const UINT32 name_length = (UINT32)lstrlenA(name);
	for (UINT32 at = 0; at + name_length <= length; ++at) {
		if (memcmp(text + at, name, name_length))
			continue;
		INT64 value = 0;
		UINT32 digit = at + name_length;
		if (digit >= length || text[digit] < '0' || text[digit] > '9')
			return -1;
		for (; digit < length && text[digit] >= '0' && text[digit] <= '9' && value < 0x7fffffff; ++digit)
			value = value * 10 + (text[digit] - '0');
		return value;
	}
	return -1;
}

static HRESULT WINAPI static_fragment(void *iface, HSTRING format, HSTRING *out)
{
	UINT32 length = 0, bytes = 0, made_length = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const WCHAR *text = WindowsGetStringRawBuffer(format, &length);
	char *utf8 = winrt_utf8(text, length, &bytes);
	if (!utf8)
		return E_OUTOFMEMORY;
	const INT64 start = header_number(utf8, bytes, "StartFragment:"), end = header_number(utf8, bytes, "EndFragment:");
	HRESULT result;
	/* Text that is not CF_HTML is its own fragment. */
	if (start < 0 || end < start || end > bytes)
		result = WindowsDuplicateString(format, out);
	else {
		WCHAR *wide = winrt_utf16(utf8 + start, (UINT32)(end - start), &made_length);
		result = wide ? WindowsCreateString(wide, made_length, out) : E_OUTOFMEMORY;
		FREE(wide);
	}
	FREE(utf8);
	return result;
}

TABLE(html_factory_table, 0, FACTORY);
TABLE(html_statics_table, 1, INSPECTABLE, (const void *)static_fragment, (const void *)create_html_format);
static const struct face_iid html_class_iids[] = {{&iid_factory, 0}, {&iid_html_statics, 1}, {NULL, 0}};
static const struct kind html_class_kind = {"HtmlFormatHelper", L"Windows.ApplicationModel.DataTransfer.HtmlFormatHelper", html_class_iids,
	NULL};
struct object html_format_helper_class = {{html_factory_table.slots, html_statics_table.slots}, &html_class_kind, 1};
