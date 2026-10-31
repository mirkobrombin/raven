/* SPDX-License-Identifier: MIT */
#include "brush.h"
#include "gmath.h"

extern GpStatus GDIPAPI GdipMultiplyMatrix(GpMatrix *, const GpMatrix *, GpMatrixOrder);
extern GpStatus GDIPAPI GdipTranslateMatrix(GpMatrix *, REAL, REAL, GpMatrixOrder);
extern GpStatus GDIPAPI GdipScaleMatrix(GpMatrix *, REAL, REAL, GpMatrixOrder);
extern GpStatus GDIPAPI GdipRotateMatrix(GpMatrix *, REAL, GpMatrixOrder);
extern ARGB gp_image_sample(struct gp_image *image, const void *attributes, REAL x, REAL y);
extern BOOL gp_image_bounds(struct gp_image *image, GpRectF *out);
extern struct gp_image *gp_image_share(struct gp_image *image);
extern void gp_image_release(struct gp_image *image);
extern struct gp_image *gp_image_crop(struct gp_image *image, const GpRectF *area);

static GpBrush *new_brush(GpBrushType type)
{
	GpBrush *brush = gp_alloc(sizeof(GpBrush));
	if (brush) {
		brush->type = type;
		gp_matrix_identity(&brush->transform);
	}
	return brush;
}

GpBrush *gp_solid_new(ARGB color)
{
	GpBrush *brush = new_brush(BrushTypeSolidColor);
	if (brush)
		brush->color = color;
	return brush;
}

GpBrush *gp_brush_clone(const GpBrush *brush)
{
	GpBrush *copy = gp_dup(brush, sizeof(GpBrush));
	if (!copy)
		return NULL;
	copy->blend_factors = copy->blend_positions = copy->preset_positions = NULL;
	copy->preset_colors = copy->surround = NULL;
	copy->path = NULL;
	BOOL good = TRUE;
	if (brush->blend_count) {
		copy->blend_factors = gp_dup(brush->blend_factors, brush->blend_count * sizeof(REAL));
		copy->blend_positions = gp_dup(brush->blend_positions, brush->blend_count * sizeof(REAL));
		good = copy->blend_factors && copy->blend_positions;
	}
	if (good && brush->preset_count) {
		copy->preset_colors = gp_dup(brush->preset_colors, brush->preset_count * sizeof(ARGB));
		copy->preset_positions = gp_dup(brush->preset_positions, brush->preset_count * sizeof(REAL));
		good = copy->preset_colors && copy->preset_positions;
	}
	if (good && brush->surround_count)
		good = (copy->surround = gp_dup(brush->surround, brush->surround_count * sizeof(ARGB))) != NULL;
	if (good && brush->path)
		good = (copy->path = gp_path_clone(brush->path)) != NULL;
	if (brush->image)
		copy->image = gp_image_share(brush->image);
	if (!good) {
		gp_brush_delete(copy);
		return NULL;
	}
	return copy;
}

void gp_brush_delete(GpBrush *brush)
{
	gp_free(brush->blend_factors);
	gp_free(brush->blend_positions);
	gp_free(brush->preset_colors);
	gp_free(brush->preset_positions);
	gp_free(brush->surround);
	if (brush->path)
		gp_path_delete(brush->path);
	if (brush->image)
		gp_image_release(brush->image);
	gp_free(brush);
}

GpStatus GDIPAPI GdipCloneBrush(GpBrush *brush, GpBrush **out)
{
	if (!brush || !out)
		return InvalidParameter;
	return (*out = gp_brush_clone(brush)) ? Ok : OutOfMemory;
}

GpStatus GDIPAPI GdipDeleteBrush(GpBrush *brush)
{
	if (!brush)
		return InvalidParameter;
	gp_brush_delete(brush);
	return Ok;
}

GpStatus GDIPAPI GdipGetBrushType(GpBrush *brush, GpBrushType *out)
{
	if (!brush || !out)
		return InvalidParameter;
	*out = brush->type;
	return Ok;
}

GpStatus GDIPAPI GdipCreateSolidFill(ARGB color, GpBrush **out)
{
	if (!out)
		return InvalidParameter;
	return (*out = gp_solid_new(color)) ? Ok : OutOfMemory;
}

GpStatus GDIPAPI GdipGetSolidFillColor(GpBrush *brush, ARGB *out)
{
	if (!brush || !out || brush->type != BrushTypeSolidColor)
		return InvalidParameter;
	*out = brush->color;
	return Ok;
}

GpStatus GDIPAPI GdipSetSolidFillColor(GpBrush *brush, ARGB color)
{
	if (!brush || brush->type != BrushTypeSolidColor)
		return InvalidParameter;
	brush->color = color;
	return Ok;
}

GpStatus GDIPAPI GdipCreateHatchBrush(INT style, ARGB fore, ARGB back, GpBrush **out)
{
	if (!out || style < 0 || style >= 53)
		return InvalidParameter;
	GpBrush *brush = new_brush(BrushTypeHatchFill);
	if (!brush)
		return OutOfMemory;
	brush->hatch = style;
	brush->color = fore;
	brush->back = back;
	*out = brush;
	return Ok;
}

GpStatus GDIPAPI GdipGetHatchStyle(GpBrush *brush, INT *out)
{
	if (!brush || !out)
		return InvalidParameter;
	*out = brush->hatch;
	return Ok;
}

GpStatus GDIPAPI GdipGetHatchForegroundColor(GpBrush *brush, ARGB *out)
{
	if (!brush || !out)
		return InvalidParameter;
	*out = brush->color;
	return Ok;
}

GpStatus GDIPAPI GdipGetHatchBackgroundColor(GpBrush *brush, ARGB *out)
{
	if (!brush || !out)
		return InvalidParameter;
	*out = brush->back;
	return Ok;
}

// Line gradients: the transform carries the unit gradient (rect) onto the drawn one, so the rectangle stays what the
// caller gave and rotation shows up only in the matrix, which is where programs read it back.
// The brush's own space runs the gradient along the rectangle's width. Its starting transform carries the left and right
// middles of the rectangle onto the two ends, and (cx, cy) is where one unit of height goes.
static void line_matrix(GpBrush *brush, REAL cx, REAL cy)
{
	const GpRectF *r = &brush->rect;
	REAL a = (brush->end.X - brush->start.X) / r->Width, d = (brush->end.Y - brush->start.Y) / r->Width;
	REAL middle = r->Y + r->Height / 2.0f;
	GpMatrix *m = &brush->transform;
	m->m[0] = a;
	m->m[1] = d;
	m->m[2] = cx;
	m->m[3] = cy;
	m->m[4] = brush->start.X - a * r->X - cx * middle;
	m->m[5] = brush->start.Y - d * r->X - cy * middle;
}

