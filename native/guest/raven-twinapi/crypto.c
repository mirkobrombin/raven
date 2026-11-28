/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Windows.Security.Cryptography: CryptographicBuffer, and of Core the hash and MAC algorithms, their names, keys and CryptographicEngine's
 * signing, over bcrypt.
 *
 * - CryptographicBuffer turns buffers to and from hexadecimal (written in lowercase, read in either case), Base64 with padding, and
 *   text in UTF-8, UTF-16LE or UTF-16BE; a string that is not of its form, or bytes that are no text of the encoding, are refused.
 *   Random bytes and numbers come from the system's generator.
 * - HashAlgorithmProvider opens MD5 and SHA1, SHA256, SHA384 and SHA512 by the names HashAlgorithmNames gives; a CryptographicHash
 *   takes data in pieces and starts over each time it gives its value.
 * - MacAlgorithmProvider opens the HMAC of the same hashes. Its key signs with CryptographicEngine.Sign and checks a signature with
 *   VerifySignature; CreateHash gives a CryptographicHash keyed with the key material. AES-CMAC is named but not offered, as Raven's
 *   bcrypt has no CMAC, and opening it fails.
 * - SymmetricKeyAlgorithmProvider opens AES, DES and 3DES in CBC or ECB, with or without PKCS #7 padding, and AES-GCM; its keys encrypt
 *   and decrypt with CryptographicEngine, AES-GCM through EncryptAndAuthenticate and DecryptAndAuthenticate with a sixteen-byte tag. */
#include "object.h"
#include "../raven-winrt/winrt.h"
#include <bcrypt.h>

static const IID iid_buffer_statics = {0x320b7e22, 0x3cb0, 0x4cdf, {0x86, 0x63, 0x1d, 0x28, 0x91, 0x00, 0x65, 0xeb}};
static const IID iid_hash_provider = {0xbe9b3080, 0xb2c3, 0x422b, {0xbc, 0xe1, 0xec, 0x90, 0xef, 0xb5, 0xd7, 0xb5}};
static const IID iid_hash_provider_statics = {0x9fac9741, 0x5cc4, 0x4336, {0xae, 0x38, 0x62, 0x12, 0xb7, 0x5a, 0x91, 0x5a}};
static const IID iid_hash_computation = {0x5904d1b6, 0xad31, 0x4603, {0xa3, 0xa4, 0xb1, 0xbd, 0xa9, 0x8e, 0x25, 0x62}};
static const IID iid_hash_names = {0x6b5e0516, 0xde96, 0x4f0a, {0x8d, 0x57, 0xdc, 0xc9, 0xda, 0xe3, 0x6c, 0x76}};
static const IID iid_mac_provider = {0x4a3fc5c3, 0x1cbd, 0x41ce, {0xa0, 0x92, 0xaa, 0x0b, 0xc5, 0xd2, 0xd2, 0xf5}};
static const IID iid_mac_provider2 = {0x6da32a15, 0xd931, 0x42ed, {0x8e, 0x7e, 0xc3, 0x01, 0xca, 0xee, 0x11, 0x9c}};
static const IID iid_mac_provider_statics = {0xc9bdc147, 0xcc77, 0x4df0, {0x9e, 0x4e, 0xb9, 0x21, 0xe0, 0x80, 0x64, 0x4c}};
static const IID iid_mac_names = {0x41412678, 0xfb1e, 0x43a4, {0x89, 0x5e, 0xa9, 0x02, 0x6e, 0x43, 0x90, 0xa3}};
static const IID iid_key = {0xed2a3b70, 0x8e7b, 0x4009, {0x84, 0x01, 0xff, 0xd1, 0xa6, 0x2e, 0xeb, 0x27}};
static const IID iid_cipher_provider = {0x3d7e4a33, 0x3bd0, 0x4902, {0x8a, 0xc8, 0x47, 0x0d, 0x50, 0xd2, 0x13, 0x76}};
static const IID iid_cipher_provider_statics = {0x8d3b2326, 0x1f37, 0x491f, {0xb6, 0x0e, 0xf5, 0x43, 0x1b, 0x26, 0xb4, 0x83}};
static const IID iid_cipher_names = {0x6870727b, 0xc996, 0x4eae, {0x84, 0xd7, 0x79, 0xb2, 0xae, 0xb7, 0x3b, 0x9c}};
static const IID iid_sealed = {0x6fa42fe7, 0x1ecb, 0x4b00, {0xbe, 0xa5, 0x60, 0xb8, 0x3f, 0x86, 0x2f, 0x17}};
static const IID iid_engine_statics = {0x9fea0639, 0x6ff7, 0x4c85, {0xa0, 0x95, 0x95, 0xeb, 0x31, 0x71, 0x5e, 0xb9}};

typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer buffer;

/* Bytes that are not of the form asked for: NTE_BAD_DATA, as the system's cryptography answers. */
#define BAD_DATA ((HRESULT)0x80090005)

static HRESULT from_status(NTSTATUS status) { return status >= 0 ? S_OK : HRESULT_FROM_NT(status); }

/* The bytes of a buffer the app gives; none for a null one. */
static HRESULT bytes_of(void *given, BYTE **bytes, UINT32 *length)
{
	*bytes = NULL;
	*length = 0;
	return given ? winrt_buffer_bytes((buffer *)given, bytes, length) : S_OK;
}

static HRESULT new_buffer(const BYTE *bytes, UINT32 length, void **out)
{
	if (!out)
		return E_POINTER;
	return winrt_buffer(bytes, length, length, (buffer **)out);
}

/* ---- CryptographicBuffer ---- */

/* BinaryStringEncoding. */
enum { UTF8, UTF16_LE, UTF16_BE };

static HRESULT WINAPI buffer_compare(void *iface, void *first, void *second, boolean *out)
{
	BYTE *left, *right;
	UINT32 left_length, right_length;
	HRESULT result = bytes_of(first, &left, &left_length);
	if (SUCCEEDED(result))
		result = bytes_of(second, &right, &right_length);
	if (FAILED(result))
		return result;
	BOOL equal = left_length == right_length;
	for (UINT32 at = 0; equal && at < left_length; ++at)
		equal = left[at] == right[at];
	return answer_flag(out, equal);
}

