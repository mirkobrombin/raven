/* SPDX-License-Identifier: MIT */
#include "wic.h"
#include <math.h>

/* Every format reads into and writes from BGRA with 8 bits a channel, straight alpha. Formats of more depth lose it on the way;
 * a format converted to itself is passed through untouched. */

typedef void (*row_reader)(const BYTE *from, UINT width, const WICColor *colors, BYTE *bgra);
typedef void (*row_writer)(const BYTE *bgra, UINT width, BYTE *to);

static void read_indexed(const BYTE *from, UINT width, const WICColor *colors, BYTE *bgra, UINT bits)
{
	UINT x;
	for (x = 0; x < width; x++) {
		const UINT bit = x * bits;
		const UINT value = (from[bit / 8] >> (8 - bits - bit % 8)) & ((1U << bits) - 1);
		memcpy(bgra + x * 4, &colors[value], 4);
	}
}

static void read_1(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_indexed(f, w, c, t, 1); }
static void read_2(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_indexed(f, w, c, t, 2); }
static void read_4(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_indexed(f, w, c, t, 4); }
static void read_8(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_indexed(f, w, c, t, 8); }

static void read_gray(const BYTE *from, UINT width, BYTE *bgra, UINT bits)
{
	const UINT top = (1U << bits) - 1;
	UINT x;
	for (x = 0; x < width; x++) {
		const UINT bit = x * bits;
		const BYTE gray = (BYTE)(((from[bit / 8] >> (8 - bits - bit % 8)) & top) * 255 / top);
		bgra[x * 4] = bgra[x * 4 + 1] = bgra[x * 4 + 2] = gray;
		bgra[x * 4 + 3] = 255;
	}
}

static void read_bw(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_gray(f, w, t, 1); }
static void read_g2(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_gray(f, w, t, 2); }
static void read_g4(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_gray(f, w, t, 4); }
static void read_g8(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_gray(f, w, t, 8); }

static void read_g16(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = t[x * 4 + 1] = t[x * 4 + 2] = f[x * 2 + 1];
		t[x * 4 + 3] = 255;
	}
}

static void read_a8(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = t[x * 4 + 1] = t[x * 4 + 2] = 0;
		t[x * 4 + 3] = f[x];
	}
}

static void read_555(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT v = f[x * 2] | f[x * 2 + 1] << 8;
		t[x * 4] = (BYTE)((v & 31) * 255 / 31);
		t[x * 4 + 1] = (BYTE)(((v >> 5) & 31) * 255 / 31);
		t[x * 4 + 2] = (BYTE)(((v >> 10) & 31) * 255 / 31);
		t[x * 4 + 3] = 255;
	}
}

static void read_5551(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	read_555(f, w, c, t);
	for (x = 0; x < w; x++)
		t[x * 4 + 3] = f[x * 2 + 1] & 0x80 ? 255 : 0;
}

static void read_565(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT v = f[x * 2] | f[x * 2 + 1] << 8;
		t[x * 4] = (BYTE)((v & 31) * 255 / 31);
		t[x * 4 + 1] = (BYTE)(((v >> 5) & 63) * 255 / 63);
		t[x * 4 + 2] = (BYTE)(((v >> 11) & 31) * 255 / 31);
		t[x * 4 + 3] = 255;
	}
}

static void read_bgr24(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		memcpy(t + x * 4, f + x * 3, 3);
		t[x * 4 + 3] = 255;
	}
}

static void read_rgb24(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = f[x * 3 + 2];
		t[x * 4 + 1] = f[x * 3 + 1];
		t[x * 4 + 2] = f[x * 3];
		t[x * 4 + 3] = 255;
	}
}

static void read_bgrx(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	memcpy(t, f, (SIZE_T)w * 4);
	for (x = 0; x < w; x++)
		t[x * 4 + 3] = 255;
}

static void read_bgra(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	memcpy(t, f, (SIZE_T)w * 4);
}

static void read_rgba(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = f[x * 4 + 2];
		t[x * 4 + 1] = f[x * 4 + 1];
		t[x * 4 + 2] = f[x * 4];
		t[x * 4 + 3] = f[x * 4 + 3];
	}
}

static void read_rgbx(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	read_rgba(f, w, c, t);
	for (x = 0; x < w; x++)
		t[x * 4 + 3] = 255;
}

static BYTE unmultiply(BYTE value, BYTE alpha)
{
	return alpha ? (BYTE)min(255, (value * 255 + alpha / 2) / alpha) : 0;
}

static void read_pbgra(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const BYTE a = f[x * 4 + 3];
		t[x * 4] = unmultiply(f[x * 4], a);
		t[x * 4 + 1] = unmultiply(f[x * 4 + 1], a);
		t[x * 4 + 2] = unmultiply(f[x * 4 + 2], a);
		t[x * 4 + 3] = a;
	}
}

static void read_prgba(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const BYTE a = f[x * 4 + 3];
		t[x * 4] = unmultiply(f[x * 4 + 2], a);
		t[x * 4 + 1] = unmultiply(f[x * 4 + 1], a);
		t[x * 4 + 2] = unmultiply(f[x * 4], a);
		t[x * 4 + 3] = a;
	}
}

