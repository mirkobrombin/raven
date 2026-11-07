/* SPDX-License-Identifier: MIT */
#include "brush.h"
#include "graphics.h"
#include "gmath.h"

extern GpStatus GDIPAPI GdipMultiplyMatrix(GpMatrix *, const GpMatrix *, GpMatrixOrder);
extern GpStatus GDIPAPI GdipTranslateMatrix(GpMatrix *, REAL, REAL, GpMatrixOrder);
extern GpStatus GDIPAPI GdipScaleMatrix(GpMatrix *, REAL, REAL, GpMatrixOrder);
extern GpStatus GDIPAPI GdipRotateMatrix(GpMatrix *, REAL, GpMatrixOrder);
extern GpStatus GDIPAPI GdipAddPathArc(GpPath *, REAL, REAL, REAL, REAL, REAL, REAL);
extern GpStatus GDIPAPI GdipAddPathBeziers(GpPath *, const GpPointF *, INT);
extern GpStatus GDIPAPI GdipAddPathCurve2(GpPath *, const GpPointF *, INT, REAL);
extern GpStatus GDIPAPI GdipAddPathClosedCurve2(GpPath *, const GpPointF *, INT, REAL);
extern GpStatus GDIPAPI GdipAddPathEllipse(GpPath *, REAL, REAL, REAL, REAL);
extern GpStatus GDIPAPI GdipAddPathLine2(GpPath *, const GpPointF *, INT);
extern GpStatus GDIPAPI GdipAddPathPie(GpPath *, REAL, REAL, REAL, REAL, REAL, REAL);
extern GpStatus GDIPAPI GdipAddPathPolygon(GpPath *, const GpPointF *, INT);
extern GpStatus GDIPAPI GdipAddPathRectangle(GpPath *, REAL, REAL, REAL, REAL);
extern GpStatus GDIPAPI GdipClosePathFigure(GpPath *);

#define SUBSAMPLES 4

enum { SmoothingNone = 3, SmoothingAntiAlias8x4 = 4, SmoothingAntiAlias8x8 = 5, SmoothingHighQuality = 2 };
enum { CompositingSourceOver, CompositingSourceCopy };
enum { CoordinateWorld, CoordinatePage, CoordinateDevice };
enum { PixelOffsetHighQuality = 2, PixelOffsetHalf = 4 };
enum { InterpolationDefault, InterpolationLow, InterpolationHigh, InterpolationNearest = 5, InterpolationBilinear = 3, InterpolationBicubic = 4, InterpolationHighBilinear = 6, InterpolationHighBicubic = 7 };

static GpGraphics *new_graphics(void)
{
	GpGraphics *graphics = gp_alloc(sizeof(GpGraphics));
	if (!graphics)
		return NULL;
	if (GdipCreateRegion(&graphics->clip) != Ok) {
		gp_free(graphics);
		return NULL;
	}
	graphics->unit = UnitDisplay;
	graphics->scale = 1.0f;
	gp_matrix_identity(&graphics->world);
	graphics->smoothing = 1;
	graphics->text_contrast = 4;
	graphics->interpolation = InterpolationBilinear;
	graphics->compositing_quality = 1;
	graphics->pixel_offset = 1;
	return graphics;
}

GpStatus GDIPAPI GdipCreateFromHDC2(HDC dc, HANDLE device, GpGraphics **out)
{
	if (!out)
		return InvalidParameter;
	if (!dc || device)
		return device ? NotImplemented : OutOfMemory;
	if (GetDeviceCaps(dc, TECHNOLOGY) == DT_PLOTTER)
		return GenericError;
	GpGraphics *graphics = new_graphics();
	if (!graphics)
		return OutOfMemory;
	graphics->dc = dc;
	graphics->window = WindowFromDC(dc);
	graphics->dpi_x = (REAL)GetDeviceCaps(dc, LOGPIXELSX);
	graphics->dpi_y = (REAL)GetDeviceCaps(dc, LOGPIXELSY);
	if (graphics->dpi_x <= 0.0f)
		graphics->dpi_x = 96.0f;
	if (graphics->dpi_y <= 0.0f)
		graphics->dpi_y = 96.0f;
	*out = graphics;
	return Ok;
}

GpStatus GDIPAPI GdipCreateFromHDC(HDC dc, GpGraphics **out)
{
	return GdipCreateFromHDC2(dc, NULL, out);
}

GpStatus GDIPAPI GdipCreateFromHWND(HWND window, GpGraphics **out)
{
	if (!out)
		return InvalidParameter;
	HDC dc = GetDC(window);
	if (!dc)
		return OutOfMemory;
	GpStatus status = GdipCreateFromHDC(dc, out);
	if (status != Ok) {
		ReleaseDC(window, dc);
		return status;
	}
	(*out)->window = window;
	(*out)->own_dc = TRUE;
	return Ok;
}

GpStatus GDIPAPI GdipCreateFromHWNDICM(HWND window, GpGraphics **out)
{
	return GdipCreateFromHWND(window, out);
}

GpStatus GDIPAPI GdipGetImageGraphicsContext(GpImage *image, GpGraphics **out)
{
	if (!image || !out)
		return InvalidParameter;
	if (image->type != ImageTypeBitmap)
		return NotImplemented;
	GpGraphics *graphics = new_graphics();
	if (!graphics)
		return OutOfMemory;
	graphics->image = image;
	graphics->dpi_x = image->dpi_x;
	graphics->dpi_y = image->dpi_y;
	graphics->width = image->width;
	graphics->height = image->height;
	*out = graphics;
	return Ok;
}

GpStatus GDIPAPI GdipDeleteGraphics(GpGraphics *graphics)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (graphics->own_dc)
		ReleaseDC(graphics->window, graphics->dc);
	while (graphics->states) {
		gp_state *state = graphics->states;
		graphics->states = state->next;
		gp_region_delete(state->clip);
		gp_free(state);
	}
	gp_region_delete(graphics->clip);
	gp_free(graphics);
	return Ok;
}

#define GRAPHICS_FIELD(get_name, set_name, type, field) \
	GpStatus GDIPAPI get_name(GpGraphics *graphics, type *out) \
	{ \
		if (!graphics || !out) \
			return InvalidParameter; \
		if (graphics->held_dc) \
			return ObjectBusy; \
		*out = graphics->field; \
		return Ok; \
	} \
	GpStatus GDIPAPI set_name(GpGraphics *graphics, type value) \
	{ \
		if (!graphics) \
			return InvalidParameter; \
		if (graphics->held_dc) \
			return ObjectBusy; \
		graphics->field = value; \
		return Ok; \
	}

GRAPHICS_FIELD(GdipGetCompositingMode, GdipSetCompositingMode, INT, compositing_mode)
GRAPHICS_FIELD(GdipGetCompositingQuality, GdipSetCompositingQuality, INT, compositing_quality)
GRAPHICS_FIELD(GdipGetPixelOffsetMode, GdipSetPixelOffsetMode, INT, pixel_offset)
GRAPHICS_FIELD(GdipGetSmoothingMode, GdipSetSmoothingMode, INT, smoothing)
GRAPHICS_FIELD(GdipGetTextContrast, GdipSetTextContrast, UINT, text_contrast)

GpStatus GDIPAPI GdipGetTextRenderingHint(GpGraphics *graphics, INT *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	*out = graphics->text_hint;
	return Ok;
}

GpStatus GDIPAPI GdipSetTextRenderingHint(GpGraphics *graphics, INT value)
{
	if (!graphics || (UINT)value > 5)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	graphics->text_hint = value;
	return Ok;
}

GpStatus GDIPAPI GdipGetInterpolationMode(GpGraphics *graphics, INT *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	*out = graphics->interpolation;
	return Ok;
}

// The two quality names stand for a concrete mode, and reading back gives that mode.
GpStatus GDIPAPI GdipSetInterpolationMode(GpGraphics *graphics, INT value)
{
	if (!graphics || (UINT)value > InterpolationHighBicubic)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (value == InterpolationDefault || value == InterpolationLow)
		value = InterpolationBilinear;
	else if (value == InterpolationHigh)
		value = InterpolationHighBicubic;
	graphics->interpolation = value;
	return Ok;
}

GpStatus GDIPAPI GdipGetDpiX(GpGraphics *graphics, REAL *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	*out = graphics->dpi_x;
	return Ok;
}

GpStatus GDIPAPI GdipGetDpiY(GpGraphics *graphics, REAL *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	*out = graphics->dpi_y;
	return Ok;
}

GpStatus GDIPAPI GdipGetPageUnit(GpGraphics *graphics, GpUnit *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	*out = graphics->unit;
	return Ok;
}

GpStatus GDIPAPI GdipSetPageUnit(GpGraphics *graphics, GpUnit unit)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (unit == UnitWorld || (UINT)unit > UnitMillimeter)
		return InvalidParameter;
	graphics->unit = unit;
	return Ok;
}

GpStatus GDIPAPI GdipGetPageScale(GpGraphics *graphics, REAL *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	*out = graphics->scale;
	return Ok;
}

GpStatus GDIPAPI GdipSetPageScale(GpGraphics *graphics, REAL scale)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (scale <= 0.0f)
		return InvalidParameter;
	graphics->scale = scale;
	return Ok;
}

GpStatus GDIPAPI GdipResetPageTransform(GpGraphics *graphics)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	graphics->unit = UnitDisplay;
	graphics->scale = 1.0f;
	return Ok;
}

GpStatus GDIPAPI GdipGetRenderingOrigin(GpGraphics *graphics, INT *x, INT *y)
{
	if (!graphics || !x || !y)
		return InvalidParameter;
	*x = graphics->origin_x;
	*y = graphics->origin_y;
	return Ok;
}

GpStatus GDIPAPI GdipSetRenderingOrigin(GpGraphics *graphics, INT x, INT y)
{
	if (!graphics)
		return InvalidParameter;
	graphics->origin_x = x;
	graphics->origin_y = y;
	return Ok;
}

GpStatus GDIPAPI GdipGetWorldTransform(GpGraphics *graphics, GpMatrix *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	*out = graphics->world;
	return Ok;
}

GpStatus GDIPAPI GdipSetWorldTransform(GpGraphics *graphics, GpMatrix *matrix)
{
	if (!graphics || !matrix)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	graphics->world = *matrix;
	return Ok;
}

GpStatus GDIPAPI GdipResetWorldTransform(GpGraphics *graphics)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	gp_matrix_identity(&graphics->world);
	return Ok;
}

GpStatus GDIPAPI GdipMultiplyWorldTransform(GpGraphics *graphics, const GpMatrix *matrix, GpMatrixOrder order)
{
	if (!graphics || !matrix)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	return GdipMultiplyMatrix(&graphics->world, matrix, order);
}

