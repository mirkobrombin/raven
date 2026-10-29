/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven Windows Runtime component: system classes of the Windows Runtime that desktop programs ask for while they start, served from one
 * library as twinapi.appcore.dll and its neighbours serve them on Windows.
 *
 * - Windows.System.Profile.RetailInfo and SharedModeSettings: the computer is not a store demonstration unit and not a shared device.
 * - Windows.Networking.Connectivity.NetworkInformation: one wired connection profile with Internet access when the computer has an IPv4
 *   address beyond loopback, as raven-netlist answers the same question through COM; its cost is unrestricted.
 * - Windows.Security.Authentication.OnlineId.OnlineIdSystemAuthenticator and Windows.Security.Authentication.Web.Core.
 *   WebAuthenticationCoreManager: the system has no Microsoft account and no web account provider, so a ticket request fails and a
 *   provider search finds none. Programs then sign in their own way; nothing here grants an identity or a licence.
 * - Windows.ApplicationModel.Core.CoreApplication: a desktop process, which has no package identity and no core window, and whose
 *   Properties are one property set for the whole process.
 *
 * Asynchronous operations complete before they are returned and call a completion handler as soon as it is set. Events take handlers
 * and never raise them. A method this component does not serve, or an interface it does not have, is named in Raven's trace. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <winstring.h>
#include <activation.h>
#include <iphlpapi.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

static LONG objects, tokens;

/* Interfaces of the Windows Runtime, with the identifiers its metadata gives them; a parameterized one's identifier is derived from its
 * signature as the runtime derives it, and the signature is written beside it. */
static const IID iid_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID iid_inspectable = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c, 0x5a, 0xd7, 0xaa, 0x65, 0x10, 0x1e, 0x90}};
static const IID iid_agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const IID iid_factory = {0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID iid_async_info = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID iid_retail_info = {0x0712c6b8, 0x8b92, 0x4f2a, {0x84, 0x99, 0x03, 0x1f, 0x17, 0x98, 0xd6, 0xef}};
static const IID iid_shared_mode = {0x893df40e, 0xcad6, 0x4d50, {0x8c, 0x49, 0x6f, 0xcf, 0xc0, 0x3e, 0xdb, 0x29}};
static const IID iid_shared_mode2 = {0x608988a4, 0xccf1, 0x4ee8, {0xa5, 0xe2, 0xfd, 0x6a, 0x1d, 0x0c, 0xfa, 0xc8}};
static const IID iid_network_information = {0x5074f851, 0x950d, 0x4165, {0x9c, 0x15, 0x36, 0x56, 0x19, 0x48, 0x1e, 0xea}};
static const IID iid_network_information2 = {0x459ced14, 0x2832, 0x49b6, {0xba, 0x6e, 0xe2, 0x65, 0xf0, 0x47, 0x86, 0xa8}};
static const IID iid_connection_profile = {0x71ba143c, 0x598e, 0x49d0, {0x84, 0xeb, 0x8f, 0xeb, 0xae, 0xdc, 0xc1, 0x95}};
static const IID iid_connection_profile2 = {0xe2045145, 0x4c9f, 0x400c, {0x91, 0x50, 0x7e, 0xc7, 0xd6, 0xe2, 0x88, 0x8a}};
static const IID iid_connection_cost = {0xbad7d829, 0x3416, 0x4b10, {0xa2, 0x02, 0xba, 0xc0, 0xb0, 0x75, 0xbd, 0xae}};
static const IID iid_connection_cost2 = {0x8e113a05, 0xe209, 0x4549, {0xbb, 0x25, 0x5e, 0x0d, 0xb6, 0x91, 0xcb, 0x05}};
static const IID iid_online_id_statics = {0x85047792, 0xf634, 0x41e3, {0x96, 0xa4, 0x51, 0x64, 0xe9, 0x02, 0xc7, 0x40}};
static const IID iid_online_id_for_user = {0x5798befb, 0x1de4, 0x4186, {0xa2, 0xe6, 0xb5, 0x63, 0xf8, 0x6a, 0xaf, 0x44}};
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
/* The same of string */
static const IID iid_string_view = {0x2f13c006, 0xa03a, 0x5f69, {0xb0, 0x90, 0x75, 0xa4, 0x3e, 0x33, 0x42, 0x3e}};
static const IID iid_string_iterable = {0xe2fcc7c1, 0x3bfc, 0x5a0b, {0xb2, 0xb0, 0x72, 0xe7, 0x69, 0xd1, 0xcb, 0x7e}};
static const IID iid_string_iterator = {0x8c304ebb, 0x6615, 0x50a4, {0x88, 0x29, 0x87, 0x9e, 0xcd, 0x44, 0x32, 0x36}};
/* IMapView<string, IInspectable>, and IIterable and IIterator of pinterface({02b51929-c1c4-4a7e-8940-0312b5c18500};string;
 * cinterface(IInspectable)) */
static const IID iid_property_view = {0xbb78502a, 0xf79d, 0x54fa, {0x92, 0xc9, 0x90, 0xc5, 0x03, 0x9f, 0xdf, 0x7e}};
static const IID iid_property_iterable = {0xfe2f3d47, 0x5d47, 0x5499, {0x83, 0x74, 0x43, 0x0c, 0x7c, 0xda, 0x02, 0x04}};
static const IID iid_property_iterator = {0x5db5fa32, 0x707c, 0x5849, {0xa0, 0x6b, 0x91, 0xc8, 0xeb, 0x9d, 0x10, 0xe8}};
/* pinterface({02b51929-c1c4-4a7e-8940-0312b5c18500};string;cinterface(IInspectable)) */
static const IID iid_property_pair = {0x09335560, 0x6c6b, 0x5a26, {0x93, 0x48, 0x97, 0xb7, 0x81, 0x13, 0x2b, 0x20}};
/* pinterface({3c2925fe-8519-45c1-aa79-197b6718c1c1};string;cinterface(IInspectable)) */
static const IID iid_property_map = {0x1b0d3570, 0x0877, 0x5ec2, {0x8a, 0x2c, 0x3b, 0x95, 0x39, 0x50, 0x6a, 0xca}};
/* pinterface({65df2bf5-bf39-41b5-aebc-5a9d865e472b};string;cinterface(IInspectable)) */
static const IID iid_observable_property_map = {0x236aac9d, 0xfb12, 0x5c4d, {0xa4, 0x1c, 0x9e, 0x44, 0x5f, 0xb4, 0xd7, 0xec}};
/* pinterface({9939f4df-050a-4c0f-aa60-77075f9c4777};string) */
static const IID iid_map_change = {0x60141efb, 0xf2f9, 0x5377, {0x96, 0xfd, 0xf8, 0xc6, 0x0d, 0x95, 0x58, 0xb5}};
static const IID iid_property_set = {0x8a43ed9f, 0xf4e6, 0x4421, {0xac, 0xf9, 0x1d, 0xab, 0x29, 0x86, 0x82, 0x0c}};

/* What the Windows Runtime and Windows report when there is nothing to give. */
#define NO_PACKAGE ((HRESULT)0x80073d54)
#define NOT_FOUND ((HRESULT)0x80070490)
#define NO_SUCH_USER ((HRESULT)0x80070525)
#define DELEGATE_ASSIGNED ((HRESULT)0x80000018)

static void trace(const char *first, const char *second)
{
	static void (WINAPI *raven_trace)(const char *);
	static BOOL looked;
	char line[200] = "twinapi: ";
	size_t at = 9;
	if (!looked) {
		raven_trace = (void (WINAPI *)(const char *))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "RavenTrace");
		looked = TRUE;
	}
	if (!raven_trace)
		return;
	for (const char *part = first; *part && at < 120; ++part)
		line[at++] = *part;
	for (const char *part = second; *part && at < 198; ++part)
		line[at++] = *part;
	line[at] = 0;
	raven_trace(line);
}

