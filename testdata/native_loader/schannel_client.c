#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>

static void zero(void *address, unsigned size)
{
	for (unsigned index = 0; index < size; ++index) ((BYTE *)address)[index] = 0;
}

static void copy(void *target, const void *source, unsigned size)
{
	for (unsigned index = 0; index < size; ++index) ((BYTE *)target)[index] = ((const BYTE *)source)[index];
}

static unsigned short command_port(void)
{
	const char *line = GetCommandLineA();
	const char *last = line;
	for (const char *cursor = line; *cursor; ++cursor) if (*cursor == ' ') last = cursor + 1;
	unsigned port = 0;
	for (; *last >= '0' && *last <= '9'; ++last) port = port * 10 + (unsigned)(*last - '0');
	return (unsigned short)port;
}

static int sent_all(SOCKET socket, const char *bytes, int length)
{
	while (length > 0) {
		int sent = send(socket, bytes, length, 0);
		if (sent <= 0) return 0;
		bytes += sent;
		length -= sent;
	}
	return 1;
}

static SecBuffer *buffer_of(SecBufferDesc *description, unsigned long type)
{
	for (unsigned long index = 0; index < description->cBuffers; ++index)
		if (description->pBuffers[index].BufferType == type) return &description->pBuffers[index];
	return NULL;
}

