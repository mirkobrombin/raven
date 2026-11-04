#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace raven {

// Lossless UTF-16 <-> UTF-8; lone surrogates become U+FFFD so host paths and messages stay valid text.
std::string to_utf8(std::u16string_view text);
std::u16string from_utf8(std::string_view text);
// Windows compares names with simple case mapping, never locale rules.
char16_t fold_case(char16_t character);
char16_t upper_case(char16_t character);
bool equal_fold(std::u16string_view left, std::u16string_view right);
int compare_fold(std::u16string_view left, std::u16string_view right);
std::u16string widen_ascii(std::string_view text);
// ANSI text of the process code page (1252, 437, ...) as UTF-16; unmapped bytes become U+FFFD.
std::u16string from_ansi(std::string_view text, uint32_t code_page);
// Inverse of from_ansi; characters the code page lacks become '?'.
std::string to_ansi(std::u16string_view text, uint32_t code_page);

// A Unicode normalization form of the text (UAX #15); no value when it holds a lone surrogate.
std::optional<std::u16string> normalize_unicode(std::u16string_view text, bool composed, bool compatibility);

// What GetLocaleInfo answers, for the guest to read: an entry of 8 bytes for each type below 0x80 and then for 0x1000 to 0x100f (the
// offset of its text in characters, or 0xffff for a type without one, the length without the terminator and the text as a number),
// then the texts, each with its terminator. Without names the locale's own name and its parent's are left out, for GetLocaleInfoEx.
constexpr uint32_t locale_table_entries = 0x90;
std::vector<uint8_t> locale_table(bool names = false);

}
