#include "msix.h"
#include "kernel_context.h"
#include "text.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/syscall.h>
#include <tuple>
#include <unistd.h>

#ifdef RAVEN_OPENSSL
#include <openssl/evp.h>
#endif

namespace raven {

namespace {

struct VerifiedPackage {
	std::u16string publisher;
	std::vector<uint8_t> block_map;
};

std::mutex verification_lock;
std::map<uint32_t, VerifiedPackage> verifications;

VerifiedPackage take_verification(uint32_t thread)
{
	std::lock_guard lock(verification_lock);
	auto found = verifications.extract(thread);
	return found.empty() ? VerifiedPackage{} : std::move(found.mapped());
}

bool identity_component(std::string_view name)
{
	return !name.empty() && name.size() <= 128 && name.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.-_") == name.npos &&
		name.back() != '.';
}

bool relative_file(std::u16string_view name)
{
	if (name.empty())
		return false;
	while (!name.empty()) {
		const size_t slash = name.find_first_of(u"\\/");
		const auto part = name.substr(0, slash);
		if (part.empty() || part.back() == u'.' || part.back() == u' ' || part.find_first_of(u":<>|?*\"\t\r\n") != part.npos)
			return false;
		if (slash == name.npos)
			return true;
		name.remove_prefix(slash + 1);
	}
	return false;
}

std::optional<MsixApplication> package_record(const std::filesystem::path &directory, std::string full_name = {})
{
	if (full_name.empty())
		full_name = directory.filename().string();
	if (!identity_component(full_name))
		return std::nullopt;
	const size_t version_at = full_name.find('_'), version_end = full_name.find('_', version_at + 1);
	const size_t architecture_end = full_name.find('_', version_end + 1), id_at = full_name.find_last_of('_');
	if (version_at == full_name.npos || version_end == full_name.npos || architecture_end == full_name.npos || id_at <= architecture_end)
		return std::nullopt;
	const auto architecture = full_name.substr(version_end + 1, architecture_end - version_end - 1);
	uint32_t machine;
	if (architecture == "x86")
		machine = 0;
	else if (architecture == "x64")
		machine = 9;
	else if (architecture == "neutral")
		machine = 11;
	else
		return std::nullopt;
	const char *first = full_name.data() + version_at + 1, *last = full_name.data() + version_end;
	uint64_t version = 0;
	for (int piece = 0; piece < 4; ++piece) {
		uint16_t part = 0;
		const auto [end, error] = std::from_chars(first, last, part);
		if (error != std::errc() || (piece == 3 ? end != last : end == last || *end != '.'))
			return std::nullopt;
		version = version << 16 | part;
		first = piece == 3 ? last : end + 1;
	}
	const auto path = directory / ".raven-package";
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error) || std::filesystem::file_size(path, error) > 65536 || error)
		return std::nullopt;
	std::ifstream input(path, std::ios::binary);
	std::string family, publisher, resource;
	if (!std::getline(input, family) || !std::getline(input, publisher) || !std::getline(input, resource) || !identity_component(family) ||
		publisher.empty() || (!resource.empty() && !identity_component(resource)))
		return std::nullopt;
	const auto name = full_name.substr(0, version_at), publisher_id = full_name.substr(id_at + 1);
	if (name.empty() || publisher_id.empty() || family != name + "_" + publisher_id ||
		resource != full_name.substr(architecture_end + 1, id_at - architecture_end - 1))
		return std::nullopt;
	const auto wide_name = from_utf8(full_name);
	return MsixApplication{wide_name, from_utf8(family), from_utf8(name), from_utf8(publisher), from_utf8(resource), from_utf8(publisher_id),
		{}, {}, std::u16string(windows_apps) + u"\\" + wide_name, version, machine};
}

