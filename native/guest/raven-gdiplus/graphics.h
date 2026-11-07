/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_GRAPHICS_H
#define RAVEN_GRAPHICS_H

#include "image.h"
#include "region.h"

typedef struct gp_state
{
	struct gp_state *next;
	UINT id;
	BOOL container;
	GpMatrix world;
	GpRegion *clip;
	GpUnit unit;
	REAL scale;
	INT compositing_mode, compositing_quality, interpolation, smoothing, pixel_offset, text_hint;
	UINT text_contrast;
	INT origin_x, origin_y;
} gp_state;

typedef struct gp_graphics
{
	HDC dc;
	HWND window;
	BOOL own_dc;
	GpImage *image;		// drawing on a bitmap
	HDC held_dc;		// GetDC handed out
	HBITMAP held_bitmap, held_old;
	void *held_bits;
	GpUnit unit;
	REAL scale, dpi_x, dpi_y;
	GpMatrix world;
	GpRegion *clip;		// in device space
	INT compositing_mode, compositing_quality, interpolation, smoothing, pixel_offset, text_hint;
	UINT text_contrast;
	INT origin_x, origin_y;
	gp_state *states;
	UINT next_state;
	INT width, height;	// device bounds of the surface
} GpGraphics;

// A surface the rasterizer writes to: 32 bit pixels, or the graphics' bitmap through its own format.
typedef struct
{
	GpGraphics *graphics;
	GpImage *image;
	DWORD *pixels;
	INT stride, width, height;
	BOOL premultiplied, opaque;
	HDC temp_dc;
	HBITMAP temp_bitmap, temp_old;
	INT dc_x, dc_y, logical_x, logical_y;
	RECT area;
} gp_surface;

void gp_world_to_device(const GpGraphics *graphics, GpMatrix *out);
GpStatus gp_surface_begin(GpGraphics *graphics, gp_surface *surface, const RECT *area);
void gp_surface_end(gp_surface *surface);
void gp_surface_blend(gp_surface *surface, INT x, INT y, ARGB color, BYTE coverage);
// A byte for each device pixel of the area: how far it lies inside the clip.
BYTE *gp_clip_mask(const GpGraphics *graphics, const RECT *area);
BOOL gp_clip_bounds(const GpGraphics *graphics, RECT *out);
REAL gp_unit_scale(const GpGraphics *graphics, GpUnit unit, REAL dpi);

#endif