static HRESULT missing(const char *method)
{
	trace(method, " is not implemented");
	return E_NOTIMPL;
}

/* An object answers several interfaces, each a table pointer at its own place at the start of the object. Each table is preceded by the
 * number of its place, so a method finds the object from the interface it was called through. */
#define FACES 8

struct face_iid {
	const IID *iid;
	SIZE_T face;
};

struct object;

struct kind {
	const char *name;
	const WCHAR *class_name;
	/* Ends with a null identifier. */
	const struct face_iid *iids;
	/* NULL for an object that lives as long as the library. */
	void (*destroy)(struct object *);
};

struct object {
	const void *const *faces[FACES];
	const struct kind *kind;
	LONG refs;
};

#define TABLE(name, place, ...) \
	static const struct { \
		SIZE_T face; \
		const void *slots[sizeof((const void *[]){__VA_ARGS__}) / sizeof(void *)]; \
	} name = {place, {__VA_ARGS__}}

static struct object *owner(void *iface)
{
	const SIZE_T face = ((const SIZE_T *)*(const void *const **)iface)[-1];
	return (struct object *)((const void *const **)iface - face);
}

static BOOL same(REFIID left, REFIID right) { return !memcmp(left, right, sizeof(IID)); }

static void trace_iid(const struct object *self, REFIID iid)
{
	static const char digits[] = "0123456789abcdef";
	static const BYTE order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	char text[48];
	size_t at = 0;
	const BYTE *bytes = (const BYTE *)iid;
	for (size_t index = 0; index < 16; ++index) {
		if (index == 4 || index == 6 || index == 8 || index == 10)
			text[at++] = '-';
		text[at++] = digits[bytes[order[index]] >> 4];
		text[at++] = digits[bytes[order[index]] & 15];
	}
	text[at] = 0;
	char line[120] = "";
	size_t used = 0;
	for (const char *part = self->kind->name; *part && used < 70; ++part)
		line[used++] = *part;
	for (const char *part = " has no interface "; *part; ++part)
		line[used++] = *part;
	line[used] = 0;
	trace(line, text);
}

static HRESULT WINAPI object_query(void *iface, REFIID iid, void **out)
{
	struct object *self = owner(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (same(iid, &iid_unknown) || same(iid, &iid_inspectable) || same(iid, &iid_agile))
		*out = &self->faces[0];
	for (const struct face_iid *entry = self->kind->iids; !*out && entry->iid; ++entry)
		if (same(iid, entry->iid))
			*out = &self->faces[entry->face];
	if (!*out) {
		trace_iid(self, iid);
		return E_NOINTERFACE;
	}
	if (self->kind->destroy)
		InterlockedIncrement(&self->refs);
	return S_OK;
}

static ULONG WINAPI object_reference(void *iface)
{
	struct object *self = owner(iface);
	return self->kind->destroy ? (ULONG)InterlockedIncrement(&self->refs) : 2;
}

static ULONG WINAPI object_release(void *iface)
{
	struct object *self = owner(iface);
	if (!self->kind->destroy)
		return 1;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		self->kind->destroy(self);
		FREE(self);
		InterlockedDecrement(&objects);
	}
	return (ULONG)refs;
}

