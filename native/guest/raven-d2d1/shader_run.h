/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Running the pixel shader of an effect's draw transform, as Direct2D does on the graphics card. Pixels go in and come out as four floats
 * each, red, green, blue and alpha, premultiplied: the precision Direct2D's effects keep between their steps. */
#pragma once

#include <windows.h>

/* One input of the shader: its pixels, row after row, and where its first pixel lies in the transform's space. */
struct shader_image {
	const float *pixels;
	/* The pixels as bytes instead, when there are no floats: blue, green, red and alpha, premultiplied. */
	const BYTE *bytes;
	UINT32 width, height;
	INT32 left, top;
	/* The D2D1_FILTER the transform asked for this input, which has the value of the Direct3D filter of the same name. */
	UINT32 filter;
};

/* Runs `code` over the width by height pixels whose first lies at (left, top) in the transform's space, with `constants` in its first
 * constant buffer and the inputs in its textures and samplers by their order, and writes what it gives into `out`, four floats a pixel, or
 * into `out_bytes` when that is given instead, four bytes a pixel as the inputs' bytes are. Fails when no Direct3D 11 device can be had,
 * or the shader is not one Direct3D takes. */
HRESULT shader_run(const BYTE *code, UINT32 size, const BYTE *constants, UINT32 constant_size, const struct shader_image *inputs,
	UINT32 count, INT32 left, INT32 top, UINT32 width, UINT32 height, float *out, BYTE *out_bytes);

/* Draws into a texture of the program's own device, with that device, where shader_run draws aside and hands the pixels back: the
 * width by height pixels whose first lies at (left, top) in the transform's space go to (x, y) of `target`, over what it holds, as a
 * premultiplied source goes over a destination. Without `code` the first input is drawn as it is. What the device's context was set to
 * is put back. Fails when the texture cannot be drawn into or the shader is not one Direct3D takes. */
HRESULT shader_draw(void *target, const BYTE *code, UINT32 size, const BYTE *constants, UINT32 constant_size,
	const struct shader_image *inputs, UINT32 count, INT32 left, INT32 top, UINT32 width, UINT32 height, INT32 x, INT32 y);

/* The most inputs a shader is given, as Direct2D's own vertex stage passes a texture coordinate for each. */
#define SHADER_INPUTS 8
