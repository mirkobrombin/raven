/* SPDX-License-Identifier: MIT */
#include "brush.h"

GpStatus GDIPAPI GdipCreatePen2(GpBrush *brush, REAL width, GpUnit unit, GpPen **out)
{
	if (!brush || !out)
		return InvalidParameter;
	if (unit == UnitDisplay)
		return InvalidParameter;
	GpPen *pen = gp_alloc(sizeof(GpPen));
	if (!pen)
		return OutOfMemory;
	if (!(pen->brush = gp_brush_clone(brush))) {
		gp_free(pen);
		return OutOfMemory;
	}
	pen->width = width;
	pen->unit = unit;
	pen->miter_limit = 10.0f;
	gp_matrix_identity(&pen->transform);
	*out = pen;
	return Ok;
}

GpStatus GDIPAPI GdipCreatePen1(ARGB color, REAL width, GpUnit unit, GpPen **out)
{
	if (!out)
		return InvalidParameter;
	GpBrush *brush = gp_solid_new(color);
	if (!brush)
		return OutOfMemory;
	GpStatus status = GdipCreatePen2(brush, width, unit, out);
	gp_brush_delete(brush);
	return status;
}

GpStatus GDIPAPI GdipClonePen(GpPen *pen, GpPen **out)
{
	if (!pen || !out)
		return InvalidParameter;
	GpPen *copy = gp_dup(pen, sizeof(GpPen));
	if (!copy)
		return OutOfMemory;
	copy->brush = NULL;
	copy->custom_start = copy->custom_end = NULL;
	copy->dashes = copy->compound = NULL;
	BOOL good = (copy->brush = gp_brush_clone(pen->brush)) != NULL;
	if (good && pen->custom_start)
		good = GdipCloneCustomLineCap(pen->custom_start, &copy->custom_start) == Ok;
	if (good && pen->custom_end)
		good = GdipCloneCustomLineCap(pen->custom_end, &copy->custom_end) == Ok;
	if (good && pen->dash_count)
		good = (copy->dashes = gp_dup(pen->dashes, pen->dash_count * sizeof(REAL))) != NULL;
	if (good && pen->compound_count)
		good = (copy->compound = gp_dup(pen->compound, pen->compound_count * sizeof(REAL))) != NULL;
	if (!good) {
		extern GpStatus GDIPAPI GdipDeletePen(GpPen *);
		GdipDeletePen(copy);
		return OutOfMemory;
	}
	*out = copy;
	return Ok;
}

GpStatus GDIPAPI GdipDeletePen(GpPen *pen)
{
	if (!pen)
		return InvalidParameter;
	if (pen->brush)
		gp_brush_delete(pen->brush);
	if (pen->custom_start)
		GdipDeleteCustomLineCap(pen->custom_start);
	if (pen->custom_end)
		GdipDeleteCustomLineCap(pen->custom_end);
	gp_free(pen->dashes);
	gp_free(pen->compound);
	gp_free(pen);
	return Ok;
}

#define PEN_GET(name, type, field) \
	GpStatus GDIPAPI name(GpPen *pen, type *out) \
	{ \
		if (!pen || !out) \
			return InvalidParameter; \
		*out = pen->field; \
		return Ok; \
	}

PEN_GET(GdipGetPenWidth, REAL, width)
PEN_GET(GdipGetPenUnit, GpUnit, unit)
PEN_GET(GdipGetPenStartCap, GpLineCap, start_cap)
PEN_GET(GdipGetPenEndCap, GpLineCap, end_cap)
PEN_GET(GdipGetPenDashCap197819, GpDashCap, dash_cap)
PEN_GET(GdipGetPenLineJoin, GpLineJoin, join)
PEN_GET(GdipGetPenMiterLimit, REAL, miter_limit)
PEN_GET(GdipGetPenMode, GpPenAlignment, alignment)
PEN_GET(GdipGetPenDashStyle, GpDashStyle, dash_style)
PEN_GET(GdipGetPenDashOffset, REAL, dash_offset)
PEN_GET(GdipGetPenDashCount, INT, dash_count)
PEN_GET(GdipGetPenCompoundCount, INT, compound_count)

