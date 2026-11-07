/* SPDX-License-Identifier: MIT */
#include "gdip.h"
#include "gmath.h"

// Turns a flattened figure into the closed polygons of its stroke: a walk down the left side, the far cap, and a walk
// back up the other side, or two rings when the figure is closed. Filling the result with the winding rule gives the
// stroke, so overlaps inside a sharp turn do no harm. Anchor caps are figures of their own, added after every body.

#define ROUND_K 0.5522847f
#define SQRT2 1.41421356f
#define SQRT3 1.73205081f

typedef struct
{
	GpPath *out;
	REAL half;
	const GpPen *pen;
	BOOL fresh;
} walker;

static void emit_type(walker *w, REAL x, REAL y, BYTE type)
{
	if (!gp_path_reserve(w->out, 1))
		return;
	gp_path_push(w->out, x, y, w->fresh ? PathPointTypeStart : type);
	w->fresh = FALSE;
}

static void emit(walker *w, REAL x, REAL y)
{
	emit_type(w, x, y, PathPointTypeLine);
}

static void quarter(walker *w, REAL cx, REAL cy, REAL ax, REAL ay, REAL bx, REAL by)
{
	emit_type(w, cx + ax + bx * ROUND_K, cy + ay + by * ROUND_K, PathPointTypeBezier);
	emit_type(w, cx + bx + ax * ROUND_K, cy + by + ay * ROUND_K, PathPointTypeBezier);
	emit_type(w, cx + bx, cy + by, PathPointTypeBezier);
}

static void finish_ring(walker *w)
{
	if (w->out->count)
		w->out->types[w->out->count - 1] |= PathPointTypeCloseSubpath;
	w->fresh = TRUE;
}

static BOOL direction(const GpPointF *a, const GpPointF *b, REAL *dx, REAL *dy)
{
	REAL x = b->X - a->X, y = b->Y - a->Y;
	REAL length = gp_sqrtf(x * x + y * y);
	if (length < 1e-6f)
		return FALSE;
	*dx = x / length;
	*dy = y / length;
	return TRUE;
}

static void round_fan(walker *w, const GpPointF *c, REAL from, REAL sweep)
{
	int steps = (int)(gp_fabs(sweep) / 0.35f) + 1;
	for (int i = 1; i < steps; i++) {
		REAL angle = from + sweep * i / steps;
		emit(w, c->X + w->half * gp_cosf(angle), c->Y + w->half * gp_sinf(angle));
	}
}

// The turn at p on the walker's left side, walking from direction (ax, ay) to (bx, by). A right turn leaves the left side
// outside the corner; on the inside the two offset points are enough, since the winding fill swallows the crossing.
static void join(walker *w, const GpPointF *p, REAL ax, REAL ay, REAL bx, REAL by)
{
	REAL half = w->half;
	REAL nax = ay * half, nay = -ax * half, nbx = by * half, nby = -bx * half;
	REAL cross = ax * by - ay * bx, dot = ax * bx + ay * by;
	if (gp_fabs(cross) <= 1e-6f && dot > 0.0f) {
		emit(w, p->X + nax, p->Y + nay);
		return;
	}
	if (cross < 0.0f) {
		emit(w, p->X + nax, p->Y + nay);
		emit(w, p->X + nbx, p->Y + nby);
		return;
	}
	if (w->pen->join == LineJoinMiter || w->pen->join == LineJoinMiterClipped) {
		// The miter tip lies along the bisector; its length over half the width is 1/cos(half the turn).
		REAL mx = nax + nbx, my = nay + nby;
		REAL square = mx * mx + my * my;
		REAL scale = square > 1e-9f ? 2.0f * half * half / square : 0.0f;
		REAL ratio = gp_sqrtf(mx * mx * scale * scale + my * my * scale * scale) / half;
		if (scale && ratio <= w->pen->miter_limit) {
			emit(w, p->X + mx * scale, p->Y + my * scale);
			return;
		}
	}
	emit(w, p->X + nax, p->Y + nay);
	if (w->pen->join == LineJoinRound) {
		REAL from = (REAL)atan2(nay, nax), to = (REAL)atan2(nby, nbx);
		REAL sweep = to - from;
		if (sweep > GP_PI)
			sweep -= 2.0f * (REAL)GP_PI;
		else if (sweep < -GP_PI)
			sweep += 2.0f * (REAL)GP_PI;
		round_fan(w, p, from, sweep);
	}
	emit(w, p->X + nbx, p->Y + nby);
}