GpStatus GDIPAPI GdipTranslateWorldTransform(GpGraphics *graphics, REAL x, REAL y, GpMatrixOrder order)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	return GdipTranslateMatrix(&graphics->world, x, y, order);
}

GpStatus GDIPAPI GdipScaleWorldTransform(GpGraphics *graphics, REAL x, REAL y, GpMatrixOrder order)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	return GdipScaleMatrix(&graphics->world, x, y, order);
}

GpStatus GDIPAPI GdipRotateWorldTransform(GpGraphics *graphics, REAL angle, GpMatrixOrder order)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	return GdipRotateMatrix(&graphics->world, angle, order);
}

// Page units to device pixels. Display and pixel units are the device's own unit on a screen.
REAL gp_unit_scale(const GpGraphics *graphics, GpUnit unit, REAL dpi)
{
	(void)graphics;
	switch (unit) {
	case UnitPoint:
		return dpi / 72.0f;
	case UnitInch:
		return dpi;
	case UnitDocument:
		return dpi / 300.0f;
	case UnitMillimeter:
		return dpi / 25.4f;
	default:
		return 1.0f;
	}
}

static void world_to_public_device(const GpGraphics *graphics, GpMatrix *out)
{
	// The display unit is the device itself, which a page scale does not reach.
	REAL page_scale = graphics->unit == UnitDisplay ? 1.0f : graphics->scale;
	REAL sx = gp_unit_scale(graphics, graphics->unit, graphics->dpi_x) * page_scale;
	REAL sy = gp_unit_scale(graphics, graphics->unit, graphics->dpi_y) * page_scale;
	GpMatrix page = {{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f}};
	gp_matrix_mul(out, &graphics->world, &page);
}

// Where the context's logical origin lands on its pixels; a program can move it at any time.
static POINT dc_origin(const GpGraphics *graphics)
{
	POINT origin = {0, 0};
	if (graphics->dc)
		LPtoDP(graphics->dc, &origin, 1);
	return origin;
}

// Drawing and clipping work on the context's pixels, past the origins set on it. The device space programs see through
// GdipTransformPoints stops before them.
void gp_world_to_device(const GpGraphics *graphics, GpMatrix *out)
{
	world_to_public_device(graphics, out);
	POINT origin = dc_origin(graphics);
	out->m[4] += (REAL)origin.x;
	out->m[5] += (REAL)origin.y;
}

static BOOL space_matrix(const GpGraphics *graphics, INT from, INT to, GpMatrix *out)
{
	GpMatrix world_to_device, world_to_page, step;
	world_to_public_device(graphics, &world_to_device);
	world_to_page = graphics->world;
	gp_matrix_identity(out);
	if (from == to)
		return TRUE;
	// Bring the source to device space, then down to the target.
	if (from == CoordinateWorld)
		*out = world_to_device;
	else if (from == CoordinatePage) {
		GpMatrix inverse;
		if (!gp_matrix_invert(&world_to_page, &inverse))
			return FALSE;
		gp_matrix_mul(out, &inverse, &world_to_device);
	}
	if (to == CoordinateDevice)
		return TRUE;
	if (!gp_matrix_invert(to == CoordinateWorld ? &world_to_device : &world_to_page, &step))
		return FALSE;
	if (to == CoordinatePage) {
		GpMatrix device_to_world;
		if (!gp_matrix_invert(&world_to_device, &device_to_world))
			return FALSE;
		gp_matrix_mul(&step, &device_to_world, &world_to_page);
	}
	gp_matrix_mul(out, out, &step);
	return TRUE;
}

GpStatus GDIPAPI GdipTransformPoints(GpGraphics *graphics, INT to, INT from, GpPointF *points, INT count)
{
	if (!graphics || !points || count <= 0)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if ((UINT)to > CoordinateDevice || (UINT)from > CoordinateDevice)
		return InvalidParameter;
	GpMatrix matrix;
	if (!space_matrix(graphics, from, to, &matrix))
		return InvalidParameter;
	gp_matrix_apply(&matrix, points, count);
	return Ok;
}

GpStatus GDIPAPI GdipTransformPointsI(GpGraphics *graphics, INT to, INT from, GpPoint *points, INT count)
{
	if (!points || count <= 0)
		return InvalidParameter;
	GpPointF *floats = gp_alloc(count * sizeof(GpPointF));
	if (!floats)
		return OutOfMemory;
	for (INT i = 0; i < count; i++) {
		floats[i].X = (REAL)points[i].X;
		floats[i].Y = (REAL)points[i].Y;
	}
	GpStatus status = GdipTransformPoints(graphics, to, from, floats, count);
	if (status == Ok) {
		for (INT i = 0; i < count; i++) {
			points[i].X = (INT)gp_floor(floats[i].X + 0.5f);
			points[i].Y = (INT)gp_floor(floats[i].Y + 0.5f);
		}
	}
	gp_free(floats);
	return status;
}

static void surface_size(const GpGraphics *graphics, INT *width, INT *height)
{
	if (graphics->image) {
		*width = graphics->image->width;
		*height = graphics->image->height;
		return;
	}
	RECT box = {0, 0, 0, 0};
	if (graphics->window && GetClientRect(graphics->window, &box)) {
		*width = box.right;
		*height = box.bottom;
		return;
	}
	if (GetClipBox(graphics->dc, &box) != ERROR) {
		POINT origin = dc_origin(graphics);
		if (box.right + origin.x > 0 && box.bottom + origin.y > 0) {
			*width = box.right + origin.x;
			*height = box.bottom + origin.y;
			return;
		}
	}
	*width = GetDeviceCaps(graphics->dc, HORZRES);
	*height = GetDeviceCaps(graphics->dc, VERTRES);
}

BOOL gp_clip_bounds(const GpGraphics *graphics, RECT *out)
{
	INT width, height;
	surface_size(graphics, &width, &height);
	RECT box = {0, 0, width, height};
	if (!gp_region_is_infinite(graphics->clip)) {
		GpRectF bounds;
		if (!gp_region_bounds(graphics->clip, &bounds))
			return FALSE;
		POINT origin = dc_origin(graphics);
		RECT clip = {(LONG)gp_floor(bounds.X) + origin.x, (LONG)gp_floor(bounds.Y) + origin.y, (LONG)gp_ceil(bounds.X + bounds.Width) + origin.x, (LONG)gp_ceil(bounds.Y + bounds.Height) + origin.y};
		if (!IntersectRect(&box, &box, &clip))
			return FALSE;
	}
	*out = box;
	return box.right > box.left && box.bottom > box.top;
}

BYTE *gp_clip_mask(const GpGraphics *graphics, const RECT *area)
{
	if (gp_region_is_infinite(graphics->clip))
		return NULL;
	INT width = area->right - area->left, height = area->bottom - area->top;
	BYTE *mask = gp_alloc((SIZE_T)width * height);
	if (!mask)
		return NULL;
	POINT origin = dc_origin(graphics);
	for (INT y = 0; y < height; y++) {
		for (INT x = 0; x < width; x++)
			mask[(SIZE_T)y * width + x] = gp_region_contains(graphics->clip, area->left - origin.x + x + 0.5f, area->top - origin.y + y + 0.5f) ? 255 : 0;
	}
	return mask;
}

// Direct 32 bit access when the target allows it; otherwise pixels go through a temporary section copied back at the end.
GpStatus gp_surface_begin(GpGraphics *graphics, gp_surface *surface, const RECT *area)
{
	memset(surface, 0, sizeof(*surface));
	surface->graphics = graphics;
	surface->area = *area;
	if (graphics->image) {
		GpImage *image = graphics->image;
		surface->image = image;
		surface->width = image->width;
		surface->height = image->height;
		if (image->format == PixelFormat32bppARGB || image->format == PixelFormat32bppPARGB || image->format == PixelFormat32bppRGB) {
			surface->pixels = (DWORD *)image->bits;
			surface->stride = image->stride;
			surface->premultiplied = image->format == PixelFormat32bppPARGB;
			surface->opaque = image->format == PixelFormat32bppRGB;
		}
		return Ok;
	}
	INT width = area->right - area->left, height = area->bottom - area->top;
	BITMAPINFO header = {{sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB}};
	void *bits;
	surface->temp_dc = CreateCompatibleDC(graphics->dc);
	surface->temp_bitmap = CreateDIBSection(graphics->dc, &header, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!surface->temp_dc || !surface->temp_bitmap) {
		if (surface->temp_dc)
			DeleteDC(surface->temp_dc);
		if (surface->temp_bitmap)
			DeleteObject(surface->temp_bitmap);
		return OutOfMemory;
	}
	surface->temp_old = SelectObject(surface->temp_dc, surface->temp_bitmap);
	POINT origin = dc_origin(graphics);
	surface->logical_x = area->left - origin.x;
	surface->logical_y = area->top - origin.y;
	BitBlt(surface->temp_dc, 0, 0, width, height, graphics->dc, surface->logical_x, surface->logical_y, SRCCOPY);
	surface->pixels = bits;
	surface->stride = width * 4;
	surface->width = width;
	surface->height = height;
	surface->dc_x = area->left;
	surface->dc_y = area->top;
	surface->opaque = TRUE;
	return Ok;
}

void gp_surface_end(gp_surface *surface)
{
	if (!surface->temp_dc)
		return;
	GdiFlush();
	BitBlt(surface->graphics->dc, surface->logical_x, surface->logical_y, surface->width, surface->height, surface->temp_dc, 0, 0, SRCCOPY);
	SelectObject(surface->temp_dc, surface->temp_old);
	DeleteObject(surface->temp_bitmap);
	DeleteDC(surface->temp_dc);
}

