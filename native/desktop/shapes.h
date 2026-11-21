#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

// Antialiased drawing for what the window manager draws itself: the Windows 11 frame and the visual styles of the controls.

namespace raven::shapes {

inline uint32_t mix(uint32_t below, uint32_t over, double alpha)
{
	const auto channel = [&](int shift) {
		const double a = double((below >> shift) & 0xff), b = double((over >> shift) & 0xff);
		return uint32_t(std::lround(a + (b - a) * alpha)) << shift;
	};
	return channel(0) | channel(8) | channel(16);
}

// Antialiased shapes: each pixel takes its coverage of the shape, computed from the distance between its center and the shape's
// outline, blended over what the pixel already shows. The surface blends one pixel; the offset maps the caller's coordinates to it.
template<class Surface>
struct Shapes {
	Surface &surface;
	double ox = 0, oy = 0;

	void blend(int64_t x, int64_t y, uint32_t color, double alpha)
	{
		if (alpha > 0.002)
			surface.blend(x, y, color, std::min(alpha, 1.0));
	}

	// Distance from a point to a rounded rectangle, negative inside.
	static double rounded_distance(double px, double py, double left, double top, double right, double bottom, double radius)
	{
		const double cx = (left + right) / 2, cy = (top + bottom) / 2, hw = (right - left) / 2, hh = (bottom - top) / 2;
		radius = std::min({radius, hw, hh});
		const double qx = std::abs(px - cx) - (hw - radius), qy = std::abs(py - cy) - (hh - radius);
		const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
		return outside + std::min(std::max(qx, qy), 0.0) - radius;
	}

	// A rounded rectangle with an optional fill and an optional border of the given width, laid inside its bounds.
	void rounded(double left, double top, double right, double bottom, double radius, std::optional<uint32_t> fill,
		std::optional<uint32_t> border, double width = 1)
	{
		if (right <= left || bottom <= top)
			return;
		left += ox; right += ox; top += oy; bottom += oy;
		// A border alone covers nothing farther than this inside every edge: such pixels are skipped, so an outline costs its perimeter,
		// not its area.
		const double band = radius + width + 1;
		for (int64_t y = int64_t(std::floor(top)); y < int64_t(std::ceil(bottom)); ++y)
			for (int64_t x = int64_t(std::floor(left)); x < int64_t(std::ceil(right)); ++x) {
				if (!fill && double(y) + 0.5 > top + band && double(y) + 0.5 < bottom - band && double(x) + 0.5 > left + band &&
					double(x) + 0.5 < right - band) {
					x = int64_t(std::ceil(right - band - 0.5)) - 1;
					continue;
				}
				const double d = rounded_distance(double(x) + 0.5, double(y) + 0.5, left, top, right, bottom, radius);
				const double outer = std::clamp(0.5 - d, 0.0, 1.0);
				if (outer <= 0)
					continue;
				const double inner = border ? std::clamp(0.5 - (d + width), 0.0, 1.0) : outer;
				if (fill && inner > 0)
					blend(x, y, *fill, inner);
				if (border && outer - inner > 0)
					blend(x, y, *border, outer - inner);
			}
	}

	void circle(double cx, double cy, double radius, std::optional<uint32_t> fill, std::optional<uint32_t> border, double width = 1)
	{
		rounded(cx - radius, cy - radius, cx + radius, cy + radius, radius, fill, border, width);
	}

	// A straight stroke with round ends.
	void segment(double x0, double y0, double x1, double y1, double width, uint32_t color)
	{
		x0 += ox; x1 += ox; y0 += oy; y1 += oy;
		const double half = width / 2, dx = x1 - x0, dy = y1 - y0, length = dx * dx + dy * dy;
		for (int64_t y = int64_t(std::floor(std::min(y0, y1) - half - 1)); y <= int64_t(std::ceil(std::max(y0, y1) + half + 1)); ++y)
			for (int64_t x = int64_t(std::floor(std::min(x0, x1) - half - 1)); x <= int64_t(std::ceil(std::max(x0, x1) + half + 1)); ++x) {
				const double px = double(x) + 0.5, py = double(y) + 0.5;
				const double t = length > 0 ? std::clamp(((px - x0) * dx + (py - y0) * dy) / length, 0.0, 1.0) : 0;
				const double d = std::hypot(px - (x0 + t * dx), py - (y0 + t * dy)) - half;
				blend(x, y, color, std::clamp(0.5 - d, 0.0, 1.0));
			}
	}

	// A chevron pointing up, down, left or right, centered in a box of the given size.
	void chevron(double cx, double cy, double size, char direction, uint32_t color, double width = 1.1)
	{
		const double a = size / 2, b = size / 4;
		switch (direction) {
		case 'd': segment(cx - a, cy - b, cx, cy + b, width, color); segment(cx, cy + b, cx + a, cy - b, width, color); break;
		case 'u': segment(cx - a, cy + b, cx, cy - b, width, color); segment(cx, cy - b, cx + a, cy + b, width, color); break;
		case 'r': segment(cx - b, cy - a, cx + b, cy, width, color); segment(cx + b, cy, cx - b, cy + a, width, color); break;
		default: segment(cx + b, cy - a, cx - b, cy, width, color); segment(cx - b, cy, cx + b, cy + a, width, color); break;
		}
	}

	// A small solid triangle, as the arrows of Windows 11 scroll bars.
	void triangle(double cx, double cy, double size, char direction, uint32_t color)
	{
		double points[3][2];
		const double a = size / 2, b = size / 3;
		switch (direction) {
		case 'u': points[0][0] = cx - a; points[0][1] = cy + b; points[1][0] = cx + a; points[1][1] = cy + b; points[2][0] = cx; points[2][1] = cy - b; break;
		case 'd': points[0][0] = cx - a; points[0][1] = cy - b; points[1][0] = cx + a; points[1][1] = cy - b; points[2][0] = cx; points[2][1] = cy + b; break;
		case 'l': points[0][0] = cx + b; points[0][1] = cy - a; points[1][0] = cx + b; points[1][1] = cy + a; points[2][0] = cx - b; points[2][1] = cy; break;
		default: points[0][0] = cx - b; points[0][1] = cy - a; points[1][0] = cx - b; points[1][1] = cy + a; points[2][0] = cx + b; points[2][1] = cy; break;
		}
		for (auto &point : points)
			point[0] += ox, point[1] += oy;
		const auto edge = [](const double *p, const double *q, double x, double y) { return (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0]); };
		const double area = edge(points[0], points[1], points[2][0], points[2][1]);
		for (int64_t y = int64_t(std::floor(cy + oy - a - 1)); y <= int64_t(std::ceil(cy + oy + a + 1)); ++y)
			for (int64_t x = int64_t(std::floor(cx + ox - a - 1)); x <= int64_t(std::ceil(cx + ox + a + 1)); ++x) {
				int inside = 0;
				for (int sy = 0; sy < 4; ++sy)
					for (int sx = 0; sx < 4; ++sx) {
						const double px = double(x) + (sx + 0.5) / 4, py = double(y) + (sy + 0.5) / 4;
						const double e0 = edge(points[0], points[1], px, py), e1 = edge(points[1], points[2], px, py), e2 = edge(points[2], points[0], px, py);
						if (area > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0))
							++inside;
					}
				blend(x, y, color, inside / 16.0);
			}
	}

	void fill(double left, double top, double right, double bottom, uint32_t color)
	{
		rounded(left, top, right, bottom, 0, color, std::nullopt);
	}
};

}
