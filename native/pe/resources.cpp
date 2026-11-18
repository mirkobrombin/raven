#include "resources.h"

#include <exception>
#include <stdexcept>

// The resource tree is read by the Rust reader (native/peparse); this side copies what it found into the host's own types.
namespace raven {

namespace {

ResourceKey key_of(const raven_pe_key &key)
{
	if (!key.name.data)
		return key.id;
	std::u16string text;
	text.reserve(key.name.size / 2);
	for (size_t index = 0; index + 1 < key.name.size; index += 2)
		text += static_cast<char16_t>(key.name.data[index] | key.name.data[index + 1] << 8);
	return text;
}

struct Context {
	std::vector<PEResource> resources;
	std::vector<std::pair<size_t, size_t>> *read = nullptr;
	std::exception_ptr failure;
};

bool resource_found(void *data, const raven_pe_resource *found) noexcept
{
	auto &context = *static_cast<Context *>(data);
	try {
		context.resources.push_back({key_of(found->type), key_of(found->name), found->language, found->entry_address,
			found->data_address, found->size, found->code_page});
		return true;
	} catch (...) {
		context.failure = std::current_exception();
		return false;
	}
}

void range_read(void *data, uint64_t offset, uint64_t size) noexcept
{
	auto &context = *static_cast<Context *>(data);
	if (context.failure)
		return;
	try {
		context.read->emplace_back(size_t(offset), size_t(size));
	} catch (...) {
		context.failure = std::current_exception();
	}
}

std::vector<PEResource> finish(Context &context, const char *refused)
{
	if (context.failure)
		std::rethrow_exception(context.failure);
	if (refused)
		throw std::runtime_error(refused);
	return std::move(context.resources);
}

}

std::vector<PEResource> read_resources(PEDirectory directory,
	std::span<const uint8_t> bytes, uint32_t image_size, std::vector<std::pair<size_t, size_t>> *read)
{
	Context context;
	context.read = read;
	const char *refused = raven_pe_read_resources({directory.address, directory.size}, bytes.data(), bytes.size(), image_size,
		sizeof(PEResource), &context, resource_found, read ? range_read : nullptr);
	return finish(context, refused);
}

std::vector<PEResource> PEImage::resources() const
{
	Context context;
	return finish(context, raven_pe_resources(bytes.data(), bytes.size(), &parsed, sizeof(PEResource), &context, resource_found));
}

}