static GpStatus make_line(const GpPointF *start, const GpPointF *end, ARGB c1, ARGB c2, GpWrapMode wrap, GpBrush **out)
{
	if (!start || !end || !out)
		return InvalidParameter;
	if (wrap == WrapModeClamp)
		return InvalidParameter;
	if (start->X == end->X && start->Y == end->Y)
		return OutOfMemory;
	GpBrush *brush = new_brush(BrushTypeLinearGradient);
	if (!brush)
		return OutOfMemory;
	brush->start = *start;
	brush->end = *end;
	brush->colors[0] = c1;
	brush->colors[1] = c2;
	brush->wrap = wrap;
	REAL dx = end->X - start->X, dy = end->Y - start->Y;
	brush->rect.X = dx < 0 ? end->X : start->X;
	brush->rect.Y = dy < 0 ? end->Y : start->Y;
	brush->rect.Width = gp_fabs(dx);
	brush->rect.Height = gp_fabs(dy);
	if (brush->rect.Height == 0.0f) {
		brush->rect.Y -= brush->rect.Width / 2.0f;
		brush->rect.Height = brush->rect.Width;
	} else if (brush->rect.Width == 0.0f) {
		brush->rect.X -= brush->rect.Height / 2.0f;
		brush->rect.Width = brush->rect.Height;
	}
	if (dx && dy) {
		// Height spans the box measured across the line: the corners off the diagonal lie that far from it.
		REAL k = 2.0f * dx * dx / (dx * dx + dy * dy);
		line_matrix(brush, -dy / gp_fabs(dx) * k, dx / gp_fabs(dx) * k);
	} else {
		line_matrix(brush, -dy / brush->rect.Width, dx / brush->rect.Width);
	}
	brush->blend_factors = gp_alloc(sizeof(REAL));
	brush->blend_positions = gp_alloc(sizeof(REAL));
	if (!brush->blend_factors || !brush->blend_positions) {
		gp_brush_delete(brush);
		return OutOfMemory;
	}
	brush->blend_factors[0] = 1.0f;
	brush->blend_positions[0] = 1.0f;
	brush->blend_count = 1;
	*out = brush;
	return Ok;
}

GpStatus GDIPAPI GdipCreateLineBrush(const GpPointF *start, const GpPointF *end, ARGB c1, ARGB c2, GpWrapMode wrap, GpBrush **out)
{
	return make_line(start, end, c1, c2, wrap, out);
}

GpStatus GDIPAPI GdipCreateLineBrushI(const GpPoint *start, const GpPoint *end, ARGB c1, ARGB c2, GpWrapMode wrap, GpBrush **out)
{
	if (!start || !end)
		return InvalidParameter;
	GpPointF a = {(REAL)start->X, (REAL)start->Y}, b = {(REAL)end->X, (REAL)end->Y};
	return make_line(&a, &b, c1, c2, wrap, out);
}

GpStatus GDIPAPI GdipCreateLineBrushFromRectWithAngle(const GpRectF *rect, ARGB c1, ARGB c2, REAL angle, BOOL scalable, GpWrapMode wrap, GpBrush **out)
{
	if (!rect || !out || wrap == WrapModeClamp)
		return InvalidParameter;
	if (!rect->Width || !rect->Height)
		return OutOfMemory;
	// The gradient line crosses the rectangle's middle; its length reaches the corners the angle points at. A scalable angle
	// is measured on the rectangle squared up, so it follows the rectangle's own proportions.
	REAL radians = gp_deg2rad(angle);
	REAL c = gp_cosf(radians), s = gp_sinf(radians);
	REAL w = rect->Width, h = rect->Height;
	if (gp_fabs(c) < 1e-6f)
		c = 0.0f;
	if (gp_fabs(s) < 1e-6f)
		s = 0.0f;
	if (scalable) {
		REAL scaled = (REAL)atan2((double)(s * gp_fabs(w)), (double)(c * gp_fabs(h)));
		c = gp_cosf(scaled);
		s = gp_sinf(scaled);
		if (gp_fabs(c) < 1e-6f)
			c = 0.0f;
		if (gp_fabs(s) < 1e-6f)
			s = 0.0f;
	}
	REAL half = (gp_fabs(w * c) + gp_fabs(h * s)) / 2.0f, across = gp_fabs(w * s) + gp_fabs(h * c);
	REAL cx = rect->X + w / 2.0f, cy = rect->Y + h / 2.0f;
	GpPointF a = {cx - c * half, cy - s * half}, b = {cx + c * half, cy + s * half};
	GpStatus status = make_line(&a, &b, c1, c2, wrap, out);
	if (status == Ok) {
		(*out)->rect = *rect;
		if (!s && c > 0.0f)
			gp_matrix_identity(&(*out)->transform);
		else
			line_matrix(*out, -s * across / h, c * across / h);
	}
	return status;
}

GpStatus GDIPAPI GdipCreateLineBrushFromRectWithAngleI(const GpRect *rect, ARGB c1, ARGB c2, REAL angle, BOOL scalable, GpWrapMode wrap, GpBrush **out)
{
	if (!rect)
		return InvalidParameter;
	GpRectF box = {(REAL)rect->X, (REAL)rect->Y, (REAL)rect->Width, (REAL)rect->Height};
	return GdipCreateLineBrushFromRectWithAngle(&box, c1, c2, angle, scalable, wrap, out);
}

GpStatus GDIPAPI GdipCreateLineBrushFromRect(const GpRectF *rect, ARGB c1, ARGB c2, LinearGradientMode mode, GpWrapMode wrap, GpBrush **out)
{
	static const REAL angles[] = {0.0f, 90.0f, 45.0f, 135.0f};
	if (!rect || !out || mode < 0 || mode > LinearGradientModeBackwardDiagonal)
		return InvalidParameter;
	return GdipCreateLineBrushFromRectWithAngle(rect, c1, c2, angles[mode], TRUE, wrap, out);
}

