#include "kernel_context.h"
#include "text.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace raven {

namespace {

// SECURITY_STATUS values of the SSPI documentation.
constexpr uint32_t sec_ok = 0, sec_continue_needed = 0x00090312, sec_context_expired = 0x00090317, sec_invalid_handle = 0x80090301,
	sec_unsupported_function = 0x80090302, sec_internal_error = 0x80090304, sec_invalid_token = 0x80090308,
	sec_secpkg_not_found = 0x80090305, sec_buffer_too_small = 0x80090321, sec_incomplete_message = 0x80090318,
	sec_wrong_principal = 0x80090322, sec_untrusted_root = 0x80090325, sec_illegal_message = 0x80090326,
	sec_insufficient_memory = 0x80090300, sec_invalid_parameter = 0x8009035d;
constexpr uint32_t buffer_empty = 0, buffer_data = 1, buffer_token = 2, buffer_missing = 4, buffer_extra = 5, buffer_trailer = 6,
	buffer_header = 7, buffer_application_protocols = 18;
constexpr uint32_t request_allocate_memory = 0x100, request_manual_validation = 0x80000;
constexpr uint32_t credentials_no_name_check = 0x4, credentials_manual_validation = 0x8;
constexpr uint32_t attribute_stream_sizes = 4, attribute_application_protocol = 35, attribute_connection_info = 0x5a;
// The record header is the whole fixed part a sender has to leave room for; the trailer covers the largest padding and tag a TLS 1.2
// block cipher adds.
constexpr uint32_t record_header = 5, record_trailer = 1024, record_payload = 16384;
constexpr uint64_t handle_tag = 0x52564e53;

// A credential of the Windows logon packages holds no TLS context: local remote procedure calls only acquire one, and never build a
// security context on it.
struct Credential {
	std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> tls{nullptr, SSL_CTX_free};
	bool verify = true, check_name = true;
};

struct Context {
	std::unique_ptr<SSL, decltype(&SSL_free)> tls{nullptr, SSL_free};
	BIO *input = nullptr, *output = nullptr;
	bool established = false, closing = false;
};

struct Provider {
	std::mutex lock;
	uint64_t table = 0, next = 1;
	std::map<uint64_t, std::shared_ptr<Credential>> credentials;
	std::map<uint64_t, std::shared_ptr<Context>> contexts;
};

bool tracing()
{
	static const bool value = std::getenv("RAVEN_TRACE_SCHANNEL");
	return value;
}

// One SecBuffer of a SecBufferDesc: the size and type words followed by the pointer, eight bytes in.
struct Buffer {
	uint64_t at;
	uint32_t size, type;
	uint64_t data;
};

std::vector<Buffer> buffers(ApiCall &call, uint64_t description)
{
	std::vector<Buffer> result;
	if (!description)
		return result;
	const uint32_t count = call.load<uint32_t>(description + 4);
	const uint64_t first = call.load_pointer(description + 8);
	const size_t stride = 8 + call.pointer_width();
	for (uint32_t index = 0; index < count && index < 16; ++index) {
		const uint64_t at = first + index * stride;
		result.push_back({at, call.load<uint32_t>(at), call.load<uint32_t>(at + 4), call.load_pointer(at + 8)});
	}
	return result;
}

void set_buffer(ApiCall &call, const Buffer &buffer, uint32_t type, uint32_t size, uint64_t data)
{
	call.store<uint32_t>(buffer.at, size);
	call.store<uint32_t>(buffer.at + 4, type);
	call.store_pointer(buffer.at + 8, data);
}

uint64_t handle_of(ApiCall &call, uint64_t address)
{
	return address && call.load_pointer(address + call.pointer_width()) == handle_tag ? call.load_pointer(address) : 0;
}

// The length of the records complete at the start of the bytes, every one a five-byte header and the length it announces.
size_t complete_records(const std::vector<uint8_t> &bytes, size_t limit = SIZE_MAX)
{
	size_t at = 0;
	for (size_t count = 0; count < limit && at + record_header <= bytes.size(); ++count) {
		const size_t length = record_header + (size_t(bytes[at + 3]) << 8 | bytes[at + 4]);
		if (at + length > bytes.size())
			break;
		at += length;
	}
	return at;
}

std::vector<uint8_t> pending_output(Context &context)
{
	std::vector<uint8_t> bytes(BIO_ctrl_pending(context.output));
	if (!bytes.empty())
		BIO_read(context.output, bytes.data(), int(bytes.size()));
	return bytes;
}

std::shared_ptr<Credential> make_credential(uint32_t flags, bool manual)
{
	auto credential = std::make_shared<Credential>();
	credential->tls.reset(SSL_CTX_new(TLS_client_method()));
	if (!credential->tls)
		return nullptr;
	SSL_CTX_set_default_verify_paths(credential->tls.get());
	credential->verify = !manual && !(flags & credentials_manual_validation);
	credential->check_name = !(flags & credentials_no_name_check);
	return credential;
}

uint32_t failure_status(Context &context)
{
	const long verified = SSL_get_verify_result(context.tls.get());
	if (verified == X509_V_ERR_HOSTNAME_MISMATCH)
		return sec_wrong_principal;
	if (verified != X509_V_OK)
		return sec_untrusted_root;
	return sec_illegal_message;
}

}