std::optional<MsixApplication> application_in(const std::filesystem::path &directory, std::u16string_view aumid, std::u16string_view image)
{
	auto record = package_record(directory);
	if (!record || (!aumid.empty() && (aumid.size() <= record->family.size() + 1 || aumid[record->family.size()] != u'!' ||
			!equal_fold(aumid.substr(0, record->family.size()), record->family))))
		return std::nullopt;
	std::ifstream input(directory / ".raven-package", std::ios::binary);
	std::string line;
	for (int field = 0; field < 3; ++field)
		std::getline(input, line);
	while (std::getline(input, line)) {
		const size_t delimiter = line.find('\t');
		if (delimiter == line.npos)
			continue;
		auto id = from_utf8(line.substr(0, delimiter));
		auto relative = from_utf8(line.substr(delimiter + 1));
		if (id.size() <= record->family.size() + 1 || id[record->family.size()] != u'!' ||
			!equal_fold(std::u16string_view(id).substr(0, record->family.size()), record->family) || !relative_file(relative))
			continue;
		if (!aumid.empty() && !equal_fold(aumid, id))
			continue;
		for (char16_t &character : relative)
			if (character == u'/')
				character = u'\\';
		auto path = record->directory + u"\\" + relative;
		if (!image.empty() && !equal_fold(image, path))
			continue;
		record->image = std::move(path);
		record->aumid = std::move(id);
		return record;
	}
	return std::nullopt;
}

#ifdef RAVEN_OPENSSL

std::optional<std::string> read_text_file(const std::filesystem::path &path)
{
	std::error_code error;
	const auto length = std::filesystem::file_size(path, error);
	if (error || length > 16 * 1024 * 1024)
		return std::nullopt;
	std::string result(length, '\0');
	std::ifstream input(path, std::ios::binary);
	if (!input.read(result.data(), std::streamsize(result.size())) || input.peek() != std::ifstream::traits_type::eof())
		return std::nullopt;
	return result;
}

const EVP_MD *digest_method(size_t size)
{
	switch (size) {
	case 32: return EVP_sha256();
	case 48: return EVP_sha384();
	case 64: return EVP_sha512();
	default: return nullptr;
	}
}

bool digest_matches(const EVP_MD *method, const void *data, size_t size, std::span<const uint8_t> expected)
{
	std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
	unsigned length = 0;
	return method && EVP_Digest(data, size, digest.data(), &length, method, nullptr) == 1 && length == expected.size() &&
		std::equal(expected.begin(), expected.end(), digest.begin());
}

struct XmlTag {
	std::string_view name;
	std::map<std::string_view, std::string_view> attributes;
	bool closing, empty;
};

std::optional<XmlTag> next_tag(std::string_view &xml)
{
	constexpr std::string_view spaces = " \n\r\t";
	for (auto beginning = xml.find('<'); beginning != xml.npos; beginning = xml.find('<')) {
		xml.remove_prefix(beginning + 1);
		if (xml.starts_with("!--") || xml.starts_with("?")) {
			const std::string_view terminator = xml.starts_with("?") ? "?>" : "-->";
			const size_t end = xml.find(terminator);
			if (end == xml.npos)
				break;
			xml.remove_prefix(end + terminator.size());
			continue;
		}
		if (xml.empty() || xml.front() == '!')
			break;
		const bool closing = xml.front() == '/';
		if (closing)
			xml.remove_prefix(1);
		const auto name_size = xml.find_first_of(" \n\r\t/>");
		if (!name_size || name_size == xml.npos)
			break;
		auto name = xml.substr(0, name_size);
		if (const auto colon = name.find(':'); colon != name.npos)
			name.remove_prefix(colon + 1);
		xml.remove_prefix(name_size);
		XmlTag tag{name, {}, closing, false};
		while (!xml.empty()) {
			const size_t first = xml.find_first_not_of(spaces);
			if (first == xml.npos)
				break;
			xml.remove_prefix(first);
			if (xml.starts_with('>') || xml.starts_with("/>")) {
				tag.empty = xml.front() == '/';
				xml.remove_prefix(tag.empty ? 2 : 1);
				return tag;
			}
			const size_t equals = xml.find_first_of("= \n\r\t");
			if (closing || !equals || equals == xml.npos)
				break;
			const auto key = xml.substr(0, equals);
			xml.remove_prefix(equals);
			const size_t equal = xml.find_first_not_of(spaces);
			if (equal == xml.npos || xml[equal] != '=')
				break;
			xml.remove_prefix(equal + 1);
			const size_t quote = xml.find_first_not_of(spaces);
			if (quote == xml.npos || (xml[quote] != '"' && xml[quote] != '\''))
				break;
			const size_t end = xml.find(xml[quote], quote + 1);
			if (end == xml.npos || !tag.attributes.emplace(key, xml.substr(quote + 1, end - quote - 1)).second)
				break;
			xml.remove_prefix(end + 1);
		}
		break;
	}
	xml = {};
	return std::nullopt;
}