GpStatus GDIPAPI GdipCreateLineBrushFromRectI(const GpRect *rect, ARGB c1, ARGB c2, LinearGradientMode mode, GpWrapMode wrap, GpBrush **out)
{
	if (!rect)
		return InvalidParameter;
	GpRectF box = {(REAL)rect->X, (REAL)rect->Y, (REAL)rect->Width, (REAL)rect->Height};
	return GdipCreateLineBrushFromRect(&box, c1, c2, mode, wrap, out);
}

#define LINE(brush) ((brush) && (brush)->type == BrushTypeLinearGradient)
#define PATH(brush) ((brush) && (brush)->type == BrushTypePathGradient)
#define TEXTURE(brush) ((brush) && (brush)->type == BrushTypeTextureFill)

GpStatus GDIPAPI GdipGetLineColors(GpBrush *brush, ARGB *out)
{
	if (!LINE(brush) || !out)
		return InvalidParameter;
	out[0] = brush->colors[0];
	out[1] = brush->colors[1];
	return Ok;
}

GpStatus GDIPAPI GdipSetLineColors(GpBrush *brush, ARGB c1, ARGB c2)
{
	if (!LINE(brush))
		return InvalidParameter;
	brush->colors[0] = c1;
	brush->colors[1] = c2;
	return Ok;
}

GpStatus GDIPAPI GdipGetLineRect(GpBrush *brush, GpRectF *out)
{
	if (!LINE(brush) || !out)
		return InvalidParameter;
	*out = brush->rect;
	return Ok;
}

GpStatus GDIPAPI GdipGetLineRectI(GpBrush *brush, GpRect *out)
{
	if (!out)
		return InvalidParameter;
	GpRectF box;
	GpStatus status = GdipGetLineRect(brush, &box);
	if (status == Ok) {
		out->X = (INT)gp_floor(box.X + 0.5f);
		out->Y = (INT)gp_floor(box.Y + 0.5f);
		out->Width = (INT)gp_floor(box.Width + 0.5f);
		out->Height = (INT)gp_floor(box.Height + 0.5f);
	}
	return status;
}

GpStatus GDIPAPI GdipGetLineGammaCorrection(GpBrush *brush, BOOL *out)
{
	if (!LINE(brush) || !out)
		return InvalidParameter;
	*out = brush->gamma;
	return Ok;
}

GpStatus GDIPAPI GdipSetLineGammaCorrection(GpBrush *brush, BOOL gamma)
{
	if (!LINE(brush))
		return InvalidParameter;
	brush->gamma = gamma;
	return Ok;
}

GpStatus GDIPAPI GdipGetLineWrapMode(GpBrush *brush, GpWrapMode *out)
{
	if (!LINE(brush) || !out)
		return InvalidParameter;
	*out = brush->wrap;
	return Ok;
}

GpStatus GDIPAPI GdipSetLineWrapMode(GpBrush *brush, GpWrapMode wrap)
{
	if (!LINE(brush) || wrap == WrapModeClamp)
		return InvalidParameter;
	brush->wrap = wrap;
	return Ok;
}

static GpStatus set_blend(GpBrush *brush, const REAL *factors, const REAL *positions, INT count)
{
	REAL *f = gp_dup(factors, count * sizeof(REAL)), *p = gp_dup(positions, count * sizeof(REAL));
	if (!f || !p) {
		gp_free(f);
		gp_free(p);
		return OutOfMemory;
	}
	gp_free(brush->blend_factors);
	gp_free(brush->blend_positions);
	brush->blend_factors = f;
	brush->blend_positions = p;
	brush->blend_count = count;
	return Ok;
}

static GpStatus get_blend(const GpBrush *brush, REAL *factors, REAL *positions, INT count)
{
	if (!factors || !positions || count <= 0)
		return InvalidParameter;
	if (count < brush->blend_count)
		return InsufficientBuffer;
	if (brush->blend_count) {
		memcpy(factors, brush->blend_factors, brush->blend_count * sizeof(REAL));
		memcpy(positions, brush->blend_positions, brush->blend_count * sizeof(REAL));
	}
	return Ok;
}

GpStatus GDIPAPI GdipSetLineBlend(GpBrush *brush, const REAL *factors, const REAL *positions, INT count)
{
	if (!LINE(brush) || !factors || !positions || count <= 0)
		return InvalidParameter;
	if (count >= 2 && (positions[0] != 0.0f || positions[count - 1] != 1.0f))
		return InvalidParameter;
	return set_blend(brush, factors, positions, count);
}

GpStatus GDIPAPI GdipGetLineBlend(GpBrush *brush, REAL *factors, REAL *positions, INT count)
{
	return LINE(brush) ? get_blend(brush, factors, positions, count) : InvalidParameter;
}

GpStatus GDIPAPI GdipGetLineBlendCount(GpBrush *brush, INT *out)
{
	if (!LINE(brush) || !out)
		return InvalidParameter;
	*out = brush->blend_count;
	return Ok;
}

static GpStatus set_preset(GpBrush *brush, const ARGB *colors, const REAL *positions, INT count)
{
	if (!colors || !positions || count < 2)
		return InvalidParameter;
	if (positions[0] != 0.0f || positions[count - 1] != 1.0f)
		return InvalidParameter;
	ARGB *c = gp_dup(colors, count * sizeof(ARGB));
	REAL *p = gp_dup(positions, count * sizeof(REAL));
	if (!c || !p) {
		gp_free(c);
		gp_free(p);
		return OutOfMemory;
	}
	gp_free(brush->preset_colors);
	gp_free(brush->preset_positions);
	brush->preset_colors = c;
	brush->preset_positions = p;
	brush->preset_count = count;
	return Ok;
}

static GpStatus get_preset(const GpBrush *brush, ARGB *colors, REAL *positions, INT count)
{
	if (!colors || !positions)
		return InvalidParameter;
	if (count <= 1)
		return InvalidParameter;
	if (!brush->preset_count)
		return GenericError;
	if (count < brush->preset_count)
		return InsufficientBuffer;
	memcpy(colors, brush->preset_colors, brush->preset_count * sizeof(ARGB));
	memcpy(positions, brush->preset_positions, brush->preset_count * sizeof(REAL));
	return Ok;
}

GpStatus GDIPAPI GdipSetLinePresetBlend(GpBrush *brush, const ARGB *colors, const REAL *positions, INT count)
{
	return LINE(brush) ? set_preset(brush, colors, positions, count) : InvalidParameter;
}

GpStatus GDIPAPI GdipGetLinePresetBlend(GpBrush *brush, ARGB *colors, REAL *positions, INT count)
{
	return LINE(brush) ? get_preset(brush, colors, positions, count) : InvalidParameter;
}

