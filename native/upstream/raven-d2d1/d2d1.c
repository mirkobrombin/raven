/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven Direct2D component: the factory, devices, device contexts and render targets, bitmaps, brushes, geometries and the other objects
 * that programs such as Paint.NET draw their windows with.
 *
 * The interfaces are COM objects written here. The pixels live with the host (ravenhost.dll, which draws with Skia): a bitmap or a
 * render target holds the number of a host surface, and every drawing call becomes one command record sent to it. Geometries are kept
 * here as paths of lines and Bezier curves. Every interface starts from a table of stubs that name the method they stand for
 * (d2d1_tables.h), so a call that nothing implements shows up by name in the trace (RAVEN_TRACE_DWRITE=1). */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ALLOC(size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size))
#define GROW(block, size) ((block) ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (block), (size)) : ALLOC(size))
#define FREE(block) HeapFree(GetProcessHeap(), 0, (block))
#define CONTAINER(pointer, type, field) ((type *)((char *)(pointer) - offsetof(type, field)))

/* ---- the host services ---- */
__declspec(dllimport) DWORD WINAPI RavenCanvasCreate(DWORD width, DWORD height);
__declspec(dllimport) void WINAPI RavenCanvasDestroy(DWORD surface);
__declspec(dllimport) DWORD WINAPI RavenCanvasSize(DWORD surface, DWORD *size);
__declspec(dllimport) DWORD WINAPI RavenCanvasPixels(DWORD surface, INT32 x, INT32 y, DWORD width, DWORD height, void *data, DWORD pitch, DWORD write);
__declspec(dllimport) DWORD WINAPI RavenCanvasDraw(DWORD surface, const void *command, const void *data, DWORD size);
__declspec(dllimport) DWORD WINAPI RavenPathCombine(const float *one, DWORD one_count, const float *two, DWORD two_count, DWORD fills, DWORD mode,
	float *result, DWORD capacity);
__declspec(dllimport) DWORD WINAPI RavenPathWiden(const float *path, DWORD count, DWORD fill, const float *stroke, DWORD stroke_count, float *result,
	DWORD capacity);
__declspec(dllimport) void WINAPI RavenTrace(const char *text);

static HRESULT unimplemented(const char *interface_name, const char *method)
{
	char text[160];
	size_t at = 0;
	const char *part[] = {"d2d1: ", interface_name, "::", method, " is not implemented"};
	size_t index, length;
	for (index = 0; index < 5; ++index)
		for (length = 0; part[index][length] && at < sizeof(text) - 1; ++length)
			text[at++] = part[index][length];
	text[at] = 0;
	RavenTrace(text);
	return E_NOTIMPL;
}

#include "d2d1_tables.h"

/* The kind of pixel of a WIC bitmap a render target draws into: 1 for 8bppAlpha, 2 for 8bppGray (both a byte a pixel), 4 for 32 bits. */
static UINT wic_depth(IWICBitmap *wic)
{
	WICPixelFormatGUID format;
	if (FAILED(IWICBitmap_GetPixelFormat(wic, &format)))
		return 4;
	if (!memcmp(&format, &GUID_WICPixelFormat8bppAlpha, sizeof(format)))
		return 1;
	if (!memcmp(&format, &GUID_WICPixelFormat8bppGray, sizeof(format)))
		return 2;
	return 4;
}

/* The headers leave this one out. */
DEFINE_GUID(IID_ID2D1HwndRenderTarget, 0x2cd90698, 0x12e2, 0x11dc, 0x9f, 0xed, 0x00, 0x11, 0x43, 0xa0, 0x55, 0xf9);

static BOOL same_guid(const GUID *a, const GUID *b)
{
	return memcmp(a, b, sizeof(GUID)) == 0;
}

static BOOL any_guid(const GUID *riid, const GUID *const *list, size_t count)
{
	size_t index;
	for (index = 0; index < count; ++index)
		if (same_guid(riid, list[index]))
			return TRUE;
	return FALSE;
}

/* A table of its own for an object, copied from the generated stubs and then filled with what is written here. */
static void **make_table(void *const *stubs, size_t count)
{
	void **table = ALLOC(count * sizeof(void *));
	memcpy(table, stubs, count * sizeof(void *));
	return table;
}

/* ---- the command record the host reads (graphics/canvas.h) ---- */
enum { OP_CLEAR = 1, OP_FILL_PATH = 2, OP_STROKE_PATH = 3, OP_DRAW_BITMAP = 4, OP_FILL_MASK = 5, OP_PUSH_CLIP = 6, OP_PUSH_LAYER = 7,
	OP_POP = 8, OP_COPY = 9 };
enum { BRUSH_NONE = 0, BRUSH_SOLID = 1, BRUSH_LINEAR = 2, BRUSH_RADIAL = 3, BRUSH_BITMAP = 4 };

struct command {
	UINT32 op, antialias;
	float transform[6];
	UINT32 brush;
	float opacity;
	float color[4];
	float brush_transform[6];
	float brush_points[6];
	UINT32 stops, extend_x, extend_y, interpolation, bitmap;
	float source[4];
	float stroke_width;
	UINT32 start_cap, end_cap, dash_cap, line_join;
	float miter_limit;
	UINT32 dashes;
	float dash_offset;
	float rect[4];
	UINT32 path_floats, fill_mode, mask_width, mask_height;
};
typedef char command_size_check[sizeof(struct command) == 204 ? 1 : -1];

/* A growing buffer of bytes: the variable parts after a command record. */
struct bytes {
	BYTE *data;
	size_t size, capacity;
};

static BOOL bytes_add(struct bytes *buffer, const void *data, size_t size)
{
	if (buffer->size + size > buffer->capacity) {
		size_t capacity = buffer->capacity ? buffer->capacity * 2 : 256;
		BYTE *grown;
		while (capacity < buffer->size + size)
			capacity *= 2;
		grown = GROW(buffer->data, capacity);
		if (!grown)
			return FALSE;
		buffer->data = grown;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->size, data, size);
	buffer->size += size;
	return TRUE;
}

/* ---- matrices ---- */
static const D2D1_MATRIX_3X2_F identity = {{{1, 0, 0, 1, 0, 0}}};

static D2D1_MATRIX_3X2_F multiply(const D2D1_MATRIX_3X2_F *a, const D2D1_MATRIX_3X2_F *b)
{
	D2D1_MATRIX_3X2_F r;
	r._11 = a->_11 * b->_11 + a->_12 * b->_21;
	r._12 = a->_11 * b->_12 + a->_12 * b->_22;
	r._21 = a->_21 * b->_11 + a->_22 * b->_21;
	r._22 = a->_21 * b->_12 + a->_22 * b->_22;
	r._31 = a->_31 * b->_11 + a->_32 * b->_21 + b->_31;
	r._32 = a->_31 * b->_12 + a->_32 * b->_22 + b->_32;
	return r;
}

static D2D1_POINT_2F apply(const D2D1_MATRIX_3X2_F *m, D2D1_POINT_2F p)
{
	D2D1_POINT_2F r = {p.x * m->_11 + p.y * m->_21 + m->_31, p.x * m->_12 + p.y * m->_22 + m->_32};
	return r;
}

static BOOL invert(const D2D1_MATRIX_3X2_F *m, D2D1_MATRIX_3X2_F *out)
{
	const float det = m->_11 * m->_22 - m->_12 * m->_21;
	if (det == 0)
		return FALSE;
	out->_11 = m->_22 / det;
	out->_12 = -m->_12 / det;
	out->_21 = -m->_21 / det;
	out->_22 = m->_11 / det;
	out->_31 = (m->_21 * m->_32 - m->_22 * m->_31) / det;
	out->_32 = (m->_12 * m->_31 - m->_11 * m->_32) / det;
	return TRUE;
}

static void put_matrix(float *to, const D2D1_MATRIX_3X2_F *m)
{
	to[0] = m->_11;
	to[1] = m->_12;
	to[2] = m->_21;
	to[3] = m->_22;
	to[4] = m->_31;
	to[5] = m->_32;
}

/* ---- paths: verbs 0 move, 1 line, 2 quadratic, 3 cubic, 4 close, each followed by its points ---- */
struct path {
	float *data;
	UINT32 count, capacity;
	D2D1_FILL_MODE fill;
};

static BOOL path_put(struct path *path, const float *values, UINT32 count)
{
	if (path->count + count > path->capacity) {
		UINT32 capacity = path->capacity ? path->capacity * 2 : 64;
		float *grown;
		while (capacity < path->count + count)
			capacity *= 2;
		grown = GROW(path->data, capacity * sizeof(float));
		if (!grown)
			return FALSE;
		path->data = grown;
		path->capacity = capacity;
	}
	memcpy(path->data + path->count, values, count * sizeof(float));
	path->count += count;
	return TRUE;
}

static void path_move(struct path *path, D2D1_POINT_2F p) { float v[3] = {0, p.x, p.y}; path_put(path, v, 3); }
static void path_line(struct path *path, D2D1_POINT_2F p) { float v[3] = {1, p.x, p.y}; path_put(path, v, 3); }
static void path_quad(struct path *path, D2D1_POINT_2F a, D2D1_POINT_2F b) { float v[5] = {2, a.x, a.y, b.x, b.y}; path_put(path, v, 5); }
static void path_cubic(struct path *path, D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c)
{
	float v[7] = {3, a.x, a.y, b.x, b.y, c.x, c.y};
	path_put(path, v, 7);
}
static void path_close(struct path *path) { float v = 4; path_put(path, &v, 1); }

static void path_free(struct path *path)
{
	FREE(path->data);
	memset(path, 0, sizeof(*path));
}

static BOOL path_copy(struct path *to, const struct path *from)
{
	memset(to, 0, sizeof(*to));
	to->fill = from->fill;
	return !from->count || path_put(to, from->data, from->count);
}

/* Each point of a path through a matrix; the verbs stay. */
static void path_transform(struct path *path, const D2D1_MATRIX_3X2_F *m)
{
	UINT32 at = 0, points, index;
	while (at < path->count) {
		const UINT32 verb = (UINT32)path->data[at++];
		points = verb <= 1 ? 1 : verb == 2 ? 2 : verb == 3 ? 3 : 0;
		for (index = 0; index < points; ++index) {
			D2D1_POINT_2F p = {path->data[at], path->data[at + 1]};
			p = apply(m, p);
			path->data[at] = p.x;
			path->data[at + 1] = p.y;
			at += 2;
		}
	}
}

/* The box around every point, control points included: never smaller than the shape. */
static D2D1_RECT_F path_bounds(const struct path *path, const D2D1_MATRIX_3X2_F *m)
{
	D2D1_RECT_F r = {0, 0, 0, 0};
	BOOL first = TRUE;
	UINT32 at = 0, points, index;
	while (at < path->count) {
		const UINT32 verb = (UINT32)path->data[at++];
		points = verb <= 1 ? 1 : verb == 2 ? 2 : verb == 3 ? 3 : 0;
		for (index = 0; index < points; ++index) {
			D2D1_POINT_2F p = {path->data[at], path->data[at + 1]};
			if (m)
				p = apply(m, p);
			if (first) {
				r.left = r.right = p.x;
				r.top = r.bottom = p.y;
				first = FALSE;
			} else {
				if (p.x < r.left) r.left = p.x;
				if (p.x > r.right) r.right = p.x;
				if (p.y < r.top) r.top = p.y;
				if (p.y > r.bottom) r.bottom = p.y;
			}
			at += 2;
		}
	}
	if (first) {
		/* An empty geometry has bounds that contain nothing: infinite reversed, as Direct2D reports. */
		r.left = r.top = 3.402823466e+38f;
		r.right = r.bottom = -3.402823466e+38f;
	}
	return r;
}

/* A quarter of an ellipse as a cubic curve: the handle length that makes the curve meet the arc at its middle. */
#define KAPPA 0.5522847498f

static void path_ellipse(struct path *path, float cx, float cy, float rx, float ry)
{
	const float kx = rx * KAPPA, ky = ry * KAPPA;
	D2D1_POINT_2F p0 = {cx + rx, cy}, p1 = {cx, cy + ry}, p2 = {cx - rx, cy}, p3 = {cx, cy - ry};
	D2D1_POINT_2F a, b;
	path_move(path, p0);
	a.x = cx + rx; a.y = cy + ky; b.x = cx + kx; b.y = cy + ry; path_cubic(path, a, b, p1);
	a.x = cx - kx; a.y = cy + ry; b.x = cx - rx; b.y = cy + ky; path_cubic(path, a, b, p2);
	a.x = cx - rx; a.y = cy - ky; b.x = cx - kx; b.y = cy - ry; path_cubic(path, a, b, p3);
	a.x = cx + kx; a.y = cy - ry; b.x = cx + rx; b.y = cy - ky; path_cubic(path, a, b, p0);
	path_close(path);
}

static void path_rectangle(struct path *path, const D2D1_RECT_F *r)
{
	D2D1_POINT_2F p;
	p.x = r->left; p.y = r->top; path_move(path, p);
	p.x = r->right; path_line(path, p);
	p.y = r->bottom; path_line(path, p);
	p.x = r->left; path_line(path, p);
	path_close(path);
}

static void path_rounded(struct path *path, const D2D1_ROUNDED_RECT *rr)
{
	const D2D1_RECT_F *r = &rr->rect;
	float rx = rr->radiusX, ry = rr->radiusY;
	const float w = r->right - r->left, h = r->bottom - r->top;
	D2D1_POINT_2F a, b, c;
	if (rx > w / 2) rx = w / 2;
	if (ry > h / 2) ry = h / 2;
	if (rx <= 0 || ry <= 0) {
		path_rectangle(path, r);
		return;
	}
	a.x = r->left + rx; a.y = r->top; path_move(path, a);
	a.x = r->right - rx; path_line(path, a);
	a.x = r->right - rx + rx * KAPPA; a.y = r->top; b.x = r->right; b.y = r->top + ry - ry * KAPPA; c.x = r->right; c.y = r->top + ry; path_cubic(path, a, b, c);
	c.y = r->bottom - ry; path_line(path, c);
	a.x = r->right; a.y = r->bottom - ry + ry * KAPPA; b.x = r->right - rx + rx * KAPPA; b.y = r->bottom; c.x = r->right - rx; c.y = r->bottom; path_cubic(path, a, b, c);
	c.x = r->left + rx; path_line(path, c);
	a.x = r->left + rx - rx * KAPPA; a.y = r->bottom; b.x = r->left; b.y = r->bottom - ry + ry * KAPPA; c.x = r->left; c.y = r->bottom - ry; path_cubic(path, a, b, c);
	c.y = r->top + ry; path_line(path, c);
	a.x = r->left; a.y = r->top + ry - ry * KAPPA; b.x = r->left + rx - rx * KAPPA; b.y = r->top; c.x = r->left + rx; c.y = r->top; path_cubic(path, a, b, c);
	path_close(path);
}

/* ---- arithmetic the component needs without a C runtime ---- */
#define PI 3.14159265358979f

static float fsqrt(float x)
{
	return x > 0 ? __builtin_sqrtf(x) : 0;
}

static float fabs_(float x)
{
	return x < 0 ? -x : x;
}

/* Sine and cosine of an angle brought to [-pi, pi], from their series to the ninth power: errors below 1e-6. */
static void sincos_(float angle, float *s, float *c)
{
	float x = angle, x2;
	while (x > PI) x -= 2 * PI;
	while (x < -PI) x += 2 * PI;
	{
		/* Fold to [-pi/2, pi/2], where the series converge fast; the cosine changes sign on the way. */
		float sign = 1;
		if (x > PI / 2) { x = PI - x; sign = -1; }
		else if (x < -PI / 2) { x = -PI - x; sign = -1; }
		x2 = x * x;
		*s = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
		*c = sign * (1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90)))));
	}
}

/* The arc tangent through its series around zero after folding the argument below tan(pi/12). */
static float atan_(float x)
{
	BOOL negative = x < 0, inverted = FALSE, shifted = FALSE;
	float x2, r;
	if (negative) x = -x;
	if (x > 1) { x = 1 / x; inverted = TRUE; }
	if (x > 0.2679491924f) { x = (x * 1.7320508076f - 1) / (x + 1.7320508076f); shifted = TRUE; }
	x2 = x * x;
	r = x * (1 - x2 * (1.0f / 3 - x2 * (1.0f / 5 - x2 * (1.0f / 7 - x2 / 9))));
	if (shifted) r += PI / 6;
	if (inverted) r = PI / 2 - r;
	return negative ? -r : r;
}

static float atan2_(float y, float x)
{
	if (x > 0) return atan_(y / x);
	if (x < 0) return y >= 0 ? atan_(y / x) + PI : atan_(y / x) - PI;
	return y > 0 ? PI / 2 : y < 0 ? -PI / 2 : 0;
}

/* An elliptical arc from the current point, as SVG and Direct2D describe it (end point, radii, rotation, sweep and size), in cubic
 * curves of at most a quarter turn each. */
static void path_arc(struct path *path, D2D1_POINT_2F from, const D2D1_ARC_SEGMENT *arc)
{
	float rx = fabs_(arc->size.width), ry = fabs_(arc->size.height), s, c, x1, y1, lambda, factor, cx1, cy1, cx, cy, theta, delta;
	float ux, uy, vx, vy, segments_f;
	int segments, index;
	const D2D1_POINT_2F to = arc->point;
	if (rx == 0 || ry == 0 || (from.x == to.x && from.y == to.y)) {
		path_line(path, to);
		return;
	}
	sincos_(arc->rotationAngle * PI / 180, &s, &c);
	x1 = c * (from.x - to.x) / 2 + s * (from.y - to.y) / 2;
	y1 = -s * (from.x - to.x) / 2 + c * (from.y - to.y) / 2;
	lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
	if (lambda > 1) {
		rx *= fsqrt(lambda);
		ry *= fsqrt(lambda);
	}
	factor = (rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1) / (rx * rx * y1 * y1 + ry * ry * x1 * x1);
	factor = fsqrt(factor < 0 ? 0 : factor);
	if ((arc->arcSize == D2D1_ARC_SIZE_LARGE) == (arc->sweepDirection == D2D1_SWEEP_DIRECTION_CLOCKWISE))
		factor = -factor;
	cx1 = factor * rx * y1 / ry;
	cy1 = -factor * ry * x1 / rx;
	cx = c * cx1 - s * cy1 + (from.x + to.x) / 2;
	cy = s * cx1 + c * cy1 + (from.y + to.y) / 2;
	ux = (x1 - cx1) / rx; uy = (y1 - cy1) / ry;
	vx = (-x1 - cx1) / rx; vy = (-y1 - cy1) / ry;
	theta = atan2_(uy, ux);
	delta = atan2_(vy, vx) - theta;
	if (arc->sweepDirection == D2D1_SWEEP_DIRECTION_CLOCKWISE && delta < 0)
		delta += 2 * PI;
	else if (arc->sweepDirection != D2D1_SWEEP_DIRECTION_CLOCKWISE && delta > 0)
		delta -= 2 * PI;
	segments_f = fabs_(delta) / (PI / 2);
	segments = (int)segments_f;
	if ((float)segments < segments_f)
		++segments;
	if (segments < 1)
		segments = 1;
	for (index = 0; index < segments; ++index) {
		const float a0 = theta + delta * index / segments, a1 = theta + delta * (index + 1) / segments;
		const float half = (a1 - a0) / 2;
		float s0, c0, s1, c1, sh, ch, k;
		D2D1_POINT_2F p1, p2, p3;
		sincos_(a0, &s0, &c0);
		sincos_(a1, &s1, &c1);
		sincos_(half, &sh, &ch);
		k = 4.0f / 3.0f * (1 - ch) / (sh == 0 ? 1 : sh);
		/* The points on the unit circle, stretched by the radii, turned by the rotation and moved to the center. */
		{
			const float e[3][2] = {{c0 - k * s0, s0 + k * c0}, {c1 + k * s1, s1 - k * c1}, {c1, s1}};
			D2D1_POINT_2F *out[3] = {&p1, &p2, &p3};
			int point;
			for (point = 0; point < 3; ++point) {
				const float px = e[point][0] * rx, py = e[point][1] * ry;
				out[point]->x = c * px - s * py + cx;
				out[point]->y = s * px + c * py + cy;
			}
		}
		if (index == segments - 1)
			p3 = to;
		path_cubic(path, p1, p2, p3);
	}
}

/* ---- point tests: the path flattened into lines, then crossings counted ---- */
struct flat {
	D2D1_POINT_2F *points;
	UINT32 *starts; /* where each figure begins */
	UINT32 count, figures, capacity, figure_capacity;
};

static void flat_add(struct flat *flat, D2D1_POINT_2F p)
{
	if (flat->count == flat->capacity) {
		const UINT32 capacity = flat->capacity ? flat->capacity * 2 : 128;
		D2D1_POINT_2F *grown = GROW(flat->points, capacity * sizeof(*grown));
		if (!grown)
			return;
		flat->points = grown;
		flat->capacity = capacity;
	}
	flat->points[flat->count++] = p;
}

static void flat_figure(struct flat *flat)
{
	if (flat->figures == flat->figure_capacity) {
		const UINT32 capacity = flat->figure_capacity ? flat->figure_capacity * 2 : 16;
		UINT32 *grown = GROW(flat->starts, capacity * sizeof(*grown));
		if (!grown)
			return;
		flat->starts = grown;
		flat->figure_capacity = capacity;
	}
	flat->starts[flat->figures++] = flat->count;
}

static void flatten(const struct path *path, const D2D1_MATRIX_3X2_F *m, float tolerance, struct flat *flat)
{
	UINT32 at = 0;
	D2D1_POINT_2F current = {0, 0};
	memset(flat, 0, sizeof(*flat));
	if (tolerance <= 0)
		tolerance = 0.25f;
	while (at < path->count) {
		const UINT32 verb = (UINT32)path->data[at++];
		D2D1_POINT_2F p[3];
		int n = verb <= 1 ? 1 : verb == 2 ? 2 : verb == 3 ? 3 : 0, index;
		for (index = 0; index < n; ++index) {
			p[index].x = path->data[at++];
			p[index].y = path->data[at++];
			if (m)
				p[index] = apply(m, p[index]);
		}
		if (verb == 0) {
			flat_figure(flat);
			flat_add(flat, p[0]);
			current = p[0];
		} else if (verb == 1) {
			flat_add(flat, p[0]);
			current = p[0];
		} else if (verb == 2 || verb == 3) {
			/* Enough steps that no chord strays further than the tolerance from the curve. */
			const D2D1_POINT_2F c1 = verb == 2 ? p[0] : p[0], c2 = verb == 2 ? p[0] : p[1], end = verb == 2 ? p[1] : p[2];
			const float span = fabs_(c1.x - current.x) + fabs_(c1.y - current.y) + fabs_(c2.x - c1.x) + fabs_(c2.y - c1.y) +
				fabs_(end.x - c2.x) + fabs_(end.y - c2.y);
			int steps = (int)(fsqrt(span / tolerance) * 2) + 1, step;
			if (steps > 256) steps = 256;
			for (step = 1; step <= steps; ++step) {
				const float t = (float)step / steps, u = 1 - t;
				D2D1_POINT_2F q;
				if (verb == 2) {
					q.x = u * u * current.x + 2 * u * t * c1.x + t * t * end.x;
					q.y = u * u * current.y + 2 * u * t * c1.y + t * t * end.y;
				} else {
					q.x = u * u * u * current.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * end.x;
					q.y = u * u * u * current.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * end.y;
				}
				flat_add(flat, q);
			}
			current = end;
		}
	}
}

static void flat_free(struct flat *flat)
{
	FREE(flat->points);
	FREE(flat->starts);
}

/* Winding number of a point: every figure counts as closed. */
static BOOL flat_contains(const struct flat *flat, D2D1_POINT_2F p, D2D1_FILL_MODE fill)
{
	int winding = 0;
	UINT32 figure;
	for (figure = 0; figure < flat->figures; ++figure) {
		const UINT32 begin = flat->starts[figure], end = figure + 1 < flat->figures ? flat->starts[figure + 1] : flat->count;
		UINT32 index;
		for (index = begin; index < end; ++index) {
			const D2D1_POINT_2F a = flat->points[index], b = flat->points[index + 1 < end ? index + 1 : begin];
			if (a.y <= p.y) {
				if (b.y > p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) > 0)
					++winding;
			} else if (b.y <= p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) < 0) {
				--winding;
			}
		}
	}
	return fill == D2D1_FILL_MODE_WINDING ? winding != 0 : (winding & 1) != 0;
}

/* Whether a point lies within half the stroke width of any segment. */
static BOOL flat_near(const struct flat *flat, D2D1_POINT_2F p, float distance, BOOL closed)
{
	UINT32 figure;
	for (figure = 0; figure < flat->figures; ++figure) {
		const UINT32 begin = flat->starts[figure], end = figure + 1 < flat->figures ? flat->starts[figure + 1] : flat->count;
		UINT32 index;
		for (index = begin; index + 1 < end || (closed && index < end); ++index) {
			const D2D1_POINT_2F a = flat->points[index], b = flat->points[index + 1 < end ? index + 1 : begin];
			const float dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
			float t = length2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2 : 0, qx, qy;
			if (t < 0) t = 0;
			if (t > 1) t = 1;
			qx = a.x + t * dx - p.x;
			qy = a.y + t * dy - p.y;
			if (qx * qx + qy * qy <= distance * distance)
				return TRUE;
		}
	}
	return FALSE;
}

/* ---- the objects: a table, a count, how to end, and the interfaces they answer for ---- */
struct object {
	void **vtbl;
	LONG refs;
	void (*destroy)(struct object *self);
	const GUID *const *iids;
	size_t iid_count;
	IUnknown *factory;
};

static void object_init(struct object *self, void **table, void (*destroy)(struct object *), const GUID *const *iids, size_t count, IUnknown *factory)
{
	self->vtbl = table;
	self->refs = 1;
	self->destroy = destroy;
	self->iids = iids;
	self->iid_count = count;
	self->factory = factory;
	if (factory)
		IUnknown_AddRef(factory);
}

static HRESULT STDMETHODCALLTYPE object_QueryInterface(void *iface, REFIID riid, void **out)
{
	struct object *self = iface;
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || any_guid(riid, self->iids, self->iid_count)) {
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
		IUnknown *factory = self->factory;
		if (self->destroy)
			self->destroy(self);
		FREE(self);
		if (factory)
			IUnknown_Release(factory);
	}
	return (ULONG)refs;
}

/* ID2D1Resource::GetFactory: every resource names the factory it came from. */
static void STDMETHODCALLTYPE object_GetFactory(void *iface, ID2D1Factory **factory)
{
	struct object *self = iface;
	*factory = (ID2D1Factory *)self->factory;
	if (self->factory)
		IUnknown_AddRef(self->factory);
}

static void set_unknown(void **table)
{
	table[0] = (void *)object_QueryInterface;
	table[1] = (void *)object_AddRef;
	table[2] = (void *)object_Release;
}

static void set_resource(void **table)
{
	set_unknown(table);
	table[3] = (void *)object_GetFactory;
}

/* ---- stroke styles ---- */
struct stroke {
	struct object base;
	D2D1_STROKE_STYLE_PROPERTIES1 properties;
	float *dashes;
	UINT32 dash_count;
};

static void stroke_destroy(struct object *base)
{
	FREE(((struct stroke *)base)->dashes);
}

static D2D1_CAP_STYLE STDMETHODCALLTYPE stroke_GetStartCap(void *iface) { return ((struct stroke *)iface)->properties.startCap; }
static D2D1_CAP_STYLE STDMETHODCALLTYPE stroke_GetEndCap(void *iface) { return ((struct stroke *)iface)->properties.endCap; }
static D2D1_CAP_STYLE STDMETHODCALLTYPE stroke_GetDashCap(void *iface) { return ((struct stroke *)iface)->properties.dashCap; }
static FLOAT STDMETHODCALLTYPE stroke_GetMiterLimit(void *iface) { return ((struct stroke *)iface)->properties.miterLimit; }
static D2D1_LINE_JOIN STDMETHODCALLTYPE stroke_GetLineJoin(void *iface) { return ((struct stroke *)iface)->properties.lineJoin; }
static FLOAT STDMETHODCALLTYPE stroke_GetDashOffset(void *iface) { return ((struct stroke *)iface)->properties.dashOffset; }
static D2D1_DASH_STYLE STDMETHODCALLTYPE stroke_GetDashStyle(void *iface) { return ((struct stroke *)iface)->properties.dashStyle; }
static UINT32 STDMETHODCALLTYPE stroke_GetDashesCount(void *iface) { return ((struct stroke *)iface)->dash_count; }
static void STDMETHODCALLTYPE stroke_GetDashes(void *iface, FLOAT *dashes, UINT32 count)
{
	struct stroke *self = iface;
	UINT32 index;
	for (index = 0; index < count; ++index)
		dashes[index] = index < self->dash_count ? self->dashes[index] : 0;
}
static D2D1_STROKE_TRANSFORM_TYPE STDMETHODCALLTYPE stroke_GetStrokeTransformType(void *iface) { return ((struct stroke *)iface)->properties.transformType; }