GpStatus GDIPAPI GdipSetPenWidth(GpPen *pen, REAL width)
{
	if (!pen)
		return InvalidParameter;
	pen->width = width;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenMiterLimit(GpPen *pen, REAL limit)
{
	if (!pen)
		return InvalidParameter;
	pen->miter_limit = limit;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenLineJoin(GpPen *pen, GpLineJoin join)
{
	if (!pen)
		return InvalidParameter;
	pen->join = join;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenMode(GpPen *pen, GpPenAlignment mode)
{
	if (!pen)
		return InvalidParameter;
	pen->alignment = mode;
	return Ok;
}

static void drop_cap(GpCustomLineCap **cap)
{
	if (*cap)
		GdipDeleteCustomLineCap(*cap);
	*cap = NULL;
}

GpStatus GDIPAPI GdipSetPenStartCap(GpPen *pen, GpLineCap cap)
{
	if (!pen)
		return InvalidParameter;
	drop_cap(&pen->custom_start);
	pen->start_cap = cap;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenEndCap(GpPen *pen, GpLineCap cap)
{
	if (!pen)
		return InvalidParameter;
	drop_cap(&pen->custom_end);
	pen->end_cap = cap;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenDashCap197819(GpPen *pen, GpDashCap cap)
{
	if (!pen)
		return InvalidParameter;
	pen->dash_cap = cap;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenLineCap197819(GpPen *pen, GpLineCap start, GpLineCap end, GpDashCap dash)
{
	if (!pen)
		return InvalidParameter;
	drop_cap(&pen->custom_start);
	drop_cap(&pen->custom_end);
	pen->start_cap = start;
	pen->end_cap = end;
	pen->dash_cap = dash;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenCustomStartCap(GpPen *pen, GpCustomLineCap *cap)
{
	if (!pen || !cap)
		return InvalidParameter;
	GpCustomLineCap *copy;
	GpStatus status = GdipCloneCustomLineCap(cap, &copy);
	if (status != Ok)
		return status;
	drop_cap(&pen->custom_start);
	pen->custom_start = copy;
	pen->start_cap = LineCapCustom;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenCustomEndCap(GpPen *pen, GpCustomLineCap *cap)
{
	if (!pen || !cap)
		return InvalidParameter;
	GpCustomLineCap *copy;
	GpStatus status = GdipCloneCustomLineCap(cap, &copy);
	if (status != Ok)
		return status;
	drop_cap(&pen->custom_end);
	pen->custom_end = copy;
	pen->end_cap = LineCapCustom;
	return Ok;
}

GpStatus GDIPAPI GdipGetPenCustomStartCap(GpPen *pen, GpCustomLineCap **out)
{
	if (!pen || !out)
		return InvalidParameter;
	*out = NULL;
	return pen->custom_start ? GdipCloneCustomLineCap(pen->custom_start, out) : Ok;
}

GpStatus GDIPAPI GdipGetPenCustomEndCap(GpPen *pen, GpCustomLineCap **out)
{
	if (!pen || !out)
		return InvalidParameter;
	*out = NULL;
	return pen->custom_end ? GdipCloneCustomLineCap(pen->custom_end, out) : Ok;
}

GpStatus GDIPAPI GdipGetPenColor(GpPen *pen, ARGB *out)
{
	if (!pen || !out)
		return InvalidParameter;
	if (pen->brush->type != BrushTypeSolidColor)
		return NotImplemented;
	*out = pen->brush->color;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenColor(GpPen *pen, ARGB color)
{
	if (!pen)
		return InvalidParameter;
	if (pen->brush->type != BrushTypeSolidColor)
		return NotImplemented;
	pen->brush->color = color;
	return Ok;
}

GpStatus GDIPAPI GdipGetPenBrushFill(GpPen *pen, GpBrush **out)
{
	if (!pen || !out)
		return InvalidParameter;
	return (*out = gp_brush_clone(pen->brush)) ? Ok : OutOfMemory;
}

GpStatus GDIPAPI GdipSetPenBrushFill(GpPen *pen, GpBrush *brush)
{
	if (!pen || !brush)
		return InvalidParameter;
	GpBrush *copy = gp_brush_clone(brush);
	if (!copy)
		return OutOfMemory;
	gp_brush_delete(pen->brush);
	pen->brush = copy;
	return Ok;
}

GpStatus GDIPAPI GdipGetPenFillType(GpPen *pen, GpPenType *out)
{
	if (!pen || !out)
		return InvalidParameter;
	*out = (GpPenType)pen->brush->type;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenDashStyle(GpPen *pen, GpDashStyle style)
{
	if (!pen)
		return InvalidParameter;
	static const REAL dash[] = {3, 1}, dot[] = {1, 1}, dash_dot[] = {3, 1, 1, 1}, dash_dot_dot[] = {3, 1, 1, 1, 1, 1};
	static const struct { const REAL *values; INT count; } table[] = {{NULL, 0}, {dash, 2}, {dot, 2}, {dash_dot, 4}, {dash_dot_dot, 6}};
	if (style < DashStyleSolid || style > DashStyleCustom)
		return InvalidParameter;
	if (style != DashStyleCustom) {
		REAL *copy = table[style].count ? gp_dup(table[style].values, table[style].count * sizeof(REAL)) : NULL;
		if (table[style].count && !copy)
			return OutOfMemory;
		gp_free(pen->dashes);
		pen->dashes = copy;
		pen->dash_count = table[style].count;
	}
	pen->dash_style = style;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenDashOffset(GpPen *pen, REAL offset)
{
	if (!pen)
		return InvalidParameter;
	pen->dash_offset = offset;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenDashArray(GpPen *pen, const REAL *dashes, INT count)
{
	if (!pen || !dashes || count <= 0)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		if (dashes[i] <= 0.0f)
			return InvalidParameter;
	}
	REAL *copy = gp_dup(dashes, count * sizeof(REAL));
	if (!copy)
		return OutOfMemory;
	gp_free(pen->dashes);
	pen->dashes = copy;
	pen->dash_count = count;
	pen->dash_style = DashStyleCustom;
	return Ok;
}

GpStatus GDIPAPI GdipGetPenDashArray(GpPen *pen, REAL *out, INT count)
{
	if (!pen || !out)
		return InvalidParameter;
	if (count > pen->dash_count || count < 0)
		return InvalidParameter;
	if (count)
		memcpy(out, pen->dashes, count * sizeof(REAL));
	return Ok;
}

GpStatus GDIPAPI GdipSetPenCompoundArray(GpPen *pen, const REAL *values, INT count)
{
	if (!pen || !values || count < 2 || (count & 1))
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		if (values[i] < 0.0f || values[i] > 1.0f || (i && values[i] < values[i - 1]))
			return InvalidParameter;
	}
	REAL *copy = gp_dup(values, count * sizeof(REAL));
	if (!copy)
		return OutOfMemory;
	gp_free(pen->compound);
	pen->compound = copy;
	pen->compound_count = count;
	return Ok;
}

GpStatus GDIPAPI GdipGetPenCompoundArray(GpPen *pen, REAL *out, INT count)
{
	if (!pen || !out)
		return InvalidParameter;
	if (count > pen->compound_count)
		return InvalidParameter;
	if (count > 0)
		memcpy(out, pen->compound, count * sizeof(REAL));
	return Ok;
}

extern GpStatus GDIPAPI GdipIsMatrixInvertible(const GpMatrix *, BOOL *);

GpStatus GDIPAPI GdipGetPenTransform(GpPen *pen, GpMatrix *out)
{
	if (!pen || !out)
		return InvalidParameter;
	*out = pen->transform;
	return Ok;
}

GpStatus GDIPAPI GdipSetPenTransform(GpPen *pen, GpMatrix *matrix)
{
	BOOL invertible;
	if (!pen || !matrix)
		return InvalidParameter;
	GdipIsMatrixInvertible(matrix, &invertible);
	if (!invertible)
		return InvalidParameter;
	pen->transform = *matrix;
	return Ok;
}

GpStatus GDIPAPI GdipResetPenTransform(GpPen *pen)
{
	if (!pen)
		return InvalidParameter;
	gp_matrix_identity(&pen->transform);
	return Ok;
}

extern GpStatus GDIPAPI GdipMultiplyMatrix(GpMatrix *, const GpMatrix *, GpMatrixOrder);
extern GpStatus GDIPAPI GdipTranslateMatrix(GpMatrix *, REAL, REAL, GpMatrixOrder);
extern GpStatus GDIPAPI GdipScaleMatrix(GpMatrix *, REAL, REAL, GpMatrixOrder);
extern GpStatus GDIPAPI GdipRotateMatrix(GpMatrix *, REAL, GpMatrixOrder);

GpStatus GDIPAPI GdipMultiplyPenTransform(GpPen *pen, const GpMatrix *matrix, GpMatrixOrder order)
{
	if (!pen)
		return InvalidParameter;
	return matrix ? GdipMultiplyMatrix(&pen->transform, matrix, order) : Ok;
}

GpStatus GDIPAPI GdipTranslatePenTransform(GpPen *pen, REAL x, REAL y, GpMatrixOrder order)
{
	return pen ? GdipTranslateMatrix(&pen->transform, x, y, order) : InvalidParameter;
}

GpStatus GDIPAPI GdipScalePenTransform(GpPen *pen, REAL x, REAL y, GpMatrixOrder order)
{
	return pen ? GdipScaleMatrix(&pen->transform, x, y, order) : InvalidParameter;
}

GpStatus GDIPAPI GdipRotatePenTransform(GpPen *pen, REAL angle, GpMatrixOrder order)
{
	return pen ? GdipRotateMatrix(&pen->transform, angle, order) : InvalidParameter;
}
