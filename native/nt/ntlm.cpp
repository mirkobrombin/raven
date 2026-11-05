#include "ntlm.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cstring>

namespace raven::ntlm {

namespace {

constexpr uint8_t signature[8] = {'N', 'T', 'L', 'M', 'S', 'S', 'P', 0};
constexpr uint32_t type_negotiate = 1, type_challenge = 2, type_authenticate = 3;
// What both sides here insist on: version 2 responses need a Unicode exchange, and the session security this implements is the
// extended one with 128-bit keys.
constexpr uint32_t required = flag_unicode | flag_ntlm | flag_extended_session | flag_128;
constexpr uint32_t supported = required | flag_request_target | flag_sign | flag_seal | flag_always_sign | flag_target_info |
	flag_version | flag_key_exchange | flag_56;
constexpr uint32_t flag_target_type_server = 0x20000;
// The AV pairs of a challenge's target information.
constexpr uint16_t av_end = 0, av_computer = 1, av_domain = 2, av_dns_computer = 3, av_dns_domain = 4, av_timestamp = 7;
// The version a message announces: Windows 10, build 19041, NTLM revision 15.
constexpr uint8_t version[8] = {10, 0, 0x61, 0x4a, 0, 0, 0, 15};

void put16(std::vector<uint8_t> &bytes, size_t at, uint16_t value)
{
	bytes[at] = uint8_t(value);
	bytes[at + 1] = uint8_t(value >> 8);
}

void put32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
	for (int byte = 0; byte < 4; ++byte)
		bytes[at + byte] = uint8_t(value >> (8 * byte));
}

uint16_t get16(std::span<const uint8_t> bytes, size_t at)
{
	return uint16_t(bytes[at] | bytes[at + 1] << 8);
}

uint32_t get32(std::span<const uint8_t> bytes, size_t at)
{
	return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
}

std::vector<uint8_t> utf16(std::u16string_view text)
{
	std::vector<uint8_t> bytes;
	for (const char16_t character : text) {
		bytes.push_back(uint8_t(character));
		bytes.push_back(uint8_t(character >> 8));
	}
	return bytes;
}

std::u16string from_utf16(std::span<const uint8_t> bytes)
{
	std::u16string text;
	for (size_t at = 0; at + 1 < bytes.size(); at += 2)
		text.push_back(char16_t(bytes[at] | bytes[at + 1] << 8));
	return text;
}

char16_t upper(char16_t character)
{
	return character >= u'a' && character <= u'z' ? char16_t(character - 32) : character;
}

// A message being written: the fixed part, then the payload its fields point into.
struct Writer {
	std::vector<uint8_t> bytes;

	explicit Writer(size_t fixed, uint32_t type) : bytes(fixed)
	{
		std::memcpy(bytes.data(), signature, 8);
		put32(bytes, 8, type);
	}
	void field(size_t at, std::span<const uint8_t> value)
	{
		put16(bytes, at, uint16_t(value.size()));
		put16(bytes, at + 2, uint16_t(value.size()));
		put32(bytes, at + 4, uint32_t(bytes.size()));
		bytes.insert(bytes.end(), value.begin(), value.end());
	}
};

// A message being read: its type checked, and each field a span inside it or nothing when it points outside.
struct Reader {
	std::span<const uint8_t> bytes;

	bool valid(uint32_t type, size_t fixed) const
	{
		return bytes.size() >= fixed && std::equal(signature, signature + 8, bytes.begin()) && get32(bytes, 8) == type;
	}
	std::optional<std::span<const uint8_t>> field(size_t at) const
	{
		const size_t length = get16(bytes, at), offset = get32(bytes, at + 4);
		if (offset > bytes.size() || length > bytes.size() - offset)
			return std::nullopt;
		return bytes.subspan(offset, length);
	}
};

std::vector<uint8_t> av_pair(uint16_t id, std::span<const uint8_t> value)
{
	std::vector<uint8_t> pair(4);
	put16(pair, 0, id);
	put16(pair, 2, uint16_t(value.size()));
	pair.insert(pair.end(), value.begin(), value.end());
	return pair;
}

std::optional<std::span<const uint8_t>> find_av(std::span<const uint8_t> info, uint16_t wanted)
{
	for (size_t at = 0; at + 4 <= info.size();) {
		const uint16_t id = get16(info, at), length = get16(info, at + 2);
		if (id == av_end || at + 4 + length > info.size())
			break;
		if (id == wanted)
			return info.subspan(at + 4, length);
		at += 4 + length;
	}
	return std::nullopt;
}

Key derived(const Key &session, const char *magic)
{
	std::vector<uint8_t> bytes(session.begin(), session.end());
	bytes.insert(bytes.end(), magic, magic + std::strlen(magic) + 1);
	return md5(bytes);
}

// The MD4 of RFC 1320: three rounds over each 64-byte block, the length in bits appended after the padding.
uint32_t rotate(uint32_t value, int count)
{
	return value << count | value >> (32 - count);
}

}

