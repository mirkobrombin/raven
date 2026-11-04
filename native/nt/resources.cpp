#include "resources.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>

namespace raven {

NativeResources::NativeResources(NativeProcess &process, NativeEnvironment &environment,
	const ModuleSet &modules, const PEModule &entry)
	: process(process), environment(environment), modules(modules), entry(entry) {}

const PEModule *NativeResources::module(uint64_t handle)
{
	if (!handle)
		return &entry;
	for (const auto &[name, image] : modules.all())
		if (image->loaded.base() == handle)
			return image.get();
	environment.set_last_error(6);
	return nullptr;
}

// A name folded to upper case, as resource names compare.
static ResourceKey folded(const ResourceKey &key)
{
	auto value = key;
	if (auto *text = std::get_if<std::u16string>(&value))
		for (auto &c : *text)
			if (c >= u'a' && c <= u'z')
				c = static_cast<char16_t>(c - (u'a' - u'A'));
	return value;
}

const NativeResources::Parsed &NativeResources::parse(const PEModule &image) const
{
	auto &cached = parsed[&image];
	const auto directory = image.image.directories[2];
	if (!directory.address && !directory.size) {
		cached = {};
		return cached;
	}
	// Self-extracting packages carry their whole payload as resources: the directory may be as large as the image that holds it.
	if (uint64_t(directory.address) + directory.size > image.image.image_size)
		throw std::runtime_error("native resource directory lies outside its image");
	// The image the runtime maps for the guest is the live one, guest changes included.
	const auto live = image.loaded.data();
	if (cached.valid && cached.read_only && cached.mapping_changes == process.mapping_changes())
		return cached;
	const auto settle = [&] {
		cached.read_only = process.has_protection(image.loaded.base() + directory.address, directory.size, PROT_READ);
		cached.mapping_changes = process.mapping_changes();
	};
	if (cached.valid && uint64_t(directory.address) + directory.size <= live.size()) {
		size_t at = 0;
		bool same = true;
		for (const auto &[offset, size] : cached.read) {
			if (std::memcmp(live.data() + directory.address + offset, cached.bytes.data() + at, size)) {
				same = false;
				break;
			}
			at += size;
		}
		if (same) {
			settle();
			return cached;
		}
	}
	// Read the live image, including guest changes, through the existing permission boundary.
	const auto bytes = process.read_memory(image.loaded.base() + directory.address, directory.size);
	cached = {};
	std::vector<std::pair<size_t, size_t>> read;
	cached.resources = read_resources(directory, bytes, image.image.image_size, &read);
	// The parse reads a few bytes at a time; touching and overlapping parts are joined so that a lookup compares a few long runs.
	std::sort(read.begin(), read.end());
	for (const auto &[offset, size] : read)
		if (!cached.read.empty() && offset <= cached.read.back().first + cached.read.back().second)
			cached.read.back().second = std::max(cached.read.back().second, offset + size - cached.read.back().first);
		else
			cached.read.emplace_back(offset, size);
	for (const auto &[offset, size] : cached.read)
		cached.bytes.insert(cached.bytes.end(), bytes.begin() + ptrdiff_t(offset), bytes.begin() + ptrdiff_t(offset + size));
	for (uint32_t index = 0; index < cached.resources.size(); ++index) {
		const auto &resource = cached.resources[index];
		const auto type = folded(resource.type);
		cached.by_name[{type, folded(resource.name)}].push_back(index);
		cached.types.insert(type);
		cached.by_entry.emplace(resource.entry_address, index);
		cached.by_data.emplace(resource.data_address, index);
	}
	cached.valid = true;
	settle();
	return cached;
}

std::optional<ResourceKey> NativeResources::key(uint64_t address, bool wide)
{
	if (address <= UINT16_MAX)
		return static_cast<uint32_t>(address);
	std::u16string text;
	const size_t width = wide ? 2 : 1;
	for (size_t index = 0; index < 32768; ++index) {
		if (address > UINT64_MAX - index * width)
			throw std::runtime_error("native resource string address overflows");
		const auto bytes = process.read_memory(address + index * width, width);
		const uint32_t character = uint32_t(bytes[0]) | (wide ? uint32_t(bytes[1]) << 8 : 0);
		if (!character) {
			if (text.empty() || text[0] != u'#')
				return text;
			uint32_t value = 0;
			for (size_t digit = 1; digit < text.size(); ++digit) {
				if (text[digit] < u'0' || text[digit] > u'9' || value > (0x7fffffffU - uint32_t(text[digit] - u'0')) / 10) {
					environment.set_last_error(87);
					return std::nullopt;
				}
				value = value * 10 + uint32_t(text[digit] - u'0');
			}
			if (text.size() == 1) {
				environment.set_last_error(87);
				return std::nullopt;
			}
			return value;
		}
		if (!wide && character > 127) {
			environment.set_last_error(120);
			return std::nullopt;
		}
		text += static_cast<char16_t>(character);
	}
	throw std::runtime_error("native resource string exceeds initial limit");
}

uint64_t NativeResources::find(uint64_t handle, uint64_t type, uint64_t name, uint16_t language, bool wide)
{
	const auto *image = module(handle);
	if (!image)
		return 0;
	const auto type_key = key(type, wide);
	const auto name_key = key(name, wide);
	if (!type_key || !name_key)
		return 0;
	const auto &index = parse(*image);
	const auto &data = index.resources;
	std::vector<const PEResource *> candidates;
	const bool has_type = index.types.contains(folded(*type_key));
	if (const auto found = index.by_name.find({folded(*type_key), folded(*name_key)}); found != index.by_name.end())
		for (const auto at : found->second)
			candidates.push_back(&data[at]);
	if (candidates.empty()) {
		const auto non_ascii = [](const ResourceKey &value) {
			const auto *text = std::get_if<std::u16string>(&value);
			return text && std::any_of(text->begin(), text->end(), [](auto c) { return c > 127; });
		};
		environment.set_last_error(non_ascii(*type_key) || non_ascii(*name_key) ? 120 :
			data.empty() ? 1812 : has_type ? 1814 : 1813);
		return 0;
	}
	const PEResource *selected = nullptr;
	for (const auto desired : {uint32_t(language), uint32_t(language & 0x3ffU), uint32_t(0)}) {
		const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto *r) { return r->language == desired; });
		if (found != candidates.end()) {
			selected = *found;
			break;
		}
	}
	if (!selected && !language && candidates.size() == 1)
		selected = candidates[0];
	// A neutral request over several languages takes English, as the guest's default locale is en-US.
	if (!selected && !language) {
		const auto english = std::find_if(candidates.begin(), candidates.end(), [](const auto *r) { return r->language == 0x409; });
		const auto primary = std::find_if(candidates.begin(), candidates.end(), [](const auto *r) { return (r->language & 0x3ffU) == 0x09; });
		if (english != candidates.end())
			selected = *english;
		else if (primary != candidates.end())
			selected = *primary;
	}
	if (!selected) {
		// Locale-dependent fallback needs thread NLS state; never invent a preferred language.
		environment.set_last_error(language ? 1815 : 120);
		return 0;
	}
	return image->loaded.base() + selected->entry_address;
}