// Source-over onto a straight or premultiplied destination; a full source replaces outright.
void gp_surface_blend(gp_surface *surface, INT x, INT y, ARGB color, BYTE coverage)
{
	INT px = x - surface->dc_x, py = y - surface->dc_y;
	if (px < 0 || py < 0 || px >= surface->width || py >= surface->height || !coverage)
		return;
	UINT sa = color >> 24;
	if (coverage != 255)
		sa = (sa * coverage + 127) / 255;
	BOOL copy = surface->graphics->compositing_mode == CompositingSourceCopy;
	ARGB under;
	if (surface->pixels)
		under = *(DWORD *)((BYTE *)surface->pixels + (SSIZE_T)py * surface->stride + px * 4);
	else
		under = gp_pixel_read(surface->image, px, py);
	if (surface->pixels && surface->premultiplied) {
		UINT a = under >> 24;
		if (a && a != 255) {
			UINT r = ((under >> 16) & 255) * 255 / a, g = ((under >> 8) & 255) * 255 / a, b = (under & 255) * 255 / a;
			under = a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
		}
	}
	if (surface->opaque)
		under |= 0xff000000;
	ARGB out;
	if (copy || sa == 255) {
		out = sa ? (color & 0xffffff) | sa << 24 : 0;
	} else if (!sa) {
		return;
	} else {
		UINT da = under >> 24, ia = 255 - sa;
		UINT oa = sa + (da * ia + 127) / 255;
		if (!oa) {
			out = 0;
		} else {
			UINT dw = (da * ia + 127) / 255;
			UINT r = (((color >> 16) & 255) * sa + ((under >> 16) & 255) * dw + oa / 2) / oa;
			UINT g = (((color >> 8) & 255) * sa + ((under >> 8) & 255) * dw + oa / 2) / oa;
			UINT b = ((color & 255) * sa + (under & 255) * dw + oa / 2) / oa;
			out = oa << 24 | r << 16 | g << 8 | b;
		}
	}
	if (surface->pixels) {
		if (surface->premultiplied && out >> 24 != 255) {
			UINT a = out >> 24;
			out = a << 24 | (((out >> 16) & 255) * a + 127) / 255 << 16 | (((out >> 8) & 255) * a + 127) / 255 << 8 | ((out & 255) * a + 127) / 255;
		}
		if (surface->opaque)
			out |= 0xff000000;
		*(DWORD *)((BYTE *)surface->pixels + (SSIZE_T)py * surface->stride + px * 4) = out;
	} else {
		gp_pixel_write(surface->image, px, py, out);
	}
}

typedef struct
{
	REAL x;
	INT direction;
} crossing;

static int compare_crossings(const void *a, const void *b)
{
	REAL x = ((const crossing *)a)->x, y = ((const crossing *)b)->x;
	return x < y ? -1 : x > y;
}

// Fills flattened figures, in device space, with the brush. Rows are sampled SUBSAMPLES times when smoothing and once at
// the pixel centre otherwise; a coverage row adds up the horizontal spans of every sample.
static GpStatus fill_device_path(GpGraphics *graphics, const GpPath *path, const GpBrush *brush, GpFillMode fill)
{
	extern void qsort(void *, size_t, size_t, int (__cdecl *)(const void *, const void *));
	if (!path->count)
		return Ok;
	GpRectF box;
	gp_path_bounds(path, &box);
	RECT limit, area = {(LONG)gp_floor(box.X) - 1, (LONG)gp_floor(box.Y) - 1, (LONG)gp_ceil(box.X + box.Width) + 1, (LONG)gp_ceil(box.Y + box.Height) + 1};
	if (!gp_clip_bounds(graphics, &limit) || !IntersectRect(&area, &area, &limit))
		return Ok;
	gp_surface surface;
	GpStatus status = gp_surface_begin(graphics, &surface, &area);
	if (status != Ok)
		return status;
	BYTE *mask = gp_clip_mask(graphics, &area);
	INT width = area.right - area.left;
	BOOL smooth = graphics->smoothing == SmoothingAntiAlias8x4 || graphics->smoothing == SmoothingAntiAlias8x8 || graphics->smoothing == SmoothingHighQuality;
	INT samples = smooth ? SUBSAMPLES : 1;
	UINT *row = gp_alloc((width + 1) * sizeof(UINT));
	crossing *crossings = gp_alloc((path->count + 2) * sizeof(crossing));
	GpMatrix to_device, inverse;
	gp_world_to_device(graphics, &to_device);
	BOOL has_inverse = gp_matrix_invert(&to_device, &inverse);
	if (!row || !crossings) {
		status = OutOfMemory;
		goto done;
	}
	REAL half = graphics->pixel_offset == 4 || graphics->pixel_offset == 2 ? 0.0f : 0.5f;
	for (INT y = area.top; y < area.bottom; y++) {
		memset(row, 0, (width + 1) * sizeof(UINT));
		BOOL touched = FALSE;
		for (INT s = 0; s < samples; s++) {
			REAL sy = y + (s + 0.5f) / samples - 0.5f + half;
			INT found = 0, start = 0;
			for (INT i = 0; i < path->count; i++) {
				BOOL ends = i + 1 == path->count || (path->types[i + 1] & PathPointTypePathTypeMask) == PathPointTypeStart;
				const GpPointF *a = &path->points[i], *b = ends ? &path->points[start] : &path->points[i + 1];
				if ((a->Y <= sy) != (b->Y <= sy)) {
					crossings[found].x = a->X + (sy - a->Y) / (b->Y - a->Y) * (b->X - a->X);
					crossings[found++].direction = a->Y <= sy ? 1 : -1;
				}
				if (ends)
					start = i + 1;
			}
			if (found < 2)
				continue;
			qsort(crossings, found, sizeof(crossing), (int (__cdecl *)(const void *, const void *))compare_crossings);
			INT winding = 0;
			for (INT i = 0; i + 1 < found; i++) {
				winding += crossings[i].direction;
				BOOL inside = fill == FillModeWinding ? winding != 0 : (i & 1) == 0;
				if (!inside)
					continue;
				REAL from = crossings[i].x - area.left, to = crossings[i + 1].x - area.left;
				if (to <= 0.0f || from >= width)
					continue;
				if (from < 0.0f) from = 0.0f;
				if (to > width) to = (REAL)width;
				if (smooth) {
					// Exact horizontal coverage, scaled so one sample's full pixel adds 256.
					INT left = (INT)from, right = (INT)to;
					if (left == right) {
						row[left] += (UINT)((to - from) * 256.0f);
					} else {
						row[left] += (UINT)(((REAL)(left + 1) - from) * 256.0f);
						for (INT p = left + 1; p < right; p++)
							row[p] += 256;
						if (right < width)
							row[right] += (UINT)((to - (REAL)right) * 256.0f);
					}
				} else {
					// A pixel counts when its centre lies in the span.
					INT left = (INT)gp_ceil(from - 0.5f + 1e-3f - (half ? 0.0f : 0.5f) + 0.5f - 0.5f), right = (INT)gp_ceil(to - 0.5f + 1e-3f - (half ? 0.0f : 0.5f));
					if (half) {
						left = (INT)gp_ceil(from - 0.5f);
						right = (INT)gp_ceil(to - 0.5f);
					}
					if (left < 0)
						left = 0;
					for (INT p = left; p < right && p < width; p++)
						row[p] = 256;
				}
				touched = TRUE;
			}
		}
		if (!touched)
			continue;
		for (INT p = 0; p < width; p++) {
			if (!row[p])
				continue;
			UINT total = smooth ? row[p] / samples : row[p];
			BYTE coverage = total >= 256 ? 255 : (BYTE)total;
			if (mask && !mask[(SIZE_T)(y - area.top) * width + p])
				continue;
			ARGB color = brush->type == BrushTypeSolidColor ? brush->color
				: gp_brush_sample(brush, has_inverse ? &inverse : NULL, area.left + p + 0.5f, y + 0.5f, graphics->origin_x, graphics->origin_y);
			gp_surface_blend(&surface, area.left + p, y, color, coverage);
		}
	}
done:
	gp_free(row);
	gp_free(crossings);
	gp_free(mask);
	gp_surface_end(&surface);
	return status;
}

GpStatus GDIPAPI GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path)
{
	if (!graphics || !brush || !path)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (!path->count)
		return Ok;
	GpPath *copy = gp_path_clone(path);
	if (!copy)
		return OutOfMemory;
	GpMatrix to_device;
	gp_world_to_device(graphics, &to_device);
	GpStatus status = gp_path_flatten(copy, &to_device, 0.25f);
	if (status == Ok)
		status = fill_device_path(graphics, copy, brush, path->fill);
	gp_path_delete(copy);
	return status;
}

// The pen's width belongs to the page when its unit is world, otherwise to its own unit. A pen narrower than a pixel
// is drawn one pixel wide, which is what zero means.
static GpStatus stroke_device(GpGraphics *graphics, GpPen *pen, const GpPath *path)
{
	GpPath *copy = gp_path_clone(path);
	if (!copy)
		return OutOfMemory;
	GpMatrix to_device, pen_scale;
	gp_world_to_device(graphics, &to_device);
	GpStatus status = gp_path_flatten(copy, NULL, 0.25f);
	if (status != Ok) {
		gp_path_delete(copy);
		return status;
	}
	REAL scale_x, scale_y;
	if (pen->unit == UnitWorld) {
		scale_x = gp_sqrtf(to_device.m[0] * to_device.m[0] + to_device.m[1] * to_device.m[1]);
		scale_y = gp_sqrtf(to_device.m[2] * to_device.m[2] + to_device.m[3] * to_device.m[3]);
		pen_scale = to_device;
	} else {
		scale_x = gp_unit_scale(graphics, pen->unit, graphics->dpi_x);
		scale_y = gp_unit_scale(graphics, pen->unit, graphics->dpi_y);
		pen_scale = to_device;
	}
	gp_matrix_apply(&to_device, copy->points, copy->count);
	GpPen device_pen = *pen;
	REAL scale = (scale_x + scale_y) / 2.0f;
	device_pen.width = pen->width * scale;
	device_pen.unit = UnitPixel;
	if (device_pen.width < 1.0f)
		device_pen.width = 1.0f;
	(void)pen_scale;
	status = gp_path_widen(copy, &device_pen, NULL, 0.25f);
	if (status == Ok)
		status = gp_path_flatten(copy, NULL, 0.25f);
	if (status == Ok)
		status = fill_device_path(graphics, copy, pen->brush, FillModeWinding);
	gp_path_delete(copy);
	return status;
}

GpStatus GDIPAPI GdipDrawPath(GpGraphics *graphics, GpPen *pen, GpPath *path)
{
	if (!graphics || !pen || !path)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (!path->count)
		return Ok;
	return stroke_device(graphics, pen, path);
}

typedef GpStatus (*path_builder)(GpPath *path, void *data);

// Most drawing calls are a throwaway path; these two wrap the common shape.
static GpStatus draw_built(GpGraphics *graphics, GpPen *pen, path_builder build, void *data)
{
	if (!graphics || !pen)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpPath *path;
	GpStatus status = gp_path_new(FillModeAlternate, &path);
	if (status != Ok)
		return status;
	status = build(path, data);
	if (status == Ok)
		status = GdipDrawPath(graphics, pen, path);
	gp_path_delete(path);
	return status;
}

static GpStatus fill_built(GpGraphics *graphics, GpBrush *brush, GpFillMode fill, path_builder build, void *data)
{
	if (!graphics || !brush)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpPath *path;
	GpStatus status = gp_path_new(fill, &path);
	if (status != Ok)
		return status;
	status = build(path, data);
	if (status == Ok)
		status = GdipFillPath(graphics, brush, path);
	gp_path_delete(path);
	return status;
}

