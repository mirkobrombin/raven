// Windows.Security.Cryptography as an app uses it: CryptographicBuffer's conversions to and from hexadecimal, Base64 and text, its
// random bytes and comparisons, hashes of known inputs in one call and in pieces, and an HMAC signed and verified with the published
// test vectors (FIPS 180-2 for SHA-256 of "abc", RFC 4231 case 2 for HMAC-SHA256), and AES in ECB, CBC with PKCS #7 and GCM
// (FIPS 197 appendix C.1, and test case 2 of the GCM specification). The exit code names the first check that failed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

typedef struct object object;
struct object {
	void **table;
};
#define CALL(self, place, type) ((type)(self)->table[place])

typedef ULONG (WINAPI *release_method)(object *);
typedef HRESULT (WINAPI *object_method)(object *, object **);
typedef HRESULT (WINAPI *string_method)(object *, HSTRING *);
typedef HRESULT (WINAPI *string_object_method)(object *, HSTRING, object **);
typedef HRESULT (WINAPI *object_string_method)(object *, object *, HSTRING *);
typedef HRESULT (WINAPI *number_method)(object *, UINT32 *);
typedef HRESULT (WINAPI *put_object_method)(object *, object *);
typedef HRESULT (WINAPI *object_object_method)(object *, object *, object **);

static const GUID buffer_statics = {0x320b7e22, 0x3cb0, 0x4cdf, {0x86, 0x63, 0x1d, 0x28, 0x91, 0x00, 0x65, 0xeb}};
static const GUID hash_statics = {0x9fac9741, 0x5cc4, 0x4336, {0xae, 0x38, 0x62, 0x12, 0xb7, 0x5a, 0x91, 0x5a}};
static const GUID hash_names = {0x6b5e0516, 0xde96, 0x4f0a, {0x8d, 0x57, 0xdc, 0xc9, 0xda, 0xe3, 0x6c, 0x76}};
static const GUID mac_statics = {0xc9bdc147, 0xcc77, 0x4df0, {0x9e, 0x4e, 0xb9, 0x21, 0xe0, 0x80, 0x64, 0x4c}};
static const GUID mac_names = {0x41412678, 0xfb1e, 0x43a4, {0x89, 0x5e, 0xa9, 0x02, 0x6e, 0x43, 0x90, 0xa3}};
static const GUID mac_provider2 = {0x6da32a15, 0xd931, 0x42ed, {0x8e, 0x7e, 0xc3, 0x01, 0xca, 0xee, 0x11, 0x9c}};
static const GUID cipher_statics = {0x8d3b2326, 0x1f37, 0x491f, {0xb6, 0x0e, 0xf5, 0x43, 0x1b, 0x26, 0xb4, 0x83}};
static const GUID cipher_names = {0x6870727b, 0xc996, 0x4eae, {0x84, 0xd7, 0x79, 0xb2, 0xae, 0xb7, 0x3b, 0x9c}};
static const GUID engine_statics = {0x9fea0639, 0x6ff7, 0x4c85, {0xa0, 0x95, 0x95, 0xeb, 0x31, 0x71, 0x5e, 0xb9}};

struct reference {
	UINT32 flags, length, unused[2];
	const WCHAR *text;
};

static object *buffers;

static object *factory(const WCHAR *name, const GUID *iid)
{
	struct reference reference = {1, (UINT32)lstrlenW(name), {0, 0}, name};
	object *out = NULL;
	return RoGetActivationFactory((HSTRING)&reference, iid, (void **)&out) == S_OK ? out : NULL;
}

static void release(object *self)
{
	if (self)
		CALL(self, 2, release_method)(self);
}

static HSTRING text(const WCHAR *value)
{
	HSTRING made = NULL;
	WindowsCreateString(value, lstrlenW(value), &made);
	return made;
}

static BOOL same(HSTRING value, const WCHAR *expected)
{
	const BOOL equal = !lstrcmpW(WindowsGetStringRawBuffer(value, NULL), expected);
	WindowsDeleteString(value);
	return equal;
}

