# Guest remote procedure calls

Raven's own rpcrt4.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64 on
the public Windows headers. It holds one NDR format string interpreter behind the whole `Ndr*` family,
the stubless client and server, COM proxies and stubs, context handles and the connection-oriented DCE
RPC protocol of C706 and MS-RPCE over named pipes (`ncalrpc`, `ncacn_np`) and TCP (`ncacn_ip_tcp`).
Sockets load on the first use of TCP, so the rest of the library never touches the network stack.

There is no security provider: authentication settings are remembered and returned, nothing is signed
or sealed, and `RpcBindingInqAuthClient` answers that a binding has no authentication. Pipes in the NDR
sense, asynchronous servers, `CreateProxyFromTypeInfo`, `CreateStubFromTypeInfo`, NDR64 and procedure
encoding through `NdrMesProcEncodeDecode` are absent. See [LICENSES.md](../../../LICENSES.md).