Key md4(std::span<const uint8_t> input)
{
	std::vector<uint8_t> bytes(input.begin(), input.end());
	const uint64_t bits = uint64_t(input.size()) * 8;
	bytes.push_back(0x80);
	while (bytes.size() % 64 != 56)
		bytes.push_back(0);
	for (int byte = 0; byte < 8; ++byte)
		bytes.push_back(uint8_t(bits >> (8 * byte)));
	uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
	for (size_t block = 0; block < bytes.size(); block += 64) {
		uint32_t x[16];
		for (int word = 0; word < 16; ++word)
			x[word] = get32(bytes, block + 4 * word);
		const uint32_t aa = a, bb = b, cc = c, dd = d;
		const auto f = [](uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); };
		const auto g = [](uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (x & z) | (y & z); };
		const auto h = [](uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; };
		static constexpr int shifts1[4] = {3, 7, 11, 19}, shifts2[4] = {3, 5, 9, 13}, shifts3[4] = {3, 9, 11, 15};
		static constexpr int order2[16] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
		static constexpr int order3[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
		uint32_t *state[4] = {&a, &d, &c, &b};
		for (int step = 0; step < 16; ++step) {
			uint32_t &target = *state[step % 4];
			const uint32_t one = *state[(step + 3) % 4], two = *state[(step + 2) % 4], three = *state[(step + 1) % 4];
			target = rotate(target + f(one, two, three) + x[step], shifts1[step % 4]);
		}
		for (int step = 0; step < 16; ++step) {
			uint32_t &target = *state[step % 4];
			const uint32_t one = *state[(step + 3) % 4], two = *state[(step + 2) % 4], three = *state[(step + 1) % 4];
			target = rotate(target + g(one, two, three) + x[order2[step]] + 0x5a827999, shifts2[step % 4]);
		}
		for (int step = 0; step < 16; ++step) {
			uint32_t &target = *state[step % 4];
			const uint32_t one = *state[(step + 3) % 4], two = *state[(step + 2) % 4], three = *state[(step + 1) % 4];
			target = rotate(target + h(one, two, three) + x[order3[step]] + 0x6ed9eba1, shifts3[step % 4]);
		}
		a += aa;
		b += bb;
		c += cc;
		d += dd;
	}
	Key result{};
	const uint32_t words[4] = {a, b, c, d};
	for (int word = 0; word < 4; ++word)
		for (int byte = 0; byte < 4; ++byte)
			result[4 * word + byte] = uint8_t(words[word] >> (8 * byte));
	return result;
}

Key md5(std::span<const uint8_t> bytes)
{
	Key result{};
	unsigned int length = 0;
	EVP_Digest(bytes.data(), bytes.size(), result.data(), &length, EVP_md5(), nullptr);
	return result;
}

Key hmac_md5(std::span<const uint8_t> key, std::span<const uint8_t> bytes)
{
	Key result{};
	unsigned int length = 0;
	HMAC(EVP_md5(), key.data(), int(key.size()), bytes.data(), bytes.size(), result.data(), &length);
	return result;
}

Rc4::Rc4(std::span<const uint8_t> key)
{
	for (int index = 0; index < 256; ++index)
		state[index] = uint8_t(index);
	uint8_t mixed = 0;
	for (int index = 0; index < 256; ++index) {
		mixed = uint8_t(mixed + state[index] + key[index % key.size()]);
		std::swap(state[index], state[mixed]);
	}
}

void Rc4::apply(std::span<uint8_t> bytes)
{
	for (auto &byte : bytes) {
		i = uint8_t(i + 1);
		j = uint8_t(j + state[i]);
		std::swap(state[i], state[j]);
		byte ^= state[uint8_t(state[i] + state[j])];
	}
}