static HRESULT WINAPI object_iids(void *iface, ULONG *count, IID **iids)
{
	if (!count || !iids)
		return E_POINTER;
	*count = 0;
	*iids = NULL;
	return S_OK;
}

static HRESULT WINAPI object_name(void *iface, HSTRING *name)
{
	const WCHAR *text = owner(iface)->kind->class_name;
	if (!name)
		return E_POINTER;
	return WindowsCreateString(text, (UINT32)lstrlenW(text), name);
}

static HRESULT WINAPI object_trust(void *iface, TrustLevel *level)
{
	if (!level)
		return E_POINTER;
	*level = BaseTrust;
	return S_OK;
}

#define INSPECTABLE (const void *)object_query, (const void *)object_reference, (const void *)object_release, (const void *)object_iids, \
	(const void *)object_name, (const void *)object_trust

/* A new object of a kind with its interface tables in place order, and one reference. */
static void *make(size_t size, const struct kind *kind, const void *const *const *tables, size_t count)
{
	struct object *self = ALLOC(size);
	if (!self)
		return NULL;
	for (size_t index = 0; index < count; ++index)
		self->faces[index] = tables[index];
	self->kind = kind;
	self->refs = 1;
	InterlockedIncrement(&objects);
	return self;
}

static void *face_of(struct object *self, size_t face) { return &self->faces[face]; }

static void release(void *iface)
{
	if (iface)
		((IUnknown *)iface)->lpVtbl->Release((IUnknown *)iface);
}

static void reference(void *iface)
{
	if (iface)
		((IUnknown *)iface)->lpVtbl->AddRef((IUnknown *)iface);
}

static HRESULT answer_flag(boolean *out, boolean value)
{
	if (!out)
		return E_POINTER;
	*out = value;
	return S_OK;
}