typedef struct
{
	const GpPointF *points;
	INT count;
	REAL tension;
} point_args;

static GpPointF *float_points(const GpPoint *points, INT count)
{
	GpPointF *out = gp_alloc((count > 0 ? count : 1) * sizeof(GpPointF));
	if (out) {
		for (INT i = 0; i < count; i++) {
			out[i].X = (REAL)points[i].X;
			out[i].Y = (REAL)points[i].Y;
		}
	}
	return out;
}

static GpStatus build_lines(GpPath *path, void *data)
{
	point_args *args = data;
	return GdipAddPathLine2(path, args->points, args->count);
}

static GpStatus build_polygon(GpPath *path, void *data)
{
	point_args *args = data;
	return GdipAddPathPolygon(path, args->points, args->count);
}

static GpStatus build_beziers(GpPath *path, void *data)
{
	point_args *args = data;
	return GdipAddPathBeziers(path, args->points, args->count);
}

static GpStatus build_curve(GpPath *path, void *data)
{
	point_args *args = data;
	return GdipAddPathCurve2(path, args->points, args->count, args->tension);
}

static GpStatus build_closed_curve(GpPath *path, void *data)
{
	point_args *args = data;
	return GdipAddPathClosedCurve2(path, args->points, args->count, args->tension);
}

GpStatus GDIPAPI GdipDrawLines(GpGraphics *graphics, GpPen *pen, const GpPointF *points, INT count)
{
	if (!pen || !graphics || count <= 0)
		return InvalidParameter;
	if (!points)
		return OutOfMemory;
	if (count < 2)
		return InvalidParameter;
	point_args args = {points, count, 0.0f};
	return draw_built(graphics, pen, build_lines, &args);
}

