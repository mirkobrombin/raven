#include "loader.h"
#include "paths.h"
#include "system_exports.h"

#include <stdexcept>
#include <algorithm>
#include <sys/mman.h>

namespace raven {

ProgramLibraries::ProgramLibraries(std::u16string_view image_path, bool is_64) : is_64(is_64)
{
	const auto prefix = NativePrefix::from_environment();
	const NativePaths paths(prefix);
	if (const auto program = paths.resolve(image_path); program && program->exists)
		folder = program->host.parent_path();
}

bool ProgramLibraries::contains(std::string_view dll)
{
	if (folder.empty() || !program_may_replace(dll))
		return false;
	if (const auto known = answers.find(dll); known != answers.end())
		return known->second;
	if (!files) {
		files.emplace();
		std::error_code error;
		for (std::filesystem::directory_iterator walk(folder, error), end; !error && walk != end; walk.increment(error)) {
			auto filename = walk->path().filename().string();
			for (auto &character : filename)
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character + ('a' - 'A'));
			files->emplace(std::move(filename), walk->path());
		}
	}
	bool present = false;
	if (const auto file = files->find(std::string(dll)); file != files->end()) {
		std::error_code error;
		if (std::filesystem::is_regular_file(file->second, error))
			if (const auto mapped = ModuleBytes::map(file->second)) {
				try {
					const PEImage image(*mapped);
					present = image.is_64 == is_64 && (image.characteristics & 0x2000);
				} catch (...) {}
			}
	}
	answers.emplace(std::string(dll), present);
	return present;
}

std::vector<const PEModule *> initialization_order(const ModuleSet &modules, const PEModule &entry)
{
	std::map<const PEModule *, std::vector<const PEModule *>> dependencies;
	bool found_entry = false;
	for (const auto &[name, module] : modules.all()) {
		found_entry |= module.get() == &entry;
		dependencies[module.get()];
		for (const auto &symbol : module->imports) {
			const auto path = modules.resolution_path(symbol);
			dependencies[module.get()].push_back(path.at(0));
			for (size_t index = 1; index < path.size(); ++index)
				dependencies[path[index - 1]].push_back(path[index]);
		}
	}
	if (!found_entry)
		throw std::runtime_error("native initialization entry is outside its module graph");
	std::map<const PEModule *, int> visited;
	std::vector<const PEModule *> result;
	const auto walk = [&](const PEModule *root) {
		if (visited[root])
			return;
		std::vector<std::pair<const PEModule *, size_t>> stack{{root, 0}};
		visited[root] = 1;
		while (!stack.empty()) {
			auto &[module, index] = stack.back();
			const auto &edges = dependencies.at(module);
			if (index == edges.size()) {
				visited[module] = 2;
				result.push_back(module);
				stack.pop_back();
				continue;
			}
			const auto next = edges[index++];
			if (!visited[next]) {
				visited[next] = 1;
				stack.emplace_back(next, 0);
			}
		}
	};
	walk(&entry);
	for (const auto &[name, module] : modules.all())
		walk(module.get());
	return result;
}

