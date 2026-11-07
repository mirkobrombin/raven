/* SPDX-License-Identifier: MIT */
#include "image.h"
#include "gmath.h"

// Image attributes: one set of adjustments per kind of drawing. The default slot applies when a kind has none of its own.
enum { AdjustDefault, AdjustBitmap, AdjustBrush, AdjustPen, AdjustText, AdjustCount };

typedef struct
{
	ARGB oldColor, newColor;
} ColorMap;

GpStatus GDIPAPI GdipCreateImageAttributes(GpImageAttributes **out)
{
	if (!out)
		return InvalidParameter;
	GpImageAttributes *attributes = gp_alloc(sizeof(GpImageAttributes));
	if (!attributes)
		return OutOfMemory;
	attributes->wrap = WrapModeClamp;
	*out = attributes;
	return Ok;
}

GpStatus GDIPAPI GdipCloneImageAttributes(const GpImageAttributes *attributes, GpImageAttributes **out)
{
	if (!attributes || !out)
		return InvalidParameter;
	GpImageAttributes *copy = gp_dup(attributes, sizeof(GpImageAttributes));
	if (!copy)
		return OutOfMemory;
	for (int i = 0; i < AdjustCount; i++) {
		if (copy->adjust[i].remap)
			copy->adjust[i].remap = gp_dup(attributes->adjust[i].remap, attributes->adjust[i].remap_count * sizeof(ColorMap));
	}
	*out = copy;
	return Ok;
}

GpStatus GDIPAPI GdipDisposeImageAttributes(GpImageAttributes *attributes)
{
	if (!attributes)
		return InvalidParameter;
	for (int i = 0; i < AdjustCount; i++)
		gp_free(attributes->adjust[i].remap);
	gp_free(attributes);
	return Ok;
}

#define CHECK_KIND(attributes, kind) \
	if (!(attributes) || (UINT)(kind) >= AdjustCount) \
		return InvalidParameter

