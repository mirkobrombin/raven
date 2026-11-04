#include "kernel_context.h"
#include "authenticode.h"
#include "text.h"

#ifdef RAVEN_OPENSSL
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#endif

#include "file_objects.h"
#include "prefix.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <unistd.h>

namespace raven {

#ifdef RAVEN_OPENSSL

namespace {

// The bytes of an ASN.1 string. OpenSSL 4 makes the structure opaque, so they are read through the accessors.
std::vector<uint8_t> asn1_bytes(const ASN1_STRING *value)
{
	const unsigned char *data = value ? ASN1_STRING_get0_data(value) : nullptr;
	return data ? std::vector<uint8_t>(data, data + ASN1_STRING_length(value)) : std::vector<uint8_t>();
}

constexpr uint32_t error_asn1_bad_tag = 0x8009310b, trust_not_time_valid = 0x1, trust_not_signature_valid = 0x8, trust_revoked = 0x4,
	trust_untrusted_root = 0x20, trust_partial_chain = 0x10000, trust_self_signed = 0x8;
constexpr uint32_t store_provider_memory = 2, store_provider_system_a = 9, store_provider_system_w = 10;

constexpr uint32_t error_invalid_data = 13, error_invalid_parameter = 87;
// A protected blob: the magic, the length of the description in bytes, the description as UTF-16, the IV, the tag and the ciphertext.
constexpr char protected_magic[4] = {'R', 'V', 'D', 'P'};

// The secret behind CryptProtectData lives in the prefix, so blobs open again in later runs of the same prefix and nowhere else.
std::array<uint8_t, 32> protection_secret()
{
	static std::mutex lock;
	std::lock_guard guard(lock);
	const auto path = NativePrefix::from_environment().root() / "dpapi.key";
	std::array<uint8_t, 32> secret{};
	{
		std::ifstream input(path, std::ios::binary);
		if (input.read(reinterpret_cast<char *>(secret.data()), secret.size()) && size_t(input.gcount()) == secret.size())
			return secret;
	}
	RAND_bytes(secret.data(), int(secret.size()));
	const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (descriptor >= 0) {
		const auto written = ::write(descriptor, secret.data(), secret.size());
		::close(descriptor);
		(void)written;
	} else if (std::ifstream input(path, std::ios::binary); input.read(reinterpret_cast<char *>(secret.data()), secret.size())) {
		// Another process of the prefix made the key first.
	}
	return secret;
}

std::array<uint8_t, 32> protection_key(std::span<const uint8_t> entropy)
{
	const auto secret = protection_secret();
	std::array<uint8_t, 32> key{};
	unsigned int length = 0;
	HMAC(EVP_sha256(), secret.data(), int(secret.size()), entropy.data(), entropy.size(), key.data(), &length);
	return key;
}

using X509Pointer = std::unique_ptr<X509, decltype(&X509_free)>;

// A certificate store: the encoded certificates it holds, in the order they were added.
class CertificateStore : public KernelObject {
public:
	std::vector<std::vector<uint8_t>> certificates;
};

// A cryptographic message, a PKCS #7 signature here, which CryptMsgGetParam reads.
class CryptMessage : public KernelObject {
public:
	std::vector<uint8_t> encoded;
};

// The short names Windows gives the attributes of a distinguished name.
const char *attribute_label(int nid)
{
	switch (nid) {
	case NID_commonName: return "CN";
	case NID_organizationName: return "O";
	case NID_organizationalUnitName: return "OU";
	case NID_localityName: return "L";
	case NID_stateOrProvinceName: return "S";
	case NID_countryName: return "C";
	case NID_pkcs9_emailAddress: return "E";
	case NID_domainComponent: return "DC";
	case NID_streetAddress: return "STREET";
	case NID_title: return "T";
	case NID_givenName: return "G";
	case NID_surname: return "SN";
	case NID_serialNumber: return "SERIALNUMBER";
	default: return nullptr;
	}
}

// A distinguished name as CertNameToStr writes it: values alone, or prefixed by their object identifier or their short name, in the order
// of the encoding or reversed, separated by commas, semicolons or line breaks.
std::string name_text(const X509_NAME *name, uint32_t type)
{
	constexpr uint32_t simple = 1, oid = 2, semicolon = 0x40000000, crlf = 0x08000000, no_plus = 0x20000000, no_quote = 0x10000000,
		reverse = 0x02000000;
	(void)no_plus;
	std::vector<std::string> parts;
	for (int index = 0; index < X509_NAME_entry_count(name); ++index) {
		const X509_NAME_ENTRY *entry = X509_NAME_get_entry(name, index);
		unsigned char *utf8 = nullptr;
		const int length = ASN1_STRING_to_UTF8(&utf8, X509_NAME_ENTRY_get_data(entry));
		std::string value(utf8 ? reinterpret_cast<char *>(utf8) : "", length > 0 ? size_t(length) : 0);
		OPENSSL_free(utf8);
		if (!(type & no_quote) && value.find_first_of(",+=\"\n<>#;") != value.npos)
			value = "\"" + value + "\"";
		const auto *object = X509_NAME_ENTRY_get_object(entry);
		char identifier[80] = {};
		OBJ_obj2txt(identifier, sizeof(identifier), object, 1);
		const char *label = attribute_label(OBJ_obj2nid(object));
		if ((type & 0xff) == simple)
			parts.push_back(value);
		else if ((type & 0xff) == oid || !label)
			parts.push_back(std::string(identifier) + "=" + value);
		else
			parts.push_back(std::string(label) + "=" + value);
	}
	if (type & reverse)
		std::reverse(parts.begin(), parts.end());
	const std::string separator = type & semicolon ? "; " : type & crlf ? "\r\n" : ", ";
	std::string text;
	for (const auto &part : parts)
		text += (text.empty() ? "" : separator) + part;
	return text;
}

// The first value of an attribute of a name, as UTF-8.
std::string name_attribute(const X509_NAME *name, int nid)
{
	const int index = X509_NAME_get_index_by_NID(name, nid, -1);
	if (index < 0)
		return {};
	unsigned char *utf8 = nullptr;
	const int length = ASN1_STRING_to_UTF8(&utf8, X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, index)));
	std::string value(utf8 ? reinterpret_cast<char *>(utf8) : "", length > 0 ? size_t(length) : 0);
	OPENSSL_free(utf8);
	return value;
}

template<typename Encode, typename Value>
std::vector<uint8_t> der_encoding(Encode encode, Value *value)
{
	const int length = encode(value, nullptr);
	std::vector<uint8_t> bytes(length > 0 ? size_t(length) : 0);
	unsigned char *at = bytes.data();
	if (length > 0)
		encode(value, &at);
	return bytes;
}

// What WinVerifyTrust found for a file, kept for the provider data built from it later, and the guest blocks those structures use.
// A catalog administrator context: the hash algorithm its file hashes use.
class CatalogAdmin : public KernelObject {
public:
	bool sha256 = false;
};

class TrustState : public KernelObject {
public:
	AuthenticodeResult result;
	uint64_t provider = 0;
	std::vector<uint64_t> blocks;
};

// Lays a structure out in the guest's memory with the natural alignment of its fields: pointers are as wide as the program's.
class Layout {
public:
	explicit Layout(size_t width) : width(width) {}
	size_t reserve(size_t size, size_t alignment)
	{
		offset = (offset + alignment - 1) / alignment * alignment;
		const auto at = offset;
		offset += size;
		return at;
	}
	size_t dword() { return reserve(4, 4); }
	size_t pointer() { return reserve(width, width); }
	size_t size() const { return (offset + width - 1) / width * width; }
	const size_t width;

private:
	size_t offset = 0;
};

std::vector<uint8_t> der_of(X509 *certificate)
{
	const int length = i2d_X509(certificate, nullptr);
	std::vector<uint8_t> bytes(length > 0 ? size_t(length) : 0);
	unsigned char *at = bytes.data();
	if (length > 0)
		i2d_X509(certificate, &at);
	return bytes;
}

X509Pointer parse(std::span<const uint8_t> bytes)
{
	const unsigned char *at = bytes.data();
	return X509Pointer(d2i_X509(nullptr, &at, long(bytes.size())), X509_free);
}

uint64_t filetime_of(const ASN1_TIME *time)
{
	if (!time)
		return 0;
	int days = 0, seconds = 0;
	ASN1_TIME *unix_epoch = ASN1_TIME_set(nullptr, 0);
	if (!unix_epoch || !ASN1_TIME_diff(&days, &seconds, unix_epoch, time)) {
		ASN1_STRING_free(unix_epoch);
		return 0;
	}
	ASN1_STRING_free(unix_epoch);
	const int64_t unix_seconds = int64_t(days) * 86400 + seconds;
	return uint64_t(unix_seconds + 11644473600LL) * 10000000ULL;
}

uint32_t trust_of(int error)
{
	switch (error) {
	case X509_V_OK: return 0;
	case X509_V_ERR_CERT_HAS_EXPIRED: case X509_V_ERR_CERT_NOT_YET_VALID: return trust_not_time_valid;
	case X509_V_ERR_CERT_SIGNATURE_FAILURE: return trust_not_signature_valid;
	case X509_V_ERR_CERT_REVOKED: return trust_revoked;
	case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT: case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY: case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
		return trust_partial_chain;
	default: return trust_untrusted_root;
	}
}

}

