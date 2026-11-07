/* SPDX-License-Identifier: MIT */
#include "graphics.h"
#include "gmath.h"

#define INFINITE_ORIGIN (-4194304.0f)
#define INFINITE_SIZE 8388608.0f
#define REGION_MAGIC 0xdbc01002
#define PATH_MAGIC 0xdbc01002

static gp_node *new_node(DWORD type)
{
	gp_node *node = gp_alloc(sizeof(gp_node));
	if (node)
		node->type = type;
	return node;
}

static void free_node(gp_node *node)
{
	if (!node)
		return;
	free_node(node->left);
	free_node(node->right);
	if (node->path)
		gp_path_delete(node->path);
	if (node->flat)
		gp_path_delete(node->flat);
	gp_free(node);
}

static gp_node *clone_node(const gp_node *node)
{
	gp_node *copy = new_node(node->type);
	if (!copy)
		return NULL;
	copy->rect = node->rect;
	if (node->path)
		copy->path = gp_path_clone(node->path);
	if (node->flat)
		copy->flat = gp_path_clone(node->flat);
	if (node->left)
		copy->left = clone_node(node->left);
	if (node->right)
		copy->right = clone_node(node->right);
	if ((node->path && !copy->path) || (node->left && !copy->left) || (node->right && !copy->right)) {
		free_node(copy);
		return NULL;
	}
	return copy;
}

static gp_node *rect_node(const GpRectF *rect)
{
	gp_node *node = new_node(NODE_RECT);
	if (node)
		node->rect = *rect;
	return node;
}

static gp_node *path_node(const GpPath *path)
{
	gp_node *node = new_node(NODE_PATH);
	if (!node)
		return NULL;
	if (!(node->path = gp_path_clone(path)) || !(node->flat = gp_path_clone(path)) || gp_path_flatten(node->flat, NULL, 0.25f) != Ok) {
		free_node(node);
		return NULL;
	}
	return node;
}

static GpStatus wrap_root(GpRegion **out, gp_node *root)
{
	if (!root)
		return OutOfMemory;
	GpRegion *region = gp_alloc(sizeof(GpRegion));
	if (!region) {
		free_node(root);
		return OutOfMemory;
	}
	region->root = root;
	*out = region;
	return Ok;
}

// A plain rectangle region stays a rectangle under replacement or an intersection with another rectangle, and the
// trivial operands fold away, so the stored tree stays what callers read back.
static GpStatus combine_node(GpRegion *region, gp_node *added, CombineMode mode)
{
	if (!added)
		return OutOfMemory;
	if ((UINT)mode > CombineModeComplement) {
		free_node(added);
		return InvalidParameter;
	}
	gp_node *root = region->root;
	if (mode == CombineModeReplace) {
		free_node(root);
		region->root = added;
		return Ok;
	}
	if (mode == CombineModeIntersect && root->type == NODE_INFINITE) {
		free_node(root);
		region->root = added;
		return Ok;
	}
	if (mode == CombineModeIntersect && added->type == NODE_INFINITE) {
		free_node(added);
		return Ok;
	}
	if (mode == CombineModeUnion && root->type == NODE_EMPTY) {
		free_node(root);
		region->root = added;
		return Ok;
	}
	gp_node *join = new_node((DWORD)mode);
	if (!join) {
		free_node(added);
		return OutOfMemory;
	}
	join->left = root;
	join->right = added;
	region->root = join;
	return Ok;
}

GpStatus GDIPAPI GdipCreateRegion(GpRegion **out)
{
	if (!out)
		return InvalidParameter;
	return wrap_root(out, new_node(NODE_INFINITE));
}

GpStatus GDIPAPI GdipCreateRegionRect(const GpRectF *rect, GpRegion **out)
{
	if (!rect || !out)
		return InvalidParameter;
	return wrap_root(out, rect_node(rect));
}

GpStatus GDIPAPI GdipCreateRegionRectI(const GpRect *rect, GpRegion **out)
{
	if (!rect || !out)
		return InvalidParameter;
	GpRectF box = {(REAL)rect->X, (REAL)rect->Y, (REAL)rect->Width, (REAL)rect->Height};
	return wrap_root(out, rect_node(&box));
}

GpStatus GDIPAPI GdipCreateRegionPath(GpPath *path, GpRegion **out)
{
	if (!path || !out)
		return InvalidParameter;
	return wrap_root(out, path_node(path));
}