std::string unescape_xml(std::string_view text)
{
	std::string result;
	while (!text.empty()) {
		const size_t entity = text.find('&');
		result += text.substr(0, entity);
		if (entity == text.npos)
			return result;
		text.remove_prefix(entity);
		const auto end = text.find(';');
		if (end == text.npos)
			return {};
		const auto name = text.substr(0, end + 1);
		if (name == "&amp;") result += '&';
		else if (name == "&quot;") result += '"';
		else if (name == "&apos;") result += '\'';
		else if (name == "&lt;") result += '<';
		else if (name == "&gt;") result += '>';
		else return {};
		text.remove_prefix(end + 1);
	}
	return result;
}

std::string attribute(const XmlTag &tag, std::string_view name)
{
	const auto found = tag.attributes.find(name);
	return found == tag.attributes.end() ? std::string{} : unescape_xml(found->second);
}

std::optional<std::string> validate_block_map(const std::filesystem::path &stage, const VerifiedPackage &proof)
{
	const auto map = read_text_file(stage / "AppxBlockMap.xml");
	const auto *method = digest_method(proof.block_map.size());
	if (!map || !digest_matches(method, map->data(), map->size(), proof.block_map))
		return std::nullopt;
	std::string_view xml = *map;
	std::set<std::string> files{"AppxBlockMap.xml", ".raven-package"};
	const auto root = next_tag(xml);
	if (!root || root->name != "BlockMap" || root->closing || root->empty)
		return std::nullopt;
	bool ended = false;
	while (auto file = next_tag(xml)) {
		if (file->closing) {
			ended = file->name == "BlockMap";
			break;
		}
		auto name = attribute(*file, "Name");
		auto size = attribute(*file, "Size");
		uint64_t remaining = 0;
		const auto [end, error] = std::from_chars(size.data(), size.data() + size.size(), remaining);
		if (file->name != "File" || !relative_file(from_utf8(name)) || error != std::errc() || end != size.data() + size.size())
			return std::nullopt;
		std::replace(name.begin(), name.end(), '\\', '/');
		if (!files.insert(name).second)
			return std::nullopt;
		std::ifstream input(stage / name, std::ios::binary);
		if (!input)
			return std::nullopt;
		std::array<char, 65536> data;
		while (remaining) {
			const auto block = next_tag(xml);
			if (!block || block->name != "Block" || block->closing || !block->empty)
				return std::nullopt;
			const auto hash = attribute(*block, "Hash");
			std::array<uint8_t, EVP_MAX_MD_SIZE + 2> decoded{};
			if (hash.size() != (proof.block_map.size() + 2) / 3 * 4 ||
				EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char *>(hash.data()), int(hash.size())) < 0)
				return std::nullopt;
			const size_t count = std::min<uint64_t>(data.size(), remaining);
			if (!input.read(data.data(), std::streamsize(count)) ||
				!digest_matches(method, data.data(), count, std::span(decoded).first(proof.block_map.size())))
				return std::nullopt;
			remaining -= count;
		}
		if (input.peek() != std::ifstream::traits_type::eof())
			return std::nullopt;
		if (!file->empty) {
			const auto close = next_tag(xml);
			if (!close || !close->closing || close->name != "File")
				return std::nullopt;
		}
	}
	if (!ended || !files.contains("AppxManifest.xml"))
		return std::nullopt;
	std::error_code error;
	for (std::filesystem::recursive_directory_iterator tree(stage, error), end; !error && tree != end; tree.increment(error))
		if (tree->is_regular_file(error) && !files.erase(tree->path().lexically_relative(stage).string()))
			return std::nullopt;
	if (error || !files.empty())
		return std::nullopt;
	return read_text_file(stage / "AppxManifest.xml");
}