// The end of a stroke at p, reached walking along (dx, dy). Anchors leave the body flat here.
static void cap(walker *w, const GpPointF *p, REAL dx, REAL dy, GpLineCap kind)
{
	REAL half = w->half;
	REAL nx = dy * half, ny = -dx * half, fx = dx * half, fy = dy * half;
	switch (kind) {
	case LineCapSquare:
		emit(w, p->X + nx + fx, p->Y + ny + fy);
		emit(w, p->X - nx + fx, p->Y - ny + fy);
		break;
	case LineCapRound:
		emit(w, p->X + nx, p->Y + ny);
		quarter(w, p->X, p->Y, nx, ny, fx, fy);
		quarter(w, p->X, p->Y, fx, fy, -nx, -ny);
		break;
	case LineCapTriangle:
		emit(w, p->X + nx, p->Y + ny);
		emit(w, p->X + fx, p->Y + fy);
		emit(w, p->X - nx, p->Y - ny);
		break;
	default:
		emit(w, p->X + nx, p->Y + ny);
		emit(w, p->X - nx, p->Y - ny);
		break;
	}
}

static INT unique_points(const GpPointF *in, INT count, GpPointF *out)
{
	INT kept = 0;
	for (INT i = 0; i < count; i++) {
		if (kept && gp_fabs(out[kept - 1].X - in[i].X) < 1e-5f && gp_fabs(out[kept - 1].Y - in[i].Y) < 1e-5f)
			continue;
		out[kept++] = in[i];
	}
	return kept;
}

// The ring opens at the start cap's exit point, so a rectangle reads from its first corner around.
static void stroke_open(walker *w, const GpPointF *p, INT count, GpLineCap start, GpLineCap end)
{
	REAL dx, dy, nx, ny;
	if (count < 2)
		return;
	direction(&p[0], &p[1], &dx, &dy);
	if (start == LineCapSquare)
		emit(w, p[0].X + dy * w->half - dx * w->half, p[0].Y - dx * w->half - dy * w->half);
	else
		emit(w, p[0].X + dy * w->half, p[0].Y - dx * w->half);
	for (INT i = 0; i + 2 < count; i++) {
		if (direction(&p[i], &p[i + 1], &dx, &dy) && direction(&p[i + 1], &p[i + 2], &nx, &ny))
			join(w, &p[i + 1], dx, dy, nx, ny);
	}
	direction(&p[count - 2], &p[count - 1], &dx, &dy);
	cap(w, &p[count - 1], dx, dy, end);
	for (INT i = count - 1; i >= 2; i--) {
		if (direction(&p[i], &p[i - 1], &dx, &dy) && direction(&p[i - 1], &p[i - 2], &nx, &ny))
			join(w, &p[i - 1], dx, dy, nx, ny);
	}
	direction(&p[1], &p[0], &dx, &dy);
	REAL rx = dy * w->half, ry = -dx * w->half, fx = dx * w->half, fy = dy * w->half;
	switch (start) {
	case LineCapSquare:
		emit(w, p[0].X + rx + fx, p[0].Y + ry + fy);
		break;
	case LineCapRound:
		emit(w, p[0].X + rx, p[0].Y + ry);
		quarter(w, p[0].X, p[0].Y, rx, ry, fx, fy);
		quarter(w, p[0].X, p[0].Y, fx, fy, -rx, -ry);
		break;
	case LineCapTriangle:
		emit(w, p[0].X + rx, p[0].Y + ry);
		emit(w, p[0].X + fx, p[0].Y + fy);
		emit(w, p[0].X - rx, p[0].Y - ry);
		break;
	default:
		emit(w, p[0].X + rx, p[0].Y + ry);
		break;
	}
	finish_ring(w);
}

static void stroke_closed(walker *w, const GpPointF *p, INT count)
{
	REAL dx, dy, nx, ny;
	for (INT k = 0; k < count; k++) {
		INT i = (k + count - 1) % count;
		if (direction(&p[i], &p[(i + 1) % count], &dx, &dy) && direction(&p[(i + 1) % count], &p[(i + 2) % count], &nx, &ny))
			join(w, &p[(i + 1) % count], dx, dy, nx, ny);
	}
	finish_ring(w);
	for (INT k = 0; k < count; k++) {
		INT i = (count - k) % count;
		INT b = (i + count - 1) % count, c = (i + count - 2) % count;
		if (direction(&p[i], &p[b], &dx, &dy) && direction(&p[b], &p[c], &nx, &ny))
			join(w, &p[b], dx, dy, nx, ny);
	}
	finish_ring(w);
}