GpStatus GDIPAPI GdipCreateRegionHrgn(HRGN handle, GpRegion **out)
{
	if (!handle || !out)
		return InvalidParameter;
	DWORD size = GetRegionData(handle, 0, NULL);
	if (!size)
		return InvalidParameter;
	RGNDATA *data = gp_alloc(size);
	if (!data)
		return OutOfMemory;
	GpStatus status = Ok;
	if (!GetRegionData(handle, size, data))
		status = GenericError;
	gp_node *root = NULL;
	if (status == Ok) {
		if (!data->rdh.nCount) {
			root = new_node(NODE_EMPTY);
		} else {
			GpPath *path;
			status = gp_path_new(FillModeAlternate, &path);
			if (status == Ok) {
				const RECT *rects = (const RECT *)data->Buffer;
				for (DWORD i = 0; i < data->rdh.nCount; i++) {
					if (!gp_path_reserve(path, 4))
						break;
					gp_path_push(path, (REAL)rects[i].left, (REAL)rects[i].top, PathPointTypeStart);
					gp_path_push(path, (REAL)rects[i].right, (REAL)rects[i].top, PathPointTypeLine);
					gp_path_push(path, (REAL)rects[i].right, (REAL)rects[i].bottom, PathPointTypeLine);
					gp_path_push(path, (REAL)rects[i].left, (REAL)rects[i].bottom, PathPointTypeLine | PathPointTypeCloseSubpath);
				}
				root = path_node(path);
				gp_path_delete(path);
			}
		}
	}
	gp_free(data);
	if (status != Ok)
		return status;
	return wrap_root(out, root);
}

GpRegion *gp_region_clone(const GpRegion *region)
{
	GpRegion *copy = gp_alloc(sizeof(GpRegion));
	if (copy && !(copy->root = clone_node(region->root))) {
		gp_free(copy);
		return NULL;
	}
	return copy;
}

void gp_region_delete(GpRegion *region)
{
	free_node(region->root);
	gp_free(region);
}

GpStatus GDIPAPI GdipCloneRegion(GpRegion *region, GpRegion **out)
{
	if (!region || !out)
		return InvalidParameter;
	return (*out = gp_region_clone(region)) ? Ok : OutOfMemory;
}

GpStatus GDIPAPI GdipDeleteRegion(GpRegion *region)
{
	if (!region)
		return InvalidParameter;
	gp_region_delete(region);
	return Ok;
}

static GpStatus set_root(GpRegion *region, DWORD type)
{
	gp_node *node = new_node(type);
	if (!node)
		return OutOfMemory;
	free_node(region->root);
	region->root = node;
	return Ok;
}

GpStatus GDIPAPI GdipSetInfinite(GpRegion *region)
{
	return region ? set_root(region, NODE_INFINITE) : InvalidParameter;
}

GpStatus GDIPAPI GdipSetEmpty(GpRegion *region)
{
	return region ? set_root(region, NODE_EMPTY) : InvalidParameter;
}

GpStatus GDIPAPI GdipCombineRegionRect(GpRegion *region, const GpRectF *rect, CombineMode mode)
{
	if (!region || !rect)
		return InvalidParameter;
	return combine_node(region, rect_node(rect), mode);
}

GpStatus GDIPAPI GdipCombineRegionRectI(GpRegion *region, const GpRect *rect, CombineMode mode)
{
	if (!rect)
		return InvalidParameter;
	GpRectF box = {(REAL)rect->X, (REAL)rect->Y, (REAL)rect->Width, (REAL)rect->Height};
	return GdipCombineRegionRect(region, &box, mode);
}

GpStatus GDIPAPI GdipCombineRegionPath(GpRegion *region, GpPath *path, CombineMode mode)
{
	if (!region || !path)
		return InvalidParameter;
	return combine_node(region, path_node(path), mode);
}

GpStatus GDIPAPI GdipCombineRegionRegion(GpRegion *region, GpRegion *other, CombineMode mode)
{
	if (!region || !other)
		return InvalidParameter;
	return combine_node(region, clone_node(other->root), mode);
}

