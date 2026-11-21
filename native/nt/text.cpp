#include "text.h"

#include "nls.h"

#include <clocale>
#include <cstdint>
#include <cwctype>

namespace raven {

std::string to_utf8(std::u16string_view text)
{
	std::string result;
	result.reserve(text.size());
	for (size_t at = 0; at < text.size(); ++at) {
		uint32_t code = text[at];
		if (code >= 0xd800 && code < 0xdc00 && at + 1 < text.size() && text[at + 1] >= 0xdc00 && text[at + 1] < 0xe000) {
			code = 0x10000 + ((code - 0xd800) << 10) + (text[at + 1] - 0xdc00);
			++at;
		} else if (code >= 0xd800 && code < 0xe000) {
			code = 0xfffd;
		}
		if (code < 0x80) {
			result.push_back(char(code));
		} else if (code < 0x800) {
			result.push_back(char(0xc0 | code >> 6));
			result.push_back(char(0x80 | (code & 0x3f)));
		} else if (code < 0x10000) {
			result.push_back(char(0xe0 | code >> 12));
			result.push_back(char(0x80 | ((code >> 6) & 0x3f)));
			result.push_back(char(0x80 | (code & 0x3f)));
		} else {
			result.push_back(char(0xf0 | code >> 18));
			result.push_back(char(0x80 | ((code >> 12) & 0x3f)));
			result.push_back(char(0x80 | ((code >> 6) & 0x3f)));
			result.push_back(char(0x80 | (code & 0x3f)));
		}
	}
	return result;
}

std::u16string from_utf8(std::string_view text)
{
	std::u16string result;
	result.reserve(text.size());
	for (size_t at = 0; at < text.size();) {
		const auto lead = static_cast<unsigned char>(text[at]);
		const size_t length = lead < 0x80 ? 1 : lead >= 0xf0 && lead < 0xf5 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc2 && lead < 0xe0 ? 2 : 0;
		uint32_t code = length == 1 ? lead : length == 2 ? lead & 0x1f : length == 3 ? lead & 0x0f : lead & 0x07;
		bool valid = length && at + length <= text.size() && lead < 0xf5;
		for (size_t extra = 1; valid && extra < length; ++extra) {
			const auto next = static_cast<unsigned char>(text[at + extra]);
			if ((next & 0xc0) != 0x80)
				valid = false;
			code = code << 6 | (next & 0x3f);
		}
		if (valid && ((length == 3 && (code < 0x800 || (code >= 0xd800 && code < 0xe000))) || (length == 4 && (code < 0x10000 || code > 0x10ffff))))
			valid = false;
		if (!valid) {
			result.push_back(0xfffd);
			++at;
			continue;
		}
		at += length;
		if (code >= 0x10000) {
			code -= 0x10000;
			result.push_back(char16_t(0xd800 + (code >> 10)));
			result.push_back(char16_t(0xdc00 + (code & 0x3ff)));
		} else {
			result.push_back(char16_t(code));
		}
	}
	return result;
}

static void use_unicode_locale()
{
	static const bool ready = std::setlocale(LC_CTYPE, "C.UTF-8") != nullptr;
	(void)ready;
}

char16_t fold_case(char16_t character)
{
	if (character < 0x80)
		return character >= u'A' && character <= u'Z' ? char16_t(character + 32) : character;
	if (character >= 0xd800 && character < 0xe000)
		return character;
	use_unicode_locale();
	return char16_t(std::towlower(character));
}

char16_t upper_case(char16_t character)
{
	if (character < 0x80)
		return character >= u'a' && character <= u'z' ? char16_t(character - 32) : character;
	if (character >= 0xd800 && character < 0xe000)
		return character;
	use_unicode_locale();
	return char16_t(std::towupper(character));
}

int compare_fold(std::u16string_view left, std::u16string_view right)
{
	const size_t common = left.size() < right.size() ? left.size() : right.size();
	for (size_t at = 0; at < common; ++at) {
		const auto a = upper_case(left[at]), b = upper_case(right[at]);
		if (a != b)
			return a < b ? -1 : 1;
	}
	return left.size() == right.size() ? 0 : left.size() < right.size() ? -1 : 1;
}

bool equal_fold(std::u16string_view left, std::u16string_view right)
{
	return compare_fold(left, right) == 0;
}

std::u16string from_ansi(std::string_view text, uint32_t code_page)
{
	if (code_page == 65001)
		return from_utf8(text);
	if (const auto *page = native_single_byte_page(code_page))
		return decode_single_byte(*page, text);
	// Windows-1252 differs from Latin-1 only in 0x80..0x9f.
	static constexpr char16_t high[32] = {0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152,
		0x8d, 0x17d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d, 0x17e, 0x178};
	std::u16string result;
	for (const char byte : text) {
		const auto value = static_cast<unsigned char>(byte);
		result.push_back(code_page == 1252 && value >= 0x80 && value < 0xa0 ? high[value - 0x80] : char16_t(value));
	}
	return result;
}

std::u16string widen_ascii(std::string_view text)
{
	std::u16string result;
	for (const char character : text)
		result.push_back(char16_t(static_cast<unsigned char>(character)));
	return result;
}


std::string to_ansi(std::u16string_view text, uint32_t code_page)
{
	// Windows-1252 has a table of its own beside the other single-byte pages, best fits included.
	if (native_single_byte_page(code_page) || code_page == 1252)
		return encode_ansi(text, code_page);
	std::string result;
	result.reserve(text.size());
	for (const auto character : text) {
		// Characters below 0x100 keep their Latin-1 value, which agrees with the Windows code pages except for 0x80..0x9f.
		if (character < 0x80 || (character >= 0xa0 && character < 0x100))
			result.push_back(char(character));
		else
			result.push_back('?');
	}
	return result;
}

}
