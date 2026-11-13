/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* dcomp.dll: the visual layer of Windows.UI.Composition, which desktop programs use to show trees of visuals in
 * their windows: rectangles filled with a colour or with the pixels of a swap chain, placed, sized, clipped and faded.
 *
 * A compositor keeps the tree as the program builds it. Windows draws a committed tree on its own composition thread; here a commit
 * draws it into each window a desktop target shows it in, over what the window paints itself, and again whenever the window paints.
 * A compositor made through a CompositorController commits when the program calls Commit, after the controller has told it that
 * there are changes; one made on its own commits by itself once the thread that changed it goes back to its messages.
 *
 * The compositor clock ticks with the display's frames, sixty a second here, and programs pace their drawing on it. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <winstring.h>
#include <activation.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.foundation.h>
#include <windows.ui.composition.h>
#include <windows.ui.composition.interop.h>
#include "composition_interfaces.h"

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define GROW(block, size) ((block) ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (block), (size)) : ALLOC(size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))

typedef __x_ABI_CWindows_CFoundation_CNumerics_CVector2 vector2;
typedef __x_ABI_CWindows_CFoundation_CNumerics_CVector3 vector3;
typedef __x_ABI_CWindows_CFoundation_CNumerics_CMatrix4x4 matrix4x4;
typedef __x_ABI_CWindows_CFoundation_CNumerics_CQuaternion quaternion;
typedef __x_ABI_CWindows_CUI_CColor color;

/* Every method nobody wrote says so in the debug output and fails, so the first one a program reaches is named. */
static HRESULT unimplemented(const char *iface, const char *method)
{
	char text[160];
	wsprintfA(text, "dcomp: %s::%s is not implemented\n", iface, method);
	OutputDebugStringA(text);
	return E_NOTIMPL;
}

#include "composition_tables.h"

/* ---- objects: one object answers several interfaces, each through a face that holds its table ---- */
#define FACES 8

struct node;
struct face {
	void **table;
	struct node *node;
};

struct node {
	struct face faces[FACES];
	const GUID *iids[FACES];
	UINT count;
	LONG refs;
	const WCHAR *name;
	void (*destroy)(struct node *self);
};

static CRITICAL_SECTION lock;
static LONG live;

static struct node *node_of(void *iface)
{
	return ((struct face *)iface)->node;
}

static void *face_of(struct node *self, const GUID *iid)
{
	UINT index;
	for (index = 0; index < self->count; ++index)
		if (IsEqualIID(iid, self->iids[index]))
			return &self->faces[index];
	return NULL;
}

static void node_add(struct node *self, const GUID *iid, void **table)
{
	self->faces[self->count].table = table;
	self->faces[self->count].node = self;
	self->iids[self->count] = iid;
	++self->count;
}

static void *node_init(struct node *self, const WCHAR *name, void (*destroy)(struct node *))
{
	self->refs = 1;
	self->name = name;
	self->destroy = destroy;
	InterlockedIncrement(&live);
	return self;
}

static void node_addref(struct node *self)
{
	InterlockedIncrement(&self->refs);
}

static void node_release(struct node *self)
{
	if (self && !InterlockedDecrement(&self->refs)) {
		if (self->destroy)
			self->destroy(self);
		FREE(self);
		InterlockedDecrement(&live);
	}
}

/* An interface asked for that no object here answers is named in the debug output: the program's next need. */
static void report_interface(const struct node *self, REFIID iid)
{
	char text[200];
	wsprintfA(text, "dcomp: %ls has no interface {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n", self->name,
		iid->Data1, iid->Data2, iid->Data3, iid->Data4[0], iid->Data4[1], iid->Data4[2], iid->Data4[3], iid->Data4[4], iid->Data4[5],
		iid->Data4[6], iid->Data4[7]);
	OutputDebugStringA(text);
}

static HRESULT STDMETHODCALLTYPE face_query(void *iface, REFIID iid, void **out)
{
	struct node *self = node_of(iface);
	void *found;
	if (!out)
		return E_POINTER;
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject))
		found = &self->faces[0];
	else if (!(found = face_of(self, iid))) {
		*out = NULL;
		if (!IsEqualIID(iid, &IID_IMarshal) && !IsEqualIID(iid, &IID_INoMarshal))
			report_interface(self, iid);
		return E_NOINTERFACE;
	}
	node_addref(self);
	*out = found;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE face_addref(void *iface)
{
	return (ULONG)InterlockedIncrement(&node_of(iface)->refs);
}

static ULONG STDMETHODCALLTYPE face_release(void *iface)
{
	struct node *self = node_of(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		if (self->destroy)
			self->destroy(self);
		FREE(self);
		InterlockedDecrement(&live);
	}
	return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE face_iids(void *iface, ULONG *count, IID **iids)
{
	struct node *self = node_of(iface);
	UINT index;
	if (!count || !iids)
		return E_POINTER;
	if (!(*iids = CoTaskMemAlloc(sizeof(IID) * self->count)))
		return E_OUTOFMEMORY;
	for (index = 0; index < self->count; ++index)
		(*iids)[index] = *self->iids[index];
	*count = self->count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE face_name(void *iface, HSTRING *name)
{
	const WCHAR *text = node_of(iface)->name;
	return name ? WindowsCreateString(text, (UINT32)lstrlenW(text), name) : E_POINTER;
}

static HRESULT STDMETHODCALLTYPE face_trust(void *iface, TrustLevel *level)
{
	if (!level)
		return E_POINTER;
	*level = BaseTrust;
	return S_OK;
}

/* Fills the IUnknown part of a table, and the IInspectable part when it has one. */
static void set_unknown(void **table)
{
	table[0] = (void *)face_query;
	table[1] = (void *)face_addref;
	table[2] = (void *)face_release;
}

static void set_inspectable(void **table)
{
	set_unknown(table);
	table[3] = (void *)face_iids;
	table[4] = (void *)face_name;
	table[5] = (void *)face_trust;
}

/* The interface pointer of an object for an interface it answers, with a reference for the caller; NULL stays NULL. */
static HRESULT hand_out(struct node *self, const GUID *iid, void **out)
{
	void *found;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!self)
		return S_OK;
	if (!(found = face_of(self, iid)))
		return E_NOINTERFACE;
	node_addref(self);
	*out = found;
	return S_OK;
}

/* The object behind an interface pointer the program passes back, when it is one of these objects with that table. */
static struct node *node_from(void *iface, void **table)
{
	return iface && ((struct face *)iface)->table == table ? node_of(iface) : NULL;
}

/* ---- the kinds of object ---- */
struct compositor;
struct visual;

struct controller {
	struct node node;
	struct compositor *compositor;
	struct {
		IUnknown *handler;
		INT64 token;
	} handlers[8];
	INT64 next_token;
};

struct target {
	struct node node;
	struct compositor *compositor;
	struct target *next;
	HWND window;
	BOOL topmost;
	struct visual *root;
	WNDPROC previous;
};

struct compositor {
	struct node node;
	struct controller *controller; /* weak: the controller holds the compositor */
	struct target *targets;
	HWND messages;
	BOOL pending;
	BOOL committed; /* nothing shows before the first commit */
	LONG frame_pending; /* a swap chain presented and the targets are not drawn again yet */
};

enum { BRUSH_COLOR, BRUSH_SURFACE };

struct brush {
	struct node node;
	struct compositor *compositor;
	int kind;
	color color;
	struct node *surface;
	__x_ABI_CWindows_CUI_CComposition_CCompositionStretch stretch;
	__x_ABI_CWindows_CUI_CComposition_CCompositionBitmapInterpolationMode interpolation;
	float horizontal, vertical;
};

struct surface {
	struct node node;
	struct compositor *compositor;
	IDXGISwapChain1 *swap_chain;
	ID3D11Texture2D *staging;
	UINT staging_width, staging_height;
	struct surface *next_watched;
	/* The part of the frame presented since the targets were last drawn, or all of it, and the part being drawn now. */
	RECT changed, drawing;
	BOOL changed_whole, drawing_whole;
};

/* The surfaces over swap chains, which see the frames their swap chains present. */
static struct surface *watched;

struct clip {
	struct node node;
	struct compositor *compositor;
	float left, top, right, bottom;
};

struct collection {
	struct node node;
	struct visual *owner; /* weak: the visual holds its collection */
	struct visual **items;
	UINT count, capacity;
};

struct visual {
	struct node node;
	struct compositor *compositor;
	BOOL sprite;
	struct visual *parent; /* weak: a parent holds its children */
	struct collection *children;
	struct brush *brush;
	struct clip *clip;
	vector3 offset, center, scale, axis;
	vector2 size, anchor;
	float opacity, angle;
	boolean visible;
	matrix4x4 transform;
	quaternion orientation;
	__x_ABI_CWindows_CUI_CComposition_CCompositionBackfaceVisibility backface;
	__x_ABI_CWindows_CUI_CComposition_CCompositionBorderMode border;
	__x_ABI_CWindows_CUI_CComposition_CCompositionCompositeMode composite;
};

struct graphics_device {
	struct node node;
	struct compositor *compositor;
	IUnknown *device;
};

static void changed(struct compositor *compositor);
static void render(struct target *target, BOOL frame_only);

/* ---- the compositor's objects share ICompositionObject: their compositor ---- */
static struct compositor *compositor_of_object(void *iface)
{
	/* Every kind keeps its compositor right after the node. */
	return *(struct compositor **)((char *)node_of(iface) + sizeof(struct node));
}

static HRESULT STDMETHODCALLTYPE object_get_Compositor(void *iface, void **out)
{
	return hand_out(&compositor_of_object(iface)->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositor, out);
}

static HRESULT STDMETHODCALLTYPE object_get_Dispatcher(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE closable_Close(void *iface)
{
	return S_OK;
}

/* ---- visuals and their children ---- */
static const WCHAR container_name[] = L"Windows.UI.Composition.ContainerVisual";
static const WCHAR sprite_name[] = L"Windows.UI.Composition.SpriteVisual";
static const WCHAR collection_name[] = L"Windows.UI.Composition.VisualCollection";

static struct visual *visual_of(void *iface)
{
	return (struct visual *)node_of(iface);
}

static void collection_destroy(struct node *base)
{
	struct collection *self = (struct collection *)base;
	UINT index;
	for (index = 0; index < self->count; ++index) {
		self->items[index]->parent = NULL;
		node_release(&self->items[index]->node);
	}
	FREE(self->items);
}

static void visual_destroy(struct node *base)
{
	struct visual *self = (struct visual *)base;
	if (self->children) {
		self->children->owner = NULL;
		node_release(&self->children->node);
	}
	node_release(self->brush ? &self->brush->node : NULL);
	node_release(self->clip ? &self->clip->node : NULL);
	node_release(&self->compositor->node);
}

static struct visual *make_visual(struct compositor *compositor, BOOL sprite)
{
	struct visual *self = ALLOC(sizeof(*self));
	struct collection *children = ALLOC(sizeof(*children));
	static const matrix4x4 identity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	if (!self || !children) {
		FREE(self);
		FREE(children);
		return NULL;
	}
	node_init(&self->node, sprite ? sprite_name : container_name, visual_destroy);
	node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionObject, ICompositionObject_table);
	node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CIVisual, IVisual_table);
	node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CIContainerVisual, IContainerVisual_table);
	if (sprite)
		node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CISpriteVisual, ISpriteVisual_table);
	node_add(&self->node, &IID___x_ABI_CWindows_CFoundation_CIClosable, IClosable_table);
	self->compositor = compositor;
	node_addref(&compositor->node);
	self->sprite = sprite;
	self->opacity = 1;
	self->visible = TRUE;
	self->scale.X = self->scale.Y = self->scale.Z = 1;
	self->axis.Z = 1;
	self->orientation.W = 1;
	self->transform = identity;
	node_init(&children->node, collection_name, collection_destroy);
	node_add(&children->node, &IID___x_ABI_CWindows_CUI_CComposition_CIVisualCollection, IVisualCollection_table);
	children->owner = self;
	self->children = children;
	return self;
}