uint64_t initialize_loader_data(NativeProcess &process, uint64_t peb,
	const ModuleSet &modules, const PEModule &entry)
{
	if (process.active() || modules.all().size() > 10000 || peb > UINT64_MAX - 0x20)
		throw std::runtime_error("invalid native loader initialization state");
	const auto image = process.read_memory(peb + 0x10, 8);
	uint64_t image_base = 0;
	for (size_t index = 0; index < image.size(); ++index)
		image_base |= uint64_t(image[index]) << (index * 8);
	if (image_base != entry.loaded.base())
		throw std::runtime_error("native loader graph does not match the PEB image");
	const auto existing = process.read_memory(peb + 0x18, 8);
	if (std::any_of(existing.begin(), existing.end(), [](uint8_t byte) { return byte != 0; }))
		throw std::runtime_error("native PEB loader data already exists");
	const auto order = initialization_order(modules, entry);
	std::vector<const PEModule *> loaded{&entry};
	size_t size = 0x58 + modules.all().size() * 0x120;
	for (const auto &[name, module] : modules.all()) {
		if (module->name.size() > 32766 || std::any_of(module->name.begin(), module->name.end(), [](unsigned char byte) { return byte > 127; }))
			throw std::runtime_error("native loader requires bounded ASCII module identities");
		size += (module->name.size() + 1) * 2;
		if (module.get() != &entry)
			loaded.push_back(module.get());
	}
	size = (size + 4095) & ~size_t(4095);
	std::vector<uint8_t> bytes(size);
	const auto put = [&](size_t offset, uint64_t value, size_t width = 8) {
		for (size_t index = 0; index < width; ++index)
			bytes.at(offset + index) = static_cast<uint8_t>(value >> (index * 8));
	};
	const uint64_t address = process.map_memory(size, PROT_READ | PROT_WRITE);
	try {
		put(0, 0x58, 4);
		put(4, 1, 1);
		std::map<const PEModule *, size_t> entries;
		size_t text = 0x58 + loaded.size() * 0x120;
		for (size_t index = 0; index < loaded.size(); ++index) {
			const auto &module = *loaded[index];
			const size_t offset = 0x58 + index * 0x120;
			entries.emplace(&module, offset);
			put(offset + 0x30, module.loaded.base());
			put(offset + 0x38, module.image.entry ? module.loaded.base() + module.image.entry : 0);
			put(offset + 0x40, module.image.image_size, 4);
			for (const size_t name_offset : {size_t(0x48), size_t(0x58)}) {
				put(offset + name_offset, module.name.size() * 2, 2);
				put(offset + name_offset + 2, (module.name.size() + 1) * 2, 2);
				put(offset + name_offset + 8, address + text);
			}
			for (size_t character = 0; character < module.name.size(); ++character)
				put(text + character * 2, static_cast<unsigned char>(module.name[character]), 2);
			text += (module.name.size() + 1) * 2;
		}
		const auto link = [&](size_t head, size_t member, const std::vector<const PEModule *> &list) {
			put(head, list.empty() ? address + head : address + entries.at(list.front()) + member);
			put(head + 8, list.empty() ? address + head : address + entries.at(list.back()) + member);
			for (size_t index = 0; index < list.size(); ++index) {
				const size_t node = entries.at(list[index]) + member;
				put(node, index + 1 == list.size() ? address + head : address + entries.at(list[index + 1]) + member);
				put(node + 8, index == 0 ? address + head : address + entries.at(list[index - 1]) + member);
			}
		};
		link(0x10, 0, loaded);
		link(0x20, 0x10, loaded);
		std::vector<const PEModule *> initialized;
		for (const auto *module : order)
			if (module->image.characteristics & 0x2000)
				initialized.push_back(module);
		link(0x30, 0x20, initialized);
		process.write_memory(address, bytes);
		// Publish only after all guest links are valid. Process mappings own the data.
		std::vector<uint8_t> pointer(8);
		for (size_t index = 0; index < pointer.size(); ++index)
			pointer[index] = static_cast<uint8_t>(address >> (index * 8));
		process.write_memory(peb + 0x18, pointer);
	} catch (...) {
		try { process.unmap_memory(address); } catch (...) {}
		throw;
	}
	return address;
}

static uint64_t loader_pointer(NativeProcess &process, uint64_t address)
{
	const auto bytes = process.read_memory(address, 8);
	uint64_t value = 0;
	for (size_t index = 0; index < bytes.size(); ++index)
		value |= uint64_t(bytes[index]) << (index * 8);
	return value;
}

static std::vector<uint8_t> loader_links(uint64_t next, uint64_t previous)
{
	std::vector<uint8_t> bytes(16);
	for (size_t index = 0; index < 8; ++index) {
		bytes[index] = static_cast<uint8_t>(next >> (index * 8));
		bytes[index + 8] = static_cast<uint8_t>(previous >> (index * 8));
	}
	return bytes;
}

NativeLoaderData::NativeLoaderData(NativeProcess &process, uint64_t peb,
	const ModuleSet &modules, const PEModule &entry)
	: process(process), modules(modules), peb(peb), loader(initialize_loader_data(process, peb, modules, entry))
{
	loaded.push_back(&entry);
	for (const auto &[name, module] : modules.all())
		if (module.get() != &entry)
			loaded.push_back(module.get());
	auto node = loader_pointer(process, loader + 0x10);
	for (const auto *module : loaded) {
		nodes.emplace(module, Node{node, 0});
		node = loader_pointer(process, node);
	}
	for (const auto *module : initialization_order(modules, entry))
		if (module->image.characteristics & 0x2000)
			started.push_back(module);
	validate();
}

void NativeLoaderData::validate() const
{
	if (loaded.empty() || loader_pointer(process, peb + 0x18) != loader ||
		loader_pointer(process, peb + 0x10) != loaded.front()->loaded.base())
		throw std::runtime_error("native PEB loader identity was modified");
	for (const auto &[module, node] : nodes)
		if (loader_pointer(process, node.address + 0x30) != module->loaded.base())
			throw std::runtime_error("native loader module identity was modified");
	const auto check = [&](uint64_t head, size_t member, const auto &list) {
		const auto first = list.empty() ? head : nodes.at(list.front()).address + member;
		const auto last = list.empty() ? head : nodes.at(list.back()).address + member;
		if (process.read_memory(head, 16) != loader_links(first, last))
			throw std::runtime_error("native loader list head was modified");
		for (size_t index = 0; index < list.size(); ++index) {
			const auto next = index + 1 == list.size() ? head : nodes.at(list[index + 1]).address + member;
			const auto previous = index ? nodes.at(list[index - 1]).address + member : head;
			if (process.read_memory(nodes.at(list[index]).address + member, 16) != loader_links(next, previous))
				throw std::runtime_error("native loader list node was modified");
		}
	};
	check(loader + 0x10, 0, loaded);
	check(loader + 0x20, 0x10, loaded);
	check(loader + 0x30, 0x20, started);
}

