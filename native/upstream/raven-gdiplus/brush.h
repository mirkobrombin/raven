/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_BRUSH_H
#define RAVEN_BRUSH_H

#include "gdip.h"

struct gp_image;

// Gradients keep two optional tables: the blend (position to factor between the two end colours) and the preset blend
// (position to colour), which wins when present.
struct gp_brush
{
	GpBrushType type;
	GpMatrix transform;
	GpWrapMode wrap;
	BOOL gamma;
	ARGB color;			// solid, hatch foreground
	ARGB back;			// hatch background
	INT hatch;
	GpPointF start, end;	// line gradient
	ARGB colors[2];		// line gradient ends
	GpRectF rect;			// line gradient rectangle, texture source bounds, path gradient bounds
	REAL *blend_factors, *blend_positions;
	INT blend_count;
	ARGB *preset_colors;
	REAL *preset_positions;
	INT preset_count;
	GpPath *path;			// path gradient boundary
	GpPointF center;
	ARGB center_color;
	ARGB *surround;
	INT surround_count;
	REAL focus_x, focus_y;
	struct gp_image *image;	// texture
	void *attributes;
};

// The colour of the brush at a device position, unpremultiplied; origin is the graphics rendering origin for hatches.
ARGB gp_brush_sample(const GpBrush *brush, const GpMatrix *inverse_world, REAL x, REAL y, INT origin_x, INT origin_y);

#endif