/* A buffer of text in UTF-8, and the hexadecimal of a buffer. */
static object *utf8(const WCHAR *value)
{
	object *made = NULL;
	HSTRING string = text(value);
	CALL(buffers, 15, HRESULT (WINAPI *)(object *, HSTRING, INT32, object **))(buffers, string, 0, &made);
	WindowsDeleteString(string);
	return made;
}

static BOOL hex_is(object *buffer, const WCHAR *expected)
{
	HSTRING value = NULL;
	return buffer && CALL(buffers, 12, object_string_method)(buffers, buffer, &value) == S_OK && same(value, expected);
}

static int check_buffers(void)
{
	object *made = NULL, *other = NULL;
	HSTRING value = NULL;
	/* Hexadecimal, read in either case, written in lowercase; odd lengths and other characters are refused. */
	HSTRING hex = text(L"00Ff7a");
	if (CALL(buffers, 11, string_object_method)(buffers, hex, &made) != S_OK || !hex_is(made, L"00ff7a")) return 10;
	WindowsDeleteString(hex);
	release(made);
	hex = text(L"abc");
	if (CALL(buffers, 11, string_object_method)(buffers, hex, &made) == S_OK || made) return 11;
	WindowsDeleteString(hex);
	hex = text(L"zz");
	if (CALL(buffers, 11, string_object_method)(buffers, hex, &made) == S_OK || made) return 12;
	WindowsDeleteString(hex);
	/* Base64 with its padding, both ways. */
	static const WCHAR *const plain[] = {L"", L"f", L"fo", L"foo", L"foob", L"fooba", L"foobar"};
	static const WCHAR *const coded[] = {L"", L"Zg==", L"Zm8=", L"Zm9v", L"Zm9vYg==", L"Zm9vYmE=", L"Zm9vYmFy"};
	for (int at = 0; at < 7; ++at) {
		object *bytes = utf8(plain[at]);
		if (!bytes || CALL(buffers, 14, object_string_method)(buffers, bytes, &value) != S_OK || !same(value, coded[at])) return 13;
		HSTRING back = text(coded[at]);
		if (CALL(buffers, 13, string_object_method)(buffers, back, &other) != S_OK) return 14;
		WindowsDeleteString(back);
		boolean equal = FALSE;
		if (CALL(buffers, 6, HRESULT (WINAPI *)(object *, object *, object *, boolean *))(buffers, bytes, other, &equal) != S_OK || !equal)
			return 15;
		release(bytes);
		release(other);
	}
	HSTRING broken = text(L"Zm9=v");
	if (CALL(buffers, 13, string_object_method)(buffers, broken, &other) == S_OK || other) return 16;
	WindowsDeleteString(broken);
	/* Text: UTF-8, UTF-16LE and UTF-16BE, and back; bytes that are no UTF-8 are refused. */
	object *encoded = utf8(L"h\x00e9");
	if (!hex_is(encoded, L"68c3a9")) return 17;
	if (CALL(buffers, 16, HRESULT (WINAPI *)(object *, INT32, object *, HSTRING *))(buffers, 0, encoded, &value) != S_OK || !same(value, L"h\x00e9"))
		return 18;
	release(encoded);
	HSTRING wide = text(L"A\x0100");
	if (CALL(buffers, 15, HRESULT (WINAPI *)(object *, HSTRING, INT32, object **))(buffers, wide, 2, &encoded) != S_OK ||
		!hex_is(encoded, L"00410100")) return 19;
	if (CALL(buffers, 16, HRESULT (WINAPI *)(object *, INT32, object *, HSTRING *))(buffers, 2, encoded, &value) != S_OK || !same(value, L"A\x0100"))
		return 20;
	release(encoded);
	WindowsDeleteString(wide);
	static const BYTE invalid[] = {0xc3, 0x28};
	if (CALL(buffers, 9, HRESULT (WINAPI *)(object *, UINT32, const BYTE *, object **))(buffers, 2, invalid, &encoded) != S_OK) return 21;
	if (CALL(buffers, 16, HRESULT (WINAPI *)(object *, INT32, object *, HSTRING *))(buffers, 0, encoded, &value) == S_OK) return 22;
	/* Back to a byte array. */
	UINT32 length = 0;
	BYTE *array = NULL;
	if (CALL(buffers, 10, HRESULT (WINAPI *)(object *, object *, UINT32 *, BYTE **))(buffers, encoded, &length, &array) != S_OK ||
		length != 2 || array[0] != 0xc3 || array[1] != 0x28) return 23;
	CoTaskMemFree(array);
	release(encoded);
	/* Random bytes of the length asked for, which differ between two draws. */
	object *first = NULL, *second = NULL;
	boolean equal = TRUE;
	if (CALL(buffers, 7, HRESULT (WINAPI *)(object *, UINT32, object **))(buffers, 32, &first) != S_OK ||
		CALL(buffers, 7, HRESULT (WINAPI *)(object *, UINT32, object **))(buffers, 32, &second) != S_OK ||
		CALL(buffers, 6, HRESULT (WINAPI *)(object *, object *, object *, boolean *))(buffers, first, second, &equal) != S_OK || equal) return 24;
	release(first);
	release(second);
	return 0;
}

