#include "kernel_context.h"
#include "nls.h"
#include "pe/resources.h"
#include "text.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>

namespace raven {

namespace {

constexpr uint32_t rt_version = 16;

void put16(std::vector<uint8_t> &bytes, uint16_t value)
{
	bytes.push_back(uint8_t(value));
	bytes.push_back(uint8_t(value >> 8));
}

void put32(std::vector<uint8_t> &bytes, uint32_t value)
{
	put16(bytes, uint16_t(value));
	put16(bytes, uint16_t(value >> 16));
}

void align(std::vector<uint8_t> &bytes)
{
	while (bytes.size() % 4)
		bytes.push_back(0);
}

// One VS_VERSIONINFO-style node: header, key, optional value, children; lengths are patched when the node ends.
size_t begin_block(std::vector<uint8_t> &bytes, std::u16string_view key, uint16_t value_length, uint16_t type)
{
	align(bytes);
	const size_t start = bytes.size();
	put16(bytes, 0);
	put16(bytes, value_length);
	put16(bytes, type);
	for (const auto character : key)
		put16(bytes, character);
	put16(bytes, 0);
	align(bytes);
	return start;
}

void end_block(std::vector<uint8_t> &bytes, size_t start)
{
	const uint16_t length = uint16_t(bytes.size() - start);
	bytes[start] = uint8_t(length);
	bytes[start + 1] = uint8_t(length >> 8);
}

void string_entry(std::vector<uint8_t> &bytes, std::u16string_view key, std::u16string_view value)
{
	const auto start = begin_block(bytes, key, uint16_t(value.size() + 1), 1);
	for (const auto character : value)
		put16(bytes, character);
	put16(bytes, 0);
	end_block(bytes, start);
}

// Version resource that Windows system libraries carry; the numbers describe Windows 10 build 19041.
std::vector<uint8_t> system_version(std::u16string_view name)
{
	std::vector<uint8_t> bytes;
	const auto root = begin_block(bytes, u"VS_VERSION_INFO", 52, 0);
	put32(bytes, 0xfeef04bd);
	put32(bytes, 0x10000);
	put32(bytes, 10U << 16);
	put32(bytes, (19041U << 16) | 1);
	put32(bytes, 10U << 16);
	put32(bytes, (19041U << 16) | 1);
	put32(bytes, 0x3f);
	put32(bytes, 0);
	put32(bytes, 0x40004);
	put32(bytes, name.ends_with(u".exe") ? 1 : 2);
	put32(bytes, 0);
	put32(bytes, 0);
	put32(bytes, 0);
	const auto strings = begin_block(bytes, u"StringFileInfo", 0, 1);
	const auto table = begin_block(bytes, u"040904b0", 0, 1);
	string_entry(bytes, u"CompanyName", u"Microsoft Corporation");
	string_entry(bytes, u"FileDescription", name);
	string_entry(bytes, u"FileVersion", u"10.0.19041.1 (WinBuild.160101.0800)");
	string_entry(bytes, u"InternalName", name);
	string_entry(bytes, u"OriginalFilename", name);
	string_entry(bytes, u"ProductName", u"Microsoft® Windows® Operating System");
	string_entry(bytes, u"ProductVersion", u"10.0.19041.1");
	end_block(bytes, table);
	end_block(bytes, strings);
	const auto variables = begin_block(bytes, u"VarFileInfo", 0, 1);
	const auto translation = begin_block(bytes, u"Translation", 4, 0);
	put16(bytes, 0x0409);
	put16(bytes, 0x04b0);
	end_block(bytes, translation);
	end_block(bytes, variables);
	end_block(bytes, root);
	return bytes;
}

struct Node {
	size_t start, length;
	uint16_t value_length, type;
	std::u16string key;
	size_t value_offset, value_size, children;
};

std::optional<Node> read_node(std::span<const uint8_t> block, size_t at, size_t limit)
{
	if (at + 6 > limit || at % 4)
		return std::nullopt;
	Node node{};
	node.start = at;
	node.length = block[at] | block[at + 1] << 8;
	node.value_length = uint16_t(block[at + 2] | block[at + 3] << 8);
	node.type = uint16_t(block[at + 4] | block[at + 5] << 8);
	if (node.length < 6 || at + node.length > limit)
		return std::nullopt;
	size_t cursor = at + 6;
	while (cursor + 1 < at + node.length && (block[cursor] || block[cursor + 1])) {
		node.key.push_back(char16_t(block[cursor] | block[cursor + 1] << 8));
		cursor += 2;
	}
	cursor += 2;
	cursor = (cursor + 3) & ~size_t(3);
	node.value_offset = cursor;
	node.value_size = node.type == 1 ? size_t(node.value_length) * 2 : node.value_length;
	node.children = (cursor + node.value_size + 3) & ~size_t(3);
	if (node.children > at + node.length)
		node.children = at + node.length;
	return node;
}

// Follows a backslash-separated path through nested version blocks; returns the node it names.
std::optional<Node> find_node(std::span<const uint8_t> block, std::u16string_view path)
{
	auto node = read_node(block, 0, block.size());
	if (!node)
		return std::nullopt;
	while (!path.empty() && path.front() == u'\\')
		path.remove_prefix(1);
	while (!path.empty()) {
		const auto end = path.find(u'\\');
		const auto part = path.substr(0, end);
		path = end == path.npos ? std::u16string_view() : path.substr(end + 1);
		// Doubled separators name nothing, as Windows reads them: the .NET Framework writes its queries with them.
		if (part.empty())
			continue;
		bool found = false;
		size_t at = node->children;
		while (at + 6 <= node->start + node->length) {
			const auto child = read_node(block, at, node->start + node->length);
			if (!child)
				return std::nullopt;
			if (equal_fold(child->key, part)) {
				node = child;
				found = true;
				break;
			}
			at = (child->start + child->length + 3) & ~size_t(3);
		}
		if (!found)
			return std::nullopt;
	}
	return node;
}

}

void register_version_api(ApiRegistry &registry, KernelContext &context)
{
	auto &paths = context.paths;
	auto &modules = context.modules;
	// The version resource of a file in the prefix, or a synthesized one for the Windows system libraries.
	const auto load = [&paths, &modules](ApiCall &call, std::u16string name) -> std::optional<std::vector<uint8_t>> {
		const auto resolved = paths.resolve(name);
		if (resolved && resolved->exists && std::filesystem::is_regular_file(resolved->host)) {
			// One read of the whole file: a stream iterator takes seconds over a library of hundreds of megabytes.
			std::ifstream stream(resolved->host, std::ios::binary | std::ios::ate);
			const auto size = stream.tellg();
			std::vector<uint8_t> bytes(size > 0 ? static_cast<size_t>(size) : 0);
			stream.seekg(0);
			if (size > 0 && !stream.read(reinterpret_cast<char *>(bytes.data()), size))
				bytes.resize(static_cast<size_t>(stream.gcount()));
			try {
				const PEImage image(bytes);
				for (const auto &resource : image.resources()) {
					const auto *type = std::get_if<uint32_t>(&resource.type);
					if (type && *type == rt_version) {
						const auto data = image.file_span(resource.data_address, resource.size);
						return std::vector<uint8_t>(data.begin(), data.end());
					}
				}
			} catch (const std::runtime_error &) {
				call.set_last_error(193);
				return std::nullopt;
			}
			call.set_last_error(1813);
			return std::nullopt;
		}
		auto base = name.substr(name.find_last_of(u"\\/") == name.npos ? 0 : name.find_last_of(u"\\/") + 1);
		std::string lower = to_utf8(base);
		for (auto &character : lower)
			character = char(std::tolower(static_cast<unsigned char>(character)));
		if (modules.all().contains(lower))
			return system_version(widen_ascii(lower));
		call.set_last_error(resolved && !resolved->parent_exists ? 3 : 2);
		return std::nullopt;
	};
	// Names and query paths arrive in the character set of the function; a narrow one is widened one byte per character.
	const auto name_at = [](ApiCall &call, size_t index, bool ansi) {
		if (!ansi)
			return call.wide(call[index]);
		const auto narrow = call.narrow(call[index]);
		return std::u16string(narrow.begin(), narrow.end());
	};
	const auto store_pointer = [](ApiCall &call, uint64_t address, uint64_t value) { call.store_pointer(address, value); };
	const auto size_of = [load, name_at](ApiCall &call, bool ansi) {
		const auto data = load(call, name_at(call, 0, ansi));
		if (!data) {
			call.ret(0);
			return;
		}
		if (call[1]) call.store<uint32_t>(call[1], 0);
		// As Windows does, room for an ANSI copy of the strings follows the block, so VerQueryValueA can answer string queries.
		call.ret(data->size() * 2 + 4);
	};
	registry.add("GetFileVersionInfoSizeW", 2, [size_of](ApiCall &call) { size_of(call, false); });
	registry.add("GetFileVersionInfoSizeA", 2, [size_of](ApiCall &call) { size_of(call, true); });
	registry.add("GetFileVersionInfoSizeExW", 3, [load](ApiCall &call) {
		const auto data = load(call, call.wide(call[1]));
		if (call[2]) call.store<uint32_t>(call[2], 0);
		call.ret(data ? data->size() : 0);
	});
	const auto info_of = [load, name_at](ApiCall &call, size_t name_index, size_t length_index, size_t data_index, bool ansi) {
		auto data = load(call, name_at(call, name_index, ansi));
		if (!data) {
			call.ret(0);
			return;
		}
		const size_t capacity = call.u32(length_index);
		if (capacity < data->size()) {
			call.fail(122);
			return;
		}
		// The ANSI area after the block starts zeroed, as far as the caller's buffer reaches.
		const size_t extra = std::min(capacity, data->size() * 2 + 4) - data->size();
		data->resize(data->size() + extra, 0);
		call.write(call[data_index], *data);
		call.ret(1);
	};
	registry.add("GetFileVersionInfoW", 4, [info_of](ApiCall &call) { info_of(call, 0, 2, 3, false); });
	registry.add("GetFileVersionInfoA", 4, [info_of](ApiCall &call) { info_of(call, 0, 2, 3, true); });
	registry.add("GetFileVersionInfoExW", 5, [info_of](ApiCall &call) { info_of(call, 1, 3, 4, false); });
	const auto query = [name_at, store_pointer, &context](ApiCall &call, bool ansi) {
		if (!call[0] || !call[1]) {
			call.fail(87);
			return;
		}
		const size_t total = call.read(call[0], 2)[0] | call.read(call[0] + 1, 1)[0] << 8;
		const auto block = call.read(call[0], total);
		const auto path = ansi ? name_at(call, 1, true) : call.wide(call[1], 512);
		const auto node = find_node(block, path);
		if (!node) {
			call.fail(1813);
			return;
		}
		// A text value asked for through the ANSI function is converted into the area after the block, at the same offset, as Windows does.
		// The call is not told the buffer's size: the area is there when the caller sized it with GetFileVersionInfoSize, as it has to.
		if (ansi && node->type == 1) {
			const auto wide = call.read(call[0] + node->value_offset, size_t(node->value_length) * 2);
			std::u16string text;
			for (size_t i = 0; i + 1 < wide.size(); i += 2) {
				const char16_t c = char16_t(wide[i] | wide[i + 1] << 8);
				if (!c) break;
				text.push_back(c);
			}
			const auto encoded = encode_ansi(text, context.launch.ansi_code_page);
			std::vector<uint8_t> narrow(encoded.begin(), encoded.end());
			narrow.push_back(0);
			const uint64_t target = call[0] + total + 4 + node->value_offset;
			call.write(target, narrow);
			if (call[2]) store_pointer(call, call[2], target);
			if (call[3]) call.store<uint32_t>(call[3], uint32_t(narrow.size()));
			call.ret(1);
			return;
		}
		if (call[2]) store_pointer(call, call[2], call[0] + node->value_offset);
		if (call[3]) call.store<uint32_t>(call[3], node->type == 1 ? node->value_length : uint32_t(node->value_size));
		call.ret(1);
	};
	registry.add("VerQueryValueW", 4, [query](ApiCall &call) { query(call, false); });
	registry.add("VerQueryValueA", 4, [query](ApiCall &call) { query(call, true); });
}

}
