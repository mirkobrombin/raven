#include "authenticode.h"

#ifdef RAVEN_OPENSSL
#include <openssl/cms.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/ts.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace raven {

#ifdef RAVEN_OPENSSL

namespace {

constexpr uint32_t trust_no_signature = 0x800b0100, trust_unknown_subject = 0x800b0003, trust_bad_digest = 0x80096010,
	trust_bad_signature = 0x80096004, certificate_expired = 0x800b0101, certificate_untrusted_root = 0x800b0109,
	certificate_chaining = 0x800b010a;

uint32_t le32(std::span<const uint8_t> bytes, size_t at)
{
	uint32_t value = 0;
	if (at + 4 <= bytes.size())
		std::memcpy(&value, bytes.data() + at, 4);
	return value;
}

uint16_t le16(std::span<const uint8_t> bytes, size_t at)
{
	uint16_t value = 0;
	if (at + 2 <= bytes.size())
		std::memcpy(&value, bytes.data() + at, 2);
	return value;
}

// A signed file: the PKCS #7 signature it carries, and how its content is hashed for the digest the signature names.
struct Signed {
	std::vector<uint8_t> signature;
	std::function<void(EVP_MD_CTX *)> hash;
};

void update(EVP_MD_CTX *context, std::span<const uint8_t> bytes, size_t from, size_t to)
{
	if (from < to && to <= bytes.size())
		EVP_DigestUpdate(context, bytes.data() + from, to - from);
}

// A program or library: the WIN_CERTIFICATE the security directory names; the digest covers the file but the checksum, the security
// directory entry and the certificate table.
std::optional<Signed> portable_executable(std::span<const uint8_t> file)
{
	const uint32_t header = le32(file, 0x3c);
	if (header + 24 + 96 > file.size() || le32(file, header) != 0x4550)
		return std::nullopt;
	const size_t optional = header + 24, checksum = optional + 64;
	const size_t directories = optional + (le16(file, optional) == 0x20b ? 112 : 96), security = directories + 4 * 8;
	const uint32_t table = le32(file, security), size = le32(file, security + 4);
	if (!table || size < 8 || uint64_t(table) + size > file.size())
		return Signed{};
	const uint32_t length = le32(file, table);
	if (le16(file, table + 6) != 2 || length < 8 || length > size)
		return Signed{};
	return Signed{std::vector<uint8_t>(file.begin() + table + 8, file.begin() + table + length), [file, checksum, security, table](EVP_MD_CTX *context) {
		update(context, file, 0, checksum);
		update(context, file, checksum + 4, security);
		update(context, file, security + 8, table);
	}};
}

// A cabinet: the per-cabinet reserve says where the signature follows the cabinet; the digest covers the header fields that do not
// change when the cabinet is signed, the folder entries and everything from the file entries to the signature.
std::optional<Signed> cabinet(std::span<const uint8_t> file)
{
	const uint16_t flags = le16(file, 30);
	if (!(flags & 4) || le16(file, 36) < 20)
		return Signed{};
	const uint32_t at = le32(file, 44), size = le32(file, 48);
	if (!at || uint64_t(at) + size > file.size())
		return Signed{};
	return Signed{std::vector<uint8_t>(file.begin() + at, file.begin() + at + size), [file, flags, at](EVP_MD_CTX *context) {
		// The signature, the size and the offset of the file entries, the version, the counts, the flags, the set, and the end of
		// the reserve; not the reserved words, the cabinet's place in its set or where the signature is.
		static constexpr std::pair<size_t, size_t> fields[] = {{0, 4}, {8, 16}, {16, 20}, {20, 26}, {26, 30}, {30, 32}, {32, 34}, {56, 60}};
		for (const auto &[from, to] : fields)
			update(context, file, from, to);
		size_t cursor = 36 + 4 + le16(file, 36);
		// The names of the previous and next cabinets of a set, with their terminators.
		for (const uint16_t flag : {1, 2})
			if (flags & flag)
				for (int text = 0; text < 2; ++text) {
					const size_t start = cursor;
					while (cursor < file.size() && file[cursor])
						++cursor;
					update(context, file, start, ++cursor);
				}
		const size_t folder_size = 8 + file[38];
		for (uint16_t folder = 0; folder < le16(file, 26); ++folder)
			update(context, file, cursor + folder * folder_size, cursor + folder * folder_size + 8);
		update(context, file, le32(file, 16), at);
	}};
}

// The parts of a compound file, the container of installer databases: its directory and the content of each stream.
class CompoundFile {
public:
	struct Entry {
		std::vector<uint8_t> name;
		uint8_t type = 0;
		uint32_t left = 0, right = 0, child = 0, start = 0, size = 0;
		std::array<uint8_t, 16> clsid{};
		// The entry's state bits, its creation and modification times and the low half of its size, as stored.
		std::array<uint8_t, 4> state{}, size_low{};
		std::array<uint8_t, 16> times{};
	};