static int check_hashes(void)
{
	object *names = factory(L"Windows.Security.Cryptography.Core.HashAlgorithmNames", &hash_names);
	object *providers = factory(L"Windows.Security.Cryptography.Core.HashAlgorithmProvider", &hash_statics), *provider = NULL, *hash = NULL,
		*value = NULL;
	HSTRING name = NULL;
	if (!names || !providers) return 30;
	if (CALL(names, 8, string_method)(names, &name) != S_OK) return 31;
	if (CALL(providers, 6, string_object_method)(providers, name, &provider) != S_OK || !provider) return 32;
	WindowsDeleteString(name);
	UINT32 length = 0;
	if (CALL(provider, 6, string_method)(provider, &name) != S_OK || !same(name, L"SHA256") ||
		CALL(provider, 7, number_method)(provider, &length) != S_OK || length != 32) return 33;
	object *abc = utf8(L"abc");
	if (CALL(provider, 8, object_object_method)(provider, abc, &value) != S_OK ||
		!hex_is(value, L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) return 34;
	release(value);
	/* In pieces, twice: the hash starts over after giving its value. */
	object *a = utf8(L"a"), *bc = utf8(L"bc");
	if (CALL(provider, 9, object_method)(provider, &hash) != S_OK || !hash) return 35;
	for (int round = 0; round < 2; ++round) {
		if (CALL(hash, 6, put_object_method)(hash, a) != S_OK || CALL(hash, 6, put_object_method)(hash, bc) != S_OK ||
			CALL(hash, 7, object_method)(hash, &value) != S_OK ||
			!hex_is(value, L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) return 36;
		release(value);
	}
	release(hash);
	release(provider);
	/* MD5 of "abc", and a name that is no algorithm. */
	CALL(names, 6, string_method)(names, &name);
	if (CALL(providers, 6, string_object_method)(providers, name, &provider) != S_OK ||
		CALL(provider, 8, object_object_method)(provider, abc, &value) != S_OK || !hex_is(value, L"900150983cd24fb0d6963f7d28e17f72")) return 37;
	WindowsDeleteString(name);
	release(value);
	release(provider);
	name = text(L"SHA3");
	if (CALL(providers, 6, string_object_method)(providers, name, &provider) == S_OK || provider) return 38;
	WindowsDeleteString(name);
	release(a);
	release(bc);
	release(abc);
	release(providers);
	release(names);
	return 0;
}

static int check_mac(void)
{
	object *names = factory(L"Windows.Security.Cryptography.Core.MacAlgorithmNames", &mac_names);
	object *providers = factory(L"Windows.Security.Cryptography.Core.MacAlgorithmProvider", &mac_statics), *provider = NULL, *key = NULL,
		*signature = NULL, *keyed = NULL, *hash = NULL;
	object *engine = factory(L"Windows.Security.Cryptography.Core.CryptographicEngine", &engine_statics);
	HSTRING name = NULL;
	if (!names || !providers || !engine) return 50;
	if (CALL(names, 8, string_method)(names, &name) != S_OK) return 51;
	if (CALL(providers, 6, string_object_method)(providers, name, &provider) != S_OK || !provider) return 52;
	WindowsDeleteString(name);
	/* RFC 4231, test case 2. */
	object *material = utf8(L"Jefe"), *data = utf8(L"what do ya want for nothing?");
	static const WCHAR expected[] = L"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
	UINT32 size = 0;
	if (CALL(provider, 8, object_object_method)(provider, material, &key) != S_OK || !key ||
		CALL(key, 6, number_method)(key, &size) != S_OK || size != 32) return 53;
	if (CALL(engine, 10, HRESULT (WINAPI *)(object *, object *, object *, object **))(engine, key, data, &signature) != S_OK ||
		!hex_is(signature, expected)) return 54;
	boolean good = FALSE;
	if (CALL(engine, 11, HRESULT (WINAPI *)(object *, object *, object *, object *, boolean *))(engine, key, data, signature, &good) != S_OK ||
		!good) return 55;
	if (CALL(engine, 11, HRESULT (WINAPI *)(object *, object *, object *, object *, boolean *))(engine, key, material, signature, &good) != S_OK ||
		good) return 56;
	/* The same through a hash keyed with the material. */
	object *value = NULL;
	if (CALL(provider, 0, HRESULT (WINAPI *)(object *, const GUID *, void **))(provider, &mac_provider2, (void **)&keyed) != S_OK ||
		CALL(keyed, 6, object_object_method)(keyed, material, &hash) != S_OK || CALL(hash, 6, put_object_method)(hash, data) != S_OK ||
		CALL(hash, 7, object_method)(hash, &value) != S_OK || !hex_is(value, expected)) return 57;
	release(value);
	release(hash);
	release(keyed);
	release(signature);
	release(key);
	release(material);
	release(data);
	release(provider);
	release(engine);
	release(providers);
	release(names);
	return 0;
}

static object *from_hex(const WCHAR *value)
{
	object *made = NULL;
	HSTRING string = text(value);
	CALL(buffers, 11, string_object_method)(buffers, string, &made);
	WindowsDeleteString(string);
	return made;
}

typedef HRESULT (WINAPI *crypt_method)(object *, object *, object *, object *, object **);

static int check_ciphers(void)
{
	object *names = factory(L"Windows.Security.Cryptography.Core.SymmetricAlgorithmNames", &cipher_names);
	object *providers = factory(L"Windows.Security.Cryptography.Core.SymmetricKeyAlgorithmProvider", &cipher_statics), *provider = NULL,
		*key = NULL, *sealed = NULL, *out = NULL, *back = NULL;
	object *engine = factory(L"Windows.Security.Cryptography.Core.CryptographicEngine", &engine_statics);
	HSTRING name = NULL;
	if (!names || !providers || !engine) return 70;
	/* AES-128 in ECB over one block. */
	if (CALL(names, 13, string_method)(names, &name) != S_OK) return 71;
	if (CALL(providers, 6, string_object_method)(providers, name, &provider) != S_OK || !provider) return 72;
	WindowsDeleteString(name);
	UINT32 block = 0, size = 0;
	object *material = from_hex(L"000102030405060708090a0b0c0d0e0f"), *plain = from_hex(L"00112233445566778899aabbccddeeff");
	if (CALL(provider, 7, number_method)(provider, &block) != S_OK || block != 16 ||
		CALL(provider, 8, object_object_method)(provider, material, &key) != S_OK || !key ||
		CALL(key, 6, number_method)(key, &size) != S_OK || size != 128) return 73;
	if (CALL(engine, 6, crypt_method)(engine, key, plain, NULL, &out) != S_OK || !hex_is(out, L"69c4e0d86a7b0430d8cdb78070b4c55a")) return 74;
	if (CALL(engine, 7, crypt_method)(engine, key, out, NULL, &back) != S_OK || !hex_is(back, L"00112233445566778899aabbccddeeff")) return 75;
	release(out);
	release(back);
	release(key);
	release(provider);
	/* AES in CBC with PKCS #7: five bytes come back as one padded block and decrypt to the same five. */
	CALL(names, 16, string_method)(names, &name);
	if (CALL(providers, 6, string_object_method)(providers, name, &provider) != S_OK || !provider) return 76;
	WindowsDeleteString(name);
	object *iv = from_hex(L"0f0e0d0c0b0a09080706050403020100"), *hello = utf8(L"hello");
	if (CALL(provider, 8, object_object_method)(provider, material, &key) != S_OK) return 77;
	UINT32 length = 0;
	BYTE *array = NULL;
	if (CALL(engine, 6, crypt_method)(engine, key, hello, iv, &out) != S_OK ||
		CALL(buffers, 10, HRESULT (WINAPI *)(object *, object *, UINT32 *, BYTE **))(buffers, out, &length, &array) != S_OK || length != 16)
		return 78;
	CoTaskMemFree(array);
	if (CALL(engine, 7, crypt_method)(engine, key, out, iv, &back) != S_OK || !hex_is(back, L"68656c6c6f")) return 79;
	release(out);
	release(back);
	release(key);
	release(provider);
	/* AES-GCM with a zero key and nonce over a zero block: the ciphertext and tag the specification gives, and a changed tag refused. */
	CALL(names, 14, string_method)(names, &name);
	if (CALL(providers, 6, string_object_method)(providers, name, &provider) != S_OK || !provider) return 80;
	WindowsDeleteString(name);
	object *zero_key = from_hex(L"00000000000000000000000000000000"), *nonce = from_hex(L"000000000000000000000000");
	if (CALL(provider, 8, object_object_method)(provider, zero_key, &key) != S_OK) return 81;
	if (CALL(engine, 8, HRESULT (WINAPI *)(object *, object *, object *, object *, object *, object **))(engine, key, zero_key, nonce, NULL,
		&sealed) != S_OK || !sealed) return 82;
	object *data = NULL, *tag = NULL;
	if (CALL(sealed, 6, object_method)(sealed, &data) != S_OK || !hex_is(data, L"0388dace60b6a392f328c2b971b2fe78") ||
		CALL(sealed, 7, object_method)(sealed, &tag) != S_OK || !hex_is(tag, L"ab6e47d42cec13bdf53a67b21257bddf")) return 83;
	typedef HRESULT (WINAPI *open_method)(object *, object *, object *, object *, object *, object *, object **);
	if (CALL(engine, 9, open_method)(engine, key, data, nonce, tag, NULL, &back) != S_OK || !hex_is(back, L"00000000000000000000000000000000"))
		return 84;
	release(back);
	back = NULL;
	object *wrong = from_hex(L"ab6e47d42cec13bdf53a67b21257bdde");
	if (CALL(engine, 9, open_method)(engine, key, data, nonce, wrong, NULL, &back) == S_OK || back) return 85;
	release(wrong);
	release(data);
	release(tag);
	release(sealed);
	release(key);
	release(provider);
	/* A name that is not offered. */
	name = text(L"RC4");
	if (CALL(providers, 6, string_object_method)(providers, name, &provider) == S_OK || provider) return 86;
	WindowsDeleteString(name);
	release(zero_key);
	release(nonce);
	release(iv);
	release(hello);
	release(material);
	release(plain);
	release(engine);
	release(providers);
	release(names);
	return 0;
}

int raven_entry(void)
{
	if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return 1;
	if (!(buffers = factory(L"Windows.Security.Cryptography.CryptographicBuffer", &buffer_statics))) return 2;
	int failed = check_buffers();
	if (!failed)
		failed = check_hashes();
	if (!failed)
		failed = check_mac();
	if (!failed)
		failed = check_ciphers();
	return failed;
}
