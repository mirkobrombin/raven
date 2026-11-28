/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven Windows Runtime component: system classes of the Windows Runtime that desktop programs ask for while they start, served from one
 * library as twinapi.appcore.dll and its neighbours serve them on Windows.
 *
 * - Windows.Globalization.Language: a BCP 47 tag with the names and layout direction of the locale of that name.
 * - Windows.System.UserProfile.GlobalizationPreferences: the calendar, clock, currency, region, first weekday and languages of the
 *   user's locale, which an app can read but not set.
 * - Windows.System.Profile.RetailInfo and SharedModeSettings: the computer is not a store demonstration unit and not a shared device.
 * - Windows.System.Profile.AnalyticsInfo: a desktop, of the family Windows.Desktop at the version Windows reports.
 * - Windows.Networking.Connectivity.NetworkInformation: one wired connection profile with Internet access when the computer has an IPv4
 *   address beyond loopback, as raven-netlist answers the same question through COM; its cost is unrestricted.
 * - Windows.Security.Authentication.OnlineId.OnlineIdSystemAuthenticator and Windows.Security.Authentication.Web.Core.
 *   WebAuthenticationCoreManager: the system has no Microsoft account and no web account provider, so a ticket request fails and a
 *   provider search finds none. Programs then sign in their own way; nothing here grants an identity or a licence.
 * - Windows.ApplicationModel.Core.CoreApplication: the process's one property set, and the view of an app it runs, which core_window.c
 *   makes with its core window; a desktop process that runs none has no view.
 * - Windows.Foundation.PropertyValue: boxes of values, in value.c; Windows.Foundation.Collections.PropertySet, in map.c.
 * - Windows.Storage.StorageFile, StorageFolder, FileIO, Streams.Buffer and Streams.InMemoryRandomAccessStream, in storage.c.
 * - Windows.Storage.Streams.DataReader and DataWriter, in data.c.
 * - Windows.Storage.Streams.RandomAccessStream and RandomAccessStreamReference, in streams.c.
 * - Windows.Storage.KnownFolders, in known.c.
 * - Windows.Storage.Pickers.FileOpenPicker, FileSavePicker and FolderPicker, in pickers.c.
 * - Windows.System.Launcher and LauncherOptions, in launcher.c.
 * - Windows.ApplicationModel.DataTransfer.Clipboard, DataPackage, StandardDataFormats, HtmlFormatHelper and ClipboardContentOptions,
 *   in clipboard.c.
 * - Windows.UI.Popups.MessageDialog, PopupMenu, UICommand and UICommandSeparator, in popups.c.
 * - Windows.UI.ViewManagement.ApplicationView and its title bar, in view.c.
 * - Windows.Security.Cryptography.CryptographicBuffer and the hash, MAC and symmetric algorithms of its Core, in crypto.c.
 * - Windows.System.Threading.ThreadPool and ThreadPoolTimer, in threading.c.
 * - Windows.Foundation.Metadata.ApiInformation: a type is present when Raven registers a runtime class of that name, and so are its
 *   members; no API contract is reported present, so programs take the paths they keep for systems without one.
 *
 * Asynchronous operations complete before they are returned and call a completion handler as soon as it is set. Events take handlers
 * and never raise them. A method this component does not serve, or an interface it does not have, is named in Raven's trace. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <appmodel.h>
#include <iphlpapi.h>

/* Interfaces of the Windows Runtime, with the identifiers its metadata gives them; a parameterized one's identifier is derived from its
 * signature as the runtime derives it, and the signature is written beside it. */