/* The properties of a visual: each change asks for a commit. */
#define VISUAL_PROPERTY(name, field, type) \
	static HRESULT STDMETHODCALLTYPE visual_get_##name(void *iface, type *value) \
	{ \
		if (!value) \
			return E_POINTER; \
		*value = visual_of(iface)->field; \
		return S_OK; \
	} \
	static HRESULT STDMETHODCALLTYPE visual_put_##name(void *iface, type value) \
	{ \
		struct visual *self = visual_of(iface); \
		EnterCriticalSection(&lock); \
		self->field = value; \
		LeaveCriticalSection(&lock); \
		changed(self->compositor); \
		return S_OK; \
	}

VISUAL_PROPERTY(AnchorPoint, anchor, vector2)
VISUAL_PROPERTY(BackfaceVisibility, backface, __x_ABI_CWindows_CUI_CComposition_CCompositionBackfaceVisibility)
VISUAL_PROPERTY(BorderMode, border, __x_ABI_CWindows_CUI_CComposition_CCompositionBorderMode)
VISUAL_PROPERTY(CenterPoint, center, vector3)
VISUAL_PROPERTY(CompositeMode, composite, __x_ABI_CWindows_CUI_CComposition_CCompositionCompositeMode)
VISUAL_PROPERTY(IsVisible, visible, boolean)
VISUAL_PROPERTY(Offset, offset, vector3)
VISUAL_PROPERTY(Opacity, opacity, float)
VISUAL_PROPERTY(Orientation, orientation, quaternion)
VISUAL_PROPERTY(RotationAngle, angle, float)
VISUAL_PROPERTY(RotationAxis, axis, vector3)
VISUAL_PROPERTY(Scale, scale, vector3)
VISUAL_PROPERTY(Size, size, vector2)
VISUAL_PROPERTY(TransformMatrix, transform, matrix4x4)

static HRESULT STDMETHODCALLTYPE visual_get_RotationAngleInDegrees(void *iface, float *value)
{
	if (!value)
		return E_POINTER;
	*value = visual_of(iface)->angle * 57.29577951f;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_put_RotationAngleInDegrees(void *iface, float value)
{
	return visual_put_RotationAngle(iface, value / 57.29577951f);
}

static HRESULT STDMETHODCALLTYPE visual_get_Clip(void *iface, void **out)
{
	struct visual *self = visual_of(iface);
	return hand_out(self->clip ? &self->clip->node : NULL, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionClip, out);
}

static HRESULT STDMETHODCALLTYPE visual_put_Clip(void *iface, void *value)
{
	struct visual *self = visual_of(iface);
	struct clip *clip = (struct clip *)node_from(value, ICompositionClip_table);
	if (value && !clip)
		return unimplemented("IVisual", "put_Clip of a clip that is not an inset clip");
	EnterCriticalSection(&lock);
	if (clip)
		node_addref(&clip->node);
	node_release(self->clip ? &self->clip->node : NULL);
	self->clip = clip;
	LeaveCriticalSection(&lock);
	changed(self->compositor);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_get_Parent(void *iface, void **out)
{
	struct visual *self = visual_of(iface);
	return hand_out(self->parent ? &self->parent->node : NULL, &IID___x_ABI_CWindows_CUI_CComposition_CIContainerVisual, out);
}

static HRESULT STDMETHODCALLTYPE visual_get_Children(void *iface, void **out)
{
	return hand_out(&visual_of(iface)->children->node, &IID___x_ABI_CWindows_CUI_CComposition_CIVisualCollection, out);
}

static HRESULT STDMETHODCALLTYPE sprite_get_Brush(void *iface, void **out)
{
	struct visual *self = visual_of(iface);
	return hand_out(self->brush ? &self->brush->node : NULL, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionBrush, out);
}

static HRESULT STDMETHODCALLTYPE sprite_put_Brush(void *iface, void *value)
{
	struct visual *self = visual_of(iface);
	struct brush *brush = (struct brush *)node_from(value, ICompositionBrush_table);
	if (value && !brush)
		return unimplemented("ISpriteVisual", "put_Brush of a brush made elsewhere");
	EnterCriticalSection(&lock);
	if (brush)
		node_addref(&brush->node);
	node_release(self->brush ? &self->brush->node : NULL);
	self->brush = brush;
	LeaveCriticalSection(&lock);
	changed(self->compositor);
	return S_OK;
}

/* The collection keeps its visuals from bottom to top. A visual has one parent: inserting one that has another fails, as on Windows. */
static struct collection *collection_of(void *iface)
{
	return (struct collection *)node_of(iface);
}

static HRESULT collection_insert(struct collection *self, void *child, UINT at)
{
	struct visual *visual = (struct visual *)node_from(child, IVisual_table);
	if (!visual)
		return E_INVALIDARG;
	EnterCriticalSection(&lock);
	if (visual->parent) {
		LeaveCriticalSection(&lock);
		return E_INVALIDARG;
	}
	if (self->count == self->capacity) {
		const UINT capacity = self->capacity ? self->capacity * 2 : 8;
		struct visual **grown = GROW(self->items, capacity * sizeof(*grown));
		if (!grown) {
			LeaveCriticalSection(&lock);
			return E_OUTOFMEMORY;
		}
		self->items = grown;
		self->capacity = capacity;
	}
	MoveMemory(self->items + at + 1, self->items + at, (self->count - at) * sizeof(*self->items));
	self->items[at] = visual;
	++self->count;
	visual->parent = self->owner;
	node_addref(&visual->node);
	LeaveCriticalSection(&lock);
	if (self->owner)
		changed(self->owner->compositor);
	return S_OK;
}

static UINT collection_find(struct collection *self, void *sibling)
{
	struct visual *visual = (struct visual *)node_from(sibling, IVisual_table);
	UINT index;
	for (index = 0; index < self->count; ++index)
		if (self->items[index] == visual)
			return index;
	return UINT_MAX;
}

static HRESULT STDMETHODCALLTYPE collection_get_Count(void *iface, INT32 *count)
{
	if (!count)
		return E_POINTER;
	*count = (INT32)collection_of(iface)->count;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE collection_InsertAbove(void *iface, void *child, void *sibling)
{
	struct collection *self = collection_of(iface);
	const UINT at = collection_find(self, sibling);
	return at == UINT_MAX ? E_INVALIDARG : collection_insert(self, child, at + 1);
}

static HRESULT STDMETHODCALLTYPE collection_InsertBelow(void *iface, void *child, void *sibling)
{
	struct collection *self = collection_of(iface);
	const UINT at = collection_find(self, sibling);
	return at == UINT_MAX ? E_INVALIDARG : collection_insert(self, child, at);
}

static HRESULT STDMETHODCALLTYPE collection_InsertAtBottom(void *iface, void *child)
{
	return collection_insert(collection_of(iface), child, 0);
}

static HRESULT STDMETHODCALLTYPE collection_InsertAtTop(void *iface, void *child)
{
	struct collection *self = collection_of(iface);
	return collection_insert(self, child, self->count);
}

static HRESULT STDMETHODCALLTYPE collection_Remove(void *iface, void *child)
{
	struct collection *self = collection_of(iface);
	struct visual *removed;
	UINT at;
	EnterCriticalSection(&lock);
	if ((at = collection_find(self, child)) == UINT_MAX) {
		LeaveCriticalSection(&lock);
		return E_INVALIDARG;
	}
	removed = self->items[at];
	MoveMemory(self->items + at, self->items + at + 1, (self->count - at - 1) * sizeof(*self->items));
	--self->count;
	removed->parent = NULL;
	LeaveCriticalSection(&lock);
	if (self->owner)
		changed(self->owner->compositor);
	node_release(&removed->node);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE collection_RemoveAll(void *iface)
{
	struct collection *self = collection_of(iface);
	while (self->count)
		collection_Remove(iface, &self->items[self->count - 1]->node.faces[1]);
	return S_OK;
}

/* ---- brushes, clips and surfaces ---- */
static const WCHAR color_brush_name[] = L"Windows.UI.Composition.CompositionColorBrush";
static const WCHAR surface_brush_name[] = L"Windows.UI.Composition.CompositionSurfaceBrush";
static const WCHAR clip_name[] = L"Windows.UI.Composition.InsetClip";
static const WCHAR surface_name[] = L"Windows.UI.Composition.CompositionSurface";
static const WCHAR device_name[] = L"Windows.UI.Composition.CompositionGraphicsDevice";

static struct brush *brush_of(void *iface)
{
	return (struct brush *)node_of(iface);
}

static void brush_destroy(struct node *base)
{
	struct brush *self = (struct brush *)base;
	node_release(self->surface);
	node_release(&self->compositor->node);
}

static struct brush *make_brush(struct compositor *compositor, int kind)
{
	struct brush *self = ALLOC(sizeof(*self));
	if (!self)
		return NULL;
	node_init(&self->node, kind == BRUSH_COLOR ? color_brush_name : surface_brush_name, brush_destroy);
	node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionObject, ICompositionObject_table);
	node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionBrush, ICompositionBrush_table);
	if (kind == BRUSH_COLOR)
		node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionColorBrush, ICompositionColorBrush_table);
	else
		node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionSurfaceBrush, ICompositionSurfaceBrush_table);
	node_add(&self->node, &IID___x_ABI_CWindows_CFoundation_CIClosable, IClosable_table);
	self->compositor = compositor;
	node_addref(&compositor->node);
	self->kind = kind;
	self->stretch = CompositionStretch_Fill;
	self->horizontal = self->vertical = 0.5f;
	return self;
}

