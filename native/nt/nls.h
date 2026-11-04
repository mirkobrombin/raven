#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace raven {

// A Windows single-byte code page other than CP1252: the character of each byte, and Windows' best-fit byte for each character it
// converts, in Unicode order.
struct NativeSingleBytePage {
	uint32_t id;
	const uint16_t *bytes;
	const std::array<uint16_t,2> *best_fit;
	size_t best_fit_count;
};

const NativeSingleBytePage *native_single_byte_page(uint32_t code_page);
std::u16string decode_single_byte(const NativeSingleBytePage &page,std::string_view text);
// The page's byte for a character, best fit unless exact is asked for; nothing when the page has no byte for it.
int encode_single_byte(const NativeSingleBytePage &page,char16_t character,bool exact);

// A Windows double-byte code page: its default characters, its lead byte ranges (pairs ending with zeros), the character of each
// single byte, the slot of each lead byte (0 for one that is not) in the table of 256 trail bytes per lead byte, and Windows'
// best-fit bytes for each character it converts, in Unicode order (a value above 0xff is a lead byte and a trail byte).
struct NativeDoubleBytePage {
	uint32_t id;
	uint16_t default_byte,default_wide;
	const uint8_t *ranges;
	const uint16_t *bytes;
	const uint8_t *slots;
	const uint16_t *pairs;
	const std::array<uint16_t,2> *best_fit;
	size_t best_fit_count;
};

const NativeDoubleBytePage *native_double_byte_page(uint32_t code_page);
// A lead byte, or a lone one at the end, or a pair the page does not define, becomes the default character and sets invalid.
std::u16string decode_double_byte(const NativeDoubleBytePage &page,std::string_view text,bool *invalid=nullptr);
// The page's bytes for a character (a lead byte in the high half when there are two), best fit unless exact is asked for;
// nothing when the page has none.
int encode_double_byte(const NativeDoubleBytePage &page,char16_t character,bool exact);

std::string encode_ansi(std::u16string_view text,uint32_t code_page);
std::string encode_utf8(std::u16string_view text,bool replace_invalid=false);

}
