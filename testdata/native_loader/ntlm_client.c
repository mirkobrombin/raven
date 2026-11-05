#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>

/* Windows authentication between two programs of the machine, played by one: a Negotiate client and server pass their tokens to each
 * other until both are done, then exchange sealed and signed messages both ways, and a changed message is refused. */

static int same(const void *left, const void *right, unsigned long size)
{
	const unsigned char *a = left, *b = right;
	for (unsigned long at = 0; at < size; ++at)
		if (a[at] != b[at])
			return 0;
	return 1;
}

/* A sealed message in a token and a data buffer, opened by the other side. */
static int seal_and_open(CtxtHandle *from, CtxtHandle *to, const char *text, unsigned long size)
{
	char data[64], signature[16];
	SecBuffer parts[2] = {{sizeof(signature), SECBUFFER_TOKEN, signature}, {size, SECBUFFER_DATA, data}};
	SecBufferDesc message = {SECBUFFER_VERSION, 2, parts};
	for (unsigned long at = 0; at < size; ++at)
		data[at] = text[at];
	if (EncryptMessage(from, 0, &message, 0) != SEC_E_OK || same(data, text, size))
		return 0;
	ULONG quality = 7;
	return DecryptMessage(to, &message, 0, &quality) == SEC_E_OK && quality == 0 && same(data, text, size);
}