#define BRUSH_PROPERTY(name, field, type) \
	static HRESULT STDMETHODCALLTYPE brush_get_##name(void *iface, type *value) \
	{ \
		if (!value) \
			return E_POINTER; \
		*value = brush_of(iface)->field; \
		return S_OK; \
	} \
	static HRESULT STDMETHODCALLTYPE brush_put_##name(void *iface, type value) \
	{ \
		struct brush *self = brush_of(iface); \
		self->field = value; \
		changed(self->compositor); \
		return S_OK; \
	}

BRUSH_PROPERTY(Color, color, color)
BRUSH_PROPERTY(Stretch, stretch, __x_ABI_CWindows_CUI_CComposition_CCompositionStretch)
BRUSH_PROPERTY(BitmapInterpolationMode, interpolation, __x_ABI_CWindows_CUI_CComposition_CCompositionBitmapInterpolationMode)
BRUSH_PROPERTY(HorizontalAlignmentRatio, horizontal, float)
BRUSH_PROPERTY(VerticalAlignmentRatio, vertical, float)

static HRESULT STDMETHODCALLTYPE brush_get_Surface(void *iface, void **out)
{
	return hand_out(brush_of(iface)->surface, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionSurface, out);
}

static HRESULT STDMETHODCALLTYPE brush_put_Surface(void *iface, void *value)
{
	struct brush *self = brush_of(iface);
	struct node *surface = node_from(value, ICompositionSurface_table);
	if (value && !surface)
		return unimplemented("ICompositionSurfaceBrush", "put_Surface of a surface made elsewhere");
	EnterCriticalSection(&lock);
	if (surface)
		node_addref(surface);
	node_release(self->surface);
	self->surface = surface;
	LeaveCriticalSection(&lock);
	changed(self->compositor);
	return S_OK;
}

static void clip_destroy(struct node *base)
{
	node_release(&((struct clip *)base)->compositor->node);
}

#define CLIP_PROPERTY(name, field) \
	static HRESULT STDMETHODCALLTYPE clip_get_##name(void *iface, float *value) \
	{ \
		if (!value) \
			return E_POINTER; \
		*value = ((struct clip *)node_of(iface))->field; \
		return S_OK; \
	} \
	static HRESULT STDMETHODCALLTYPE clip_put_##name(void *iface, float value) \
	{ \
		struct clip *self = (struct clip *)node_of(iface); \
		self->field = value; \
		changed(self->compositor); \
		return S_OK; \
	}

CLIP_PROPERTY(LeftInset, left)
CLIP_PROPERTY(TopInset, top)
CLIP_PROPERTY(RightInset, right)
CLIP_PROPERTY(BottomInset, bottom)

static void surface_destroy(struct node *base)
{
	struct surface *self = (struct surface *)base, **link;
	EnterCriticalSection(&lock);
	for (link = &watched; *link; link = &(*link)->next_watched)
		if (*link == self) {
			*link = self->next_watched;
			break;
		}
	LeaveCriticalSection(&lock);
	if (self->staging)
		ID3D11Texture2D_Release(self->staging);
	if (self->swap_chain)
		IDXGISwapChain1_Release(self->swap_chain);
	node_release(&self->compositor->node);
}

static void device_destroy(struct node *base)
{
	struct graphics_device *self = (struct graphics_device *)base;
	if (self->device)
		IUnknown_Release(self->device);
	node_release(&self->compositor->node);
}

/* ---- desktop window targets ---- */
static const WCHAR target_name[] = L"Windows.UI.Composition.Desktop.DesktopWindowTarget";
static const WCHAR target_property[] = L"RavenCompositionTarget";

static void target_destroy(struct node *base)
{
	struct target *self = (struct target *)base;
	struct target **link;
	EnterCriticalSection(&lock);
	for (link = &self->compositor->targets; *link; link = &(*link)->next)
		if (*link == self) {
			*link = self->next;
			break;
		}
	if (self->window && IsWindow(self->window) && GetPropW(self->window, target_property) == self) {
		RemovePropW(self->window, target_property);
		if (self->previous)
			SetWindowLongPtrW(self->window, GWLP_WNDPROC, (LONG_PTR)self->previous);
	}
	LeaveCriticalSection(&lock);
	node_release(self->root ? &self->root->node : NULL);
	node_release(&self->compositor->node);
}

static HRESULT STDMETHODCALLTYPE target_get_Root(void *iface, void **out)
{
	struct target *self = (struct target *)node_of(iface);
	return hand_out(self->root ? &self->root->node : NULL, &IID___x_ABI_CWindows_CUI_CComposition_CIVisual, out);
}

static HRESULT STDMETHODCALLTYPE target_put_Root(void *iface, void *value)
{
	struct target *self = (struct target *)node_of(iface);
	struct visual *root = (struct visual *)node_from(value, IVisual_table);
	if (value && !root)
		return E_INVALIDARG;
	EnterCriticalSection(&lock);
	if (root)
		node_addref(&root->node);
	node_release(self->root ? &self->root->node : NULL);
	self->root = root;
	LeaveCriticalSection(&lock);
	changed(self->compositor);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_get_IsTopmost(void *iface, boolean *topmost)
{
	if (!topmost)
		return E_POINTER;
	*topmost = (boolean)((struct target *)node_of(iface))->topmost;
	return S_OK;
}

/* What the window paints itself goes under the visuals: once it has painted, they are drawn over it again. */
static LRESULT CALLBACK target_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	struct target *self = GetPropW(window, target_property);
	WNDPROC previous = self ? self->previous : DefWindowProcW;
	const LRESULT result = CallWindowProcW(previous, window, message, wparam, lparam);
	if (self && self->compositor->committed && (message == WM_PAINT || message == WM_SIZE))
		render(self, FALSE);
	return result;
}

/* ---- the compositor ---- */
static const WCHAR compositor_name[] = L"Windows.UI.Composition.Compositor";
static const WCHAR messages_class[] = L"RavenCompositionMessages";
#define WM_RAVEN_COMMIT (WM_APP + 0x5a1)
#define WM_RAVEN_FRAME (WM_APP + 0x5a2)

static struct compositor *compositor_of(void *iface)
{
	return (struct compositor *)node_of(iface);
}

static void commit(struct compositor *self);

/* Draws every target of the compositor again. */
static void show(struct compositor *self, BOOL frame_only)
{
	struct target **targets, *target;
	struct surface *surface;
	UINT count = 0, index;
	EnterCriticalSection(&lock);
	/* What the frames changed up to here is what this drawing shows; what they change meanwhile waits for the next. */
	for (surface = watched; surface; surface = surface->next_watched)
		if (surface->compositor == self) {
			surface->drawing = surface->changed;
			surface->drawing_whole = surface->changed_whole;
			SetRectEmpty(&surface->changed);
			surface->changed_whole = FALSE;
		}
	for (target = self->targets; target; target = target->next)
		++count;
	if (!(targets = ALLOC(count * sizeof(*targets) + 1))) {
		LeaveCriticalSection(&lock);
		return;
	}
	for (target = self->targets, index = 0; target; target = target->next) {
		node_addref(&target->node);
		targets[index++] = target;
	}
	LeaveCriticalSection(&lock);
	for (index = 0; index < count; ++index) {
		render(targets[index], frame_only);
		node_release(&targets[index]->node);
	}
	FREE(targets);
}

static void commit(struct compositor *self)
{
	EnterCriticalSection(&lock);
	self->pending = FALSE;
	self->committed = TRUE;
	LeaveCriticalSection(&lock);
	show(self, FALSE);
}

/* Changes are announced once until the next commit: through the controller's CommitNeeded event when there is a controller, or by a
 * commit of their own once the thread is back at its messages. */
static void raise_commit_needed(struct controller *controller);