static HRESULT WINAPI buffer_random(void *iface, UINT32 length, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	BYTE *bytes = ALLOC(length ? length : 1);
	if (!bytes)
		return E_OUTOFMEMORY;
	HRESULT result = from_status(BCryptGenRandom(NULL, bytes, length, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
	if (SUCCEEDED(result))
		result = new_buffer(bytes, length, out);
	SecureZeroMemory(bytes, length);
	FREE(bytes);
	return result;
}

static HRESULT WINAPI buffer_random_number(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	return from_status(BCryptGenRandom(NULL, (BYTE *)out, sizeof *out, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

static HRESULT WINAPI buffer_from_array(void *iface, UINT32 length, const BYTE *bytes, void **out)
{
	if (length && !bytes)
		return E_POINTER;
	return new_buffer(bytes, length, out);
}

static HRESULT WINAPI buffer_to_array(void *iface, void *given, UINT32 *length, BYTE **out)
{
	BYTE *bytes;
	UINT32 count;
	if (!length || !out)
		return E_POINTER;
	*length = 0;
	*out = NULL;
	HRESULT result = bytes_of(given, &bytes, &count);
	if (FAILED(result) || !given)
		return result;
	if (!(*out = CoTaskMemAlloc(count ? count : 1)))
		return E_OUTOFMEMORY;
	for (UINT32 at = 0; at < count; ++at)
		(*out)[at] = bytes[at];
	*length = count;
	return S_OK;
}

static int hex_digit(WCHAR c)
{
	if (c >= L'0' && c <= L'9')
		return c - L'0';
	if (c >= L'a' && c <= L'f')
		return c - L'a' + 10;
	if (c >= L'A' && c <= L'F')
		return c - L'A' + 10;
	return -1;
}

static HRESULT WINAPI buffer_from_hex(void *iface, HSTRING text, void **out)
{
	UINT32 length = 0;
	const WCHAR *digits = WindowsGetStringRawBuffer(text, &length);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (length % 2)
		return BAD_DATA;
	BYTE *bytes = ALLOC(length / 2 + 1);
	if (!bytes)
		return E_OUTOFMEMORY;
	HRESULT result = S_OK;
	for (UINT32 at = 0; at < length && SUCCEEDED(result); at += 2) {
		const int high = hex_digit(digits[at]), low = hex_digit(digits[at + 1]);
		if (high < 0 || low < 0)
			result = BAD_DATA;
		else
			bytes[at / 2] = (BYTE)(high << 4 | low);
	}
	if (SUCCEEDED(result))
		result = new_buffer(bytes, length / 2, out);
	FREE(bytes);
	return result;
}

static HRESULT WINAPI buffer_to_hex(void *iface, void *given, HSTRING *out)
{
	static const WCHAR digits[] = L"0123456789abcdef";
	BYTE *bytes;
	UINT32 length;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = bytes_of(given, &bytes, &length);
	if (FAILED(result))
		return result;
	WCHAR *text = ALLOC(((SIZE_T)length * 2 + 1) * sizeof *text);
	if (!text)
		return E_OUTOFMEMORY;
	for (UINT32 at = 0; at < length; ++at) {
		text[at * 2] = digits[bytes[at] >> 4];
		text[at * 2 + 1] = digits[bytes[at] & 15];
	}
	result = WindowsCreateString(text, length * 2, out);
	FREE(text);
	return result;
}

static const char base64_digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_value(WCHAR c)
{
	for (int at = 0; at < 64; ++at)
		if (c == (WCHAR)base64_digits[at])
			return at;
	return -1;
}

static HRESULT WINAPI buffer_from_base64(void *iface, HSTRING text, void **out)
{
	UINT32 length = 0;
	const WCHAR *digits = WindowsGetStringRawBuffer(text, &length);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (length % 4)
		return BAD_DATA;
	BYTE *bytes = ALLOC(length / 4 * 3 + 1);
	if (!bytes)
		return E_OUTOFMEMORY;
	UINT32 count = 0;
	HRESULT result = S_OK;
	for (UINT32 at = 0; at < length && SUCCEEDED(result); at += 4) {
		int values[4], padding = 0;
		for (int place = 0; place < 4; ++place) {
			const WCHAR c = digits[at + place];
			/* Padding only ends the last group, after at least two digits. */
			if (c == L'=' && at + 4 == length && place >= 2 && (place == 3 || digits[at + 3] == L'=')) {
				values[place] = 0;
				++padding;
			} else if (padding || (values[place] = base64_value(c)) < 0) {
				result = BAD_DATA;
				break;
			}
		}
		if (FAILED(result))
			break;
		const UINT32 group = (UINT32)values[0] << 18 | (UINT32)values[1] << 12 | (UINT32)values[2] << 6 | (UINT32)values[3];
		bytes[count++] = (BYTE)(group >> 16);
		if (padding < 2)
			bytes[count++] = (BYTE)(group >> 8);
		if (padding < 1)
			bytes[count++] = (BYTE)group;
	}
	if (SUCCEEDED(result))
		result = new_buffer(bytes, count, out);
	FREE(bytes);
	return result;
}

static HRESULT WINAPI buffer_to_base64(void *iface, void *given, HSTRING *out)
{
	BYTE *bytes;
	UINT32 length;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = bytes_of(given, &bytes, &length);
	if (FAILED(result))
		return result;
	const UINT32 size = (length + 2) / 3 * 4;
	WCHAR *text = ALLOC(((SIZE_T)size + 1) * sizeof *text);
	if (!text)
		return E_OUTOFMEMORY;
	for (UINT32 at = 0, place = 0; at < length; at += 3, place += 4) {
		const UINT32 left = length - at;
		const UINT32 group = (UINT32)bytes[at] << 16 | (left > 1 ? (UINT32)bytes[at + 1] << 8 : 0) | (left > 2 ? bytes[at + 2] : 0);
		text[place] = (WCHAR)base64_digits[group >> 18 & 63];
		text[place + 1] = (WCHAR)base64_digits[group >> 12 & 63];
		text[place + 2] = left > 1 ? (WCHAR)base64_digits[group >> 6 & 63] : L'=';
		text[place + 3] = left > 2 ? (WCHAR)base64_digits[group & 63] : L'=';
	}
	result = WindowsCreateString(text, size, out);
	FREE(text);
	return result;
}

static HRESULT WINAPI buffer_from_string(void *iface, HSTRING text, INT32 encoding, void **out)
{
	UINT32 length = 0;
	const WCHAR *characters = WindowsGetStringRawBuffer(text, &length);
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (encoding == UTF8) {
		const int size = length ? WideCharToMultiByte(CP_UTF8, 0, characters, (int)length, NULL, 0, NULL, NULL) : 0;
		char *bytes = ALLOC(size + 1);
		if (!bytes)
			return E_OUTOFMEMORY;
		if (size)
			WideCharToMultiByte(CP_UTF8, 0, characters, (int)length, bytes, size, NULL, NULL);
		const HRESULT result = new_buffer((const BYTE *)bytes, (UINT32)size, out);
		FREE(bytes);
		return result;
	}
	if (encoding != UTF16_LE && encoding != UTF16_BE)
		return E_INVALIDARG;
	BYTE *bytes = ALLOC((SIZE_T)length * 2 + 1);
	if (!bytes)
		return E_OUTOFMEMORY;
	for (UINT32 at = 0; at < length; ++at) {
		const BYTE low = (BYTE)characters[at], high = (BYTE)(characters[at] >> 8);
		bytes[at * 2] = encoding == UTF16_LE ? low : high;
		bytes[at * 2 + 1] = encoding == UTF16_LE ? high : low;
	}
	const HRESULT result = new_buffer(bytes, length * 2, out);
	FREE(bytes);
	return result;
}

static HRESULT WINAPI buffer_to_string(void *iface, INT32 encoding, void *given, HSTRING *out)
{
	BYTE *bytes;
	UINT32 length;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = bytes_of(given, &bytes, &length);
	if (FAILED(result))
		return result;
	if (encoding == UTF8) {
		if (!length)
			return S_OK;
		const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, (int)length, NULL, 0);
		if (!size)
			return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
		WCHAR *text = ALLOC((SIZE_T)size * sizeof *text);
		if (!text)
			return E_OUTOFMEMORY;
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, (int)length, text, size);
		result = WindowsCreateString(text, (UINT32)size, out);
		FREE(text);
		return result;
	}
	if (encoding != UTF16_LE && encoding != UTF16_BE)
		return E_INVALIDARG;
	if (length % 2)
		return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
	WCHAR *text = ALLOC((SIZE_T)length + sizeof(WCHAR));
	if (!text)
		return E_OUTOFMEMORY;
	for (UINT32 at = 0; at < length / 2; ++at)
		text[at] = encoding == UTF16_LE ? (WCHAR)(bytes[at * 2] | bytes[at * 2 + 1] << 8) : (WCHAR)(bytes[at * 2] << 8 | bytes[at * 2 + 1]);
	result = WindowsCreateString(text, length / 2, out);
	FREE(text);
	return result;
}

TABLE(buffer_factory_table, 0, FACTORY);
TABLE(buffer_statics_table, 1, INSPECTABLE, (const void *)buffer_compare, (const void *)buffer_random, (const void *)buffer_random_number,
	(const void *)buffer_from_array, (const void *)buffer_to_array, (const void *)buffer_from_hex, (const void *)buffer_to_hex,
	(const void *)buffer_from_base64, (const void *)buffer_to_base64, (const void *)buffer_from_string, (const void *)buffer_to_string);
static const struct face_iid buffer_class_iids[] = {{&iid_factory, 0}, {&iid_buffer_statics, 1}, {NULL, 0}};
static const struct kind buffer_class_kind = {"CryptographicBuffer", L"Windows.Security.Cryptography.CryptographicBuffer", buffer_class_iids, NULL};
struct object cryptographic_buffer_class = {{buffer_factory_table.slots, buffer_statics_table.slots}, &buffer_class_kind, 1};

/* ---- the algorithms ---- */

/* Each algorithm by the name the Windows Runtime gives it, the bcrypt hash behind it and whether it is that hash's HMAC. */
struct algorithm {
	const WCHAR *name, *hash;
	BOOL mac;
	UINT32 length;
};

static const struct algorithm algorithms[] = {
	{L"MD5", BCRYPT_MD5_ALGORITHM, FALSE, 16}, {L"SHA1", BCRYPT_SHA1_ALGORITHM, FALSE, 20},
	{L"SHA256", BCRYPT_SHA256_ALGORITHM, FALSE, 32}, {L"SHA384", BCRYPT_SHA384_ALGORITHM, FALSE, 48},
	{L"SHA512", BCRYPT_SHA512_ALGORITHM, FALSE, 64},
	{L"HMAC_MD5", BCRYPT_MD5_ALGORITHM, TRUE, 16}, {L"HMAC_SHA1", BCRYPT_SHA1_ALGORITHM, TRUE, 20},
	{L"HMAC_SHA256", BCRYPT_SHA256_ALGORITHM, TRUE, 32}, {L"HMAC_SHA384", BCRYPT_SHA384_ALGORITHM, TRUE, 48},
	{L"HMAC_SHA512", BCRYPT_SHA512_ALGORITHM, TRUE, 64},
};

static const struct algorithm *find_algorithm(HSTRING name, BOOL mac)
{
	const WCHAR *text = WindowsGetStringRawBuffer(name, NULL);
	for (size_t at = 0; at < ARRAYSIZE(algorithms); ++at)
		if (algorithms[at].mac == mac && !lstrcmpiW(text, algorithms[at].name))
			return &algorithms[at];
	return NULL;
}

static HRESULT answer_name(const WCHAR *name, HSTRING *out)
{
	if (!out)
		return E_POINTER;
	return WindowsCreateString(name, (UINT32)lstrlenW(name), out);
}

/* A provider of an algorithm, its open bcrypt handle, and a hash or MAC computation of it with the key's bytes. */
struct provider {
	struct object base;
	const struct algorithm *algorithm;
	BCRYPT_ALG_HANDLE handle;
};

struct computation {
	struct object base;
	struct provider *provider;
	BYTE *key;
	UINT32 key_length;
	BCRYPT_HASH_HANDLE hash;
	SRWLOCK lock;
};

static struct provider *provider_of(void *iface) { return (struct provider *)owner(iface); }
static struct computation *computation_of(void *iface) { return (struct computation *)owner(iface); }

static void destroy_provider(struct object *base)
{
	struct provider *self = (struct provider *)base;
	if (self->handle)
		BCryptCloseAlgorithmProvider(self->handle, 0);
}

static void destroy_computation(struct object *base)
{
	struct computation *self = (struct computation *)base;
	if (self->hash)
		BCryptDestroyHash(self->hash);
	if (self->key) {
		SecureZeroMemory(self->key, self->key_length);
		FREE(self->key);
	}
	release(face_of(&self->provider->base, 0));
}

static HRESULT WINAPI computation_append(void *iface, void *data)
{
	struct computation *self = computation_of(iface);
	BYTE *bytes;
	UINT32 length;
	HRESULT result = bytes_of(data, &bytes, &length);
	if (FAILED(result))
		return result;
	if (!data)
		return E_INVALIDARG;
	AcquireSRWLockExclusive(&self->lock);
	result = from_status(BCryptHashData(self->hash, bytes, length, 0));
	ReleaseSRWLockExclusive(&self->lock);
	return result;
}

/* The value so far, and a fresh computation with the same key after it. */
static HRESULT WINAPI computation_value(void *iface, void **out)
{
	struct computation *self = computation_of(iface);
	BYTE value[64];
	const UINT32 length = self->provider->algorithm->length;
	if (!out)
		return E_POINTER;
	*out = NULL;
	AcquireSRWLockExclusive(&self->lock);
	HRESULT result = from_status(BCryptFinishHash(self->hash, value, length, 0));
	BCryptDestroyHash(self->hash);
	self->hash = NULL;
	const HRESULT again = from_status(BCryptCreateHash(self->provider->handle, &self->hash, NULL, 0, self->key, self->key_length, 0));
	ReleaseSRWLockExclusive(&self->lock);
	if (SUCCEEDED(result))
		result = FAILED(again) ? again : new_buffer(value, length, out);
	return result;
}

TABLE(computation_table, 0, INSPECTABLE, (const void *)computation_append, (const void *)computation_value);
static const struct face_iid computation_iids[] = {{&iid_hash_computation, 0}, {NULL, 0}};
static const struct kind computation_kind = {"CryptographicHash", L"Windows.Security.Cryptography.Core.CryptographicHash", computation_iids,
	destroy_computation};

static HRESULT computation_make(struct provider *provider, const BYTE *key, UINT32 key_length, struct computation **out)
{
	static const void *const *const tables[] = {computation_table.slots};
	struct computation *self = make(sizeof *self, &computation_kind, tables, 1);
	*out = NULL;
	if (!self)
		return E_OUTOFMEMORY;
	InitializeSRWLock(&self->lock);
	reference(face_of(&provider->base, 0));
	self->provider = provider;
	HRESULT result = S_OK;
	if (key_length && !(self->key = ALLOC(key_length)))
		result = E_OUTOFMEMORY;
	for (UINT32 at = 0; SUCCEEDED(result) && at < key_length; ++at)
		self->key[at] = key[at];
	self->key_length = key_length;
	if (SUCCEEDED(result))
		result = from_status(BCryptCreateHash(provider->handle, &self->hash, NULL, 0, self->key, key_length, 0));
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = self;
	return S_OK;
}

struct cipher_provider;

/* A key of a MAC algorithm, with its material kept for signing, or of a symmetric algorithm, with bcrypt's key. */
struct key {
	struct object base;
	struct provider *provider;
	struct cipher_provider *cipher;
	BCRYPT_KEY_HANDLE handle;
	BYTE *material;
	UINT32 length;
};

static struct key *key_of(void *iface) { return (struct key *)owner(iface); }

static void destroy_key(struct object *base)
{
	struct key *self = (struct key *)base;
	if (self->material) {
		SecureZeroMemory(self->material, self->length);
		FREE(self->material);
	}
	if (self->handle)
		BCryptDestroyKey(self->handle);
	release(self->provider ? face_of(&self->provider->base, 0) : face_of((struct object *)self->cipher, 0));
}

static HRESULT WINAPI key_size(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = key_of(iface)->length * 8;
	return S_OK;
}

/* A MAC key has no key pair to export. */
static HRESULT WINAPI key_export(void *iface, void **out)
{
	answer_null(out);
	return NTE_NOT_SUPPORTED;
}

static HRESULT WINAPI key_export_typed(void *iface, INT32 type, void **out) { return key_export(iface, out); }

TABLE(key_table, 0, INSPECTABLE, (const void *)key_size, (const void *)key_export, (const void *)key_export_typed, (const void *)key_export,
	(const void *)key_export_typed);
static const struct face_iid key_iids[] = {{&iid_key, 0}, {NULL, 0}};
static const struct kind key_kind = {"CryptographicKey", L"Windows.Security.Cryptography.Core.CryptographicKey", key_iids, destroy_key};

static HRESULT WINAPI provider_name(void *iface, HSTRING *out) { return answer_name(provider_of(iface)->algorithm->name, out); }

static HRESULT WINAPI provider_length(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = provider_of(iface)->algorithm->length;
	return S_OK;
}

static HRESULT WINAPI provider_hash_data(void *iface, void *data, void **out)
{
	struct computation *computation;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!data)
		return E_INVALIDARG;
	HRESULT result = computation_make(provider_of(iface), NULL, 0, &computation);
	if (FAILED(result))
		return result;
	result = computation_append(face_of(&computation->base, 0), data);
	if (SUCCEEDED(result))
		result = computation_value(face_of(&computation->base, 0), out);
	release(face_of(&computation->base, 0));
	return result;
}

static HRESULT keyed_hash(struct provider *provider, void *material, void **out)
{
	BYTE *bytes;
	UINT32 length;
	struct computation *computation;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = bytes_of(material, &bytes, &length);
	if (SUCCEEDED(result))
		result = computation_make(provider, bytes, length, &computation);
	if (SUCCEEDED(result))
		*out = face_of(&computation->base, 0);
	return result;
}

static HRESULT WINAPI provider_create_hash(void *iface, void **out) { return keyed_hash(provider_of(iface), NULL, out); }

static HRESULT WINAPI provider_create_key(void *iface, void *material, void **out)
{
	static const void *const *const tables[] = {key_table.slots};
	struct provider *provider = provider_of(iface);
	BYTE *bytes;
	UINT32 length;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!material)
		return E_INVALIDARG;
	HRESULT result = bytes_of(material, &bytes, &length);
	if (FAILED(result))
		return result;
	struct key *self = make(sizeof *self, &key_kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	reference(face_of(&provider->base, 0));
	self->provider = provider;
	if (!(self->material = ALLOC(length ? length : 1))) {
		release(face_of(&self->base, 0));
		return E_OUTOFMEMORY;
	}
	for (UINT32 at = 0; at < length; ++at)
		self->material[at] = bytes[at];
	self->length = length;
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI provider_mac_hash(void *iface, void *material, void **out) { return keyed_hash(provider_of(iface), material, out); }

TABLE(hash_provider_table, 0, INSPECTABLE, (const void *)provider_name, (const void *)provider_length, (const void *)provider_hash_data,
	(const void *)provider_create_hash);
static const struct face_iid hash_provider_iids[] = {{&iid_hash_provider, 0}, {NULL, 0}};
static const struct kind hash_provider_kind = {"HashAlgorithmProvider", L"Windows.Security.Cryptography.Core.HashAlgorithmProvider",
	hash_provider_iids, destroy_provider};
TABLE(mac_provider_table, 0, INSPECTABLE, (const void *)provider_name, (const void *)provider_length, (const void *)provider_create_key);
TABLE(mac_provider2_table, 1, INSPECTABLE, (const void *)provider_mac_hash);
static const struct face_iid mac_provider_iids[] = {{&iid_mac_provider, 0}, {&iid_mac_provider2, 1}, {NULL, 0}};
static const struct kind mac_provider_kind = {"MacAlgorithmProvider", L"Windows.Security.Cryptography.Core.MacAlgorithmProvider",
	mac_provider_iids, destroy_provider};

static HRESULT open_provider(HSTRING name, BOOL mac, void **out)
{
	static const void *const *const hash_tables[] = {hash_provider_table.slots};
	static const void *const *const mac_tables[] = {mac_provider_table.slots, mac_provider2_table.slots};
	if (!out)
		return E_POINTER;
	*out = NULL;
	const struct algorithm *algorithm = find_algorithm(name, mac);
	if (!algorithm)
		return NTE_NOT_SUPPORTED;
	struct provider *self = mac ? make(sizeof *self, &mac_provider_kind, mac_tables, 2) : make(sizeof *self, &hash_provider_kind, hash_tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	self->algorithm = algorithm;
	const HRESULT result = from_status(BCryptOpenAlgorithmProvider(&self->handle, algorithm->hash, NULL, mac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0));
	if (FAILED(result)) {
		self->handle = NULL;
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI open_hash(void *iface, HSTRING name, void **out) { return open_provider(name, FALSE, out); }
static HRESULT WINAPI open_mac(void *iface, HSTRING name, void **out) { return open_provider(name, TRUE, out); }

TABLE(hash_factory_table, 0, FACTORY);
TABLE(hash_statics_table, 1, INSPECTABLE, (const void *)open_hash);
static const struct face_iid hash_class_iids[] = {{&iid_factory, 0}, {&iid_hash_provider_statics, 1}, {NULL, 0}};
static const struct kind hash_class_kind = {"HashAlgorithmProvider", L"Windows.Security.Cryptography.Core.HashAlgorithmProvider",
	hash_class_iids, NULL};
struct object hash_provider_class = {{hash_factory_table.slots, hash_statics_table.slots}, &hash_class_kind, 1};

TABLE(mac_factory_table, 0, FACTORY);
TABLE(mac_statics_table, 1, INSPECTABLE, (const void *)open_mac);
static const struct face_iid mac_class_iids[] = {{&iid_factory, 0}, {&iid_mac_provider_statics, 1}, {NULL, 0}};
static const struct kind mac_class_kind = {"MacAlgorithmProvider", L"Windows.Security.Cryptography.Core.MacAlgorithmProvider",
	mac_class_iids, NULL};
struct object mac_provider_class = {{mac_factory_table.slots, mac_statics_table.slots}, &mac_class_kind, 1};

/* ---- the names ---- */

static HRESULT WINAPI name_md5(void *iface, HSTRING *out) { return answer_name(L"MD5", out); }
static HRESULT WINAPI name_sha1(void *iface, HSTRING *out) { return answer_name(L"SHA1", out); }
static HRESULT WINAPI name_sha256(void *iface, HSTRING *out) { return answer_name(L"SHA256", out); }
static HRESULT WINAPI name_sha384(void *iface, HSTRING *out) { return answer_name(L"SHA384", out); }
static HRESULT WINAPI name_sha512(void *iface, HSTRING *out) { return answer_name(L"SHA512", out); }
static HRESULT WINAPI name_hmac_md5(void *iface, HSTRING *out) { return answer_name(L"HMAC_MD5", out); }
static HRESULT WINAPI name_hmac_sha1(void *iface, HSTRING *out) { return answer_name(L"HMAC_SHA1", out); }
static HRESULT WINAPI name_hmac_sha256(void *iface, HSTRING *out) { return answer_name(L"HMAC_SHA256", out); }
static HRESULT WINAPI name_hmac_sha384(void *iface, HSTRING *out) { return answer_name(L"HMAC_SHA384", out); }
static HRESULT WINAPI name_hmac_sha512(void *iface, HSTRING *out) { return answer_name(L"HMAC_SHA512", out); }
static HRESULT WINAPI name_aes_cmac(void *iface, HSTRING *out) { return answer_name(L"AES_CMAC", out); }

TABLE(hash_names_factory_table, 0, FACTORY);
TABLE(hash_names_table, 1, INSPECTABLE, (const void *)name_md5, (const void *)name_sha1, (const void *)name_sha256, (const void *)name_sha384,
	(const void *)name_sha512);
static const struct face_iid hash_names_iids[] = {{&iid_factory, 0}, {&iid_hash_names, 1}, {NULL, 0}};
static const struct kind hash_names_kind = {"HashAlgorithmNames", L"Windows.Security.Cryptography.Core.HashAlgorithmNames", hash_names_iids, NULL};
struct object hash_names_class = {{hash_names_factory_table.slots, hash_names_table.slots}, &hash_names_kind, 1};

TABLE(mac_names_factory_table, 0, FACTORY);
TABLE(mac_names_table, 1, INSPECTABLE, (const void *)name_hmac_md5, (const void *)name_hmac_sha1, (const void *)name_hmac_sha256,
	(const void *)name_hmac_sha384, (const void *)name_hmac_sha512, (const void *)name_aes_cmac);
static const struct face_iid mac_names_iids[] = {{&iid_factory, 0}, {&iid_mac_names, 1}, {NULL, 0}};
static const struct kind mac_names_kind = {"MacAlgorithmNames", L"Windows.Security.Cryptography.Core.MacAlgorithmNames", mac_names_iids, NULL};
struct object mac_names_class = {{mac_names_factory_table.slots, mac_names_table.slots}, &mac_names_kind, 1};

/* ---- the symmetric algorithms ---- */

/* Each symmetric algorithm by its name, the bcrypt cipher and chaining mode behind it, and whether it pads to whole blocks with PKCS #7.
 * RC2, RC4 and AES-CCM are named but not offered: Raven's bcrypt has none of them. */
struct cipher {
	const WCHAR *name, *cipher, *mode;
	UINT32 block;
	BOOL padded, chained, authenticated;
};

static const struct cipher ciphers[] = {
	{L"AES_CBC", BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_CBC, 16, FALSE, TRUE, FALSE},
	{L"AES_ECB", BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_ECB, 16, FALSE, FALSE, FALSE},
	{L"AES_GCM", BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_GCM, 16, FALSE, FALSE, TRUE},
	{L"AES_CBC_PKCS7", BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_CBC, 16, TRUE, TRUE, FALSE},
	{L"AES_ECB_PKCS7", BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_ECB, 16, TRUE, FALSE, FALSE},
	{L"DES_CBC", BCRYPT_DES_ALGORITHM, BCRYPT_CHAIN_MODE_CBC, 8, FALSE, TRUE, FALSE},
	{L"DES_ECB", BCRYPT_DES_ALGORITHM, BCRYPT_CHAIN_MODE_ECB, 8, FALSE, FALSE, FALSE},
	{L"DES_CBC_PKCS7", BCRYPT_DES_ALGORITHM, BCRYPT_CHAIN_MODE_CBC, 8, TRUE, TRUE, FALSE},
	{L"DES_ECB_PKCS7", BCRYPT_DES_ALGORITHM, BCRYPT_CHAIN_MODE_ECB, 8, TRUE, FALSE, FALSE},
	{L"3DES_CBC", BCRYPT_3DES_ALGORITHM, BCRYPT_CHAIN_MODE_CBC, 8, FALSE, TRUE, FALSE},
	{L"3DES_ECB", BCRYPT_3DES_ALGORITHM, BCRYPT_CHAIN_MODE_ECB, 8, FALSE, FALSE, FALSE},
	{L"3DES_CBC_PKCS7", BCRYPT_3DES_ALGORITHM, BCRYPT_CHAIN_MODE_CBC, 8, TRUE, TRUE, FALSE},
	{L"3DES_ECB_PKCS7", BCRYPT_3DES_ALGORITHM, BCRYPT_CHAIN_MODE_ECB, 8, TRUE, FALSE, FALSE},
};

struct cipher_provider {
	struct object base;
	const struct cipher *algorithm;
	BCRYPT_ALG_HANDLE handle;
};

static struct cipher_provider *cipher_provider_of(void *iface) { return (struct cipher_provider *)owner(iface); }

static void destroy_cipher_provider(struct object *base)
{
	struct cipher_provider *self = (struct cipher_provider *)base;
	if (self->handle)
		BCryptCloseAlgorithmProvider(self->handle, 0);
}

static HRESULT WINAPI cipher_name(void *iface, HSTRING *out) { return answer_name(cipher_provider_of(iface)->algorithm->name, out); }

static HRESULT WINAPI cipher_block(void *iface, UINT32 *out)
{
	if (!out)
		return E_POINTER;
	*out = cipher_provider_of(iface)->algorithm->block;
	return S_OK;
}

static HRESULT WINAPI cipher_create_key(void *iface, void *material, void **out)
{
	static const void *const *const tables[] = {key_table.slots};
	struct cipher_provider *provider = cipher_provider_of(iface);
	BYTE *bytes;
	UINT32 length;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!material)
		return E_INVALIDARG;
	HRESULT result = bytes_of(material, &bytes, &length);
	if (FAILED(result))
		return result;
	struct key *self = make(sizeof *self, &key_kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	reference(face_of(&provider->base, 0));
	self->cipher = provider;
	self->length = length;
	result = from_status(BCryptGenerateSymmetricKey(provider->handle, &self->handle, NULL, 0, bytes, length, 0));
	if (FAILED(result)) {
		self->handle = NULL;
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

TABLE(cipher_provider_table, 0, INSPECTABLE, (const void *)cipher_name, (const void *)cipher_block, (const void *)cipher_create_key);
static const struct face_iid cipher_provider_iids[] = {{&iid_cipher_provider, 0}, {NULL, 0}};
static const struct kind cipher_provider_kind = {"SymmetricKeyAlgorithmProvider",
	L"Windows.Security.Cryptography.Core.SymmetricKeyAlgorithmProvider", cipher_provider_iids, destroy_cipher_provider};

static HRESULT WINAPI open_cipher(void *iface, HSTRING name, void **out)
{
	static const void *const *const tables[] = {cipher_provider_table.slots};
	const WCHAR *text = WindowsGetStringRawBuffer(name, NULL);
	const struct cipher *algorithm = NULL;
	if (!out)
		return E_POINTER;
	*out = NULL;
	for (size_t at = 0; at < ARRAYSIZE(ciphers) && !algorithm; ++at)
		if (!lstrcmpiW(text, ciphers[at].name))
			algorithm = &ciphers[at];
	if (!algorithm)
		return NTE_NOT_SUPPORTED;
	struct cipher_provider *self = make(sizeof *self, &cipher_provider_kind, tables, 1);
	if (!self)
		return E_OUTOFMEMORY;
	self->algorithm = algorithm;
	HRESULT result = from_status(BCryptOpenAlgorithmProvider(&self->handle, algorithm->cipher, NULL, 0));
	if (FAILED(result))
		self->handle = NULL;
	else
		result = from_status(BCryptSetProperty(self->handle, BCRYPT_CHAINING_MODE, (UCHAR *)algorithm->mode,
			(ULONG)((lstrlenW(algorithm->mode) + 1) * sizeof(WCHAR)), 0));
	if (FAILED(result)) {
		release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

TABLE(cipher_factory_table, 0, FACTORY);
TABLE(cipher_statics_table, 1, INSPECTABLE, (const void *)open_cipher);
static const struct face_iid cipher_class_iids[] = {{&iid_factory, 0}, {&iid_cipher_provider_statics, 1}, {NULL, 0}};
static const struct kind cipher_class_kind = {"SymmetricKeyAlgorithmProvider", L"Windows.Security.Cryptography.Core.SymmetricKeyAlgorithmProvider",
	cipher_class_iids, NULL};
struct object cipher_provider_class = {{cipher_factory_table.slots, cipher_statics_table.slots}, &cipher_class_kind, 1};

#define CIPHER_NAME(function, text) static HRESULT WINAPI function(void *iface, HSTRING *out) { return answer_name(text, out); }
CIPHER_NAME(name_des_cbc, L"DES_CBC")
CIPHER_NAME(name_des_ecb, L"DES_ECB")
CIPHER_NAME(name_3des_cbc, L"3DES_CBC")
CIPHER_NAME(name_3des_ecb, L"3DES_ECB")
CIPHER_NAME(name_rc2_cbc, L"RC2_CBC")
CIPHER_NAME(name_rc2_ecb, L"RC2_ECB")
CIPHER_NAME(name_aes_cbc, L"AES_CBC")
CIPHER_NAME(name_aes_ecb, L"AES_ECB")
CIPHER_NAME(name_aes_gcm, L"AES_GCM")
CIPHER_NAME(name_aes_ccm, L"AES_CCM")
CIPHER_NAME(name_aes_cbc_pkcs7, L"AES_CBC_PKCS7")
CIPHER_NAME(name_aes_ecb_pkcs7, L"AES_ECB_PKCS7")
CIPHER_NAME(name_des_cbc_pkcs7, L"DES_CBC_PKCS7")
CIPHER_NAME(name_des_ecb_pkcs7, L"DES_ECB_PKCS7")
CIPHER_NAME(name_3des_cbc_pkcs7, L"3DES_CBC_PKCS7")
CIPHER_NAME(name_3des_ecb_pkcs7, L"3DES_ECB_PKCS7")
CIPHER_NAME(name_rc2_cbc_pkcs7, L"RC2_CBC_PKCS7")
CIPHER_NAME(name_rc2_ecb_pkcs7, L"RC2_ECB_PKCS7")
CIPHER_NAME(name_rc4, L"RC4")

TABLE(cipher_names_factory_table, 0, FACTORY);
TABLE(cipher_names_table, 1, INSPECTABLE, (const void *)name_des_cbc, (const void *)name_des_ecb, (const void *)name_3des_cbc,
	(const void *)name_3des_ecb, (const void *)name_rc2_cbc, (const void *)name_rc2_ecb, (const void *)name_aes_cbc, (const void *)name_aes_ecb,
	(const void *)name_aes_gcm, (const void *)name_aes_ccm, (const void *)name_aes_cbc_pkcs7, (const void *)name_aes_ecb_pkcs7,
	(const void *)name_des_cbc_pkcs7, (const void *)name_des_ecb_pkcs7, (const void *)name_3des_cbc_pkcs7, (const void *)name_3des_ecb_pkcs7,
	(const void *)name_rc2_cbc_pkcs7, (const void *)name_rc2_ecb_pkcs7, (const void *)name_rc4);
static const struct face_iid cipher_names_iids[] = {{&iid_factory, 0}, {&iid_cipher_names, 1}, {NULL, 0}};
static const struct kind cipher_names_kind = {"SymmetricAlgorithmNames", L"Windows.Security.Cryptography.Core.SymmetricAlgorithmNames",
	cipher_names_iids, NULL};
struct object cipher_names_class = {{cipher_names_factory_table.slots, cipher_names_table.slots}, &cipher_names_kind, 1};

/* ---- CryptographicEngine ---- */

/* The key behind a CryptographicKey of this component, or NULL for any other. */
static struct key *own_key(void *given)
{
	return given && method(given, 0) == (void *)object_query && owner(given)->kind == &key_kind ? key_of(given) : NULL;
}

/* The key of a MAC algorithm, which signs. */
static struct key *mac_key(void *given)
{
	struct key *key = own_key(given);
	return key && key->provider ? key : NULL;
}

static HRESULT sign(struct key *key, void *data, BYTE *value)
{
	struct computation *computation;
	BYTE *bytes;
	UINT32 length;
	HRESULT result = bytes_of(data, &bytes, &length);
	if (SUCCEEDED(result))
		result = computation_make(key->provider, key->material, key->length, &computation);
	if (FAILED(result))
		return result;
	AcquireSRWLockExclusive(&computation->lock);
	result = from_status(BCryptHashData(computation->hash, bytes, length, 0));
	if (SUCCEEDED(result))
		result = from_status(BCryptFinishHash(computation->hash, value, key->provider->algorithm->length, 0));
	ReleaseSRWLockExclusive(&computation->lock);
	release(face_of(&computation->base, 0));
	return result;
}

static HRESULT WINAPI engine_sign(void *iface, void *given, void *data, void **out)
{
	struct key *key = mac_key(given);
	BYTE value[64];
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!key || !data)
		return E_INVALIDARG;
	const HRESULT result = sign(key, data, value);
	return SUCCEEDED(result) ? new_buffer(value, key->provider->algorithm->length, out) : result;
}

static HRESULT WINAPI engine_verify(void *iface, void *given, void *data, void *signature, boolean *out)
{
	struct key *key = mac_key(given);
	BYTE value[64], *expected;
	UINT32 length;
	if (!out)
		return E_POINTER;
	*out = FALSE;
	if (!key || !data || !signature)
		return E_INVALIDARG;
	HRESULT result = bytes_of(signature, &expected, &length);
	if (SUCCEEDED(result))
		result = sign(key, data, value);
	if (FAILED(result))
		return result;
	/* Every byte is compared, so the time taken tells nothing of where they differ. */
	BYTE difference = length != key->provider->algorithm->length;
	for (UINT32 at = 0; at < length && at < key->provider->algorithm->length; ++at)
		difference |= value[at] ^ expected[at];
	*out = !difference;
	return S_OK;
}

/* Runs a symmetric key over the data: the IV is copied, so every call starts from the one given, or from zeros. */
static HRESULT crypt(struct key *key, void *data, void *iv, BOOL encrypt, void **out)
{
	BYTE *bytes, *start, chain[16] = {0};
	UINT32 length, start_length;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!key || !key->cipher || !data || key->cipher->algorithm->authenticated)
		return E_INVALIDARG;
	HRESULT result = bytes_of(data, &bytes, &length);
	if (SUCCEEDED(result))
		result = bytes_of(iv, &start, &start_length);
	if (FAILED(result))
		return result;
	const UINT32 block = key->cipher->algorithm->block;
	const BOOL chained = key->cipher->algorithm->chained;
	if (chained && iv && start_length != block)
		return E_INVALIDARG;
	for (UINT32 at = 0; iv && at < start_length && at < sizeof chain; ++at)
		chain[at] = start[at];
	const ULONG flags = key->cipher->algorithm->padded ? BCRYPT_BLOCK_PADDING : 0;
	ULONG size = 0;
	NTSTATUS status = encrypt ? BCryptEncrypt(key->handle, bytes, length, NULL, chained ? chain : NULL, chained ? block : 0, NULL, 0, &size, flags)
		: BCryptDecrypt(key->handle, bytes, length, NULL, chained ? chain : NULL, chained ? block : 0, NULL, 0, &size, flags);
	if (status < 0)
		return HRESULT_FROM_NT(status);
	BYTE *output = ALLOC(size ? size : 1);
	if (!output)
		return E_OUTOFMEMORY;
	status = encrypt ? BCryptEncrypt(key->handle, bytes, length, NULL, chained ? chain : NULL, chained ? block : 0, output, size, &size, flags)
		: BCryptDecrypt(key->handle, bytes, length, NULL, chained ? chain : NULL, chained ? block : 0, output, size, &size, flags);
	result = status < 0 ? HRESULT_FROM_NT(status) : new_buffer(output, size, out);
	SecureZeroMemory(output, size);
	FREE(output);
	return result;
}

static HRESULT WINAPI engine_encrypt(void *iface, void *key, void *data, void *iv, void **out) { return crypt(own_key(key), data, iv, TRUE, out); }
static HRESULT WINAPI engine_decrypt(void *iface, void *key, void *data, void *iv, void **out) { return crypt(own_key(key), data, iv, FALSE, out); }

/* EncryptedAndAuthenticatedData: the encrypted bytes and their tag. */
struct sealed {
	struct object base;
	IUnknown *data, *tag;
};

static void destroy_sealed(struct object *base)
{
	struct sealed *self = (struct sealed *)base;
	release(self->data);
	release(self->tag);
}

static HRESULT WINAPI sealed_data(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = ((struct sealed *)owner(iface))->data);
	return S_OK;
}

static HRESULT WINAPI sealed_tag(void *iface, void **out)
{
	if (!out)
		return E_POINTER;
	reference(*out = ((struct sealed *)owner(iface))->tag);
	return S_OK;
}

TABLE(sealed_table, 0, INSPECTABLE, (const void *)sealed_data, (const void *)sealed_tag);
static const struct face_iid sealed_iids[] = {{&iid_sealed, 0}, {NULL, 0}};
static const struct kind sealed_kind = {"EncryptedAndAuthenticatedData", L"Windows.Security.Cryptography.Core.EncryptedAndAuthenticatedData",
	sealed_iids, destroy_sealed};

/* AES-GCM over the data with its nonce and the extra data it authenticates: a sixteen-byte tag is made, or checked. */
static HRESULT seal(struct key *key, void *data, void *nonce, void *tag, void *extra, BOOL encrypt, BYTE **output, ULONG *size, BYTE *made_tag)
{
	BYTE *bytes, *nonce_bytes, *tag_bytes, *extra_bytes;
	UINT32 length, nonce_length, tag_length, extra_length;
	*output = NULL;
	*size = 0;
	if (!key || !key->cipher || !key->cipher->algorithm->authenticated || !data || !nonce || (!encrypt && !tag))
		return E_INVALIDARG;
	HRESULT result = bytes_of(data, &bytes, &length);
	if (SUCCEEDED(result))
		result = bytes_of(nonce, &nonce_bytes, &nonce_length);
	if (SUCCEEDED(result))
		result = bytes_of(tag, &tag_bytes, &tag_length);
	if (SUCCEEDED(result))
		result = bytes_of(extra, &extra_bytes, &extra_length);
	if (FAILED(result))
		return result;
	if (!encrypt && tag_length != 16)
		return E_INVALIDARG;
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	info.pbNonce = nonce_bytes;
	info.cbNonce = nonce_length;
	info.pbAuthData = extra_length ? extra_bytes : NULL;
	info.cbAuthData = extra_length;
	info.pbTag = encrypt ? made_tag : tag_bytes;
	info.cbTag = 16;
	if (!(*output = ALLOC(length ? length : 1)))
		return E_OUTOFMEMORY;
	const NTSTATUS status = encrypt ? BCryptEncrypt(key->handle, bytes, length, &info, NULL, 0, *output, length, size, 0)
		: BCryptDecrypt(key->handle, bytes, length, &info, NULL, 0, *output, length, size, 0);
	if (status < 0) {
		FREE(*output);
		*output = NULL;
		return HRESULT_FROM_NT(status);
	}
	return S_OK;
}

static HRESULT WINAPI engine_encrypt_authenticated(void *iface, void *key, void *data, void *nonce, void *extra, void **out)
{
	static const void *const *const tables[] = {sealed_table.slots};
	BYTE *output, tag[16];
	ULONG size;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = seal(own_key(key), data, nonce, NULL, extra, TRUE, &output, &size, tag);
	if (FAILED(result))
		return result;
	struct sealed *self = make(sizeof *self, &sealed_kind, tables, 1);
	result = self ? new_buffer(output, size, (void **)&self->data) : E_OUTOFMEMORY;
	if (SUCCEEDED(result))
		result = new_buffer(tag, sizeof tag, (void **)&self->tag);
	FREE(output);
	if (FAILED(result)) {
		if (self)
			release(face_of(&self->base, 0));
		return result;
	}
	*out = face_of(&self->base, 0);
	return S_OK;
}

static HRESULT WINAPI engine_decrypt_authenticated(void *iface, void *key, void *data, void *nonce, void *tag, void *extra, void **out)
{
	BYTE *output;
	ULONG size;
	if (!out)
		return E_POINTER;
	*out = NULL;
	HRESULT result = seal(own_key(key), data, nonce, tag, extra, FALSE, &output, &size, NULL);
	if (FAILED(result))
		return result;
	result = new_buffer(output, size, out);
	SecureZeroMemory(output, size);
	FREE(output);
	return result;
}

static HRESULT WINAPI engine_derive(void *iface, void *key, void *parameters, UINT32 length, void **out)
{
	answer_null(out);
	return missing("CryptographicEngine.DeriveKeyMaterial");
}

TABLE(engine_factory_table, 0, FACTORY);
TABLE(engine_statics_table, 1, INSPECTABLE, (const void *)engine_encrypt, (const void *)engine_decrypt,
	(const void *)engine_encrypt_authenticated, (const void *)engine_decrypt_authenticated, (const void *)engine_sign,
	(const void *)engine_verify, (const void *)engine_derive);
static const struct face_iid engine_iids[] = {{&iid_factory, 0}, {&iid_engine_statics, 1}, {NULL, 0}};
static const struct kind engine_kind = {"CryptographicEngine", L"Windows.Security.Cryptography.Core.CryptographicEngine", engine_iids, NULL};
struct object cryptographic_engine_class = {{engine_factory_table.slots, engine_statics_table.slots}, &engine_kind, 1};
