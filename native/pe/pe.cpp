#include "pe.h"

#include <exception>
#include <stdexcept>

// Layout reference: https://learn.microsoft.com/en-us/windows/win32/debug/pe-format. The reading and its checks are the Rust
// reader's (native/peparse); this side copies what it found into the host's own types.
namespace raven {

namespace {

void check(const char *refused)
{
	if (refused)
		throw std::runtime_error(refused);
}

std::string_view view(raven_pe_text text)
{
	return {reinterpret_cast<const char *>(text.data), text.size};
}

// What a caller's function threw, kept until the Rust reader returns: nothing may unwind through its frames.
struct Collector {
	std::exception_ptr failure;

	template<typename Step> bool run(Step &&step) noexcept
	{
		try {
			step();
			return true;
		} catch (...) {
			failure = std::current_exception();
			return false;
		}
	}

	void finish(const char *refused) const
	{
		if (failure)
			std::rethrow_exception(failure);
		check(refused);
	}
};

PEImport import_of(const raven_pe_import &found)
{
	return {std::string(view(found.module)), std::string(view(found.name)), found.ordinal, found.by_ordinal, found.iat_address};
}

}

PEImage::PEImage(std::span<const uint8_t> bytes) : bytes(bytes)
{
	check(raven_pe_parse(bytes.data(), bytes.size(), &parsed));
	is_64 = parsed.is_64;
	machine = parsed.machine;
	characteristics = parsed.characteristics;
	image_base = parsed.image_base;
	stack_reserve = parsed.stack_reserve;
	subsystem = parsed.subsystem;
	heap_reserve = parsed.heap_reserve;
	heap_commit = parsed.heap_commit;
	image_size = parsed.image_size;
	headers_size = parsed.headers_size;
	entry = parsed.entry;
	section_alignment = parsed.section_alignment;
	file_alignment = parsed.file_alignment;
	for (size_t index = 0; index < directories.size(); ++index)
		directories[index] = {parsed.directories[index].address, parsed.directories[index].size};
	sections.reserve(parsed.section_count);
	for (uint32_t index = 0; index < parsed.section_count; ++index) {
		const auto &section = parsed.sections[index];
		sections.push_back({std::string(reinterpret_cast<const char *>(section.name), section.name_size), section.address,
			section.virtual_size, section.file_offset, section.file_size, section.characteristics});
	}
}

std::span<const uint8_t> PEImage::file_span(uint32_t rva, size_t size) const
{
	size_t offset = 0;
	check(raven_pe_file_span(bytes.data(), bytes.size(), &parsed, rva, size, &offset));
	return bytes.subspan(offset, size);
}

std::string PEImage::string_at(uint32_t rva) const
{
	return std::string(text_at(rva));
}

std::string_view PEImage::text_at(uint32_t rva) const
{
	raven_pe_text text{};
	check(raven_pe_text_at(bytes.data(), bytes.size(), &parsed, rva, &text));
	return view(text);
}

bool PEImage::il_only() const
{
	bool answer = false;
	check(raven_pe_il_only(bytes.data(), bytes.size(), &parsed, &answer));
	return answer;
}

std::vector<PEImport> PEImage::imports() const
{
	struct Context : Collector {
		std::vector<PEImport> result;
	} context;
	const auto each = [](void *data, const raven_pe_import *found) {
		auto &self = *static_cast<Context *>(data);
		return self.run([&] { self.result.push_back(import_of(*found)); });
	};
	context.finish(raven_pe_imports(bytes.data(), bytes.size(), &parsed, &context, each));
	return std::move(context.result);
}

std::vector<PEDelayImport> PEImage::delay_imports() const
{
	struct Context : Collector {
		std::vector<PEDelayImport> result;
	} context;
	const auto descriptor = [](void *data, const raven_pe_delay_descriptor *found) {
		auto &self = *static_cast<Context *>(data);
		return self.run([&] {
			self.result.push_back({std::string(view(found->module)), found->descriptor_address, found->module_handle_address,
				found->iat_address, found->lookup_address, found->bound_address, found->unload_address, found->timestamp, {}});
		});
	};
	const auto each = [](void *data, const raven_pe_import *found) {
		auto &self = *static_cast<Context *>(data);
		return self.run([&] { self.result.back().imports.push_back(import_of(*found)); });
	};
	context.finish(raven_pe_delay_imports(bytes.data(), bytes.size(), &parsed, &context, descriptor, each));
	return std::move(context.result);
}

std::vector<PEExport> PEImage::exports() const
{
	struct Context : Collector {
		std::vector<PEExport> result;
	} context;
	const auto each = [](void *data, const raven_pe_export *found) {
		auto &self = *static_cast<Context *>(data);
		return self.run([&] { self.result.push_back({found->ordinal, found->address, {}, std::string(view(found->forwarder))}); });
	};
	const auto named = [](void *data, uint32_t index, raven_pe_text name) {
		auto &self = *static_cast<Context *>(data);
		return self.run([&] { self.result.at(index).names.emplace_back(view(name)); });
	};
	context.finish(raven_pe_exports(bytes.data(), bytes.size(), &parsed, &context, each, named));
	return std::move(context.result);
}

bool widen_il_only_image(std::vector<uint8_t> &file)
{
	return raven_pe_widen_il_only(file.data(), file.size());
}

}