GpStatus GDIPAPI GdipDrawLinesI(GpGraphics *graphics, GpPen *pen, const GpPoint *points, INT count)
{
	if (!pen || !graphics || count <= 0)
		return InvalidParameter;
	if (!points)
		return OutOfMemory;
	GpPointF *floats = float_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipDrawLines(graphics, pen, floats, count);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipDrawLine(GpGraphics *graphics, GpPen *pen, REAL x1, REAL y1, REAL x2, REAL y2)
{
	GpPointF points[2] = {{x1, y1}, {x2, y2}};
	return GdipDrawLines(graphics, pen, points, 2);
}

GpStatus GDIPAPI GdipDrawLineI(GpGraphics *graphics, GpPen *pen, INT x1, INT y1, INT x2, INT y2)
{
	return GdipDrawLine(graphics, pen, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2);
}

GpStatus GDIPAPI GdipDrawBeziers(GpGraphics *graphics, GpPen *pen, const GpPointF *points, INT count)
{
	if (!graphics || !pen || !points || count <= 0)
		return InvalidParameter;
	point_args args = {points, count, 0.0f};
	return draw_built(graphics, pen, build_beziers, &args);
}

GpStatus GDIPAPI GdipDrawBeziersI(GpGraphics *graphics, GpPen *pen, const GpPoint *points, INT count)
{
	if (!graphics || !pen || !points || count <= 0)
		return InvalidParameter;
	GpPointF *floats = float_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipDrawBeziers(graphics, pen, floats, count);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipDrawBezier(GpGraphics *graphics, GpPen *pen, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4)
{
	GpPointF points[4] = {{x1, y1}, {x2, y2}, {x3, y3}, {x4, y4}};
	return GdipDrawBeziers(graphics, pen, points, 4);
}

GpStatus GDIPAPI GdipDrawBezierI(GpGraphics *graphics, GpPen *pen, INT x1, INT y1, INT x2, INT y2, INT x3, INT y3, INT x4, INT y4)
{
	return GdipDrawBezier(graphics, pen, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2, (REAL)x3, (REAL)y3, (REAL)x4, (REAL)y4);
}

GpStatus GDIPAPI GdipDrawCurve2(GpGraphics *graphics, GpPen *pen, const GpPointF *points, INT count, REAL tension)
{
	if (!graphics || !pen || !points || count <= 1)
		return InvalidParameter;
	point_args args = {points, count, tension};
	return draw_built(graphics, pen, build_curve, &args);
}

GpStatus GDIPAPI GdipDrawCurve2I(GpGraphics *graphics, GpPen *pen, const GpPoint *points, INT count, REAL tension)
{
	if (count < 0)
		return OutOfMemory;
	if (!graphics || !pen || !points || count <= 1)
		return InvalidParameter;
	GpPointF *floats = float_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipDrawCurve2(graphics, pen, floats, count, tension);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipDrawCurve(GpGraphics *graphics, GpPen *pen, const GpPointF *points, INT count)
{
	return GdipDrawCurve2(graphics, pen, points, count, 0.5f);
}

GpStatus GDIPAPI GdipDrawCurveI(GpGraphics *graphics, GpPen *pen, const GpPoint *points, INT count)
{
	return GdipDrawCurve2I(graphics, pen, points, count, 0.5f);
}

GpStatus GDIPAPI GdipDrawCurve3(GpGraphics *graphics, GpPen *pen, const GpPointF *points, INT count, INT offset, INT segments, REAL tension)
{
	if (!graphics || !pen || !points || count <= 0 || offset < 0 || offset + segments >= count)
		return InvalidParameter;
	return GdipDrawCurve2(graphics, pen, points + offset, segments + 1, tension);
}

GpStatus GDIPAPI GdipDrawCurve3I(GpGraphics *graphics, GpPen *pen, const GpPoint *points, INT count, INT offset, INT segments, REAL tension)
{
	if (count < 0)
		return OutOfMemory;
	if (!graphics || !pen || !points || count <= 0 || offset < 0 || offset + segments >= count)
		return InvalidParameter;
	return GdipDrawCurve2I(graphics, pen, points + offset, segments + 1, tension);
}

GpStatus GDIPAPI GdipDrawClosedCurve2(GpGraphics *graphics, GpPen *pen, const GpPointF *points, INT count, REAL tension)
{
	if (!graphics || !pen || !points || count <= 0)
		return InvalidParameter;
	point_args args = {points, count, tension};
	return draw_built(graphics, pen, build_closed_curve, &args);
}

GpStatus GDIPAPI GdipDrawClosedCurve2I(GpGraphics *graphics, GpPen *pen, const GpPoint *points, INT count, REAL tension)
{
	if (!graphics || !pen || !points || count <= 0)
		return InvalidParameter;
	GpPointF *floats = float_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipDrawClosedCurve2(graphics, pen, floats, count, tension);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipDrawClosedCurve(GpGraphics *graphics, GpPen *pen, const GpPointF *points, INT count)
{
	return GdipDrawClosedCurve2(graphics, pen, points, count, 0.5f);
}

GpStatus GDIPAPI GdipDrawClosedCurveI(GpGraphics *graphics, GpPen *pen, const GpPoint *points, INT count)
{
	return GdipDrawClosedCurve2I(graphics, pen, points, count, 0.5f);
}

GpStatus GDIPAPI GdipDrawPolygon(GpGraphics *graphics, GpPen *pen, const GpPointF *points, INT count)
{
	if (!graphics || !pen || !points || count <= 0)
		return InvalidParameter;
	point_args args = {points, count, 0.0f};
	return draw_built(graphics, pen, build_polygon, &args);
}

GpStatus GDIPAPI GdipDrawPolygonI(GpGraphics *graphics, GpPen *pen, const GpPoint *points, INT count)
{
	if (!graphics || !pen || !points || count <= 0)
		return InvalidParameter;
	GpPointF *floats = float_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipDrawPolygon(graphics, pen, floats, count);
	gp_free(floats);
	return status;
}

typedef struct
{
	REAL x, y, width, height, start, sweep;
} shape_args;

static GpStatus build_arc(GpPath *path, void *data)
{
	shape_args *a = data;
	return GdipAddPathArc(path, a->x, a->y, a->width, a->height, a->start, a->sweep);
}

static GpStatus build_ellipse(GpPath *path, void *data)
{
	shape_args *a = data;
	return GdipAddPathEllipse(path, a->x, a->y, a->width, a->height);
}

static GpStatus build_pie(GpPath *path, void *data)
{
	shape_args *a = data;
	return GdipAddPathPie(path, a->x, a->y, a->width, a->height, a->start, a->sweep);
}

static GpStatus build_rect(GpPath *path, void *data)
{
	shape_args *a = data;
	return GdipAddPathRectangle(path, a->x, a->y, a->width, a->height);
}

GpStatus GDIPAPI GdipDrawArc(GpGraphics *graphics, GpPen *pen, REAL x, REAL y, REAL width, REAL height, REAL start, REAL sweep)
{
	if (!graphics || !pen || width <= 0.0f || height <= 0.0f)
		return InvalidParameter;
	shape_args args = {x, y, width, height, start, sweep};
	return draw_built(graphics, pen, build_arc, &args);
}

GpStatus GDIPAPI GdipDrawArcI(GpGraphics *graphics, GpPen *pen, INT x, INT y, INT width, INT height, REAL start, REAL sweep)
{
	return GdipDrawArc(graphics, pen, (REAL)x, (REAL)y, (REAL)width, (REAL)height, start, sweep);
}

GpStatus GDIPAPI GdipDrawEllipse(GpGraphics *graphics, GpPen *pen, REAL x, REAL y, REAL width, REAL height)
{
	shape_args args = {x, y, width, height, 0.0f, 0.0f};
	return draw_built(graphics, pen, build_ellipse, &args);
}

GpStatus GDIPAPI GdipDrawEllipseI(GpGraphics *graphics, GpPen *pen, INT x, INT y, INT width, INT height)
{
	return GdipDrawEllipse(graphics, pen, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus GDIPAPI GdipDrawPie(GpGraphics *graphics, GpPen *pen, REAL x, REAL y, REAL width, REAL height, REAL start, REAL sweep)
{
	if (!graphics || !pen || width <= 0.0f || height <= 0.0f)
		return InvalidParameter;
	shape_args args = {x, y, width, height, start, sweep};
	return draw_built(graphics, pen, build_pie, &args);
}

GpStatus GDIPAPI GdipDrawPieI(GpGraphics *graphics, GpPen *pen, INT x, INT y, INT width, INT height, REAL start, REAL sweep)
{
	return GdipDrawPie(graphics, pen, (REAL)x, (REAL)y, (REAL)width, (REAL)height, start, sweep);
}

GpStatus GDIPAPI GdipDrawRectangle(GpGraphics *graphics, GpPen *pen, REAL x, REAL y, REAL width, REAL height)
{
	shape_args args = {x, y, width, height, 0.0f, 0.0f};
	return draw_built(graphics, pen, build_rect, &args);
}

GpStatus GDIPAPI GdipDrawRectangleI(GpGraphics *graphics, GpPen *pen, INT x, INT y, INT width, INT height)
{
	return GdipDrawRectangle(graphics, pen, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus GDIPAPI GdipDrawRectangles(GpGraphics *graphics, GpPen *pen, const GpRectF *rects, INT count)
{
	if (!graphics || !pen || !rects || count <= 0)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		GpStatus status = GdipDrawRectangle(graphics, pen, rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);
		if (status != Ok)
			return status;
	}
	return Ok;
}

GpStatus GDIPAPI GdipDrawRectanglesI(GpGraphics *graphics, GpPen *pen, const GpRect *rects, INT count)
{
	if (!graphics || !pen || !rects || count <= 0)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		GpStatus status = GdipDrawRectangleI(graphics, pen, rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);
		if (status != Ok)
			return status;
	}
	return Ok;
}

GpStatus GDIPAPI GdipFillPolygon(GpGraphics *graphics, GpBrush *brush, const GpPointF *points, INT count, GpFillMode fill)
{
	if (!graphics || !brush || !points || !count)
		return InvalidParameter;
	if (count == 1 || count == 2)
		return Ok;
	if (count < 0)
		return OutOfMemory;
	point_args args = {points, count, 0.0f};
	return fill_built(graphics, brush, fill, build_polygon, &args);
}

GpStatus GDIPAPI GdipFillPolygonI(GpGraphics *graphics, GpBrush *brush, const GpPoint *points, INT count, GpFillMode fill)
{
	if (!graphics || !brush || !points || !count)
		return InvalidParameter;
	if (count < 0)
		return OutOfMemory;
	GpPointF *floats = float_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipFillPolygon(graphics, brush, floats, count, fill);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipFillPolygon2(GpGraphics *graphics, GpBrush *brush, const GpPointF *points, INT count)
{
	return GdipFillPolygon(graphics, brush, points, count, FillModeAlternate);
}

GpStatus GDIPAPI GdipFillPolygon2I(GpGraphics *graphics, GpBrush *brush, const GpPoint *points, INT count)
{
	return GdipFillPolygonI(graphics, brush, points, count, FillModeAlternate);
}

GpStatus GDIPAPI GdipFillClosedCurve2(GpGraphics *graphics, GpBrush *brush, const GpPointF *points, INT count, REAL tension, GpFillMode fill)
{
	if (!graphics || !brush || !points)
		return InvalidParameter;
	if (count == 1 || count == 2)
		return Ok;
	if (count <= 0)
		return InvalidParameter;
	point_args args = {points, count, tension};
	return fill_built(graphics, brush, fill, build_closed_curve, &args);
}

GpStatus GDIPAPI GdipFillClosedCurve2I(GpGraphics *graphics, GpBrush *brush, const GpPoint *points, INT count, REAL tension, GpFillMode fill)
{
	if (!graphics || !brush || !points)
		return InvalidParameter;
	if (count <= 0)
		return count ? OutOfMemory : InvalidParameter;
	GpPointF *floats = float_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipFillClosedCurve2(graphics, brush, floats, count, tension, fill);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipFillClosedCurve(GpGraphics *graphics, GpBrush *brush, const GpPointF *points, INT count)
{
	return GdipFillClosedCurve2(graphics, brush, points, count, 0.5f, FillModeAlternate);
}

GpStatus GDIPAPI GdipFillClosedCurveI(GpGraphics *graphics, GpBrush *brush, const GpPoint *points, INT count)
{
	return GdipFillClosedCurve2I(graphics, brush, points, count, 0.5f, FillModeAlternate);
}

GpStatus GDIPAPI GdipFillEllipse(GpGraphics *graphics, GpBrush *brush, REAL x, REAL y, REAL width, REAL height)
{
	shape_args args = {x, y, width, height, 0.0f, 0.0f};
	return fill_built(graphics, brush, FillModeAlternate, build_ellipse, &args);
}

GpStatus GDIPAPI GdipFillEllipseI(GpGraphics *graphics, GpBrush *brush, INT x, INT y, INT width, INT height)
{
	return GdipFillEllipse(graphics, brush, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus GDIPAPI GdipFillPie(GpGraphics *graphics, GpBrush *brush, REAL x, REAL y, REAL width, REAL height, REAL start, REAL sweep)
{
	if (!graphics || !brush)
		return InvalidParameter;
	shape_args args = {x, y, width, height, start, sweep};
	return fill_built(graphics, brush, FillModeAlternate, build_pie, &args);
}

GpStatus GDIPAPI GdipFillPieI(GpGraphics *graphics, GpBrush *brush, INT x, INT y, INT width, INT height, REAL start, REAL sweep)
{
	return GdipFillPie(graphics, brush, (REAL)x, (REAL)y, (REAL)width, (REAL)height, start, sweep);
}

GpStatus GDIPAPI GdipFillRectangle(GpGraphics *graphics, GpBrush *brush, REAL x, REAL y, REAL width, REAL height)
{
	shape_args args = {x, y, width, height, 0.0f, 0.0f};
	return fill_built(graphics, brush, FillModeAlternate, build_rect, &args);
}

GpStatus GDIPAPI GdipFillRectangleI(GpGraphics *graphics, GpBrush *brush, INT x, INT y, INT width, INT height)
{
	return GdipFillRectangle(graphics, brush, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus GDIPAPI GdipFillRectangles(GpGraphics *graphics, GpBrush *brush, const GpRectF *rects, INT count)
{
	if (!graphics || !brush || !rects || count <= 0)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		GpStatus status = GdipFillRectangle(graphics, brush, rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);
		if (status != Ok)
			return status;
	}
	return Ok;
}

GpStatus GDIPAPI GdipFillRectanglesI(GpGraphics *graphics, GpBrush *brush, const GpRect *rects, INT count)
{
	if (!graphics || !brush || !rects || count <= 0)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		GpStatus status = GdipFillRectangleI(graphics, brush, rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);
		if (status != Ok)
			return status;
	}
	return Ok;
}

// A region fills through its device rows, each already pixel-aligned.
GpStatus GDIPAPI GdipFillRegion(GpGraphics *graphics, GpBrush *brush, GpRegion *region)
{
	if (!graphics || !brush || !region)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpMatrix to_device;
	gp_world_to_device(graphics, &to_device);
	GpRegion *work = gp_region_clone(region);
	if (!work)
		return OutOfMemory;
	GpStatus status = gp_region_transform(work, &to_device);
	if (status == Ok && !gp_region_is_infinite(work)) {
		GpRectF *rects;
		INT count;
		status = gp_region_pixels(work, NULL, &rects, &count);
		if (status == Ok) {
			GpMatrix inverse;
			BOOL has_inverse = gp_matrix_invert(&to_device, &inverse);
			RECT limit;
			if (gp_clip_bounds(graphics, &limit)) {
				gp_surface surface;
				if (gp_surface_begin(graphics, &surface, &limit) == Ok) {
					BYTE *mask = gp_clip_mask(graphics, &limit);
					INT width = limit.right - limit.left;
					for (INT i = 0; i < count; i++) {
						RECT box = {(LONG)gp_floor(rects[i].X + 0.5f), (LONG)gp_floor(rects[i].Y + 0.5f),
							(LONG)gp_floor(rects[i].X + rects[i].Width + 0.5f), (LONG)gp_floor(rects[i].Y + rects[i].Height + 0.5f)};
						if (!IntersectRect(&box, &box, &limit))
							continue;
						for (INT y = box.top; y < box.bottom; y++) {
							for (INT x = box.left; x < box.right; x++) {
								if (mask && !mask[(SIZE_T)(y - limit.top) * width + (x - limit.left)])
									continue;
								ARGB color = gp_brush_sample(brush, has_inverse ? &inverse : NULL, x + 0.5f, y + 0.5f, graphics->origin_x, graphics->origin_y);
								gp_surface_blend(&surface, x, y, color, 255);
							}
						}
					}
					gp_free(mask);
					gp_surface_end(&surface);
				}
			}
			gp_free(rects);
		}
	} else if (status == Ok) {
		RECT limit;
		if (gp_clip_bounds(graphics, &limit)) {
			GpRectF box = {(REAL)limit.left, (REAL)limit.top, (REAL)(limit.right - limit.left), (REAL)(limit.bottom - limit.top)};
			GpMatrix saved = graphics->world;
			GpUnit saved_unit = graphics->unit;
			REAL saved_scale = graphics->scale;
			gp_matrix_identity(&graphics->world);
			graphics->unit = UnitDisplay;
			graphics->scale = 1.0f;
			status = GdipFillRectangle(graphics, brush, box.X, box.Y, box.Width, box.Height);
			graphics->world = saved;
			graphics->unit = saved_unit;
			graphics->scale = saved_scale;
		}
	}
	gp_region_delete(work);
	return status;
}

GpStatus GDIPAPI GdipGraphicsClear(GpGraphics *graphics, ARGB color)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	RECT limit;
	if (!gp_clip_bounds(graphics, &limit))
		return Ok;
	gp_surface surface;
	GpStatus status = gp_surface_begin(graphics, &surface, &limit);
	if (status != Ok)
		return status;
	BYTE *mask = gp_clip_mask(graphics, &limit);
	INT width = limit.right - limit.left, mode = graphics->compositing_mode;
	graphics->compositing_mode = CompositingSourceCopy;
	for (INT y = limit.top; y < limit.bottom; y++) {
		for (INT x = limit.left; x < limit.right; x++) {
			if (!mask || mask[(SIZE_T)(y - limit.top) * width + (x - limit.left)])
				gp_surface_blend(&surface, x, y, color, 255);
		}
	}
	graphics->compositing_mode = mode;
	gp_free(mask);
	gp_surface_end(&surface);
	return Ok;
}

GpStatus GDIPAPI GdipFlush(GpGraphics *graphics, INT intention)
{
	(void)intention;
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (graphics->dc)
		GdiFlush();
	return Ok;
}

GpStatus GDIPAPI GdipGetNearestColor(GpGraphics *graphics, ARGB *color)
{
	if (!graphics || !color)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	// A 16 bit bitmap can only keep the top bits of each channel, and it has no alpha.
	if (graphics->image) {
		if (graphics->image->format == PixelFormat16bppRGB565)
			*color = (*color & 0x00f8fcf8) | 0xff000000;
		else if (graphics->image->format == PixelFormat16bppRGB555)
			*color = (*color & 0x00f8f8f8) | 0xff000000;
	}
	return Ok;
}

// Clip: kept in the device space programs see, so neither later world changes nor origins set on the context move it, and
// read back through the current world.
GpStatus GDIPAPI GdipSetClipRegion(GpGraphics *graphics, GpRegion *region, CombineMode mode)
{
	if (!graphics || !region)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpRegion *work = gp_region_clone(region);
	if (!work)
		return OutOfMemory;
	GpMatrix to_device;
	world_to_public_device(graphics, &to_device);
	GpStatus status = gp_region_transform(work, &to_device);
	if (status == Ok)
		status = GdipCombineRegionRegion(graphics->clip, work, mode);
	gp_region_delete(work);
	return status;
}

GpStatus GDIPAPI GdipSetClipRect(GpGraphics *graphics, REAL x, REAL y, REAL width, REAL height, CombineMode mode)
{
	if (!graphics)
		return InvalidParameter;
	GpRectF box = {x, y, width, height};
	GpRegion *region;
	GpStatus status = GdipCreateRegion(&region);
	if (status != Ok)
		return status;
	status = GdipCombineRegionRect(region, &box, CombineModeReplace);
	if (status == Ok)
		status = GdipSetClipRegion(graphics, region, mode);
	gp_region_delete(region);
	return status;
}

GpStatus GDIPAPI GdipSetClipRectI(GpGraphics *graphics, INT x, INT y, INT width, INT height, CombineMode mode)
{
	return GdipSetClipRect(graphics, (REAL)x, (REAL)y, (REAL)width, (REAL)height, mode);
}

GpStatus GDIPAPI GdipSetClipPath(GpGraphics *graphics, GpPath *path, CombineMode mode)
{
	if (!graphics || !path)
		return InvalidParameter;
	GpRegion *region;
	GpStatus status = GdipCreateRegion(&region);
	if (status != Ok)
		return status;
	status = GdipCombineRegionPath(region, path, CombineModeReplace);
	if (status == Ok)
		status = GdipSetClipRegion(graphics, region, mode);
	gp_region_delete(region);
	return status;
}

GpStatus GDIPAPI GdipSetClipHrgn(GpGraphics *graphics, HRGN handle, CombineMode mode)
{
	extern GpStatus GDIPAPI GdipCreateRegionHrgn(HRGN, GpRegion **);
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpRegion *region;
	GpStatus status = GdipCreateRegionHrgn(handle, &region);
	if (status != Ok)
		return status;
	// A handle counts the context's pixels.
	POINT origin = dc_origin(graphics);
	GpMatrix to_clip = {{1.0f, 0.0f, 0.0f, 1.0f, (REAL)-origin.x, (REAL)-origin.y}};
	status = gp_region_transform(region, &to_clip);
	if (status == Ok)
		status = GdipCombineRegionRegion(graphics->clip, region, mode);
	gp_region_delete(region);
	return status;
}

GpStatus GDIPAPI GdipSetClipGraphics(GpGraphics *graphics, GpGraphics *other, CombineMode mode)
{
	if (!graphics || !other)
		return InvalidParameter;
	return GdipCombineRegionRegion(graphics->clip, other->clip, mode);
}

GpStatus GDIPAPI GdipResetClip(GpGraphics *graphics)
{
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	return GdipSetInfinite(graphics->clip);
}

GpStatus GDIPAPI GdipTranslateClip(GpGraphics *graphics, REAL dx, REAL dy)
{
	extern GpStatus GDIPAPI GdipTranslateRegion(GpRegion *, REAL, REAL);
	if (!graphics)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpPointF delta = {dx, dy};
	GpMatrix to_device;
	world_to_public_device(graphics, &to_device);
	to_device.m[4] = to_device.m[5] = 0.0f;
	gp_matrix_apply(&to_device, &delta, 1);
	return GdipTranslateRegion(graphics->clip, delta.X, delta.Y);
}

GpStatus GDIPAPI GdipTranslateClipI(GpGraphics *graphics, INT dx, INT dy)
{
	return GdipTranslateClip(graphics, (REAL)dx, (REAL)dy);
}

static GpStatus clip_to_world(const GpGraphics *graphics, GpRegion *region)
{
	GpMatrix to_device, inverse;
	world_to_public_device(graphics, &to_device);
	if (!gp_matrix_invert(&to_device, &inverse))
		return GenericError;
	return gp_region_transform(region, &inverse);
}

GpStatus GDIPAPI GdipGetClip(GpGraphics *graphics, GpRegion *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpRegion *copy = gp_region_clone(graphics->clip);
	if (!copy)
		return OutOfMemory;
	GpStatus status = clip_to_world(graphics, copy);
	if (status == Ok)
		status = GdipCombineRegionRegion(out, copy, CombineModeReplace);
	gp_region_delete(copy);
	return status;
}

GpStatus GDIPAPI GdipGetClipBounds(GpGraphics *graphics, GpRectF *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpRegion *copy = gp_region_clone(graphics->clip);
	if (!copy)
		return OutOfMemory;
	GpStatus status = clip_to_world(graphics, copy);
	if (status == Ok)
		gp_region_bounds(copy, out);
	gp_region_delete(copy);
	return status;
}

GpStatus GDIPAPI GdipGetClipBoundsI(GpGraphics *graphics, GpRect *out)
{
	if (!out)
		return InvalidParameter;
	GpRectF box;
	GpStatus status = GdipGetClipBounds(graphics, &box);
	if (status == Ok) {
		out->X = (INT)gp_floor(box.X + 0.5f);
		out->Y = (INT)gp_floor(box.Y + 0.5f);
		out->Width = (INT)gp_floor(box.Width + 0.5f);
		out->Height = (INT)gp_floor(box.Height + 0.5f);
	}
	return status;
}

GpStatus GDIPAPI GdipGetVisibleClipBounds(GpGraphics *graphics, GpRectF *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	RECT limit;
	if (!gp_clip_bounds(graphics, &limit)) {
		out->X = out->Y = out->Width = out->Height = 0.0f;
		return Ok;
	}
	GpPointF corners[2] = {{(REAL)limit.left, (REAL)limit.top}, {(REAL)limit.right, (REAL)limit.bottom}};
	GpMatrix to_device, inverse;
	gp_world_to_device(graphics, &to_device);
	if (gp_matrix_invert(&to_device, &inverse))
		gp_matrix_apply(&inverse, corners, 2);
	out->X = corners[0].X;
	out->Y = corners[0].Y;
	out->Width = corners[1].X - corners[0].X;
	out->Height = corners[1].Y - corners[0].Y;
	return Ok;
}

GpStatus GDIPAPI GdipGetVisibleClipBoundsI(GpGraphics *graphics, GpRect *out)
{
	if (!out)
		return InvalidParameter;
	GpRectF box;
	GpStatus status = GdipGetVisibleClipBounds(graphics, &box);
	if (status == Ok) {
		out->X = (INT)gp_ceil(box.X);
		out->Y = (INT)gp_ceil(box.Y);
		out->Width = (INT)gp_ceil(box.Width);
		out->Height = (INT)gp_ceil(box.Height);
	}
	return status;
}

GpStatus GDIPAPI GdipIsClipEmpty(GpGraphics *graphics, BOOL *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	*out = gp_region_is_empty(graphics->clip);
	return Ok;
}

GpStatus GDIPAPI GdipIsVisibleClipEmpty(GpGraphics *graphics, BOOL *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	RECT limit;
	*out = !gp_clip_bounds(graphics, &limit);
	return Ok;
}

GpStatus GDIPAPI GdipIsVisiblePoint(GpGraphics *graphics, REAL x, REAL y, BOOL *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	GpPointF point = {x, y};
	GpMatrix to_device;
	gp_world_to_device(graphics, &to_device);
	gp_matrix_apply(&to_device, &point, 1);
	INT width, height;
	surface_size(graphics, &width, &height);
	// A point belongs to the pixel it rounds to.
	REAL x_pixel = gp_floor(point.X + 0.5f), y_pixel = gp_floor(point.Y + 0.5f);
	POINT origin = dc_origin(graphics);
	*out = x_pixel >= 0.0f && y_pixel >= 0.0f && x_pixel < width && y_pixel < height && gp_region_contains(graphics->clip, x_pixel - origin.x, y_pixel - origin.y);
	return Ok;
}

GpStatus GDIPAPI GdipIsVisiblePointI(GpGraphics *graphics, INT x, INT y, BOOL *out)
{
	return GdipIsVisiblePoint(graphics, (REAL)x, (REAL)y, out);
}

GpStatus GDIPAPI GdipIsVisibleRect(GpGraphics *graphics, REAL x, REAL y, REAL width, REAL height, BOOL *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (!width || !height) {
		*out = FALSE;
		return Ok;
	}
	RECT limit;
	if (!gp_clip_bounds(graphics, &limit)) {
		*out = FALSE;
		return Ok;
	}
	GpPointF corners[2] = {{x, y}, {x + width, y + height}};
	GpMatrix to_device;
	gp_world_to_device(graphics, &to_device);
	gp_matrix_apply(&to_device, corners, 2);
	RECT box = {(LONG)gp_floor(corners[0].X), (LONG)gp_floor(corners[0].Y), (LONG)gp_ceil(corners[1].X), (LONG)gp_ceil(corners[1].Y)};
	*out = FALSE;
	if (IntersectRect(&box, &box, &limit)) {
		POINT origin = dc_origin(graphics);
		for (INT py = box.top; py < box.bottom && !*out; py++) {
			for (INT px = box.left; px < box.right && !*out; px++)
				*out = gp_region_contains(graphics->clip, px - origin.x + 0.5f, py - origin.y + 0.5f);
		}
	}
	return Ok;
}

GpStatus GDIPAPI GdipIsVisibleRectI(GpGraphics *graphics, INT x, INT y, INT width, INT height, BOOL *out)
{
	return GdipIsVisibleRect(graphics, (REAL)x, (REAL)y, (REAL)width, (REAL)height, out);
}

static gp_state *push_state(GpGraphics *graphics, BOOL container)
{
	gp_state *state = gp_alloc(sizeof(gp_state));
	if (!state)
		return NULL;
	if (!(state->clip = gp_region_clone(graphics->clip))) {
		gp_free(state);
		return NULL;
	}
	state->id = ++graphics->next_state;
	state->container = container;
	state->world = graphics->world;
	state->unit = graphics->unit;
	state->scale = graphics->scale;
	state->compositing_mode = graphics->compositing_mode;
	state->compositing_quality = graphics->compositing_quality;
	state->interpolation = graphics->interpolation;
	state->smoothing = graphics->smoothing;
	state->pixel_offset = graphics->pixel_offset;
	state->text_hint = graphics->text_hint;
	state->text_contrast = graphics->text_contrast;
	state->origin_x = graphics->origin_x;
	state->origin_y = graphics->origin_y;
	state->next = graphics->states;
	graphics->states = state;
	return state;
}

static GpStatus pop_state(GpGraphics *graphics, UINT id, BOOL container)
{
	gp_state *state = graphics->states;
	while (state && !(state->id == id && state->container == container))
		state = state->next;
	if (!state)
		return Ok;
	graphics->world = state->world;
	graphics->unit = state->unit;
	graphics->scale = state->scale;
	graphics->compositing_mode = state->compositing_mode;
	graphics->compositing_quality = state->compositing_quality;
	graphics->interpolation = state->interpolation;
	graphics->smoothing = state->smoothing;
	graphics->pixel_offset = state->pixel_offset;
	graphics->text_hint = state->text_hint;
	graphics->text_contrast = state->text_contrast;
	graphics->origin_x = state->origin_x;
	graphics->origin_y = state->origin_y;
	gp_region_delete(graphics->clip);
	graphics->clip = state->clip;
	state->clip = NULL;
	// Restoring drops this state and every one saved after it.
	while (graphics->states != state) {
		gp_state *top = graphics->states;
		graphics->states = top->next;
		gp_region_delete(top->clip);
		gp_free(top);
	}
	graphics->states = state->next;
	gp_free(state);
	return Ok;
}

GpStatus GDIPAPI GdipSaveGraphics(GpGraphics *graphics, UINT *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	gp_state *state = push_state(graphics, FALSE);
	if (!state)
		return OutOfMemory;
	*out = state->id;
	return Ok;
}

GpStatus GDIPAPI GdipRestoreGraphics(GpGraphics *graphics, UINT id)
{
	return graphics ? pop_state(graphics, id, FALSE) : InvalidParameter;
}

GpStatus GDIPAPI GdipBeginContainer2(GpGraphics *graphics, UINT *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	gp_state *state = push_state(graphics, TRUE);
	if (!state)
		return OutOfMemory;
	*out = state->id;
	return Ok;
}

GpStatus GDIPAPI GdipBeginContainer(GpGraphics *graphics, const GpRectF *to, const GpRectF *from, GpUnit unit, UINT *out)
{
	if (!graphics || !to || !from || !out || (INT)unit < UnitPixel || unit > UnitMillimeter)
		return InvalidParameter;
	GpStatus status = GdipBeginContainer2(graphics, out);
	if (status != Ok)
		return status;
	// The source rectangle's size is divided by its unit, so a larger unit shrinks what is drawn.
	REAL ux = gp_unit_scale(graphics, unit, graphics->dpi_x), uy = gp_unit_scale(graphics, unit, graphics->dpi_y);
	REAL sx = from->Width ? to->Width / (from->Width * ux) : 1.0f, sy = from->Height ? to->Height / (from->Height * uy) : 1.0f;
	GpMatrix step = {{sx, 0.0f, 0.0f, sy, to->X - from->X * sx, to->Y - from->Y * sy}};
	return GdipMultiplyMatrix(&graphics->world, &step, MatrixOrderPrepend);
}

GpStatus GDIPAPI GdipBeginContainerI(GpGraphics *graphics, const GpRect *to, const GpRect *from, GpUnit unit, UINT *out)
{
	if (!to || !from)
		return InvalidParameter;
	GpRectF a = {(REAL)to->X, (REAL)to->Y, (REAL)to->Width, (REAL)to->Height}, b = {(REAL)from->X, (REAL)from->Y, (REAL)from->Width, (REAL)from->Height};
	return GdipBeginContainer(graphics, &a, &b, unit, out);
}

GpStatus GDIPAPI GdipEndContainer(GpGraphics *graphics, UINT id)
{
	return graphics ? pop_state(graphics, id, TRUE) : InvalidParameter;
}

GpStatus GDIPAPI GdipComment(GpGraphics *graphics, UINT size, const BYTE *data)
{
	if (!graphics || !data || !size)
		return InvalidParameter;
	return graphics->held_dc ? ObjectBusy : NotImplemented;
}

GpStatus GDIPAPI GdipGraphicsSetAbort(GpGraphics *graphics, void *abort)
{
	(void)abort;
	return graphics ? Ok : InvalidParameter;
}

HPALETTE GDIPAPI GdipCreateHalftonePalette(void)
{
	HDC dc = GetDC(NULL);
	HPALETTE palette = dc ? CreateHalftonePalette(dc) : NULL;
	if (dc)
		ReleaseDC(NULL, dc);
	return palette;
}

// GetDC on a bitmap hands out a write-only surface: a section filled with a colour nobody expects, from which only the
// pixels that changed come back at ReleaseDC.
#define UNTOUCHED_PIXEL 0x000d0b0c

GpStatus GDIPAPI GdipGetDC(GpGraphics *graphics, HDC *out)
{
	if (!graphics || !out)
		return InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (!graphics->image) {
		graphics->held_dc = graphics->dc;
		*out = graphics->dc;
		return Ok;
	}
	GpImage *image = graphics->image;
	BITMAPINFO header = {{sizeof(BITMAPINFOHEADER), (LONG)image->width, -(LONG)image->height, 1, 32, BI_RGB}};
	HDC dc = CreateCompatibleDC(NULL);
	if (!dc)
		return GenericError;
	void *bits;
	HBITMAP bitmap = CreateDIBSection(dc, &header, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bitmap) {
		DeleteDC(dc);
		return GenericError;
	}
	DWORD *to = bits;
	for (UINT i = 0; i < image->width * image->height; i++)
		*to++ = UNTOUCHED_PIXEL;
	graphics->held_old = SelectObject(dc, bitmap);
	graphics->held_bitmap = bitmap;
	graphics->held_bits = bits;
	graphics->held_dc = dc;
	*out = dc;
	return Ok;
}

GpStatus GDIPAPI GdipReleaseDC(GpGraphics *graphics, HDC dc)
{
	if (!graphics || !dc || dc != graphics->held_dc)
		return InvalidParameter;
	graphics->held_dc = NULL;
	if (!graphics->image)
		return Ok;
	GdiFlush();
	GpImage *image = graphics->image;
	const DWORD *from = graphics->held_bits;
	for (UINT y = 0; y < image->height; y++) {
		for (UINT x = 0; x < image->width; x++) {
			DWORD pixel = *from++ & 0xffffff;
			if (pixel != UNTOUCHED_PIXEL)
				gp_pixel_write(image, x, y, pixel | 0xff000000);
		}
	}
	SelectObject(dc, graphics->held_old);
	DeleteObject(graphics->held_bitmap);
	DeleteDC(dc);
	graphics->held_bitmap = NULL;
	graphics->held_bits = NULL;
	return Ok;
}

// Reads the source at a position counted from pixel origins, inside the source rectangle: the closest pixel, or the four
// around it weighted by distance. Colours mix by how opaque they are, so transparent neighbours bring no colour along.
static ARGB sample_image(const GpImage *image, REAL x, REAL y, BOOL nearest, REAL sx, REAL sy, REAL sw, REAL sh)
{
	INT left = (INT)gp_ceil(sx), top = (INT)gp_ceil(sy), right = (INT)gp_ceil(sx + sw) - 1, bottom = (INT)gp_ceil(sy + sh) - 1;
	if (right >= (INT)image->width)
		right = (INT)image->width - 1;
	if (bottom >= (INT)image->height)
		bottom = (INT)image->height - 1;
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
#define CLAMPED(value, low, high) ((value) < (low) ? (low) : (value) > (high) ? (high) : (value))
	if (nearest) {
		INT px = (INT)gp_floor(x + 0.5f), py = (INT)gp_floor(y + 0.5f);
		return gp_pixel_read(image, CLAMPED(px, left, right), CLAMPED(py, top, bottom));
	}
	INT x0 = (INT)gp_floor(x), y0 = (INT)gp_floor(y);
	REAL fx = x - x0, fy = y - y0;
	INT xs[2] = {x0, x0 + 1}, ys[2] = {CLAMPED(y0, top, bottom), CLAMPED(y0 + 1, top, bottom)};
#undef CLAMPED
	REAL a = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f;
	for (INT j = 0; j < 2; j++) {
		for (INT i = 0; i < 2; i++) {
			REAL weight = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
			// Past the source's sides there is nothing: the edge fades out along the last step.
			if (weight <= 0.0f || xs[i] < left || xs[i] > right)
				continue;
			ARGB c = gp_pixel_read(image, xs[i], ys[j]);
			REAL opacity = (c >> 24) * weight;
			a += opacity;
			r += ((c >> 16) & 255) * opacity;
			g += ((c >> 8) & 255) * opacity;
			b += (c & 255) * opacity;
		}
	}
	if (a <= 0.0f)
		return 0;
	return (ARGB)(a + 0.5f) << 24 | (ARGB)(r / a + 0.5f) << 16 | (ARGB)(g / a + 0.5f) << 8 | (ARGB)(b / a + 0.5f);
}

// Paints the parallelogram named by three points from the source rectangle, sampling through the inverse mapping.
static GpStatus draw_image_points(GpGraphics *graphics, GpImage *image, const GpPointF *points, REAL sx, REAL sy, REAL sw, REAL sh, const GpImageAttributes *attributes)
{
	GpPointF corners[3] = {points[0], points[1], points[2]};
	GpMatrix to_device;
	gp_world_to_device(graphics, &to_device);
	gp_matrix_apply(&to_device, corners, 3);
	GpMatrix map, inverse;
	map.m[0] = (corners[1].X - corners[0].X) / sw;
	map.m[1] = (corners[1].Y - corners[0].Y) / sw;
	map.m[2] = (corners[2].X - corners[0].X) / sh;
	map.m[3] = (corners[2].Y - corners[0].Y) / sh;
	map.m[4] = corners[0].X - map.m[0] * sx - map.m[2] * sy;
	map.m[5] = corners[0].Y - map.m[1] * sx - map.m[3] * sy;
	if (!gp_matrix_invert(&map, &inverse))
		return Ok;
	REAL dx = corners[1].X - corners[0].X, dy = corners[1].Y - corners[0].Y, ex = corners[2].X - corners[0].X, ey = corners[2].Y - corners[0].Y;
	REAL xs[4] = {corners[0].X, corners[1].X, corners[2].X, corners[0].X + dx + ex}, ys[4] = {corners[0].Y, corners[1].Y, corners[2].Y, corners[0].Y + dy + ey};
	REAL left = xs[0], right = xs[0], top = ys[0], bottom = ys[0];
	for (int i = 1; i < 4; i++) {
		if (xs[i] < left) left = xs[i];
		if (xs[i] > right) right = xs[i];
		if (ys[i] < top) top = ys[i];
		if (ys[i] > bottom) bottom = ys[i];
	}
	RECT limit, area = {(LONG)gp_floor(left), (LONG)gp_floor(top), (LONG)gp_ceil(right), (LONG)gp_ceil(bottom)};
	if (!gp_clip_bounds(graphics, &limit) || !IntersectRect(&area, &area, &limit))
		return Ok;
	gp_surface surface;
	GpStatus status = gp_surface_begin(graphics, &surface, &area);
	if (status != Ok)
		return status;
	BYTE *mask = gp_clip_mask(graphics, &area);
	INT width = area.right - area.left;
	// Half-pixel offsets count pixels from their centres; the other modes from their corners.
	REAL half = graphics->pixel_offset == PixelOffsetHighQuality || graphics->pixel_offset == PixelOffsetHalf ? 0.5f : 0.0f;
	BOOL nearest = graphics->interpolation == InterpolationNearest;
	for (INT y = area.top; y < area.bottom; y++) {
		for (INT x = area.left; x < area.right; x++) {
			if (mask && !mask[(SIZE_T)(y - area.top) * width + (x - area.left)])
				continue;
			GpPointF p = {x + half, y + half};
			gp_matrix_apply(&inverse, &p, 1);
			if (p.X < sx || p.Y < sy || p.X >= sx + sw || p.Y >= sy + sh)
				continue;
			// A position closest to the pixel past the source's end is outside it.
			if (nearest && (gp_floor(p.X - half + 0.5f + 1e-4f) >= sx + sw || gp_floor(p.Y - half + 0.5f + 1e-4f) >= sy + sh))
				continue;
			ARGB color = sample_image(image, p.X - half, p.Y - half, nearest, sx, sy, sw, sh);
			if (attributes)
				color = gp_attributes_apply(attributes, color);
			gp_surface_blend(&surface, x, y, color, 255);
		}
	}
	gp_free(mask);
	gp_surface_end(&surface);
	return Ok;
}

GpStatus GDIPAPI GdipDrawImagePointsRect(GpGraphics *graphics, GpImage *image, const GpPointF *points, INT count, REAL sx, REAL sy, REAL sw, REAL sh, GpUnit unit, const GpImageAttributes *attributes, void *abort, void *data)
{
	(void)abort;
	(void)data;
	if (!graphics || !image || !points || count != 3)
		return count == 4 ? NotImplemented : InvalidParameter;
	if (graphics->held_dc)
		return ObjectBusy;
	if (image->type == ImageTypeMetafile)
		return NotImplemented;
	if (unit != UnitPixel) {
		REAL factor = gp_unit_scale(graphics, unit, image->dpi_x);
		sx *= factor;
		sy *= factor;
		sw *= factor;
		sh *= factor;
	}
	// An empty source has nothing to paint.
	if (sw == 0.0f || sh == 0.0f)
		return Ok;
	return draw_image_points(graphics, image, points, sx, sy, sw, sh, attributes);
}

GpStatus GDIPAPI GdipDrawImagePointsRectI(GpGraphics *graphics, GpImage *image, const GpPoint *points, INT count, INT sx, INT sy, INT sw, INT sh, GpUnit unit, const GpImageAttributes *attributes, void *abort, void *data)
{
	if (!points || count != 3)
		return InvalidParameter;
	GpPointF floats[3];
	for (int i = 0; i < 3; i++) {
		floats[i].X = (REAL)points[i].X;
		floats[i].Y = (REAL)points[i].Y;
	}
	return GdipDrawImagePointsRect(graphics, image, floats, count, (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh, unit, attributes, abort, data);
}

GpStatus GDIPAPI GdipDrawImageRectRect(GpGraphics *graphics, GpImage *image, REAL dx, REAL dy, REAL dw, REAL dh, REAL sx, REAL sy, REAL sw, REAL sh, GpUnit unit, const GpImageAttributes *attributes, void *abort, void *data)
{
	GpPointF points[3] = {{dx, dy}, {dx + dw, dy}, {dx, dy + dh}};
	return GdipDrawImagePointsRect(graphics, image, points, 3, sx, sy, sw, sh, unit, attributes, abort, data);
}

GpStatus GDIPAPI GdipDrawImageRectRectI(GpGraphics *graphics, GpImage *image, INT dx, INT dy, INT dw, INT dh, INT sx, INT sy, INT sw, INT sh, GpUnit unit, const GpImageAttributes *attributes, void *abort, void *data)
{
	return GdipDrawImageRectRect(graphics, image, (REAL)dx, (REAL)dy, (REAL)dw, (REAL)dh, (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh, unit, attributes, abort, data);
}

GpStatus GDIPAPI GdipDrawImagePoints(GpGraphics *graphics, GpImage *image, const GpPointF *points, INT count)
{
	if (!image)
		return InvalidParameter;
	return GdipDrawImagePointsRect(graphics, image, points, count, 0.0f, 0.0f, (REAL)image->width, (REAL)image->height, UnitPixel, NULL, NULL, NULL);
}

GpStatus GDIPAPI GdipDrawImagePointsI(GpGraphics *graphics, GpImage *image, const GpPoint *points, INT count)
{
	if (!image)
		return InvalidParameter;
	return GdipDrawImagePointsRectI(graphics, image, points, count, 0, 0, image->width, image->height, UnitPixel, NULL, NULL, NULL);
}

// Without a size, an image keeps its physical size: its pixels at its resolution, in the graphics' resolution.
GpStatus GDIPAPI GdipDrawImageRect(GpGraphics *graphics, GpImage *image, REAL x, REAL y, REAL width, REAL height)
{
	if (!image)
		return InvalidParameter;
	return GdipDrawImageRectRect(graphics, image, x, y, width, height, 0.0f, 0.0f, (REAL)image->width, (REAL)image->height, UnitPixel, NULL, NULL, NULL);
}

GpStatus GDIPAPI GdipDrawImageRectI(GpGraphics *graphics, GpImage *image, INT x, INT y, INT width, INT height)
{
	return GdipDrawImageRect(graphics, image, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus GDIPAPI GdipDrawImage(GpGraphics *graphics, GpImage *image, REAL x, REAL y)
{
	if (!graphics || !image)
		return InvalidParameter;
	REAL width = image->width * graphics->dpi_x / (image->dpi_x > 0.0f ? image->dpi_x : graphics->dpi_x);
	REAL height = image->height * graphics->dpi_y / (image->dpi_y > 0.0f ? image->dpi_y : graphics->dpi_y);
	return GdipDrawImageRect(graphics, image, x, y, width, height);
}

GpStatus GDIPAPI GdipDrawImageI(GpGraphics *graphics, GpImage *image, INT x, INT y)
{
	return GdipDrawImage(graphics, image, (REAL)x, (REAL)y);
}

GpStatus GDIPAPI GdipDrawImagePointRect(GpGraphics *graphics, GpImage *image, REAL x, REAL y, REAL sx, REAL sy, REAL sw, REAL sh, GpUnit unit)
{
	if (!graphics || !image)
		return InvalidParameter;
	// Source pixels keep their physical size: the image's resolution against the target's.
	REAL fx = gp_unit_scale(graphics, unit, graphics->dpi_x), fy = gp_unit_scale(graphics, unit, graphics->dpi_y);
	if (unit == UnitPixel || unit == UnitDisplay) {
		fx = image->dpi_x > 0.0f ? graphics->dpi_x / image->dpi_x : 1.0f;
		fy = image->dpi_y > 0.0f ? graphics->dpi_y / image->dpi_y : 1.0f;
	}
	REAL width = sw * fx, height = sh * fy;
	GpPointF points[3] = {{x, y}, {x + width, y}, {x, y + height}};
	return GdipDrawImagePointsRect(graphics, image, points, 3, sx, sy, sw, sh, unit, NULL, NULL, NULL);
}

GpStatus GDIPAPI GdipDrawImagePointRectI(GpGraphics *graphics, GpImage *image, INT x, INT y, INT sx, INT sy, INT sw, INT sh, GpUnit unit)
{
	return GdipDrawImagePointRect(graphics, image, (REAL)x, (REAL)y, (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh, unit);
}

GpStatus GDIPAPI GdipDrawImageFX(GpGraphics *graphics, GpImage *image, GpRectF *source, GpMatrix *matrix, void *effect, GpImageAttributes *attributes, GpUnit unit)
{
	// Effects themselves cannot be created here, so only the image, its source rectangle and the transform remain.
	(void)effect;
	if (!graphics || !image)
		return InvalidParameter;
	GpRectF whole = {0.0f, 0.0f, (REAL)image->width, (REAL)image->height};
	const GpRectF *from = source ? source : &whole;
	GpPointF points[3] = {{0.0f, 0.0f}, {from->Width, 0.0f}, {0.0f, from->Height}};
	if (matrix)
		gp_matrix_apply(matrix, points, 3);
	return GdipDrawImagePointsRect(graphics, image, points, 3, from->X, from->Y, from->Width, from->Height, source ? unit : UnitPixel, attributes, NULL, NULL);
}

GpStatus GDIPAPI GdipCreateBitmapFromGraphics(INT width, INT height, GpGraphics *graphics, GpImage **out)
{
	if (!graphics || !out)
		return InvalidParameter;
	GpStatus status = gp_bitmap_new(width, height, PixelFormat32bppPARGB, out);
	if (status == Ok) {
		(*out)->dpi_x = graphics->dpi_x;
		(*out)->dpi_y = graphics->dpi_y;
	}
	return status;
}

typedef struct
{
	GpImage *image;
} GpCachedBitmap;

GpStatus GDIPAPI GdipCreateCachedBitmap(GpImage *image, GpGraphics *graphics, GpCachedBitmap **out)
{
	if (!image || !graphics || !out)
		return InvalidParameter;
	GpCachedBitmap *cached = gp_alloc(sizeof(GpCachedBitmap));
	if (!cached)
		return OutOfMemory;
	GpStatus status = GdipCloneImage(image, &cached->image);
	if (status != Ok) {
		gp_free(cached);
		return status;
	}
	*out = cached;
	return Ok;
}

GpStatus GDIPAPI GdipDeleteCachedBitmap(GpCachedBitmap *cached)
{
	if (!cached)
		return InvalidParameter;
	gp_image_release(cached->image);
	gp_free(cached);
	return Ok;
}

GpStatus GDIPAPI GdipDrawCachedBitmap(GpGraphics *graphics, GpCachedBitmap *cached, INT x, INT y)
{
	if (!graphics || !cached)
		return InvalidParameter;
	return GdipDrawImage(graphics, cached->image, (REAL)x, (REAL)y);
}