static void **stroke_table;
static const GUID *const stroke_iids[] = {&IID_ID2D1Resource, &IID_ID2D1StrokeStyle, &IID_ID2D1StrokeStyle1};

static HRESULT make_stroke(IUnknown *factory, const D2D1_STROKE_STYLE_PROPERTIES1 *properties, const FLOAT *dashes, UINT32 count, void **out)
{
	struct stroke *self = ALLOC(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	object_init(&self->base, stroke_table, stroke_destroy, stroke_iids, 3, factory);
	self->properties = *properties;
	if (properties->dashStyle == D2D1_DASH_STYLE_CUSTOM && count) {
		self->dashes = ALLOC(count * sizeof(float));
		memcpy(self->dashes, dashes, count * sizeof(float));
		self->dash_count = count;
	}
	*out = self;
	return S_OK;
}

/* The dash pattern in units of the stroke width, as the predefined styles define it; the command gets lengths. */
static UINT32 stroke_pattern(const struct stroke *stroke, float width, float *out)
{
	static const float dash[] = {2, 2}, dot[] = {0, 2}, dash_dot[] = {2, 2, 0, 2}, dash_dot_dot[] = {2, 2, 0, 2, 0, 2};
	const float *pattern = NULL;
	UINT32 count = 0, index;
	if (!stroke)
		return 0;
	switch (stroke->properties.dashStyle) {
	case D2D1_DASH_STYLE_DASH: pattern = dash; count = 2; break;
	case D2D1_DASH_STYLE_DOT: pattern = dot; count = 2; break;
	case D2D1_DASH_STYLE_DASH_DOT: pattern = dash_dot; count = 4; break;
	case D2D1_DASH_STYLE_DASH_DOT_DOT: pattern = dash_dot_dot; count = 6; break;
	case D2D1_DASH_STYLE_CUSTOM: pattern = stroke->dashes; count = stroke->dash_count > 64 ? 64 : stroke->dash_count; break;
	default: return 0;
	}
	if (count & 1) {
		/* An odd pattern repeats twice to pair every dash with a gap. */
		for (index = 0; index < count * 2 && index < 64; ++index)
			out[index] = pattern[index % count] * width;
		return count * 2 > 64 ? 64 : count * 2;
	}
	for (index = 0; index < count; ++index)
		out[index] = pattern[index] * width;
	return count;
}

/* ---- geometries ---- */
enum { GEOMETRY_RECTANGLE, GEOMETRY_ROUNDED, GEOMETRY_ELLIPSE, GEOMETRY_PATH, GEOMETRY_GROUP, GEOMETRY_TRANSFORMED };

struct geometry {
	struct object base;
	int kind;
	struct path path; /* the shape in its own space */
	D2D1_RECT_F rect;
	D2D1_ROUNDED_RECT rounded;
	D2D1_ELLIPSE ellipse;
	D2D1_MATRIX_3X2_F transform; /* a transformed geometry's matrix */
	struct geometry *source;
	struct geometry **members;
	UINT32 member_count;
	/* A path geometry: open while its sink is, then fixed; its figures and segments are counted. */
	int state;
	UINT32 figures, segments;
};

static void geometry_destroy(struct object *base)
{
	struct geometry *self = (struct geometry *)base;
	UINT32 index;
	path_free(&self->path);
	if (self->source)
		object_Release(self->source);
	for (index = 0; index < self->member_count; ++index)
		object_Release(self->members[index]);
	FREE(self->members);
}

static struct geometry *geometry_of(void *iface)
{
	return iface;
}

static HRESULT STDMETHODCALLTYPE geometry_GetBounds(void *iface, const D2D1_MATRIX_3X2_F *transform, D2D1_RECT_F *bounds)
{
	*bounds = path_bounds(&geometry_of(iface)->path, transform);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE geometry_GetWidenedBounds(void *iface, FLOAT width, ID2D1StrokeStyle *stroke, const D2D1_MATRIX_3X2_F *transform,
	FLOAT tolerance, D2D1_RECT_F *bounds)
{
	*bounds = path_bounds(&geometry_of(iface)->path, transform);
	if (bounds->left <= bounds->right) {
		/* Half the width on every side, more where miters may reach. */
		const float grow = width * (stroke && ((struct stroke *)stroke)->properties.lineJoin == D2D1_LINE_JOIN_MITER ?
			((struct stroke *)stroke)->properties.miterLimit : 1);
		bounds->left -= grow;
		bounds->top -= grow;
		bounds->right += grow;
		bounds->bottom += grow;
	}
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE geometry_FillContainsPoint(void *iface, D2D1_POINT_2F point, const D2D1_MATRIX_3X2_F *transform, FLOAT tolerance,
	BOOL *contains)
{
	struct geometry *self = geometry_of(iface);
	struct flat flat;
	flatten(&self->path, transform, tolerance, &flat);
	*contains = flat_contains(&flat, point, self->path.fill);
	flat_free(&flat);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE geometry_StrokeContainsPoint(void *iface, D2D1_POINT_2F point, FLOAT width, ID2D1StrokeStyle *stroke,
	const D2D1_MATRIX_3X2_F *transform, FLOAT tolerance, BOOL *contains)
{
	struct geometry *self = geometry_of(iface);
	struct flat flat;
	flatten(&self->path, transform, tolerance, &flat);
	*contains = flat_near(&flat, point, width / 2 + (tolerance > 0 ? tolerance : 0.25f), TRUE);
	flat_free(&flat);
	return S_OK;
}

/* Feeds a path to a simplified sink: lines and cubic curves, or only lines when the curves are flattened. */
static HRESULT replay(const struct path *path, const D2D1_MATRIX_3X2_F *transform, BOOL lines_only, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
	UINT32 at = 0;
	BOOL open = FALSE;
	D2D1_POINT_2F current = {0, 0};
	ID2D1SimplifiedGeometrySink_SetFillMode(sink, path->fill);
	while (at < path->count) {
		const UINT32 verb = (UINT32)path->data[at++];
		D2D1_POINT_2F p[3];
		int n = verb <= 1 ? 1 : verb == 2 ? 2 : verb == 3 ? 3 : 0, index;
		for (index = 0; index < n; ++index) {
			p[index].x = path->data[at++];
			p[index].y = path->data[at++];
			if (transform)
				p[index] = apply(transform, p[index]);
		}
		if (verb == 0) {
			if (open)
				ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_OPEN);
			ID2D1SimplifiedGeometrySink_BeginFigure(sink, p[0], D2D1_FIGURE_BEGIN_FILLED);
			open = TRUE;
			current = p[0];
		} else if (verb == 1) {
			ID2D1SimplifiedGeometrySink_AddLines(sink, p, 1);
			current = p[0];
		} else if (verb == 2 || verb == 3) {
			const D2D1_POINT_2F end = verb == 2 ? p[1] : p[2];
			D2D1_BEZIER_SEGMENT bezier;
			if (verb == 2) {
				bezier.point1.x = current.x + 2.0f / 3 * (p[0].x - current.x);
				bezier.point1.y = current.y + 2.0f / 3 * (p[0].y - current.y);
				bezier.point2.x = end.x + 2.0f / 3 * (p[0].x - end.x);
				bezier.point2.y = end.y + 2.0f / 3 * (p[0].y - end.y);
			} else {
				bezier.point1 = p[0];
				bezier.point2 = p[1];
			}
			bezier.point3 = end;
			if (lines_only) {
				int steps = 16, step;
				for (step = 1; step <= steps; ++step) {
					const float t = (float)step / steps, u = 1 - t;
					D2D1_POINT_2F q;
					q.x = u * u * u * current.x + 3 * u * u * t * bezier.point1.x + 3 * u * t * t * bezier.point2.x + t * t * t * end.x;
					q.y = u * u * u * current.y + 3 * u * u * t * bezier.point1.y + 3 * u * t * t * bezier.point2.y + t * t * t * end.y;
					ID2D1SimplifiedGeometrySink_AddLines(sink, &q, 1);
				}
			} else {
				ID2D1SimplifiedGeometrySink_AddBeziers(sink, &bezier, 1);
			}
			current = end;
		} else if (verb == 4 && open) {
			ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
			open = FALSE;
		}
	}
	if (open)
		ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_OPEN);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE geometry_Simplify(void *iface, D2D1_GEOMETRY_SIMPLIFICATION_OPTION option, const D2D1_MATRIX_3X2_F *transform,
	FLOAT tolerance, ID2D1SimplifiedGeometrySink *sink)
{
	return replay(&geometry_of(iface)->path, transform, option == D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES, tolerance, sink);
}

/* Two paths combined by the host (union, intersection, exclusive or, exclusion), the result fed to a sink. */
static HRESULT combine_paths(const struct path *one, const struct path *two, D2D1_COMBINE_MODE mode, const D2D1_MATRIX_3X2_F *transform,
	ID2D1SimplifiedGeometrySink *sink)
{
	const DWORD fills = (one->fill == D2D1_FILL_MODE_WINDING ? 1 : 0) | (two->fill == D2D1_FILL_MODE_WINDING ? 2 : 0);
	DWORD answer = RavenPathCombine(one->data, one->count, two->data, two->count, fills, (DWORD)mode, NULL, 0), count;
	struct path result = {0};
	HRESULT hr;
	if (answer == 0x7fffffff)
		return E_FAIL;
	count = answer & 0x7fffffff;
	if (count) {
		result.data = ALLOC(count * sizeof(float));
		if (!result.data)
			return E_OUTOFMEMORY;
		result.capacity = count;
		answer = RavenPathCombine(one->data, one->count, two->data, two->count, fills, (DWORD)mode, result.data, count);
		result.count = answer & 0x7fffffff;
	}
	result.fill = (answer >> 31) ? D2D1_FILL_MODE_WINDING : D2D1_FILL_MODE_ALTERNATE;
	hr = replay(&result, transform, FALSE, 0, sink);
	path_free(&result);
	return hr;
}

static HRESULT STDMETHODCALLTYPE geometry_CombineWithGeometry(void *iface, ID2D1Geometry *input, D2D1_COMBINE_MODE mode,
	const D2D1_MATRIX_3X2_F *input_transform, FLOAT tolerance, ID2D1SimplifiedGeometrySink *sink)
{
	struct geometry *self = geometry_of(iface);
	struct path other;
	HRESULT hr;
	if (!input || !sink)
		return E_INVALIDARG;
	path_copy(&other, &((struct geometry *)input)->path);
	if (input_transform)
		path_transform(&other, input_transform);
	hr = combine_paths(&self->path, &other, mode, NULL, sink);
	path_free(&other);
	return hr;
}

/* The area a stroke of the geometry covers, as figures to fill: the host strokes the path with the width and style. */
static HRESULT STDMETHODCALLTYPE geometry_Widen(void *iface, FLOAT width, ID2D1StrokeStyle *style, const D2D1_MATRIX_3X2_F *transform, FLOAT tolerance,
	ID2D1SimplifiedGeometrySink *sink)
{
	struct geometry *self = geometry_of(iface);
	struct stroke *pattern = (struct stroke *)style;
	float stroke[7 + 64];
	struct path source, result = {0};
	DWORD answer, count;
	HRESULT hr;
	memset(stroke, 0, sizeof(stroke));
	stroke[0] = width;
	stroke[4] = 10;
	if (pattern) {
		stroke[1] = (float)pattern->properties.startCap;
		stroke[2] = (float)pattern->properties.endCap;
		stroke[3] = (float)pattern->properties.lineJoin;
		stroke[4] = pattern->properties.miterLimit;
		stroke[5] = pattern->properties.dashOffset * width;
		stroke[6] = (float)stroke_pattern(pattern, width, stroke + 7);
	}
	/* The stroke is taken in the space the transform leads to, as Direct2D widens a transformed geometry. */
	path_copy(&source, &self->path);
	if (transform)
		path_transform(&source, transform);
	answer = RavenPathWiden(source.data, source.count, source.fill == D2D1_FILL_MODE_WINDING, stroke, 7 + (DWORD)stroke[6], NULL, 0);
	if (answer == 0x7fffffff) {
		path_free(&source);
		return E_FAIL;
	}
	count = answer & 0x7fffffff;
	if (count && (result.data = ALLOC(count * sizeof(float)))) {
		result.capacity = count;
		answer = RavenPathWiden(source.data, source.count, source.fill == D2D1_FILL_MODE_WINDING, stroke, 7 + (DWORD)stroke[6], result.data, count);
		result.count = answer & 0x7fffffff;
	}
	result.fill = (answer >> 31) ? D2D1_FILL_MODE_WINDING : D2D1_FILL_MODE_ALTERNATE;
	hr = replay(&result, NULL, FALSE, 0, sink);
	path_free(&result);
	path_free(&source);
	return hr;
}

/* The outline: the figures with their overlaps merged, as the union with nothing gives them. */
static HRESULT STDMETHODCALLTYPE geometry_Outline(void *iface, const D2D1_MATRIX_3X2_F *transform, FLOAT tolerance, ID2D1SimplifiedGeometrySink *sink)
{
	struct path empty = {0};
	empty.fill = D2D1_FILL_MODE_ALTERNATE;
	return combine_paths(&geometry_of(iface)->path, &empty, D2D1_COMBINE_MODE_UNION, transform, sink);
}

/* The area of the flattened figures, by the shoelace formula; the length, by summing the segments. */
static HRESULT STDMETHODCALLTYPE geometry_ComputeArea(void *iface, const D2D1_MATRIX_3X2_F *transform, FLOAT tolerance, FLOAT *area)
{
	struct flat flat;
	UINT32 figure;
	float total = 0;
	flatten(&geometry_of(iface)->path, transform, tolerance, &flat);
	for (figure = 0; figure < flat.figures; ++figure) {
		const UINT32 begin = flat.starts[figure], end = figure + 1 < flat.figures ? flat.starts[figure + 1] : flat.count;
		UINT32 index;
		float sum = 0;
		for (index = begin; index < end; ++index) {
			const D2D1_POINT_2F a = flat.points[index], b = flat.points[index + 1 < end ? index + 1 : begin];
			sum += a.x * b.y - b.x * a.y;
		}
		total += fabs_(sum) / 2;
	}
	flat_free(&flat);
	*area = total;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE geometry_ComputeLength(void *iface, const D2D1_MATRIX_3X2_F *transform, FLOAT tolerance, FLOAT *length)
{
	struct flat flat;
	UINT32 figure;
	float total = 0;
	flatten(&geometry_of(iface)->path, transform, tolerance, &flat);
	for (figure = 0; figure < flat.figures; ++figure) {
		const UINT32 begin = flat.starts[figure], end = figure + 1 < flat.figures ? flat.starts[figure + 1] : flat.count;
		UINT32 index;
		for (index = begin; index + 1 < end; ++index) {
			const float dx = flat.points[index + 1].x - flat.points[index].x, dy = flat.points[index + 1].y - flat.points[index].y;
			total += fsqrt(dx * dx + dy * dy);
		}
	}
	flat_free(&flat);
	*length = total;
	return S_OK;
}

static void STDMETHODCALLTYPE geometry_GetRect(void *iface, D2D1_RECT_F *rect) { *rect = geometry_of(iface)->rect; }
static void STDMETHODCALLTYPE geometry_GetRoundedRect(void *iface, D2D1_ROUNDED_RECT *rect) { *rect = geometry_of(iface)->rounded; }
static void STDMETHODCALLTYPE geometry_GetEllipse(void *iface, D2D1_ELLIPSE *ellipse) { *ellipse = geometry_of(iface)->ellipse; }
static void STDMETHODCALLTYPE geometry_GetSourceGeometry(void *iface, ID2D1Geometry **source)
{
	struct geometry *self = geometry_of(iface);
	*source = (ID2D1Geometry *)self->source;
	if (self->source)
		object_AddRef(self->source);
}
static void STDMETHODCALLTYPE geometry_GetTransform(void *iface, D2D1_MATRIX_3X2_F *transform) { *transform = geometry_of(iface)->transform; }
static D2D1_FILL_MODE STDMETHODCALLTYPE geometry_GetFillMode(void *iface) { return geometry_of(iface)->path.fill; }
static UINT32 STDMETHODCALLTYPE geometry_GetSourceGeometryCount(void *iface) { return geometry_of(iface)->member_count; }
static void STDMETHODCALLTYPE geometry_GetSourceGeometries(void *iface, ID2D1Geometry **out, UINT32 count)
{
	struct geometry *self = geometry_of(iface);
	UINT32 index;
	for (index = 0; index < count; ++index) {
		out[index] = index < self->member_count ? (ID2D1Geometry *)self->members[index] : NULL;
		if (out[index])
			object_AddRef(out[index]);
	}
}
static HRESULT STDMETHODCALLTYPE geometry_GetSegmentCount(void *iface, UINT32 *count)
{
	struct geometry *self = geometry_of(iface);
	if (self->state != 2)
		return D2DERR_WRONG_STATE;
	*count = self->segments;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE geometry_GetFigureCount(void *iface, UINT32 *count)
{
	struct geometry *self = geometry_of(iface);
	if (self->state != 2)
		return D2DERR_WRONG_STATE;
	*count = self->figures;
	return S_OK;
}

static void **rectangle_table, **rounded_table, **ellipse_table, **path_table, **group_table, **transformed_table, **sink_table;
static const GUID *const rectangle_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Geometry, &IID_ID2D1RectangleGeometry};
static const GUID *const rounded_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Geometry, &IID_ID2D1RoundedRectangleGeometry};
static const GUID *const ellipse_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Geometry, &IID_ID2D1EllipseGeometry};
static const GUID *const path_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Geometry, &IID_ID2D1PathGeometry, &IID_ID2D1PathGeometry1};
static const GUID *const group_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Geometry, &IID_ID2D1GeometryGroup};
static const GUID *const transformed_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Geometry, &IID_ID2D1TransformedGeometry};

static struct geometry *make_geometry(IUnknown *factory, int kind)
{
	struct geometry *self = ALLOC(sizeof(*self));
	void **table = kind == GEOMETRY_RECTANGLE ? rectangle_table : kind == GEOMETRY_ROUNDED ? rounded_table : kind == GEOMETRY_ELLIPSE ? ellipse_table :
		kind == GEOMETRY_PATH ? path_table : kind == GEOMETRY_GROUP ? group_table : transformed_table;
	const GUID *const *iids = kind == GEOMETRY_RECTANGLE ? rectangle_iids : kind == GEOMETRY_ROUNDED ? rounded_iids : kind == GEOMETRY_ELLIPSE ?
		ellipse_iids : kind == GEOMETRY_PATH ? path_iids : kind == GEOMETRY_GROUP ? group_iids : transformed_iids;
	if (!self)
		return NULL;
	object_init(&self->base, table, geometry_destroy, iids, kind == GEOMETRY_PATH ? 4 : 3, factory);
	self->kind = kind;
	self->path.fill = D2D1_FILL_MODE_ALTERNATE;
	self->transform = identity;
	return self;
}

/* ---- the sink a path geometry is written through ---- */
struct sink {
	struct object base;
	struct geometry *geometry;
	D2D1_POINT_2F current;
	BOOL in_figure;
};

static void sink_destroy(struct object *base)
{
	object_Release(((struct sink *)base)->geometry);
}

static struct geometry *sink_target(void *iface)
{
	return ((struct sink *)iface)->geometry;
}

static void STDMETHODCALLTYPE sink_SetFillMode(void *iface, D2D1_FILL_MODE mode) { sink_target(iface)->path.fill = mode; }
static void STDMETHODCALLTYPE sink_SetSegmentFlags(void *iface, D2D1_PATH_SEGMENT flags) {}
static void STDMETHODCALLTYPE sink_BeginFigure(void *iface, D2D1_POINT_2F start, D2D1_FIGURE_BEGIN begin)
{
	struct sink *self = iface;
	path_move(&self->geometry->path, start);
	self->current = start;
	self->in_figure = TRUE;
	++self->geometry->figures;
}
static void STDMETHODCALLTYPE sink_AddLines(void *iface, const D2D1_POINT_2F *points, UINT32 count)
{
	struct sink *self = iface;
	UINT32 index;
	for (index = 0; index < count; ++index)
		path_line(&self->geometry->path, points[index]);
	if (count)
		self->current = points[count - 1];
	self->geometry->segments += count;
}
static void STDMETHODCALLTYPE sink_AddBeziers(void *iface, const D2D1_BEZIER_SEGMENT *beziers, UINT32 count)
{
	struct sink *self = iface;
	UINT32 index;
	for (index = 0; index < count; ++index)
		path_cubic(&self->geometry->path, beziers[index].point1, beziers[index].point2, beziers[index].point3);
	if (count)
		self->current = beziers[count - 1].point3;
	self->geometry->segments += count;
}
static void STDMETHODCALLTYPE sink_EndFigure(void *iface, D2D1_FIGURE_END end)
{
	struct sink *self = iface;
	if (end == D2D1_FIGURE_END_CLOSED)
		path_close(&self->geometry->path);
	self->in_figure = FALSE;
}
static HRESULT STDMETHODCALLTYPE sink_Close(void *iface)
{
	struct sink *self = iface;
	if (self->geometry->state != 1)
		return D2DERR_WRONG_STATE;
	self->geometry->state = 2;
	return S_OK;
}
static void STDMETHODCALLTYPE sink_AddLine(void *iface, D2D1_POINT_2F point) { sink_AddLines(iface, &point, 1); }
static void STDMETHODCALLTYPE sink_AddBezier(void *iface, const D2D1_BEZIER_SEGMENT *bezier) { sink_AddBeziers(iface, bezier, 1); }
static void STDMETHODCALLTYPE sink_AddQuadraticBeziers(void *iface, const D2D1_QUADRATIC_BEZIER_SEGMENT *beziers, UINT32 count)
{
	struct sink *self = iface;
	UINT32 index;
	for (index = 0; index < count; ++index)
		path_quad(&self->geometry->path, beziers[index].point1, beziers[index].point2);
	if (count)
		self->current = beziers[count - 1].point2;
	self->geometry->segments += count;
}
static void STDMETHODCALLTYPE sink_AddQuadraticBezier(void *iface, const D2D1_QUADRATIC_BEZIER_SEGMENT *bezier) { sink_AddQuadraticBeziers(iface, bezier, 1); }
static void STDMETHODCALLTYPE sink_AddArc(void *iface, const D2D1_ARC_SEGMENT *arc)
{
	struct sink *self = iface;
	path_arc(&self->geometry->path, self->current, arc);
	self->current = arc->point;
	++self->geometry->segments;
}

static const GUID *const sink_iids[] = {&IID_ID2D1SimplifiedGeometrySink, &IID_ID2D1GeometrySink};