static const IID iid_language = {0xea79a752, 0xf7c2, 0x4265, {0xb1, 0xbd, 0xc4, 0xde, 0xc4, 0xe4, 0xf0, 0x80}};
static const IID iid_language2 = {0x6a47e5b5, 0xd94d, 0x4886, {0xa4, 0x04, 0xa5, 0xa5, 0xb9, 0xd5, 0xb4, 0x94}};
static const IID iid_language_factory = {0x9b0252ac, 0x0c27, 0x44f8, {0xb7, 0x92, 0x97, 0x93, 0xfb, 0x66, 0xc6, 0x3e}};
static const IID iid_language_statics = {0xb23cd557, 0x0865, 0x46d4, {0x89, 0xb8, 0xd5, 0x9b, 0xe8, 0x99, 0x0f, 0x0d}};
static const IID iid_language_statics2 = {0x30199f6e, 0x914b, 0x4b2a, {0x9d, 0x6e, 0xe3, 0xb0, 0xe2, 0x7d, 0xbe, 0x4f}};
static const IID iid_application_languages = {0x75b40847, 0x0a4c, 0x4a92, {0x95, 0x65, 0xfd, 0x63, 0xc9, 0x5f, 0x7a, 0xed}};
static const IID iid_retail_info = {0x0712c6b8, 0x8b92, 0x4f2a, {0x84, 0x99, 0x03, 0x1f, 0x17, 0x98, 0xd6, 0xef}};
static const IID iid_shared_mode = {0x893df40e, 0xcad6, 0x4d50, {0x8c, 0x49, 0x6f, 0xcf, 0xc0, 0x3e, 0xdb, 0x29}};
static const IID iid_shared_mode2 = {0x608988a4, 0xccf1, 0x4ee8, {0xa5, 0xe2, 0xfd, 0x6a, 0x1d, 0x0c, 0xfa, 0xc8}};
static const IID iid_analytics_info = {0x1d5ee066, 0x188d, 0x5ba9, {0x43, 0x87, 0xac, 0xae, 0xb0, 0xe7, 0xe3, 0x05}};
static const IID iid_analytics_version = {0x926130b8, 0x9955, 0x4c74, {0xbd, 0xc1, 0x7c, 0xd0, 0xde, 0xcf, 0x9b, 0x03}};
static const IID iid_network_information = {0x5074f851, 0x950d, 0x4165, {0x9c, 0x15, 0x36, 0x56, 0x19, 0x48, 0x1e, 0xea}};
static const IID iid_network_information2 = {0x459ced14, 0x2832, 0x49b6, {0xba, 0x6e, 0xe2, 0x65, 0xf0, 0x47, 0x86, 0xa8}};
static const IID iid_connection_profile = {0x71ba143c, 0x598e, 0x49d0, {0x84, 0xeb, 0x8f, 0xeb, 0xae, 0xdc, 0xc1, 0x95}};
static const IID iid_connection_profile2 = {0xe2045145, 0x4c9f, 0x400c, {0x91, 0x50, 0x7e, 0xc7, 0xd6, 0xe2, 0x88, 0x8a}};
static const IID iid_connection_cost = {0xbad7d829, 0x3416, 0x4b10, {0xa2, 0x02, 0xba, 0xc0, 0xb0, 0x75, 0xbd, 0xae}};
static const IID iid_connection_cost2 = {0x8e113a05, 0xe209, 0x4549, {0xbb, 0x25, 0x5e, 0x0d, 0xb6, 0x91, 0xcb, 0x05}};
static const IID iid_online_id_statics = {0x85047792, 0xf634, 0x41e3, {0x96, 0xa4, 0x51, 0x64, 0xe9, 0x02, 0xc7, 0x40}};
static const IID iid_online_id_for_user = {0x5798befb, 0x1de4, 0x4186, {0xa2, 0xe6, 0xb5, 0x63, 0xf8, 0x6a, 0xaf, 0x44}};
static const IID iid_ticket_request = {0x297445d3, 0xfb63, 0x4135, {0x89, 0x09, 0x4e, 0x35, 0x4c, 0x06, 0x14, 0x66}};
static const IID iid_ticket_request_factory = {0xbebb0a08, 0x9e73, 0x4077, {0x96, 0x14, 0x08, 0x61, 0x4c, 0x0b, 0xc2, 0x45}};
static const IID iid_ticket_result = {0xdb0a5ff8, 0xb098, 0x4acd, {0x9d, 0x13, 0x9e, 0x64, 0x06, 0x52, 0xb5, 0xb6}};
static const IID iid_web_core = {0x6aca7c92, 0xa581, 0x4479, {0x9c, 0x10, 0x75, 0x2e, 0xff, 0x44, 0xfd, 0x34}};
static const IID iid_web_core2 = {0xf584184a, 0x8b57, 0x4820, {0xb6, 0xa4, 0x70, 0xa5, 0xb6, 0xfc, 0xf4, 0x4a}};
static const IID iid_web_core3 = {0x2404eeb2, 0x8924, 0x4d93, {0xab, 0x3a, 0x99, 0x68, 0x8b, 0x41, 0x9d, 0x56}};
static const IID iid_web_core4 = {0x54e633fe, 0x96e0, 0x41e8, {0x98, 0x32, 0x12, 0x98, 0x89, 0x7c, 0x2a, 0xaf}};
static const IID iid_web_core5 = {0xd07c1ded, 0x270f, 0x4554, {0x99, 0x66, 0x27, 0xb7, 0xdf, 0x05, 0xb9, 0x65}};
static const IID iid_core_application = {0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
static const IID iid_core_application2 = {0x998681fb, 0x1ab6, 0x4b7f, {0xbe, 0x4a, 0x9a, 0x06, 0x45, 0x22, 0x4c, 0x04}};
static const IID iid_core_application3 = {0xfeec0d39, 0x598b, 0x4507, {0x8a, 0x67, 0x77, 0x26, 0x32, 0x58, 0x0a, 0x57}};
static const IID iid_core_application_exit = {0xcf86461d, 0x261e, 0x4b72, {0x9a, 0xcd, 0x44, 0xed, 0x2a, 0xce, 0x6a, 0x29}};
static const IID iid_core_immersive = {0x1ada0e3e, 0xe4a2, 0x4123, {0xb4, 0x51, 0xdc, 0x96, 0xbf, 0x80, 0x04, 0x19}};
static const IID iid_core_unhandled_error = {0xf0e24ab0, 0xdd09, 0x42e1, {0xb0, 0xbc, 0xe0, 0xe1, 0x31, 0xf7, 0x8d, 0x7e}};
/* pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};rc(Windows.Security.Credentials.WebAccountProvider;{29dcc8c3-7ab9-4a7c-a336-b942f9dbf7c7})) */
static const IID iid_provider_operation = {0x88c66009, 0x12f7, 0x58e2, {0x8d, 0xbe, 0x6e, 0xfc, 0x62, 0x0c, 0x85, 0xba}};
/* pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};rc(Windows.Security.Authentication.OnlineId.OnlineIdSystemTicketResult;
 * {db0a5ff8-b098-4acd-9d13-9e640652b5b6})) */
static const IID iid_ticket_operation = {0x162f5870, 0x5a4a, 0x503c, {0x98, 0x7f, 0xa0, 0x5a, 0x13, 0x12, 0xd8, 0xe4}};
/* IVectorView, IIterable and IIterator of rc(Windows.Networking.Connectivity.ConnectionProfile;{71ba143c-598e-49d0-84eb-8febaedcc195}) */
static const IID iid_profile_view = {0x6db1b492, 0x3852, 0x5df8, {0xa2, 0x9d, 0x69, 0x44, 0x00, 0x2f, 0x58, 0xd4}};
static const IID iid_profile_iterable = {0x34dabef9, 0x87d0, 0x5b1c, {0xa7, 0xac, 0x9d, 0x29, 0x0a, 0xde, 0xb0, 0xc8}};
static const IID iid_profile_iterator = {0x89913732, 0xa08b, 0x5cb2, {0xaf, 0x16, 0xbb, 0xbb, 0x22, 0x23, 0x83, 0x9e}};
/* The same of rc(Windows.Networking.Connectivity.LanIdentifier;{48aa53aa-1108-4546-a6cb-9a74da4b7ba0}) */
static const IID iid_lan_view = {0x41286159, 0xb91d, 0x5736, {0xad, 0x8b, 0xe1, 0x6f, 0xcf, 0x8a, 0xce, 0xd0}};
static const IID iid_lan_iterable = {0xaccef3cd, 0x5d92, 0x5c01, {0x8a, 0xc4, 0x79, 0xfe, 0x74, 0xcd, 0x73, 0x3e}};
static const IID iid_lan_iterator = {0x2c5d2f7e, 0xce9c, 0x5253, {0xa0, 0xf4, 0x01, 0xe5, 0xbd, 0xc1, 0x19, 0x88}};
/* The same of rc(Windows.Networking.HostName;{bf8ecaad-ed96-49a7-9084-d416cae88dcb}) */
static const IID iid_host_view = {0xf4706ab1, 0x55a3, 0x5270, {0xaf, 0xb2, 0x73, 0x29, 0x88, 0xfe, 0x82, 0x27}};
static const IID iid_host_iterable = {0x9e5f3ed0, 0xcf1c, 0x5d38, {0x83, 0x2c, 0xac, 0xea, 0x61, 0x64, 0xbf, 0x5c}};
static const IID iid_host_iterator = {0x557bf83c, 0xa428, 0x5dbd, {0xa0, 0xfe, 0x05, 0xf6, 0xee, 0x54, 0x3d, 0x45}};
static const IID iid_api_information = {0x997439fe, 0xf681, 0x4a11, {0xb4, 0x16, 0xc1, 0x3a, 0x47, 0xe8, 0xba, 0x36}};

/* The collections this file hands out. */
static const struct winrt_items_kind profile_list = {&iid_profile_view, &iid_profile_iterable, &iid_profile_iterator, FALSE, NULL};
static const struct winrt_items_kind lan_list = {&iid_lan_view, &iid_lan_iterable, &iid_lan_iterator, FALSE, NULL};
static const struct winrt_items_kind host_list = {&iid_host_view, &iid_host_iterable, &iid_host_iterator, FALSE, NULL};

/* The most languages a list of them is read with. */
#define MOST_ITEMS 4


/* Windows.System.Profile.RetailInfo and SharedModeSettings. */
static HRESULT WINAPI retail_demo(void *iface, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI retail_properties(void *iface, void **out)
{
	return map_view(NULL, 0, out);
}

static HRESULT WINAPI shared_mode_enabled(void *iface, boolean *out) { return answer_flag(out, FALSE); }
static HRESULT WINAPI shared_mode_avoid_storage(void *iface, boolean *out) { return answer_flag(out, FALSE); }

TABLE(retail_factory_table, 0, FACTORY);
TABLE(retail_statics_table, 1, INSPECTABLE, (const void *)retail_demo, (const void *)retail_properties);
static const struct face_iid retail_iids[] = {{&iid_factory, 0}, {&iid_retail_info, 1}, {NULL, 0}};
static const struct kind retail_kind = {"RetailInfo", L"Windows.System.Profile.RetailInfo", retail_iids, NULL};
static struct object retail_info = {{retail_factory_table.slots, retail_statics_table.slots}, &retail_kind, 1};

TABLE(shared_factory_table, 0, FACTORY);
TABLE(shared_statics_table, 1, INSPECTABLE, (const void *)shared_mode_enabled);
TABLE(shared_statics2_table, 2, INSPECTABLE, (const void *)shared_mode_avoid_storage);
static const struct face_iid shared_iids[] = {{&iid_factory, 0}, {&iid_shared_mode, 1}, {&iid_shared_mode2, 2}, {NULL, 0}};
static const struct kind shared_kind = {"SharedModeSettings", L"Windows.System.Profile.SharedModeSettings", shared_iids, NULL};
static struct object shared_mode = {{shared_factory_table.slots, shared_statics_table.slots, shared_statics2_table.slots}, &shared_kind, 1};

/* Windows.System.Profile.AnalyticsInfo. The family's version is written as Windows writes it: the major, minor, build and revision numbers
 * in sixteen bits each, read as one number in decimal. The numbers are those RtlGetVersion reports and the registry's update build
 * revision. */
static HRESULT WINAPI analytics_family(void *iface, HSTRING *out)
{
	static const WCHAR family[] = L"Windows.Desktop";
	if (!out)
		return E_POINTER;
	return WindowsCreateString(family, ARRAYSIZE(family) - 1, out);
}

static HRESULT WINAPI analytics_family_version(void *iface, HSTRING *out)
{
	LONG (WINAPI *get_version)(RTL_OSVERSIONINFOW *) =
		(LONG (WINAPI *)(RTL_OSVERSIONINFOW *))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	RTL_OSVERSIONINFOW version = {sizeof version};
	DWORD revision = 0, size = sizeof revision;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!get_version || get_version(&version))
		return E_FAIL;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, NULL, &revision, &size))
		revision = 0;
	ULONGLONG packed = (ULONGLONG)(version.dwMajorVersion & 0xffff) << 48 | (ULONGLONG)(version.dwMinorVersion & 0xffff) << 32 |
		(ULONGLONG)(version.dwBuildNumber & 0xffff) << 16 | (revision & 0xffff);
	WCHAR text[20];
	UINT32 at = ARRAYSIZE(text);
	do {
		text[--at] = (WCHAR)(L'0' + packed % 10);
		packed /= 10;
	} while (packed);
	return WindowsCreateString(text + at, ARRAYSIZE(text) - at, out);
}