Key nt_hash(std::u16string_view password)
{
	return md4(utf16(password));
}

Key response_key(const Key &hash, std::u16string_view user, std::u16string_view domain)
{
	std::u16string text;
	for (const char16_t character : user)
		text.push_back(upper(character));
	text += domain;
	return hmac_md5(hash, utf16(text));
}

std::vector<uint8_t> Session::negotiate()
{
	Writer message(40, type_negotiate);
	put32(message.bytes, 12, supported);
	put32(message.bytes, 20, 40);
	put32(message.bytes, 28, 40);
	std::memcpy(message.bytes.data() + 32, version, 8);
	return message.bytes;
}

std::optional<std::vector<uint8_t>> Session::authenticate(std::span<const uint8_t> challenge, const Identity &identity, uint64_t time,
	const std::array<uint8_t, 8> &client_challenge, const Key &session_key)
{
	const Reader reader{challenge};
	if (server || ready || !reader.valid(type_challenge, 48))
		return std::nullopt;
	negotiated = get32(challenge, 20);
	const auto info = reader.field(40);
	if ((negotiated & required) != required || !info)
		return std::nullopt;
	std::copy_n(challenge.begin() + 24, 8, server_nonce.begin());
	// The server's clock, when it sent one, is the time of the response; the LM response is then left empty.
	if (const auto stamp = find_av(*info, av_timestamp); stamp && stamp->size() == 8) {
		time = 0;
		for (int byte = 0; byte < 8; ++byte)
			time |= uint64_t((*stamp)[byte]) << (8 * byte);
	}
	std::vector<uint8_t> temp = {1, 1, 0, 0, 0, 0, 0, 0};
	for (int byte = 0; byte < 8; ++byte)
		temp.push_back(uint8_t(time >> (8 * byte)));
	temp.insert(temp.end(), client_challenge.begin(), client_challenge.end());
	temp.insert(temp.end(), 4, 0);
	temp.insert(temp.end(), info->begin(), info->end());
	temp.insert(temp.end(), 4, 0);
	const Key key = response_key(identity.hash, identity.user, identity.domain);
	std::vector<uint8_t> proven(server_nonce.begin(), server_nonce.end());
	proven.insert(proven.end(), temp.begin(), temp.end());
	const Key proof = hmac_md5(key, proven);
	std::vector<uint8_t> response(proof.begin(), proof.end());
	response.insert(response.end(), temp.begin(), temp.end());
	const Key base = hmac_md5(key, proof);
	std::vector<uint8_t> encrypted_key;
	if (negotiated & flag_key_exchange) {
		exported = session_key;
		encrypted_key.assign(session_key.begin(), session_key.end());
		Rc4(base).apply(encrypted_key);
	} else
		exported = base;
	Writer message(72, type_authenticate);
	const std::vector<uint8_t> lm(24, 0);
	message.field(12, lm);
	message.field(20, response);
	message.field(28, utf16(identity.domain));
	message.field(36, utf16(identity.user));
	message.field(44, {});
	message.field(52, encrypted_key);
	put32(message.bytes, 60, negotiated);
	std::memcpy(message.bytes.data() + 64, version, 8);
	account = identity.user;
	account_domain = identity.domain;
	derive_keys();
	return message.bytes;
}

std::optional<std::vector<uint8_t>> Session::challenge(std::span<const uint8_t> negotiate, std::u16string_view computer,
	const std::array<uint8_t, 8> &server_challenge, uint64_t time)
{
	const Reader reader{negotiate};
	if (!server || ready || !reader.valid(type_negotiate, 16))
		return std::nullopt;
	const uint32_t offered = get32(negotiate, 12);
	if ((offered & required) != required)
		return std::nullopt;
	negotiated = (offered & supported) | flag_target_info | flag_target_type_server;
	server_nonce = server_challenge;
	const auto name = utf16(computer);
	std::vector<uint8_t> stamp(8);
	for (int byte = 0; byte < 8; ++byte)
		stamp[byte] = uint8_t(time >> (8 * byte));
	std::vector<uint8_t> info;
	for (const auto &pair : {av_pair(av_domain, name), av_pair(av_computer, name), av_pair(av_dns_domain, name),
			 av_pair(av_dns_computer, name), av_pair(av_timestamp, stamp), av_pair(av_end, {})})
		info.insert(info.end(), pair.begin(), pair.end());
	Writer message(56, type_challenge);
	message.field(12, name);
	put32(message.bytes, 20, negotiated);
	std::copy(server_challenge.begin(), server_challenge.end(), message.bytes.begin() + 24);
	message.field(40, info);
	std::memcpy(message.bytes.data() + 48, version, 8);
	return message.bytes;
}