bool validate_manifest(std::string_view xml, const std::filesystem::path &stage, const MsixApplication &record)
{
	std::vector<std::string_view> parents;
	std::map<std::string, std::string> applications;
	bool has_identity = false;
	while (auto tag = next_tag(xml)) {
		if (tag->closing) {
			if (parents.empty() || parents.back() != tag->name)
				return false;
			parents.pop_back();
			continue;
		}
		if (parents.empty() && tag->name != "Package")
			return false;
		if (parents.size() == 1 && tag->name == "Identity") {
			const auto publisher = from_utf8(attribute(*tag, "Publisher"));
			std::array<uint8_t, 32> digest{};
			unsigned length = 0;
			if (has_identity || publisher != record.publisher ||
				EVP_Digest(publisher.data(), publisher.size() * sizeof(char16_t), digest.data(), &length, EVP_sha256(), nullptr) != 1)
				return false;
			constexpr std::string_view alphabet = "0123456789abcdefghjkmnpqrstvwxyz";
			uint64_t bits = 0;
			for (size_t i = 0; i < 8; ++i)
				bits = bits << 8 | digest[i];
			std::string id;
			for (int shift = 59; shift >= -1; shift -= 5)
				id += alphabet[(shift < 0 ? bits << 1 : bits >> shift) & 31];
			const auto full = attribute(*tag, "Name") + "_" + attribute(*tag, "Version") + "_" +
				attribute(*tag, "ProcessorArchitecture") + "_" + attribute(*tag, "ResourceId") + "_" + id;
			if (from_utf8(full) != record.full_name)
				return false;
			has_identity = true;
		}
		if (parents.size() == 2 && parents.back() == "Applications" && tag->name == "Application" &&
			attribute(*tag, "EntryPoint") == "Windows.FullTrustApplication") {
			const auto id = attribute(*tag, "Id"), executable = attribute(*tag, "Executable");
			if (id.empty() || !relative_file(from_utf8(executable)) || !applications.emplace(to_utf8(record.family) + "!" + id, executable).second)
				return false;
		}
		if (!tag->empty)
			parents.push_back(tag->name);
	}
	if (!parents.empty() || !has_identity || applications.empty())
		return false;
	std::ifstream input(stage / ".raven-package", std::ios::binary);
	std::string line;
	for (int field = 0; field < 3; ++field)
		std::getline(input, line);
	while (std::getline(input, line)) {
		const auto delimiter = line.find('\t');
		const auto app = applications.find(line.substr(0, delimiter));
		if (delimiter == line.npos || app == applications.end() || app->second != line.substr(delimiter + 1))
			return false;
		applications.erase(app);
	}
	return applications.empty();
}

#endif