static LRESULT CALLBACK messages_window(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_RAVEN_COMMIT) {
		struct compositor *self = (struct compositor *)lparam;
		if (self->controller)
			raise_commit_needed(self->controller);
		else
			commit(self);
		node_release(&self->node);
		return 0;
	}
	if (message == WM_RAVEN_FRAME) {
		struct compositor *self = (struct compositor *)lparam;
		InterlockedExchange(&self->frame_pending, FALSE);
		show(self, TRUE);
		node_release(&self->node);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

static void changed(struct compositor *self)
{
	if (InterlockedCompareExchange((LONG *)&self->pending, TRUE, FALSE))
		return;
	node_addref(&self->node);
	if (!self->messages || !PostMessageW(self->messages, WM_RAVEN_COMMIT, 0, (LPARAM)self))
		node_release(&self->node);
}

static void compositor_destroy(struct node *base)
{
	struct compositor *self = (struct compositor *)base;
	if (self->messages)
		DestroyWindow(self->messages);
}

static struct compositor *make_compositor(void)
{
	static LONG registered;
	struct compositor *self = ALLOC(sizeof(*self));
	if (!self)
		return NULL;
	if (!InterlockedExchange(&registered, 1)) {
		WNDCLASSW type = {0};
		type.lpfnWndProc = messages_window;
		type.hInstance = GetModuleHandleW(NULL);
		type.lpszClassName = messages_class;
		RegisterClassW(&type);
	}
	node_init(&self->node, compositor_name, compositor_destroy);
	node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositor, ICompositor_table);
	node_add(&self->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositorInterop, ICompositorInterop_table);
	node_add(&self->node, &IID_ICompositorDesktopInterop, ICompositorDesktopInterop_table);
	node_add(&self->node, &IID___x_ABI_CWindows_CFoundation_CIClosable, IClosable_table);
	self->messages = CreateWindowExW(0, messages_class, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
	return self;
}

static HRESULT STDMETHODCALLTYPE compositor_CreateContainerVisual(void *iface, void **out)
{
	struct visual *visual = make_visual(compositor_of(iface), FALSE);
	HRESULT hr;
	if (!visual)
		return E_OUTOFMEMORY;
	hr = hand_out(&visual->node, &IID___x_ABI_CWindows_CUI_CComposition_CIContainerVisual, out);
	node_release(&visual->node);
	return hr;
}

static HRESULT STDMETHODCALLTYPE compositor_CreateSpriteVisual(void *iface, void **out)
{
	struct visual *visual = make_visual(compositor_of(iface), TRUE);
	HRESULT hr;
	if (!visual)
		return E_OUTOFMEMORY;
	hr = hand_out(&visual->node, &IID___x_ABI_CWindows_CUI_CComposition_CISpriteVisual, out);
	node_release(&visual->node);
	return hr;
}

static HRESULT STDMETHODCALLTYPE compositor_CreateColorBrushWithColor(void *iface, color value, void **out)
{
	struct brush *brush = make_brush(compositor_of(iface), BRUSH_COLOR);
	HRESULT hr;
	if (!brush)
		return E_OUTOFMEMORY;
	brush->color = value;
	hr = hand_out(&brush->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionColorBrush, out);
	node_release(&brush->node);
	return hr;
}

static HRESULT STDMETHODCALLTYPE compositor_CreateColorBrush(void *iface, void **out)
{
	const color white = {255, 255, 255, 255};
	return compositor_CreateColorBrushWithColor(iface, white, out);
}

static HRESULT STDMETHODCALLTYPE compositor_CreateSurfaceBrushWithSurface(void *iface, void *surface, void **out)
{
	struct brush *brush = make_brush(compositor_of(iface), BRUSH_SURFACE);
	HRESULT hr;
	if (!brush)
		return E_OUTOFMEMORY;
	if (surface && FAILED(hr = brush_put_Surface(&brush->node.faces[3], surface))) {
		node_release(&brush->node);
		return hr;
	}
	hr = hand_out(&brush->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionSurfaceBrush, out);
	node_release(&brush->node);
	return hr;
}

static HRESULT STDMETHODCALLTYPE compositor_CreateSurfaceBrush(void *iface, void **out)
{
	return compositor_CreateSurfaceBrushWithSurface(iface, NULL, out);
}

static HRESULT STDMETHODCALLTYPE compositor_CreateInsetClipWithInsets(void *iface, float left, float top, float right, float bottom, void **out)
{
	struct compositor *compositor = compositor_of(iface);
	struct clip *clip = ALLOC(sizeof(*clip));
	HRESULT hr;
	if (!clip)
		return E_OUTOFMEMORY;
	node_init(&clip->node, clip_name, clip_destroy);
	node_add(&clip->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionObject, ICompositionObject_table);
	node_add(&clip->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionClip, ICompositionClip_table);
	node_add(&clip->node, &IID___x_ABI_CWindows_CUI_CComposition_CIInsetClip, IInsetClip_table);
	node_add(&clip->node, &IID___x_ABI_CWindows_CFoundation_CIClosable, IClosable_table);
	clip->compositor = compositor;
	node_addref(&compositor->node);
	clip->left = left;
	clip->top = top;
	clip->right = right;
	clip->bottom = bottom;
	hr = hand_out(&clip->node, &IID___x_ABI_CWindows_CUI_CComposition_CIInsetClip, out);
	node_release(&clip->node);
	return hr;
}

static HRESULT STDMETHODCALLTYPE compositor_CreateInsetClip(void *iface, void **out)
{
	return compositor_CreateInsetClipWithInsets(iface, 0, 0, 0, 0, out);
}

/* The program's lock on its device's immediate context, which its own threads share with this one; null when the device has none. */
static ID3D10Multithread *hold_device(ID3D11Device *device)
{
	ID3D10Multithread *guard = NULL;
	if (FAILED(ID3D11Device_QueryInterface(device, &IID_ID3D10Multithread, (void **)&guard)))
		return NULL;
	if (!ID3D10Multithread_GetMultithreadProtected(guard))
		ID3D10Multithread_SetMultithreadProtected(guard, TRUE);
	ID3D10Multithread_Enter(guard);
	return guard;
}

static void release_device(ID3D10Multithread *guard)
{
	if (guard) {
		ID3D10Multithread_Leave(guard);
		ID3D10Multithread_Release(guard);
	}
}

/* The desktop compositor takes the back buffer as the program presents it, so the frame is copied out at that moment: afterwards the
 * Direct3D libraries may hand the same buffer back to the program to draw the next frame into. */
static void capture_frame(struct surface *surface, const RECT *dirty)
{
	ID3D11Texture2D *buffer = NULL;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	ID3D10Multithread *guard;
	D3D11_TEXTURE2D_DESC desc;
	BOOL fresh = FALSE;
	if (FAILED(IDXGISwapChain1_GetBuffer(surface->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&buffer)))
		return;
	ID3D11Texture2D_GetDesc(buffer, &desc);
	ID3D11Texture2D_GetDevice(buffer, &device);
	ID3D11Device_GetImmediateContext(device, &context);
	guard = hold_device(device);
	if (!surface->staging || surface->staging_width != desc.Width || surface->staging_height != desc.Height) {
		D3D11_TEXTURE2D_DESC staging = desc;
		fresh = TRUE;
		if (surface->staging)
			ID3D11Texture2D_Release(surface->staging);
		surface->staging = NULL;
		staging.Usage = D3D11_USAGE_STAGING;
		staging.BindFlags = 0;
		staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		staging.MiscFlags = 0;
		staging.MipLevels = staging.ArraySize = 1;
		staging.SampleDesc.Count = 1;
		staging.SampleDesc.Quality = 0;
		if (SUCCEEDED(ID3D11Device_CreateTexture2D(device, &staging, NULL, &surface->staging))) {
			surface->staging_width = desc.Width;
			surface->staging_height = desc.Height;
		}
	}
	if (surface->staging) {
		/* A present that names what changed leaves the rest of the frame as the copy already has it. */
		RECT part = {0, 0, (LONG)desc.Width, (LONG)desc.Height};
		const BOOL whole = !dirty || fresh || !IntersectRect(&part, &part, dirty);
		if (whole) {
			ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)surface->staging, (ID3D11Resource *)buffer);
		} else {
			const D3D11_BOX box = {(UINT)part.left, (UINT)part.top, 0, (UINT)part.right, (UINT)part.bottom, 1};
			ID3D11DeviceContext_CopySubresourceRegion(context, (ID3D11Resource *)surface->staging, 0, box.left, box.top, 0, (ID3D11Resource *)buffer, 0,
				&box);
		}
		EnterCriticalSection(&lock);
		if (whole)
			surface->changed_whole = TRUE;
		else
			UnionRect(&surface->changed, &surface->changed, &part);
		LeaveCriticalSection(&lock);
	}
	release_device(guard);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
	ID3D11Texture2D_Release(buffer);
}

/* The surface over a swap chain, with a reference, or null. */
static struct surface *surface_of_chain(IDXGISwapChain1 *chain)
{
	struct surface *surface;
	EnterCriticalSection(&lock);
	for (surface = watched; surface && surface->swap_chain != chain; surface = surface->next_watched)
		;
	if (surface)
		node_addref(&surface->node);
	LeaveCriticalSection(&lock);
	return surface;
}

/* A frame presented shows without a commit: the compositor that shows the surface draws its targets again on its own thread. */
static void show_frame(struct surface *surface)
{
	struct compositor *compositor = surface->compositor;
	if (!compositor->committed || !compositor->messages || InterlockedCompareExchange(&compositor->frame_pending, TRUE, FALSE))
		return;
	node_addref(&compositor->node);
	if (!PostMessageW(compositor->messages, WM_RAVEN_FRAME, 0, (LPARAM)compositor)) {
		InterlockedExchange(&compositor->frame_pending, FALSE);
		node_release(&compositor->node);
	}
}

typedef HRESULT (STDMETHODCALLTYPE *present_function)(IDXGISwapChain1 *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *present1_function)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

/* The present calls of each kind of swap chain shown in a surface, as they were before they were taken over. */
static struct presenter {
	void **table;
	present_function present;
	present1_function present1;
} presenters[8];

static const struct presenter *presenter_of(IDXGISwapChain1 *chain)
{
	UINT index;
	for (index = 0; index < ARRAYSIZE(presenters) && presenters[index].table; ++index)
		if (presenters[index].table == *(void ***)chain)
			return &presenters[index];
	return NULL;
}

