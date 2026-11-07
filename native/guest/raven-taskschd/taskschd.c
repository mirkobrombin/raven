/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven Task Scheduler component: the Schedule.Service class of taskschd.dll, through which programs register tasks, read them back and
 * run them on demand. Installers use it to start a program as the signed-in user once they finish.
 *
 * A registered task is kept the way Windows keeps it, as task XML in a file under System32\Tasks named after its path, and a folder is a
 * directory there. Nothing watches the clock: a task runs when a program asks for it or, with a registration trigger, when it is
 * registered. Running a task starts each of its programs with the caller's account. Every interface starts from a table of stubs that
 * name the method they stand for (taskschd_tables.h). */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <oleauto.h>
#include <taskschd.h>
#include <stddef.h>
#include <string.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define GROW(block, size) ((block) ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (block), (size)) : ALLOC(size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

__declspec(dllimport) void WINAPI RavenTrace(const char *text);

static HRESULT unimplemented(const char *interface_name, const char *method)
{
	char text[160];
	size_t at = 0;
	const char *part[] = {"taskschd: ", interface_name, "::", method, " is not implemented"};
	size_t index, length;
	for (index = 0; index < 5; ++index)
		for (length = 0; part[index][length] && at < sizeof(text) - 1; ++length)
			text[at++] = part[index][length];
	text[at] = 0;
	RavenTrace(text);
	return E_NOTIMPL;
}

#include "taskschd_tables.h"

/* A missing task is a missing file, a missing folder a missing path. */
#define NOT_FOUND HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
#define NO_FOLDER HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
#define ALREADY_EXISTS HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)
#define NOT_CONNECTED HRESULT_FROM_WIN32(ERROR_ONLY_IF_CONNECTED)
#define TASK_HAS_NOT_RUN ((HRESULT)0x00041303)
/* The scheduler errors winerror.h defines, which the MinGW headers leave out. */
#ifndef SCHED_E_MALFORMEDXML
#define SCHED_E_MISSINGNODE ((HRESULT)0x80041319)
#define SCHED_E_MALFORMEDXML ((HRESULT)0x8004131A)
#define SCHED_E_TASK_DISABLED ((HRESULT)0x80041332)
#endif

static BOOL same_guid(const GUID *a, const GUID *b)
{
	return memcmp(a, b, sizeof(GUID)) == 0;
}

/* ---- objects ---- */
struct object {
	void **vtbl;
	LONG refs;
	void (*destroy)(struct object *self);
	const GUID *iid, *base_iid; /* the interface the object stands for and the one it extends, IDispatch aside */
};

static HRESULT STDMETHODCALLTYPE object_QueryInterface(void *iface, REFIID riid, void **out)
{
	struct object *self = iface;
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IDispatch) || (self->iid && same_guid(riid, self->iid)) ||
		(self->base_iid && same_guid(riid, self->base_iid))) {
		InterlockedIncrement(&self->refs);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE object_AddRef(void *iface)
{
	return (ULONG)InterlockedIncrement(&((struct object *)iface)->refs);
}

static ULONG STDMETHODCALLTYPE object_Release(void *iface)
{
	struct object *self = iface;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		if (self->destroy)
			self->destroy(self);
		FREE(self);
	}
	return (ULONG)refs;
}

