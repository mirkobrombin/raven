/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven Windows Animation component: the animation manager, its variables and storyboards, the library of transitions and the timer
 * that programs such as Paint.NET drive their interface animations with.
 *
 * The objects are COM classes written here and handed out by DllGetClassObject. A storyboard runs its transitions one after another for
 * each variable from the moment it is scheduled; the manager's Update works out every variable at a given time and tells the change
 * handlers; the timer calls the manager about sixty times a second from a thread of its own while the manager is busy. Every interface
 * starts from a table of stubs that name the method they stand for (uianimation_tables.h). */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <uianimation.h>
#include <stddef.h>
#include <string.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define GROW(block, size) ((block) ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (block), (size)) : ALLOC(size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))
#define CONTAINER(pointer, type, field) ((type *)((char *)(pointer) - offsetof(type, field)))

__declspec(dllimport) void WINAPI RavenTrace(const char *text);

static HRESULT unimplemented(const char *interface_name, const char *method)
{
	char text[160];
	size_t at = 0;
	const char *part[] = {"uianimation: ", interface_name, "::", method, " is not implemented"};
	size_t index, length;
	for (index = 0; index < 5; ++index)
		for (length = 0; part[index][length] && at < sizeof(text) - 1; ++length)
			text[at++] = part[index][length];
	text[at] = 0;
	RavenTrace(text);
	return E_NOTIMPL;
}

#include "uianimation_tables.h"

/* The animation errors winerror.h defines, which the MinGW headers leave out. */
#ifndef UI_E_VALUE_NOT_SET
#define UI_E_VALUE_NOT_SET ((HRESULT)0x80510005)
#define UI_E_VALUE_NOT_DETERMINED ((HRESULT)0x80510006)
#define UI_E_STORYBOARD_ACTIVE ((HRESULT)0x80510101)
#endif

static BOOL same_guid(const GUID *a, const GUID *b)
{
	return memcmp(a, b, sizeof(GUID)) == 0;
}

static void **make_table(void *const *stubs, size_t count)
{
	void **table = ALLOC(count * sizeof(void *));
	memcpy(table, stubs, count * sizeof(void *));
	return table;
}

/* ---- arithmetic without a C runtime ---- */
#define PI 3.14159265358979323846

static double dsqrt(double x)
{
	return x > 0 ? __builtin_sqrt(x) : 0;
}

static double dabs(double x)
{
	return x < 0 ? -x : x;
}

static double dsin(double x)
{
	double x2;
	while (x > PI) x -= 2 * PI;
	while (x < -PI) x += 2 * PI;
	if (x > PI / 2) x = PI - x;
	else if (x < -PI / 2) x = -PI - x;
	x2 = x * x;
	return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}

static double now_seconds(void)
{
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (double)counter.QuadPart / (double)frequency.QuadPart;
}

/* ---- objects: a table, a count and how to end ---- */
struct object {
	void **vtbl;
	LONG refs;
	void (*destroy)(struct object *self);
	const GUID *iid;
};

