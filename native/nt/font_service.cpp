#include "kernel_context.h"
#include "prefix.h"
#include "text.h"

#ifdef RAVEN_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_TYPE1_TABLES_H
#include FT_BDF_H
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace raven {

#ifdef RAVEN_FREETYPE

namespace {

// The host side of the DirectWrite component: font files are read, measured and rasterized with FreeType, and the guest gets numbers and
// bitmaps back. Handles are small integers; the file bytes are copied because FreeType keeps pointing at them.
struct HostFont {
	std::vector<uint8_t> data;
	FT_Face face = nullptr;
};

std::mutex font_mutex;
FT_Library library = nullptr;
std::map<uint32_t, std::unique_ptr<HostFont>> fonts;
// Every scanned face, kept for GDI's family enumeration.
std::vector<InstalledFamily> scanned_faces;
uint32_t next_font = 1;

FT_Library freetype()
{
	if (!library && FT_Init_FreeType(&library))
		library = nullptr;
	return library;
}

void put32(std::vector<uint8_t> &bytes, uint32_t value)
{
	for (int shift = 0; shift < 32; shift += 8)
		bytes.push_back(uint8_t(value >> shift));
}

void put_text(std::vector<uint8_t> &bytes, const char *text)
{
	const std::string value = text ? text : "";
	bytes.insert(bytes.end(), value.begin(), value.end());
	bytes.push_back(0);
}

// The English name a face's name table gives for an id, in UTF-8: Windows names are UTF-16 big endian, Macintosh names single byte.
std::string sfnt_name(FT_Face face, uint32_t id)
{
	std::string result;
	const FT_UInt count = FT_Get_Sfnt_Name_Count(face);
	for (FT_UInt index = 0; index < count && result.empty(); ++index) {
		FT_SfntName name;
		if (FT_Get_Sfnt_Name(face, index, &name) || name.name_id != id)
			continue;
		if (name.platform_id == 3 && (name.language_id & 0x3ff) == 9) {
			for (FT_UInt at = 0; at + 1 < name.string_len; at += 2) {
				const uint32_t unit = uint32_t(name.string[at]) << 8 | name.string[at + 1];
				if (unit < 0x80)
					result.push_back(char(unit));
				else if (unit < 0x800) {
					result.push_back(char(0xc0 | unit >> 6));
					result.push_back(char(0x80 | (unit & 0x3f)));
				} else {
					result.push_back(char(0xe0 | unit >> 12));
					result.push_back(char(0x80 | ((unit >> 6) & 0x3f)));
					result.push_back(char(0x80 | (unit & 0x3f)));
				}
			}
		} else if (name.platform_id == 1 && name.language_id == 0) {
			result.assign(reinterpret_cast<const char *>(name.string), name.string_len);
		}
	}
	return result;
}

// The system fonts, as records the guest reads: sizes, weight, style, width, flags, then path, family, face, full and PostScript names,
// then the Win32 family and the typographic family and face. The family and face are the weight-stretch-style ones DirectWrite groups
// by (name ids 21 and 22, else 1 and 2); GDI names a family by its Win32 name (id 1), which keeps Calibri Light or Arial Narrow apart
// from Calibri and Arial, where FreeType's family name is the typographic one (id 16).
// They are the fonts installed in the Windows Fonts folder of the prefix, which keeps the ones programs install, and the host's own font
// folders, which stand for the fonts Windows ships: a prefix whose folder predates the links to them still has its system fonts. A file
// the folder links is read once.
std::vector<uint8_t> scan_fonts()
{
	static std::vector<uint8_t> cache;
	static bool done = false;
	if (done)
		return cache;
	done = true;
	if (!freetype())
		return cache;
	std::error_code error;
	std::vector<std::filesystem::path> roots;
	try {
		const auto installed = NativePrefix::from_environment().root() / "drive_c" / "windows" / "Fonts";
		if (std::filesystem::is_directory(installed, error))
			roots.push_back(installed);
	} catch (const std::exception &) {}
	roots.insert(roots.end(), {"/usr/share/fonts", "/usr/local/share/fonts"});
	std::set<std::filesystem::path> seen;
	for (const auto &root : roots) {
		if (!std::filesystem::exists(root, error))
			continue;
		for (std::filesystem::recursive_directory_iterator walk(root, std::filesystem::directory_options::follow_directory_symlink, error), end; walk != end; walk.increment(error)) {
			if (error || !walk->is_regular_file(error))
				continue;
			auto extension = walk->path().extension().string();
			for (auto &character : extension)
				character = char(std::tolower(character));
			if (extension != ".ttf" && extension != ".otf" && extension != ".ttc" && extension != ".otc")
				continue;
			std::error_code resolving;
			const auto target = std::filesystem::weakly_canonical(walk->path(), resolving);
			if (!seen.insert(resolving ? walk->path() : target).second)
				continue;
			FT_Face probe = nullptr;
			if (FT_New_Face(library, walk->path().c_str(), 0, &probe))
				continue;
			const FT_Long faces = probe->num_faces;
			FT_Done_Face(probe);
			for (FT_Long index = 0; index < faces; ++index) {
				FT_Face face = nullptr;
				if (FT_New_Face(library, walk->path().c_str(), index, &face))
					continue;
				if (face->family_name && FT_IS_SCALABLE(face)) {
					uint32_t weight = (face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
					uint32_t stretch = 5;
					if (const auto *os2 = static_cast<TT_OS2 *>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2))) {
						if (os2->usWeightClass >= 1 && os2->usWeightClass <= 1000)
							weight = os2->usWeightClass;
						if (os2->usWidthClass >= 1 && os2->usWidthClass <= 9)
							stretch = os2->usWidthClass;
					}
					const uint32_t style = (face->style_flags & FT_STYLE_FLAG_ITALIC) ? 2 : 0;
					const uint32_t flags = (FT_IS_FIXED_WIDTH(face) ? 2U : 0U);
					const auto named = [&](uint32_t id, uint32_t fallback, const char *last) {
						auto text = sfnt_name(face, id);
						if (text.empty())
							text = sfnt_name(face, fallback);
						return text.empty() ? std::string(last ? last : "") : text;
					};
					const auto win32_family = named(1, 1, face->family_name), win32_face = named(2, 2, face->style_name);
					const auto family = named(21, 1, face->family_name), style_name = named(22, 2, face->style_name);
					const auto typographic_family = named(16, 1, face->family_name), typographic_face = named(17, 2, face->style_name);
					InstalledFamily found{from_utf8(win32_family), weight, style != 0, (flags & 2) != 0};
					if (const auto *os2 = static_cast<TT_OS2 *>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2))) {
						found.unicode_ranges[0] = uint32_t(os2->ulUnicodeRange1);
						found.unicode_ranges[1] = uint32_t(os2->ulUnicodeRange2);
						found.unicode_ranges[2] = uint32_t(os2->ulUnicodeRange3);
						found.unicode_ranges[3] = uint32_t(os2->ulUnicodeRange4);
						found.code_pages[0] = uint32_t(os2->ulCodePageRange1);
						found.code_pages[1] = uint32_t(os2->ulCodePageRange2);
					}
					// A font that claims no script still draws Latin text.
					if (!found.code_pages[0] && !found.code_pages[1]) {
						found.unicode_ranges[0] |= 3;
						found.code_pages[0] = 1;
					}
					scanned_faces.push_back(std::move(found));
					std::vector<uint8_t> record;
					put32(record, 0);
					put32(record, uint32_t(index));
					put32(record, weight);
					put32(record, style);
					put32(record, stretch);
					put32(record, flags);
					put_text(record, walk->path().c_str());
					put_text(record, family.c_str());
					put_text(record, style_name.c_str());
					const auto full = named(4, 4, (win32_family + (win32_face.empty() ? "" : " " + win32_face)).c_str());
					put_text(record, full.c_str());
					put_text(record, FT_Get_Postscript_Name(face));
					put_text(record, win32_family.c_str());
					put_text(record, typographic_family.c_str());
					put_text(record, typographic_face.c_str());
					const uint32_t size = uint32_t(record.size());
					std::memcpy(record.data(), &size, 4);
					cache.insert(cache.end(), record.begin(), record.end());
				}
				FT_Done_Face(face);
			}
		}
	}
	return cache;
}

}