static HRESULT STDMETHODCALLTYPE geometry_Open(void *iface, ID2D1GeometrySink **out)
{
	struct geometry *self = geometry_of(iface);
	struct sink *sink;
	if (self->state != 0)
		return D2DERR_WRONG_STATE;
	if (!(sink = ALLOC(sizeof(*sink))))
		return E_OUTOFMEMORY;
	object_init(&sink->base, sink_table, sink_destroy, sink_iids, 2, NULL);
	sink->geometry = self;
	object_AddRef(self);
	self->state = 1;
	*out = (ID2D1GeometrySink *)sink;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE geometry_Stream(void *iface, ID2D1GeometrySink *sink)
{
	struct geometry *self = geometry_of(iface);
	if (self->state != 2)
		return D2DERR_WRONG_STATE;
	return replay(&self->path, NULL, FALSE, 0, (ID2D1SimplifiedGeometrySink *)sink);
}

/* Geometry slots shared by every kind (ID2D1Geometry, 4 to 16). */
static void set_geometry(void **table)
{
	set_resource(table);
	table[ID2D1RectangleGeometry_GetBounds_slot] = (void *)geometry_GetBounds;
	table[ID2D1RectangleGeometry_GetWidenedBounds_slot] = (void *)geometry_GetWidenedBounds;
	table[ID2D1RectangleGeometry_StrokeContainsPoint_slot] = (void *)geometry_StrokeContainsPoint;
	table[ID2D1RectangleGeometry_FillContainsPoint_slot] = (void *)geometry_FillContainsPoint;
	table[ID2D1RectangleGeometry_Simplify_slot] = (void *)geometry_Simplify;
	table[ID2D1RectangleGeometry_ComputeArea_slot] = (void *)geometry_ComputeArea;
	table[ID2D1RectangleGeometry_ComputeLength_slot] = (void *)geometry_ComputeLength;
	table[ID2D1RectangleGeometry_CombineWithGeometry_slot] = (void *)geometry_CombineWithGeometry;
	table[ID2D1RectangleGeometry_Outline_slot] = (void *)geometry_Outline;
	table[ID2D1RectangleGeometry_Widen_slot] = (void *)geometry_Widen;
}

static void build_geometry_tables(void)
{
	rectangle_table = make_table(ID2D1RectangleGeometry_table, ID2D1RectangleGeometry_methods);
	set_geometry(rectangle_table);
	rectangle_table[ID2D1RectangleGeometry_GetRect_slot] = (void *)geometry_GetRect;
	rounded_table = make_table(ID2D1RoundedRectangleGeometry_table, ID2D1RoundedRectangleGeometry_methods);
	set_geometry(rounded_table);
	rounded_table[ID2D1RoundedRectangleGeometry_GetRoundedRect_slot] = (void *)geometry_GetRoundedRect;
	ellipse_table = make_table(ID2D1EllipseGeometry_table, ID2D1EllipseGeometry_methods);
	set_geometry(ellipse_table);
	ellipse_table[ID2D1EllipseGeometry_GetEllipse_slot] = (void *)geometry_GetEllipse;
	path_table = make_table(ID2D1PathGeometry1_table, ID2D1PathGeometry1_methods);
	set_geometry(path_table);
	path_table[ID2D1PathGeometry1_Open_slot] = (void *)geometry_Open;
	path_table[ID2D1PathGeometry1_Stream_slot] = (void *)geometry_Stream;
	path_table[ID2D1PathGeometry1_GetSegmentCount_slot] = (void *)geometry_GetSegmentCount;
	path_table[ID2D1PathGeometry1_GetFigureCount_slot] = (void *)geometry_GetFigureCount;
	group_table = make_table(ID2D1GeometryGroup_table, ID2D1GeometryGroup_methods);
	set_geometry(group_table);
	group_table[ID2D1GeometryGroup_GetFillMode_slot] = (void *)geometry_GetFillMode;
	group_table[ID2D1GeometryGroup_GetSourceGeometryCount_slot] = (void *)geometry_GetSourceGeometryCount;
	group_table[ID2D1GeometryGroup_GetSourceGeometries_slot] = (void *)geometry_GetSourceGeometries;
	transformed_table = make_table(ID2D1TransformedGeometry_table, ID2D1TransformedGeometry_methods);
	set_geometry(transformed_table);
	transformed_table[ID2D1TransformedGeometry_GetSourceGeometry_slot] = (void *)geometry_GetSourceGeometry;
	transformed_table[ID2D1TransformedGeometry_GetTransform_slot] = (void *)geometry_GetTransform;
	sink_table = make_table(ID2D1GeometrySink_table, ID2D1GeometrySink_methods);
	set_unknown(sink_table);
	sink_table[ID2D1GeometrySink_SetFillMode_slot] = (void *)sink_SetFillMode;
	sink_table[ID2D1GeometrySink_SetSegmentFlags_slot] = (void *)sink_SetSegmentFlags;
	sink_table[ID2D1GeometrySink_BeginFigure_slot] = (void *)sink_BeginFigure;
	sink_table[ID2D1GeometrySink_AddLines_slot] = (void *)sink_AddLines;
	sink_table[ID2D1GeometrySink_AddBeziers_slot] = (void *)sink_AddBeziers;
	sink_table[ID2D1GeometrySink_EndFigure_slot] = (void *)sink_EndFigure;
	sink_table[ID2D1GeometrySink_Close_slot] = (void *)sink_Close;
	sink_table[ID2D1GeometrySink_AddLine_slot] = (void *)sink_AddLine;
	sink_table[ID2D1GeometrySink_AddBezier_slot] = (void *)sink_AddBezier;
	sink_table[ID2D1GeometrySink_AddQuadraticBezier_slot] = (void *)sink_AddQuadraticBezier;
	sink_table[ID2D1GeometrySink_AddQuadraticBeziers_slot] = (void *)sink_AddQuadraticBeziers;
	sink_table[ID2D1GeometrySink_AddArc_slot] = (void *)sink_AddArc;
	stroke_table = make_table(ID2D1StrokeStyle1_table, ID2D1StrokeStyle1_methods);
	set_resource(stroke_table);
	stroke_table[ID2D1StrokeStyle1_GetStartCap_slot] = (void *)stroke_GetStartCap;
	stroke_table[ID2D1StrokeStyle1_GetEndCap_slot] = (void *)stroke_GetEndCap;
	stroke_table[ID2D1StrokeStyle1_GetDashCap_slot] = (void *)stroke_GetDashCap;
	stroke_table[ID2D1StrokeStyle1_GetMiterLimit_slot] = (void *)stroke_GetMiterLimit;
	stroke_table[ID2D1StrokeStyle1_GetLineJoin_slot] = (void *)stroke_GetLineJoin;
	stroke_table[ID2D1StrokeStyle1_GetDashOffset_slot] = (void *)stroke_GetDashOffset;
	stroke_table[ID2D1StrokeStyle1_GetDashStyle_slot] = (void *)stroke_GetDashStyle;
	stroke_table[ID2D1StrokeStyle1_GetDashesCount_slot] = (void *)stroke_GetDashesCount;
	stroke_table[ID2D1StrokeStyle1_GetDashes_slot] = (void *)stroke_GetDashes;
	stroke_table[ID2D1StrokeStyle1_GetStrokeTransformType_slot] = (void *)stroke_GetStrokeTransformType;
}

/* ---- gradient stops ---- */
struct stops {
	struct object base;
	D2D1_GRADIENT_STOP *stops;
	UINT32 count;
	D2D1_GAMMA gamma;
	D2D1_EXTEND_MODE extend;
};

static void stops_destroy(struct object *base)
{
	FREE(((struct stops *)base)->stops);
}

static UINT32 STDMETHODCALLTYPE stops_GetGradientStopCount(void *iface) { return ((struct stops *)iface)->count; }
static void STDMETHODCALLTYPE stops_GetGradientStops(void *iface, D2D1_GRADIENT_STOP *out, UINT32 count)
{
	struct stops *self = iface;
	memcpy(out, self->stops, (count < self->count ? count : self->count) * sizeof(*out));
}
static D2D1_GAMMA STDMETHODCALLTYPE stops_GetColorInterpolationGamma(void *iface) { return ((struct stops *)iface)->gamma; }
static D2D1_EXTEND_MODE STDMETHODCALLTYPE stops_GetExtendMode(void *iface) { return ((struct stops *)iface)->extend; }
static D2D1_COLOR_SPACE STDMETHODCALLTYPE stops_GetSpace(void *iface) { return D2D1_COLOR_SPACE_SRGB; }
static D2D1_BUFFER_PRECISION STDMETHODCALLTYPE stops_GetBufferPrecision(void *iface) { return D2D1_BUFFER_PRECISION_8BPC_UNORM; }
static D2D1_COLOR_INTERPOLATION_MODE STDMETHODCALLTYPE stops_GetColorInterpolationMode(void *iface) { return D2D1_COLOR_INTERPOLATION_MODE_STRAIGHT; }

static void **stops_table;
static const GUID *const stops_iids[] = {&IID_ID2D1Resource, &IID_ID2D1GradientStopCollection, &IID_ID2D1GradientStopCollection1};

static HRESULT make_stops(IUnknown *factory, const D2D1_GRADIENT_STOP *stops, UINT32 count, D2D1_GAMMA gamma, D2D1_EXTEND_MODE extend, void **out)
{
	struct stops *self;
	if (!count || !stops)
		return E_INVALIDARG;
	if (!(self = ALLOC(sizeof(*self))))
		return E_OUTOFMEMORY;
	object_init(&self->base, stops_table, stops_destroy, stops_iids, 3, factory);
	self->stops = ALLOC(count * sizeof(*stops));
	memcpy(self->stops, stops, count * sizeof(*stops));
	self->count = count;
	self->gamma = gamma;
	self->extend = extend;
	/* The stops in order of position, as Direct2D sorts them. */
	{
		UINT32 i, j;
		for (i = 1; i < count; ++i)
			for (j = i; j > 0 && self->stops[j].position < self->stops[j - 1].position; --j) {
				const D2D1_GRADIENT_STOP swap = self->stops[j];
				self->stops[j] = self->stops[j - 1];
				self->stops[j - 1] = swap;
			}
	}
	*out = self;
	return S_OK;
}

/* ---- bitmaps: a host surface, its size in pixels and its resolution ---- */
struct bitmap {
	struct object base;
	DWORD surface;
	D2D1_SIZE_U pixels;
	D2D1_PIXEL_FORMAT format;
	float dpi_x, dpi_y;
	D2D1_BITMAP_OPTIONS options;
	IDXGISurface *dxgi;
	BYTE *mapped;
	UINT32 mapped_pitch;
	D2D1_MAP_OPTIONS map_options;
	ID2D1ColorContext *color_context;
};

static void bitmap_destroy(struct object *base)
{
	struct bitmap *self = (struct bitmap *)base;
	if (self->surface)
		RavenCanvasDestroy(self->surface);
	if (self->dxgi)
		IDXGISurface_Release(self->dxgi);
	FREE(self->mapped);
}

static D2D1_SIZE_F *STDMETHODCALLTYPE bitmap_GetSize(void *iface, D2D1_SIZE_F *size)
{
	struct bitmap *self = iface;
	size->width = self->pixels.width * 96.0f / self->dpi_x;
	size->height = self->pixels.height * 96.0f / self->dpi_y;
	return size;
}
static D2D1_SIZE_U *STDMETHODCALLTYPE bitmap_GetPixelSize(void *iface, D2D1_SIZE_U *size) { *size = ((struct bitmap *)iface)->pixels; return size; }
static D2D1_PIXEL_FORMAT *STDMETHODCALLTYPE bitmap_GetPixelFormat(void *iface, D2D1_PIXEL_FORMAT *format) { *format = ((struct bitmap *)iface)->format; return format; }
static void STDMETHODCALLTYPE bitmap_GetDpi(void *iface, FLOAT *x, FLOAT *y)
{
	*x = ((struct bitmap *)iface)->dpi_x;
	*y = ((struct bitmap *)iface)->dpi_y;
}
static D2D1_BITMAP_OPTIONS STDMETHODCALLTYPE bitmap_GetOptions(void *iface) { return ((struct bitmap *)iface)->options; }
static void STDMETHODCALLTYPE bitmap_GetColorContext(void *iface, ID2D1ColorContext **context) { *context = NULL; }
static HRESULT STDMETHODCALLTYPE bitmap_GetSurface(void *iface, IDXGISurface **surface)
{
	struct bitmap *self = iface;
	*surface = self->dxgi;
	if (!self->dxgi)
		return D2DERR_INVALID_CALL;
	IDXGISurface_AddRef(self->dxgi);
	return S_OK;
}

/* Pixels in the format of the bitmap: four bytes each, blue first, premultiplied unless the format says alpha is ignored. Programs
 * hand over 32-bit formats; other formats are refused. */

static HRESULT STDMETHODCALLTYPE bitmap_CopyFromMemory(void *iface, const D2D1_RECT_U *rect, const void *data, UINT32 pitch)
{
	struct bitmap *self = iface;
	D2D1_RECT_U area = {0, 0, self->pixels.width, self->pixels.height};
	if (rect)
		area = *rect;
	if (area.right <= area.left || area.bottom <= area.top)
		return S_OK;
	if (self->format.format == DXGI_FORMAT_A8_UNORM) {
		/* An alpha-only bitmap gives a byte a pixel: the host keeps it as black with that alpha. */
		const UINT32 width = area.right - area.left, height = area.bottom - area.top;
		BYTE *copy = ALLOC((size_t)width * height * 4);
		UINT32 row, column;
		if (!copy)
			return E_OUTOFMEMORY;
		for (row = 0; row < height; ++row) {
			const BYTE *from = (const BYTE *)data + (size_t)row * pitch;
			BYTE *to = copy + (size_t)row * width * 4;
			for (column = 0; column < width; ++column) {
				to[column * 4] = to[column * 4 + 1] = to[column * 4 + 2] = 0;
				to[column * 4 + 3] = from[column];
			}
		}
		RavenCanvasPixels(self->surface, (INT32)area.left, (INT32)area.top, width, height, copy, width * 4, 1);
		FREE(copy);
		return S_OK;
	}
	if (self->format.alphaMode == D2D1_ALPHA_MODE_IGNORE) {
		/* The alpha byte means nothing: the host keeps the pixels opaque. */
		const UINT32 width = area.right - area.left, height = area.bottom - area.top;
		BYTE *copy = ALLOC((size_t)width * height * 4);
		UINT32 row, column;
		if (!copy)
			return E_OUTOFMEMORY;
		for (row = 0; row < height; ++row) {
			const BYTE *from = (const BYTE *)data + (size_t)row * pitch;
			BYTE *to = copy + (size_t)row * width * 4;
			for (column = 0; column < width; ++column) {
				to[column * 4] = from[column * 4];
				to[column * 4 + 1] = from[column * 4 + 1];
				to[column * 4 + 2] = from[column * 4 + 2];
				to[column * 4 + 3] = 255;
			}
		}
		RavenCanvasPixels(self->surface, (INT32)area.left, (INT32)area.top, width, height, copy, width * 4, 1);
		FREE(copy);
		return S_OK;
	}
	return RavenCanvasPixels(self->surface, (INT32)area.left, (INT32)area.top, area.right - area.left, area.bottom - area.top, (void *)data, pitch, 1) ?
		S_OK : E_FAIL;
}

static void copy_surface(DWORD to, DWORD from, const D2D1_POINT_2U *point, const D2D1_RECT_U *rect, D2D1_SIZE_U source_size)
{
	struct command command;
	memset(&command, 0, sizeof(command));
	command.op = OP_COPY;
	command.bitmap = from;
	command.source[0] = rect ? (float)rect->left : 0;
	command.source[1] = rect ? (float)rect->top : 0;
	command.source[2] = rect ? (float)(rect->right - rect->left) : (float)source_size.width;
	command.source[3] = rect ? (float)(rect->bottom - rect->top) : (float)source_size.height;
	command.rect[0] = point ? (float)point->x : 0;
	command.rect[1] = point ? (float)point->y : 0;
	RavenCanvasDraw(to, &command, NULL, 0);
}

static HRESULT STDMETHODCALLTYPE bitmap_CopyFromBitmap(void *iface, const D2D1_POINT_2U *point, ID2D1Bitmap *source, const D2D1_RECT_U *rect)
{
	struct bitmap *self = iface, *from = (struct bitmap *)source;
	if (!from)
		return E_INVALIDARG;
	copy_surface(self->surface, from->surface, point, rect, from->pixels);
	return S_OK;
}

/* A CPU-readable bitmap is read whole into memory the program sees; writes go back on unmapping. */
static HRESULT STDMETHODCALLTYPE bitmap_Map(void *iface, D2D1_MAP_OPTIONS options, D2D1_MAPPED_RECT *mapped)
{
	struct bitmap *self = iface;
	const BOOL alpha = self->format.format == DXGI_FORMAT_A8_UNORM;
	if (self->mapped)
		return D2DERR_WRONG_STATE;
	/* The mapping is laid out as the format is: four bytes a pixel, or one for an alpha-only bitmap. */
	self->mapped_pitch = self->pixels.width * (alpha ? 1 : 4);
	if (!(self->mapped = ALLOC((size_t)self->pixels.width * 4 * self->pixels.height)))
		return E_OUTOFMEMORY;
	if (!(options & D2D1_MAP_OPTIONS_DISCARD)) {
		RavenCanvasPixels(self->surface, 0, 0, self->pixels.width, self->pixels.height, self->mapped, self->pixels.width * 4, 0);
		if (alpha) {
			UINT32 index;
			for (index = 0; index < self->pixels.width * self->pixels.height; ++index)
				self->mapped[index] = self->mapped[index * 4 + 3];
		}
	}
	self->map_options = options;
	mapped->pitch = self->mapped_pitch;
	mapped->bits = self->mapped;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE bitmap_Unmap(void *iface)
{
	struct bitmap *self = iface;
	if (!self->mapped)
		return D2DERR_WRONG_STATE;
	if (self->map_options & D2D1_MAP_OPTIONS_WRITE)
		bitmap_CopyFromMemory(self, NULL, self->mapped, self->mapped_pitch);
	FREE(self->mapped);
	self->mapped = NULL;
	return S_OK;
}

static void **bitmap_table;
static const GUID *const bitmap_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Image, &IID_ID2D1Bitmap, &IID_ID2D1Bitmap1};

static struct bitmap *make_bitmap(IUnknown *factory, D2D1_SIZE_U size, D2D1_PIXEL_FORMAT format, float dpi_x, float dpi_y, D2D1_BITMAP_OPTIONS options)
{
	struct bitmap *self;
	if (!size.width || !size.height)
		size.width = size.height = 1;
	if (!(self = ALLOC(sizeof(*self))))
		return NULL;
	object_init(&self->base, bitmap_table, bitmap_destroy, bitmap_iids, 4, factory);
	self->surface = RavenCanvasCreate(size.width, size.height);
	if (!self->surface) {
		object_Release(self);
		return NULL;
	}
	self->pixels = size;
	if (format.format == DXGI_FORMAT_UNKNOWN)
		format.format = DXGI_FORMAT_B8G8R8A8_UNORM;
	if (format.alphaMode == D2D1_ALPHA_MODE_UNKNOWN)
		format.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
	self->format = format;
	self->dpi_x = dpi_x > 0 ? dpi_x : 96;
	self->dpi_y = dpi_y > 0 ? dpi_y : 96;
	self->options = options;
	return self;
}

/* ---- brushes ---- */
enum { KIND_SOLID, KIND_LINEAR, KIND_RADIAL, KIND_BITMAP, KIND_IMAGE };

struct brush {
	struct object base;
	int kind;
	float opacity;
	D2D1_MATRIX_3X2_F transform;
	D2D1_COLOR_F color;
	struct stops *stops;
	D2D1_POINT_2F start, end, center, offset;
	float radius_x, radius_y;
	struct bitmap *bitmap;
	D2D1_EXTEND_MODE extend_x, extend_y;
	D2D1_INTERPOLATION_MODE interpolation;
	D2D1_RECT_F source;
	BOOL has_source;
};

static void brush_destroy(struct object *base)
{
	struct brush *self = (struct brush *)base;
	if (self->stops)
		object_Release(self->stops);
	if (self->bitmap)
		object_Release(self->bitmap);
}

static void STDMETHODCALLTYPE brush_SetOpacity(void *iface, FLOAT opacity) { ((struct brush *)iface)->opacity = opacity; }
static void STDMETHODCALLTYPE brush_SetTransform(void *iface, const D2D1_MATRIX_3X2_F *m) { ((struct brush *)iface)->transform = *m; }
static FLOAT STDMETHODCALLTYPE brush_GetOpacity(void *iface) { return ((struct brush *)iface)->opacity; }
static void STDMETHODCALLTYPE brush_GetTransform(void *iface, D2D1_MATRIX_3X2_F *m) { *m = ((struct brush *)iface)->transform; }
static void STDMETHODCALLTYPE brush_SetColor(void *iface, const D2D1_COLOR_F *color) { ((struct brush *)iface)->color = *color; }
static D2D1_COLOR_F *STDMETHODCALLTYPE brush_GetColor(void *iface, D2D1_COLOR_F *color) { *color = ((struct brush *)iface)->color; return color; }
static void STDMETHODCALLTYPE brush_SetStartPoint(void *iface, D2D1_POINT_2F p) { ((struct brush *)iface)->start = p; }
static void STDMETHODCALLTYPE brush_SetEndPoint(void *iface, D2D1_POINT_2F p) { ((struct brush *)iface)->end = p; }
static D2D1_POINT_2F *STDMETHODCALLTYPE brush_GetStartPoint(void *iface, D2D1_POINT_2F *p) { *p = ((struct brush *)iface)->start; return p; }
static D2D1_POINT_2F *STDMETHODCALLTYPE brush_GetEndPoint(void *iface, D2D1_POINT_2F *p) { *p = ((struct brush *)iface)->end; return p; }
static void STDMETHODCALLTYPE brush_GetGradientStopCollection(void *iface, ID2D1GradientStopCollection **stops)
{
	struct brush *self = iface;
	*stops = (ID2D1GradientStopCollection *)self->stops;
	if (self->stops)
		object_AddRef(self->stops);
}
static void STDMETHODCALLTYPE brush_SetCenter(void *iface, D2D1_POINT_2F p) { ((struct brush *)iface)->center = p; }
static void STDMETHODCALLTYPE brush_SetGradientOriginOffset(void *iface, D2D1_POINT_2F p) { ((struct brush *)iface)->offset = p; }
static void STDMETHODCALLTYPE brush_SetRadiusX(void *iface, FLOAT r) { ((struct brush *)iface)->radius_x = r; }
static void STDMETHODCALLTYPE brush_SetRadiusY(void *iface, FLOAT r) { ((struct brush *)iface)->radius_y = r; }
static D2D1_POINT_2F *STDMETHODCALLTYPE brush_GetCenter(void *iface, D2D1_POINT_2F *p) { *p = ((struct brush *)iface)->center; return p; }
static D2D1_POINT_2F *STDMETHODCALLTYPE brush_GetGradientOriginOffset(void *iface, D2D1_POINT_2F *p) { *p = ((struct brush *)iface)->offset; return p; }
static FLOAT STDMETHODCALLTYPE brush_GetRadiusX(void *iface) { return ((struct brush *)iface)->radius_x; }
static FLOAT STDMETHODCALLTYPE brush_GetRadiusY(void *iface) { return ((struct brush *)iface)->radius_y; }
static void STDMETHODCALLTYPE brush_SetExtendModeX(void *iface, D2D1_EXTEND_MODE mode) { ((struct brush *)iface)->extend_x = mode; }
static void STDMETHODCALLTYPE brush_SetExtendModeY(void *iface, D2D1_EXTEND_MODE mode) { ((struct brush *)iface)->extend_y = mode; }
static D2D1_EXTEND_MODE STDMETHODCALLTYPE brush_GetExtendModeX(void *iface) { return ((struct brush *)iface)->extend_x; }
static D2D1_EXTEND_MODE STDMETHODCALLTYPE brush_GetExtendModeY(void *iface) { return ((struct brush *)iface)->extend_y; }
/* The interpolation of Direct2D 1.0 (nearest or linear) and of 1.1 share one field: the old modes are the first two new ones. */
static void STDMETHODCALLTYPE brush_SetInterpolationMode(void *iface, UINT32 mode) { ((struct brush *)iface)->interpolation = mode; }
static UINT32 STDMETHODCALLTYPE brush_GetInterpolationMode(void *iface) { return ((struct brush *)iface)->interpolation; }
static void STDMETHODCALLTYPE brush_SetBitmap(void *iface, ID2D1Bitmap *bitmap)
{
	struct brush *self = iface;
	if (bitmap)
		object_AddRef(bitmap);
	if (self->bitmap)
		object_Release(self->bitmap);
	self->bitmap = (struct bitmap *)bitmap;
}
static void STDMETHODCALLTYPE brush_GetBitmap(void *iface, ID2D1Bitmap **bitmap)
{
	struct brush *self = iface;
	*bitmap = (ID2D1Bitmap *)self->bitmap;
	if (self->bitmap)
		object_AddRef(self->bitmap);
}
/* An image brush draws a bitmap; other images (effects, command lists) are not drawn by this component. */
static void STDMETHODCALLTYPE brush_SetImage(void *iface, ID2D1Image *image)
{
	struct brush *self = iface;
	ID2D1Bitmap *bitmap = NULL;
	if (image && FAILED(IUnknown_QueryInterface((IUnknown *)image, &IID_ID2D1Bitmap, (void **)&bitmap)))
		bitmap = NULL;
	brush_SetBitmap(iface, bitmap);
	if (bitmap)
		object_Release(bitmap);
	(void)self;
}
static void STDMETHODCALLTYPE brush_GetImage(void *iface, ID2D1Image **image) { brush_GetBitmap(iface, (ID2D1Bitmap **)image); }
static void STDMETHODCALLTYPE brush_SetSourceRectangle(void *iface, const D2D1_RECT_F *rect)
{
	((struct brush *)iface)->source = *rect;
	((struct brush *)iface)->has_source = TRUE;
}
static void STDMETHODCALLTYPE brush_GetSourceRectangle(void *iface, D2D1_RECT_F *rect) { *rect = ((struct brush *)iface)->source; }

static void **solid_table, **linear_table, **radial_table, **bitmap_brush_table, **image_brush_table;
static const GUID *const solid_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Brush, &IID_ID2D1SolidColorBrush};
static const GUID *const linear_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Brush, &IID_ID2D1LinearGradientBrush};
static const GUID *const radial_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Brush, &IID_ID2D1RadialGradientBrush};
static const GUID *const bitmap_brush_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Brush, &IID_ID2D1BitmapBrush, &IID_ID2D1BitmapBrush1};
static const GUID *const image_brush_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Brush, &IID_ID2D1ImageBrush};

static struct brush *make_brush(IUnknown *factory, int kind, const D2D1_BRUSH_PROPERTIES *properties)
{
	struct brush *self = ALLOC(sizeof(*self));
	if (!self)
		return NULL;
	switch (kind) {
	case KIND_SOLID: object_init(&self->base, solid_table, brush_destroy, solid_iids, 3, factory); break;
	case KIND_LINEAR: object_init(&self->base, linear_table, brush_destroy, linear_iids, 3, factory); break;
	case KIND_RADIAL: object_init(&self->base, radial_table, brush_destroy, radial_iids, 3, factory); break;
	case KIND_BITMAP: object_init(&self->base, bitmap_brush_table, brush_destroy, bitmap_brush_iids, 4, factory); break;
	default: object_init(&self->base, image_brush_table, brush_destroy, image_brush_iids, 3, factory); break;
	}
	self->kind = kind;
	self->opacity = properties ? properties->opacity : 1;
	self->transform = properties ? properties->transform : identity;
	self->interpolation = D2D1_INTERPOLATION_MODE_LINEAR;
	return self;
}

/* What a command record says of a brush. */
static BOOL put_brush(struct command *command, struct bytes *data, const struct brush *brush)
{
	if (!brush)
		return FALSE;
	command->opacity = brush->opacity;
	put_matrix(command->brush_transform, &brush->transform);
	switch (brush->kind) {
	case KIND_SOLID:
		command->brush = BRUSH_SOLID;
		command->color[0] = brush->color.r;
		command->color[1] = brush->color.g;
		command->color[2] = brush->color.b;
		command->color[3] = brush->color.a;
		return TRUE;
	case KIND_LINEAR:
	case KIND_RADIAL: {
		UINT32 index;
		if (!brush->stops)
			return FALSE;
		command->brush = brush->kind == KIND_LINEAR ? BRUSH_LINEAR : BRUSH_RADIAL;
		command->stops = brush->stops->count > 256 ? 256 : brush->stops->count;
		command->extend_x = command->extend_y = brush->stops->extend;
		if (brush->kind == KIND_LINEAR) {
			command->brush_points[0] = brush->start.x;
			command->brush_points[1] = brush->start.y;
			command->brush_points[2] = brush->end.x;
			command->brush_points[3] = brush->end.y;
		} else {
			command->brush_points[0] = brush->center.x;
			command->brush_points[1] = brush->center.y;
			command->brush_points[2] = brush->offset.x;
			command->brush_points[3] = brush->offset.y;
			command->brush_points[4] = brush->radius_x;
			command->brush_points[5] = brush->radius_y;
		}
		for (index = 0; index < command->stops; ++index) {
			const D2D1_GRADIENT_STOP *stop = &brush->stops->stops[index];
			const float values[5] = {stop->position, stop->color.r, stop->color.g, stop->color.b, stop->color.a};
			if (!bytes_add(data, values, sizeof(values)))
				return FALSE;
		}
		return TRUE;
	}
	default: {
		/* A bitmap brush paints the bitmap at its size in device pixels from the brush origin. */
		D2D1_MATRIX_3X2_F scale = identity;
		if (!brush->bitmap)
			return FALSE;
		command->brush = BRUSH_BITMAP;
		command->bitmap = brush->bitmap->surface;
		command->extend_x = brush->extend_x;
		command->extend_y = brush->extend_y;
		command->interpolation = brush->interpolation;
		scale._11 = 96.0f / brush->bitmap->dpi_x;
		scale._22 = 96.0f / brush->bitmap->dpi_y;
		scale = multiply(&scale, &brush->transform);
		put_matrix(command->brush_transform, &scale);
		if (brush->kind == KIND_IMAGE && brush->has_source) {
			command->source[0] = brush->source.left;
			command->source[1] = brush->source.top;
			command->source[2] = brush->source.right - brush->source.left;
			command->source[3] = brush->source.bottom - brush->source.top;
		}
		return TRUE;
	}
	}
}

/* ---- layers and drawing state blocks ---- */
struct layer {
	struct object base;
	D2D1_SIZE_F size;
};

static D2D1_SIZE_F *STDMETHODCALLTYPE layer_GetSize(void *iface, D2D1_SIZE_F *size) { *size = ((struct layer *)iface)->size; return size; }

static void **layer_table;
static const GUID *const layer_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Layer};

struct state_block {
	struct object base;
	D2D1_DRAWING_STATE_DESCRIPTION1 description;
	IDWriteRenderingParams *params;
};

static void state_block_destroy(struct object *base)
{
	struct state_block *self = (struct state_block *)base;
	if (self->params)
		IDWriteRenderingParams_Release(self->params);
}

static void STDMETHODCALLTYPE state_GetDescription(void *iface, D2D1_DRAWING_STATE_DESCRIPTION *out)
{
	memcpy(out, &((struct state_block *)iface)->description, sizeof(*out));
}
static void STDMETHODCALLTYPE state_SetDescription(void *iface, const D2D1_DRAWING_STATE_DESCRIPTION *in)
{
	memcpy(&((struct state_block *)iface)->description, in, sizeof(*in));
}
static void STDMETHODCALLTYPE state_GetDescription1(void *iface, D2D1_DRAWING_STATE_DESCRIPTION1 *out) { *out = ((struct state_block *)iface)->description; }
static void STDMETHODCALLTYPE state_SetDescription1(void *iface, const D2D1_DRAWING_STATE_DESCRIPTION1 *in) { ((struct state_block *)iface)->description = *in; }
static void STDMETHODCALLTYPE state_SetTextRenderingParams(void *iface, IDWriteRenderingParams *params)
{
	struct state_block *self = iface;
	if (params)
		IDWriteRenderingParams_AddRef(params);
	if (self->params)
		IDWriteRenderingParams_Release(self->params);
	self->params = params;
}
static void STDMETHODCALLTYPE state_GetTextRenderingParams(void *iface, IDWriteRenderingParams **params)
{
	struct state_block *self = iface;
	*params = self->params;
	if (self->params)
		IDWriteRenderingParams_AddRef(self->params);
}

static void **state_table;
static const GUID *const state_iids[] = {&IID_ID2D1Resource, &IID_ID2D1DrawingStateBlock, &IID_ID2D1DrawingStateBlock1};