uint32_t publish_package(KernelContext &context, std::u16string_view stage, std::u16string_view full_name)
{
	const auto proof = take_verification(context.environment.thread_id());
	if (proof.block_map.empty())
		return 0x800b0100U;
	const auto name = to_utf8(full_name);
	if (!identity_component(name))
		return 0x80070057U;
	const auto source = context.paths.resolve(stage);
	const auto temp = context.paths.resolve(context.paths.temporary());
	if (!source || !temp || !source->exists || source->host.parent_path() != temp->host ||
		!source->host.filename().string().starts_with(".raven-msix-"))
		return 0x80070057U;
	const NativePrefix prefix = NativePrefix::from_environment();
	const auto store = msix_store(prefix);
	std::error_code error;
	if (!std::filesystem::is_directory(std::filesystem::symlink_status(source->host, error)) || error)
		return 0x80070005U;
	std::filesystem::create_directory(store, error);
	if (error || !std::filesystem::is_directory(std::filesystem::symlink_status(store, error)) || error)
		return 0x80070005U;
	const auto pending = store / source->host.filename();
	if (!std::filesystem::create_directory(pending, error) || error)
		return 0x80070005U;
	struct Pending {
		std::filesystem::path path;
		~Pending() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
	} cleanup{pending};
	std::filesystem::copy(source->host, pending, std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks, error);
	if (error)
		return 0x80070005U;
	for (std::filesystem::recursive_directory_iterator files(pending, error), end; !error && files != end; files.increment(error)) {
		const auto status = files->symlink_status(error);
		if (error || (!std::filesystem::is_directory(status) &&
				(!std::filesystem::is_regular_file(status) || files->hard_link_count(error) != 1 || error)))
			return 0x80070005U;
	}
	if (error)
		return 0x80070005U;
#ifdef RAVEN_OPENSSL
	const auto record = package_record(pending, name);
	const auto manifest = validate_block_map(pending, proof);
	if (!record || record->publisher != proof.publisher || !manifest || !validate_manifest(*manifest, pending, *record))
		return 0x80096010U;
#else
	return 0x800b0100U;
#endif
	const auto target = store / name;
	if (::syscall(SYS_renameat2, AT_FDCWD, pending.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) < 0)
		return errno == EEXIST ? 0x800700b7U : 0x80070005U;
	return 0;
}

}

std::filesystem::path msix_store(const NativePrefix &prefix)
{
	return prefix.root() / "msix-packages";
}

void remember_msix_verification(uint32_t thread, std::u16string_view publisher, std::span<const uint8_t> hashes)
{
	std::lock_guard lock(verification_lock);
	verifications.erase(thread);
	for (const size_t size : {32u, 48u, 64u}) {
		const size_t position = 4 + 3 * (4 + size);
		if ((hashes.size() == 4 + 4 * (4 + size) || hashes.size() == 4 + 5 * (4 + size)) &&
			std::memcmp(hashes.data() + position, "AXBM", 4) == 0) {
			const auto digest = hashes.subspan(position + 4, size);
			verifications.emplace(thread, VerifiedPackage{std::u16string(publisher), {digest.begin(), digest.end()}});
			break;
		}
	}
}

std::optional<MsixApplication> find_msix_application(const NativePrefix &prefix, std::u16string_view aumid, std::u16string_view image)
{
	if (aumid.empty() && image.empty())
		return std::nullopt;
	const auto store = msix_store(prefix);
	if (!image.empty()) {
		if (image.size() <= windows_apps.size() + 1 || !equal_fold(image.substr(0, windows_apps.size()), windows_apps) ||
			image[windows_apps.size()] != u'\\')
			return std::nullopt;
		const auto leaf = image.substr(windows_apps.size() + 1);
		const auto count = leaf.find(u'\\');
		const auto name = count == leaf.npos ? std::string() : to_utf8(leaf.substr(0, count));
		return identity_component(name) ? application_in(store / name, aumid, image) : std::nullopt;
	}
	std::error_code error;
	std::optional<MsixApplication> best;
	for (std::filesystem::directory_iterator packages(store, error), end; !error && packages != end; packages.increment(error))
		if (packages->is_directory(error) && !error)
			if (auto app = application_in(packages->path(), aumid, {}); app && (!best || app->version > best->version))
				best = std::move(app);
	return best;
}