static HRESULT answer_null(void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

/* An event takes a handler for something that never happens: it gets a token of its own and is not kept. */
static HRESULT WINAPI event_add(void *iface, void *handler, INT64 *out)
{
	if (!handler || !out)
		return E_INVALIDARG;
	*out = InterlockedIncrement(&tokens);
	return S_OK;
}

static HRESULT WINAPI event_remove(void *iface, INT64 token) { return S_OK; }

/* The activation factory face of a class that has only static members: the class cannot be instantiated. */
static HRESULT WINAPI static_activate(void *iface, IInspectable **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

#define FACTORY INSPECTABLE, (const void *)static_activate

/* Asynchronous operations, complete when they are made: IAsyncOperation<T> at place 0 and IAsyncInfo at place 1. */
struct operation {
	struct object base;
	IUnknown *result;
	IUnknown *handler;
};

static void destroy_operation(struct object *base)
{
	struct operation *self = (struct operation *)base;
	release(self->result);
	release(self->handler);
}

typedef HRESULT (WINAPI *completion)(IUnknown *handler, void *operation, INT32 status);

static HRESULT WINAPI operation_set_completed(void *iface, IUnknown *handler)
{
	struct operation *self = (struct operation *)owner(iface);
	if (!handler)
		return E_INVALIDARG;
	if (InterlockedCompareExchangePointer((void **)&self->handler, handler, NULL))
		return DELEGATE_ASSIGNED;
	reference(handler);
	/* AsyncStatus.Completed */
	return ((completion)((void **)handler->lpVtbl)[3])(handler, face_of(&self->base, 0), 1);
}

static HRESULT WINAPI operation_completed(void *iface, IUnknown **out)
{
	struct operation *self = (struct operation *)owner(iface);
	if (!out)
		return E_POINTER;
	reference(*out = self->handler);
	return S_OK;
}

static HRESULT WINAPI operation_results(void *iface, IUnknown **out)
{
	struct operation *self = (struct operation *)owner(iface);
	if (!out)
		return E_POINTER;
	reference(*out = self->result);
	return S_OK;
}

static HRESULT WINAPI info_id(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI info_status(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 1;
	return S_OK;
}

static HRESULT WINAPI info_error(void *iface, HRESULT *out)
{
	if (!out)
		return E_POINTER;
	*out = S_OK;
	return S_OK;
}

static HRESULT WINAPI info_nothing(void *iface) { return S_OK; }

TABLE(operation_table, 0, INSPECTABLE, (const void *)operation_set_completed, (const void *)operation_completed, (const void *)operation_results);
TABLE(info_table, 1, INSPECTABLE, (const void *)info_id, (const void *)info_status, (const void *)info_error, (const void *)info_nothing,
	(const void *)info_nothing);

static const struct face_iid provider_operation_iids[] = {{&iid_provider_operation, 0}, {&iid_async_info, 1}, {NULL, 0}};
static const struct kind provider_operation = {"IAsyncOperation<WebAccountProvider>",
	L"Windows.Foundation.IAsyncOperation`1<Windows.Security.Credentials.WebAccountProvider>", provider_operation_iids, destroy_operation};
static const struct face_iid ticket_operation_iids[] = {{&iid_ticket_operation, 0}, {&iid_async_info, 1}, {NULL, 0}};
static const struct kind ticket_operation = {"IAsyncOperation<OnlineIdSystemTicketResult>",
	L"Windows.Foundation.IAsyncOperation`1<Windows.Security.Authentication.OnlineId.OnlineIdSystemTicketResult>", ticket_operation_iids,
	destroy_operation};

/* An operation that has finished with a result, which it takes a reference to; a null result is a result too. */
static HRESULT completed(const struct kind *kind, IUnknown *result, void **out)
{
	static const void *const *const tables[] = {operation_table.slots, info_table.slots};
	if (!out)
		return E_POINTER;
	struct operation *self = make(sizeof *self, kind, tables, 2);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	reference(self->result = result);
	return S_OK;
}

/* Read-only collections: IVectorView<T> at place 0 and IIterable<T> at place 1 over objects or strings, and their iterators. A map view
 * with nothing in it is the same collection seen as IMapView<string, IInspectable>. */
#define MOST_ITEMS 4

struct vector {
	struct object base;
	const struct kind *iterator;
	BOOL strings;
	UINT32 count;
	void *items[MOST_ITEMS];
};

struct iterator {
	struct object base;
	struct vector *vector;
	UINT32 at;
};

static void destroy_vector(struct object *base)
{
	struct vector *self = (struct vector *)base;
	for (UINT32 index = 0; index < self->count; ++index)
		if (self->strings)
			WindowsDeleteString(self->items[index]);
		else
			release(self->items[index]);
}

static void destroy_iterator(struct object *base) { release(face_of(&((struct iterator *)base)->vector->base, 0)); }

/* An item handed out: a string copy or a new reference. */
static HRESULT item_out(struct vector *self, UINT32 index, void **out)
{
	if (self->strings)
		return WindowsDuplicateString(self->items[index], (HSTRING *)out);
	reference(*out = self->items[index]);
	return S_OK;
}

static HRESULT WINAPI vector_at(void *iface, UINT32 index, void **out)
{
	struct vector *self = (struct vector *)owner(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return index < self->count ? item_out(self, index, out) : E_BOUNDS;
}

static HRESULT WINAPI vector_size(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct vector *)owner(iface))->count;
	return S_OK;
}

static HRESULT WINAPI vector_index(void *iface, void *value, UINT32 *index, boolean *found)
{
	struct vector *self = (struct vector *)owner(iface);
	if (!index || !found)
		return E_POINTER;
	*index = 0;
	*found = FALSE;
	for (UINT32 at = 0; at < self->count; ++at) {
		INT32 order = 1;
		if (self->strings ? SUCCEEDED(WindowsCompareStringOrdinal(self->items[at], value, &order)) && !order : self->items[at] == value) {
			*index = at;
			*found = TRUE;
			break;
		}
	}
	return S_OK;
}

static HRESULT many(struct vector *self, UINT32 start, UINT32 capacity, void **items, UINT32 *actual)
{
	if (!actual || (capacity && !items))
		return E_POINTER;
	*actual = 0;
	if (start > self->count)
		return E_BOUNDS;
	while (start + *actual < self->count && *actual < capacity) {
		const HRESULT result = item_out(self, start + *actual, &items[*actual]);
		if (FAILED(result))
			return result;
		++*actual;
	}
	return S_OK;
}

static HRESULT WINAPI vector_many(void *iface, UINT32 start, UINT32 capacity, void **items, UINT32 *actual)
{
	return many((struct vector *)owner(iface), start, capacity, items, actual);
}

static HRESULT WINAPI iterator_current(void *iface, void **out)
{
	struct iterator *self = (struct iterator *)owner(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->at < self->vector->count ? item_out(self->vector, self->at, out) : E_BOUNDS;
}

static HRESULT WINAPI iterator_has_current(void *iface, boolean *out)
{
	struct iterator *self = (struct iterator *)owner(iface);
	return answer_flag(out, self->at < self->vector->count);
}

static HRESULT WINAPI iterator_next(void *iface, boolean *out)
{
	struct iterator *self = (struct iterator *)owner(iface);
	if (self->at < self->vector->count)
		++self->at;
	return answer_flag(out, self->at < self->vector->count);
}

static HRESULT WINAPI iterator_many(void *iface, UINT32 capacity, void **items, UINT32 *actual)
{
	struct iterator *self = (struct iterator *)owner(iface);
	const HRESULT result = many(self->vector, self->at, capacity, items, actual);
	if (SUCCEEDED(result))
		self->at += *actual;
	return result;
}

TABLE(iterator_table, 0, INSPECTABLE, (const void *)iterator_current, (const void *)iterator_has_current, (const void *)iterator_next,
	(const void *)iterator_many);

static HRESULT WINAPI vector_first(void *iface, void **out)
{
	static const void *const *const tables[] = {iterator_table.slots};
	struct vector *vector = (struct vector *)owner(iface);
	if (!out)
		return E_POINTER;
	struct iterator *self = make(sizeof *self, vector->iterator, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	self->vector = vector;
	reference(face_of(&vector->base, 0));
	return S_OK;
}

TABLE(vector_table, 0, INSPECTABLE, (const void *)vector_at, (const void *)vector_size, (const void *)vector_index, (const void *)vector_many);
TABLE(iterable_table, 1, INSPECTABLE, (const void *)vector_first);

#define COLLECTION(prefix, type_name, view, iterable, iterator) \
	static const struct face_iid prefix##_view_iids[] = {{&view, 0}, {&iterable, 1}, {NULL, 0}}; \
	static const struct kind prefix##_view = {"IVectorView<" type_name ">", L"Windows.Foundation.Collections.IVectorView`1<" type_name ">", \
		prefix##_view_iids, destroy_vector}; \
	static const struct face_iid prefix##_iterator_iids[] = {{&iterator, 0}, {NULL, 0}}; \
	static const struct kind prefix##_iterator = {"IIterator<" type_name ">", L"Windows.Foundation.Collections.IIterator`1<" type_name ">", \
		prefix##_iterator_iids, destroy_iterator};

COLLECTION(profile, "Windows.Networking.Connectivity.ConnectionProfile", iid_profile_view, iid_profile_iterable, iid_profile_iterator)
COLLECTION(lan, "Windows.Networking.Connectivity.LanIdentifier", iid_lan_view, iid_lan_iterable, iid_lan_iterator)
COLLECTION(host, "Windows.Networking.HostName", iid_host_view, iid_host_iterable, iid_host_iterator)
COLLECTION(string, "String", iid_string_view, iid_string_iterable, iid_string_iterator)

/* A collection of objects or strings, which it takes references to or copies of. */
static HRESULT collection(const struct kind *view, const struct kind *iterator, const void *const *table, BOOL strings, void *const *items,
	UINT32 count, void **out)
{
	const void *const *const tables[] = {table, iterable_table.slots};
	if (!out)
		return E_POINTER;
	struct vector *self = make(sizeof *self, view, tables, 2);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	self->iterator = iterator;
	self->strings = strings;
	for (; self->count < count && self->count < MOST_ITEMS; ++self->count) {
		if (strings)
			WindowsDuplicateString(items[self->count], (HSTRING *)&self->items[self->count]);
		else
			reference(self->items[self->count] = items[self->count]);
	}
	return S_OK;
}

static HRESULT empty_collection(const struct kind *view, const struct kind *iterator, void **out)
{
	return collection(view, iterator, vector_table.slots, FALSE, NULL, 0, out);
}

/* Maps from strings to objects. A snapshot is IMapView<String, Object> at place 0 and IIterable<IKeyValuePair<String, Object>> at place 1;
 * its iterator hands out one pair object per entry. */
struct entry {
	HSTRING key;
	IInspectable *value;
};

struct pairs {
	struct object base;
	UINT32 count;
	struct entry entries[];
};

struct pair {
	struct object base;
	struct entry entry;
};

struct pair_iterator {
	struct object base;
	struct pairs *pairs;
	UINT32 at;
};

static HRESULT keep(struct entry *entry, HSTRING key, IInspectable *value)
{
	const HRESULT result = WindowsDuplicateString(key, &entry->key);
	if (SUCCEEDED(result))
		reference(entry->value = value);
	return result;
}

static void forget(struct entry *entry)
{
	WindowsDeleteString(entry->key);
	release(entry->value);
}

static void destroy_pairs(struct object *base)
{
	struct pairs *self = (struct pairs *)base;
	for (UINT32 index = 0; index < self->count; ++index)
		forget(&self->entries[index]);
}

static void destroy_pair(struct object *base) { forget(&((struct pair *)base)->entry); }
static void destroy_pair_iterator(struct object *base) { release(face_of(&((struct pair_iterator *)base)->pairs->base, 0)); }

/* The place of a key among entries, or count when no entry has it. */
static UINT32 find(const struct entry *entries, UINT32 count, HSTRING key)
{
	for (UINT32 at = 0; at < count; ++at) {
		INT32 order = 1;
		if (SUCCEEDED(WindowsCompareStringOrdinal(entries[at].key, key, &order)) && !order)
			return at;
	}
	return count;
}

static HRESULT lookup(const struct entry *entries, UINT32 count, HSTRING key, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	const UINT32 at = find(entries, count, key);
	*out = NULL;
	if (at == count)
		return E_BOUNDS;
	reference(*out = entries[at].value);
	return S_OK;
}

static HRESULT WINAPI pair_key(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct pair *)owner(iface))->entry.key, out);
}

static HRESULT WINAPI pair_value(void *iface, IInspectable **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = ((struct pair *)owner(iface))->entry.value);
	return S_OK;
}