void NativeLoaderData::publish(const std::map<const PEModule *, Node> &proposed,
	const std::vector<const PEModule *> &load, const std::vector<const PEModule *> &init)
{
	validate();
	std::map<uint64_t, std::vector<uint8_t>> patches;
	const auto link = [&](uint64_t head, size_t member, const auto &list) {
		patches.emplace(head, loader_links(list.empty() ? head : proposed.at(list.front()).address + member,
			list.empty() ? head : proposed.at(list.back()).address + member));
		for (size_t index = 0; index < list.size(); ++index)
			patches.emplace(proposed.at(list[index]).address + member, loader_links(
				index + 1 == list.size() ? head : proposed.at(list[index + 1]).address + member,
				index ? proposed.at(list[index - 1]).address + member : head));
	};
	link(loader + 0x10, 0, load);
	link(loader + 0x20, 0x10, load);
	link(loader + 0x30, 0x20, init);
	std::map<uint64_t, std::vector<uint8_t>> originals;
	for (const auto &[address, bytes] : patches)
		originals.emplace(address, process.read_memory(address, bytes.size()));
	std::vector<uint64_t> written;
	written.reserve(patches.size());
	try {
		for (const auto &[address, bytes] : patches) {
			process.write_memory(address, bytes);
			written.push_back(address);
		}
	} catch (...) {
		for (auto address = written.rbegin(); address != written.rend(); ++address)
			process.write_memory(*address, originals.at(*address));
		throw;
	}
}

void NativeLoaderData::add(const PEModule &module)
{
	const auto found = modules.all().find(module.name);
	if (found == modules.all().end() || found->second.get() != &module || nodes.contains(&module) ||
		nodes.size() >= 10000 || !module.image.is_64 || !(module.image.characteristics & 0x2000) ||
		module.name.size() > 32766 || std::any_of(module.name.begin(), module.name.end(), [](unsigned char c) { return c > 127; }))
		throw std::runtime_error("invalid dynamic native loader module");
	validate();
	if (process.read_memory(module.loaded.base(), 2) != std::vector<uint8_t>{0x4d, 0x5a})
		throw std::runtime_error("dynamic native loader module is not mapped");
	std::vector<uint8_t> bytes((0x120 + (module.name.size() + 1) * 2 + 4095) & ~size_t(4095));
	const auto address = process.map_memory(bytes.size(), PROT_READ | PROT_WRITE);
	try {
		const auto put = [&](size_t offset, uint64_t value, size_t width = 8) {
			for (size_t index = 0; index < width; ++index)
				bytes.at(offset + index) = static_cast<uint8_t>(value >> (index * 8));
		};
		put(0x30, module.loaded.base());
		put(0x38, module.image.entry ? module.loaded.base() + module.image.entry : 0);
		put(0x40, module.image.image_size, 4);
		for (const size_t offset : {size_t(0x48), size_t(0x58)}) {
			put(offset, module.name.size() * 2, 2);
			put(offset + 2, (module.name.size() + 1) * 2, 2);
			put(offset + 8, address + 0x120);
		}
		for (size_t index = 0; index < module.name.size(); ++index)
			put(0x120 + index * 2, static_cast<unsigned char>(module.name[index]), 2);
		process.write_memory(address, bytes);
		auto proposed = nodes;
		auto load = loaded;
		proposed.emplace(&module, Node{address, address});
		load.push_back(&module);
		publish(proposed, load, started);
		nodes.swap(proposed);
		loaded.swap(load);
	} catch (...) {
		process.unmap_memory(address);
		throw;
	}
}

void NativeLoaderData::initialized(const PEModule &module)
{
	if (!nodes.contains(&module) || !nodes.at(&module).allocation ||
		std::find(started.begin(), started.end(), &module) != started.end())
		throw std::runtime_error("invalid dynamic native loader initialization notification");
	auto init = started;
	init.push_back(&module);
	publish(nodes, loaded, init);
	started.swap(init);
}

void NativeLoaderData::remove(const PEModule &module)
{
	if (!nodes.contains(&module) || !nodes.at(&module).allocation)
		throw std::runtime_error("native loader release requires a dynamic entry");
	auto proposed = nodes;
	auto load = loaded;
	auto init = started;
	const auto allocation = proposed.at(&module).allocation;
	proposed.erase(&module);
	std::erase(load, &module);
	std::erase(init, &module);
	publish(proposed, load, init);
	process.unmap_memory(allocation);
	nodes.swap(proposed);
	loaded.swap(load);
	started.swap(init);
}

}
