/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.UI.Notifications: ToastNotificationManager, ToastNotification and ToastNotifier. A toast is shown as a notification of the
 * Linux desktop through the runtime: the first text of its content is the title, the others the text, as the toast's own layout shows
 * them first in bold and then below.
 *
 * - The templates are those of the Windows 8 catalog, as GetTemplateContent hands them out, to be filled in by the app.
 * - Every notifier is the program's: its notifications carry the program's name, whatever application id the app gives.
 * - A toast is shown once, as the desktop shows it; Hide, its expiration, its tag and its group change nothing there, and its events
 *   never happen, as the desktop tells nothing back. Notifications are always enabled.
 * - Scheduled toasts are named in Raven's trace. */
#include "object.h"
#include "../raven-winrt/winrt.h"

__declspec(dllimport) BOOL WINAPI RavenNotify(const WCHAR *application, const WCHAR *title, const WCHAR *text);

static const IID iid_manager_statics = {0x50ac103f, 0xd235, 0x4598, {0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4}};
static const IID iid_toast = {0x997e2675, 0x059e, 0x4e60, {0x8b, 0x06, 0x17, 0x60, 0x91, 0x7c, 0x8b, 0x80}};
static const IID iid_toast2 = {0x9dfb9fd1, 0x143a, 0x490e, {0x90, 0xbf, 0xb9, 0xfb, 0xa7, 0x13, 0x2d, 0xe7}};
static const IID iid_toast_factory = {0x04124b20, 0x82c6, 0x4229, {0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53}};
static const IID iid_notifier = {0x75927b93, 0x03f3, 0x41ec, {0x91, 0xd3, 0x6e, 0x5b, 0xac, 0x1b, 0x38, 0xe7}};
static const IID iid_xml_document = {0xf7f3a506, 0x1e87, 0x42d6, {0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94}};

/* The texts a toast shows at most: the title and the lines below it. */
#define TOAST_TEXTS 8

/* A ToastNotification: its content, what the app set on it, and the handlers of its events. */
struct toast {
	struct object base;
	SRWLOCK lock;
	IUnknown *content, *expiration;
	HSTRING tag, group;
	boolean suppress;
	struct handlers dismissed, activated, failed;
};

static struct toast *toast_of(void *iface) { return (struct toast *)owner(iface); }

static void destroy_toast(struct object *base)
{
	struct toast *self = (struct toast *)base;
	release(self->content);
	release(self->expiration);
	WindowsDeleteString(self->tag);
	WindowsDeleteString(self->group);
	handlers_clear(&self->dismissed);
	handlers_clear(&self->activated);
	handlers_clear(&self->failed);
}

static HRESULT get_reference(struct toast *self, IUnknown **field, void **out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	reference(*out = *field);
	ReleaseSRWLockShared(&self->lock);
	return S_OK;
}

static HRESULT get_text(struct toast *self, HSTRING *field, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	AcquireSRWLockShared(&self->lock);
	const HRESULT result = WindowsDuplicateString(*field, out);
	ReleaseSRWLockShared(&self->lock);
	return result;
}

static HRESULT put_text(struct toast *self, HSTRING *field, HSTRING value)
{
	HSTRING copy = NULL;
	const HRESULT result = WindowsDuplicateString(value, &copy);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&self->lock);
	HSTRING old = *field;
	*field = copy;
	ReleaseSRWLockExclusive(&self->lock);
	WindowsDeleteString(old);
	return S_OK;
}

