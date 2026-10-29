#include "kernel_context.h"
#include "text.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>
#include <sys/stat.h>

namespace raven {

namespace {

constexpr size_t max_path = 260;
// The text the searching functions look through may be a whole document: it is read to its end, however long.
constexpr size_t whole_text = size_t(1) << 30;

bool separator(char16_t character)
{
	return character == u'\\' || character == u'/';
}

// Index of the file name inside a path: after the last separator or drive colon.
size_t file_name_start(std::u16string_view path)
{
	size_t start = 0;
	// A separator or colon starts the name only when a name follows it: "C:" and "C:\" are their own file names, as Windows answers.
	for (size_t at = 0; at + 1 < path.size(); ++at)
		if ((separator(path[at]) || path[at] == u':') && !separator(path[at + 1]))
			start = at + 1;
	return start;
}

// The scheme of a URL: a letter, then letters, digits, '+', '-' or '.', before a colon. A single letter is a drive, not a scheme.
size_t scheme_length(std::u16string_view text)
{
	size_t at = 0;
	const auto letter = [](char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); };
	while (at < text.size() && (letter(text[at]) || (at && ((text[at] >= u'0' && text[at] <= u'9') || text[at] == u'+' || text[at] == u'-' ||
		text[at] == u'.'))))
		++at;
	return at >= 2 && at < text.size() && text[at] == u':' ? at : 0;
}

// What UrlIs tells of a text: a URL, an opaque one (nothing hierarchical after the scheme), a file URL, a directory.
bool url_is(std::u16string_view text, uint32_t kind)
{
	const size_t scheme = scheme_length(text);
	switch (kind) {
	case 0:
		return scheme;
	case 1:
		return scheme && (scheme + 1 == text.size() || !separator(text[scheme + 1]));
	case 3: {
		std::u16string lower(text.substr(0, scheme));
		std::transform(lower.begin(), lower.end(), lower.begin(), [](char16_t c) { return c >= u'A' && c <= u'Z' ? char16_t(c + 32) : c; });
		return lower == u"file";
	}
	case 5:
		return !text.empty() && separator(text.back());
	default:
		return false;
	}
}

size_t extension_start(std::u16string_view path)
{
	// The last dot after the last separator, a trailing one included.
	size_t dot = std::u16string_view::npos;
	for (size_t at = 0; at < path.size(); ++at)
		if (separator(path[at]) || (path[at] == u':' && at == 1))
			dot = std::u16string_view::npos;
		else if (path[at] == u'.')
			dot = at;
	return dot == std::u16string_view::npos ? path.size() : dot;
}

// Length of the root part: "X:\", "\\server\share\" or a single leading separator.
size_t root_length(std::u16string_view path)
{
	if (path.size() >= 3 && path[1] == u':' && separator(path[2]))
		return 3;
	if (path.size() >= 2 && separator(path[0]) && separator(path[1])) {
		size_t at = 2, parts = 0;
		while (at < path.size() && parts < 2) {
			while (at < path.size() && !separator(path[at]))
				++at;
			++parts;
			if (at < path.size() && parts < 2)
				++at;
		}
		if (at < path.size())
			++at;
		return at;
	}
	if (!path.empty() && separator(path[0]))
		return 1;
	return 0;
}

bool is_unc_root(std::u16string_view path)
{
	return path.size() >= 2 && separator(path[0]) && separator(path[1]);
}

std::u16string canonical(std::u16string_view path)
{
	const size_t root = root_length(path);
	std::u16string result(path.substr(0, root));
	for (auto &character : result)
		if (character == u'/')
			character = u'\\';
	std::vector<std::u16string> parts;
	size_t at = root;
	const bool trailing = !path.empty() && separator(path.back()) && path.size() > root;
	while (at <= path.size()) {
		size_t end = at;
		while (end < path.size() && !separator(path[end]))
			++end;
		const auto part = path.substr(at, end - at);
		if (part == u"..") {
			if (!parts.empty())
				parts.pop_back();
		} else if (!part.empty() && part != u".") {
			parts.emplace_back(part);
		}
		if (end >= path.size())
			break;
		at = end + 1;
	}
	for (size_t index = 0; index < parts.size(); ++index) {
		if (!result.empty() && result.back() != u'\\')
			result.push_back(u'\\');
		result += parts[index];
	}
	if (trailing && !parts.empty())
		result.push_back(u'\\');
	if (result.empty())
		result = u"\\";
	return result;
}

std::u16string join(std::u16string_view directory, std::u16string_view file)
{
	if (directory.empty())
		return std::u16string(file);
	if (file.empty())
		return std::u16string(directory);
	std::u16string result(directory);
	if (!separator(result.back()))
		result.push_back(u'\\');
	result += separator(file.front()) ? file.substr(1) : file;
	return result;
}

bool absolute(std::u16string_view path)
{
	return root_length(path) > 0;
}

bool match_one(std::u16string_view spec, std::u16string_view name)
{
	size_t p = 0, n = 0, star = std::u16string_view::npos, mark = 0;
	while (n < name.size()) {
		if (p < spec.size() && (spec[p] == u'?' || equal_fold(spec.substr(p, 1), name.substr(n, 1)))) {
			++p;
			++n;
		} else if (p < spec.size() && spec[p] == u'*') {
			star = p++;
			mark = n;
		} else if (star != std::u16string_view::npos) {
			p = star + 1;
			n = ++mark;
		} else {
			return false;
		}
	}
	while (p < spec.size() && spec[p] == u'*')
		++p;
	return p == spec.size();
}

// HLS in the 0..240 range of shlwapi, from 8-bit RGB.
void rgb_to_hls(uint32_t rgb, uint16_t &hue, uint16_t &luminance, uint16_t &saturation)
{
	const int r = rgb & 255, g = (rgb >> 8) & 255, b = (rgb >> 16) & 255;
	const int maximum = std::max({r, g, b}), minimum = std::min({r, g, b});
	const int sum = maximum + minimum, difference = maximum - minimum;
	luminance = uint16_t(((sum * 240) + 255) / 510);
	if (!difference) {
		saturation = 0;
		hue = 160;
		return;
	}
	saturation = uint16_t(luminance <= 120 ? (difference * 240 + sum / 2) / sum : (difference * 240 + (510 - sum) / 2) / (510 - sum));
	const int rc = ((maximum - r) * 40 + difference / 2) / difference, gc = ((maximum - g) * 40 + difference / 2) / difference,
		bc = ((maximum - b) * 40 + difference / 2) / difference;
	int result = r == maximum ? bc - gc : g == maximum ? 80 + rc - bc : 160 + gc - rc;
	if (result < 0)
		result += 240;
	if (result > 240)
		result -= 240;
	hue = uint16_t(result);
}

int hue_component(int n1, int n2, int hue)
{
	if (hue < 0) hue += 240;
	if (hue > 240) hue -= 240;
	if (hue < 40) return n1 + ((n2 - n1) * hue + 20) / 40;
	if (hue < 120) return n2;
	if (hue < 160) return n1 + ((n2 - n1) * (160 - hue) + 20) / 40;
	return n1;
}

uint32_t hls_to_rgb(int hue, int luminance, int saturation)
{
	int r, g, b;
	if (!saturation) {
		r = g = b = luminance * 255 / 240;
	} else {
		const int m2 = luminance <= 120 ? (luminance * (240 + saturation) + 120) / 240 : luminance + saturation - (luminance * saturation + 120) / 240;
		const int m1 = 2 * luminance - m2;
		const auto scale = [](int value) { return std::clamp((value * 255 + 120) / 240, 0, 255); };
		r = scale(hue_component(m1, m2, hue + 80));
		g = scale(hue_component(m1, m2, hue));
		b = scale(hue_component(m1, m2, hue - 80));
	}
	return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16;
}

std::u16string compact(std::u16string_view path, size_t capacity)
{
	if (path.size() < capacity)
		return std::u16string(path);
	if (capacity < 4)
		return std::u16string(path.substr(0, capacity ? capacity - 1 : 0));
	const size_t name = file_name_start(path);
	const auto file = path.substr(name);
	const std::u16string_view ellipsis = u"...";
	if (file.size() + ellipsis.size() + 1 >= capacity) {
		std::u16string result(file.substr(0, capacity - 1 - ellipsis.size()));
		return result + std::u16string(ellipsis);
	}
	const size_t room = capacity - 1 - ellipsis.size() - file.size();
	std::u16string result(path.substr(0, std::min(room, name)));
	return result + std::u16string(ellipsis) + std::u16string(file);
}

}