static HRESULT WINAPI analytics_form(void *iface, HSTRING *out)
{
	static const WCHAR form[] = L"Desktop";
	if (!out)
		return E_POINTER;
	return WindowsCreateString(form, ARRAYSIZE(form) - 1, out);
}

TABLE(analytics_version_table, 0, INSPECTABLE, (const void *)analytics_family, (const void *)analytics_family_version);
static const struct face_iid analytics_version_iids[] = {{&iid_analytics_version, 0}, {NULL, 0}};
static const struct kind analytics_version_kind = {"AnalyticsVersionInfo", L"Windows.System.Profile.AnalyticsVersionInfo",
	analytics_version_iids, NULL};
static struct object analytics_version = {{analytics_version_table.slots}, &analytics_version_kind, 1};

static HRESULT WINAPI analytics_version_info(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = face_of(&analytics_version, 0);
	return S_OK;
}

TABLE(analytics_factory_table, 0, FACTORY);
TABLE(analytics_statics_table, 1, INSPECTABLE, (const void *)analytics_version_info, (const void *)analytics_form);
static const struct face_iid analytics_iids[] = {{&iid_factory, 0}, {&iid_analytics_info, 1}, {NULL, 0}};
static const struct kind analytics_kind = {"AnalyticsInfo", L"Windows.System.Profile.AnalyticsInfo", analytics_iids, NULL};
static struct object analytics_info = {{analytics_factory_table.slots, analytics_statics_table.slots}, &analytics_kind, 1};

/* Windows.Networking.Connectivity: the computer is connected when it has an IPv4 address beyond loopback. */
static BOOL connected(void)
{
	ULONG size = 0;
	if (GetAdaptersInfo(NULL, &size) != ERROR_BUFFER_OVERFLOW || !size)
		return FALSE;
	IP_ADAPTER_INFO *adapters = ALLOC(size);
	const BOOL result = adapters && GetAdaptersInfo(adapters, &size) == NO_ERROR;
	FREE(adapters);
	return result;
}