// An anchor at c whose outward direction along the line is (dx, dy).
static void anchor(walker *w, const GpPointF *c, REAL dx, REAL dy, GpLineCap kind)
{
	REAL half = w->half < 1.0f ? 1.0f : w->half, width = half * 2.0f;
	REAL nx = -dy, ny = dx;
	switch (kind) {
	case LineCapSquareAnchor: {
		REAL s = half * SQRT2;
		emit(w, c->X + (-dx + nx) * s, c->Y + (-dy + ny) * s);
		emit(w, c->X + (-dx - nx) * s, c->Y + (-dy - ny) * s);
		emit(w, c->X + (dx - nx) * s, c->Y + (dy - ny) * s);
		emit(w, c->X + (dx + nx) * s, c->Y + (dy + ny) * s);
		break;
	}
	case LineCapRoundAnchor: {
		REAL ax = -nx * width, ay = -ny * width, bx = dx * width, by = dy * width;
		emit(w, c->X + ax, c->Y + ay);
		quarter(w, c->X, c->Y, ax, ay, bx, by);
		quarter(w, c->X, c->Y, bx, by, -ax, -ay);
		quarter(w, c->X, c->Y, -ax, -ay, -bx, -by);
		quarter(w, c->X, c->Y, -bx, -by, ax, ay);
		break;
	}
	case LineCapDiamondAnchor:
		emit(w, c->X + dx * width, c->Y + dy * width);
		emit(w, c->X + nx * width, c->Y + ny * width);
		emit(w, c->X - dx * width, c->Y - dy * width);
		emit(w, c->X - nx * width, c->Y - ny * width);
		break;
	case LineCapArrowAnchor: {
		REAL back = width * SQRT3;
		emit(w, c->X, c->Y);
		emit(w, c->X - dx * back + nx * width, c->Y - dy * back + ny * width);
		emit(w, c->X - dx * back - nx * width, c->Y - dy * back - ny * width);
		break;
	}
	default:
		return;
	}
	finish_ring(w);
}

static BOOL is_anchor(GpLineCap kind)
{
	return kind == LineCapSquareAnchor || kind == LineCapRoundAnchor || kind == LineCapDiamondAnchor || kind == LineCapArrowAnchor;
}

// Walks the polyline handing each "on" run to the open stroker with the pen's dash cap at both ends. The pattern counts
// in pen widths, and a pen thinner than a pixel still measures its dashes in whole pixels.
static void stroke_dashes(walker *w, const GpPointF *p, INT count, BOOL closed, const REAL *pattern, INT length)
{
	REAL unit = w->half * 2.0f;
	if (unit < 1.0f)
		unit = 1.0f;
	REAL period = 0.0f;
	for (INT i = 0; i < length; i++)
		period += pattern[i];
	if (period <= 0.0f)
		return;
	INT phase = 0;
	REAL left = (REAL)fmod(w->pen->dash_offset, period);
	while (left >= pattern[phase]) {
		left -= pattern[phase];
		phase = (phase + 1) % length;
	}
	left = (pattern[phase] - left) * unit;
	INT total = closed ? count + 1 : count;
	GpPointF *run = gp_alloc((total * 2 + 2) * sizeof(GpPointF));
	if (!run)
		return;
	INT used = 0;
	GpLineCap dash = (GpLineCap)w->pen->dash_cap;
	if (!(phase & 1))
		run[used++] = p[0];
	for (INT i = 0; i + 1 < total; i++) {
		GpPointF a = p[i % count], b = p[(i + 1) % count];
		REAL dx = b.X - a.X, dy = b.Y - a.Y, span = gp_sqrtf(dx * dx + dy * dy), walked = 0.0f;
		while (span - walked > left) {
			walked += left;
			GpPointF at = {a.X + dx * walked / span, a.Y + dy * walked / span};
			if (!(phase & 1)) {
				run[used++] = at;
				stroke_open(w, run, used, dash, dash);
				used = 0;
			} else {
				run[used++] = at;
			}
			phase = (phase + 1) % length;
			left = pattern[phase] * unit;
			if (left <= 0.0f)
				left = 1e-3f;
		}
		left -= span - walked;
		if (!(phase & 1) && used < total * 2)
			run[used++] = b;
	}
	if (!(phase & 1) && used > 1)
		stroke_open(w, run, used, dash, dash);
	gp_free(run);
}

