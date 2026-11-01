#include "nt/text.h"

#include <algorithm>
#include <iterator>

namespace raven {
namespace {

struct Expansion { uint32_t character, offset, length; };
struct ClassRange { uint32_t first, last, value; };
struct Composition { uint32_t first, second, result; };

#include "upstream/unicode/normalization.inc"

// The Hangul syllables are not in the tables: UAX #15 gives them by arithmetic.
constexpr uint32_t s_base = 0xac00, l_base = 0x1100, v_base = 0x1161, t_base = 0x11a7;
constexpr uint32_t l_count = 19, v_count = 21, t_count = 28, n_count = v_count * t_count, s_count = l_count * n_count;

uint32_t combining_class(uint32_t character)
{
	const auto found = std::upper_bound(std::begin(class_ranges), std::end(class_ranges), character,
		[](uint32_t value, const ClassRange &range) { return value < range.first; });
	if (found == std::begin(class_ranges))
		return 0;
	const auto &range = *std::prev(found);
	return character <= range.last ? range.value : 0;
}

template<size_t size>
const Expansion *find_expansion(const Expansion (&table)[size], uint32_t character)
{
	const auto found = std::lower_bound(std::begin(table), std::end(table), character,
		[](const Expansion &entry, uint32_t value) { return entry.character < value; });
	return found != std::end(table) && found->character == character ? found : nullptr;
}

void decompose(uint32_t character, bool compatibility, std::u32string &out)
{
	if (character >= s_base && character < s_base + s_count) {
		const uint32_t index = character - s_base;
		out.push_back(l_base + index / n_count);
		out.push_back(v_base + index % n_count / t_count);
		if (index % t_count)
			out.push_back(t_base + index % t_count);
		return;
	}
	const Expansion *entry = compatibility ? find_expansion(compatibility_expansions, character) : nullptr;
	if (!entry)
		entry = find_expansion(canonical_expansions, character);
	if (!entry) {
		out.push_back(character);
		return;
	}
	out.append(reinterpret_cast<const char32_t *>(expansion_pool + entry->offset), entry->length);
}

void order_marks(std::u32string &text)
{
	for (size_t start = 0; start < text.size();) {
		if (!combining_class(text[start])) {
			++start;
			continue;
		}
		size_t end = start;
		while (end < text.size() && combining_class(text[end]))
			++end;
		std::stable_sort(text.begin() + start, text.begin() + end,
			[](char32_t left, char32_t right) { return combining_class(left) < combining_class(right); });
		start = end;
	}
}

uint32_t compose_pair(uint32_t first, uint32_t second)
{
	if (first >= l_base && first < l_base + l_count && second >= v_base && second < v_base + v_count)
		return s_base + ((first - l_base) * v_count + (second - v_base)) * t_count;
	if (first >= s_base && first < s_base + s_count && !((first - s_base) % t_count) && second > t_base && second < t_base + t_count)
		return first + (second - t_base);
	const auto found = std::lower_bound(std::begin(compositions), std::end(compositions), std::pair{first, second},
		[](const Composition &entry, const std::pair<uint32_t, uint32_t> &value) {
			return std::pair{entry.first, entry.second} < value;
		});
	return found != std::end(compositions) && found->first == first && found->second == second ? found->result : 0;
}

void compose(std::u32string &text)
{
	if (text.empty())
		return;
	size_t written = 0, starter = text.npos;
	uint32_t last_class = 0;
	for (size_t index = 0; index < text.size(); ++index) {
		const uint32_t character = text[index], current = combining_class(character);
		// A mark can join the starter unless a mark of the same or a higher class stands between them.
		const bool blocked = written && starter != written - 1 && last_class >= current;
		if (starter != text.npos && !blocked) {
			if (const uint32_t joined = compose_pair(text[starter], character)) {
				text[starter] = joined;
				continue;
			}
		}
		if (!current)
			starter = written;
		last_class = current;
		text[written++] = character;
	}
	text.resize(written);
}

}

std::optional<std::u16string> normalize_unicode(std::u16string_view text, bool composed, bool compatibility)
{
	std::u32string decomposed;
	decomposed.reserve(text.size());
	for (size_t index = 0; index < text.size(); ++index) {
		uint32_t character = text[index];
		if (character >= 0xdc00 && character <= 0xdfff)
			return std::nullopt;
		if (character >= 0xd800 && character <= 0xdbff) {
			if (index + 1 == text.size() || text[index + 1] < 0xdc00 || text[index + 1] > 0xdfff)
				return std::nullopt;
			character = 0x10000 + ((character - 0xd800) << 10) + (text[++index] - 0xdc00);
		}
		decompose(character, compatibility, decomposed);
	}
	order_marks(decomposed);
	if (composed)
		compose(decomposed);
	std::u16string result;
	result.reserve(decomposed.size());
	for (const uint32_t character : decomposed) {
		if (character < 0x10000) {
			result.push_back(char16_t(character));
		} else {
			result.push_back(char16_t(0xd800 + ((character - 0x10000) >> 10)));
			result.push_back(char16_t(0xdc00 + ((character - 0x10000) & 0x3ff)));
		}
	}
	return result;
}

}