void register_shlwapi_api(ApiRegistry &registry, KernelContext &context)
{
	auto &paths = context.paths;
	// Path functions edit a MAX_PATH buffer in place; this reads it, applies the edit and writes it back.
	const auto edit = [](ApiCall &call, const std::function<bool(std::u16string &)> &change) {
		auto text = call.wide(call[0], max_path * 4);
		const bool changed = change(text);
		call.put_wide(call[0], text, max_path * 4);
		call.ret(changed);
	};
	// The system the shell reports is a Windows 10 Pro workstation; a 32-bit program also learns that it runs under WOW64, which installers
	// read before they turn to the 64-bit registry view and program folder.
	registry.add("IsOS", 1, [](ApiCall &call) {
		switch (call.u32(0)) {
		case 1: case 2: case 3: case 5: case 7: case 8: case 17: case 18: case 20: case 26: case 27:
			call.ret(1);
			break;
		case 30:
			call.ret(call.pointer_width() == 4);
			break;
		default:
			call.ret(0);
		}
	});
	// Completion lists are drawn by the shell; an edit control without one still works, so enabling it succeeds without a list.
	registry.add("SHAutoComplete", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("PathRemoveFileSpecW", 1, [edit](ApiCall &call) {
		edit(call, [](std::u16string &path) {
			const size_t root = root_length(path);
			if (path.empty() || (path.size() == root && root))
				return false;
			const auto last = path.find_last_of(u"\\/");
			if (last == path.npos) {
				if (path.size() > 2 && path[1] == u':') {
					path.resize(2);
					return true;
				}
				path.clear();
				return true;
			}
			if (is_unc_root(path) && last < root) {
				if (last + 1 == path.size() && path.size() > root - 1 && root == path.size())
					return false;
			}
			if (last + 1 == path.size()) {
				path.pop_back();
				return true;
			}
			if (last < root && root)
				path.resize(root);
			else
				path.resize(last);
			return true;
		});
	});
	registry.add("PathStripPathW", 1, [edit](ApiCall &call) {
		edit(call, [](std::u16string &path) {
			path.erase(0, file_name_start(path));
			return true;
		});
	});
	registry.add("PathRemoveExtensionW", 1, [edit](ApiCall &call) {
		edit(call, [](std::u16string &path) {
			path.resize(extension_start(path));
			return true;
		});
	});
	registry.add("PathAddExtensionW", 2, [](ApiCall &call) {
		auto path = call.wide(call[0], max_path * 4);
		const auto extension = call[1] ? call.wide(call[1]) : u".exe";
		if (extension_start(path) != path.size()) {
			call.ret(0);
			return;
		}
		if (path.size() + extension.size() >= max_path) {
			call.ret(0);
			return;
		}
		path += extension;
		call.put_wide(call[0], path, max_path);
		call.ret(1);
	});
	registry.add("PathAppendW", 2, [](ApiCall &call) {
		auto path = call.wide(call[0], max_path * 4);
		const auto more = call.wide(call[1]);
		auto joined = canonical(join(path, more));
		if (joined.size() >= max_path) {
			call.ret(0);
			return;
		}
		call.put_wide(call[0], joined, max_path);
		call.ret(1);
	});
	registry.add("PathCombineW", 3, [](ApiCall &call) {
		const auto directory = call[1] ? call.wide(call[1]) : std::u16string();
		const auto file = call[2] ? call.wide(call[2]) : std::u16string();
		const auto combined = absolute(file) && !(separator(file[0]) && !is_unc_root(file)) ? file : join(directory, file);
		const auto result = canonical(combined);
		if (result.size() >= max_path) {
			call.ret(0);
			return;
		}
		call.put_wide(call[0], result, max_path);
		call.ret(call[0]);
	});
	registry.add("PathCanonicalizeW", 2, [](ApiCall &call) {
		const auto result = canonical(call.wide(call[1]));
		if (result.size() >= max_path) {
			call.fail(206);
			return;
		}
		call.put_wide(call[0], result, max_path);
		call.ret(1);
	});
	registry.add("PathIsRelativeW", 1, [](ApiCall &call) {
		const auto path = call.wide(call[0]);
		call.ret(path.empty() || !(root_length(path) > 0));
	});
	// The path cut back to its root, "C:\" or "\\server\share\"; a path without one is emptied and fails.
	registry.add("PathStripToRootW", 1, [](ApiCall &call) {
		const auto path = call.wide(call[0]);
		const size_t root = root_length(path);
		call.put_wide(call[0], path.substr(0, root), root + 1);
		call.ret(root > 0);
	});
	registry.add("PathStripToRootA", 1, [](ApiCall &call) {
		const auto path = call.narrow(call[0]);
		const size_t root = root_length(widen_ascii(path));
		call.put_narrow(call[0], path.substr(0, root), root + 1);
		call.ret(root > 0);
	});
	// %XX sequences become the characters they stand for; URL_DONT_UNESCAPE_EXTRA_INFO keeps what follows '?' or '#' as it is, and
	// URL_UNESCAPE_INPLACE writes over the URL itself.
	const auto unescape = [](std::u16string url, uint32_t flags) {
		std::u16string result;
		const auto digit = [](char16_t c) { return c >= u'0' && c <= u'9' ? c - u'0' : c >= u'a' && c <= u'f' ? c - u'a' + 10 : c >= u'A' && c <= u'F' ? c - u'A' + 10 : -1; };
		bool rest = false;
		for (size_t at = 0; at < url.size(); ++at) {
			if ((flags & 0x02000000U) && (url[at] == u'?' || url[at] == u'#'))
				rest = true;
			if (!rest && url[at] == u'%' && at + 2 < url.size() + 0 && digit(url[at + 1]) >= 0 && digit(url[at + 2]) >= 0) {
				result.push_back(char16_t(digit(url[at + 1]) * 16 + digit(url[at + 2])));
				at += 2;
			} else {
				result.push_back(url[at]);
			}
		}
		return result;
	};
	const auto url_unescape = [unescape](ApiCall &call, bool wide) {
		const auto url = wide ? call.wide(call[0]) : widen_ascii(call.narrow(call[0]));
		const auto result = unescape(url, call.u32(3));
		const auto store = [&](uint64_t address, size_t capacity) {
			if (wide)
				call.put_wide(address, result, capacity);
			else
				call.put_narrow(address, std::string(result.begin(), result.end()), capacity);
		};
		if (call.u32(3) & 0x00100000U) {
			store(call[0], result.size() + 1);
			call.ret(0);
			return;
		}
		if (!call[1] || !call[2]) {
			call.ret(0x80070057U);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[2]);
		if (capacity <= result.size()) {
			call.store<uint32_t>(call[2], uint32_t(result.size() + 1));
			call.ret(0x80004003U);
			return;
		}
		store(call[1], capacity);
		call.store<uint32_t>(call[2], uint32_t(result.size()));
		call.ret(0);
	};
	// A bare name is looked for as SearchPath finds programs; anything else becomes a full path. Both write at most the given length.
	registry.add("PathSearchAndQualifyW", 3, [&registry](ApiCall &call) {
		const auto path = call.wide(call[0]);
		if (path.find_first_of(u"\\/:") == std::u16string::npos)
			if (const auto search = registry.find("SearchPathW")) {
				ApiCall inner(call.process, call.environment, {0, call[0], 0, call[2], call[1], 0});
				search->second(inner);
				if (inner.result && inner.result < call.u32(2)) {
					call.ret(1);
					return;
				}
			}
		const auto full = registry.find("GetFullPathNameW");
		ApiCall inner(call.process, call.environment, {call[0], call[2], call[1], 0});
		full->second(inner);
		call.ret(inner.result && inner.result < call.u32(2));
	});
	// A scheme for text that has none: a Windows path becomes a file URL (URL_APPLY_GUESSFILE), "www." and "ftp." names get their scheme
	// (URL_APPLY_GUESSSCHEME), anything else http (URL_APPLY_DEFAULT). Text that has a scheme already is left as it is (S_FALSE).
	const auto apply_scheme = [](std::u16string text, uint32_t flags) -> std::optional<std::u16string> {
		if (scheme_length(text) && !(flags & 8))
			return std::nullopt;
		const bool drive = text.size() >= 2 && text[1] == u':' && ((text[0] >= u'a' && text[0] <= u'z') || (text[0] >= u'A' && text[0] <= u'Z'));
		if ((flags & 4) && (drive || (text.size() >= 2 && separator(text[0]) && separator(text[1])))) {
			std::u16string url = drive ? u"file:///" : u"file:";
			for (const auto character : text)
				url.push_back(character == u'\\' ? u'/' : character);
			return url;
		}
		if (flags & 2) {
			std::u16string lower(text.substr(0, 4));
			for (auto &character : lower)
				if (character >= u'A' && character <= u'Z')
					character = char16_t(character + 32);
			if (lower == u"www.")
				return u"http://" + text;
			if (lower == u"ftp.")
				return u"ftp://" + text;
		}
		if (flags & 1)
			return u"http://" + text;
		return std::nullopt;
	};
	const auto url_apply_scheme = [apply_scheme](ApiCall &call, bool wide) {
		const auto text = wide ? call.wide(call[0]) : widen_ascii(call.narrow(call[0]));
		const auto result = apply_scheme(text, call.u32(3));
		if (!result) {
			call.ret(1);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[2]);
		if (capacity <= result->size()) {
			call.store<uint32_t>(call[2], uint32_t(result->size() + 1));
			call.ret(0x80004003U);
			return;
		}
		if (wide)
			call.put_wide(call[1], *result, capacity);
		else
			call.put_narrow(call[1], std::string(result->begin(), result->end()), capacity);
		call.store<uint32_t>(call[2], uint32_t(result->size()));
		call.ret(0);
	};
	// The standard form of a URL. A Windows path becomes a file URL, "file:///C:/dir/name" with forward slashes, or with
	// URL_FILE_USE_PATHURL "file://C:\dir\name"; the scheme is written in lower case; "." and ".." steps of the path are resolved unless
	// URL_DONT_SIMPLIFY; URL_UNESCAPE decodes %XX sequences and URL_ESCAPE_SPACES_ONLY writes spaces as %20.
	const auto canonicalize = [unescape](std::u16string text, uint32_t flags) {
		const bool drive = text.size() >= 2 && text[1] == u':' && ((text[0] >= u'a' && text[0] <= u'z') || (text[0] >= u'A' && text[0] <= u'Z'));
		const bool unc = text.size() >= 2 && separator(text[0]) && separator(text[1]);
		const bool path_url = flags & 0x00010000U;
		std::u16string url;
		size_t path_start = 0;
		if (!scheme_length(text) && (drive || unc)) {
			url = path_url ? u"file://" : drive ? u"file:///" : u"file:";
			path_start = url.size();
			for (const auto character : text)
				url.push_back(path_url ? (character == u'/' ? u'\\' : character) : character == u'\\' ? u'/' : character);
		} else {
			url = text;
			const size_t scheme = scheme_length(url);
			for (size_t at = 0; at < scheme; ++at)
				if (url[at] >= u'A' && url[at] <= u'Z')
					url[at] = char16_t(url[at] + 32);
			path_start = scheme ? scheme + 1 : 0;
			while (path_start < url.size() && path_start < scheme + 3 && url[path_start] == u'/')
				++path_start;
			if (scheme && url.compare(0, scheme, u"file") == 0 && path_url) {
				std::u16string path = url.substr(path_start);
				for (auto &character : path)
					if (character == u'/')
						character = u'\\';
				url = u"file://" + path;
				path_start = 7;
			}
		}
		if (!(flags & 0x08000000U)) {
			const char16_t slash = path_url ? u'\\' : u'/';
			const size_t end = std::min(url.size(), url.find_first_of(u"?#", path_start));
			std::vector<std::u16string> steps;
			size_t at = path_start;
			while (at <= end) {
				size_t next = url.find(slash, at);
				if (next == std::u16string::npos || next > end)
					next = end;
				const auto step = url.substr(at, next - at);
				if (step == u"..") {
					if (steps.size() > 1)
						steps.pop_back();
				} else if (step != u".") {
					steps.push_back(step);
				}
				at = next + 1;
			}
			std::u16string path;
			for (size_t index = 0; index < steps.size(); ++index) {
				if (index)
					path.push_back(slash);
				path += steps[index];
			}
			url = url.substr(0, path_start) + path + url.substr(end);
		}
		if (flags & 0x10000000U)
			url = unescape(url, flags);
		if (flags & 0x04000000U) {
			std::u16string escaped;
			for (const auto character : url)
				escaped += character == u' ' ? std::u16string(u"%20") : std::u16string(1, character);
			url = escaped;
		}
		return url;
	};
	const auto url_canonicalize = [canonicalize](ApiCall &call, bool wide) {
		if (!call[0] || !call[1] || !call[2]) {
			call.ret(0x80070057U);
			return;
		}
		const auto text = wide ? call.wide(call[0]) : widen_ascii(call.narrow(call[0]));
		const auto result = canonicalize(text, call.u32(3));
		const uint32_t capacity = call.load<uint32_t>(call[2]);
		if (capacity <= result.size()) {
			call.store<uint32_t>(call[2], uint32_t(result.size() + 1));
			call.ret(0x80004003U);
			return;
		}
		if (wide)
			call.put_wide(call[1], result, capacity);
		else
			call.put_narrow(call[1], std::string(result.begin(), result.end()), capacity);
		call.store<uint32_t>(call[2], uint32_t(result.size()));
		call.ret(0);
	};
	// A relative URL joined to a base: one with a scheme of its own stands alone, one that starts with a slash replaces the base's path
	// after its host, anything else replaces the base's last step. The result is canonicalized with the same flags.
	const auto url_combine = [canonicalize](ApiCall &call, bool wide) {
		if (!call[0] || !call[1] || !call[3]) {
			call.ret(0x80070057U);
			return;
		}
		const auto text = [&](uint64_t address) { return wide ? call.wide(address) : widen_ascii(call.narrow(address)); };
		const uint32_t flags = call.u32(4);
		const auto relative = text(call[1]);
		std::u16string joined;
		// A full Windows path, "C:\..." or "\\server\...", is a file URL of its own.
		const bool drive = relative.size() >= 2 && relative[1] == u':' && ((relative[0] >= u'a' && relative[0] <= u'z') || (relative[0] >= u'A' && relative[0] <= u'Z'));
		const bool unc = relative.size() >= 2 && separator(relative[0]) && separator(relative[1]);
		if (scheme_length(relative) || drive || unc)
			joined = relative;
		else {
			const auto base = canonicalize(text(call[0]), flags & ~0x14000000U);
			const char16_t slash = (flags & 0x00010000U) ? u'\\' : u'/';
			size_t path = scheme_length(base);
			path = path ? path + 1 : 0;
			if (base.compare(path, 2, u"//") == 0) {
				const size_t host_end = base.find_first_of(u"/\\", path + 2);
				path = host_end == std::u16string::npos ? base.size() : host_end;
				if (base.compare(0, 8, u"file:///") == 0)
					path = base.find_first_of(u"/\\", 8) == std::u16string::npos ? base.size() : base.find_first_of(u"/\\", 8);
			}
			if (relative.empty())
				joined = base;
			else if (separator(relative[0]))
				joined = base.substr(0, path) + relative;
			else {
				const size_t end = std::min(base.size(), base.find_first_of(u"?#"));
				const size_t last = base.find_last_of(u"/\\", end ? end - 1 : 0);
				joined = (last == std::u16string::npos || last < path ? base.substr(0, end) + slash : base.substr(0, last + 1)) + relative;
			}
		}
		const auto result = canonicalize(joined, flags);
		const uint32_t capacity = call.load<uint32_t>(call[3]);
		if (!call[2] || capacity <= result.size()) {
			call.store<uint32_t>(call[3], uint32_t(result.size() + 1));
			call.ret(0x80004003U);
			return;
		}
		if (wide)
			call.put_wide(call[2], result, capacity);
		else
			call.put_narrow(call[2], std::string(result.begin(), result.end()), capacity);
		call.store<uint32_t>(call[3], uint32_t(result.size()));
		call.ret(0);
	};
	registry.add("UrlCombineW", 5, [url_combine](ApiCall &call) { url_combine(call, true); });
	registry.add("UrlCombineA", 5, [url_combine](ApiCall &call) { url_combine(call, false); });
	// Writes a result into a caller's buffer whose length in characters is behind a pointer, as the Url* functions do: too small a
	// buffer gets the length needed, terminator included, and E_POINTER.
	const auto put_url = [](ApiCall &call, bool wide, uint64_t buffer, uint64_t length, const std::u16string &result) {
		const uint32_t capacity = call.load<uint32_t>(length);
		if (!buffer || capacity <= result.size()) {
			call.store<uint32_t>(length, uint32_t(result.size() + 1));
			call.ret(0x80004003U);
			return;
		}
		if (wide)
			call.put_wide(buffer, result, capacity);
		else
			call.put_narrow(buffer, std::string(result.begin(), result.end()), capacity);
		call.store<uint32_t>(length, uint32_t(result.size()));
		call.ret(0);
	};
	// Characters a URL cannot hold as they are become %XX: controls, spaces, non-ASCII and " # % < > [ \ ] ^ ` { | } after the scheme and
	// host. '%' itself only with URL_ESCAPE_PERCENT; URL_ESCAPE_SPACES_ONLY escapes nothing else; URL_ESCAPE_SEGMENT_ONLY takes the
	// whole text as one path segment, so '/', '?' and '#' are escaped too. What follows '?' or '#' is left alone otherwise.
	const auto url_escape = [put_url](ApiCall &call, bool wide) {
		if (!call[0] || !call[2]) {
			call.ret(0x80070057U);
			return;
		}
		const auto url = wide ? call.wide(call[0]) : widen_ascii(call.narrow(call[0]));
		const uint32_t flags = call.u32(3);
		const bool segment = flags & 0x00002000U, spaces = flags & 0x04000000U, percent = flags & 0x00001000U;
		size_t start = 0;
		if (!segment) {
			start = scheme_length(url);
			start = start ? start + 1 : 0;
			if (url.compare(start, 2, u"//") == 0) {
				const size_t host = url.find(u'/', start + 2);
				start = host == std::u16string::npos ? url.size() : host;
			}
		}
		std::u16string result = url.substr(0, start);
		bool rest = false;
		for (size_t at = start; at < url.size(); ++at) {
			const char16_t character = url[at];
			if (!segment && (character == u'?' || character == u'#'))
				rest = true;
			const bool unsafe = spaces ? character == u' ' : character <= u' ' || character >= 0x7f ||
				std::u16string_view(u"\"<>[\\]^`{|}").find(character) != std::u16string_view::npos ||
				(percent && character == u'%') || (segment && std::u16string_view(u"/?#").find(character) != std::u16string_view::npos);
			if (rest || !unsafe || character > 0xff) {
				result.push_back(character);
				continue;
			}
			static constexpr char hex[] = "0123456789ABCDEF";
			result += u'%';
			result += char16_t(hex[character >> 4]);
			result += char16_t(hex[character & 15]);
		}
		if (flags & 0x00080000U) {
			call.put_wide(call[0], result, result.size() + 1);
			call.ret(0);
			return;
		}
		put_url(call, wide, call[1], call[2], result);
	};
	// One part of a URL "scheme://user:password@host:port/path?query": URL_PART_SCHEME, _HOSTNAME, _USERNAME, _PASSWORD, _PORT or _QUERY.
	// URL_PARTFLAG_KEEPSCHEME puts the scheme and a colon before the part. A part the URL does not have is empty, with S_FALSE.
	const auto url_get_part = [put_url](ApiCall &call, bool wide) {
		if (!call[0] || !call[1] || !call[2] || call.u32(3) < 1 || call.u32(3) > 6) {
			call.ret(0x80070057U);
			return;
		}
		const auto url = wide ? call.wide(call[0]) : widen_ascii(call.narrow(call[0]));
		const size_t scheme = scheme_length(url);
		if (!scheme) {
			call.ret(0x80070057U);
			return;
		}
		std::u16string part;
		const uint32_t which = call.u32(3);
		if (which == 1)
			part = url.substr(0, scheme);
		else if (which == 6) {
			const size_t query = url.find(u'?');
			if (query != std::u16string::npos)
				part = url.substr(query + 1, url.find(u'#', query) == std::u16string::npos ? std::u16string::npos : url.find(u'#', query) - query - 1);
		} else if (url.compare(scheme + 1, 2, u"//") == 0) {
			const size_t start = scheme + 3;
			const size_t end = std::min(url.size(), url.find_first_of(u"/\\?#", start));
			const auto authority = url.substr(start, end - start);
			const size_t at = authority.rfind(u'@');
			const auto credentials = at == std::u16string::npos ? std::u16string() : authority.substr(0, at);
			const auto host = at == std::u16string::npos ? authority : authority.substr(at + 1);
			const size_t colon = host.find(u':'), separator = credentials.find(u':');
			if (which == 2)
				part = host.substr(0, colon);
			else if (which == 5 && colon != std::u16string::npos)
				part = host.substr(colon + 1);
			else if (which == 3)
				part = credentials.substr(0, separator);
			else if (which == 4 && separator != std::u16string::npos)
				part = credentials.substr(separator + 1);
		}
		if ((call.u32(4) & 1) && which != 1 && !part.empty())
			part = url.substr(0, scheme) + u":" + part;
		put_url(call, wide, call[1], call[2], part);
		if (part.empty() && call.result == 0)
			call.ret(1);
	};
	registry.add("UrlGetPartW", 5, [url_get_part](ApiCall &call) { url_get_part(call, true); });
	registry.add("UrlGetPartA", 5, [url_get_part](ApiCall &call) { url_get_part(call, false); });
	registry.add("UrlEscapeW", 4, [url_escape](ApiCall &call) { url_escape(call, true); });
	registry.add("UrlEscapeA", 4, [url_escape](ApiCall &call) { url_escape(call, false); });
	// The Windows path a file URL names: "file:///C:/dir/a%20b" is "C:\dir\a b", "file://server/share" is "\\server\share" and
	// "file://C:\dir" is "C:\dir". A URL of another scheme is not a path.
	const auto path_from_url = [unescape, put_url](ApiCall &call, bool wide) {
		if (!call[0] || !call[1] || !call[2]) {
			call.ret(0x80070057U);
			return;
		}
		const auto url = wide ? call.wide(call[0]) : widen_ascii(call.narrow(call[0]));
		std::u16string lower = url.substr(0, 5);
		for (auto &character : lower)
			if (character >= u'A' && character <= u'Z')
				character = char16_t(character + 32);
		if (lower != u"file:") {
			call.ret(0x80070057U);
			return;
		}
		std::u16string rest = unescape(url.substr(5), 0);
		for (auto &character : rest)
			if (character == u'/')
				character = u'\\';
		size_t slashes = 0;
		while (slashes < rest.size() && rest[slashes] == u'\\')
			++slashes;
		const auto body = rest.substr(slashes);
		const bool drive = body.size() >= 2 && (body[1] == u':' || body[1] == u'|');
		std::u16string path;
		if (drive) {
			path = body;
			path[1] = u':';
		} else if (slashes == 2 && body.compare(0, 10, u"localhost\\") == 0) {
			path = body.substr(10);
		} else if (slashes >= 2) {
			path = u"\\\\" + body;
		} else {
			path = rest;
		}
		put_url(call, wide, call[1], call[2], path);
	};
	registry.add("PathCreateFromUrlW", 4, [path_from_url](ApiCall &call) { path_from_url(call, true); });
	registry.add("PathCreateFromUrlA", 4, [path_from_url](ApiCall &call) { path_from_url(call, false); });
	registry.add("UrlCanonicalizeW", 4, [url_canonicalize](ApiCall &call) { url_canonicalize(call, true); });
	registry.add("UrlCanonicalizeA", 4, [url_canonicalize](ApiCall &call) { url_canonicalize(call, false); });
	registry.add("UrlApplySchemeW", 4, [url_apply_scheme](ApiCall &call) { url_apply_scheme(call, true); });
	registry.add("UrlApplySchemeA", 4, [url_apply_scheme](ApiCall &call) { url_apply_scheme(call, false); });
	registry.add("UrlUnescapeW", 4, [url_unescape](ApiCall &call) { url_unescape(call, true); });
	registry.add("UrlUnescapeA", 4, [url_unescape](ApiCall &call) { url_unescape(call, false); });
	registry.add("UrlIsW", 2, [](ApiCall &call) { call.ret(call[0] && url_is(call.wide(call[0]), call.u32(1))); });
	registry.add("UrlIsA", 2, [](ApiCall &call) { call.ret(call[0] && url_is(widen_ascii(call.narrow(call[0])), call.u32(1))); });
	registry.add("PathIsURLW", 1, [](ApiCall &call) { call.ret(call[0] && url_is(call.wide(call[0]), 0)); });
	registry.add("PathIsURLA", 1, [](ApiCall &call) { call.ret(call[0] && url_is(widen_ascii(call.narrow(call[0])), 0)); });
	registry.add("PathIsRootW", 1, [](ApiCall &call) {
		const auto path = call.wide(call[0]);
		call.ret(!path.empty() && root_length(path) == path.size());
	});
	registry.add("PathIsUNCW", 1, [](ApiCall &call) { call.ret(is_unc_root(call.wide(call[0]))); });
	registry.add("PathIsNetworkPathW", 1, [](ApiCall &call) { call.ret(is_unc_root(call.wide(call[0]))); });
	registry.add("PathIsFileSpecW", 1, [](ApiCall &call) { call.ret(call.wide(call[0]).find_first_of(u"\\/:") == std::u16string::npos); });
	registry.add("PathGetDriveNumberW", 1, [](ApiCall &call) {
		const auto path = call.wide(call[0]);
		if (path.size() >= 2 && path[1] == u':' && ((path[0] >= u'a' && path[0] <= u'z') || (path[0] >= u'A' && path[0] <= u'Z')))
			call.ret(uint32_t(upper_case(path[0]) - u'A'));
		else
			call.ret(uint32_t(-1));
	});
	registry.add("PathFindFileNameW", 1, [](ApiCall &call) {
		const auto path = call.wide(call[0]);
		call.ret(call[0] + file_name_start(path) * 2);
	});
	registry.add("PathFindExtensionW", 1, [](ApiCall &call) {
		const auto path = call.wide(call[0]);
		call.ret(call[0] + extension_start(path) * 2);
	});
	registry.add("PathSkipRootW", 1, [](ApiCall &call) {
		const auto path = call.wide(call[0]);
		const size_t root = root_length(path);
		call.ret(root ? call[0] + root * 2 : 0);
	});
	// The PathCch forms report through an HRESULT: E_INVALIDARG for a missing argument or root, and the strsafe overflow code when the
	// result does not fit the buffer.
	registry.add("PathCchSkipRoot", 2, [](ApiCall &call) {
		const size_t root = call[0] && call[1] ? root_length(call.wide(call[0])) : 0;
		if (!root) {
			call.ret(0x80070057U);
			return;
		}
		call.store_pointer(call[1], call[0] + root * 2);
		call.ret(0);
	});
	registry.add("PathCchCombineEx", 5, [](ApiCall &call) {
		if (!call[0] || !call[1] || call[1] > 32768 || (!call[2] && !call[3])) {
			call.ret(0x80070057U);
			return;
		}
		const auto directory = call[2] ? call.wide(call[2]) : std::u16string();
		const auto file = call[3] ? call.wide(call[3]) : std::u16string();
		const auto combined = absolute(file) && !(separator(file[0]) && !is_unc_root(file)) ? file : join(directory, file);
		const auto result = canonical(combined);
		if (result.size() >= call[1]) {
			call.store<uint16_t>(call[0], 0);
			call.ret(0x8007007aU);
			return;
		}
		call.put_wide(call[0], result, call[1]);
		call.ret(0);
	});
	// PathCchCanonicalize is the Ex form with no flags; both remove the . and .. parts the way PathCchCombineEx does.
	const auto path_cch_canonicalize = [](ApiCall &call) {
		if (!call[0] || !call[1] || call[1] > 32768 || !call[2]) {
			call.ret(0x80070057U);
			return;
		}
		const auto result = canonical(call.wide(call[2]));
		if (result.size() >= call[1]) {
			call.store<uint16_t>(call[0], 0);
			call.ret(0x8007007aU);
			return;
		}
		call.put_wide(call[0], result, call[1]);
		call.ret(0);
	};
	registry.add("PathCchCanonicalize", 3, path_cch_canonicalize);
	// PathCchRemoveFileSpec drops the last element and keeps the root; S_FALSE says there was nothing left to remove.
	registry.add("PathCchRemoveFileSpec", 2, [](ApiCall &call) {
		if (!call[0] || !call[1] || call[1] > 32768) {
			call.ret(0x80070057U);
			return;
		}
		auto path = call.wide(call[0], size_t(call[1]));
		const size_t root = root_length(path);
		if (path.size() <= root) {
			call.ret(1);
			return;
		}
		size_t end = path.size();
		while (end > root && !separator(path[end - 1]))
			--end;
		path.resize(end > root ? end - 1 : root);
		call.put_wide(call[0], path, call[1]);
		call.ret(0);
	});
	registry.add("PathCchCanonicalizeEx", 4, path_cch_canonicalize);
	registry.add("PathAddBackslashW", 1, [](ApiCall &call) {
		auto path = call.wide(call[0], max_path * 4);
		if (path.empty() || separator(path.back())) {
			call.ret(call[0] + path.size() * 2);
			return;
		}
		if (path.size() + 1 >= max_path) {
			call.ret(0);
			return;
		}
		path.push_back(u'\\');
		call.put_wide(call[0], path, max_path);
		call.ret(call[0] + path.size() * 2);
	});
	registry.add("PathRemoveBackslashW", 1, [](ApiCall &call) {
		auto path = call.wide(call[0], max_path * 4);
		if (path.size() > root_length(path) && !path.empty() && separator(path.back())) {
			path.pop_back();
			call.put_wide(call[0], path, max_path);
		}
		call.ret(call[0] + path.size() * 2);
	});
	registry.add("PathQuoteSpacesW", 1, [](ApiCall &call) {
		auto path = call.wide(call[0], max_path * 4);
		if (path.find(u' ') != path.npos && path.size() + 2 < max_path) {
			path = u"\"" + path + u"\"";
			call.put_wide(call[0], path, max_path);
		}
	});
	registry.add("PathUnquoteSpacesW", 1, [](ApiCall &call) {
		auto path = call.wide(call[0], max_path * 4);
		if (path.size() >= 2 && path.front() == u'"' && path.back() == u'"') {
			path = path.substr(1, path.size() - 2);
			call.put_wide(call[0], path, max_path);
		}
	});
	// The arguments of a command line: after the first space outside quotes, or the terminator when there is none.
	for (const bool narrow : {false, true})
		registry.add(narrow ? "PathGetArgsA" : "PathGetArgsW", 1, [narrow](ApiCall &call) {
			if (!call[0]) {
				call.ret(0);
				return;
			}
			const size_t unit = narrow ? 1 : 2;
			const auto text = narrow ? widen_ascii(call.narrow(call[0], max_path * 8)) : call.wide(call[0], max_path * 8);
			bool quoted = false;
			for (size_t at = 0; at < text.size(); ++at) {
				if (text[at] == u'"')
					quoted = !quoted;
				else if (text[at] == u' ' && !quoted) {
					call.ret(call[0] + (at + 1) * unit);
					return;
				}
			}
			call.ret(call[0] + text.size() * unit);
		});
	registry.add("PathMatchSpecW", 2, [](ApiCall &call) {
		const auto file = call.wide(call[0]);
		const auto spec = call.wide(call[1]);
		bool matched = false;
		size_t at = 0;
		while (at <= spec.size() && !matched) {
			const size_t end = std::min(spec.find(u';', at), spec.size());
			auto part = std::u16string_view(spec).substr(at, end - at);
			while (!part.empty() && part.front() == u' ')
				part.remove_prefix(1);
			matched = !part.empty() && match_one(part, file);
			at = end + 1;
		}
		call.ret(matched);
	});
	registry.add("PathCompactPathExW", 4, [](ApiCall &call) {
		const auto path = call.wide(call[1]);
		const size_t capacity = call.u32(2);
		if (!capacity) {
			call.ret(0);
			return;
		}
		const auto result = compact(path, capacity);
		call.put_wide(call[0], result, capacity);
		call.ret(1);
	});
	registry.add("PathFileExistsW", 1, [&](ApiCall &call) {
		const auto resolved = paths.resolve(call.wide(call[0]));
		call.ret(resolved && resolved->exists);
	});
	registry.add("PathIsDirectoryW", 1, [&](ApiCall &call) {
		const auto resolved = paths.resolve(call.wide(call[0]));
		struct stat metadata{};
		call.ret(resolved && resolved->exists && !::stat(resolved->host.c_str(), &metadata) && S_ISDIR(metadata.st_mode) ? 0x10 : 0);
	});
	registry.add("PathIsDirectoryEmptyW", 1, [&](ApiCall &call) {
		const auto resolved = paths.resolve(call.wide(call[0]));
		std::error_code error;
		call.ret(resolved && std::filesystem::is_directory(resolved->host, error) && std::filesystem::is_empty(resolved->host, error));
	});

	registry.add("StrCmpW", 2, [](ApiCall &call) {
		const auto a = call.wide(call[0]), b = call.wide(call[1]);
		call.ret(uint32_t(a < b ? -1 : a > b ? 1 : 0));
	});
	registry.add("StrCmpIW", 2, [](ApiCall &call) { call.ret(uint32_t(compare_fold(call.wide(call[0]), call.wide(call[1])))); });
	registry.add("StrCmpNW", 3, [](ApiCall &call) {
		const size_t count = call.u32(2);
		const auto a = call.wide(call[0]).substr(0, count), b = call.wide(call[1]).substr(0, count);
		call.ret(uint32_t(a < b ? -1 : a > b ? 1 : 0));
	});
	registry.add("StrCmpNIW", 3, [](ApiCall &call) {
		const size_t count = call.u32(2);
		call.ret(uint32_t(compare_fold(call.wide(call[0]).substr(0, count), call.wide(call[1]).substr(0, count))));
	});
	registry.add("StrStrW", 2, [](ApiCall &call) {
		const auto text = call.wide(call[0], whole_text), needle = call.wide(call[1]);
		const auto found = text.find(needle);
		call.ret(found == text.npos ? 0 : call[0] + found * 2);
	});
	registry.add("StrStrIW", 2, [](ApiCall &call) {
		const auto text = call.wide(call[0], whole_text), needle = call.wide(call[1]);
		for (size_t at = 0; at + needle.size() <= text.size(); ++at)
			if (equal_fold(std::u16string_view(text).substr(at, needle.size()), needle)) {
				call.ret(call[0] + at * 2);
				return;
			}
		call.ret(0);
	});
	registry.add("StrChrW", 2, [](ApiCall &call) {
		const auto text = call.wide(call[0], whole_text);
		const auto found = text.find(char16_t(call[1]));
		call.ret(found == text.npos ? 0 : call[0] + found * 2);
	});
	// The first character of the string that is one of the set's.
	registry.add("StrPBrkW", 2, [](ApiCall &call) {
		// Without a string or a set there is nothing to find.
		if (!call[0] || !call[1]) {
			call.ret(0);
			return;
		}
		const auto text = call.wide(call[0], whole_text), set = call.wide(call[1]);
		const auto found = text.find_first_of(set);
		call.ret(found == text.npos || set.empty() ? 0 : call[0] + found * 2);
	});
	registry.add("StrRChrW", 3, [](ApiCall &call) {
		const auto text = call.wide(call[0], whole_text);
		const auto end = call[1] ? std::min<size_t>((call[1] - call[0]) / 2, text.size()) : text.size();
		const auto found = text.substr(0, end).rfind(char16_t(call[2]));
		call.ret(found == text.npos ? 0 : call[0] + found * 2);
	});
	registry.add("StrToIntW", 1, [](ApiCall &call) {
		const auto text = call.wide(call[0]);
		size_t at = 0;
		bool negative = false;
		if (at < text.size() && text[at] == u'-') {
			negative = true;
			++at;
		}
		int64_t value = 0;
		for (; at < text.size() && text[at] >= u'0' && text[at] <= u'9'; ++at)
			value = std::min<int64_t>(value * 10 + (text[at] - u'0'), 0x7fffffffLL + 1);
		call.ret(uint32_t(int32_t(negative ? -value : value)));
	});
	// StrToIntExW and StrToInt64ExW: leading blanks, a sign, then decimal digits, or 0x and hexadecimal ones with STIF_SUPPORT_HEX. FALSE
	// when no digit follows; the value goes out at the width the function names.
	const auto number = [](ApiCall &call, size_t width) {
		const auto text = call.wide(call[0]);
		size_t at = 0;
		while (at < text.size() && (text[at] == u' ' || text[at] == u'\t'))
			++at;
		bool negative = false;
		if (at < text.size() && (text[at] == u'-' || text[at] == u'+'))
			negative = text[at++] == u'-';
		const bool hex = (call.u32(1) & 1) && at + 1 < text.size() && text[at] == u'0' && (text[at + 1] | 0x20) == u'x';
		if (hex)
			at += 2;
		uint64_t value = 0;
		size_t digits = 0;
		for (; at < text.size(); ++at, ++digits) {
			const char16_t c = text[at], lower = char16_t(c | 0x20);
			const int digit = c >= u'0' && c <= u'9' ? c - u'0' : hex && lower >= u'a' && lower <= u'f' ? lower - u'a' + 10 : -1;
			if (digit < 0)
				break;
			value = value * (hex ? 16 : 10) + uint64_t(digit);
		}
		if (!digits || !call[2]) {
			call.ret(0);
			return;
		}
		const uint64_t result = negative ? uint64_t(0) - value : value;
		if (width == 8)
			call.store<uint64_t>(call[2], result);
		else
			call.store<uint32_t>(call[2], uint32_t(result));
		call.ret(1);
	};
	registry.add("StrToIntExW", 3, [number](ApiCall &call) { number(call, 4); });
	registry.add("StrToInt64ExW", 3, [number](ApiCall &call) { number(call, 8); });
	registry.add("StrTrimW", 2, [](ApiCall &call) {
		auto text = call.wide(call[0]);
		const auto trim = call.wide(call[1]);
		const auto first = text.find_first_not_of(trim);
		const bool changed = first != 0 || (!text.empty() && trim.find(text.back()) != trim.npos);
		if (first == text.npos) text.clear();
		else text = text.substr(first, text.find_last_not_of(trim) - first + 1);
		call.put_wide(call[0], text, text.size() + 1);
		call.ret(changed);
	});
	registry.add("StrCpyNW", 3, [](ApiCall &call) {
		const int32_t capacity = call.i32(2);
		const auto text = call.wide(call[1]);
		if (capacity > 0)
			call.put_wide(call[0], text.substr(0, size_t(capacity) - 1), size_t(capacity));
		call.ret(call[0]);
	});
	registry.add("StrNCatW", 3, [](ApiCall &call) {
		auto text = call.wide(call[0]);
		const int32_t capacity = call.i32(2);
		const auto more = call.wide(call[1]);
		if (capacity > int32_t(text.size() + 1)) {
			text += more.substr(0, size_t(capacity) - text.size() - 1);
			call.put_wide(call[0], text, text.size() + 1);
		}
		call.ret(call[0]);
	});
	registry.add("StrDupW", 1, [&](ApiCall &call) {
		const auto text = call.wide(call[0]);
		const auto copy = context.allocator.allocate((text.size() + 1) * 2, false);
		if (!copy) {
			call.ret(0);
			return;
		}
		call.put_wide(copy, text, text.size() + 1);
		call.ret(copy);
	});
	// The wide copy of a string in memory from the task allocator, which the caller frees with CoTaskMemFree.
	for (const bool wide : {false, true})
		registry.add(wide ? "SHStrDupW" : "SHStrDupA", 2, [&context, wide](ApiCall &call) {
			if (!call[0] || !call[1]) {
				if (call[1])
					call.store_pointer(call[1], 0);
				call.ret(0x80070057U);
				return;
			}
			const auto text = wide ? call.wide(call[0]) : widen_ascii(call.narrow(call[0]));
			const auto copy = context.allocator.allocate((text.size() + 1) * 2, false);
			call.store_pointer(call[1], copy);
			if (!copy) {
				call.ret(0x8007000eU);
				return;
			}
			call.put_wide(copy, text, text.size() + 1);
			call.ret(0);
		});
	registry.add("StrFormatByteSizeW", 3, [](ApiCall &call) {
		const uint64_t value = call[0];
		static constexpr const char *units[] = {" bytes", " KB", " MB", " GB", " TB", " PB", " EB"};
		std::string text;
		if (value < 1024) {
			text = std::to_string(value) + units[0];
		} else {
			double scaled = double(value);
			size_t unit = 0;
			while (scaled >= 1024 && unit < 6) {
				scaled /= 1024;
				++unit;
			}
			// Windows shows three significant digits.
			char buffer[32];
			const int decimals = scaled >= 100 ? 0 : scaled >= 10 ? 1 : 2;
			std::snprintf(buffer, sizeof buffer, "%.*f", decimals, scaled);
			text = std::string(buffer) + units[unit];
		}
		call.put_wide(call[1], widen_ascii(text), call.u32(2));
		call.ret(call[1]);
	});

	registry.add("ColorRGBToHLS", 4, [](ApiCall &call) {
		uint16_t hue, luminance, saturation;
		rgb_to_hls(call.u32(0), hue, luminance, saturation);
		if (call[1]) call.store<uint16_t>(call[1], hue);
		if (call[2]) call.store<uint16_t>(call[2], luminance);
		if (call[3]) call.store<uint16_t>(call[3], saturation);
	});
	registry.add("ColorHLSToRGB", 3, [](ApiCall &call) {
		call.ret(hls_to_rgb(int(call[0] & 0xffff), int(call[1] & 0xffff), int(call[2] & 0xffff)));
	});
	registry.add("ColorAdjustLuma", 3, [](ApiCall &call) {
		const uint32_t color = call.u32(0);
		const int32_t amount = call.i32(1);
		if (!amount) {
			call.ret(color);
			return;
		}
		uint16_t hue, luminance, saturation;
		rgb_to_hls(color, hue, luminance, saturation);
		int value = luminance;
		if (call.u32(2))
			value = value * (1000 + amount) / 1000;
		else
			value += amount * 240 / 1000;
		call.ret(hls_to_rgb(hue, std::clamp(value, 0, 240), saturation));
	});
	registry.add("AssocQueryStringW", 6, [](ApiCall &call) { call.ret(0x80070483U); });
}

}