// A world pen widens first and goes through the matrix with the path; any other pen keeps its size, so the path moves
// first. Zero width means nothing in pixels and one unit in world space.
GpStatus gp_path_widen(GpPath *path, const GpPen *pen, const GpMatrix *matrix, REAL flatness)
{
	BOOL world = pen->unit == UnitWorld;
	GpStatus status = gp_path_flatten(path, world ? NULL : matrix, flatness);
	if (status != Ok)
		return status;
	GpPath *out;
	if ((status = gp_path_new(FillModeWinding, &out)) != Ok)
		return status;
	REAL width = pen->width;
	if (width <= 0.0f && world)
		width = 1.0f;
	walker w = {out, width / 2.0f, pen, TRUE};
	INT pattern_length = pen->dash_style == DashStyleSolid ? 0 : pen->dash_count;
	const REAL *dashes = pen->dashes;
	GpPointF *clean = gp_alloc((path->count + 1) * sizeof(GpPointF));
	if (!clean) {
		gp_path_delete(out);
		return OutOfMemory;
	}
	GpLineCap start_cap = is_anchor(pen->start_cap) ? LineCapFlat : pen->start_cap;
	GpLineCap end_cap = is_anchor(pen->end_cap) ? LineCapFlat : pen->end_cap;
	if (width > 0.0f) {
		for (INT start = 0; start < path->count;) {
			INT end = start + 1;
			while (end < path->count && (path->types[end] & PathPointTypePathTypeMask) != PathPointTypeStart)
				end++;
			BOOL closed = (path->types[end - 1] & PathPointTypeCloseSubpath) != 0;
			INT count = unique_points(path->points + start, end - start, clean);
			if (closed && count > 1 && gp_fabs(clean[0].X - clean[count - 1].X) < 1e-5f && gp_fabs(clean[0].Y - clean[count - 1].Y) < 1e-5f)
				count--;
			if (count > 1 && !closed) {
				// An arrow stands in front of the line, so the body stops where its head begins.
				REAL dx, dy;
				if (pen->start_cap == LineCapArrowAnchor && direction(&clean[0], &clean[1], &dx, &dy)) {
					clean[0].X += dx * width;
					clean[0].Y += dy * width;
				}
				if (pen->end_cap == LineCapArrowAnchor && direction(&clean[count - 2], &clean[count - 1], &dx, &dy)) {
					clean[count - 1].X -= dx * width;
					clean[count - 1].Y -= dy * width;
				}
			}
			if (pattern_length && count > 1)
				stroke_dashes(&w, clean, count, closed, dashes, pattern_length);
			else if (closed && count > 2)
				stroke_closed(&w, clean, count);
			else
				stroke_open(&w, clean, count, start_cap, end_cap);
			start = end;
		}
	}
	for (INT start = 0; start < path->count;) {
		INT end = start + 1;
		while (end < path->count && (path->types[end] & PathPointTypePathTypeMask) != PathPointTypeStart)
			end++;
		BOOL closed = (path->types[end - 1] & PathPointTypeCloseSubpath) != 0;
		INT count = unique_points(path->points + start, end - start, clean);
		REAL dx, dy;
		if (!closed && count > 1) {
			if (direction(&clean[1], &clean[0], &dx, &dy))
				anchor(&w, &clean[0], dx, dy, pen->start_cap);
			if (direction(&clean[count - 2], &clean[count - 1], &dx, &dy))
				anchor(&w, &clean[count - 1], dx, dy, pen->end_cap);
		}
		start = end;
	}
	gp_free(clean);
	if (world && matrix && out->count)
		gp_matrix_apply(matrix, out->points, out->count);
	gp_free(path->points);
	gp_free(path->types);
	path->points = out->points;
	path->types = out->types;
	path->count = out->count;
	path->capacity = out->capacity;
	path->fill = FillModeWinding;
	path->new_figure = TRUE;
	gp_free(out);
	return Ok;
}
