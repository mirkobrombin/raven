/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_RPC_OWN_H
#define RAVEN_RPC_OWN_H

#include <stddef.h>
#include <string.h>
// The public header gives this one a parameter that the export does not have.
#define I_RpcReceive header_I_RpcReceive
#define USE_STUBLESS_PROXY
#include <windows.h>
#include <objbase.h>
#include <rpc.h>
#include <rpcndr.h>
#include <rpcasync.h>
#include <rpcproxy.h>
#include <midles.h>
#include <ndrtypes.h>
#undef I_RpcReceive

RPC_STATUS RPC_ENTRY I_RpcReceive(RPC_MESSAGE *message);

typedef enum ndr_op
{
	NDR_SIZE,
	NDR_MARSHAL,
	NDR_UNMARSHAL,
	NDR_MEMSIZE,	// advances the buffer and adds up the memory the data would take
	NDR_SKIP,	// advances the buffer only
	NDR_FREE,
} ndr_op;

void ndr_run(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const BYTE *format);
void ndr_run_pointer(MIDL_STUB_MESSAGE *m, ndr_op op, void **slot, const BYTE *format);
void ndr_interface(MIDL_STUB_MESSAGE *m, ndr_op op, BYTE *memory, const IID *iid);
#define NDR_TRACKS_MEMORY ((ULONG_PTR)0x4d454d4c)
void ndr_release_block(MIDL_STUB_MESSAGE *m, void *block);
void ndr_release_all(MIDL_STUB_MESSAGE *m);
SIZE_T ndr_memory_size(MIDL_STUB_MESSAGE *m, const BYTE *format);
BYTE *ndr_flat_start(MIDL_STUB_MESSAGE *m, const BYTE *format);
SIZE_T ndr_fixed_size(MIDL_STUB_MESSAGE *m, const BYTE *format);
SIZE_T ndr_out_size(MIDL_STUB_MESSAGE *m, BYTE *args, const BYTE *format);
void *rpc_stubless_thunk(ULONG index);
void *rpc_delegate_thunk(ULONG index);
void *rpc_delegate_vtable(void);

LONG rpc_guard(void (*call)(void *), void *arg);
char *rpc_narrow(const WCHAR *wide);
WCHAR *rpc_widen(const char *narrow);
WCHAR *rpc_copy(const WCHAR *text);
void rpc_free(void *block);

// ---- connections and bindings ----

#define PDU_REQUEST 0
#define PDU_RESPONSE 2
#define PDU_FAULT 3
#define PDU_BIND 11
#define PDU_BIND_ACK 12
#define PDU_BIND_NAK 13
#define PDU_ALTER_CONTEXT 14
#define PDU_ALTER_CONTEXT_RESP 15
#define PDU_SHUTDOWN 17
#define PDU_CANCEL 18
#define PDU_ORPHANED 19

#define PDU_FIRST 0x01
#define PDU_LAST 0x02
#define PDU_OBJECT_UUID 0x80

#define PDU_FRAGMENT_SIZE 5840

typedef struct rpc_stream
{
	HANDLE pipe;
	ULONG_PTR socket;
	BOOL is_socket;
	ULONG_PTR client_pid;
} rpc_stream;

typedef struct rpc_pdu
{
	BYTE type, flags, drep[4];
	USHORT frag_length, auth_length;
	ULONG call_id;
	BYTE *body;	// frag_length - 16 bytes
} rpc_pdu;

BOOL stream_read(rpc_stream *stream, void *data, ULONG size);
BOOL stream_write(rpc_stream *stream, const void *data, ULONG size);
void stream_close(rpc_stream *stream);
RPC_STATUS stream_connect(rpc_stream *stream, const WCHAR *protseq, const WCHAR *network, const WCHAR *endpoint);
BOOL protseq_supported(const WCHAR *protseq);
BOOL pdu_read(rpc_stream *stream, rpc_pdu *pdu);
BOOL pdu_write(rpc_stream *stream, BYTE type, BYTE flags, ULONG call_id, const void *head, ULONG head_size, const void *data, ULONG data_size);

typedef struct rpc_listener
{
	BOOL is_socket;
	ULONG_PTR socket;
	HANDLE pending;		// the pipe instance waiting for its client
	WCHAR pipe_name[MAX_PATH];
} rpc_listener;

RPC_STATUS listener_open(rpc_listener *listener, const WCHAR *protseq, const WCHAR *endpoint, WCHAR **resolved);
void listener_pause(rpc_listener *listener);
void listener_resume(rpc_listener *listener);
BOOL listener_accept(rpc_listener *listener, rpc_stream *stream);
void listener_wake(rpc_listener *listener);

typedef struct rpc_bound_context
{
	RPC_SYNTAX_IDENTIFIER abstract;
	USHORT id;
	struct rpc_bound_context *next;
} rpc_bound_context;

typedef struct rpc_connection
{
	rpc_stream stream;
	rpc_bound_context *contexts;
	USHORT next_context;
	struct rpc_connection *next;
} rpc_connection;

#define RPC_BINDING_MAGIC 0x444e4252

typedef struct rpc_binding
{
	DWORD magic;
	BOOL server;
	WCHAR *protseq, *network, *endpoint, *options;
	UUID object;
	BOOL has_object;
	SRWLOCK lock;
	rpc_connection *idle;
	ULONG call_id;
	ULONG auth_level, auth_service, authz_service;
	WCHAR *principal;
	RPC_AUTH_IDENTITY_HANDLE identity;
	// what a server's handle knows of the call it stands for
	struct rpc_server_call *call;
	const BYTE *request;
	ULONG request_size;
} rpc_binding;

rpc_binding *rpc_binding_of(RPC_BINDING_HANDLE handle);

typedef struct rpc_server_call
{
	rpc_stream *stream;
	ULONG call_id;
	USHORT context;
	BYTE *reply;
	ULONG reply_capacity;
	BOOL cancelled;
} rpc_server_call;

BOOL rpc_in_request(const void *pointer);
RPC_STATUS rpc_server_get_buffer(RPC_MESSAGE *message);
RPC_STATUS rpc_client_get_buffer(RPC_MESSAGE *message);

#endif
