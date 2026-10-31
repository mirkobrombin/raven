/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_REGION_H
#define RAVEN_REGION_H

#include "gdip.h"

enum
{
	NODE_RECT = 0x10000000,
	NODE_PATH = 0x10000001,
	NODE_EMPTY = 0x10000002,
	NODE_INFINITE = 0x10000003
};

// A region stays the tree of the operations that built it, the shape its serialized form records. Combining nodes use
// the CombineMode numbers as their type.
typedef struct gp_node
{
	DWORD type;
	GpRectF rect;
	GpPath *path;		// the caller's path
	GpPath *flat;		// flattened copy, for testing points
	struct gp_node *left, *right;
} gp_node;

typedef struct gp_region
{
	gp_node *root;
} GpRegion;

BOOL gp_region_contains(const GpRegion *region, REAL x, REAL y);
// Bounds of the region; FALSE when it is empty.
BOOL gp_region_bounds(const GpRegion *region, GpRectF *out);
BOOL gp_region_is_infinite(const GpRegion *region);
BOOL gp_region_is_empty(const GpRegion *region);
GpRegion *gp_region_clone(const GpRegion *region);
void gp_region_delete(GpRegion *region);
GpStatus gp_region_transform(GpRegion *region, const GpMatrix *matrix);
// The region as rectangles grouped in rows, after an optional transform.
GpStatus gp_region_scans(const GpRegion *region, const GpMatrix *matrix, GpRectF **out, INT *count);
// The same as the pixels the screen shows.
GpStatus gp_region_pixels(const GpRegion *region, const GpMatrix *matrix, GpRectF **out, INT *count);

GpStatus GDIPAPI GdipCreateRegion(GpRegion **out);
GpStatus GDIPAPI GdipDeleteRegion(GpRegion *region);
GpStatus GDIPAPI GdipCombineRegionRect(GpRegion *region, const GpRectF *rect, CombineMode mode);
GpStatus GDIPAPI GdipCombineRegionPath(GpRegion *region, GpPath *path, CombineMode mode);
GpStatus GDIPAPI GdipCombineRegionRegion(GpRegion *region, GpRegion *other, CombineMode mode);
GpStatus GDIPAPI GdipSetInfinite(GpRegion *region);

#endif