static BOOL node_contains(const gp_node *node, REAL x, REAL y)
{
	switch (node->type) {
	case NODE_RECT:
		return x >= node->rect.X && x < node->rect.X + node->rect.Width && y >= node->rect.Y && y < node->rect.Y + node->rect.Height;
	case NODE_PATH:
		return gp_flat_contains(node->flat->points, node->flat->types, node->flat->count, node->flat->fill, x, y);
	case NODE_EMPTY:
		return FALSE;
	case NODE_INFINITE:
		return TRUE;
	case CombineModeIntersect:
		return node_contains(node->left, x, y) && node_contains(node->right, x, y);
	case CombineModeUnion:
		return node_contains(node->left, x, y) || node_contains(node->right, x, y);
	case CombineModeXor:
		return node_contains(node->left, x, y) != node_contains(node->right, x, y);
	case CombineModeExclude:
		return node_contains(node->left, x, y) && !node_contains(node->right, x, y);
	case CombineModeComplement:
		return node_contains(node->right, x, y) && !node_contains(node->left, x, y);
	default:
		return FALSE;
	}
}

// Pixels a rectangle's edge falls on: coordinates count in sixteenths, and a pixel starts on its own left or top edge.
static REAL pixel_edge(REAL value)
{
	return gp_ceil(gp_floor(value * 16.0f + 0.5f) / 16.0f);
}

// Whether the pixel at whole coordinates belongs to the region as the screen shows it.
static BOOL node_covers(const gp_node *node, REAL x, REAL y)
{
	switch (node->type) {
	case NODE_RECT:
		return x >= pixel_edge(node->rect.X) && x < pixel_edge(node->rect.X + node->rect.Width)
			&& y >= pixel_edge(node->rect.Y) && y < pixel_edge(node->rect.Y + node->rect.Height);
	case NODE_PATH:
		return gp_flat_contains(node->flat->points, node->flat->types, node->flat->count, node->flat->fill, x + 0.5f, y + 0.5f);
	case NODE_EMPTY:
		return FALSE;
	case NODE_INFINITE:
		return TRUE;
	case CombineModeIntersect:
		return node_covers(node->left, x, y) && node_covers(node->right, x, y);
	case CombineModeUnion:
		return node_covers(node->left, x, y) || node_covers(node->right, x, y);
	case CombineModeXor:
		return node_covers(node->left, x, y) != node_covers(node->right, x, y);
	case CombineModeExclude:
		return node_covers(node->left, x, y) && !node_covers(node->right, x, y);
	case CombineModeComplement:
		return node_covers(node->right, x, y) && !node_covers(node->left, x, y);
	default:
		return FALSE;
	}
}

BOOL gp_region_contains(const GpRegion *region, REAL x, REAL y)
{
	return node_contains(region->root, x, y);
}

// Edge lists for the grid the region is cut into: every rectangle edge, and every whole pixel across a path's bounds.
typedef struct
{
	REAL *values;
	INT count, capacity;
} edge_list;

static void add_edge(edge_list *list, REAL value)
{
	if (list->count == list->capacity) {
		INT capacity = list->capacity ? list->capacity * 2 : 64;
		REAL *grown = gp_realloc(list->values, capacity * sizeof(REAL));
		if (!grown)
			return;
		list->values = grown;
		list->capacity = capacity;
	}
	list->values[list->count++] = value;
}

static void collect_edges(const gp_node *node, edge_list *xs, edge_list *ys, BOOL pixels)
{
	if (node->type == NODE_RECT) {
		REAL right = node->rect.X + node->rect.Width, bottom = node->rect.Y + node->rect.Height;
		add_edge(xs, pixels ? pixel_edge(node->rect.X) : node->rect.X);
		add_edge(xs, pixels ? pixel_edge(right) : right);
		add_edge(ys, pixels ? pixel_edge(node->rect.Y) : node->rect.Y);
		add_edge(ys, pixels ? pixel_edge(bottom) : bottom);
	} else if (node->type == NODE_INFINITE) {
		add_edge(xs, INFINITE_ORIGIN);
		add_edge(xs, INFINITE_ORIGIN + INFINITE_SIZE);
		add_edge(ys, INFINITE_ORIGIN);
		add_edge(ys, INFINITE_ORIGIN + INFINITE_SIZE);
	} else if (node->type == NODE_PATH) {
		GpRectF box;
		gp_path_bounds(node->flat, &box);
		INT left = (INT)gp_floor(box.X), right = (INT)gp_ceil(box.X + box.Width);
		INT top = (INT)gp_floor(box.Y), bottom = (INT)gp_ceil(box.Y + box.Height);
		for (INT x = left; x <= right; x++)
			add_edge(xs, (REAL)x);
		for (INT y = top; y <= bottom; y++)
			add_edge(ys, (REAL)y);
	} else if (node->left) {
		collect_edges(node->left, xs, ys, pixels);
		collect_edges(node->right, xs, ys, pixels);
	}
}