static void read_rgb48(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = f[x * 6 + 5];
		t[x * 4 + 1] = f[x * 6 + 3];
		t[x * 4 + 2] = f[x * 6 + 1];
		t[x * 4 + 3] = 255;
	}
}

static void read_rgba64(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = f[x * 8 + 5];
		t[x * 4 + 1] = f[x * 8 + 3];
		t[x * 4 + 2] = f[x * 8 + 1];
		t[x * 4 + 3] = f[x * 8 + 7];
	}
}

static void read_bgra64(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = f[x * 8 + 1];
		t[x * 4 + 1] = f[x * 8 + 3];
		t[x * 4 + 2] = f[x * 8 + 5];
		t[x * 4 + 3] = f[x * 8 + 7];
	}
}

static void read_cmyk(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT k = 255 - f[x * 4 + 3];
		t[x * 4] = (BYTE)((255 - f[x * 4 + 2]) * k / 255);
		t[x * 4 + 1] = (BYTE)((255 - f[x * 4 + 1]) * k / 255);
		t[x * 4 + 2] = (BYTE)((255 - f[x * 4]) * k / 255);
		t[x * 4 + 3] = 255;
	}
}

static void write_bgr24(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++)
		memcpy(t + x * 3, f + x * 4, 3);
}

static void write_rgb24(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 3] = f[x * 4 + 2];
		t[x * 3 + 1] = f[x * 4 + 1];
		t[x * 3 + 2] = f[x * 4];
	}
}

static void write_bgra(const BYTE *f, UINT w, BYTE *t)
{
	memcpy(t, f, (SIZE_T)w * 4);
}

static void write_rgba(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = f[x * 4 + 2];
		t[x * 4 + 1] = f[x * 4 + 1];
		t[x * 4 + 2] = f[x * 4];
		t[x * 4 + 3] = f[x * 4 + 3];
	}
}

static void write_pbgra(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT a = f[x * 4 + 3];
		t[x * 4] = (BYTE)((f[x * 4] * a + 127) / 255);
		t[x * 4 + 1] = (BYTE)((f[x * 4 + 1] * a + 127) / 255);
		t[x * 4 + 2] = (BYTE)((f[x * 4 + 2] * a + 127) / 255);
		t[x * 4 + 3] = (BYTE)a;
	}
}

static void write_prgba(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT a = f[x * 4 + 3];
		t[x * 4] = (BYTE)((f[x * 4 + 2] * a + 127) / 255);
		t[x * 4 + 1] = (BYTE)((f[x * 4 + 1] * a + 127) / 255);
		t[x * 4 + 2] = (BYTE)((f[x * 4] * a + 127) / 255);
		t[x * 4 + 3] = (BYTE)a;
	}
}

static float to_linear(BYTE value);
static BYTE to_srgb(float value);

/* Luminance is weighed in linear light. */
static BYTE gray_of(const BYTE *p)
{
	if (p[0] == p[1] && p[1] == p[2])
		return p[0];
	return to_srgb(0.2126f * to_linear(p[2]) + 0.7152f * to_linear(p[1]) + 0.0722f * to_linear(p[0]));
}

static void write_g8(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++)
		t[x] = gray_of(f + x * 4);
}

static void write_g16(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++)
		t[x * 2] = t[x * 2 + 1] = gray_of(f + x * 4);
}

static void write_a8(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++)
		t[x] = f[x * 4 + 3];
}

static void write_bw(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	memset(t, 0, (w + 7) / 8);
	for (x = 0; x < w; x++)
		if (gray_of(f + x * 4) >= 128)
			t[x / 8] |= 0x80 >> (x % 8);
}

static void write_555(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT v = (f[x * 4] >> 3) | (f[x * 4 + 1] >> 3) << 5 | (f[x * 4 + 2] >> 3) << 10;
		t[x * 2] = (BYTE)v;
		t[x * 2 + 1] = (BYTE)(v >> 8);
	}
}

static void write_565(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT v = (f[x * 4] >> 3) | (f[x * 4 + 1] >> 2) << 5 | (f[x * 4 + 2] >> 3) << 11;
		t[x * 2] = (BYTE)v;
		t[x * 2 + 1] = (BYTE)(v >> 8);
	}
}

static void write_5551(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	write_555(f, w, t);
	for (x = 0; x < w; x++)
		if (f[x * 4 + 3] >= 128)
			t[x * 2 + 1] |= 0x80;
}

static void write_rgb48(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 6] = t[x * 6 + 1] = f[x * 4 + 2];
		t[x * 6 + 2] = t[x * 6 + 3] = f[x * 4 + 1];
		t[x * 6 + 4] = t[x * 6 + 5] = f[x * 4];
	}
}

static void write_rgba64(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 8] = t[x * 8 + 1] = f[x * 4 + 2];
		t[x * 8 + 2] = t[x * 8 + 3] = f[x * 4 + 1];
		t[x * 8 + 4] = t[x * 8 + 5] = f[x * 4];
		t[x * 8 + 6] = t[x * 8 + 7] = f[x * 4 + 3];
	}
}

/* The floating formats hold linear light, the others are sRGB. */
static double natural_log(double x)
{
	double sum = 0, term, square;
	int power = 0, index;
	while (x > 1.5) {
		x /= 2;
		power++;
	}
	while (x < 0.75) {
		x *= 2;
		power--;
	}
	term = (x - 1) / (x + 1);
	square = term * term;
	for (index = 1; index < 24; index += 2) {
		sum += term / index;
		term *= square;
	}
	return 2 * sum + power * 0.693147180559945;
}

