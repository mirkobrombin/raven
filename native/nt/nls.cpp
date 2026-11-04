#include "nls.h"
#include "nls-cp1252.h"
#include "nls-sbcs.h"
#include "nls-dbcs.h"

#include <algorithm>
#include <stdexcept>

namespace raven {

const NativeSingleBytePage *native_single_byte_page(uint32_t code_page)
{
	for (const auto &page : native_single_byte_pages)
		if (page.id==code_page) return &page;
	return nullptr;
}

std::u16string decode_single_byte(const NativeSingleBytePage &page,std::string_view text)
{
	std::u16string result;
	result.reserve(text.size());
	for (const auto byte : text) result.push_back(char16_t(page.bytes[static_cast<unsigned char>(byte)]));
	return result;
}

int encode_single_byte(const NativeSingleBytePage &page,char16_t character,bool exact)
{
	const auto end=page.best_fit+page.best_fit_count;
	const auto found=std::lower_bound(page.best_fit,end,uint16_t(character),
		[](const std::array<uint16_t,2> &entry,uint16_t value) { return entry[0]<value; });
	if (found==end || (*found)[0]!=character) return -1;
	// A best fit is a byte that reads back as another character.
	if (exact && page.bytes[(*found)[1]]!=character) return -1;
	return (*found)[1];
}

const NativeDoubleBytePage *native_double_byte_page(uint32_t code_page)
{
	for (const auto &page : native_double_byte_pages)
		if (page.id==code_page) return &page;
	return nullptr;
}

std::u16string decode_double_byte(const NativeDoubleBytePage &page,std::string_view text,bool *invalid)
{
	std::u16string result;
	result.reserve(text.size());
	for (size_t at=0;at<text.size();++at) {
		const auto byte=static_cast<unsigned char>(text[at]);
		const auto slot=page.slots[byte];
		if (!slot) { result.push_back(char16_t(page.bytes[byte]));continue; }
		// A lead byte takes the byte after it as its trail, whatever that byte is.
		const auto character=at+1<text.size() ? page.pairs[(slot-1)*256U+static_cast<unsigned char>(text[at+1])] : uint16_t(0);
		if (at+1<text.size()) ++at;
		if (!character && invalid) *invalid=true;
		result.push_back(char16_t(character ? character : page.default_wide));
	}
	return result;
}

int encode_double_byte(const NativeDoubleBytePage &page,char16_t character,bool exact)
{
	const auto end=page.best_fit+page.best_fit_count;
	const auto found=std::lower_bound(page.best_fit,end,uint16_t(character),
		[](const std::array<uint16_t,2> &entry,uint16_t value) { return entry[0]<value; });
	if (found==end || (*found)[0]!=character) return -1;
	const auto bytes=(*found)[1];
	if (exact) {
		const auto back=bytes>0xff ? page.pairs[(page.slots[bytes>>8]-1)*256U+(bytes & 0xff)] : page.bytes[bytes];
		if (back!=character) return -1;
	}
	return bytes;
}

std::string encode_ansi(std::u16string_view text,uint32_t code_page)
{
	std::string result;
	result.reserve(text.size());
	if (const auto *page=native_single_byte_page(code_page)) {
		for (const auto character : text) {
			const auto byte=encode_single_byte(*page,character,false);
			result.push_back(byte<0 ? '?' : static_cast<char>(byte));
		}
		return result;
	}
	if (const auto *page=native_double_byte_page(code_page)) {
		for (const auto character : text) {
			const auto bytes=encode_double_byte(*page,character,false);
			if (bytes<0) result.push_back(static_cast<char>(page->default_byte));
			else if (bytes>0xff) { result.push_back(static_cast<char>(bytes>>8));result.push_back(static_cast<char>(bytes)); }
			else result.push_back(static_cast<char>(bytes));
		}
		return result;
	}
	if (code_page!=1252) throw std::runtime_error("unsupported native ANSI code page");
	for (const auto character : text) result.push_back(static_cast<char>(native_cp1252[character]));
	return result;
}

}