static HRESULT WINAPI cost_type(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* NetworkCostType.Unrestricted */
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI cost_false(void *iface, boolean *out) { return answer_flag(out, FALSE); }

TABLE(cost_table, 0, INSPECTABLE, (const void *)cost_type, (const void *)cost_false, (const void *)cost_false, (const void *)cost_false);
TABLE(cost2_table, 1, INSPECTABLE, (const void *)cost_false);
static const struct face_iid cost_iids[] = {{&iid_connection_cost, 0}, {&iid_connection_cost2, 1}, {NULL, 0}};
static const struct kind cost_kind = {"ConnectionCost", L"Windows.Networking.Connectivity.ConnectionCost", cost_iids, NULL};
static struct object connection_cost = {{cost_table.slots, cost2_table.slots}, &cost_kind, 1};

static const WCHAR network_name[] = L"Network";

static HRESULT WINAPI profile_name(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsCreateString(network_name, ARRAYSIZE(network_name) - 1, out);
}

static HRESULT WINAPI profile_level(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* NetworkConnectivityLevel.InternetAccess or None */
	*out = connected() ? 3 : 0;
	return S_OK;
}

static HRESULT WINAPI profile_names(void *iface, void **out)
{
	HSTRING name;
	HRESULT result = WindowsCreateString(network_name, ARRAYSIZE(network_name) - 1, &name);
	if (FAILED(result))
		return result;
	result = winrt_items(&string_list, (IInspectable **)&name, 1, FALSE, out);
	WindowsDeleteString(name);
	return result;
}

static HRESULT WINAPI profile_cost(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = face_of(&connection_cost, 0);
	return S_OK;
}

static HRESULT WINAPI profile_plan(void *iface, void **out) { answer_null(out); return missing("ConnectionProfile.GetDataPlanStatus"); }
static HRESULT WINAPI profile_adapter(void *iface, void **out) { answer_null(out); return missing("ConnectionProfile.NetworkAdapter"); }

static HRESULT WINAPI profile_usage(void *iface, INT64 start, INT64 end, void **out)
{
	answer_null(out);
	return missing("ConnectionProfile.GetLocalUsage");
}

static HRESULT WINAPI profile_roaming_usage(void *iface, INT64 start, INT64 end, UINT32 states, void **out)
{
	answer_null(out);
	return missing("ConnectionProfile.GetLocalUsagePerRoamingStates");
}

static HRESULT WINAPI profile_security(void *iface, void **out) { answer_null(out); return missing("ConnectionProfile.NetworkSecuritySettings"); }
static HRESULT WINAPI profile_false(void *iface, boolean *out) { return answer_flag(out, FALSE); }
static HRESULT WINAPI profile_none(void *iface, void **out) { return answer_null(out); }

static HRESULT WINAPI profile_domain(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* DomainConnectivityLevel.None */
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI profile_usage_async(void *iface, INT64 start, INT64 end, UINT32 granularity, UINT32 states, void **out)
{
	answer_null(out);
	return missing("ConnectionProfile.GetNetworkUsageAsync");
}

static HRESULT WINAPI profile_intervals_async(void *iface, INT64 start, INT64 end, UINT32 states, void **out)
{
	answer_null(out);
	return missing("ConnectionProfile.GetConnectivityIntervalsAsync");
}

TABLE(profile_table, 0, INSPECTABLE, (const void *)profile_name, (const void *)profile_level, (const void *)profile_names,
	(const void *)profile_cost, (const void *)profile_plan, (const void *)profile_adapter, (const void *)profile_usage,
	(const void *)profile_roaming_usage, (const void *)profile_security);
TABLE(profile2_table, 1, INSPECTABLE, (const void *)profile_false, (const void *)profile_false, (const void *)profile_none,
	(const void *)profile_none, (const void *)profile_none, (const void *)profile_none, (const void *)profile_domain,
	(const void *)profile_usage_async, (const void *)profile_intervals_async);
static const struct face_iid profile_iids[] = {{&iid_connection_profile, 0}, {&iid_connection_profile2, 1}, {NULL, 0}};
static const struct kind profile_kind = {"ConnectionProfile", L"Windows.Networking.Connectivity.ConnectionProfile", profile_iids, NULL};
static struct object connection_profile = {{profile_table.slots, profile2_table.slots}, &profile_kind, 1};

static HRESULT WINAPI network_profiles(void *iface, void **out)
{
	IInspectable *profile = face_of(&connection_profile, 0);
	return winrt_items(&profile_list, &profile, connected() ? 1 : 0, FALSE, out);
}

static HRESULT WINAPI network_internet_profile(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = connected() ? face_of(&connection_profile, 0) : NULL;
	return S_OK;
}

static HRESULT WINAPI network_lan_identifiers(void *iface, void **out) { return winrt_items(&lan_list, NULL, 0, FALSE, out); }
static HRESULT WINAPI network_host_names(void *iface, void **out) { return winrt_items(&host_list, NULL, 0, FALSE, out); }

static HRESULT WINAPI network_proxy(void *iface, void *uri, void **out)
{
	answer_null(out);
	return missing("NetworkInformation.GetProxyConfigurationAsync");
}

static HRESULT WINAPI network_endpoints(void *iface, void *pairs, UINT32 options, void **out)
{
	answer_null(out);
	return missing("NetworkInformation.GetSortedEndpointPairs");
}

static HRESULT WINAPI network_find_profiles(void *iface, void *filter, void **out)
{
	answer_null(out);
	return missing("NetworkInformation.FindConnectionProfilesAsync");
}

TABLE(network_factory_table, 0, FACTORY);
TABLE(network_statics_table, 1, INSPECTABLE, (const void *)network_profiles, (const void *)network_internet_profile,
	(const void *)network_lan_identifiers, (const void *)network_host_names, (const void *)network_proxy, (const void *)network_endpoints,
	(const void *)event_add, (const void *)event_remove);
TABLE(network_statics2_table, 2, INSPECTABLE, (const void *)network_find_profiles);
static const struct face_iid network_iids[] = {{&iid_factory, 0}, {&iid_network_information, 1}, {&iid_network_information2, 2}, {NULL, 0}};
static const struct kind network_kind = {"NetworkInformation", L"Windows.Networking.Connectivity.NetworkInformation", network_iids, NULL};
static struct object network_information = {{network_factory_table.slots, network_statics_table.slots, network_statics2_table.slots},
	&network_kind, 1};

/* Windows.Security.Authentication.OnlineId: no Microsoft account is connected to the system, so a ticket request fails. */
static HRESULT WINAPI ticket_identity(void *iface, void **out) { return answer_null(out); }

static HRESULT WINAPI ticket_status(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	/* OnlineIdSystemTicketStatus.Error */
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI ticket_error(void *iface, HRESULT *out)
{
	if (!out)
		return E_POINTER;
	*out = NO_SUCH_USER;
	return S_OK;
}

TABLE(ticket_table, 0, INSPECTABLE, (const void *)ticket_identity, (const void *)ticket_status, (const void *)ticket_error);
static const struct face_iid ticket_iids[] = {{&iid_ticket_result, 0}, {NULL, 0}};
static const struct kind ticket_kind = {"OnlineIdSystemTicketResult", L"Windows.Security.Authentication.OnlineId.OnlineIdSystemTicketResult",
	ticket_iids, NULL};
static struct object no_ticket = {{ticket_table.slots}, &ticket_kind, 1};

static GUID application_id;

static HRESULT WINAPI for_user_ticket(void *iface, void *request, void **out)
{
	if (!request)
		return E_INVALIDARG;
	const union winrt_value ticket = {.object = face_of(&no_ticket, 0)};
	return winrt_async_done(&iid_ticket_operation, WINRT_RESULT_OBJECT, S_OK, &ticket, out);
}

static HRESULT WINAPI for_user_set_application(void *iface, GUID value)
{
	application_id = value;
	return S_OK;
}

static HRESULT WINAPI for_user_application(void *iface, GUID *out)
{
	if (!out)
		return E_POINTER;
	*out = application_id;
	return S_OK;
}

static HRESULT WINAPI for_user_user(void *iface, void **out) { return answer_null(out); }

TABLE(for_user_table, 0, INSPECTABLE, (const void *)for_user_ticket, (const void *)for_user_set_application, (const void *)for_user_application,
	(const void *)for_user_user);
static const struct face_iid for_user_iids[] = {{&iid_online_id_for_user, 0}, {NULL, 0}};
static const struct kind for_user_kind = {"OnlineIdSystemAuthenticatorForUser",
	L"Windows.Security.Authentication.OnlineId.OnlineIdSystemAuthenticatorForUser", for_user_iids, NULL};
static struct object authenticator_for_user = {{for_user_table.slots}, &for_user_kind, 1};

static HRESULT WINAPI online_id_default(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = face_of(&authenticator_for_user, 0);
	return S_OK;
}

static HRESULT WINAPI online_id_for_user(void *iface, void *user, void **out) { return online_id_default(iface, out); }

TABLE(online_id_factory_table, 0, FACTORY);
TABLE(online_id_statics_table, 1, INSPECTABLE, (const void *)online_id_default, (const void *)online_id_for_user);
static const struct face_iid online_id_iids[] = {{&iid_factory, 0}, {&iid_online_id_statics, 1}, {NULL, 0}};
static const struct kind online_id_kind = {"OnlineIdSystemAuthenticator", L"Windows.Security.Authentication.OnlineId.OnlineIdSystemAuthenticator",
	online_id_iids, NULL};
static struct object online_id = {{online_id_factory_table.slots, online_id_statics_table.slots}, &online_id_kind, 1};

/* OnlineIdServiceTicketRequest: the service and policy a ticket would be asked for, kept as given; the advanced form names no policy. */
struct ticket_request {
	struct object base;
	HSTRING service, policy;
};

static void destroy_ticket_request(struct object *base)
{
	struct ticket_request *self = (struct ticket_request *)base;
	WindowsDeleteString(self->service);
	WindowsDeleteString(self->policy);
}

static HRESULT WINAPI ticket_request_service(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct ticket_request *)owner(iface))->service, out);
}