void register_msix_api(ApiRegistry &registry, KernelContext &context)
{
	registry.add("RavenPublishPackage", 2, [&context](ApiCall &call) {
		call.ret(call[0] && call[1] ? publish_package(context, call.wide(call[0]), call.wide(call[1], 129)) : 0x80070057U);
	});
	std::shared_ptr<const MsixApplication> app;
	const NativePrefix prefix = NativePrefix::from_environment();
	if (!context.launch.app_user_model_id.empty())
		if (auto found = find_msix_application(prefix, context.launch.app_user_model_id, context.launch.image_path))
			app = std::make_shared<MsixApplication>(std::move(*found));
	const auto write_name = [](ApiCall &call, uint64_t length, uint64_t output, const std::u16string *value) {
		if (!value) {
			if (length)
				call.store<uint32_t>(length, 0);
			call.ret(15700);
			return;
		}
		if (!length) {
			call.ret(87);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(length);
		const uint32_t required = uint32_t(value->size() + 1);
		call.store<uint32_t>(length, required);
		if (capacity < required || !output) {
			call.ret(122);
			return;
		}
		call.put_wide(output, *value, capacity);
		call.ret(0);
	};
	for (const auto &[name, other, field] : {
		std::tuple{"GetCurrentPackageFullName", "GetPackageFullName", &MsixApplication::full_name},
		std::tuple{"GetCurrentPackageFamilyName", "GetPackageFamilyName", &MsixApplication::family},
		std::tuple{"GetCurrentApplicationUserModelId", "GetApplicationUserModelId", &MsixApplication::aumid},
		std::tuple{"GetCurrentPackagePath", static_cast<const char *>(nullptr), &MsixApplication::directory}}) {
		registry.add(name, 2, [app, field, write_name](ApiCall &call) { write_name(call, call[0], call[1], app ? &((*app).*field) : nullptr); });
		if (other)
			registry.add(other, 3, [app, field, write_name](ApiCall &call) {
				const auto value = app && (call[0] == UINT64_MAX || (call.pointer_width() == 4 && call[0] == UINT32_MAX)) ? &((*app).*field) : nullptr;
				write_name(call, call[1], call[2], value);
			});
	}
	registry.add("GetCurrentPackageId", 2, [app](ApiCall &call) {
		if (!app) {
			if (call[0])
				call.store<uint32_t>(call[0], 0);
			call.ret(15700);
			return;
		}
		if (!call[0]) {
			call.ret(87);
			return;
		}
		const std::array fields{&app->name, &app->publisher, &app->resource, &app->publisher_id};
		const size_t width = call.pointer_width(), header = 16 + fields.size() * width;
		size_t required = header;
		for (const auto *field : fields)
			required += (field->size() + 1) * sizeof(char16_t);
		const auto capacity = call.load<uint32_t>(call[0]);
		call.store<uint32_t>(call[0], uint32_t(required));
		if (!call[1] || capacity < required) {
			call.ret(122);
			return;
		}
		call.store<uint32_t>(call[1], 0);
		call.store<uint32_t>(call[1] + 4, app->architecture);
		call.store<uint64_t>(call[1] + 8, app->version);
		uint64_t text = call[1] + header;
		for (size_t index = 0; index < fields.size(); ++index) {
			call.store_pointer(call[1] + 16 + index * width, text);
			call.put_wide(text, *fields[index], fields[index]->size() + 1);
			text += (fields[index]->size() + 1) * sizeof(char16_t);
		}
		call.ret(0);
	});
	registry.add("GetPackagesByPackageFamily", 5, [prefix](ApiCall &call) {
		if (!call[0] || !call[1] || !call[3]) {
			call.ret(87);
			return;
		}
		const auto family = call.wide(call[0], 129);
		std::vector<std::u16string> names;
		std::error_code error;
		for (std::filesystem::directory_iterator packages(msix_store(prefix), error), end; !error && packages != end; packages.increment(error))
			if (auto package = package_record(packages->path()); package && equal_fold(package->family, family))
				names.push_back(std::move(package->full_name));
		std::sort(names.begin(), names.end());
		size_t required = 0;
		for (const auto &name : names)
			required += name.size() + 1;
		const auto count = call.load<uint32_t>(call[1]), length = call.load<uint32_t>(call[3]);
		call.store<uint32_t>(call[1], uint32_t(names.size()));
		call.store<uint32_t>(call[3], uint32_t(required));
		if (!names.empty() && (!call[2] || !call[4] || count < names.size() || length < required)) {
			call.ret(122);
			return;
		}
		uint64_t text = call[4];
		for (size_t index = 0; index < names.size(); ++index) {
			call.store_pointer(call[2] + index * call.pointer_width(), text);
			call.put_wide(text, names[index], names[index].size() + 1);
			text += (names[index].size() + 1) * sizeof(char16_t);
		}
		call.ret(0);
	});
}

}
