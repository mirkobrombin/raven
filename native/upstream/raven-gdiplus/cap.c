/* SPDX-License-Identifier: MIT */
#include "gdip.h"

GpStatus GDIPAPI GdipCreateCustomLineCap(GpPath *fill, GpPath *stroke, GpLineCap base, REAL inset, GpCustomLineCap **out)
{
	if (!out || (!fill && !stroke))
		return InvalidParameter;
	GpCustomLineCap *cap = gp_alloc(sizeof(GpCustomLineCap));
	if (!cap)
		return OutOfMemory;
	if ((fill && !(cap->fill = gp_path_clone(fill))) || (stroke && !(cap->stroke = gp_path_clone(stroke)))) {
		GdipDeleteCustomLineCap(cap);
		return OutOfMemory;
	}
	cap->base_cap = (base == LineCapFlat || base == LineCapRound || base == LineCapTriangle) ? base : LineCapFlat;
	cap->inset = inset;
	cap->scale = 1.0f;
	cap->join = LineJoinMiter;
	*out = cap;
	return Ok;
}

GpStatus GDIPAPI GdipDeleteCustomLineCap(GpCustomLineCap *cap)
{
	if (!cap)
		return InvalidParameter;
	if (cap->fill)
		gp_path_delete(cap->fill);
	if (cap->stroke)
		gp_path_delete(cap->stroke);
	gp_free(cap);
	return Ok;
}

GpStatus GDIPAPI GdipCloneCustomLineCap(GpCustomLineCap *cap, GpCustomLineCap **out)
{
	if (!cap || !out)
		return InvalidParameter;
	GpCustomLineCap *copy = gp_dup(cap, sizeof(GpCustomLineCap));
	if (!copy)
		return OutOfMemory;
	copy->fill = copy->stroke = NULL;
	if ((cap->fill && !(copy->fill = gp_path_clone(cap->fill))) || (cap->stroke && !(copy->stroke = gp_path_clone(cap->stroke)))) {
		GdipDeleteCustomLineCap(copy);
		return OutOfMemory;
	}
	*out = copy;
	return Ok;
}

GpStatus GDIPAPI GdipGetCustomLineCapType(GpCustomLineCap *cap, CustomLineCapType *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->type;
	return Ok;
}

GpStatus GDIPAPI GdipGetCustomLineCapBaseCap(GpCustomLineCap *cap, GpLineCap *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->base_cap;
	return Ok;
}

GpStatus GDIPAPI GdipSetCustomLineCapBaseCap(GpCustomLineCap *cap, GpLineCap base)
{
	if (!cap || base == LineCapCustom || (base >= LineCapNoAnchor && base < LineCapCustom))
		return InvalidParameter;
	cap->base_cap = base;
	return Ok;
}

GpStatus GDIPAPI GdipGetCustomLineCapBaseInset(GpCustomLineCap *cap, REAL *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->inset;
	return Ok;
}

GpStatus GDIPAPI GdipSetCustomLineCapBaseInset(GpCustomLineCap *cap, REAL inset)
{
	if (!cap)
		return InvalidParameter;
	cap->inset = inset;
	return Ok;
}

GpStatus GDIPAPI GdipSetCustomLineCapStrokeCaps(GpCustomLineCap *cap, GpLineCap start, GpLineCap end)
{
	if (!cap || (start >= LineCapNoAnchor && start != LineCapCustom) || (end >= LineCapNoAnchor && end != LineCapCustom))
		return InvalidParameter;
	cap->start_cap = start;
	cap->end_cap = end;
	return Ok;
}

GpStatus GDIPAPI GdipGetCustomLineCapStrokeCaps(GpCustomLineCap *cap, GpLineCap *start, GpLineCap *end)
{
	if (!cap || !start || !end)
		return InvalidParameter;
	*start = cap->start_cap;
	*end = cap->end_cap;
	return Ok;
}

GpStatus GDIPAPI GdipGetCustomLineCapStrokeJoin(GpCustomLineCap *cap, GpLineJoin *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->join;
	return Ok;
}

GpStatus GDIPAPI GdipSetCustomLineCapStrokeJoin(GpCustomLineCap *cap, GpLineJoin join)
{
	if (!cap)
		return InvalidParameter;
	cap->join = join;
	return Ok;
}

GpStatus GDIPAPI GdipGetCustomLineCapWidthScale(GpCustomLineCap *cap, REAL *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->scale;
	return Ok;
}