bool Session::accept(std::span<const uint8_t> authenticate,
	const std::function<std::optional<Key>(std::u16string_view user, std::u16string_view domain)> &lookup)
{
	const Reader reader{authenticate};
	if (!server || ready || !negotiated || !reader.valid(type_authenticate, 64))
		return false;
	const auto response = reader.field(20), domain = reader.field(28), user = reader.field(36), encrypted_key = reader.field(52);
	if (!response || !domain || !user || !encrypted_key || response->size() < 16 + 28)
		return false;
	const auto name = from_utf16(*user), realm = from_utf16(*domain);
	const auto hash = lookup(name, realm);
	if (!hash)
		return false;
	const Key key = response_key(*hash, name, realm);
	std::vector<uint8_t> proven(server_nonce.begin(), server_nonce.end());
	proven.insert(proven.end(), response->begin() + 16, response->end());
	const Key proof = hmac_md5(key, proven);
	if (!std::equal(proof.begin(), proof.end(), response->begin()))
		return false;
	const Key base = hmac_md5(key, proof);
	if (negotiated & flag_key_exchange) {
		if (encrypted_key->size() != 16)
			return false;
		std::copy(encrypted_key->begin(), encrypted_key->end(), exported.begin());
		Rc4(base).apply(exported);
	} else
		exported = base;
	account = name;
	account_domain = realm;
	derive_keys();
	return true;
}

void Session::derive_keys()
{
	static constexpr const char *client_signing = "session key to client-to-server signing key magic constant";
	static constexpr const char *server_signing = "session key to server-to-client signing key magic constant";
	static constexpr const char *client_sealing = "session key to client-to-server sealing key magic constant";
	static constexpr const char *server_sealing = "session key to server-to-client sealing key magic constant";
	const Key client_seal = derived(exported, client_sealing), server_seal = derived(exported, server_sealing);
	Direction &client = server ? incoming : outgoing, &host = server ? outgoing : incoming;
	client.signing = derived(exported, client_signing);
	host.signing = derived(exported, server_signing);
	client.sealing = Rc4(client_seal);
	host.sealing = Rc4(server_seal);
	ready = true;
}

std::array<uint8_t, 16> Session::mac(Direction &direction, const std::vector<std::span<uint8_t>> &parts)
{
	std::vector<uint8_t> signed_bytes(4);
	put32(signed_bytes, 0, direction.sequence);
	for (const auto &part : parts)
		signed_bytes.insert(signed_bytes.end(), part.begin(), part.end());
	const Key digest = hmac_md5(direction.signing, signed_bytes);
	std::array<uint8_t, 16> result{1, 0, 0, 0};
	std::copy_n(digest.begin(), 8, result.begin() + 4);
	if (negotiated & flag_key_exchange)
		direction.sealing.apply({result.data() + 4, 8});
	for (int byte = 0; byte < 4; ++byte)
		result[12 + byte] = uint8_t(direction.sequence >> (8 * byte));
	++direction.sequence;
	return result;
}

std::array<uint8_t, 16> Session::sign(const std::vector<std::span<uint8_t>> &parts, bool seal)
{
	if (!seal)
		return mac(outgoing, parts);
	// The signature covers the message as it was; the stream that seals it goes on to seal the checksum.
	std::vector<std::vector<uint8_t>> plain;
	for (const auto &part : parts)
		plain.emplace_back(part.begin(), part.end());
	for (const auto &part : parts)
		outgoing.sealing.apply(part);
	std::vector<std::span<uint8_t>> views(plain.begin(), plain.end());
	return mac(outgoing, views);
}

bool Session::verify(const std::vector<std::span<uint8_t>> &parts, std::span<const uint8_t> signature, bool sealed)
{
	if (signature.size() != 16)
		return false;
	if (sealed)
		for (const auto &part : parts)
			incoming.sealing.apply(part);
	const auto expected = mac(incoming, parts);
	return std::equal(expected.begin(), expected.end(), signature.begin());
}

}
