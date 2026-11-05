#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace raven::ntlm {

// The NTLM authentication protocol of [MS-NLMP], version 2 responses with extended session security: the three messages, the keys
// they agree on and the signing and sealing of the messages that follow. Nothing here draws random numbers or reads the clock; the
// caller hands both in, so the exchange is the same every time it is given the same values.

using Key = std::array<uint8_t, 16>;

Key md4(std::span<const uint8_t> bytes);
Key md5(std::span<const uint8_t> bytes);
Key hmac_md5(std::span<const uint8_t> key, std::span<const uint8_t> bytes);

// RC4 keeps its state from one message to the next, as the sealing of a session does.
class Rc4 {
public:
	Rc4() = default;
	explicit Rc4(std::span<const uint8_t> key);
	void apply(std::span<uint8_t> bytes);

private:
	std::array<uint8_t, 256> state{};
	uint8_t i = 0, j = 0;
};

// NTOWFv1: the MD4 of the password in UTF-16; NTOWFv2: the HMAC-MD5 under it of the user name in upper case and the domain.
Key nt_hash(std::u16string_view password);
Key response_key(const Key &hash, std::u16string_view user, std::u16string_view domain);

// The account a client authenticates as.
struct Identity {
	std::u16string user, domain;
	Key hash{};
};

// Flags of the negotiation, as the messages carry them.
constexpr uint32_t flag_unicode = 0x1, flag_request_target = 0x4, flag_sign = 0x10, flag_seal = 0x20, flag_ntlm = 0x200,
	flag_always_sign = 0x8000, flag_target_type_domain = 0x10000, flag_extended_session = 0x80000, flag_target_info = 0x800000,
	flag_version = 0x2000000, flag_128 = 0x20000000, flag_key_exchange = 0x40000000, flag_56 = 0x80000000;

class Session {
public:
	explicit Session(bool server) : server(server) {}

	// The client's side: its first message, then its answer to the server's challenge. The answer fails on a message it cannot read or
	// one that asks for less than version 2 responses with extended session security and 128-bit keys.
	std::vector<uint8_t> negotiate();
	std::optional<std::vector<uint8_t>> authenticate(std::span<const uint8_t> challenge, const Identity &identity, uint64_t time,
		const std::array<uint8_t, 8> &client_challenge, const Key &session_key);

	// The server's side: its challenge to a client's first message, then its verdict on the answer. `lookup` gives the hash of an
	// account's password, or nothing for an account it does not know.
	std::optional<std::vector<uint8_t>> challenge(std::span<const uint8_t> negotiate, std::u16string_view computer,
		const std::array<uint8_t, 8> &server_challenge, uint64_t time);
	bool accept(std::span<const uint8_t> authenticate,
		const std::function<std::optional<Key>(std::u16string_view user, std::u16string_view domain)> &lookup);

	bool established() const { return ready; }
	uint32_t flags() const { return negotiated; }
	const std::u16string &user() const { return account; }
	const std::u16string &domain() const { return account_domain; }
	const Key &session_key() const { return exported; }

	// Messages after the exchange: a signature of sixteen bytes over the parts in order, with the parts sealed in place first when
	// asked. What comes in is checked, and opened in place first when it was sealed.
	std::array<uint8_t, 16> sign(const std::vector<std::span<uint8_t>> &parts, bool seal);
	bool verify(const std::vector<std::span<uint8_t>> &parts, std::span<const uint8_t> signature, bool sealed);

private:
	struct Direction {
		Key signing{};
		Rc4 sealing;
		uint32_t sequence = 0;
	};

	void derive_keys();
	std::array<uint8_t, 16> mac(Direction &direction, const std::vector<std::span<uint8_t>> &parts);

	bool server, ready = false;
	uint32_t negotiated = 0;
	std::array<uint8_t, 8> server_nonce{};
	std::u16string account, account_domain;
	Key exported{};
	Direction outgoing, incoming;
};

}