static int compare_reals(const void *a, const void *b)
{
	REAL x = *(const REAL *)a, y = *(const REAL *)b;
	return x < y ? -1 : x > y;
}

static void sort_edges(edge_list *list)
{
	extern void qsort(void *, size_t, size_t, int (__cdecl *)(const void *, const void *));
	if (!list->count)
		return;
	qsort(list->values, list->count, sizeof(REAL), (int (__cdecl *)(const void *, const void *))compare_reals);
	INT kept = 1;
	for (INT i = 1; i < list->count; i++) {
		if (list->values[i] != list->values[kept - 1])
			list->values[kept++] = list->values[i];
	}
	list->count = kept;
}

static GpStatus cut_region(const GpRegion *region, const GpMatrix *matrix, BOOL pixels, GpRectF **out, INT *count)
{
	GpRegion *work = gp_region_clone(region);
	if (!work)
		return OutOfMemory;
	if (matrix) {
		GpStatus status = gp_region_transform(work, matrix);
		if (status != Ok) {
			gp_region_delete(work);
			return status;
		}
	}
	edge_list xs = {0}, ys = {0};
	collect_edges(work->root, &xs, &ys, pixels);
	sort_edges(&xs);
	sort_edges(&ys);
	INT capacity = 16, used = 0, row_start = 0, previous_start = 0, previous_count = 0;
	GpRectF *rects = gp_alloc(capacity * sizeof(GpRectF));
	if (!rects) {
		gp_region_delete(work);
		return OutOfMemory;
	}
	for (INT j = 0; j + 1 < ys.count; j++) {
		REAL top = ys.values[j], bottom = ys.values[j + 1], cy = (top + bottom) / 2.0f;
		row_start = used;
		for (INT i = 0; i + 1 < xs.count; i++) {
			REAL left = xs.values[i], right = xs.values[i + 1], cx = (left + right) / 2.0f;
			if (pixels ? !node_covers(work->root, left, top) : !node_contains(work->root, cx, cy))
				continue;
			if (used > row_start && rects[used - 1].X + rects[used - 1].Width == left) {
				rects[used - 1].Width = right - rects[used - 1].X;
				continue;
			}
			if (used == capacity) {
				capacity *= 2;
				GpRectF *grown = gp_realloc(rects, capacity * sizeof(GpRectF));
				if (!grown)
					break;
				rects = grown;
			}
			rects[used].X = left;
			rects[used].Y = top;
			rects[used].Width = right - left;
			rects[used].Height = bottom - top;
			used++;
		}
		// A row laid out like the one above it extends that one.
		INT here = used - row_start;
		BOOL same = here && here == previous_count && rects[previous_start].Y + rects[previous_start].Height == top;
		for (INT i = 0; same && i < here; i++) {
			if (rects[previous_start + i].X != rects[row_start + i].X || rects[previous_start + i].Width != rects[row_start + i].Width)
				same = FALSE;
		}
		if (same) {
			for (INT i = 0; i < here; i++)
				rects[previous_start + i].Height += bottom - top;
			used = row_start;
		} else if (here) {
			previous_start = row_start;
			previous_count = here;
		}
	}
	gp_free(xs.values);
	gp_free(ys.values);
	gp_region_delete(work);
	*out = rects;
	*count = used;
	return Ok;
}

GpStatus gp_region_pixels(const GpRegion *region, const GpMatrix *matrix, GpRectF **out, INT *count)
{
	return cut_region(region, matrix, TRUE, out, count);
}

GpStatus gp_region_scans(const GpRegion *region, const GpMatrix *matrix, GpRectF **out, INT *count)
{
	return cut_region(region, matrix, FALSE, out, count);
}

BOOL gp_region_is_infinite(const GpRegion *region)
{
	return region->root->type == NODE_INFINITE;
}

BOOL gp_region_is_empty(const GpRegion *region)
{
	if (region->root->type == NODE_EMPTY)
		return TRUE;
	if (region->root->type == NODE_INFINITE)
		return FALSE;
	GpRectF *rects;
	INT count;
	if (gp_region_scans(region, NULL, &rects, &count) != Ok)
		return FALSE;
	gp_free(rects);
	return count == 0;
}