static void build_resource_tables(void)
{
	stops_table = make_table(ID2D1GradientStopCollection1_table, ID2D1GradientStopCollection1_methods);
	set_resource(stops_table);
	stops_table[ID2D1GradientStopCollection1_GetGradientStopCount_slot] = (void *)stops_GetGradientStopCount;
	stops_table[ID2D1GradientStopCollection1_GetGradientStops_slot] = (void *)stops_GetGradientStops;
	stops_table[ID2D1GradientStopCollection1_GetColorInterpolationGamma_slot] = (void *)stops_GetColorInterpolationGamma;
	stops_table[ID2D1GradientStopCollection1_GetExtendMode_slot] = (void *)stops_GetExtendMode;
	stops_table[ID2D1GradientStopCollection1_GetGradientStops1_slot] = (void *)stops_GetGradientStops;
	stops_table[ID2D1GradientStopCollection1_GetPreInterpolationSpace_slot] = (void *)stops_GetSpace;
	stops_table[ID2D1GradientStopCollection1_GetPostInterpolationSpace_slot] = (void *)stops_GetSpace;
	stops_table[ID2D1GradientStopCollection1_GetBufferPrecision_slot] = (void *)stops_GetBufferPrecision;
	stops_table[ID2D1GradientStopCollection1_GetColorInterpolationMode_slot] = (void *)stops_GetColorInterpolationMode;

	bitmap_table = make_table(ID2D1Bitmap1_table, ID2D1Bitmap1_methods);
	set_resource(bitmap_table);
	bitmap_table[ID2D1Bitmap1_GetSize_slot] = (void *)bitmap_GetSize;
	bitmap_table[ID2D1Bitmap1_GetPixelSize_slot] = (void *)bitmap_GetPixelSize;
	bitmap_table[ID2D1Bitmap1_GetPixelFormat_slot] = (void *)bitmap_GetPixelFormat;
	bitmap_table[ID2D1Bitmap1_GetDpi_slot] = (void *)bitmap_GetDpi;
	bitmap_table[ID2D1Bitmap1_CopyFromBitmap_slot] = (void *)bitmap_CopyFromBitmap;
	bitmap_table[ID2D1Bitmap1_CopyFromMemory_slot] = (void *)bitmap_CopyFromMemory;
	bitmap_table[ID2D1Bitmap1_GetColorContext_slot] = (void *)bitmap_GetColorContext;
	bitmap_table[ID2D1Bitmap1_GetOptions_slot] = (void *)bitmap_GetOptions;
	bitmap_table[ID2D1Bitmap1_GetSurface_slot] = (void *)bitmap_GetSurface;
	bitmap_table[ID2D1Bitmap1_Map_slot] = (void *)bitmap_Map;
	bitmap_table[ID2D1Bitmap1_Unmap_slot] = (void *)bitmap_Unmap;

	solid_table = make_table(ID2D1SolidColorBrush_table, ID2D1SolidColorBrush_methods);
	linear_table = make_table(ID2D1LinearGradientBrush_table, ID2D1LinearGradientBrush_methods);
	radial_table = make_table(ID2D1RadialGradientBrush_table, ID2D1RadialGradientBrush_methods);
	bitmap_brush_table = make_table(ID2D1BitmapBrush1_table, ID2D1BitmapBrush1_methods);
	image_brush_table = make_table(ID2D1ImageBrush_table, ID2D1ImageBrush_methods);
	{
		void **tables[] = {solid_table, linear_table, radial_table, bitmap_brush_table, image_brush_table};
		int index;
		for (index = 0; index < 5; ++index) {
			set_resource(tables[index]);
			tables[index][ID2D1SolidColorBrush_SetOpacity_slot] = (void *)brush_SetOpacity;
			tables[index][ID2D1SolidColorBrush_SetTransform_slot] = (void *)brush_SetTransform;
			tables[index][ID2D1SolidColorBrush_GetOpacity_slot] = (void *)brush_GetOpacity;
			tables[index][ID2D1SolidColorBrush_GetTransform_slot] = (void *)brush_GetTransform;
		}
	}
	solid_table[ID2D1SolidColorBrush_SetColor_slot] = (void *)brush_SetColor;
	solid_table[ID2D1SolidColorBrush_GetColor_slot] = (void *)brush_GetColor;
	linear_table[ID2D1LinearGradientBrush_SetStartPoint_slot] = (void *)brush_SetStartPoint;
	linear_table[ID2D1LinearGradientBrush_SetEndPoint_slot] = (void *)brush_SetEndPoint;
	linear_table[ID2D1LinearGradientBrush_GetStartPoint_slot] = (void *)brush_GetStartPoint;
	linear_table[ID2D1LinearGradientBrush_GetEndPoint_slot] = (void *)brush_GetEndPoint;
	linear_table[ID2D1LinearGradientBrush_GetGradientStopCollection_slot] = (void *)brush_GetGradientStopCollection;
	radial_table[ID2D1RadialGradientBrush_SetCenter_slot] = (void *)brush_SetCenter;
	radial_table[ID2D1RadialGradientBrush_SetGradientOriginOffset_slot] = (void *)brush_SetGradientOriginOffset;
	radial_table[ID2D1RadialGradientBrush_SetRadiusX_slot] = (void *)brush_SetRadiusX;
	radial_table[ID2D1RadialGradientBrush_SetRadiusY_slot] = (void *)brush_SetRadiusY;
	radial_table[ID2D1RadialGradientBrush_GetCenter_slot] = (void *)brush_GetCenter;
	radial_table[ID2D1RadialGradientBrush_GetGradientOriginOffset_slot] = (void *)brush_GetGradientOriginOffset;
	radial_table[ID2D1RadialGradientBrush_GetRadiusX_slot] = (void *)brush_GetRadiusX;
	radial_table[ID2D1RadialGradientBrush_GetRadiusY_slot] = (void *)brush_GetRadiusY;
	radial_table[ID2D1RadialGradientBrush_GetGradientStopCollection_slot] = (void *)brush_GetGradientStopCollection;
	bitmap_brush_table[ID2D1BitmapBrush1_SetExtendModeX_slot] = (void *)brush_SetExtendModeX;
	bitmap_brush_table[ID2D1BitmapBrush1_SetExtendModeY_slot] = (void *)brush_SetExtendModeY;
	bitmap_brush_table[ID2D1BitmapBrush1_SetInterpolationMode_slot] = (void *)brush_SetInterpolationMode;
	bitmap_brush_table[ID2D1BitmapBrush1_SetBitmap_slot] = (void *)brush_SetBitmap;
	bitmap_brush_table[ID2D1BitmapBrush1_GetExtendModeX_slot] = (void *)brush_GetExtendModeX;
	bitmap_brush_table[ID2D1BitmapBrush1_GetExtendModeY_slot] = (void *)brush_GetExtendModeY;
	bitmap_brush_table[ID2D1BitmapBrush1_GetInterpolationMode_slot] = (void *)brush_GetInterpolationMode;
	bitmap_brush_table[ID2D1BitmapBrush1_GetBitmap_slot] = (void *)brush_GetBitmap;
	bitmap_brush_table[ID2D1BitmapBrush1_SetInterpolationMode1_slot] = (void *)brush_SetInterpolationMode;
	bitmap_brush_table[ID2D1BitmapBrush1_GetInterpolationMode1_slot] = (void *)brush_GetInterpolationMode;
	image_brush_table[ID2D1ImageBrush_SetImage_slot] = (void *)brush_SetImage;
	image_brush_table[ID2D1ImageBrush_SetExtendModeX_slot] = (void *)brush_SetExtendModeX;
	image_brush_table[ID2D1ImageBrush_SetExtendModeY_slot] = (void *)brush_SetExtendModeY;
	image_brush_table[ID2D1ImageBrush_SetInterpolationMode_slot] = (void *)brush_SetInterpolationMode;
	image_brush_table[ID2D1ImageBrush_SetSourceRectangle_slot] = (void *)brush_SetSourceRectangle;
	image_brush_table[ID2D1ImageBrush_GetImage_slot] = (void *)brush_GetImage;
	image_brush_table[ID2D1ImageBrush_GetExtendModeX_slot] = (void *)brush_GetExtendModeX;
	image_brush_table[ID2D1ImageBrush_GetExtendModeY_slot] = (void *)brush_GetExtendModeY;
	image_brush_table[ID2D1ImageBrush_GetInterpolationMode_slot] = (void *)brush_GetInterpolationMode;
	image_brush_table[ID2D1ImageBrush_GetSourceRectangle_slot] = (void *)brush_GetSourceRectangle;

	layer_table = make_table(ID2D1Layer_table, ID2D1Layer_methods);
	set_resource(layer_table);
	layer_table[ID2D1Layer_GetSize_slot] = (void *)layer_GetSize;

	state_table = make_table(ID2D1DrawingStateBlock1_table, ID2D1DrawingStateBlock1_methods);
	set_resource(state_table);
	state_table[ID2D1DrawingStateBlock1_GetDescription_slot] = (void *)state_GetDescription;
	state_table[ID2D1DrawingStateBlock1_SetDescription_slot] = (void *)state_SetDescription;
	state_table[ID2D1DrawingStateBlock1_SetTextRenderingParams_slot] = (void *)state_SetTextRenderingParams;
	state_table[ID2D1DrawingStateBlock1_GetTextRenderingParams_slot] = (void *)state_GetTextRenderingParams;
	state_table[ID2D1DrawingStateBlock1_GetDescription_8_slot] = (void *)state_GetDescription1;
	state_table[ID2D1DrawingStateBlock1_SetDescription_9_slot] = (void *)state_SetDescription1;
}

/* ---- render targets and device contexts ---- */
enum { TARGET_HWND, TARGET_DC, TARGET_BITMAP, TARGET_WIC, TARGET_DXGI, TARGET_CONTEXT };

struct device;

/* One object answers as its render target, as a device context and for GDI interop: three interface pointers in one block. */
struct target {
	void **primary;
	void **context;
	void **gdi;
	LONG refs;
	int kind;
	IUnknown *factory;
	struct device *device;
	struct bitmap *bitmap; /* what is drawn into */
	HWND hwnd;
	HDC dc;
	RECT dc_rect;
	IWICBitmap *wic;
	D2D1_MATRIX_3X2_F transform;
	float dpi_x, dpi_y;
	D2D1_ANTIALIAS_MODE antialias;
	D2D1_TEXT_ANTIALIAS_MODE text_antialias;
	D2D1_UNIT_MODE unit;
	D2D1_PRIMITIVE_BLEND blend;
	D2D1_RENDERING_CONTROLS controls;
	D2D1_TAG tag1, tag2;
	IDWriteRenderingParams *params;
	BOOL drawing;
	/* Whether Direct2D drew since the target last handed its pixels on: a WIC bitmap is written back only then. */
	BOOL drawn;
	HRESULT error;
	UINT32 pushed;
	D2D1_PIXEL_FORMAT format;
	/* GDI interop: a DIB section the program draws into between GetDC and ReleaseDC. */
	HDC gdi_dc;
	HBITMAP gdi_bitmap, gdi_previous;
	void *gdi_bits;
};

static void **hwnd_table, **dcrt_table, **bitmaprt_table, **rt_table, **context_table, **context_secondary_table, **gdi_table;

static struct target *target_of(void *iface)
{
	void **table = *(void ***)iface;
	if (table == context_secondary_table)
		return CONTAINER(iface, struct target, context);
	if (table == gdi_table)
		return CONTAINER(iface, struct target, gdi);
	return iface;
}

static const GUID *const context_iids[] = {&IID_ID2D1DeviceContext, &IID_ID2D1DeviceContext1, &IID_ID2D1DeviceContext2, &IID_ID2D1DeviceContext3,
	&IID_ID2D1DeviceContext4, &IID_ID2D1DeviceContext5};

static HRESULT STDMETHODCALLTYPE target_QueryInterface(void *iface, REFIID riid, void **out)
{
	struct target *self = target_of(iface);
	void *found = NULL;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_ID2D1Resource) || same_guid(riid, &IID_ID2D1RenderTarget))
		found = &self->primary;
	else if (any_guid(riid, context_iids, 6))
		found = self->kind == TARGET_CONTEXT ? (void *)&self->primary : (void *)&self->context;
	else if (same_guid(riid, &IID_ID2D1HwndRenderTarget) && self->kind == TARGET_HWND)
		found = &self->primary;
	else if (same_guid(riid, &IID_ID2D1DCRenderTarget) && self->kind == TARGET_DC)
		found = &self->primary;
	else if (same_guid(riid, &IID_ID2D1BitmapRenderTarget) && self->kind == TARGET_BITMAP)
		found = &self->primary;
	else if (same_guid(riid, &IID_ID2D1GdiInteropRenderTarget))
		found = &self->gdi;
	if (!found) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	*out = found;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE target_AddRef(void *iface)
{
	return (ULONG)InterlockedIncrement(&target_of(iface)->refs);
}

static ULONG STDMETHODCALLTYPE target_Release(void *iface)
{
	struct target *self = target_of(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		if (self->bitmap)
			object_Release(self->bitmap);
		if (self->params)
			IDWriteRenderingParams_Release(self->params);
		if (self->wic)
			IWICBitmap_Release(self->wic);
		if (self->device)
			object_Release(self->device);
		if (self->factory)
			IUnknown_Release(self->factory);
		FREE(self);
	}
	return (ULONG)refs;
}

static void STDMETHODCALLTYPE target_GetFactory(void *iface, ID2D1Factory **factory)
{
	struct target *self = target_of(iface);
	*factory = (ID2D1Factory *)self->factory;
	IUnknown_AddRef(self->factory);
}

/* The matrix from the program's coordinates to pixels: its transform, then the resolution (device-independent pixels are 1/96 inch). */
static D2D1_MATRIX_3X2_F world(const struct target *self)
{
	D2D1_MATRIX_3X2_F scale = identity;
	if (self->unit == D2D1_UNIT_MODE_DIPS) {
		scale._11 = self->dpi_x / 96;
		scale._22 = self->dpi_y / 96;
	}
	return multiply(&self->transform, &scale);
}

static void command_start(const struct target *self, struct command *command, UINT32 op)
{
	D2D1_MATRIX_3X2_F m = world(self);
	memset(command, 0, sizeof(*command));
	command->op = op;
	command->antialias = self->antialias == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
	put_matrix(command->transform, &m);
	command->opacity = 1;
}

static void send(struct target *self, const struct command *command, const struct bytes *data)
{
	if (!self->bitmap)
		return;
	self->drawn = TRUE;
	if (!RavenCanvasDraw(self->bitmap->surface, command, data ? data->data : NULL, data ? (DWORD)data->size : 0) && !self->error)
		self->error = D2DERR_INVALID_CALL;
}

/* A shape filled or stroked with a brush: the record, the brush's stops, the dash pattern and the path in that order. */
static void draw_path(struct target *self, const struct path *path, ID2D1Brush *brush, BOOL stroke, float width, ID2D1StrokeStyle *style)
{
	struct command command;
	struct bytes data = {0};
	struct stroke *pattern = (struct stroke *)style;
	command_start(self, &command, stroke ? OP_STROKE_PATH : OP_FILL_PATH);
	if (!path->count || !put_brush(&command, &data, (struct brush *)brush)) {
		FREE(data.data);
		return;
	}
	if (stroke) {
		float dashes[64];
		command.stroke_width = width;
		if (pattern) {
			command.start_cap = pattern->properties.startCap;
			command.end_cap = pattern->properties.endCap;
			command.dash_cap = pattern->properties.dashCap;
			command.line_join = pattern->properties.lineJoin;
			command.miter_limit = pattern->properties.miterLimit;
			command.dash_offset = pattern->properties.dashOffset * width;
			command.dashes = stroke_pattern(pattern, width, dashes);
			bytes_add(&data, dashes, command.dashes * sizeof(float));
		} else {
			command.miter_limit = 10;
		}
	}
	command.fill_mode = path->fill == D2D1_FILL_MODE_WINDING ? 1 : 0;
	command.path_floats = path->count;
	bytes_add(&data, path->data, path->count * sizeof(float));
	send(self, &command, &data);
	FREE(data.data);
}

static void STDMETHODCALLTYPE target_DrawLine(void *iface, D2D1_POINT_2F p0, D2D1_POINT_2F p1, ID2D1Brush *brush, FLOAT width, ID2D1StrokeStyle *style)
{
	struct path path = {0};
	path_move(&path, p0);
	path_line(&path, p1);
	draw_path(target_of(iface), &path, brush, TRUE, width, style);
	path_free(&path);
}

static void STDMETHODCALLTYPE target_DrawRectangle(void *iface, const D2D1_RECT_F *rect, ID2D1Brush *brush, FLOAT width, ID2D1StrokeStyle *style)
{
	struct path path = {0};
	path_rectangle(&path, rect);
	draw_path(target_of(iface), &path, brush, TRUE, width, style);
	path_free(&path);
}

static void STDMETHODCALLTYPE target_FillRectangle(void *iface, const D2D1_RECT_F *rect, ID2D1Brush *brush)
{
	struct path path = {0};
	path_rectangle(&path, rect);
	draw_path(target_of(iface), &path, brush, FALSE, 0, NULL);
	path_free(&path);
}

static void STDMETHODCALLTYPE target_DrawRoundedRectangle(void *iface, const D2D1_ROUNDED_RECT *rect, ID2D1Brush *brush, FLOAT width, ID2D1StrokeStyle *style)
{
	struct path path = {0};
	path_rounded(&path, rect);
	draw_path(target_of(iface), &path, brush, TRUE, width, style);
	path_free(&path);
}

static void STDMETHODCALLTYPE target_FillRoundedRectangle(void *iface, const D2D1_ROUNDED_RECT *rect, ID2D1Brush *brush)
{
	struct path path = {0};
	path_rounded(&path, rect);
	draw_path(target_of(iface), &path, brush, FALSE, 0, NULL);
	path_free(&path);
}

static void STDMETHODCALLTYPE target_DrawEllipse(void *iface, const D2D1_ELLIPSE *ellipse, ID2D1Brush *brush, FLOAT width, ID2D1StrokeStyle *style)
{
	struct path path = {0};
	path_ellipse(&path, ellipse->point.x, ellipse->point.y, ellipse->radiusX, ellipse->radiusY);
	draw_path(target_of(iface), &path, brush, TRUE, width, style);
	path_free(&path);
}

static void STDMETHODCALLTYPE target_FillEllipse(void *iface, const D2D1_ELLIPSE *ellipse, ID2D1Brush *brush)
{
	struct path path = {0};
	path_ellipse(&path, ellipse->point.x, ellipse->point.y, ellipse->radiusX, ellipse->radiusY);
	draw_path(target_of(iface), &path, brush, FALSE, 0, NULL);
	path_free(&path);
}

static void STDMETHODCALLTYPE target_DrawGeometry(void *iface, ID2D1Geometry *geometry, ID2D1Brush *brush, FLOAT width, ID2D1StrokeStyle *style)
{
	if (geometry)
		draw_path(target_of(iface), &((struct geometry *)geometry)->path, brush, TRUE, width, style);
}

static void STDMETHODCALLTYPE target_FillGeometry(void *iface, ID2D1Geometry *geometry, ID2D1Brush *brush, ID2D1Brush *opacity_brush)
{
	if (geometry)
		draw_path(target_of(iface), &((struct geometry *)geometry)->path, brush, FALSE, 0, NULL);
}

/* ---- geometry realizations: a geometry kept ready to fill or to stroke; it is drawn again from the geometry each time ---- */
struct realization {
	struct object base;
	struct geometry *geometry;
	BOOL stroked;
	float width;
	ID2D1StrokeStyle *style;
};

static void realization_destroy(struct object *base)
{
	struct realization *self = (struct realization *)base;
	IUnknown_Release((IUnknown *)self->geometry);
	if (self->style)
		IUnknown_Release((IUnknown *)self->style);
}

static void **realization_table;
static const GUID *const realization_iids[] = {&IID_ID2D1Resource, &IID_ID2D1GeometryRealization};

static HRESULT make_realization(void *iface, ID2D1Geometry *geometry, BOOL stroked, float width, ID2D1StrokeStyle *style,
	ID2D1GeometryRealization **out)
{
	struct realization *self;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!geometry)
		return E_INVALIDARG;
	if (!realization_table) {
		realization_table = make_table(ID2D1GeometryRealization_table, ID2D1GeometryRealization_methods);
		set_resource(realization_table);
	}
	if (!(self = ALLOC(sizeof(*self))))
		return E_OUTOFMEMORY;
	object_init(&self->base, realization_table, realization_destroy, realization_iids, 2, target_of(iface)->factory);
	self->geometry = (struct geometry *)geometry;
	IUnknown_AddRef((IUnknown *)geometry);
	self->stroked = stroked;
	self->width = width;
	self->style = style;
	if (style)
		IUnknown_AddRef((IUnknown *)style);
	*out = (ID2D1GeometryRealization *)self;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE context_CreateFilledGeometryRealization(void *iface, ID2D1Geometry *geometry, FLOAT tolerance,
	ID2D1GeometryRealization **out)
{
	return make_realization(iface, geometry, FALSE, 0, NULL, out);
}

static HRESULT STDMETHODCALLTYPE context_CreateStrokedGeometryRealization(void *iface, ID2D1Geometry *geometry, FLOAT tolerance,
	FLOAT width, ID2D1StrokeStyle *style, ID2D1GeometryRealization **out)
{
	return make_realization(iface, geometry, TRUE, width, style, out);
}

static void STDMETHODCALLTYPE context_DrawGeometryRealization(void *iface, ID2D1GeometryRealization *realization, ID2D1Brush *brush)
{
	const struct realization *self = (const struct realization *)realization;
	if (self)
		draw_path(target_of(iface), &self->geometry->path, brush, self->stroked, self->width, self->style);
}

/* A bitmap into a destination rectangle (by default its size in device-independent pixels) from a part of it (by default all). */
static void draw_bitmap(struct target *self, struct bitmap *bitmap, const D2D1_RECT_F *destination, float opacity, UINT32 interpolation,
	const D2D1_RECT_F *source)
{
	struct command command;
	if (!bitmap)
		return;
	command_start(self, &command, OP_DRAW_BITMAP);
	command.bitmap = bitmap->surface;
	command.opacity = opacity;
	command.interpolation = interpolation;
	if (source) {
		command.source[0] = source->left * bitmap->dpi_x / 96;
		command.source[1] = source->top * bitmap->dpi_y / 96;
		command.source[2] = (source->right - source->left) * bitmap->dpi_x / 96;
		command.source[3] = (source->bottom - source->top) * bitmap->dpi_y / 96;
	} else {
		command.source[2] = (float)bitmap->pixels.width;
		command.source[3] = (float)bitmap->pixels.height;
	}
	if (destination) {
		command.rect[0] = destination->left;
		command.rect[1] = destination->top;
		command.rect[2] = destination->right;
		command.rect[3] = destination->bottom;
	} else {
		command.rect[2] = source ? source->right - source->left : bitmap->pixels.width * 96.0f / bitmap->dpi_x;
		command.rect[3] = source ? source->bottom - source->top : bitmap->pixels.height * 96.0f / bitmap->dpi_y;
	}
	send(self, &command, NULL);
}

static void STDMETHODCALLTYPE target_DrawBitmap(void *iface, ID2D1Bitmap *bitmap, const D2D1_RECT_F *destination, FLOAT opacity,
	D2D1_BITMAP_INTERPOLATION_MODE interpolation, const D2D1_RECT_F *source)
{
	draw_bitmap(target_of(iface), (struct bitmap *)bitmap, destination, opacity, interpolation, source);
}

static void STDMETHODCALLTYPE target_DrawBitmap1(void *iface, ID2D1Bitmap *bitmap, const D2D1_RECT_F *destination, FLOAT opacity,
	D2D1_INTERPOLATION_MODE interpolation, const D2D1_RECT_F *source, const D2D1_MATRIX_4X4_F *perspective)
{
	draw_bitmap(target_of(iface), (struct bitmap *)bitmap, destination, opacity, interpolation, source);
}

struct effect;
static struct effect *effect_of_image(ID2D1Image *image);

/* DrawImage of a bitmap at an offset; images that are not bitmaps (effects, command lists) draw nothing here. */
static void STDMETHODCALLTYPE target_DrawImage(void *iface, ID2D1Image *image, const D2D1_POINT_2F *offset, const D2D1_RECT_F *rect,
	D2D1_INTERPOLATION_MODE interpolation, D2D1_COMPOSITE_MODE mode)
{
	struct target *self = target_of(iface);
	ID2D1Bitmap *bitmap = NULL;
	struct bitmap *b;
	D2D1_RECT_F destination, source;
	if (!image || FAILED(IUnknown_QueryInterface((IUnknown *)image, &IID_ID2D1Bitmap, (void **)&bitmap))) {
		if (!effect_of_image(image))
			unimplemented("ID2D1DeviceContext", "DrawImage of an image that is not a bitmap");
		return;
	}
	b = (struct bitmap *)bitmap;
	if (rect)
		source = *rect;
	else {
		source.left = source.top = 0;
		source.right = b->pixels.width * 96.0f / b->dpi_x;
		source.bottom = b->pixels.height * 96.0f / b->dpi_y;
	}
	destination.left = (offset ? offset->x : 0);
	destination.top = (offset ? offset->y : 0);
	destination.right = destination.left + source.right - source.left;
	destination.bottom = destination.top + source.bottom - source.top;
	draw_bitmap(self, b, &destination, 1, interpolation, &source);
	object_Release(bitmap);
}

/* The coverage of a mask bitmap (its alpha) painted with a brush into a destination rectangle. */
static void STDMETHODCALLTYPE target_FillOpacityMask(void *iface, ID2D1Bitmap *mask, ID2D1Brush *brush, D2D1_OPACITY_MASK_CONTENT content,
	const D2D1_RECT_F *destination, const D2D1_RECT_F *source)
{
	struct target *self = target_of(iface);
	struct bitmap *bitmap = (struct bitmap *)mask;
	struct command command;
	struct bytes data = {0};
	BYTE *pixels;
	UINT32 index, count;
	if (!bitmap)
		return;
	count = bitmap->pixels.width * bitmap->pixels.height;
	if (!(pixels = ALLOC((size_t)count * 4)))
		return;
	RavenCanvasPixels(bitmap->surface, 0, 0, bitmap->pixels.width, bitmap->pixels.height, pixels, bitmap->pixels.width * 4, 0);
	for (index = 0; index < count; ++index)
		pixels[index] = pixels[index * 4 + 3];
	command_start(self, &command, OP_FILL_MASK);
	if (!put_brush(&command, &data, (struct brush *)brush)) {
		FREE(pixels);
		FREE(data.data);
		return;
	}
	{
		/* The part of the mask the source names (by default all of it) is stretched to the destination (by default the mask's size at
		 * the origin) through the transform. */
		const UINT32 full_width = bitmap->pixels.width, full_height = bitmap->pixels.height;
		UINT32 left = 0, top = 0, width = full_width, height = full_height, row;
		D2D1_MATRIX_3X2_F stretch = identity, m = world(self);
		if (source) {
			const float x0 = source->left < 0 ? 0 : source->left, y0 = source->top < 0 ? 0 : source->top;
			const float x1 = source->right > full_width ? full_width : source->right, y1 = source->bottom > full_height ? full_height : source->bottom;
			if (x1 <= x0 || y1 <= y0) {
				FREE(pixels);
				FREE(data.data);
				return;
			}
			left = (UINT32)x0;
			top = (UINT32)y0;
			width = (UINT32)(x1 + 0.5f) - left;
			height = (UINT32)(y1 + 0.5f) - top;
			if (!width || !height || left + width > full_width || top + height > full_height) {
				FREE(pixels);
				FREE(data.data);
				return;
			}
			for (row = 0; row < height; ++row)
				memmove(pixels + (size_t)row * width, pixels + (size_t)(top + row) * full_width + left, width);
		}
		if (destination) {
			stretch._11 = (destination->right - destination->left) / width;
			stretch._22 = (destination->bottom - destination->top) / height;
			stretch._31 = destination->left;
			stretch._32 = destination->top;
		} else {
			stretch._11 = (float)full_width / width;
			stretch._22 = (float)full_height / height;
		}
		m = multiply(&stretch, &m);
		put_matrix(command.transform, &m);
		command.mask_width = width;
		command.mask_height = height;
		count = width * height;
	}
	bytes_add(&data, pixels, count);
	send(self, &command, &data);
	FREE(pixels);
	FREE(data.data);
}

/* ID2D1DeviceContext::FillOpacityMask: the same, without the content kind the render target form takes. */
static void STDMETHODCALLTYPE context_FillOpacityMask(void *iface, ID2D1Bitmap *mask, ID2D1Brush *brush, const D2D1_RECT_F *destination,
	const D2D1_RECT_F *source)
{
	target_FillOpacityMask(iface, mask, brush, D2D1_OPACITY_MASK_CONTENT_GRAPHICS, destination, source);
}