static HRESULT STDMETHODCALLTYPE surface_present(IDXGISwapChain1 *chain, UINT interval, UINT flags)
{
	const struct presenter *presenter = presenter_of(chain);
	struct surface *surface = flags & DXGI_PRESENT_TEST ? NULL : surface_of_chain(chain);
	HRESULT hr;
	if (surface)
		capture_frame(surface, NULL);
	hr = presenter->present(chain, interval, flags);
	if (surface) {
		if (SUCCEEDED(hr))
			show_frame(surface);
		node_release(&surface->node);
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE surface_present1(IDXGISwapChain1 *chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS *parameters)
{
	const struct presenter *presenter = presenter_of(chain);
	struct surface *surface = flags & DXGI_PRESENT_TEST ? NULL : surface_of_chain(chain);
	HRESULT hr;
	if (surface) {
		/* The rectangles a present names are all of the frame that changed; none named means all of it did. */
		RECT dirty = {0, 0, 0, 0};
		UINT index;
		for (index = 0; parameters && parameters->pDirtyRects && index < parameters->DirtyRectsCount; ++index)
			UnionRect(&dirty, &dirty, &parameters->pDirtyRects[index]);
		capture_frame(surface, IsRectEmpty(&dirty) ? NULL : &dirty);
	}
	hr = presenter->present1(chain, interval, flags, parameters);
	if (surface) {
		if (SUCCEEDED(hr))
			show_frame(surface);
		node_release(&surface->node);
	}
	return hr;
}

/* The swap chain's present calls are taken over once per kind of swap chain, to see each frame the program presents. */
static void watch_presents(struct surface *surface)
{
	const UINT present_slot = offsetof(IDXGISwapChain1Vtbl, Present) / sizeof(void *);
	const UINT present1_slot = offsetof(IDXGISwapChain1Vtbl, Present1) / sizeof(void *);
	void **table = *(void ***)surface->swap_chain;
	UINT index;
	DWORD old;
	EnterCriticalSection(&lock);
	surface->next_watched = watched;
	watched = surface;
	for (index = 0; index < ARRAYSIZE(presenters) && presenters[index].table && presenters[index].table != table; ++index)
		;
	if (index < ARRAYSIZE(presenters) && !presenters[index].table &&
		VirtualProtect(&table[present_slot], (present1_slot - present_slot + 1) * sizeof(void *), PAGE_READWRITE, &old)) {
		presenters[index].present = (present_function)table[present_slot];
		presenters[index].present1 = (present1_function)table[present1_slot];
		presenters[index].table = table;
		table[present_slot] = (void *)surface_present;
		table[present1_slot] = (void *)surface_present1;
		VirtualProtect(&table[present_slot], (present1_slot - present_slot + 1) * sizeof(void *), old, &old);
	}
	LeaveCriticalSection(&lock);
}

static HRESULT STDMETHODCALLTYPE interop_CreateCompositionSurfaceForSwapChain(void *iface, IUnknown *swap_chain, void **out)
{
	struct compositor *compositor = compositor_of(iface);
	struct surface *surface;
	IDXGISwapChain1 *chain = NULL;
	UINT presented = 0;
	HRESULT hr;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!swap_chain || FAILED(IUnknown_QueryInterface(swap_chain, &IID_IDXGISwapChain1, (void **)&chain)))
		return E_INVALIDARG;
	if (!(surface = ALLOC(sizeof(*surface)))) {
		IDXGISwapChain1_Release(chain);
		return E_OUTOFMEMORY;
	}
	node_init(&surface->node, surface_name, surface_destroy);
	node_add(&surface->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionSurface, ICompositionSurface_table);
	node_add(&surface->node, &IID___x_ABI_CWindows_CFoundation_CIClosable, IClosable_table);
	surface->compositor = compositor;
	node_addref(&compositor->node);
	surface->swap_chain = chain;
	watch_presents(surface);
	/* A swap chain that presented before the surface was made shows that frame, which buffer 0 still holds. */
	if (SUCCEEDED(IDXGISwapChain1_GetLastPresentCount(chain, &presented)) && presented)
		capture_frame(surface, NULL);
	hr = hand_out(&surface->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionSurface, out);
	node_release(&surface->node);
	return hr;
}

static HRESULT STDMETHODCALLTYPE interop_CreateGraphicsDevice(void *iface, IUnknown *rendering, void **out)
{
	struct compositor *compositor = compositor_of(iface);
	struct graphics_device *device;
	HRESULT hr;
	if (!rendering)
		return E_INVALIDARG;
	if (!(device = ALLOC(sizeof(*device))))
		return E_OUTOFMEMORY;
	node_init(&device->node, device_name, device_destroy);
	node_add(&device->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionObject, ICompositionObject_table);
	node_add(&device->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionGraphicsDevice, ICompositionGraphicsDevice_table);
	node_add(&device->node, &IID___x_ABI_CWindows_CFoundation_CIClosable, IClosable_table);
	device->compositor = compositor;
	node_addref(&compositor->node);
	device->device = rendering;
	IUnknown_AddRef(rendering);
	hr = hand_out(&device->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionGraphicsDevice, out);
	node_release(&device->node);
	return hr;
}

static HRESULT STDMETHODCALLTYPE desktop_CreateDesktopWindowTarget(void *iface, HWND window, BOOL topmost, void **out)
{
	struct compositor *compositor = compositor_of(iface);
	struct target *target;
	HRESULT hr;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsWindow(window))
		return E_INVALIDARG;
	if (GetPropW(window, target_property))
		return E_ACCESSDENIED;
	if (!(target = ALLOC(sizeof(*target))))
		return E_OUTOFMEMORY;
	node_init(&target->node, target_name, target_destroy);
	node_add(&target->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionObject, ICompositionObject_table);
	node_add(&target->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositionTarget, ICompositionTarget_table);
	node_add(&target->node, &IID_IDesktopWindowTarget, IDesktopWindowTarget_table);
	node_add(&target->node, &IID___x_ABI_CWindows_CFoundation_CIClosable, IClosable_table);
	target->compositor = compositor;
	node_addref(&compositor->node);
	target->window = window;
	target->topmost = topmost;
	EnterCriticalSection(&lock);
	SetPropW(window, target_property, target);
	target->previous = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)target_window);
	target->next = compositor->targets;
	compositor->targets = target;
	LeaveCriticalSection(&lock);
	hr = hand_out(&target->node, &IID_IDesktopWindowTarget, out);
	node_release(&target->node);
	return hr;
}

static HRESULT STDMETHODCALLTYPE desktop_EnsureOnThread(void *iface, DWORD thread)
{
	return S_OK;
}

/* ---- the controller ---- */
static const WCHAR controller_name[] = L"Windows.UI.Composition.Core.CompositorController";

static void controller_destroy(struct node *base)
{
	struct controller *self = (struct controller *)base;
	UINT index;
	for (index = 0; index < ARRAYSIZE(self->handlers); ++index)
		if (self->handlers[index].handler)
			IUnknown_Release(self->handlers[index].handler);
	self->compositor->controller = NULL;
	node_release(&self->compositor->node);
}

static void raise_commit_needed(struct controller *self)
{
	IUnknown *handlers[ARRAYSIZE(self->handlers)];
	UINT index, count = 0;
	EnterCriticalSection(&lock);
	for (index = 0; index < ARRAYSIZE(self->handlers); ++index)
		if (self->handlers[index].handler) {
			handlers[count] = self->handlers[index].handler;
			IUnknown_AddRef(handlers[count]);
			++count;
		}
	LeaveCriticalSection(&lock);
	for (index = 0; index < count; ++index) {
		typedef HRESULT (STDMETHODCALLTYPE *invoke)(IUnknown *, void *, IInspectable *);
		((invoke)((void **)handlers[index]->lpVtbl)[3])(handlers[index], &self->node.faces[0], NULL);
		IUnknown_Release(handlers[index]);
	}
}

static HRESULT STDMETHODCALLTYPE controller_get_Compositor(void *iface, void **out)
{
	struct controller *self = (struct controller *)node_of(iface);
	return hand_out(&self->compositor->node, &IID___x_ABI_CWindows_CUI_CComposition_CICompositor, out);
}