BOOL gp_region_bounds(const GpRegion *region, GpRectF *out)
{
	// A lone path knows its bounds better than the pixels it was cut into.
	if (region->root->type == NODE_PATH) {
		gp_path_bounds(region->root->flat, out);
		return TRUE;
	}
	GpRectF *rects;
	INT count;
	if (gp_region_scans(region, NULL, &rects, &count) != Ok || !count) {
		if (count == 0)
			gp_free(rects);
		out->X = out->Y = out->Width = out->Height = 0.0f;
		return FALSE;
	}
	REAL left = rects[0].X, top = rects[0].Y, right = left + rects[0].Width, bottom = top + rects[0].Height;
	for (INT i = 1; i < count; i++) {
		if (rects[i].X < left) left = rects[i].X;
		if (rects[i].Y < top) top = rects[i].Y;
		if (rects[i].X + rects[i].Width > right) right = rects[i].X + rects[i].Width;
		if (rects[i].Y + rects[i].Height > bottom) bottom = rects[i].Y + rects[i].Height;
	}
	gp_free(rects);
	out->X = left;
	out->Y = top;
	out->Width = right - left;
	out->Height = bottom - top;
	return TRUE;
}

static BOOL rect_stays_rect(const GpMatrix *matrix)
{
	return matrix->m[1] == 0.0f && matrix->m[2] == 0.0f;
}

static GpStatus transform_node(gp_node *node, const GpMatrix *matrix)
{
	if (node->type == NODE_RECT) {
		GpPointF corners[4] = {{node->rect.X, node->rect.Y}, {node->rect.X + node->rect.Width, node->rect.Y},
			{node->rect.X + node->rect.Width, node->rect.Y + node->rect.Height}, {node->rect.X, node->rect.Y + node->rect.Height}};
		gp_matrix_apply(matrix, corners, 4);
		if (rect_stays_rect(matrix)) {
			node->rect.X = corners[0].X < corners[2].X ? corners[0].X : corners[2].X;
			node->rect.Y = corners[0].Y < corners[2].Y ? corners[0].Y : corners[2].Y;
			node->rect.Width = gp_fabs(corners[2].X - corners[0].X);
			node->rect.Height = gp_fabs(corners[2].Y - corners[0].Y);
			return Ok;
		}
		GpPath *path;
		GpStatus status = gp_path_new(FillModeAlternate, &path);
		if (status != Ok)
			return status;
		if (!gp_path_reserve(path, 4)) {
			gp_path_delete(path);
			return OutOfMemory;
		}
		gp_path_push(path, corners[0].X, corners[0].Y, PathPointTypeStart);
		gp_path_push(path, corners[1].X, corners[1].Y, PathPointTypeLine);
		gp_path_push(path, corners[2].X, corners[2].Y, PathPointTypeLine);
		gp_path_push(path, corners[3].X, corners[3].Y, PathPointTypeLine | PathPointTypeCloseSubpath);
		node->type = NODE_PATH;
		node->path = path;
		node->flat = gp_path_clone(path);
		return node->flat ? Ok : OutOfMemory;
	}
	if (node->type == NODE_PATH) {
		gp_matrix_apply(matrix, node->path->points, node->path->count);
		gp_matrix_apply(matrix, node->flat->points, node->flat->count);
		return Ok;
	}
	if (node->left) {
		GpStatus status = transform_node(node->left, matrix);
		return status == Ok ? transform_node(node->right, matrix) : status;
	}
	return Ok;
}

GpStatus gp_region_transform(GpRegion *region, const GpMatrix *matrix)
{
	return transform_node(region->root, matrix);
}

GpStatus GDIPAPI GdipTransformRegion(GpRegion *region, GpMatrix *matrix)
{
	if (!region || !matrix)
		return InvalidParameter;
	return gp_region_transform(region, matrix);
}

GpStatus GDIPAPI GdipTranslateRegion(GpRegion *region, REAL dx, REAL dy)
{
	if (!region)
		return InvalidParameter;
	GpMatrix step = {{1.0f, 0.0f, 0.0f, 1.0f, dx, dy}};
	return gp_region_transform(region, &step);
}