	explicit CompoundFile(std::span<const uint8_t> file) : file(file)
	{
		sector = size_t(1) << le16(file, 30);
		mini_sector = size_t(1) << le16(file, 32);
		cutoff = le32(file, 56);
		std::vector<uint32_t> fat_sectors;
		for (size_t index = 0; index < 109; ++index)
			fat_sectors.push_back(le32(file, 76 + index * 4));
		for (uint32_t next = le32(file, 68), count = le32(file, 72); count && next < 0xfffffffa && fat_sectors.size() < 1 << 20; --count) {
			for (size_t index = 0; index + 1 < sector / 4; ++index)
				fat_sectors.push_back(le32(file, offset(next) + index * 4));
			next = le32(file, offset(next) + sector - 4);
		}
		fat_sectors.resize(std::min<size_t>(fat_sectors.size(), le32(file, 44)));
		for (const auto at : fat_sectors)
			for (size_t index = 0; index < sector / 4; ++index)
				fat.push_back(le32(file, offset(at) + index * 4));
		const auto directory = read(le32(file, 48));
		for (size_t at = 0; at + 128 <= directory.size(); at += 128) {
			Entry entry;
			const size_t name_size = std::min<size_t>(le16(directory, at + 64), 64);
			entry.name.assign(directory.begin() + at, directory.begin() + at + name_size);
			entry.type = directory[at + 66];
			entry.left = le32(directory, at + 68);
			entry.right = le32(directory, at + 72);
			entry.child = le32(directory, at + 76);
			std::memcpy(entry.clsid.data(), directory.data() + at + 80, 16);
			std::memcpy(entry.state.data(), directory.data() + at + 96, 4);
			std::memcpy(entry.times.data(), directory.data() + at + 100, 16);
			std::memcpy(entry.size_low.data(), directory.data() + at + 120, 4);
			entry.start = le32(directory, at + 116);
			entry.size = le32(directory, at + 120);
			entries.push_back(std::move(entry));
		}
		if (!entries.empty()) {
			mini_stream = read(entries[0].start, entries[0].size);
			const auto table = read(le32(file, 60));
			for (size_t at = 0; at + 4 <= table.size(); at += 4)
				mini_fat.push_back(le32(table, at));
		}
	}

	std::vector<uint8_t> stream(const Entry &entry) const
	{
		if (entry.size >= cutoff)
			return read(entry.start, entry.size);
		std::vector<uint8_t> bytes;
		for (uint32_t at = entry.start; at < 0xfffffffa && at < mini_fat.size() && bytes.size() < entry.size; at = mini_fat[at])
			if ((at + 1) * mini_sector <= mini_stream.size())
				bytes.insert(bytes.end(), mini_stream.begin() + at * mini_sector, mini_stream.begin() + (at + 1) * mini_sector);
		bytes.resize(std::min<size_t>(bytes.size(), entry.size));
		return bytes;
	}