static HRESULT add_handler(struct toast *self, struct handlers *list, void *handler, INT64 *token)
{
	if (!handler || !token)
		return E_INVALIDARG;
	AcquireSRWLockExclusive(&self->lock);
	const HRESULT result = handlers_add(list, handler, token);
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

static HRESULT remove_handler(struct toast *self, struct handlers *list, INT64 token)
{
	AcquireSRWLockExclusive(&self->lock);
	const HRESULT result = handlers_remove(list, token);
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

static HRESULT WINAPI toast_content(void *iface, void **out) { return get_reference(toast_of(iface), &toast_of(iface)->content, out); }

static HRESULT WINAPI toast_set_expiration(void *iface, IUnknown *value)
{
	struct toast *self = toast_of(iface);
	reference(value);
	AcquireSRWLockExclusive(&self->lock);
	IUnknown *old = self->expiration;
	self->expiration = value;
	ReleaseSRWLockExclusive(&self->lock);
	release(old);
	return S_OK;
}

static HRESULT WINAPI toast_expiration(void *iface, void **out) { return get_reference(toast_of(iface), &toast_of(iface)->expiration, out); }
static HRESULT WINAPI toast_add_dismissed(void *iface, void *handler, INT64 *token) { return add_handler(toast_of(iface), &toast_of(iface)->dismissed, handler, token); }
static HRESULT WINAPI toast_remove_dismissed(void *iface, INT64 token) { return remove_handler(toast_of(iface), &toast_of(iface)->dismissed, token); }
static HRESULT WINAPI toast_add_activated(void *iface, void *handler, INT64 *token) { return add_handler(toast_of(iface), &toast_of(iface)->activated, handler, token); }
static HRESULT WINAPI toast_remove_activated(void *iface, INT64 token) { return remove_handler(toast_of(iface), &toast_of(iface)->activated, token); }
static HRESULT WINAPI toast_add_failed(void *iface, void *handler, INT64 *token) { return add_handler(toast_of(iface), &toast_of(iface)->failed, handler, token); }
static HRESULT WINAPI toast_remove_failed(void *iface, INT64 token) { return remove_handler(toast_of(iface), &toast_of(iface)->failed, token); }
static HRESULT WINAPI toast_set_tag(void *iface, HSTRING value) { return put_text(toast_of(iface), &toast_of(iface)->tag, value); }
static HRESULT WINAPI toast_tag(void *iface, HSTRING *out) { return get_text(toast_of(iface), &toast_of(iface)->tag, out); }
static HRESULT WINAPI toast_set_group(void *iface, HSTRING value) { return put_text(toast_of(iface), &toast_of(iface)->group, value); }
static HRESULT WINAPI toast_group(void *iface, HSTRING *out) { return get_text(toast_of(iface), &toast_of(iface)->group, out); }
static HRESULT WINAPI toast_set_suppress(void *iface, boolean value) { toast_of(iface)->suppress = value; return S_OK; }

static HRESULT WINAPI toast_suppress(void *iface, boolean *out)
{
	if (!out)
		return E_POINTER;
	*out = *(volatile boolean *)&toast_of(iface)->suppress;
	return S_OK;
}

TABLE(toast_table, 0, INSPECTABLE, (const void *)toast_content, (const void *)toast_set_expiration, (const void *)toast_expiration,
	(const void *)toast_add_dismissed, (const void *)toast_remove_dismissed, (const void *)toast_add_activated,
	(const void *)toast_remove_activated, (const void *)toast_add_failed, (const void *)toast_remove_failed);
TABLE(toast2_table, 1, INSPECTABLE, (const void *)toast_set_tag, (const void *)toast_tag, (const void *)toast_set_group,
	(const void *)toast_group, (const void *)toast_set_suppress, (const void *)toast_suppress);
static const struct face_iid toast_iids[] = {{&iid_toast, 0}, {&iid_toast2, 1}, {NULL, 0}};
static const struct kind toast_kind = {"ToastNotification", L"Windows.UI.Notifications.ToastNotification", toast_iids, destroy_toast};

static HRESULT WINAPI toast_create(void *iface, IUnknown *content, void **out)
{
	static const void *const *const tables[] = {toast_table.slots, toast2_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	IUnknown *document = NULL;
	if (!content || FAILED(content->lpVtbl->QueryInterface(content, &iid_xml_document, (void **)&document)))
		return E_INVALIDARG;
	struct toast *self = make(sizeof *self, &toast_kind, tables, 2);
	if (!self) {
		release(document);
		return E_OUTOFMEMORY;
	}
	InitializeSRWLock(&self->lock);
	self->content = document;
	*out = face_of(&self->base, 0);
	return S_OK;
}

TABLE(toast_factory_table, 0, FACTORY);
TABLE(toast_create_table, 1, INSPECTABLE, (const void *)toast_create);
static const struct face_iid toast_class_iids[] = {{&iid_factory, 0}, {&iid_toast_factory, 1}, {NULL, 0}};
static const struct kind toast_class_kind = {"ToastNotification", L"Windows.UI.Notifications.ToastNotification", toast_class_iids, NULL};
struct object toast_notification_class = {{toast_factory_table.slots, toast_create_table.slots}, &toast_class_kind, 1};

/* Shows a toast of ours: its texts, the first as the title and the others below it, one to a line. */
static HRESULT WINAPI notifier_show(void *iface, void *toast)
{
	BSTR texts[TOAST_TEXTS];
	UINT32 count = 0;
	if (!toast || method(toast, 0) != (void *)object_query || owner(toast)->kind != &toast_kind)
		return E_INVALIDARG;
	struct toast *self = toast_of(toast);
	if (self->suppress)
		return S_OK;
	IUnknown *content = NULL;
	get_reference(self, &self->content, (void **)&content);
	HRESULT result = xml_selected_texts(content, L"/toast/visual/binding/text", texts, TOAST_TEXTS, &count);
	release(content);
	if (FAILED(result))
		return result;
	size_t length = 1;
	for (UINT32 index = 1; index < count; ++index)
		length += SysStringLen(texts[index]) + 1;
	WCHAR *body = ALLOC(length * sizeof(WCHAR));
	if (body) {
		WCHAR *at = body;
		for (UINT32 index = 1; index < count; ++index) {
			if (index > 1)
				*at++ = L'\n';
			CopyMemory(at, texts[index], SysStringLen(texts[index]) * sizeof(WCHAR));
			at += SysStringLen(texts[index]);
		}
		*at = 0;
		if (!RavenNotify(NULL, count ? texts[0] : L"", body))
			trace("ToastNotifier.Show", "the desktop could not be asked to show the notification");
		FREE(body);
	}
	for (UINT32 index = 0; index < count; ++index)
		SysFreeString(texts[index]);
	return body ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI notifier_hide(void *iface, void *toast) { return toast ? S_OK : E_INVALIDARG; }

/* NotificationSetting.Enabled. */
static HRESULT WINAPI notifier_setting(void *iface, INT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return S_OK;
}

static HRESULT WINAPI notifier_schedule(void *iface, void *toast) { return missing("ToastNotifier.AddToSchedule"); }
static HRESULT WINAPI notifier_unschedule(void *iface, void *toast) { return missing("ToastNotifier.RemoveFromSchedule"); }
static HRESULT WINAPI notifier_scheduled(void *iface, void **out) { return missing("ToastNotifier.GetScheduledToastNotifications"); }

TABLE(notifier_table, 0, INSPECTABLE, (const void *)notifier_show, (const void *)notifier_hide, (const void *)notifier_setting,
	(const void *)notifier_schedule, (const void *)notifier_unschedule, (const void *)notifier_scheduled);
static const struct face_iid notifier_iids[] = {{&iid_notifier, 0}, {NULL, 0}};
static const struct kind notifier_kind = {"ToastNotifier", L"Windows.UI.Notifications.ToastNotifier", notifier_iids, NULL};
static struct object notifier = {{notifier_table.slots}, &notifier_kind, 1};

static HRESULT WINAPI manager_notifier(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = face_of(&notifier, 0));
	return S_OK;
}

static HRESULT WINAPI manager_notifier_with_id(void *iface, HSTRING id, void **out) { return manager_notifier(iface, out); }

/* ToastTemplateType: the four with an image, then the four of text alone, each with the texts of its layout. */
static HRESULT WINAPI manager_template(void *iface, INT32 type, void **out)
{
	static const WCHAR *const names[] = {L"ToastImageAndText01", L"ToastImageAndText02", L"ToastImageAndText03", L"ToastImageAndText04",
		L"ToastText01", L"ToastText02", L"ToastText03", L"ToastText04"};
	static const int texts[] = {1, 2, 2, 3, 1, 2, 2, 3};
	WCHAR xml[256];
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (type < 0 || type >= (INT32)ARRAYSIZE(names))
		return E_INVALIDARG;
	int length = wsprintfW(xml, L"<toast><visual><binding template=\"%s\">%s", names[type], type < 4 ? L"<image id=\"1\" src=\"\"/>" : L"");
	for (int index = 1; index <= texts[type]; ++index)
		length += wsprintfW(xml + length, L"<text id=\"%d\"></text>", index);
	lstrcpyW(xml + length, L"</binding></visual></toast>");
	return xml_document_from_text(xml, out);
}

TABLE(manager_factory_table, 0, FACTORY);
TABLE(manager_statics_table, 1, INSPECTABLE, (const void *)manager_notifier, (const void *)manager_notifier_with_id,
	(const void *)manager_template);
static const struct face_iid manager_iids[] = {{&iid_factory, 0}, {&iid_manager_statics, 1}, {NULL, 0}};
static const struct kind manager_kind = {"ToastNotificationManager", L"Windows.UI.Notifications.ToastNotificationManager", manager_iids, NULL};
struct object toast_manager_class = {{manager_factory_table.slots, manager_statics_table.slots}, &manager_kind, 1};