static double natural_exp(double x)
{
	double sum = 1, term = 1;
	int halves = 0, index;
	while (x > 0.5 || x < -0.5) {
		x /= 2;
		halves++;
	}
	for (index = 1; index < 14; index++) {
		term *= x / index;
		sum += term;
	}
	while (halves--)
		sum *= sum;
	return sum;
}

static float to_linear(BYTE value)
{
	const double s = value / 255.0;
	return (float)(s <= 0.04045 ? s / 12.92 : natural_exp(2.4 * natural_log((s + 0.055) / 1.055)));
}

static BYTE to_srgb(float value)
{
	double s;
	if (value <= 0)
		return 0;
	if (value >= 1)
		return 255;
	s = value <= 0.0031308 ? 12.92 * value : 1.055 * natural_exp(natural_log(value) / 2.4) - 0.055;
	return (BYTE)(s * 255 + 0.505);
}

static void read_gray_float(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = t[x * 4 + 1] = t[x * 4 + 2] = to_srgb(((const float *)f)[x]);
		t[x * 4 + 3] = 255;
	}
}

static void write_gray_float(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++)
		((float *)t)[x] = 0.2126f * to_linear(f[x * 4 + 2]) + 0.7152f * to_linear(f[x * 4 + 1]) + 0.0722f * to_linear(f[x * 4]);
}

static void read_floats(const BYTE *f, UINT w, BYTE *t, UINT channels, BOOL premultiplied)
{
	const float *from = (const float *)f;
	UINT x;
	for (x = 0; x < w; x++, from += channels) {
		const float alpha = channels == 4 && from[3] < 1 ? (from[3] > 0 ? from[3] : 0) : 1;
		const float scale = premultiplied && alpha > 0 ? 1 / alpha : 1;
		t[x * 4] = to_srgb(from[2] * scale);
		t[x * 4 + 1] = to_srgb(from[1] * scale);
		t[x * 4 + 2] = to_srgb(from[0] * scale);
		t[x * 4 + 3] = (BYTE)(alpha * 255 + 0.5f);
	}
}

static void read_rgb96f(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_floats(f, w, t, 3, FALSE); }
static void read_rgb128f(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	read_floats(f, w, t, 4, FALSE);
	for (x = 0; x < w; x++)
		t[x * 4 + 3] = 255;
}
static void read_rgba128f(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_floats(f, w, t, 4, FALSE); }
static void read_prgba128f(const BYTE *f, UINT w, const WICColor *c, BYTE *t) { read_floats(f, w, t, 4, TRUE); }

static void write_floats(const BYTE *f, UINT w, BYTE *t, BOOL premultiplied, BOOL opaque)
{
	float *to = (float *)t;
	UINT x;
	for (x = 0; x < w; x++, to += 4) {
		const float alpha = opaque ? 1 : f[x * 4 + 3] / 255.0f, scale = premultiplied ? alpha : 1;
		to[0] = to_linear(f[x * 4 + 2]) * scale;
		to[1] = to_linear(f[x * 4 + 1]) * scale;
		to[2] = to_linear(f[x * 4]) * scale;
		to[3] = alpha;
	}
}

static void write_rgb128f(const BYTE *f, UINT w, BYTE *t) { write_floats(f, w, t, FALSE, TRUE); }
static void write_rgba128f(const BYTE *f, UINT w, BYTE *t) { write_floats(f, w, t, FALSE, FALSE); }
static void write_prgba128f(const BYTE *f, UINT w, BYTE *t) { write_floats(f, w, t, TRUE, FALSE); }

static float half_value(USHORT half)
{
	const int exponent = (half >> 10) & 31, fraction = half & 1023;
	float value = exponent ? (1024 + fraction) / 1024.0f : fraction / 1024.0f;
	int steps = (exponent ? exponent : 1) - 15;
	for (; steps > 0; steps--)
		value *= 2;
	for (; steps < 0; steps++)
		value /= 2;
	return half & 0x8000 ? -value : value;
}

static void read_rgb48half(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	const USHORT *from = (const USHORT *)f;
	UINT x;
	for (x = 0; x < w; x++, from += 3) {
		t[x * 4] = to_srgb(half_value(from[2]));
		t[x * 4 + 1] = to_srgb(half_value(from[1]));
		t[x * 4 + 2] = to_srgb(half_value(from[0]));
		t[x * 4 + 3] = 255;
	}
}

static void read_prgba64(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	const USHORT *from = (const USHORT *)f;
	UINT x;
	for (x = 0; x < w; x++, from += 4) {
		const UINT alpha = from[3], scale = alpha ? alpha : 1;
		const UINT red = from[0] * 255U / scale, green = from[1] * 255U / scale, blue = from[2] * 255U / scale;
		t[x * 4] = (BYTE)min(blue, 255);
		t[x * 4 + 1] = (BYTE)min(green, 255);
		t[x * 4 + 2] = (BYTE)min(red, 255);
		t[x * 4 + 3] = (BYTE)(alpha >> 8);
	}
}