	// A storage's entries, from the tree its child entry roots.
	std::vector<const Entry *> children(const Entry &storage) const
	{
		std::vector<const Entry *> found;
		std::vector<uint32_t> pending{storage.child};
		while (!pending.empty() && found.size() < entries.size()) {
			const uint32_t at = pending.back();
			pending.pop_back();
			if (at >= entries.size())
				continue;
			found.push_back(&entries[at]);
			pending.push_back(entries[at].left);
			pending.push_back(entries[at].right);
		}
		return found;
	}

	std::vector<Entry> entries;

private:
	size_t offset(uint32_t at) const { return (size_t(at) + 1) * sector; }
	std::vector<uint8_t> read(uint32_t at, size_t limit = SIZE_MAX) const
	{
		std::vector<uint8_t> bytes;
		for (size_t steps = 0; at < 0xfffffffa && at < fat.size() && bytes.size() < limit && steps <= fat.size(); at = fat[at], ++steps)
			if (offset(at) + sector <= file.size())
				bytes.insert(bytes.end(), file.begin() + offset(at), file.begin() + offset(at) + sector);
		if (limit != SIZE_MAX)
			bytes.resize(std::min(bytes.size(), limit));
		return bytes;
	}

	std::span<const uint8_t> file;
	size_t sector = 512, mini_sector = 64, cutoff = 4096;
	std::vector<uint32_t> fat, mini_fat;
	std::vector<uint8_t> mini_stream;
};

// A directory entry's name as the compound file stores it: UTF-16 with its terminator.
std::vector<uint8_t> entry_name(std::u16string_view text)
{
	std::vector<uint8_t> bytes;
	for (const auto character : text) {
		bytes.push_back(uint8_t(character));
		bytes.push_back(uint8_t(character >> 8));
	}
	bytes.insert(bytes.end(), {0, 0});
	return bytes;
}

// An installer database: the signature is the DigitalSignature stream; the digest covers every other stream, storage by storage in the
// order of their names as UTF-16 bytes, each storage followed by its class identifier. A database that also carries
// MsiDigitalSignatureEx has its entries' metadata hashed first, and that hash, which the stream holds, begins the digest.
std::optional<Signed> installer_database(std::span<const uint8_t> file)
{
	auto compound = std::make_shared<CompoundFile>(file);
	if (compound->entries.empty())
		return std::nullopt;
	const auto signature_name = entry_name(u"\x05" u"DigitalSignature"), extended_name = entry_name(u"\x05" u"MsiDigitalSignatureEx");
	std::vector<uint8_t> signature, metadata;
	bool extended = false;
	for (const auto *entry : compound->children(compound->entries[0]))
		if (entry->type == 2 && entry->name == signature_name)
			signature = compound->stream(*entry);
		else if (entry->type == 2 && entry->name == extended_name) {
			metadata = compound->stream(*entry);
			extended = true;
		}
	if (signature.empty())
		return Signed{};
	const auto sorted = [compound](const CompoundFile::Entry &storage) {
		auto children = compound->children(storage);
		std::sort(children.begin(), children.end(), [](const auto *left, const auto *right) {
			const size_t common = std::min(left->name.size(), right->name.size());
			const int order = std::memcmp(left->name.data(), right->name.data(), common);
			return order ? order < 0 : left->name.size() < right->name.size();
		});
		return children;
	};
	const auto skipped = [signature_name, extended_name](const CompoundFile::Entry &entry) {
		return entry.type == 2 && (entry.name == signature_name || entry.name == extended_name);
	};
	return Signed{std::move(signature), [compound, sorted, skipped, metadata, extended](EVP_MD_CTX *context) {
		if (extended) {
			// The metadata hash: every entry's name without its terminator (the root has none), the class identifier of a storage or
			// the low half of a stream's size, the state bits, and the times of all but the root.
			std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> prehash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
			EVP_DigestInit_ex(prehash.get(), EVP_MD_CTX_get0_md(context), nullptr);
			const auto describe = [&](const CompoundFile::Entry &entry) {
				const bool root = entry.type == 5;
				if (!root && entry.name.size() >= 2)
					EVP_DigestUpdate(prehash.get(), entry.name.data(), entry.name.size() - 2);
				if (entry.type == 2)
					EVP_DigestUpdate(prehash.get(), entry.size_low.data(), entry.size_low.size());
				else
					EVP_DigestUpdate(prehash.get(), entry.clsid.data(), entry.clsid.size());
				EVP_DigestUpdate(prehash.get(), entry.state.data(), entry.state.size());
				if (!root)
					EVP_DigestUpdate(prehash.get(), entry.times.data(), entry.times.size());
			};
			const std::function<void(const CompoundFile::Entry &, int)> walk = [&](const CompoundFile::Entry &storage, int depth) {
				describe(storage);
				for (const auto *child : sorted(storage)) {
					if (skipped(*child))
						continue;
					if (child->type == 2)
						describe(*child);
					else if (child->type == 1 && depth < 32)
						walk(*child, depth + 1);
				}
			};
			walk(compound->entries[0], 0);
			unsigned char computed[EVP_MAX_MD_SIZE];
			unsigned int size = 0;
			EVP_DigestFinal_ex(prehash.get(), computed, &size);
			// Metadata that no longer matches its hash spoils the digest, as the signature then covers other content.
			if (metadata.size() != size || std::memcmp(metadata.data(), computed, size) != 0)
				EVP_DigestUpdate(context, computed, size);
			EVP_DigestUpdate(context, metadata.data(), metadata.size());
		}
		const std::function<void(const CompoundFile::Entry &, int)> storage = [&](const CompoundFile::Entry &entry, int depth) {
			for (const auto *child : sorted(entry)) {
				if (child->type == 2 && !skipped(*child)) {
					const auto bytes = compound->stream(*child);
					EVP_DigestUpdate(context, bytes.data(), bytes.size());
				} else if (child->type == 1 && depth < 32) {
					storage(*child, depth + 1);
				}
			}
			EVP_DigestUpdate(context, entry.clsid.data(), entry.clsid.size());
		};
		storage(compound->entries[0], 0);
	}};
}

// A DER length after its tag at `at`: the header size and the content size.
std::optional<std::pair<size_t, size_t>> der_header(std::span<const uint8_t> bytes, size_t at)
{
	if (at + 2 > bytes.size())
		return std::nullopt;
	const uint8_t first = bytes[at + 1];
	if (first < 0x80)
		return std::pair{size_t(2), size_t(first)};
	const size_t count = first & 0x7f;
	if (!count || count > 4 || at + 2 + count > bytes.size())
		return std::nullopt;
	size_t length = 0;
	for (size_t index = 0; index < count; ++index)
		length = length << 8 | bytes[at + 2 + index];
	return std::pair{2 + count, length};
}

// The time a timestamp countersignature gives the signature, as seconds since 1970.
std::optional<time_t> stamped_time(PKCS7_SIGNER_INFO *signer)
{
	const auto to_seconds = [](const ASN1_TIME *time) -> std::optional<time_t> {
		std::tm parts{};
		if (!time || !ASN1_TIME_to_tm(time, &parts))
			return std::nullopt;
		return timegm(&parts);
	};
	if (const ASN1_TYPE *counter = PKCS7_get_attribute(signer, NID_pkcs9_countersignature); counter && counter->type == V_ASN1_SEQUENCE) {
		const unsigned char *at = ASN1_STRING_get0_data(counter->value.sequence);
		std::unique_ptr<PKCS7_SIGNER_INFO, decltype(&PKCS7_SIGNER_INFO_free)> info(
			d2i_PKCS7_SIGNER_INFO(nullptr, &at, ASN1_STRING_length(counter->value.sequence)), PKCS7_SIGNER_INFO_free);
		if (info)
			if (const ASN1_TYPE *signed_at = PKCS7_get_signed_attribute(info.get(), NID_pkcs9_signingTime); signed_at && signed_at->type == V_ASN1_UTCTIME)
				return to_seconds(signed_at->value.utctime);
	}
	// An RFC 3161 timestamp token, which newer signatures carry under Microsoft's attribute; its identifier is compared as text, since the
	// library may not know it.
	const STACK_OF(X509_ATTRIBUTE) *unsigned_attributes = PKCS7_get_attributes(signer);
	for (int index = 0; index < sk_X509_ATTRIBUTE_num(unsigned_attributes); ++index) {
		X509_ATTRIBUTE *attribute = sk_X509_ATTRIBUTE_value(unsigned_attributes, index);
		char name[64] = {};
		OBJ_obj2txt(name, sizeof(name), X509_ATTRIBUTE_get0_object(attribute), 1);
		const ASN1_TYPE *token = X509_ATTRIBUTE_get0_type(attribute, 0);
		if (std::strcmp(name, "1.3.6.1.4.1.311.3.3.1") != 0 || !token || token->type != V_ASN1_SEQUENCE)
			continue;
		// The token is CMS signed data, which the PKCS #7 parser does not accept in every form; its content is the TSTInfo.
		const unsigned char *at = ASN1_STRING_get0_data(token->value.sequence);
		std::unique_ptr<CMS_ContentInfo, decltype(&CMS_ContentInfo_free)> signed_token(
			d2i_CMS_ContentInfo(nullptr, &at, ASN1_STRING_length(token->value.sequence)), CMS_ContentInfo_free);
		ASN1_OCTET_STRING **content = signed_token ? CMS_get0_content(signed_token.get()) : nullptr;
		if (content && *content) {
			const unsigned char *info_at = ASN1_STRING_get0_data(*content);
			std::unique_ptr<TS_TST_INFO, decltype(&TS_TST_INFO_free)> info(d2i_TS_TST_INFO(nullptr, &info_at, ASN1_STRING_length(*content)), TS_TST_INFO_free);
			if (info)
				return to_seconds(TS_TST_INFO_get_time(info.get()));
		}
	}
	return std::nullopt;
}

std::vector<uint8_t> der_of(X509 *certificate)
{
	const int length = i2d_X509(certificate, nullptr);
	std::vector<uint8_t> bytes(length > 0 ? size_t(length) : 0);
	unsigned char *at = bytes.data();
	if (length > 0)
		i2d_X509(certificate, &at);
	return bytes;
}

}