GpStatus GDIPAPI GdipTranslateRegionI(GpRegion *region, INT dx, INT dy)
{
	return GdipTranslateRegion(region, (REAL)dx, (REAL)dy);
}

GpStatus GDIPAPI GdipGetRegionBounds(GpRegion *region, const void *graphics, GpRectF *out)
{
	if (!region || !graphics || !out)
		return InvalidParameter;
	gp_region_bounds(region, out);
	return Ok;
}

GpStatus GDIPAPI GdipGetRegionBoundsI(GpRegion *region, const void *graphics, GpRect *out)
{
	if (!out)
		return InvalidParameter;
	GpRectF box;
	GpStatus status = GdipGetRegionBounds(region, graphics, &box);
	if (status == Ok) {
		out->X = (INT)gp_floor(box.X + 0.5f);
		out->Y = (INT)gp_floor(box.Y + 0.5f);
		out->Width = (INT)gp_ceil(box.Width);
		out->Height = (INT)gp_ceil(box.Height);
	}
	return status;
}

GpStatus GDIPAPI GdipIsEmptyRegion(GpRegion *region, const void *graphics, BOOL *out)
{
	if (!region || !graphics || !out)
		return InvalidParameter;
	*out = gp_region_is_empty(region);
	return Ok;
}

GpStatus GDIPAPI GdipIsInfiniteRegion(GpRegion *region, const void *graphics, BOOL *out)
{
	if (!region || !graphics || !out)
		return InvalidParameter;
	*out = gp_region_is_infinite(region);
	return Ok;
}

// Regions are equal when they cover the same rows of rectangles.
GpStatus GDIPAPI GdipIsEqualRegion(GpRegion *region, GpRegion *other, const void *graphics, BOOL *out)
{
	if (!region || !other || !graphics || !out)
		return InvalidParameter;
	GpRectF *a, *b;
	INT count_a, count_b;
	GpStatus status = cut_region(region, NULL, TRUE, &a, &count_a);
	if (status != Ok)
		return status;
	status = cut_region(other, NULL, TRUE, &b, &count_b);
	if (status != Ok) {
		gp_free(a);
		return status;
	}
	*out = count_a == count_b && (!count_a || !memcmp(a, b, count_a * sizeof(GpRectF)));
	gp_free(a);
	gp_free(b);
	return Ok;
}

GpStatus GDIPAPI GdipIsVisibleRegionPoint(GpRegion *region, REAL x, REAL y, const void *graphics, BOOL *out)
{
	(void)graphics;
	if (!region || !out)
		return InvalidParameter;
	*out = node_covers(region->root, gp_floor(x + 0.5f), gp_floor(y + 0.5f));
	return Ok;
}

GpStatus GDIPAPI GdipIsVisibleRegionPointI(GpRegion *region, INT x, INT y, const void *graphics, BOOL *out)
{
	return GdipIsVisibleRegionPoint(region, (REAL)x, (REAL)y, graphics, out);
}

GpStatus GDIPAPI GdipIsVisibleRegionRect(GpRegion *region, REAL x, REAL y, REAL width, REAL height, const void *graphics, BOOL *out)
{
	(void)graphics;
	if (!region || !out)
		return InvalidParameter;
	if (!width || !height) {
		*out = FALSE;
		return Ok;
	}
	GpRegion *probe = gp_region_clone(region);
	if (!probe)
		return OutOfMemory;
	GpRectF box = {x, y, width, height};
	GpStatus status = GdipCombineRegionRect(probe, &box, CombineModeIntersect);
	if (status == Ok) {
		GpRectF *rects;
		INT count;
		status = cut_region(probe, NULL, TRUE, &rects, &count);
		if (status == Ok) {
			*out = count > 0;
			gp_free(rects);
		}
	}
	gp_region_delete(probe);
	return status;
}

GpStatus GDIPAPI GdipIsVisibleRegionRectI(GpRegion *region, INT x, INT y, INT width, INT height, const void *graphics, BOOL *out)
{
	return GdipIsVisibleRegionRect(region, (REAL)x, (REAL)y, (REAL)width, (REAL)height, graphics, out);
}

GpStatus GDIPAPI GdipGetRegionScansCount(GpRegion *region, UINT *out, GpMatrix *matrix)
{
	if (!region || !out || !matrix)
		return InvalidParameter;
	GpRectF *rects;
	INT count;
	GpStatus status = cut_region(region, matrix, TRUE, &rects, &count);
	if (status == Ok) {
		gp_free(rects);
		*out = count;
	}
	return status;
}