// The Schannel package of SSPI as a TLS client over the host's OpenSSL: the handshake, record protection and close are the
// buffer exchanges of the SSPI documentation, with the records framed by OpenSSL through memory BIOs.
void register_schannel_api(ApiRegistry &registry, KernelContext &context)
{
	auto provider = std::make_shared<Provider>();
	registry.add("InitSecurityInterfaceA", 0, [](ApiCall &call) { call.fail(50); });
	// The table is built outside the lock: the guest allocator can give the processor to another guest thread, which may ask for it too on
	// the same host thread. The first table published is the one every caller gets.
	registry.add("InitSecurityInterfaceW", 0, [provider, &context](ApiCall &call) {
		{
			std::lock_guard guard(provider->lock);
			if (provider->table) {
				call.ret(provider->table);
				return;
			}
		}
		{
			// SecurityFunctionTableW: the version, then the entry points in the order of the SSPI header, reserved slots left null.
			static constexpr const char *entries[] = {"EnumerateSecurityPackagesW", "QueryCredentialsAttributesW", "AcquireCredentialsHandleW",
				"FreeCredentialsHandle", nullptr, "InitializeSecurityContextW", "AcceptSecurityContext", "CompleteAuthToken",
				"DeleteSecurityContext", "ApplyControlToken", "QueryContextAttributesW", "ImpersonateSecurityContext",
				"RevertSecurityContext", "MakeSignature", "VerifySignature", "FreeContextBuffer", "QuerySecurityPackageInfoW", nullptr,
				nullptr, "ExportSecurityContext", "ImportSecurityContextW", "AddCredentialsW", nullptr, "QuerySecurityContextToken",
				"EncryptMessage", "DecryptMessage", "SetContextAttributesW", "SetCredentialsAttributesW", "ChangeAccountPasswordW", nullptr,
				nullptr};
			const size_t width = call.pointer_width(), count = std::size(entries);
			const uint64_t table = context.allocator.allocate(width * (count + 1));
			if (!table) {
				call.fail(8);
				return;
			}
			call.store<uint32_t>(table, 4);
			for (size_t index = 0; index < count; ++index)
				if (entries[index])
					call.store_pointer(table + width * (index + 1), context.modules.resolve({"secur32.dll", entries[index], 0, false, 0}));
			std::unique_lock guard(provider->lock);
			const bool first = !provider->table;
			if (first)
				provider->table = table;
			const uint64_t published = provider->table;
			guard.unlock();
			if (!first)
				context.allocator.release(table);
			call.ret(published);
		}
	});
	registry.add("AcquireCredentialsHandleW", 9, [provider](ApiCall &call) {
		const auto package = call[1] ? call.wide(call[1]) : std::u16string();
		const bool logon = equal_fold(package, u"Negotiate") || equal_fold(package, u"NTLM") || equal_fold(package, u"Kerberos");
		if (!logon && !equal_fold(package, u"Microsoft Unified Security Protocol Provider") && !equal_fold(package, u"Schannel")
			&& !equal_fold(package, u"Default TLS SSP")) {
			call.ret(sec_secpkg_not_found);
			return;
		}
		if (logon) {
			std::lock_guard guard(provider->lock);
			const uint64_t id = provider->next++;
			provider->credentials[id] = std::make_shared<Credential>();
			call.store_pointer(call[7], id);
			call.store_pointer(call[7] + call.pointer_width(), handle_tag);
			if (call[8])
				call.store<int64_t>(call[8], INT64_MAX);
			call.ret(sec_ok);
			return;
		}
		// SCHANNEL_CRED (version 4) and SCH_CREDENTIALS (version 5) keep their flags at different places. The older structure offers
		// no TLS 1.3, which only SCH_CREDENTIALS reaches, and its grbitEnabledProtocols, when it names any, bound the versions.
		uint32_t flags = 0, enabled = 0;
		int highest = TLS1_3_VERSION;
		if (const uint64_t data = call[4]) {
			const uint32_t version = call.load<uint32_t>(data);
			const bool wide = call.pointer_width() == 8;
			if (version == 4) {
				flags = call.load<uint32_t>(data + (wide ? 72 : 48));
				enabled = call.load<uint32_t>(data + (wide ? 56 : 32));
				highest = TLS1_2_VERSION;
			} else if (version == 5) {
				flags = call.load<uint32_t>(data + (wide ? 52 : 32));
			}
		}
		auto credential = make_credential(flags, false);
		if (!credential) {
			call.ret(sec_internal_error);
			return;
		}
		// SP_PROT_TLS1_0, 1_1, 1_2 and 1_3, the client and server bits of each.
		static constexpr std::pair<uint32_t, int> versions[] = {{0xc0, TLS1_VERSION}, {0x300, TLS1_1_VERSION}, {0xc00, TLS1_2_VERSION},
			{0x3000, TLS1_3_VERSION}};
		int lowest = 0;
		if (enabled) {
			int top = 0;
			for (const auto &[bits, protocol] : versions)
				if (enabled & bits) {
					if (!lowest)
						lowest = protocol;
					top = protocol;
				}
			if (top)
				highest = std::min(highest, top);
		}
		if (lowest)
			SSL_CTX_set_min_proto_version(credential->tls.get(), lowest);
		SSL_CTX_set_max_proto_version(credential->tls.get(), highest);
		std::lock_guard guard(provider->lock);
		const uint64_t id = provider->next++;
		provider->credentials[id] = credential;
		call.store_pointer(call[7], id);
		call.store_pointer(call[7] + call.pointer_width(), handle_tag);
		if (call[8])
			call.store<int64_t>(call[8], INT64_MAX);
		call.ret(sec_ok);
	});
	registry.add("FreeCredentialsHandle", 1, [provider](ApiCall &call) {
		std::lock_guard guard(provider->lock);
		call.ret(provider->credentials.erase(handle_of(call, call[0])) ? sec_ok : sec_invalid_handle);
	});
	registry.add("DeleteSecurityContext", 1, [provider](ApiCall &call) {
		std::lock_guard guard(provider->lock);
		call.ret(provider->contexts.erase(handle_of(call, call[0])) ? sec_ok : sec_invalid_handle);
	});
	// The packages: the TLS provider under its two names, and the Windows logon packages, whose RPC identifiers let local remote procedure
	// calls register and bind with them. Each SecPkgInfo (capabilities, version, RPC identifier, largest token, name, comment) sits in one
	// block with its strings, freed by FreeContextBuffer.
	const auto package_list = [&context](ApiCall &call, bool narrow, std::optional<std::u16string> only) -> uint32_t {
		struct Package {
			std::u16string_view name, comment;
			uint32_t capabilities;
			uint16_t rpc;
			uint32_t largest_token;
		};
		static constexpr Package packages[] = {
			{u"Microsoft Unified Security Protocol Provider", u"Schannel Security Package", 0x107b3, 14, 0x6000},
			{u"Schannel", u"Schannel Security Package", 0x107b3, 14, 0x6000},
			{u"Negotiate", u"Microsoft Package Negotiator", 0x83bb3, 9, 48256},
			{u"NTLM", u"NTLM Security Package", 0x82b37, 10, 2888},
			{u"Kerberos", u"Microsoft Kerberos V1.0", 0x20bbbf, 16, 48000}};
		std::vector<const Package *> chosen;
		for (const auto &package : packages)
			if (!only || equal_fold(*only, package.name))
				chosen.push_back(&package);
		if (chosen.empty())
			return sec_secpkg_not_found;
		const size_t width = call.pointer_width(), entry = width == 8 ? 32 : 20, unit = narrow ? 1 : 2;
		size_t size = entry * chosen.size();
		for (const auto package : chosen)
			size += (package->name.size() + 1 + package->comment.size() + 1) * unit;
		const uint64_t block = context.allocator.allocate(size, true);
		if (!block)
			return sec_insufficient_memory;
		uint64_t text = block + entry * chosen.size();
		const auto put = [&](std::u16string_view value) {
			const uint64_t at = text;
			if (narrow)
				call.put_narrow(at, to_utf8(value), value.size() + 1);
			else
				call.put_wide(at, value, value.size() + 1);
			text += (value.size() + 1) * unit;
			return at;
		};
		for (size_t index = 0; index < chosen.size(); ++index) {
			const uint64_t at = block + entry * index;
			call.store<uint32_t>(at, chosen[index]->capabilities);
			call.store<uint16_t>(at + 4, 1);
			call.store<uint16_t>(at + 6, chosen[index]->rpc);
			call.store<uint32_t>(at + 8, chosen[index]->largest_token);
			call.store_pointer(at + (width == 8 ? 16 : 12), put(chosen[index]->name));
			call.store_pointer(at + (width == 8 ? 24 : 16), put(chosen[index]->comment));
		}
		if (only) {
			call.store_pointer(call[1], block);
		} else {
			call.store<uint32_t>(call[0], uint32_t(chosen.size()));
			call.store_pointer(call[1], block);
		}
		return sec_ok;
	};
	for (const bool narrow : {false, true}) {
		registry.add(narrow ? "EnumerateSecurityPackagesA" : "EnumerateSecurityPackagesW", 2, [package_list, narrow](ApiCall &call) {
			call.ret(!call[0] || !call[1] ? sec_invalid_parameter : package_list(call, narrow, std::nullopt));
		});
		registry.add(narrow ? "QuerySecurityPackageInfoA" : "QuerySecurityPackageInfoW", 2, [package_list, narrow](ApiCall &call) {
			if (!call[0] || !call[1]) {
				call.ret(sec_invalid_parameter);
				return;
			}
			const auto name = narrow ? widen_ascii(call.narrow(call[0], 256)) : call.wide(call[0], 256);
			call.ret(package_list(call, narrow, name));
		});
	}
	registry.add("FreeContextBuffer", 1, [&context](ApiCall &call) {
		if (call[0])
			context.allocator.release(call[0]);
		call.ret(sec_ok);
	});
	registry.add("ApplyControlToken", 2, [provider](ApiCall &call) {
		std::shared_ptr<Context> session;
		{
			std::lock_guard guard(provider->lock);
			if (const auto found = provider->contexts.find(handle_of(call, call[0])); found != provider->contexts.end())
				session = found->second;
		}
		if (!session) {
			call.ret(sec_invalid_handle);
			return;
		}
		// SCHANNEL_SHUTDOWN asks the next InitializeSecurityContext for the close notification.
		for (const auto &buffer : buffers(call, call[1]))
			if (buffer.type == buffer_token && buffer.size >= 4 && call.load<uint32_t>(buffer.data) == 1)
				session->closing = true;
		call.ret(sec_ok);
	});
	registry.add("InitializeSecurityContextW", 12, [provider, &context](ApiCall &call) {
		const uint32_t request = call.u32(3);
		std::shared_ptr<Context> session;
		{
			std::lock_guard guard(provider->lock);
			if (call[1]) {
				if (const auto found = provider->contexts.find(handle_of(call, call[1])); found != provider->contexts.end())
					session = found->second;
				if (!session) {
					call.ret(sec_invalid_handle);
					return;
				}
			} else {
				const auto found = provider->credentials.find(handle_of(call, call[0]));
				if (found == provider->credentials.end()) {
					call.ret(sec_invalid_handle);
					return;
				}
				const auto &credential = *found->second;
				if (!credential.tls) {
					call.ret(sec_unsupported_function);
					return;
				}
				session = std::make_shared<Context>();
				session->tls.reset(SSL_new(credential.tls.get()));
				session->input = BIO_new(BIO_s_mem());
				session->output = BIO_new(BIO_s_mem());
				SSL_set_bio(session->tls.get(), session->input, session->output);
				SSL_set_connect_state(session->tls.get());
				const auto target = call[2] ? to_utf8(call.wide(call[2])) : std::string();
				in6_addr address;
				const bool numeric = inet_pton(AF_INET, target.c_str(), &address) == 1 || inet_pton(AF_INET6, target.c_str(), &address) == 1;
				if (!target.empty() && !numeric)
					SSL_set_tlsext_host_name(session->tls.get(), target.c_str());
				const bool verify = credential.verify && !(request & request_manual_validation);
				SSL_set_verify(session->tls.get(), verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
				if (verify && credential.check_name && !target.empty()) {
					if (numeric)
						X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(session->tls.get()), target.c_str());
					else
						SSL_set1_host(session->tls.get(), target.c_str());
				}
				// SEC_APPLICATION_PROTOCOLS: the size of the lists, then lists of an extension kind, a length and the protocols in
				// the wire form of ALPN.
				for (const auto &buffer : buffers(call, call[6])) {
					if (buffer.type != buffer_application_protocols || buffer.size < 10)
						continue;
					const auto lists = call.read(buffer.data, buffer.size);
					for (size_t at = 4; at + 6 <= lists.size();) {
						const uint32_t kind = lists[at] | lists[at + 1] << 8 | lists[at + 2] << 16 | uint32_t(lists[at + 3]) << 24;
						const size_t length = lists[at + 4] | lists[at + 5] << 8;
						if (at + 6 + length > lists.size())
							break;
						if (kind == 2)
							SSL_set_alpn_protos(session->tls.get(), lists.data() + at + 6, unsigned(length));
						at += 6 + length;
					}
				}
				const uint64_t id = provider->next++;
				provider->contexts[id] = session;
				call.store_pointer(call[8], id);
				call.store_pointer(call[8] + call.pointer_width(), handle_tag);
			}
		}
		uint32_t status = sec_continue_needed;
		if (session->closing) {
			SSL_shutdown(session->tls.get());
			status = sec_ok;
		} else {
			// Records go in one at a time so whatever follows the end of the handshake stays with the caller as extra data.
			const auto inputs = buffers(call, call[6]);
			const Buffer *token = nullptr, *spare = nullptr;
			for (const auto &buffer : inputs)
				if (buffer.type == buffer_token && !token)
					token = &buffer;
				else if (buffer.type == buffer_empty && token && !spare)
					spare = &buffer;
			const auto bytes = token && token->size ? call.read(token->data, token->size) : std::vector<uint8_t>();
			size_t used = 0;
			bool failed = false;
			if (!bytes.empty() && !complete_records(bytes)) {
				if (spare)
					set_buffer(call, *spare, buffer_missing, uint32_t(record_header + 1), 0);
				call.ret(sec_incomplete_message);
				return;
			}
			for (;;) {
				const int result = SSL_do_handshake(session->tls.get());
				if (result == 1) {
					session->established = true;
					status = sec_ok;
					break;
				}
				const int error = SSL_get_error(session->tls.get(), result);
				if (error != SSL_ERROR_WANT_READ) {
					failed = true;
					break;
				}
				const size_t record = complete_records(std::vector<uint8_t>(bytes.begin() + used, bytes.end()), 1);
				if (!record)
					break;
				BIO_write(session->input, bytes.data() + used, int(record));
				used += record;
			}
			if (failed) {
				status = failure_status(*session);
				if (tracing()) {
					char text[256];
					ERR_error_string_n(ERR_peek_last_error(), text, sizeof(text));
					std::fprintf(stderr, "schannel handshake failed: %s verify %ld\n", text, SSL_get_verify_result(session->tls.get()));
				}
				ERR_clear_error();
			}
			if (spare && used < bytes.size() && !failed)
				set_buffer(call, *spare, buffer_extra, uint32_t(bytes.size() - used), 0);
		}
		const auto produced = pending_output(*session);
		// The token goes to the first token buffer, or the first empty one when the caller marked none.
		const auto outputs = buffers(call, call[9]);
		auto chosen = std::find_if(outputs.begin(), outputs.end(), [](const Buffer &buffer) { return buffer.type == buffer_token; });
		if (chosen == outputs.end())
			chosen = std::find_if(outputs.begin(), outputs.end(), [](const Buffer &buffer) { return buffer.type == buffer_empty; });
		if (chosen != outputs.end()) {
			const Buffer &buffer = *chosen;
			uint64_t data = buffer.data;
			if (request & request_allocate_memory) {
				data = produced.empty() ? 0 : context.allocator.allocate(produced.size(), false);
				if (!produced.empty() && !data) {
					call.ret(sec_insufficient_memory);
					return;
				}
			} else if (produced.size() > buffer.size) {
				call.ret(sec_buffer_too_small);
				return;
			}
			if (!produced.empty())
				call.write(data, produced);
			set_buffer(call, buffer, buffer_token, uint32_t(produced.size()), data);
		}
		if (call[10])
			call.store<uint32_t>(call[10], request);
		if (call[11])
			call.store<int64_t>(call[11], INT64_MAX);
		if (tracing())
			std::fprintf(stderr, "schannel handshake status %x sent %zu\n", status, produced.size());
		call.ret(status);
	});
	const auto session_of = [provider](ApiCall &call) {
		std::lock_guard guard(provider->lock);
		const auto found = provider->contexts.find(handle_of(call, call[0]));
		return found == provider->contexts.end() ? nullptr : found->second;
	};
	registry.add("QueryContextAttributesW", 3, [session_of](ApiCall &call) {
		const auto session = session_of(call);
		if (!session) {
			call.ret(sec_invalid_handle);
			return;
		}
		switch (call.u32(1)) {
		case attribute_stream_sizes: {
			// SecPkgContext_StreamSizes: header, trailer, largest message, buffer count and block size.
			const uint32_t sizes[] = {record_header, record_trailer, record_payload, 4, 16};
			call.write(call[2], {reinterpret_cast<const uint8_t *>(sizes), sizeof(sizes)});
			break;
		}
		case attribute_application_protocol: {
			// SecPkgContext_ApplicationProtocol: the outcome, the ALPN extension kind, then the chosen protocol with its length.
			const unsigned char *chosen = nullptr;
			unsigned int length = 0;
			SSL_get0_alpn_selected(session->tls.get(), &chosen, &length);
			std::vector<uint8_t> result(8 + 1 + 255);
			result[0] = length ? 1 : 0;
			result[4] = 2;
			result[8] = uint8_t(length);
			if (length)
				std::memcpy(result.data() + 9, chosen, length);
			call.write(call[2], result);
			break;
		}
		case attribute_connection_info: {
			// SecPkgContext_ConnectionInfo: protocol, cipher, hash and exchange with their strengths.
			const int version = SSL_version(session->tls.get());
			const uint32_t protocol = version == TLS1_3_VERSION ? 0x2000 : version == TLS1_2_VERSION ? 0x800 : 0x200;
			const uint32_t info[] = {protocol, 0x6610, 256, 0x800c, 256, 0xae06, 256};
			call.write(call[2], {reinterpret_cast<const uint8_t *>(info), sizeof(info)});
			break;
		}
		default:
			call.ret(sec_unsupported_function);
			return;
		}
		call.ret(sec_ok);
	});
	registry.add("EncryptMessage", 4, [session_of](ApiCall &call) {
		const auto session = session_of(call);
		if (!session || !session->established) {
			call.ret(sec_invalid_handle);
			return;
		}
		const auto list = buffers(call, call[2]);
		const Buffer *header = nullptr, *data = nullptr, *trailer = nullptr;
		for (const auto &buffer : list)
			if (buffer.type == buffer_header)
				header = &buffer;
			else if (buffer.type == buffer_data)
				data = &buffer;
			else if (buffer.type == buffer_trailer)
				trailer = &buffer;
		if (!header || !data || !trailer || data->size > record_payload) {
			call.ret(sec_invalid_token);
			return;
		}
		const auto plain = call.read(data->data, data->size);
		if (!plain.empty() && SSL_write(session->tls.get(), plain.data(), int(plain.size())) <= 0) {
			ERR_clear_error();
			call.ret(sec_internal_error);
			return;
		}
		// The record is spread over the three buffers in order, so a caller that sends them back to back sends it whole.
		const auto record = pending_output(*session);
		const size_t first = std::min<size_t>(header->size, record.size()), second = std::min<size_t>(data->size, record.size() - first),
			rest = record.size() - first - second;
		if (rest > trailer->size) {
			call.ret(sec_buffer_too_small);
			return;
		}
		call.write(header->data, {record.data(), first});
		call.write(data->data, {record.data() + first, second});
		call.write(trailer->data, {record.data() + first + second, rest});
		set_buffer(call, *header, buffer_header, uint32_t(first), header->data);
		set_buffer(call, *data, buffer_data, uint32_t(second), data->data);
		set_buffer(call, *trailer, buffer_trailer, uint32_t(rest), trailer->data);
		if (tracing())
			std::fprintf(stderr, "schannel sealed %zu bytes into %zu\n", plain.size(), record.size());
		call.ret(sec_ok);
	});
	registry.add("DecryptMessage", 4, [session_of](ApiCall &call) {
		const auto session = session_of(call);
		if (!session || !session->established) {
			call.ret(sec_invalid_handle);
			return;
		}
		const auto list = buffers(call, call[1]);
		const Buffer *data = nullptr;
		for (const auto &buffer : list)
			if (buffer.type == buffer_data) {
				data = &buffer;
				break;
			}
		if (!data || list.size() < 4) {
			call.ret(sec_invalid_token);
			return;
		}
		const auto bytes = call.read(data->data, data->size);
		const size_t record = complete_records(bytes, 1);
		std::vector<const Buffer *> empty;
		for (const auto &buffer : list)
			if (&buffer != data && buffer.type == buffer_empty)
				empty.push_back(&buffer);
		if (!record) {
			if (tracing())
				std::fprintf(stderr, "schannel record incomplete with %zu bytes\n", bytes.size());
			if (!empty.empty()) {
				const size_t wanted = bytes.size() < record_header ? record_header : record_header + (size_t(bytes[3]) << 8 | bytes[4]);
				set_buffer(call, *empty.front(), buffer_missing, uint32_t(wanted - bytes.size()), 0);
			}
			call.ret(sec_incomplete_message);
			return;
		}
		// One record per call, decrypted in place: the header, the plaintext, the rest of the record and whatever followed it.
		BIO_write(session->input, bytes.data(), int(record));
		std::vector<uint8_t> plain;
		uint32_t status = sec_ok;
		for (;;) {
			uint8_t chunk[record_payload];
			const int read = SSL_read(session->tls.get(), chunk, sizeof(chunk));
			if (read > 0) {
				plain.insert(plain.end(), chunk, chunk + read);
				continue;
			}
			const int error = SSL_get_error(session->tls.get(), read);
			if (error == SSL_ERROR_ZERO_RETURN)
				status = sec_context_expired;
			else if (error != SSL_ERROR_WANT_READ)
				status = sec_illegal_message;
			ERR_clear_error();
			break;
		}
		if (status == sec_illegal_message || plain.size() + record_header > record) {
			if (tracing())
				std::fprintf(stderr, "schannel record of %zu bytes did not open into %zu\n", record, plain.size());
			call.ret(sec_illegal_message);
			return;
		}
		call.write(data->data + record_header, plain);
		set_buffer(call, list[0], buffer_header, record_header, data->data);
		set_buffer(call, list[1], buffer_data, uint32_t(plain.size()), data->data + record_header);
		set_buffer(call, list[2], buffer_trailer, uint32_t(record - record_header - plain.size()), data->data + record_header + plain.size());
		if (record < bytes.size())
			set_buffer(call, list[3], buffer_extra, uint32_t(bytes.size() - record), data->data + record);
		else
			set_buffer(call, list[3], buffer_empty, 0, 0);
		if (tracing())
			std::fprintf(stderr, "schannel opened %zu of %zu bytes into %zu, status %x\n", record, bytes.size(), plain.size(), status);
		call.ret(status);
	});
}

}