GpStatus GDIPAPI GdipGetLinePresetBlendCount(GpBrush *brush, INT *out)
{
	if (!LINE(brush) || !out)
		return InvalidParameter;
	*out = brush->preset_count;
	return Ok;
}

// A triangle: nothing at the ends, the scale at the focus.
static GpStatus linear_blend(GpBrush *brush, REAL focus, REAL scale)
{
	REAL factors[3], positions[3];
	INT count = 0;
	if (focus != 0.0f) {
		factors[count] = 0.0f;
		positions[count++] = 0.0f;
	}
	factors[count] = scale;
	positions[count++] = focus;
	if (focus != 1.0f) {
		factors[count] = 0.0f;
		positions[count++] = 1.0f;
	}
	return set_blend(brush, factors, positions, count);
}

GpStatus GDIPAPI GdipSetLineLinearBlend(GpBrush *brush, REAL focus, REAL scale)
{
	return LINE(brush) ? linear_blend(brush, focus, scale) : InvalidParameter;
}

// A bell: an error function curve rising to the focus and falling after it, sampled at fixed steps.
static double erf_approx(double x)
{
	double t = 1.0 / (1.0 + 0.5 * (x < 0 ? -x : x));
	double y = t * pow(2.71828182845904523536, -x * x - 1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418
		+ t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))));
	return x >= 0 ? 1.0 - y : y - 1.0;
}

static GpStatus sigma_blend(GpBrush *brush, REAL focus, REAL scale)
{
	if (focus < 0.0f || focus > 1.0f || scale < 0.0f || scale > 1.0f)
		return InvalidParameter;
	enum { steps = 15 };
	REAL factors[steps * 2 + 3], positions[steps * 2 + 3];
	const double spread = 2.0;
	INT count = 0;
	if (focus != 0.0f) {
		double low = erf_approx(0.0 - spread), range = erf_approx(spread) - low;
		for (INT i = 0; i <= steps; i++) {
			REAL at = focus * i / steps;
			positions[count] = at;
			factors[count++] = (REAL)(scale * (erf_approx(spread * (2.0 * i / steps - 1.0)) - low) / range);
		}
	}
	if (focus != 1.0f) {
		double low = erf_approx(0.0 - spread), range = erf_approx(spread) - low;
		for (INT i = focus != 0.0f ? 1 : 0; i <= steps; i++) {
			positions[count] = focus + (1.0f - focus) * i / steps;
			factors[count++] = (REAL)(scale * (erf_approx(spread * (1.0 - 2.0 * i / steps)) - low) / range);
		}
	}
	return set_blend(brush, factors, positions, count);
}

GpStatus GDIPAPI GdipSetLineSigmaBlend(GpBrush *brush, REAL focus, REAL scale)
{
	return LINE(brush) ? sigma_blend(brush, focus, scale) : InvalidParameter;
}

#define TRANSFORM_API(stem, check) \
	GpStatus GDIPAPI GdipGet##stem##Transform(GpBrush *brush, GpMatrix *out) \
	{ \
		if (!check(brush) || !out) \
			return InvalidParameter; \
		*out = brush->transform; \
		return Ok; \
	} \
	GpStatus GDIPAPI GdipSet##stem##Transform(GpBrush *brush, const GpMatrix *matrix) \
	{ \
		if (!check(brush) || !matrix) \
			return InvalidParameter; \
		brush->transform = *matrix; \
		return Ok; \
	} \
	GpStatus GDIPAPI GdipReset##stem##Transform(GpBrush *brush) \
	{ \
		if (!check(brush)) \
			return InvalidParameter; \
		gp_matrix_identity(&brush->transform); \
		return Ok; \
	} \
	GpStatus GDIPAPI GdipMultiply##stem##Transform(GpBrush *brush, const GpMatrix *matrix, GpMatrixOrder order) \
	{ \
		if (!check(brush)) \
			return InvalidParameter; \
		return matrix ? GdipMultiplyMatrix(&brush->transform, matrix, order) : Ok; \
	} \
	GpStatus GDIPAPI GdipTranslate##stem##Transform(GpBrush *brush, REAL x, REAL y, GpMatrixOrder order) \
	{ \
		return check(brush) ? GdipTranslateMatrix(&brush->transform, x, y, order) : InvalidParameter; \
	} \
	GpStatus GDIPAPI GdipScale##stem##Transform(GpBrush *brush, REAL x, REAL y, GpMatrixOrder order) \
	{ \
		return check(brush) ? GdipScaleMatrix(&brush->transform, x, y, order) : InvalidParameter; \
	} \
	GpStatus GDIPAPI GdipRotate##stem##Transform(GpBrush *brush, REAL angle, GpMatrixOrder order) \
	{ \
		return check(brush) ? GdipRotateMatrix(&brush->transform, angle, order) : InvalidParameter; \
	}

TRANSFORM_API(Line, LINE)
TRANSFORM_API(Texture, TEXTURE)
TRANSFORM_API(PathGradient, PATH)

static GpStatus make_path_gradient(GpPath *path, GpBrush **out)
{
	if (path->count < 2 && !path->count)
		return OutOfMemory;
	GpBrush *brush = new_brush(BrushTypePathGradient);
	if (!brush)
		return OutOfMemory;
	brush->path = path;
	brush->center_color = 0xffffffff;
	brush->colors[1] = 0xffffffff;
	brush->wrap = WrapModeClamp;
	brush->surround = gp_alloc(sizeof(ARGB));
	brush->blend_factors = gp_alloc(sizeof(REAL));
	brush->blend_positions = gp_alloc(sizeof(REAL));
	if (!brush->surround || !brush->blend_factors || !brush->blend_positions) {
		brush->path = NULL;
		gp_brush_delete(brush);
		return OutOfMemory;
	}
	brush->surround[0] = 0xffffffff;
	brush->surround_count = 1;
	brush->blend_factors[0] = 1.0f;
	brush->blend_positions[0] = 1.0f;
	brush->blend_count = 1;
	// The centre defaults to the mean of the path's own points; the bounds come from the flattened figure.
	GpPath *flat = gp_path_clone(path);
	if (flat && gp_path_flatten(flat, NULL, 0.25f) == Ok) {
		REAL x = 0.0f, y = 0.0f;
		for (INT i = 0; i < path->count; i++) {
			x += path->points[i].X;
			y += path->points[i].Y;
		}
		brush->center.X = x / path->count;
		brush->center.Y = y / path->count;
		gp_path_bounds(flat, &brush->rect);
	}
	if (flat)
		gp_path_delete(flat);
	*out = brush;
	return Ok;
}