static HRESULT STDMETHODCALLTYPE object_QueryInterface(void *iface, REFIID riid, void **out)
{
	struct object *self = iface;
	if (same_guid(riid, &IID_IUnknown) || (self->iid && same_guid(riid, self->iid))) {
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

static void set_unknown(void **table)
{
	table[0] = (void *)object_QueryInterface;
	table[1] = (void *)object_AddRef;
	table[2] = (void *)object_Release;
}

static void object_init(struct object *self, void **table, void (*destroy)(struct object *), const GUID *iid)
{
	self->vtbl = table;
	self->refs = 1;
	self->destroy = destroy;
	self->iid = iid;
}

/* ---- transitions ---- */
enum { T_INSTANT, T_CONSTANT, T_DISCRETE, T_LINEAR, T_LINEAR_SPEED, T_SINE_VELOCITY, T_SINE_RANGE, T_ACCEL_DECEL, T_REVERSAL, T_CUBIC,
	T_SMOOTH_STOP, T_PARABOLIC, T_CUSTOM };

struct transition {
	struct object base;
	int kind;
	double duration, final, a, b, c, d; /* the parameters the kind takes */
	double initial, velocity;
	BOOL has_initial, has_velocity;
	IUIAnimationInterpolator *interpolator; /* a transition the program describes */
};

static void transition_destroy(struct object *base)
{
	struct transition *self = (struct transition *)base;
	if (self->interpolator)
		IUIAnimationInterpolator_Release(self->interpolator);
}

/* Where a transition puts its variable `t` seconds after it starts from `start` with `velocity`. */
static double transition_value(const struct transition *self, double start, double velocity, double t, double duration)
{
	const double f = self->final;
	double u;
	if (self->has_initial)
		start = self->initial;
	if (self->has_velocity)
		velocity = self->velocity;
	if (self->kind == T_CUSTOM) {
		double value = start;
		IUIAnimationInterpolator_InterpolateValue(self->interpolator, t < duration ? t : duration, &value);
		return value;
	}
	if (t >= duration && self->kind != T_SINE_VELOCITY && self->kind != T_SINE_RANGE && self->kind != T_CONSTANT && self->kind != T_REVERSAL)
		return self->kind == T_CONSTANT ? start : f;
	u = duration > 0 ? t / duration : 1;
	if (u > 1) u = 1;
	switch (self->kind) {
	case T_INSTANT: return f;
	case T_CONSTANT: return start;
	case T_DISCRETE: return t < self->a ? start : f;
	case T_LINEAR:
	case T_LINEAR_SPEED: return start + (f - start) * u;
	case T_SINE_VELOCITY: return start + velocity * self->a / (2 * PI) * dsin(2 * PI * t / self->a);
	case T_SINE_RANGE: {
		/* Between the minimum and the maximum, starting from where the variable is. */
		const double middle = (self->a + self->b) / 2, amplitude = (self->b - self->a) / 2;
		return middle + amplitude * dsin(2 * PI * t / self->c + (self->d == 0 ? PI / 2 : -PI / 2));
	}
	case T_ACCEL_DECEL: {
		/* Constant acceleration for a share of the time, constant speed, then constant deceleration: the speed in the middle is what
		 * covers the distance. */
		const double ra = self->a, rd = self->b, peak = 1 / (1 - ra / 2 - rd / 2);
		double s;
		if (u < ra) s = peak * u * u / (2 * ra);
		else if (u <= 1 - rd) s = peak * (u - ra / 2);
		else s = 1 - peak * (1 - u) * (1 - u) / (2 * rd);
		return start + (f - start) * s;
	}
	case T_REVERSAL: return start + velocity * t * (1 - t / (duration > 0 ? duration : 1));
	case T_CUBIC:
	case T_SMOOTH_STOP: {
		/* Hermite curve from the start value and speed to the final value and speed. */
		const double v1 = self->kind == T_CUBIC ? self->a : 0, d = duration > 0 ? duration : 1;
		const double h00 = 2 * u * u * u - 3 * u * u + 1, h10 = u * u * u - 2 * u * u + u, h01 = -2 * u * u * u + 3 * u * u, h11 = u * u * u - u * u;
		return h00 * start + h10 * d * velocity + h01 * f + h11 * d * v1;
	}
	case T_PARABOLIC: return start + (f - start) * u * u;
	}
	return f;
}

/* The length of a transition from a start value and speed. */
static double transition_duration(const struct transition *self, double start, double velocity)
{
	if (self->has_initial)
		start = self->initial;
	if (self->has_velocity)
		velocity = self->velocity;
	switch (self->kind) {
	case T_CUSTOM: {
		/* The interpolator learns where it starts from, then says how long it takes and where it ends. */
		double duration = 0;
		IUIAnimationInterpolator_SetInitialValueAndVelocity(self->interpolator, start, velocity);
		IUIAnimationInterpolator_GetDuration(self->interpolator, &duration);
		return duration;
	}
	case T_INSTANT: return 0;
	case T_LINEAR_SPEED: return self->a > 0 ? dabs(self->final - start) / self->a : 0;
	case T_SMOOTH_STOP: {
		const double needed = velocity != 0 ? 2 * dabs(self->final - start) / dabs(velocity) : self->a;
		return needed < self->a ? needed : self->a;
	}
	case T_PARABOLIC: return self->a > 0 ? dsqrt(2 * dabs(self->final - start) / self->a) : 0;
	default: return self->duration;
	}
}

static HRESULT STDMETHODCALLTYPE transition_SetInitialValue(void *iface, double value)
{
	struct transition *self = iface;
	self->initial = value;
	self->has_initial = TRUE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE transition_SetInitialVelocity(void *iface, double velocity)
{
	struct transition *self = iface;
	self->velocity = velocity;
	self->has_velocity = TRUE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE transition_IsDurationKnown(void *iface)
{
	const int kind = ((struct transition *)iface)->kind;
	return kind == T_LINEAR_SPEED || kind == T_SMOOTH_STOP || kind == T_PARABOLIC ? S_FALSE : S_OK;
}
static HRESULT STDMETHODCALLTYPE transition_GetDuration(void *iface, UI_ANIMATION_SECONDS *duration)
{
	struct transition *self = iface;
	if (transition_IsDurationKnown(iface) != S_OK)
		return UI_E_VALUE_NOT_DETERMINED;
	*duration = transition_duration(self, 0, 0);
	return S_OK;
}

static void **transition_table;

static HRESULT make_transition(int kind, double duration, double final, double a, double b, double c, double d, IUIAnimationTransition **out)
{
	struct transition *self = ALLOC(sizeof(*self));
	if (!out)
		return E_POINTER;
	if (!self)
		return E_OUTOFMEMORY;
	object_init(&self->base, transition_table, transition_destroy, &IID_IUIAnimationTransition);
	self->kind = kind;
	self->duration = duration;
	self->final = final;
	self->a = a;
	self->b = b;
	self->c = c;
	self->d = d;
	*out = (IUIAnimationTransition *)self;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE library_Instantaneous(void *iface, double final, IUIAnimationTransition **out) { return make_transition(T_INSTANT, 0, final, 0, 0, 0, 0, out); }
static HRESULT STDMETHODCALLTYPE library_Constant(void *iface, double duration, IUIAnimationTransition **out) { return make_transition(T_CONSTANT, duration, 0, 0, 0, 0, 0, out); }
static HRESULT STDMETHODCALLTYPE library_Discrete(void *iface, double delay, double final, double hold, IUIAnimationTransition **out)
{
	return make_transition(T_DISCRETE, delay + hold, final, delay, hold, 0, 0, out);
}
static HRESULT STDMETHODCALLTYPE library_Linear(void *iface, double duration, double final, IUIAnimationTransition **out)
{
	return make_transition(T_LINEAR, duration, final, 0, 0, 0, 0, out);
}
static HRESULT STDMETHODCALLTYPE library_LinearFromSpeed(void *iface, double speed, double final, IUIAnimationTransition **out)
{
	return make_transition(T_LINEAR_SPEED, 0, final, speed, 0, 0, 0, out);
}
static HRESULT STDMETHODCALLTYPE library_SineFromVelocity(void *iface, double duration, double period, IUIAnimationTransition **out)
{
	return make_transition(T_SINE_VELOCITY, duration, 0, period > 0 ? period : 1, 0, 0, 0, out);
}
static HRESULT STDMETHODCALLTYPE library_SineFromRange(void *iface, double duration, double minimum, double maximum, double period,
	UI_ANIMATION_SLOPE slope, IUIAnimationTransition **out)
{
	return make_transition(T_SINE_RANGE, duration, 0, minimum, maximum, period > 0 ? period : 1, slope, out);
}
static HRESULT STDMETHODCALLTYPE library_AccelerateDecelerate(void *iface, double duration, double final, double acceleration, double deceleration,
	IUIAnimationTransition **out)
{
	if (acceleration < 0 || deceleration < 0 || acceleration + deceleration > 1)
		return E_INVALIDARG;
	return make_transition(T_ACCEL_DECEL, duration, final, acceleration > 0 ? acceleration : 1e-6, deceleration > 0 ? deceleration : 1e-6, 0, 0, out);
}
static HRESULT STDMETHODCALLTYPE library_Reversal(void *iface, double duration, IUIAnimationTransition **out) { return make_transition(T_REVERSAL, duration, 0, 0, 0, 0, 0, out); }
static HRESULT STDMETHODCALLTYPE library_Cubic(void *iface, double duration, double final, double final_velocity, IUIAnimationTransition **out)
{
	return make_transition(T_CUBIC, duration, final, final_velocity, 0, 0, 0, out);
}
static HRESULT STDMETHODCALLTYPE library_SmoothStop(void *iface, double maximum, double final, IUIAnimationTransition **out)
{
	return make_transition(T_SMOOTH_STOP, maximum, final, maximum, 0, 0, 0, out);
}
static HRESULT STDMETHODCALLTYPE library_Parabolic(void *iface, double final, double final_velocity, double acceleration, IUIAnimationTransition **out)
{
	return make_transition(T_PARABOLIC, 0, final, acceleration > 0 ? acceleration : 1, final_velocity, 0, 0, out);
}

static void **library_table;

/* The transition factory: a transition that a program's interpolator describes. */
static HRESULT STDMETHODCALLTYPE transition_factory_CreateTransition(void *iface, IUIAnimationInterpolator *interpolator, IUIAnimationTransition **out)
{
	struct transition *self;
	double final = 0;
	HRESULT hr;
	if (!interpolator)
		return E_POINTER;
	if (FAILED(hr = make_transition(T_CUSTOM, 0, 0, 0, 0, 0, 0, out)))
		return hr;
	self = (struct transition *)*out;
	self->interpolator = interpolator;
	IUIAnimationInterpolator_AddRef(interpolator);
	IUIAnimationInterpolator_SetInitialValueAndVelocity(interpolator, 0, 0);
	if (SUCCEEDED(IUIAnimationInterpolator_GetFinalValue(interpolator, &final)))
		self->final = final;
	return S_OK;
}

static void **transition_factory_table;

/* ---- variables ---- */
struct storyboard;

struct variable {
	struct object base;
	double value, previous, final;
	double lower, upper;
	BOOL has_lower, has_upper;
	UI_ANIMATION_ROUNDING_MODE rounding;
	IUnknown *tag_object;
	UINT32 tag_id;
	IUIAnimationVariableChangeHandler *handler;
	IUIAnimationVariableIntegerChangeHandler *integer_handler;
	struct storyboard *storyboard;
	struct manager *manager;
};

static INT32 rounded(const struct variable *self, double value)
{
	switch (self->rounding) {
	case UI_ANIMATION_ROUNDING_FLOOR: return (INT32)(value < 0 && (double)(INT32)value != value ? (INT32)value - 1 : (INT32)value);
	case UI_ANIMATION_ROUNDING_CEILING: return (INT32)(value > 0 && (double)(INT32)value != value ? (INT32)value + 1 : (INT32)value);
	default: return (INT32)(value < 0 ? value - 0.5 : value + 0.5);
	}
}

static double bounded(const struct variable *self, double value)
{
	if (self->has_lower && value < self->lower) value = self->lower;
	if (self->has_upper && value > self->upper) value = self->upper;
	return value;
}

static void variable_destroy(struct object *base)
{
	struct variable *self = (struct variable *)base;
	if (self->handler)
		IUIAnimationVariableChangeHandler_Release(self->handler);
	if (self->integer_handler)
		IUIAnimationVariableIntegerChangeHandler_Release(self->integer_handler);
	if (self->tag_object)
		IUnknown_Release(self->tag_object);
}

static HRESULT STDMETHODCALLTYPE variable_GetValue(void *iface, double *value) { *value = ((struct variable *)iface)->value; return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_GetFinalValue(void *iface, double *value) { *value = ((struct variable *)iface)->final; return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_GetPreviousValue(void *iface, double *value) { *value = ((struct variable *)iface)->previous; return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_GetIntegerValue(void *iface, INT32 *value) { *value = rounded(iface, ((struct variable *)iface)->value); return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_GetFinalIntegerValue(void *iface, INT32 *value) { *value = rounded(iface, ((struct variable *)iface)->final); return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_GetPreviousIntegerValue(void *iface, INT32 *value) { *value = rounded(iface, ((struct variable *)iface)->previous); return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_GetCurrentStoryboard(void *iface, IUIAnimationStoryboard **storyboard)
{
	struct variable *self = iface;
	*storyboard = (IUIAnimationStoryboard *)self->storyboard;
	if (self->storyboard)
		object_AddRef(self->storyboard);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE variable_SetLowerBound(void *iface, double bound) { ((struct variable *)iface)->lower = bound; ((struct variable *)iface)->has_lower = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_SetUpperBound(void *iface, double bound) { ((struct variable *)iface)->upper = bound; ((struct variable *)iface)->has_upper = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_SetRoundingMode(void *iface, UI_ANIMATION_ROUNDING_MODE mode) { ((struct variable *)iface)->rounding = mode; return S_OK; }
static HRESULT STDMETHODCALLTYPE variable_SetTag(void *iface, IUnknown *object, UINT32 id)
{
	struct variable *self = iface;
	if (object)
		IUnknown_AddRef(object);
	if (self->tag_object)
		IUnknown_Release(self->tag_object);
	self->tag_object = object;
	self->tag_id = id;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE variable_GetTag(void *iface, IUnknown **object, UINT32 *id)
{
	struct variable *self = iface;
	if (object) {
		*object = self->tag_object;
		if (self->tag_object)
			IUnknown_AddRef(self->tag_object);
	}
	if (id)
		*id = self->tag_id;
	return self->tag_object || self->tag_id ? S_OK : UI_E_VALUE_NOT_SET;
}
static HRESULT STDMETHODCALLTYPE variable_SetVariableChangeHandler(void *iface, IUIAnimationVariableChangeHandler *handler)
{
	struct variable *self = iface;
	if (handler)
		IUIAnimationVariableChangeHandler_AddRef(handler);
	if (self->handler)
		IUIAnimationVariableChangeHandler_Release(self->handler);
	self->handler = handler;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE variable_SetVariableIntegerChangeHandler(void *iface, IUIAnimationVariableIntegerChangeHandler *handler)
{
	struct variable *self = iface;
	if (handler)
		IUIAnimationVariableIntegerChangeHandler_AddRef(handler);
	if (self->integer_handler)
		IUIAnimationVariableIntegerChangeHandler_Release(self->integer_handler);
	self->integer_handler = handler;
	return S_OK;
}

static void **variable_table;

/* ---- storyboards ---- */
struct segment {
	struct variable *variable;
	struct transition *transition;
	double begin, length;      /* seconds from the start of the storyboard */
	double start, velocity;    /* the variable's value and speed when the segment begins, known once it does */
	BOOL started;
	UINT32 keyframe_after;     /* the keyframe that marks this segment's end, if any */
};

struct storyboard {
	struct object base;
	struct manager *manager;
	struct segment *segments;
	UINT32 count, capacity;
	double *keyframes;         /* keyframe times from the start, known when the segments before them are */
	UINT32 keyframe_count;
	UI_ANIMATION_STORYBOARD_STATUS status;
	double scheduled;
	IUnknown *tag_object;
	UINT32 tag_id;
	IUIAnimationStoryboardEventHandler *handler;
	BOOL active;
};

struct manager {
	void **vtbl;
	void **update_vtbl;
	LONG refs;
	CRITICAL_SECTION lock;
	struct storyboard **active;
	UINT32 active_count, active_capacity;
	struct variable **variables;
	UINT32 variable_count, variable_capacity;
	UI_ANIMATION_MANAGER_STATUS status;
	UI_ANIMATION_MODE mode;
	BOOL paused;
	IUIAnimationManagerEventHandler *handler;
	IUIAnimationTimerClientEventHandler *client;
	double last_time;
};

static void storyboard_destroy(struct object *base)
{
	struct storyboard *self = (struct storyboard *)base;
	UINT32 index;
	for (index = 0; index < self->count; ++index) {
		object_Release(self->segments[index].variable);
		object_Release(self->segments[index].transition);
	}
	FREE(self->segments);
	FREE(self->keyframes);
	if (self->handler)
		IUIAnimationStoryboardEventHandler_Release(self->handler);
	if (self->tag_object)
		IUnknown_Release(self->tag_object);
}

/* When the last segment already in the storyboard for a variable ends; 0 when the variable has none yet. */
static double variable_end(const struct storyboard *self, const struct variable *variable)
{
	double end = 0;
	UINT32 index;
	for (index = 0; index < self->count; ++index)
		if (self->segments[index].variable == variable && self->segments[index].begin + self->segments[index].length > end)
			end = self->segments[index].begin + self->segments[index].length;
	return end;
}

static HRESULT add_segment(struct storyboard *self, IUIAnimationVariable *variable, IUIAnimationTransition *transition, double begin)
{
	struct segment *segment;
	if (!variable || !transition)
		return E_POINTER;
	if (self->status != UI_ANIMATION_STORYBOARD_BUILDING)
		return UI_E_STORYBOARD_ACTIVE;
	if (self->count == self->capacity) {
		const UINT32 capacity = self->capacity ? self->capacity * 2 : 8;
		struct segment *grown = GROW(self->segments, capacity * sizeof(*grown));
		if (!grown)
			return E_OUTOFMEMORY;
		self->segments = grown;
		self->capacity = capacity;
	}
	segment = &self->segments[self->count++];
	memset(segment, 0, sizeof(*segment));
	segment->variable = (struct variable *)variable;
	segment->transition = (struct transition *)transition;
	object_AddRef(variable);
	object_AddRef(transition);
	segment->begin = begin;
	/* A duration that depends on the start value is known only when the segment begins; until then it counts as its estimate. */
	segment->length = transition_duration(segment->transition, ((struct variable *)variable)->final, 0);
	((struct variable *)variable)->final = segment->transition->kind == T_CONSTANT || segment->transition->kind == T_SINE_VELOCITY ||
		segment->transition->kind == T_SINE_RANGE || segment->transition->kind == T_REVERSAL ? ((struct variable *)variable)->final :
		segment->transition->final;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE storyboard_AddTransition(void *iface, IUIAnimationVariable *variable, IUIAnimationTransition *transition)
{
	struct storyboard *self = iface;
	return add_segment(self, variable, transition, variable_end(self, (struct variable *)variable));
}

/* Keyframes: a token that stands for a time in the storyboard. */
static UI_ANIMATION_KEYFRAME new_keyframe(struct storyboard *self, double time)
{
	double *grown = GROW(self->keyframes, (self->keyframe_count + 1) * sizeof(double));
	if (!grown)
		return NULL;
	self->keyframes = grown;
	self->keyframes[self->keyframe_count++] = time;
	return (UI_ANIMATION_KEYFRAME)(UINT_PTR)self->keyframe_count;
}

static double keyframe_time(const struct storyboard *self, UI_ANIMATION_KEYFRAME keyframe)
{
	const UINT_PTR index = (UINT_PTR)keyframe;
	if (keyframe == UI_ANIMATION_KEYFRAME_STORYBOARD_START || !index || index > self->keyframe_count)
		return 0;
	return self->keyframes[index - 1];
}

static HRESULT STDMETHODCALLTYPE storyboard_AddKeyframeAtOffset(void *iface, UI_ANIMATION_KEYFRAME existing, UI_ANIMATION_SECONDS offset,
	UI_ANIMATION_KEYFRAME *keyframe)
{
	struct storyboard *self = iface;
	*keyframe = new_keyframe(self, keyframe_time(self, existing) + offset);
	return *keyframe ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE storyboard_AddKeyframeAfterTransition(void *iface, IUIAnimationTransition *transition, UI_ANIMATION_KEYFRAME *keyframe)
{
	struct storyboard *self = iface;
	UINT32 index;
	for (index = 0; index < self->count; ++index)
		if (self->segments[index].transition == (struct transition *)transition) {
			*keyframe = new_keyframe(self, self->segments[index].begin + self->segments[index].length);
			return *keyframe ? S_OK : E_OUTOFMEMORY;
		}
	return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE storyboard_AddTransitionAtKeyframe(void *iface, IUIAnimationVariable *variable, IUIAnimationTransition *transition,
	UI_ANIMATION_KEYFRAME start)
{
	struct storyboard *self = iface;
	const double at = keyframe_time(self, start), end = variable_end(self, (struct variable *)variable);
	return add_segment(self, variable, transition, at > end ? at : end);
}

static HRESULT STDMETHODCALLTYPE storyboard_AddTransitionBetweenKeyframes(void *iface, IUIAnimationVariable *variable, IUIAnimationTransition *transition,
	UI_ANIMATION_KEYFRAME start, UI_ANIMATION_KEYFRAME end)
{
	struct storyboard *self = iface;
	HRESULT hr = storyboard_AddTransitionAtKeyframe(iface, variable, transition, start);
	if (SUCCEEDED(hr)) {
		struct segment *segment = &self->segments[self->count - 1];
		const double length = keyframe_time(self, end) - segment->begin;
		segment->length = length > 0 ? length : 0;
		segment->transition->duration = segment->length;
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE storyboard_RepeatBetweenKeyframes(void *iface, UI_ANIMATION_KEYFRAME start, UI_ANIMATION_KEYFRAME end, INT32 count)
{
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE storyboard_HoldVariable(void *iface, IUIAnimationVariable *variable) { return S_OK; }
static HRESULT STDMETHODCALLTYPE storyboard_SetLongestAcceptableDelay(void *iface, UI_ANIMATION_SECONDS delay) { return S_OK; }

static void set_status(struct storyboard *self, UI_ANIMATION_STORYBOARD_STATUS status)
{
	const UI_ANIMATION_STORYBOARD_STATUS previous = self->status;
	if (previous == status)
		return;
	self->status = status;
	if (self->handler)
		IUIAnimationStoryboardEventHandler_OnStoryboardStatusChanged(self->handler, (IUIAnimationStoryboard *)self, status, previous);
}

static void manager_activate(struct manager *manager, struct storyboard *storyboard);

static HRESULT STDMETHODCALLTYPE storyboard_Schedule(void *iface, UI_ANIMATION_SECONDS time, UI_ANIMATION_SCHEDULING_RESULT *result)
{
	struct storyboard *self = iface;
	UINT32 index;
	if (self->status != UI_ANIMATION_STORYBOARD_BUILDING) {
		if (result)
			*result = UI_ANIMATION_SCHEDULING_ALREADY_SCHEDULED;
		return S_OK;
	}
	self->scheduled = time;
	/* A variable belongs to the newest storyboard that animates it. */
	for (index = 0; index < self->count; ++index)
		self->segments[index].variable->storyboard = self;
	set_status(self, UI_ANIMATION_STORYBOARD_SCHEDULED);
	manager_activate(self->manager, self);
	if (result)
		*result = UI_ANIMATION_SCHEDULING_SUCCEEDED;
	return S_OK;
}

static void finish_storyboard(struct storyboard *self, UI_ANIMATION_STORYBOARD_STATUS status, BOOL jump);

static HRESULT STDMETHODCALLTYPE storyboard_Conclude(void *iface) { return S_OK; }
static HRESULT STDMETHODCALLTYPE storyboard_Finish(void *iface, UI_ANIMATION_SECONDS completion)
{
	finish_storyboard(iface, UI_ANIMATION_STORYBOARD_FINISHED, TRUE);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE storyboard_Abandon(void *iface)
{
	finish_storyboard(iface, UI_ANIMATION_STORYBOARD_CANCELLED, FALSE);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE storyboard_SetTag(void *iface, IUnknown *object, UINT32 id)
{
	struct storyboard *self = iface;
	if (object)
		IUnknown_AddRef(object);
	if (self->tag_object)
		IUnknown_Release(self->tag_object);
	self->tag_object = object;
	self->tag_id = id;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE storyboard_GetTag(void *iface, IUnknown **object, UINT32 *id)
{
	struct storyboard *self = iface;
	if (object) {
		*object = self->tag_object;
		if (self->tag_object)
			IUnknown_AddRef(self->tag_object);
	}
	if (id)
		*id = self->tag_id;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE storyboard_GetStatus(void *iface, UI_ANIMATION_STORYBOARD_STATUS *status) { *status = ((struct storyboard *)iface)->status; return S_OK; }
static HRESULT STDMETHODCALLTYPE storyboard_GetElapsedTime(void *iface, UI_ANIMATION_SECONDS *elapsed)
{
	struct storyboard *self = iface;
	*elapsed = self->status == UI_ANIMATION_STORYBOARD_BUILDING ? 0 : self->manager->last_time - self->scheduled;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE storyboard_SetStoryboardEventHandler(void *iface, IUIAnimationStoryboardEventHandler *handler)
{
	struct storyboard *self = iface;
	if (handler)
		IUIAnimationStoryboardEventHandler_AddRef(handler);
	if (self->handler)
		IUIAnimationStoryboardEventHandler_Release(self->handler);
	self->handler = handler;
	return S_OK;
}

static void **storyboard_table;

/* ---- the manager ---- */
static void **manager_table, **update_table;

static struct manager *manager_of(void *iface)
{
	return *(void ***)iface == update_table ? CONTAINER(iface, struct manager, update_vtbl) : iface;
}

static void set_manager_status(struct manager *self, UI_ANIMATION_MANAGER_STATUS status)
{
	const UI_ANIMATION_MANAGER_STATUS previous = self->status;
	if (previous == status)
		return;
	self->status = status;
	if (self->client)
		IUIAnimationTimerClientEventHandler_OnTimerClientStatusChanged(self->client,
			status == UI_ANIMATION_MANAGER_BUSY ? UI_ANIMATION_TIMER_CLIENT_BUSY : UI_ANIMATION_TIMER_CLIENT_IDLE,
			previous == UI_ANIMATION_MANAGER_BUSY ? UI_ANIMATION_TIMER_CLIENT_BUSY : UI_ANIMATION_TIMER_CLIENT_IDLE);
	if (self->handler)
		IUIAnimationManagerEventHandler_OnManagerStatusChanged(self->handler, status, previous);
}

static void manager_activate(struct manager *self, struct storyboard *storyboard)
{
	if (!storyboard->active) {
		if (self->active_count == self->active_capacity) {
			const UINT32 capacity = self->active_capacity ? self->active_capacity * 2 : 8;
			struct storyboard **grown = GROW(self->active, capacity * sizeof(*grown));
			if (!grown)
				return;
			self->active = grown;
			self->active_capacity = capacity;
		}
		self->active[self->active_count++] = storyboard;
		object_AddRef(storyboard);
		storyboard->active = TRUE;
	}
	set_manager_status(self, UI_ANIMATION_MANAGER_BUSY);
}

/* A variable takes a new value and its handlers hear of it. */
static BOOL move_variable(struct variable *variable, struct storyboard *storyboard, double value)
{
	const double previous = variable->value;
	value = bounded(variable, value);
	if (value == previous)
		return FALSE;
	variable->previous = previous;
	variable->value = value;
	if (variable->handler)
		IUIAnimationVariableChangeHandler_OnValueChanged(variable->handler, (IUIAnimationStoryboard *)storyboard, (IUIAnimationVariable *)variable,
			value, previous);
	if (variable->integer_handler && rounded(variable, value) != rounded(variable, previous))
		IUIAnimationVariableIntegerChangeHandler_OnIntegerValueChanged(variable->integer_handler, (IUIAnimationStoryboard *)storyboard,
			(IUIAnimationVariable *)variable, rounded(variable, value), rounded(variable, previous));
	return TRUE;
}

/* Every segment of a storyboard at a time: true when something moved; `done` when the storyboard has run its course. */
static BOOL advance(struct storyboard *self, double time, BOOL *done)
{
	const double elapsed = time - self->scheduled;
	BOOL moved = FALSE;
	UINT32 index;
	*done = TRUE;
	for (index = 0; index < self->count; ++index) {
		struct segment *segment = &self->segments[index];
		double t;
		if (segment->variable->storyboard != self)
			continue;
		if (elapsed < segment->begin) {
			*done = FALSE;
			continue;
		}
		if (!segment->started) {
			segment->started = TRUE;
			segment->start = segment->variable->value;
			segment->velocity = 0;
			segment->length = transition_duration(segment->transition, segment->start, 0);
		}
		t = elapsed - segment->begin;
		if (t < segment->length)
			*done = FALSE;
		/* A later segment of the same variable that has begun takes over from this one. */
		{
			UINT32 later;
			BOOL superseded = FALSE;
			for (later = index + 1; later < self->count; ++later)
				if (self->segments[later].variable == segment->variable && elapsed >= self->segments[later].begin)
					superseded = TRUE;
			if (superseded)
				continue;
		}
		moved |= move_variable(segment->variable, self, transition_value(segment->transition, segment->start, segment->velocity,
			t < segment->length ? t : segment->length, segment->length));
	}
	return moved;
}

static void finish_storyboard(struct storyboard *self, UI_ANIMATION_STORYBOARD_STATUS status, BOOL jump)
{
	UINT32 index;
	if (jump)
		for (index = 0; index < self->count; ++index)
			if (self->segments[index].variable->storyboard == self)
				move_variable(self->segments[index].variable, self, self->segments[index].transition->kind == T_CONSTANT ?
					self->segments[index].variable->value : self->segments[index].transition->final);
	for (index = 0; index < self->count; ++index)
		if (self->segments[index].variable->storyboard == self)
			self->segments[index].variable->storyboard = NULL;
	set_status(self, status);
}

static HRESULT update(struct manager *self, double time, UI_ANIMATION_UPDATE_RESULT *result)
{
	BOOL moved = FALSE;
	UINT32 index = 0;
	EnterCriticalSection(&self->lock);
	self->last_time = time;
	if (self->paused) {
		LeaveCriticalSection(&self->lock);
		if (result)
			*result = UI_ANIMATION_UPDATE_NO_CHANGE;
		return S_OK;
	}
	while (index < self->active_count) {
		struct storyboard *storyboard = self->active[index];
		BOOL done = FALSE;
		object_AddRef(storyboard);
		if (storyboard->status == UI_ANIMATION_STORYBOARD_SCHEDULED && time >= storyboard->scheduled)
			set_status(storyboard, UI_ANIMATION_STORYBOARD_PLAYING);
		if (storyboard->status == UI_ANIMATION_STORYBOARD_PLAYING || storyboard->status == UI_ANIMATION_STORYBOARD_SCHEDULED) {
			const BOOL changed = advance(storyboard, time, &done);
			moved |= changed;
			if (changed && storyboard->handler)
				IUIAnimationStoryboardEventHandler_OnStoryboardUpdated(storyboard->handler, (IUIAnimationStoryboard *)storyboard);
		} else {
			done = TRUE;
		}
		if (done) {
			if (storyboard->status == UI_ANIMATION_STORYBOARD_PLAYING || storyboard->status == UI_ANIMATION_STORYBOARD_SCHEDULED)
				finish_storyboard(storyboard, UI_ANIMATION_STORYBOARD_FINISHED, FALSE);
			storyboard->active = FALSE;
			self->active[index] = self->active[--self->active_count];
			object_Release(storyboard);
		} else {
			++index;
		}
		object_Release(storyboard);
	}
	if (!self->active_count)
		set_manager_status(self, UI_ANIMATION_MANAGER_IDLE);
	LeaveCriticalSection(&self->lock);
	if (result)
		*result = moved ? UI_ANIMATION_UPDATE_VARIABLES_CHANGED : UI_ANIMATION_UPDATE_NO_CHANGE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE manager_QueryInterface(void *iface, REFIID riid, void **out)
{
	struct manager *self = manager_of(iface);
	void *found = NULL;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IUIAnimationManager))
		found = self;
	else if (same_guid(riid, &IID_IUIAnimationTimerUpdateHandler))
		found = &self->update_vtbl;
	if (!found) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	*out = found;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE manager_AddRef(void *iface) { return (ULONG)InterlockedIncrement(&manager_of(iface)->refs); }

static ULONG STDMETHODCALLTYPE manager_Release(void *iface)
{
	struct manager *self = manager_of(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		UINT32 index;
		for (index = 0; index < self->active_count; ++index)
			object_Release(self->active[index]);
		FREE(self->active);
		for (index = 0; index < self->variable_count; ++index)
			object_Release(self->variables[index]);
		FREE(self->variables);
		if (self->handler)
			IUIAnimationManagerEventHandler_Release(self->handler);
		if (self->client)
			IUIAnimationTimerClientEventHandler_Release(self->client);
		DeleteCriticalSection(&self->lock);
		FREE(self);
	}
	return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE manager_CreateAnimationVariable(void *iface, double initial, IUIAnimationVariable **out)
{
	struct manager *self = manager_of(iface);
	struct variable *variable = ALLOC(sizeof(*variable));
	if (!variable)
		return E_OUTOFMEMORY;
	object_init(&variable->base, variable_table, variable_destroy, &IID_IUIAnimationVariable);
	variable->value = variable->previous = variable->final = initial;
	variable->manager = self;
	EnterCriticalSection(&self->lock);
	if (self->variable_count == self->variable_capacity) {
		const UINT32 capacity = self->variable_capacity ? self->variable_capacity * 2 : 16;
		struct variable **grown = GROW(self->variables, capacity * sizeof(*grown));
		if (grown) {
			self->variables = grown;
			self->variable_capacity = capacity;
		}
	}
	if (self->variable_count < self->variable_capacity) {
		self->variables[self->variable_count++] = variable;
		object_AddRef(variable);
	}
	LeaveCriticalSection(&self->lock);
	*out = (IUIAnimationVariable *)variable;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE manager_CreateStoryboard(void *iface, IUIAnimationStoryboard **out)
{
	struct storyboard *storyboard = ALLOC(sizeof(*storyboard));
	if (!storyboard)
		return E_OUTOFMEMORY;
	object_init(&storyboard->base, storyboard_table, storyboard_destroy, &IID_IUIAnimationStoryboard);
	storyboard->manager = manager_of(iface);
	*out = (IUIAnimationStoryboard *)storyboard;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE manager_ScheduleTransition(void *iface, IUIAnimationVariable *variable, IUIAnimationTransition *transition,
	UI_ANIMATION_SECONDS time)
{
	IUIAnimationStoryboard *storyboard = NULL;
	HRESULT hr = manager_CreateStoryboard(iface, &storyboard);
	if (FAILED(hr))
		return hr;
	if (SUCCEEDED(hr = storyboard_AddTransition(storyboard, variable, transition)))
		hr = storyboard_Schedule(storyboard, time, NULL);
	object_Release(storyboard);
	return hr;
}

static HRESULT STDMETHODCALLTYPE manager_FinishAllStoryboards(void *iface, UI_ANIMATION_SECONDS completion)
{
	struct manager *self = manager_of(iface);
	UINT32 index;
	EnterCriticalSection(&self->lock);
	for (index = 0; index < self->active_count; ++index)
		finish_storyboard(self->active[index], UI_ANIMATION_STORYBOARD_FINISHED, TRUE);
	LeaveCriticalSection(&self->lock);
	return update(self, self->last_time, NULL);
}

static HRESULT STDMETHODCALLTYPE manager_AbandonAllStoryboards(void *iface)
{
	struct manager *self = manager_of(iface);
	UINT32 index;
	EnterCriticalSection(&self->lock);
	for (index = 0; index < self->active_count; ++index)
		finish_storyboard(self->active[index], UI_ANIMATION_STORYBOARD_CANCELLED, FALSE);
	LeaveCriticalSection(&self->lock);
	return update(self, self->last_time, NULL);
}

static HRESULT STDMETHODCALLTYPE manager_Update(void *iface, UI_ANIMATION_SECONDS time, UI_ANIMATION_UPDATE_RESULT *result)
{
	return update(manager_of(iface), time, result);
}

static HRESULT STDMETHODCALLTYPE manager_GetVariableFromTag(void *iface, IUnknown *object, UINT32 id, IUIAnimationVariable **out)
{
	struct manager *self = manager_of(iface);
	UINT32 index;
	for (index = 0; index < self->variable_count; ++index)
		if (self->variables[index]->tag_object == object && self->variables[index]->tag_id == id) {
			*out = (IUIAnimationVariable *)self->variables[index];
			object_AddRef(*out);
			return S_OK;
		}
	*out = NULL;
	return UI_E_VALUE_NOT_SET;
}

static HRESULT STDMETHODCALLTYPE manager_GetStoryboardFromTag(void *iface, IUnknown *object, UINT32 id, IUIAnimationStoryboard **out)
{
	struct manager *self = manager_of(iface);
	UINT32 index;
	for (index = 0; index < self->active_count; ++index)
		if (self->active[index]->tag_object == object && self->active[index]->tag_id == id) {
			*out = (IUIAnimationStoryboard *)self->active[index];
			object_AddRef(*out);
			return S_OK;
		}
	*out = NULL;
	return UI_E_VALUE_NOT_SET;
}

static HRESULT STDMETHODCALLTYPE manager_GetStatus(void *iface, UI_ANIMATION_MANAGER_STATUS *status) { *status = manager_of(iface)->status; return S_OK; }
static HRESULT STDMETHODCALLTYPE manager_SetAnimationMode(void *iface, UI_ANIMATION_MODE mode) { manager_of(iface)->mode = mode; return S_OK; }
static HRESULT STDMETHODCALLTYPE manager_Pause(void *iface) { manager_of(iface)->paused = TRUE; return S_OK; }
static HRESULT STDMETHODCALLTYPE manager_Resume(void *iface) { manager_of(iface)->paused = FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE manager_SetManagerEventHandler(void *iface, IUIAnimationManagerEventHandler *handler)
{
	struct manager *self = manager_of(iface);
	if (handler)
		IUIAnimationManagerEventHandler_AddRef(handler);
	if (self->handler)
		IUIAnimationManagerEventHandler_Release(self->handler);
	self->handler = handler;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE manager_SetComparison(void *iface, void *comparison) { return S_OK; }
static HRESULT STDMETHODCALLTYPE manager_SetDefaultLongestAcceptableDelay(void *iface, UI_ANIMATION_SECONDS delay) { return S_OK; }
static HRESULT STDMETHODCALLTYPE manager_Shutdown(void *iface)
{
	manager_AbandonAllStoryboards(iface);
	manager_SetManagerEventHandler(iface, NULL);
	return S_OK;
}

/* The manager as the timer's update handler: the timer calls it, and it tells the timer when it has work. */
static HRESULT STDMETHODCALLTYPE update_OnUpdate(void *iface, UI_ANIMATION_SECONDS time, UI_ANIMATION_UPDATE_RESULT *result)
{
	return update(manager_of(iface), time, result);
}

static HRESULT STDMETHODCALLTYPE update_SetTimerClientEventHandler(void *iface, IUIAnimationTimerClientEventHandler *client)
{
	struct manager *self = manager_of(iface);
	if (client)
		IUIAnimationTimerClientEventHandler_AddRef(client);
	if (self->client)
		IUIAnimationTimerClientEventHandler_Release(self->client);
	self->client = client;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE update_ClearTimerClientEventHandler(void *iface) { return update_SetTimerClientEventHandler(iface, NULL); }

/* ---- the timer ---- */
struct timer {
	void **vtbl;
	void **client_vtbl; /* IUIAnimationTimerClientEventHandler, given to the update handler */
	LONG refs;
	IUIAnimationTimerUpdateHandler *updater;
	IUIAnimationTimerEventHandler *events;
	HANDLE thread, wake;
	volatile LONG enabled, busy, stopping;
};

static void **timer_table, **client_table;

static struct timer *timer_of(void *iface)
{
	return *(void ***)iface == client_table ? CONTAINER(iface, struct timer, client_vtbl) : iface;
}

static DWORD WINAPI timer_thread(void *parameter)
{
	struct timer *self = parameter;
	while (!self->stopping) {
		if (!self->enabled || !self->busy) {
			WaitForSingleObject(self->wake, 250);
			continue;
		}
		if (self->events)
			IUIAnimationTimerEventHandler_OnPreUpdate(self->events);
		if (self->updater) {
			UI_ANIMATION_UPDATE_RESULT result = UI_ANIMATION_UPDATE_NO_CHANGE;
			IUIAnimationTimerUpdateHandler_OnUpdate(self->updater, now_seconds(), &result);
		}
		if (self->events)
			IUIAnimationTimerEventHandler_OnPostUpdate(self->events);
		Sleep(16);
	}
	return 0;
}

static HRESULT STDMETHODCALLTYPE timer_QueryInterface(void *iface, REFIID riid, void **out)
{
	struct timer *self = timer_of(iface);
	void *found = NULL;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IUIAnimationTimer))
		found = self;
	else if (same_guid(riid, &IID_IUIAnimationTimerClientEventHandler))
		found = &self->client_vtbl;
	if (!found) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	*out = found;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE timer_AddRef(void *iface) { return (ULONG)InterlockedIncrement(&timer_of(iface)->refs); }

static ULONG STDMETHODCALLTYPE timer_Release(void *iface)
{
	struct timer *self = timer_of(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		if (self->thread) {
			self->stopping = TRUE;
			SetEvent(self->wake);
			WaitForSingleObject(self->thread, INFINITE);
			CloseHandle(self->thread);
		}
		if (self->wake)
			CloseHandle(self->wake);
		if (self->updater)
			IUIAnimationTimerUpdateHandler_Release(self->updater);
		if (self->events)
			IUIAnimationTimerEventHandler_Release(self->events);
		FREE(self);
	}
	return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE timer_SetTimerUpdateHandler(void *iface, IUIAnimationTimerUpdateHandler *updater, UI_ANIMATION_IDLE_BEHAVIOR idle)
{
	struct timer *self = timer_of(iface);
	if (self->updater) {
		IUIAnimationTimerUpdateHandler_ClearTimerClientEventHandler(self->updater);
		IUIAnimationTimerUpdateHandler_Release(self->updater);
	}
	self->updater = updater;
	if (updater) {
		IUIAnimationTimerUpdateHandler_AddRef(updater);
		IUIAnimationTimerUpdateHandler_SetTimerClientEventHandler(updater, (IUIAnimationTimerClientEventHandler *)&self->client_vtbl);
	}
	/* The manager may be busy already. */
	self->busy = TRUE;
	SetEvent(self->wake);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_SetTimerEventHandler(void *iface, IUIAnimationTimerEventHandler *events)
{
	struct timer *self = timer_of(iface);
	if (events)
		IUIAnimationTimerEventHandler_AddRef(events);
	if (self->events)
		IUIAnimationTimerEventHandler_Release(self->events);
	self->events = events;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_Enable(void *iface)
{
	struct timer *self = timer_of(iface);
	if (self->enabled)
		return S_FALSE;
	self->enabled = TRUE;
	if (!self->thread)
		self->thread = CreateThread(NULL, 0, timer_thread, self, 0, NULL);
	SetEvent(self->wake);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_Disable(void *iface)
{
	struct timer *self = timer_of(iface);
	if (!self->enabled)
		return S_FALSE;
	self->enabled = FALSE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_IsEnabled(void *iface) { return timer_of(iface)->enabled ? S_OK : S_FALSE; }
static HRESULT STDMETHODCALLTYPE timer_GetTime(void *iface, UI_ANIMATION_SECONDS *seconds) { *seconds = now_seconds(); return S_OK; }
static HRESULT STDMETHODCALLTYPE timer_SetFrameRateThreshold(void *iface, UINT32 threshold) { return S_OK; }

static HRESULT STDMETHODCALLTYPE client_OnTimerClientStatusChanged(void *iface, UI_ANIMATION_TIMER_CLIENT_STATUS status,
	UI_ANIMATION_TIMER_CLIENT_STATUS previous)
{
	struct timer *self = timer_of(iface);
	self->busy = status == UI_ANIMATION_TIMER_CLIENT_BUSY;
	if (self->busy)
		SetEvent(self->wake);
	return S_OK;
}

/* ---- the classes ---- */
static void build_tables(void)
{
	transition_table = make_table(IUIAnimationTransition_table, IUIAnimationTransition_methods);
	set_unknown(transition_table);
	transition_table[IUIAnimationTransition_SetInitialValue_slot] = (void *)transition_SetInitialValue;
	transition_table[IUIAnimationTransition_SetInitialVelocity_slot] = (void *)transition_SetInitialVelocity;
	transition_table[IUIAnimationTransition_IsDurationKnown_slot] = (void *)transition_IsDurationKnown;
	transition_table[IUIAnimationTransition_GetDuration_slot] = (void *)transition_GetDuration;

	library_table = make_table(IUIAnimationTransitionLibrary_table, IUIAnimationTransitionLibrary_methods);
	set_unknown(library_table);
	library_table[IUIAnimationTransitionLibrary_CreateInstantaneousTransition_slot] = (void *)library_Instantaneous;
	library_table[IUIAnimationTransitionLibrary_CreateConstantTransition_slot] = (void *)library_Constant;
	library_table[IUIAnimationTransitionLibrary_CreateDiscreteTransition_slot] = (void *)library_Discrete;
	library_table[IUIAnimationTransitionLibrary_CreateLinearTransition_slot] = (void *)library_Linear;
	library_table[IUIAnimationTransitionLibrary_CreateLinearTransitionFromSpeed_slot] = (void *)library_LinearFromSpeed;
	library_table[IUIAnimationTransitionLibrary_CreateSinusoidalTransitionFromVelocity_slot] = (void *)library_SineFromVelocity;
	library_table[IUIAnimationTransitionLibrary_CreateSinusoidalTransitionFromRange_slot] = (void *)library_SineFromRange;
	library_table[IUIAnimationTransitionLibrary_CreateAccelerateDecelerateTransition_slot] = (void *)library_AccelerateDecelerate;
	library_table[IUIAnimationTransitionLibrary_CreateReversalTransition_slot] = (void *)library_Reversal;
	library_table[IUIAnimationTransitionLibrary_CreateCubicTransition_slot] = (void *)library_Cubic;
	library_table[IUIAnimationTransitionLibrary_CreateSmoothStopTransition_slot] = (void *)library_SmoothStop;
	library_table[IUIAnimationTransitionLibrary_CreateParabolicTransitionFromAcceleration_slot] = (void *)library_Parabolic;

	transition_factory_table = make_table(IUIAnimationTransitionFactory_table, IUIAnimationTransitionFactory_methods);
	set_unknown(transition_factory_table);
	transition_factory_table[IUIAnimationTransitionFactory_CreateTransition_slot] = (void *)transition_factory_CreateTransition;

	variable_table = make_table(IUIAnimationVariable_table, IUIAnimationVariable_methods);
	set_unknown(variable_table);
	variable_table[IUIAnimationVariable_GetValue_slot] = (void *)variable_GetValue;
	variable_table[IUIAnimationVariable_GetFinalValue_slot] = (void *)variable_GetFinalValue;
	variable_table[IUIAnimationVariable_GetPreviousValue_slot] = (void *)variable_GetPreviousValue;
	variable_table[IUIAnimationVariable_GetIntegerValue_slot] = (void *)variable_GetIntegerValue;
	variable_table[IUIAnimationVariable_GetFinalIntegerValue_slot] = (void *)variable_GetFinalIntegerValue;
	variable_table[IUIAnimationVariable_GetPreviousIntegerValue_slot] = (void *)variable_GetPreviousIntegerValue;
	variable_table[IUIAnimationVariable_GetCurrentStoryboard_slot] = (void *)variable_GetCurrentStoryboard;
	variable_table[IUIAnimationVariable_SetLowerBound_slot] = (void *)variable_SetLowerBound;
	variable_table[IUIAnimationVariable_SetUpperBound_slot] = (void *)variable_SetUpperBound;
	variable_table[IUIAnimationVariable_SetRoundingMode_slot] = (void *)variable_SetRoundingMode;
	variable_table[IUIAnimationVariable_SetTag_slot] = (void *)variable_SetTag;
	variable_table[IUIAnimationVariable_GetTag_slot] = (void *)variable_GetTag;
	variable_table[IUIAnimationVariable_SetVariableChangeHandler_slot] = (void *)variable_SetVariableChangeHandler;
	variable_table[IUIAnimationVariable_SetVariableIntegerChangeHandler_slot] = (void *)variable_SetVariableIntegerChangeHandler;

	storyboard_table = make_table(IUIAnimationStoryboard_table, IUIAnimationStoryboard_methods);
	set_unknown(storyboard_table);
	storyboard_table[IUIAnimationStoryboard_AddTransition_slot] = (void *)storyboard_AddTransition;
	storyboard_table[IUIAnimationStoryboard_AddKeyframeAtOffset_slot] = (void *)storyboard_AddKeyframeAtOffset;
	storyboard_table[IUIAnimationStoryboard_AddKeyframeAfterTransition_slot] = (void *)storyboard_AddKeyframeAfterTransition;
	storyboard_table[IUIAnimationStoryboard_AddTransitionAtKeyframe_slot] = (void *)storyboard_AddTransitionAtKeyframe;
	storyboard_table[IUIAnimationStoryboard_AddTransitionBetweenKeyframes_slot] = (void *)storyboard_AddTransitionBetweenKeyframes;
	storyboard_table[IUIAnimationStoryboard_RepeatBetweenKeyframes_slot] = (void *)storyboard_RepeatBetweenKeyframes;
	storyboard_table[IUIAnimationStoryboard_HoldVariable_slot] = (void *)storyboard_HoldVariable;
	storyboard_table[IUIAnimationStoryboard_SetLongestAcceptableDelay_slot] = (void *)storyboard_SetLongestAcceptableDelay;
	storyboard_table[IUIAnimationStoryboard_Schedule_slot] = (void *)storyboard_Schedule;
	storyboard_table[IUIAnimationStoryboard_Conclude_slot] = (void *)storyboard_Conclude;
	storyboard_table[IUIAnimationStoryboard_Finish_slot] = (void *)storyboard_Finish;
	storyboard_table[IUIAnimationStoryboard_Abandon_slot] = (void *)storyboard_Abandon;
	storyboard_table[IUIAnimationStoryboard_SetTag_slot] = (void *)storyboard_SetTag;
	storyboard_table[IUIAnimationStoryboard_GetTag_slot] = (void *)storyboard_GetTag;
	storyboard_table[IUIAnimationStoryboard_GetStatus_slot] = (void *)storyboard_GetStatus;
	storyboard_table[IUIAnimationStoryboard_GetElapsedTime_slot] = (void *)storyboard_GetElapsedTime;
	storyboard_table[IUIAnimationStoryboard_SetStoryboardEventHandler_slot] = (void *)storyboard_SetStoryboardEventHandler;

	manager_table = make_table(IUIAnimationManager_table, IUIAnimationManager_methods);
	manager_table[0] = (void *)manager_QueryInterface;
	manager_table[1] = (void *)manager_AddRef;
	manager_table[2] = (void *)manager_Release;
	manager_table[IUIAnimationManager_CreateAnimationVariable_slot] = (void *)manager_CreateAnimationVariable;
	manager_table[IUIAnimationManager_ScheduleTransition_slot] = (void *)manager_ScheduleTransition;
	manager_table[IUIAnimationManager_CreateStoryboard_slot] = (void *)manager_CreateStoryboard;
	manager_table[IUIAnimationManager_FinishAllStoryboards_slot] = (void *)manager_FinishAllStoryboards;
	manager_table[IUIAnimationManager_AbandonAllStoryboards_slot] = (void *)manager_AbandonAllStoryboards;
	manager_table[IUIAnimationManager_Update_slot] = (void *)manager_Update;
	manager_table[IUIAnimationManager_GetVariableFromTag_slot] = (void *)manager_GetVariableFromTag;
	manager_table[IUIAnimationManager_GetStoryboardFromTag_slot] = (void *)manager_GetStoryboardFromTag;
	manager_table[IUIAnimationManager_GetStatus_slot] = (void *)manager_GetStatus;
	manager_table[IUIAnimationManager_SetAnimationMode_slot] = (void *)manager_SetAnimationMode;
	manager_table[IUIAnimationManager_Pause_slot] = (void *)manager_Pause;
	manager_table[IUIAnimationManager_Resume_slot] = (void *)manager_Resume;
	manager_table[IUIAnimationManager_SetManagerEventHandler_slot] = (void *)manager_SetManagerEventHandler;
	manager_table[IUIAnimationManager_SetCancelPriorityComparison_slot] = (void *)manager_SetComparison;
	manager_table[IUIAnimationManager_SetTrimPriorityComparison_slot] = (void *)manager_SetComparison;
	manager_table[IUIAnimationManager_SetCompressPriorityComparison_slot] = (void *)manager_SetComparison;
	manager_table[IUIAnimationManager_SetConcludePriorityComparison_slot] = (void *)manager_SetComparison;
	manager_table[IUIAnimationManager_SetDefaultLongestAcceptableDelay_slot] = (void *)manager_SetDefaultLongestAcceptableDelay;
	manager_table[IUIAnimationManager_Shutdown_slot] = (void *)manager_Shutdown;
	update_table = make_table(IUIAnimationTimerUpdateHandler_table, IUIAnimationTimerUpdateHandler_methods);
	update_table[0] = (void *)manager_QueryInterface;
	update_table[1] = (void *)manager_AddRef;
	update_table[2] = (void *)manager_Release;
	update_table[IUIAnimationTimerUpdateHandler_OnUpdate_slot] = (void *)update_OnUpdate;
	update_table[IUIAnimationTimerUpdateHandler_SetTimerClientEventHandler_slot] = (void *)update_SetTimerClientEventHandler;
	update_table[IUIAnimationTimerUpdateHandler_ClearTimerClientEventHandler_slot] = (void *)update_ClearTimerClientEventHandler;

	timer_table = make_table(IUIAnimationTimer_table, IUIAnimationTimer_methods);
	timer_table[0] = (void *)timer_QueryInterface;
	timer_table[1] = (void *)timer_AddRef;
	timer_table[2] = (void *)timer_Release;
	timer_table[IUIAnimationTimer_SetTimerUpdateHandler_slot] = (void *)timer_SetTimerUpdateHandler;
	timer_table[IUIAnimationTimer_SetTimerEventHandler_slot] = (void *)timer_SetTimerEventHandler;
	timer_table[IUIAnimationTimer_Enable_slot] = (void *)timer_Enable;
	timer_table[IUIAnimationTimer_Disable_slot] = (void *)timer_Disable;
	timer_table[IUIAnimationTimer_IsEnabled_slot] = (void *)timer_IsEnabled;
	timer_table[IUIAnimationTimer_GetTime_slot] = (void *)timer_GetTime;
	timer_table[IUIAnimationTimer_SetFrameRateThreshold_slot] = (void *)timer_SetFrameRateThreshold;
	client_table = ALLOC(4 * sizeof(void *));
	client_table[0] = (void *)timer_QueryInterface;
	client_table[1] = (void *)timer_AddRef;
	client_table[2] = (void *)timer_Release;
	client_table[3] = (void *)client_OnTimerClientStatusChanged;
}

static HRESULT create_instance(const CLSID *clsid, REFIID riid, void **out)
{
	HRESULT hr;
	if (same_guid(clsid, &CLSID_UIAnimationManager)) {
		struct manager *self = ALLOC(sizeof(*self));
		if (!self)
			return E_OUTOFMEMORY;
		self->vtbl = manager_table;
		self->update_vtbl = update_table;
		self->refs = 1;
		self->mode = UI_ANIMATION_MODE_SYSTEM_DEFAULT;
		InitializeCriticalSection(&self->lock);
		hr = manager_QueryInterface(self, riid, out);
		manager_Release(self);
		return hr;
	}
	if (same_guid(clsid, &CLSID_UIAnimationTransitionLibrary)) {
		struct object *self = ALLOC(sizeof(*self));
		if (!self)
			return E_OUTOFMEMORY;
		object_init(self, library_table, NULL, &IID_IUIAnimationTransitionLibrary);
		hr = object_QueryInterface(self, riid, out);
		object_Release(self);
		return hr;
	}
	if (same_guid(clsid, &CLSID_UIAnimationTransitionFactory)) {
		struct object *self = ALLOC(sizeof(*self));
		if (!self)
			return E_OUTOFMEMORY;
		object_init(self, transition_factory_table, NULL, &IID_IUIAnimationTransitionFactory);
		hr = object_QueryInterface(self, riid, out);
		object_Release(self);
		return hr;
	}
	if (same_guid(clsid, &CLSID_UIAnimationTimer)) {
		struct timer *self = ALLOC(sizeof(*self));
		if (!self)
			return E_OUTOFMEMORY;
		self->vtbl = timer_table;
		self->client_vtbl = client_table;
		self->refs = 1;
		self->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		hr = timer_QueryInterface(self, riid, out);
		timer_Release(self);
		return hr;
	}
	*out = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

/* ---- the class factory and the exports ---- */
struct class_factory {
	IClassFactory iface;
	CLSID clsid;
	LONG refs;
};

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
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
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return create_instance(&((struct class_factory *)iface)->clsid, riid, out);
}
static HRESULT STDMETHODCALLTYPE factory_LockServer(IClassFactory *iface, BOOL lock) { return S_OK; }

static IClassFactoryVtbl factory_vtbl = {factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer};

static INIT_ONCE tables_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tables_ready(INIT_ONCE *once, void *parameter, void **context)
{
	build_tables();
	return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	struct class_factory *factory;
	HRESULT hr;
	InitOnceExecuteOnce(&tables_once, tables_ready, NULL, NULL);
	if (!same_guid(clsid, &CLSID_UIAnimationManager) && !same_guid(clsid, &CLSID_UIAnimationTransitionLibrary) &&
		!same_guid(clsid, &CLSID_UIAnimationTimer) && !same_guid(clsid, &CLSID_UIAnimationTransitionFactory)) {
		*out = NULL;
		return CLASS_E_CLASSNOTAVAILABLE;
	}
	if (!(factory = ALLOC(sizeof(*factory))))
		return E_OUTOFMEMORY;
	factory->iface.lpVtbl = &factory_vtbl;
	factory->clsid = *clsid;
	factory->refs = 1;
	hr = factory_QueryInterface(&factory->iface, riid, out);
	factory_Release(&factory->iface);
	return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
