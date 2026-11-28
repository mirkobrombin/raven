#include "authenticode.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef RAVEN_OPENSSL
#include <openssl/pem.h>
#include <openssl/x509_vfy.h>

// With --anchored, every certificate named (PEM) must lead to a root of the trust anchors on a day it was valid: the code signing CAs
// whose roots Windows carries and host bundles leave out.
static int anchored(int argc, char **argv)
{
	for (int index = 2; index < argc; ++index) {
		X509 *certificate = nullptr;
		if (FILE *input = std::fopen(argv[index], "r")) {
			certificate = PEM_read_X509(input, nullptr, nullptr, nullptr);
			std::fclose(input);
		}
		X509_STORE_CTX *context = X509_STORE_CTX_new();
		if (!certificate || !context || !X509_STORE_CTX_init(context, raven::trust_anchors(), certificate, nullptr)) {
			std::printf("%s: not read\n", argv[index]);
			return 1;
		}
		// A day after it was issued, as for a signature stamped then.
		tm issued{};
		ASN1_TIME_to_tm(X509_get0_notBefore(certificate), &issued);
		X509_STORE_CTX_set_time(context, 0, timegm(&issued) + 86400);
		const bool trusted = X509_verify_cert(context) == 1;
		std::printf("%s: %s\n", argv[index], trusted ? "anchored" : X509_verify_cert_error_string(X509_STORE_CTX_get_error(context)));
		X509_STORE_CTX_free(context);
		X509_free(certificate);
		if (!trusted)
			return 1;
	}
	std::printf("PASS: certificates anchored\n");
	return 0;
}
#endif

// Every file named must carry a trusted Authenticode signature, and a copy of the first with one byte of its content changed must not.
int main(int argc, char **argv)
{
#ifdef RAVEN_OPENSSL
	if (argc > 2 && !std::strcmp(argv[1], "--anchored"))
		return anchored(argc, argv);
#endif
	if (argc < 2) {
		std::fprintf(stderr, "usage: authenticode_test SIGNED_FILE... | --anchored CERTIFICATE.pem...\n");
		return 2;
	}
	std::vector<uint8_t> first;
	for (int index = 1; index < argc; ++index) {
		std::ifstream input(argv[index], std::ios::binary);
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		const auto result = raven::verify_authenticode(bytes);
		std::printf("%s: status %08x, %zu certificates, %s\n", argv[index], result.status, result.chain.size(), result.anchored ? "anchored" : "not anchored");
		if (result.status || !result.anchored)
			return 1;
		if (index == 1)
			first = bytes;
	}
	// A byte in the middle of the file: content the digest covers.
	first[first.size() / 2] ^= 0x55;
	const auto tampered = raven::verify_authenticode(first);
	std::printf("changed copy: status %08x\n", tampered.status);
	if (tampered.status != 0x80096010)
		return 1;
	std::printf("PASS: signatures verified and a changed file refused\n");
	return 0;
}
