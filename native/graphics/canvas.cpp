#include "canvas.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkShader.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/pathops/SkPathOps.h"
#include "include/core/SkPathUtils.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace raven::canvas {

namespace {

struct Surface {
	SkBitmap bitmap;
	std::unique_ptr<SkCanvas> canvas;
	// How many canvas saves each pushed clip or layer took, so that one pop undoes one push.
	std::vector<int> pushes;
};

std::mutex surfaces_mutex;
std::map<uint32_t, std::unique_ptr<Surface>> surfaces;
uint32_t next_surface = 1;

Surface *find(uint32_t handle)
{
	const auto found = surfaces.find(handle);
	return found == surfaces.end() ? nullptr : found->second.get();
}

// Direct2D's matrix maps (x, y) to (x * m11 + y * m21 + dx, x * m12 + y * m22 + dy).
SkMatrix matrix(const float m[6])
{
	return SkMatrix::MakeAll(m[0], m[2], m[4], m[1], m[3], m[5], 0, 0, 1);
}

SkTileMode tile(uint32_t extend)
{
	return extend == 1 ? SkTileMode::kRepeat : extend == 2 ? SkTileMode::kMirror : SkTileMode::kClamp;
}

SkSamplingOptions sampling(uint32_t interpolation)
{
	// Nearest neighbor (0) stays blocky; the smoother modes of Direct2D become linear or cubic filtering.
	if (!interpolation)
		return SkSamplingOptions(SkFilterMode::kNearest);
	if (interpolation >= 3)
		return SkSamplingOptions(SkCubicResampler::Mitchell());
	return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone);
}

// The variable parts after a command record, checked against what the record says they hold.
struct Parts {
	const float *stops = nullptr, *dashes = nullptr, *path = nullptr;
	const uint8_t *mask = nullptr;
};

bool split(const Command &command, std::span<const uint8_t> data, Parts &parts)
{
	const size_t floats = size_t(command.stops) * 5 + command.dashes + command.path_floats;
	const size_t mask = size_t(command.mask_width) * command.mask_height;
	if (command.stops > 256 || command.dashes > 64 || command.path_floats > (64u << 20) || mask > (256u << 20) ||
		data.size() < floats * 4 + mask)
		return false;
	const auto *base = reinterpret_cast<const float *>(data.data());
	parts.stops = base;
	parts.dashes = base + size_t(command.stops) * 5;
	parts.path = parts.dashes + command.dashes;
	parts.mask = data.data() + floats * 4;
	return true;
}

bool build_floats(std::span<const float> data, uint32_t fill, SkPath &path);

bool build_path(const Command &command, const float *data, SkPath &path)
{
	return build_floats(std::span<const float>(data, command.path_floats), command.fill_mode, path);
}

bool build_floats(std::span<const float> data, uint32_t fill, SkPath &path)
{
	Command command{};
	command.path_floats = uint32_t(data.size());
	command.fill_mode = fill;
	SkPathBuilder builder(command.fill_mode == 1 ? SkPathFillType::kWinding : SkPathFillType::kEvenOdd);
	for (size_t at = 0; at < command.path_floats;) {
		const auto verb = uint32_t(data[at++]);
		const size_t points = verb == 0 || verb == 1 ? 1 : verb == 2 ? 2 : verb == 3 ? 3 : 0;
		if (verb > 4 || at + points * 2 > command.path_floats)
			return false;
		const float *p = data.data() + at;
		switch (verb) {
		case 0: builder.moveTo(p[0], p[1]); break;
		case 1: builder.lineTo(p[0], p[1]); break;
		case 2: builder.quadTo(p[0], p[1], p[2], p[3]); break;
		case 3: builder.cubicTo(p[0], p[1], p[2], p[3], p[4], p[5]); break;
		default: builder.close(); break;
		}
		at += points * 2;
	}
	path = builder.detach();
	return true;
}