uint64_t NativeResources::access(uint64_t handle, uint64_t resource, bool size)
{
	const auto *image = module(handle);
	if (!image)
		return 0;
	const auto &index = parse(*image);
	if (resource >= image->loaded.base() && resource - image->loaded.base() <= UINT32_MAX)
		if (const auto found = index.by_entry.find(uint32_t(resource - image->loaded.base())); found != index.by_entry.end()) {
			const auto &item = index.resources[found->second];
			if (size)
				return item.size;
			if (item.size > 256 * 1024 * 1024)
				throw std::runtime_error("native resource payload exceeds initial policy limit");
			readable(image->loaded.base() + item.data_address, item.size);
			return image->loaded.base() + item.data_address;
		}
	environment.set_last_error(6);
	return 0;
}

std::optional<NativeResourceView> NativeResources::view(uint64_t handle, uint32_t type, uint64_t name, uint16_t language)
{
	const auto found = find(handle, type, name, language, true);
	if (!found)
		return std::nullopt;
	const auto *image = module(handle);
	if (!image)
		return std::nullopt;
	const auto &index = parse(*image);
	if (const auto entry = index.by_entry.find(uint32_t(found - image->loaded.base())); entry != index.by_entry.end()) {
		const auto &item = index.resources[entry->second];
		if (item.size > 128 * 1024 * 1024)
			throw std::runtime_error("native resource view exceeds payload policy");
		return NativeResourceView{image->loaded.base() + item.data_address, item.size, item.language};
	}
	environment.set_last_error(6);
	return std::nullopt;
}

// A payload handed out must be readable; one the guest's protection keeps readable is not copied to find out.
void NativeResources::readable(uint64_t address, size_t size)
{
	if (!process.has_protection(address, size, PROT_READ))
		process.read_memory(address, size);
}

uint64_t NativeResources::lock(uint64_t data)
{
	for (const auto &[name, image] : modules.all())
		if (data >= image->loaded.base() && data - image->loaded.base() < image->image.image_size)
			if (const auto &index = parse(*image); index.by_data.contains(uint32_t(data - image->loaded.base()))) {
				const auto &resource = index.resources[index.by_data.at(uint32_t(data - image->loaded.base()))];
				if (resource.size > 256 * 1024 * 1024)
					throw std::runtime_error("native resource payload exceeds initial policy limit");
				readable(data, resource.size);
				return data;
			}
	environment.set_last_error(6);
	return 0;
}

bool NativeResources::dispatch(std::string_view name)
{
	uint64_t result = 0;
	if (name == "FindResourceA" || name == "FindResourceW" || name == "FindResourceExA" || name == "FindResourceExW") {
		const bool extended = name == "FindResourceExA" || name == "FindResourceExW";
		const auto args = process.api_arguments(extended ? 4 : 3);
		result = find(args[0], args[extended ? 1 : 2], args[extended ? 2 : 1],
			static_cast<uint16_t>(extended ? args[3] : 0), name.back() == 'W');
	} else if (name == "LoadResource" || name == "SizeofResource") {
		const auto args = process.api_arguments(2);
		result = access(args[0], args[1], name == "SizeofResource");
	} else if (name == "LockResource") {
		result = lock(process.api_arguments(1)[0]);
	} else {
		return false;
	}
	process.return_api(result);
	return true;
}

}
