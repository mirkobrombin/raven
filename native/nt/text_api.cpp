#include "kernel_context.h"
#include "nls.h"
#include "text.h"
#include "pe/resources.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <functional>
#include <map>

namespace raven {

namespace {

constexpr uint32_t locale_return_number = 0x20000000;

// Locale names and their identifiers, as the public locale identifier reference lists them: the languages programs ship translations
// for, and the neutral names of the most common. Formats and texts stay US English whatever locale a program names.
constexpr std::pair<std::u16string_view, uint32_t> known_locales[]{{u"ar", 0x01}, {u"de", 0x07}, {u"en", 0x09}, {u"es", 0x0a}, {u"fr", 0x0c},
	{u"it", 0x10}, {u"ja", 0x11}, {u"ko", 0x12}, {u"nl", 0x13}, {u"pl", 0x15}, {u"pt", 0x16}, {u"ru", 0x19}, {u"sv", 0x1d}, {u"tr", 0x1f},
	{u"zh-Hans", 0x04}, {u"zh-Hant", 0x7c04}, {u"ar-SA", 0x401}, {u"bg-BG", 0x402}, {u"ca-ES", 0x403}, {u"zh-TW", 0x404}, {u"cs-CZ", 0x405},
	{u"da-DK", 0x406}, {u"de-DE", 0x407}, {u"el-GR", 0x408}, {u"en-US", 0x409}, {u"fi-FI", 0x40b}, {u"fr-FR", 0x40c}, {u"he-IL", 0x40d},
	{u"hu-HU", 0x40e}, {u"is-IS", 0x40f}, {u"it-IT", 0x410}, {u"ja-JP", 0x411}, {u"ko-KR", 0x412}, {u"nl-NL", 0x413}, {u"nb-NO", 0x414},
	{u"pl-PL", 0x415}, {u"pt-BR", 0x416}, {u"ro-RO", 0x418}, {u"ru-RU", 0x419}, {u"hr-HR", 0x41a}, {u"sk-SK", 0x41b}, {u"sq-AL", 0x41c},
	{u"sv-SE", 0x41d}, {u"th-TH", 0x41e}, {u"tr-TR", 0x41f}, {u"ur-PK", 0x420}, {u"id-ID", 0x421}, {u"uk-UA", 0x422}, {u"be-BY", 0x423},
	{u"sl-SI", 0x424}, {u"et-EE", 0x425}, {u"lv-LV", 0x426}, {u"lt-LT", 0x427}, {u"fa-IR", 0x429}, {u"vi-VN", 0x42a}, {u"hy-AM", 0x42b},
	{u"eu-ES", 0x42d}, {u"mk-MK", 0x42f}, {u"af-ZA", 0x436}, {u"ka-GE", 0x437}, {u"hi-IN", 0x439}, {u"ms-MY", 0x43e}, {u"kk-KZ", 0x43f},
	{u"sw-KE", 0x441}, {u"bn-IN", 0x445}, {u"pa-IN", 0x446}, {u"gu-IN", 0x447}, {u"ta-IN", 0x449}, {u"te-IN", 0x44a}, {u"kn-IN", 0x44b},
	{u"ml-IN", 0x44c}, {u"mr-IN", 0x44e}, {u"gl-ES", 0x456}, {u"rm-CH", 0x417}, {u"az-Latn-AZ", 0x42c}, {u"hsb-DE", 0x42e},
	{u"tn-ZA", 0x432}, {u"xh-ZA", 0x434}, {u"zu-ZA", 0x435}, {u"fo-FO", 0x438}, {u"mt-MT", 0x43a}, {u"se-NO", 0x43b}, {u"ky-KG", 0x440},
	{u"tk-TM", 0x442}, {u"uz-Latn-UZ", 0x443}, {u"tt-RU", 0x444}, {u"or-IN", 0x448}, {u"as-IN", 0x44d}, {u"sa-IN", 0x44f},
	{u"mn-MN", 0x450}, {u"bo-CN", 0x451}, {u"cy-GB", 0x452}, {u"km-KH", 0x453}, {u"lo-LA", 0x454}, {u"my-MM", 0x455}, {u"kok-IN", 0x457},
	{u"sd-Deva-IN", 0x459}, {u"syr-SY", 0x45a}, {u"si-LK", 0x45b}, {u"am-ET", 0x45e}, {u"ne-NP", 0x461}, {u"fy-NL", 0x462},
	{u"ps-AF", 0x463}, {u"dv-MV", 0x465}, {u"yo-NG", 0x46a}, {u"ba-RU", 0x46d}, {u"lb-LU", 0x46e}, {u"kl-GL", 0x46f}, {u"ig-NG", 0x470},
	{u"ii-CN", 0x478}, {u"br-FR", 0x47e}, {u"ug-CN", 0x480}, {u"mi-NZ", 0x481}, {u"oc-FR", 0x482}, {u"co-FR", 0x483}, {u"sah-RU", 0x485},
	{u"rw-RW", 0x487}, {u"wo-SN", 0x488}, {u"prs-AF", 0x48c}, {u"ku-Arab-IQ", 0x492}, {u"ar-IQ", 0x801}, {u"zh-CN", 0x804},
	{u"bn-BD", 0x845}, {u"pa-Arab-PK", 0x846}, {u"sd-Arab-PK", 0x859}, {u"ar-EG", 0xc01}, {u"ar-LY", 0x1001}, {u"zh-SG", 0x1004},
	{u"ar-DZ", 0x1401}, {u"zh-MO", 0x1404}, {u"ar-MA", 0x1801}, {u"ar-TN", 0x1c01}, {u"ar-OM", 0x2001}, {u"ar-YE", 0x2401},
	{u"ar-SY", 0x2801}, {u"ar-JO", 0x2c01}, {u"ar-LB", 0x3001}, {u"ar-KW", 0x3401}, {u"ar-AE", 0x3801}, {u"ar-BH", 0x3c01},
	{u"ar-QA", 0x4001}, {u"de-CH", 0x807}, {u"en-GB", 0x809}, {u"es-MX", 0x80a},
	{u"fr-BE", 0x80c}, {u"it-CH", 0x810}, {u"nl-BE", 0x813}, {u"nn-NO", 0x814}, {u"pt-PT", 0x816}, {u"zh-HK", 0xc04}, {u"de-AT", 0xc07},
	{u"en-AU", 0xc09}, {u"es-ES", 0xc0a}, {u"fr-CA", 0xc0c}, {u"en-CA", 0x1009}, {u"fr-CH", 0x100c}, {u"en-IE", 0x1809}, {u"sr-Latn-RS", 0x241a},
	{u"sr-Cyrl-RS", 0x281a}, {u"en-IN", 0x4009}, {u"es-US", 0x540a}};

const std::map<uint32_t, std::u16string> &locale_strings()
{
	static const std::map<uint32_t, std::u16string> table = [] {
		std::map<uint32_t, std::u16string> t{
			{0x1, u"0409"}, {0x2, u"English (United States)"}, {0x3, u"English (United States)"}, {0x4, u"English"}, {0x5, u"United States"},
			{0x6, u"United States"}, {0x7, u"English"}, {0x8, u"USA"}, {0x9, u"0409"}, {0xa, u"1"}, {0xb, u"437"},
			{0xc, u","}, {0xd, u"0"}, {0xe, u"."}, {0xf, u","}, {0x10, u"3;0"}, {0x11, u"1"}, {0x12, u"2"}, {0x13, u"1"},
			{0x14, u"$"}, {0x15, u"USD"}, {0x16, u"US Dollar"}, {0x17, u"."}, {0x18, u","}, {0x19, u"3;0"}, {0x1a, u"2"},
			{0x1b, u"0"}, {0x1c, u"0"}, {0x1d, u"/"}, {0x1e, u":"}, {0x1f, u"M/d/yyyy"}, {0x20, u"dddd, MMMM d, yyyy"},
			{0x21, u"0"}, {0x22, u"1"}, {0x23, u"0"}, {0x24, u"0"}, {0x25, u"0"}, {0x26, u"1"}, {0x27, u"1"}, {0x28, u"AM"},
			{0x29, u"PM"}, {0x1001, u"English"}, {0x1002, u"United States"}, {0x1003, u"h:mm:ss tt"}, {0x1004, u"1252"},
			{0x1005, u"437"}, {0x1006, u"437"}, {0x1007, u"1"}, {0x1008, u"1"}, {0x1009, u"1"}, {0x100b, u"0"}, {0x100c, u"6"},
			{0x100d, u"0"}, {0x100a, u"1"}, {0x1010, u"1"}, {0x1011, u"10000"}, {0x1012, u"037"}, {0x1014, u"1"}, {0x59, u"en"}, {0x5a, u"US"}, {0x5b, u"ENU"}, {0x5c, u"en-US"}, {0x5d, u"USA"}, {0x66, u"en-US"},
			{0x6e, u"."}, {0x74, u"0"}, {0x75, u"0"}, {0x1000, u"0409"}, {0x4a, u"+"}, {0x4b, u"-"}, {0x50, u""}, {0x51, u"-"},
			{0x52, u"3"}, {0x53, u"0"}, {0x54, u"1"}, {0x55, u"0"}, {0x56, u"1"}, {0x57, u"0"},
		};
		static constexpr const char16_t *days[] = {u"Monday", u"Tuesday", u"Wednesday", u"Thursday", u"Friday", u"Saturday", u"Sunday"};
		static constexpr const char16_t *months[] = {u"January", u"February", u"March", u"April", u"May", u"June", u"July", u"August",
			u"September", u"October", u"November", u"December"};
		for (uint32_t index = 0; index < 7; ++index) {
			t[0x2a + index] = days[index];
			t[0x31 + index] = std::u16string(days[index]).substr(0, 3);
		}
		for (uint32_t index = 0; index < 12; ++index) {
			t[0x38 + index] = months[index];
			t[0x44 + index] = std::u16string(months[index]).substr(0, 3);
		}
		// The fields Windows Vista and later added, which the .NET runtime reads for its cultures: not-a-number and the infinities, the
		// scripts, the parent culture, reading order, percent signs, the month-day and short time patterns, the OpenType tag and the sort.
		for (const auto &[type, text] : std::initializer_list<std::pair<uint32_t, const char16_t *>>{{0x69, u"NaN"}, {0x6a, u"\u221e"},
			{0x6b, u"-\u221e"}, {0x6c, u"Latn;"}, {0x6d, u"en"}, {0x70, u"0"}, {0x71, u"0"}, {0x76, u"%"}, {0x77, u"\u2030"},
			{0x78, u"MMMM d"}, {0x79, u"h:mm tt"}, {0x7a, u"ENG "}, {0x7b, u"en-US"}})
			t.emplace(type, text);
		return t;
	}();
	return table;
}

std::optional<std::u16string> locale_text(uint32_t type)
{
	type &= ~locale_return_number & ~0x80000000U;
	const auto found = locale_strings().find(type);
	if (found == locale_strings().end())
		return std::nullopt;
	return found->second;
}

uint32_t as_number(const std::u16string &text)
{
	uint32_t value = 0;
	for (const auto character : text)
		if (character >= u'0' && character <= u'9')
			value = value * 10 + (character - u'0');
		else if (character == u';')
			break;
	return value;
}

// Reads a counted or zero-terminated UTF-16 argument; a count of -1 means "until the terminator".
std::u16string counted_wide(ApiCall &call, uint64_t address, int32_t count)
{
	if (count < 0)
		return call.wide(address);
	const auto bytes = call.read(address, size_t(count) * 2);
	std::u16string text(size_t(count), u'\0');
	std::memcpy(text.data(), bytes.data(), bytes.size());
	return text;
}

// The Latin letters with one combining mark, from the Unicode character database: the letter, its base and the mark.
constexpr std::array<std::array<char16_t, 3>, 161> latin_marks{{
	{0x00c0, 0x0041, 0x0300}, {0x00c1, 0x0041, 0x0301}, {0x00c2, 0x0041, 0x0302}, {0x00c3, 0x0041, 0x0303}, {0x00c4, 0x0041, 0x0308}, {0x00c5, 0x0041, 0x030a}, {0x00c7, 0x0043, 0x0327}, {0x00c8, 0x0045, 0x0300},
	{0x00c9, 0x0045, 0x0301}, {0x00ca, 0x0045, 0x0302}, {0x00cb, 0x0045, 0x0308}, {0x00cc, 0x0049, 0x0300}, {0x00cd, 0x0049, 0x0301}, {0x00ce, 0x0049, 0x0302}, {0x00cf, 0x0049, 0x0308}, {0x00d1, 0x004e, 0x0303},
	{0x00d2, 0x004f, 0x0300}, {0x00d3, 0x004f, 0x0301}, {0x00d4, 0x004f, 0x0302}, {0x00d5, 0x004f, 0x0303}, {0x00d6, 0x004f, 0x0308}, {0x00d9, 0x0055, 0x0300}, {0x00da, 0x0055, 0x0301}, {0x00db, 0x0055, 0x0302},
	{0x00dc, 0x0055, 0x0308}, {0x00dd, 0x0059, 0x0301}, {0x00e0, 0x0061, 0x0300}, {0x00e1, 0x0061, 0x0301}, {0x00e2, 0x0061, 0x0302}, {0x00e3, 0x0061, 0x0303}, {0x00e4, 0x0061, 0x0308}, {0x00e5, 0x0061, 0x030a},
	{0x00e7, 0x0063, 0x0327}, {0x00e8, 0x0065, 0x0300}, {0x00e9, 0x0065, 0x0301}, {0x00ea, 0x0065, 0x0302}, {0x00eb, 0x0065, 0x0308}, {0x00ec, 0x0069, 0x0300}, {0x00ed, 0x0069, 0x0301}, {0x00ee, 0x0069, 0x0302},
	{0x00ef, 0x0069, 0x0308}, {0x00f1, 0x006e, 0x0303}, {0x00f2, 0x006f, 0x0300}, {0x00f3, 0x006f, 0x0301}, {0x00f4, 0x006f, 0x0302}, {0x00f5, 0x006f, 0x0303}, {0x00f6, 0x006f, 0x0308}, {0x00f9, 0x0075, 0x0300},
	{0x00fa, 0x0075, 0x0301}, {0x00fb, 0x0075, 0x0302}, {0x00fc, 0x0075, 0x0308}, {0x00fd, 0x0079, 0x0301}, {0x00ff, 0x0079, 0x0308}, {0x0100, 0x0041, 0x0304}, {0x0101, 0x0061, 0x0304}, {0x0102, 0x0041, 0x0306},
	{0x0103, 0x0061, 0x0306}, {0x0104, 0x0041, 0x0328}, {0x0105, 0x0061, 0x0328}, {0x0106, 0x0043, 0x0301}, {0x0107, 0x0063, 0x0301}, {0x0108, 0x0043, 0x0302}, {0x0109, 0x0063, 0x0302}, {0x010a, 0x0043, 0x0307},
	{0x010b, 0x0063, 0x0307}, {0x010c, 0x0043, 0x030c}, {0x010d, 0x0063, 0x030c}, {0x010e, 0x0044, 0x030c}, {0x010f, 0x0064, 0x030c}, {0x0112, 0x0045, 0x0304}, {0x0113, 0x0065, 0x0304}, {0x0114, 0x0045, 0x0306},
	{0x0115, 0x0065, 0x0306}, {0x0116, 0x0045, 0x0307}, {0x0117, 0x0065, 0x0307}, {0x0118, 0x0045, 0x0328}, {0x0119, 0x0065, 0x0328}, {0x011a, 0x0045, 0x030c}, {0x011b, 0x0065, 0x030c}, {0x011c, 0x0047, 0x0302},
	{0x011d, 0x0067, 0x0302}, {0x011e, 0x0047, 0x0306}, {0x011f, 0x0067, 0x0306}, {0x0120, 0x0047, 0x0307}, {0x0121, 0x0067, 0x0307}, {0x0122, 0x0047, 0x0327}, {0x0123, 0x0067, 0x0327}, {0x0124, 0x0048, 0x0302},
	{0x0125, 0x0068, 0x0302}, {0x0128, 0x0049, 0x0303}, {0x0129, 0x0069, 0x0303}, {0x012a, 0x0049, 0x0304}, {0x012b, 0x0069, 0x0304}, {0x012c, 0x0049, 0x0306}, {0x012d, 0x0069, 0x0306}, {0x012e, 0x0049, 0x0328},
	{0x012f, 0x0069, 0x0328}, {0x0130, 0x0049, 0x0307}, {0x0134, 0x004a, 0x0302}, {0x0135, 0x006a, 0x0302}, {0x0136, 0x004b, 0x0327}, {0x0137, 0x006b, 0x0327}, {0x0139, 0x004c, 0x0301}, {0x013a, 0x006c, 0x0301},
	{0x013b, 0x004c, 0x0327}, {0x013c, 0x006c, 0x0327}, {0x013d, 0x004c, 0x030c}, {0x013e, 0x006c, 0x030c}, {0x0143, 0x004e, 0x0301}, {0x0144, 0x006e, 0x0301}, {0x0145, 0x004e, 0x0327}, {0x0146, 0x006e, 0x0327},
	{0x0147, 0x004e, 0x030c}, {0x0148, 0x006e, 0x030c}, {0x014c, 0x004f, 0x0304}, {0x014d, 0x006f, 0x0304}, {0x014e, 0x004f, 0x0306}, {0x014f, 0x006f, 0x0306}, {0x0150, 0x004f, 0x030b}, {0x0151, 0x006f, 0x030b},
	{0x0154, 0x0052, 0x0301}, {0x0155, 0x0072, 0x0301}, {0x0156, 0x0052, 0x0327}, {0x0157, 0x0072, 0x0327}, {0x0158, 0x0052, 0x030c}, {0x0159, 0x0072, 0x030c}, {0x015a, 0x0053, 0x0301}, {0x015b, 0x0073, 0x0301},
	{0x015c, 0x0053, 0x0302}, {0x015d, 0x0073, 0x0302}, {0x015e, 0x0053, 0x0327}, {0x015f, 0x0073, 0x0327}, {0x0160, 0x0053, 0x030c}, {0x0161, 0x0073, 0x030c}, {0x0162, 0x0054, 0x0327}, {0x0163, 0x0074, 0x0327},
	{0x0164, 0x0054, 0x030c}, {0x0165, 0x0074, 0x030c}, {0x0168, 0x0055, 0x0303}, {0x0169, 0x0075, 0x0303}, {0x016a, 0x0055, 0x0304}, {0x016b, 0x0075, 0x0304}, {0x016c, 0x0055, 0x0306}, {0x016d, 0x0075, 0x0306},
	{0x016e, 0x0055, 0x030a}, {0x016f, 0x0075, 0x030a}, {0x0170, 0x0055, 0x030b}, {0x0171, 0x0075, 0x030b}, {0x0172, 0x0055, 0x0328}, {0x0173, 0x0075, 0x0328}, {0x0174, 0x0057, 0x0302}, {0x0175, 0x0077, 0x0302},
	{0x0176, 0x0059, 0x0302}, {0x0177, 0x0079, 0x0302}, {0x0178, 0x0059, 0x0308}, {0x0179, 0x005a, 0x0301}, {0x017a, 0x007a, 0x0301}, {0x017b, 0x005a, 0x0307}, {0x017c, 0x007a, 0x0307}, {0x017d, 0x005a, 0x030c},
	{0x017e, 0x007a, 0x030c},
}};

// FoldStringW's mappings, applied in turn to each character: the compatibility zone, digits, ligatures, then composition.
std::u16string fold_string(std::u16string_view text, uint32_t flags)
{
	constexpr uint32_t fold_zone = 0x10, precomposed = 0x20, composite = 0x40, fold_digits = 0x80, expand_ligatures = 0x2000;
	// Each block of decimal digits starts at its zero.
	static constexpr char16_t digit_zeros[]{0x0660, 0x06f0, 0x07c0, 0x0966, 0x09e6, 0x0a66, 0x0ae6, 0x0b66, 0x0be6, 0x0c66, 0x0ce6, 0x0d66,
		0x0de6, 0x0e50, 0x0ed0, 0x0f20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946, 0x19d0, 0x2080, 0xff10};
	std::u16string folded;
	for (char16_t character : text) {
		if (flags & fold_zone) {
			if (character >= 0xff01 && character <= 0xff5e) character = char16_t(character - 0xfee0);
			else if (character == 0x3000) character = u' ';
		}
		if (flags & fold_digits) {
			for (const auto zero : digit_zeros)
				if (character >= zero && character < zero + 10) character = char16_t(u'0' + (character - zero));
			if (character == 0x00b9) character = u'1';
			else if (character == 0x00b2 || character == 0x00b3) character = char16_t(u'2' + (character - 0x00b2));
			else if (character == 0x2070) character = u'0';
			else if (character >= 0x2074 && character <= 0x2079) character = char16_t(u'4' + (character - 0x2074));
		}
		if (flags & expand_ligatures) {
			static constexpr std::pair<char16_t, std::u16string_view> ligatures[]{{0x00c6, u"AE"}, {0x00e6, u"ae"}, {0x0152, u"OE"}, {0x0153, u"oe"},
				{0x00df, u"ss"}, {0x0132, u"IJ"}, {0x0133, u"ij"}, {0xfb00, u"ff"}, {0xfb01, u"fi"}, {0xfb02, u"fl"}, {0xfb03, u"ffi"}, {0xfb04, u"ffl"},
				{0xfb05, u"st"}, {0xfb06, u"st"}};
			const auto found = std::find_if(std::begin(ligatures), std::end(ligatures), [character](const auto &item) { return item.first == character; });
			if (found != std::end(ligatures)) {
				folded += found->second;
				continue;
			}
		}
		if (flags & composite) {
			const auto found = std::find_if(latin_marks.begin(), latin_marks.end(), [character](const auto &item) { return item[0] == character; });
			if (found != latin_marks.end()) {
				folded += (*found)[1];
				folded += (*found)[2];
				continue;
			}
		}
		if ((flags & precomposed) && !folded.empty()) {
			const char16_t base = folded.back();
			const auto found = std::find_if(latin_marks.begin(), latin_marks.end(), [base, character](const auto &item) {
				return item[1] == base && item[2] == character;
			});
			if (found != latin_marks.end()) {
				folded.back() = (*found)[0];
				continue;
			}
		}
		folded += character;
	}
	return folded;
}

std::string counted_narrow(ApiCall &call, uint64_t address, int32_t count)
{
	if (count < 0)
		return call.narrow(address);
	const auto bytes = call.read(address, size_t(count));
	return std::string(bytes.begin(), bytes.end());
}

std::u16string decode_ansi(std::string_view text, uint32_t code_page)
{
	if (code_page == 65001)
		return from_utf8(text);
	if (const auto *page = native_single_byte_page(code_page))
		return decode_single_byte(*page, text);
	std::u16string result;
	for (const auto byte : text) {
		const auto value = static_cast<unsigned char>(byte);
		if (value >= 0x80 && value < 0xa0 && code_page == 1252) {
			static constexpr char16_t high[32] = {0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152,
				0x8d, 0x17d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d, 0x17e, 0x178};
			result.push_back(high[value - 0x80]);
		} else {
			result.push_back(char16_t(value));
		}
	}
	return result;
}

int collate(std::u16string_view left, std::u16string_view right, uint32_t flags)
{
	const bool ignore_case = flags & (0x1 | 0x10000000 | 0x10);
	const int folded = compare_fold(left, right);
	if (folded || ignore_case)
		return folded;
	for (size_t at = 0; at < std::min(left.size(), right.size()); ++at)
		if (left[at] != right[at])
			return fold_case(left[at]) == left[at] ? -1 : 1;
	return 0;
}

// Windows date and time pictures: d dd ddd dddd M MM MMM MMMM y yy yyyy and quoted literals; h hh H HH m mm s ss t tt.
std::u16string picture(std::u16string_view format, const uint16_t *time, bool date)
{
	static const std::u16string_view days[] = {u"Sunday", u"Monday", u"Tuesday", u"Wednesday", u"Thursday", u"Friday", u"Saturday"};
	static const std::u16string_view months[] = {u"January", u"February", u"March", u"April", u"May", u"June", u"July", u"August", u"September",
		u"October", u"November", u"December"};
	std::u16string result;
	const auto two = [](unsigned value) { return std::u16string{char16_t(u'0' + value / 10 % 10), char16_t(u'0' + value % 10)}; };
	for (size_t at = 0; at < format.size();) {
		const char16_t letter = format[at];
		size_t run = 1;
		while (at + run < format.size() && format[at + run] == letter)
			++run;
		if (letter == u'\'') {
			size_t end = at + 1;
			while (end < format.size()) {
				if (format[end] == u'\'' && end + 1 < format.size() && format[end + 1] == u'\'')
					end += 2;
				else if (format[end] == u'\'')
					break;
				else
					result.push_back(format[end++]);
			}
			at = end + 1;
			continue;
		}
		if (date && letter == u'd') {
			if (run >= 4) result += days[time[2] % 7];
			else if (run == 3) result += days[time[2] % 7].substr(0, 3);
			else if (run == 2) result += two(time[3]);
			else result += widen_ascii(std::to_string(time[3]));
		} else if (date && letter == u'M') {
			if (run >= 4) result += months[(time[1] + 11) % 12];
			else if (run == 3) result += months[(time[1] + 11) % 12].substr(0, 3);
			else if (run == 2) result += two(time[1]);
			else result += widen_ascii(std::to_string(time[1]));
		} else if (date && letter == u'y') {
			if (run >= 3) result += widen_ascii(std::to_string(time[0]));
			else if (run == 2) result += two(time[0] % 100);
			else result += widen_ascii(std::to_string(time[0] % 100));
		} else if (!date && (letter == u'h' || letter == u'H')) {
			unsigned hour = time[4];
			if (letter == u'h') {
				hour %= 12;
				if (!hour) hour = 12;
			}
			result += run >= 2 ? two(hour) : widen_ascii(std::to_string(hour));
		} else if (!date && letter == u'm') {
			result += run >= 2 ? two(time[5]) : widen_ascii(std::to_string(time[5]));
		} else if (!date && letter == u's') {
			result += run >= 2 ? two(time[6]) : widen_ascii(std::to_string(time[6]));
		} else if (!date && letter == u't') {
			const std::u16string marker = time[4] < 12 ? u"AM" : u"PM";
			result += run >= 2 ? marker : marker.substr(0, 1);
		} else {
			result.append(run, letter);
		}
		at += run;
	}
	return result;
}

std::optional<std::u16string> system_message(uint32_t id)
{
	// Standard English wording of the system message table; every entry ends with the CR LF Windows appends.
	static const std::map<uint32_t, const char16_t *> messages{
		{0, u"The operation completed successfully."}, {1, u"Incorrect function."}, {2, u"The system cannot find the file specified."},
		{3, u"The system cannot find the path specified."}, {4, u"The system cannot open the file."}, {5, u"Access is denied."},
		{6, u"The handle is invalid."}, {7, u"The storage control blocks were destroyed."}, {8, u"Not enough memory resources are available to process this command."},
		{9, u"The storage control block address is invalid."}, {10, u"The environment is incorrect."}, {11, u"An attempt was made to load a program with an incorrect format."},
		{12, u"The access code is invalid."}, {13, u"The data is invalid."}, {14, u"Not enough memory resources are available to complete this operation."},
		{15, u"The system cannot find the drive specified."}, {16, u"The directory cannot be removed."}, {17, u"The system cannot move the file to a different disk drive."},
		{18, u"There are no more files."}, {19, u"The media is write protected."}, {20, u"The system cannot find the device specified."},
		{21, u"The device is not ready."}, {22, u"The device does not recognize the command."}, {23, u"Data error (cyclic redundancy check)."},
		{24, u"The program issued a command but the command length is incorrect."}, {25, u"The drive cannot locate a specific area or track on the disk."},
		{26, u"The specified disk or diskette cannot be accessed."}, {27, u"The drive cannot find the sector requested."}, {28, u"The printer is out of paper."},
		{29, u"The system cannot write to the specified device."}, {30, u"The system cannot read from the specified device."}, {31, u"A device attached to the system is not functioning."},
		{32, u"The process cannot access the file because it is being used by another process."},
		{33, u"The process cannot access the file because another process has locked a portion of the file."},
		{36, u"Too many files opened for sharing."}, {38, u"Reached the end of the file."}, {39, u"The disk is full."},
		{50, u"The request is not supported."}, {51, u"Windows cannot find the network path. Verify that the network path is correct and the destination computer is not busy or turned off."},
		{53, u"The network path was not found."}, {54, u"The network is busy."}, {55, u"The specified network resource or device is no longer available."},
		{65, u"Network access is denied."}, {67, u"The network name cannot be found."}, {80, u"The file exists."}, {82, u"The directory or file cannot be created."},
		{86, u"The specified network password is not correct."}, {87, u"The parameter is incorrect."}, {88, u"A write fault occurred on the network."},
		{109, u"The pipe has been ended."}, {110, u"The system cannot open the device or file specified."}, {111, u"The file name is too long."},
		{112, u"There is not enough space on the disk."}, {120, u"This function is not supported on this system."}, {122, u"The data area passed to a system call is too small."},
		{123, u"The filename, directory name, or volume label syntax is incorrect."}, {126, u"The specified module could not be found."},
		{127, u"The specified procedure could not be found."}, {129, u"%1 is not a valid Win32 application."}, {131, u"An attempt was made to move the file pointer before the beginning of the file."},
		{145, u"The directory is not empty."}, {161, u"The specified path is invalid."}, {170, u"The requested resource is in use."}, {175, u"The specified device does not exist."},
		{183, u"Cannot create a file when that file already exists."}, {193, u"%1 is not a valid Win32 application."}, {203, u"The system could not find the environment option that was entered."},
		{206, u"The filename or extension is too long."}, {234, u"More data is available."}, {258, u"The wait operation timed out."}, {259, u"No more data is available."},
		{267, u"The directory name is invalid."}, {287, u"The specified file is not a reparse point."}, {288, u"Attempt to release mutex not owned by caller."},
		{298, u"Too many posts were made to a semaphore."}, {299, u"Only part of a ReadProcessMemory or WriteProcessMemory request was completed."},
		{317, u"The system cannot find message text for message number 0x%1 in the message file for %2."}, {487, u"Attempt to access invalid address."},
		{534, u"Arithmetic result exceeded 32 bits."}, {535, u"There is a process on other end of the pipe."}, {536, u"Waiting for a process to open the other end of the pipe."},
		{740, u"The requested operation requires elevation."}, {995, u"The I/O operation has been aborted because of either a thread exit or an application request."},
		{996, u"Overlapped I/O event is not in a signaled state."}, {997, u"Overlapped I/O operation is in progress."}, {998, u"Invalid access to memory location."},
		{1004, u"Invalid flags."}, {1008, u"An attempt was made to reference a token that does not exist."}, {1010, u"The configuration registry key is invalid."},
		{1018, u"Illegal operation attempted on a registry key that has been marked for deletion."}, {1060, u"The specified service does not exist as an installed service."},
		{1150, u"The specified program requires a newer version of Windows."}, {1168, u"Element not found."}, {1223, u"The operation was canceled by the user."},
		{1235, u"The request was aborted."}, {1314, u"A required privilege is not held by the client."}, {1400, u"Invalid window handle."}, {1401, u"Invalid menu handle."},
		{1402, u"Invalid cursor handle."}, {1403, u"Invalid accelerator table handle."}, {1404, u"Invalid hook handle."}, {1405, u"Invalid handle to a multiple-window position structure."},
		{1406, u"Cannot create a top-level child window."}, {1407, u"Cannot find window class."}, {1408, u"Invalid window; it belongs to other thread."},
		{1409, u"Hot key is already registered."}, {1410, u"Class already exists."}, {1411, u"Class does not exist."}, {1412, u"Class still has open windows."},
		{1413, u"Invalid index."}, {1414, u"Invalid icon handle."}, {1415, u"Using private DIALOG window words."}, {1416, u"The list box identifier was not found."},
		{1417, u"No wildcards were found."}, {1418, u"Thread does not have a clipboard open."}, {1419, u"Hot key is not registered."}, {1420, u"The window is not a valid dialog window."},
		{1421, u"Control ID not found."}, {1422, u"Invalid message for a combo box because it does not have an edit control."}, {1423, u"The window is not a combo box."},
		{1424, u"Height must be less than 256."}, {1425, u"Invalid device context (DC) handle."}, {1426, u"Invalid hook procedure type."}, {1427, u"Invalid hook procedure."},
		{1428, u"Cannot set nonlocal hook without a module handle."}, {1429, u"This hook procedure can only be set globally."}, {1430, u"The journal hook procedure is already installed."},
		{1431, u"The hook procedure is not installed."}, {1432, u"Invalid message for single-selection list box."}, {1433, u"LB_SETCOUNT sent to non-lazy list box."},
		{1434, u"This list box does not support tab stops."}, {1435, u"Cannot destroy object created by another thread."}, {1436, u"Child windows cannot have menus."},
		{1437, u"The window does not have a system menu."}, {1438, u"Invalid message box style."}, {1439, u"Invalid system-wide (SPI_*) parameter."},
		{1440, u"Screen already locked."}, {1441, u"All handles to windows in a multiple-window position structure must have the same parent."},
		{1442, u"The window is not a child window."}, {1443, u"Invalid GW_* command."}, {1444, u"Invalid thread identifier."},
		{1445, u"Cannot process a message from a window that is not a multiple document interface (MDI) window."}, {1446, u"Popup menu already active."},
		{1447, u"The window does not have scroll bars."}, {1448, u"Scroll bar range cannot be greater than MAXLONG."}, {1449, u"Cannot show or remove the window in the way specified."},
		{1450, u"Insufficient system resources exist to complete the requested service."}, {1460, u"This operation returned because the timeout period expired."},
		{1627, u"Function failed during execution."}, {1629, u"Data of this type is not supported."}, {1813, u"The specified resource type cannot be found in the image file."},
		{1814, u"The specified resource name cannot be found in the image file."}, {1815, u"The specified resource language ID cannot be found in the image file."},
		{1816, u"Not enough quota is available to process this command."}, {2250, u"The network connection does not exist."}, {5023, u"The group or resource is not in the correct state to perform the requested operation."},
		{0x80004001U, u"Not implemented"}, {0x80004002U, u"No such interface supported"}, {0x80004003U, u"Invalid pointer"}, {0x80004004U, u"Operation aborted"},
		{0x80004005U, u"Unspecified error"}, {0x8000ffffU, u"Catastrophic failure"}, {0x8007000eU, u"Not enough memory resources are available to complete this operation."},
		{0x80070005U, u"Access is denied."}, {0x80070006U, u"The handle is invalid."}, {0x80070057U, u"The parameter is incorrect."},
		{0x80040154U, u"Class not registered"}, {0x800401f3U, u"Invalid class string"}, {0x800b0100U, u"No signature was present in the subject."},
		{0x80092004U, u"Cannot find object or property."}, {0x80092009U, u"Cannot find the requested object."}, {0x80091004U, u"Invalid cryptographic message type."},
		{0x80090008U, u"The parameter is incorrect."}, {0x80090016U, u"Keyset does not exist"},
	};
	const auto found = messages.find(id);
	if (found == messages.end())
		return std::nullopt;
	return std::u16string(found->second) + u"\r\n";
}

// Replaces %1..%99 (with optional !printf! formats), %n, %r, %t, %% and %0 in a message template.
// The inserts of a message: %1 to %99, each with an optional printf format between exclamation marks, which `argument` turns into
// text; and the escapes %n, %r, %t, %0 and a doubled character.
std::u16string insert_arguments(std::u16string_view text, const std::function<std::u16string(unsigned, std::u16string_view)> &argument)
{
	std::u16string result;
	for (size_t at = 0; at < text.size(); ++at) {
		if (text[at] != u'%' || at + 1 >= text.size()) {
			result.push_back(text[at]);
			continue;
		}
		const char16_t next = text[at + 1];
		if (next >= u'1' && next <= u'9') {
			size_t end = at + 1;
			unsigned index = 0;
			while (end < text.size() && text[end] >= u'0' && text[end] <= u'9' && index < 100)
				index = index * 10 + (text[end++] - u'0');
			std::u16string_view format;
			if (end < text.size() && text[end] == u'!') {
				const auto close = text.find(u'!', end + 1);
				if (close != text.npos) {
					format = text.substr(end + 1, close - end - 1);
					end = close + 1;
				}
			}
			if (index >= 1 && index <= 99)
				result += argument(index, format);
			at = end - 1;
		} else if (next == u'n') {
			result += u"\r\n";
			++at;
		} else if (next == u'r') {
			result.push_back(u'\r');
			++at;
		} else if (next == u't') {
			result.push_back(u'\t');
			++at;
		} else if (next == u'0') {
			break;
		} else {
			result.push_back(next);
			++at;
		}
	}
	return result;
}

// The text of message `id` in a module's message table, the RT_MESSAGETABLE resource named 1: blocks of consecutive identifiers, each
// pointing at entries that carry their length and whether they are UTF-16. The language asked for is preferred, any other serves.
std::optional<std::u16string> module_message(KernelContext &context, uint64_t base, uint32_t id, uint32_t language)
{
	const PEModule *image = !base || base == context.entry.loaded.base() ? &context.entry : nullptr;
	for (auto found = context.modules.all().begin(); !image && found != context.modules.all().end(); ++found)
		if (found->second->loaded.base() == base)
			image = found->second.get();
	if (!image || image->runtime_api)
		return std::nullopt;
	const auto directory = image->image.directories[2];
	if (!directory.address || !directory.size || directory.size > 16 * 1024 * 1024)
		return std::nullopt;
	const auto resources = read_resources(directory, context.process.read_memory(image->loaded.base() + directory.address, directory.size),
		image->image.image_size);
	const PEResource *table = nullptr;
	for (const auto &resource : resources)
		if (resource.type == ResourceKey(uint32_t(11)) && resource.name == ResourceKey(uint32_t(1)) &&
			(!table || (language && resource.language == language)))
			table = &resource;
	if (!table || table->size < 4)
		return std::nullopt;
	const auto data = context.process.read_memory(image->loaded.base() + table->data_address, table->size);
	const auto word = [&](size_t at) -> uint32_t {
		if (at + 4 > data.size())
			return 0;
		return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 | uint32_t(data[at + 3]) << 24;
	};
	const uint32_t blocks = word(0);
	for (uint32_t block = 0; block < blocks && 4 + size_t(block + 1) * 12 <= data.size(); ++block) {
		const uint32_t low = word(4 + block * 12), high = word(8 + block * 12);
		if (id < low || id > high)
			continue;
		size_t at = word(12 + block * 12);
		for (uint32_t skip = low; skip < id && at + 4 <= data.size(); ++skip)
			at += uint16_t(word(at));
		if (at + 4 > data.size())
			return std::nullopt;
		const size_t length = uint16_t(word(at)), flags = uint16_t(word(at) >> 16);
		if (length < 4 || at + length > data.size())
			return std::nullopt;
		std::u16string text;
		if (flags & 1) {
			for (size_t offset = at + 4; offset + 1 < at + length; offset += 2)
				text.push_back(char16_t(data[offset] | data[offset + 1] << 8));
		} else {
			text = decode_ansi(std::string_view(reinterpret_cast<const char *>(data.data() + at + 4), length - 4), 0);
		}
		while (!text.empty() && !text.back())
			text.pop_back();
		return text;
	}
	return std::nullopt;
}

// Punycode, RFC 3492: the bias adaptation after each code point is written.
uint32_t punycode_adapt(uint32_t delta, uint32_t points, bool first)
{
	delta = first ? delta / 700 : delta / 2;
	delta += delta / points;
	uint32_t k = 0;
	for (; delta > 35 * 26 / 2; k += 36)
		delta /= 35;
	return k + 36 * delta / (delta + 38);
}

std::optional<std::string> punycode_encode(const std::u32string &input)
{
	const auto digit = [](uint32_t value) { return char(value < 26 ? 'a' + value : '0' + value - 26); };
	std::string output;
	for (const auto point : input)
		if (point < 0x80)
			output.push_back(char(point));
	const uint32_t basic = uint32_t(output.size());
	uint32_t handled = basic, n = 0x80, delta = 0, bias = 72;
	if (basic)
		output.push_back('-');
	while (handled < input.size()) {
		uint32_t next = UINT32_MAX;
		for (const auto point : input)
			if (point >= n && point < next)
				next = point;
		if (uint64_t(next - n) * (handled + 1) > UINT32_MAX - delta)
			return std::nullopt;
		delta += (next - n) * (handled + 1);
		n = next;
		for (const auto point : input) {
			if (point < n && !++delta)
				return std::nullopt;
			if (point != n)
				continue;
			uint32_t q = delta;
			for (uint32_t k = 36;; k += 36) {
				const uint32_t t = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
				if (q < t)
					break;
				output.push_back(digit(t + (q - t) % (36 - t)));
				q = (q - t) / (36 - t);
			}
			output.push_back(digit(q));
			bias = punycode_adapt(delta, handled + 1, handled == basic);
			delta = 0;
			++handled;
		}
		++delta;
		++n;
	}
	return output;
}

std::optional<std::u32string> punycode_decode(std::string_view input)
{
	std::u32string output;
	const auto last = input.rfind('-');
	if (last != std::string_view::npos) {
		for (const char character : input.substr(0, last))
			output.push_back(char32_t(uint8_t(character)));
		input.remove_prefix(last + 1);
	}
	uint32_t n = 0x80, i = 0, bias = 72;
	for (size_t at = 0; at < input.size();) {
		const uint32_t old = i;
		for (uint32_t weight = 1, k = 36;; k += 36) {
			if (at >= input.size())
				return std::nullopt;
			const char c = input[at++];
			const uint32_t value = c >= '0' && c <= '9' ? c - '0' + 26 : c >= 'a' && c <= 'z' ? c - 'a' : c >= 'A' && c <= 'Z' ? c - 'A' : 36;
			if (value >= 36 || uint64_t(value) * weight > UINT32_MAX - i)
				return std::nullopt;
			i += value * weight;
			const uint32_t t = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
			if (value < t)
				break;
			if (uint64_t(weight) * (36 - t) > UINT32_MAX)
				return std::nullopt;
			weight *= 36 - t;
		}
		const uint32_t count = uint32_t(output.size()) + 1;
		bias = punycode_adapt(i - old, count, old == 0);
		if (i / count > 0x10ffff - n)
			return std::nullopt;
		n += i / count;
		i %= count;
		output.insert(output.begin() + i, char32_t(n));
		++i;
	}
	return output;
}

// The labels of a domain name; IDNA also separates them with the ideographic and full-width full stops.
std::vector<std::u16string> domain_labels(const std::u16string &name)
{
	std::vector<std::u16string> labels(1);
	for (const auto character : name) {
		if (character == u'.' || character == 0x3002 || character == 0xff0e || character == 0xff61)
			labels.emplace_back();
		else
			labels.back().push_back(character);
	}
	return labels;
}

// IdnToAscii: labels with characters beyond ASCII become "xn--" and their Punycode, after case folding; empty when the name is invalid.
std::optional<std::u16string> idn_to_ascii(const std::u16string &name)
{
	const auto labels = domain_labels(name);
	std::u16string result;
	for (size_t index = 0; index < labels.size(); ++index) {
		const auto &label = labels[index];
		// Only the last label may be empty, the root of a fully qualified name.
		if (label.empty() && (index + 1 < labels.size() || labels.size() == 1))
			return std::nullopt;
		std::u32string points;
		bool ascii = true;
		for (size_t at = 0; at < label.size(); ++at) {
			char32_t point = label[at];
			if (point >= 0xd800 && point < 0xdc00 && at + 1 < label.size() && label[at + 1] >= 0xdc00 && label[at + 1] < 0xe000)
				point = 0x10000 + ((point - 0xd800) << 10) + (label[++at] - 0xdc00);
			else if (point >= 0xd800 && point < 0xe000)
				return std::nullopt;
			ascii &= point < 0x80;
			points.push_back(point);
		}
		std::string encoded;
		if (ascii) {
			for (const auto point : points)
				encoded.push_back(char(point));
		} else {
			for (auto &point : points)
				if (point < 0x10000)
					point = fold_case(char16_t(point));
			const auto punycode = punycode_encode(points);
			if (!punycode)
				return std::nullopt;
			encoded = "xn--" + *punycode;
		}
		if (encoded.size() > 63)
			return std::nullopt;
		if (index)
			result.push_back(u'.');
		result.append(encoded.begin(), encoded.end());
	}
	if (result.size() > 255)
		return std::nullopt;
	return result;
}

// IdnToUnicode: an ASCII name whose "xn--" labels are decoded; empty when the name is not ASCII or a label does not decode.
std::optional<std::u16string> idn_to_unicode(const std::u16string &name)
{
	const auto labels = domain_labels(name);
	std::u16string result;
	for (size_t index = 0; index < labels.size(); ++index) {
		const auto &label = labels[index];
		if ((label.empty() && (index + 1 < labels.size() || labels.size() == 1)) || label.size() > 63)
			return std::nullopt;
		std::string ascii;
		for (const auto character : label) {
			if (character >= 0x80)
				return std::nullopt;
			ascii.push_back(char(character));
		}
		if (index)
			result.push_back(u'.');
		const bool encoded = ascii.size() > 4 && (ascii[0] | 0x20) == 'x' && (ascii[1] | 0x20) == 'n' && ascii[2] == '-' && ascii[3] == '-';
		if (!encoded) {
			result += label;
			continue;
		}
		const auto points = punycode_decode(std::string_view(ascii).substr(4));
		if (!points)
			return std::nullopt;
		for (const auto point : *points) {
			if (point >= 0x10000) {
				result.push_back(char16_t(0xd800 + ((point - 0x10000) >> 10)));
				result.push_back(char16_t(0xdc00 + ((point - 0x10000) & 0x3ff)));
			} else {
				result.push_back(char16_t(point));
			}
		}
	}
	return result;
}

}

std::vector<uint8_t> locale_table()
{
	std::vector<uint8_t> table(locale_table_entries * 8, 0xff);
	std::u16string pool;
	for (uint32_t index = 0; index < locale_table_entries; ++index) {
		// The locale's own name and its parent's depend on the locale asked for, which the served block does not read: the runtime answers.
		if (index == 0x5c || index == 0x6d)
			continue;
		const auto text = locale_text(index < 0x80 ? index : 0x1000 + index - 0x80);
		if (!text)
			continue;
		const uint16_t offset = uint16_t(pool.size()), length = uint16_t(text->size());
		const uint32_t number = as_number(*text);
		std::memcpy(table.data() + index * 8, &offset, 2);
		std::memcpy(table.data() + index * 8 + 2, &length, 2);
		std::memcpy(table.data() + index * 8 + 4, &number, 4);
		pool += *text;
		pool += u'\0';
	}
	const size_t start = table.size();
	table.resize(start + pool.size() * 2);
	std::memcpy(table.data() + start, pool.data(), pool.size() * 2);
	return table;
}

void register_text_api(ApiRegistry &registry, KernelContext &context)
{
	auto &memory = context.memory;
	const auto ansi_page = [&](uint32_t code_page) {
		return code_page == 0 || code_page == 3 ? context.launch.ansi_code_page : code_page == 1 ? 437 : code_page;
	};

	registry.add("lstrlenW", 1, [](ApiCall &call) { call.ret(call[0] ? call.wide(call[0], 1 << 24).size() : 0); });
	registry.add("lstrlenA", 1, [](ApiCall &call) { call.ret(call[0] ? call.narrow(call[0], 1 << 24).size() : 0); });
	registry.add("lstrcpyW", 2, [](ApiCall &call) {
		const auto text = call.wide(call[1], 1 << 24);
		call.put_wide(call[0], text, text.size() + 1);
		call.ret(call[0]);
	});
	registry.add("lstrcpyA", 2, [](ApiCall &call) {
		const auto text = call.narrow(call[1], 1 << 24);
		call.put_narrow(call[0], text, text.size() + 1);
		call.ret(call[0]);
	});
	registry.add("lstrcpynW", 3, [](ApiCall &call) {
		const int32_t capacity = call.i32(2);
		if (capacity <= 0) {
			call.ret(0);
			return;
		}
		const auto text = call.wide(call[1], 1 << 24);
		call.put_wide(call[0], text.substr(0, size_t(capacity) - 1), size_t(capacity));
		call.ret(call[0]);
	});
	registry.add("lstrcpynA", 3, [](ApiCall &call) {
		const int32_t capacity = call.i32(2);
		if (capacity <= 0) {
			call.ret(0);
			return;
		}
		const auto text = call.narrow(call[1], 1 << 24);
		call.put_narrow(call[0], text.substr(0, size_t(capacity) - 1), size_t(capacity));
		call.ret(call[0]);
	});
	registry.add("lstrcmpW", 2, [](ApiCall &call) { call.ret(uint32_t(collate(call.wide(call[0]), call.wide(call[1]), 0))); });
	registry.add("lstrcmpiW", 2, [](ApiCall &call) { call.ret(uint32_t(collate(call.wide(call[0]), call.wide(call[1]), 1))); });
	registry.add("lstrcmpiA", 2, [](ApiCall &call) {
		call.ret(uint32_t(collate(widen_ascii(call.narrow(call[0])), widen_ascii(call.narrow(call[1])), 1)));
	});
	registry.add("lstrcmpA", 2, [](ApiCall &call) {
		call.ret(uint32_t(collate(widen_ascii(call.narrow(call[0])), widen_ascii(call.narrow(call[1])), 0)));
	});

	registry.add("WideCharToMultiByte", 8, [&, ansi_page](ApiCall &call) {
		const uint32_t page = ansi_page(call.u32(0));
		const int32_t count = call.i32(3);
		const int32_t capacity = call.i32(5);
		if (!call[2] || !count || capacity < 0 || (capacity && !call[4])) {
			call.fail(87);
			return;
		}
		const auto source = counted_wide(call, call[2], count);
		std::string encoded;
		bool replaced = false;
		if (page == 65001) {
			encoded = encode_utf8(source, true);
		} else if (const auto *single = native_single_byte_page(page)) {
			// WC_NO_BEST_FIT_CHARS keeps only characters the page has a byte of their own for.
			const bool exact = call.u32(1) & 0x400;
			for (const auto character : source) {
				const auto byte = encode_single_byte(*single, character, exact);
				encoded.push_back(byte < 0 ? '?' : char(byte));
				replaced |= byte < 0;
			}
		} else if (page == 1252 || page == 28591 || page == 20127) {
			encoded = encode_ansi(source, 1252);
			for (size_t at = 0; at < source.size(); ++at)
				if (source[at] > 0xff && !(source[at] >= 0x2013 && source[at] <= 0x2122) && encoded[at] == '?')
					replaced = true;
		} else {
			call.fail(87);
			return;
		}
		if (count < 0)
			encoded.push_back('\0');
		if (call[7]) call.store<uint32_t>(call[7], replaced);
		if (!capacity) {
			call.ret(encoded.size());
			return;
		}
		if (size_t(capacity) < encoded.size()) {
			call.fail(122);
			return;
		}
		call.write(call[4], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size()));
		call.ret(encoded.size());
	});

