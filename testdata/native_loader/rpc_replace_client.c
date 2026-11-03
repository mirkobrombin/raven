#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <rpc.h>
#include "rpc_replace.h"

/* A server manager may free what the stub unmarshalled for an [in, out] pointer and hand back a block of its own; the stub frees what
 * the parameter holds after the reply, and must not free the block the manager already gave back. Every free is recorded, so a block
 * freed twice shows here instead of corrupting the heap of a later caller. */
static void *freed[64];
static int freed_count, freed_twice;

void *__RPC_USER MIDL_user_allocate(size_t size)
{
	void *block = HeapAlloc(GetProcessHeap(), 0, size);
	for (int i = 0; i < freed_count; i++)
		if (freed[i] == block)
			freed[i] = freed[--freed_count];
	return block;
}

void __RPC_USER MIDL_user_free(void *block)
{
	if (!block)
		return;
	for (int i = 0; i < freed_count; i++) {
		if (freed[i] == block) {
			freed_twice++;
			return;
		}
	}
	if (freed_count < 64)
		freed[freed_count++] = block;
	HeapFree(GetProcessHeap(), 0, block);
}

void server_replace_name(handle_t binding, named *item)
{
	(void)binding;
	MIDL_user_free(item->name);
	item->name = MIDL_user_allocate(sizeof(L"replaced"));
	if (item->name)
		lstrcpyW(item->name, L"replaced");
}

int raven_entry(void)
{
	if (RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)L"raven-replace", NULL) != RPC_S_OK) return 1;
	if (RpcServerRegisterIf(server_rpc_replace_v1_0_s_ifspec, NULL, NULL) != RPC_S_OK) return 2;
	if (RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE) != RPC_S_OK) return 3;
	RPC_WSTR text = NULL;
	RPC_BINDING_HANDLE binding = NULL;
	if (RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL, (RPC_WSTR)L"raven-replace", NULL, &text) != RPC_S_OK) return 4;
	if (RpcBindingFromStringBindingW(text, &binding) != RPC_S_OK) return 5;
	RpcStringFreeW(&text);
	for (int round = 0; round < 8; round++) {
		named item = {MIDL_user_allocate(sizeof(L"original"))};
		if (!item.name) return 6;
		lstrcpyW(item.name, L"original");
		replace_name(binding, &item);
		if (!item.name || lstrcmpW(item.name, L"replaced")) return 7;
		MIDL_user_free(item.name);
	}
	RpcBindingFree(&binding);
	return freed_twice ? 10 + freed_twice : 0;
}