int raven_entry(void)
{
	CredHandle outbound, inbound;
	TimeStamp expiry;
	if (AcquireCredentialsHandleW(NULL, L"Negotiate", SECPKG_CRED_OUTBOUND, NULL, NULL, NULL, NULL, &outbound, &expiry) != SEC_E_OK ||
		AcquireCredentialsHandleW(NULL, L"Negotiate", SECPKG_CRED_INBOUND, NULL, NULL, NULL, NULL, &inbound, &expiry) != SEC_E_OK)
		return 1;
	CtxtHandle client, server;
	static char to_server[3000], to_client[3000];
	ULONG attributes = 0;
	SecBuffer client_out = {sizeof(to_server), SECBUFFER_TOKEN, to_server}, server_in = {0, SECBUFFER_TOKEN, to_server};
	SecBuffer server_out = {sizeof(to_client), SECBUFFER_TOKEN, to_client}, client_in = {0, SECBUFFER_TOKEN, to_client};
	SecBufferDesc client_output = {SECBUFFER_VERSION, 1, &client_out}, server_input = {SECBUFFER_VERSION, 1, &server_in};
	SecBufferDesc server_output = {SECBUFFER_VERSION, 1, &server_out}, client_input = {SECBUFFER_VERSION, 1, &client_in};
	const ULONG wanted = ISC_REQ_CONFIDENTIALITY | ISC_REQ_INTEGRITY | ISC_REQ_CONNECTION | ISC_REQ_MUTUAL_AUTH;
	SECURITY_STATUS status = InitializeSecurityContextW(&outbound, NULL, L"localhost", wanted, 0, SECURITY_NATIVE_DREP, NULL, 0, &client,
		&client_output, &attributes, &expiry);
	if (status != SEC_I_CONTINUE_NEEDED || !client_out.cbBuffer)
		return 2;
	server_in.cbBuffer = client_out.cbBuffer;
	status = AcceptSecurityContext(&inbound, NULL, &server_input, ASC_REQ_CONFIDENTIALITY | ASC_REQ_CONNECTION, SECURITY_NATIVE_DREP,
		&server, &server_output, &attributes, &expiry);
	if (status != SEC_I_CONTINUE_NEEDED || !server_out.cbBuffer)
		return 3;
	client_in.cbBuffer = server_out.cbBuffer;
	client_out.cbBuffer = sizeof(to_server);
	status = InitializeSecurityContextW(&outbound, &client, L"localhost", wanted, 0, SECURITY_NATIVE_DREP, &client_input, 0, &client,
		&client_output, &attributes, &expiry);
	if (status != SEC_E_OK || !client_out.cbBuffer || !(attributes & ISC_RET_MUTUAL_AUTH) || !(attributes & ISC_RET_CONFIDENTIALITY))
		return 4;
	/* A server that sees the same answer again is past its exchange and refuses it. */
	server_in.cbBuffer = client_out.cbBuffer;
	server_out.cbBuffer = sizeof(to_client);
	status = AcceptSecurityContext(&inbound, &server, &server_input, ASC_REQ_CONFIDENTIALITY | ASC_REQ_CONNECTION, SECURITY_NATIVE_DREP,
		&server, &server_output, &attributes, &expiry);
	if (status != SEC_E_OK || server_out.cbBuffer)
		return 5;
	if (AcceptSecurityContext(&inbound, &server, &server_input, 0, SECURITY_NATIVE_DREP, &server, &server_output, &attributes, &expiry) ==
		SEC_E_OK)
		return 6;

	/* Both sides name the account the program runs as, and agree on the sizes a caller leaves room for. */
	SecPkgContext_NamesW names = {0};
	SecPkgContext_Sizes sizes = {0};
	WCHAR user[64];
	DWORD length = 64;
	if (QueryContextAttributesW(&server, SECPKG_ATTR_NAMES, &names) != SEC_E_OK || !names.sUserName || !GetUserNameW(user, &length))
		return 7;
	const WCHAR *slash = names.sUserName;
	while (*slash && *slash != L'\\')
		++slash;
	if (!*slash || lstrcmpiW(slash + 1, user))
		return 8;
	FreeContextBuffer(names.sUserName);
	if (QueryContextAttributesW(&client, SECPKG_ATTR_SIZES, &sizes) != SEC_E_OK || sizes.cbMaxSignature != 16 || sizes.cbSecurityTrailer != 16)
		return 9;
	if (ImpersonateSecurityContext(&server) != SEC_E_OK || RevertSecurityContext(&server) != SEC_E_OK)
		return 10;
	/* The client's token is one the server can ask about and close. */
	HANDLE token = NULL;
	TOKEN_TYPE kind = 0;
	DWORD kind_size = 0;
	if (QuerySecurityContextToken(&server, &token) != SEC_E_OK || !token ||
		!GetTokenInformation(token, TokenType, &kind, sizeof(kind), &kind_size) || !CloseHandle(token))
		return 20;

	/* Sealed messages both ways, twice each, so the sequence and the streams of both directions move on together. */
	for (int round = 0; round < 2; ++round)
		if (!seal_and_open(&client, &server, "<Discover/>", 11) || !seal_and_open(&server, &client, "<return/>", 9))
			return 11 + round;

	/* A signed message verifies; changed on the way, it no longer does. */
	char data[4] = {1, 2, 3, 4}, signature[16];
	SecBuffer parts[2] = {{4, SECBUFFER_DATA, data}, {sizeof(signature), SECBUFFER_TOKEN, signature}};
	SecBufferDesc message = {SECBUFFER_VERSION, 2, parts};
	ULONG quality = 0;
	if (MakeSignature(&client, 0, &message, 0) != SEC_E_OK || parts[1].cbBuffer != 16 || data[0] != 1 ||
		VerifySignature(&server, &message, 0, &quality) != SEC_E_OK)
		return 13;
	if (MakeSignature(&client, 0, &message, 0) != SEC_E_OK)
		return 14;
	data[2] = 9;
	if (VerifySignature(&server, &message, 0, &quality) == SEC_E_OK)
		return 15;

	/* An answer made for another server's challenge is refused. */
	CtxtHandle stranger, keeper;
	client_out.cbBuffer = sizeof(to_server);
	if (InitializeSecurityContextW(&outbound, NULL, L"localhost", wanted, 0, SECURITY_NATIVE_DREP, NULL, 0, &stranger, &client_output,
			&attributes, &expiry) != SEC_I_CONTINUE_NEEDED)
		return 16;
	server_in.cbBuffer = client_out.cbBuffer;
	server_out.cbBuffer = sizeof(to_client);
	if (AcceptSecurityContext(&inbound, NULL, &server_input, 0, SECURITY_NATIVE_DREP, &keeper, &server_output, &attributes, &expiry) !=
		SEC_I_CONTINUE_NEEDED)
		return 17;
	to_client[24] ^= 1; /* the server's challenge */
	client_in.cbBuffer = server_out.cbBuffer;
	client_out.cbBuffer = sizeof(to_server);
	if (InitializeSecurityContextW(&outbound, &stranger, L"localhost", wanted, 0, SECURITY_NATIVE_DREP, &client_input, 0, &stranger,
			&client_output, &attributes, &expiry) != SEC_E_OK)
		return 18;
	server_in.cbBuffer = client_out.cbBuffer;
	server_out.cbBuffer = sizeof(to_client);
	if (AcceptSecurityContext(&inbound, &keeper, &server_input, 0, SECURITY_NATIVE_DREP, &keeper, &server_output, &attributes, &expiry) !=
		SEC_E_LOGON_DENIED)
		return 19;
	DeleteSecurityContext(&stranger);
	DeleteSecurityContext(&keeper);
	DeleteSecurityContext(&client);
	DeleteSecurityContext(&server);
	FreeCredentialsHandle(&outbound);
	FreeCredentialsHandle(&inbound);
	return 0;
}