	registry.add("CompareStringW", 6, [](ApiCall &call) {
		const auto left = counted_wide(call, call[2], call.i32(3)), right = counted_wide(call, call[4], call.i32(5));
		call.ret(uint32_t(collate(left, right, call.u32(1)) + 2));
	});
	registry.add("CompareStringEx", 9, [](ApiCall &call) {
		const auto left = counted_wide(call, call[2], call.i32(3)), right = counted_wide(call, call[4], call.i32(5));
		call.ret(uint32_t(collate(left, right, call.u32(1)) + 2));
	});
	// Where one text occurs in another, as the comparison flags make them equal: FIND_STARTSWITH and FIND_ENDSWITH test only the start
	// or the end, FIND_FROMEND looks for the last occurrence. The index is returned and the length found stored; -1 when it is not there.
	const auto find_string = [](ApiCall &call, uint32_t flags, size_t source_at, size_t value_at, uint64_t found_at) {
		if (!call[source_at] || !call[value_at]) {
			call.fail(87, uint64_t(int64_t(-1)));
			return;
		}
		const auto source = counted_wide(call, call[source_at], call.i32(source_at + 1));
		const auto value = counted_wide(call, call[value_at], call.i32(value_at + 1));
		const uint32_t compare = flags & ~0xf00000U;
		const auto matches = [&](size_t at) { return collate(source.substr(at, value.size()), value, compare) == 0; };
		int64_t index = -1;
		if (value.size() <= source.size()) {
			const size_t last = source.size() - value.size();
			if (flags & 0x100000U)
				index = matches(0) ? 0 : -1;
			else if (flags & 0x200000U)
				index = matches(last) ? int64_t(last) : -1;
			else if (flags & 0x800000U) {
				for (size_t at = last + 1; at-- > 0 && index < 0;)
					if (matches(at))
						index = int64_t(at);
			} else {
				for (size_t at = 0; at <= last && index < 0; ++at)
					if (matches(at))
						index = int64_t(at);
			}
		}
		if (index >= 0 && found_at)
			call.store<uint32_t>(found_at, uint32_t(value.size()));
		call.set_last_error(0);
		call.ret(uint64_t(index));
	};
	registry.add("FindNLSString", 7, [find_string](ApiCall &call) { find_string(call, call.u32(1), 2, 4, call[6]); });
	registry.add("FindNLSStringEx", 10, [find_string](ApiCall &call) { find_string(call, call.u32(1), 2, 4, call[6]); });
	// Code unit by code unit, optionally after upper-casing both, as file systems and the registry compare names.
	registry.add("CompareStringOrdinal", 5, [](ApiCall &call) {
		if (!call[0] || !call[2]) {
			call.fail(87);
			return;
		}
		auto left = counted_wide(call, call[0], call.i32(1)), right = counted_wide(call, call[2], call.i32(3));
		if (call.u32(4)) {
			for (auto &character : left)
				character = upper_case(character);
			for (auto &character : right)
				character = upper_case(character);
		}
		call.ret(left < right ? 1 : left == right ? 2 : 3);
	});
	const auto map_string = [](ApiCall &call, uint32_t flags, std::u16string text, uint64_t destination, int32_t capacity) {
		if (flags & 0x100) for (auto &character : text) character = fold_case(character);
		else if (flags & 0x200) for (auto &character : text) character = upper_case(character);
		const size_t needed = text.size();
		if (!capacity) {
			call.ret(needed);
			return;
		}
		if (size_t(capacity) < needed) {
			call.fail(122);
			return;
		}
		call.put_wide(destination, text, size_t(capacity) + 1);
		std::vector<uint8_t> bytes(text.size() * 2);
		std::memcpy(bytes.data(), text.data(), bytes.size());
		call.write(destination, bytes);
		call.ret(needed);
	};
	registry.add("LCMapStringW", 6, [map_string](ApiCall &call) {
		const int32_t length = call.i32(3);
		auto text = counted_wide(call, call[2], length);
		if (length < 0) text.push_back(u'\0');
		const auto zero = length < 0;
		if (zero) text.pop_back();
		map_string(call, call.u32(1), text, call[4], call.i32(5));
		if (zero && call.result) call.ret(call.result + 1);
	});
	registry.add("LCMapStringEx", 9, [map_string](ApiCall &call) {
		const int32_t length = call.i32(3);
		auto text = counted_wide(call, call[2], length);
		map_string(call, call.u32(1), text, call[4], call.i32(5));
		if (length < 0 && call.result) call.ret(call.result + 1);
	});
	registry.add("FoldStringW", 5, [](ApiCall &call) {
		const uint32_t flags = call.u32(0);
		const int32_t length = call.i32(2), capacity = call.i32(4);
		constexpr uint32_t known = 0x10 | 0x20 | 0x40 | 0x80 | 0x2000;
		const bool composing = flags & 0x20, decomposing = flags & 0x40;
		if (!flags || (flags & ~known) || (composing && decomposing) || ((flags & 0x2000) && (composing || decomposing))) {
			call.fail(1004);
			return;
		}
		if (!call[1] || !length || length < -1 || capacity < 0 || (capacity && !call[3]) || (capacity && call[1] == call[3])) {
			call.fail(87);
			return;
		}
		auto text = fold_string(counted_wide(call, call[1], length), flags);
		// A string given with its terminator is answered with one.
		if (length < 0) text.push_back(u'\0');
		if (!capacity) {
			call.ret(text.size());
			return;
		}
		if (text.size() > size_t(capacity)) {
			call.fail(122);
			return;
		}
		std::vector<uint8_t> bytes(text.size() * 2);
		std::memcpy(bytes.data(), text.data(), bytes.size());
		call.write(call[3], bytes);
		call.ret(text.size());
	});
	// The input is counted or terminated, and a terminated one is answered with its terminator.
	const auto convert_name = [](ApiCall &call, const std::function<std::optional<std::u16string>(const std::u16string &)> &convert) {
		const int32_t length = call.i32(2), capacity = call.i32(4);
		if (!call[1] || !length || length < -1 || capacity < 0 || (capacity && !call[3])) {
			call.fail(87);
			return;
		}
		auto result = convert(counted_wide(call, call[1], length));
		if (!result) {
			call.fail(123);
			return;
		}
		if (length < 0)
			result->push_back(u'\0');
		if (!capacity) {
			call.ret(result->size());
			return;
		}
		if (result->size() > size_t(capacity)) {
			call.fail(122);
			return;
		}
		std::vector<uint8_t> bytes(result->size() * 2);
		std::memcpy(bytes.data(), result->data(), bytes.size());
		call.write(call[3], bytes);
		call.ret(result->size());
	};
	registry.add("IdnToAscii", 5, [convert_name](ApiCall &call) { convert_name(call, idn_to_ascii); });
	registry.add("IdnToUnicode", 5, [convert_name](ApiCall &call) { convert_name(call, idn_to_unicode); });
	registry.add("LCMapStringA", 6, [&, map_string, ansi_page](ApiCall &call) {
		const int32_t length = call.i32(3);
		const auto narrow = counted_narrow(call, call[2], length);
		const auto page = ansi_page(0);
		auto text = decode_ansi(narrow, page);
		const uint32_t flags = call.u32(1);
		if (flags & 0x100) for (auto &character : text) character = fold_case(character);
		else if (flags & 0x200) for (auto &character : text) character = upper_case(character);
		auto result = encode_ansi(text, page);
		const int32_t capacity = call.i32(5);
		if (!capacity) {
			call.ret(result.size());
			return;
		}
		if (size_t(capacity) < result.size()) {
			call.fail(122);
			return;
		}
		call.write(call[4], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(result.data()), result.size()));
		call.ret(result.size());
	});
	// The character classes of a text for CT_CTYPE1, CT_CTYPE2 and CT_CTYPE3.
	const auto classify = [](uint32_t kind, const std::u16string &text) {
		static const bool unicode = std::setlocale(LC_CTYPE, "C.UTF-8") != nullptr;
		(void)unicode;
		std::vector<uint8_t> bytes(text.size() * 2);
		for (size_t at = 0; at < text.size(); ++at) {
			const wint_t c = text[at];
			uint16_t value = 0;
			if (kind == 1) {
				if (std::iswupper(c)) value |= 0x1 | 0x100;
				if (std::iswlower(c)) value |= 0x2 | 0x100;
				if (std::iswdigit(c)) value |= 0x4;
				if (std::iswspace(c)) value |= 0x8;
				if (std::iswpunct(c)) value |= 0x10;
				if (std::iswcntrl(c)) value |= 0x20;
				if (c == u' ' || c == u'\t') value |= 0x40;
				if (std::iswxdigit(c)) value |= 0x80;
				if (std::iswalpha(c) && !(value & 0x100)) value |= 0x100;
				if (c >= 0x20 || std::iswcntrl(c)) value |= 0x200;
			} else if (kind == 2) {
				value = std::iswalpha(c) ? 1 : std::iswdigit(c) ? 3 : std::iswspace(c) ? 12 : 11;
			} else {
				if (std::iswalpha(c)) value |= 0x8000;
				if (std::iswpunct(c)) value |= 0x10;
				if (std::iswspace(c)) value |= 0x8;
				if (c >= 0x300 && c < 0x370) value |= 0x1;
			}
			std::memcpy(bytes.data() + at * 2, &value, 2);
		}
		return bytes;
	};
	// GetStringTypeW(kind, text, count, types): four arguments.
	registry.add("GetStringTypeW", 4, [classify](ApiCall &call) {
		const uint32_t kind = call.u32(0);
		if (!call[1] || !call[3] || (kind != 1 && kind != 2 && kind != 4)) {
			call.fail(87);
			return;
		}
		call.write(call[3], classify(kind, counted_wide(call, call[1], call.i32(2))));
		call.ret(1);
	});
	// GetStringTypeExW(locale, kind, text, count, types) and GetStringTypeA(locale, kind, text, count, types): five.
	registry.add("GetStringTypeExW", 5, [classify](ApiCall &call) {
		const uint32_t kind = call.u32(1);
		if (!call[2] || !call[4] || (kind != 1 && kind != 2 && kind != 4)) {
			call.fail(87);
			return;
		}
		call.write(call[4], classify(kind, counted_wide(call, call[2], call.i32(3))));
		call.ret(1);
	});
	registry.add("GetStringTypeA", 5, [classify, ansi_page](ApiCall &call) {
		const uint32_t kind = call.u32(1);
		if (!call[2] || !call[4] || (kind != 1 && kind != 2 && kind != 4)) {
			call.fail(87);
			return;
		}
		const auto text = decode_ansi(counted_narrow(call, call[2], call.i32(3)), ansi_page(call.u32(0)));
		call.write(call[4], classify(kind, text));
		call.ret(1);
	});

	struct CodePage { uint32_t id, maximum; const char *name; };
	static constexpr CodePage code_pages[] = {
		{437, 1, "OEM United States"}, {850, 1, "OEM Multilingual Latin 1"}, {1250, 1, "Central European (Windows)"}, {1251, 1, "Cyrillic (Windows)"},
		{1252, 1, "Western European (Windows)"}, {1253, 1, "Greek (Windows)"}, {1254, 1, "Turkish (Windows)"}, {1255, 1, "Hebrew (Windows)"},
		{1256, 1, "Arabic (Windows)"}, {1257, 1, "Baltic (Windows)"}, {1258, 1, "Vietnamese (Windows)"},
		{28591, 1, "Western European (ISO)"}, {20127, 1, "US-ASCII"}, {65001, 4, "Unicode (UTF-8)"}
	};
	const auto code_page = [](uint32_t page) -> const CodePage * {
		if (page == 0 || page == 3) page = 1252;
		if (page == 1) page = 437;
		for (const auto &entry : code_pages)
			if (entry.id == page) return &entry;
		return nullptr;
	};
	registry.add("GetCPInfo", 2, [code_page](ApiCall &call) {
		const auto page = code_page(call.u32(0));
		if (!page || !call[1]) {
			call.fail(87);
			return;
		}
		std::vector<uint8_t> bytes(20);
		std::memcpy(bytes.data(), &page->maximum, 4);
		bytes[4] = '?';
		call.write(call[1], bytes);
		call.ret(1);
	});
	for (const auto suffix : {std::string("A"), std::string("W")}) {
		registry.add("GetCPInfoEx" + suffix, 3, [code_page, suffix](ApiCall &call) {
			const auto page = code_page(call.u32(0));
			if (!page || call.u32(1) || !call[2]) {
				call.fail(87);
				return;
			}
			const size_t width = suffix == "W" ? 2 : 1;
			std::vector<uint8_t> bytes(24 + 260 * width);
			std::memcpy(bytes.data(), &page->maximum, 4);
			bytes[4] = '?';
			bytes[18] = '?';
			std::memcpy(bytes.data() + 20, &page->id, 4);
			for (size_t index = 0; page->name[index]; ++index)
				bytes[24 + index * width] = uint8_t(page->name[index]);
			call.write(call[2], bytes);
			call.ret(1);
		});
	}
	registry.add("IsValidCodePage", 1, [](ApiCall &call) {
		const uint32_t page = call.u32(0);
		call.ret(page == 437 || page == 850 || page == 1252 || page == 28591 || page == 20127 || page == 65001 || page == 1200 || page == 1201 ||
			native_single_byte_page(page) != nullptr);
	});
	registry.add("GetOEMCP", 0, [](ApiCall &call) { call.ret(437); });

	registry.add("GetUserDefaultLCID", 0, [](ApiCall &call) { call.ret(0x409); });
	registry.add("GetSystemDefaultLCID", 0, [](ApiCall &call) { call.ret(0x409); });
	registry.add("GetUserDefaultLangID", 0, [](ApiCall &call) { call.ret(0x409); });
	registry.add("GetSystemDefaultLangID", 0, [](ApiCall &call) { call.ret(0x409); });
	registry.add("GetUserDefaultUILanguage", 0, [](ApiCall &call) { call.ret(0x409); });
	registry.add("GetSystemDefaultUILanguage", 0, [](ApiCall &call) { call.ret(0x409); });
	registry.add("GetThreadUILanguage", 0, [](ApiCall &call) { call.ret(0x409); });
	registry.add("GetThreadLocale", 0, [](ApiCall &call) { call.ret(0x409); });
	registry.add("SetThreadLocale", 1, [](ApiCall &call) { call.ret(1); });
	// The default locales stand for the user's and the system's, both US English here; a language without a sublanguage takes its default.
	registry.add("ConvertDefaultLocale", 1, [](ApiCall &call) {
		const uint32_t locale = call.u32(0);
		call.ret(locale == 0 || locale == 0x400 || locale == 0x800 ? 0x409 : (locale & 0xfc00) ? locale : locale | 0x400);
	});
	registry.add("IsValidLocale", 2, [](ApiCall &call) { call.ret(call.u32(0) == 0x409 || call.u32(0) == 0x400 || call.u32(0) == 0x800); });
	registry.add("IsValidLocaleName", 1, [](ApiCall &call) {
		call.ret(call[0] && compare_fold(call.wide(call[0], 85), u"en-US") == 0);
	});
	const auto locale_name = [](ApiCall &call, uint64_t buffer, int32_t capacity) {
		const std::u16string name = u"en-US";
		if (capacity && size_t(capacity) <= name.size()) {
			call.fail(122);
			return;
		}
		call.put_wide(buffer, name, size_t(capacity));
		call.ret(name.size() + 1);
	};
	registry.add("GetUserDefaultLocaleName", 2, [locale_name](ApiCall &call) { locale_name(call, call[0], call.i32(1)); });
	registry.add("GetSystemDefaultLocaleName", 2, [locale_name](ApiCall &call) { locale_name(call, call[0], call.i32(1)); });
	// A name the table does not know, as the culture-neutral "x-none" programs use, is not a locale. No name is the user's, the empty one
	// the invariant locale.
	registry.add("LocaleNameToLCID", 2, [](ApiCall &call) {
		if (!call[0]) {
			call.ret(0x409);
			return;
		}
		const auto name = call.wide(call[0], 85);
		if (name.empty()) {
			call.ret(0x7f);
			return;
		}
		if (name == u"!x-sys-default-locale") {
			call.ret(0x409);
			return;
		}
		for (const auto &[known, locale] : known_locales)
			if (!compare_fold(name, known)) {
				call.ret(locale);
				return;
			}
		call.fail(87);
	});
	registry.add("LCIDToLocaleName", 4, [](ApiCall &call) {
		const uint32_t locale = call.u32(0);
		std::u16string name;
		if (locale == 0 || locale == 0x400 || locale == 0x800)
			name = u"en-US";
		else if (locale != 0x7f) {
			const auto known = std::find_if(std::begin(known_locales), std::end(known_locales), [locale](const auto &entry) { return entry.second == locale; });
			if (known == std::end(known_locales)) {
				call.fail(87);
				return;
			}
			name = known->first;
		}
		const int32_t capacity = call.i32(2);
		if (capacity && size_t(capacity) <= name.size()) {
			call.fail(122);
			return;
		}
		call.put_wide(call[1], name, size_t(capacity));
		call.ret(name.size() + 1);
	});
	const auto locale_info = [](ApiCall &call, uint32_t type, uint64_t buffer, int32_t capacity, bool wide) {
		const auto text = locale_text(type);
		if (!text) {
			call.fail(87);
			return;
		}
		if (type & locale_return_number) {
			// The capacity counts characters: two wide ones hold the number.
			if (capacity < (wide ? 2 : 4)) {
				call.fail(122);
				return;
			}
			call.store<uint32_t>(buffer, as_number(*text));
			call.ret(2);
			return;
		}
		const size_t needed = text->size() + 1;
		if (!capacity) {
			call.ret(needed);
			return;
		}
		if (size_t(capacity) < needed) {
			call.fail(122);
			return;
		}
		if (wide)
			call.put_wide(buffer, *text, size_t(capacity));
		else
			call.put_narrow(buffer, to_utf8(*text), size_t(capacity));
		call.ret(needed);
	};
	// VerLanguageName(language, buffer, characters): the name Windows gives a language identifier in its own English interface, as
	// version resources name the language of their strings. The count leaves out the terminator.
	const auto language_name = [](ApiCall &call, bool wide) {
		static const std::map<uint32_t, std::u16string_view> names{
			{0x0000, u"Language Neutral"}, {0x0401, u"Arabic (Saudi Arabia)"}, {0x0405, u"Czech (Czech Republic)"}, {0x0406, u"Danish (Denmark)"},
			{0x0407, u"German (Germany)"}, {0x0408, u"Greek (Greece)"}, {0x0409, u"English (United States)"}, {0x040a, u"Spanish (Spain, Traditional Sort)"},
			{0x040b, u"Finnish (Finland)"}, {0x040c, u"French (France)"}, {0x040d, u"Hebrew (Israel)"}, {0x040e, u"Hungarian (Hungary)"},
			{0x0410, u"Italian (Italy)"}, {0x0411, u"Japanese (Japan)"}, {0x0412, u"Korean (Korea)"}, {0x0413, u"Dutch (Netherlands)"},
			{0x0414, u"Norwegian, Bokm\u00e5l (Norway)"}, {0x0415, u"Polish (Poland)"}, {0x0416, u"Portuguese (Brazil)"},
			{0x0419, u"Russian (Russia)"}, {0x041b, u"Slovak (Slovakia)"}, {0x041d, u"Swedish (Sweden)"}, {0x041f, u"Turkish (Turkey)"},
			{0x0422, u"Ukrainian (Ukraine)"}, {0x0423, u"Belarusian (Belarus)"}, {0x0424, u"Slovenian (Slovenia)"}, {0x0427, u"Lithuanian (Lithuania)"},
			{0x0429, u"Persian (Iran)"}, {0x0439, u"Hindi (India)"}, {0x0804, u"Chinese (Simplified, PRC)"}, {0x0404, u"Chinese (Traditional, Taiwan)"},
			{0x0809, u"English (United Kingdom)"}, {0x0816, u"Portuguese (Portugal)"}, {0x0c0a, u"Spanish (Spain, International Sort)"},
			{0x0807, u"German (Switzerland)"}, {0x080c, u"French (Belgium)"}, {0x0c0c, u"French (Canada)"}, {0x0418, u"Romanian (Romania)"},
			{0x041a, u"Croatian (Croatia)"}, {0x0402, u"Bulgarian (Bulgaria)"}, {0x041e, u"Thai (Thailand)"}, {0x042a, u"Vietnamese (Vietnam)"}};
		const auto found = names.find(call.u32(0) & 0xffff);
		const std::u16string name(found == names.end() ? u"Language Neutral" : found->second);
		const size_t capacity = call.u32(2);
		if (wide) {
			if (call[1] && capacity)
				call.put_wide(call[1], name, capacity);
			call.ret(call[1] && capacity ? std::min(name.size(), capacity - 1) : name.size());
		} else {
			const auto narrow = to_utf8(name);
			if (call[1] && capacity)
				call.put_narrow(call[1], narrow, capacity);
			call.ret(call[1] && capacity ? std::min(narrow.size(), capacity - 1) : narrow.size());
		}
	};
	registry.add("VerLanguageNameW", 3, [language_name](ApiCall &call) { language_name(call, true); });
	registry.add("VerLanguageNameA", 3, [language_name](ApiCall &call) { language_name(call, false); });
	registry.add("GetLocaleInfoW", 4, [locale_info](ApiCall &call) { locale_info(call, call.u32(1), call[2], call.i32(3), true); });
	registry.add("GetLocaleInfoA", 4, [locale_info](ApiCall &call) { locale_info(call, call.u32(1), call[2], call.i32(3), false); });
	// GetLocaleInfoEx answers for the one locale there is, en-US, and for its neutral form "en", whose name is its own and whose parent is
	// the invariant culture: a culture's chain of parents ends there.
	registry.add("GetLocaleInfoEx", 4, [locale_info](ApiCall &call) {
		const auto name = call[0] ? call.wide(call[0], 85) : std::u16string();
		const uint32_t type = call.u32(1) & ~0x80000000U;
		const bool neutral = !name.empty() && name.find(u'-') == name.npos && name.front() != u'!';
		if (neutral && (type == 0x5c || type == 0x6d)) {
			const std::u16string text = type == 0x5c ? name : u"";
			if (!call.i32(3)) {
				call.ret(text.size() + 1);
				return;
			}
			if (size_t(call.i32(3)) < text.size() + 1) {
				call.fail(122);
				return;
			}
			call.put_wide(call[2], text, size_t(call.i32(3)));
			call.ret(text.size() + 1);
			return;
		}
		locale_info(call, call.u32(1), call[2], call.i32(3), true);
	});
	registry.add("EnumSystemLocalesW", 2, [&](ApiCall &call) {
		const auto scratch = memory.allocate(0, 4096, 0x3000, 4);
		if (scratch.status) {
			call.fail(8);
			return;
		}
		const std::u16string name = u"00000409";
		std::vector<uint8_t> bytes((name.size() + 1) * 2);
		std::memcpy(bytes.data(), name.data(), name.size() * 2);
		call.write(scratch.address, bytes);
		const uint64_t arguments[] = {scratch.address};
		const auto result = context.call(call[0], arguments);
		memory.free(scratch.address, 0, 0x8000);
		if (result.stop != NativeStop::returned)
			throw result;
		call.ret(1);
	});

	// The Gregorian calendar of en-US, the one calendar the locale has: each kind of calendar data with every value Windows lists for
	// it, the preferred one first. The flags above the kind (no user override, ANSI code page, number, genitive names) are set aside.
	const auto calendar_values = [](uint32_t type) -> std::vector<std::u16string> {
		static constexpr const char16_t *days[] = {u"Monday", u"Tuesday", u"Wednesday", u"Thursday", u"Friday", u"Saturday", u"Sunday"};
		static constexpr const char16_t *months[] = {u"January", u"February", u"March", u"April", u"May", u"June", u"July", u"August",
			u"September", u"October", u"November", u"December"};
		type &= 0x0fffffff;
		if (type >= 7 && type <= 13)
			return {days[type - 7]};
		if (type >= 14 && type <= 20)
			return {std::u16string(days[type - 14]).substr(0, 3)};
		if (type >= 21 && type <= 33)
			return {type == 33 ? u"" : months[type - 21]};
		if (type >= 34 && type <= 46)
			return {type == 46 ? u"" : std::u16string(months[type - 34]).substr(0, 3)};
		if (type >= 0x31 && type <= 0x37)
			return {std::u16string(days[type - 0x31]).substr(0, 2)};
		switch (type) {
		case 1: return {u"1"};
		case 2: return {u"Gregorian Calendar"};
		case 3: return {u"0"};
		case 4: case 0x3b: return {u"A.D."};
		case 5: return {u"M/d/yyyy", u"M/d/yy", u"MM/dd/yy", u"MM/dd/yyyy", u"yy/MM/dd", u"yyyy-MM-dd", u"dd-MMM-yy"};
		case 6: return {u"dddd, MMMM d, yyyy", u"MMMM d, yyyy", u"dddd, d MMMM, yyyy", u"d MMMM, yyyy"};
		case 0x2f: return {u"MMMM yyyy"};
		case 0x30: return {u"2049"};
		case 0x38: return {u"MMMM d"};
		case 0x39: case 0x3c: return {u"AD"};
		case 0x3a: return {u"dddd, MMMM d"};
		default: return {};
		}
	};
	// EnumCalendarInfoExEx(routine, locale, calendar, reserved, type, parameter): the routine hears each value with the calendar's number
	// until it answers FALSE.
	registry.add("EnumCalendarInfoExEx", 6, [&, calendar_values](ApiCall &call) {
		const uint32_t calendar = call.u32(2);
		if (!call[0]) {
			call.fail(87);
			return;
		}
		if (calendar != 1 && calendar != UINT32_MAX) {
			call.ret(1);
			return;
		}
		const auto scratch = memory.allocate(0, 4096, 0x3000, 4);
		if (scratch.status) {
			call.fail(8);
			return;
		}
		for (const auto &value : calendar_values(call.u32(4))) {
			std::vector<uint8_t> bytes((value.size() + 1) * 2);
			std::memcpy(bytes.data(), value.data(), value.size() * 2);
			call.write(scratch.address, bytes);
			const uint64_t arguments[] = {scratch.address, 1, 0, call[5]};
			const auto result = context.call(call[0], arguments);
			if (result.stop != NativeStop::returned) {
				memory.free(scratch.address, 0, 0x8000);
				throw result;
			}
			if (!uint32_t(result.value))
				break;
		}
		memory.free(scratch.address, 0, 0x8000);
		call.ret(1);
	});
	// Hands each string to an enumeration routine with the extra arguments after it, until the routine answers FALSE.
	const auto enumerate = [&](ApiCall &call, uint64_t routine, const std::vector<std::u16string> &values, std::vector<uint64_t> after) {
		const auto scratch = memory.allocate(0, 4096, 0x3000, 4);
		if (scratch.status) {
			call.fail(8);
			return;
		}
		for (const auto &value : values) {
			std::vector<uint8_t> bytes((value.size() + 1) * 2);
			std::memcpy(bytes.data(), value.data(), value.size() * 2);
			call.write(scratch.address, bytes);
			std::vector<uint64_t> arguments{scratch.address};
			arguments.insert(arguments.end(), after.begin(), after.end());
			const auto result = context.call(routine, arguments);
			if (result.stop != NativeStop::returned) {
				memory.free(scratch.address, 0, 0x8000);
				throw result;
			}
			if (!uint32_t(result.value))
				break;
		}
		memory.free(scratch.address, 0, 0x8000);
		call.ret(1);
	};
	registry.add("EnumSystemLocalesEx", 4, [enumerate](ApiCall &call) {
		if (!call[0] || call[3]) {
			call.fail(87);
			return;
		}
		const uint32_t flags = call.u32(1);
		if (flags & ~0x3fU) {
			call.fail(1004);
			return;
		}
		// The locale data above contains en-US only: a built-in specific locale, without custom or alternate-sort data.
		if (((flags & 0xf) && !(flags & 1)) || ((flags & 0x30) && !(flags & 0x20))) {
			call.ret(1);
			return;
		}
		enumerate(call, call[0], {u"en-US"}, {0x21, call[2]});
	});
	// EnumTimeFormatsEx(routine, locale, flags, parameter): the long time formats, or without seconds (TIME_NOSECONDS) the short ones.
	registry.add("EnumTimeFormatsEx", 4, [enumerate](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const bool short_form = call.u32(2) & 2;
		enumerate(call, call[0], short_form ? std::vector<std::u16string>{u"h:mm tt", u"hh:mm tt", u"H:mm", u"HH:mm"} :
			std::vector<std::u16string>{u"h:mm:ss tt", u"hh:mm:ss tt", u"H:mm:ss", u"HH:mm:ss"}, {call[3]});
	});
	// EnumDateFormatsExEx(routine, locale, flags, parameter): DATE_SHORTDATE, DATE_LONGDATE, DATE_YEARMONTH or DATE_MONTHDAY, each with
	// the calendar's number.
	registry.add("EnumDateFormatsExEx", 4, [enumerate, calendar_values](ApiCall &call) {
		const uint32_t flags = call.u32(2);
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const uint32_t type = flags & 2 ? 6 : flags & 8 ? 0x2f : flags & 0x80 ? 0x38 : 5;
		enumerate(call, call[0], calendar_values(type), {1, call[3]});
	});
	// GetCalendarInfo: the first value of a kind, or with CAL_RETURN_NUMBER its number in `value`.
	const auto calendar_info = [calendar_values](ApiCall &call, uint32_t calendar, uint32_t type, uint64_t buffer, int32_t capacity, uint64_t value) {
		const auto values = calendar_values(type);
		if (calendar != 1 || values.empty()) {
			call.fail(87);
			return;
		}
		if (type & 0x20000000) {
			if (!value) {
				call.fail(87);
				return;
			}
			uint32_t number = 0;
			for (const auto character : values[0])
				number = number * 10 + uint32_t(character - u'0');
			call.store<uint32_t>(value, number);
			call.ret(2);
			return;
		}
		const size_t needed = values[0].size() + 1;
		if (!capacity) {
			call.ret(needed);
			return;
		}
		if (size_t(capacity) < needed) {
			call.fail(122);
			return;
		}
		call.put_wide(buffer, values[0], size_t(capacity));
		call.ret(needed);
	};
	registry.add("GetCalendarInfoEx", 7, [calendar_info](ApiCall &call) {
		calendar_info(call, call.u32(1), call.u32(3), call[4], call.i32(5), call[6]);
	});
	registry.add("GetCalendarInfoW", 6, [calendar_info](ApiCall &call) {
		calendar_info(call, call.u32(1), call.u32(2), call[3], call.i32(4), call[5]);
	});

	const auto format_date_time = [](ApiCall &call, bool date, uint64_t flags_index, uint64_t time_index, uint64_t format_index,
		uint64_t buffer_index, uint64_t size_index) {
		const uint32_t flags = call.u32(flags_index);
		uint16_t fields[8];
		if (call[time_index]) {
			const auto bytes = call.read(call[time_index], 16);
			std::memcpy(fields, bytes.data(), 16);
		} else {
			timespec now{};
			::clock_gettime(CLOCK_REALTIME, &now);
			tm local{};
			::localtime_r(&now.tv_sec, &local);
			const uint16_t current[8] = {uint16_t(local.tm_year + 1900), uint16_t(local.tm_mon + 1), uint16_t(local.tm_wday), uint16_t(local.tm_mday),
				uint16_t(local.tm_hour), uint16_t(local.tm_min), uint16_t(local.tm_sec), uint16_t(now.tv_nsec / 1000000)};
			std::memcpy(fields, current, 16);
		}
		std::u16string pattern;
		if (call[format_index])
			pattern = call.wide(call[format_index]);
		else if (date)
			pattern = flags & 2 ? u"dddd, MMMM d, yyyy" : u"M/d/yyyy";
		else {
			pattern = flags & 0x8 ? u"HH:mm:ss" : u"h:mm:ss tt";
			if (flags & 2) pattern = flags & 0x8 ? u"HH:mm" : u"h:mm tt";
			if (flags & 1) pattern = flags & 0x8 ? u"HH" : u"h tt";
			if (flags & 4) {
				const auto marker = pattern.find(u" tt");
				if (marker != pattern.npos) pattern.erase(marker);
			}
		}
		const auto text = picture(pattern, fields, date);
		const int32_t capacity = call.i32(size_index);
		if (!capacity) {
			call.ret(text.size() + 1);
			return;
		}
		if (size_t(capacity) <= text.size()) {
			call.fail(122);
			return;
		}
		call.put_wide(call[buffer_index], text, size_t(capacity));
		call.ret(text.size() + 1);
	};
	registry.add("GetDateFormatW", 6, [format_date_time](ApiCall &call) { format_date_time(call, true, 1, 2, 3, 4, 5); });
	registry.add("GetDateFormatEx", 7, [format_date_time](ApiCall &call) { format_date_time(call, true, 1, 2, 3, 4, 5); });
	registry.add("GetTimeFormatW", 6, [format_date_time](ApiCall &call) { format_date_time(call, false, 1, 2, 3, 4, 5); });
	registry.add("GetTimeFormatEx", 6, [format_date_time](ApiCall &call) { format_date_time(call, false, 1, 2, 3, 4, 5); });

	// GetNumberFormat: a plain number ("-1234.5") laid out with the locale's digits after the separator, grouping and negative form. The
	// locale is the English (United States) one the dates use; a NUMBERFMT the caller passes replaces it as a whole.
	const auto format_number = [](ApiCall &call) {
		const uint32_t flags = call.u32(1);
		const auto value = call.wide(call[2]);
		uint32_t digits = 2, leading_zero = 1, grouping = 3, negative_order = 1;
		std::u16string decimal = u".", thousand = u",";
		if (call[3]) {
			if (flags) {
				call.fail(1004);
				return;
			}
			const size_t width = call.pointer_width();
			digits = call.load<uint32_t>(call[3]);
			leading_zero = call.load<uint32_t>(call[3] + 4);
			grouping = call.load<uint32_t>(call[3] + 8);
			decimal = call.wide(call.load_pointer(call[3] + 12 + (width == 8 ? 4 : 0)), 4);
			thousand = call.wide(call.load_pointer(call[3] + 12 + width + (width == 8 ? 4 : 0)), 4);
			negative_order = call.load<uint32_t>(call[3] + 12 + 2 * width + (width == 8 ? 4 : 0));
			if (digits > 9 || leading_zero > 1 || negative_order > 4 || (grouping > 9 && grouping != 32)) {
				call.fail(87);
				return;
			}
		}
		// The value is digits with an optional leading minus and at most one decimal point.
		const bool negative = !value.empty() && value[0] == u'-';
		std::u16string whole, fraction;
		bool point = false;
		for (size_t index = negative ? 1 : 0; index < value.size(); ++index) {
			const auto character = value[index];
			if (character == u'.' && !point)
				point = true;
			else if (character >= u'0' && character <= u'9')
				(point ? fraction : whole).push_back(character);
			else {
				call.fail(87);
				return;
			}
		}
		if (whole.empty() && fraction.empty()) {
			call.fail(87);
			return;
		}
		// Digits past the ones shown round half up into the rest.
		const bool round_up = fraction.size() > digits && fraction[digits] >= u'5';
		fraction.resize(digits, u'0');
		if (round_up) {
			auto number = whole + fraction;
			size_t index = number.size();
			while (index > 0 && number[index - 1] == u'9')
				number[--index] = u'0';
			if (index == 0)
				number.insert(number.begin(), u'1');
			else
				++number[index - 1];
			whole = number.substr(0, number.size() - digits);
			fraction = number.substr(number.size() - digits);
		}
		whole.erase(0, std::min(whole.find_first_not_of(u'0'), whole.size()));
		std::u16string grouped;
		if (whole.empty()) {
			if (leading_zero || !digits)
				grouped = u"0";
		} else {
			// Grouping 32 is three digits next to the separator and two in every group beyond.
			size_t size = grouping == 32 ? 3 : grouping, count = 0;
			for (size_t index = whole.size(); index > 0; --index) {
				if (size && count == size) {
					grouped.insert(0, thousand);
					count = 0;
					if (grouping == 32)
						size = 2;
				}
				grouped.insert(grouped.begin(), whole[index - 1]);
				++count;
			}
		}
		std::u16string text = grouped + (digits ? decimal + fraction : u"");
		if (negative && text.find_first_not_of(u"0.,") != std::u16string::npos) {
			static constexpr std::u16string_view before[] = {u"(", u"-", u"- ", u"", u""}, after[] = {u")", u"", u"", u"-", u" -"};
			text = std::u16string(before[negative_order]) + text + std::u16string(after[negative_order]);
		}
		const int32_t capacity = call.i32(5);
		if (!capacity) {
			call.ret(text.size() + 1);
			return;
		}
		if (size_t(capacity) <= text.size()) {
			call.fail(122);
			return;
		}
		call.put_wide(call[4], text, size_t(capacity));
		call.ret(text.size() + 1);
	};
	registry.add("GetNumberFormatW", 6, format_number);
	registry.add("GetNumberFormatEx", 6, format_number);

	// The message comes from the text given, a module's message table or the system's; its inserts from an array of arguments or from the
	// caller's va_list, one pointer-wide slot each.
	const auto format_message = [&](ApiCall &call, bool wide) {
		const uint32_t flags = call.u32(0);
		std::optional<std::u16string> text;
		if (flags & 0x400)
			text = wide ? call.wide(call[1]) : decode_ansi(call.narrow(call[1]), 0);
		else {
			if (flags & 0x800)
				text = module_message(context, call[1], call.u32(2), call.u32(3));
			if (!text && (flags & 0x1000))
				text = system_message(call.u32(2));
			if (!text && !(flags & 0x1800)) {
				call.fail(87);
				return;
			}
			if (!text) {
				call.fail(317);
				return;
			}
		}
		if (!(flags & 0x200)) {
			const size_t width = call.pointer_width();
			uint64_t arguments = 0;
			if (call[6])
				arguments = flags & 0x2000 ? call[6] : call.load_pointer(call[6]);
			text = insert_arguments(*text, [&](unsigned index, std::u16string_view format) -> std::u16string {
				if (!arguments)
					return u"";
				uint64_t value = 0;
				try {
					value = width == 4 ? call.load<uint32_t>(arguments + (index - 1) * 4) : call.load<uint64_t>(arguments + (index - 1) * 8);
				} catch (const ApiFault &) {
					return u"";
				}
				const char16_t kind = format.empty() ? u's' : format.back();
				if (kind == u's' || kind == u'S') {
					// A string is wide in the wide call unless the format says h; S swaps the width.
					const bool narrow = format.find(u'h') != format.npos || (format.find(u'l') == format.npos && (wide == (kind == u'S')));
					if (!value)
						return u"(null)";
					try {
						return narrow ? decode_ansi(call.narrow(value, 32768), 0) : call.wide(value, 32768);
					} catch (const ApiFault &) {
						return u"";
					}
				}
				if (kind == u'c' || kind == u'C')
					return std::u16string(1, char16_t(value));
				char digits[32];
				const auto spec = to_utf8(std::u16string(format));
				const bool large = spec.find("I64") != spec.npos || spec.find("ll") != spec.npos;
				std::string pattern = "%";
				pattern += spec.substr(0, spec.size() - 1);
				for (const char *size : {"I64", "ll", "l", "h", "I32"})
					if (const auto found = pattern.find(size); found != pattern.npos)
						pattern.erase(found, std::strlen(size));
				pattern += large ? "ll" : "";
				pattern += char(kind);
				if (large)
					std::snprintf(digits, sizeof digits, pattern.c_str(), (unsigned long long)value);
				else if (kind == u'd' || kind == u'i')
					std::snprintf(digits, sizeof digits, pattern.c_str(), int32_t(value));
				else
					std::snprintf(digits, sizeof digits, pattern.c_str(), uint32_t(value));
				return widen_ascii(digits);
			});
		}
		if (wide) {
			if (flags & 0x100) {
				const auto address = context.allocator.allocate((text->size() + 1) * 2, false);
				if (!address) {
					call.fail(8);
					return;
				}
				call.put_wide(address, *text, text->size() + 1);
				call.store_pointer(call[4], address);
				call.ret(text->size());
				return;
			}
			if (call.u32(5) <= text->size()) {
				call.fail(122);
				return;
			}
			call.put_wide(call[4], *text, call.u32(5));
			call.ret(text->size());
			return;
		}
		const auto narrow = to_utf8(*text);
		if (flags & 0x100) {
			const auto address = context.allocator.allocate(narrow.size() + 1, false);
			if (!address) {
				call.fail(8);
				return;
			}
			call.put_narrow(address, narrow, narrow.size() + 1);
			call.store_pointer(call[4], address);
			call.ret(narrow.size());
			return;
		}
		if (call.u32(5) <= narrow.size()) {
			call.fail(122);
			return;
		}
		call.put_narrow(call[4], narrow, call.u32(5));
		call.ret(narrow.size());
	};
	registry.add("FormatMessageW", 7, [format_message](ApiCall &call) { format_message(call, true); });
	registry.add("FormatMessageA", 7, [format_message](ApiCall &call) { format_message(call, false); });
}

}