TABLE(pair_table, 0, INSPECTABLE, (const void *)pair_key, (const void *)pair_value);
static const struct face_iid pair_iids[] = {{&iid_property_pair, 0}, {NULL, 0}};
static const struct kind pair_kind = {"IKeyValuePair<String, Object>", L"Windows.Foundation.Collections.IKeyValuePair`2<String, Object>",
	pair_iids, destroy_pair};

static HRESULT pair_out(const struct entry *entry, void **out)
{
	static const void *const *const tables[] = {pair_table.slots};
	struct pair *self = make(sizeof *self, &pair_kind, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	return keep(&self->entry, entry->key, entry->value);
}

static HRESULT WINAPI pair_current(void *iface, void **out)
{
	struct pair_iterator *self = (struct pair_iterator *)owner(iface);
	if (!out)
		return E_POINTER;
	*out = NULL;
	return self->at < self->pairs->count ? pair_out(&self->pairs->entries[self->at], out) : E_BOUNDS;
}

static HRESULT WINAPI pair_has_current(void *iface, boolean *out)
{
	struct pair_iterator *self = (struct pair_iterator *)owner(iface);
	return answer_flag(out, self->at < self->pairs->count);
}

static HRESULT WINAPI pair_next(void *iface, boolean *out)
{
	struct pair_iterator *self = (struct pair_iterator *)owner(iface);
	if (self->at < self->pairs->count)
		++self->at;
	return answer_flag(out, self->at < self->pairs->count);
}

static HRESULT WINAPI pair_many(void *iface, UINT32 capacity, void **items, UINT32 *actual)
{
	struct pair_iterator *self = (struct pair_iterator *)owner(iface);
	if (!actual || (capacity && !items))
		return E_POINTER;
	for (*actual = 0; *actual < capacity && self->at < self->pairs->count; ++*actual, ++self->at) {
		const HRESULT result = pair_out(&self->pairs->entries[self->at], &items[*actual]);
		if (FAILED(result))
			return result;
	}
	return S_OK;
}

TABLE(pair_iterator_table, 0, INSPECTABLE, (const void *)pair_current, (const void *)pair_has_current, (const void *)pair_next,
	(const void *)pair_many);
static const struct face_iid pair_iterator_iids[] = {{&iid_property_iterator, 0}, {NULL, 0}};
static const struct kind pair_iterator_kind = {"IIterator<IKeyValuePair<String, Object>>",
	L"Windows.Foundation.Collections.IIterator`1<Windows.Foundation.Collections.IKeyValuePair`2<String, Object>>", pair_iterator_iids,
	destroy_pair_iterator};

static HRESULT WINAPI pairs_first(void *iface, void **out)
{
	static const void *const *const tables[] = {pair_iterator_table.slots};
	struct pairs *pairs = (struct pairs *)owner(iface);
	if (!out)
		return E_POINTER;
	struct pair_iterator *self = make(sizeof *self, &pair_iterator_kind, tables, 1);
	if (!(*out = self ? face_of(&self->base, 0) : NULL))
		return E_OUTOFMEMORY;
	self->pairs = pairs;
	reference(face_of(&pairs->base, 0));
	return S_OK;
}

static HRESULT WINAPI pairs_lookup(void *iface, HSTRING key, IInspectable **out)
{
	struct pairs *self = (struct pairs *)owner(iface);
	return lookup(self->entries, self->count, key, out);
}

static HRESULT WINAPI pairs_size(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct pairs *)owner(iface))->count;
	return S_OK;
}