static HRESULT WINAPI ticket_request_policy(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct ticket_request *)owner(iface))->policy, out);
}

TABLE(ticket_request_table, 0, INSPECTABLE, (const void *)ticket_request_service, (const void *)ticket_request_policy);
static const struct face_iid ticket_request_iids[] = {{&iid_ticket_request, 0}, {NULL, 0}};
static const struct kind ticket_request_kind = {"OnlineIdServiceTicketRequest",
	L"Windows.Security.Authentication.OnlineId.OnlineIdServiceTicketRequest", ticket_request_iids, destroy_ticket_request};

static HRESULT ticket_request(HSTRING service, HSTRING policy, void **out)
{
	static const void *const *const tables[] = {ticket_request_table.slots};
	if (!out)
		return E_POINTER;
	struct ticket_request *self = make(sizeof *self, &ticket_request_kind, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	if (FAILED(WindowsDuplicateString(service, &self->service)) || FAILED(WindowsDuplicateString(policy, &self->policy))) {
		release(*out);
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	return S_OK;
}

static HRESULT WINAPI ticket_request_create(void *iface, HSTRING service, HSTRING policy, void **out)
{
	return ticket_request(service, policy, out);
}

static HRESULT WINAPI ticket_request_create_advanced(void *iface, HSTRING service, void **out) { return ticket_request(service, NULL, out); }

TABLE(ticket_request_factory_table, 0, FACTORY);
TABLE(ticket_request_create_table, 1, INSPECTABLE, (const void *)ticket_request_create, (const void *)ticket_request_create_advanced);
static const struct face_iid ticket_request_factory_iids[] = {{&iid_factory, 0}, {&iid_ticket_request_factory, 1}, {NULL, 0}};
static const struct kind ticket_request_factory_kind = {"OnlineIdServiceTicketRequest",
	L"Windows.Security.Authentication.OnlineId.OnlineIdServiceTicketRequest", ticket_request_factory_iids, NULL};
static struct object ticket_request_factory = {{ticket_request_factory_table.slots, ticket_request_create_table.slots},
	&ticket_request_factory_kind, 1};

/* Windows.Security.Authentication.Web.Core: there is no web account provider. A search for one finds none, and a request names a
 * provider, which a program cannot have from this system. */
static HRESULT no_provider(void **out) { return winrt_async_done(&iid_provider_operation, WINRT_RESULT_OBJECT, S_OK, NULL, out); }

static HRESULT WINAPI web_one(void *iface, void *request, void **out)
{
	answer_null(out);
	return E_INVALIDARG;
}

static HRESULT WINAPI web_two(void *iface, void *request, void *account, void **out)
{
	answer_null(out);
	return E_INVALIDARG;
}

static HRESULT WINAPI web_find_provider(void *iface, HSTRING id, void **out) { return no_provider(out); }
static HRESULT WINAPI web_find_provider_authority(void *iface, HSTRING id, HSTRING authority, void **out) { return no_provider(out); }

static HRESULT WINAPI web_find_provider_user(void *iface, HSTRING id, HSTRING authority, void *user, void **out)
{
	return no_provider(out);
}

static HRESULT WINAPI web_monitor(void *iface, void *accounts, void **out)
{
	answer_null(out);
	return missing("WebAuthenticationCoreManager.CreateWebAccountMonitor");
}

TABLE(web_factory_table, 0, FACTORY);
TABLE(web_statics_table, 1, INSPECTABLE, (const void *)web_one, (const void *)web_two, (const void *)web_one, (const void *)web_two,
	(const void *)web_two, (const void *)web_find_provider, (const void *)web_find_provider_authority);
TABLE(web_statics2_table, 2, INSPECTABLE, (const void *)web_find_provider_user);
TABLE(web_statics3_table, 3, INSPECTABLE, (const void *)web_monitor);
TABLE(web_statics4_table, 4, INSPECTABLE, (const void *)web_one, (const void *)web_two, (const void *)web_find_provider,
	(const void *)web_find_provider_authority, (const void *)web_find_provider_user);
TABLE(web_statics5_table, 5, INSPECTABLE, (const void *)web_one);
static const struct face_iid web_iids[] = {{&iid_factory, 0}, {&iid_web_core, 1}, {&iid_web_core2, 2}, {&iid_web_core3, 3}, {&iid_web_core4, 4},
	{&iid_web_core5, 5}, {NULL, 0}};
static const struct kind web_kind = {"WebAuthenticationCoreManager", L"Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager",
	web_iids, NULL};
static struct object web_core = {{web_factory_table.slots, web_statics_table.slots, web_statics2_table.slots, web_statics3_table.slots,
	web_statics4_table.slots, web_statics5_table.slots}, &web_kind, 1};

/* Windows.ApplicationModel.Core.CoreApplication: the package identity is the package's, and the views are those of the app it runs.
 * Id is the application's identifier within its package, the part of its user model identifier after the family name, which is at
 * most 130 characters long. */
static HRESULT WINAPI core_id(void *iface, HSTRING *out)
{
	WCHAR model[130];
	UINT32 length = ARRAYSIZE(model);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (GetCurrentApplicationUserModelId(&length, model) != ERROR_SUCCESS)
		return NO_PACKAGE;
	const WCHAR *id = model;
	while (*id && *id != L'!')
		++id;
	if (!*id)
		return NO_PACKAGE;
	++id;
	return WindowsCreateString(id, (UINT32)lstrlenW(id), out);
}

/* The process's one property set, made when a program first asks for it. */
static struct property_set *process_properties;
static INIT_ONCE process_properties_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK make_process_properties(INIT_ONCE *once, void *parameter, void **context)
{
	process_properties = property_set_make();
	return process_properties != NULL;
}

static HRESULT WINAPI core_properties(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!InitOnceExecuteOnce(&process_properties_once, make_process_properties, NULL, NULL))
		return E_OUTOFMEMORY;
	reference(*out = property_set_face(process_properties));
	return S_OK;
}

static HRESULT WINAPI core_view(void *iface, void **out) { return application_view(out); }
static HRESULT WINAPI core_run(void *iface, IUnknown *source) { return application_run(source); }

static HRESULT WINAPI core_run_factories(void *iface, void *factories) { return missing("CoreApplication.RunWithActivationFactories"); }
static HRESULT WINAPI core_prelaunch(void *iface, boolean value) { return S_OK; }

static HRESULT WINAPI core_restart(void *iface, HSTRING arguments, void **out)
{
	answer_null(out);
	return missing("CoreApplication.RequestRestartAsync");
}

static HRESULT WINAPI core_restart_user(void *iface, void *user, HSTRING arguments, void **out)
{
	answer_null(out);
	return missing("CoreApplication.RequestRestartForUserAsync");
}

static HRESULT WINAPI core_exit(void *iface) { return application_exit(); }
static HRESULT WINAPI core_views(void *iface, void **out) { answer_null(out); return missing("CoreApplication.Views"); }

static HRESULT WINAPI core_new_view(void *iface, void *first, void *second, void **out)
{
	answer_null(out);
	return missing("CoreApplication.CreateNewView");
}

TABLE(core_factory_table, 0, FACTORY);
TABLE(core_table, 1, INSPECTABLE, (const void *)core_id, (const void *)event_add, (const void *)event_remove, (const void *)event_add,
	(const void *)event_remove, (const void *)core_properties, (const void *)core_view, (const void *)core_run, (const void *)core_run_factories);
TABLE(core2_table, 2, INSPECTABLE, (const void *)event_add, (const void *)event_remove, (const void *)event_add, (const void *)event_remove,
	(const void *)event_add, (const void *)event_remove, (const void *)core_prelaunch);
TABLE(core3_table, 3, INSPECTABLE, (const void *)core_restart, (const void *)core_restart_user);
TABLE(core_exit_table, 4, INSPECTABLE, (const void *)core_exit, (const void *)event_add, (const void *)event_remove);
TABLE(core_immersive_table, 5, INSPECTABLE, (const void *)core_views, (const void *)core_new_view, (const void *)core_view);
TABLE(core_unhandled_table, 6, INSPECTABLE, (const void *)event_add, (const void *)event_remove);
static const struct face_iid core_iids[] = {{&iid_factory, 0}, {&iid_core_application, 1}, {&iid_core_application2, 2},
	{&iid_core_application3, 3}, {&iid_core_application_exit, 4}, {&iid_core_immersive, 5}, {&iid_core_unhandled_error, 6}, {NULL, 0}};
static const struct kind core_kind = {"CoreApplication", L"Windows.ApplicationModel.Core.CoreApplication", core_iids, NULL};
static struct object core_application = {{core_factory_table.slots, core_table.slots, core2_table.slots, core3_table.slots,
	core_exit_table.slots, core_immersive_table.slots, core_unhandled_table.slots}, &core_kind, 1};

/* Windows.Globalization.Language: a well-formed BCP 47 tag, named and laid out as the system's locale of that name says. */
struct language {
	struct object base;
	HSTRING tag;
};

static void destroy_language(struct object *base)
{
	WindowsDeleteString(((struct language *)base)->tag);
}

/* What the locale of the language's tag answers, or the empty text for a language the system has no locale for. */
static int language_info(void *iface, LCTYPE type, WCHAR *text, int capacity)
{
	const WCHAR *tag = WindowsGetStringRawBuffer(((struct language *)owner(iface))->tag, NULL);
	const int written = GetLocaleInfoEx(tag, type, text, capacity);
	if (written <= 0)
		text[0] = 0;
	return written > 0 ? written - 1 : 0;
}

static HRESULT WINAPI language_tag(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct language *)owner(iface))->tag, out);
}