GpStatus GDIPAPI GdipCreatePathGradient(const GpPointF *points, INT count, GpWrapMode wrap, GpBrush **out)
{
	if (!points || !out)
		return InvalidParameter;
	if (count <= 0)
		return OutOfMemory;
	if ((UINT)wrap > WrapModeClamp)
		return InvalidParameter;
	if (count == 1)
		return OutOfMemory;
	GpPath *path;
	if (gp_path_new(FillModeAlternate, &path) != Ok)
		return OutOfMemory;
	if (!gp_path_reserve(path, count)) {
		gp_path_delete(path);
		return OutOfMemory;
	}
	gp_path_push(path, points[0].X, points[0].Y, PathPointTypeStart);
	for (INT i = 1; i < count; i++)
		gp_path_push(path, points[i].X, points[i].Y, PathPointTypeLine);
	GpStatus status = make_path_gradient(path, out);
	if (status != Ok)
		gp_path_delete(path);
	else
		(*out)->wrap = wrap;
	return status;
}

GpStatus GDIPAPI GdipCreatePathGradientI(const GpPoint *points, INT count, GpWrapMode wrap, GpBrush **out)
{
	if (!points || !out)
		return InvalidParameter;
	if (count <= 0)
		return OutOfMemory;
	GpPointF *floats = gp_alloc(count * sizeof(GpPointF));
	if (!floats)
		return OutOfMemory;
	for (INT i = 0; i < count; i++) {
		floats[i].X = (REAL)points[i].X;
		floats[i].Y = (REAL)points[i].Y;
	}
	GpStatus status = GdipCreatePathGradient(floats, count, wrap, out);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipCreatePathGradientFromPath(const GpPath *path, GpBrush **out)
{
	if (!out)
		return InvalidParameter;
	if (!path)
		return OutOfMemory;
	if (!path->count)
		return OutOfMemory;
	GpPath *copy = gp_path_clone(path);
	if (!copy)
		return OutOfMemory;
	GpStatus status = make_path_gradient(copy, out);
	if (status != Ok)
		gp_path_delete(copy);
	return status;
}

GpStatus GDIPAPI GdipGetPathGradientCenterColor(GpBrush *brush, ARGB *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->center_color;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientCenterColor(GpBrush *brush, ARGB color)
{
	if (!PATH(brush))
		return InvalidParameter;
	brush->center_color = color;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientCenterPoint(GpBrush *brush, GpPointF *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->center;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientCenterPointI(GpBrush *brush, GpPoint *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	out->X = (INT)gp_floor(brush->center.X + 0.5f);
	out->Y = (INT)gp_floor(brush->center.Y + 0.5f);
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientCenterPoint(GpBrush *brush, GpPointF *point)
{
	if (!PATH(brush) || !point)
		return InvalidParameter;
	brush->center = *point;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientCenterPointI(GpBrush *brush, GpPoint *point)
{
	if (!PATH(brush) || !point)
		return InvalidParameter;
	brush->center.X = (REAL)point->X;
	brush->center.Y = (REAL)point->Y;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientFocusScales(GpBrush *brush, REAL *x, REAL *y)
{
	if (!PATH(brush) || !x || !y)
		return InvalidParameter;
	*x = brush->focus_x;
	*y = brush->focus_y;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientFocusScales(GpBrush *brush, REAL x, REAL y)
{
	if (!PATH(brush))
		return InvalidParameter;
	brush->focus_x = x;
	brush->focus_y = y;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientGammaCorrection(GpBrush *brush, BOOL *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->gamma;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientGammaCorrection(GpBrush *brush, BOOL gamma)
{
	if (!PATH(brush))
		return InvalidParameter;
	brush->gamma = gamma;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientWrapMode(GpBrush *brush, GpWrapMode *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->wrap;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientWrapMode(GpBrush *brush, GpWrapMode wrap)
{
	if (!PATH(brush))
		return InvalidParameter;
	brush->wrap = wrap;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientPointCount(GpBrush *brush, INT *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->path->count;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientSurroundColorCount(GpBrush *brush, INT *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->path->count;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientSurroundColorsWithCount(GpBrush *brush, ARGB *colors, INT *count)
{
	if (!PATH(brush) || !colors || !count)
		return InvalidParameter;
	if (*count < brush->path->count)
		return InvalidParameter;
	for (INT i = 0; i < *count && i < brush->path->count; i++)
		colors[i] = brush->surround[i < brush->surround_count ? i : brush->surround_count - 1];
	*count = brush->surround_count;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientSurroundColorsWithCount(GpBrush *brush, const ARGB *colors, INT *count)
{
	if (!PATH(brush) || !colors || !count || *count <= 0 || *count > brush->path->count)
		return InvalidParameter;
	ARGB *copy = gp_dup(colors, *count * sizeof(ARGB));
	if (!copy)
		return OutOfMemory;
	gp_free(brush->surround);
	brush->surround = copy;
	// One colour all around reads back as a single colour.
	INT kept = 1;
	for (INT i = 1; i < *count; i++) {
		if (copy[i] != copy[0])
			kept = *count;
	}
	brush->surround_count = kept;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientRect(GpBrush *brush, GpRectF *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->rect;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientRectI(GpBrush *brush, GpRect *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	out->X = (INT)gp_floor(brush->rect.X + 0.5f);
	out->Y = (INT)gp_floor(brush->rect.Y + 0.5f);
	out->Width = (INT)gp_floor(brush->rect.Width + 0.5f);
	out->Height = (INT)gp_floor(brush->rect.Height + 0.5f);
	return Ok;
}

GpStatus GDIPAPI GdipGetPathGradientPath(GpBrush *brush, GpPath *out)
{
	(void)brush;
	(void)out;
	return NotImplemented;
}

GpStatus GDIPAPI GdipSetPathGradientPath(GpBrush *brush, const GpPath *path)
{
	(void)brush;
	(void)path;
	return NotImplemented;
}

GpStatus GDIPAPI GdipSetPathGradientBlend(GpBrush *brush, const REAL *factors, const REAL *positions, INT count)
{
	if (!PATH(brush) || !factors || !positions || count <= 0)
		return InvalidParameter;
	if (count >= 2 && (positions[0] != 0.0f || positions[count - 1] != 1.0f))
		return InvalidParameter;
	return set_blend(brush, factors, positions, count);
}

GpStatus GDIPAPI GdipGetPathGradientBlend(GpBrush *brush, REAL *factors, REAL *positions, INT count)
{
	if (!PATH(brush))
		return InvalidParameter;
	// A blend of one has no position to report.
	if (brush->blend_count == 1 && factors && positions && count > 0) {
		factors[0] = brush->blend_factors[0];
		return Ok;
	}
	return get_blend(brush, factors, positions, count);
}

GpStatus GDIPAPI GdipGetPathGradientBlendCount(GpBrush *brush, INT *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->blend_count;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientPresetBlend(GpBrush *brush, const ARGB *colors, const REAL *positions, INT count)
{
	return PATH(brush) ? set_preset(brush, colors, positions, count) : InvalidParameter;
}

GpStatus GDIPAPI GdipGetPathGradientPresetBlend(GpBrush *brush, ARGB *colors, REAL *positions, INT count)
{
	if (count < 0)
		return OutOfMemory;
	return PATH(brush) ? get_preset(brush, colors, positions, count) : InvalidParameter;
}

GpStatus GDIPAPI GdipGetPathGradientPresetBlendCount(GpBrush *brush, INT *out)
{
	if (!PATH(brush) || !out)
		return InvalidParameter;
	*out = brush->preset_count;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathGradientLinearBlend(GpBrush *brush, REAL focus, REAL scale)
{
	return PATH(brush) ? linear_blend(brush, focus, scale) : InvalidParameter;
}

GpStatus GDIPAPI GdipSetPathGradientSigmaBlend(GpBrush *brush, REAL focus, REAL scale)
{
	return PATH(brush) ? sigma_blend(brush, focus, scale) : InvalidParameter;
}

GpStatus GDIPAPI GdipCreateTexture2(struct gp_image *image, GpWrapMode wrap, REAL x, REAL y, REAL width, REAL height, GpBrush **out)
{
	if (!image || !out || (UINT)wrap > WrapModeClamp)
		return InvalidParameter;
	GpRectF area = {x, y, width, height}, bounds;
	if (!gp_image_bounds(image, &bounds))
		return InvalidParameter;
	if (x < 0.0f || y < 0.0f || width <= 0.0f || height <= 0.0f || x + width > bounds.Width || y + height > bounds.Height)
		return OutOfMemory;
	GpBrush *brush = new_brush(BrushTypeTextureFill);
	if (!brush)
		return OutOfMemory;
	if (!(brush->image = gp_image_crop(image, &area))) {
		gp_free(brush);
		return OutOfMemory;
	}
	brush->wrap = wrap;
	brush->rect.Width = width;
	brush->rect.Height = height;
	*out = brush;
	return Ok;
}

GpStatus GDIPAPI GdipCreateTexture(struct gp_image *image, GpWrapMode wrap, GpBrush **out)
{
	GpRectF bounds;
	if (!image || !out)
		return InvalidParameter;
	if (!gp_image_bounds(image, &bounds))
		return InvalidParameter;
	return GdipCreateTexture2(image, wrap, 0.0f, 0.0f, bounds.Width, bounds.Height, out);
}

GpStatus GDIPAPI GdipCreateTexture2I(struct gp_image *image, GpWrapMode wrap, INT x, INT y, INT width, INT height, GpBrush **out)
{
	return GdipCreateTexture2(image, wrap, (REAL)x, (REAL)y, (REAL)width, (REAL)height, out);
}

GpStatus GDIPAPI GdipCreateTextureIA(struct gp_image *image, const void *attributes, REAL x, REAL y, REAL width, REAL height, GpBrush **out)
{
	GpStatus status = GdipCreateTexture2(image, WrapModeTile, x, y, width, height, out);
	if (status == Ok)
		(*out)->attributes = (void *)attributes;
	return status;
}

GpStatus GDIPAPI GdipCreateTextureIAI(struct gp_image *image, const void *attributes, INT x, INT y, INT width, INT height, GpBrush **out)
{
	return GdipCreateTextureIA(image, attributes, (REAL)x, (REAL)y, (REAL)width, (REAL)height, out);
}

GpStatus GDIPAPI GdipGetTextureImage(GpBrush *brush, struct gp_image **out)
{
	extern GpStatus GDIPAPI GdipCloneImage(struct gp_image *, struct gp_image **);
	if (!TEXTURE(brush) || !out)
		return InvalidParameter;
	return GdipCloneImage(brush->image, out);
}

GpStatus GDIPAPI GdipGetTextureWrapMode(GpBrush *brush, GpWrapMode *out)
{
	if (!TEXTURE(brush) || !out)
		return InvalidParameter;
	*out = brush->wrap;
	return Ok;
}

GpStatus GDIPAPI GdipSetTextureWrapMode(GpBrush *brush, GpWrapMode wrap)
{
	if (!TEXTURE(brush))
		return InvalidParameter;
	brush->wrap = wrap;
	return Ok;
}

static ARGB mix(ARGB a, ARGB b, REAL t)
{
	ARGB out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		REAL ca = (REAL)((a >> shift) & 0xff), cb = (REAL)((b >> shift) & 0xff);
		out |= (ARGB)(INT)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return out;
}

// Position along a gradient to colour, through the preset table or the blend table.
static ARGB gradient_color(const GpBrush *brush, ARGB from, ARGB to, REAL t)
{
	if (brush->preset_count) {
		for (INT i = 1; i < brush->preset_count; i++) {
			if (t <= brush->preset_positions[i]) {
				REAL span = brush->preset_positions[i] - brush->preset_positions[i - 1];
				return mix(brush->preset_colors[i - 1], brush->preset_colors[i], span > 0.0f ? (t - brush->preset_positions[i - 1]) / span : 0.0f);
			}
		}
		return brush->preset_colors[brush->preset_count - 1];
	}
	REAL factor = t;
	if (brush->blend_count > 1) {
		factor = brush->blend_factors[brush->blend_count - 1];
		for (INT i = 1; i < brush->blend_count; i++) {
			if (t <= brush->blend_positions[i]) {
				REAL span = brush->blend_positions[i] - brush->blend_positions[i - 1];
				REAL part = span > 0.0f ? (t - brush->blend_positions[i - 1]) / span : 0.0f;
				factor = brush->blend_factors[i - 1] + (brush->blend_factors[i] - brush->blend_factors[i - 1]) * part;
				break;
			}
		}
	}
	return mix(from, to, factor);
}

// Folds a coordinate into one tile; a flip mirrors every other tile.
static REAL wrap_axis(REAL value, REAL size, BOOL flip, BOOL *outside, BOOL clamp)
{
	if (clamp) {
		*outside = value < 0.0f || value >= size;
		return value;
	}
	REAL cell = gp_floor(value / size);
	REAL inside = value - cell * size;
	if (flip && ((INT)cell & 1))
		inside = size - inside;
	return inside;
}

// Eight rows of eight two-bit cells: how much of the foreground each pixel of the pattern shows.
static const WORD hatch_rows[][8] =
{
	{0xffff, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},	// Horizontal
	{0xc000, 0xc000, 0xc000, 0xc000, 0xc000, 0xc000, 0xc000, 0xc000},	// Vertical
	{0x9001, 0x6400, 0x1900, 0x0640, 0x0190, 0x0064, 0x0019, 0x4006},	// ForwardDiagonal
	{0x4006, 0x0019, 0x0064, 0x0190, 0x0640, 0x1900, 0x6400, 0x9001},	// BackwardDiagonal
	{0xffff, 0xc000, 0xc000, 0xc000, 0xc000, 0xc000, 0xc000, 0xc000},	// Cross
	{0x9006, 0x6419, 0x1964, 0x0690, 0x0690, 0x1964, 0x6419, 0x9006},	// DiagonalCross
	{0xc000, 0x0000, 0x0000, 0x0000, 0x00c0, 0x0000, 0x0000, 0x0000},	// 05Percent
	{0xc000, 0x0000, 0x00c0, 0x0000, 0xc000, 0x0000, 0x00c0, 0x0000},	// 10Percent
	{0xc0c0, 0x0000, 0x0c0c, 0x0000, 0xc0c0, 0x0000, 0x0c0c, 0x0000},	// 20Percent
	{0xc0c0, 0x0c0c, 0xc0c0, 0x0c0c, 0xc0c0, 0x0c0c, 0xc0c0, 0x0c0c},	// 25Percent
	{0xcccc, 0x3030, 0xcccc, 0x0303, 0xcccc, 0x3030, 0xcccc, 0x0303},	// 30Percent
	{0xcccc, 0x3333, 0xcccc, 0x3303, 0xcccc, 0x3333, 0xcccc, 0x0333},	// 40Percent
	{0xcccc, 0x3333, 0xcccc, 0x3333, 0xcccc, 0x3333, 0xcccc, 0x3333},	// 50Percent
	{0xfcfc, 0x3333, 0xcfcf, 0x3333, 0xfcfc, 0x3333, 0xcfcf, 0x3333},	// 60Percent
	{0x3f3f, 0xf3f3, 0x3f3f, 0xf3f3, 0x3f3f, 0xf3f3, 0x3f3f, 0xf3f3},	// 70Percent
	{0x3f3f, 0xffff, 0xf3f3, 0xffff, 0x3f3f, 0xffff, 0xf3f3, 0xffff},	// 75Percent
	{0xfcff, 0xffff, 0xfffc, 0xffff, 0xfcff, 0xffff, 0xfffc, 0xffff},	// 80Percent
	{0xffff, 0xffff, 0xffff, 0xff3f, 0xffff, 0xffff, 0xffff, 0x3fff},	// 90Percent
	{0xc0c0, 0x3030, 0x0c0c, 0x0303, 0xc0c0, 0x3030, 0x0c0c, 0x0303},	// LightDownwardDiagonal
	{0x0303, 0x0c0c, 0x3030, 0xc0c0, 0x0303, 0x0c0c, 0x3030, 0xc0c0},	// LightUpwardDiagonal
	{0xf0f0, 0x3c3c, 0x0f0f, 0xc3c3, 0xf0f0, 0x3c3c, 0x0f0f, 0xc3c3},	// DarkDownwardDiagonal
	{0x0f0f, 0x3c3c, 0xf0f0, 0xc3c3, 0x0f0f, 0x3c3c, 0xf0f0, 0xc3c3},	// DarkUpwardDiagonal
	{0xf003, 0xfc00, 0x3f00, 0x0fc0, 0x03f0, 0x00fc, 0x003f, 0xc00f},	// WideDownwardDiagonal
	{0xc00f, 0x003f, 0x00fc, 0x03f0, 0x0fc0, 0x3f00, 0xfc00, 0xf003},	// WideUpwardDiagonal
	{0xc0c0, 0xc0c0, 0xc0c0, 0xc0c0, 0xc0c0, 0xc0c0, 0xc0c0, 0xc0c0},	// LightVertical
	{0xffff, 0x0000, 0x0000, 0x0000, 0xffff, 0x0000, 0x0000, 0x0000},	// LightHorizontal
	{0x3333, 0x3333, 0x3333, 0x3333, 0x3333, 0x3333, 0x3333, 0x3333},	// NarrowVertical
	{0xffff, 0x0000, 0xffff, 0x0000, 0xffff, 0x0000, 0xffff, 0x0000},	// NarrowHorizontal
	{0xf0f0, 0xf0f0, 0xf0f0, 0xf0f0, 0xf0f0, 0xf0f0, 0xf0f0, 0xf0f0},	// DarkVertical
	{0xffff, 0xffff, 0x0000, 0x0000, 0xffff, 0xffff, 0x0000, 0x0000},	// DarkHorizontal
	{0x0000, 0x0000, 0xc0c0, 0x3030, 0x0c0c, 0x0303, 0x0000, 0x0000},	// DashedDownwardDiagonal
	{0x0000, 0x0000, 0x0303, 0x0c0c, 0x3030, 0xc0c0, 0x0000, 0x0000},	// DashedUpwardDiagonal
	{0xff00, 0x0000, 0x0000, 0x0000, 0x00ff, 0x0000, 0x0000, 0x0000},	// DashedHorizontal
	{0xc000, 0xc000, 0xc000, 0xc000, 0x00c0, 0x00c0, 0x00c0, 0x00c0},	// DashedVertical
	{0xc000, 0x00c0, 0x3000, 0x000c, 0x0300, 0x0003, 0x0c00, 0x0030},	// SmallConfetti
	{0xcf03, 0x0f00, 0x000f, 0x03cf, 0xf3c0, 0xf000, 0x00f0, 0xc0f3},	// LargeConfetti
	{0xc003, 0x300c, 0x0c30, 0x03c0, 0xc003, 0x300c, 0x0c30, 0x03c0},	// ZigZag
	{0x0000, 0x03c0, 0x0c33, 0xf000, 0x0000, 0x03c0, 0x0c33, 0xf000},	// Wave
	{0x0003, 0x000c, 0x0030, 0x00c0, 0x03c0, 0x0c30, 0x300c, 0xc003},	// DiagonalBrick
	{0xffff, 0xc000, 0xc000, 0xc000, 0xffff, 0x00c0, 0x00c0, 0x00c0},	// HorizontalBrick
	{0xc0c0, 0x3330, 0x0c0c, 0x3033, 0xc0c0, 0x0330, 0x0c0c, 0x3303},	// Weave
	{0xcccc, 0x3333, 0xcccc, 0x3333, 0xff00, 0xff00, 0xff00, 0xff00},	// Plaid
	{0x0000, 0x0300, 0x00c0, 0x0300, 0x0000, 0xc000, 0x0003, 0xc000},	// Divot
	{0xcccc, 0x0000, 0xc000, 0x0000, 0xc000, 0x0000, 0xc000, 0x0000},	// DottedGrid
	{0xc000, 0x0000, 0x0c0c, 0x0000, 0x00c0, 0x0000, 0x0c0c, 0x0000},	// DottedDiamond
	{0x000f, 0xc030, 0x30c0, 0x0f00, 0x00f0, 0x000c, 0x0003, 0x0003},	// Shingle
	{0xffff, 0x3c3c, 0xffff, 0xc3c3, 0xffff, 0x3c3c, 0xffff, 0xc3c3},	// Trellis
	{0x3f3f, 0xc0c3, 0xc0ff, 0xc0ff, 0x3f3f, 0xc3c0, 0xffc0, 0xffc0},	// Sphere
	{0xffff, 0xc0c0, 0xc0c0, 0xc0c0, 0xffff, 0xc0c0, 0xc0c0, 0xc0c0},	// SmallGrid
	{0xc3c3, 0x3c3c, 0x3c3c, 0xc3c3, 0xc3c3, 0x3c3c, 0x3c3c, 0xc3c3},	// SmallCheckerBoard
	{0xff00, 0xff00, 0xff00, 0xff00, 0x00ff, 0x00ff, 0x00ff, 0x00ff},	// LargeCheckerBoard
	{0xc00c, 0x3030, 0x0cc0, 0x0300, 0x0cc0, 0x3030, 0xc00c, 0x0003},	// OutlinedDiamond
	{0x0300, 0x0fc0, 0x3ff0, 0xfffc, 0x3ff0, 0x0fc0, 0x0300, 0x0000},	// SolidDiamond
};

static ARGB hatch_color(const GpBrush *brush, INT x, INT y)
{
	static const REAL shares[4] = {0.0f, 0.25f, 0.918f, 1.0f};
	if ((UINT)brush->hatch >= ARRAYSIZE(hatch_rows))
		return brush->back;
	UINT level = (hatch_rows[brush->hatch][y & 7] >> (2 * (7 - (x & 7)))) & 3;
	return mix(brush->back, brush->color, shares[level]);
}

ARGB gp_brush_sample(const GpBrush *brush, const GpMatrix *inverse_world, REAL x, REAL y, INT origin_x, INT origin_y)
{
	GpPointF p = {x, y};
	if (brush->type == BrushTypeSolidColor)
		return brush->color;
	if (brush->type == BrushTypeHatchFill)
		return hatch_color(brush, (INT)x - origin_x, (INT)y - origin_y);
	if (inverse_world)
		gp_matrix_apply(inverse_world, &p, 1);
	GpMatrix inverse;
	if (gp_matrix_invert(&brush->transform, &inverse))
		gp_matrix_apply(&inverse, &p, 1);
	if (brush->type == BrushTypeLinearGradient) {
		REAL t = (p.X - brush->rect.X) / brush->rect.Width;
		BOOL outside = FALSE;
		BOOL flip = brush->wrap == WrapModeTileFlipX || brush->wrap == WrapModeTileFlipY || brush->wrap == WrapModeTileFlipXY;
		t = wrap_axis(t, 1.0f, flip, &outside, FALSE);
		return gradient_color(brush, brush->colors[0], brush->colors[1], t);
	}
	if (brush->type == BrushTypeTextureFill) {
		BOOL out_x = FALSE, out_y = FALSE, clamp = brush->wrap == WrapModeClamp;
		REAL u = wrap_axis(p.X, brush->rect.Width, brush->wrap == WrapModeTileFlipX || brush->wrap == WrapModeTileFlipXY, &out_x, clamp);
		REAL v = wrap_axis(p.Y, brush->rect.Height, brush->wrap == WrapModeTileFlipY || brush->wrap == WrapModeTileFlipXY, &out_y, clamp);
		if (out_x || out_y)
			return 0;
		return gp_image_sample(brush->image, brush->attributes, u, v);
	}
	// A path gradient grades along the ray from the centre: how far the point lies toward the boundary it would cross.
	const GpPath *path = brush->path;
	REAL best = -1.0f;
	ARGB edge = brush->surround[0];
	REAL rx = p.X - brush->center.X, ry = p.Y - brush->center.Y;
	if (rx == 0.0f && ry == 0.0f)
		return brush->center_color;
	for (INT i = 0; i < path->count; i++) {
		const GpPointF *a = &path->points[i], *b = &path->points[(i + 1) % path->count];
		REAL ex = b->X - a->X, ey = b->Y - a->Y;
		REAL den = rx * ey - ry * ex;
		if (den == 0.0f)
			continue;
		REAL ax = a->X - brush->center.X, ay = a->Y - brush->center.Y;
		REAL along = (ax * ey - ay * ex) / den, u = (ax * ry - ay * rx) / den;
		if (along <= 0.0f || u < 0.0f || u > 1.0f)
			continue;
		if (best < 0.0f || along < best) {
			best = along;
			INT last = brush->surround_count - 1;
			ARGB c1 = brush->surround[i < last ? i : last], c2 = brush->surround[i + 1 < last ? i + 1 : last];
			edge = mix(c1, c2, u);
		}
	}
	if (best < 0.0f)
		return 0;
	REAL t = 1.0f / best;
	if (t > 1.0f)
		return 0;
	// Distance 0 at the boundary, 1 at the centre: blends count from the outside in.
	return gradient_color(brush, edge, brush->center_color, 1.0f - t);
}