std::vector<InstalledFamily> installed_font_families()
{
	std::lock_guard lock(font_mutex);
	scan_fonts();
	// One entry per family: the upright face whose weight is nearest to regular.
	std::map<std::u16string, InstalledFamily> chosen;
	const auto distance = [](const InstalledFamily &face) { return (face.italic ? 1000 : 0) + std::abs(int(face.weight) - 400); };
	for (const auto &face : scanned_faces) {
		if (face.name.empty())
			continue;
		auto key = face.name;
		for (auto &c : key)
			if (c >= u'A' && c <= u'Z')
				c = char16_t(c + 32);
		const auto found = chosen.find(key);
		if (found == chosen.end() || distance(face) < distance(found->second))
			chosen[key] = face;
	}
	std::vector<InstalledFamily> families;
	for (auto &[key, face] : chosen)
		families.push_back(std::move(face));
	return families;
}

namespace {

HostFont *font_of(uint32_t handle)
{
	const auto found = fonts.find(handle);
	return found == fonts.end() ? nullptr : found->second.get();
}

struct PathSink {
	std::vector<uint8_t> *bytes;
	FT_Vector current;
};

void emit(std::vector<uint8_t> &bytes, uint32_t operation, std::initializer_list<FT_Pos> coordinates)
{
	put32(bytes, operation);
	size_t count = 0;
	for (const auto value : coordinates) {
		put32(bytes, uint32_t(int32_t(value)));
		++count;
	}
	for (; count < 6; ++count)
		put32(bytes, 0);
}

}

