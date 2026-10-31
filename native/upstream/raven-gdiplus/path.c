/* SPDX-License-Identifier: MIT */
#include "graphics.h"
#include "gmath.h"

// A path is a point list with one type byte per point. new_figure says the next point opens a figure, whatever type
// the adding call would otherwise give it.

GpStatus gp_path_new(GpFillMode fill, GpPath **out)
{
	GpPath *path = gp_alloc(sizeof(GpPath));
	if (!path)
		return OutOfMemory;
	path->fill = fill;
	path->new_figure = TRUE;
	*out = path;
	return Ok;
}

void gp_path_delete(GpPath *path)
{
	gp_free(path->points);
	gp_free(path->types);
	gp_free(path);
}

BOOL gp_path_reserve(GpPath *path, INT extra)
{
	INT wanted = path->count + extra;
	if (wanted <= path->capacity)
		return TRUE;
	INT capacity = path->capacity ? path->capacity * 2 : 16;
	if (capacity < wanted)
		capacity = wanted;
	GpPointF *points = gp_realloc(path->points, capacity * sizeof(GpPointF));
	if (!points)
		return FALSE;
	path->points = points;
	BYTE *types = gp_realloc(path->types, capacity);
	if (!types)
		return FALSE;
	path->types = types;
	path->capacity = capacity;
	return TRUE;
}

void gp_path_push(GpPath *path, REAL x, REAL y, BYTE type)
{
	path->points[path->count].X = x;
	path->points[path->count].Y = y;
	path->types[path->count++] = type;
}

GpPath *gp_path_clone(const GpPath *path)
{
	GpPath *copy;
	if (gp_path_new(path->fill, &copy) != Ok)
		return NULL;
	if (!gp_path_reserve(copy, path->count ? path->count : 1)) {
		gp_path_delete(copy);
		return NULL;
	}
	if (path->count) {
		memcpy(copy->points, path->points, path->count * sizeof(GpPointF));
		memcpy(copy->types, path->types, path->count);
	}
	copy->count = path->count;
	copy->new_figure = path->new_figure;
	return copy;
}

static GpPointF *widen_points(const GpPoint *points, INT count)
{
	GpPointF *out = gp_alloc((count ? count : 1) * sizeof(GpPointF));
	if (out) {
		for (INT i = 0; i < count; i++) {
			out[i].X = (REAL)points[i].X;
			out[i].Y = (REAL)points[i].Y;
		}
	}
	return out;
}

static BYTE first_type(GpPath *path, BYTE type)
{
	BYTE result = path->new_figure ? PathPointTypeStart : type;
	path->new_figure = FALSE;
	return result;
}

static void close_figure(GpPath *path)
{
	if (path->count)
		path->types[path->count - 1] |= PathPointTypeCloseSubpath;
	path->new_figure = TRUE;
}

GpStatus GDIPAPI GdipCreatePath(GpFillMode fill, GpPath **out)
{
	if (!out)
		return InvalidParameter;
	return gp_path_new(fill, out);
}

GpStatus GDIPAPI GdipCreatePath2(const GpPointF *points, const BYTE *types, INT count, GpFillMode fill, GpPath **out)
{
	if (!out)
		return InvalidParameter;
	if (!points || !types)
		return InvalidParameter;
	if (count <= 0) {
		*out = NULL;
		return OutOfMemory;
	}
	GpStatus status = gp_path_new(fill, out);
	if (status != Ok)
		return status;
	if (!gp_path_reserve(*out, count)) {
		gp_path_delete(*out);
		*out = NULL;
		return OutOfMemory;
	}
	memcpy((*out)->points, points, count * sizeof(GpPointF));
	memcpy((*out)->types, types, count);
	// The first point always opens a figure. A malformed list, ending on a lone start or cutting a Bezier short, makes an
	// empty path rather than an error.
	(*out)->types[0] = ((*out)->types[0] & ~PathPointTypePathTypeMask) | PathPointTypeStart;
	BOOL valid = count == 1 || (types[count - 1] & PathPointTypePathTypeMask) != PathPointTypeStart;
	INT run = 0;
	for (INT i = 1; i < count && valid; i++) {
		if ((types[i] & PathPointTypePathTypeMask) == PathPointTypeBezier) {
			run++;
		} else {
			valid = !(run % 3);
			run = 0;
		}
	}
	if (run % 3)
		valid = FALSE;
	if (valid)
		(*out)->count = count;
	(*out)->new_figure = !valid || (types[count - 1] & PathPointTypeCloseSubpath) != 0;
	return Ok;
}