void register_crypt32_api(ApiRegistry &registry, KernelContext &context)
{
	auto &objects = context.objects;
	auto &allocator = context.allocator;
	// A certificate context in the guest points at its encoded bytes and at a CERT_INFO with the fields programs read most.
	const auto make_context = [&allocator](ApiCall &call, const std::vector<uint8_t> &encoded, uint64_t store) -> uint64_t {
		const auto certificate = parse(encoded);
		if (!certificate)
			return 0;
		const size_t width = call.pointer_width();
		Layout info(width);
		const auto version_at = info.dword();
		const auto serial_size = info.dword(), serial_data = info.pointer();
		const auto algorithm_name = info.pointer(), parameters_size = info.dword(), parameters_data = info.pointer();
		const auto issuer_size = info.dword(), issuer_data = info.pointer();
		const auto not_before = info.reserve(8, 4), not_after = info.reserve(8, 4);
		const auto subject_size = info.dword(), subject_data = info.pointer();
		const auto key_algorithm_name = info.pointer(), key_parameters_size = info.dword(), key_parameters_data = info.pointer();
		const auto key_size = info.dword(), key_data = info.pointer(), key_unused = info.dword();
		const auto issuer_unique = info.reserve(width == 4 ? 12 : 24, width), subject_unique = info.reserve(width == 4 ? 12 : 24, width);
		const auto extension_count = info.dword(), extension_data = info.pointer();
		Layout head(width);
		const auto encoding_at = head.dword(), encoded_at = head.pointer(), encoded_size_at = head.dword(), info_at = head.pointer(), store_at = head.pointer();
		// The serial number as CRYPT_INTEGER_BLOB holds it, little-endian.
		const auto *name = X509_get_serialNumber(certificate.get());
		std::vector<uint8_t> serial = asn1_bytes(name);
		std::reverse(serial.begin(), serial.end());
		const auto name_bytes = [](const X509_NAME *value) {
			const int length = i2d_X509_NAME(value, nullptr);
			std::vector<uint8_t> bytes(length > 0 ? size_t(length) : 0);
			unsigned char *at = bytes.data();
			if (length > 0)
				i2d_X509_NAME(value, &at);
			return bytes;
		};
		const auto issuer = name_bytes(X509_get_issuer_name(certificate.get())), subject = name_bytes(X509_get_subject_name(certificate.get()));
		const auto object_text = [](const ASN1_OBJECT *object) {
			char text[80] = {};
			OBJ_obj2txt(text, sizeof(text), object, 1);
			return std::vector<uint8_t>(text, text + std::strlen(text) + 1);
		};
		const auto parameters = [](const ASN1_TYPE *value) {
			return value ? der_encoding(i2d_ASN1_TYPE, const_cast<ASN1_TYPE *>(value)) : std::vector<uint8_t>();
		};
		// The signature algorithm, the public key with its algorithm, and the extensions.
		const X509_ALGOR *signature_algorithm = X509_get0_tbs_sigalg(certificate.get());
		const auto signature_oid = object_text(signature_algorithm->algorithm), signature_parameters = parameters(signature_algorithm->parameter);
		ASN1_OBJECT *key_object = nullptr;
		const unsigned char *key_bytes = nullptr;
		int key_length = 0;
		X509_ALGOR *key_algorithm = nullptr;
		X509_PUBKEY_get0_param(&key_object, &key_bytes, &key_length, &key_algorithm, X509_get_X509_PUBKEY(certificate.get()));
		const auto key_oid = object_text(key_object);
		const auto key_parameters = parameters(key_algorithm ? key_algorithm->parameter : nullptr);
		const std::vector<uint8_t> key(key_bytes, key_bytes + std::max(key_length, 0));
		const auto *extensions = X509_get0_extensions(certificate.get());
		const int extension_total = extensions ? sk_X509_EXTENSION_num(extensions) : 0;
		Layout extension(width);
		const auto extension_oid = extension.pointer(), extension_critical = extension.dword(), extension_size = extension.dword(),
			extension_value = extension.pointer();
		size_t extension_bytes = size_t(extension_total) * extension.size();
		for (int index = 0; index < extension_total; ++index) {
			X509_EXTENSION *entry = sk_X509_EXTENSION_value(extensions, index);
			extension_bytes += object_text(X509_EXTENSION_get_object(entry)).size() + size_t(ASN1_STRING_length(X509_EXTENSION_get_data(entry)));
		}
		// One block: the context, the CERT_INFO, the extension array, then the byte strings they point at.
		const size_t info_start = (head.size() + 15) / 16 * 16, bytes_start = (info_start + info.size() + 15) / 16 * 16;
		const size_t total = bytes_start + encoded.size() + serial.size() + issuer.size() + subject.size() + signature_oid.size() +
			signature_parameters.size() + key_oid.size() + key_parameters.size() + key.size() + extension_bytes + 64;
		const auto block = allocator.allocate(total, true);
		if (!block)
			return 0;
		std::vector<uint8_t> image(total);
		const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(image.data() + at, &value, 4); };
		const auto put_pointer = [&](size_t at, uint64_t value) { std::memcpy(image.data() + at, &value, width); };
		size_t cursor = bytes_start;
		const auto place = [&](const std::vector<uint8_t> &bytes) {
			const auto address = block + cursor;
			std::memcpy(image.data() + cursor, bytes.data(), bytes.size());
			cursor += bytes.size();
			return address;
		};
		put32(encoding_at, 0x10001);
		put_pointer(encoded_at, place(encoded));
		put32(encoded_size_at, uint32_t(encoded.size()));
		put_pointer(info_at, block + info_start);
		put_pointer(store_at, store);
		put32(info_start + version_at, uint32_t(X509_get_version(certificate.get())));
		put32(info_start + serial_size, uint32_t(serial.size()));
		put_pointer(info_start + serial_data, place(serial));
		put32(info_start + issuer_size, uint32_t(issuer.size()));
		put_pointer(info_start + issuer_data, place(issuer));
		put32(info_start + subject_size, uint32_t(subject.size()));
		put_pointer(info_start + subject_data, place(subject));
		const auto before = filetime_of(X509_get0_notBefore(certificate.get())), after = filetime_of(X509_get0_notAfter(certificate.get()));
		std::memcpy(image.data() + info_start + not_before, &before, 8);
		std::memcpy(image.data() + info_start + not_after, &after, 8);
		put_pointer(info_start + algorithm_name, place(signature_oid));
		put32(info_start + parameters_size, uint32_t(signature_parameters.size()));
		put_pointer(info_start + parameters_data, signature_parameters.empty() ? 0 : place(signature_parameters));
		put_pointer(info_start + key_algorithm_name, place(key_oid));
		put32(info_start + key_parameters_size, uint32_t(key_parameters.size()));
		put_pointer(info_start + key_parameters_data, key_parameters.empty() ? 0 : place(key_parameters));
		put32(info_start + key_size, uint32_t(key.size()));
		put_pointer(info_start + key_data, key.empty() ? 0 : place(key));
		put32(info_start + key_unused, 0);
		// CERT_EXTENSION: the identifier, whether it is critical, and the encoded value.
		cursor = (cursor + 15) / 16 * 16;
		const size_t array = cursor;
		cursor += size_t(extension_total) * extension.size();
		put32(info_start + extension_count, uint32_t(extension_total));
		put_pointer(info_start + extension_data, extension_total ? block + array : 0);
		for (int index = 0; index < extension_total; ++index) {
			X509_EXTENSION *entry = sk_X509_EXTENSION_value(extensions, index);
			const size_t at = array + size_t(index) * extension.size();
			const auto *value = X509_EXTENSION_get_data(entry);
			put_pointer(at + extension_oid, place(object_text(X509_EXTENSION_get_object(entry))));
			put32(at + extension_critical, uint32_t(X509_EXTENSION_get_critical(entry) > 0));
			put32(at + extension_size, uint32_t(ASN1_STRING_length(value)));
			put_pointer(at + extension_value, place(asn1_bytes(value)));
		}
		(void)issuer_unique; (void)subject_unique;
		call.write(block, image);
		return block;
	};
		// Where the fields the trust helpers read lie in CRYPT_PROVIDER_DATA, CRYPT_PROVIDER_SGNR and CRYPT_PROVIDER_CERT, and their sizes.
	struct ProviderLayout {
		size_t data_size, data_error, data_signer_count, data_signers, data_final_error;
		size_t signer_size, signer_cert_count, signer_certs, signer_error, signer_chain;
		size_t cert_size, cert_context, cert_trusted_root, cert_self_signed, cert_error, cert_element;
	};
	const auto provider_layout = [](size_t width) {
		ProviderLayout offsets{};
		Layout data(width);
		data.dword();
		data.pointer();
		data.dword();
		data.pointer();
		data.pointer();
		data.pointer();
		offsets.data_error = data.dword();
		data.dword();
		data.dword();
		data.pointer();
		data.dword();
		data.pointer();
		data.dword();
		data.pointer();
		data.dword();
		data.pointer();
		offsets.data_signer_count = data.dword();
		offsets.data_signers = data.pointer();
		data.dword();
		data.pointer();
		data.dword();
		data.pointer();
		data.pointer();
		data.dword();
		data.reserve(8, 4);
		data.pointer();
		data.dword();
		offsets.data_final_error = data.dword();
		data.pointer();
		data.dword();
		data.dword();
		data.pointer();
		data.pointer();
		offsets.data_size = data.size();
		Layout signer(width);
		signer.dword();
		signer.reserve(8, 4);
		offsets.signer_cert_count = signer.dword();
		offsets.signer_certs = signer.pointer();
		signer.dword();
		signer.pointer();
		offsets.signer_error = signer.dword();
		signer.dword();
		signer.pointer();
		offsets.signer_chain = signer.pointer();
		offsets.signer_size = signer.size();
		Layout cert(width);
		cert.dword();
		offsets.cert_context = cert.pointer();
		cert.dword();
		offsets.cert_trusted_root = cert.dword();
		offsets.cert_self_signed = cert.dword();
		cert.dword();
		cert.dword();
		cert.dword();
		offsets.cert_error = cert.dword();
		cert.pointer();
		cert.dword();
		cert.pointer();
		cert.dword();
		cert.dword();
		offsets.cert_element = cert.pointer();
		offsets.cert_size = cert.size();
		return offsets;
	};
