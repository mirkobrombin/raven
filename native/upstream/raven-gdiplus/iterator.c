/* SPDX-License-Identifier: MIT */
#include "gdip.h"

// A path iterator is a snapshot of a path with three independent cursors: figures, runs of one point type, markers.

GpStatus GDIPAPI GdipCreatePathIter(GpPathIterator **out, GpPath *path)
{
	if (!out)
		return InvalidParameter;
	GpPathIterator *iterator = gp_alloc(sizeof(GpPathIterator));
	if (!iterator)
		return OutOfMemory;
	if (path && path->count) {
		iterator->points = gp_dup(path->points, path->count * sizeof(GpPointF));
		iterator->types = gp_dup(path->types, path->count);
		if (!iterator->points || !iterator->types) {
			gp_free(iterator->points);
			gp_free(iterator->types);
			gp_free(iterator);
			return OutOfMemory;
		}
		iterator->count = path->count;
	}
	*out = iterator;
	return Ok;
}

GpStatus GDIPAPI GdipDeletePathIter(GpPathIterator *iterator)
{
	if (!iterator)
		return InvalidParameter;
	gp_free(iterator->points);
	gp_free(iterator->types);
	gp_free(iterator);
	return Ok;
}

GpStatus GDIPAPI GdipPathIterGetCount(GpPathIterator *iterator, INT *out)
{
	if (!iterator || !out)
		return InvalidParameter;
	*out = iterator->count;
	return Ok;
}

static BOOL is_start(const GpPathIterator *iterator, INT index)
{
	return (iterator->types[index] & PathPointTypePathTypeMask) == PathPointTypeStart;
}

GpStatus GDIPAPI GdipPathIterGetSubpathCount(GpPathIterator *iterator, INT *out)
{
	if (!iterator || !out)
		return InvalidParameter;
	INT figures = 0;
	for (INT i = 0; i < iterator->count; i++)
		figures += is_start(iterator, i);
	*out = figures;
	return Ok;
}

GpStatus GDIPAPI GdipPathIterHasCurve(GpPathIterator *iterator, BOOL *out)
{
	if (!iterator || !out)
		return InvalidParameter;
	*out = FALSE;
	for (INT i = 0; i < iterator->count; i++) {
		if ((iterator->types[i] & PathPointTypePathTypeMask) == PathPointTypeBezier)
			*out = TRUE;
	}
	return Ok;
}

GpStatus GDIPAPI GdipPathIterRewind(GpPathIterator *iterator)
{
	if (!iterator)
		return InvalidParameter;
	iterator->subpath = iterator->kind = iterator->kind_end = iterator->marker = 0;
	return Ok;
}

GpStatus GDIPAPI GdipPathIterCopyData(GpPathIterator *iterator, INT *out, GpPointF *points, BYTE *types, INT first, INT last)
{
	if (!iterator || !out || !points || !types)
		return InvalidParameter;
	if (last < first || first < 0 || last >= iterator->count) {
		*out = 0;
		return Ok;
	}
	*out = last - first + 1;
	memcpy(points, iterator->points + first, *out * sizeof(GpPointF));
	memcpy(types, iterator->types + first, *out);
	return Ok;
}

GpStatus GDIPAPI GdipPathIterEnumerate(GpPathIterator *iterator, INT *out, GpPointF *points, BYTE *types, INT count)
{
	if (!out)
		return InvalidParameter;
	if (count < 0)
		return InvalidParameter;
	return GdipPathIterCopyData(iterator, out, points, types, 0, (count < (iterator ? iterator->count : 0) ? count : (iterator ? iterator->count : 0)) - 1);
}