GpStatus GDIPAPI GdipCreatePath2I(const GpPoint *points, const BYTE *types, INT count, GpFillMode fill, GpPath **out)
{
	if (!points)
		return InvalidParameter;
	GpPointF *floats = widen_points(points, count > 0 ? count : 0);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipCreatePath2(floats, types, count, fill, out);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipClonePath(GpPath *path, GpPath **out)
{
	if (!path || !out)
		return InvalidParameter;
	return (*out = gp_path_clone(path)) ? Ok : OutOfMemory;
}

GpStatus GDIPAPI GdipDeletePath(GpPath *path)
{
	if (!path)
		return InvalidParameter;
	gp_path_delete(path);
	return Ok;
}

GpStatus GDIPAPI GdipResetPath(GpPath *path)
{
	if (!path)
		return InvalidParameter;
	path->count = 0;
	path->fill = FillModeAlternate;
	path->new_figure = TRUE;
	return Ok;
}

GpStatus GDIPAPI GdipGetPointCount(GpPath *path, INT *out)
{
	if (!path || !out)
		return InvalidParameter;
	*out = path->count;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathFillMode(GpPath *path, GpFillMode *out)
{
	if (!path || !out)
		return InvalidParameter;
	*out = path->fill;
	return Ok;
}

GpStatus GDIPAPI GdipSetPathFillMode(GpPath *path, GpFillMode fill)
{
	if (!path)
		return InvalidParameter;
	path->fill = fill;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathPoints(GpPath *path, GpPointF *out, INT count)
{
	if (!path || !out || count <= 0)
		return InvalidParameter;
	if (count < path->count)
		return InsufficientBuffer;
	memcpy(out, path->points, path->count * sizeof(GpPointF));
	return Ok;
}

GpStatus GDIPAPI GdipGetPathPointsI(GpPath *path, GpPoint *out, INT count)
{
	if (!path || !out || count <= 0)
		return InvalidParameter;
	if (count < path->count)
		return InsufficientBuffer;
	for (INT i = 0; i < path->count; i++) {
		out[i].X = (INT)gp_floor(path->points[i].X + 0.5f);
		out[i].Y = (INT)gp_floor(path->points[i].Y + 0.5f);
	}
	return Ok;
}

GpStatus GDIPAPI GdipGetPathTypes(GpPath *path, BYTE *out, INT count)
{
	if (!path || !out || count <= 0)
		return InvalidParameter;
	if (count < path->count)
		return InsufficientBuffer;
	memcpy(out, path->types, path->count);
	return Ok;
}

GpStatus GDIPAPI GdipGetPathData(GpPath *path, GpPathData *out)
{
	if (!path || !out)
		return InvalidParameter;
	memcpy(out->Points, path->points, path->count * sizeof(GpPointF));
	memcpy(out->Types, path->types, path->count);
	out->Count = path->count;
	return Ok;
}

GpStatus GDIPAPI GdipGetPathLastPoint(GpPath *path, GpPointF *out)
{
	if (!path || !out)
		return InvalidParameter;
	if (path->count)
		*out = path->points[path->count - 1];
	return Ok;
}

GpStatus GDIPAPI GdipStartPathFigure(GpPath *path)
{
	if (!path)
		return InvalidParameter;
	path->new_figure = TRUE;
	return Ok;
}

GpStatus GDIPAPI GdipClosePathFigure(GpPath *path)
{
	if (!path)
		return InvalidParameter;
	close_figure(path);
	return Ok;
}

GpStatus GDIPAPI GdipClosePathFigures(GpPath *path)
{
	if (!path)
		return InvalidParameter;
	for (INT i = 1; i < path->count; i++) {
		if ((path->types[i] & PathPointTypePathTypeMask) == PathPointTypeStart)
			path->types[i - 1] |= PathPointTypeCloseSubpath;
	}
	close_figure(path);
	return Ok;
}

GpStatus GDIPAPI GdipSetPathMarker(GpPath *path)
{
	if (!path)
		return InvalidParameter;
	if (path->count)
		path->types[path->count - 1] |= PathPointTypePathMarker;
	return Ok;
}

GpStatus GDIPAPI GdipClearPathMarkers(GpPath *path)
{
	if (!path)
		return InvalidParameter;
	for (INT i = 0; i < path->count; i++)
		path->types[i] &= ~PathPointTypePathMarker;
	return Ok;
}

GpStatus GDIPAPI GdipAddPathLine2(GpPath *path, const GpPointF *points, INT count)
{
	if (!path || !points || count < 1)
		return InvalidParameter;
	if (!gp_path_reserve(path, count))
		return OutOfMemory;
	// A line that starts where the figure stands continues it without repeating the point.
	if (path->new_figure || !path->count || path->points[path->count - 1].X != points[0].X || path->points[path->count - 1].Y != points[0].Y)
		gp_path_push(path, points[0].X, points[0].Y, first_type(path, PathPointTypeLine));
	for (INT i = 1; i < count; i++)
		gp_path_push(path, points[i].X, points[i].Y, PathPointTypeLine);
	return Ok;
}

GpStatus GDIPAPI GdipAddPathLine2I(GpPath *path, const GpPoint *points, INT count)
{
	if (!points || count < 1)
		return InvalidParameter;
	GpPointF *floats = widen_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipAddPathLine2(path, floats, count);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipAddPathLine(GpPath *path, REAL x1, REAL y1, REAL x2, REAL y2)
{
	GpPointF points[2] = {{x1, y1}, {x2, y2}};
	return GdipAddPathLine2(path, points, 2);
}

GpStatus GDIPAPI GdipAddPathLineI(GpPath *path, INT x1, INT y1, INT x2, INT y2)
{
	return GdipAddPathLine(path, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2);
}

GpStatus GDIPAPI GdipAddPathBeziers(GpPath *path, const GpPointF *points, INT count)
{
	if (!path || !points || count < 4 || (count - 1) % 3)
		return InvalidParameter;
	if (!gp_path_reserve(path, count))
		return OutOfMemory;
	// A curve that starts where the figure stands continues it without repeating the point.
	if (path->new_figure || !path->count || path->points[path->count - 1].X != points[0].X || path->points[path->count - 1].Y != points[0].Y)
		gp_path_push(path, points[0].X, points[0].Y, first_type(path, PathPointTypeLine));
	for (INT i = 1; i < count; i++)
		gp_path_push(path, points[i].X, points[i].Y, PathPointTypeBezier);
	return Ok;
}

GpStatus GDIPAPI GdipAddPathBeziersI(GpPath *path, const GpPoint *points, INT count)
{
	if (!points || count < 4 || (count - 1) % 3)
		return InvalidParameter;
	GpPointF *floats = widen_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipAddPathBeziers(path, floats, count);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipAddPathBezier(GpPath *path, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4)
{
	GpPointF points[4] = {{x1, y1}, {x2, y2}, {x3, y3}, {x4, y4}};
	return GdipAddPathBeziers(path, points, 4);
}

GpStatus GDIPAPI GdipAddPathBezierI(GpPath *path, INT x1, INT y1, INT x2, INT y2, INT x3, INT y3, INT x4, INT y4)
{
	return GdipAddPathBezier(path, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2, (REAL)x3, (REAL)y3, (REAL)x4, (REAL)y4);
}

// An angle measured on the circle the ellipse was stretched from, keeping which full turn it belongs to.
static double circle_angle(double degrees, REAL width, REAL height)
{
	double radians = degrees * GP_PI / 180.0;
	double turns = floor(radians / (2.0 * GP_PI) + 0.5);
	double mapped = atan2(gp_fabs(width) * sin(radians), gp_fabs(height) * cos(radians));
	double reduced = radians - turns * 2.0 * GP_PI;
	if (mapped - reduced > GP_PI)
		mapped -= 2.0 * GP_PI;
	else if (reduced - mapped > GP_PI)
		mapped += 2.0 * GP_PI;
	return mapped + turns * 2.0 * GP_PI;
}

// Writes the Bezier points of an elliptical arc when out is given; answers how many points the arc takes. Both ends are
// mapped onto the circle, and the quarter turns are counted from the start there.
INT gp_arc_points(GpPointF *out, REAL x, REAL y, REAL width, REAL height, REAL start, REAL sweep)
{
	if (sweep > 360.0f)
		sweep = 360.0f;
	else if (sweep < -360.0f)
		sweep = -360.0f;
	double rx = width / 2.0, ry = height / 2.0, cx = x + rx, cy = y + ry;
	double from = circle_angle(start, width, height), last;
	if (gp_fabs(sweep) >= 360.0f)
		last = from + (sweep < 0 ? -2.0 : 2.0) * GP_PI;
	else
		last = circle_angle((double)start + sweep, width, height);
	double remaining = last - from, quarter = GP_PI / 2.0;
	INT segments = 0;
	if (out) {
		out[0].X = (REAL)(cx + rx * cos(from));
		out[0].Y = (REAL)(cy + ry * sin(from));
	}
	while (remaining > 1e-5 || remaining < -1e-5) {
		double part = remaining > quarter ? quarter : remaining < -quarter ? -quarter : remaining;
		double to = from + part;
		if (out) {
			double k = 4.0 / 3.0 * tan(part / 4.0);
			GpPointF *p = out + 1 + segments * 3;
			p[0].X = (REAL)(cx + rx * (cos(from) - k * sin(from)));
			p[0].Y = (REAL)(cy + ry * (sin(from) + k * cos(from)));
			p[1].X = (REAL)(cx + rx * (cos(to) + k * sin(to)));
			p[1].Y = (REAL)(cy + ry * (sin(to) - k * cos(to)));
			p[2].X = (REAL)(cx + rx * cos(to));
			p[2].Y = (REAL)(cy + ry * sin(to));
		}
		from = to;
		remaining -= part;
		segments++;
	}
	return segments * 3 + 1;
}

GpStatus GDIPAPI GdipAddPathArc(GpPath *path, REAL x, REAL y, REAL width, REAL height, REAL start, REAL sweep)
{
	if (!path || width <= 0.0f || height <= 0.0f)
		return InvalidParameter;
	INT count = gp_arc_points(NULL, x, y, width, height, start, sweep);
	if (count <= 1)
		return Ok;
	GpPointF *points = gp_alloc(count * sizeof(GpPointF));
	if (!points)
		return OutOfMemory;
	gp_arc_points(points, x, y, width, height, start, sweep);
	GpStatus status = GdipAddPathBeziers(path, points, count);
	gp_free(points);
	// A whole turn is a closed figure.
	if (status == Ok && gp_fabs(sweep) >= 360.0f)
		close_figure(path);
	return status;
}

GpStatus GDIPAPI GdipAddPathArcI(GpPath *path, INT x, INT y, INT width, INT height, REAL start, REAL sweep)
{
	return GdipAddPathArc(path, (REAL)x, (REAL)y, (REAL)width, (REAL)height, start, sweep);
}

GpStatus GDIPAPI GdipAddPathEllipse(GpPath *path, REAL x, REAL y, REAL width, REAL height)
{
	if (!path)
		return InvalidParameter;
	if (!gp_path_reserve(path, 13))
		return OutOfMemory;
	GpPointF points[13];
	gp_arc_points(points, x, y, width, height, 0.0f, 360.0f);
	gp_path_push(path, points[0].X, points[0].Y, PathPointTypeStart);
	for (int i = 1; i < 13; i++)
		gp_path_push(path, points[i].X, points[i].Y, PathPointTypeBezier);
	close_figure(path);
	return Ok;
}

GpStatus GDIPAPI GdipAddPathEllipseI(GpPath *path, INT x, INT y, INT width, INT height)
{
	return GdipAddPathEllipse(path, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus GDIPAPI GdipAddPathPie(GpPath *path, REAL x, REAL y, REAL width, REAL height, REAL start, REAL sweep)
{
	if (!path)
		return InvalidParameter;
	// The centre goes in first and stays there when the ellipse is refused.
	if (width <= 0.0f || height <= 0.0f) {
		if (!gp_path_reserve(path, 1))
			return OutOfMemory;
		gp_path_push(path, x + width / 2.0f, y + height / 2.0f, PathPointTypeStart);
		close_figure(path);
		return InvalidParameter;
	}
	INT count = gp_arc_points(NULL, x, y, width, height, start, sweep);
	GpPointF *points = gp_alloc(count * sizeof(GpPointF));
	if (!points)
		return OutOfMemory;
	if (!gp_path_reserve(path, count + 1)) {
		gp_free(points);
		return OutOfMemory;
	}
	gp_arc_points(points, x, y, width, height, start, sweep);
	gp_path_push(path, x + width / 2.0f, y + height / 2.0f, PathPointTypeStart);
	gp_path_push(path, points[0].X, points[0].Y, PathPointTypeLine);
	for (INT i = 1; i < count; i++)
		gp_path_push(path, points[i].X, points[i].Y, PathPointTypeBezier);
	close_figure(path);
	gp_free(points);
	return Ok;
}

GpStatus GDIPAPI GdipAddPathPieI(GpPath *path, INT x, INT y, INT width, INT height, REAL start, REAL sweep)
{
	return GdipAddPathPie(path, (REAL)x, (REAL)y, (REAL)width, (REAL)height, start, sweep);
}

GpStatus GDIPAPI GdipAddPathRectangle(GpPath *path, REAL x, REAL y, REAL width, REAL height)
{
	if (!path)
		return InvalidParameter;
	if (width <= 0.0f || height <= 0.0f)
		return Ok;
	if (!gp_path_reserve(path, 4))
		return OutOfMemory;
	gp_path_push(path, x, y, PathPointTypeStart);
	gp_path_push(path, x + width, y, PathPointTypeLine);
	gp_path_push(path, x + width, y + height, PathPointTypeLine);
	gp_path_push(path, x, y + height, PathPointTypeLine);
	close_figure(path);
	return Ok;
}

GpStatus GDIPAPI GdipAddPathRectangleI(GpPath *path, INT x, INT y, INT width, INT height)
{
	return GdipAddPathRectangle(path, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus GDIPAPI GdipAddPathRectangles(GpPath *path, const GpRectF *rects, INT count)
{
	if (!path || !rects || count < 1)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		GpStatus status = GdipAddPathRectangle(path, rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);
		if (status != Ok)
			return status;
	}
	return Ok;
}

GpStatus GDIPAPI GdipAddPathRectanglesI(GpPath *path, const GpRect *rects, INT count)
{
	if (!path || !rects || count < 1)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		GpStatus status = GdipAddPathRectangleI(path, rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);
		if (status != Ok)
			return status;
	}
	return Ok;
}

GpStatus GDIPAPI GdipAddPathPolygon(GpPath *path, const GpPointF *points, INT count)
{
	if (!path || !points || count < 3)
		return InvalidParameter;
	if (!gp_path_reserve(path, count))
		return OutOfMemory;
	gp_path_push(path, points[0].X, points[0].Y, PathPointTypeStart);
	for (INT i = 1; i < count; i++)
		gp_path_push(path, points[i].X, points[i].Y, PathPointTypeLine);
	close_figure(path);
	return Ok;
}

GpStatus GDIPAPI GdipAddPathPolygonI(GpPath *path, const GpPoint *points, INT count)
{
	if (!points || count < 3)
		return InvalidParameter;
	GpPointF *floats = widen_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipAddPathPolygon(path, floats, count);
	gp_free(floats);
	return status;
}

// A cardinal spline as Beziers. Each inner knot pulls its two control points along the chord of its neighbours, by a third
// of the tension.
static GpPointF *curve_controls(const GpPointF *points, INT count, REAL tension, BOOL closed, INT *out_count)
{
	INT segments = closed ? count : count - 1;
	GpPointF *out = gp_alloc((segments * 3 + 1) * sizeof(GpPointF));
	if (!out)
		return NULL;
	REAL t = tension / 3.0f;
	for (INT i = 0; i < segments; i++) {
		const GpPointF *a = &points[i], *b = &points[(i + 1) % count];
		const GpPointF *before = closed ? &points[(i + count - 1) % count] : (i ? &points[i - 1] : a);
		const GpPointF *after = closed ? &points[(i + 2) % count] : (i + 2 < count ? &points[i + 2] : b);
		GpPointF *o = out + i * 3;
		o[0] = *a;
		o[1].X = a->X + (b->X - before->X) * t;
		o[1].Y = a->Y + (b->Y - before->Y) * t;
		o[2].X = b->X - (after->X - a->X) * t;
		o[2].Y = b->Y - (after->Y - a->Y) * t;
		o[3] = *b;
	}
	*out_count = segments * 3 + 1;
	return out;
}

GpStatus GDIPAPI GdipAddPathCurve2(GpPath *path, const GpPointF *points, INT count, REAL tension)
{
	if (!path || !points || count <= 1)
		return InvalidParameter;
	INT total;
	GpPointF *controls = curve_controls(points, count, tension, FALSE, &total);
	if (!controls)
		return OutOfMemory;
	GpStatus status = GdipAddPathBeziers(path, controls, total);
	gp_free(controls);
	return status;
}

GpStatus GDIPAPI GdipAddPathCurve2I(GpPath *path, const GpPoint *points, INT count, REAL tension)
{
	if (!points || count <= 1)
		return InvalidParameter;
	GpPointF *floats = widen_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipAddPathCurve2(path, floats, count, tension);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipAddPathCurve(GpPath *path, const GpPointF *points, INT count)
{
	return GdipAddPathCurve2(path, points, count, 0.5f);
}

GpStatus GDIPAPI GdipAddPathCurveI(GpPath *path, const GpPoint *points, INT count)
{
	return GdipAddPathCurve2I(path, points, count, 0.5f);
}

// A stretch of a longer curve: its neighbours outside the stretch still shape its ends.
GpStatus GDIPAPI GdipAddPathCurve3(GpPath *path, const GpPointF *points, INT count, INT offset, INT segments, REAL tension)
{
	if (!path || !points || count <= 1 || offset < 0 || segments < 1 || offset + segments >= count)
		return InvalidParameter;
	INT total;
	GpPointF *controls = curve_controls(points, count, tension, FALSE, &total);
	if (!controls)
		return OutOfMemory;
	GpStatus status = GdipAddPathBeziers(path, controls + offset * 3, segments * 3 + 1);
	gp_free(controls);
	return status;
}

GpStatus GDIPAPI GdipAddPathCurve3I(GpPath *path, const GpPoint *points, INT count, INT offset, INT segments, REAL tension)
{
	if (!path || !points || count <= 1)
		return InvalidParameter;
	GpPointF *floats = widen_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipAddPathCurve3(path, floats, count, offset, segments, tension);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipAddPathClosedCurve2(GpPath *path, const GpPointF *points, INT count, REAL tension)
{
	if (!path || !points || count <= 2)
		return InvalidParameter;
	INT total;
	GpPointF *controls = curve_controls(points, count, tension, TRUE, &total);
	if (!controls)
		return OutOfMemory;
	if (!gp_path_reserve(path, total)) {
		gp_free(controls);
		return OutOfMemory;
	}
	gp_path_push(path, controls[0].X, controls[0].Y, PathPointTypeStart);
	for (INT i = 1; i < total; i++)
		gp_path_push(path, controls[i].X, controls[i].Y, PathPointTypeBezier);
	close_figure(path);
	gp_free(controls);
	return Ok;
}

GpStatus GDIPAPI GdipAddPathClosedCurve2I(GpPath *path, const GpPoint *points, INT count, REAL tension)
{
	if (!points || count <= 2)
		return InvalidParameter;
	GpPointF *floats = widen_points(points, count);
	if (!floats)
		return OutOfMemory;
	GpStatus status = GdipAddPathClosedCurve2(path, floats, count, tension);
	gp_free(floats);
	return status;
}

GpStatus GDIPAPI GdipAddPathClosedCurve(GpPath *path, const GpPointF *points, INT count)
{
	return GdipAddPathClosedCurve2(path, points, count, 0.5f);
}

GpStatus GDIPAPI GdipAddPathClosedCurveI(GpPath *path, const GpPoint *points, INT count)
{
	return GdipAddPathClosedCurve2I(path, points, count, 0.5f);
}

GpStatus GDIPAPI GdipAddPathPath(GpPath *path, const GpPath *other, BOOL connect)
{
	if (!path || !other)
		return InvalidParameter;
	if (!other->count)
		return Ok;
	if (!gp_path_reserve(path, other->count))
		return OutOfMemory;
	INT base = path->count;
	memcpy(path->points + base, other->points, other->count * sizeof(GpPointF));
	memcpy(path->types + base, other->types, other->count);
	path->count += other->count;
	if (connect && base && !path->new_figure && !(path->types[base - 1] & PathPointTypeCloseSubpath))
		path->types[base] = PathPointTypeLine;
	path->new_figure = (other->types[other->count - 1] & PathPointTypeCloseSubpath) != 0;
	return Ok;
}

// Reads the whole path backward, so figures swap order too. A figure's closing flag stays on the point that ends it, and a
// marker, which names the point ending a run, moves to its mirror's predecessor.
GpStatus GDIPAPI GdipReversePath(GpPath *path)
{
	if (!path)
		return InvalidParameter;
	if (!path->count)
		return Ok;
	INT total = path->count;
	GpPointF *points = gp_alloc(total * sizeof(GpPointF));
	BYTE *types = gp_alloc(total);
	if (!points || !types) {
		gp_free(points);
		gp_free(types);
		return OutOfMemory;
	}
	for (INT start = 0; start < total;) {
		INT end = start + 1;
		while (end < total && (path->types[end] & PathPointTypePathTypeMask) != PathPointTypeStart)
			end++;
		for (INT i = start; i < end; i++) {
			INT mirrored = total - 1 - i;
			points[mirrored] = path->points[i];
			types[mirrored] = i + 1 == end ? PathPointTypeStart : path->types[i + 1] & PathPointTypePathTypeMask;
		}
		if (path->types[end - 1] & PathPointTypeCloseSubpath)
			types[total - 1 - start] |= PathPointTypeCloseSubpath;
		for (INT i = start; i < end; i++) {
			if ((path->types[i] & PathPointTypePathMarker) && i + 1 < end)
				types[total - 2 - i] |= PathPointTypePathMarker;
		}
		start = end;
	}
	memcpy(path->points, points, total * sizeof(GpPointF));
	memcpy(path->types, types, total);
	gp_free(points);
	gp_free(types);
	return Ok;
}

GpStatus GDIPAPI GdipTransformPath(GpPath *path, GpMatrix *matrix)
{
	if (!path)
		return InvalidParameter;
	if (path->count && matrix)
		gp_matrix_apply(matrix, path->points, path->count);
	return Ok;
}

// Splits a Bezier in two until the curve's middle lies close enough to its chord.
static BOOL flatten_bezier(GpPath *out, const GpPointF *p, REAL tolerance, int depth)
{
	GpPointF m[7];
	m[0] = p[0];
	m[6] = p[3];
	m[1].X = (p[0].X + p[1].X) / 2.0f; m[1].Y = (p[0].Y + p[1].Y) / 2.0f;
	GpPointF mid = {(p[1].X + p[2].X) / 2.0f, (p[1].Y + p[2].Y) / 2.0f};
	m[5].X = (p[2].X + p[3].X) / 2.0f; m[5].Y = (p[2].Y + p[3].Y) / 2.0f;
	m[2].X = (m[1].X + mid.X) / 2.0f; m[2].Y = (m[1].Y + mid.Y) / 2.0f;
	m[4].X = (mid.X + m[5].X) / 2.0f; m[4].Y = (mid.Y + m[5].Y) / 2.0f;
	m[3].X = (m[2].X + m[4].X) / 2.0f; m[3].Y = (m[2].Y + m[4].Y) / 2.0f;
	REAL dx = p[3].X - p[0].X, dy = p[3].Y - p[0].Y;
	REAL chord = gp_sqrtf(dx * dx + dy * dy);
	REAL off = chord > 1e-6f ? gp_fabs((m[3].X - p[0].X) * dy - (m[3].Y - p[0].Y) * dx) / chord : gp_sqrtf((m[3].X - p[0].X) * (m[3].X - p[0].X) + (m[3].Y - p[0].Y) * (m[3].Y - p[0].Y));
	if (off <= tolerance * 0.64f || depth >= 16) {
		if (!gp_path_reserve(out, 1))
			return FALSE;
		gp_path_push(out, p[3].X, p[3].Y, PathPointTypeLine);
		return TRUE;
	}
	return flatten_bezier(out, m, tolerance, depth + 1) && flatten_bezier(out, m + 3, tolerance, depth + 1);
}

GpStatus gp_path_flatten(GpPath *path, const GpMatrix *matrix, REAL flatness)
{
	if (matrix)
		gp_matrix_apply(matrix, path->points, path->count);
	GpPath *out;
	if (gp_path_new(path->fill, &out) != Ok)
		return OutOfMemory;
	if (flatness <= 0.0f)
		flatness = 0.25f;
	for (INT i = 0; i < path->count;) {
		BYTE type = path->types[i];
		if ((type & PathPointTypePathTypeMask) == PathPointTypeBezier && i && i + 2 < path->count) {
			GpPointF controls[4] = {path->points[i - 1], path->points[i], path->points[i + 1], path->points[i + 2]};
			BYTE flags = path->types[i + 2] & ~PathPointTypePathTypeMask;
			if (!flatten_bezier(out, controls, flatness, 0)) {
				gp_path_delete(out);
				return OutOfMemory;
			}
			out->types[out->count - 1] |= flags;
			i += 3;
			continue;
		}
		if (!gp_path_reserve(out, 1)) {
			gp_path_delete(out);
			return OutOfMemory;
		}
		gp_path_push(out, path->points[i].X, path->points[i].Y, type);
		i++;
	}
	gp_free(path->points);
	gp_free(path->types);
	path->points = out->points;
	path->types = out->types;
	path->count = out->count;
	path->capacity = out->capacity;
	gp_free(out);
	return Ok;
}

GpStatus GDIPAPI GdipFlattenPath(GpPath *path, GpMatrix *matrix, REAL flatness)
{
	if (!path)
		return InvalidParameter;
	if (!path->count)
		return Ok;
	return gp_path_flatten(path, matrix, flatness);
}

void gp_path_bounds(const GpPath *path, GpRectF *out)
{
	if (!path->count) {
		out->X = out->Y = out->Width = out->Height = 0.0f;
		return;
	}
	REAL left = path->points[0].X, right = left, top = path->points[0].Y, bottom = top;
	for (INT i = 1; i < path->count; i++) {
		REAL x = path->points[i].X, y = path->points[i].Y;
		if (x < left) left = x;
		if (x > right) right = x;
		if (y < top) top = y;
		if (y > bottom) bottom = y;
	}
	out->X = left;
	out->Y = top;
	out->Width = right - left;
	out->Height = bottom - top;
}

// Bounds run over the flattened figure and only then through the matrix, whose result is the box around the box's corners.
// A pen grows them by the widest reach of its stroke, an outer bound rather than the exact outline. A lone point has none.
GpStatus GDIPAPI GdipGetPathWorldBounds(GpPath *path, GpRectF *out, const GpMatrix *matrix, const GpPen *pen)
{
	if (!path || !out)
		return InvalidParameter;
	if (path->count <= 1) {
		out->X = out->Y = out->Width = out->Height = 0.0f;
		return Ok;
	}
	GpPath *copy = gp_path_clone(path);
	if (!copy)
		return OutOfMemory;
	GpStatus status = gp_path_flatten(copy, NULL, 0.25f);
	if (status == Ok) {
		GpRectF box;
		gp_path_bounds(copy, &box);
		if (pen) {
			REAL reach = pen->width / 2.0f;
			if (copy->count > 2 && pen->miter_limit > 1.0f)
				reach *= pen->miter_limit;
			if ((pen->end_cap == LineCapSquareAnchor || pen->start_cap == LineCapSquareAnchor) && pen->width * 2.2f > reach)
				reach = pen->width * 2.2f;
			box.X -= reach;
			box.Y -= reach;
			box.Width += reach * 2.0f;
			box.Height += reach * 2.0f;
		}
		*out = box;
		if (matrix) {
			GpPointF corners[4] = {{box.X, box.Y}, {box.X + box.Width, box.Y}, {box.X, box.Y + box.Height}, {box.X + box.Width, box.Y + box.Height}};
			gp_matrix_apply(matrix, corners, 4);
			REAL left = corners[0].X, right = left, top = corners[0].Y, bottom = top;
			for (int i = 1; i < 4; i++) {
				if (corners[i].X < left) left = corners[i].X;
				if (corners[i].X > right) right = corners[i].X;
				if (corners[i].Y < top) top = corners[i].Y;
				if (corners[i].Y > bottom) bottom = corners[i].Y;
			}
			out->X = left;
			out->Y = top;
			out->Width = right - left;
			out->Height = bottom - top;
		}
	}
	gp_path_delete(copy);
	return status;
}

GpStatus GDIPAPI GdipGetPathWorldBoundsI(GpPath *path, GpRect *out, const GpMatrix *matrix, const GpPen *pen)
{
	if (!out)
		return InvalidParameter;
	GpRectF bounds;
	GpStatus status = GdipGetPathWorldBounds(path, &bounds, matrix, pen);
	if (status == Ok) {
		out->X = (INT)bounds.X;
		out->Y = (INT)bounds.Y;
		out->Width = (INT)bounds.Width;
		out->Height = (INT)bounds.Height;
	}
	return status;
}

// Point-in-polygon over flattened points. Every figure counts as closed.
BOOL gp_flat_contains(const GpPointF *points, const BYTE *types, INT count, GpFillMode fill, REAL x, REAL y)
{
	INT winding = 0, start = 0;
	for (INT i = 0; i < count; i++) {
		BOOL ends = i + 1 == count || (types[i + 1] & PathPointTypePathTypeMask) == PathPointTypeStart;
		const GpPointF *a = &points[i], *b = ends ? &points[start] : &points[i + 1];
		if ((a->Y <= y) != (b->Y <= y)) {
			REAL at = a->X + (y - a->Y) / (b->Y - a->Y) * (b->X - a->X);
			if (at > x)
				winding += a->Y <= y ? 1 : -1;
		}
		if (ends)
			start = i + 1;
	}
	return fill == FillModeWinding ? winding != 0 : (winding & 1) != 0;
}

GpStatus GDIPAPI GdipIsVisiblePathPoint(GpPath *path, REAL x, REAL y, const void *graphics, BOOL *out)
{
	(void)graphics;
	if (!path || !out)
		return InvalidParameter;
	GpPath *copy = gp_path_clone(path);
	if (!copy)
		return OutOfMemory;
	GpStatus status = gp_path_flatten(copy, NULL, 0.25f);
	if (status == Ok)
		*out = gp_flat_contains(copy->points, copy->types, copy->count, copy->fill, x, y);
	gp_path_delete(copy);
	return status;
}

GpStatus GDIPAPI GdipIsVisiblePathPointI(GpPath *path, INT x, INT y, const void *graphics, BOOL *out)
{
	return GdipIsVisiblePathPoint(path, (REAL)x, (REAL)y, graphics, out);
}

// With a graphics the question is asked on the device: point and path go through its world, and the pen keeps its own size
// unless it is a world pen.
GpStatus GDIPAPI GdipIsOutlineVisiblePathPoint(GpPath *path, REAL x, REAL y, GpPen *pen, const GpGraphics *graphics, BOOL *out)
{
	if (!path || !pen || !out)
		return InvalidParameter;
	GpPath *copy = gp_path_clone(path);
	if (!copy)
		return OutOfMemory;
	GpPen device_pen = *pen;
	GpPointF point = {x, y};
	if (graphics) {
		GpMatrix to_device;
		gp_world_to_device(graphics, &to_device);
		gp_matrix_apply(&to_device, &point, 1);
		gp_matrix_apply(&to_device, copy->points, copy->count);
		REAL scale = pen->unit == UnitWorld ? gp_sqrtf(gp_fabs(to_device.m[0] * to_device.m[3] - to_device.m[1] * to_device.m[2])) : gp_unit_scale(graphics, pen->unit, graphics->dpi_x);
		device_pen.width = pen->width * scale;
		device_pen.unit = UnitPixel;
		if (device_pen.width < 1.0f)
			device_pen.width = 1.0f;
	}
	GpStatus status = gp_path_widen(copy, &device_pen, NULL, 0.25f);
	if (status == Ok)
		status = gp_path_flatten(copy, NULL, 0.25f);
	if (status == Ok)
		*out = gp_flat_contains(copy->points, copy->types, copy->count, FillModeWinding, point.X, point.Y);
	gp_path_delete(copy);
	return status;
}

GpStatus GDIPAPI GdipIsOutlineVisiblePathPointI(GpPath *path, INT x, INT y, GpPen *pen, const GpGraphics *graphics, BOOL *out)
{
	return GdipIsOutlineVisiblePathPoint(path, (REAL)x, (REAL)y, pen, graphics, out);
}

GpStatus GDIPAPI GdipWidenPath(GpPath *path, GpPen *pen, GpMatrix *matrix, REAL flatness)
{
	if (!path || !pen)
		return InvalidParameter;
	if (!path->count)
		return OutOfMemory;
	return gp_path_widen(path, pen, matrix, flatness);
}

// Bilinear or perspective mapping of the source rectangle onto four points; warped paths come out flattened.
GpStatus GDIPAPI GdipWarpPath(GpPath *path, GpMatrix *matrix, const GpPointF *points, INT count, REAL x, REAL y, REAL width, REAL height, WarpMode mode, REAL flatness)
{
	if (!path || !points || count < 3 || count > 4 || count == 0)
		return InvalidParameter;
	if (!path->count)
		return Ok;
	GpStatus status = gp_path_flatten(path, matrix, flatness);
	if (status != Ok)
		return status;
	GpPointF d = count == 4 ? points[3] : (GpPointF){points[1].X + points[2].X - points[0].X, points[1].Y + points[2].Y - points[0].Y};
	for (INT i = 0; i < path->count; i++) {
		REAL u = width ? (path->points[i].X - x) / width : 0.0f, v = height ? (path->points[i].Y - y) / height : 0.0f;
		REAL px, py;
		if (mode == WarpModePerspective && count == 4) {
			// The projective map of the unit square, from its corner images.
			REAL sx = points[0].X - points[1].X - points[2].X + d.X, sy = points[0].Y - points[1].Y - points[2].Y + d.Y;
			REAL dx1 = points[1].X - d.X, dy1 = points[1].Y - d.Y, dx2 = points[2].X - d.X, dy2 = points[2].Y - d.Y;
			REAL den = dx1 * dy2 - dx2 * dy1;
			REAL g = den ? (sx * dy2 - dx2 * sy) / den : 0.0f, h = den ? (dx1 * sy - sx * dy1) / den : 0.0f;
			REAL w = g * u + h * v + 1.0f;
			px = ((points[1].X - points[0].X + g * points[1].X) * u + (points[2].X - points[0].X + h * points[2].X) * v + points[0].X) / w;
			py = ((points[1].Y - points[0].Y + g * points[1].Y) * u + (points[2].Y - points[0].Y + h * points[2].Y) * v + points[0].Y) / w;
		} else {
			px = (1 - v) * ((1 - u) * points[0].X + u * points[1].X) + v * ((1 - u) * points[2].X + u * d.X);
			py = (1 - v) * ((1 - u) * points[0].Y + u * points[1].Y) + v * ((1 - u) * points[2].Y + u * d.Y);
		}
		path->points[i].X = px;
		path->points[i].Y = py;
	}
	return Ok;
}

// Merging overlapping figures into one outline needs boolean operations on paths, which the library does not have.
GpStatus GDIPAPI GdipWindingModeOutline(GpPath *path, GpMatrix *matrix, REAL flatness)
{
	(void)matrix;
	(void)flatness;
	return path ? NotImplemented : InvalidParameter;
}