// Encoded bytes of a context the guest holds.
	const auto encoded_of = [](ApiCall &call, uint64_t certificate) {
		const size_t width = call.pointer_width();
		const auto bytes = call.load_pointer(certificate + (width == 4 ? 4 : 8));
		const auto size = call.load<uint32_t>(certificate + (width == 4 ? 8 : 16));
		return call.read(bytes, size);
	};

	registry.add("CertOpenStore", 5, [&objects](ApiCall &call) {
		const uint64_t provider = call[0];
		auto store = std::make_shared<CertificateStore>();
		if (provider == store_provider_system_w || provider == store_provider_system_a) {
			// The root store is the host's bundle; every other system store starts empty.
			const auto name = provider == store_provider_system_w ? call.wide(call[4], 64) : [&] {
				const auto narrow = call.narrow(call[4], 64);
				return std::u16string(narrow.begin(), narrow.end());
			}();
			if (name == u"ROOT" || name == u"root") {
				if (auto *roots = trust_anchors()) {
					const auto *objects_list = X509_STORE_get0_objects(roots);
					for (int index = 0; objects_list && index < sk_X509_OBJECT_num(objects_list); ++index)
						if (auto *certificate = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(objects_list, index)))
							store->certificates.push_back(der_of(certificate));
				}
			}
		} else if (provider != store_provider_memory) {
			call.fail(0x80092009U, 0);
			return;
		}
		const auto made = objects.add_object(store);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("CertCloseStore", 2, [&objects](ApiCall &call) {
		if (!objects.object_as<CertificateStore>(call[0])) {
			call.fail(6);
			return;
		}
		objects.close(call[0]);
		call.ret(1);
	});
	registry.add("CertCreateCertificateContext", 3, [make_context](ApiCall &call) {
		if (!call[1] || !call.u32(2)) {
			call.fail(87, 0);
			return;
		}
		const auto encoded = call.read(call[1], call.u32(2));
		const auto made = make_context(call, encoded, 0);
		if (!made) {
			call.fail(error_asn1_bad_tag, 0);
			return;
		}
		call.ret(made);
	});
	registry.add("CertAddEncodedCertificateToStore", 6, [&objects, make_context](ApiCall &call) {
		const auto store = objects.object_as<CertificateStore>(call[0]);
		if (!store || !call[2]) {
			call.fail(store ? 87 : 6);
			return;
		}
		const auto encoded = call.read(call[2], call.u32(3));
		if (!parse(encoded)) {
			call.fail(error_asn1_bad_tag);
			return;
		}
		store->certificates.push_back(encoded);
		if (call[5]) {
			const auto made = make_context(call, encoded, call[0]);
			call.store_pointer(call[5], made);
		}
		call.ret(1);
	});
	registry.add("CertAddCertificateContextToStore", 4, [&objects, encoded_of, make_context](ApiCall &call) {
		const auto store = objects.object_as<CertificateStore>(call[0]);
		if (!store || !call[1]) {
			call.fail(store ? 87 : 6);
			return;
		}
		const auto encoded = encoded_of(call, call[1]);
		store->certificates.push_back(encoded);
		if (call[3]) {
			const auto made = make_context(call, encoded, call[0]);
			call.store_pointer(call[3], made);
		}
		call.ret(1);
	});
	registry.add("CertDuplicateCertificateContext", 1, [encoded_of, make_context](ApiCall &call) {
		if (!call[0]) {
			call.ret(0);
			return;
		}
		call.ret(make_context(call, encoded_of(call, call[0]), 0));
	});
	registry.add("CertFreeCertificateContext", 1, [&allocator](ApiCall &call) {
		if (call[0])
			allocator.release(call[0]);
		call.ret(1);
	});
	registry.add("CertEnumCertificatesInStore", 2, [&objects, &allocator, make_context](ApiCall &call) {
		const auto store = objects.object_as<CertificateStore>(call[0]);
		if (!store) {
			call.fail(6, 0);
			return;
		}
		// The previous context says where the walk stands: it names the store and the certificate it was made from.
		size_t next = 0;
		if (call[1]) {
			const size_t width = call.pointer_width();
			const auto size = call.load<uint32_t>(call[1] + (width == 4 ? 8 : 16));
			const auto bytes = call.read(call.load_pointer(call[1] + (width == 4 ? 4 : 8)), size);
			for (size_t index = 0; index < store->certificates.size(); ++index)
				if (store->certificates[index] == bytes) {
					next = index + 1;
					break;
				}
			allocator.release(call[1]);
		}
		if (next >= store->certificates.size()) {
			call.fail(0x80092004U, 0);
			return;
		}
		call.ret(make_context(call, store->certificates[next], call[0]));
	});
	// The Windows structures of a chain: CERT_CHAIN_CONTEXT, one CERT_SIMPLE_CHAIN and a CERT_CHAIN_ELEMENT for each certificate, signer
	// first; null when the guest has no room for them.
	const auto write_chain = [&allocator, make_context](ApiCall &call, const std::vector<std::vector<uint8_t>> &chain, uint32_t status,
		bool anchored) -> uint64_t {
		// Layouts of the three structures.
		const size_t width = call.pointer_width();
		Layout context_layout(width), simple_layout(width), element_layout(width);
		const auto context_size = context_layout.dword(), context_error = context_layout.dword(), context_info = context_layout.dword(),
			context_chains = context_layout.dword(), context_array = context_layout.pointer(), context_lower = context_layout.dword(),
			context_lower_array = context_layout.pointer(), context_fresh = context_layout.dword(), context_fresh_time = context_layout.dword();
		const auto simple_size = simple_layout.dword(), simple_error = simple_layout.dword(), simple_info = simple_layout.dword(),
			simple_count = simple_layout.dword(), simple_array = simple_layout.pointer(), simple_list = simple_layout.pointer(),
			simple_fresh = simple_layout.dword(), simple_fresh_time = simple_layout.dword();
		const auto element_size = element_layout.dword(), element_context = element_layout.pointer(), element_error = element_layout.dword(),
			element_info = element_layout.dword(), element_revocation = element_layout.pointer(), element_issuance = element_layout.pointer(),
			element_application = element_layout.pointer(), element_message = element_layout.pointer();
		const size_t chain_pointers = chain.size() * width, elements_size = chain.size() * element_layout.size();
		size_t total = context_layout.size() + width + simple_layout.size() + chain_pointers + elements_size + 64;
		const auto block = allocator.allocate(total, true);
		if (!block)
			return uint64_t(0);
		std::vector<uint8_t> image(total);
		const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(image.data() + at, &value, 4); };
		const auto put_pointer = [&](size_t at, uint64_t value) { std::memcpy(image.data() + at, &value, width); };
		const size_t context_at = 0, array_at = (context_layout.size() + 15) / 16 * 16, simple_at = array_at + 16,
			pointers_at = (simple_at + simple_layout.size() + 15) / 16 * 16, elements_at = pointers_at + (chain_pointers + 15) / 16 * 16;
		put32(context_at + context_size, uint32_t(context_layout.size()));
		put32(context_at + context_error, status);
		put32(context_at + context_info, 0);
		put32(context_at + context_chains, 1);
		put_pointer(context_at + context_array, block + array_at);
		put32(context_at + context_lower, 0);
		put_pointer(context_at + context_lower_array, 0);
		put32(context_at + context_fresh, 0);
		put32(context_at + context_fresh_time, 0);
		put_pointer(array_at, block + simple_at);
		put32(simple_at + simple_size, uint32_t(simple_layout.size()));
		put32(simple_at + simple_error, status);
		put32(simple_at + simple_info, 0);
		put32(simple_at + simple_count, uint32_t(chain.size()));
		put_pointer(simple_at + simple_array, block + pointers_at);
		put_pointer(simple_at + simple_list, 0);
		put32(simple_at + simple_fresh, 0);
		put32(simple_at + simple_fresh_time, 0);
		// The element contexts are separate allocations, which the guest releases through CertFreeCertificateContext.
		for (size_t index = 0; index < chain.size(); ++index) {
			const auto element = elements_at + index * element_layout.size();
			put_pointer(pointers_at + index * width, block + element);
			put32(element + element_size, uint32_t(element_layout.size()));
			put_pointer(element + element_context, make_context(call, chain[index], 0));
			const bool last = index + 1 == chain.size();
			put32(element + element_error, last ? status : 0);
			put32(element + element_info, last && anchored ? trust_self_signed : 0);
			(void)element_revocation; (void)element_issuance; (void)element_application; (void)element_message;
		}
		call.write(block, image);
		return block;
	};
	registry.add("CertFreeCertificateChain", 1, [&allocator](ApiCall &call) {
		if (call[0])
			allocator.release(call[0]);
	});
	// The chain is built with the host's X.509 verifier against the host's trust anchors; the guest receives the Windows structures:
	// CERT_CHAIN_CONTEXT, one CERT_SIMPLE_CHAIN and a CERT_CHAIN_ELEMENT for each certificate.
	registry.add("CertGetCertificateChain", 8, [&objects, encoded_of, write_chain](ApiCall &call) {
		if (!call[1] || !call[7]) {
			call.fail(87);
			return;
		}
		const auto leaf = parse(encoded_of(call, call[1]));
		if (!leaf) {
			call.fail(error_asn1_bad_tag);
			return;
		}
		X509_STORE_CTX *verify = X509_STORE_CTX_new();
		STACK_OF(X509) *untrusted = sk_X509_new_null();
		if (!verify || !untrusted || !trust_anchors()) {
			if (verify)
				X509_STORE_CTX_free(verify);
			if (untrusted)
				sk_X509_free(untrusted);
			call.fail(8);
			return;
		}
		std::vector<X509Pointer> held;
		if (const auto extra = objects.object_as<CertificateStore>(call[3]))
			for (const auto &encoded : extra->certificates)
				if (auto parsed = parse(encoded)) {
					sk_X509_push(untrusted, parsed.get());
					held.push_back(std::move(parsed));
				}
		X509_STORE_CTX_init(verify, trust_anchors(), leaf.get(), untrusted);
		X509_STORE_CTX_set_flags(verify, X509_V_FLAG_TRUSTED_FIRST);
		const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		uint64_t at = now;
		if (call[2]) {
			const auto ticks = call.load<uint64_t>(call[2]);
			if (ticks > 11644473600ULL * 10000000ULL)
				at = ticks / 10000000ULL - 11644473600ULL;
		}
		X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(verify), time_t(at));
		// Every problem is noted and the check goes on, as the Windows chain engine reports them all in the trust status; old authorities
		// that name no basic constraints are not asked to declare themselves CAs.
		uint32_t found = 0;
		X509_STORE_CTX_set_app_data(verify, &found);
		X509_STORE_CTX_set_verify_cb(verify, [](int accepted, X509_STORE_CTX *store) {
			if (accepted)
				return 1;
			const int problem = X509_STORE_CTX_get_error(store);
			X509 *current = X509_STORE_CTX_get_current_cert(store);
			if (problem == X509_V_ERR_INVALID_CA && current && !(X509_get_extension_flags(current) & EXFLAG_BCONS))
				return 1;
			*static_cast<uint32_t *>(X509_STORE_CTX_get_app_data(store)) |= trust_of(problem);
			return 1;
		});
		X509_verify_cert(verify);
		STACK_OF(X509) *built = X509_STORE_CTX_get0_chain(verify);
		std::vector<std::vector<uint8_t>> chain;
		for (int index = 0; built && index < sk_X509_num(built); ++index)
			chain.push_back(der_of(sk_X509_value(built, index)));
		if (chain.empty())
			chain.push_back(der_of(leaf.get()));
		const bool anchored = !(found & (trust_partial_chain | trust_untrusted_root));
		X509_STORE_CTX_free(verify);
		sk_X509_free(untrusted);
		const auto block = write_chain(call, chain, found, anchored);
		if (!block) {
			call.fail(8);
			return;
		}
		call.store_pointer(call[7], block);
		call.ret(1);
	});

	// CryptHashPublicKeyInfo: the hash of a CERT_PUBLIC_KEY_INFO in its encoded form, SubjectPublicKeyInfo, with SHA-1 unless the call
	// names MD5 or SHA-256.
	registry.add("CryptHashPublicKeyInfo", 7, [](ApiCall &call) {
		if (!call[4] || !call[6]) {
			call.fail(error_invalid_parameter);
			return;
		}
		const size_t width = call.pointer_width();
		Layout layout(width);
		const auto oid_at = layout.pointer(), parameters_size = layout.dword(), parameters_at = layout.pointer(), key_size = layout.dword(),
			key_at = layout.pointer(), unused_at = layout.dword();
		const auto der = [](uint8_t tag, std::vector<uint8_t> content) {
			std::vector<uint8_t> bytes{tag};
			if (content.size() < 0x80) {
				bytes.push_back(uint8_t(content.size()));
			} else {
				std::vector<uint8_t> length;
				for (size_t value = content.size(); value; value >>= 8)
					length.insert(length.begin(), uint8_t(value));
				bytes.push_back(uint8_t(0x80 | length.size()));
				bytes.insert(bytes.end(), length.begin(), length.end());
			}
			bytes.insert(bytes.end(), content.begin(), content.end());
			return bytes;
		};
		std::unique_ptr<ASN1_OBJECT, decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(call.narrow(call.load_pointer(call[4] + oid_at), 80).c_str(), 1),
			ASN1_OBJECT_free);
		if (!object) {
			call.fail(0x80092002U);
			return;
		}
		auto algorithm = der_encoding(i2d_ASN1_OBJECT, object.get());
		if (const uint32_t size = call.load<uint32_t>(call[4] + parameters_size))
			for (const auto byte : call.read(call.load_pointer(call[4] + parameters_at), size))
				algorithm.push_back(byte);
		std::vector<uint8_t> bits{uint8_t(call.load<uint32_t>(call[4] + unused_at))};
		if (const uint32_t size = call.load<uint32_t>(call[4] + key_size))
			for (const auto byte : call.read(call.load_pointer(call[4] + key_at), size))
				bits.push_back(byte);
		auto content = der(0x30, algorithm);
		const auto key = der(0x03, bits);
		content.insert(content.end(), key.begin(), key.end());
		const auto information = der(0x30, content);
		const uint32_t algid = call.u32(1);
		const EVP_MD *method = algid == 0x8003 ? EVP_md5() : algid == 0x800c ? EVP_sha256() : EVP_sha1();
		std::vector<uint8_t> hash(size_t(EVP_MD_get_size(method)));
		unsigned int length = 0;
		EVP_Digest(information.data(), information.size(), hash.data(), &length, method, nullptr);
		const uint32_t capacity = call.load<uint32_t>(call[6]);
		call.store<uint32_t>(call[6], uint32_t(hash.size()));
		if (!call[5]) {
			call.ret(1);
			return;
		}
		if (capacity < hash.size()) {
			call.fail(234);
			return;
		}
		call.write(call[5], hash);
		call.ret(1);
	});
	// CertVerifyCertificateChainPolicy: the trust status of the chain against the base, Authenticode and SSL policies, less what the
	// caller asks to ignore; the Microsoft root policy also wants the chain to end at one of Microsoft's roots.
	registry.add("CertVerifyCertificateChainPolicy", 4, [encoded_of](ApiCall &call) {
		constexpr uint64_t microsoft_root = 7;
		constexpr uint32_t ignore_time = 0x1 | 0x2 | 0x4, ignore_unknown_root = 0x10, ignore_revocation = 0xf00;
		if (!call[1] || !call[3] || call[0] > 0xffff) {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		const size_t width = call.pointer_width();
		const uint32_t status = call.load<uint32_t>(call[1] + 4);
		const uint32_t flags = call[2] ? call.load<uint32_t>(call[2] + 4) : 0;
		uint32_t error = 0;
		if ((status & trust_not_signature_valid))
			error = 0x80096004U;
		else if ((status & trust_revoked) && !(flags & ignore_revocation))
			error = 0x80092010U;
		else if ((status & trust_partial_chain))
			error = 0x800b010aU;
		else if ((status & trust_untrusted_root) && !(flags & ignore_unknown_root))
			error = 0x800b0109U;
		else if ((status & trust_not_time_valid) && !(flags & ignore_time))
			error = 0x800b0101U;
		if (!error && call[0] == microsoft_root) {
			// The SHA-1 of Microsoft Root Authority and of the Microsoft Root Certificate Authorities of 2001, 2010 and 2011.
			static const std::array<std::array<uint8_t, 20>, 4> roots{{
				{0xa4, 0x34, 0x89, 0x15, 0x9a, 0x52, 0x0f, 0x0d, 0x93, 0xd0, 0x32, 0xcc, 0xaf, 0x37, 0xe7, 0xfe, 0x20, 0xa8, 0xb4, 0x19},
				{0xcd, 0xd4, 0xee, 0xae, 0x60, 0x00, 0xac, 0x7f, 0x40, 0xc3, 0x80, 0x2c, 0x17, 0x1e, 0x30, 0x14, 0x80, 0x30, 0xc0, 0x72},
				{0x3b, 0x1e, 0xfd, 0x3a, 0x66, 0xea, 0x28, 0xb1, 0x66, 0x97, 0x39, 0x47, 0x03, 0xa7, 0x2c, 0xa3, 0x40, 0xa0, 0x5b, 0xd5},
				{0x8f, 0x43, 0x28, 0x8a, 0xd2, 0x72, 0xf3, 0x10, 0x3b, 0x6f, 0xb1, 0x42, 0x84, 0x85, 0xea, 0x30, 0x14, 0xc0, 0xbc, 0xfe}}};
			const auto simple = call.load_pointer(call.load_pointer(call[1] + 16));
			const uint32_t count = call.load<uint32_t>(simple + 12);
			const auto root = count ? call.load_pointer(call.load_pointer(simple + 16) + (count - 1) * width) : 0;
			std::array<uint8_t, 20> hash{};
			if (root) {
				const auto bytes = encoded_of(call, call.load_pointer(root + width));
				SHA1(bytes.data(), bytes.size(), hash.data());
			}
			if (std::find(roots.begin(), roots.end(), hash) == roots.end())
				error = 0x800b0109U;
		}
		// CERT_CHAIN_POLICY_STATUS: the error, and the chain and element it concerns, here none.
		call.store<uint32_t>(call[3] + 4, error);
		call.store<int32_t>(call[3] + 8, error ? 0 : -1);
		call.store<int32_t>(call[3] + 12, -1);
		call.ret(1);
	});
	// Properties a certificate context answers from its encoded certificate: its SHA-1 and MD5 hashes, and its key identifier, which is
	// the subject key identifier the certificate names or else the SHA-1 of its public key.
	// CERT_ENHKEY_USAGE with no identifiers and CRYPT_E_NOT_FOUND: the certificate holds no restriction, good for every use.
	registry.add("CertGetEnhancedKeyUsage", 4, [](ApiCall &call) {
		const uint32_t size = call.pointer_width() == 4 ? 8 : 16;
		if (!call[3]) {
			call.fail(87, 0);
			return;
		}
		const uint32_t room = call.load<uint32_t>(call[3]);
		call.store<uint32_t>(call[3], size);
		if (call[2]) {
			if (room < size) {
				call.fail(234, 0);
				return;
			}
			call.store<uint32_t>(call[2], 0);
			call.store_pointer(call[2] + call.pointer_width(), 0);
		}
		call.fail(0x80092004U, 1);
	});
	registry.add("CertGetCertificateContextProperty", 4, [encoded_of](ApiCall &call) {
		constexpr uint32_t sha1_hash = 3, md5_hash = 4, key_identifier = 20, more_data = 234, not_found = 0x80092004U;
		if (!call[0] || !call[3]) {
			call.fail(error_invalid_parameter);
			return;
		}
		const auto encoded = encoded_of(call, call[0]);
		std::vector<uint8_t> value;
		if (call.u32(1) == sha1_hash) {
			value.resize(SHA_DIGEST_LENGTH);
			SHA1(encoded.data(), encoded.size(), value.data());
		} else if (call.u32(1) == md5_hash) {
			value.resize(16);
			unsigned int size = 0;
			EVP_Digest(encoded.data(), encoded.size(), value.data(), &size, EVP_md5(), nullptr);
		} else if (call.u32(1) == key_identifier) {
			if (const auto certificate = parse(encoded)) {
				if (const ASN1_OCTET_STRING *subject = X509_get0_subject_key_id(certificate.get())) {
					value = asn1_bytes(subject);
				} else if (const ASN1_BIT_STRING *key = X509_get0_pubkey_bitstr(certificate.get())) {
					value.resize(SHA_DIGEST_LENGTH);
					SHA1(ASN1_STRING_get0_data(key), size_t(ASN1_STRING_length(key)), value.data());
				}
			}
		}
		if (value.empty()) {
			call.fail(not_found);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[3]);
		call.store<uint32_t>(call[3], uint32_t(value.size()));
		if (!call[2]) {
			call.ret(1);
			return;
		}
		if (capacity < value.size()) {
			call.fail(more_data);
			return;
		}
		call.write(call[2], value);
		call.ret(1);
	});
	// Security catalogs: a context hashes files as the catalogs index them. No catalog is installed, so a lookup by hash finds none and a
	// program falls back to the file's own signature.
	const auto acquire_catalogs = [&context](ApiCall &call, bool sha256) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		auto admin = std::make_shared<CatalogAdmin>();
		admin->sha256 = sha256;
		const auto made = context.objects.add_object(admin);
		if (made.status) {
			call.fail(8);
			return;
		}
		call.store_pointer(call[0], made.value);
		call.ret(1);
	};
	registry.add("CryptCATAdminAcquireContext", 3, [acquire_catalogs](ApiCall &call) { acquire_catalogs(call, false); });
	registry.add("CryptCATAdminAcquireContext2", 5, [acquire_catalogs](ApiCall &call) {
		const auto algorithm = call[2] ? call.wide(call[2], 32) : std::u16string();
		if (!algorithm.empty() && algorithm != u"SHA1" && algorithm != u"SHA256") {
			call.fail(0x80090008U);
			return;
		}
		acquire_catalogs(call, algorithm == u"SHA256");
	});
	registry.add("CryptCATAdminReleaseContext", 2, [&context](ApiCall &call) {
		if (!context.objects.object_as<CatalogAdmin>(call[0])) {
			call.fail(87);
			return;
		}
		context.objects.close(call[0]);
		call.ret(1);
	});
	// The hash of the file behind a handle, read from its start without moving the handle's position.
	const auto hash_file = [&context](ApiCall &call, uint64_t handle, uint64_t size_at, uint64_t hash_at, bool sha256) {
		const auto file = context.objects.object_as<FileObject>(handle);
		if (!file || file->directory || !size_at) {
			call.fail(87);
			return;
		}
		const uint32_t length = sha256 ? 32 : 20, capacity = call.load<uint32_t>(size_at);
		call.store<uint32_t>(size_at, length);
		if (!hash_at || capacity < length) {
			call.fail(122);
			return;
		}
		std::vector<uint8_t> bytes;
		std::array<uint8_t, 65536> chunk;
		for (off_t offset = 0;;) {
			const auto count = ::pread(file->descriptor, chunk.data(), chunk.size(), offset);
			if (count < 0) {
				call.fail(30);
				return;
			}
			if (count == 0)
				break;
			bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
			offset += count;
		}
		const auto digest = subject_digest(bytes, sha256);
		if (digest.size() != length) {
			call.fail(0x80090008U);
			return;
		}
		call.write(hash_at, digest);
		call.ret(1);
	};
	registry.add("CryptCATAdminCalcHashFromFileHandle", 4, [hash_file](ApiCall &call) { hash_file(call, call[0], call[1], call[2], false); });
	registry.add("CryptCATAdminCalcHashFromFileHandle2", 5, [&context, hash_file](ApiCall &call) {
		const auto admin = context.objects.object_as<CatalogAdmin>(call[0]);
		if (!admin) {
			call.fail(87);
			return;
		}
		hash_file(call, call[1], call[2], call[3], admin->sha256);
	});
	registry.add("CryptCATAdminEnumCatalogFromHash", 5, [&context](ApiCall &call) {
		if (!context.objects.object_as<CatalogAdmin>(call[0]) || !call[1]) {
			call.fail(87);
			return;
		}
		call.fail(1168);
	});
	registry.add("CryptCATAdminReleaseCatalogContext", 3, [](ApiCall &call) { call.fail(87); });
	registry.add("CryptCATCatalogInfoFromContext", 3, [](ApiCall &call) { call.fail(87); });
	// Authenticode: WinVerifyTrust checks the file a WINTRUST_FILE_INFO names. With WTD_STATEACTION_VERIFY it keeps what it found under a
	// state handle, from which the trust helpers build the provider data, the signer and its certificate chain; WTD_STATEACTION_CLOSE
	// releases them.
	registry.add("WinVerifyTrust", 3, [&context](ApiCall &call) {
		constexpr uint32_t choice_file = 1, action_verify = 1, action_close = 2, unknown_subject = 0x800b0003U, file_not_found = 0x80070002U;
		if (!call[2]) {
			call.ret(0x80070057U);
			return;
		}
		const size_t width = call.pointer_width();
		Layout data(width);
		data.dword();
		data.pointer();
		data.pointer();
		data.dword();
		data.dword();
		const auto choice_at = data.dword(), union_at = data.pointer(), action_at = data.dword(), state_at = data.pointer();
		const uint32_t action = call.load<uint32_t>(call[2] + action_at);
		if (action == action_close) {
			const auto handle = call.load_pointer(call[2] + state_at);
			if (const auto state = context.objects.object_as<TrustState>(handle)) {
				for (const auto block : state->blocks)
					context.allocator.release(block);
				context.objects.close(handle);
			}
			call.store_pointer(call[2] + state_at, 0);
			call.ret(0);
			return;
		}
		if (call.load<uint32_t>(call[2] + choice_at) != choice_file || !call.load_pointer(call[2] + union_at)) {
			call.ret(unknown_subject);
			return;
		}
		// WINTRUST_FILE_INFO: its size, then the path.
		const auto path = call.load_pointer(call.load_pointer(call[2] + union_at) + width);
		const auto resolved = path ? context.paths.resolve(call.wide(path, 32768)) : std::nullopt;
		std::ifstream input;
		if (resolved && resolved->exists)
			input.open(resolved->host, std::ios::binary);
		if (!input) {
			call.ret(file_not_found);
			return;
		}
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		auto result = verify_authenticode(bytes);
		const uint32_t status = result.status;
		if (action == action_verify) {
			auto state = std::make_shared<TrustState>();
			state->result = std::move(result);
			const auto made = context.objects.add_object(state);
			call.store_pointer(call[2] + state_at, made.status ? 0 : made.value);
		}
		call.ret(status);
	});
	// The provider data of a verification: one signer, whose certificates are those of the chain, signer first, and the chain context.
	registry.add("WTHelperProvDataFromStateData", 1, [&context, write_chain, provider_layout](ApiCall &call) {
		const auto state = context.objects.object_as<TrustState>(call[0]);
		if (!state) {
			call.ret(0);
			return;
		}
		if (!state->provider) {
			const size_t width = call.pointer_width();
			const auto offsets = provider_layout(width);
			const auto &result = state->result;
			const uint32_t chain_status = result.anchored ? 0 : result.status == 0x800b010aU ? trust_partial_chain : trust_untrusted_root;
			const auto chain = write_chain(call, result.chain, chain_status, result.anchored);
			const size_t certs_at = (offsets.data_size + offsets.signer_size + 15) / 16 * 16;
			const size_t total = certs_at + result.chain.size() * offsets.cert_size;
			const auto block = context.allocator.allocate(total, true);
			if (!chain || !block) {
				call.fail(8, 0);
				return;
			}
			std::vector<uint8_t> image(total);
			const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(image.data() + at, &value, 4); };
			const auto put_pointer = [&](size_t at, uint64_t value) { std::memcpy(image.data() + at, &value, width); };
			put32(0, uint32_t(offsets.data_size));
			put32(offsets.data_error, result.status);
			put32(offsets.data_final_error, result.status);
			put32(offsets.data_signer_count, 1);
			put_pointer(offsets.data_signers, block + offsets.data_size);
			const size_t signer = offsets.data_size;
			put32(signer, uint32_t(offsets.signer_size));
			put32(signer + offsets.signer_cert_count, uint32_t(result.chain.size()));
			put_pointer(signer + offsets.signer_certs, block + certs_at);
			put32(signer + offsets.signer_error, result.status);
			put_pointer(signer + offsets.signer_chain, chain);
			// Each certificate shares the context its chain element made: CERT_CHAIN_CONTEXT, CERT_SIMPLE_CHAIN, CERT_CHAIN_ELEMENT.
			const auto elements = call.load_pointer(call.load_pointer(call.load_pointer(chain + 16)) + 16);
			for (size_t index = 0; index < result.chain.size(); ++index) {
				const size_t at = certs_at + index * offsets.cert_size;
				const auto element = call.load_pointer(elements + index * width);
				const bool last = index + 1 == result.chain.size();
				put32(at, uint32_t(offsets.cert_size));
				put_pointer(at + offsets.cert_context, call.load_pointer(element + width));
				put32(at + offsets.cert_trusted_root, last && result.anchored);
				put32(at + offsets.cert_self_signed, last && result.anchored);
				put32(at + offsets.cert_error, last ? result.status : 0);
				put_pointer(at + offsets.cert_element, element);
			}
			call.write(block, image);
			state->provider = block;
			state->blocks = {block, chain};
		}
		call.ret(state->provider);
	});
	registry.add("WTHelperGetProvSignerFromChain", 4, [provider_layout](ApiCall &call) {
		const auto offsets = provider_layout(call.pointer_width());
		if (!call[0] || call.u32(2) || call.u32(1) >= call.load<uint32_t>(call[0] + offsets.data_signer_count)) {
			call.ret(0);
			return;
		}
		call.ret(call.load_pointer(call[0] + offsets.data_signers) + call.u32(1) * offsets.signer_size);
	});
	registry.add("WTHelperGetProvCertFromChain", 2, [provider_layout](ApiCall &call) {
		const auto offsets = provider_layout(call.pointer_width());
		if (!call[0] || call.u32(1) >= call.load<uint32_t>(call[0] + offsets.signer_cert_count)) {
			call.ret(0);
			return;
		}
		call.ret(call.load_pointer(call[0] + offsets.signer_certs) + call.u32(1) * offsets.cert_size);
	});

	// Where CERT_INFO keeps the serial number and the issuer, which signer and search structures share.
	struct CertInfoLayout {
		size_t serial_size, serial_data, issuer_size, issuer_data, size;
	};
	const auto cert_info_layout = [](size_t width) {
		Layout info(width);
		info.dword();
		CertInfoLayout offsets{};
		offsets.serial_size = info.dword();
		offsets.serial_data = info.pointer();
		info.pointer();
		info.dword();
		info.pointer();
		offsets.issuer_size = info.dword();
		offsets.issuer_data = info.pointer();
		info.reserve(8, 4);
		info.reserve(8, 4);
		info.dword();
		info.pointer();
		info.pointer();
		info.dword();
		info.pointer();
		info.dword();
		info.pointer();
		info.dword();
		info.reserve(width == 4 ? 12 : 24, width);
		info.reserve(width == 4 ? 12 : 24, width);
		info.dword();
		info.pointer();
		offsets.size = info.size();
		return offsets;
	};
	// A structure written for the guest into one buffer at `base`: the fixed part, then the bytes its pointers lead to.
	struct Packed {
		std::vector<uint8_t> bytes;
		uint64_t base;
		size_t width;
		size_t reserve(size_t size)
		{
			const size_t at = (bytes.size() + 7) / 8 * 8;
			bytes.resize(at + size);
			return at;
		}
		uint64_t append(std::span<const uint8_t> data)
		{
			const size_t at = reserve(data.size());
			if (!data.empty())
				std::memcpy(bytes.data() + at, data.data(), data.size());
			return base + at;
		}
		uint64_t append_text(const std::string &text)
		{
			std::vector<uint8_t> data(text.begin(), text.end());
			data.push_back(0);
			return append(data);
		}
		void put32(size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); }
		void put_pointer(size_t at, uint64_t value) { std::memcpy(bytes.data() + at, &value, width); }
		void put_blob(size_t size_at, size_t data_at, std::span<const uint8_t> data)
		{
			const auto address = data.empty() ? 0 : append(data);
			put32(size_at, uint32_t(data.size()));
			put_pointer(data_at, address);
		}
	};
	// A serial number as CRYPT_INTEGER_BLOB holds it: little-endian.
	const auto serial_bytes = [](const ASN1_INTEGER *serial) {
		std::vector<uint8_t> bytes = asn1_bytes(serial);
		std::reverse(bytes.begin(), bytes.end());
		return bytes;
	};
	// CMSG_SIGNER_INFO, with its attributes as CRYPT_ATTRIBUTES whose values stay encoded.
	const auto signer_image = [serial_bytes](PKCS7_SIGNER_INFO *signer, uint64_t base, size_t width) {
		Packed packed{{}, base, width};
		Layout info(width);
		const auto version = info.dword(), issuer_size = info.dword(), issuer_data = info.pointer(), serial_size = info.dword(),
			serial_data = info.pointer(), hash_oid = info.pointer(), hash_parameters_size = info.dword(), hash_parameters = info.pointer(),
			encryption_oid = info.pointer(), encryption_parameters_size = info.dword(), encryption_parameters = info.pointer(),
			encrypted_size = info.dword(), encrypted_data = info.pointer(), signed_count = info.dword(), signed_list = info.pointer(),
			unsigned_count = info.dword(), unsigned_list = info.pointer();
		packed.reserve(info.size());
		packed.put32(version, uint32_t(ASN1_INTEGER_get(signer->version)));
		packed.put_blob(issuer_size, issuer_data, der_encoding(i2d_X509_NAME, signer->issuer_and_serial->issuer));
		packed.put_blob(serial_size, serial_data, serial_bytes(signer->issuer_and_serial->serial));
		const auto algorithm = [&](X509_ALGOR *value, size_t oid_at, size_t size_at, size_t data_at) {
			char text[80] = {};
			OBJ_obj2txt(text, sizeof(text), value->algorithm, 1);
			packed.put_pointer(oid_at, packed.append_text(text));
			packed.put_blob(size_at, data_at, value->parameter ? der_encoding(i2d_ASN1_TYPE, value->parameter) : std::vector<uint8_t>());
		};
		algorithm(signer->digest_alg, hash_oid, hash_parameters_size, hash_parameters);
		algorithm(signer->digest_enc_alg, encryption_oid, encryption_parameters_size, encryption_parameters);
		packed.put_blob(encrypted_size, encrypted_data, asn1_bytes(signer->enc_digest));
		const auto attributes = [&](STACK_OF(X509_ATTRIBUTE) *list, size_t count_at, size_t array_at) {
			const int count = list ? sk_X509_ATTRIBUTE_num(list) : 0;
			Layout attribute(width);
			const auto attribute_oid = attribute.pointer(), attribute_count = attribute.dword(), attribute_values = attribute.pointer();
			const size_t blob_size = width == 4 ? 8 : 16;
			const size_t array = packed.reserve(size_t(count) * attribute.size());
			packed.put32(count_at, uint32_t(count));
			packed.put_pointer(array_at, count ? base + array : 0);
			for (int index = 0; index < count; ++index) {
				X509_ATTRIBUTE *entry = sk_X509_ATTRIBUTE_value(list, index);
				const size_t at = array + size_t(index) * attribute.size();
				char text[80] = {};
				OBJ_obj2txt(text, sizeof(text), X509_ATTRIBUTE_get0_object(entry), 1);
				packed.put_pointer(at + attribute_oid, packed.append_text(text));
				const int values = X509_ATTRIBUTE_count(entry);
				const size_t blobs = packed.reserve(size_t(values) * blob_size);
				packed.put32(at + attribute_count, uint32_t(values));
				packed.put_pointer(at + attribute_values, values ? base + blobs : 0);
				for (int value = 0; value < values; ++value)
					packed.put_blob(blobs + size_t(value) * blob_size, blobs + size_t(value) * blob_size + (width == 4 ? 4 : 8),
						der_encoding(i2d_ASN1_TYPE, X509_ATTRIBUTE_get0_type(entry, value)));
			}
		};
		attributes(signer->auth_attr, signed_count, signed_list);
		attributes(signer->unauth_attr, unsigned_count, unsigned_list);
		return std::move(packed.bytes);
	};
	// A query result or a parameter the guest receives in its buffer: the size first, and the bytes when they fit.
	const auto answer_bytes = [](ApiCall &call, uint64_t data, uint64_t size_at, const std::function<std::vector<uint8_t>(uint64_t)> &build) {
		const auto measured = build(0);
		const uint32_t capacity = call.load<uint32_t>(size_at);
		call.store<uint32_t>(size_at, uint32_t(measured.size()));
		if (!data) {
			call.ret(1);
			return;
		}
		if (capacity < measured.size()) {
			call.fail(234);
			return;
		}
		call.write(data, build(data));
		call.ret(1);
	};
	using PKCS7Pointer = std::unique_ptr<PKCS7, decltype(&PKCS7_free)>;
	const auto message_of = [&objects](uint64_t handle) -> PKCS7Pointer {
		const auto message = objects.object_as<CryptMessage>(handle);
		if (!message)
			return PKCS7Pointer(nullptr, PKCS7_free);
		const unsigned char *at = message->encoded.data();
		return PKCS7Pointer(d2i_PKCS7(nullptr, &at, long(message->encoded.size())), PKCS7_free);
	};
	// CryptQueryObject: a file or blob holding a signature in the program, cabinet or database it signs, a bare PKCS #7 signature, or
	// a certificate; the signature comes back as a message and a store of its certificates.
	registry.add("CryptQueryObject", 11, [&context, &objects, make_context](ApiCall &call) {
		constexpr uint32_t object_file = 1, object_blob = 2, content_certificate = 1, content_signed = 8, content_signed_embedded = 10;
		constexpr uint32_t encoding = 0x10001, format_binary = 1, not_found = 0x80092009U;
		const uint32_t expected = call.u32(2);
		std::vector<uint8_t> bytes;
		if (call.u32(0) == object_file && call[1]) {
			const auto resolved = context.paths.resolve(call.wide(call[1], 32768));
			std::ifstream input;
			if (resolved && resolved->exists)
				input.open(resolved->host, std::ios::binary);
			if (!input) {
				call.fail(2, 0);
				return;
			}
			bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		} else if (call.u32(0) == object_blob && call[1]) {
			bytes = call.read(call.load_pointer(call[1] + call.pointer_width()), call.load<uint32_t>(call[1]));
		} else {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		uint32_t content = 0;
		std::vector<uint8_t> signature;
		if (expected & (1u << content_signed_embedded)) {
			signature = embedded_signature(bytes);
			if (!signature.empty())
				content = content_signed_embedded;
		}
		if (!content && (expected & (1u << content_signed))) {
			const unsigned char *at = bytes.data();
			if (PKCS7Pointer parsed(d2i_PKCS7(nullptr, &at, long(bytes.size())), PKCS7_free); parsed && PKCS7_type_is_signed(parsed.get())) {
				signature = bytes;
				content = content_signed;
			}
		}
		if (!content && (expected & (1u << content_certificate)) && parse(bytes)) {
			if (call[10])
				call.store_pointer(call[10], make_context(call, bytes, 0));
			content = content_certificate;
		}
		if (!content) {
			call.fail(not_found, 0);
			return;
		}
		if (call[5])
			call.store<uint32_t>(call[5], encoding);
		if (call[6])
			call.store<uint32_t>(call[6], content);
		if (call[7])
			call.store<uint32_t>(call[7], format_binary);
		if (content != content_certificate) {
			const unsigned char *at = signature.data();
			PKCS7Pointer parsed(d2i_PKCS7(nullptr, &at, long(signature.size())), PKCS7_free);
			if (call[8]) {
				auto store = std::make_shared<CertificateStore>();
				STACK_OF(X509) *certificates = parsed && PKCS7_type_is_signed(parsed.get()) ? parsed->d.sign->cert : nullptr;
				for (int index = 0; certificates && index < sk_X509_num(certificates); ++index)
					store->certificates.push_back(der_of(sk_X509_value(certificates, index)));
				const auto made = objects.add_object(store);
				call.store_pointer(call[8], made.status ? 0 : made.value);
			}
			if (call[9]) {
				auto message = std::make_shared<CryptMessage>();
				message->encoded = std::move(signature);
				const auto made = objects.add_object(message);
				call.store_pointer(call[9], made.status ? 0 : made.value);
			}
		}
		call.ret(1);
	});
	// CryptDecodeObject: the encoded structures signature readers take apart: a signer of a message (a countersignature), a UTC time and
	// an octet string. With CRYPT_DECODE_ALLOC_FLAG, CryptDecodeObjectEx places the result in memory it allocates.
	const auto decode = [signer_image, &allocator](ApiCall &call, uint64_t type, uint64_t data, uint32_t size, bool allocate, uint64_t out,
		uint64_t size_at) {
		constexpr uint64_t utc_time = 17, octet_string = 25, signer_info = 500;
		if (type > 0xffff) {
			const auto name = call.narrow(type, 80);
			type = name == "1.2.840.113549.1.9.5" ? utc_time : 0;
		}
		const auto bytes = call.read(data, size);
		const size_t width = call.pointer_width();
		std::function<std::vector<uint8_t>(uint64_t)> build;
		if (type == signer_info) {
			const unsigned char *at = bytes.data();
			auto parsed = std::shared_ptr<PKCS7_SIGNER_INFO>(d2i_PKCS7_SIGNER_INFO(nullptr, &at, long(bytes.size())), PKCS7_SIGNER_INFO_free);
			if (parsed)
				build = [parsed, signer_image, width](uint64_t base) { return signer_image(parsed.get(), base, width); };
		} else if (type == utc_time) {
			const unsigned char *at = bytes.data();
			if (ASN1_TIME *time = d2i_ASN1_TIME(nullptr, &at, long(bytes.size()))) {
				const uint64_t ticks = filetime_of(time);
				ASN1_TIME_free(time);
				build = [ticks](uint64_t) {
					std::vector<uint8_t> image(8);
					std::memcpy(image.data(), &ticks, 8);
					return image;
				};
			}
		} else if (type == octet_string) {
			const unsigned char *at = bytes.data();
			if (ASN1_OCTET_STRING *octets = d2i_ASN1_OCTET_STRING(nullptr, &at, long(bytes.size()))) {
				std::vector<uint8_t> content = asn1_bytes(octets);
				ASN1_OCTET_STRING_free(octets);
				build = [content, width](uint64_t base) {
					std::vector<uint8_t> image(width * 2 + content.size());
					const uint32_t length = uint32_t(content.size());
					const uint64_t pointer = base + width * 2;
					std::memcpy(image.data(), &length, 4);
					std::memcpy(image.data() + width, &pointer, width);
					std::memcpy(image.data() + width * 2, content.data(), content.size());
					return image;
				};
			}
		}
		if (!build) {
			call.fail(type ? 0x8009310bU : 0x80092002U);
			return;
		}
		if (allocate) {
			const auto measured = build(0);
			const auto block = allocator.allocate(measured.size(), true);
			if (!block) {
				call.fail(8);
				return;
			}
			call.write(block, build(block));
			call.store_pointer(out, block);
			if (size_at)
				call.store<uint32_t>(size_at, uint32_t(measured.size()));
			call.ret(1);
			return;
		}
		const auto measured = build(0);
		const uint32_t capacity = call.load<uint32_t>(size_at);
		call.store<uint32_t>(size_at, uint32_t(measured.size()));
		if (!out) {
			call.ret(1);
			return;
		}
		if (capacity < measured.size()) {
			call.fail(234);
			return;
		}
		call.write(out, build(out));
		call.ret(1);
	};
	registry.add("CryptDecodeObject", 7, [decode](ApiCall &call) { decode(call, call[1], call[2], call.u32(3), false, call[5], call[6]); });
	registry.add("CryptDecodeObjectEx", 8, [decode](ApiCall &call) {
		constexpr uint32_t allocate_flag = 0x8000;
		decode(call, call[1], call[2], call.u32(3), call.u32(4) & allocate_flag, call[6], call[7]);
	});
	registry.add("CryptMsgClose", 1, [&objects](ApiCall &call) {
		if (objects.object_as<CryptMessage>(call[0]))
			objects.close(call[0]);
		call.ret(1);
	});
	// The parameters of a signed message: its type and content, its signers and their certificates' issuer and serial number, and the
	// certificates it carries.
	registry.add("CryptMsgGetParam", 5, [message_of, signer_image, answer_bytes, serial_bytes, cert_info_layout](ApiCall &call) {
		constexpr uint32_t type_param = 1, content_param = 2, inner_type_param = 4, signer_count_param = 5, signer_info_param = 6,
			signer_cert_info_param = 7, cert_count_param = 11, cert_param = 12;
		const auto message = message_of(call[0]);
		if (!message || !PKCS7_type_is_signed(message.get()) || !call[4]) {
			call.fail(message ? 0x8009100cU : 6);
			return;
		}
		const auto *signers = PKCS7_get_signer_info(message.get());
		const auto *certificates = message->d.sign->cert;
		const uint32_t index = call.u32(2);
		const auto word = [&](uint32_t value) {
			answer_bytes(call, call[3], call[4], [value](uint64_t) {
				std::vector<uint8_t> bytes(4);
				std::memcpy(bytes.data(), &value, 4);
				return bytes;
			});
		};
		switch (call.u32(1)) {
		case type_param:
			word(2);
			return;
		case content_param: {
			const auto *other = message->d.sign->contents->d.other;
			const std::vector<uint8_t> inner = other && other->type == V_ASN1_SEQUENCE
				? asn1_bytes(other->value.sequence) : std::vector<uint8_t>();
			answer_bytes(call, call[3], call[4], [&](uint64_t) { return inner; });
			return;
		}
		case inner_type_param: {
			char text[80] = {};
			OBJ_obj2txt(text, sizeof(text), message->d.sign->contents->type, 1);
			std::vector<uint8_t> oid(text, text + std::strlen(text) + 1);
			answer_bytes(call, call[3], call[4], [&](uint64_t) { return oid; });
			return;
		}
		case signer_count_param:
			word(uint32_t(sk_PKCS7_SIGNER_INFO_num(signers)));
			return;
		case signer_info_param:
		case signer_cert_info_param: {
			if (int(index) >= sk_PKCS7_SIGNER_INFO_num(signers)) {
				call.fail(0x80091008U);
				return;
			}
			PKCS7_SIGNER_INFO *signer = sk_PKCS7_SIGNER_INFO_value(signers, int(index));
			const size_t width = call.pointer_width();
			if (call.u32(1) == signer_info_param) {
				answer_bytes(call, call[3], call[4], [&](uint64_t base) { return signer_image(signer, base, width); });
				return;
			}
			answer_bytes(call, call[3], call[4], [&](uint64_t base) {
				const auto offsets = cert_info_layout(width);
				Packed packed{{}, base, width};
				packed.reserve(offsets.size);
				packed.put_blob(offsets.serial_size, offsets.serial_data, serial_bytes(signer->issuer_and_serial->serial));
				packed.put_blob(offsets.issuer_size, offsets.issuer_data, der_encoding(i2d_X509_NAME, signer->issuer_and_serial->issuer));
				return std::move(packed.bytes);
			});
			return;
		}
		case cert_count_param:
			word(uint32_t(certificates ? sk_X509_num(certificates) : 0));
			return;
		case cert_param: {
			if (!certificates || int(index) >= sk_X509_num(certificates)) {
				call.fail(0x80091008U);
				return;
			}
			const auto bytes = der_of(sk_X509_value(certificates, int(index)));
			answer_bytes(call, call[3], call[4], [&](uint64_t) { return bytes; });
			return;
		}
		default:
			call.fail(0x8009100bU);
		}
	});
	// CryptMsgGetAndVerifySigner: the certificate of a signer, found among the message's own certificates and the stores given, whose
	// signature over the content is checked unless only the signer is asked for.
	registry.add("CryptMsgGetAndVerifySigner", 6, [&objects, message_of, make_context](ApiCall &call) {
		constexpr uint32_t signer_only = 2, use_index = 4, signer_not_found = 0x8009100eU, bad_signature = 0x80091003U;
		const auto message = message_of(call[0]);
		if (!message || !PKCS7_type_is_signed(message.get())) {
			call.fail(message ? 0x8009100cU : 6, 0);
			return;
		}
		STACK_OF(X509) *candidates = sk_X509_new_null();
		std::vector<X509Pointer> held;
		for (int index = 0; message->d.sign->cert && index < sk_X509_num(message->d.sign->cert); ++index)
			sk_X509_push(candidates, sk_X509_value(message->d.sign->cert, index));
		for (uint32_t store = 0; store < call.u32(1) && call[2]; ++store)
			if (const auto extra = objects.object_as<CertificateStore>(call.load_pointer(call[2] + store * call.pointer_width())))
				for (const auto &bytes : extra->certificates)
					if (auto parsed = parse(bytes)) {
						sk_X509_push(candidates, parsed.get());
						held.push_back(std::move(parsed));
					}
		const auto *signers = PKCS7_get_signer_info(message.get());
		const uint32_t first = (call.u32(3) & use_index) && call[5] ? call.load<uint32_t>(call[5]) : 0;
		for (int index = int(first); index < sk_PKCS7_SIGNER_INFO_num(signers); ++index) {
			PKCS7_SIGNER_INFO *signer = sk_PKCS7_SIGNER_INFO_value(signers, index);
			X509 *certificate = X509_find_by_issuer_and_serial(candidates, signer->issuer_and_serial->issuer, signer->issuer_and_serial->serial);
			if (!certificate) {
				if (call.u32(3) & use_index)
					break;
				continue;
			}
			if (!(call.u32(3) & signer_only)) {
				// The content an Authenticode signer signs is the indirect data without its own header.
				const auto *other = message->d.sign->contents->d.other;
				std::unique_ptr<BIO, decltype(&BIO_free)> content(nullptr, BIO_free);
				if (other && other->type == V_ASN1_SEQUENCE && ASN1_STRING_length(other->value.sequence) > 2) {
					const unsigned char *bytes = ASN1_STRING_get0_data(other->value.sequence);
					const size_t length = size_t(ASN1_STRING_length(other->value.sequence));
					const size_t header = bytes[1] < 0x80 ? 2 : 2 + (bytes[1] & 0x7f);
					content.reset(BIO_new_mem_buf(bytes + header, int(length - header)));
				}
				STACK_OF(X509) *one = sk_X509_new_null();
				sk_X509_push(one, certificate);
				const int verified = PKCS7_verify(message.get(), one, nullptr, content.get(), nullptr, PKCS7_NOVERIFY | PKCS7_BINARY | PKCS7_NOINTERN);
				sk_X509_free(one);
				if (verified != 1) {
					sk_X509_free(candidates);
					call.fail(bad_signature, 0);
					return;
				}
			}
			if (call[4])
				call.store_pointer(call[4], make_context(call, der_of(certificate), 0));
			if (call[5])
				call.store<uint32_t>(call[5], uint32_t(index));
			sk_X509_free(candidates);
			call.ret(1);
			return;
		}
		sk_X509_free(candidates);
		call.fail(signer_not_found, 0);
	});
	// CertFindCertificateInStore: by issuer and serial number, by SHA-1 hash, by encoded or written subject or issuer name, or any; the
	// search goes on after the previous context, which it releases.
	registry.add("CertFindCertificateInStore", 6, [&objects, &allocator, make_context, encoded_of, serial_bytes, cert_info_layout](ApiCall &call) {
		constexpr uint32_t find_any = 0, find_sha1 = 1 << 16, find_subject_name = (2 << 16) | 7, find_issuer_name = (2 << 16) | 4,
			find_subject_string = (8 << 16) | 7, find_issuer_string = (8 << 16) | 4, find_subject_cert = 11 << 16, not_found = 0x80092004U;
		const auto store = objects.object_as<CertificateStore>(call[0]);
		if (!store) {
			call.fail(6, 0);
			return;
		}
		const size_t width = call.pointer_width();
		size_t next = 0;
		if (call[5]) {
			const auto previous = encoded_of(call, call[5]);
			for (size_t index = 0; index < store->certificates.size(); ++index)
				if (store->certificates[index] == previous) {
					next = index + 1;
					break;
				}
			allocator.release(call[5]);
		}
		const uint32_t type = call.u32(3);
		const auto blob = [&](uint64_t at) { return call.read(call.load_pointer(at + width), call.load<uint32_t>(at)); };
		const auto lowered = [](std::u16string text) {
			for (auto &character : text)
				character = fold_case(character);
			return text;
		};
		for (; next < store->certificates.size(); ++next) {
			const auto certificate = parse(store->certificates[next]);
			if (!certificate)
				continue;
			bool match = false;
			if (type == find_any) {
				match = true;
			} else if (type == find_sha1 && call[4]) {
				std::vector<uint8_t> hash(SHA_DIGEST_LENGTH);
				SHA1(store->certificates[next].data(), store->certificates[next].size(), hash.data());
				match = blob(call[4]) == hash;
			} else if ((type == find_subject_name || type == find_issuer_name) && call[4]) {
				match = blob(call[4]) == der_encoding(i2d_X509_NAME, type == find_subject_name ? X509_get_subject_name(certificate.get())
					: X509_get_issuer_name(certificate.get()));
			} else if ((type == find_subject_string || type == find_issuer_string) && call[4]) {
				const auto wanted = lowered(call.wide(call[4], 1024));
				const auto text = lowered(from_utf8(name_text(type == find_subject_string ? X509_get_subject_name(certificate.get())
					: X509_get_issuer_name(certificate.get()), 3)));
				match = text.find(wanted) != text.npos;
			} else if (type == find_subject_cert && call[4]) {
				const auto offsets = cert_info_layout(width);
				match = blob(call[4] + offsets.issuer_size) == der_encoding(i2d_X509_NAME, X509_get_issuer_name(certificate.get())) &&
					blob(call[4] + offsets.serial_size) == serial_bytes(X509_get_serialNumber(certificate.get()));
			}
			if (match) {
				call.ret(make_context(call, store->certificates[next], call[0]));
				return;
			}
		}
		call.fail(not_found, 0);
	});
	// CertGetNameString: the display name, an attribute, the distinguished name, or the e-mail or DNS name, of the subject or the issuer.
	for (const bool wide : {true, false})
		registry.add(wide ? "CertGetNameStringW" : "CertGetNameStringA", 6, [encoded_of, wide](ApiCall &call) {
			constexpr uint32_t email = 1, rdn = 2, attribute = 3, simple_display = 4, friendly_display = 5, dns = 6, issuer_flag = 1;
			std::string text;
			if (call[0])
				if (const auto certificate = parse(encoded_of(call, call[0]))) {
					const X509_NAME *name = call.u32(2) & issuer_flag ? X509_get_issuer_name(certificate.get()) : X509_get_subject_name(certificate.get());
					switch (call.u32(1)) {
					case email: text = name_attribute(name, NID_pkcs9_emailAddress); break;
					case rdn: text = name_text(name, call[3] ? call.load<uint32_t>(call[3]) : 3); break;
					case attribute:
						if (call[3])
							text = name_attribute(name, OBJ_txt2nid(call.narrow(call[3], 80).c_str()));
						break;
					case simple_display: case friendly_display:
						for (const int nid : {NID_commonName, NID_organizationalUnitName, NID_organizationName, NID_pkcs9_emailAddress})
							if (text.empty())
								text = name_attribute(name, nid);
						break;
					case dns: text = name_attribute(name, NID_commonName); break;
					}
				}
			if (!call[4] || !call.u32(5)) {
				call.ret((wide ? from_utf8(text).size() : text.size()) + 1);
				return;
			}
			if (wide) {
				const auto converted = from_utf8(text).substr(0, call.u32(5) - 1);
				call.put_wide(call[4], converted, converted.size() + 1);
				call.ret(converted.size() + 1);
			} else {
				const auto converted = text.substr(0, call.u32(5) - 1);
				call.put_narrow(call[4], converted, converted.size() + 1);
				call.ret(converted.size() + 1);
			}
		});
	registry.add("CertNameToStrW", 5, [](ApiCall &call) {
		std::u16string text;
		if (call[1]) {
			const auto bytes = call.read(call.load_pointer(call[1] + call.pointer_width()), call.load<uint32_t>(call[1]));
			const unsigned char *at = bytes.data();
			if (X509_NAME *name = d2i_X509_NAME(nullptr, &at, long(bytes.size()))) {
				text = from_utf8(name_text(name, call.u32(2)));
				X509_NAME_free(name);
			}
		}
		if (!call[3] || !call.u32(4)) {
			call.ret(text.size() + 1);
			return;
		}
		const auto converted = text.substr(0, call.u32(4) - 1);
		call.put_wide(call[3], converted, converted.size() + 1);
		call.ret(converted.size() + 1);
	});

	// Data protection: the blob is sealed with AES-256-GCM under a key made from the prefix secret and the optional entropy.
	const auto blob_in = [](ApiCall &call, uint64_t blob) {
		const size_t width = call.pointer_width();
		if (!blob)
			return std::optional<std::vector<uint8_t>>();
		const auto size = call.load<uint32_t>(blob);
		const uint64_t data = width == 4 ? call.load<uint32_t>(blob + 4) : call.load<uint64_t>(blob + 8);
		if (!size)
			return std::optional(std::vector<uint8_t>());
		if (!data)
			return std::optional<std::vector<uint8_t>>();
		return std::optional(call.read(data, size));
	};
	const auto blob_out = [&context](ApiCall &call, uint64_t blob, std::span<const uint8_t> bytes) {
		const size_t width = call.pointer_width();
		const auto memory = context.allocator.allocate(std::max<size_t>(bytes.size(), 1), true);
		if (!memory)
			return false;
		if (!bytes.empty())
			call.write(memory, bytes);
		call.store<uint32_t>(blob, uint32_t(bytes.size()));
		if (width == 4)
			call.store<uint32_t>(blob + 4, uint32_t(memory));
		else
			call.store<uint64_t>(blob + 8, memory);
		return true;
	};
	// No key storage provider is installed here, so key creation and use fail the way they do on a machine without one; programs fall back.
	for (const auto &[name, count] : {std::pair<const char *, size_t>{"NCryptOpenStorageProvider", 3}, {"NCryptIsAlgSupported", 3},
		{"NCryptCreatePersistedKey", 6}, {"NCryptFinalizeKey", 2}, {"NCryptExportKey", 8}, {"NCryptOpenKey", 5}, {"NCryptSignHash", 8},
		{"NCryptGetProperty", 6}, {"NCryptSetProperty", 5}, {"NCryptDeleteKey", 2}, {"NCryptEnumKeys", 5}})
		registry.add(name, count, [](ApiCall &call) { call.ret(0x80090029U); });
	registry.add("NCryptFreeObject", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("CryptProtectData", 7, [blob_in, blob_out](ApiCall &call) {
		const auto plain = blob_in(call, call[0]);
		if (!plain || !call[6]) {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		const auto entropy = blob_in(call, call[2]).value_or(std::vector<uint8_t>());
		const auto description = call[1] ? call.wide(call[1]) : std::u16string();
		const auto key = protection_key(entropy);
		uint8_t iv[12], tag[16];
		RAND_bytes(iv, sizeof(iv));
		std::vector<uint8_t> sealed(plain->size());
		int length = 0, total = 0;
		std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
		if (!EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, key.data(), iv) ||
			(!plain->empty() && !EVP_EncryptUpdate(context.get(), sealed.data(), &length, plain->data(), int(plain->size()))) ||
			!EVP_EncryptFinal_ex(context.get(), sealed.data() + length, &total) || !EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag)) {
			call.fail(error_invalid_data, 0);
			return;
		}
		std::vector<uint8_t> blob(protected_magic, protected_magic + 4);
		const uint32_t description_bytes = uint32_t(description.size() * 2);
		blob.insert(blob.end(), reinterpret_cast<const uint8_t *>(&description_bytes), reinterpret_cast<const uint8_t *>(&description_bytes) + 4);
		blob.insert(blob.end(), reinterpret_cast<const uint8_t *>(description.data()), reinterpret_cast<const uint8_t *>(description.data()) + description_bytes);
		blob.insert(blob.end(), iv, iv + sizeof(iv));
		blob.insert(blob.end(), tag, tag + sizeof(tag));
		blob.insert(blob.end(), sealed.begin(), sealed.end());
		if (!blob_out(call, call[6], blob)) {
			call.fail(8, 0);
			return;
		}
		call.ret(1);
	});
	registry.add("CryptUnprotectData", 7, [blob_in, blob_out, &context](ApiCall &call) {
		const auto blob = blob_in(call, call[0]);
		if (!blob || !call[6]) {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		if (blob->size() < 4 + 4 + 12 + 16 || std::memcmp(blob->data(), protected_magic, 4)) {
			call.fail(error_invalid_data, 0);
			return;
		}
		uint32_t description_bytes;
		std::memcpy(&description_bytes, blob->data() + 4, 4);
		if (description_bytes % 2 || size_t(8) + description_bytes + 28 > blob->size()) {
			call.fail(error_invalid_data, 0);
			return;
		}
		const uint8_t *iv = blob->data() + 8 + description_bytes, *tag = iv + 12, *cipher = tag + 16;
		const size_t cipher_size = blob->size() - (8 + description_bytes + 28);
		const auto entropy = blob_in(call, call[2]).value_or(std::vector<uint8_t>());
		const auto key = protection_key(entropy);
		std::vector<uint8_t> plain(cipher_size);
		int length = 0, total = 0;
		std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher_context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
		if (!EVP_DecryptInit_ex(cipher_context.get(), EVP_aes_256_gcm(), nullptr, key.data(), iv) ||
			(cipher_size && !EVP_DecryptUpdate(cipher_context.get(), plain.data(), &length, cipher, int(cipher_size))) ||
			!EVP_CIPHER_CTX_ctrl(cipher_context.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t *>(tag)) ||
			EVP_DecryptFinal_ex(cipher_context.get(), plain.data() + length, &total) <= 0) {
			// Wrong entropy or a changed blob.
			call.fail(error_invalid_data, 0);
			return;
		}
		if (call[1]) {
			std::u16string description(description_bytes / 2, u'\0');
			std::memcpy(description.data(), blob->data() + 8, description_bytes);
			const auto memory = context.allocator.allocate(description_bytes + 2, true);
			if (!memory) {
				call.fail(8, 0);
				return;
			}
			call.put_wide(memory, description, description.size() + 1);
			call.store_pointer(call[1], memory);
		}
		if (!blob_out(call, call[6], plain)) {
			call.fail(8, 0);
			return;
		}
		call.ret(1);
	});
}

#else

void register_crypt32_api(ApiRegistry &, KernelContext &)
{
}

#endif

}