GpStatus GDIPAPI GdipGetRegionScans(GpRegion *region, GpRectF *out, INT *count, GpMatrix *matrix)
{
	if (!region || !count || !matrix)
		return InvalidParameter;
	GpRectF *rects;
	INT made;
	GpStatus status = cut_region(region, matrix, TRUE, &rects, &made);
	if (status == Ok) {
		*count = made;
		if (out && made)
			memcpy(out, rects, made * sizeof(GpRectF));
		gp_free(rects);
	}
	return status;
}

GpStatus GDIPAPI GdipGetRegionScansI(GpRegion *region, GpRect *out, INT *count, GpMatrix *matrix)
{
	if (!region || !count || !matrix)
		return InvalidParameter;
	GpRectF *rects;
	INT made;
	GpStatus status = cut_region(region, matrix, TRUE, &rects, &made);
	if (status == Ok) {
		*count = made;
		if (out) {
			for (INT i = 0; i < made; i++) {
				out[i].X = (INT)gp_floor(rects[i].X + 0.5f);
				out[i].Y = (INT)gp_floor(rects[i].Y + 0.5f);
				out[i].Width = (INT)gp_ceil(rects[i].Width);
				out[i].Height = (INT)gp_ceil(rects[i].Height);
			}
		}
		gp_free(rects);
	}
	return status;
}

// With a graphics the handle is in device units, so the region goes through its world first.
GpStatus GDIPAPI GdipGetRegionHRgn(GpRegion *region, const GpGraphics *graphics, HRGN *out)
{
	if (!region || !out)
		return InvalidParameter;
	if (region->root->type == NODE_INFINITE) {
		*out = NULL;
		return Ok;
	}
	GpRectF *rects;
	INT count;
	GpMatrix to_device;
	if (graphics)
		gp_world_to_device(graphics, &to_device);
	GpStatus status = gp_region_pixels(region, graphics ? &to_device : NULL, &rects, &count);
	if (status != Ok)
		return status;
	HRGN result = CreateRectRgn(0, 0, 0, 0);
	for (INT i = 0; result && i < count; i++) {
		HRGN part = CreateRectRgn((INT)gp_floor(rects[i].X + 0.5f), (INT)gp_floor(rects[i].Y + 0.5f),
			(INT)gp_floor(rects[i].X + rects[i].Width + 0.5f), (INT)gp_floor(rects[i].Y + rects[i].Height + 0.5f));
		if (part) {
			CombineRgn(result, result, part, RGN_OR);
			DeleteObject(part);
		}
	}
	gp_free(rects);
	*out = result;
	return result ? Ok : GenericError;
}

// Serialized form, as metafiles carry it: a header with size, checksum, magic and child count, then the tree in prefix
// order. Paths whose points are whole numbers in the 16 bit range are stored as shorts.
static BOOL path_as_shorts(const GpPath *path)
{
	for (INT i = 0; i < path->count; i++) {
		REAL x = path->points[i].X, y = path->points[i].Y;
		if (x != (REAL)(short)x || y != (REAL)(short)y)
			return FALSE;
	}
	return TRUE;
}

static UINT path_bytes(const GpPath *path)
{
	UINT points = path->count * (path_as_shorts(path) ? 4 : 8);
	return 12 + points + ((path->count + 3) & ~3);
}

static UINT node_bytes(const gp_node *node, INT *children)
{
	switch (node->type) {
	case NODE_RECT:
		return 4 + sizeof(GpRectF);
	case NODE_PATH:
		return 8 + path_bytes(node->path);
	case NODE_EMPTY:
	case NODE_INFINITE:
		return 4;
	default:
		*children += 2;
		return 4 + node_bytes(node->left, children) + node_bytes(node->right, children);
	}
}

static BYTE *put_dword(BYTE *at, DWORD value)
{
	memcpy(at, &value, 4);
	return at + 4;
}

static BYTE *write_path(BYTE *at, const GpPath *path)
{
	BOOL shorts = path_as_shorts(path);
	at = put_dword(at, PATH_MAGIC);
	at = put_dword(at, path->count);
	at = put_dword(at, shorts ? 0x4000 : 0);
	for (INT i = 0; i < path->count; i++) {
		if (shorts) {
			short xy[2] = {(short)path->points[i].X, (short)path->points[i].Y};
			memcpy(at, xy, 4);
			at += 4;
		} else {
			memcpy(at, &path->points[i], 8);
			at += 8;
		}
	}
	memcpy(at, path->types, path->count);
	at += path->count;
	at += (4 - (path->count & 3)) & 3;
	return at;
}

