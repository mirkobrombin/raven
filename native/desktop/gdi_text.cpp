#include "api.h"
#include "gdi.h"
#include "nt/kernel_context.h"
#include "nt/text.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <map>
#include <sys/mman.h>

#ifdef RAVEN_SKIA

namespace raven {

namespace {

std::vector<uint16_t> to_units(std::u16string_view text)
{
	return std::vector<uint16_t>(text.begin(), text.end());
}

void write_size(const ApiCall &call, uint64_t address, int32_t width, int32_t height)
{
	if (!address)
		return;
	call.store<int32_t>(address, width);
	call.store<int32_t>(address + 4, height);
}

std::u16string widen(const ApiCall &call, uint64_t address, int32_t count, bool ansi)
{
	if (count < 0)
		throw uint32_t(87);
	if (count > 65536)
		throw uint32_t(87);
	if (!count)
		return {};
	if (!ansi) {
		const auto bytes = call.read(address, size_t(count) * 2);
		std::u16string text(size_t(count), 0);
		std::memcpy(text.data(), bytes.data(), bytes.size());
		return text;
	}
	const auto bytes = call.read(address, size_t(count));
	return from_ansi(std::string(bytes.begin(), bytes.end()), 1252);
}

// The families GDI enumerates: the usual Windows faces the bundled fonts stand for, then every family installed in the prefix.
std::vector<InstalledFamily> enumerable_families()
{
	static const char16_t *const usual[] = {u"Arial", u"Courier New", u"Times New Roman", u"MS Shell Dlg", u"MS Sans Serif", u"Tahoma",
		u"Segoe UI", u"Verdana", u"Lucida Console", u"Consolas", u"Microsoft Sans Serif"};
	std::vector<InstalledFamily> families;
	for (const auto *name : usual) {
		InstalledFamily family{name};
		const std::u16string_view view(name);
		family.fixed = view == u"Courier New" || view == u"Lucida Console" || view == u"Consolas";
		family.unicode_ranges[0] = 3;
		family.code_pages[0] = 1;
		families.push_back(family);
	}
	for (auto &installed : installed_font_families())
		if (std::none_of(families.begin(), families.end(), [&](const InstalledFamily &family) { return equal_fold(family.name, installed.name); }))
			families.push_back(std::move(installed));
	return families;
}

// The LOGFONT of an enumerated family and the NEWTEXTMETRICEX that follows the ENUMLOGFONTEX at metrics: the ANSI forms keep characters
// in bytes, so their fields after tmLastChar sit four bytes earlier. Sizes are those of a 12-pixel em.
void describe_family(std::vector<uint8_t> &record, size_t metrics, const InstalledFamily &family, bool ansi, uint8_t charset)
{
	const auto put = [&](size_t at, int32_t value) { std::memcpy(record.data() + at, &value, 4); };
	const int32_t weight = int32_t(family.weight);
	// The LOGFONT of a TrueType face as the enumeration gives it: the cell height and average width of the metrics below, stroke
	// output and clip precision and proof quality.
	put(0, 16);
	put(4, 7);
	put(16, weight);
	record[20] = family.italic;
	record[23] = charset;
	record[24] = 3;
	record[25] = 2;
	record[26] = 1;
	record[27] = family.fixed ? 0x31 : 0x22;
	put(metrics, 16);
	put(metrics + 4, 13);
	put(metrics + 8, 3);
	put(metrics + 12, 4);
	put(metrics + 20, 7);
	put(metrics + 24, family.fixed ? 7 : 16);
	put(metrics + 28, weight);
	put(metrics + 36, 96);
	put(metrics + 40, 96);
	size_t at = metrics + 44;
	if (ansi) {
		record[at] = 0x20;
		record[at + 1] = 0xff;
		record[at + 2] = 0x1f;
		record[at + 3] = 0x20;
		at += 4;
	} else {
		const uint16_t characters[] = {0x20, 0xfffc, 0x1f, 0x20};
		std::memcpy(record.data() + at, characters, sizeof characters);
		at += 8;
	}
	record[at] = family.italic;
	// TMPF_TRUETYPE | TMPF_VECTOR, and the bit Windows sets for a variable pitch, under the family kind.
	record[at + 3] = uint8_t((family.fixed ? 0x06 : 0x07) | (family.fixed ? 0x30 : 0x20));
	record[at + 4] = charset;
	at += 8;
	put(at, (family.weight >= 600 ? 0x20 : 0) | (family.italic ? 0x01 : 0) | (family.weight < 600 && !family.italic ? 0x40 : 0));
	put(at + 4, 2048);
	put(at + 8, 2355);
	put(at + 12, 900);
	for (size_t i = 0; i < 4; ++i)
		put(at + 16 + i * 4, int32_t(family.unicode_ranges[i]));
	put(at + 32, int32_t(family.code_pages[0]));
	put(at + 36, int32_t(family.code_pages[1]));
}

std::u16string_view style_of(const InstalledFamily &family)
{
	if (family.weight >= 600)
		return family.italic ? u"Bold Italic" : u"Bold";
	return family.italic ? u"Italic" : u"Regular";
}

// The character sets a face covers, from the code page bits of its OS/2 table in the order TranslateCharsetInfo gives them, each with
// the script name EnumFontFamiliesEx reports. A symbol font has the symbol set alone; a face that claims none is Western.
struct FontScript {
	uint8_t charset;
	const char16_t *script;
};

std::vector<FontScript> scripts_of(const InstalledFamily &family)
{
	static const struct { int bit; FontScript script; } bits[] = {
		{0, {0, u"Western"}}, {1, {238, u"Central European"}}, {2, {204, u"Cyrillic"}}, {3, {161, u"Greek"}}, {4, {162, u"Turkish"}},
		{5, {177, u"Hebrew"}}, {6, {178, u"Arabic"}}, {7, {186, u"Baltic"}}, {8, {163, u"Vietnamese"}}, {16, {222, u"Thai"}},
		{17, {128, u"Japanese"}}, {18, {134, u"CHINESE_GB2312"}}, {19, {129, u"Hangul"}}, {20, {136, u"CHINESE_BIG5"}},
		{21, {130, u"Hangul(Johab)"}}};
	if (family.code_pages[0] & 0x80000000U)
		return {{2, u"Symbol"}};
	std::vector<FontScript> scripts;
	for (const auto &entry : bits)
		if (family.code_pages[0] & (1U << entry.bit))
			scripts.push_back(entry.script);
	if (scripts.empty())
		scripts.push_back({0, u"Western"});
	return scripts;
}

// The reserved last argument of EnumFontFamiliesEx, which callers pass as zero, carries this when EnumFontFamilies forwards to it.
constexpr uint64_t once_marker = 0x52564e31;

// The records EnumFontFamiliesEx hands out for a family: one per character set it covers when DEFAULT_CHARSET (or no LOGFONT) asks
// for any, the one for the set asked for otherwise, and the first alone for the older EnumFontFamilies, which lists a family once.
std::vector<FontScript> enumerated_scripts(const InstalledFamily &family, int charset, bool once)
{
	auto scripts = scripts_of(family);
	if (once)
		scripts.resize(1);
	else if (charset != 1)
		std::erase_if(scripts, [&](const FontScript &script) { return script.charset != charset; });
	return scripts;
}

}

void NativeDesktop::register_text_api(ApiRegistry &registry)
{
	const auto require_dc = [this](const ApiCall &call, size_t index) -> DesktopDC & {
		auto *dc = objects.dc(call[index]);
		if (!dc)
			throw uint32_t(6);
		return *dc;
	};
	const auto selected_font = [this](const DesktopDC &dc) -> const DesktopFont & {
		const auto *font = objects.font(dc.font);
		if (!font)
			throw uint32_t(6);
		return *font;
	};
	const auto guarded = [](ApiCall &call, uint64_t failure, const std::function<void()> &body) {
		try {
			body();
		} catch (const uint32_t &error) {
			call.fail(error, failure);
		} catch (const std::bad_alloc &) {
			call.fail(8, failure);
		}
	};
	// Cumulative advances: the qualified measurement when the font is inside the measured subset, the general one otherwise.
	const auto measured_advances = [this](std::span<const uint8_t, 92> definition, std::span<const uint16_t> units) {
		try {
			return objects.text().advances(definition, units);
		} catch (const uint32_t &error) {
			if (std::getenv("RAVEN_TRACE_API")) fprintf(stderr, "logical fallback after %u\n", error);
			return objects.text().logical_advances(definition, units);
		}
	};
	// Draws one run of text with GDI alignment, background and clipping rules; returns the run's advance width.
	const auto draw_run = [this, measured_advances](DesktopDC &dc, int32_t x, int32_t y, uint32_t options, const DesktopRect *clip_rect,
		std::u16string_view text, std::span<const int32_t> deltas) -> int32_t {
		Painter painter(objects, dc);
		const auto &font = *objects.font(dc.font);
		const auto metrics = objects.text().logical_metrics(font.definition);
		const auto units = to_units(text);
		int32_t advance = 0;
		std::vector<int32_t> offsets;
		if (!units.empty()) {
			if (!deltas.empty()) {
				int32_t position = 0;
				for (size_t i = 0; i < units.size(); ++i) {
					offsets.push_back(position);
					position += deltas[i];
				}
				advance = position;
			} else {
				const auto cumulative = measured_advances(font.definition, units);
				advance = cumulative.back();
			}
		}
		// Horizontal alignment moves the starting point by the run width; vertical alignment by the cell.
		const uint32_t align = dc.text_align;
		if ((align & 6) == 6)
			x -= advance / 2;
		else if (align & 2)
			x -= advance;
		if ((align & 24) == 24)
			y -= metrics.ascent;
		else if (align & 8)
			y -= metrics.height;
		const int64_t device_x = int64_t(x) + dc.origin_x, device_y = int64_t(y) + dc.origin_y;
		DesktopRect clip{INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};
		if (clip_rect && (options & 4))
			clip = *clip_rect;
		if (clip_rect && (options & 2)) {
			DesktopBrush brush;
			brush.color = dc.background_color;
			painter.fill(clip_rect->left, clip_rect->top, clip_rect->right, clip_rect->bottom, brush);
		}
		if (!units.empty() && dc.background_mode == 2) {
			DesktopBrush brush;
			brush.color = dc.background_color;
			const int32_t left = std::min(x, x + advance), right = std::max(x, x + advance);
			painter.fill(std::max(left, clip.left), std::max(y, clip.top), std::min(right, clip.right), std::min(y + metrics.height, clip.bottom), brush);
		}
		if (units.empty() || advance == 0)
			return advance;
		const int32_t margin = 4;
		const uint32_t box_width = uint32_t(std::min<int64_t>(int64_t(advance) + 2 * margin + 2 * metrics.height, 16384));
		const uint32_t box_height = uint32_t(metrics.height + 4);
		std::vector<uint8_t> coverage;
		try {
			// Qualified fonts keep their measured monochrome glyphs; everything else is anti-aliased.
			try {
				std::vector<std::array<int32_t, 2>> positions;
				for (const auto offset : offsets)
					positions.push_back({offset, 0});
				const auto binary = objects.text().mask(font.definition, units, box_width, box_height, margin, 2, positions);
				coverage.assign(binary.size(), 0);
				for (size_t i = 0; i < binary.size(); ++i)
					coverage[i] = binary[i] ? 255 : 0;
			} catch (const uint32_t &) {
				coverage = objects.text().render(font.definition, units, box_width, box_height, margin, 2, offsets);
			}
		} catch (const uint32_t &) {
			return advance;
		}
		const bool underline = font.definition[21] != 0, strike = font.definition[22] != 0;
		const uint32_t color = dc.text_color;
		for (uint32_t row = 0; row < box_height; ++row)
			for (uint32_t column = 0; column < box_width; ++column) {
				const int64_t px = device_x - margin + column, py = device_y - 2 + row;
				const int64_t logical_x = px - dc.origin_x, logical_y = py - dc.origin_y;
				if (logical_x < clip.left || logical_x >= clip.right || logical_y < clip.top || logical_y >= clip.bottom)
					continue;
				const uint8_t alpha = coverage[size_t(row) * box_width + column];
				if (!alpha)
					continue;
				if (alpha >= 250 || painter.bitmap.depth == 1) {
					painter.plot(px, py, color, 13);
					continue;
				}
				if (!painter.visible(px, py))
					continue;
				const uint32_t below = painter.read(px, py);
				const auto mix = [&](uint32_t foreground, uint32_t background) { return (foreground * alpha + background * (255 - alpha) + 127) / 255; };
				painter.plot(px, py, mix(color_red(color), color_red(below)) | mix(color_green(color), color_green(below)) << 8 | mix(color_blue(color), color_blue(below)) << 16, 13);
			}
		const int32_t thickness = std::max(1, int32_t(std::lround(metrics.height / 16.0)));
		const auto line_across = [&](int32_t line_y) {
			for (int32_t t = 0; t < thickness; ++t)
				for (int32_t dx = 0; dx < advance; ++dx) {
					const int64_t px = device_x + dx, py = device_y + line_y + t;
					const int64_t logical_x = px - dc.origin_x, logical_y = py - dc.origin_y;
					if (logical_x < clip.left || logical_x >= clip.right || logical_y < clip.top || logical_y >= clip.bottom)
						continue;
					painter.plot(px, py, color, 13);
				}
		};
		if (underline)
			line_across(metrics.ascent + std::max(1, metrics.descent / 3));
		if (strike)
			line_across(metrics.ascent * 3 / 5);
		if (painter.changed() && dc.window && dc.leased && updated)
			updated(dc.window);
		return advance;
	};
	const auto extended_text_out = [=, this](ApiCall &call, bool ansi) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			selected_font(dc);
			const uint32_t options = call.u32(3);
			const int32_t count = call.i32(6);
			std::optional<DesktopRect> rect;
			if (call[4] && (options & 6))
				rect = DesktopRect{call.load<int32_t>(call[4]), call.load<int32_t>(call[4] + 4), call.load<int32_t>(call[4] + 8), call.load<int32_t>(call[4] + 12)};
			if ((options & 2) && !rect)
				throw uint32_t(87);
			if (count < 0)
				throw uint32_t(87);
			const auto text = count > 0 ? widen(call, call[5], count, ansi) : std::u16string();
			std::vector<int32_t> deltas;
			if (call[7] && count > 0) {
				const bool pair = (options & 0x2000) != 0;
				const auto bytes = call.read(call[7], size_t(count) * (pair ? 8 : 4));
				for (int32_t i = 0; i < count; ++i) {
					int32_t value;
					std::memcpy(&value, bytes.data() + size_t(i) * (pair ? 8 : 4), 4);
					deltas.push_back(value);
				}
			}
			int32_t x = call.i32(1), y = call.i32(2);
			if (dc.text_align & 1) {
				x = dc.position_x;
				y = dc.position_y;
			}
			const auto advance = draw_run(dc, x, y, options, rect ? &*rect : nullptr, text, deltas);
			if (dc.text_align & 1)
				dc.position_x = x + advance;
			call.ret(1);
		});
	};
	registry.add("ExtTextOutW", 8, [=](ApiCall &call) { extended_text_out(call, false); });
	registry.add("ExtTextOutA", 8, [=](ApiCall &call) { extended_text_out(call, true); });
	const auto text_out = [=, this](ApiCall &call, bool ansi) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			selected_font(dc);
			if (call.i32(4) < 0)
				throw uint32_t(87);
			const auto text = call.i32(4) > 0 ? widen(call, call[3], call.i32(4), ansi) : std::u16string();
			int32_t x = call.i32(1), y = call.i32(2);
			if (dc.text_align & 1) {
				x = dc.position_x;
				y = dc.position_y;
			}
			const auto advance = draw_run(dc, x, y, 0, nullptr, text, {});
			if (dc.text_align & 1)
				dc.position_x = x + advance;
			call.ret(1);
		});
	};
	registry.add("TextOutW", 5, [=](ApiCall &call) { text_out(call, false); });
	registry.add("TextOutA", 5, [=](ApiCall &call) { text_out(call, true); });

	// ---- measurements ----
	const auto extent = [=, this](ApiCall &call, bool ansi) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			const auto text = widen(call, call[1], call.i32(2), ansi);
			if (text.empty()) {
				write_size(call, call[3], 0, 0);
				call.ret(1);
				return;
			}
			const auto cumulative = measured_advances(font.definition, to_units(text));
			write_size(call, call[3], cumulative.back(), objects.text().logical_metrics(font.definition).height);
			call.ret(1);
		});
	};
	for (const auto *name : {"GetTextExtentPoint32W", "GetTextExtentPointW"})
		registry.add(name, 4, [=](ApiCall &call) { extent(call, false); });
	for (const auto *name : {"GetTextExtentPoint32A", "GetTextExtentPointA"})
		registry.add(name, 4, [=](ApiCall &call) { extent(call, true); });
	const auto extent_ex = [=, this](ApiCall &call, bool ansi) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			const auto text = widen(call, call[1], call.i32(2), ansi);
			const int32_t max_extent = call.i32(3);
			std::vector<int32_t> cumulative;
			if (!text.empty())
				cumulative = measured_advances(font.definition, to_units(text));
			int32_t fit = 0;
			if (call[4]) {
				fit = int32_t(cumulative.size());
				if (max_extent >= 0)
					for (size_t i = 0; i < cumulative.size(); ++i)
						if (cumulative[i] > max_extent) {
							fit = int32_t(i);
							break;
						}
				call.store<int32_t>(call[4], fit);
			}
			if (call[5])
				for (size_t i = 0; i < cumulative.size(); ++i)
					call.store<int32_t>(call[5] + i * 4, cumulative[i]);
			write_size(call, call[6], cumulative.empty() ? 0 : cumulative.back(), cumulative.empty() ? 0 : objects.text().logical_metrics(font.definition).height);
			call.ret(1);
		});
	};
	registry.add("GetTextExtentExPointW", 7, [=](ApiCall &call) { extent_ex(call, false); });
	registry.add("GetTextExtentExPointA", 7, [=](ApiCall &call) { extent_ex(call, true); });
	const auto text_metrics = [=, this](ApiCall &call, bool ansi) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			const auto info = objects.text().logical_metrics(font.definition);
			std::vector<uint8_t> bytes(ansi ? 56 : 60, 0);
			const auto put32 = [&](size_t at, int32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
			put32(0, info.height); put32(4, info.ascent); put32(8, info.descent); put32(12, info.internal_leading);
			put32(16, info.external_leading); put32(20, info.average_width); put32(24, info.maximum_width);
			uint32_t weight;
			std::memcpy(&weight, font.definition.data() + 16, 4);
			put32(28, int32_t(weight ? weight : 400));
			put32(36, 96); put32(40, 96);
			size_t at = 44;
			if (ansi) {
				bytes[at] = uint8_t(info.first); bytes[at + 1] = uint8_t(std::min<uint32_t>(info.last, 255)); bytes[at + 2] = uint8_t(info.default_character); bytes[at + 3] = uint8_t(info.break_character);
				at += 4;
			} else {
				const uint16_t chars[4] = {info.first, info.last, info.default_character, info.break_character};
				std::memcpy(bytes.data() + at, chars, 8);
				at += 8;
			}
			bytes[at] = font.definition[20];
			bytes[at + 1] = font.definition[21];
			bytes[at + 2] = font.definition[22];
			bytes[at + 3] = info.pitch;
			bytes[at + 4] = font.definition[23] == 1 ? 0 : font.definition[23];
			call.write(call[1], bytes);
			call.ret(1);
		});
	};
	registry.add("GetTextMetricsW", 2, [=](ApiCall &call) { text_metrics(call, false); });
	// GetTextFaceW(dc, count, buffer): the face name of the font selected into the device context, cut to the buffer; without a buffer
	// the length it needs. Both count the terminating null.
	registry.add("GetTextFaceW", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			std::u16string face;
			for (size_t at = 28; at + 1 < font.definition.size() && at < 28 + 64; at += 2) {
				const char16_t character = char16_t(font.definition[at] | font.definition[at + 1] << 8);
				if (!character)
					break;
				face.push_back(character);
			}
			// The face actually used, as Windows names it: the dialog aliases become the fonts the substitution table gives them, a face
			// the system enumerates keeps its own name, and any other comes out as the default face.
			const auto lower = [](std::u16string text) {
				for (auto &character : text)
					if (character >= u'A' && character <= u'Z')
						character = char16_t(character + 32);
				return text;
			};
			if (lower(face) == u"ms shell dlg")
				face = u"Microsoft Sans Serif";
			else if (lower(face) == u"ms shell dlg 2")
				face = u"Tahoma";
			else {
				const auto families = enumerable_families();
				const auto found = std::find_if(families.begin(), families.end(), [&](const InstalledFamily &family) { return lower(family.name) == lower(face); });
				face = found == families.end() ? u"Arial" : found->name;
			}
			if (!call[2]) {
				call.ret(face.size() + 1);
				return;
			}
			const int32_t count = call.i32(1);
			if (count <= 0) {
				call.fail(87);
				return;
			}
			face.resize(std::min(face.size(), size_t(count - 1)));
			call.put_wide(call[2], face, face.size() + 1);
			call.ret(face.size() + 1);
		});
	});
	// GetTextCharsetInfo(dc, FONTSIGNATURE, flags): the character set of the selected font, and the scripts and code pages its OS/2 table
	// claims; a face the system does not enumerate stands in for the default face, which covers Latin.
	// What a font covers is known by the blocks its OS/2 table claims, the first of which are listed here.
	registry.add("GetFontUnicodeRanges", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			static constexpr struct { int bit; uint16_t first, last; } blocks[] = {
				{0, 0x20, 0x7e}, {1, 0xa0, 0xff}, {2, 0x100, 0x17f}, {3, 0x180, 0x24f}, {7, 0x370, 0x3ff}, {9, 0x400, 0x4ff}, {11, 0x590, 0x5ff}, {13, 0x600, 0x6ff},
				{24, 0xe00, 0xe7f}, {31, 0x2000, 0x206f}, {33, 0x20a0, 0x20cf}, {49, 0x3040, 0x309f}, {50, 0x30a0, 0x30ff}, {56, 0xac00, 0xd7a3}, {59, 0x4e00, 0x9fff},
			};
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			std::u16string face;
			for (size_t at = 28; at + 1 < font.definition.size() && at < 28 + 64; at += 2) {
				const char16_t character = char16_t(font.definition[at] | font.definition[at + 1] << 8);
				if (!character)
					break;
				face.push_back(character);
			}
			InstalledFamily family{face};
			family.unicode_ranges[0] = 3;
			const auto families = enumerable_families();
			if (const auto found = std::find_if(families.begin(), families.end(), [&](const InstalledFamily &item) { return equal_fold(item.name, face); });
				found != families.end())
				family = *found;
			std::vector<uint8_t> set(16, 0);
			uint32_t ranges = 0, glyphs = 0;
			for (const auto &block : blocks) {
				if (!(family.unicode_ranges[block.bit / 32] >> (block.bit % 32) & 1))
					continue;
				const uint16_t count = uint16_t(block.last - block.first + 1);
				set.push_back(uint8_t(block.first));
				set.push_back(uint8_t(block.first >> 8));
				set.push_back(uint8_t(count));
				set.push_back(uint8_t(count >> 8));
				glyphs += count;
				++ranges;
			}
			const uint32_t size = uint32_t(set.size());
			std::memcpy(set.data(), &size, 4);
			std::memcpy(set.data() + 8, &glyphs, 4);
			std::memcpy(set.data() + 12, &ranges, 4);
			if (call[1])
				call.write(call[1], set);
			call.ret(size);
		});
	});
	registry.add("TranslateCharsetInfo", 3, [](ApiCall &call) {
		// In the order of the code page bits of a font signature; the last bit is the symbol set.
		static constexpr struct { uint32_t charset, code_page; int bit; } sets[] = {
			{0, 1252, 0}, {238, 1250, 1}, {204, 1251, 2}, {161, 1253, 3}, {162, 1254, 4}, {177, 1255, 5}, {178, 1256, 6}, {186, 1257, 7},
			{163, 1258, 8}, {222, 874, 16}, {128, 932, 17}, {134, 936, 18}, {129, 949, 19}, {136, 950, 20}, {130, 1361, 21}, {2, 42, 31},
		};
		const uint32_t kind = call.u32(2);
		const uint32_t mask = kind == 3 && call[0] ? call.load<uint32_t>(call[0]) : 0;
		const auto found = std::find_if(std::begin(sets), std::end(sets), [&](const auto &set) {
			if (kind == 1) return set.charset == uint32_t(call[0]);
			if (kind == 2) return set.code_page == uint32_t(call[0]);
			return (mask >> set.bit & 1) != 0;
		});
		if (found == std::end(sets) || !call[1]) {
			call.ret(0);
			return;
		}
		std::vector<uint8_t> info(32, 0);
		std::memcpy(info.data(), &found->charset, 4);
		std::memcpy(info.data() + 4, &found->code_page, 4);
		const uint32_t bit = 1U << found->bit;
		std::memcpy(info.data() + 24, &bit, 4);
		call.write(call[1], info);
		call.ret(1);
	});
	registry.add("GetTextCharsetInfo", 3, [=, this](ApiCall &call) {
		guarded(call, 1, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			std::u16string face;
			for (size_t at = 28; at + 1 < font.definition.size() && at < 28 + 64; at += 2) {
				const char16_t character = char16_t(font.definition[at] | font.definition[at + 1] << 8);
				if (!character)
					break;
				face.push_back(character);
			}
			InstalledFamily family{face};
			family.unicode_ranges[0] = 3;
			family.code_pages[0] = 1;
			const auto families = enumerable_families();
			if (const auto found = std::find_if(families.begin(), families.end(), [&](const InstalledFamily &item) { return equal_fold(item.name, face); });
				found != families.end())
				family = *found;
			if (call[1]) {
				std::vector<uint8_t> signature(24, 0);
				std::memcpy(signature.data(), family.unicode_ranges, 16);
				std::memcpy(signature.data() + 16, family.code_pages, 8);
				call.write(call[1], signature);
			}
			// DEFAULT_CHARSET is realized as the ANSI set; any other request comes back as asked.
			call.ret(font.definition[23] == 1 ? 0 : font.definition[23]);
		});
	});
	// GetGlyphOutlineW(dc, character, format, GLYPHMETRICS, size, buffer, MAT2): the metrics of a character of the selected font and its
	// bitmap, rendered as text draws it: GGO_METRICS (0), GGO_BITMAP (1, a bit per pixel) and GGO_GRAY2, GRAY4 and GRAY8 (4, 5, 6: 4, 16
	// and 64 levels a byte), rows padded to four bytes. Glyph indices, outlines and transforms other than the identity are not drawn:
	// GDI_ERROR, as for a font that cannot give them.
	registry.add("GetGlyphOutlineW", 7, [=, this](ApiCall &call) {
		guarded(call, 0xffffffffU, [&] {
			auto &dc = require_dc(call, 0);
			const uint32_t format = call.u32(2);
			if (!call[3] || (format & 0x80) || (format != 0 && format != 1 && format != 4 && format != 5 && format != 6))
				throw uint32_t(87);
			if (call[6]) {
				const auto matrix = call.read(call[6], 16);
				const std::array<uint8_t, 16> identity{0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};
				if (!std::equal(identity.begin(), identity.end(), matrix.begin()))
					throw uint32_t(120);
			}
			const auto &font = selected_font(dc);
			const auto info = objects.text().logical_metrics(font.definition);
			const uint16_t character = uint16_t(call.u32(1));
			const auto advance = objects.text().logical_advances(font.definition, std::span<const uint16_t>(&character, 1));
			const int32_t cell = advance.empty() ? 0 : advance.back();
			const int32_t pad = std::max(4, info.height / 2);
			const uint32_t width = uint32_t(cell + 2 * pad), height = uint32_t(std::max(1, info.ascent + info.descent));
			const auto coverage = objects.text().render(font.definition, std::span<const uint16_t>(&character, 1), width, height, pad, 0);
			int32_t left = int32_t(width), right = -1, top = int32_t(height), bottom = -1;
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x)
					if (coverage[size_t(y) * width + x]) {
						left = std::min(left, int32_t(x)); right = std::max(right, int32_t(x));
						top = std::min(top, int32_t(y)); bottom = std::max(bottom, int32_t(y));
					}
			const bool empty = right < 0;
			const uint32_t box_width = empty ? 1 : uint32_t(right - left + 1), box_height = empty ? 1 : uint32_t(bottom - top + 1);
			std::vector<uint8_t> metrics(20, 0);
			const auto put32 = [&](size_t at, int32_t value) { std::memcpy(metrics.data() + at, &value, 4); };
			put32(0, int32_t(box_width)); put32(4, int32_t(box_height));
			put32(8, empty ? 0 : left - pad); put32(12, empty ? 0 : info.ascent - top);
			const int16_t increment = int16_t(cell), none = 0;
			std::memcpy(metrics.data() + 16, &increment, 2); std::memcpy(metrics.data() + 18, &none, 2);
			call.write(call[3], metrics);
			if (format == 0) {
				call.ret(1);
				return;
			}
			const uint32_t levels = format == 4 ? 4 : format == 5 ? 16 : 64;
			const uint32_t stride = format == 1 ? ((box_width + 31) / 32) * 4 : (box_width + 3) & ~3U;
			std::vector<uint8_t> bitmap(empty ? 0 : size_t(stride) * box_height, 0);
			for (uint32_t y = 0; !empty && y < box_height; ++y)
				for (uint32_t x = 0; x < box_width; ++x) {
					const uint8_t value = coverage[size_t(top + y) * width + uint32_t(left) + x];
					if (format == 1) {
						if (value >= 128)
							bitmap[size_t(y) * stride + x / 8] |= uint8_t(0x80 >> (x % 8));
					} else {
						bitmap[size_t(y) * stride + x] = uint8_t((value * levels + 127) / 255);
					}
				}
			if (!call[5] || !call.u32(4)) {
				call.ret(bitmap.size());
				return;
			}
			if (call.u32(4) < bitmap.size())
				throw uint32_t(122);
			call.write(call[5], bitmap);
			call.ret(bitmap.size());
		});
	});
	// GetFontData(dc, table, offset, buffer, size): a table of the font file the selected font uses, its tag in file order (the bytes of
	// "hhea" as they read); without a buffer the length from the offset. The whole file (table 0) is not handed out: GDI_ERROR, as for a
	// table the font does not have.
	registry.add("GetFontData", 5, [=, this](ApiCall &call) {
		guarded(call, 0xffffffffU, [&] {
			auto &dc = require_dc(call, 0);
			const uint32_t tag = call.u32(1);
			const auto table = tag ? objects.text().logical_table(selected_font(dc).definition, __builtin_bswap32(tag)) : std::vector<uint8_t>{};
			const uint32_t offset = call.u32(2);
			if (table.empty() || offset > table.size()) {
				call.fail(1, 0xffffffffU);
				return;
			}
			const size_t available = table.size() - offset;
			if (!call[3] || !call.u32(4)) {
				call.ret(available);
				return;
			}
			const size_t count = std::min<size_t>(available, call.u32(4));
			call.write(call[3], std::span<const uint8_t>(table.data() + offset, count));
			call.ret(count);
		});
	});
	// GetOutlineTextMetricsW(dc, size, OUTLINETEXTMETRICW): the text metrics inside, the em square of the font's design units, the
	// underline and strikeout lines in pixels, and the names after the structure, which its name fields locate.
	registry.add("GetOutlineTextMetricsW", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			const auto info = objects.text().logical_metrics(font.definition);
			const size_t width = call.pointer_width(), base = width == 4 ? 216 : 232;
			std::u16string face;
			for (size_t at = 28; at + 1 < font.definition.size() && face.size() < 32; at += 2) {
				const char16_t character = char16_t(font.definition[at] | font.definition[at + 1] << 8);
				if (!character)
					break;
				face.push_back(character);
			}
			const std::u16string names[4] = {face, face, u"Regular", face};
			size_t total = base;
			for (const auto &name : names)
				total += (name.size() + 1) * 2;
			if (!call[2]) {
				call.ret(total);
				return;
			}
			// A buffer that only holds the structure, as GDI+ passes, gets the structure; the names that do not fit are left out.
			if (call.u32(1) < base)
				throw uint32_t(87);
			std::vector<uint8_t> bytes(total, 0);
			const auto put32 = [&](size_t at, int32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
			put32(0, int32_t(total));
			put32(4, info.height); put32(8, info.ascent); put32(12, info.descent); put32(16, info.internal_leading);
			put32(20, info.external_leading); put32(24, info.average_width); put32(28, info.maximum_width);
			uint32_t weight;
			std::memcpy(&weight, font.definition.data() + 16, 4);
			put32(32, int32_t(weight ? weight : 400));
			put32(40, 96); put32(44, 96);
			const uint16_t characters[4] = {info.first, info.last, info.default_character, info.break_character};
			std::memcpy(bytes.data() + 48, characters, 8);
			bytes[56] = font.definition[20]; bytes[57] = font.definition[21]; bytes[58] = font.definition[22]; bytes[59] = info.pitch;
			bytes[60] = font.definition[23] == 1 ? 0 : font.definition[23];
			const int32_t em = std::max<int32_t>(info.height - info.internal_leading, 1);
			put32(76, (weight >= 700 ? 0x20 : 0) | (font.definition[20] ? 1 : 0) | 0x40);
			const uint32_t design_em = objects.text().logical_units_per_em(font.definition);
			put32(84, 1); put32(96, design_em ? int32_t(design_em) : em);
			put32(100, info.ascent); put32(104, -info.descent); put32(108, info.external_leading);
			put32(112, em * 7 / 10); put32(116, em / 2);
			put32(120, 0); put32(124, info.ascent); put32(128, info.maximum_width); put32(132, -info.descent);
			put32(136, info.ascent); put32(140, -info.descent); put32(144, info.external_leading); put32(148, 9);
			put32(152, em * 2 / 3); put32(156, em * 2 / 3); put32(160, 0); put32(164, em / 7);
			put32(168, em * 2 / 3); put32(172, em * 2 / 3); put32(176, 0); put32(180, em * 3 / 8);
			const int32_t line = std::max(1, em / 14);
			put32(184, line); put32(188, em * 3 / 10); put32(192, line); put32(196, -std::max(1, em / 10));
			size_t at = base;
			for (size_t index = 0; index < 4; ++index) {
				const uint64_t offset = at;
				std::memcpy(bytes.data() + 200 + index * width, &offset, width);
				std::memcpy(bytes.data() + at, names[index].data(), names[index].size() * 2);
				at += (names[index].size() + 1) * 2;
			}
			bytes.resize(std::min<size_t>(total, call.u32(1)));
			call.write(call[2], bytes);
			call.ret(bytes.size());
		});
	});
	registry.add("GetTextMetricsA", 2, [=](ApiCall &call) { text_metrics(call, true); });
	registry.add("GetCharWidthW", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			const uint32_t first = call.u32(1), last = call.u32(2);
			if (last < first || last - first > 65535)
				throw uint32_t(87);
			for (uint32_t c = first; c <= last; ++c) {
				const uint16_t unit = uint16_t(c);
				const auto advance = measured_advances(font.definition, std::span<const uint16_t>(&unit, 1));
				call.store<int32_t>(call[3] + uint64_t(c - first) * 4, advance.empty() ? 0 : advance[0]);
			}
			call.ret(1);
		});
	});
	registry.add("GetCharABCWidthsW", 4, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			const uint32_t first = call.u32(1), last = call.u32(2);
			if (last < first || last - first > 65535)
				throw uint32_t(87);
			for (uint32_t c = first; c <= last; ++c) {
				const uint16_t unit = uint16_t(c);
				const auto advance = measured_advances(font.definition, std::span<const uint16_t>(&unit, 1));
				// ABC spacing: no side bearings, so the whole advance is the glyph body.
				const uint64_t at = call[3] + uint64_t(c - first) * 12;
				call.store<int32_t>(at, 0);
				call.store<uint32_t>(at + 4, uint32_t(advance.empty() ? 0 : advance[0]));
				call.store<int32_t>(at + 8, 0);
			}
			call.ret(1);
		});
	});
	// Glyph indices are the UTF-16 units themselves: a font's glyph for a character is named by the character, which ExtTextOutW with
	// ETO_GLYPH_INDEX draws and the glyph measurements below measure.
	for (const bool narrow : {false, true})
		registry.add(narrow ? "GetGlyphIndicesA" : "GetGlyphIndicesW", 5, [=, this](ApiCall &call) {
			guarded(call, 0xffffffffU, [&] {
				require_dc(call, 0);
				const int32_t count = call.i32(2);
				if (count < 0 || !call[1] || !call[3])
					throw uint32_t(87);
				const auto text = widen(call, call[1], count, narrow);
				std::vector<uint8_t> bytes(text.size() * 2);
				std::memcpy(bytes.data(), text.data(), bytes.size());
				call.write(call[3], bytes);
				call.ret(uint32_t(text.size()));
			});
		});
	// GetCharABCWidthsI(dc, first glyph, count, glyph array, ABC) and GetCharWidthI(dc, first glyph, count, glyph array, widths).
	for (const bool abc : {true, false})
		registry.add(abc ? "GetCharABCWidthsI" : "GetCharWidthI", 5, [=, this](ApiCall &call) {
			guarded(call, 0, [&] {
				auto &dc = require_dc(call, 0);
				const auto &font = selected_font(dc);
				const uint32_t first = call.u32(1), count = call.u32(2);
				if (!call[4] || count > 65536)
					throw uint32_t(87);
				for (uint32_t index = 0; index < count; ++index) {
					const uint16_t unit = call[3] ? call.load<uint16_t>(call[3] + uint64_t(index) * 2) : uint16_t(first + index);
					const auto advance = measured_advances(font.definition, std::span<const uint16_t>(&unit, 1));
					const int32_t width = advance.empty() ? 0 : advance[0];
					if (abc) {
						const uint64_t at = call[4] + uint64_t(index) * 12;
						call.store<int32_t>(at, 0);
						call.store<uint32_t>(at + 4, uint32_t(width));
						call.store<int32_t>(at + 8, 0);
					} else {
						call.store<int32_t>(call[4] + uint64_t(index) * 4, width);
					}
				}
				call.ret(1);
			});
		});
	registry.add("GetCharWidthInfo", 2, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			// CHAR_WIDTH_INFO: left bearing, right bearing, and the run width in device units.
			call.store<int32_t>(call[1], 0);
			call.store<int32_t>(call[1] + 4, 0);
			call.store<int32_t>(call[1] + 8, 0);
			call.ret(1);
		});
	});
	registry.add("GdiGetCharDimensions", 3, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			const auto info = objects.text().logical_metrics(font.definition);
			static const std::u16string alphabet = u"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
			const auto cumulative = measured_advances(font.definition, to_units(alphabet));
			if (call[1]) {
				std::vector<uint8_t> bytes(60, 0);
				const auto put32 = [&](size_t at, int32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
				put32(0, info.height); put32(4, info.ascent); put32(8, info.descent); put32(20, info.average_width); put32(24, info.maximum_width);
				call.write(call[1], bytes);
			}
			if (call[2])
				call.store<int32_t>(call[2], info.height);
			call.ret(uint64_t((cumulative.back() / 26 + 1) / 2));
		});
	});

	// ---- fonts ----
	const auto add_font = [this](const std::array<uint8_t, 92> &definition) {
		DesktopFont font;
		font.definition = definition;
		const auto handle = objects.add(font);
		if (!handle)
			throw uint32_t(8);
		return handle;
	};
	registry.add("CreateFontIndirectW", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const auto bytes = call.read(call[0], 92);
			std::array<uint8_t, 92> definition{};
			std::copy(bytes.begin(), bytes.end(), definition.begin());
			call.ret(add_font(definition));
		});
	});
	registry.add("CreateFontIndirectA", 1, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			const auto bytes = call.read(call[0], 60);
			std::array<uint8_t, 92> definition{};
			std::copy(bytes.begin(), bytes.begin() + 28, definition.begin());
			std::string face;
			for (size_t i = 0; i < 32 && bytes[28 + i]; ++i)
				face.push_back(char(bytes[28 + i]));
			const auto wide = from_ansi(face, 1252);
			for (size_t i = 0; i < std::min<size_t>(wide.size(), 31); ++i) {
				definition[28 + i * 2] = uint8_t(wide[i]);
				definition[29 + i * 2] = uint8_t(wide[i] >> 8);
			}
			call.ret(add_font(definition));
		});
	});
	registry.add("CreateFontW", 14, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			std::array<uint8_t, 92> definition{};
			const auto put32 = [&](size_t at, int32_t value) { std::memcpy(definition.data() + at, &value, 4); };
			put32(0, call.i32(0)); put32(4, call.i32(1)); put32(8, call.i32(2)); put32(12, call.i32(3)); put32(16, call.i32(4));
			definition[20] = uint8_t(call.u32(5)); definition[21] = uint8_t(call.u32(6)); definition[22] = uint8_t(call.u32(7));
			definition[23] = uint8_t(call.u32(8)); definition[24] = uint8_t(call.u32(9)); definition[25] = uint8_t(call.u32(10));
			definition[26] = uint8_t(call.u32(11)); definition[27] = uint8_t(call.u32(12));
			if (call[13]) {
				const auto face = call.wide(call[13], 64);
				for (size_t i = 0; i < std::min<size_t>(face.size(), 31); ++i) {
					definition[28 + i * 2] = uint8_t(face[i]);
					definition[29 + i * 2] = uint8_t(face[i] >> 8);
				}
			}
			call.ret(add_font(definition));
		});
	});
	registry.add("CreateFontA", 14, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			std::array<uint8_t, 92> definition{};
			const auto put32 = [&](size_t at, int32_t value) { std::memcpy(definition.data() + at, &value, 4); };
			put32(0, call.i32(0)); put32(4, call.i32(1)); put32(8, call.i32(2)); put32(12, call.i32(3)); put32(16, call.i32(4));
			definition[20] = uint8_t(call.u32(5)); definition[21] = uint8_t(call.u32(6)); definition[22] = uint8_t(call.u32(7));
			definition[23] = uint8_t(call.u32(8)); definition[24] = uint8_t(call.u32(9)); definition[25] = uint8_t(call.u32(10));
			definition[26] = uint8_t(call.u32(11)); definition[27] = uint8_t(call.u32(12));
			if (call[13]) {
				const auto narrow = call.narrow(call[13], 64);
				const std::u16string face(narrow.begin(), narrow.end());
				for (size_t i = 0; i < std::min<size_t>(face.size(), 31); ++i) {
					definition[28 + i * 2] = uint8_t(face[i]);
					definition[29 + i * 2] = uint8_t(face[i] >> 8);
				}
			}
			call.ret(add_font(definition));
		});
	});
	// The bundled families stand for the common Windows faces, and the fonts installed in the prefix follow under their own names.
	registry.add("EnumFontFamiliesExW", 5, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			const auto families = enumerable_families();
			std::u16string wanted;
			int charset = 1;
			if (call[1]) {
				charset = call.read(call[1] + 23, 1)[0];
				const auto bytes = call.read(call[1] + 28, 64);
				for (size_t i = 0; i < 32; ++i) {
					const char16_t c = char16_t(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
					if (!c)
						break;
					wanted.push_back(c);
				}
			}
			if (!call[2]) {
				call.ret(0);
				return;
			}
			// A process without a way to call back into its code enumerates no family rather than fail.
			if (!invoke) {
				call.ret(1);
				return;
			}
			const auto lower = [](std::u16string text) {
				for (auto &c : text)
					if (c >= 'A' && c <= 'Z')
						c = char16_t(c + 32);
				return text;
			};
			uint64_t result = 1;
			const auto scratch_size = uint64_t(4096);
			const auto scratch = process.map_memory(scratch_size, PROT_READ | PROT_WRITE);
			try {
				for (const auto &family : families) {
					if (!wanted.empty() && lower(family.name) != lower(wanted))
						continue;
					for (const auto &script : enumerated_scripts(family, charset, call[4] == once_marker)) {
						// ENUMLOGFONTEXW (LOGFONTW, full name, style, script) followed by NEWTEXTMETRICEXW.
						std::vector<uint8_t> record(512, 0);
						describe_family(record, 348, family, false, script.charset);
						const auto put_text = [&](size_t at, std::u16string_view text, size_t limit) {
							for (size_t i = 0; i < std::min(text.size(), limit); ++i) {
								record[at + i * 2] = uint8_t(text[i]);
								record[at + i * 2 + 1] = uint8_t(text[i] >> 8);
							}
						};
						put_text(28, family.name, 31);
						put_text(92, family.name, 63);
						put_text(92 + 128, style_of(family), 31);
						put_text(92 + 192, script.script, 31);
						call.write(scratch, record);
						const uint64_t arguments[] = {scratch, scratch + 348, 4, call[3]};
						const auto outcome = invoke(call[2], arguments);
						if (outcome.stop != NativeStop::returned)
							throw outcome;
						result = outcome.value;
						if (!uint32_t(outcome.value))
							break;
					}
					if (!uint32_t(result))
						break;
				}
			} catch (...) {
				process.unmap_memory(scratch);
				throw;
			}
			process.unmap_memory(scratch);
			call.ret(uint32_t(result));
		});
	});
	// EnumFontFamiliesW and EnumFontFamiliesA(dc, family, procedure, parameter): the extended form for that family, or for every family
	// when none is named, in any character set.
	const auto enumerate_families = [=, this, &registry](ApiCall &call, bool ansi) {
		guarded(call, 0, [&] {
			const auto extended = registry.find(ansi ? "EnumFontFamiliesExA" : "EnumFontFamiliesExW");
			const size_t size = ansi ? 60 : 92;
			std::vector<uint8_t> logical(size, 0);
			logical[23] = 1;
			if (call[1]) {
				if (ansi) {
					const auto face = call.narrow(call[1], 32);
					std::memcpy(logical.data() + 28, face.data(), std::min<size_t>(face.size(), 31));
				} else {
					const auto face = call.wide(call[1], 32);
					for (size_t i = 0; i < std::min<size_t>(face.size(), 31); ++i) {
						logical[28 + i * 2] = uint8_t(face[i]);
						logical[29 + i * 2] = uint8_t(face[i] >> 8);
					}
				}
			}
			const auto block = process.map_memory(4096, PROT_READ | PROT_WRITE);
			try {
				call.write(block, logical);
				ApiCall inner(call.process, call.environment, {call[0], block, call[2], call[3], once_marker});
				extended->second(inner);
				process.unmap_memory(block);
				call.ret(inner.result);
			} catch (...) {
				process.unmap_memory(block);
				throw;
			}
		});
	};
	registry.add("EnumFontFamiliesW", 4, [enumerate_families](ApiCall &call) { enumerate_families(call, false); });
	// EnumFontsW is the older form with the same records and callback.
	registry.add("EnumFontsW", 4, [enumerate_families](ApiCall &call) { enumerate_families(call, false); });
	registry.add("EnumFontFamiliesA", 4, [enumerate_families](ApiCall &call) { enumerate_families(call, true); });
	// The ANSI form hands the callback ENUMLOGFONTEXA records: a 60-byte LOGFONTA, then the full name, the style and the script as bytes.
	registry.add("EnumFontFamiliesExA", 5, [=, this](ApiCall &call) {
		guarded(call, 0, [&] {
			require_dc(call, 0);
			const auto families = enumerable_families();
			std::string wanted;
			int charset = 1;
			if (call[1]) {
				charset = call.read(call[1] + 23, 1)[0];
				const auto bytes = call.read(call[1] + 28, 32);
				for (size_t i = 0; i < 32 && bytes[i]; ++i)
					wanted.push_back(char(bytes[i]));
			}
			if (!call[2]) {
				call.ret(0);
				return;
			}
			const auto lower = [](std::string text) {
				for (auto &c : text)
					if (c >= 'A' && c <= 'Z')
						c = char(c + 32);
				return text;
			};
			uint64_t result = 1;
			const auto scratch = process.map_memory(4096, PROT_READ | PROT_WRITE);
			try {
				for (const auto &family : families) {
					const auto name = to_ansi(family.name, 1252);
					if (!wanted.empty() && lower(name) != lower(wanted))
						continue;
					for (const auto &script : enumerated_scripts(family, charset, call[4] == once_marker)) {
						// ENUMLOGFONTEXA (LOGFONTA, full name, style, script) followed by NEWTEXTMETRICEXA.
						std::vector<uint8_t> record(512, 0);
						describe_family(record, 256, family, true, script.charset);
						const auto style = to_ansi(style_of(family), 1252);
						const auto script_name = to_ansi(script.script, 1252);
						std::memcpy(record.data() + 28, name.data(), std::min<size_t>(name.size(), 31));
						std::memcpy(record.data() + 60, name.data(), std::min<size_t>(name.size(), 63));
						std::memcpy(record.data() + 124, style.data(), std::min<size_t>(style.size(), 31));
						std::memcpy(record.data() + 156, script_name.data(), std::min<size_t>(script_name.size(), 31));
						call.write(scratch, record);
						const uint64_t arguments[] = {scratch, scratch + 256, 4, call[3]};
						const auto outcome = invoke(call[2], arguments);
						if (outcome.stop != NativeStop::returned)
							throw outcome;
						result = outcome.value;
						if (!uint32_t(outcome.value))
							break;
					}
					if (!uint32_t(result))
						break;
				}
			} catch (...) {
				process.unmap_memory(scratch);
				throw;
			}
			process.unmap_memory(scratch);
			call.ret(uint32_t(result));
		});
	});
	// ---- Uniscribe subset for left-to-right text ----
	// A string analysis keeps the advance of every character measured with the font selected when it was made, which is what
	// the edit control needs for alignment, caret placement and hit testing. Shaping and bidirectional text are not provided.
	struct ScriptString {
		uint64_t dc = 0;
		std::u16string text;
		std::vector<int32_t> position;
		uint64_t block = 0;
	};
	const auto strings = std::make_shared<std::map<uint64_t, ScriptString>>();
	const auto next_string = std::make_shared<uint64_t>(0x5c0000);
	const auto find_string = [strings](uint64_t handle) -> ScriptString * {
		const auto found = strings->find(handle);
		return found == strings->end() ? nullptr : &found->second;
	};
	registry.add("ScriptStringAnalyse", 13, [=, this](ApiCall &call) {
		try {
			auto &dc = require_dc(call, 0);
			const auto &font = selected_font(dc);
			const int32_t length = call.i32(2), charset = call.i32(4);
			const uint32_t flags = call.u32(5);
			if (length <= 0 || length > 65536 || !call[1] || !call[12]) {
				call.ret(0x80070057);
				return;
			}
			ScriptString entry;
			entry.dc = call[0];
			if (flags & 1) {
				const auto character = call.read(call[1], 2);
				entry.text.assign(size_t(length), char16_t(character[0] | (character[1] << 8)));
			} else {
				entry.text = widen(call, call[1], length, charset != -1);
			}
			const auto cumulative = measured_advances(font.definition, to_units(entry.text));
			std::vector<int32_t> tabs;
			int32_t scale = 0, origin = 0;
			if ((flags & 2) && call[10]) {
				const auto count = call.load<int32_t>(call[10]);
				// SCRIPT_TABDEF: the count, the scale, the stops' address and the origin, after the address at the program's width.
				scale = call.load<int32_t>(call[10] + 4);
				origin = call.load<int32_t>(call[10] + 8 + call.pointer_width());
				const auto stops = call.load_pointer(call[10] + 8);
				for (int32_t i = 0; i < count && i < 64 && stops; ++i)
					tabs.push_back(call.load<int32_t>(stops + uint64_t(i) * 4));
			}
			int32_t tab_default = 0;
			if (flags & 2)
				tab_default = 8 * std::max(1, int32_t(measured_advances(font.definition, to_units(u"0")).back()));
			entry.position.assign(1, 0);
			for (size_t i = 0; i < entry.text.size(); ++i) {
				const int32_t at = entry.position.back();
				int32_t advance = cumulative[i] - (i ? cumulative[i - 1] : 0);
				if ((flags & 2) && entry.text[i] == u'\t') {
					const auto unit = [&](int32_t value) { return scale ? int32_t(int64_t(value) * scale / 4) : value; };
					int32_t stop = 0;
					bool found = false;
					if (tabs.size() > 1) {
						for (const auto value : tabs)
							if (origin + unit(value) > at) { stop = origin + unit(value); found = true; break; }
					}
					if (!found) {
						const int32_t step = tabs.size() == 1 ? std::max(1, unit(tabs[0])) : tab_default;
						stop = origin + ((at - origin) / step + 1) * step;
					}
					advance = std::max(0, stop - at);
				}
				entry.position.push_back(at + advance);
			}
			entry.block = process.map_memory(4096, PROT_READ | PROT_WRITE);
			call.store<int32_t>(entry.block, entry.position.back());
			call.store<int32_t>(entry.block + 4, objects.text().logical_metrics(font.definition).height);
			call.store<int32_t>(entry.block + 8, int32_t(entry.text.size()));
			const auto handle = (*next_string)++;
			strings->emplace(handle, std::move(entry));
			call.store_pointer(call[12], handle);
			call.ret(0);
		} catch (const uint32_t &) {
			call.ret(0x80070006);
		}
	});
	registry.add("ScriptStringFree", 1, [=, this](ApiCall &call) {
		if (!call[0]) {
			call.ret(0x80070057);
			return;
		}
		const auto handle = call.load_pointer(call[0]);
		const auto found = strings->find(handle);
		if (found == strings->end()) {
			call.ret(0x80070057);
			return;
		}
		if (found->second.block)
			process.unmap_memory(found->second.block);
		strings->erase(found);
		call.store_pointer(call[0], 0);
		call.ret(0);
	});
	registry.add("ScriptString_pSize", 1, [=](ApiCall &call) {
		const auto *entry = find_string(call[0]);
		call.ret(entry ? entry->block : 0);
	});
	registry.add("ScriptString_pcOutChars", 1, [=](ApiCall &call) {
		const auto *entry = find_string(call[0]);
		call.ret(entry ? entry->block + 8 : 0);
	});
	registry.add("ScriptStringGetLogicalWidths", 2, [=](ApiCall &call) {
		const auto *entry = find_string(call[0]);
		if (!entry || !call[1]) {
			call.ret(0x80070057);
			return;
		}
		for (size_t i = 0; i < entry->text.size(); ++i)
			call.store<int32_t>(call[1] + i * 4, entry->position[i + 1] - entry->position[i]);
		call.ret(0);
	});
	registry.add("ScriptStringCPtoX", 4, [=](ApiCall &call) {
		const auto *entry = find_string(call[0]);
		if (!entry || !call[3]) {
			call.ret(0x80070057);
			return;
		}
		const int32_t index = call.i32(1);
		const bool trailing = call.u32(2) != 0;
		const int32_t count = int32_t(entry->text.size());
		// The trailing edge of the position before the first character is where the string starts, as the edit control asks.
		if (index < -1 || index > count || (index == -1 && !trailing)) {
			call.ret(0x80070057);
			return;
		}
		call.store<int32_t>(call[3], entry->position[size_t(std::min(count, index + (trailing ? 1 : 0)))]);
		call.ret(0);
	});
	registry.add("ScriptStringXtoCP", 4, [=](ApiCall &call) {
		const auto *entry = find_string(call[0]);
		if (!entry || !call[2] || !call[3]) {
			call.ret(0x80070057);
			return;
		}
		const int32_t x = call.i32(1), count = int32_t(entry->text.size());
		int32_t index = count, trailing = 0;
		if (x < 0) {
			index = -1;
			trailing = 1;
		} else if (x < entry->position.back()) {
			for (int32_t i = 0; i < count; ++i)
				if (x < entry->position[size_t(i) + 1]) {
					index = i;
					trailing = x - entry->position[size_t(i)] >= (entry->position[size_t(i) + 1] - entry->position[size_t(i)] + 1) / 2;
					break;
				}
		}
		call.store<int32_t>(call[2], index);
		call.store<int32_t>(call[3], trailing);
		call.ret(0);
	});
	// The item, shape and place calls of Uniscribe for text that runs left to right in one script: one item, one glyph per character
	// whose index is the character itself, and the advances the font gives. The script cache keeps the font a device context had, so
	// the calls that receive no device context still know it, as on Windows.
	const auto cached_font = [=, this](ApiCall &call, uint64_t dc_handle, uint64_t cache) -> const DesktopFont * {
		if (const auto *dc = dc_handle ? objects.dc(dc_handle) : nullptr; dc && objects.font(dc->font)) {
			if (cache)
				call.store_pointer(cache, dc->font);
			return objects.font(dc->font);
		}
		return cache ? objects.font(call.load_pointer(cache)) : nullptr;
	};
	const auto glyph_advances = [=](const DesktopFont &font, const std::u16string &text) {
		const auto cumulative = measured_advances(font.definition, to_units(text));
		std::vector<int32_t> advances(text.size());
		for (size_t index = 0; index < text.size() && index < cumulative.size(); ++index)
			advances[index] = cumulative[index] - (index ? cumulative[index - 1] : 0);
		return advances;
	};
	// ScriptItemize(text, count, item capacity, control, state, SCRIPT_ITEM, item count): one item and the end marker after it.
	registry.add("ScriptItemize", 7, [](ApiCall &call) {
		const int32_t count = call.i32(1), capacity = call.i32(2);
		if (!call[0] || count <= 0 || !call[5]) {
			call.ret(0x80070057U);
			return;
		}
		if (capacity < 2) {
			call.ret(0x8007000eU);
			return;
		}
		const std::vector<uint8_t> items(16, 0);
		call.write(call[5], items);
		call.store<int32_t>(call[5] + 8, count);
		if (call[6])
			call.store<int32_t>(call[6], 1);
		call.ret(0);
	});
	// ScriptShape(dc, cache, text, count, glyph capacity, analysis, glyphs, clusters, visual attributes, glyph count).
	registry.add("ScriptShape", 10, [=](ApiCall &call) {
		const int32_t count = call.i32(3), capacity = call.i32(4);
		if (!call[2] || count <= 0 || !call[6] || !call[7] || !call[8] || !call[9]) {
			call.ret(0x80070057U);
			return;
		}
		if (!cached_font(call, call[0], call[1])) {
			call.ret(0x8000000aU);
			return;
		}
		if (capacity < count) {
			call.ret(0x8007000eU);
			return;
		}
		const auto text = call.read(call[2], size_t(count) * 2);
		std::vector<uint8_t> clusters(size_t(count) * 2), attributes(size_t(count) * 2);
		for (int32_t index = 0; index < count; ++index) {
			clusters[size_t(index) * 2] = uint8_t(index);
			clusters[size_t(index) * 2 + 1] = uint8_t(index >> 8);
			// SCRIPT_VISATTR: every glyph starts its cluster; a space stretches as a blank when text is justified.
			const char16_t character = char16_t(text[size_t(index) * 2] | text[size_t(index) * 2 + 1] << 8);
			attributes[size_t(index) * 2] = uint8_t(0x10 | (character == u' ' ? 4 : 2));
		}
		call.write(call[6], text);
		call.write(call[7], clusters);
		call.write(call[8], attributes);
		call.store<int32_t>(call[9], count);
		call.ret(0);
	});
	// ScriptPlace(dc, cache, glyphs, count, visual attributes, analysis, advances, offsets, ABC).
	registry.add("ScriptPlace", 9, [=](ApiCall &call) {
		const int32_t count = call.i32(3);
		const auto *font = cached_font(call, call[0], call[1]);
		if (!call[2] || count <= 0 || !call[6]) {
			call.ret(0x80070057U);
			return;
		}
		if (!font) {
			call.ret(0x8000000aU);
			return;
		}
		const auto bytes = call.read(call[2], size_t(count) * 2);
		std::u16string text(size_t(count), u'\0');
		std::memcpy(text.data(), bytes.data(), bytes.size());
		const auto advances = glyph_advances(*font, text);
		int64_t total = 0;
		for (int32_t index = 0; index < count; ++index) {
			call.store<int32_t>(call[6] + uint64_t(index) * 4, advances[size_t(index)]);
			total += advances[size_t(index)];
		}
		if (call[7])
			call.write(call[7], std::vector<uint8_t>(size_t(count) * 8, 0));
		if (call[8]) {
			call.store<int32_t>(call[8], 0);
			call.store<uint32_t>(call[8] + 4, uint32_t(total));
			call.store<int32_t>(call[8] + 8, 0);
		}
		call.ret(0);
	});
	// ScriptTextOut(dc, cache, x, y, options, rect, analysis, reserved, reserved, glyphs, count, advances, justified advances, offsets).
	registry.add("ScriptTextOut", 14, [=, this](ApiCall &call) {
		auto *dc = objects.dc(call[0]);
		const int32_t count = call.i32(10);
		if (!dc || !objects.font(dc->font) || !call[9] || count <= 0 || !call[11]) {
			call.ret(0x80070057U);
			return;
		}
		const auto bytes = call.read(call[9], size_t(count) * 2);
		std::u16string text(size_t(count), u'\0');
		std::memcpy(text.data(), bytes.data(), bytes.size());
		std::vector<int32_t> deltas(static_cast<size_t>(count));
		for (int32_t index = 0; index < count; ++index)
			deltas[size_t(index)] = call.load<int32_t>((call[12] ? call[12] : call[11]) + uint64_t(index) * 4);
		std::optional<DesktopRect> rect;
		if (call[5])
			rect = DesktopRect{call.load<int32_t>(call[5]), call.load<int32_t>(call[5] + 4), call.load<int32_t>(call[5] + 8), call.load<int32_t>(call[5] + 12)};
		draw_run(*dc, call.i32(2), call.i32(3), call.u32(4) & 6, rect ? &*rect : nullptr, text, deltas);
		call.ret(0);
	});
	// ScriptCPtoX(position, trailing, characters, glyphs, clusters, visual attributes, advances, analysis, x).
	registry.add("ScriptCPtoX", 9, [](ApiCall &call) {
		const int32_t position = call.i32(0), count = call.i32(2);
		if (!call[6] || !call[8] || count < 0) {
			call.ret(0x80070057U);
			return;
		}
		int32_t x = 0;
		for (int32_t index = 0; index < std::min(position, count); ++index)
			x += call.load<int32_t>(call[6] + uint64_t(index) * 4);
		if (call.u32(1) && position >= 0 && position < count)
			x += call.load<int32_t>(call[6] + uint64_t(position) * 4);
		call.store<int32_t>(call[8], x);
		call.ret(0);
	});
	// ScriptXtoCP(x, characters, glyphs, clusters, visual attributes, advances, analysis, position, trailing).
	registry.add("ScriptXtoCP", 9, [](ApiCall &call) {
		const int32_t x = call.i32(0), count = call.i32(1);
		if (!call[5] || !call[7] || !call[8] || count < 0) {
			call.ret(0x80070057U);
			return;
		}
		int32_t position = count, trailing = 0, start = 0;
		if (x < 0) {
			position = -1;
			trailing = 1;
		} else {
			for (int32_t index = 0; index < count; ++index) {
				const int32_t advance = call.load<int32_t>(call[5] + uint64_t(index) * 4);
				if (x < start + advance) {
					position = index;
					trailing = x - start >= (advance + 1) / 2;
					break;
				}
				start += advance;
			}
		}
		call.store<int32_t>(call[7], position);
		call.store<int32_t>(call[8], trailing);
		call.ret(0);
	});
	// ScriptLayout(runs, levels, visual to logical, logical to visual): left to right runs keep their order.
	registry.add("ScriptLayout", 4, [](ApiCall &call) {
		const int32_t runs = call.i32(0);
		if (runs < 0 || !call[1]) {
			call.ret(0x80070057U);
			return;
		}
		for (int32_t index = 0; index < runs; ++index) {
			if (call[2])
				call.store<int32_t>(call[2] + uint64_t(index) * 4, index);
			if (call[3])
				call.store<int32_t>(call[3] + uint64_t(index) * 4, index);
		}
		call.ret(0);
	});
	registry.add("ScriptFreeCache", 1, [](ApiCall &call) {
		if (call[0])
			call.store_pointer(call[0], 0);
		call.ret(0);
	});
	registry.add("ScriptStringOut", 8, [=, this](ApiCall &call) {
		const auto *entry = find_string(call[0]);
		auto *dc = entry ? objects.dc(entry->dc) : nullptr;
		if (!dc || !objects.font(dc->font)) {
			call.ret(0x80070057);
			return;
		}
		const int32_t x = call.i32(1), y = call.i32(2);
		const uint32_t options = call.u32(3);
		std::optional<DesktopRect> rect;
		if (call[4])
			rect = DesktopRect{call.load<int32_t>(call[4]), call.load<int32_t>(call[4] + 4), call.load<int32_t>(call[4] + 8), call.load<int32_t>(call[4] + 12)};
		const int32_t count = int32_t(entry->text.size());
		const int32_t low = std::clamp(call.i32(5), 0, count), high = std::clamp(call.i32(6), 0, count);
		const bool disabled = call.u32(7) != 0;
		const auto saved_text = dc->text_color, saved_background = dc->background_color, saved_mode = dc->background_mode;
		const auto saved_align = dc->text_align;
		dc->text_align &= ~uint32_t(1 | 2 | 4 | 24);
		if (disabled)
			dc->text_color = objects.system_color(17);
		const auto segment = [&](int32_t from, int32_t to, bool selected) {
			if (from >= to)
				return;
			if (selected && !disabled) {
				dc->background_color = objects.system_color(13);
				dc->text_color = objects.system_color(14);
				dc->background_mode = 2;
			}
			draw_run(*dc, x + entry->position[size_t(from)], y, options, rect ? &*rect : nullptr, std::u16string_view(entry->text).substr(size_t(from), size_t(to - from)), {});
			dc->text_color = disabled ? objects.system_color(17) : saved_text;
			dc->background_color = saved_background;
			dc->background_mode = saved_mode;
		};
		if (low < high) {
			segment(0, low, false);
			segment(low, high, true);
			segment(high, count, false);
		} else {
			segment(0, count, false);
		}
		dc->text_color = saved_text;
		dc->text_align = saved_align;
		call.ret(0);
	});
	// ScriptGetProperties(table, count): the properties of the scripts ScriptItemize names, which is the one neutral script: no language,
	// the ANSI character set and simple text. The table stays for the life of the process, as Uniscribe's does.
	auto script_table = std::make_shared<uint64_t>(0);
	registry.add("ScriptGetProperties", 2, [=, this](ApiCall &call) {
		if (!call[0] && !call[1]) {
			call.ret(0x80070057U);
			return;
		}
		if (!*script_table) {
			const auto block = process.map_memory(4096, PROT_READ | PROT_WRITE);
			if (!block) {
				call.ret(0x8007000eU);
				return;
			}
			// The array of pointers, then the eight bytes of SCRIPT_PROPERTIES the pointer names.
			const uint64_t properties = block + 16;
			call.write(properties, std::vector<uint8_t>(8, 0));
			if (call.pointer_width() == 8)
				call.store<uint64_t>(block, properties);
			else
				call.store<uint32_t>(block, uint32_t(properties));
			*script_table = block;
		}
		if (call[0])
			call.store_pointer(call[0], *script_table);
		if (call[1])
			call.store<int32_t>(call[1], 1);
		call.ret(0);
	});
	// ScriptRecordDigitSubstitution(locale, SCRIPT_DIGITSUBSTITUTE): the locale's own language for national and traditional digits, and
	// no substitution, as the locales Raven reports set it.
	registry.add("ScriptRecordDigitSubstitution", 2, [](ApiCall &call) {
		if (!call[1]) {
			call.ret(0x80070057U);
			return;
		}
		const uint32_t locale = call.u32(0);
		// The default locales stand for US English; a locale's primary language is the low ten bits of its language.
		const uint16_t language = uint16_t((locale == 0 || locale == 0x400 || locale == 0x800 || locale == 0x7f ? 0x409 : locale) & 0x3ff);
		const uint32_t languages = uint32_t(language) | uint32_t(language) << 16;
		call.store<uint32_t>(call[1], languages);
		call.store<uint32_t>(call[1] + 4, 1);
		call.store<uint32_t>(call[1] + 8, 0);
		call.ret(0);
	});
	// Word and character stops for plain text: every character is a cursor stop and a word starts after white space.
	registry.add("ScriptBreak", 4, [=](ApiCall &call) {
		const int32_t length = call.i32(1);
		if (!call[0] || !call[3] || length <= 0 || length > 65536) {
			call.ret(0x80070057);
			return;
		}
		const auto text = widen(call, call[0], length, false);
		std::vector<uint8_t> attributes(static_cast<size_t>(length), 0);
		for (size_t i = 0; i < text.size(); ++i) {
			const bool space = text[i] == u' ' || text[i] == u'\t' || text[i] == u'\n' || text[i] == u'\r';
			const bool start = !space && (!i || text[i - 1] == u' ' || text[i - 1] == u'\t' || text[i - 1] == u'\n' || text[i - 1] == u'\r');
			attributes[i] = uint8_t((start ? 1 : 0) | (space ? 2 : 0) | 4 | (start ? 8 : 0));
		}
		call.write(call[3], attributes);
		call.ret(0);
	});
}

}

#else

namespace raven {

void NativeDesktop::register_text_api(ApiRegistry &)
{
}

}

#endif