// The paint of a brush: its color and opacity, or the shader a gradient or a bitmap gives, in brush space mapped by its transform.
bool brush_paint(const Command &command, const Parts &parts, SkPaint &paint)
{
	paint.setAntiAlias(command.antialias != 0);
	const float opacity = std::clamp(command.opacity, 0.0f, 1.0f);
	const auto brush = Brush(command.brush);
	if (brush == Brush::solid || brush == Brush::none) {
		paint.setColor4f({command.color[0], command.color[1], command.color[2], command.color[3] * opacity});
		return true;
	}
	const SkMatrix local = matrix(command.brush_transform);
	sk_sp<SkShader> shader;
	if (brush == Brush::linear || brush == Brush::radial) {
		std::vector<SkColor4f> colors;
		std::vector<float> positions;
		for (uint32_t index = 0; index < command.stops; ++index) {
			const float *stop = parts.stops + index * 5;
			positions.push_back(std::clamp(stop[0], 0.0f, 1.0f));
			colors.push_back({stop[1], stop[2], stop[3], stop[4]});
		}
		if (colors.empty())
			return false;
		if (colors.size() == 1) {
			paint.setColor4f({colors[0].fR, colors[0].fG, colors[0].fB, colors[0].fA * opacity});
			return true;
		}
		// Positions must not go back: equal neighbors are a hard edge, as in Direct2D.
		for (size_t index = 1; index < positions.size(); ++index)
			positions[index] = std::max(positions[index], positions[index - 1]);
		const SkGradient gradient(SkGradient::Colors(colors, positions, tile(command.extend_x)), SkGradient::Interpolation());
		if (brush == Brush::linear) {
			const SkPoint points[2] = {{command.brush_points[0], command.brush_points[1]}, {command.brush_points[2], command.brush_points[3]}};
			shader = SkShaders::LinearGradient(points, gradient, &local);
		} else {
			// Center, origin offset and the two radii: an elliptic gradient is a circular one stretched along the axes.
			const float cx = command.brush_points[0], cy = command.brush_points[1], ox = command.brush_points[2], oy = command.brush_points[3];
			const float rx = std::max(command.brush_points[4], 1e-6f), ry = std::max(command.brush_points[5], 1e-6f);
			SkMatrix stretch = local;
			stretch.preConcat(SkMatrix::MakeAll(rx, 0, cx, 0, ry, cy, 0, 0, 1));
			if (ox == 0 && oy == 0)
				shader = SkShaders::RadialGradient({0, 0}, 1, gradient, &stretch);
			else
				shader = SkShaders::TwoPointConicalGradient({ox / rx, oy / ry}, 0, {0, 0}, 1, gradient, &stretch);
		}
	} else if (brush == Brush::bitmap) {
		std::lock_guard lock(surfaces_mutex);
		const auto *source = find(command.bitmap);
		if (!source)
			return false;
		SkBitmap view;
		const SkIRect area = SkIRect::MakeXYWH(int(command.source[0]), int(command.source[1]), int(command.source[2]), int(command.source[3]));
		if (command.source[2] > 0 && command.source[3] > 0 && source->bitmap.extractSubset(&view, area))
			shader = view.asImage()->makeShader(tile(command.extend_x), tile(command.extend_y), sampling(command.interpolation), &local);
		else
			shader = source->bitmap.asImage()->makeShader(tile(command.extend_x), tile(command.extend_y), sampling(command.interpolation), &local);
	}
	if (!shader)
		return false;
	paint.setShader(std::move(shader));
	paint.setAlphaf(opacity);
	return true;
}

void stroke_paint(const Command &command, const Parts &parts, SkPaint &paint)
{
	paint.setStyle(SkPaint::kStroke_Style);
	paint.setStrokeWidth(std::max(command.stroke_width, 0.0f));
	// Flat, square, round and triangle caps; Skia has no triangle and draws it round.
	paint.setStrokeCap(command.start_cap == 1 ? SkPaint::kSquare_Cap : command.start_cap >= 2 ? SkPaint::kRound_Cap : SkPaint::kButt_Cap);
	paint.setStrokeJoin(command.line_join == 1 ? SkPaint::kBevel_Join : command.line_join == 2 ? SkPaint::kRound_Join : SkPaint::kMiter_Join);
	paint.setStrokeMiter(std::max(command.miter_limit, 1.0f));
	if (command.dashes >= 2) {
		std::vector<float> intervals(parts.dashes, parts.dashes + (command.dashes & ~1u));
		for (auto &interval : intervals)
			interval = std::max(interval, 0.01f);
		paint.setPathEffect(SkDashPathEffect::Make(intervals, command.dash_offset));
	}
}

}

// A path back in the float form of the commands; a conic section is written as the quadratic curve it bends like.
void floats_of(const SkPath &path, std::vector<float> &result, uint32_t &result_fill)
{
	result.clear();
	const auto points = path.points();
	size_t at = 0;
	for (const auto verb : path.verbs()) {
		const auto put = [&](float code, size_t count) {
			result.push_back(code);
			for (size_t index = 0; index < count && at < points.size(); ++index, ++at) {
				result.push_back(points[at].fX);
				result.push_back(points[at].fY);
			}
		};
		switch (verb) {
		case SkPathVerb::kMove: put(0, 1); break;
		case SkPathVerb::kLine: put(1, 1); break;
		case SkPathVerb::kQuad:
		case SkPathVerb::kConic: put(2, 2); break;
		case SkPathVerb::kCubic: put(3, 3); break;
		case SkPathVerb::kClose: result.push_back(4); break;
		}
	}
	result_fill = path.getFillType() == SkPathFillType::kWinding ? 1 : 0;
}

