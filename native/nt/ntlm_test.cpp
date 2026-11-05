#include "ntlm.h"

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

static void check(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

static std::string hex(std::span<const uint8_t> bytes)
{
	std::string text;
	char digits[3];
	for (const auto byte : bytes) {
		std::snprintf(digits, sizeof(digits), "%02x", byte);
		text += digits;
	}
	return text;
}

static std::span<const uint8_t> ascii(const char *text)
{
	return {reinterpret_cast<const uint8_t *>(text), std::char_traits<char>::length(text)};
}

int main()
{
	try {
		using namespace raven::ntlm;
		// RFC 1320, appendix A.5.
		check(hex(md4({})) == "31d6cfe0d16ae931b73c59d7e0c089c0" && hex(md4(ascii("abc"))) == "a448017aaf21d8525fc10ae87aa6729d" &&
			hex(md4(ascii("message digest"))) == "d9130a8164549fe818874806e1c7014b" &&
			hex(md4(ascii("12345678901234567890123456789012345678901234567890123456789012345678901234567890"))) ==
				"e33b4ddc9c38f2199c3e7b164fcc0536", "MD4 differs from RFC 1320");
		// [MS-NLMP] 4.2.2.1.2 and 4.2.4.1.1: the user "User" of the domain "Domain" with the password "Password".
		const Key hash = nt_hash(u"Password");
		check(hex(hash) == "a4f49c406510bdcab6824ee7c30fd852", "NTOWFv1 differs from MS-NLMP");
		check(hex(response_key(hash, u"User", u"Domain")) == "0c868a403bfd7a93a3001ef22ef02e3f", "NTOWFv2 differs from MS-NLMP");
		std::vector<uint8_t> plain(ascii("Plaintext").begin(), ascii("Plaintext").end());
		Rc4(ascii("Key")).apply(plain);
		check(hex(plain) == "bbf316e8d940af0ad3", "RC4 differs from its reference");

		// A whole exchange: the server knows the account, the client proves it, and both end with the same keys.
		const Identity identity{u"mirko", u"RAVEN", hash};
		const auto lookup = [&](std::u16string_view user, std::u16string_view domain) -> std::optional<Key> {
			if (user == u"mirko" && domain == u"RAVEN")
				return hash;
			return std::nullopt;
		};
		Session client(false), server(true);
		const auto first = client.negotiate();
		const auto challenge = server.challenge(first, u"RAVEN", {1, 2, 3, 4, 5, 6, 7, 8}, 0x01d9000000000000ULL);
		check(challenge.has_value(), "server refused the client's first message");
		const auto answer = client.authenticate(*challenge, identity, 0, {9, 9, 9, 9, 9, 9, 9, 9}, Key{7, 7, 7});
		check(answer && client.established() && !server.established(), "client did not answer the challenge");
		check(server.accept(*answer, lookup) && server.established(), "server refused a correct answer");
		check(server.user() == u"mirko" && server.domain() == u"RAVEN" && client.session_key() == server.session_key() &&
			client.session_key() == Key{7, 7, 7} && (server.flags() & flag_key_exchange), "the two sides agree on different sessions");

		// Sealed both ways, several messages in a row, and a message in two parts.
		for (int round = 0; round < 3; ++round) {
			std::vector<uint8_t> request(ascii("<Discover/>").begin(), ascii("<Discover/>").end()), original = request;
			const auto tag = client.sign({request}, true);
			check(request != original, "sealing left the message as it was");
			check(server.verify({request}, tag, true) && request == original, "server could not open a sealed message");
			std::vector<uint8_t> head = {1, 2, 3}, tail = {4, 5, 6, 7};
			const auto reply = server.sign({head, tail}, true);
			check(client.verify({head, tail}, reply, true) && head == std::vector<uint8_t>{1, 2, 3} &&
				tail == std::vector<uint8_t>{4, 5, 6, 7}, "client could not open a sealed reply in two parts");
		}
		// Signed only, then a message changed on the way, then one played again.
		std::vector<uint8_t> message = {42, 43, 44};
		const auto tag = client.sign({message}, false);
		check(message == std::vector<uint8_t>{42, 43, 44} && server.verify({message}, tag, false), "signed message did not verify");
		const auto second = client.sign({message}, false);
		message[1] ^= 1;
		check(!server.verify({message}, second, false), "a changed message verified");
		message[1] ^= 1;
		check(!server.verify({message}, tag, false), "a message played again verified");

		// A wrong password, an unknown account and a broken message are refused.
		Session other(false), guard(true);
		const auto offered = guard.challenge(other.negotiate(), u"RAVEN", {8, 7, 6, 5, 4, 3, 2, 1}, 1);
		const auto wrong = other.authenticate(*offered, {u"mirko", u"RAVEN", nt_hash(u"secret")}, 0, {}, {});
		check(wrong && !guard.accept(*wrong, lookup) && !guard.established(), "a wrong password was accepted");
		Session stranger(false), keeper(true);
		const auto asked = keeper.challenge(stranger.negotiate(), u"RAVEN", {}, 1);
		const auto unknown = stranger.authenticate(*asked, {u"nobody", u"RAVEN", hash}, 0, {}, {});
		check(unknown && !keeper.accept(*unknown, lookup), "an unknown account was accepted");
		std::vector<uint8_t> broken = first;
		broken[8] = 9;
		check(!Session(true).challenge(broken, u"RAVEN", {}, 1) && !Session(false).authenticate(first, identity, 0, {}, {}),
			"a message of the wrong type was read");
		std::cout << "NTLM digests, responses, exchange, sealing and refusals passed\n";
		return 0;
	} catch (const std::exception &failure) {
		std::cerr << failure.what() << '\n';
		return 1;
	}
}