static void STDMETHODCALLTYPE target_SetTransform(void *iface, const D2D1_MATRIX_3X2_F *m) { target_of(iface)->transform = *m; }
static void STDMETHODCALLTYPE target_GetTransform(void *iface, D2D1_MATRIX_3X2_F *m) { *m = target_of(iface)->transform; }
static void STDMETHODCALLTYPE target_SetAntialiasMode(void *iface, D2D1_ANTIALIAS_MODE mode) { target_of(iface)->antialias = mode; }
static D2D1_ANTIALIAS_MODE STDMETHODCALLTYPE target_GetAntialiasMode(void *iface) { return target_of(iface)->antialias; }
static void STDMETHODCALLTYPE target_SetTextAntialiasMode(void *iface, D2D1_TEXT_ANTIALIAS_MODE mode) { target_of(iface)->text_antialias = mode; }
static D2D1_TEXT_ANTIALIAS_MODE STDMETHODCALLTYPE target_GetTextAntialiasMode(void *iface) { return target_of(iface)->text_antialias; }
static void STDMETHODCALLTYPE target_SetTextRenderingParams(void *iface, IDWriteRenderingParams *params)
{
	struct target *self = target_of(iface);
	if (params)
		IDWriteRenderingParams_AddRef(params);
	if (self->params)
		IDWriteRenderingParams_Release(self->params);
	self->params = params;
}
static void STDMETHODCALLTYPE target_GetTextRenderingParams(void *iface, IDWriteRenderingParams **params)
{
	struct target *self = target_of(iface);
	*params = self->params;
	if (self->params)
		IDWriteRenderingParams_AddRef(self->params);
}
static void STDMETHODCALLTYPE target_SetTags(void *iface, D2D1_TAG tag1, D2D1_TAG tag2)
{
	target_of(iface)->tag1 = tag1;
	target_of(iface)->tag2 = tag2;
}
static void STDMETHODCALLTYPE target_GetTags(void *iface, D2D1_TAG *tag1, D2D1_TAG *tag2)
{
	if (tag1) *tag1 = target_of(iface)->tag1;
	if (tag2) *tag2 = target_of(iface)->tag2;
}

static void push_layer(struct target *self, const D2D1_LAYER_PARAMETERS *parameters)
{
	struct command command;
	struct bytes data = {0};
	command_start(self, &command, OP_PUSH_LAYER);
	command.opacity = parameters->opacity;
	command.rect[0] = parameters->contentBounds.left;
	command.rect[1] = parameters->contentBounds.top;
	command.rect[2] = parameters->contentBounds.right;
	command.rect[3] = parameters->contentBounds.bottom;
	command.antialias = parameters->maskAntialiasMode == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
	if (parameters->geometricMask) {
		/* The mask is in its own transform, then the target's: its points are carried to the target's space here. */
		struct path mask;
		path_copy(&mask, &((struct geometry *)parameters->geometricMask)->path);
		path_transform(&mask, &parameters->maskTransform);
		command.fill_mode = mask.fill == D2D1_FILL_MODE_WINDING ? 1 : 0;
		command.path_floats = mask.count;
		bytes_add(&data, mask.data, mask.count * sizeof(float));
		path_free(&mask);
	}
	if (parameters->opacityBrush)
		unimplemented("ID2D1RenderTarget", "PushLayer with an opacity brush");
	send(self, &command, &data);
	FREE(data.data);
	++self->pushed;
}

static void STDMETHODCALLTYPE target_PushLayer(void *iface, const D2D1_LAYER_PARAMETERS *parameters, ID2D1Layer *layer)
{
	push_layer(target_of(iface), parameters);
}

static void STDMETHODCALLTYPE target_PushLayer1(void *iface, const D2D1_LAYER_PARAMETERS1 *parameters, ID2D1Layer *layer)
{
	push_layer(target_of(iface), (const D2D1_LAYER_PARAMETERS *)parameters);
}

static void pop(struct target *self)
{
	struct command command;
	if (!self->pushed)
		return;
	command_start(self, &command, OP_POP);
	send(self, &command, NULL);
	--self->pushed;
}

static void STDMETHODCALLTYPE target_PopLayer(void *iface) { pop(target_of(iface)); }
static void STDMETHODCALLTYPE target_PopAxisAlignedClip(void *iface) { pop(target_of(iface)); }

static void STDMETHODCALLTYPE target_PushAxisAlignedClip(void *iface, const D2D1_RECT_F *rect, D2D1_ANTIALIAS_MODE mode)
{
	struct target *self = target_of(iface);
	struct command command;
	command_start(self, &command, OP_PUSH_CLIP);
	command.antialias = mode == D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
	command.rect[0] = rect->left;
	command.rect[1] = rect->top;
	command.rect[2] = rect->right;
	command.rect[3] = rect->bottom;
	send(self, &command, NULL);
	++self->pushed;
}

static void STDMETHODCALLTYPE target_Clear(void *iface, const D2D1_COLOR_F *color)
{
	struct target *self = target_of(iface);
	struct command command;
	command_start(self, &command, OP_CLEAR);
	if (color) {
		command.color[0] = color->r;
		command.color[1] = color->g;
		command.color[2] = color->b;
		command.color[3] = self->format.alphaMode == D2D1_ALPHA_MODE_IGNORE ? 1 : color->a;
	}
	send(self, &command, NULL);
}

static void STDMETHODCALLTYPE target_SaveDrawingState(void *iface, ID2D1DrawingStateBlock *block)
{
	struct target *self = target_of(iface);
	struct state_block *state = (struct state_block *)block;
	state->description.antialiasMode = self->antialias;
	state->description.textAntialiasMode = self->text_antialias;
	state->description.tag1 = self->tag1;
	state->description.tag2 = self->tag2;
	state->description.transform = self->transform;
	state->description.primitiveBlend = self->blend;
	state->description.unitMode = self->unit;
	state_SetTextRenderingParams(state, self->params);
}

static void STDMETHODCALLTYPE target_RestoreDrawingState(void *iface, ID2D1DrawingStateBlock *block)
{
	struct target *self = target_of(iface);
	struct state_block *state = (struct state_block *)block;
	self->antialias = state->description.antialiasMode;
	self->text_antialias = state->description.textAntialiasMode;
	self->tag1 = state->description.tag1;
	self->tag2 = state->description.tag2;
	self->transform = state->description.transform;
	self->blend = state->description.primitiveBlend;
	self->unit = state->description.unitMode;
	target_SetTextRenderingParams(iface, state->params);
}

/* A WIC target starts from the bitmap's pixels, and takes them again at each BeginDraw, as the program may have written the bitmap itself
 * in between (Paint.NET renders brush masks on the CPU into the bitmap of a target it then draws on); an 8-bit bitmap gives the alpha or
 * the grey of each pixel. */
static void read_wic(struct target *self)
{
	UINT width = 0, height = 0, depth, bytes;
	BYTE *pixels;
	IWICBitmap_GetSize(self->wic, &width, &height);
	if (!self->bitmap || width != self->bitmap->pixels.width || height != self->bitmap->pixels.height)
		return;
	depth = wic_depth(self->wic);
	bytes = depth == 4 ? 4 : 1;
	if (!(pixels = ALLOC((size_t)width * height * 4)))
		return;
	if (SUCCEEDED(IWICBitmap_CopyPixels(self->wic, NULL, width * bytes, width * height * bytes, pixels))) {
		if (depth != 4) {
			UINT index = width * height;
			const BOOL alpha = depth == 1;
			while (index--) {
				const BYTE value = pixels[index];
				pixels[index * 4] = pixels[index * 4 + 1] = pixels[index * 4 + 2] = alpha ? 0 : value;
				pixels[index * 4 + 3] = alpha ? value : 255;
			}
		}
		RavenCanvasPixels(self->bitmap->surface, 0, 0, width, height, pixels, width * 4, 1);
	}
	FREE(pixels);
}

/* What a finished frame turns into: the window's pixels, the bound device context's, the Direct3D texture's or the WIC bitmap's. */
static void present(struct target *self)
{
	struct bitmap *bitmap = self->bitmap;
	BYTE *pixels;
	const UINT32 width = bitmap ? bitmap->pixels.width : 0, height = bitmap ? bitmap->pixels.height : 0;
	if (!bitmap || (self->kind != TARGET_HWND && self->kind != TARGET_DC && self->kind != TARGET_WIC && !bitmap->dxgi))
		return;
	/* A WIC bitmap nothing was drawn on keeps what the program wrote into it. */
	if (self->kind == TARGET_WIC && !self->drawn)
		return;
	self->drawn = FALSE;
	if (!(pixels = ALLOC((size_t)width * height * 4)))
		return;
	RavenCanvasPixels(bitmap->surface, 0, 0, width, height, pixels, width * 4, 0);
	if (self->kind == TARGET_HWND || self->kind == TARGET_DC) {
		BITMAPINFO info;
		HDC dc = self->kind == TARGET_HWND ? GetDC(self->hwnd) : self->dc;
		memset(&info, 0, sizeof(info));
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = (LONG)width;
		info.bmiHeader.biHeight = -(LONG)height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		if (dc) {
			const int x = self->kind == TARGET_DC ? self->dc_rect.left : 0, y = self->kind == TARGET_DC ? self->dc_rect.top : 0;
			SetDIBitsToDevice(dc, x, y, width, height, 0, 0, 0, height, pixels, &info, DIB_RGB_COLORS);
			if (self->kind == TARGET_HWND)
				ReleaseDC(self->hwnd, dc);
		}
	} else if (self->kind == TARGET_WIC && self->wic) {
		WICRect rect = {0, 0, (INT)width, (INT)height};
		IWICBitmapLock *lock = NULL;
		const UINT depth = wic_depth(self->wic);
		if (SUCCEEDED(IWICBitmap_Lock(self->wic, &rect, WICBitmapLockWrite, &lock))) {
			UINT stride = 0, size = 0, row, column;
			BYTE *data = NULL;
			IWICBitmapLock_GetStride(lock, &stride);
			IWICBitmapLock_GetDataPointer(lock, &size, &data);
			for (row = 0; data && row < height && (size_t)row * stride + width * (depth == 4 ? 4 : 1) <= size; ++row) {
				const BYTE *from = pixels + (size_t)row * width * 4;
				BYTE *to = data + (size_t)row * stride;
				if (depth == 4)
					memcpy(to, from, width * 4);
				else
					/* An 8-bit target keeps the alpha (A8, the format of DXGI_FORMAT_A8_UNORM targets) or the grey level. */
					for (column = 0; column < width; ++column)
						to[column] = depth == 1 ? from[column * 4 + 3] : from[column * 4];
			}
			IWICBitmapLock_Release(lock);
		} else
			unimplemented("IWICBitmap", "Lock for writing failed, so the frame");
	} else if (bitmap->dxgi) {
		/* The texture behind the surface takes the pixels through its device's immediate context; RGBA textures get their red and blue
		 * swapped on the way. */
		ID3D11Resource *resource = NULL;
		if (SUCCEEDED(IDXGISurface_QueryInterface(bitmap->dxgi, &IID_ID3D11Resource, (void **)&resource))) {
			ID3D11Device *device = NULL;
			ID3D11DeviceContext *context = NULL;
			DXGI_SURFACE_DESC desc;
			IDXGISurface_GetDesc(bitmap->dxgi, &desc);
			if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
				UINT32 index;
				for (index = 0; index < width * height; ++index) {
					const BYTE blue = pixels[index * 4];
					pixels[index * 4] = pixels[index * 4 + 2];
					pixels[index * 4 + 2] = blue;
				}
			}
			ID3D11Resource_GetDevice(resource, &device);
			if (device) {
				ID3D11Device_GetImmediateContext(device, &context);
				if (context) {
					ID3D11DeviceContext_UpdateSubresource(context, resource, 0, NULL, pixels, width * 4, 0);
					ID3D11DeviceContext_Release(context);
				}
				ID3D11Device_Release(device);
			}
			ID3D11Resource_Release(resource);
		}
	}
	FREE(pixels);
}

/* A render target bound to a device context draws over what the context already shows: its pixels are read in when drawing begins. */
static void read_bound_dc(struct target *self)
{
	const UINT32 width = self->bitmap->pixels.width, height = self->bitmap->pixels.height;
	BITMAPINFO info;
	void *bits = NULL;
	HDC memory;
	HBITMAP section, previous;
	UINT32 index;
	memset(&info, 0, sizeof(info));
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = (LONG)width;
	info.bmiHeader.biHeight = -(LONG)height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	if (!(memory = CreateCompatibleDC(self->dc)))
		return;
	if ((section = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, NULL, 0)) && bits) {
		previous = SelectObject(memory, section);
		BitBlt(memory, 0, 0, (int)width, (int)height, self->dc, self->dc_rect.left, self->dc_rect.top, SRCCOPY);
		/* GDI leaves the fourth byte undefined: the pixels are opaque. */
		for (index = 0; index < width * height; ++index)
			((BYTE *)bits)[index * 4 + 3] = 255;
		RavenCanvasPixels(self->bitmap->surface, 0, 0, width, height, bits, width * 4, 1);
		SelectObject(memory, previous);
	}
	if (section)
		DeleteObject(section);
	DeleteDC(memory);
}

static void STDMETHODCALLTYPE target_BeginDraw(void *iface)
{
	struct target *self = target_of(iface);
	self->drawing = TRUE;
	self->error = S_OK;
	if (self->kind == TARGET_DC && self->dc && self->bitmap)
		read_bound_dc(self);
	if (self->kind == TARGET_WIC && self->wic)
		read_wic(self);
	self->drawn = FALSE;
}

static HRESULT STDMETHODCALLTYPE target_EndDraw(void *iface, D2D1_TAG *tag1, D2D1_TAG *tag2)
{
	struct target *self = target_of(iface);
	HRESULT result = self->drawing ? self->error : D2DERR_WRONG_STATE;
	if (self->pushed && SUCCEEDED(result))
		result = D2DERR_POP_CALL_DID_NOT_MATCH_PUSH;
	while (self->pushed)
		pop(self);
	present(self);
	self->drawing = FALSE;
	if (tag1) *tag1 = self->tag1;
	if (tag2) *tag2 = self->tag2;
	return result;
}

static HRESULT STDMETHODCALLTYPE target_Flush(void *iface, D2D1_TAG *tag1, D2D1_TAG *tag2)
{
	struct target *self = target_of(iface);
	if (self->bitmap && self->bitmap->dxgi)
		present(self);
	if (tag1) *tag1 = self->tag1;
	if (tag2) *tag2 = self->tag2;
	return self->error;
}

static D2D1_PIXEL_FORMAT *STDMETHODCALLTYPE target_GetPixelFormat(void *iface, D2D1_PIXEL_FORMAT *format) { *format = target_of(iface)->format; return format; }
static void STDMETHODCALLTYPE target_SetDpi(void *iface, FLOAT x, FLOAT y)
{
	struct target *self = target_of(iface);
	if (x <= 0 || y <= 0)
		x = y = 96;
	self->dpi_x = x;
	self->dpi_y = y;
}
static void STDMETHODCALLTYPE target_GetDpi(void *iface, FLOAT *x, FLOAT *y)
{
	*x = target_of(iface)->dpi_x;
	*y = target_of(iface)->dpi_y;
}
static D2D1_SIZE_F *STDMETHODCALLTYPE target_GetSize(void *iface, D2D1_SIZE_F *size)
{
	struct target *self = target_of(iface);
	size->width = self->bitmap ? self->bitmap->pixels.width * 96.0f / self->dpi_x : 0;
	size->height = self->bitmap ? self->bitmap->pixels.height * 96.0f / self->dpi_y : 0;
	return size;
}
static D2D1_SIZE_U *STDMETHODCALLTYPE target_GetPixelSize(void *iface, D2D1_SIZE_U *size)
{
	struct target *self = target_of(iface);
	size->width = self->bitmap ? self->bitmap->pixels.width : 0;
	size->height = self->bitmap ? self->bitmap->pixels.height : 0;
	return size;
}
static UINT32 STDMETHODCALLTYPE target_GetMaximumBitmapSize(void *iface) { return 16384; }
static BOOL STDMETHODCALLTYPE target_IsSupported(void *iface, const D2D1_RENDER_TARGET_PROPERTIES *properties) { return TRUE; }