GpStatus GDIPAPI GdipSetCustomLineCapWidthScale(GpCustomLineCap *cap, REAL scale)
{
	if (!cap)
		return InvalidParameter;
	cap->scale = scale;
	return Ok;
}

// The arrow's outline, in units of pen widths, follows its three numbers each time one of them changes.
static GpStatus shape_arrow(GpAdjustableArrowCap *cap)
{
	REAL half = cap->arrow_width / 2.0f, height = cap->arrow_height, inset = cap->arrow_inset;
	GpPath *path = cap->arrow_filled ? cap->fill : cap->stroke;
	if (!path)
		return Ok;
	path->count = 0;
	if (!gp_path_reserve(path, 4))
		return OutOfMemory;
	gp_path_push(path, half, -height, PathPointTypeStart);
	gp_path_push(path, 0.0f, 0.0f, PathPointTypeLine);
	gp_path_push(path, -half, -height, PathPointTypeLine);
	if (cap->arrow_filled) {
		gp_path_push(path, 0.0f, inset - height, PathPointTypeLine);
		path->types[path->count - 1] |= PathPointTypeCloseSubpath;
	}
	cap->inset = cap->arrow_width != 0.0f ? height / cap->arrow_width : 0.0f;
	return Ok;
}

GpStatus GDIPAPI GdipCreateAdjustableArrowCap(REAL height, REAL width, BOOL filled, GpAdjustableArrowCap **out)
{
	if (!out)
		return InvalidParameter;
	GpAdjustableArrowCap *cap = gp_alloc(sizeof(GpAdjustableArrowCap));
	if (!cap)
		return OutOfMemory;
	GpPath **slot = filled ? &cap->fill : &cap->stroke;
	if (gp_path_new(FillModeWinding, slot) != Ok) {
		gp_free(cap);
		return OutOfMemory;
	}
	cap->type = CustomLineCapTypeAdjustableArrow;
	cap->base_cap = LineCapTriangle;
	cap->scale = 1.0f;
	cap->join = LineJoinMiter;
	cap->arrow_height = height;
	cap->arrow_width = width;
	cap->arrow_filled = filled;
	GpStatus status = shape_arrow(cap);
	if (status != Ok) {
		GdipDeleteCustomLineCap(cap);
		return status;
	}
	*out = cap;
	return Ok;
}

GpStatus GDIPAPI GdipGetAdjustableArrowCapHeight(GpAdjustableArrowCap *cap, REAL *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->arrow_height;
	return Ok;
}

GpStatus GDIPAPI GdipSetAdjustableArrowCapHeight(GpAdjustableArrowCap *cap, REAL height)
{
	if (!cap)
		return InvalidParameter;
	cap->arrow_height = height;
	return shape_arrow(cap);
}

GpStatus GDIPAPI GdipGetAdjustableArrowCapWidth(GpAdjustableArrowCap *cap, REAL *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->arrow_width;
	return Ok;
}

GpStatus GDIPAPI GdipSetAdjustableArrowCapWidth(GpAdjustableArrowCap *cap, REAL width)
{
	if (!cap)
		return InvalidParameter;
	cap->arrow_width = width;
	return shape_arrow(cap);
}

GpStatus GDIPAPI GdipGetAdjustableArrowCapMiddleInset(GpAdjustableArrowCap *cap, REAL *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->arrow_inset;
	return Ok;
}

GpStatus GDIPAPI GdipSetAdjustableArrowCapMiddleInset(GpAdjustableArrowCap *cap, REAL inset)
{
	if (!cap)
		return InvalidParameter;
	cap->arrow_inset = inset;
	return shape_arrow(cap);
}

GpStatus GDIPAPI GdipGetAdjustableArrowCapFillState(GpAdjustableArrowCap *cap, BOOL *out)
{
	if (!cap || !out)
		return InvalidParameter;
	*out = cap->arrow_filled;
	return Ok;
}

GpStatus GDIPAPI GdipSetAdjustableArrowCapFillState(GpAdjustableArrowCap *cap, BOOL filled)
{
	if (!cap)
		return InvalidParameter;
	if (filled == cap->arrow_filled)
		return Ok;
	GpPath **from = cap->arrow_filled ? &cap->fill : &cap->stroke, **to = filled ? &cap->fill : &cap->stroke;
	*to = *from;
	*from = NULL;
	cap->arrow_filled = filled;
	return shape_arrow(cap);
}