/* A language without a locale is called by its tag. */
static HRESULT language_name(void *iface, LCTYPE type, HSTRING *out)
{
	WCHAR text[LOCALE_NAME_MAX_LENGTH * 2];
	if (!out)
		return E_POINTER;
	const int length = language_info(iface, type, text, ARRAYSIZE(text));
	return length ? WindowsCreateString(text, length, out) : language_tag(iface, out);
}

static HRESULT WINAPI language_display_name(void *iface, HSTRING *out) { return language_name(iface, 0x00000002 /* LOCALE_SLOCALIZEDDISPLAYNAME */, out); }
static HRESULT WINAPI language_native_name(void *iface, HSTRING *out) { return language_name(iface, 0x00000073 /* LOCALE_SNATIVEDISPLAYNAME */, out); }

static HRESULT WINAPI language_script(void *iface, HSTRING *out)
{
	WCHAR text[64];
	if (!out)
		return E_POINTER;
	language_info(iface, 0x0000006c /* LOCALE_SSCRIPTS */, text, ARRAYSIZE(text));
	/* The locale lists its scripts in order, each ended by a semicolon; the language's own is the first. */
	int length = 0;
	while (text[length] && text[length] != L';')
		++length;
	return WindowsCreateString(text, length, out);
}

/* LOCALE_IREADINGLAYOUT numbers the vertical layouts the other way round from LanguageLayoutDirection. */
static HRESULT WINAPI language_layout(void *iface, INT32 *out)
{
	static const INT32 directions[] = {0, 1, 3, 2};
	DWORD layout = 0;
	if (!out)
		return E_POINTER;
	language_info(iface, 0x00000070 /* LOCALE_IREADINGLAYOUT */ | LOCALE_RETURN_NUMBER, (WCHAR *)&layout, sizeof layout / sizeof(WCHAR));
	*out = directions[layout & 3];
	return S_OK;
}

TABLE(language_table, 0, INSPECTABLE, (const void *)language_tag, (const void *)language_display_name, (const void *)language_native_name,
	(const void *)language_script);