static HRESULT STDMETHODCALLTYPE controller_Commit(void *iface)
{
	commit(((struct controller *)node_of(iface))->compositor);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE controller_add_CommitNeeded(void *iface, IUnknown *handler, EventRegistrationToken *token)
{
	struct controller *self = (struct controller *)node_of(iface);
	UINT index;
	if (!handler || !token)
		return E_POINTER;
	EnterCriticalSection(&lock);
	for (index = 0; index < ARRAYSIZE(self->handlers); ++index)
		if (!self->handlers[index].handler) {
			IUnknown_AddRef(handler);
			self->handlers[index].handler = handler;
			self->handlers[index].token = ++self->next_token;
			token->value = self->handlers[index].token;
			LeaveCriticalSection(&lock);
			return S_OK;
		}
	LeaveCriticalSection(&lock);
	return E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE controller_remove_CommitNeeded(void *iface, EventRegistrationToken token)
{
	struct controller *self = (struct controller *)node_of(iface);
	IUnknown *removed = NULL;
	UINT index;
	EnterCriticalSection(&lock);
	for (index = 0; index < ARRAYSIZE(self->handlers); ++index)
		if (self->handlers[index].handler && self->handlers[index].token == token.value) {
			removed = self->handlers[index].handler;
			self->handlers[index].handler = NULL;
		}
	LeaveCriticalSection(&lock);
	if (removed)
		IUnknown_Release(removed);
	return S_OK;
}

static struct controller *make_controller(void)
{
	struct controller *self = ALLOC(sizeof(*self));
	if (!self)
		return NULL;
	if (!(self->compositor = make_compositor())) {
		FREE(self);
		return NULL;
	}
	node_init(&self->node, controller_name, controller_destroy);
	node_add(&self->node, &IID_ICompositorController, ICompositorController_table);
	node_add(&self->node, &IID___x_ABI_CWindows_CFoundation_CIClosable, IClosable_table);
	self->compositor->controller = self;
	return self;
}

/* ---- drawing a committed tree into a window ----
 * The visuals are drawn from the root down, each child over its parent and over the siblings below it, in the window's client
 * coordinates: an offset moves a visual and its children, a scale sizes them, opacity fades them and an inset clip trims them.
 * Rotations and full transform matrices are not drawn.
 *
 * The tree is read under the lock into a list of fills; the pixels of swap chains are then read without it, since the thread that
 * draws into a swap chain may hold its device while it changes the tree. */
struct fill {
	RECT area; /* the part of the window it covers */
	float left, top, width, height, opacity;
	color color;
	struct surface *surface; /* with a reference, or NULL for a colour */
	__x_ABI_CWindows_CUI_CComposition_CCompositionStretch stretch;
	float horizontal, vertical;
};

struct fills {
	struct fill *items;
	UINT count, capacity;
	RECT drawn; /* what the visuals cover: only that goes into the window, so a tree with nothing to show leaves it as it is */
};

static void collect(struct fills *fills, struct visual *visual, float origin_x, float origin_y, float scale_x, float scale_y, const RECT *clip,
	float opacity)
{
	float left, top, width, height;
	RECT area, visible;
	UINT index;
	if (!visual->visible || visual->opacity <= 0)
		return;
	opacity *= visual->opacity;
	left = origin_x + (visual->offset.X - visual->anchor.X * visual->size.X) * scale_x;
	top = origin_y + (visual->offset.Y - visual->anchor.Y * visual->size.Y) * scale_y;
	scale_x *= visual->scale.X;
	scale_y *= visual->scale.Y;
	width = visual->size.X * scale_x;
	height = visual->size.Y * scale_y;
	visible = *clip;
	if (visual->clip) {
		RECT inset;
		inset.left = (LONG)(left + visual->clip->left * scale_x);
		inset.top = (LONG)(top + visual->clip->top * scale_y);
		inset.right = (LONG)(left + width - visual->clip->right * scale_x);
		inset.bottom = (LONG)(top + height - visual->clip->bottom * scale_y);
		if (!IntersectRect(&visible, &visible, &inset))
			return;
	}
	area.left = (LONG)(left + 0.5f);
	area.top = (LONG)(top + 0.5f);
	area.right = (LONG)(left + width + 0.5f);
	area.bottom = (LONG)(top + height + 0.5f);
	if (visual->sprite && visual->brush && IntersectRect(&area, &area, &visible) &&
		(visual->brush->kind == BRUSH_COLOR || (visual->brush->surface && visual->brush->surface->destroy == surface_destroy))) {
		struct fill *fill;
		if (fills->count == fills->capacity) {
			const UINT capacity = fills->capacity ? fills->capacity * 2 : 16;
			struct fill *grown = GROW(fills->items, capacity * sizeof(*grown));
			if (!grown)
				return;
			fills->items = grown;
			fills->capacity = capacity;
		}
		fill = &fills->items[fills->count++];
		fill->area = area;
		fill->left = left;
		fill->top = top;
		fill->width = width;
		fill->height = height;
		fill->opacity = opacity;
		fill->color = visual->brush->color;
		fill->surface = visual->brush->kind == BRUSH_SURFACE ? (struct surface *)visual->brush->surface : NULL;
		if (fill->surface)
			node_addref(&fill->surface->node);
		fill->stretch = visual->brush->stretch;
		fill->horizontal = visual->brush->horizontal;
		fill->vertical = visual->brush->vertical;
		UnionRect(&fills->drawn, &fills->drawn, &area);
	}
	for (index = 0; index < visual->children->count; ++index)
		collect(fills, visual->children->items[index], left, top, scale_x, scale_y, &visible, opacity);
}

struct canvas {
	DWORD *pixels;
	INT width, height;
};

static BYTE scale_byte(UINT value, UINT by)
{
	return (BYTE)((value * by + 127) / 255);
}

/* Premultiplied source over the canvas. */
static void blend_pixel(DWORD *to, DWORD from)
{
	const UINT alpha = from >> 24, keep = 255 - alpha;
	DWORD below = *to, result;
	if (!alpha)
		return;
	if (alpha == 255) {
		*to = from;
		return;
	}
	result = (DWORD)(alpha + scale_byte(below >> 24, keep)) << 24;
	result |= (DWORD)(((from >> 16) & 255) + scale_byte((below >> 16) & 255, keep)) << 16;
	result |= (DWORD)(((from >> 8) & 255) + scale_byte((below >> 8) & 255, keep)) << 8;
	result |= (DWORD)((from & 255) + scale_byte(below & 255, keep));
	*to = result;
}

static DWORD fade(DWORD pixel, UINT opacity)
{
	if (opacity >= 255)
		return pixel;
	return (DWORD)scale_byte(pixel >> 24, opacity) << 24 | (DWORD)scale_byte((pixel >> 16) & 255, opacity) << 16 |
		(DWORD)scale_byte((pixel >> 8) & 255, opacity) << 8 | scale_byte(pixel & 255, opacity);
}

static void fill_color(struct canvas *canvas, const struct fill *fill)
{
	const UINT alpha = (UINT)(fill->color.A * fill->opacity + 0.5f);
	const DWORD pixel = (DWORD)alpha << 24 | (DWORD)scale_byte(fill->color.R, alpha) << 16 | (DWORD)scale_byte(fill->color.G, alpha) << 8 |
		scale_byte(fill->color.B, alpha);
	INT x, y;
	for (y = fill->area.top; y < fill->area.bottom; ++y)
		for (x = fill->area.left; x < fill->area.right; ++x)
			blend_pixel(&canvas->pixels[y * canvas->width + x], pixel);
}

/* ---- half-float swap chains ----
 * A swap chain of half floats holds scRGB: linear light, premultiplied, where 1 is the white of sRGB. The screen shows sRGB, so each value is
 * brought through the sRGB curve, from a table of 4096 steps of linear light; what lies above white is shown as white. */
enum { LINEAR_STEPS = 4096 };
static BYTE linear_srgb[LINEAR_STEPS + 1];
static LONG linear_srgb_ready;

static float half_float(UINT16 half)
{
	union { UINT32 u; float f; } bits;
	const UINT32 sign = (UINT32)(half & 0x8000) << 16, exponent = (half >> 10) & 31, mantissa = half & 0x3ff;
	if (!exponent) {
		/* A subnormal half is its mantissa times 2^-24. */
		bits.f = mantissa * (1.0f / 16777216.0f);
		bits.u |= sign;
		return bits.f;
	}
	bits.u = exponent == 31 ? sign | 0x7f800000 | (mantissa << 13) : sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
	return bits.f;
}

/* x to the power 1/2.4 for x in (0, 1], from the logarithm and exponential by their series. */
static float root_2_4(float x)
{
	union { float f; UINT32 u; } bits = {x}, scale;
	const int exponent = (int)((bits.u >> 23) & 255) - 127;
	float t, t2, ln, y, fraction, term = 1, sum = 1;
	int whole, step;
	bits.u = (bits.u & 0x7fffff) | 0x3f800000;
	t = (bits.f - 1) / (bits.f + 1);
	t2 = t * t;
	ln = 2 * t * (1 + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 * (1.0f / 9 + t2 / 11)))));
	y = (exponent + ln * 1.44269504f) / 2.4f;
	whole = (int)y - (y < (int)y);
	fraction = (y - whole) * 0.69314718f;
	for (step = 1; step < 9; ++step) {
		term *= fraction / step;
		sum += term;
	}
	scale.u = (UINT32)(whole + 127) << 23;
	return sum * scale.f;
}

static void make_linear_srgb(void)
{
	UINT index;
	for (index = 0; index <= LINEAR_STEPS; ++index) {
		const float linear = (float)index / LINEAR_STEPS;
		const float srgb = linear <= 0.0031308f ? linear * 12.92f : 1.055f * root_2_4(linear) - 0.055f;
		linear_srgb[index] = (BYTE)(srgb * 255 + 0.5f);
	}
}

static BYTE srgb_of(float linear)
{
	return linear <= 0 ? 0 : linear >= 1 ? 255 : linear_srgb[(UINT)(linear * LINEAR_STEPS + 0.5f)];
}

/* A premultiplied scRGB pixel as a premultiplied 8-bit sRGB one. */
static DWORD scrgb_pixel(const UINT16 *halves)
{
	const float alpha = half_float(halves[3]);
	const UINT a = alpha <= 0 ? 0 : alpha >= 1 ? 255 : (UINT)(alpha * 255 + 0.5f);
	const float unpremultiply = alpha > 0 ? 1 / alpha : 0;
	if (!a)
		return 0;
	return (DWORD)a << 24 | (DWORD)scale_byte(srgb_of(half_float(halves[0]) * unpremultiply), a) << 16 |
		(DWORD)scale_byte(srgb_of(half_float(halves[1]) * unpremultiply), a) << 8 | scale_byte(srgb_of(half_float(halves[2]) * unpremultiply), a);
}

/* Where a frame of a size falls in the window, as the brush places it: stretched to fill the visual, kept to scale inside or over it, or
 * left at its size. */
static void place_frame(const struct fill *fill, UINT width, UINT height, float *x0, float *y0, float *sx, float *sy)
{
	*sx = fill->width / (float)width;
	*sy = fill->height / (float)height;
	if (fill->stretch == CompositionStretch_None)
		*sx = *sy = 1;
	else if (fill->stretch == CompositionStretch_Uniform)
		*sx = *sy = *sx < *sy ? *sx : *sy;
	else if (fill->stretch == CompositionStretch_UniformToFill)
		*sx = *sy = *sx > *sy ? *sx : *sy;
	*x0 = fill->left + (fill->width - width * *sx) * fill->horizontal;
	*y0 = fill->top + (fill->height - height * *sy) * fill->vertical;
}

/* The frame the swap chain last presented, read from its staging copy while the device is held, drawn into the visual's rectangle as the brush
 * places it: stretched to fill it, kept to scale inside or over it, or left at its size. */