static HRESULT WINAPI pairs_has_key(void *iface, HSTRING key, boolean *out)
{
	struct pairs *self = (struct pairs *)owner(iface);
	return answer_flag(out, find(self->entries, self->count, key) < self->count);
}

/* A view may decline to split, and a snapshot does. */
static HRESULT WINAPI pairs_split(void *iface, void **first, void **second)
{
	if (!first || !second)
		return E_POINTER;
	*first = *second = NULL;
	return S_OK;
}

TABLE(pairs_table, 0, INSPECTABLE, (const void *)pairs_lookup, (const void *)pairs_size, (const void *)pairs_has_key, (const void *)pairs_split);
TABLE(pairs_iterable_table, 1, INSPECTABLE, (const void *)pairs_first);
static const struct face_iid pairs_iids[] = {{&iid_property_view, 0}, {&iid_property_iterable, 1}, {NULL, 0}};
static const struct kind pairs_kind = {"IMapView<String, Object>", L"Windows.Foundation.Collections.IMapView`2<String, Object>", pairs_iids,
	destroy_pairs};

/* A snapshot holding its own copies of the keys and references to the values. */
static struct pairs *snapshot(const struct entry *entries, UINT32 count)
{
	static const void *const *const tables[] = {pairs_table.slots, pairs_iterable_table.slots};
	struct pairs *self = make(offsetof(struct pairs, entries) + count * sizeof(struct entry), &pairs_kind, tables, 2);
	if (!self)
		return NULL;
	for (; self->count < count; ++self->count)
		if (FAILED(keep(&self->entries[self->count], entries[self->count].key, entries[self->count].value))) {
			release(face_of(&self->base, 0));
			return NULL;
		}
	return self;
}

static HRESULT view_out(const struct entry *entries, UINT32 count, void **out)
{
	if (!out)
		return E_POINTER;
	struct pairs *self = snapshot(entries, count);
	*out = self ? face_of(&self->base, 0) : NULL;
	return self ? S_OK : E_OUTOFMEMORY;
}

/* The process's property set, which CoreApplication.Properties answers: IObservableMap<String, Object> and IPropertySet at place 0,
 * IMap<String, Object> at place 1, IIterable<IKeyValuePair<String, Object>> at place 2. It keeps what a program stores while the process
 * lives and calls the program's handlers after each change, outside its lock. An iterator walks a snapshot taken when it is made. */
#define MOST_HANDLERS 16

enum map_change { change_reset, change_inserted, change_removed, change_changed };

struct change {
	struct object base;
	INT32 kind;
	HSTRING key;
};

struct handler {
	IUnknown *handler;
	INT64 token;
};

static SRWLOCK property_lock = SRWLOCK_INIT;
static struct entry *property_entries;
static UINT32 property_count, property_capacity;
static struct handler property_handlers[MOST_HANDLERS];
static struct object property_set;

static void destroy_change(struct object *base) { WindowsDeleteString(((struct change *)base)->key); }

static HRESULT WINAPI change_action(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct change *)owner(iface))->kind;
	return S_OK;
}

static HRESULT WINAPI change_key(void *iface, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsDuplicateString(((struct change *)owner(iface))->key, out);
}

TABLE(change_table, 0, INSPECTABLE, (const void *)change_action, (const void *)change_key);
static const struct face_iid change_iids[] = {{&iid_map_change, 0}, {NULL, 0}};
static const struct kind change_args_kind = {"IMapChangedEventArgs<String>", L"Windows.Foundation.Collections.IMapChangedEventArgs`1<String>",
	change_iids, destroy_change};