TABLE(language2_table, 1, INSPECTABLE, (const void *)language_layout);
static const struct face_iid language_iids[] = {{&iid_language, 0}, {&iid_language2, 1}, {NULL, 0}};
static const struct kind language_kind = {"Language", L"Windows.Globalization.Language", language_iids, destroy_language};

/* RFC 5646 in shape only: subtags of one to eight letters or digits, the first of two to eight letters. */
static BOOL language_well_formed(HSTRING tag)
{
	UINT32 length = 0, subtag = 0;
	BOOL first = TRUE;
	const WCHAR *text = WindowsGetStringRawBuffer(tag, &length);
	for (UINT32 index = 0; index <= length; ++index) {
		const WCHAR letter = index < length ? text[index] : L'-';
		if (letter == L'-') {
			if (!subtag || subtag > 8 || (first && subtag < 2))
				return FALSE;
			subtag = 0;
			first = FALSE;
			continue;
		}
		const BOOL alpha = (letter >= L'a' && letter <= L'z') || (letter >= L'A' && letter <= L'Z');
		if (!alpha && (first || letter < L'0' || letter > L'9'))
			return FALSE;
		++subtag;
	}
	return TRUE;
}

static HRESULT WINAPI language_create(void *iface, HSTRING tag, void **out)
{
	static const void *const *const tables[] = {language_table.slots, language2_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!language_well_formed(tag))
		return E_INVALIDARG;
	struct language *self = make(sizeof *self, &language_kind, tables, 2);
	if (!self)
		return E_OUTOFMEMORY;
	*out = face_of(&self->base, 0);
	if (FAILED(WindowsDuplicateString(tag, &self->tag))) {
		release(*out);
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	return S_OK;
}

static HRESULT WINAPI language_is_well_formed(void *iface, HSTRING tag, boolean *out) { return answer_flag(out, language_well_formed(tag)); }

/* The input method follows the user's language: the host's keyboard does not name one of its own. */
static HRESULT WINAPI language_input_tag(void *iface, HSTRING *out)
{
	WCHAR name[LOCALE_NAME_MAX_LENGTH];
	if (!out)
		return E_POINTER;
	const int length = GetUserDefaultLocaleName(name, ARRAYSIZE(name));
	return WindowsCreateString(name, length > 0 ? length - 1 : 0, out);
}

static HRESULT WINAPI language_set_input_tag(void *iface, HSTRING tag, boolean *out) { return answer_flag(out, FALSE); }

TABLE(language_factory_table, 0, FACTORY);
TABLE(language_create_table, 1, INSPECTABLE, (const void *)language_create);
TABLE(language_statics_table, 2, INSPECTABLE, (const void *)language_is_well_formed, (const void *)language_input_tag);
TABLE(language_statics2_table, 3, INSPECTABLE, (const void *)language_set_input_tag);
static const struct face_iid language_factory_iids[] = {{&iid_factory, 0}, {&iid_language_factory, 1}, {&iid_language_statics, 2},
	{&iid_language_statics2, 3}, {NULL, 0}};
static const struct kind language_factory_kind = {"Language", L"Windows.Globalization.Language", language_factory_iids, NULL};
static struct object language_factory = {{language_factory_table.slots, language_create_table.slots, language_statics_table.slots,
	language_statics2_table.slots}, &language_factory_kind, 1};

static HRESULT WINAPI app_language_override(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return missing("ApplicationLanguages.PrimaryLanguageOverride");
}

static HRESULT WINAPI app_set_language_override(void *iface, HSTRING tag)
{
	return missing("ApplicationLanguages.put_PrimaryLanguageOverride");
}

static HRESULT WINAPI app_languages(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	WCHAR names[MOST_ITEMS * LOCALE_NAME_MAX_LENGTH + 1];
	ULONG count = 0, size = ARRAYSIZE(names);
	if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names, &size))
		return HRESULT_FROM_WIN32(GetLastError());
	if (count > MOST_ITEMS || size < 2 || size > ARRAYSIZE(names) || names[size - 1] || names[size - 2])
		return E_UNEXPECTED;

	IInspectable *items[MOST_ITEMS];
	HSTRING_HEADER headers[MOST_ITEMS];
	const WCHAR *name = names, *end = names + size - 1;
	for (ULONG index = 0; index < count; ++index) {
		if (name >= end || !*name)
			return E_UNEXPECTED;
		const int length = lstrlenW(name);
		HSTRING item;
		const HRESULT hr = WindowsCreateStringReference(name, length, &headers[index], &item);
		if (FAILED(hr))
			return hr;
		items[index] = (IInspectable *)item;
		name += length + 1;
	}
	if (name != end)
		return E_UNEXPECTED;
	return winrt_items(&string_list, items, count, FALSE, out);
}

static HRESULT WINAPI app_manifest_languages(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return missing("ApplicationLanguages.ManifestLanguages");
}

TABLE(app_languages_statics_table, 1, INSPECTABLE, (const void *)app_language_override, (const void *)app_set_language_override,
	(const void *)app_languages, (const void *)app_manifest_languages);
static const struct face_iid app_languages_iids[] = {{&iid_factory, 0}, {&iid_application_languages, 1}, {NULL, 0}};
static const struct kind app_languages_kind = {"ApplicationLanguages", L"Windows.Globalization.ApplicationLanguages", app_languages_iids, NULL};
static struct object app_languages_factory = {{language_factory_table.slots, app_languages_statics_table.slots}, &app_languages_kind, 1};

/* Windows.System.UserProfile.GlobalizationPreferences, from the user's locale: its calendar, clock, currency, region and first day of
 * the week, and the languages ApplicationLanguages gives. The preferences can only be read: setting them answers false, as for an app
 * without the capability to change them. */
static const IID iid_globalization = {0x01bf4326, 0xed37, 0x4e96, {0xb0, 0xe9, 0xc1, 0x34, 0x0d, 0x1e, 0xa1, 0x58}};
static const IID iid_globalization2 = {0xfcce85f1, 0x4300, 0x4cd0, {0x9c, 0xac, 0x1a, 0x8e, 0x7b, 0x7e, 0x18, 0xf4}};

/* One string, in a list of it. */
static HRESULT one_name(const WCHAR *name, void **out)
{
	HSTRING_HEADER header;
	HSTRING item;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const HRESULT result = WindowsCreateStringReference(name, (UINT32)lstrlenW(name), &header, &item);
	return FAILED(result) ? result : winrt_items(&string_list, (IInspectable **)&item, 1, FALSE, out);
}

static UINT32 locale_number(LCTYPE type)
{
	UINT32 value = 0;
	GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type | LOCALE_RETURN_NUMBER, (WCHAR *)&value, sizeof value / sizeof(WCHAR));
	return value;
}