static void draw_swap_chain(struct canvas *canvas, const struct fill *fill)
{
	struct surface *surface = fill->surface;
	ID3D11Texture2D *buffer = NULL;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	ID3D10Multithread *guard;
	D3D11_TEXTURE2D_DESC desc;
	D3D11_MAPPED_SUBRESOURCE mapped;
	float x0, y0, sx, sy;
	INT x, y;
	/* What shows is the frame taken when the program last presented; before its first present nothing does. */
	if (FAILED(IDXGISwapChain1_GetBuffer(surface->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&buffer)))
		return;
	ID3D11Texture2D_GetDesc(buffer, &desc);
	if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
		static LONG reported;
		if (!InterlockedExchange(&reported, 1))
			unimplemented("ICompositionSurface", "drawing a swap chain that is neither 8-bit BGRA nor half-float RGBA");
		ID3D11Texture2D_Release(buffer);
		return;
	}
	/* Two threads making the table at once write the same values. */
	if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && !linear_srgb_ready) {
		make_linear_srgb();
		InterlockedExchange(&linear_srgb_ready, 1);
	}
	ID3D11Texture2D_GetDevice(buffer, &device);
	ID3D11Device_GetImmediateContext(device, &context);
	/* The program's own threads use the same immediate context, so it is used here only while the device's lock is held. */
	guard = hold_device(device);
	if (surface->staging && surface->staging_width == desc.Width && surface->staging_height == desc.Height) {
		if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)surface->staging, 0, D3D11_MAP_READ, 0, &mapped))) {
			const UINT alpha = (UINT)(fill->opacity * 255 + 0.5f);
			place_frame(fill, desc.Width, desc.Height, &x0, &y0, &sx, &sy);
			for (y = fill->area.top; y < fill->area.bottom; ++y) {
				const INT source_y = (INT)((y + 0.5f - y0) / sy);
				const BYTE *row;
				if (source_y < 0 || source_y >= (INT)desc.Height)
					continue;
				row = (const BYTE *)mapped.pData + (size_t)source_y * mapped.RowPitch;
				for (x = fill->area.left; x < fill->area.right; ++x) {
					const INT source_x = (INT)((x + 0.5f - x0) / sx);
					if (source_x < 0 || source_x >= (INT)desc.Width)
						continue;
					blend_pixel(&canvas->pixels[y * canvas->width + x], fade(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ?
						scrgb_pixel((const UINT16 *)row + (size_t)source_x * 4) : ((const DWORD *)row)[source_x], alpha));
				}
			}
				ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)surface->staging, 0);
		}
	}
	release_device(guard);
	ID3D11DeviceContext_Release(context);
	ID3D11Device_Release(device);
	ID3D11Texture2D_Release(buffer);
}

/* Draws a target's visuals into its window. For a frame presented with nothing else changed only the part of the window the changed part
 * of each frame falls on is drawn again, as the desktop compositor takes the rectangles a present names. */
static void render(struct target *target, BOOL frame_only)
{
	struct fills fills = {0};
	struct canvas canvas;
	BITMAPINFO info = {{sizeof(info.bmiHeader)}};
	RECT client, everything;
	HDC window_dc, memory_dc;
	HBITMAP bitmap, previous;
	BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	void *bits = NULL;
	UINT index;
	if (!IsWindow(target->window) || !GetClientRect(target->window, &client) || client.right <= 0 || client.bottom <= 0)
		return;
	everything = client;
	EnterCriticalSection(&lock);
	if (target->root)
		collect(&fills, target->root, 0, 0, 1, 1, &everything, 1);
	if (frame_only) {
		RECT fresh = {0, 0, 0, 0};
		for (index = 0; index < fills.count; ++index) {
			const struct fill *fill = &fills.items[index];
			const struct surface *surface = fill->surface;
			RECT part = fill->area;
			float x0, y0, sx, sy;
			if (!surface || (!surface->drawing_whole && IsRectEmpty(&surface->drawing)))
				continue;
			if (!surface->drawing_whole && surface->staging_width && surface->staging_height) {
				place_frame(fill, surface->staging_width, surface->staging_height, &x0, &y0, &sx, &sy);
				part.left = (LONG)(x0 + surface->drawing.left * sx) - 1;
				part.top = (LONG)(y0 + surface->drawing.top * sy) - 1;
				part.right = (LONG)(x0 + surface->drawing.right * sx) + 2;
				part.bottom = (LONG)(y0 + surface->drawing.bottom * sy) + 2;
				IntersectRect(&part, &part, &fill->area);
			}
			UnionRect(&fresh, &fresh, &part);
		}
		/* Every visual is drawn over that part, in its order, so what lies under and over the frame shows as before. */
		for (index = 0; index < fills.count; ++index)
			IntersectRect(&fills.items[index].area, &fills.items[index].area, &fresh);
		IntersectRect(&fills.drawn, &fills.drawn, &fresh);
	}
	LeaveCriticalSection(&lock);
	if (!IsRectEmpty(&fills.drawn)) {
		info.bmiHeader.biWidth = client.right;
		info.bmiHeader.biHeight = -client.bottom;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		window_dc = GetDC(target->window);
		memory_dc = CreateCompatibleDC(window_dc);
		bitmap = CreateDIBSection(window_dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
		if (bitmap && bits) {
			const RECT *drawn = &fills.drawn;
			canvas.pixels = bits;
			canvas.width = client.right;
			canvas.height = client.bottom;
			for (index = 0; index < fills.count; ++index)
				if (fills.items[index].surface)
					draw_swap_chain(&canvas, &fills.items[index]);
				else
					fill_color(&canvas, &fills.items[index]);
			previous = SelectObject(memory_dc, bitmap);
			AlphaBlend(window_dc, drawn->left, drawn->top, drawn->right - drawn->left, drawn->bottom - drawn->top, memory_dc, drawn->left,
				drawn->top, drawn->right - drawn->left, drawn->bottom - drawn->top, blend);
			SelectObject(memory_dc, previous);
		}
		if (bitmap)
			DeleteObject(bitmap);
		DeleteDC(memory_dc);
		ReleaseDC(target->window, window_dc);
	}
	for (index = 0; index < fills.count; ++index)
		if (fills.items[index].surface)
			node_release(&fills.items[index].surface->node);
	FREE(fills.items);
}

/* ---- tables ---- */
static void build_tables(void)
{
	void **inspectable[] = {ICompositionObject_table, ICompositor_table, IVisual_table, IContainerVisual_table, ISpriteVisual_table,
		IVisualCollection_table, ICompositionBrush_table, ICompositionColorBrush_table, ICompositionSurfaceBrush_table,
		ICompositionSurface_table, ICompositionTarget_table, ICompositionGraphicsDevice_table, ICompositionDrawingSurface_table,
		ICompositionClip_table, IInsetClip_table, ICompositionPropertySet_table, ICompositionScopedBatch_table, IClosable_table,
		ICompositorController_table, IDesktopWindowTarget_table};
	UINT index;
	for (index = 0; index < ARRAYSIZE(inspectable); ++index)
		set_inspectable(inspectable[index]);
	set_unknown(ICompositorInterop_table);
	set_unknown(ICompositorDesktopInterop_table);

	ICompositionObject_table[ICompositionObject_get_Compositor_slot] = (void *)object_get_Compositor;
	ICompositionObject_table[ICompositionObject_get_Dispatcher_slot] = (void *)object_get_Dispatcher;
	IClosable_table[IClosable_Close_slot] = (void *)closable_Close;

	ICompositor_table[ICompositor_CreateColorBrush_slot] = (void *)compositor_CreateColorBrush;
	ICompositor_table[ICompositor_CreateColorBrushWithColor_slot] = (void *)compositor_CreateColorBrushWithColor;
	ICompositor_table[ICompositor_CreateContainerVisual_slot] = (void *)compositor_CreateContainerVisual;
	ICompositor_table[ICompositor_CreateSpriteVisual_slot] = (void *)compositor_CreateSpriteVisual;
	ICompositor_table[ICompositor_CreateSurfaceBrush_slot] = (void *)compositor_CreateSurfaceBrush;
	ICompositor_table[ICompositor_CreateSurfaceBrushWithSurface_slot] = (void *)compositor_CreateSurfaceBrushWithSurface;
	ICompositor_table[ICompositor_CreateInsetClip_slot] = (void *)compositor_CreateInsetClip;
	ICompositor_table[ICompositor_CreateInsetClipWithInsets_slot] = (void *)compositor_CreateInsetClipWithInsets;
	ICompositorInterop_table[ICompositorInterop_CreateCompositionSurfaceForSwapChain_slot] = (void *)interop_CreateCompositionSurfaceForSwapChain;
	ICompositorInterop_table[ICompositorInterop_CreateGraphicsDevice_slot] = (void *)interop_CreateGraphicsDevice;
	ICompositorDesktopInterop_table[ICompositorDesktopInterop_CreateDesktopWindowTarget_slot] = (void *)desktop_CreateDesktopWindowTarget;
	ICompositorDesktopInterop_table[ICompositorDesktopInterop_EnsureOnThread_slot] = (void *)desktop_EnsureOnThread;

	IVisual_table[IVisual_get_AnchorPoint_slot] = (void *)visual_get_AnchorPoint;
	IVisual_table[IVisual_put_AnchorPoint_slot] = (void *)visual_put_AnchorPoint;
	IVisual_table[IVisual_get_BackfaceVisibility_slot] = (void *)visual_get_BackfaceVisibility;
	IVisual_table[IVisual_put_BackfaceVisibility_slot] = (void *)visual_put_BackfaceVisibility;
	IVisual_table[IVisual_get_BorderMode_slot] = (void *)visual_get_BorderMode;
	IVisual_table[IVisual_put_BorderMode_slot] = (void *)visual_put_BorderMode;
	IVisual_table[IVisual_get_CenterPoint_slot] = (void *)visual_get_CenterPoint;
	IVisual_table[IVisual_put_CenterPoint_slot] = (void *)visual_put_CenterPoint;
	IVisual_table[IVisual_get_Clip_slot] = (void *)visual_get_Clip;
	IVisual_table[IVisual_put_Clip_slot] = (void *)visual_put_Clip;
	IVisual_table[IVisual_get_CompositeMode_slot] = (void *)visual_get_CompositeMode;
	IVisual_table[IVisual_put_CompositeMode_slot] = (void *)visual_put_CompositeMode;
	IVisual_table[IVisual_get_IsVisible_slot] = (void *)visual_get_IsVisible;
	IVisual_table[IVisual_put_IsVisible_slot] = (void *)visual_put_IsVisible;
	IVisual_table[IVisual_get_Offset_slot] = (void *)visual_get_Offset;
	IVisual_table[IVisual_put_Offset_slot] = (void *)visual_put_Offset;
	IVisual_table[IVisual_get_Opacity_slot] = (void *)visual_get_Opacity;
	IVisual_table[IVisual_put_Opacity_slot] = (void *)visual_put_Opacity;
	IVisual_table[IVisual_get_Orientation_slot] = (void *)visual_get_Orientation;
	IVisual_table[IVisual_put_Orientation_slot] = (void *)visual_put_Orientation;
	IVisual_table[IVisual_get_Parent_slot] = (void *)visual_get_Parent;
	IVisual_table[IVisual_get_RotationAngle_slot] = (void *)visual_get_RotationAngle;
	IVisual_table[IVisual_put_RotationAngle_slot] = (void *)visual_put_RotationAngle;
	IVisual_table[IVisual_get_RotationAngleInDegrees_slot] = (void *)visual_get_RotationAngleInDegrees;
	IVisual_table[IVisual_put_RotationAngleInDegrees_slot] = (void *)visual_put_RotationAngleInDegrees;
	IVisual_table[IVisual_get_RotationAxis_slot] = (void *)visual_get_RotationAxis;
	IVisual_table[IVisual_put_RotationAxis_slot] = (void *)visual_put_RotationAxis;
	IVisual_table[IVisual_get_Scale_slot] = (void *)visual_get_Scale;
	IVisual_table[IVisual_put_Scale_slot] = (void *)visual_put_Scale;
	IVisual_table[IVisual_get_Size_slot] = (void *)visual_get_Size;
	IVisual_table[IVisual_put_Size_slot] = (void *)visual_put_Size;
	IVisual_table[IVisual_get_TransformMatrix_slot] = (void *)visual_get_TransformMatrix;
	IVisual_table[IVisual_put_TransformMatrix_slot] = (void *)visual_put_TransformMatrix;
	IContainerVisual_table[IContainerVisual_get_Children_slot] = (void *)visual_get_Children;
	ISpriteVisual_table[ISpriteVisual_get_Brush_slot] = (void *)sprite_get_Brush;
	ISpriteVisual_table[ISpriteVisual_put_Brush_slot] = (void *)sprite_put_Brush;

	IVisualCollection_table[IVisualCollection_get_Count_slot] = (void *)collection_get_Count;
	IVisualCollection_table[IVisualCollection_InsertAbove_slot] = (void *)collection_InsertAbove;
	IVisualCollection_table[IVisualCollection_InsertAtBottom_slot] = (void *)collection_InsertAtBottom;
	IVisualCollection_table[IVisualCollection_InsertAtTop_slot] = (void *)collection_InsertAtTop;
	IVisualCollection_table[IVisualCollection_InsertBelow_slot] = (void *)collection_InsertBelow;
	IVisualCollection_table[IVisualCollection_Remove_slot] = (void *)collection_Remove;
	IVisualCollection_table[IVisualCollection_RemoveAll_slot] = (void *)collection_RemoveAll;

	ICompositionColorBrush_table[ICompositionColorBrush_get_Color_slot] = (void *)brush_get_Color;
	ICompositionColorBrush_table[ICompositionColorBrush_put_Color_slot] = (void *)brush_put_Color;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_get_BitmapInterpolationMode_slot] = (void *)brush_get_BitmapInterpolationMode;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_put_BitmapInterpolationMode_slot] = (void *)brush_put_BitmapInterpolationMode;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_get_HorizontalAlignmentRatio_slot] = (void *)brush_get_HorizontalAlignmentRatio;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_put_HorizontalAlignmentRatio_slot] = (void *)brush_put_HorizontalAlignmentRatio;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_get_Stretch_slot] = (void *)brush_get_Stretch;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_put_Stretch_slot] = (void *)brush_put_Stretch;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_get_Surface_slot] = (void *)brush_get_Surface;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_put_Surface_slot] = (void *)brush_put_Surface;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_get_VerticalAlignmentRatio_slot] = (void *)brush_get_VerticalAlignmentRatio;
	ICompositionSurfaceBrush_table[ICompositionSurfaceBrush_put_VerticalAlignmentRatio_slot] = (void *)brush_put_VerticalAlignmentRatio;

	IInsetClip_table[IInsetClip_get_LeftInset_slot] = (void *)clip_get_LeftInset;
	IInsetClip_table[IInsetClip_put_LeftInset_slot] = (void *)clip_put_LeftInset;
	IInsetClip_table[IInsetClip_get_TopInset_slot] = (void *)clip_get_TopInset;
	IInsetClip_table[IInsetClip_put_TopInset_slot] = (void *)clip_put_TopInset;
	IInsetClip_table[IInsetClip_get_RightInset_slot] = (void *)clip_get_RightInset;
	IInsetClip_table[IInsetClip_put_RightInset_slot] = (void *)clip_put_RightInset;
	IInsetClip_table[IInsetClip_get_BottomInset_slot] = (void *)clip_get_BottomInset;
	IInsetClip_table[IInsetClip_put_BottomInset_slot] = (void *)clip_put_BottomInset;

	ICompositionTarget_table[ICompositionTarget_get_Root_slot] = (void *)target_get_Root;
	ICompositionTarget_table[ICompositionTarget_put_Root_slot] = (void *)target_put_Root;
	IDesktopWindowTarget_table[IDesktopWindowTarget_get_IsTopmost_slot] = (void *)target_get_IsTopmost;

	ICompositorController_table[ICompositorController_get_Compositor_slot] = (void *)controller_get_Compositor;
	ICompositorController_table[ICompositorController_Commit_slot] = (void *)controller_Commit;
	ICompositorController_table[ICompositorController_add_CommitNeeded_slot] = (void *)controller_add_CommitNeeded;
	ICompositorController_table[ICompositorController_remove_CommitNeeded_slot] = (void *)controller_remove_CommitNeeded;
}