static void write_prgba64(const BYTE *f, UINT w, BYTE *t)
{
	USHORT *to = (USHORT *)t;
	UINT x;
	for (x = 0; x < w; x++, to += 4) {
		const UINT alpha = f[x * 4 + 3];
		const UINT wide = alpha * 257;
		to[0] = (USHORT)(f[x * 4 + 2] * 257U * wide >> 16);
		to[1] = (USHORT)(f[x * 4 + 1] * 257U * wide >> 16);
		to[2] = (USHORT)(f[x * 4] * 257U * wide >> 16);
		to[3] = (USHORT)wide;
	}
}

static void read_gray_half(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	const USHORT *from = (const USHORT *)f;
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = t[x * 4 + 1] = t[x * 4 + 2] = to_srgb(half_value(from[x]));
		t[x * 4 + 3] = 255;
	}
}

static void read_bgr48(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = f[x * 6 + 1];
		t[x * 4 + 1] = f[x * 6 + 3];
		t[x * 4 + 2] = f[x * 6 + 5];
		t[x * 4 + 3] = 255;
	}
}

static void write_bgr48(const BYTE *f, UINT w, BYTE *t)
{
	USHORT *to = (USHORT *)t;
	UINT x;
	for (x = 0; x < w; x++) {
		to[x * 3] = (USHORT)(f[x * 4] * 257);
		to[x * 3 + 1] = (USHORT)(f[x * 4 + 1] * 257);
		to[x * 3 + 2] = (USHORT)(f[x * 4 + 2] * 257);
	}
}

static void read_rgbx64(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		t[x * 4] = f[x * 8 + 5];
		t[x * 4 + 1] = f[x * 8 + 3];
		t[x * 4 + 2] = f[x * 8 + 1];
		t[x * 4 + 3] = 255;
	}
}

static void write_rgbx64(const BYTE *f, UINT w, BYTE *t)
{
	USHORT *to = (USHORT *)t;
	UINT x;
	for (x = 0; x < w; x++) {
		to[x * 4] = (USHORT)(f[x * 4 + 2] * 257);
		to[x * 4 + 1] = (USHORT)(f[x * 4 + 1] * 257);
		to[x * 4 + 2] = (USHORT)(f[x * 4] * 257);
		to[x * 4 + 3] = 0xffff;
	}
}

static void write_bgra64(const BYTE *f, UINT w, BYTE *t)
{
	USHORT *to = (USHORT *)t;
	UINT x;
	for (x = 0; x < w * 4; x++)
		to[x] = (USHORT)(f[x] * 257);
}

static void read_pbgra64(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	const USHORT *from = (const USHORT *)f;
	UINT x;
	for (x = 0; x < w; x++, from += 4) {
		const UINT alpha = from[3], scale = alpha ? alpha : 1;
		t[x * 4] = (BYTE)min(from[0] * 255U / scale, 255);
		t[x * 4 + 1] = (BYTE)min(from[1] * 255U / scale, 255);
		t[x * 4 + 2] = (BYTE)min(from[2] * 255U / scale, 255);
		t[x * 4 + 3] = (BYTE)(alpha >> 8);
	}
}

static void write_pbgra64(const BYTE *f, UINT w, BYTE *t)
{
	USHORT *to = (USHORT *)t;
	UINT x;
	for (x = 0; x < w; x++, to += 4) {
		const UINT wide = f[x * 4 + 3] * 257U;
		to[0] = (USHORT)(f[x * 4] * 257U * wide >> 16);
		to[1] = (USHORT)(f[x * 4 + 1] * 257U * wide >> 16);
		to[2] = (USHORT)(f[x * 4 + 2] * 257U * wide >> 16);
		to[3] = (USHORT)wide;
	}
}

static void write_g2(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	memset(t, 0, (w * 2 + 7) / 8);
	for (x = 0; x < w; x++)
		t[x / 4] |= (BYTE)(gray_of(f + x * 4) >> 6 << (6 - x % 4 * 2));
}

static void write_g4(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	memset(t, 0, (w + 1) / 2);
	for (x = 0; x < w; x++)
		t[x / 2] |= (BYTE)(gray_of(f + x * 4) >> 4 << (x % 2 ? 0 : 4));
}

static void read_bgr101010(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT value = ((const UINT *)f)[x];
		t[x * 4] = (BYTE)(value >> 2);
		t[x * 4 + 1] = (BYTE)(value >> 12);
		t[x * 4 + 2] = (BYTE)(value >> 22);
		t[x * 4 + 3] = 255;
	}
}

static void write_bgr101010(const BYTE *f, UINT w, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++)
		((UINT *)t)[x] = (f[x * 4] * 1023U / 255) | (f[x * 4 + 1] * 1023U / 255) << 10 | (f[x * 4 + 2] * 1023U / 255) << 20;
}

static void read_r10g10b10a2(const BYTE *f, UINT w, const WICColor *c, BYTE *t)
{
	UINT x;
	for (x = 0; x < w; x++) {
		const UINT value = ((const UINT *)f)[x];
		t[x * 4] = (BYTE)(value >> 22);
		t[x * 4 + 1] = (BYTE)(value >> 12);
		t[x * 4 + 2] = (BYTE)(value >> 2);
		t[x * 4 + 3] = (BYTE)((value >> 30) * 85);
	}
}