X509_STORE *trust_anchors()
{
	static X509_STORE *store = [] {
		auto *created = X509_STORE_new();
		if (!created)
			return created;
		for (const char *path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"})
			if (X509_STORE_load_file(created, path))
				break;
		std::error_code error;
		const auto roots = std::filesystem::read_symlink("/proc/self/exe", error).parent_path() / "roots";
		for (const auto &entry : std::filesystem::directory_iterator(roots, error)) {
			std::ifstream input(entry.path(), std::ios::binary);
			const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			const auto *at = reinterpret_cast<const unsigned char *>(bytes.data());
			if (X509 *certificate = d2i_X509(nullptr, &at, long(bytes.size()))) {
				X509_STORE_add_cert(created, certificate);
				X509_free(certificate);
			}
		}
		return created;
	}();
	return store;
}

namespace {

std::optional<Signed> signed_content(std::span<const uint8_t> file)
{
	if (file.size() >= 0x40 && file[0] == 'M' && file[1] == 'Z')
		return portable_executable(file);
	if (file.size() >= 60 && std::memcmp(file.data(), "MSCF", 4) == 0)
		return cabinet(file);
	if (file.size() >= 512 && le32(file, 0) == 0xe011cfd0 && le32(file, 4) == 0xe11ab1a1)
		return installer_database(file);
	return std::nullopt;
}

}

std::vector<uint8_t> embedded_signature(std::span<const uint8_t> file)
{
	auto found = signed_content(file);
	return found ? std::move(found->signature) : std::vector<uint8_t>();
}

std::vector<uint8_t> subject_digest(std::span<const uint8_t> file, bool sha256)
{
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	if (!context || !EVP_DigestInit_ex(context.get(), sha256 ? EVP_sha256() : EVP_sha1(), nullptr))
		return {};
	// A program or library is hashed as its signature would be, without the checksum, the security directory entry and the certificate
	// table; any other file whole.
	const uint32_t header = file.size() >= 0x40 && file[0] == 'M' && file[1] == 'Z' ? le32(file, 0x3c) : 0;
	if (header && uint64_t(header) + 24 + 96 <= file.size() && le32(file, header) == 0x4550) {
		const size_t optional = header + 24, checksum = optional + 64;
		const size_t security = optional + (le16(file, optional) == 0x20b ? 112 : 96) + 4 * 8;
		const uint32_t table = le32(file, security), size = le32(file, security + 4);
		const size_t end = table && size && uint64_t(table) + size <= file.size() ? table : file.size();
		update(context.get(), file, 0, checksum);
		update(context.get(), file, checksum + 4, security);
		update(context.get(), file, security + 8, end);
	} else
		update(context.get(), file, 0, file.size());
	std::vector<uint8_t> digest(EVP_MAX_MD_SIZE);
	unsigned int length = 0;
	if (!EVP_DigestFinal_ex(context.get(), digest.data(), &length))
		return {};
	digest.resize(length);
	return digest;
}

AuthenticodeResult verify_authenticode(std::span<const uint8_t> file)
{
	AuthenticodeResult result;
	std::optional<Signed> found = signed_content(file);
	if (!found) {
		result.status = trust_unknown_subject;
		return result;
	}
	if (found->signature.empty()) {
		result.status = trust_no_signature;
		return result;
	}
	const unsigned char *at = found->signature.data();
	std::unique_ptr<PKCS7, decltype(&PKCS7_free)> signature(d2i_PKCS7(nullptr, &at, long(found->signature.size())), PKCS7_free);
	if (!signature || !PKCS7_type_is_signed(signature.get()) || !signature->d.sign->contents ||
		signature->d.sign->contents->d.other == nullptr || signature->d.sign->contents->d.other->type != V_ASN1_SEQUENCE) {
		result.status = trust_no_signature;
		return result;
	}
	// SpcIndirectDataContent: what was signed, then the DigestInfo of the file's content.
	const auto *content = signature->d.sign->contents->d.other->value.sequence;
	const std::span<const uint8_t> indirect(ASN1_STRING_get0_data(content), size_t(ASN1_STRING_length(content)));
	const auto outer = der_header(indirect, 0);
	const auto first = outer ? der_header(indirect, outer->first) : std::nullopt;
	if (!outer || !first) {
		result.status = trust_no_signature;
		return result;
	}
	const size_t digest_at = outer->first + first->first + first->second;
	const unsigned char *digest_bytes = indirect.data() + digest_at;
	std::unique_ptr<X509_SIG, decltype(&X509_SIG_free)> digest_info(
		digest_at < indirect.size() ? d2i_X509_SIG(nullptr, &digest_bytes, long(indirect.size() - digest_at)) : nullptr, X509_SIG_free);
	if (!digest_info) {
		result.status = trust_no_signature;
		return result;
	}
	const X509_ALGOR *algorithm = nullptr;
	const ASN1_OCTET_STRING *expected = nullptr;
	X509_SIG_get0(digest_info.get(), &algorithm, &expected);
	const EVP_MD *method = EVP_get_digestbyobj(algorithm->algorithm);
	if (!method) {
		result.status = trust_bad_digest;
		return result;
	}
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hashing(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	unsigned char computed[EVP_MAX_MD_SIZE];
	unsigned int computed_size = 0;
	EVP_DigestInit_ex(hashing.get(), method, nullptr);
	found->hash(hashing.get());
	EVP_DigestFinal_ex(hashing.get(), computed, &computed_size);
	if (int(computed_size) != ASN1_STRING_length(expected) || std::memcmp(computed, ASN1_STRING_get0_data(expected), computed_size) != 0) {
		result.status = trust_bad_digest;
		return result;
	}
	// The signer's signature covers the content of SpcIndirectDataContent, without its own header.
	std::unique_ptr<BIO, decltype(&BIO_free)> signed_data(BIO_new_mem_buf(indirect.data() + outer->first, int(indirect.size() - outer->first)), BIO_free);
	if (PKCS7_verify(signature.get(), nullptr, nullptr, signed_data.get(), nullptr, PKCS7_NOVERIFY | PKCS7_BINARY) != 1) {
		result.status = trust_bad_signature;
		return result;
	}
	STACK_OF(X509) *signers = PKCS7_get0_signers(signature.get(), nullptr, 0);
	X509 *signer = signers && sk_X509_num(signers) ? sk_X509_value(signers, 0) : nullptr;
	PKCS7_SIGNER_INFO *signer_info = sk_PKCS7_SIGNER_INFO_value(PKCS7_get_signer_info(signature.get()), 0);
	if (!signer || !signer_info) {
		sk_X509_free(signers);
		result.status = trust_bad_signature;
		return result;
	}
	// The chain is judged at the time the signature was stamped, as Windows does, or now when it carries no timestamp.
	std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> verify(X509_STORE_CTX_new(), X509_STORE_CTX_free);
	X509_STORE_CTX_init(verify.get(), trust_anchors(), signer, signature->d.sign->cert);
	X509_STORE_CTX_set_purpose(verify.get(), X509_PURPOSE_ANY);
	// Old Microsoft authorities name no basic constraints; Windows does not ask a certificate without them to declare itself a CA.
	X509_STORE_CTX_set_verify_cb(verify.get(), [](int accepted, X509_STORE_CTX *store) {
		if (!accepted && X509_STORE_CTX_get_error(store) == X509_V_ERR_INVALID_CA)
			if (X509 *certificate = X509_STORE_CTX_get_current_cert(store); certificate && !(X509_get_extension_flags(certificate) & EXFLAG_BCONS))
				return 1;
		return accepted;
	});
	const auto when = stamped_time(signer_info);
	X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(verify.get()),
		when.value_or(time_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count())));
	const bool trusted = X509_verify_cert(verify.get()) == 1;
	// RAVEN_TRACE_AUTHENTICODE names what the chain check found and the time it judged the signature at.
	if (std::getenv("RAVEN_TRACE_AUTHENTICODE"))
		std::fprintf(stderr, "authenticode: %s, stamped %lld, chain: %s\n", trusted ? "trusted" : "not trusted", when ? (long long)*when : -1LL,
			trusted ? "ok" : X509_verify_cert_error_string(X509_STORE_CTX_get_error(verify.get())));
	if (STACK_OF(X509) *chain = X509_STORE_CTX_get0_chain(verify.get()))
		for (int index = 0; index < sk_X509_num(chain); ++index)
			result.chain.push_back(der_of(sk_X509_value(chain, index)));
	if (result.chain.empty())
		result.chain.push_back(der_of(signer));
	sk_X509_free(signers);
	result.anchored = trusted;
	if (!trusted) {
		switch (X509_STORE_CTX_get_error(verify.get())) {
		case X509_V_ERR_CERT_HAS_EXPIRED: case X509_V_ERR_CERT_NOT_YET_VALID: result.status = certificate_expired; break;
		case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT: case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY: case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
			result.status = certificate_chaining;
			break;
		default: result.status = certificate_untrusted_root; break;
		}
	}
	return result;
}

#else

AuthenticodeResult verify_authenticode(std::span<const uint8_t>)
{
	// Without the cryptography library no signature can be checked: the file is reported as unsigned.
	return {0x800b0100, {}, false};
}

std::vector<uint8_t> embedded_signature(std::span<const uint8_t>)
{
	return {};
}

std::vector<uint8_t> subject_digest(std::span<const uint8_t>, bool)
{
	return {};
}

#endif

}