bool widen(std::span<const float> path, uint32_t fill, std::span<const float> stroke, std::vector<float> &result, uint32_t &result_fill)
{
	SkPath source;
	if (stroke.size() < 7 || stroke.size() < 7 + size_t(stroke[6]) || !build_floats(path, fill, source))
		return false;
	Command command{};
	command.stroke_width = stroke[0];
	command.start_cap = uint32_t(stroke[1]);
	command.end_cap = uint32_t(stroke[2]);
	command.line_join = uint32_t(stroke[3]);
	command.miter_limit = stroke[4];
	command.dash_offset = stroke[5];
	command.dashes = uint32_t(stroke[6]);
	Parts parts;
	parts.dashes = stroke.data() + 7;
	SkPaint paint;
	stroke_paint(command, parts, paint);
	floats_of(skpathutils::FillPathWithPaint(source, paint), result, result_fill);
	return true;
}

bool combine(std::span<const float> one, uint32_t one_fill, std::span<const float> two, uint32_t two_fill, uint32_t mode, std::vector<float> &result,
	uint32_t &result_fill)
{
	static const SkPathOp ops[] = {kUnion_SkPathOp, kIntersect_SkPathOp, kXOR_SkPathOp, kDifference_SkPathOp};
	SkPath first, second;
	if (mode > 3 || !build_floats(one, one_fill, first) || !build_floats(two, two_fill, second))
		return false;
	const auto combined = ::Op(first, second, ops[mode]);
	if (!combined)
		return false;
	floats_of(*combined, result, result_fill);
	return true;
}

uint32_t create(uint32_t width, uint32_t height)
{
	if (!width || !height || width > 32768 || height > 32768)
		return 0;
	auto surface = std::make_unique<Surface>();
	if (!surface->bitmap.tryAllocPixels(SkImageInfo::Make(int(width), int(height), kBGRA_8888_SkColorType, kPremul_SkAlphaType)))
		return 0;
	surface->bitmap.eraseColor(SK_ColorTRANSPARENT);
	surface->canvas = std::make_unique<SkCanvas>(surface->bitmap);
	std::lock_guard lock(surfaces_mutex);
	const uint32_t handle = next_surface++;
	surfaces.emplace(handle, std::move(surface));
	return handle;
}

void destroy(uint32_t surface)
{
	std::lock_guard lock(surfaces_mutex);
	surfaces.erase(surface);
}

bool size(uint32_t handle, uint32_t &width, uint32_t &height)
{
	std::lock_guard lock(surfaces_mutex);
	const auto *surface = find(handle);
	if (!surface)
		return false;
	width = uint32_t(surface->bitmap.width());
	height = uint32_t(surface->bitmap.height());
	return true;
}

bool pixels(uint32_t handle, int32_t x, int32_t y, uint32_t width, uint32_t height, uint8_t *data, uint32_t pitch, bool write)
{
	std::lock_guard lock(surfaces_mutex);
	auto *surface = find(handle);
	if (!surface || pitch < width * 4)
		return false;
	const auto info = SkImageInfo::Make(int(width), int(height), kBGRA_8888_SkColorType, kPremul_SkAlphaType);
	return write ? surface->bitmap.writePixels(SkPixmap(info, data, pitch), x, y) : surface->bitmap.readPixels(info, data, pitch, x, y);
}