int raven_entry(void)
{
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
	PSecurityFunctionTableW table = InitSecurityInterfaceW();
	if (!table || !table->AcquireCredentialsHandleW || !table->InitializeSecurityContextW || !table->DecryptMessage) return 2;
	SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKADDR_IN address;
	zero(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(command_port());
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (socket_handle == INVALID_SOCKET || connect(socket_handle, (SOCKADDR *)&address, sizeof(address))) return 3;

	// The test certificate is self-signed, so the client validates it the way curl does with verification off.
	SCHANNEL_CRED settings;
	zero(&settings, sizeof(settings));
	settings.dwVersion = SCHANNEL_CRED_VERSION;
	settings.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_NO_SERVERNAME_CHECK;
	CredHandle credentials;
	TimeStamp expiry;
	if (table->AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &settings, NULL, NULL,
		&credentials, &expiry) != SEC_E_OK) return 4;

	const ULONG request = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT |
		ISC_REQ_SEQUENCE_DETECT | ISC_REQ_STREAM;
	CtxtHandle context;
	ULONG attributes = 0;
	static char incoming[65536];
	int stored = 0;
	SecBuffer output = {0, SECBUFFER_EMPTY, NULL};
	SecBufferDesc output_description = {SECBUFFER_VERSION, 1, &output};
	SECURITY_STATUS status = table->InitializeSecurityContextW(&credentials, NULL, L"localhost", request, 0, 0, NULL, 0,
		&context, &output_description, &attributes, &expiry);
	if (status != SEC_I_CONTINUE_NEEDED || !output.cbBuffer || !output.pvBuffer) return 5;
	for (int round = 0; status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE; ++round) {
		if (round > 32) return 6;
		if (output.cbBuffer && output.pvBuffer) {
			if (!sent_all(socket_handle, output.pvBuffer, (int)output.cbBuffer)) return 7;
			table->FreeContextBuffer(output.pvBuffer);
		}
		output.cbBuffer = 0;
		output.pvBuffer = NULL;
		output.BufferType = SECBUFFER_TOKEN;
		if (status == SEC_E_INCOMPLETE_MESSAGE || !stored) {
			int read = recv(socket_handle, incoming + stored, (int)sizeof(incoming) - stored, 0);
			if (read <= 0) return 8;
			stored += read;
		}
		SecBuffer input[2] = {{(ULONG)stored, SECBUFFER_TOKEN, incoming}, {0, SECBUFFER_EMPTY, NULL}};
		SecBufferDesc input_description = {SECBUFFER_VERSION, 2, input};
		status = table->InitializeSecurityContextW(&credentials, &context, L"localhost", request, 0, 0,
			&input_description, 0, NULL, &output_description, &attributes, &expiry);
		if (status == SEC_E_INCOMPLETE_MESSAGE) continue;
		if (input[1].BufferType == SECBUFFER_EXTRA) {
			copy(incoming, incoming + stored - input[1].cbBuffer, input[1].cbBuffer);
			stored = (int)input[1].cbBuffer;
		} else stored = 0;
	}
	if (status != SEC_E_OK) return 9;
	if (output.cbBuffer && output.pvBuffer) {
		if (!sent_all(socket_handle, output.pvBuffer, (int)output.cbBuffer)) return 10;
		table->FreeContextBuffer(output.pvBuffer);
	}

	SecPkgContext_StreamSizes sizes;
	if (table->QueryContextAttributesW(&context, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK ||
		!sizes.cbHeader || !sizes.cbMaximumMessage) return 11;
	/* The server's certificate, whose store holds the chain it sent; a link to it may be added to another store. */
	PCCERT_CONTEXT remote = NULL;
	if (table->QueryContextAttributesW(&context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) != SEC_E_OK || !remote ||
		!remote->cbCertEncoded || !remote->hCertStore) return 20;
	PCCERT_CONTEXT walked = CertEnumCertificatesInStore(remote->hCertStore, NULL);
	if (!walked || walked->cbCertEncoded != remote->cbCertEncoded) return 21;
	CertFreeCertificateContext(walked);
	HCERTSTORE linked = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
	if (!linked || !CertAddCertificateLinkToStore(linked, remote, CERT_STORE_ADD_ALWAYS, NULL)) return 22;
	walked = CertEnumCertificatesInStore(linked, NULL);
	if (!walked || walked->cbCertEncoded != remote->cbCertEncoded) return 23;
	CertFreeCertificateContext(walked);
	CertCloseStore(linked, 0);
	CertFreeCertificateContext(remote);
	static char record[32768];
	copy(record + sizes.cbHeader, "ping", 4);
	SecBuffer message[4] = {
		{sizes.cbHeader, SECBUFFER_STREAM_HEADER, record},
		{4, SECBUFFER_DATA, record + sizes.cbHeader},
		{sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record + sizes.cbHeader + 4},
		{0, SECBUFFER_EMPTY, NULL}};
	SecBufferDesc message_description = {SECBUFFER_VERSION, 4, message};
	if (table->EncryptMessage(&context, 0, &message_description, 0) != SEC_E_OK) return 12;
	if (!sent_all(socket_handle, record, (int)(message[0].cbBuffer + message[1].cbBuffer + message[2].cbBuffer))) return 13;

	char reply[4];
	int replied = 0;
	for (int round = 0; replied < 4; ++round) {
		if (round > 32) return 14;
		SecBuffer parts[4] = {{(ULONG)stored, SECBUFFER_DATA, incoming}, {0, SECBUFFER_EMPTY, NULL},
			{0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}};
		SecBufferDesc parts_description = {SECBUFFER_VERSION, 4, parts};
		status = stored ? table->DecryptMessage(&context, &parts_description, 0, NULL) : SEC_E_INCOMPLETE_MESSAGE;
		if (status == SEC_E_INCOMPLETE_MESSAGE) {
			int read = recv(socket_handle, incoming + stored, (int)sizeof(incoming) - stored, 0);
			if (read <= 0) return 15;
			stored += read;
			continue;
		}
		if (status != SEC_E_OK) return 16;
		SecBuffer *plain = buffer_of(&parts_description, SECBUFFER_DATA);
		if (plain) {
			if (replied + (int)plain->cbBuffer > 4) return 17;
			copy(reply + replied, plain->pvBuffer, plain->cbBuffer);
			replied += (int)plain->cbBuffer;
		}
		SecBuffer *extra = buffer_of(&parts_description, SECBUFFER_EXTRA);
		if (extra) {
			copy(incoming, incoming + stored - extra->cbBuffer, extra->cbBuffer);
			stored = (int)extra->cbBuffer;
		} else stored = 0;
	}
	if (reply[0] != 'p' || reply[1] != 'o' || reply[2] != 'n' || reply[3] != 'g') return 18;

	if (table->DeleteSecurityContext(&context) != SEC_E_OK || table->FreeCredentialsHandle(&credentials) != SEC_E_OK) return 19;
	closesocket(socket_handle);
	WSACleanup();
	return 0;
}