/* What loses nothing goes to the floating formats straight from its own depth, not through 8 bits. */
static float linear_16(USHORT value)
{
	const double s = value / 65535.0;
	return (float)(s <= 0.04045 ? s / 12.92 : natural_exp(2.4 * natural_log((s + 0.055) / 1.055)));
}

static BOOL read_float_row(const GUID *format, const BYTE *f, UINT w, float *t)
{
	const USHORT *words = (const USHORT *)f;
	const float *floats = (const float *)f;
	UINT x;
	for (x = 0; x < w; x++, t += 4) {
		if (IsEqualGUID(format, &GUID_WICPixelFormat48bppRGB)) {
			t[0] = linear_16(words[x * 3]);
			t[1] = linear_16(words[x * 3 + 1]);
			t[2] = linear_16(words[x * 3 + 2]);
			t[3] = 1;
		}
		else if (IsEqualGUID(format, &GUID_WICPixelFormat64bppPRGBA)) {
			const USHORT alpha = words[x * 4 + 3];
			const float scale = alpha ? 65535.0f / alpha : 1;
			UINT index;
			for (index = 0; index < 3; index++) {
				const float value = words[x * 4 + index] * scale;
				t[index] = linear_16(value >= 65535 ? 65535 : (USHORT)(value + 0.5f));
			}
			t[3] = alpha / 65535.0f;
		}
		else if (IsEqualGUID(format, &GUID_WICPixelFormat96bppRGBFloat)) {
			t[0] = floats[x * 3];
			t[1] = floats[x * 3 + 1];
			t[2] = floats[x * 3 + 2];
			t[3] = 1;
		}
		else if (IsEqualGUID(format, &GUID_WICPixelFormat48bppRGBHalf)) {
			t[0] = half_value(words[x * 3]);
			t[1] = half_value(words[x * 3 + 1]);
			t[2] = half_value(words[x * 3 + 2]);
			t[3] = 1;
		}
		else if (IsEqualGUID(format, &GUID_WICPixelFormat16bppGrayHalf)) {
			t[0] = t[1] = t[2] = half_value(words[x]);
			t[3] = 1;
		}
		else
			return FALSE;
	}
	return TRUE;
}

static BOOL write_float_row(const GUID *format, const float *f, UINT w, BYTE *t)
{
	const BOOL premultiplied = IsEqualGUID(format, &GUID_WICPixelFormat128bppPRGBAFloat);
	float *to = (float *)t;
	UINT x;
	if (!premultiplied && !IsEqualGUID(format, &GUID_WICPixelFormat128bppRGBFloat) && !IsEqualGUID(format, &GUID_WICPixelFormat128bppRGBAFloat))
		return FALSE;
	for (x = 0; x < w; x++, f += 4, to += 4) {
		const float scale = premultiplied ? f[3] : 1;
		to[0] = f[0] * scale;
		to[1] = f[1] * scale;
		to[2] = f[2] * scale;
		to[3] = IsEqualGUID(format, &GUID_WICPixelFormat128bppRGBFloat) ? 1 : f[3];
	}
	return TRUE;
}

static const struct
{
	const GUID *format;
	row_reader read;
	row_writer write;
	BOOL indexed;
} formats[] = {
	{&GUID_WICPixelFormat1bppIndexed, read_1, NULL, TRUE},
	{&GUID_WICPixelFormat2bppIndexed, read_2, NULL, TRUE},
	{&GUID_WICPixelFormat4bppIndexed, read_4, NULL, TRUE},
	{&GUID_WICPixelFormat8bppIndexed, read_8, NULL, TRUE},
	{&GUID_WICPixelFormatBlackWhite, read_bw, write_bw},
	{&GUID_WICPixelFormat2bppGray, read_g2, write_g2},
	{&GUID_WICPixelFormat4bppGray, read_g4, write_g4},
	{&GUID_WICPixelFormat8bppGray, read_g8, write_g8},
	{&GUID_WICPixelFormat8bppAlpha, read_a8, write_a8},
	{&GUID_WICPixelFormat16bppGray, read_g16, write_g16},
	{&GUID_WICPixelFormat16bppBGR555, read_555, write_555},
	{&GUID_WICPixelFormat16bppBGR565, read_565, write_565},
	{&GUID_WICPixelFormat16bppBGRA5551, read_5551, write_5551},
	{&GUID_WICPixelFormat24bppBGR, read_bgr24, write_bgr24},
	{&GUID_WICPixelFormat24bppRGB, read_rgb24, write_rgb24},
	{&GUID_WICPixelFormat32bppBGR, read_bgrx, write_bgra},
	{&GUID_WICPixelFormat32bppRGB, read_rgbx, write_rgba},
	{&GUID_WICPixelFormat32bppBGRA, read_bgra, write_bgra},
	{&GUID_WICPixelFormat32bppRGBA, read_rgba, write_rgba},
	{&GUID_WICPixelFormat32bppPBGRA, read_pbgra, write_pbgra},
	{&GUID_WICPixelFormat32bppPRGBA, read_prgba, write_prgba},
	{&GUID_WICPixelFormat32bppCMYK, read_cmyk, NULL},
	{&GUID_WICPixelFormat48bppRGB, read_rgb48, write_rgb48},
	{&GUID_WICPixelFormat64bppRGBA, read_rgba64, write_rgba64},
	{&GUID_WICPixelFormat64bppBGRA, read_bgra64, write_bgra64},
	{&GUID_WICPixelFormat32bppBGR101010, read_bgr101010, write_bgr101010},
	{&GUID_WICPixelFormat32bppR10G10B10A2, read_r10g10b10a2, NULL},
	{&GUID_WICPixelFormat48bppBGR, read_bgr48, write_bgr48},
	{&GUID_WICPixelFormat64bppRGB, read_rgbx64, write_rgbx64},
	{&GUID_WICPixelFormat64bppPBGRA, read_pbgra64, write_pbgra64},
	{&GUID_WICPixelFormat16bppGrayHalf, read_gray_half, NULL},
	{&GUID_WICPixelFormat64bppPRGBA, read_prgba64, write_prgba64},
	{&GUID_WICPixelFormat32bppGrayFloat, read_gray_float, write_gray_float},
	{&GUID_WICPixelFormat48bppRGBHalf, read_rgb48half, NULL},
	{&GUID_WICPixelFormat96bppRGBFloat, read_rgb96f, NULL},
	{&GUID_WICPixelFormat128bppRGBFloat, read_rgb128f, write_rgb128f},
	{&GUID_WICPixelFormat128bppRGBAFloat, read_rgba128f, write_rgba128f},
	{&GUID_WICPixelFormat128bppPRGBAFloat, read_prgba128f, write_prgba128f},
};