GpStatus GDIPAPI GdipPathIterNextSubpath(GpPathIterator *iterator, INT *out, INT *first, INT *last, BOOL *closed)
{
	if (!iterator || !out || !first || !last || !closed)
		return InvalidParameter;
	*out = 0;
	if (!iterator->count)
		return Ok;
	if (iterator->subpath >= iterator->count) {
		*first = *last = 0;
		*closed = TRUE;
		return Ok;
	}
	INT end = iterator->subpath + 1;
	while (end < iterator->count && !is_start(iterator, end))
		end++;
	*first = iterator->subpath;
	*last = end - 1;
	*closed = (iterator->types[end - 1] & PathPointTypeCloseSubpath) != 0;
	*out = end - iterator->subpath;
	iterator->subpath = end;
	iterator->kind = *first;
	iterator->kind_end = end;
	return Ok;
}

GpStatus GDIPAPI GdipPathIterNextSubpathPath(GpPathIterator *iterator, INT *out, GpPath *path, BOOL *closed)
{
	if (!iterator || !out || !closed)
		return InvalidParameter;
	INT first, last;
	GpStatus status = GdipPathIterNextSubpath(iterator, out, &first, &last, closed);
	if (status != Ok || !path || !*out)
		return status;
	path->count = 0;
	path->new_figure = TRUE;
	{
		if (!gp_path_reserve(path, *out))
			return OutOfMemory;
		memcpy(path->points, iterator->points + first, *out * sizeof(GpPointF));
		memcpy(path->types, iterator->types + first, *out);
		path->count = *out;
	}
	return Ok;
}

// Runs of one point type are walked inside the figure the last NextSubpath chose. A run shares its first point with the
// run before it, so a line then a Bezier meet at one index; a lone point is a run with no type to report.
GpStatus GDIPAPI GdipPathIterNextPathType(GpPathIterator *iterator, INT *out, BYTE *type, INT *first, INT *last)
{
	if (!iterator || !out || !type || !first || !last)
		return InvalidParameter;
	*out = 0;
	INT begin = iterator->kind, limit = iterator->kind_end;
	if (begin >= limit)
		return Ok;
	if (begin == limit - 1) {
		iterator->kind = limit;
		if (begin && !is_start(iterator, begin))
			return Ok;
		*first = *last = begin;
		*out = 1;
		return Ok;
	}
	BYTE kind = iterator->types[begin + 1] & PathPointTypePathTypeMask;
	INT end = begin + 1;
	while (end + 1 < limit && (iterator->types[end + 1] & PathPointTypePathTypeMask) == kind)
		end++;
	*first = begin;
	*last = end;
	*type = kind;
	*out = end - begin + 1;
	iterator->kind = end;
	return Ok;
}

GpStatus GDIPAPI GdipPathIterNextMarker(GpPathIterator *iterator, INT *out, INT *first, INT *last)
{
	if (!iterator || !out || !first || !last)
		return InvalidParameter;
	*out = 0;
	if (iterator->marker >= iterator->count)
		return Ok;
	INT end = iterator->marker;
	while (end + 1 < iterator->count && !(iterator->types[end] & PathPointTypePathMarker))
		end++;
	*first = iterator->marker;
	*last = end;
	*out = end - iterator->marker + 1;
	iterator->marker = end + 1;
	return Ok;
}

GpStatus GDIPAPI GdipPathIterNextMarkerPath(GpPathIterator *iterator, INT *out, GpPath *path)
{
	if (!iterator || !out)
		return InvalidParameter;
	INT first, last;
	GpStatus status = GdipPathIterNextMarker(iterator, out, &first, &last);
	if (status != Ok || !path || !*out)
		return status;
	path->count = 0;
	path->new_figure = TRUE;
	{
		if (!gp_path_reserve(path, *out))
			return OutOfMemory;
		memcpy(path->points, iterator->points + first, *out * sizeof(GpPointF));
		memcpy(path->types, iterator->types + first, *out);
		path->count = *out;
	}
	return Ok;
}

GpStatus GDIPAPI GdipPathIterIsValid(GpPathIterator *iterator, BOOL *out)
{
	if (!iterator || !out)
		return InvalidParameter;
	*out = TRUE;
	return Ok;
}