static void changed(INT32 kind, HSTRING key)
{
	static const void *const *const tables[] = {change_table.slots};
	IUnknown *handlers[MOST_HANDLERS];
	size_t count = 0;
	AcquireSRWLockShared(&property_lock);
	for (size_t index = 0; index < MOST_HANDLERS; ++index)
		if (property_handlers[index].handler)
			reference(handlers[count++] = property_handlers[index].handler);
	ReleaseSRWLockShared(&property_lock);
	if (!count)
		return;
	struct change *args = make(sizeof *args, &change_args_kind, tables, 1);
	if (args) {
		args->kind = kind;
		WindowsDuplicateString(key, &args->key);
	}
	for (size_t index = 0; index < count; ++index) {
		/* MapChangedEventHandler<String, Object>.Invoke(sender, args) follows IUnknown. */
		typedef HRESULT (WINAPI *invoke_method)(IUnknown *, void *, void *);
		if (args)
			((invoke_method)((void **)handlers[index]->lpVtbl)[3])(handlers[index], face_of(&property_set, 0), face_of(&args->base, 0));
		release(handlers[index]);
	}
	if (args)
		release(face_of(&args->base, 0));
}

static HRESULT WINAPI properties_changed_add(void *iface, IUnknown *handler, INT64 *out)
{
	if (!handler || !out)
		return E_INVALIDARG;
	HRESULT result = E_OUTOFMEMORY;
	AcquireSRWLockExclusive(&property_lock);
	for (size_t index = 0; index < MOST_HANDLERS; ++index)
		if (!property_handlers[index].handler) {
			reference(property_handlers[index].handler = handler);
			*out = property_handlers[index].token = InterlockedIncrement(&tokens);
			result = S_OK;
			break;
		}
	ReleaseSRWLockExclusive(&property_lock);
	return result;
}

static HRESULT WINAPI properties_changed_remove(void *iface, INT64 token)
{
	IUnknown *handler = NULL;
	AcquireSRWLockExclusive(&property_lock);
	for (size_t index = 0; index < MOST_HANDLERS; ++index)
		if (property_handlers[index].handler && property_handlers[index].token == token) {
			handler = property_handlers[index].handler;
			property_handlers[index].handler = NULL;
			break;
		}
	ReleaseSRWLockExclusive(&property_lock);
	release(handler);
	return S_OK;
}

static HRESULT WINAPI properties_lookup(void *iface, HSTRING key, IInspectable **out)
{
	AcquireSRWLockShared(&property_lock);
	const HRESULT result = lookup(property_entries, property_count, key, out);
	ReleaseSRWLockShared(&property_lock);
	return result;
}

static HRESULT WINAPI properties_size(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&property_lock);
	*out = property_count;
	ReleaseSRWLockShared(&property_lock);
	return S_OK;
}

static HRESULT WINAPI properties_has_key(void *iface, HSTRING key, boolean *out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&property_lock);
	*out = find(property_entries, property_count, key) < property_count;
	ReleaseSRWLockShared(&property_lock);
	return S_OK;
}

static HRESULT WINAPI properties_view(void *iface, void **out)
{
	AcquireSRWLockShared(&property_lock);
	const HRESULT result = view_out(property_entries, property_count, out);
	ReleaseSRWLockShared(&property_lock);
	return result;
}

static HRESULT WINAPI properties_insert(void *iface, HSTRING key, IInspectable *value, boolean *replaced)
{
	if (!replaced)
		return E_POINTER;
	HRESULT result = S_OK;
	IInspectable *old = NULL;
	AcquireSRWLockExclusive(&property_lock);
	const UINT32 at = find(property_entries, property_count, key);
	*replaced = at < property_count;
	if (*replaced) {
		old = property_entries[at].value;
		reference(property_entries[at].value = value);
	} else {
		if (property_count == property_capacity) {
			const UINT32 capacity = property_capacity ? property_capacity * 2 : 16;
			struct entry *grown = property_entries ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, property_entries, capacity * sizeof *grown)
				: ALLOC(capacity * sizeof *grown);
			if (grown) {
				property_entries = grown;
				property_capacity = capacity;
			}
		}
		result = property_count < property_capacity ? keep(&property_entries[property_count], key, value) : E_OUTOFMEMORY;
		if (SUCCEEDED(result))
			++property_count;
	}
	ReleaseSRWLockExclusive(&property_lock);
	release(old);
	if (SUCCEEDED(result))
		changed(*replaced ? change_changed : change_inserted, key);
	return result;
}

static HRESULT WINAPI properties_remove(void *iface, HSTRING key)
{
	struct entry removed = {NULL, NULL};
	AcquireSRWLockExclusive(&property_lock);
	const UINT32 at = find(property_entries, property_count, key);
	const BOOL found = at < property_count;
	if (found) {
		removed = property_entries[at];
		property_entries[at] = property_entries[--property_count];
	}
	ReleaseSRWLockExclusive(&property_lock);
	if (!found)
		return E_BOUNDS;
	forget(&removed);
	changed(change_removed, key);
	return S_OK;
}

static HRESULT WINAPI properties_clear(void *iface)
{
	AcquireSRWLockExclusive(&property_lock);
	struct entry *entries = property_entries;
	const UINT32 count = property_count;
	property_entries = NULL;
	property_count = property_capacity = 0;
	ReleaseSRWLockExclusive(&property_lock);
	for (UINT32 index = 0; index < count; ++index)
		forget(&entries[index]);
	if (entries)
		FREE(entries);
	changed(change_reset, NULL);
	return S_OK;
}