static int format_index(const GUID *format)
{
	int index;
	for (index = 0; index < (int)ARRAYSIZE(formats); index++)
		if (IsEqualGUID(format, formats[index].format))
			return index;
	return -1;
}

typedef struct
{
	IWICFormatConverter iface;
	LONG references;
	IWICBitmapSource *inner;
	GUID source_format, target_format;
	UINT width, height;
	WICColor colors[256];
	WICColor target_colors[256];
	UINT target_color_count;
	IWICPalette *target_palette;
} converter;

static converter *converter_of(IWICFormatConverter *iface)
{
	return WIC_CONTAINING(iface, converter, iface);
}

static HRESULT WINAPI converter_QueryInterface(IWICFormatConverter *iface, REFIID riid, void **out)
{
	if (!out)
		return E_INVALIDARG;
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWICBitmapSource) || IsEqualGUID(riid, &IID_IWICFormatConverter)) {
		*out = iface;
		IWICFormatConverter_AddRef(iface);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI converter_AddRef(IWICFormatConverter *iface)
{
	return InterlockedIncrement(&converter_of(iface)->references);
}

static ULONG WINAPI converter_Release(IWICFormatConverter *iface)
{
	converter *self = converter_of(iface);
	const ULONG left = InterlockedDecrement(&self->references);
	if (!left) {
		if (self->inner)
			IWICBitmapSource_Release(self->inner);
		if (self->target_palette)
			IWICPalette_Release(self->target_palette);
		wic_free(self);
	}
	return left;
}

static HRESULT WINAPI converter_GetSize(IWICFormatConverter *iface, UINT *width, UINT *height)
{
	converter *self = converter_of(iface);
	if (!width || !height)
		return E_INVALIDARG;
	if (!self->inner)
		return WINCODEC_ERR_NOTINITIALIZED;
	*width = self->width;
	*height = self->height;
	return S_OK;
}

static HRESULT WINAPI converter_GetPixelFormat(IWICFormatConverter *iface, WICPixelFormatGUID *format)
{
	converter *self = converter_of(iface);
	if (!format)
		return E_INVALIDARG;
	if (!self->inner)
		return WINCODEC_ERR_NOTINITIALIZED;
	*format = self->target_format;
	return S_OK;
}

static HRESULT WINAPI converter_GetResolution(IWICFormatConverter *iface, double *x, double *y)
{
	converter *self = converter_of(iface);
	if (!x || !y)
		return E_INVALIDARG;
	if (!self->inner)
		return WINCODEC_ERR_NOTINITIALIZED;
	return IWICBitmapSource_GetResolution(self->inner, x, y);
}

static HRESULT WINAPI converter_CopyPalette(IWICFormatConverter *iface, IWICPalette *target)
{
	converter *self = converter_of(iface);
	if (!target)
		return E_INVALIDARG;
	if (!self->inner)
		return WINCODEC_ERR_WRONGSTATE;
	if (self->target_palette)
		return IWICPalette_InitializeFromPalette(target, self->target_palette);
	return IWICBitmapSource_CopyPalette(self->inner, target);
}

static UINT nearest_color(const converter *self, const BYTE *bgra)
{
	UINT index, best = 0, best_distance = ~0U;
	for (index = 0; index < self->target_color_count; index++) {
		const BYTE *entry = (const BYTE *)&self->target_colors[index];
		const int blue = entry[0] - bgra[0], green = entry[1] - bgra[1], red = entry[2] - bgra[2], alpha = entry[3] - bgra[3];
		const UINT distance = blue * blue + green * green + red * red + alpha * alpha;
		if (distance < best_distance) {
			best_distance = distance;
			best = index;
		}
	}
	return best;
}

static HRESULT WINAPI converter_CopyPixels(IWICFormatConverter *iface, const WICRect *wanted, UINT stride, UINT size, BYTE *target)
{
	float *float_middle;
	converter *self = converter_of(iface);
	const UINT target_bits = wic_format_bits(&self->target_format);
	WICRect whole;
	UINT source_bits, source_stride, row, row_bytes;
	int from, to;
	BYTE *source_row, *middle;
	HRESULT result = S_OK;
	if (!self->inner)
		return WINCODEC_ERR_WRONGSTATE;
	if (!target)
		return E_INVALIDARG;
	whole.X = whole.Y = 0;
	whole.Width = (INT)self->width;
	whole.Height = (INT)self->height;
	if (!wanted)
		wanted = &whole;
	if (IsEqualGUID(&self->source_format, &self->target_format))
		return IWICBitmapSource_CopyPixels(self->inner, wanted, stride, size, target);
	from = format_index(&self->source_format);
	to = format_index(&self->target_format);
	if (from < 0 || to < 0 || (!formats[to].write && !formats[to].indexed))
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	if (wanted->X < 0 || wanted->Y < 0 || wanted->Width < 0 || wanted->Height < 0 || (UINT)wanted->X + wanted->Width > self->width || (UINT)wanted->Y + wanted->Height > self->height)
		return E_INVALIDARG;
	row_bytes = (UINT)(((UINT64)wanted->Width * target_bits + 7) / 8);
	if (stride < row_bytes)
		return E_INVALIDARG;
	if (!wanted->Width || !wanted->Height)
		return S_OK;
	if ((UINT64)stride * (wanted->Height - 1) + row_bytes > size)
		return WINCODEC_ERR_INSUFFICIENTBUFFER;
	source_bits = wic_format_bits(&self->source_format);
	source_stride = wic_stride(wanted->Width + 8, source_bits);
	source_row = wic_alloc(source_stride);
	middle = wic_alloc((SIZE_T)wanted->Width * 4);
	float_middle = wic_alloc((SIZE_T)wanted->Width * 4 * sizeof(float));
	if (!source_row || !middle) {
		wic_free(source_row);
		wic_free(middle);
	wic_free(float_middle);
		return E_OUTOFMEMORY;
	}
	for (row = 0; row < (UINT)wanted->Height && SUCCEEDED(result); row++) {
		WICRect line = {wanted->X, wanted->Y + (INT)row, wanted->Width, 1};
		BYTE *out_row = target + (SIZE_T)row * stride;
		if (FAILED(result = IWICBitmapSource_CopyPixels(self->inner, &line, source_stride, source_stride, source_row)))
			break;
		if (float_middle && read_float_row(&self->source_format, source_row, (UINT)wanted->Width, float_middle)
			&& write_float_row(&self->target_format, float_middle, (UINT)wanted->Width, out_row))
			continue;
		/* Premultiplied stays premultiplied, without losing what undoing it would. */
		if (IsEqualGUID(&self->source_format, &GUID_WICPixelFormat32bppPBGRA) && IsEqualGUID(&self->target_format, &GUID_WICPixelFormat64bppPRGBA)) {
			USHORT *to = (USHORT *)out_row;
			UINT x;
			for (x = 0; x < (UINT)wanted->Width; x++, to += 4) {
				const BYTE *from = source_row + (SIZE_T)x * 4;
				to[0] = (USHORT)(from[2] * 257U * 65535U >> 16);
				to[1] = (USHORT)(from[1] * 257U * 65535U >> 16);
				to[2] = (USHORT)(from[0] * 257U * 65535U >> 16);
				to[3] = (USHORT)(from[3] * 257U);
			}
			continue;
		}
		/* 48 and 64 bits keep their full depth between each other. */
		if (IsEqualGUID(&self->source_format, &GUID_WICPixelFormat48bppRGB) && IsEqualGUID(&self->target_format, &GUID_WICPixelFormat64bppRGBA)) {
			UINT x;
			for (x = 0; x < (UINT)wanted->Width; x++) {
				memcpy(out_row + (SIZE_T)x * 8, source_row + (SIZE_T)x * 6, 6);
				out_row[x * 8 + 6] = out_row[x * 8 + 7] = 0xff;
			}
			continue;
		}
		formats[from].read(source_row, (UINT)wanted->Width, self->colors, middle);
		if (formats[to].indexed) {
			UINT x;
			memset(out_row, 0, row_bytes);
			for (x = 0; x < (UINT)wanted->Width; x++) {
				const UINT bit = x * target_bits;
				out_row[bit / 8] |= (BYTE)(nearest_color(self, middle + (SIZE_T)x * 4) << (8 - target_bits - bit % 8));
			}
		}
		else
			formats[to].write(middle, (UINT)wanted->Width, out_row);
	}
	wic_free(source_row);
	wic_free(middle);
	wic_free(float_middle);
	return result;
}

static HRESULT WINAPI converter_Initialize(IWICFormatConverter *iface, IWICBitmapSource *source, REFWICPixelFormatGUID target,
	WICBitmapDitherType dither, IWICPalette *palette, double threshold, WICBitmapPaletteType palette_type)
{
	converter *self = converter_of(iface);
	int from, to;
	HRESULT result;
	if (!source || !target)
		return E_INVALIDARG;
	if (self->inner)
		return WINCODEC_ERR_WRONGSTATE;
	if (FAILED(result = IWICBitmapSource_GetPixelFormat(source, &self->source_format)))
		return result;
	if (FAILED(result = IWICBitmapSource_GetSize(source, &self->width, &self->height)))
		return result;
	from = format_index(&self->source_format);
	to = format_index(target);
	if (!IsEqualGUID(&self->source_format, target)) {
		if (from < 0 || to < 0 || !wic_format_bits(target))
			return WINCODEC_ERR_UNSUPPORTEDOPERATION;
		if (formats[to].indexed) {
			/* A palette given is used as it is, even empty. Without one the type names how to make it, and Custom names no way. */
			if (palette)
				IWICPalette_AddRef(palette);
			else {
				const UINT limit = 1U << wic_format_bits(target);
				UINT count = 0;
				if (palette_type == WICBitmapPaletteTypeCustom)
					return E_INVALIDARG;
				if (FAILED(result = wic_palette_create(&palette)))
					return result;
				result = palette_type == WICBitmapPaletteTypeMedianCut ? IWICPalette_InitializeFromBitmap(palette, source, limit, FALSE)
					: IWICPalette_InitializePredefined(palette, palette_type, FALSE);
				if (SUCCEEDED(result))
					IWICPalette_GetColorCount(palette, &count);
				if (SUCCEEDED(result) && count > limit && limit < 256)
					result = E_INVALIDARG;
				if (FAILED(result)) {
					IWICPalette_Release(palette);
					return result;
				}
			}
			self->target_palette = palette;
			IWICPalette_GetColors(palette, 256, self->target_colors, &self->target_color_count);
		}
		else if (!formats[to].write)
			return WINCODEC_ERR_UNSUPPORTEDOPERATION;
		if (formats[from].indexed) {
			IWICPalette *colors;
			UINT count = 0;
			if (FAILED(result = wic_palette_create(&colors)))
				return result;
			result = IWICBitmapSource_CopyPalette(source, colors);
			if (SUCCEEDED(result))
				IWICPalette_GetColors(colors, 256, self->colors, &count);
			IWICPalette_Release(colors);
			if (FAILED(result))
				return result;
		}
	}
	self->target_format = *target;
	IWICBitmapSource_AddRef(source);
	self->inner = source;
	return S_OK;
}

static HRESULT WINAPI converter_CanConvert(IWICFormatConverter *iface, REFWICPixelFormatGUID from, REFWICPixelFormatGUID to, BOOL *result)
{
	int source, target;
	if (!from || !to || !result)
		return E_INVALIDARG;
	source = format_index(from);
	target = format_index(to);
	/* These three can be read when asked for directly, but are no converter's advertised sources. */
	*result = source >= 0 && target >= 0 && (formats[target].write || formats[target].indexed) && !IsEqualGUID(from, &GUID_WICPixelFormat32bppCMYK)
		&& !IsEqualGUID(from, &GUID_WICPixelFormat48bppRGBHalf) && !IsEqualGUID(from, &GUID_WICPixelFormat16bppGrayHalf);
	return *result ? S_OK : WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
}

static const IWICFormatConverterVtbl converter_vtbl = {
	converter_QueryInterface, converter_AddRef, converter_Release, converter_GetSize, converter_GetPixelFormat, converter_GetResolution,
	converter_CopyPalette, converter_CopyPixels, converter_Initialize, converter_CanConvert,
};

HRESULT wic_converter_create(IWICFormatConverter **out)
{
	converter *self = wic_alloc(sizeof(*self));
	if (!self)
		return E_OUTOFMEMORY;
	self->iface.lpVtbl = &converter_vtbl;
	self->references = 1;
	*out = &self->iface;
	return S_OK;
}

HRESULT WINAPI WICConvertBitmapSource(REFWICPixelFormatGUID target, IWICBitmapSource *source, IWICBitmapSource **out)
{
	IWICFormatConverter *created;
	HRESULT result;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!wic_format_bits(target))
		return WINCODEC_ERR_COMPONENTNOTFOUND;
	if (FAILED(result = wic_converter_create(&created)))
		return result;
	/* An indexed target nobody gave a palette to gets one made from the source. */
	if (wic_format_bits(target) <= 8 && !IsEqualGUID(target, &GUID_WICPixelFormat8bppGray) && !IsEqualGUID(target, &GUID_WICPixelFormat8bppAlpha)
		&& !IsEqualGUID(target, &GUID_WICPixelFormatBlackWhite) && !IsEqualGUID(target, &GUID_WICPixelFormat2bppGray) && !IsEqualGUID(target, &GUID_WICPixelFormat4bppGray)) {
		IWICPalette *colors;
		if (FAILED(result = wic_palette_create(&colors))) {
			IWICFormatConverter_Release(created);
			return result;
		}
		result = IWICPalette_InitializeFromBitmap(colors, source, 1U << wic_format_bits(target), FALSE);
		if (SUCCEEDED(result))
			result = IWICFormatConverter_Initialize(created, source, target, WICBitmapDitherTypeNone, colors, 0.0, WICBitmapPaletteTypeCustom);
		IWICPalette_Release(colors);
	}
	else
		result = IWICFormatConverter_Initialize(created, source, target, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
	if (FAILED(result)) {
		IWICFormatConverter_Release(created);
		return result;
	}
	*out = (IWICBitmapSource *)created;
	return S_OK;
}