void register_font_service(ApiRegistry &registry, KernelContext &)
{
	registry.add("RavenFontScan", 2, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		const auto records = scan_fonts();
		if (call[0] && call.u32(1) >= records.size())
			call.write(call[0], records);
		call.ret(uint32_t(records.size()));
	});
	registry.add("RavenFontOpen", 3, [](ApiCall &call) {
		if (!call[0] || !call.u32(1)) {
			call.ret(0);
			return;
		}
		std::lock_guard lock(font_mutex);
		if (!freetype()) {
			call.ret(0);
			return;
		}
		auto font = std::make_unique<HostFont>();
		font->data = call.read(call[0], call.u32(1));
		if (FT_New_Memory_Face(library, font->data.data(), FT_Long(font->data.size()), FT_Long(call.u32(2)), &font->face)) {
			call.ret(0);
			return;
		}
		const uint32_t handle = next_font++;
		fonts[handle] = std::move(font);
		call.ret(handle);
	});
	registry.add("RavenFontClose", 1, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		if (const auto found = fonts.find(call.u32(0)); found != fonts.end()) {
			FT_Done_Face(found->second->face);
			fonts.erase(found);
		}
	});
	// Eighteen words: the design measures of the face, then flags and counts.
	registry.add("RavenFontInfo", 2, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		const auto *font = font_of(call.u32(0));
		if (!font || !call[1]) {
			call.ret(0);
			return;
		}
		const FT_Face face = font->face;
		int32_t info[20] = {};
		info[0] = face->units_per_EM;
		info[1] = face->ascender;
		info[2] = -face->descender;
		info[3] = face->height - (face->ascender - face->descender);
		const auto *os2 = static_cast<TT_OS2 *>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
		info[4] = os2 && os2->version >= 2 ? os2->sCapHeight : face->ascender * 7 / 10;
		info[5] = os2 && os2->version >= 2 ? os2->sxHeight : face->ascender / 2;
		info[6] = face->underline_position;
		info[7] = face->underline_thickness;
		info[8] = os2 ? os2->yStrikeoutPosition : face->ascender / 3;
		info[9] = os2 ? os2->yStrikeoutSize : face->underline_thickness;
		info[10] = int32_t(face->num_glyphs);
		info[11] = (FT_IS_SCALABLE(face) ? 1 : 0) | ((face->style_flags & FT_STYLE_FLAG_ITALIC) ? 2 : 0) | ((face->style_flags & FT_STYLE_FLAG_BOLD) ? 4 : 0) |
			(FT_IS_FIXED_WIDTH(face) ? 8 : 0) | (FT_HAS_COLOR(face) ? 16 : 0) | (FT_Get_Sfnt_Table(face, FT_SFNT_OS2) && os2->fsSelection == 0xffff ? 32 : 0);
		info[12] = os2 ? int32_t(os2->usWeightClass) : (face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
		info[13] = int32_t(face->num_faces);
		info[14] = face->bbox.xMin;
		info[15] = face->bbox.yMin;
		info[16] = face->bbox.xMax;
		info[17] = face->bbox.yMax;
		info[18] = FT_IS_SFNT(face) && !FT_Get_Sfnt_Table(face, FT_SFNT_HEAD) ? 1 : 0;
		info[19] = os2 ? int32_t(os2->usWidthClass) : 5;
		call.write(call[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(info), sizeof(info)));
		call.ret(1);
	});
	registry.add("RavenFontGlyphs", 4, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		const auto *font = font_of(call.u32(0));
		if (!font || !call[1] || !call[3]) {
			call.ret(0);
			return;
		}
		const uint32_t count = call.u32(2);
		const auto points = call.read(call[1], size_t(count) * 4);
		std::vector<uint16_t> glyphs(count);
		for (uint32_t index = 0; index < count; ++index) {
			uint32_t codepoint;
			std::memcpy(&codepoint, points.data() + index * 4, 4);
			glyphs[index] = uint16_t(FT_Get_Char_Index(font->face, codepoint));
		}
		call.write(call[3], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(glyphs.data()), glyphs.size() * 2));
		call.ret(1);
	});
	// DWRITE_GLYPH_METRICS in design units: left bearing, advance, right bearing, top bearing, vertical advance, bottom bearing, vertical origin.
	registry.add("RavenFontMetrics", 3, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		const auto *font = font_of(call.u32(0));
		if (!font || !call[2] || FT_Load_Glyph(font->face, call.u32(1), FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP)) {
			call.ret(0);
			return;
		}
		const auto &metrics = font->face->glyph->metrics;
		int32_t out[7] = {};
		out[0] = int32_t(metrics.horiBearingX);
		out[1] = int32_t(metrics.horiAdvance);
		out[2] = int32_t(metrics.horiAdvance - metrics.horiBearingX - metrics.width);
		out[3] = int32_t(font->face->ascender - metrics.horiBearingY);
		out[4] = int32_t(font->face->ascender - font->face->descender);
		out[5] = int32_t(out[4] - out[3] - metrics.height);
		out[6] = int32_t(font->face->ascender);
		call.write(call[2], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(out), sizeof(out)));
		call.ret(1);
	});
	// A glyph bitmap: the em size and the 2 by 2 matrix are 16.16 fixed point; the mode is 0 for black and white, 1 for gray levels and
	// 2 for three horizontal sub-pixels. The bounds are four words (left, top, right, bottom) in pixels with y downward.
	registry.add("RavenFontRender", 10, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		const auto *font = font_of(call.u32(0));
		if (!font) {
			call.ret(0);
			return;
		}
		const FT_Face face = font->face;
		const double em = double(int32_t(call.u32(2))) / 65536.0;
		if (FT_Set_Char_Size(face, FT_F26Dot6(em * 64.0), 0, 72, 72)) {
			call.ret(0);
			return;
		}
		FT_Matrix matrix{0x10000, 0, 0, 0x10000};
		if (call[3]) {
			const auto words = call.read(call[3], 16);
			int32_t value[4];
			std::memcpy(value, words.data(), 16);
			// The matrix arrives in device order (x scale, x shear, y shear, y scale) with y downward.
			matrix = {value[0], -value[2], -value[1], value[3]};
		}
		// The pixel shift is the fractional position of the glyph origin, 16.16 fixed point, y downward.
		FT_Vector shift{FT_Pos(int32_t(call.u32(8))) * 64 / 65536, -FT_Pos(int32_t(call.u32(9))) * 64 / 65536};
		FT_Set_Transform(face, &matrix, &shift);
		const uint32_t mode = call.u32(4);
		if (FT_Load_Glyph(face, call.u32(1), FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | (mode == 0 ? FT_LOAD_TARGET_MONO : 0))) {
			FT_Set_Transform(face, nullptr, nullptr);
			call.ret(0);
			return;
		}
		const auto render = mode == 0 ? FT_RENDER_MODE_MONO : mode == 2 ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL;
		if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(face->glyph, render)) {
			FT_Set_Transform(face, nullptr, nullptr);
			call.ret(0);
			return;
		}
		FT_Set_Transform(face, nullptr, nullptr);
		const FT_Bitmap &bitmap = face->glyph->bitmap;
		const int32_t pixel_width = mode == 2 ? int32_t(bitmap.width / 3) : int32_t(bitmap.width), rows = int32_t(bitmap.rows);
		const size_t row_bytes = mode == 2 ? size_t(pixel_width) * 3 : size_t(pixel_width), total = row_bytes * size_t(rows);
		const int32_t bounds[4] = {face->glyph->bitmap_left, -face->glyph->bitmap_top, face->glyph->bitmap_left + pixel_width,
			-face->glyph->bitmap_top + rows};
		if (call[7])
			call.write(call[7], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(bounds), sizeof(bounds)));
		if (call[5] && call.u32(6) >= total && total) {
			std::vector<uint8_t> out(total);
			for (int32_t row = 0; row < rows; ++row)
				for (size_t column = 0; column < row_bytes; ++column) {
					uint8_t value;
					if (mode == 0)
						value = (bitmap.buffer[size_t(row) * size_t(bitmap.pitch) + column / 8] >> (7 - column % 8)) & 1 ? 255 : 0;
					else
						value = bitmap.buffer[size_t(row) * size_t(bitmap.pitch) + column];
					out[size_t(row) * row_bytes + column] = value;
				}
			call.write(call[5], out);
		}
		call.ret(uint32_t(total));
	});
	// The outline of a glyph in design units, y up, as a list of records: an operation (0 move, 1 line, 2 quadratic, 3 cubic, 4 close)
	// and six coordinates.
	registry.add("RavenFontOutline", 4, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		const auto *font = font_of(call.u32(0));
		if (!font || FT_Load_Glyph(font->face, call.u32(1), FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) || font->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
			call.ret(0);
			return;
		}
		std::vector<uint8_t> bytes;
		FT_Outline_Funcs functions{};
		functions.move_to = [](const FT_Vector *to, void *user) {
			auto &sink = *static_cast<PathSink *>(user);
			// Each contour but the first is closed before the next begins.
			if (!sink.bytes->empty())
				emit(*sink.bytes, 4, {});
			emit(*sink.bytes, 0, {to->x, to->y});
			sink.current = *to;
			return 0;
		};
		functions.line_to = [](const FT_Vector *to, void *user) {
			auto &sink = *static_cast<PathSink *>(user);
			emit(*sink.bytes, 1, {to->x, to->y});
			sink.current = *to;
			return 0;
		};
		functions.conic_to = [](const FT_Vector *control, const FT_Vector *to, void *user) {
			auto &sink = *static_cast<PathSink *>(user);
			emit(*sink.bytes, 2, {control->x, control->y, to->x, to->y});
			sink.current = *to;
			return 0;
		};
		functions.cubic_to = [](const FT_Vector *first, const FT_Vector *second, const FT_Vector *to, void *user) {
			auto &sink = *static_cast<PathSink *>(user);
			emit(*sink.bytes, 3, {first->x, first->y, second->x, second->y, to->x, to->y});
			sink.current = *to;
			return 0;
		};
		PathSink sink{&bytes, {0, 0}};
		FT_Outline_Decompose(&font->face->glyph->outline, &functions, &sink);
		if (!bytes.empty())
			emit(bytes, 4, {});
		if (call[2] && call.u32(3) >= bytes.size() && !bytes.empty())
			call.write(call[2], bytes);
		call.ret(uint32_t(bytes.size()));
	});
	registry.add("RavenFontTable", 5, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		const auto *font = font_of(call.u32(0));
		if (!font) {
			call.ret(0xffffffffU);
			return;
		}
		FT_ULong length = 0;
		if (FT_Load_Sfnt_Table(font->face, call.u32(1), 0, nullptr, &length)) {
			call.ret(0xffffffffU);
			return;
		}
		if (call[3] && call.u32(4) >= length) {
			std::vector<uint8_t> table(length);
			FT_ULong size = length;
			if (!FT_Load_Sfnt_Table(font->face, call.u32(1), FT_Long(call.u32(2)), table.data(), &size))
				call.write(call[3], table);
		}
		call.ret(uint32_t(length));
	});
	// A name from the name table in English, as UTF-8.
	registry.add("RavenFontName", 4, [](ApiCall &call) {
		std::lock_guard lock(font_mutex);
		const auto *font = font_of(call.u32(0));
		if (!font) {
			call.ret(0);
			return;
		}
		std::string result = sfnt_name(font->face, call.u32(1));
		if (call[2] && call.u32(3) > result.size()) {
			result.push_back('\0');
			call.write(call[2], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(result.data()), result.size()));
			result.pop_back();
		}
		call.ret(uint32_t(result.size()));
	});
	// The component reports what it cannot do here; the text goes to the trace only.
	registry.add("RavenTrace", 1, [](ApiCall &call) {
		if (std::getenv("RAVEN_TRACE_DWRITE"))
			std::fprintf(stderr, "%s\n", call.narrow(call[0], 512).c_str());
	});
}

#else

void register_font_service(ApiRegistry &, KernelContext &)
{
}

std::vector<InstalledFamily> installed_font_families()
{
	return {};
}

#endif

}