bool draw(uint32_t handle, const Command &command, std::span<const uint8_t> data)
{
	Parts parts;
	if (!split(command, data, parts))
		return false;
	Surface *surface;
	{
		std::lock_guard lock(surfaces_mutex);
		surface = find(handle);
	}
	if (!surface)
		return false;
	auto &canvas = *surface->canvas;
	const SkMatrix transform = matrix(command.transform);
	const SkRect rect = SkRect::MakeLTRB(command.rect[0], command.rect[1], command.rect[2], command.rect[3]);
	switch (Op(command.op)) {
	case Op::clear:
		canvas.resetMatrix();
		canvas.drawColor(SkColor4f{command.color[0], command.color[1], command.color[2], command.color[3]}, SkBlendMode::kSrc);
		return true;
	case Op::fill_path:
	case Op::stroke_path: {
		SkPath path;
		SkPaint paint;
		if (!build_path(command, parts.path, path) || !brush_paint(command, parts, paint))
			return false;
		if (Op(command.op) == Op::stroke_path)
			stroke_paint(command, parts, paint);
		canvas.setMatrix(transform);
		canvas.drawPath(path, paint);
		return true;
	}
	case Op::draw_bitmap: {
		sk_sp<SkImage> image;
		// Another surface is drawn from its own pixels, without the copy an image of a changing bitmap takes: Office draws each icon
		// of its ribbon from one large atlas. The surfaces stay locked until the drawing is done, so the pixels cannot go away under
		// it. A surface drawn onto itself is read from a copy taken before the drawing.
		std::unique_lock lock(surfaces_mutex);
		const auto *source = find(command.bitmap);
		if (!source)
			return false;
		image = source == surface ? SkImages::RasterFromPixmapCopy(source->bitmap.pixmap()) : SkImages::RasterFromPixmap(source->bitmap.pixmap(), nullptr, nullptr);
		if (!image)
			return false;
		SkPaint paint;
		paint.setAlphaf(std::clamp(command.opacity, 0.0f, 1.0f));
		paint.setAntiAlias(command.antialias != 0);
		const SkRect from = SkRect::MakeXYWH(command.source[0], command.source[1], command.source[2], command.source[3]);
		canvas.setMatrix(transform);
		// A source rectangle reaching past the bitmap repeats its edge pixels there, as Direct2D draws it: a program fills a tile from a
		// one-pixel bitmap this way. Skia would cut the source to the bitmap and shrink the destination with it.
		if (!SkRect::Make(image->bounds()).contains(from) && !from.isEmpty()) {
			const SkMatrix mapping = SkMatrix::RectToRect(from, rect);
			paint.setShader(image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, sampling(command.interpolation), &mapping));
			canvas.drawRect(rect, paint);
			return true;
		}
		canvas.drawImageRect(image, from, rect, sampling(command.interpolation), &paint, SkCanvas::kFast_SrcRectConstraint);
		return true;
	}
	case Op::fill_mask: {
		if (!command.mask_width || !command.mask_height)
			return true;
		SkPaint paint;
		if (!brush_paint(command, parts, paint))
			return false;
		const auto info = SkImageInfo::MakeA8(int(command.mask_width), int(command.mask_height));
		const auto mask = SkImages::RasterFromPixmapCopy(SkPixmap(info, parts.mask, command.mask_width));
		if (!mask)
			return false;
		canvas.setMatrix(transform);
		canvas.drawImage(mask, command.rect[0], command.rect[1], SkSamplingOptions(), &paint);
		return true;
	}
	case Op::push_clip:
		surface->pushes.push_back(1);
		canvas.save();
		canvas.setMatrix(transform);
		canvas.clipRect(rect, command.antialias != 0);
		return true;
	case Op::push_layer: {
		// The content bounds clip, the geometric mask clips with its own antialiasing, and the opacity applies to the whole layer.
		int saves = 1;
		canvas.save();
		canvas.setMatrix(transform);
		if (rect.width() > 0 && rect.height() > 0 && rect.width() < 1e30f)
			canvas.clipRect(rect, command.antialias != 0);
		if (command.path_floats) {
			SkPath mask;
			if (!build_path(command, parts.path, mask)) {
				canvas.restore();
				return false;
			}
			canvas.clipPath(mask, command.antialias != 0);
		}
		if (command.opacity < 1.0f) {
			canvas.resetMatrix();
			canvas.saveLayerAlphaf(nullptr, std::clamp(command.opacity, 0.0f, 1.0f));
			++saves;
		}
		surface->pushes.push_back(saves);
		return true;
	}
	case Op::pop:
		if (surface->pushes.empty())
			return false;
		for (int index = 0; index < surface->pushes.back(); ++index)
			canvas.restore();
		surface->pushes.pop_back();
		return true;
	case Op::copy: {
		// A plain copy of pixels from a surface, past every clip and blend: CopyFromBitmap and CopyFromRenderTarget.
		std::lock_guard lock(surfaces_mutex);
		const auto *source = find(command.bitmap);
		if (!source)
			return false;
		SkBitmap area;
		const SkIRect from = SkIRect::MakeXYWH(int(command.source[0]), int(command.source[1]), int(command.source[2]), int(command.source[3]));
		if (!source->bitmap.extractSubset(&area, from))
			return false;
		SkBitmap copy;
		if (!copy.tryAllocPixels(area.info()) || !area.readPixels(copy.pixmap()))
			return false;
		return surface->bitmap.writePixels(copy.pixmap(), int(command.rect[0]), int(command.rect[1]));
	}
	}
	return false;
}

}