/* The calendar of the locale's CALID by the name the Windows Runtime gives it. */
static HRESULT WINAPI preferred_calendars(void *iface, void **out)
{
	switch (locale_number(LOCALE_ICALENDARTYPE)) {
	case 3: return one_name(L"JapaneseCalendar", out);
	case 4: return one_name(L"TaiwanCalendar", out);
	case 5: return one_name(L"KoreanCalendar", out);
	case 6: return one_name(L"HijriCalendar", out);
	case 7: return one_name(L"ThaiCalendar", out);
	case 8: return one_name(L"HebrewCalendar", out);
	case 22: return one_name(L"PersianCalendar", out);
	case 23: return one_name(L"UmAlQuraCalendar", out);
	default: return one_name(L"GregorianCalendar", out);
	}
}

static HRESULT WINAPI preferred_clocks(void *iface, void **out)
{
	return one_name(locale_number(LOCALE_ITIME) ? L"24HourClock" : L"12HourClock", out);
}

static HRESULT WINAPI preferred_currencies(void *iface, void **out)
{
	WCHAR code[16];
	if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SINTLSYMBOL, code, ARRAYSIZE(code)))
		return HRESULT_FROM_WIN32(GetLastError());
	return one_name(code, out);
}

static HRESULT WINAPI home_region(void *iface, HSTRING *out)
{
	WCHAR region[16];
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, region, ARRAYSIZE(region)))
		return HRESULT_FROM_WIN32(GetLastError());
	return WindowsCreateString(region, (UINT32)lstrlenW(region), out);
}

/* LOCALE_IFIRSTDAYOFWEEK counts from Monday, DayOfWeek from Sunday. */
static HRESULT WINAPI week_starts_on(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = (INT32)((locale_number(LOCALE_IFIRSTDAYOFWEEK) + 1) % 7);
	return S_OK;
}

static HRESULT WINAPI try_set_region(void *iface, HSTRING region, boolean *out) { return answer_flag(out, FALSE); }
static HRESULT WINAPI try_set_languages(void *iface, void *languages, boolean *out) { return answer_flag(out, FALSE); }

TABLE(globalization_statics_table, 1, INSPECTABLE, (const void *)preferred_calendars, (const void *)preferred_clocks,
	(const void *)preferred_currencies, (const void *)app_languages, (const void *)home_region, (const void *)week_starts_on);
TABLE(globalization_statics2_table, 2, INSPECTABLE, (const void *)try_set_region, (const void *)try_set_languages);
static const struct face_iid globalization_iids[] = {{&iid_factory, 0}, {&iid_globalization, 1}, {&iid_globalization2, 2}, {NULL, 0}};
static const struct kind globalization_kind = {"GlobalizationPreferences", L"Windows.System.UserProfile.GlobalizationPreferences",
	globalization_iids, NULL};
static struct object globalization_preferences = {{language_factory_table.slots, globalization_statics_table.slots,
	globalization_statics2_table.slots}, &globalization_kind, 1};

/* Windows.Foundation.Metadata.ApiInformation. A type is present when a runtime class of that name is registered for activation. */
static boolean registered(HSTRING type)
{
	static const WCHAR root[] = L"SOFTWARE\\Microsoft\\WindowsRuntime\\ActivatableClassId\\";
	WCHAR path[ARRAYSIZE(root) + 256];
	UINT32 length = 0;
	const WCHAR *text = WindowsGetStringRawBuffer(type, &length);
	HKEY key;
	if (!length || length >= 256)
		return FALSE;
	memcpy(path, root, sizeof(root) - sizeof(WCHAR));
	memcpy(path + ARRAYSIZE(root) - 1, text, length * sizeof(WCHAR));
	path[ARRAYSIZE(root) - 1 + length] = 0;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key))
		return FALSE;
	RegCloseKey(key);
	return TRUE;
}

static HRESULT WINAPI api_type(void *iface, HSTRING type, boolean *out) { return answer_flag(out, registered(type)); }
static HRESULT WINAPI api_member(void *iface, HSTRING type, HSTRING member, boolean *out) { return answer_flag(out, registered(type)); }

static HRESULT WINAPI api_method_arity(void *iface, HSTRING type, HSTRING member, UINT32 count, boolean *out)
{
	return answer_flag(out, registered(type));
}

static HRESULT WINAPI api_contract(void *iface, HSTRING contract, UINT16 major, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI api_contract_minor(void *iface, HSTRING contract, UINT16 major, UINT16 minor, boolean *out)
{
	return answer_flag(out, FALSE);
}

TABLE(api_factory_table, 0, FACTORY);
TABLE(api_statics_table, 1, INSPECTABLE, (const void *)api_type, (const void *)api_member, (const void *)api_method_arity,
	(const void *)api_member, (const void *)api_member, (const void *)api_member, (const void *)api_member, (const void *)api_member,
	(const void *)api_contract, (const void *)api_contract_minor);
static const struct face_iid api_iids[] = {{&iid_factory, 0}, {&iid_api_information, 1}, {NULL, 0}};
static const struct kind api_kind = {"ApiInformation", L"Windows.Foundation.Metadata.ApiInformation", api_iids, NULL};
static struct object api_information = {{api_factory_table.slots, api_statics_table.slots}, &api_kind, 1};

static struct object *const classes[] = {&language_factory, &app_languages_factory, &globalization_preferences, &retail_info, &shared_mode, &analytics_info, &network_information,
	&online_id, &ticket_request_factory, &web_core, &core_application, &api_information,
	&core_window_class, &core_cursor_class, &display_information_class,
	&package_class, &application_data_class, &property_value_class, &property_set_class, &composite_value_class,
	&storage_file_class, &storage_folder_class, &file_io_class, &buffer_class, &memory_stream_class, &data_reader_class,
	&data_writer_class, &random_access_stream_class, &stream_reference_class, &known_folders_class,
	&open_picker_class, &save_picker_class, &folder_picker_class, &launcher_class, &launcher_options_class, &clipboard_class,
	&data_package_class, &standard_formats_class, &html_format_helper_class, &clipboard_content_options_class,
	&message_dialog_class, &popup_menu_class, &ui_command_class, &ui_command_separator_class, &application_view_class,
	&cryptographic_buffer_class, &hash_provider_class, &hash_names_class, &mac_provider_class, &mac_names_class, &cryptographic_engine_class,
	&cipher_provider_class, &cipher_names_class, &thread_pool_class, &thread_pool_timer_class};

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
	UINT32 length = 0;
	if (!out)
		return E_POINTER;
	*out = NULL;
	const WCHAR *text = WindowsGetStringRawBuffer(name, &length);
	for (size_t index = 0; index < ARRAYSIZE(classes); ++index)
		if (CompareStringOrdinal(text, (int)length, classes[index]->kind->class_name, -1, FALSE) == CSTR_EQUAL) {
			*out = (IActivationFactory *)face_of(classes[index], 0);
			return S_OK;
		}
	return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