static HRESULT WINAPI properties_first(void *iface, void **out)
{
	AcquireSRWLockShared(&property_lock);
	struct pairs *pairs = snapshot(property_entries, property_count);
	ReleaseSRWLockShared(&property_lock);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!pairs)
		return E_OUTOFMEMORY;
	const HRESULT result = pairs_first(face_of(&pairs->base, 1), out);
	release(face_of(&pairs->base, 0));
	return result;
}

TABLE(properties_observable_table, 0, INSPECTABLE, (const void *)properties_changed_add, (const void *)properties_changed_remove);
TABLE(properties_map_table, 1, INSPECTABLE, (const void *)properties_lookup, (const void *)properties_size, (const void *)properties_has_key,
	(const void *)properties_view, (const void *)properties_insert, (const void *)properties_remove, (const void *)properties_clear);
TABLE(properties_iterable_table, 2, INSPECTABLE, (const void *)properties_first);
static const struct face_iid property_set_iids[] = {{&iid_observable_property_map, 0}, {&iid_property_set, 0}, {&iid_property_map, 1},
	{&iid_property_iterable, 2}, {NULL, 0}};
static const struct kind property_set_kind = {"PropertySet", L"Windows.Foundation.Collections.PropertySet", property_set_iids, NULL};
static struct object property_set = {{properties_observable_table.slots, properties_map_table.slots, properties_iterable_table.slots},
	&property_set_kind, 1};

/* Windows.System.Profile.RetailInfo and SharedModeSettings. */
static HRESULT WINAPI retail_demo(void *iface, boolean *out) { return answer_flag(out, FALSE); }

static HRESULT WINAPI retail_properties(void *iface, void **out)
{
	return view_out(NULL, 0, out);
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
	result = collection(&string_view, &string_iterator, vector_table.slots, TRUE, (void *const *)&name, 1, out);
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
	void *profile = face_of(&connection_profile, 0);
	return collection(&profile_view, &profile_iterator, vector_table.slots, FALSE, &profile, connected() ? 1 : 0, out);
}

static HRESULT WINAPI network_internet_profile(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = connected() ? face_of(&connection_profile, 0) : NULL;
	return S_OK;
}

static HRESULT WINAPI network_lan_identifiers(void *iface, void **out) { return empty_collection(&lan_view, &lan_iterator, out); }
static HRESULT WINAPI network_host_names(void *iface, void **out) { return empty_collection(&host_view, &host_iterator, out); }

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
	return completed(&ticket_operation, (IUnknown *)face_of(&no_ticket, 0), out);
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

/* Windows.Security.Authentication.Web.Core: there is no web account provider. A search for one finds none, and a request names a
 * provider, which a program cannot have from this system. */
static HRESULT no_provider(void **out) { return completed(&provider_operation, NULL, out); }

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

/* Windows.ApplicationModel.Core.CoreApplication in a desktop process: no package identity and no views. */
static HRESULT WINAPI core_id(void *iface, HSTRING *out)
{
	if (out)
		*out = NULL;
	return NO_PACKAGE;
}

static HRESULT WINAPI core_properties(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = face_of(&property_set, 0);
	return S_OK;
}

static HRESULT WINAPI core_no_view(void *iface, void **out)
{
	answer_null(out);
	return NOT_FOUND;
}

static HRESULT WINAPI core_run(void *iface, void *source) { return missing("CoreApplication.Run"); }
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

static HRESULT WINAPI core_exit(void *iface) { return missing("CoreApplication.Exit"); }
static HRESULT WINAPI core_views(void *iface, void **out) { answer_null(out); return missing("CoreApplication.Views"); }

static HRESULT WINAPI core_new_view(void *iface, void *first, void *second, void **out)
{
	answer_null(out);
	return missing("CoreApplication.CreateNewView");
}

TABLE(core_factory_table, 0, FACTORY);
TABLE(core_table, 1, INSPECTABLE, (const void *)core_id, (const void *)event_add, (const void *)event_remove, (const void *)event_add,
	(const void *)event_remove, (const void *)core_properties, (const void *)core_no_view, (const void *)core_run, (const void *)core_run);
TABLE(core2_table, 2, INSPECTABLE, (const void *)event_add, (const void *)event_remove, (const void *)event_add, (const void *)event_remove,
	(const void *)event_add, (const void *)event_remove, (const void *)core_prelaunch);
TABLE(core3_table, 3, INSPECTABLE, (const void *)core_restart, (const void *)core_restart_user);
TABLE(core_exit_table, 4, INSPECTABLE, (const void *)core_exit, (const void *)event_add, (const void *)event_remove);
TABLE(core_immersive_table, 5, INSPECTABLE, (const void *)core_views, (const void *)core_new_view, (const void *)core_no_view);
TABLE(core_unhandled_table, 6, INSPECTABLE, (const void *)event_add, (const void *)event_remove);
static const struct face_iid core_iids[] = {{&iid_factory, 0}, {&iid_core_application, 1}, {&iid_core_application2, 2},
	{&iid_core_application3, 3}, {&iid_core_application_exit, 4}, {&iid_core_immersive, 5}, {&iid_core_unhandled_error, 6}, {NULL, 0}};
static const struct kind core_kind = {"CoreApplication", L"Windows.ApplicationModel.Core.CoreApplication", core_iids, NULL};
static struct object core_application = {{core_factory_table.slots, core_table.slots, core2_table.slots, core3_table.slots,
	core_exit_table.slots, core_immersive_table.slots, core_unhandled_table.slots}, &core_kind, 1};

static struct object *const classes[] = {&retail_info, &shared_mode, &network_information, &online_id, &web_core, &core_application};

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