static BYTE *write_node(BYTE *at, const gp_node *node)
{
	at = put_dword(at, node->type);
	switch (node->type) {
	case NODE_RECT:
		memcpy(at, &node->rect, sizeof(GpRectF));
		return at + sizeof(GpRectF);
	case NODE_PATH:
		at = put_dword(at, path_bytes(node->path));
		return write_path(at, node->path);
	case NODE_EMPTY:
	case NODE_INFINITE:
		return at;
	default:
		at = write_node(at, node->left);
		return write_node(at, node->right);
	}
}

GpStatus GDIPAPI GdipGetRegionDataSize(GpRegion *region, UINT *out)
{
	if (!region || !out)
		return InvalidParameter;
	INT children = 0;
	*out = 16 + node_bytes(region->root, &children);
	return Ok;
}

GpStatus GDIPAPI GdipGetRegionData(GpRegion *region, BYTE *out, UINT size, UINT *needed)
{
	if (!region || !out || !size)
		return InvalidParameter;
	INT children = 0;
	UINT total = 16 + node_bytes(region->root, &children);
	if (needed)
		*needed = 0;
	if (size < total) {
		// A short buffer learns only the size field.
		if (needed && size >= 4)
			*needed = 4;
		return InsufficientBuffer;
	}
	memset(out, 0, total);
	BYTE *at = put_dword(out, total - 8);
	at = put_dword(at, 0);
	at = put_dword(at, REGION_MAGIC);
	at = put_dword(at, children);
	write_node(at, region->root);
	if (needed)
		*needed = total;
	return Ok;
}

static const BYTE *read_node(const BYTE *at, const BYTE *end, gp_node **out)
{
	DWORD type;
	if (end - at < 4)
		return NULL;
	memcpy(&type, at, 4);
	at += 4;
	gp_node *node = new_node(type);
	if (!node)
		return NULL;
	*out = node;
	if (type == NODE_RECT) {
		if (end - at < (INT)sizeof(GpRectF))
			return NULL;
		memcpy(&node->rect, at, sizeof(GpRectF));
		return at + sizeof(GpRectF);
	}
	if (type == NODE_PATH) {
		DWORD size, magic, count, flags;
		if (end - at < 16)
			return NULL;
		memcpy(&size, at, 4);
		memcpy(&magic, at + 4, 4);
		memcpy(&count, at + 8, 4);
		memcpy(&flags, at + 12, 4);
		const BYTE *body = at + 16;
		UINT stride = (flags & 0x4000) ? 4 : 8;
		if (!count)
			return NULL;
		if ((INT)(end - body) < (INT)(count * stride + count))
			return NULL;
		if (gp_path_new(FillModeAlternate, &node->path) != Ok || !gp_path_reserve(node->path, count ? count : 1))
			return NULL;
		for (DWORD i = 0; i < count; i++) {
			REAL x, y;
			if (flags & 0x4000) {
				short xy[2];
				memcpy(xy, body + i * 4, 4);
				x = xy[0];
				y = xy[1];
			} else {
				memcpy(&x, body + i * 8, 4);
				memcpy(&y, body + i * 8 + 4, 4);
			}
			gp_path_push(node->path, x, y, body[count * stride + i]);
		}
		if (!(node->flat = gp_path_clone(node->path)) || gp_path_flatten(node->flat, NULL, 0.25f) != Ok)
			return NULL;
		return at + 4 + size;
	}
	if (type == NODE_EMPTY || type == NODE_INFINITE)
		return at;
	if (type > CombineModeComplement)
		return NULL;
	at = read_node(at, end, &node->left);
	if (!at)
		return NULL;
	return read_node(at, end, &node->right);
}

GpStatus GDIPAPI GdipCreateRegionRgnData(const BYTE *data, INT size, GpRegion **out)
{
	if (!data || !out || size < 16)
		return InvalidParameter;
	gp_node *root = NULL;
	if (!read_node(data + 16, data + size, &root)) {
		free_node(root);
		return GenericError;
	}
	return wrap_root(out, root);
}