/* ---- resources a target makes ---- */
static HRESULT STDMETHODCALLTYPE target_CreateBitmap(void *iface, D2D1_SIZE_U size, const void *data, UINT32 pitch,
	const D2D1_BITMAP_PROPERTIES *properties, ID2D1Bitmap **out)
{
	struct target *self = target_of(iface);
	D2D1_PIXEL_FORMAT format = properties ? properties->pixelFormat : self->format;
	struct bitmap *bitmap = make_bitmap(self->factory, size, format, properties ? properties->dpiX : 96, properties ? properties->dpiY : 96,
		D2D1_BITMAP_OPTIONS_NONE);
	if (!bitmap)
		return E_OUTOFMEMORY;
	if (data)
		bitmap_CopyFromMemory(bitmap, NULL, data, pitch);
	*out = (ID2D1Bitmap *)bitmap;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateBitmap1(void *iface, D2D1_SIZE_U size, const void *data, UINT32 pitch,
	const D2D1_BITMAP_PROPERTIES1 *properties, ID2D1Bitmap1 **out)
{
	struct target *self = target_of(iface);
	D2D1_PIXEL_FORMAT format = properties ? properties->pixelFormat : self->format;
	struct bitmap *bitmap = make_bitmap(self->factory, size, format, properties ? properties->dpiX : 96, properties ? properties->dpiY : 96,
		properties ? properties->bitmapOptions : D2D1_BITMAP_OPTIONS_NONE);
	if (!bitmap)
		return E_OUTOFMEMORY;
	if (data)
		bitmap_CopyFromMemory(bitmap, NULL, data, pitch);
	*out = (ID2D1Bitmap1 *)bitmap;
	return S_OK;
}

/* A WIC source read as premultiplied BGRA, converted first when it holds another format. */
static HRESULT bitmap_from_wic(struct target *self, IWICBitmapSource *source, const D2D1_PIXEL_FORMAT *wanted, float dpi_x, float dpi_y,
	D2D1_BITMAP_OPTIONS options, struct bitmap **out)
{
	UINT width = 0, height = 0;
	WICPixelFormatGUID format;
	IWICBitmapSource *converted = NULL;
	struct bitmap *bitmap;
	BYTE *pixels;
	HRESULT hr;
	D2D1_SIZE_U size;
	D2D1_PIXEL_FORMAT pixel_format = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
	if (!source)
		return E_INVALIDARG;
	if (FAILED(hr = IWICBitmapSource_GetSize(source, &width, &height)) || FAILED(hr = IWICBitmapSource_GetPixelFormat(source, &format)))
		return hr;
	if (!same_guid(&format, &GUID_WICPixelFormat32bppPBGRA)) {
		if (FAILED(hr = WICConvertBitmapSource(&GUID_WICPixelFormat32bppPBGRA, source, &converted)))
			return hr;
		source = converted;
	}
	if (wanted && wanted->alphaMode == D2D1_ALPHA_MODE_IGNORE)
		pixel_format.alphaMode = D2D1_ALPHA_MODE_IGNORE;
	size.width = width;
	size.height = height;
	if (!(bitmap = make_bitmap(self->factory, size, pixel_format, dpi_x, dpi_y, options))) {
		if (converted)
			IWICBitmapSource_Release(converted);
		return E_OUTOFMEMORY;
	}
	if ((pixels = ALLOC((size_t)width * height * 4))) {
		if (SUCCEEDED(hr = IWICBitmapSource_CopyPixels(source, NULL, width * 4, width * height * 4, pixels)))
			bitmap_CopyFromMemory(bitmap, NULL, pixels, width * 4);
		FREE(pixels);
	} else {
		hr = E_OUTOFMEMORY;
	}
	if (converted)
		IWICBitmapSource_Release(converted);
	if (FAILED(hr)) {
		object_Release(bitmap);
		return hr;
	}
	*out = bitmap;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateBitmapFromWicBitmap(void *iface, IWICBitmapSource *source, const D2D1_BITMAP_PROPERTIES *properties,
	ID2D1Bitmap **out)
{
	return bitmap_from_wic(target_of(iface), source, properties ? &properties->pixelFormat : NULL, properties ? properties->dpiX : 96,
		properties ? properties->dpiY : 96, D2D1_BITMAP_OPTIONS_NONE, (struct bitmap **)out);
}

static HRESULT STDMETHODCALLTYPE target_CreateBitmapFromWicBitmap1(void *iface, IWICBitmapSource *source, const D2D1_BITMAP_PROPERTIES1 *properties,
	ID2D1Bitmap1 **out)
{
	return bitmap_from_wic(target_of(iface), source, properties ? &properties->pixelFormat : NULL, properties ? properties->dpiX : 96,
		properties ? properties->dpiY : 96, properties ? properties->bitmapOptions : D2D1_BITMAP_OPTIONS_NONE, (struct bitmap **)out);
}

/* A bitmap over a DXGI surface: the size and format of the surface, and the surface kept to hand the pixels back at the end of a frame. */
static HRESULT bitmap_from_dxgi(struct target *self, IDXGISurface *surface, const D2D1_BITMAP_PROPERTIES1 *properties, struct bitmap **out)
{
	DXGI_SURFACE_DESC desc;
	D2D1_SIZE_U size;
	D2D1_PIXEL_FORMAT format;
	struct bitmap *bitmap;
	HRESULT hr;
	if (!surface)
		return E_INVALIDARG;
	if (FAILED(hr = IDXGISurface_GetDesc(surface, &desc)))
		return hr;
	size.width = desc.Width;
	size.height = desc.Height;
	format.format = desc.Format;
	format.alphaMode = properties && properties->pixelFormat.alphaMode ? properties->pixelFormat.alphaMode : D2D1_ALPHA_MODE_PREMULTIPLIED;
	if (!(bitmap = make_bitmap(self->factory, size, format, properties ? properties->dpiX : 96, properties ? properties->dpiY : 96,
			properties ? properties->bitmapOptions : D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW)))
		return E_OUTOFMEMORY;
	bitmap->dxgi = surface;
	IDXGISurface_AddRef(surface);
	*out = bitmap;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateBitmapFromDxgiSurface(void *iface, IDXGISurface *surface, const D2D1_BITMAP_PROPERTIES1 *properties,
	ID2D1Bitmap1 **out)
{
	return bitmap_from_dxgi(target_of(iface), surface, properties, (struct bitmap **)out);
}

static HRESULT STDMETHODCALLTYPE target_CreateSharedBitmap(void *iface, REFIID riid, void *data, const D2D1_BITMAP_PROPERTIES *properties, ID2D1Bitmap **out)
{
	struct target *self = target_of(iface);
	if (same_guid(riid, &IID_ID2D1Bitmap) || same_guid(riid, &IID_ID2D1Bitmap1)) {
		object_AddRef(data);
		*out = data;
		return S_OK;
	}
	if (same_guid(riid, &IID_IDXGISurface) || same_guid(riid, &IID_IDXGISurface1))
		return bitmap_from_dxgi(self, data, NULL, (struct bitmap **)out);
	if (same_guid(riid, &IID_IWICBitmapLock)) {
		/* The locked pixels are copied in: the lock's rectangle, stride and format. */
		UINT width = 0, height = 0, stride = 0, size = 0;
		BYTE *pixels = NULL;
		D2D1_SIZE_U bitmap_size;
		D2D1_PIXEL_FORMAT format = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
		struct bitmap *bitmap;
		IWICBitmapLock_GetSize((IWICBitmapLock *)data, &width, &height);
		IWICBitmapLock_GetStride((IWICBitmapLock *)data, &stride);
		IWICBitmapLock_GetDataPointer((IWICBitmapLock *)data, &size, &pixels);
		bitmap_size.width = width;
		bitmap_size.height = height;
		if (properties)
			format = properties->pixelFormat;
		if (!(bitmap = make_bitmap(self->factory, bitmap_size, format, properties ? properties->dpiX : 96, properties ? properties->dpiY : 96, 0)))
			return E_OUTOFMEMORY;
		if (pixels)
			bitmap_CopyFromMemory(bitmap, NULL, pixels, stride);
		*out = (ID2D1Bitmap *)bitmap;
		return S_OK;
	}
	return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE target_CreateBitmapBrush(void *iface, ID2D1Bitmap *bitmap, const D2D1_BITMAP_BRUSH_PROPERTIES *bitmap_properties,
	const D2D1_BRUSH_PROPERTIES *properties, ID2D1BitmapBrush **out)
{
	struct brush *brush = make_brush(target_of(iface)->factory, KIND_BITMAP, properties);
	if (!brush)
		return E_OUTOFMEMORY;
	brush_SetBitmap(brush, bitmap);
	if (bitmap_properties) {
		brush->extend_x = bitmap_properties->extendModeX;
		brush->extend_y = bitmap_properties->extendModeY;
		brush->interpolation = bitmap_properties->interpolationMode;
	}
	*out = (ID2D1BitmapBrush *)brush;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateBitmapBrush1(void *iface, ID2D1Bitmap *bitmap, const D2D1_BITMAP_BRUSH_PROPERTIES1 *bitmap_properties,
	const D2D1_BRUSH_PROPERTIES *properties, ID2D1BitmapBrush1 **out)
{
	struct brush *brush = make_brush(target_of(iface)->factory, KIND_BITMAP, properties);
	if (!brush)
		return E_OUTOFMEMORY;
	brush_SetBitmap(brush, bitmap);
	if (bitmap_properties) {
		brush->extend_x = bitmap_properties->extendModeX;
		brush->extend_y = bitmap_properties->extendModeY;
		brush->interpolation = bitmap_properties->interpolationMode;
	}
	*out = (ID2D1BitmapBrush1 *)brush;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateImageBrush(void *iface, ID2D1Image *image, const D2D1_IMAGE_BRUSH_PROPERTIES *image_properties,
	const D2D1_BRUSH_PROPERTIES *properties, ID2D1ImageBrush **out)
{
	struct brush *brush = make_brush(target_of(iface)->factory, KIND_IMAGE, properties);
	if (!brush)
		return E_OUTOFMEMORY;
	brush_SetImage(brush, image);
	if (image_properties) {
		brush->extend_x = image_properties->extendModeX;
		brush->extend_y = image_properties->extendModeY;
		brush->interpolation = image_properties->interpolationMode;
		brush_SetSourceRectangle(brush, &image_properties->sourceRectangle);
	}
	*out = (ID2D1ImageBrush *)brush;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateSolidColorBrush(void *iface, const D2D1_COLOR_F *color, const D2D1_BRUSH_PROPERTIES *properties,
	ID2D1SolidColorBrush **out)
{
	struct brush *brush = make_brush(target_of(iface)->factory, KIND_SOLID, properties);
	if (!brush)
		return E_OUTOFMEMORY;
	brush->color = *color;
	*out = (ID2D1SolidColorBrush *)brush;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateGradientStopCollection(void *iface, const D2D1_GRADIENT_STOP *stops, UINT32 count, D2D1_GAMMA gamma,
	D2D1_EXTEND_MODE extend, ID2D1GradientStopCollection **out)
{
	return make_stops(target_of(iface)->factory, stops, count, gamma, extend, (void **)out);
}

static HRESULT STDMETHODCALLTYPE target_CreateGradientStopCollection1(void *iface, const D2D1_GRADIENT_STOP *stops, UINT32 count,
	D2D1_COLOR_SPACE pre, D2D1_COLOR_SPACE post, D2D1_BUFFER_PRECISION precision, D2D1_EXTEND_MODE extend, D2D1_COLOR_INTERPOLATION_MODE mode,
	ID2D1GradientStopCollection1 **out)
{
	return make_stops(target_of(iface)->factory, stops, count, D2D1_GAMMA_2_2, extend, (void **)out);
}

static HRESULT STDMETHODCALLTYPE target_CreateLinearGradientBrush(void *iface, const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES *gradient,
	const D2D1_BRUSH_PROPERTIES *properties, ID2D1GradientStopCollection *stops, ID2D1LinearGradientBrush **out)
{
	struct brush *brush = make_brush(target_of(iface)->factory, KIND_LINEAR, properties);
	if (!brush)
		return E_OUTOFMEMORY;
	brush->start = gradient->startPoint;
	brush->end = gradient->endPoint;
	brush->stops = (struct stops *)stops;
	if (stops)
		object_AddRef(stops);
	*out = (ID2D1LinearGradientBrush *)brush;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateRadialGradientBrush(void *iface, const D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES *gradient,
	const D2D1_BRUSH_PROPERTIES *properties, ID2D1GradientStopCollection *stops, ID2D1RadialGradientBrush **out)
{
	struct brush *brush = make_brush(target_of(iface)->factory, KIND_RADIAL, properties);
	if (!brush)
		return E_OUTOFMEMORY;
	brush->center = gradient->center;
	brush->offset = gradient->gradientOriginOffset;
	brush->radius_x = gradient->radiusX;
	brush->radius_y = gradient->radiusY;
	brush->stops = (struct stops *)stops;
	if (stops)
		object_AddRef(stops);
	*out = (ID2D1RadialGradientBrush *)brush;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE target_CreateLayer(void *iface, const D2D1_SIZE_F *size, ID2D1Layer **out)
{
	struct layer *layer = ALLOC(sizeof(*layer));
	if (!layer)
		return E_OUTOFMEMORY;
	object_init(&layer->base, layer_table, NULL, layer_iids, 2, target_of(iface)->factory);
	if (size)
		layer->size = *size;
	*out = (ID2D1Layer *)layer;
	return S_OK;
}

static struct target *make_target(IUnknown *factory, int kind, const D2D1_RENDER_TARGET_PROPERTIES *properties);

static HRESULT STDMETHODCALLTYPE target_CreateCompatibleRenderTarget(void *iface, const D2D1_SIZE_F *size, const D2D1_SIZE_U *pixel_size,
	const D2D1_PIXEL_FORMAT *format, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS options, ID2D1BitmapRenderTarget **out)
{
	struct target *self = target_of(iface), *compatible;
	D2D1_RENDER_TARGET_PROPERTIES properties;
	D2D1_SIZE_U pixels;
	memset(&properties, 0, sizeof(properties));
	properties.pixelFormat = format ? *format : self->format;
	if (properties.pixelFormat.format == DXGI_FORMAT_UNKNOWN)
		properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
	if (properties.pixelFormat.alphaMode == D2D1_ALPHA_MODE_UNKNOWN)
		properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
	properties.dpiX = self->dpi_x;
	properties.dpiY = self->dpi_y;
	if (pixel_size)
		pixels = *pixel_size;
	else if (size) {
		pixels.width = (UINT32)(size->width * self->dpi_x / 96 + 0.5f);
		pixels.height = (UINT32)(size->height * self->dpi_y / 96 + 0.5f);
		if (size->width > 0 && pixel_size == NULL) {
			properties.dpiX = self->dpi_x;
			properties.dpiY = self->dpi_y;
		}
	} else {
		pixels = self->bitmap ? self->bitmap->pixels : (D2D1_SIZE_U){1, 1};
	}
	if (size && pixel_size && size->width > 0 && size->height > 0) {
		properties.dpiX = pixel_size->width * 96.0f / size->width;
		properties.dpiY = pixel_size->height * 96.0f / size->height;
	}
	if (!(compatible = make_target(self->factory, TARGET_BITMAP, &properties)))
		return E_OUTOFMEMORY;
	if (!(compatible->bitmap = make_bitmap(self->factory, pixels, properties.pixelFormat, properties.dpiX, properties.dpiY, 0))) {
		target_Release(compatible);
		return E_OUTOFMEMORY;
	}
	*out = (ID2D1BitmapRenderTarget *)compatible;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE bitmaprt_GetBitmap(void *iface, ID2D1Bitmap **bitmap)
{
	struct target *self = target_of(iface);
	*bitmap = (ID2D1Bitmap *)self->bitmap;
	if (self->bitmap)
		object_AddRef(self->bitmap);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE bitmap_CopyFromRenderTarget(void *iface, const D2D1_POINT_2U *point, ID2D1RenderTarget *source, const D2D1_RECT_U *rect)
{
	struct bitmap *self = iface;
	struct target *from = target_of(source);
	if (!from || !from->bitmap)
		return E_INVALIDARG;
	copy_surface(self->surface, from->bitmap->surface, point, rect, from->bitmap->pixels);
	return S_OK;
}

/* ---- text: a layout drawn through a renderer that turns each glyph run into a coverage mask ---- */
static IDWriteFactory *text_factory;

static IDWriteFactory *writer(void)
{
	if (!text_factory)
		DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&text_factory);
	return text_factory;
}

static void draw_glyph_run(struct target *self, D2D1_POINT_2F baseline, const DWRITE_GLYPH_RUN *run, ID2D1Brush *brush, DWRITE_MEASURING_MODE measuring)
{
	IDWriteGlyphRunAnalysis *analysis = NULL;
	D2D1_MATRIX_3X2_F m = world(self);
	DWRITE_MATRIX transform = {m._11, m._12, m._21, m._22, m._31, m._32};
	const BOOL aliased = self->text_antialias == D2D1_TEXT_ANTIALIAS_MODE_ALIASED;
	const DWRITE_TEXTURE_TYPE texture = aliased ? DWRITE_TEXTURE_ALIASED_1x1 : DWRITE_TEXTURE_CLEARTYPE_3x1;
	RECT bounds;
	if (!writer() || !run || !run->glyphCount)
		return;
	if (FAILED(IDWriteFactory_CreateGlyphRunAnalysis(writer(), run, 1.0f, &transform,
			aliased ? DWRITE_RENDERING_MODE_ALIASED : DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, measuring, baseline.x, baseline.y, &analysis)))
		return;
	if (SUCCEEDED(IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, texture, &bounds)) && bounds.right > bounds.left && bounds.bottom > bounds.top) {
		const UINT32 width = (UINT32)(bounds.right - bounds.left), height = (UINT32)(bounds.bottom - bounds.top);
		const UINT32 per_pixel = aliased ? 1 : 3, size = width * height * per_pixel;
		BYTE *coverage = ALLOC(size);
		if (coverage && SUCCEEDED(IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis, texture, &bounds, coverage, size))) {
			struct command command;
			struct bytes data = {0};
			UINT32 index;
			/* Three subpixel coverages become one grey level; the mask is in device pixels, so the record carries no transform. */
			if (!aliased)
				for (index = 0; index < width * height; ++index)
					coverage[index] = (BYTE)((coverage[index * 3] + coverage[index * 3 + 1] + coverage[index * 3 + 2]) / 3);
			command_start(self, &command, OP_FILL_MASK);
			put_matrix(command.transform, &identity);
			if (put_brush(&command, &data, (struct brush *)brush)) {
				/* The brush keeps its place in the program's coordinates. */
				D2D1_MATRIX_3X2_F brush_space = ((struct brush *)brush)->kind == KIND_SOLID ? identity : multiply(&((struct brush *)brush)->transform, &m);
				if (((struct brush *)brush)->kind != KIND_SOLID)
					put_matrix(command.brush_transform, &brush_space);
				command.rect[0] = (float)bounds.left;
				command.rect[1] = (float)bounds.top;
				command.mask_width = width;
				command.mask_height = height;
				bytes_add(&data, coverage, width * height);
				send(self, &command, &data);
			}
			FREE(data.data);
		}
		FREE(coverage);
	}
	IDWriteGlyphRunAnalysis_Release(analysis);
}

static void STDMETHODCALLTYPE target_DrawGlyphRun(void *iface, D2D1_POINT_2F baseline, const DWRITE_GLYPH_RUN *run, ID2D1Brush *brush,
	DWRITE_MEASURING_MODE measuring)
{
	draw_glyph_run(target_of(iface), baseline, run, brush, measuring);
}

static void STDMETHODCALLTYPE target_DrawGlyphRun1(void *iface, D2D1_POINT_2F baseline, const DWRITE_GLYPH_RUN *run,
	const DWRITE_GLYPH_RUN_DESCRIPTION *description, ID2D1Brush *brush, DWRITE_MEASURING_MODE measuring)
{
	draw_glyph_run(target_of(iface), baseline, run, brush, measuring);
}

/* The renderer a text layout draws through: glyph runs, underlines and strikethroughs, with the default brush unless the layout gives
 * a drawing effect that is a brush. */
struct renderer {
	IDWriteTextRenderer iface;
	struct target *target;
	ID2D1Brush *brush;
};

static ID2D1Brush *effect_brush(struct renderer *renderer, IUnknown *effect)
{
	ID2D1Brush *brush = NULL;
	if (effect && SUCCEEDED(IUnknown_QueryInterface(effect, &IID_ID2D1Brush, (void **)&brush))) {
		object_Release(brush);
		return brush;
	}
	return renderer->brush;
}

static HRESULT STDMETHODCALLTYPE renderer_QueryInterface(IDWriteTextRenderer *iface, REFIID riid, void **out)
{
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IDWritePixelSnapping) || same_guid(riid, &IID_IDWriteTextRenderer)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE renderer_AddRef(IDWriteTextRenderer *iface) { return 2; }
static ULONG STDMETHODCALLTYPE renderer_Release(IDWriteTextRenderer *iface) { return 1; }
static HRESULT STDMETHODCALLTYPE renderer_IsPixelSnappingDisabled(IDWriteTextRenderer *iface, void *context, BOOL *disabled)
{
	*disabled = FALSE;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE renderer_GetCurrentTransform(IDWriteTextRenderer *iface, void *context, DWRITE_MATRIX *out)
{
	D2D1_MATRIX_3X2_F m = world(((struct renderer *)iface)->target);
	DWRITE_MATRIX transform = {m._11, m._12, m._21, m._22, m._31, m._32};
	*out = transform;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE renderer_GetPixelsPerDip(IDWriteTextRenderer *iface, void *context, FLOAT *ppd)
{
	*ppd = ((struct renderer *)iface)->target->dpi_x / 96;
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE renderer_DrawGlyphRun(IDWriteTextRenderer *iface, void *context, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
	const DWRITE_GLYPH_RUN *run, const DWRITE_GLYPH_RUN_DESCRIPTION *description, IUnknown *effect)
{
	struct renderer *renderer = (struct renderer *)iface;
	D2D1_POINT_2F baseline = {x, y};
	draw_glyph_run(renderer->target, baseline, run, effect_brush(renderer, effect), mode);
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE renderer_DrawLine(struct renderer *renderer, FLOAT x, FLOAT y, FLOAT width, FLOAT offset, FLOAT thickness,
	IUnknown *effect)
{
	D2D1_RECT_F rect = {x, y + offset, x + width, y + offset + (thickness > 0 ? thickness : 1)};
	target_FillRectangle(renderer->target, &rect, effect_brush(renderer, effect));
	return S_OK;
}
static HRESULT STDMETHODCALLTYPE renderer_DrawUnderline(IDWriteTextRenderer *iface, void *context, FLOAT x, FLOAT y, const DWRITE_UNDERLINE *line,
	IUnknown *effect)
{
	return renderer_DrawLine((struct renderer *)iface, x, y, line->width, line->offset, line->thickness, effect);
}
static HRESULT STDMETHODCALLTYPE renderer_DrawStrikethrough(IDWriteTextRenderer *iface, void *context, FLOAT x, FLOAT y,
	const DWRITE_STRIKETHROUGH *line, IUnknown *effect)
{
	return renderer_DrawLine((struct renderer *)iface, x, y, line->width, line->offset, line->thickness, effect);
}
static HRESULT STDMETHODCALLTYPE renderer_DrawInlineObject(IDWriteTextRenderer *iface, void *context, FLOAT x, FLOAT y, IDWriteInlineObject *object,
	BOOL sideways, BOOL rtl, IUnknown *effect)
{
	return object ? IDWriteInlineObject_Draw(object, context, iface, x, y, sideways, rtl, effect) : S_OK;
}

static IDWriteTextRendererVtbl renderer_vtbl = {
	renderer_QueryInterface, renderer_AddRef, renderer_Release, renderer_IsPixelSnappingDisabled, renderer_GetCurrentTransform,
	renderer_GetPixelsPerDip, renderer_DrawGlyphRun, renderer_DrawUnderline, renderer_DrawStrikethrough, renderer_DrawInlineObject};

static void draw_layout(struct target *self, D2D1_POINT_2F origin, IDWriteTextLayout *layout, ID2D1Brush *brush)
{
	struct renderer renderer;
	renderer.iface.lpVtbl = &renderer_vtbl;
	renderer.target = self;
	renderer.brush = brush;
	IDWriteTextLayout_Draw(layout, NULL, &renderer.iface, origin.x, origin.y);
}

static void STDMETHODCALLTYPE target_DrawTextLayout(void *iface, D2D1_POINT_2F origin, IDWriteTextLayout *layout, ID2D1Brush *brush,
	D2D1_DRAW_TEXT_OPTIONS options)
{
	if (layout)
		draw_layout(target_of(iface), origin, layout, brush);
}

static void STDMETHODCALLTYPE target_DrawText(void *iface, const WCHAR *text, UINT32 length, IDWriteTextFormat *format, const D2D1_RECT_F *rect,
	ID2D1Brush *brush, D2D1_DRAW_TEXT_OPTIONS options, DWRITE_MEASURING_MODE measuring)
{
	struct target *self = target_of(iface);
	IDWriteTextLayout *layout = NULL;
	D2D1_POINT_2F origin;
	if (!format || !rect || !writer())
		return;
	if (FAILED(IDWriteFactory_CreateTextLayout(writer(), text, length, format, rect->right - rect->left, rect->bottom - rect->top, &layout)))
		return;
	if (options & D2D1_DRAW_TEXT_OPTIONS_CLIP)
		target_PushAxisAlignedClip(iface, rect, self->antialias);
	origin.x = rect->left;
	origin.y = rect->top;
	draw_layout(self, origin, layout, brush);
	if (options & D2D1_DRAW_TEXT_OPTIONS_CLIP)
		target_PopAxisAlignedClip(iface);
	IDWriteTextLayout_Release(layout);
}

/* ---- GDI interop: a DIB section over the target's pixels while the program draws with GDI ---- */
static HRESULT STDMETHODCALLTYPE gdi_GetDC(void *iface, D2D1_DC_INITIALIZE_MODE mode, HDC *out)
{
	struct target *self = target_of(iface);
	BITMAPINFO info;
	const UINT32 width = self->bitmap ? self->bitmap->pixels.width : 0, height = self->bitmap ? self->bitmap->pixels.height : 0;
	if (!self->bitmap || self->gdi_dc)
		return D2DERR_WRONG_STATE;
	memset(&info, 0, sizeof(info));
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = (LONG)width;
	info.bmiHeader.biHeight = -(LONG)height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	self->gdi_dc = CreateCompatibleDC(NULL);
	self->gdi_bitmap = CreateDIBSection(self->gdi_dc, &info, DIB_RGB_COLORS, &self->gdi_bits, NULL, 0);
	if (!self->gdi_bitmap || !self->gdi_bits) {
		DeleteDC(self->gdi_dc);
		self->gdi_dc = NULL;
		return E_FAIL;
	}
	self->gdi_previous = SelectObject(self->gdi_dc, self->gdi_bitmap);
	if (mode == D2D1_DC_INITIALIZE_MODE_COPY)
		RavenCanvasPixels(self->bitmap->surface, 0, 0, width, height, self->gdi_bits, width * 4, 0);
	*out = self->gdi_dc;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE gdi_ReleaseDC(void *iface, const RECT *update)
{
	struct target *self = target_of(iface);
	const UINT32 width = self->bitmap ? self->bitmap->pixels.width : 0, height = self->bitmap ? self->bitmap->pixels.height : 0;
	if (!self->gdi_dc)
		return D2DERR_WRONG_STATE;
	RavenCanvasPixels(self->bitmap->surface, 0, 0, width, height, self->gdi_bits, width * 4, 1);
	SelectObject(self->gdi_dc, self->gdi_previous);
	DeleteObject(self->gdi_bitmap);
	DeleteDC(self->gdi_dc);
	self->gdi_dc = NULL;
	self->gdi_bitmap = NULL;
	self->gdi_bits = NULL;
	return S_OK;
}

/* ---- what one kind of target adds ---- */
static D2D1_WINDOW_STATE STDMETHODCALLTYPE hwnd_CheckWindowState(void *iface)
{
	return IsWindow(target_of(iface)->hwnd) && IsIconic(target_of(iface)->hwnd) ? D2D1_WINDOW_STATE_OCCLUDED : D2D1_WINDOW_STATE_NONE;
}

static HRESULT replace_surface(struct target *self, D2D1_SIZE_U size)
{
	struct bitmap *bitmap = make_bitmap(self->factory, size, self->format, self->dpi_x, self->dpi_y, D2D1_BITMAP_OPTIONS_TARGET);
	if (!bitmap)
		return E_OUTOFMEMORY;
	if (self->bitmap)
		object_Release(self->bitmap);
	self->bitmap = bitmap;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE hwnd_Resize(void *iface, const D2D1_SIZE_U *size)
{
	return replace_surface(target_of(iface), *size);
}

static HWND STDMETHODCALLTYPE hwnd_GetHwnd(void *iface) { return target_of(iface)->hwnd; }

static HRESULT STDMETHODCALLTYPE dcrt_BindDC(void *iface, HDC dc, const RECT *rect)
{
	struct target *self = target_of(iface);
	D2D1_SIZE_U size;
	if (!dc || !rect)
		return E_INVALIDARG;
	self->dc = dc;
	self->dc_rect = *rect;
	size.width = rect->right > rect->left ? (UINT32)(rect->right - rect->left) : 1;
	size.height = rect->bottom > rect->top ? (UINT32)(rect->bottom - rect->top) : 1;
	if (self->bitmap && self->bitmap->pixels.width == size.width && self->bitmap->pixels.height == size.height)
		return S_OK;
	return replace_surface(self, size);
}

/* ---- what a device context adds ---- */
static HRESULT make_device(IUnknown *factory, IUnknown *dxgi, ID2D1Device **out);

/* A render target made for a window, a device context or a bitmap has a device of its own, made when it is first asked for. */
static void STDMETHODCALLTYPE context_GetDevice(void *iface, ID2D1Device **device)
{
	struct target *self = target_of(iface);
	if (!self->device) {
		ID2D1Device *made = NULL;
		if (SUCCEEDED(make_device(self->factory, NULL, &made)))
			self->device = (struct device *)made;
	}
	*device = (ID2D1Device *)self->device;
	if (self->device)
		object_AddRef(self->device);
}

static void STDMETHODCALLTYPE context_SetTarget(void *iface, ID2D1Image *image)
{
	struct target *self = target_of(iface);
	ID2D1Bitmap *bitmap = NULL;
	if (image && FAILED(IUnknown_QueryInterface((IUnknown *)image, &IID_ID2D1Bitmap, (void **)&bitmap))) {
		unimplemented("ID2D1DeviceContext", "SetTarget to an image that is not a bitmap");
		return;
	}
	if (self->bitmap)
		object_Release(self->bitmap);
	self->bitmap = (struct bitmap *)bitmap;
	if (bitmap) {
		self->format = self->bitmap->format;
		self->dpi_x = self->bitmap->dpi_x;
		self->dpi_y = self->bitmap->dpi_y;
	}
}

static void STDMETHODCALLTYPE context_GetTarget(void *iface, ID2D1Image **image)
{
	struct target *self = target_of(iface);
	*image = (ID2D1Image *)self->bitmap;
	if (self->bitmap)
		object_AddRef(self->bitmap);
}

static void STDMETHODCALLTYPE context_SetRenderingControls(void *iface, const D2D1_RENDERING_CONTROLS *controls) { target_of(iface)->controls = *controls; }
static void STDMETHODCALLTYPE context_GetRenderingControls(void *iface, D2D1_RENDERING_CONTROLS *controls) { *controls = target_of(iface)->controls; }
static void STDMETHODCALLTYPE context_SetPrimitiveBlend(void *iface, D2D1_PRIMITIVE_BLEND blend) { target_of(iface)->blend = blend; }
static D2D1_PRIMITIVE_BLEND STDMETHODCALLTYPE context_GetPrimitiveBlend(void *iface) { return target_of(iface)->blend; }
static void STDMETHODCALLTYPE context_SetUnitMode(void *iface, D2D1_UNIT_MODE mode) { target_of(iface)->unit = mode; }
static D2D1_UNIT_MODE STDMETHODCALLTYPE context_GetUnitMode(void *iface) { return target_of(iface)->unit; }
static BOOL STDMETHODCALLTYPE context_IsDxgiFormatSupported(void *iface, DXGI_FORMAT format)
{
	return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM ||
		format == DXGI_FORMAT_A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}
static BOOL STDMETHODCALLTYPE context_IsBufferPrecisionSupported(void *iface, D2D1_BUFFER_PRECISION precision)
{
	return precision == D2D1_BUFFER_PRECISION_8BPC_UNORM || precision == D2D1_BUFFER_PRECISION_8BPC_UNORM_SRGB;
}

/* ---- a program's own effects ----
 * RegisterEffectFromString names an effect's custom properties in XML and binds each to a getter and a setter on the object the program's
 * factory makes. The registration is kept so that CreateEffect makes that object and routes the property values through the bindings. */
enum { REGISTERED_EFFECTS = 64, EFFECT_PROPERTIES = 32 };

/* D2D1_PROPERTY_BINDING and its functions, as d2d1effectauthor.h lays them out. */
typedef HRESULT (CALLBACK *property_setter)(IUnknown *effect, const BYTE *data, UINT32 size);
typedef HRESULT (CALLBACK *property_getter)(const IUnknown *effect, BYTE *data, UINT32 size, UINT32 *actual);

struct property_binding {
	const WCHAR *name;
	property_setter set;
	property_getter get;
};

struct effect_property {
	WCHAR *name;
	property_setter set;
	property_getter get;
};

struct registered_effect {
	CLSID clsid;
	PD2D1_EFFECT_FACTORY factory;
	struct effect_property properties[EFFECT_PROPERTIES];
	UINT32 property_count;
};

static struct registered_effect registered_effects[REGISTERED_EFFECTS];
static UINT32 registered_effect_count;
static CRITICAL_SECTION registered_lock;
static LONG registered_lock_ready;

static void lock_registered(void)
{
	if (!InterlockedCompareExchange(&registered_lock_ready, 1, 0))
		InitializeCriticalSection(&registered_lock);
	EnterCriticalSection(&registered_lock);
}

static struct registered_effect *find_registered(REFCLSID clsid)
{
	UINT32 index;
	for (index = 0; index < registered_effect_count; ++index)
		if (same_guid(&registered_effects[index].clsid, clsid))
			return &registered_effects[index];
	return NULL;
}

static int wide_equal(const WCHAR *a, const WCHAR *b)
{
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

static size_t wide_length(const WCHAR *text)
{
	size_t length = 0;
	while (text[length])
		++length;
	return length;
}

/* Whether text starts with prefix. */
static int wide_starts(const WCHAR *text, const WCHAR *prefix)
{
	while (*prefix && *text == *prefix) {
		++text;
		++prefix;
	}
	return !*prefix;
}

static WCHAR *wide_copy(const WCHAR *text, size_t length)
{
	WCHAR *copy = ALLOC((length + 1) * sizeof(WCHAR));
	if (copy) {
		memcpy(copy, text, length * sizeof(WCHAR));
		copy[length] = 0;
	}
	return copy;
}

/* The custom properties are the <Property> elements directly inside <Effect>, in order, other than the ones that describe the effect. */
static void parse_effect_properties(struct registered_effect *effect, const WCHAR *xml)
{
	static const WCHAR *const described[] = {L"DisplayName", L"Author", L"Category", L"Description"};
	int depth = 0;
	const WCHAR *at;
	for (at = xml; *at; ++at) {
		if (at[0] == '<' && at[1] == '/' && wide_starts(at + 2, L"Property")) {
			--depth;
			continue;
		}
		if (at[0] != '<' || !wide_starts(at + 1, L"Property") || (at[9] != ' ' && at[9] != '\t' && at[9] != '\r' && at[9] != '\n'))
			continue;
		{
			const WCHAR *end = at, *name = NULL;
			size_t length = 0;
			int closed;
			while (*end && *end != '>')
				++end;
			closed = end > at && end[-1] == '/';
			for (name = at; name < end; ++name)
				if (wide_starts(name, L"name=") && (name[5] == '\'' || name[5] == '"')) {
					const WCHAR quote = name[5];
					name += 6;
					while (name + length < end && name[length] != quote)
						++length;
					break;
				}
			if (depth == 0 && name < end && length) {
				WCHAR *copy = wide_copy(name, length);
				UINT32 index;
				int skip = 0;
				for (index = 0; copy && index < sizeof(described) / sizeof(described[0]); ++index)
					skip |= wide_equal(copy, described[index]);
				if (copy && !skip && effect->property_count < EFFECT_PROPERTIES)
					effect->properties[effect->property_count++].name = copy;
				else
					FREE(copy);
			}
			if (!closed)
				++depth;
			at = end;
			if (!*at)
				break;
		}
	}
}

static HRESULT register_effect(REFCLSID clsid, const WCHAR *xml, const D2D1_PROPERTY_BINDING *given, UINT32 count, PD2D1_EFFECT_FACTORY factory)
{
	const struct property_binding *bindings = (const struct property_binding *)given;
	struct registered_effect *effect;
	UINT32 index, property;
	if (!clsid || !xml || !factory || (count && !bindings))
		return E_INVALIDARG;
	lock_registered();
	if (!(effect = find_registered(clsid))) {
		if (registered_effect_count == REGISTERED_EFFECTS) {
			LeaveCriticalSection(&registered_lock);
			return E_OUTOFMEMORY;
		}
		effect = &registered_effects[registered_effect_count++];
		memset(effect, 0, sizeof(*effect));
		effect->clsid = *clsid;
		effect->factory = factory;
		parse_effect_properties(effect, xml);
		for (index = 0; index < count; ++index)
			for (property = 0; property < effect->property_count; ++property)
				if (bindings[index].name && wide_equal(bindings[index].name, effect->properties[property].name)) {
					effect->properties[property].set = bindings[index].set;
					effect->properties[property].get = bindings[index].get;
				}
	}
	LeaveCriticalSection(&registered_lock);
	return S_OK;
}

/* ---- effects ----
 * An effect keeps its inputs and the values set on its properties, and gives out one image as its output. What an effect computes is not
 * run here: its output draws nothing, which leaves what is under it, and its bounds are those of its first input. A program's own effects
 * (RegisterEffectFromString) are created the same way, so a program that draws through them keeps going. */
enum { EFFECT_INPUTS = 16, EFFECT_VALUES = 64 };

struct effect_value {
	UINT32 index, size;
	BYTE *data;
};

struct effect;

struct effect_image {
	struct object base;
	struct effect *effect;
};

struct effect {
	struct object base;
	/* ID2D1Resource, which an effect also is: its methods share the slots of the property methods, so it is a table of its own. */
	void **resource;
	struct effect_image *output;
	ID2D1Image *inputs[EFFECT_INPUTS];
	UINT32 input_count;
	struct effect_value values[EFFECT_VALUES];
	UINT32 value_count;
	/* For a program's own effect: the object its factory made, which the property bindings act on, and the registration. */
	IUnknown *impl;
	const struct registered_effect *registered;
};

static const GUID *const effect_iids[] = {&IID_ID2D1Properties, &IID_ID2D1Effect};
static const GUID *const effect_image_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Image};
static void **effect_table, **effect_image_table;

static void effect_image_destroy(struct object *base)
{
	(void)base;
}

static void effect_destroy(struct object *base)
{
	struct effect *self = (struct effect *)base;
	UINT32 index;
	for (index = 0; index < EFFECT_INPUTS; ++index)
		if (self->inputs[index])
			IUnknown_Release((IUnknown *)self->inputs[index]);
	for (index = 0; index < self->value_count; ++index)
		FREE(self->values[index].data);
	if (self->output) {
		self->output->effect = NULL;
		object_Release(self->output);
	}
	if (self->impl)
		IUnknown_Release(self->impl);
}

static struct effect_value *effect_value_at(struct effect *self, UINT32 index)
{
	UINT32 at;
	for (at = 0; at < self->value_count; ++at)
		if (self->values[at].index == index)
			return &self->values[at];
	return NULL;
}

/* The custom property at an index, for a program's own effect. */
static const struct effect_property *effect_property_at(const struct effect *self, UINT32 index)
{
	return self->registered && index < self->registered->property_count ? &self->registered->properties[index] : NULL;
}

static UINT32 STDMETHODCALLTYPE effect_GetPropertyCount(void *iface)
{
	const struct effect *self = iface;
	return self->registered ? self->registered->property_count : 0;
}

static HRESULT STDMETHODCALLTYPE effect_GetPropertyName(void *iface, UINT32 index, WCHAR *name, UINT32 count)
{
	const struct effect_property *property = effect_property_at(iface, index);
	size_t length;
	if (name && count)
		name[0] = 0;
	if (!property)
		return D2DERR_INVALID_PROPERTY;
	length = wide_length(property->name);
	if (!name || count <= length)
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	memcpy(name, property->name, (length + 1) * sizeof(WCHAR));
	return S_OK;
}

static UINT32 STDMETHODCALLTYPE effect_GetPropertyNameLength(void *iface, UINT32 index)
{
	const struct effect_property *property = effect_property_at(iface, index);
	return property ? (UINT32)wide_length(property->name) : 0;
}

static D2D1_PROPERTY_TYPE STDMETHODCALLTYPE effect_GetType(void *iface, UINT32 index) { return D2D1_PROPERTY_TYPE_UNKNOWN; }

static UINT32 STDMETHODCALLTYPE effect_GetPropertyIndex(void *iface, const WCHAR *name)
{
	const struct effect *self = iface;
	UINT32 index;
	for (index = 0; name && self->registered && index < self->registered->property_count; ++index)
		if (wide_equal(self->registered->properties[index].name, name))
			return index;
	return UINT_MAX;
}

static HRESULT STDMETHODCALLTYPE effect_SetValue(void *iface, UINT32 index, D2D1_PROPERTY_TYPE type, const BYTE *data, UINT32 size);
static HRESULT STDMETHODCALLTYPE effect_GetValue(void *iface, UINT32 index, D2D1_PROPERTY_TYPE type, BYTE *data, UINT32 size);

static HRESULT STDMETHODCALLTYPE effect_SetValueByName(void *iface, const WCHAR *name, D2D1_PROPERTY_TYPE type, const BYTE *data, UINT32 size)
{
	const UINT32 index = effect_GetPropertyIndex(iface, name);
	return index == UINT_MAX ? S_OK : effect_SetValue(iface, index, type, data, size);
}

static HRESULT STDMETHODCALLTYPE effect_SetValue(void *iface, UINT32 index, D2D1_PROPERTY_TYPE type, const BYTE *data, UINT32 size)
{
	struct effect *self = iface;
	struct effect_value *value = effect_value_at(self, index);
	const struct effect_property *property = effect_property_at(self, index);
	BYTE *copy;
	if (size && !data)
		return E_INVALIDARG;
	if (property && property->set && self->impl)
		return property->set(self->impl, data, size);
	if (!value) {
		if (self->value_count == EFFECT_VALUES)
			return E_OUTOFMEMORY;
		value = &self->values[self->value_count++];
		value->index = index;
		value->size = 0;
		value->data = NULL;
	}
	if (!(copy = ALLOC(size ? size : 1)))
		return E_OUTOFMEMORY;
	if (size)
		memcpy(copy, data, size);
	FREE(value->data);
	value->data = copy;
	value->size = size;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE effect_GetValueByName(void *iface, const WCHAR *name, D2D1_PROPERTY_TYPE type, BYTE *data, UINT32 size)
{
	const UINT32 index = effect_GetPropertyIndex(iface, name);
	if (index != UINT_MAX)
		return effect_GetValue(iface, index, type, data, size);
	if (data && size)
		memset(data, 0, size);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE effect_GetValue(void *iface, UINT32 index, D2D1_PROPERTY_TYPE type, BYTE *data, UINT32 size)
{
	const struct effect *self = iface;
	const struct effect_value *value = effect_value_at(iface, index);
	const struct effect_property *property = effect_property_at(self, index);
	if (size && !data)
		return E_INVALIDARG;
	if (property && property->get && self->impl) {
		UINT32 actual = 0;
		return property->get(self->impl, data, size, &actual);
	}
	if (size)
		memset(data, 0, size);
	if (value)
		memcpy(data, value->data, value->size < size ? value->size : size);
	return S_OK;
}

static UINT32 STDMETHODCALLTYPE effect_GetValueSize(void *iface, UINT32 index)
{
	const struct effect *self = iface;
	const struct effect_value *value = effect_value_at(iface, index);
	const struct effect_property *property = effect_property_at(self, index);
	if (property && property->get && self->impl) {
		UINT32 actual = 0;
		property->get(self->impl, NULL, 0, &actual);
		return actual;
	}
	return value ? value->size : 0;
}

static HRESULT STDMETHODCALLTYPE effect_GetSubProperties(void *iface, UINT32 index, ID2D1Properties **out)
{
	if (out)
		*out = NULL;
	return D2DERR_NO_SUBPROPERTIES;
}

static void STDMETHODCALLTYPE effect_SetInput(void *iface, UINT32 index, ID2D1Image *input, BOOL invalidate)
{
	struct effect *self = iface;
	if (index >= EFFECT_INPUTS)
		return;
	if (input)
		IUnknown_AddRef((IUnknown *)input);
	if (self->inputs[index])
		IUnknown_Release((IUnknown *)self->inputs[index]);
	self->inputs[index] = input;
	if (index >= self->input_count)
		self->input_count = index + 1;
}

static HRESULT STDMETHODCALLTYPE effect_SetInputCount(void *iface, UINT32 count)
{
	struct effect *self = iface;
	UINT32 index;
	if (count > EFFECT_INPUTS)
		return E_INVALIDARG;
	for (index = count; index < EFFECT_INPUTS; ++index)
		if (self->inputs[index]) {
			IUnknown_Release((IUnknown *)self->inputs[index]);
			self->inputs[index] = NULL;
		}
	self->input_count = count;
	return S_OK;
}

static void STDMETHODCALLTYPE effect_GetInput(void *iface, UINT32 index, ID2D1Image **input)
{
	struct effect *self = iface;
	*input = index < EFFECT_INPUTS ? self->inputs[index] : NULL;
	if (*input)
		IUnknown_AddRef((IUnknown *)*input);
}

static UINT32 STDMETHODCALLTYPE effect_GetInputCount(void *iface)
{
	return ((struct effect *)iface)->input_count;
}

static void STDMETHODCALLTYPE effect_GetOutput(void *iface, ID2D1Image **output)
{
	struct effect *self = iface;
	*output = (ID2D1Image *)self->output;
	object_AddRef(self->output);
}

static void **effect_resource_table;

static struct effect *effect_of(void *iface)
{
	return *(void ***)iface == effect_resource_table ? CONTAINER(iface, struct effect, resource) : iface;
}

static HRESULT STDMETHODCALLTYPE effect_QueryInterface(void *iface, REFIID riid, void **out)
{
	struct effect *self = effect_of(iface);
	void *found = NULL;
	if (!out)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || any_guid(riid, effect_iids, 2))
		found = self;
	else if (same_guid(riid, &IID_ID2D1Resource))
		found = &self->resource;
	else if (same_guid(riid, &IID_ID2D1Image)) {
		/* An effect answers for the image it gives out, as Direct2D's effects do: a program draws the effect where an image goes. */
		object_AddRef(self->output);
		*out = self->output;
		return S_OK;
	}
	*out = found;
	if (!found)
		return E_NOINTERFACE;
	InterlockedIncrement(&self->base.refs);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE effect_AddRef(void *iface)
{
	return object_AddRef(effect_of(iface));
}

static ULONG STDMETHODCALLTYPE effect_Release(void *iface)
{
	return object_Release(effect_of(iface));
}

static void STDMETHODCALLTYPE effect_GetFactory(void *iface, ID2D1Factory **factory)
{
	object_GetFactory(effect_of(iface), factory);
}

static HRESULT make_effect(IUnknown *factory, ID2D1Effect **out)
{
	struct effect *self;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!effect_table) {
		effect_table = make_table(ID2D1Properties_table, ID2D1Properties_methods + 5);
		effect_table[0] = (void *)effect_QueryInterface;
		effect_table[1] = (void *)effect_AddRef;
		effect_table[2] = (void *)effect_Release;
		effect_resource_table = ALLOC(4 * sizeof(void *));
		effect_resource_table[0] = (void *)effect_QueryInterface;
		effect_resource_table[1] = (void *)effect_AddRef;
		effect_resource_table[2] = (void *)effect_Release;
		effect_resource_table[3] = (void *)effect_GetFactory;
		effect_table[ID2D1Properties_GetPropertyCount_slot] = (void *)effect_GetPropertyCount;
		effect_table[ID2D1Properties_GetPropertyName_slot] = (void *)effect_GetPropertyName;
		effect_table[ID2D1Properties_GetPropertyNameLength_slot] = (void *)effect_GetPropertyNameLength;
		effect_table[ID2D1Properties_GetType_slot] = (void *)effect_GetType;
		effect_table[ID2D1Properties_GetPropertyIndex_slot] = (void *)effect_GetPropertyIndex;
		effect_table[ID2D1Properties_SetValueByName_slot] = (void *)effect_SetValueByName;
		effect_table[ID2D1Properties_SetValue_slot] = (void *)effect_SetValue;
		effect_table[ID2D1Properties_GetValueByName_slot] = (void *)effect_GetValueByName;
		effect_table[ID2D1Properties_GetValue_slot] = (void *)effect_GetValue;
		effect_table[ID2D1Properties_GetValueSize_slot] = (void *)effect_GetValueSize;
		effect_table[ID2D1Properties_GetSubProperties_slot] = (void *)effect_GetSubProperties;
		effect_table[ID2D1Properties_methods] = (void *)effect_SetInput;
		effect_table[ID2D1Properties_methods + 1] = (void *)effect_SetInputCount;
		effect_table[ID2D1Properties_methods + 2] = (void *)effect_GetInput;
		effect_table[ID2D1Properties_methods + 3] = (void *)effect_GetInputCount;
		effect_table[ID2D1Properties_methods + 4] = (void *)effect_GetOutput;
		effect_image_table = ALLOC(4 * sizeof(void *));
		set_resource(effect_image_table);
	}
	if (!(self = ALLOC(sizeof(*self))))
		return E_OUTOFMEMORY;
	memset(self, 0, sizeof(*self));
	if (!(self->output = ALLOC(sizeof(*self->output)))) {
		FREE(self);
		return E_OUTOFMEMORY;
	}
	object_init(&self->output->base, effect_image_table, effect_image_destroy, effect_image_iids, 2, factory);
	self->output->effect = self;
	object_init(&self->base, effect_table, effect_destroy, effect_iids, 2, factory);
	self->resource = effect_resource_table;
	*out = (ID2D1Effect *)self;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE context_CreateEffect(void *iface, REFCLSID clsid, ID2D1Effect **effect)
{
	struct target *self = target_of(iface);
	const struct registered_effect *registered;
	IUnknown *impl = NULL;
	HRESULT result;
	if (!clsid)
		return E_INVALIDARG;
	lock_registered();
	registered = find_registered(clsid);
	LeaveCriticalSection(&registered_lock);
	{
		/* RAVEN_TRACE_DWRITE names the effects a program creates, as the trace names what is not implemented. */
		static const char digits[] = "0123456789abcdef";
		char text[80] = "d2d1: CreateEffect ";
		const BYTE *bytes = (const BYTE *)clsid;
		size_t at = 19, index;
		for (index = 0; index < 16; ++index) {
			text[at++] = digits[bytes[index] >> 4];
			text[at++] = digits[bytes[index] & 15];
		}
		text[at++] = registered ? '+' : '-';
		text[at] = 0;
		RavenTrace(text);
	}
	/* A program's own effect starts with the object its factory makes. */
	if (registered && FAILED(result = registered->factory(&impl)))
		return result;
	if (FAILED(result = make_effect(self->factory, effect))) {
		if (impl)
			IUnknown_Release(impl);
		return result;
	}
	((struct effect *)*effect)->impl = impl;
	((struct effect *)*effect)->registered = registered;
	return S_OK;
}

/* The image an effect gives out, when the image is one: an effect's output reaches back to its effect while the effect lives. */
static struct effect *effect_of_image(ID2D1Image *image)
{
	struct effect_image *output = (struct effect_image *)image;
	if (!image)
		return NULL;
	/* The effect itself stands for its output, as an effect answers for ID2D1Image. */
	if (effect_table && *(void ***)image == effect_table)
		return (struct effect *)image;
	return effect_image_table && output->base.vtbl == effect_image_table ? output->effect : NULL;
}

/* The bounds of an image: a bitmap's size in device-independent pixels, then through the transform for the world bounds. */
static HRESULT STDMETHODCALLTYPE context_GetImageLocalBounds(void *iface, ID2D1Image *image, D2D1_RECT_F *bounds)
{
	ID2D1Bitmap *bitmap = NULL;
	struct bitmap *b;
	if (!image || !bounds)
		return E_INVALIDARG;
	if (FAILED(IUnknown_QueryInterface((IUnknown *)image, &IID_ID2D1Bitmap, (void **)&bitmap))) {
		/* An effect's output covers what its first input covers; with no input, nothing. */
		const struct effect *effect = effect_of_image(image);
		if (effect && effect->inputs[0])
			return context_GetImageLocalBounds(iface, effect->inputs[0], bounds);
		if (!effect)
			unimplemented("ID2D1DeviceContext", "GetImageLocalBounds of an image that is neither a bitmap nor an effect's output");
		/* What draws nothing here covers nothing. */
		bounds->left = bounds->top = bounds->right = bounds->bottom = 0;
		return S_OK;
	}
	b = (struct bitmap *)bitmap;
	bounds->left = bounds->top = 0;
	bounds->right = b->pixels.width * 96.0f / b->dpi_x;
	bounds->bottom = b->pixels.height * 96.0f / b->dpi_y;
	object_Release(bitmap);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE context_GetImageWorldBounds(void *iface, ID2D1Image *image, D2D1_RECT_F *bounds)
{
	struct path path = {0};
	D2D1_MATRIX_3X2_F m = target_of(iface)->transform;
	HRESULT hr = context_GetImageLocalBounds(iface, image, bounds);
	if (FAILED(hr))
		return hr;
	path_rectangle(&path, bounds);
	*bounds = path_bounds(&path, &m);
	path_free(&path);
	return S_OK;
}

static struct target *make_target(IUnknown *factory, int kind, const D2D1_RENDER_TARGET_PROPERTIES *properties)
{
	struct target *self = ALLOC(sizeof(*self));
	if (!self)
		return NULL;
	self->primary = kind == TARGET_HWND ? hwnd_table : kind == TARGET_DC ? dcrt_table : kind == TARGET_BITMAP ? bitmaprt_table :
		kind == TARGET_CONTEXT ? context_table : rt_table;
	self->context = context_secondary_table;
	self->gdi = gdi_table;
	self->refs = 1;
	self->kind = kind;
	self->factory = factory;
	IUnknown_AddRef(factory);
	self->transform = identity;
	self->dpi_x = properties && properties->dpiX > 0 ? properties->dpiX : 96;
	self->dpi_y = properties && properties->dpiY > 0 ? properties->dpiY : 96;
	self->format.format = properties && properties->pixelFormat.format ? properties->pixelFormat.format : DXGI_FORMAT_B8G8R8A8_UNORM;
	self->format.alphaMode = properties && properties->pixelFormat.alphaMode ? properties->pixelFormat.alphaMode : D2D1_ALPHA_MODE_PREMULTIPLIED;
	self->antialias = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
	self->text_antialias = D2D1_TEXT_ANTIALIAS_MODE_DEFAULT;
	self->unit = D2D1_UNIT_MODE_DIPS;
	return self;
}

/* ---- devices ---- */
struct device {
	struct object base;
	IUnknown *dxgi;
	UINT64 texture_memory;
	D2D1_RENDERING_PRIORITY priority;
};

static void device_destroy(struct object *base)
{
	struct device *self = (struct device *)base;
	if (self->dxgi)
		IUnknown_Release(self->dxgi);
}

static HRESULT STDMETHODCALLTYPE device_CreateDeviceContext(void *iface, D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext **out)
{
	struct device *self = iface;
	struct target *context = make_target(self->base.factory, TARGET_CONTEXT, NULL);
	if (!context)
		return E_OUTOFMEMORY;
	context->device = self;
	object_AddRef(self);
	*out = (ID2D1DeviceContext *)context;
	return S_OK;
}
static void STDMETHODCALLTYPE device_SetMaximumTextureMemory(void *iface, UINT64 bytes) { ((struct device *)iface)->texture_memory = bytes; }
static UINT64 STDMETHODCALLTYPE device_GetMaximumTextureMemory(void *iface) { return ((struct device *)iface)->texture_memory; }
static void STDMETHODCALLTYPE device_ClearResources(void *iface, UINT32 milliseconds) {}
static D2D1_RENDERING_PRIORITY STDMETHODCALLTYPE device_GetRenderingPriority(void *iface) { return ((struct device *)iface)->priority; }
static void STDMETHODCALLTYPE device_SetRenderingPriority(void *iface, D2D1_RENDERING_PRIORITY priority) { ((struct device *)iface)->priority = priority; }
static HRESULT STDMETHODCALLTYPE device_FlushDeviceContexts(void *iface, ID2D1Bitmap *bitmap) { return S_OK; }
static HRESULT STDMETHODCALLTYPE device_GetDxgiDevice(void *iface, IDXGIDevice **out)
{
	struct device *self = iface;
	*out = (IDXGIDevice *)self->dxgi;
	if (!self->dxgi)
		return D2DERR_INVALID_CALL;
	IUnknown_AddRef(self->dxgi);
	return S_OK;
}
static void STDMETHODCALLTYPE device_SetMaximumColorGlyphCacheMemory(void *iface, UINT64 bytes) {}
static UINT64 STDMETHODCALLTYPE device_GetMaximumColorGlyphCacheMemory(void *iface) { return 0; }

static void **device_table;
static const GUID *const device_iids[] = {&IID_ID2D1Resource, &IID_ID2D1Device, &IID_ID2D1Device1, &IID_ID2D1Device2, &IID_ID2D1Device3,
	&IID_ID2D1Device4};

static HRESULT make_device(IUnknown *factory, IUnknown *dxgi, ID2D1Device **out)
{
	struct device *self = ALLOC(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	object_init(&self->base, device_table, device_destroy, device_iids, 6, factory);
	self->dxgi = dxgi;
	if (dxgi)
		IUnknown_AddRef(dxgi);
	self->texture_memory = 128 << 20;
	*out = (ID2D1Device *)self;
	return S_OK;
}

/* ---- the factory ---- */
struct factory {
	void **vtbl;
	void **multithread;
	LONG refs;
	D2D1_FACTORY_TYPE type;
	CRITICAL_SECTION lock;
};

static void **factory_table, **multithread_table;

static struct factory *factory_of(void *iface)
{
	return *(void ***)iface == multithread_table ? CONTAINER(iface, struct factory, multithread) : iface;
}

static const GUID *const factory_iids[] = {&IID_ID2D1Factory, &IID_ID2D1Factory1, &IID_ID2D1Factory2, &IID_ID2D1Factory3, &IID_ID2D1Factory4,
	&IID_ID2D1Factory5, &IID_ID2D1Factory6, &IID_ID2D1Factory7};

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(void *iface, REFIID riid, void **out)
{
	struct factory *self = factory_of(iface);
	void *found = NULL;
	if (same_guid(riid, &IID_IUnknown) || any_guid(riid, factory_iids, 8))
		found = self;
	else if (same_guid(riid, &IID_ID2D1Multithread))
		found = &self->multithread;
	if (!found) {
		*out = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&self->refs);
	*out = found;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE factory_AddRef(void *iface)
{
	return (ULONG)InterlockedIncrement(&factory_of(iface)->refs);
}

static ULONG STDMETHODCALLTYPE factory_Release(void *iface)
{
	struct factory *self = factory_of(iface);
	const LONG refs = InterlockedDecrement(&self->refs);
	if (!refs) {
		DeleteCriticalSection(&self->lock);
		FREE(self);
	}
	return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE factory_ReloadSystemMetrics(void *iface) { return S_OK; }
static void STDMETHODCALLTYPE factory_GetDesktopDpi(void *iface, FLOAT *x, FLOAT *y)
{
	*x = 96;
	*y = 96;
}

static HRESULT STDMETHODCALLTYPE factory_CreateRectangleGeometry(void *iface, const D2D1_RECT_F *rect, ID2D1RectangleGeometry **out)
{
	struct geometry *geometry = make_geometry(iface, GEOMETRY_RECTANGLE);
	if (!geometry)
		return E_OUTOFMEMORY;
	geometry->rect = *rect;
	path_rectangle(&geometry->path, rect);
	*out = (ID2D1RectangleGeometry *)geometry;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateRoundedRectangleGeometry(void *iface, const D2D1_ROUNDED_RECT *rect, ID2D1RoundedRectangleGeometry **out)
{
	struct geometry *geometry = make_geometry(iface, GEOMETRY_ROUNDED);
	if (!geometry)
		return E_OUTOFMEMORY;
	geometry->rounded = *rect;
	path_rounded(&geometry->path, rect);
	*out = (ID2D1RoundedRectangleGeometry *)geometry;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateEllipseGeometry(void *iface, const D2D1_ELLIPSE *ellipse, ID2D1EllipseGeometry **out)
{
	struct geometry *geometry = make_geometry(iface, GEOMETRY_ELLIPSE);
	if (!geometry)
		return E_OUTOFMEMORY;
	geometry->ellipse = *ellipse;
	path_ellipse(&geometry->path, ellipse->point.x, ellipse->point.y, ellipse->radiusX, ellipse->radiusY);
	*out = (ID2D1EllipseGeometry *)geometry;
	return S_OK;
}

/* A group is the figures of its members in one path, under the group's fill mode. */
static HRESULT STDMETHODCALLTYPE factory_CreateGeometryGroup(void *iface, D2D1_FILL_MODE fill, ID2D1Geometry **members, UINT32 count,
	ID2D1GeometryGroup **out)
{
	struct geometry *geometry = make_geometry(iface, GEOMETRY_GROUP);
	UINT32 index;
	if (!geometry)
		return E_OUTOFMEMORY;
	geometry->path.fill = fill;
	geometry->members = ALLOC((count ? count : 1) * sizeof(*geometry->members));
	for (index = 0; index < count; ++index) {
		struct geometry *member = (struct geometry *)members[index];
		geometry->members[index] = member;
		object_AddRef(member);
		if (member->path.count)
			path_put(&geometry->path, member->path.data, member->path.count);
	}
	geometry->member_count = count;
	*out = (ID2D1GeometryGroup *)geometry;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateTransformedGeometry(void *iface, ID2D1Geometry *source, const D2D1_MATRIX_3X2_F *transform,
	ID2D1TransformedGeometry **out)
{
	struct geometry *geometry = make_geometry(iface, GEOMETRY_TRANSFORMED);
	if (!geometry)
		return E_OUTOFMEMORY;
	geometry->source = (struct geometry *)source;
	object_AddRef(source);
	geometry->transform = *transform;
	path_copy(&geometry->path, &geometry->source->path);
	path_transform(&geometry->path, transform);
	*out = (ID2D1TransformedGeometry *)geometry;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreatePathGeometry(void *iface, ID2D1PathGeometry **out)
{
	struct geometry *geometry = make_geometry(iface, GEOMETRY_PATH);
	if (!geometry)
		return E_OUTOFMEMORY;
	*out = (ID2D1PathGeometry *)geometry;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateStrokeStyle(void *iface, const D2D1_STROKE_STYLE_PROPERTIES *properties, const FLOAT *dashes,
	UINT32 count, ID2D1StrokeStyle **out)
{
	D2D1_STROKE_STYLE_PROPERTIES1 properties1;
	memcpy(&properties1, properties, sizeof(*properties));
	properties1.transformType = D2D1_STROKE_TRANSFORM_TYPE_NORMAL;
	return make_stroke(iface, &properties1, dashes, count, (void **)out);
}

static HRESULT STDMETHODCALLTYPE factory_CreateStrokeStyle1(void *iface, const D2D1_STROKE_STYLE_PROPERTIES1 *properties, const FLOAT *dashes,
	UINT32 count, ID2D1StrokeStyle1 **out)
{
	return make_stroke(iface, properties, dashes, count, (void **)out);
}

static HRESULT make_state_block(IUnknown *factory, const D2D1_DRAWING_STATE_DESCRIPTION1 *description, IDWriteRenderingParams *params, void **out)
{
	struct state_block *self = ALLOC(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	object_init(&self->base, state_table, state_block_destroy, state_iids, 3, factory);
	self->description.transform = identity;
	if (description)
		self->description = *description;
	state_SetTextRenderingParams(self, params);
	*out = self;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateDrawingStateBlock(void *iface, const D2D1_DRAWING_STATE_DESCRIPTION *description,
	IDWriteRenderingParams *params, ID2D1DrawingStateBlock **out)
{
	D2D1_DRAWING_STATE_DESCRIPTION1 description1;
	memset(&description1, 0, sizeof(description1));
	description1.transform = identity;
	if (description)
		memcpy(&description1, description, sizeof(*description));
	return make_state_block(iface, &description1, params, (void **)out);
}

static HRESULT STDMETHODCALLTYPE factory_CreateDrawingStateBlock1(void *iface, const D2D1_DRAWING_STATE_DESCRIPTION1 *description,
	IDWriteRenderingParams *params, ID2D1DrawingStateBlock1 **out)
{
	return make_state_block(iface, description, params, (void **)out);
}

static HRESULT STDMETHODCALLTYPE factory_CreateHwndRenderTarget(void *iface, const D2D1_RENDER_TARGET_PROPERTIES *properties,
	const D2D1_HWND_RENDER_TARGET_PROPERTIES *hwnd_properties, ID2D1HwndRenderTarget **out)
{
	struct target *target = make_target(iface, TARGET_HWND, properties);
	D2D1_SIZE_U size = hwnd_properties->pixelSize;
	HRESULT hr;
	if (!target)
		return E_OUTOFMEMORY;
	target->hwnd = hwnd_properties->hwnd;
	if (!size.width || !size.height) {
		RECT client;
		GetClientRect(hwnd_properties->hwnd, &client);
		size.width = client.right > client.left ? (UINT32)(client.right - client.left) : 1;
		size.height = client.bottom > client.top ? (UINT32)(client.bottom - client.top) : 1;
	}
	if (FAILED(hr = replace_surface(target, size))) {
		target_Release(target);
		return hr;
	}
	*out = (ID2D1HwndRenderTarget *)target;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateDCRenderTarget(void *iface, const D2D1_RENDER_TARGET_PROPERTIES *properties, ID2D1DCRenderTarget **out)
{
	struct target *target = make_target(iface, TARGET_DC, properties);
	if (!target)
		return E_OUTOFMEMORY;
	*out = (ID2D1DCRenderTarget *)target;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateWicBitmapRenderTarget(void *iface, IWICBitmap *wic, const D2D1_RENDER_TARGET_PROPERTIES *properties,
	ID2D1RenderTarget **out)
{
	struct target *target = make_target(iface, TARGET_WIC, properties);
	UINT width = 0, height = 0;
	D2D1_SIZE_U size;
	HRESULT hr;
	if (!target)
		return E_OUTOFMEMORY;
	IWICBitmap_GetSize(wic, &width, &height);
	size.width = width;
	size.height = height;
	target->wic = wic;
	IWICBitmap_AddRef(wic);
	if (FAILED(hr = replace_surface(target, size))) {
		target_Release(target);
		return hr;
	}
	read_wic(target);
	*out = (ID2D1RenderTarget *)target;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateDxgiSurfaceRenderTarget(void *iface, IDXGISurface *surface, const D2D1_RENDER_TARGET_PROPERTIES *properties,
	ID2D1RenderTarget **out)
{
	struct target *target = make_target(iface, TARGET_DXGI, properties);
	struct bitmap *bitmap;
	HRESULT hr;
	if (!target)
		return E_OUTOFMEMORY;
	if (FAILED(hr = bitmap_from_dxgi(target, surface, NULL, &bitmap))) {
		target_Release(target);
		return hr;
	}
	target->bitmap = bitmap;
	*out = (ID2D1RenderTarget *)target;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_CreateDevice(void *iface, IDXGIDevice *dxgi, ID2D1Device **out)
{
	return make_device(iface, (IUnknown *)dxgi, out);
}

/* Effects of the program's own: the registration is kept for CreateEffect (see register_effect). */
static HRESULT STDMETHODCALLTYPE factory_RegisterEffectFromString(void *iface, REFCLSID clsid, const WCHAR *xml, const D2D1_PROPERTY_BINDING *bindings,
	UINT32 count, PD2D1_EFFECT_FACTORY effect_factory)
{
	return register_effect(clsid, xml, bindings, count, effect_factory);
}

static HRESULT STDMETHODCALLTYPE factory_RegisterEffectFromStream(void *iface, REFCLSID clsid, IStream *stream, const D2D1_PROPERTY_BINDING *bindings,
	UINT32 count, PD2D1_EFFECT_FACTORY effect_factory)
{
	/* The stream holds the XML in UTF-8 or UTF-16 with a byte order mark; it is read whole and widened when it is not UTF-16. */
	char *bytes;
	WCHAR *xml;
	ULONG read = 0, total = 0, capacity = 65536;
	HRESULT result;
	if (!stream)
		return E_INVALIDARG;
	if (!(bytes = ALLOC(capacity + 2)))
		return E_OUTOFMEMORY;
	while (total < capacity && SUCCEEDED(IStream_Read(stream, bytes + total, capacity - total, &read)) && read)
		total += read;
	bytes[total] = bytes[total + 1] = 0;
	if (total >= 2 && (BYTE)bytes[0] == 0xff && (BYTE)bytes[1] == 0xfe) {
		result = register_effect(clsid, (const WCHAR *)(bytes + 2), bindings, count, effect_factory);
	} else {
		const int length = MultiByteToWideChar(CP_UTF8, 0, bytes, -1, NULL, 0);
		if (!(xml = ALLOC((length ? length : 1) * sizeof(WCHAR)))) {
			FREE(bytes);
			return E_OUTOFMEMORY;
		}
		MultiByteToWideChar(CP_UTF8, 0, bytes, -1, xml, length);
		result = register_effect(clsid, xml, bindings, count, effect_factory);
		FREE(xml);
	}
	FREE(bytes);
	return result;
}
static HRESULT STDMETHODCALLTYPE factory_UnregisterEffect(void *iface, REFCLSID clsid) { return S_OK; }
static HRESULT STDMETHODCALLTYPE factory_GetRegisteredEffects(void *iface, CLSID *effects, UINT32 capacity, UINT32 *returned, UINT32 *registered)
{
	if (returned) *returned = 0;
	if (registered) *registered = 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE multithread_QueryInterface(void *iface, REFIID riid, void **out) { return factory_QueryInterface(iface, riid, out); }
static BOOL STDMETHODCALLTYPE multithread_GetMultithreadProtected(void *iface) { return factory_of(iface)->type == D2D1_FACTORY_TYPE_MULTI_THREADED; }
static void STDMETHODCALLTYPE multithread_Enter(void *iface) { EnterCriticalSection(&factory_of(iface)->lock); }
static void STDMETHODCALLTYPE multithread_Leave(void *iface) { LeaveCriticalSection(&factory_of(iface)->lock); }

/* ---- the method tables ---- */
static void set_target(void **table)
{
	table[0] = (void *)target_QueryInterface;
	table[1] = (void *)target_AddRef;
	table[2] = (void *)target_Release;
	table[ID2D1DeviceContext5_GetFactory_slot] = (void *)target_GetFactory;
	table[ID2D1DeviceContext5_CreateBitmap_slot] = (void *)target_CreateBitmap;
	table[ID2D1DeviceContext5_CreateBitmapFromWicBitmap_slot] = (void *)target_CreateBitmapFromWicBitmap;
	table[ID2D1DeviceContext5_CreateSharedBitmap_slot] = (void *)target_CreateSharedBitmap;
	table[ID2D1DeviceContext5_CreateBitmapBrush_slot] = (void *)target_CreateBitmapBrush;
	table[ID2D1DeviceContext5_CreateSolidColorBrush_slot] = (void *)target_CreateSolidColorBrush;
	table[ID2D1DeviceContext5_CreateGradientStopCollection_slot] = (void *)target_CreateGradientStopCollection;
	table[ID2D1DeviceContext5_CreateLinearGradientBrush_slot] = (void *)target_CreateLinearGradientBrush;
	table[ID2D1DeviceContext5_CreateRadialGradientBrush_slot] = (void *)target_CreateRadialGradientBrush;
	table[ID2D1DeviceContext5_CreateCompatibleRenderTarget_slot] = (void *)target_CreateCompatibleRenderTarget;
	table[ID2D1DeviceContext5_CreateLayer_slot] = (void *)target_CreateLayer;
	table[ID2D1DeviceContext5_DrawLine_slot] = (void *)target_DrawLine;
	table[ID2D1DeviceContext5_DrawRectangle_slot] = (void *)target_DrawRectangle;
	table[ID2D1DeviceContext5_FillRectangle_slot] = (void *)target_FillRectangle;
	table[ID2D1DeviceContext5_DrawRoundedRectangle_slot] = (void *)target_DrawRoundedRectangle;
	table[ID2D1DeviceContext5_FillRoundedRectangle_slot] = (void *)target_FillRoundedRectangle;
	table[ID2D1DeviceContext5_DrawEllipse_slot] = (void *)target_DrawEllipse;
	table[ID2D1DeviceContext5_FillEllipse_slot] = (void *)target_FillEllipse;
	table[ID2D1DeviceContext5_DrawGeometry_slot] = (void *)target_DrawGeometry;
	table[ID2D1DeviceContext5_FillGeometry_slot] = (void *)target_FillGeometry;
	table[ID2D1DeviceContext5_FillOpacityMask_slot] = (void *)target_FillOpacityMask;
	table[ID2D1DeviceContext5_FillOpacityMask_91_slot] = (void *)context_FillOpacityMask;
	table[ID2D1DeviceContext5_CreateFilledGeometryRealization_slot] = (void *)context_CreateFilledGeometryRealization;
	table[ID2D1DeviceContext5_CreateStrokedGeometryRealization_slot] = (void *)context_CreateStrokedGeometryRealization;
	table[ID2D1DeviceContext5_DrawGeometryRealization_slot] = (void *)context_DrawGeometryRealization;
	table[ID2D1DeviceContext5_DrawBitmap_slot] = (void *)target_DrawBitmap;
	table[ID2D1DeviceContext5_DrawText_slot] = (void *)target_DrawText;
	table[ID2D1DeviceContext5_DrawTextLayout_slot] = (void *)target_DrawTextLayout;
	table[ID2D1DeviceContext5_DrawGlyphRun_slot] = (void *)target_DrawGlyphRun;
	table[ID2D1DeviceContext5_SetTransform_slot] = (void *)target_SetTransform;
	table[ID2D1DeviceContext5_GetTransform_slot] = (void *)target_GetTransform;
	table[ID2D1DeviceContext5_SetAntialiasMode_slot] = (void *)target_SetAntialiasMode;
	table[ID2D1DeviceContext5_GetAntialiasMode_slot] = (void *)target_GetAntialiasMode;
	table[ID2D1DeviceContext5_SetTextAntialiasMode_slot] = (void *)target_SetTextAntialiasMode;
	table[ID2D1DeviceContext5_GetTextAntialiasMode_slot] = (void *)target_GetTextAntialiasMode;
	table[ID2D1DeviceContext5_SetTextRenderingParams_slot] = (void *)target_SetTextRenderingParams;
	table[ID2D1DeviceContext5_GetTextRenderingParams_slot] = (void *)target_GetTextRenderingParams;
	table[ID2D1DeviceContext5_SetTags_slot] = (void *)target_SetTags;
	table[ID2D1DeviceContext5_GetTags_slot] = (void *)target_GetTags;
	table[ID2D1DeviceContext5_PushLayer_slot] = (void *)target_PushLayer;
	table[ID2D1DeviceContext5_PopLayer_slot] = (void *)target_PopLayer;
	table[ID2D1DeviceContext5_Flush_slot] = (void *)target_Flush;
	table[ID2D1DeviceContext5_SaveDrawingState_slot] = (void *)target_SaveDrawingState;
	table[ID2D1DeviceContext5_RestoreDrawingState_slot] = (void *)target_RestoreDrawingState;
	table[ID2D1DeviceContext5_PushAxisAlignedClip_slot] = (void *)target_PushAxisAlignedClip;
	table[ID2D1DeviceContext5_PopAxisAlignedClip_slot] = (void *)target_PopAxisAlignedClip;
	table[ID2D1DeviceContext5_Clear_slot] = (void *)target_Clear;
	table[ID2D1DeviceContext5_BeginDraw_slot] = (void *)target_BeginDraw;
	table[ID2D1DeviceContext5_EndDraw_slot] = (void *)target_EndDraw;
	table[ID2D1DeviceContext5_GetPixelFormat_slot] = (void *)target_GetPixelFormat;
	table[ID2D1DeviceContext5_SetDpi_slot] = (void *)target_SetDpi;
	table[ID2D1DeviceContext5_GetDpi_slot] = (void *)target_GetDpi;
	table[ID2D1DeviceContext5_GetSize_slot] = (void *)target_GetSize;
	table[ID2D1DeviceContext5_GetPixelSize_slot] = (void *)target_GetPixelSize;
	table[ID2D1DeviceContext5_GetMaximumBitmapSize_slot] = (void *)target_GetMaximumBitmapSize;
	table[ID2D1DeviceContext5_IsSupported_slot] = (void *)target_IsSupported;
}

static void set_context(void **table)
{
	set_target(table);
	table[ID2D1DeviceContext5_CreateBitmap_57_slot] = (void *)target_CreateBitmap1;
	table[ID2D1DeviceContext5_CreateBitmapFromWicBitmap_58_slot] = (void *)target_CreateBitmapFromWicBitmap1;
	table[ID2D1DeviceContext5_CreateBitmapFromDxgiSurface_slot] = (void *)target_CreateBitmapFromDxgiSurface;
	table[ID2D1DeviceContext5_CreateGradientStopCollection_64_slot] = (void *)target_CreateGradientStopCollection1;
	table[ID2D1DeviceContext5_CreateImageBrush_slot] = (void *)target_CreateImageBrush;
	table[ID2D1DeviceContext5_CreateBitmapBrush_66_slot] = (void *)target_CreateBitmapBrush1;
	table[ID2D1DeviceContext5_IsDxgiFormatSupported_slot] = (void *)context_IsDxgiFormatSupported;
	table[ID2D1DeviceContext5_IsBufferPrecisionSupported_slot] = (void *)context_IsBufferPrecisionSupported;
	table[ID2D1DeviceContext5_GetImageLocalBounds_slot] = (void *)context_GetImageLocalBounds;
	table[ID2D1DeviceContext5_CreateEffect_slot] = (void *)context_CreateEffect;
	table[ID2D1DeviceContext5_GetImageWorldBounds_slot] = (void *)context_GetImageWorldBounds;
	table[ID2D1DeviceContext5_GetDevice_slot] = (void *)context_GetDevice;
	table[ID2D1DeviceContext5_SetTarget_slot] = (void *)context_SetTarget;
	table[ID2D1DeviceContext5_GetTarget_slot] = (void *)context_GetTarget;
	table[ID2D1DeviceContext5_SetRenderingControls_slot] = (void *)context_SetRenderingControls;
	table[ID2D1DeviceContext5_GetRenderingControls_slot] = (void *)context_GetRenderingControls;
	table[ID2D1DeviceContext5_SetPrimitiveBlend_slot] = (void *)context_SetPrimitiveBlend;
	table[ID2D1DeviceContext5_GetPrimitiveBlend_slot] = (void *)context_GetPrimitiveBlend;
	table[ID2D1DeviceContext5_SetUnitMode_slot] = (void *)context_SetUnitMode;
	table[ID2D1DeviceContext5_GetUnitMode_slot] = (void *)context_GetUnitMode;
	table[ID2D1DeviceContext5_DrawGlyphRun_82_slot] = (void *)target_DrawGlyphRun1;
	table[ID2D1DeviceContext5_DrawImage_slot] = (void *)target_DrawImage;
	table[ID2D1DeviceContext5_DrawBitmap_85_slot] = (void *)target_DrawBitmap1;
	table[ID2D1DeviceContext5_PushLayer_86_slot] = (void *)target_PushLayer1;
}

static void build_tables(void)
{
	build_geometry_tables();
	build_resource_tables();
	hwnd_table = make_table(ID2D1HwndRenderTarget_table, ID2D1HwndRenderTarget_methods);
	set_target(hwnd_table);
	hwnd_table[ID2D1HwndRenderTarget_CheckWindowState_slot] = (void *)hwnd_CheckWindowState;
	hwnd_table[ID2D1HwndRenderTarget_Resize_slot] = (void *)hwnd_Resize;
	hwnd_table[ID2D1HwndRenderTarget_GetHwnd_slot] = (void *)hwnd_GetHwnd;
	dcrt_table = make_table(ID2D1DCRenderTarget_table, ID2D1DCRenderTarget_methods);
	set_target(dcrt_table);
	dcrt_table[ID2D1DCRenderTarget_BindDC_slot] = (void *)dcrt_BindDC;
	bitmaprt_table = make_table(ID2D1BitmapRenderTarget_table, ID2D1BitmapRenderTarget_methods);
	set_target(bitmaprt_table);
	bitmaprt_table[ID2D1BitmapRenderTarget_GetBitmap_slot] = (void *)bitmaprt_GetBitmap;
	rt_table = make_table(ID2D1DCRenderTarget_table, ID2D1DCRenderTarget_methods);
	set_target(rt_table);
	context_table = make_table(ID2D1DeviceContext5_table, ID2D1DeviceContext5_methods);
	set_context(context_table);
	context_secondary_table = make_table(ID2D1DeviceContext5_table, ID2D1DeviceContext5_methods);
	set_context(context_secondary_table);
	gdi_table = make_table(ID2D1GdiInteropRenderTarget_table, ID2D1GdiInteropRenderTarget_methods);
	gdi_table[0] = (void *)target_QueryInterface;
	gdi_table[1] = (void *)target_AddRef;
	gdi_table[2] = (void *)target_Release;
	gdi_table[ID2D1GdiInteropRenderTarget_GetDC_slot] = (void *)gdi_GetDC;
	gdi_table[ID2D1GdiInteropRenderTarget_ReleaseDC_slot] = (void *)gdi_ReleaseDC;
	bitmap_table[ID2D1Bitmap1_CopyFromRenderTarget_slot] = (void *)bitmap_CopyFromRenderTarget;

	device_table = make_table(ID2D1Device4_table, ID2D1Device4_methods);
	set_resource(device_table);
	device_table[ID2D1Device4_CreateDeviceContext_slot] = (void *)device_CreateDeviceContext;
	device_table[ID2D1Device4_SetMaximumTextureMemory_slot] = (void *)device_SetMaximumTextureMemory;
	device_table[ID2D1Device4_GetMaximumTextureMemory_slot] = (void *)device_GetMaximumTextureMemory;
	device_table[ID2D1Device4_ClearResources_slot] = (void *)device_ClearResources;
	device_table[ID2D1Device4_GetRenderingPriority_slot] = (void *)device_GetRenderingPriority;
	device_table[ID2D1Device4_SetRenderingPriority_slot] = (void *)device_SetRenderingPriority;
	device_table[ID2D1Device4_CreateDeviceContext_11_slot] = (void *)device_CreateDeviceContext;
	device_table[ID2D1Device4_CreateDeviceContext_12_slot] = (void *)device_CreateDeviceContext;
	device_table[ID2D1Device4_FlushDeviceContexts_slot] = (void *)device_FlushDeviceContexts;
	device_table[ID2D1Device4_GetDxgiDevice_slot] = (void *)device_GetDxgiDevice;
	device_table[ID2D1Device4_CreateDeviceContext_15_slot] = (void *)device_CreateDeviceContext;
	device_table[ID2D1Device4_CreateDeviceContext_16_slot] = (void *)device_CreateDeviceContext;
	device_table[ID2D1Device4_SetMaximumColorGlyphCacheMemory_slot] = (void *)device_SetMaximumColorGlyphCacheMemory;
	device_table[ID2D1Device4_GetMaximumColorGlyphCacheMemory_slot] = (void *)device_GetMaximumColorGlyphCacheMemory;

	factory_table = make_table(ID2D1Factory7_table, ID2D1Factory7_methods);
	factory_table[0] = (void *)factory_QueryInterface;
	factory_table[1] = (void *)factory_AddRef;
	factory_table[2] = (void *)factory_Release;
	factory_table[ID2D1Factory7_ReloadSystemMetrics_slot] = (void *)factory_ReloadSystemMetrics;
	factory_table[ID2D1Factory7_GetDesktopDpi_slot] = (void *)factory_GetDesktopDpi;
	factory_table[ID2D1Factory7_CreateRectangleGeometry_slot] = (void *)factory_CreateRectangleGeometry;
	factory_table[ID2D1Factory7_CreateRoundedRectangleGeometry_slot] = (void *)factory_CreateRoundedRectangleGeometry;
	factory_table[ID2D1Factory7_CreateEllipseGeometry_slot] = (void *)factory_CreateEllipseGeometry;
	factory_table[ID2D1Factory7_CreateGeometryGroup_slot] = (void *)factory_CreateGeometryGroup;
	factory_table[ID2D1Factory7_CreateTransformedGeometry_slot] = (void *)factory_CreateTransformedGeometry;
	factory_table[ID2D1Factory7_CreatePathGeometry_slot] = (void *)factory_CreatePathGeometry;
	factory_table[ID2D1Factory7_CreateStrokeStyle_slot] = (void *)factory_CreateStrokeStyle;
	factory_table[ID2D1Factory7_CreateDrawingStateBlock_slot] = (void *)factory_CreateDrawingStateBlock;
	factory_table[ID2D1Factory7_CreateWicBitmapRenderTarget_slot] = (void *)factory_CreateWicBitmapRenderTarget;
	factory_table[ID2D1Factory7_CreateHwndRenderTarget_slot] = (void *)factory_CreateHwndRenderTarget;
	factory_table[ID2D1Factory7_CreateDxgiSurfaceRenderTarget_slot] = (void *)factory_CreateDxgiSurfaceRenderTarget;
	factory_table[ID2D1Factory7_CreateDCRenderTarget_slot] = (void *)factory_CreateDCRenderTarget;
	factory_table[ID2D1Factory7_CreateDevice_slot] = (void *)factory_CreateDevice;
	factory_table[ID2D1Factory7_CreateStrokeStyle_18_slot] = (void *)factory_CreateStrokeStyle1;
	factory_table[ID2D1Factory7_CreatePathGeometry_19_slot] = (void *)factory_CreatePathGeometry;
	factory_table[ID2D1Factory7_CreateDrawingStateBlock_20_slot] = (void *)factory_CreateDrawingStateBlock1;
	factory_table[ID2D1Factory7_RegisterEffectFromStream_slot] = (void *)factory_RegisterEffectFromStream;
	factory_table[ID2D1Factory7_RegisterEffectFromString_slot] = (void *)factory_RegisterEffectFromString;
	factory_table[ID2D1Factory7_UnregisterEffect_slot] = (void *)factory_UnregisterEffect;
	factory_table[ID2D1Factory7_GetRegisteredEffects_slot] = (void *)factory_GetRegisteredEffects;
	factory_table[ID2D1Factory7_CreateDevice_27_slot] = (void *)factory_CreateDevice;
	factory_table[ID2D1Factory7_CreateDevice_28_slot] = (void *)factory_CreateDevice;
	factory_table[ID2D1Factory7_CreateDevice_29_slot] = (void *)factory_CreateDevice;
	factory_table[ID2D1Factory7_CreateDevice_30_slot] = (void *)factory_CreateDevice;
	factory_table[ID2D1Factory7_CreateDevice_31_slot] = (void *)factory_CreateDevice;
	factory_table[ID2D1Factory7_CreateDevice_32_slot] = (void *)factory_CreateDevice;
	multithread_table = make_table(ID2D1Multithread_table, ID2D1Multithread_methods);
	multithread_table[0] = (void *)multithread_QueryInterface;
	multithread_table[1] = (void *)factory_AddRef;
	multithread_table[2] = (void *)factory_Release;
	multithread_table[ID2D1Multithread_GetMultithreadProtected_slot] = (void *)multithread_GetMultithreadProtected;
	multithread_table[ID2D1Multithread_Enter_slot] = (void *)multithread_Enter;
	multithread_table[ID2D1Multithread_Leave_slot] = (void *)multithread_Leave;
}

/* ---- the exports ---- */
static INIT_ONCE tables_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tables_ready(INIT_ONCE *once, void *parameter, void **context)
{
	build_tables();
	return TRUE;
}

HRESULT WINAPI D2D1CreateFactory(D2D1_FACTORY_TYPE type, REFIID riid, const D2D1_FACTORY_OPTIONS *options, void **out)
{
	struct factory *factory;
	HRESULT hr;
	if (!out)
		return E_POINTER;
	*out = NULL;
	InitOnceExecuteOnce(&tables_once, tables_ready, NULL, NULL);
	if (!(factory = ALLOC(sizeof(*factory))))
		return E_OUTOFMEMORY;
	factory->vtbl = factory_table;
	factory->multithread = multithread_table;
	factory->refs = 1;
	factory->type = type;
	InitializeCriticalSection(&factory->lock);
	hr = factory_QueryInterface(factory, riid, out);
	factory_Release(factory);
	return hr;
}

HRESULT WINAPI D2D1CreateDevice(IDXGIDevice *dxgi, const D2D1_CREATION_PROPERTIES *properties, ID2D1Device **out)
{
	ID2D1Factory1 *factory = NULL;
	HRESULT hr = D2D1CreateFactory(properties && properties->threadingMode == D2D1_THREADING_MODE_MULTI_THREADED ? D2D1_FACTORY_TYPE_MULTI_THREADED :
		D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory1, NULL, (void **)&factory);
	if (FAILED(hr))
		return hr;
	hr = make_device((IUnknown *)factory, (IUnknown *)dxgi, out);
	factory_Release(factory);
	return hr;
}

HRESULT WINAPI D2D1CreateDeviceContext(IDXGISurface *surface, const D2D1_CREATION_PROPERTIES *properties, ID2D1DeviceContext **out)
{
	ID2D1Device *device = NULL;
	struct bitmap *bitmap = NULL;
	struct target *context;
	HRESULT hr = D2D1CreateDevice(NULL, properties, &device);
	if (FAILED(hr))
		return hr;
	hr = device_CreateDeviceContext(device, D2D1_DEVICE_CONTEXT_OPTIONS_NONE, out);
	object_Release(device);
	if (FAILED(hr))
		return hr;
	context = target_of(*out);
	if (SUCCEEDED(bitmap_from_dxgi(context, surface, NULL, &bitmap))) {
		context_SetTarget(context, (ID2D1Image *)bitmap);
		object_Release(bitmap);
	}
	return S_OK;
}

void WINAPI D2D1MakeRotateMatrix(FLOAT angle, D2D1_POINT_2F center, D2D1_MATRIX_3X2_F *matrix)
{
	float s, c;
	sincos_(angle * PI / 180, &s, &c);
	matrix->_11 = c;
	matrix->_12 = s;
	matrix->_21 = -s;
	matrix->_22 = c;
	matrix->_31 = center.x - center.x * c + center.y * s;
	matrix->_32 = center.y - center.x * s - center.y * c;
}

void WINAPI D2D1MakeSkewMatrix(FLOAT angle_x, FLOAT angle_y, D2D1_POINT_2F center, D2D1_MATRIX_3X2_F *matrix)
{
	float sx, cx, sy, cy;
	sincos_(angle_x * PI / 180, &sx, &cx);
	sincos_(angle_y * PI / 180, &sy, &cy);
	matrix->_11 = 1;
	matrix->_12 = cy != 0 ? sy / cy : 0;
	matrix->_21 = cx != 0 ? sx / cx : 0;
	matrix->_22 = 1;
	matrix->_31 = -center.y * matrix->_21;
	matrix->_32 = -center.x * matrix->_12;
}

BOOL WINAPI D2D1IsMatrixInvertible(const D2D1_MATRIX_3X2_F *matrix)
{
	return matrix->_11 * matrix->_22 - matrix->_12 * matrix->_21 != 0;
}

BOOL WINAPI D2D1InvertMatrix(D2D1_MATRIX_3X2_F *matrix)
{
	D2D1_MATRIX_3X2_F inverse;
	if (!invert(matrix, &inverse))
		return FALSE;
	*matrix = inverse;
	return TRUE;
}

void WINAPI D2D1SinCos(FLOAT angle, FLOAT *s, FLOAT *c)
{
	sincos_(angle, s, c);
}

FLOAT WINAPI D2D1Tan(FLOAT angle)
{
	float s, c;
	sincos_(angle, &s, &c);
	return c != 0 ? s / c : 0;
}

FLOAT WINAPI D2D1Vec3Length(FLOAT x, FLOAT y, FLOAT z)
{
	return fsqrt(x * x + y * y + z * z);
}

/* The natural logarithm from the exponent and the mantissa of the float, and e^x from a power of two and a short series: enough for
 * color curves. */
static float flog(float x)
{
	union { float f; UINT32 bits; } value = {x};
	const int exponent = (int)((value.bits >> 23) & 0xff) - 127;
	float m, t, t2;
	value.bits = (value.bits & 0x7fffff) | 0x3f800000;
	m = value.f;
	/* log(m) for m in [1, 2) through atanh: log(m) = 2 atanh((m - 1) / (m + 1)). */
	t = (m - 1) / (m + 1);
	t2 = t * t;
	return 2 * t * (1 + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 / 9)))) + exponent * 0.6931471806f;
}

static float fexp(float x)
{
	union { float f; UINT32 bits; } scale;
	int k;
	float r, sum;
	if (x < -87) return 0;
	if (x > 88) x = 88;
	k = (int)(x / 0.6931471806f + (x < 0 ? -0.5f : 0.5f));
	r = x - k * 0.6931471806f;
	sum = 1 + r * (1 + r / 2 * (1 + r / 3 * (1 + r / 4 * (1 + r / 5 * (1 + r / 6)))));
	scale.bits = (UINT32)(k + 127) << 23;
	return sum * scale.f;
}

static float fpow(float x, float y)
{
	return x <= 0 ? 0 : fexp(y * flog(x));
}

/* sRGB and scRGB, the two spaces of Direct2D: the standard curves both ways, alpha untouched. */
static float linear_to_srgb(float v)
{
	return v <= 0.0031308f ? v * 12.92f : 1.055f * fpow(v, 1 / 2.4f) - 0.055f;
}

static float srgb_to_linear(float v)
{
	return v <= 0.04045f ? v / 12.92f : fpow((v + 0.055f) / 1.055f, 2.4f);
}

D2D1_COLOR_F WINAPI D2D1ConvertColorSpace(D2D1_COLOR_SPACE source, D2D1_COLOR_SPACE destination, const D2D1_COLOR_F *color)
{
	D2D1_COLOR_F result = *color;
	if (source == D2D1_COLOR_SPACE_SRGB && destination == D2D1_COLOR_SPACE_SCRGB) {
		result.r = srgb_to_linear(color->r);
		result.g = srgb_to_linear(color->g);
		result.b = srgb_to_linear(color->b);
	} else if (source == D2D1_COLOR_SPACE_SCRGB && destination == D2D1_COLOR_SPACE_SRGB) {
		result.r = linear_to_srgb(color->r);
		result.g = linear_to_srgb(color->g);
		result.b = linear_to_srgb(color->b);
	}
	return result;
}

FLOAT WINAPI D2D1ComputeMaximumScaleFactor(const D2D1_MATRIX_3X2_F *matrix)
{
	/* The larger singular value of the 2x2 part. */
	const float a = matrix->_11, b = matrix->_12, c = matrix->_21, d = matrix->_22;
	const float s1 = a * a + b * b + c * c + d * d, s2 = fsqrt((a * a + b * b - c * c - d * d) * (a * a + b * b - c * c - d * d) +
		4 * (a * c + b * d) * (a * c + b * d));
	return fsqrt((s1 + s2) / 2);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);
	return TRUE;
}