/* Every interface here is a dual one; scripts reach it through type information this component does not carry. */
static HRESULT STDMETHODCALLTYPE dispatch_GetTypeInfoCount(void *iface, UINT *count)
{
	if (!count)
		return E_POINTER;
	*count = 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE dispatch_GetTypeInfo(void *iface, UINT index, LCID locale, ITypeInfo **out)
{
	if (out)
		*out = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dispatch_GetIDsOfNames(void *iface, REFIID riid, LPOLESTR *names, UINT count, LCID locale, DISPID *ids)
{
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dispatch_Invoke(void *iface, DISPID member, REFIID riid, LCID locale, WORD flags, DISPPARAMS *parameters,
	VARIANT *result, EXCEPINFO *exception, UINT *argument_error)
{
	return E_NOTIMPL;
}

static void set_dispatch(void **table)
{
	table[0] = (void *)object_QueryInterface;
	table[1] = (void *)object_AddRef;
	table[2] = (void *)object_Release;
	table[3] = (void *)dispatch_GetTypeInfoCount;
	table[4] = (void *)dispatch_GetTypeInfo;
	table[5] = (void *)dispatch_GetIDsOfNames;
	table[6] = (void *)dispatch_Invoke;
}

static void *object_new(size_t size, void **table, void (*destroy)(struct object *), const GUID *iid)
{
	struct object *self = ALLOC(size);
	if (!self)
		return NULL;
	self->vtbl = table;
	self->refs = 1;
	self->destroy = destroy;
	self->iid = iid;
	return self;
}

static void release(struct object *object)
{
	if (object)
		object_Release(object);
}

static struct object *keep(struct object *object)
{
	if (object)
		object_AddRef(object);
	return object;
}

/* ---- strings ---- */
static HRESULT get_string(BSTR value, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = value ? SysAllocStringLen(value, SysStringLen(value)) : NULL;
	return value && !*out ? E_OUTOFMEMORY : S_OK;
}

static HRESULT put_string(BSTR *field, const WCHAR *value)
{
	BSTR copy = value ? SysAllocString(value) : NULL;
	if (value && !copy)
		return E_OUTOFMEMORY;
	SysFreeString(*field);
	*field = copy;
	return S_OK;
}

static BOOL empty(const WCHAR *text)
{
	return !text || !*text;
}

/* A string the program passed as a VARIANT, or none when it passed an empty or missing one. */
static const WCHAR *variant_text(const VARIANT *value)
{
	if (V_VT(value) == VT_BSTR && !empty(V_BSTR(value)))
		return V_BSTR(value);
	return NULL;
}

/* Growing wide text, the task XML is written into. */
struct text {
	WCHAR *data;
	size_t length, capacity;
};

static void add_count(struct text *text, const WCHAR *part, size_t count)
{
	if (text->length + count + 1 > text->capacity) {
		size_t capacity = text->capacity ? text->capacity : 1024;
		WCHAR *data;
		while (text->length + count + 1 > capacity)
			capacity *= 2;
		data = GROW(text->data, capacity * sizeof(WCHAR));
		if (!data)
			return;
		text->data = data;
		text->capacity = capacity;
	}
	memcpy(text->data + text->length, part, count * sizeof(WCHAR));
	text->length += count;
	text->data[text->length] = 0;
}

static void add(struct text *text, const WCHAR *part)
{
	add_count(text, part, lstrlenW(part));
}

static void add_escaped(struct text *text, const WCHAR *part)
{
	for (; *part; ++part)
		switch (*part) {
		case '&': add(text, L"&amp;"); break;
		case '<': add(text, L"&lt;"); break;
		case '>': add(text, L"&gt;"); break;
		case '"': add(text, L"&quot;"); break;
		default: add_count(text, part, 1);
		}
}

static void add_number(struct text *text, LONG value)
{
	WCHAR digits[16];
	int at = 15;
	DWORD magnitude = value < 0 ? (DWORD)-value : (DWORD)value;
	digits[at] = 0;
	do
		digits[--at] = (WCHAR)('0' + magnitude % 10);
	while (magnitude /= 10);
	if (value < 0)
		digits[--at] = '-';
	add(text, digits + at);
}

static void add_indent(struct text *text, int depth)
{
	while (depth-- > 0)
		add(text, L"  ");
}

static void add_element(struct text *text, int depth, const WCHAR *tag, const WCHAR *value)
{
	if (empty(value))
		return;
	add_indent(text, depth);
	add(text, L"<");
	add(text, tag);
	add(text, L">");
	add_escaped(text, value);
	add(text, L"</");
	add(text, tag);
	add(text, L">\r\n");
}

static void add_flag(struct text *text, int depth, const WCHAR *tag, VARIANT_BOOL value)
{
	add_element(text, depth, tag, value ? L"true" : L"false");
}

static void add_open(struct text *text, int depth, const WCHAR *tag)
{
	add_indent(text, depth);
	add(text, L"<");
	add(text, tag);
	add(text, L">\r\n");
}

static void add_close(struct text *text, int depth, const WCHAR *tag)
{
	add_indent(text, depth);
	add(text, L"</");
	add(text, tag);
	add(text, L">\r\n");
}

/* ---- reading task XML ---- */
/* The part of the XML between `from` and `end` that holds the first element named `tag`: its content, and where the element ends. A
 * self-closed element has empty content. */
static BOOL find_element(const WCHAR *from, const WCHAR *end, const WCHAR *tag, const WCHAR **inner, const WCHAR **inner_end, const WCHAR **after)
{
	const int length = lstrlenW(tag);
	const WCHAR *at;
	for (at = from; at + length + 1 < end; ++at) {
		const WCHAR *close;
		int depth = 0;
		if (at[0] != '<' || memcmp(at + 1, tag, length * sizeof(WCHAR)) || (at[length + 1] != '>' && at[length + 1] != ' ' &&
			at[length + 1] != '/' && at[length + 1] != '\r' && at[length + 1] != '\n' && at[length + 1] != '\t'))
			continue;
		for (close = at + length + 1; close < end && *close != '>'; ++close)
			;
		if (close >= end)
			return FALSE;
		if (close[-1] == '/') {
			*inner = *inner_end = close;
			*after = close + 1;
			return TRUE;
		}
		*inner = close + 1;
		/* The matching end tag, past any nested element of the same name. */
		for (close = *inner; close + length + 2 < end; ++close) {
			if (close[0] != '<')
				continue;
			if (close[1] == '/' && !memcmp(close + 2, tag, length * sizeof(WCHAR)) && close[length + 2] == '>') {
				if (depth--)
					continue;
				*inner_end = close;
				*after = close + length + 3;
				return TRUE;
			}
			if (!memcmp(close + 1, tag, length * sizeof(WCHAR)) && (close[length + 1] == '>' || close[length + 1] == ' '))
				++depth;
		}
		return FALSE;
	}
	return FALSE;
}

/* The text of the element `tag` inside the range, with its entities replaced, or none when the element is missing. */
static BSTR element_text(const WCHAR *from, const WCHAR *end, const WCHAR *tag)
{
	const WCHAR *inner, *inner_end, *after;
	WCHAR *value;
	BSTR result;
	size_t length = 0;
	if (!find_element(from, end, tag, &inner, &inner_end, &after))
		return NULL;
	value = ALLOC((inner_end - inner + 1) * sizeof(WCHAR));
	if (!value)
		return NULL;
	while (inner < inner_end) {
		static const struct { const WCHAR *name; WCHAR character; } entities[] = {
			{L"&amp;", '&'}, {L"&lt;", '<'}, {L"&gt;", '>'}, {L"&quot;", '"'}, {L"&apos;", '\''}};
		size_t index;
		BOOL replaced = FALSE;
		if (*inner == '&')
			for (index = 0; index < sizeof(entities) / sizeof(*entities); ++index) {
				const int size = lstrlenW(entities[index].name);
				if (inner + size <= inner_end && !memcmp(inner, entities[index].name, size * sizeof(WCHAR))) {
					value[length++] = entities[index].character;
					inner += size;
					replaced = TRUE;
					break;
				}
			}
		if (!replaced)
			value[length++] = *inner++;
	}
	result = SysAllocStringLen(value, (UINT)length);
	FREE(value);
	return result;
}

static void read_string(BSTR *field, const WCHAR *from, const WCHAR *end, const WCHAR *tag)
{
	BSTR value = element_text(from, end, tag);
	if (!value)
		return;
	SysFreeString(*field);
	*field = value;
}

static void read_flag(VARIANT_BOOL *field, const WCHAR *from, const WCHAR *end, const WCHAR *tag)
{
	BSTR value = element_text(from, end, tag);
	if (!value)
		return;
	*field = !lstrcmpiW(value, L"true") || !lstrcmpW(value, L"1") ? VARIANT_TRUE : VARIANT_FALSE;
	SysFreeString(value);
}

static void read_number(int *field, const WCHAR *from, const WCHAR *end, const WCHAR *tag)
{
	BSTR value = element_text(from, end, tag);
	const WCHAR *at;
	int number = 0;
	if (!value)
		return;
	for (at = value; *at >= '0' && *at <= '9'; ++at)
		number = number * 10 + (*at - '0');
	*field = number;
	SysFreeString(value);
}

/* The position of a name in a list of names, or -1. */
static int read_choice(const WCHAR *from, const WCHAR *end, const WCHAR *tag, const WCHAR *const *names, int count)
{
	BSTR value = element_text(from, end, tag);
	int index, found = -1;
	if (!value)
		return -1;
	for (index = 0; index < count; ++index)
		if (names[index] && !lstrcmpiW(value, names[index]))
			found = index;
	SysFreeString(value);
	return found;
}

static const WCHAR *const logon_names[] = {L"None", L"Password", L"S4U", L"InteractiveToken", L"Group", L"ServiceAccount",
	L"InteractiveTokenOrPassword"};
static const WCHAR *const run_level_names[] = {L"LeastPrivilege", L"HighestAvailable"};
static const WCHAR *const instance_names[] = {L"Parallel", L"Queue", L"IgnoreNew", L"StopExisting"};
/* The element of each trigger type, by TASK_TRIGGER_TYPE2. */
static const WCHAR *const trigger_names[] = {L"EventTrigger", L"TimeTrigger", L"CalendarTrigger", L"CalendarTrigger", L"CalendarTrigger",
	L"CalendarTrigger", L"IdleTrigger", L"RegistrationTrigger", L"BootTrigger", L"LogonTrigger", NULL, L"SessionStateChangeTrigger"};

/* ---- properties ---- */
#define STRING_PROPERTY(type, field) \
	static HRESULT STDMETHODCALLTYPE type##_get_##field(void *iface, BSTR *out) { return get_string(((struct type *)iface)->field, out); } \
	static HRESULT STDMETHODCALLTYPE type##_put_##field(void *iface, BSTR value) { return put_string(&((struct type *)iface)->field, value); }
#define VALUE_PROPERTY(type, kind, field) \
	static HRESULT STDMETHODCALLTYPE type##_get_##field(void *iface, kind *out) \
	{ \
		if (!out) \
			return E_POINTER; \
		*out = ((struct type *)iface)->field; \
		return S_OK; \
	} \
	static HRESULT STDMETHODCALLTYPE type##_put_##field(void *iface, kind value) { ((struct type *)iface)->field = value; return S_OK; }
#define OBJECT_PROPERTY(type, field) \
	static HRESULT STDMETHODCALLTYPE type##_get_##field(void *iface, void **out) \
	{ \
		if (!out) \
			return E_POINTER; \
		*out = keep(((struct type *)iface)->field); \
		return S_OK; \
	} \
	static HRESULT STDMETHODCALLTYPE type##_put_##field(void *iface, struct object *value) \
	{ \
		struct type *self = iface; \
		if (!value) \
			return E_POINTER; \
		keep(value); \
		release(self->field); \
		self->field = value; \
		return S_OK; \
	}
#define BIND(interface_name, type, field) \
	(interface_name##_table[interface_name##_get_##field##_slot] = (void *)type##_get_##field, \
	 interface_name##_table[interface_name##_put_##field##_slot] = (void *)type##_put_##field)
#define BIND_INTO(table, interface_name, type, field) \
	((table)[interface_name##_get_##field##_slot] = (void *)type##_get_##field, \
	 (table)[interface_name##_put_##field##_slot] = (void *)type##_put_##field)

/* ---- repetition, idle and network settings ---- */
struct repetition {
	struct object base;
	BSTR Interval, Duration;
	VARIANT_BOOL StopAtDurationEnd;
};
STRING_PROPERTY(repetition, Interval)
STRING_PROPERTY(repetition, Duration)
VALUE_PROPERTY(repetition, VARIANT_BOOL, StopAtDurationEnd)

static void repetition_destroy(struct object *base)
{
	struct repetition *self = (struct repetition *)base;
	SysFreeString(self->Interval);
	SysFreeString(self->Duration);
}

static struct object *repetition_new(void)
{
	return object_new(sizeof(struct repetition), IRepetitionPattern_table, repetition_destroy, &IID_IRepetitionPattern);
}

struct idle {
	struct object base;
	BSTR IdleDuration, WaitTimeout;
	VARIANT_BOOL StopOnIdleEnd, RestartOnIdle;
};
STRING_PROPERTY(idle, IdleDuration)
STRING_PROPERTY(idle, WaitTimeout)
VALUE_PROPERTY(idle, VARIANT_BOOL, StopOnIdleEnd)
VALUE_PROPERTY(idle, VARIANT_BOOL, RestartOnIdle)

static void idle_destroy(struct object *base)
{
	struct idle *self = (struct idle *)base;
	SysFreeString(self->IdleDuration);
	SysFreeString(self->WaitTimeout);
}

static struct object *idle_new(void)
{
	struct idle *self = object_new(sizeof(struct idle), IIdleSettings_table, idle_destroy, &IID_IIdleSettings);
	if (self) {
		self->IdleDuration = SysAllocString(L"PT10M");
		self->WaitTimeout = SysAllocString(L"PT1H");
		self->StopOnIdleEnd = VARIANT_TRUE;
	}
	return &self->base;
}

struct network {
	struct object base;
	BSTR Name, Id;
};
STRING_PROPERTY(network, Name)
STRING_PROPERTY(network, Id)

static void network_destroy(struct object *base)
{
	struct network *self = (struct network *)base;
	SysFreeString(self->Name);
	SysFreeString(self->Id);
}

static struct object *network_new(void)
{
	return object_new(sizeof(struct network), INetworkSettings_table, network_destroy, &IID_INetworkSettings);
}

/* ---- triggers ---- */
struct trigger {
	struct object base;
	TASK_TRIGGER_TYPE2 Type;
	BSTR Id, ExecutionTimeLimit, StartBoundary, EndBoundary, Delay, RandomDelay, UserId;
	VARIANT_BOOL Enabled;
	short DaysInterval;
	struct object *Repetition;
};
STRING_PROPERTY(trigger, Id)
STRING_PROPERTY(trigger, ExecutionTimeLimit)
STRING_PROPERTY(trigger, StartBoundary)
STRING_PROPERTY(trigger, EndBoundary)
STRING_PROPERTY(trigger, Delay)
STRING_PROPERTY(trigger, RandomDelay)
STRING_PROPERTY(trigger, UserId)
VALUE_PROPERTY(trigger, VARIANT_BOOL, Enabled)
VALUE_PROPERTY(trigger, short, DaysInterval)
OBJECT_PROPERTY(trigger, Repetition)

static HRESULT STDMETHODCALLTYPE trigger_get_Type(void *iface, TASK_TRIGGER_TYPE2 *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct trigger *)iface)->Type;
	return S_OK;
}

static void trigger_destroy(struct object *base)
{
	struct trigger *self = (struct trigger *)base;
	SysFreeString(self->Id);
	SysFreeString(self->ExecutionTimeLimit);
	SysFreeString(self->StartBoundary);
	SysFreeString(self->EndBoundary);
	SysFreeString(self->Delay);
	SysFreeString(self->RandomDelay);
	SysFreeString(self->UserId);
	release(self->Repetition);
}

/* Each trigger type that has its own interface answers for it with a table that carries the extra properties after ITrigger's. */
static void bind_trigger(void **table)
{
	set_dispatch(table);
	table[ITrigger_get_Type_slot] = (void *)trigger_get_Type;
	BIND_INTO(table, ITrigger, trigger, Id);
	BIND_INTO(table, ITrigger, trigger, Repetition);
	BIND_INTO(table, ITrigger, trigger, ExecutionTimeLimit);
	BIND_INTO(table, ITrigger, trigger, StartBoundary);
	BIND_INTO(table, ITrigger, trigger, EndBoundary);
	BIND_INTO(table, ITrigger, trigger, Enabled);
}

static struct object *trigger_new(TASK_TRIGGER_TYPE2 type)
{
	void **table = ITrigger_table;
	const GUID *iid = &IID_ITrigger;
	struct trigger *self;
	switch (type) {
	case TASK_TRIGGER_TIME: table = ITimeTrigger_table; iid = &IID_ITimeTrigger; break;
	case TASK_TRIGGER_DAILY: table = IDailyTrigger_table; iid = &IID_IDailyTrigger; break;
	case TASK_TRIGGER_LOGON: table = ILogonTrigger_table; iid = &IID_ILogonTrigger; break;
	case TASK_TRIGGER_BOOT: table = IBootTrigger_table; iid = &IID_IBootTrigger; break;
	case TASK_TRIGGER_REGISTRATION: table = IRegistrationTrigger_table; iid = &IID_IRegistrationTrigger; break;
	default: break;
	}
	if ((unsigned)type >= sizeof(trigger_names) / sizeof(*trigger_names) || !trigger_names[type])
		return NULL;
	self = object_new(sizeof(struct trigger), table, trigger_destroy, iid);
	if (!self)
		return NULL;
	self->base.base_iid = &IID_ITrigger;
	self->Type = type;
	self->Enabled = VARIANT_TRUE;
	self->DaysInterval = 1;
	self->Repetition = repetition_new();
	return &self->base;
}

/* ---- actions ---- */
struct exec {
	struct object base;
	BSTR Id, Path, Arguments, WorkingDirectory;
};
STRING_PROPERTY(exec, Id)
STRING_PROPERTY(exec, Path)
STRING_PROPERTY(exec, Arguments)
STRING_PROPERTY(exec, WorkingDirectory)

static HRESULT STDMETHODCALLTYPE exec_get_Type(void *iface, TASK_ACTION_TYPE *out)
{
	if (!out)
		return E_POINTER;
	*out = TASK_ACTION_EXEC;
	return S_OK;
}

static void exec_destroy(struct object *base)
{
	struct exec *self = (struct exec *)base;
	SysFreeString(self->Id);
	SysFreeString(self->Path);
	SysFreeString(self->Arguments);
	SysFreeString(self->WorkingDirectory);
}

static struct object *exec_new(void)
{
	struct exec *self = object_new(sizeof(struct exec), IExecAction_table, exec_destroy, &IID_IExecAction);
	if (self)
		self->base.base_iid = &IID_IAction;
	return &self->base;
}

/* ---- collections ---- */
/* The items of a definition's triggers or actions, or of a list handed out: registered tasks, folders or running tasks. */
struct list {
	struct object base;
	struct object **items;
	LONG count;
	BSTR Context;
	BSTR (*name_of)(struct object *item); /* the name an item is found by, for a list that has one */
};

static void list_destroy(struct object *base)
{
	struct list *self = (struct list *)base;
	LONG index;
	for (index = 0; index < self->count; ++index)
		release(self->items[index]);
	FREE(self->items);
	SysFreeString(self->Context);
}

static struct list *list_new(void **table, const GUID *iid)
{
	return object_new(sizeof(struct list), table, list_destroy, iid);
}

static BOOL list_add(struct list *self, struct object *item)
{
	struct object **items = GROW(self->items, (self->count + 1) * sizeof(*items));
	if (!items)
		return FALSE;
	self->items = items;
	self->items[self->count++] = item;
	return TRUE;
}

static void list_clear(struct list *self)
{
	while (self->count)
		release(self->items[--self->count]);
}

static HRESULT STDMETHODCALLTYPE list_get_Count(void *iface, LONG *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct list *)iface)->count;
	return S_OK;
}

/* An item by its position from 1, which is how the scheduler's collections count. */
static HRESULT STDMETHODCALLTYPE list_get_Item(void *iface, LONG index, void **out)
{
	struct list *self = iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (index < 1 || index > self->count)
		return E_INVALIDARG;
	*out = keep(self->items[index - 1]);
	return S_OK;
}

/* A position from 1 or a name the program passed as a VARIANT, as the list of an item it found, or -1. */
static LONG list_find(struct list *self, const VARIANT *index)
{
	LONG at;
	if (V_VT(index) == VT_BSTR && self->name_of) {
		for (at = 0; at < self->count; ++at) {
			BSTR name = self->name_of(self->items[at]);
			const BOOL match = name && !lstrcmpiW(name, V_BSTR(index));
			if (match)
				return at;
		}
		return -1;
	}
	switch (V_VT(index)) {
	case VT_I2: at = V_I2(index); break;
	case VT_UI2: at = V_UI2(index); break;
	case VT_I4: case VT_INT: at = V_I4(index); break;
	case VT_UI4: case VT_UINT: at = (LONG)V_UI4(index); break;
	case VT_I1: at = V_I1(index); break;
	case VT_UI1: at = V_UI1(index); break;
	default: return -1;
	}
	--at;
	return at >= 0 && at < self->count ? at : -1;
}

static HRESULT STDMETHODCALLTYPE list_get_ItemByVariant(void *iface, VARIANT index, void **out)
{
	struct list *self = iface;
	const LONG at = list_find(self, &index);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (at < 0)
		return E_INVALIDARG;
	*out = keep(self->items[at]);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE list_Remove(void *iface, VARIANT index)
{
	struct list *self = iface;
	const LONG at = list_find(self, &index);
	if (at < 0)
		return E_INVALIDARG;
	release(self->items[at]);
	memmove(self->items + at, self->items + at + 1, (self->count - at - 1) * sizeof(*self->items));
	--self->count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE list_Clear(void *iface)
{
	list_clear(iface);
	return S_OK;
}

STRING_PROPERTY(list, Context)

static HRESULT STDMETHODCALLTYPE triggers_Create(void *iface, TASK_TRIGGER_TYPE2 type, ITrigger **out)
{
	struct object *trigger;
	if (!out)
		return E_POINTER;
	*out = NULL;
	trigger = trigger_new(type);
	if (!trigger)
		return E_INVALIDARG;
	if (!list_add(iface, trigger)) {
		release(trigger);
		return E_OUTOFMEMORY;
	}
	*out = (ITrigger *)keep(trigger);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE actions_Create(void *iface, TASK_ACTION_TYPE type, IAction **out)
{
	struct object *action;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (type != TASK_ACTION_EXEC)
		return unimplemented("IActionCollection", "Create of an action other than a program");
	action = exec_new();
	if (!action)
		return E_OUTOFMEMORY;
	if (!list_add(iface, action)) {
		release(action);
		return E_OUTOFMEMORY;
	}
	*out = (IAction *)keep(action);
	return S_OK;
}

/* ---- the parts of a definition ---- */
struct registration {
	struct object base;
	BSTR Description, Author, Version, Date, Documentation, URI, Source;
};
STRING_PROPERTY(registration, Description)
STRING_PROPERTY(registration, Author)
STRING_PROPERTY(registration, Version)
STRING_PROPERTY(registration, Date)
STRING_PROPERTY(registration, Documentation)
STRING_PROPERTY(registration, URI)
STRING_PROPERTY(registration, Source)

static void registration_destroy(struct object *base)
{
	struct registration *self = (struct registration *)base;
	SysFreeString(self->Description);
	SysFreeString(self->Author);
	SysFreeString(self->Version);
	SysFreeString(self->Date);
	SysFreeString(self->Documentation);
	SysFreeString(self->URI);
	SysFreeString(self->Source);
}

struct principal {
	struct object base;
	BSTR Id, DisplayName, UserId, GroupId;
	TASK_LOGON_TYPE LogonType;
	TASK_RUNLEVEL_TYPE RunLevel;
};
STRING_PROPERTY(principal, Id)
STRING_PROPERTY(principal, DisplayName)
STRING_PROPERTY(principal, UserId)
STRING_PROPERTY(principal, GroupId)
VALUE_PROPERTY(principal, TASK_LOGON_TYPE, LogonType)
VALUE_PROPERTY(principal, TASK_RUNLEVEL_TYPE, RunLevel)

static void principal_destroy(struct object *base)
{
	struct principal *self = (struct principal *)base;
	SysFreeString(self->Id);
	SysFreeString(self->DisplayName);
	SysFreeString(self->UserId);
	SysFreeString(self->GroupId);
}

struct settings {
	struct object base;
	VARIANT_BOOL AllowDemandStart, StopIfGoingOnBatteries, DisallowStartIfOnBatteries, AllowHardTerminate, StartWhenAvailable,
		RunOnlyIfNetworkAvailable, Enabled, Hidden, RunOnlyIfIdle, WakeToRun;
	BSTR RestartInterval, ExecutionTimeLimit, DeleteExpiredTaskAfter;
	INT RestartCount, Priority;
	TASK_INSTANCES_POLICY MultipleInstances;
	TASK_COMPATIBILITY Compatibility;
	struct object *IdleSettings, *NetworkSettings;
};
VALUE_PROPERTY(settings, VARIANT_BOOL, AllowDemandStart)
VALUE_PROPERTY(settings, VARIANT_BOOL, StopIfGoingOnBatteries)
VALUE_PROPERTY(settings, VARIANT_BOOL, DisallowStartIfOnBatteries)
VALUE_PROPERTY(settings, VARIANT_BOOL, AllowHardTerminate)
VALUE_PROPERTY(settings, VARIANT_BOOL, StartWhenAvailable)
VALUE_PROPERTY(settings, VARIANT_BOOL, RunOnlyIfNetworkAvailable)
VALUE_PROPERTY(settings, VARIANT_BOOL, Enabled)
VALUE_PROPERTY(settings, VARIANT_BOOL, Hidden)
VALUE_PROPERTY(settings, VARIANT_BOOL, RunOnlyIfIdle)
VALUE_PROPERTY(settings, VARIANT_BOOL, WakeToRun)
STRING_PROPERTY(settings, RestartInterval)
STRING_PROPERTY(settings, ExecutionTimeLimit)
STRING_PROPERTY(settings, DeleteExpiredTaskAfter)
VALUE_PROPERTY(settings, INT, RestartCount)
VALUE_PROPERTY(settings, INT, Priority)
VALUE_PROPERTY(settings, TASK_INSTANCES_POLICY, MultipleInstances)
VALUE_PROPERTY(settings, TASK_COMPATIBILITY, Compatibility)
OBJECT_PROPERTY(settings, IdleSettings)
OBJECT_PROPERTY(settings, NetworkSettings)

static void settings_destroy(struct object *base)
{
	struct settings *self = (struct settings *)base;
	SysFreeString(self->RestartInterval);
	SysFreeString(self->ExecutionTimeLimit);
	SysFreeString(self->DeleteExpiredTaskAfter);
	release(self->IdleSettings);
	release(self->NetworkSettings);
}

/* ---- definitions ---- */
struct definition {
	struct object base;
	struct object *RegistrationInfo, *Triggers, *Settings, *Principal, *Actions;
	BSTR Data;
};
OBJECT_PROPERTY(definition, RegistrationInfo)
OBJECT_PROPERTY(definition, Triggers)
OBJECT_PROPERTY(definition, Settings)
OBJECT_PROPERTY(definition, Principal)
OBJECT_PROPERTY(definition, Actions)
STRING_PROPERTY(definition, Data)

static void definition_destroy(struct object *base)
{
	struct definition *self = (struct definition *)base;
	release(self->RegistrationInfo);
	release(self->Triggers);
	release(self->Settings);
	release(self->Principal);
	release(self->Actions);
	SysFreeString(self->Data);
}

/* A new definition with the defaults Windows gives one. */
static struct definition *definition_new(void)
{
	struct definition *self = object_new(sizeof(struct definition), ITaskDefinition_table, definition_destroy, &IID_ITaskDefinition);
	struct settings *settings;
	if (!self)
		return NULL;
	self->RegistrationInfo = object_new(sizeof(struct registration), IRegistrationInfo_table, registration_destroy, &IID_IRegistrationInfo);
	self->Triggers = &list_new(ITriggerCollection_table, &IID_ITriggerCollection)->base;
	self->Principal = object_new(sizeof(struct principal), IPrincipal_table, principal_destroy, &IID_IPrincipal);
	self->Actions = &list_new(IActionCollection_table, &IID_IActionCollection)->base;
	settings = object_new(sizeof(struct settings), ITaskSettings_table, settings_destroy, &IID_ITaskSettings);
	self->Settings = &settings->base;
	if (!self->RegistrationInfo || !self->Triggers || !self->Principal || !self->Actions || !settings) {
		release(&self->base);
		return NULL;
	}
	settings->AllowDemandStart = settings->StopIfGoingOnBatteries = settings->DisallowStartIfOnBatteries = VARIANT_TRUE;
	settings->AllowHardTerminate = settings->Enabled = VARIANT_TRUE;
	settings->ExecutionTimeLimit = SysAllocString(L"PT72H");
	settings->Priority = 7;
	settings->MultipleInstances = TASK_INSTANCES_IGNORE_NEW;
	settings->Compatibility = TASK_COMPATIBILITY_V2;
	settings->IdleSettings = idle_new();
	settings->NetworkSettings = network_new();
	return self;
}

static void write_xml(struct definition *self, struct text *text)
{
	struct registration *registration = (struct registration *)self->RegistrationInfo;
	struct list *triggers = (struct list *)self->Triggers, *actions = (struct list *)self->Actions;
	struct principal *principal = (struct principal *)self->Principal;
	struct settings *settings = (struct settings *)self->Settings;
	struct idle *idle = (struct idle *)settings->IdleSettings;
	LONG index;
	add(text, L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n");
	add_open(text, 1, L"RegistrationInfo");
	add_element(text, 2, L"Date", registration->Date);
	add_element(text, 2, L"Author", registration->Author);
	add_element(text, 2, L"Version", registration->Version);
	add_element(text, 2, L"Description", registration->Description);
	add_element(text, 2, L"Documentation", registration->Documentation);
	add_element(text, 2, L"Source", registration->Source);
	add_element(text, 2, L"URI", registration->URI);
	add_close(text, 1, L"RegistrationInfo");
	add_open(text, 1, L"Triggers");
	for (index = 0; index < triggers->count; ++index) {
		struct trigger *trigger = (struct trigger *)triggers->items[index];
		struct repetition *repetition = (struct repetition *)trigger->Repetition;
		const WCHAR *name = trigger_names[trigger->Type];
		add_open(text, 2, name);
		if (repetition && !empty(repetition->Interval)) {
			add_open(text, 3, L"Repetition");
			add_element(text, 4, L"Interval", repetition->Interval);
			add_element(text, 4, L"Duration", repetition->Duration);
			add_flag(text, 4, L"StopAtDurationEnd", repetition->StopAtDurationEnd);
			add_close(text, 3, L"Repetition");
		}
		add_element(text, 3, L"StartBoundary", trigger->StartBoundary);
		add_element(text, 3, L"EndBoundary", trigger->EndBoundary);
		add_element(text, 3, L"ExecutionTimeLimit", trigger->ExecutionTimeLimit);
		add_flag(text, 3, L"Enabled", trigger->Enabled);
		add_element(text, 3, L"RandomDelay", trigger->RandomDelay);
		add_element(text, 3, L"Delay", trigger->Delay);
		add_element(text, 3, L"UserId", trigger->UserId);
		if (trigger->Type == TASK_TRIGGER_DAILY) {
			add_open(text, 3, L"ScheduleByDay");
			add_indent(text, 4);
			add(text, L"<DaysInterval>");
			add_number(text, trigger->DaysInterval);
			add(text, L"</DaysInterval>\r\n");
			add_close(text, 3, L"ScheduleByDay");
		}
		add_close(text, 2, name);
	}
	add_close(text, 1, L"Triggers");
	add_open(text, 1, L"Principals");
	add_indent(text, 2);
	add(text, L"<Principal id=\"");
	add_escaped(text, empty(principal->Id) ? L"Author" : principal->Id);
	add(text, L"\">\r\n");
	add_element(text, 3, L"UserId", principal->UserId);
	add_element(text, 3, L"GroupId", principal->GroupId);
	add_element(text, 3, L"DisplayName", principal->DisplayName);
	if (principal->LogonType && (unsigned)principal->LogonType < sizeof(logon_names) / sizeof(*logon_names))
		add_element(text, 3, L"LogonType", logon_names[principal->LogonType]);
	add_element(text, 3, L"RunLevel", run_level_names[principal->RunLevel == TASK_RUNLEVEL_HIGHEST]);
	add_close(text, 2, L"Principal");
	add_close(text, 1, L"Principals");
	add_open(text, 1, L"Settings");
	add_element(text, 2, L"MultipleInstancesPolicy", instance_names[(unsigned)settings->MultipleInstances & 3]);
	add_flag(text, 2, L"DisallowStartIfOnBatteries", settings->DisallowStartIfOnBatteries);
	add_flag(text, 2, L"StopIfGoingOnBatteries", settings->StopIfGoingOnBatteries);
	add_flag(text, 2, L"AllowHardTerminate", settings->AllowHardTerminate);
	add_flag(text, 2, L"StartWhenAvailable", settings->StartWhenAvailable);
	add_flag(text, 2, L"RunOnlyIfNetworkAvailable", settings->RunOnlyIfNetworkAvailable);
	if (idle) {
		add_open(text, 2, L"IdleSettings");
		add_element(text, 3, L"Duration", idle->IdleDuration);
		add_element(text, 3, L"WaitTimeout", idle->WaitTimeout);
		add_flag(text, 3, L"StopOnIdleEnd", idle->StopOnIdleEnd);
		add_flag(text, 3, L"RestartOnIdle", idle->RestartOnIdle);
		add_close(text, 2, L"IdleSettings");
	}
	add_flag(text, 2, L"AllowStartOnDemand", settings->AllowDemandStart);
	add_flag(text, 2, L"Enabled", settings->Enabled);
	add_flag(text, 2, L"Hidden", settings->Hidden);
	add_flag(text, 2, L"RunOnlyIfIdle", settings->RunOnlyIfIdle);
	add_flag(text, 2, L"WakeToRun", settings->WakeToRun);
	add_element(text, 2, L"ExecutionTimeLimit", settings->ExecutionTimeLimit);
	add_element(text, 2, L"DeleteExpiredTaskAfter", settings->DeleteExpiredTaskAfter);
	if (!empty(settings->RestartInterval)) {
		add_open(text, 2, L"RestartOnFailure");
		add_element(text, 3, L"Interval", settings->RestartInterval);
		add_indent(text, 3);
		add(text, L"<Count>");
		add_number(text, settings->RestartCount);
		add(text, L"</Count>\r\n");
		add_close(text, 2, L"RestartOnFailure");
	}
	add_indent(text, 2);
	add(text, L"<Priority>");
	add_number(text, settings->Priority);
	add(text, L"</Priority>\r\n");
	add_close(text, 1, L"Settings");
	add_element(text, 1, L"Data", self->Data);
	add_indent(text, 1);
	add(text, L"<Actions Context=\"");
	add_escaped(text, empty(actions->Context) ? L"Author" : actions->Context);
	add(text, L"\">\r\n");
	for (index = 0; index < actions->count; ++index) {
		struct exec *exec = (struct exec *)actions->items[index];
		add_open(text, 2, L"Exec");
		add_element(text, 3, L"Command", exec->Path);
		add_element(text, 3, L"Arguments", exec->Arguments);
		add_element(text, 3, L"WorkingDirectory", exec->WorkingDirectory);
		add_close(text, 2, L"Exec");
	}
	add_close(text, 1, L"Actions");
	add(text, L"</Task>\r\n");
}

static BSTR definition_xml(struct definition *self)
{
	struct text text = {0};
	BSTR result;
	write_xml(self, &text);
	result = text.data ? SysAllocStringLen(text.data, (UINT)text.length) : NULL;
	FREE(text.data);
	return result;
}

/* Fills a definition from task XML. Triggers come back grouped by type, in the order of the trigger types. */
static HRESULT read_xml(struct definition *self, const WCHAR *xml, size_t length)
{
	const WCHAR *end = xml + length, *inner, *inner_end, *after, *task, *task_end;
	struct registration *registration = (struct registration *)self->RegistrationInfo;
	struct principal *principal = (struct principal *)self->Principal;
	struct settings *settings = (struct settings *)self->Settings;
	struct list *triggers = (struct list *)self->Triggers, *actions = (struct list *)self->Actions;
	int choice;
	unsigned type;
	if (!find_element(xml, end, L"Task", &task, &task_end, &after))
		return SCHED_E_MALFORMEDXML;
	if (find_element(task, task_end, L"RegistrationInfo", &inner, &inner_end, &after)) {
		read_string(&registration->Date, inner, inner_end, L"Date");
		read_string(&registration->Author, inner, inner_end, L"Author");
		read_string(&registration->Version, inner, inner_end, L"Version");
		read_string(&registration->Description, inner, inner_end, L"Description");
		read_string(&registration->Documentation, inner, inner_end, L"Documentation");
		read_string(&registration->Source, inner, inner_end, L"Source");
		read_string(&registration->URI, inner, inner_end, L"URI");
	}
	list_clear(triggers);
	if (find_element(task, task_end, L"Triggers", &inner, &inner_end, &after))
		for (type = 0; type < sizeof(trigger_names) / sizeof(*trigger_names); ++type) {
			const WCHAR *from = inner, *body, *body_end, *next;
			/* The calendar types share an element; only the daily one is read back. */
			if (!trigger_names[type] || (type > TASK_TRIGGER_DAILY && type <= TASK_TRIGGER_MONTHLYDOW))
				continue;
			while (find_element(from, inner_end, trigger_names[type], &body, &body_end, &next)) {
				struct trigger *trigger = (struct trigger *)trigger_new(type);
				const WCHAR *part, *part_end, *part_after;
				int days = 1;
				if (!trigger)
					return E_OUTOFMEMORY;
				read_string(&trigger->StartBoundary, body, body_end, L"StartBoundary");
				read_string(&trigger->EndBoundary, body, body_end, L"EndBoundary");
				read_string(&trigger->ExecutionTimeLimit, body, body_end, L"ExecutionTimeLimit");
				read_flag(&trigger->Enabled, body, body_end, L"Enabled");
				read_string(&trigger->RandomDelay, body, body_end, L"RandomDelay");
				read_string(&trigger->Delay, body, body_end, L"Delay");
				read_string(&trigger->UserId, body, body_end, L"UserId");
				read_number(&days, body, body_end, L"DaysInterval");
				trigger->DaysInterval = (short)days;
				if (find_element(body, body_end, L"Repetition", &part, &part_end, &part_after)) {
					struct repetition *repetition = (struct repetition *)trigger->Repetition;
					read_string(&repetition->Interval, part, part_end, L"Interval");
					read_string(&repetition->Duration, part, part_end, L"Duration");
					read_flag(&repetition->StopAtDurationEnd, part, part_end, L"StopAtDurationEnd");
				}
				if (!list_add(triggers, &trigger->base)) {
					release(&trigger->base);
					return E_OUTOFMEMORY;
				}
				from = next;
			}
		}
	if (find_element(task, task_end, L"Principal", &inner, &inner_end, &after)) {
		read_string(&principal->UserId, inner, inner_end, L"UserId");
		read_string(&principal->GroupId, inner, inner_end, L"GroupId");
		read_string(&principal->DisplayName, inner, inner_end, L"DisplayName");
		if ((choice = read_choice(inner, inner_end, L"LogonType", logon_names, sizeof(logon_names) / sizeof(*logon_names))) >= 0)
			principal->LogonType = choice;
		if ((choice = read_choice(inner, inner_end, L"RunLevel", run_level_names, 2)) >= 0)
			principal->RunLevel = choice;
	}
	if (find_element(task, task_end, L"Settings", &inner, &inner_end, &after)) {
		const WCHAR *part, *part_end, *part_after;
		const WCHAR *own_end = inner_end;
		/* The settings' own Enabled and time limit come after the idle settings, which have elements of the same names. */
		if (find_element(inner, inner_end, L"IdleSettings", &part, &part_end, &part_after)) {
			struct idle *idle = (struct idle *)settings->IdleSettings;
			read_string(&idle->IdleDuration, part, part_end, L"Duration");
			read_string(&idle->WaitTimeout, part, part_end, L"WaitTimeout");
			read_flag(&idle->StopOnIdleEnd, part, part_end, L"StopOnIdleEnd");
			read_flag(&idle->RestartOnIdle, part, part_end, L"RestartOnIdle");
		}
		if (find_element(inner, inner_end, L"RestartOnFailure", &part, &part_end, &part_after)) {
			read_string(&settings->RestartInterval, part, part_end, L"Interval");
			read_number(&settings->RestartCount, part, part_end, L"Count");
		}
		if ((choice = read_choice(inner, own_end, L"MultipleInstancesPolicy", instance_names, 4)) >= 0)
			settings->MultipleInstances = choice;
		read_flag(&settings->DisallowStartIfOnBatteries, inner, own_end, L"DisallowStartIfOnBatteries");
		read_flag(&settings->StopIfGoingOnBatteries, inner, own_end, L"StopIfGoingOnBatteries");
		read_flag(&settings->AllowHardTerminate, inner, own_end, L"AllowHardTerminate");
		read_flag(&settings->StartWhenAvailable, inner, own_end, L"StartWhenAvailable");
		read_flag(&settings->RunOnlyIfNetworkAvailable, inner, own_end, L"RunOnlyIfNetworkAvailable");
		read_flag(&settings->AllowDemandStart, inner, own_end, L"AllowStartOnDemand");
		read_flag(&settings->Enabled, inner, own_end, L"Enabled");
		read_flag(&settings->Hidden, inner, own_end, L"Hidden");
		read_flag(&settings->RunOnlyIfIdle, inner, own_end, L"RunOnlyIfIdle");
		read_flag(&settings->WakeToRun, inner, own_end, L"WakeToRun");
		read_string(&settings->ExecutionTimeLimit, inner, own_end, L"ExecutionTimeLimit");
		read_string(&settings->DeleteExpiredTaskAfter, inner, own_end, L"DeleteExpiredTaskAfter");
		read_number(&settings->Priority, inner, own_end, L"Priority");
	}
	read_string(&self->Data, task, task_end, L"Data");
	list_clear(actions);
	if (find_element(task, task_end, L"Actions", &inner, &inner_end, &after)) {
		const WCHAR *from = inner, *body, *body_end, *next;
		while (find_element(from, inner_end, L"Exec", &body, &body_end, &next)) {
			struct exec *exec = (struct exec *)exec_new();
			if (!exec)
				return E_OUTOFMEMORY;
			read_string(&exec->Path, body, body_end, L"Command");
			read_string(&exec->Arguments, body, body_end, L"Arguments");
			read_string(&exec->WorkingDirectory, body, body_end, L"WorkingDirectory");
			if (!list_add(actions, &exec->base)) {
				release(&exec->base);
				return E_OUTOFMEMORY;
			}
			from = next;
		}
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE definition_get_XmlText(void *iface, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = definition_xml(iface);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE definition_put_XmlText(void *iface, BSTR xml)
{
	if (empty(xml))
		return E_INVALIDARG;
	return read_xml(iface, xml, SysStringLen(xml));
}

/* ---- the task store ---- */
/* Where a task or folder path such as \Vendor\Task lives on disk. */
static WCHAR *store_path(const WCHAR *path)
{
	WCHAR *result = ALLOC((MAX_PATH + 32 + (path ? lstrlenW(path) : 0)) * sizeof(WCHAR));
	UINT length;
	if (!result)
		return NULL;
	length = GetWindowsDirectoryW(result, MAX_PATH);
	lstrcpyW(result + length, L"\\System32\\Tasks");
	if (path && *path && lstrcmpW(path, L"\\")) {
		if (*path != '\\')
			lstrcatW(result, L"\\");
		lstrcatW(result, path);
	}
	return result;
}

/* A 32-bit program sees System32 as SysWOW64; the tasks are the machine's, so its file calls here bypass the redirection. */
static void *store_open(void)
{
	void *previous = NULL;
#ifndef _WIN64
	Wow64DisableWow64FsRedirection(&previous);
#endif
	return previous;
}

static void store_close(void *previous)
{
#ifndef _WIN64
	Wow64RevertWow64FsRedirection(previous);
#else
	(void)previous;
#endif
}

/* A path a folder resolves for a task or a subfolder: one that starts with a backslash is absolute, any other is the folder's. */
static WCHAR *join_path(const WCHAR *folder, const WCHAR *name)
{
	WCHAR *result = ALLOC((lstrlenW(folder) + (name ? lstrlenW(name) : 0) + 2) * sizeof(WCHAR));
	if (!result)
		return NULL;
	if (name && *name == '\\') {
		lstrcpyW(result, name);
		return result;
	}
	lstrcpyW(result, folder);
	if (name && *name) {
		if (lstrcmpW(folder, L"\\"))
			lstrcatW(result, L"\\");
		lstrcatW(result, name);
	}
	return result;
}

static const WCHAR *last_part(const WCHAR *path)
{
	const WCHAR *at, *name = path;
	for (at = path; *at; ++at)
		if (*at == '\\' && at[1])
			name = at + 1;
	return name;
}

/* Every directory down to the one that holds `file`. */
static void make_parents(WCHAR *file)
{
	WCHAR *at;
	for (at = file + 3; *at; ++at)
		if (*at == '\\') {
			*at = 0;
			CreateDirectoryW(file, NULL);
			*at = '\\';
		}
}

static HRESULT store_write(const WCHAR *path, const WCHAR *xml, size_t length)
{
	WCHAR *file = store_path(path);
	void *redirection;
	HANDLE handle;
	DWORD written;
	HRESULT result = S_OK;
	static const BYTE mark[] = {0xff, 0xfe};
	if (!file)
		return E_OUTOFMEMORY;
	redirection = store_open();
	make_parents(file);
	handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		result = HRESULT_FROM_WIN32(GetLastError());
	else {
		if (!WriteFile(handle, mark, sizeof(mark), &written, NULL) || !WriteFile(handle, xml, (DWORD)(length * sizeof(WCHAR)), &written, NULL))
			result = HRESULT_FROM_WIN32(GetLastError());
		CloseHandle(handle);
	}
	store_close(redirection);
	FREE(file);
	return result;
}

/* The XML of a stored task, as UTF-16 whether the file holds UTF-16 or UTF-8. */
static HRESULT store_read(const WCHAR *path, WCHAR **xml, size_t *length)
{
	WCHAR *file = store_path(path);
	void *redirection;
	HANDLE handle;
	DWORD size, done = 0;
	BYTE *bytes;
	HRESULT result = S_OK;
	*xml = NULL;
	if (!file)
		return E_OUTOFMEMORY;
	redirection = store_open();
	handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	store_close(redirection);
	FREE(file);
	if (handle == INVALID_HANDLE_VALUE)
		return NOT_FOUND;
	size = GetFileSize(handle, NULL);
	bytes = ALLOC(size + 2);
	if (!bytes || !ReadFile(handle, bytes, size, &done, NULL))
		result = bytes ? HRESULT_FROM_WIN32(GetLastError()) : E_OUTOFMEMORY;
	CloseHandle(handle);
	if (FAILED(result)) {
		FREE(bytes);
		return result;
	}
	if (done >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
		*length = (done - 2) / sizeof(WCHAR);
		*xml = ALLOC((*length + 1) * sizeof(WCHAR));
		if (*xml)
			memcpy(*xml, bytes + 2, *length * sizeof(WCHAR));
	} else {
		const DWORD skip = done >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
		const int count = MultiByteToWideChar(CP_UTF8, 0, (const char *)bytes + skip, (int)(done - skip), NULL, 0);
		*xml = ALLOC((count + 1) * sizeof(WCHAR));
		if (*xml)
			MultiByteToWideChar(CP_UTF8, 0, (const char *)bytes + skip, (int)(done - skip), *xml, count);
		*length = count;
	}
	FREE(bytes);
	return *xml ? S_OK : E_OUTOFMEMORY;
}

static DWORD store_attributes(const WCHAR *path)
{
	WCHAR *file = store_path(path);
	void *redirection;
	DWORD attributes;
	if (!file)
		return INVALID_FILE_ATTRIBUTES;
	redirection = store_open();
	attributes = GetFileAttributesW(file);
	store_close(redirection);
	FREE(file);
	return attributes;
}

/* ---- running tasks ---- */
struct running {
	struct object base;
	BSTR name, path, guid, action;
	DWORD pid;
};

static void running_destroy(struct object *base)
{
	struct running *self = (struct running *)base;
	SysFreeString(self->name);
	SysFreeString(self->path);
	SysFreeString(self->guid);
	SysFreeString(self->action);
}

static HRESULT STDMETHODCALLTYPE running_get_Name(void *iface, BSTR *out) { return get_string(((struct running *)iface)->name, out); }
static HRESULT STDMETHODCALLTYPE running_get_InstanceGuid(void *iface, BSTR *out) { return get_string(((struct running *)iface)->guid, out); }
static HRESULT STDMETHODCALLTYPE running_get_Path(void *iface, BSTR *out) { return get_string(((struct running *)iface)->path, out); }
static HRESULT STDMETHODCALLTYPE running_get_CurrentAction(void *iface, BSTR *out) { return get_string(((struct running *)iface)->action, out); }

/* Running while the program it started is. */
static HRESULT STDMETHODCALLTYPE running_get_State(void *iface, TASK_STATE *out)
{
	struct running *self = iface;
	HANDLE process;
	DWORD code = 0;
	if (!out)
		return E_POINTER;
	*out = TASK_STATE_READY;
	process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, self->pid);
	if (process) {
		if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE)
			*out = TASK_STATE_RUNNING;
		CloseHandle(process);
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE running_get_EnginePID(void *iface, DWORD *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct running *)iface)->pid;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE running_Stop(void *iface)
{
	HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, ((struct running *)iface)->pid);
	if (process) {
		TerminateProcess(process, 0x41306);
		CloseHandle(process);
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE running_Refresh(void *iface)
{
	return S_OK;
}

static WCHAR *expand(const WCHAR *text)
{
	DWORD size;
	WCHAR *result;
	if (empty(text))
		return NULL;
	size = ExpandEnvironmentStringsW(text, NULL, 0);
	result = ALLOC((size + 1) * sizeof(WCHAR));
	if (result && !ExpandEnvironmentStringsW(text, result, size + 1))
		lstrcpyW(result, text);
	return result;
}

static BSTR new_guid(void)
{
	static const WCHAR digits[] = L"0123456789ABCDEF";
	WCHAR text[39];
	LARGE_INTEGER counter;
	DWORD64 mix;
	int at, index;
	QueryPerformanceCounter(&counter);
	mix = (DWORD64)counter.QuadPart * 0x9e3779b97f4a7c15ULL ^ ((DWORD64)GetCurrentProcessId() << 32) ^ GetTickCount();
	text[0] = '{';
	for (at = 1, index = 0; index < 32; ++index) {
		if (index == 8 || index == 12 || index == 16 || index == 20)
			text[at++] = '-';
		text[at++] = digits[mix & 15];
		mix = (mix >> 4) | (mix << 60);
		if ((index & 15) == 15)
			mix = mix * 0x2545f4914f6cdd1dULL + index;
	}
	text[at++] = '}';
	text[at] = 0;
	return SysAllocString(text);
}

/* Starts each program a definition names, as the caller, and answers with the first one as the running task. */
static HRESULT run_definition(struct definition *definition, const WCHAR *path, struct running **out)
{
	struct list *actions = (struct list *)definition->Actions;
	LONG index;
	HRESULT result = S_OK;
	*out = NULL;
	if (!actions->count)
		return SCHED_E_TASK_NOT_READY;
	for (index = 0; index < actions->count; ++index) {
		struct exec *exec = (struct exec *)actions->items[index];
		WCHAR *program = expand(exec->Path), *arguments = expand(exec->Arguments), *directory = expand(exec->WorkingDirectory);
		WCHAR *command;
		STARTUPINFOW startup = {sizeof(startup)};
		PROCESS_INFORMATION process;
		if (!program) {
			FREE(arguments);
			FREE(directory);
			continue;
		}
		command = ALLOC((lstrlenW(program) + (arguments ? lstrlenW(arguments) : 0) + 4) * sizeof(WCHAR));
		if (command) {
			const BOOL quote = *program != '"';
			if (quote)
				lstrcatW(command, L"\"");
			lstrcatW(command, program);
			if (quote)
				lstrcatW(command, L"\"");
			if (arguments) {
				lstrcatW(command, L" ");
				lstrcatW(command, arguments);
			}
			if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process)) {
				CloseHandle(process.hThread);
				CloseHandle(process.hProcess);
				if (!*out) {
					struct running *running = object_new(sizeof(struct running), IRunningTask_table, running_destroy, &IID_IRunningTask);
					if (running) {
						running->pid = process.dwProcessId;
						running->name = SysAllocString(last_part(path));
						running->path = SysAllocString(path);
						running->guid = new_guid();
						running->action = SysAllocString(program);
					}
					*out = running;
				}
			} else if (SUCCEEDED(result))
				result = HRESULT_FROM_WIN32(GetLastError());
		}
		FREE(command);
		FREE(program);
		FREE(arguments);
		FREE(directory);
	}
	return *out ? S_OK : FAILED(result) ? result : SCHED_E_TASK_NOT_READY;
}

/* An automation date: days since the end of 1899, with the time of day as the fraction. */
static DATE variant_time(const SYSTEMTIME *time)
{
	/* Days from 1 March of year 0 in the proleptic Gregorian calendar, which keeps leap days at the end of a year. */
	const int year = time->wYear - (time->wMonth <= 2), month = (time->wMonth + 9) % 12;
	const LONG days = 365L * year + year / 4 - year / 100 + year / 400 + (153 * month + 2) / 5 + time->wDay - 1;
	const LONG epoch = 365L * 1899 + 1899 / 4 - 1899 / 100 + 1899 / 400 + (153 * 9 + 2) / 5 + 30 - 1;
	return (DATE)(days - epoch) + (time->wHour * 3600.0 + time->wMinute * 60.0 + time->wSecond + time->wMilliseconds / 1000.0) / 86400.0;
}

/* ---- registered tasks ---- */
struct registered {
	struct object base;
	BSTR path;
	struct definition *definition;
	DATE last_run;
	LONG last_result;
};

static void registered_destroy(struct object *base)
{
	struct registered *self = (struct registered *)base;
	SysFreeString(self->path);
	release(&self->definition->base);
}

static BSTR registered_name(struct object *item)
{
	return item ? (BSTR)last_part(((struct registered *)item)->path) : NULL;
}

static struct registered *registered_new(const WCHAR *path, struct definition *definition)
{
	struct registered *self = object_new(sizeof(struct registered), IRegisteredTask_table, registered_destroy, &IID_IRegisteredTask);
	if (!self)
		return NULL;
	self->path = SysAllocString(path);
	self->definition = (struct definition *)keep(&definition->base);
	self->last_result = TASK_HAS_NOT_RUN;
	return self;
}

static HRESULT STDMETHODCALLTYPE registered_get_Name(void *iface, BSTR *out)
{
	if (!out)
		return E_POINTER;
	*out = SysAllocString(last_part(((struct registered *)iface)->path));
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE registered_get_Path(void *iface, BSTR *out) { return get_string(((struct registered *)iface)->path, out); }

static HRESULT STDMETHODCALLTYPE registered_get_State(void *iface, TASK_STATE *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct settings *)((struct registered *)iface)->definition->Settings)->Enabled ? TASK_STATE_READY : TASK_STATE_DISABLED;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE registered_get_Enabled(void *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct settings *)((struct registered *)iface)->definition->Settings)->Enabled;
	return S_OK;
}

/* Turning a task on or off changes it where it is stored. */
static HRESULT STDMETHODCALLTYPE registered_put_Enabled(void *iface, VARIANT_BOOL enabled)
{
	struct registered *self = iface;
	BSTR xml;
	HRESULT result;
	((struct settings *)self->definition->Settings)->Enabled = enabled ? VARIANT_TRUE : VARIANT_FALSE;
	xml = definition_xml(self->definition);
	if (!xml)
		return E_OUTOFMEMORY;
	result = store_write(self->path, xml, SysStringLen(xml));
	SysFreeString(xml);
	return result;
}

static HRESULT registered_run(struct registered *self, IRunningTask **out)
{
	struct running *running;
	SYSTEMTIME now;
	HRESULT result;
	if (out)
		*out = NULL;
	if (!((struct settings *)self->definition->Settings)->Enabled)
		return SCHED_E_TASK_DISABLED;
	result = run_definition(self->definition, self->path, &running);
	GetLocalTime(&now);
	self->last_run = variant_time(&now);
	self->last_result = result;
	if (FAILED(result))
		return result;
	if (out)
		*out = (IRunningTask *)running;
	else
		release(&running->base);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE registered_Run(void *iface, VARIANT parameters, IRunningTask **out)
{
	return registered_run(iface, out);
}

static HRESULT STDMETHODCALLTYPE registered_RunEx(void *iface, VARIANT parameters, LONG flags, LONG session, BSTR user, IRunningTask **out)
{
	return registered_run(iface, out);
}

static HRESULT STDMETHODCALLTYPE registered_GetInstances(void *iface, LONG flags, IRunningTaskCollection **out)
{
	if (!out)
		return E_POINTER;
	*out = (IRunningTaskCollection *)list_new(IRunningTaskCollection_table, &IID_IRunningTaskCollection);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE registered_get_LastRunTime(void *iface, DATE *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct registered *)iface)->last_run;
	return ((struct registered *)iface)->last_run ? S_OK : SCHED_S_TASK_HAS_NOT_RUN;
}

static HRESULT STDMETHODCALLTYPE registered_get_LastTaskResult(void *iface, LONG *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct registered *)iface)->last_result;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE registered_get_NumberOfMissedRuns(void *iface, LONG *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return S_OK;
}

/* Nothing keeps time for the triggers, so no run is ever due. */
static HRESULT STDMETHODCALLTYPE registered_get_NextRunTime(void *iface, DATE *out)
{
	if (!out)
		return E_POINTER;
	*out = 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE registered_get_Definition(void *iface, ITaskDefinition **out)
{
	if (!out)
		return E_POINTER;
	*out = (ITaskDefinition *)keep(&((struct registered *)iface)->definition->base);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE registered_get_Xml(void *iface, BSTR *out)
{
	return definition_get_XmlText(((struct registered *)iface)->definition, out);
}

static HRESULT STDMETHODCALLTYPE registered_Stop(void *iface, LONG flags)
{
	return S_OK;
}

/* A stored task, read back. */
static HRESULT load_task(const WCHAR *path, struct registered **out)
{
	struct definition *definition;
	WCHAR *xml;
	size_t length;
	HRESULT result;
	*out = NULL;
	if ((result = store_read(path, &xml, &length)) != S_OK)
		return result;
	definition = definition_new();
	if (!definition) {
		FREE(xml);
		return E_OUTOFMEMORY;
	}
	result = read_xml(definition, xml, length);
	FREE(xml);
	if (SUCCEEDED(result)) {
		*out = registered_new(path, definition);
		if (!*out)
			result = E_OUTOFMEMORY;
	}
	release(&definition->base);
	return result;
}

/* ---- folders ---- */
struct folder {
	struct object base;
	BSTR path;
};

static void folder_destroy(struct object *base)
{
	SysFreeString(((struct folder *)base)->path);
}

static BSTR folder_name(struct object *item)
{
	return item ? (BSTR)last_part(((struct folder *)item)->path) : NULL;
}

static struct folder *folder_new(const WCHAR *path)
{
	struct folder *self = object_new(sizeof(struct folder), ITaskFolder_table, folder_destroy, &IID_ITaskFolder);
	if (!self)
		return NULL;
	self->path = SysAllocString(path);
	return self;
}

static HRESULT STDMETHODCALLTYPE folder_get_Name(void *iface, BSTR *out)
{
	const WCHAR *path = ((struct folder *)iface)->path;
	if (!out)
		return E_POINTER;
	*out = SysAllocString(lstrcmpW(path, L"\\") ? last_part(path) : path);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE folder_get_Path(void *iface, BSTR *out) { return get_string(((struct folder *)iface)->path, out); }

static HRESULT open_folder(const WCHAR *base, const WCHAR *name, ITaskFolder **out)
{
	WCHAR *path;
	DWORD attributes;
	if (!out)
		return E_POINTER;
	*out = NULL;
	path = join_path(base, name);
	if (!path)
		return E_OUTOFMEMORY;
	/* A trailing backslash names the same folder. */
	if (lstrlenW(path) > 1 && path[lstrlenW(path) - 1] == '\\')
		path[lstrlenW(path) - 1] = 0;
	attributes = store_attributes(path);
	if (lstrcmpW(path, L"\\") && (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))) {
		FREE(path);
		return NO_FOLDER;
	}
	*out = (ITaskFolder *)folder_new(path);
	FREE(path);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE folder_GetFolder(void *iface, BSTR name, ITaskFolder **out)
{
	return open_folder(((struct folder *)iface)->path, name, out);
}

/* The folder's entries that are directories, or that are files, under the store. */
static HRESULT list_folder(const WCHAR *path, BOOL directories, struct list *list, BOOL hidden_too)
{
	WCHAR *pattern = store_path(path), *joined;
	WIN32_FIND_DATAW entry;
	HANDLE find;
	void *redirection;
	if (!pattern)
		return E_OUTOFMEMORY;
	lstrcatW(pattern, L"\\*");
	redirection = store_open();
	find = FindFirstFileW(pattern, &entry);
	store_close(redirection);
	FREE(pattern);
	if (find == INVALID_HANDLE_VALUE)
		return S_OK;
	do {
		struct object *item = NULL;
		if (!lstrcmpW(entry.cFileName, L".") || !lstrcmpW(entry.cFileName, L"..") ||
			!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != !directories)
			continue;
		joined = join_path(path, entry.cFileName);
		if (!joined)
			break;
		if (directories)
			item = &folder_new(joined)->base;
		else {
			struct registered *task;
			if (load_task(joined, &task) == S_OK) {
				if (hidden_too || !((struct settings *)task->definition->Settings)->Hidden)
					item = &task->base;
				else
					release(&task->base);
			}
		}
		FREE(joined);
		if (item && !list_add(list, item))
			release(item);
	} while (FindNextFileW(find, &entry));
	FindClose(find);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE folder_GetFolders(void *iface, LONG flags, ITaskFolderCollection **out)
{
	struct list *list;
	if (!out)
		return E_POINTER;
	list = list_new(ITaskFolderCollection_table, &IID_ITaskFolderCollection);
	*out = (ITaskFolderCollection *)list;
	if (!list)
		return E_OUTOFMEMORY;
	list->name_of = folder_name;
	return list_folder(((struct folder *)iface)->path, TRUE, list, TRUE);
}

static HRESULT STDMETHODCALLTYPE folder_CreateFolder(void *iface, BSTR name, VARIANT sddl, ITaskFolder **out)
{
	WCHAR *path, *file;
	void *redirection;
	BOOL made;
	DWORD error;
	if (out)
		*out = NULL;
	if (empty(name))
		return E_INVALIDARG;
	path = join_path(((struct folder *)iface)->path, name);
	file = path ? store_path(path) : NULL;
	if (!file) {
		FREE(path);
		return E_OUTOFMEMORY;
	}
	redirection = store_open();
	lstrcatW(file, L"\\");
	make_parents(file);
	file[lstrlenW(file) - 1] = 0;
	made = CreateDirectoryW(file, NULL);
	error = GetLastError();
	store_close(redirection);
	FREE(file);
	if (!made && error != ERROR_ALREADY_EXISTS) {
		FREE(path);
		return HRESULT_FROM_WIN32(error);
	}
	if (!made) {
		FREE(path);
		return ALREADY_EXISTS;
	}
	if (out)
		*out = (ITaskFolder *)folder_new(path);
	FREE(path);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE folder_DeleteFolder(void *iface, BSTR name, LONG flags)
{
	WCHAR *path = join_path(((struct folder *)iface)->path, name), *file = path ? store_path(path) : NULL;
	void *redirection;
	BOOL removed;
	DWORD error;
	FREE(path);
	if (!file)
		return E_OUTOFMEMORY;
	redirection = store_open();
	removed = RemoveDirectoryW(file);
	error = GetLastError();
	store_close(redirection);
	FREE(file);
	return removed ? S_OK : HRESULT_FROM_WIN32(error);
}

static HRESULT STDMETHODCALLTYPE folder_GetTask(void *iface, BSTR name, IRegisteredTask **out)
{
	WCHAR *path;
	HRESULT result;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (empty(name))
		return E_INVALIDARG;
	path = join_path(((struct folder *)iface)->path, name);
	if (!path)
		return E_OUTOFMEMORY;
	result = load_task(path, (struct registered **)out);
	FREE(path);
	return result;
}

static HRESULT STDMETHODCALLTYPE folder_GetTasks(void *iface, LONG flags, IRegisteredTaskCollection **out)
{
	struct list *list;
	if (!out)
		return E_POINTER;
	list = list_new(IRegisteredTaskCollection_table, &IID_IRegisteredTaskCollection);
	*out = (IRegisteredTaskCollection *)list;
	if (!list)
		return E_OUTOFMEMORY;
	list->name_of = registered_name;
	return list_folder(((struct folder *)iface)->path, FALSE, list, (flags & TASK_ENUM_HIDDEN) != 0);
}

static HRESULT STDMETHODCALLTYPE folder_DeleteTask(void *iface, BSTR name, LONG flags)
{
	WCHAR *path = join_path(((struct folder *)iface)->path, name), *file = path ? store_path(path) : NULL;
	void *redirection;
	BOOL deleted;
	FREE(path);
	if (!file)
		return E_OUTOFMEMORY;
	redirection = store_open();
	deleted = DeleteFileW(file);
	store_close(redirection);
	FREE(file);
	return deleted ? S_OK : NOT_FOUND;
}

/* Stores a definition under a path, with the account and logon the program gave, and runs it at once when it has a registration
 * trigger. */
static HRESULT register_definition(struct folder *folder, const WCHAR *name, struct definition *definition, LONG flags, const VARIANT *user,
	TASK_LOGON_TYPE logon, IRegisteredTask **out)
{
	struct principal *principal = (struct principal *)definition->Principal;
	struct registration *registration = (struct registration *)definition->RegistrationInfo;
	struct list *triggers = (struct list *)definition->Triggers;
	struct registered *task;
	BSTR generated = NULL, xml;
	WCHAR *path;
	DWORD attributes;
	HRESULT result;
	LONG index;
	if (out)
		*out = NULL;
	if (!((struct list *)definition->Actions)->count)
		return SCHED_E_MISSINGNODE;
	if (empty(name))
		name = generated = new_guid();
	path = join_path(folder->path, name);
	SysFreeString(generated);
	if (!path)
		return E_OUTOFMEMORY;
	attributes = store_attributes(path);
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		FREE(path);
		return E_ACCESSDENIED;
	}
	if ((attributes != INVALID_FILE_ATTRIBUTES && !(flags & TASK_UPDATE)) || (attributes == INVALID_FILE_ATTRIBUTES && !(flags & TASK_CREATE) &&
		!(flags & TASK_VALIDATE_ONLY))) {
		FREE(path);
		return attributes != INVALID_FILE_ATTRIBUTES ? ALREADY_EXISTS : NOT_FOUND;
	}
	if (flags & TASK_VALIDATE_ONLY) {
		FREE(path);
		return S_OK;
	}
	if (variant_text(user))
		put_string(&principal->UserId, variant_text(user));
	if (logon != TASK_LOGON_NONE)
		principal->LogonType = logon;
	put_string(&registration->URI, path);
	xml = definition_xml(definition);
	result = xml ? store_write(path, xml, SysStringLen(xml)) : E_OUTOFMEMORY;
	SysFreeString(xml);
	if (FAILED(result)) {
		FREE(path);
		return result;
	}
	task = registered_new(path, definition);
	FREE(path);
	if (!task)
		return E_OUTOFMEMORY;
	for (index = 0; index < triggers->count; ++index) {
		struct trigger *trigger = (struct trigger *)triggers->items[index];
		if (trigger->Type == TASK_TRIGGER_REGISTRATION && trigger->Enabled) {
			registered_run(task, NULL);
			break;
		}
	}
	if (out)
		*out = (IRegisteredTask *)task;
	else
		release(&task->base);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE folder_RegisterTaskDefinition(void *iface, BSTR name, ITaskDefinition *definition, LONG flags, VARIANT user,
	VARIANT password, TASK_LOGON_TYPE logon, VARIANT sddl, IRegisteredTask **out)
{
	if (!definition)
		return E_POINTER;
	return register_definition(iface, name, (struct definition *)definition, flags, &user, logon, out);
}

static HRESULT STDMETHODCALLTYPE folder_RegisterTask(void *iface, BSTR name, BSTR xml, LONG flags, VARIANT user, VARIANT password,
	TASK_LOGON_TYPE logon, VARIANT sddl, IRegisteredTask **out)
{
	struct definition *definition;
	HRESULT result;
	if (empty(xml))
		return E_INVALIDARG;
	definition = definition_new();
	if (!definition)
		return E_OUTOFMEMORY;
	result = read_xml(definition, xml, SysStringLen(xml));
	if (SUCCEEDED(result))
		result = register_definition(iface, name, definition, flags, &user, logon, out);
	release(&definition->base);
	return result;
}

/* ---- the service ---- */
struct service {
	struct object base;
	VARIANT_BOOL connected;
};

static HRESULT STDMETHODCALLTYPE service_GetFolder(void *iface, BSTR path, ITaskFolder **out)
{
	if (!((struct service *)iface)->connected) {
		if (out)
			*out = NULL;
		return NOT_CONNECTED;
	}
	return open_folder(L"\\", empty(path) ? L"\\" : path, out);
}

/* This component runs no task of its own, so none is ever running on the scheduler's side. */
static HRESULT STDMETHODCALLTYPE service_GetRunningTasks(void *iface, LONG flags, IRunningTaskCollection **out)
{
	if (!out)
		return E_POINTER;
	*out = (IRunningTaskCollection *)list_new(IRunningTaskCollection_table, &IID_IRunningTaskCollection);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE service_NewTask(void *iface, DWORD flags, ITaskDefinition **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!((struct service *)iface)->connected)
		return NOT_CONNECTED;
	*out = (ITaskDefinition *)definition_new();
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* The local machine is the only one this scheduler reaches. */
static HRESULT STDMETHODCALLTYPE service_Connect(void *iface, VARIANT server, VARIANT user, VARIANT domain, VARIANT password)
{
	const WCHAR *name = variant_text(&server);
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
	if (name) {
		GetComputerNameW(computer, &size);
		if (name[0] == '\\' && name[1] == '\\')
			name += 2;
		if (lstrcmpiW(name, computer) && lstrcmpiW(name, L"localhost") && lstrcmpW(name, L"."))
			return HRESULT_FROM_WIN32(ERROR_BAD_NETPATH);
	}
	((struct service *)iface)->connected = VARIANT_TRUE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE service_get_Connected(void *iface, VARIANT_BOOL *out)
{
	if (!out)
		return E_POINTER;
	*out = ((struct service *)iface)->connected;
	return S_OK;
}

static HRESULT computer_name(BSTR *out)
{
	WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
	if (!out)
		return E_POINTER;
	if (!GetComputerNameW(computer, &size))
		lstrcpyW(computer, L"localhost");
	*out = SysAllocString(computer);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE service_get_TargetServer(void *iface, BSTR *out)
{
	return ((struct service *)iface)->connected ? computer_name(out) : NOT_CONNECTED;
}

static HRESULT STDMETHODCALLTYPE service_get_ConnectedDomain(void *iface, BSTR *out)
{
	return ((struct service *)iface)->connected ? computer_name(out) : NOT_CONNECTED;
}

static HRESULT STDMETHODCALLTYPE service_get_ConnectedUser(void *iface, BSTR *out)
{
	WCHAR user[257];
	DWORD size = 257;
	if (!out)
		return E_POINTER;
	if (!((struct service *)iface)->connected)
		return NOT_CONNECTED;
	if (!GetUserNameW(user, &size))
		return HRESULT_FROM_WIN32(GetLastError());
	*out = SysAllocString(user);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* Version 1.6, the scheduler of Windows 10. */
static HRESULT STDMETHODCALLTYPE service_get_HighestVersion(void *iface, DWORD *out)
{
	if (!out)
		return E_POINTER;
	if (!((struct service *)iface)->connected)
		return NOT_CONNECTED;
	*out = (1 << 16) | 6;
	return S_OK;
}

/* ---- tables ---- */
static void prepare_tables(void)
{
	static void **const dispatch_tables[] = {ITaskService_table, ITaskFolder_table, ITaskFolderCollection_table, ITaskDefinition_table,
		IRegistrationInfo_table, ITriggerCollection_table, IRepetitionPattern_table, ITaskSettings_table, IIdleSettings_table,
		INetworkSettings_table, IPrincipal_table, IActionCollection_table, IExecAction_table, IRegisteredTask_table,
		IRegisteredTaskCollection_table, IRunningTask_table, IRunningTaskCollection_table};
	size_t index;
	for (index = 0; index < sizeof(dispatch_tables) / sizeof(*dispatch_tables); ++index)
		set_dispatch(dispatch_tables[index]);

	ITaskService_table[ITaskService_GetFolder_slot] = (void *)service_GetFolder;
	ITaskService_table[ITaskService_GetRunningTasks_slot] = (void *)service_GetRunningTasks;
	ITaskService_table[ITaskService_NewTask_slot] = (void *)service_NewTask;
	ITaskService_table[ITaskService_Connect_slot] = (void *)service_Connect;
	ITaskService_table[ITaskService_get_Connected_slot] = (void *)service_get_Connected;
	ITaskService_table[ITaskService_get_TargetServer_slot] = (void *)service_get_TargetServer;
	ITaskService_table[ITaskService_get_ConnectedUser_slot] = (void *)service_get_ConnectedUser;
	ITaskService_table[ITaskService_get_ConnectedDomain_slot] = (void *)service_get_ConnectedDomain;
	ITaskService_table[ITaskService_get_HighestVersion_slot] = (void *)service_get_HighestVersion;

	ITaskFolder_table[ITaskFolder_get_Name_slot] = (void *)folder_get_Name;
	ITaskFolder_table[ITaskFolder_get_Path_slot] = (void *)folder_get_Path;
	ITaskFolder_table[ITaskFolder_GetFolder_slot] = (void *)folder_GetFolder;
	ITaskFolder_table[ITaskFolder_GetFolders_slot] = (void *)folder_GetFolders;
	ITaskFolder_table[ITaskFolder_CreateFolder_slot] = (void *)folder_CreateFolder;
	ITaskFolder_table[ITaskFolder_DeleteFolder_slot] = (void *)folder_DeleteFolder;
	ITaskFolder_table[ITaskFolder_GetTask_slot] = (void *)folder_GetTask;
	ITaskFolder_table[ITaskFolder_GetTasks_slot] = (void *)folder_GetTasks;
	ITaskFolder_table[ITaskFolder_DeleteTask_slot] = (void *)folder_DeleteTask;
	ITaskFolder_table[ITaskFolder_RegisterTask_slot] = (void *)folder_RegisterTask;
	ITaskFolder_table[ITaskFolder_RegisterTaskDefinition_slot] = (void *)folder_RegisterTaskDefinition;

	ITaskFolderCollection_table[ITaskFolderCollection_get_Count_slot] = (void *)list_get_Count;
	ITaskFolderCollection_table[ITaskFolderCollection_get_Item_slot] = (void *)list_get_ItemByVariant;
	IRegisteredTaskCollection_table[IRegisteredTaskCollection_get_Count_slot] = (void *)list_get_Count;
	IRegisteredTaskCollection_table[IRegisteredTaskCollection_get_Item_slot] = (void *)list_get_ItemByVariant;
	IRunningTaskCollection_table[IRunningTaskCollection_get_Count_slot] = (void *)list_get_Count;
	IRunningTaskCollection_table[IRunningTaskCollection_get_Item_slot] = (void *)list_get_ItemByVariant;

	BIND(ITaskDefinition, definition, RegistrationInfo);
	BIND(ITaskDefinition, definition, Triggers);
	BIND(ITaskDefinition, definition, Settings);
	BIND(ITaskDefinition, definition, Data);
	BIND(ITaskDefinition, definition, Principal);
	BIND(ITaskDefinition, definition, Actions);
	BIND(ITaskDefinition, definition, XmlText);

	BIND(IRegistrationInfo, registration, Description);
	BIND(IRegistrationInfo, registration, Author);
	BIND(IRegistrationInfo, registration, Version);
	BIND(IRegistrationInfo, registration, Date);
	BIND(IRegistrationInfo, registration, Documentation);
	BIND(IRegistrationInfo, registration, URI);
	BIND(IRegistrationInfo, registration, Source);

	ITriggerCollection_table[ITriggerCollection_get_Count_slot] = (void *)list_get_Count;
	ITriggerCollection_table[ITriggerCollection_get_Item_slot] = (void *)list_get_Item;
	ITriggerCollection_table[ITriggerCollection_Create_slot] = (void *)triggers_Create;
	ITriggerCollection_table[ITriggerCollection_Remove_slot] = (void *)list_Remove;
	ITriggerCollection_table[ITriggerCollection_Clear_slot] = (void *)list_Clear;
	bind_trigger(ITrigger_table);
	bind_trigger(ITimeTrigger_table);
	BIND(ITimeTrigger, trigger, RandomDelay);
	bind_trigger(IDailyTrigger_table);
	BIND(IDailyTrigger, trigger, DaysInterval);
	BIND(IDailyTrigger, trigger, RandomDelay);
	bind_trigger(ILogonTrigger_table);
	BIND(ILogonTrigger, trigger, Delay);
	BIND(ILogonTrigger, trigger, UserId);
	bind_trigger(IBootTrigger_table);
	BIND(IBootTrigger, trigger, Delay);
	bind_trigger(IRegistrationTrigger_table);
	BIND(IRegistrationTrigger, trigger, Delay);
	BIND(IRepetitionPattern, repetition, Interval);
	BIND(IRepetitionPattern, repetition, Duration);
	BIND(IRepetitionPattern, repetition, StopAtDurationEnd);

	BIND(ITaskSettings, settings, AllowDemandStart);
	BIND(ITaskSettings, settings, RestartInterval);
	BIND(ITaskSettings, settings, RestartCount);
	BIND(ITaskSettings, settings, MultipleInstances);
	BIND(ITaskSettings, settings, StopIfGoingOnBatteries);
	BIND(ITaskSettings, settings, DisallowStartIfOnBatteries);
	BIND(ITaskSettings, settings, AllowHardTerminate);
	BIND(ITaskSettings, settings, StartWhenAvailable);
	BIND(ITaskSettings, settings, RunOnlyIfNetworkAvailable);
	BIND(ITaskSettings, settings, ExecutionTimeLimit);
	BIND(ITaskSettings, settings, Enabled);
	BIND(ITaskSettings, settings, DeleteExpiredTaskAfter);
	BIND(ITaskSettings, settings, Priority);
	BIND(ITaskSettings, settings, Compatibility);
	BIND(ITaskSettings, settings, Hidden);
	BIND(ITaskSettings, settings, IdleSettings);
	BIND(ITaskSettings, settings, RunOnlyIfIdle);
	BIND(ITaskSettings, settings, WakeToRun);
	BIND(ITaskSettings, settings, NetworkSettings);
	BIND(IIdleSettings, idle, IdleDuration);
	BIND(IIdleSettings, idle, WaitTimeout);
	BIND(IIdleSettings, idle, StopOnIdleEnd);
	BIND(IIdleSettings, idle, RestartOnIdle);
	BIND(INetworkSettings, network, Name);
	BIND(INetworkSettings, network, Id);

	BIND(IPrincipal, principal, Id);
	BIND(IPrincipal, principal, DisplayName);
	BIND(IPrincipal, principal, UserId);
	BIND(IPrincipal, principal, LogonType);
	BIND(IPrincipal, principal, GroupId);
	BIND(IPrincipal, principal, RunLevel);

	IActionCollection_table[IActionCollection_get_Count_slot] = (void *)list_get_Count;
	IActionCollection_table[IActionCollection_get_Item_slot] = (void *)list_get_Item;
	IActionCollection_table[IActionCollection_Create_slot] = (void *)actions_Create;
	IActionCollection_table[IActionCollection_Remove_slot] = (void *)list_Remove;
	IActionCollection_table[IActionCollection_Clear_slot] = (void *)list_Clear;
	BIND(IActionCollection, list, Context);
	IExecAction_table[IExecAction_get_Type_slot] = (void *)exec_get_Type;
	BIND(IExecAction, exec, Id);
	BIND(IExecAction, exec, Path);
	BIND(IExecAction, exec, Arguments);
	BIND(IExecAction, exec, WorkingDirectory);

	IRegisteredTask_table[IRegisteredTask_get_Name_slot] = (void *)registered_get_Name;
	IRegisteredTask_table[IRegisteredTask_get_Path_slot] = (void *)registered_get_Path;
	IRegisteredTask_table[IRegisteredTask_get_State_slot] = (void *)registered_get_State;
	IRegisteredTask_table[IRegisteredTask_get_Enabled_slot] = (void *)registered_get_Enabled;
	IRegisteredTask_table[IRegisteredTask_put_Enabled_slot] = (void *)registered_put_Enabled;
	IRegisteredTask_table[IRegisteredTask_Run_slot] = (void *)registered_Run;
	IRegisteredTask_table[IRegisteredTask_RunEx_slot] = (void *)registered_RunEx;
	IRegisteredTask_table[IRegisteredTask_GetInstances_slot] = (void *)registered_GetInstances;
	IRegisteredTask_table[IRegisteredTask_get_LastRunTime_slot] = (void *)registered_get_LastRunTime;
	IRegisteredTask_table[IRegisteredTask_get_LastTaskResult_slot] = (void *)registered_get_LastTaskResult;
	IRegisteredTask_table[IRegisteredTask_get_NumberOfMissedRuns_slot] = (void *)registered_get_NumberOfMissedRuns;
	IRegisteredTask_table[IRegisteredTask_get_NextRunTime_slot] = (void *)registered_get_NextRunTime;
	IRegisteredTask_table[IRegisteredTask_get_Definition_slot] = (void *)registered_get_Definition;
	IRegisteredTask_table[IRegisteredTask_get_Xml_slot] = (void *)registered_get_Xml;
	IRegisteredTask_table[IRegisteredTask_Stop_slot] = (void *)registered_Stop;

	IRunningTask_table[IRunningTask_get_Name_slot] = (void *)running_get_Name;
	IRunningTask_table[IRunningTask_get_InstanceGuid_slot] = (void *)running_get_InstanceGuid;
	IRunningTask_table[IRunningTask_get_Path_slot] = (void *)running_get_Path;
	IRunningTask_table[IRunningTask_get_State_slot] = (void *)running_get_State;
	IRunningTask_table[IRunningTask_get_CurrentAction_slot] = (void *)running_get_CurrentAction;
	IRunningTask_table[IRunningTask_Stop_slot] = (void *)running_Stop;
	IRunningTask_table[IRunningTask_Refresh_slot] = (void *)running_Refresh;
	IRunningTask_table[IRunningTask_get_EnginePID_slot] = (void *)running_get_EnginePID;
}

/* ---- class factory ---- */
struct class_factory {
	IClassFactory iface;
	LONG refs;
};

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IClassFactory)) {
		IClassFactory_AddRef(iface);
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE factory_AddRef(IClassFactory *iface) { return (ULONG)InterlockedIncrement(&((struct class_factory *)iface)->refs); }

static ULONG STDMETHODCALLTYPE factory_Release(IClassFactory *iface)
{
	struct class_factory *self = (struct class_factory *)iface;
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs)
		FREE(self);
	return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	struct service *service;
	HRESULT result;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	service = object_new(sizeof(struct service), ITaskService_table, NULL, &IID_ITaskService);
	if (!service)
		return E_OUTOFMEMORY;
	result = object_QueryInterface(service, riid, out);
	object_Release(service);
	return result;
}

static HRESULT STDMETHODCALLTYPE factory_LockServer(IClassFactory *iface, BOOL lock)
{
	return S_OK;
}

static IClassFactoryVtbl factory_table = {factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	struct class_factory *factory;
	HRESULT result;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!same_guid(clsid, &CLSID_TaskScheduler))
		return CLASS_E_CLASSNOTAVAILABLE;
	factory = ALLOC(sizeof(*factory));
	if (!factory)
		return E_OUTOFMEMORY;
	factory->iface.lpVtbl = &factory_table;
	factory->refs = 1;
	result = IClassFactory_QueryInterface(&factory->iface, riid, out);
	IClassFactory_Release(&factory->iface);
	return result;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		prepare_tables();
	return TRUE;
}