/* ---- activation ---- */
struct factory {
	IActivationFactory iface;
	const WCHAR *name;
};

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

static HRESULT WINAPI factory_iids(IActivationFactory *iface, ULONG *count, IID **iids)
{
	if (!count || !iids)
		return E_POINTER;
	*count = 0;
	*iids = NULL;
	return S_OK;
}

static HRESULT WINAPI factory_name(IActivationFactory *iface, HSTRING *name)
{
	const WCHAR *text = ((struct factory *)iface)->name;
	return name ? WindowsCreateString(text, (UINT32)lstrlenW(text), name) : E_POINTER;
}

static HRESULT WINAPI factory_trust(IActivationFactory *iface, TrustLevel *level)
{
	if (!level)
		return E_POINTER;
	*level = BaseTrust;
	return S_OK;
}

static HRESULT WINAPI factory_activate(IActivationFactory *iface, IInspectable **out)
{
	struct factory *self = (struct factory *)iface;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (self->name == controller_name) {
		struct controller *controller = make_controller();
		if (!controller)
			return E_OUTOFMEMORY;
		*out = (IInspectable *)&controller->node.faces[0];
		return S_OK;
	} else {
		struct compositor *compositor = make_compositor();
		if (!compositor)
			return E_OUTOFMEMORY;
		*out = (IInspectable *)&compositor->node.faces[0];
		return S_OK;
	}
}

static IActivationFactoryVtbl factory_table = {factory_query, factory_reference, factory_release, factory_iids, factory_name, factory_trust,
	factory_activate};
static struct factory factories[] = {{{&factory_table}, controller_name}, {{&factory_table}, compositor_name}};

__declspec(dllexport) HRESULT WINAPI DllGetActivationFactory(HSTRING name, IActivationFactory **out)
{
	UINT32 length = 0;
	const WCHAR *text;
	UINT index;
	if (!out)
		return E_POINTER;
	*out = NULL;
	text = WindowsGetStringRawBuffer(name, &length);
	for (index = 0; index < ARRAYSIZE(factories); ++index)
		if (CompareStringOrdinal(text, (int)length, factories[index].name, -1, FALSE) == CSTR_EQUAL) {
			*out = &factories[index].iface;
			return S_OK;
		}
	return CLASS_E_CLASSNOTAVAILABLE;
}

/* ---- the compositor clock ---- */
#define FRAME_MS 16

/* Waits for the next tick of the clock, for one of the handles, or for the timeout, whichever comes first: the index of the handle, the
 * handle count for the tick, or WAIT_TIMEOUT. */
__declspec(dllexport) DWORD WINAPI DCompositionWaitForCompositorClock(UINT count, const HANDLE *handles, DWORD timeout)
{
	const DWORD tick = FRAME_MS - (DWORD)(GetTickCount64() % FRAME_MS);
	const DWORD wait = timeout < tick ? timeout : tick;
	DWORD result;
	if (count > MAXIMUM_WAIT_OBJECTS || (count && !handles)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return WAIT_FAILED;
	}
	if (count) {
		result = WaitForMultipleObjects(count, handles, FALSE, wait);
		if (result != WAIT_TIMEOUT)
			return result;
	} else
		Sleep(wait);
	return wait == tick ? WAIT_OBJECT_0 + count : WAIT_TIMEOUT;
}

/* Asks for the clock to run at full rate for a while; it always does here. */
__declspec(dllexport) HRESULT WINAPI DCompositionBoostCompositorClock(BOOL enable)
{
	return S_OK;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return live ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
		InitializeCriticalSection(&lock);
		build_tables();
	}
	return TRUE;
}