GpStatus GDIPAPI GdipResetImageAttributes(GpImageAttributes *attributes, INT kind)
{
	CHECK_KIND(attributes, kind);
	gp_free(attributes->adjust[kind].remap);
	memset(&attributes->adjust[kind], 0, sizeof(gp_adjust));
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesToIdentity(GpImageAttributes *attributes, INT kind)
{
	return GdipResetImageAttributes(attributes, kind);
}

GpStatus GDIPAPI GdipSetImageAttributesColorMatrix(GpImageAttributes *attributes, INT kind, BOOL enable, const REAL *matrix, const REAL *gray, INT flags)
{
	CHECK_KIND(attributes, kind);
	gp_adjust *a = &attributes->adjust[kind];
	if (enable) {
		if (!matrix || (UINT)flags > 2)
			return InvalidParameter;
		if (flags == 2 && !gray)
			return InvalidParameter;
		memcpy(a->matrix, matrix, sizeof(a->matrix));
		if (gray) {
			memcpy(a->gray, gray, sizeof(a->gray));
			a->has_gray = TRUE;
		}
		a->matrix_flags = flags;
	}
	a->has_matrix = enable;
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesThreshold(GpImageAttributes *attributes, INT kind, BOOL enable, REAL threshold)
{
	CHECK_KIND(attributes, kind);
	attributes->adjust[kind].has_threshold = enable;
	attributes->adjust[kind].threshold = threshold;
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesGamma(GpImageAttributes *attributes, INT kind, BOOL enable, REAL gamma)
{
	CHECK_KIND(attributes, kind);
	if (enable && gamma <= 0.0f)
		return InvalidParameter;
	attributes->adjust[kind].has_gamma = enable;
	attributes->adjust[kind].gamma = gamma;
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesNoOp(GpImageAttributes *attributes, INT kind, BOOL enable)
{
	CHECK_KIND(attributes, kind);
	attributes->adjust[kind].no_op = enable;
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesColorKeys(GpImageAttributes *attributes, INT kind, BOOL enable, ARGB low, ARGB high)
{
	CHECK_KIND(attributes, kind);
	attributes->adjust[kind].has_keys = enable;
	attributes->adjust[kind].key_low = low;
	attributes->adjust[kind].key_high = high;
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesOutputChannel(GpImageAttributes *attributes, INT kind, BOOL enable, INT channel)
{
	CHECK_KIND(attributes, kind);
	attributes->adjust[kind].has_channel = enable;
	attributes->adjust[kind].channel = channel;
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesOutputChannelColorProfile(GpImageAttributes *attributes, INT kind, BOOL enable, const WCHAR *profile)
{
	(void)enable;
	(void)profile;
	CHECK_KIND(attributes, kind);
	return NotImplemented;
}

GpStatus GDIPAPI GdipSetImageAttributesRemapTable(GpImageAttributes *attributes, INT kind, BOOL enable, UINT count, const ColorMap *map)
{
	CHECK_KIND(attributes, kind);
	gp_adjust *a = &attributes->adjust[kind];
	if (enable) {
		if (!map || !count)
			return InvalidParameter;
		void *copy = gp_dup(map, count * sizeof(ColorMap));
		if (!copy)
			return OutOfMemory;
		gp_free(a->remap);
		a->remap = copy;
		a->remap_count = count;
	}
	a->has_remap = enable;
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesWrapMode(GpImageAttributes *attributes, GpWrapMode wrap, ARGB color, BOOL clamp)
{
	if (!attributes)
		return InvalidParameter;
	attributes->wrap = wrap;
	attributes->wrap_color = color;
	attributes->wrap_clamp = clamp;
	return Ok;
}

GpStatus GDIPAPI GdipSetImageAttributesICMMode(GpImageAttributes *attributes, BOOL on)
{
	(void)on;
	return attributes ? Ok : InvalidParameter;
}

GpStatus GDIPAPI GdipSetImageAttributesCachedBackground(GpImageAttributes *attributes, BOOL enable)
{
	(void)enable;
	return attributes ? NotImplemented : InvalidParameter;
}

static ARGB apply_kind(const GpImageAttributes *attributes, ARGB color, INT kind);

GpStatus GDIPAPI GdipGetImageAttributesAdjustedPalette(GpImageAttributes *attributes, ColorPalette *palette, INT kind)
{
	if (!attributes || !palette || !palette->Count || (UINT)kind >= AdjustCount || kind == AdjustDefault)
		return InvalidParameter;
	for (UINT i = 0; i < palette->Count; i++)
		palette->Entries[i] = apply_kind(attributes, palette->Entries[i], kind);
	return Ok;
}

static BYTE clamp_channel(REAL value)
{
	return value <= 0.0f ? 0 : value >= 255.0f ? 255 : (BYTE)(value + 0.5f);
}

// Applied in the documented order: remap, keys, matrix, threshold, gamma, channel output.
static ARGB apply_kind(const GpImageAttributes *attributes, ARGB color, INT kind)
{
	const gp_adjust *a = &attributes->adjust[kind];
	const gp_adjust *d = &attributes->adjust[AdjustDefault];
	if (!a->has_matrix && !a->has_keys && !a->has_gamma && !a->has_threshold && !a->has_remap && !a->has_channel && !a->no_op)
		a = d;
	if (a->no_op)
		return color;
	if (a->has_remap) {
		const ColorMap *map = a->remap;
		for (UINT i = 0; i < a->remap_count; i++) {
			if (map[i].oldColor == color) {
				color = map[i].newColor;
				break;
			}
		}
	}
	UINT ca = color >> 24, cr = (color >> 16) & 255, cg = (color >> 8) & 255, cb = color & 255;
	if (a->has_keys) {
		BOOL inside = TRUE;
		for (int shift = 0; shift < 24; shift += 8) {
			UINT value = (color >> shift) & 255;
			if (value < ((a->key_low >> shift) & 255) || value > ((a->key_high >> shift) & 255))
				inside = FALSE;
		}
		if (inside)
			return 0;
	}
	if (a->has_matrix) {
		REAL in[5] = {cr / 255.0f, cg / 255.0f, cb / 255.0f, ca / 255.0f, 1.0f};
		BOOL gray = cr == cg && cg == cb;
		const REAL (*m)[5] = a->matrix;
		if (gray && a->matrix_flags == 1)
			m = NULL;
		else if (gray && a->matrix_flags == 2 && a->has_gray)
			m = a->gray;
		if (m) {
			REAL out[4];
			for (int c = 0; c < 4; c++) {
				out[c] = 0.0f;
				for (int r = 0; r < 5; r++)
					out[c] += in[r] * m[r][c];
			}
			cr = clamp_channel(out[0] * 255.0f);
			cg = clamp_channel(out[1] * 255.0f);
			cb = clamp_channel(out[2] * 255.0f);
			ca = clamp_channel(out[3] * 255.0f);
		}
	}
	if (a->has_threshold) {
		REAL limit = a->threshold * 255.0f;
		cr = cr >= limit ? 255 : 0;
		cg = cg >= limit ? 255 : 0;
		cb = cb >= limit ? 255 : 0;
	}
	if (a->has_gamma) {
		cr = clamp_channel((REAL)gp_m_pow(cr / 255.0, a->gamma) * 255.0f);
		cg = clamp_channel((REAL)gp_m_pow(cg / 255.0, a->gamma) * 255.0f);
		cb = clamp_channel((REAL)gp_m_pow(cb / 255.0, a->gamma) * 255.0f);
	}
	if (a->has_channel) {
		// Cyan, magenta, yellow and key each come out as a gray level of the inverse channel.
		UINT inverse[3] = {255 - cr, 255 - cg, 255 - cb}, key = inverse[0];
		if (inverse[1] < key) key = inverse[1];
		if (inverse[2] < key) key = inverse[2];
		UINT value = a->channel == 3 ? key : (key >= 255 ? 0 : (inverse[a->channel] - key) * 255 / (255 - key));
		cr = cg = cb = 255 - value;
	}
	return ca << 24 | cr << 16 | cg << 8 | cb;
}

ARGB gp_attributes_apply(const GpImageAttributes *attributes, ARGB color)
{
	return apply_kind(attributes, color, AdjustBitmap);
}
