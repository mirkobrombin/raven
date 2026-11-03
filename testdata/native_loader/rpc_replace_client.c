#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <rpc.h>
#include "rpc_replace.h"

/* How a server stub frees what it unmarshalled. A manager may free what an [in, out] pointer points to and hand back a block of its
 * own: the stub then frees that block, never the one already given back. A structure behind a pointer goes after the pointers it
 * holds, which are read from it, not from a freed block. Every block handed out is recorded, and a freed one is filled with a pattern
 * and never reused, so a free of anything not live shows here instead of corrupting the heap of a later caller. */
static void *live[256];
static int live_count, bad_frees;

void *__RPC_USER MIDL_user_allocate(size_t size)
{
	void *block = HeapAlloc(GetProcessHeap(), 0, size);
	if (block && live_count < 256)
		live[live_count++] = block;
	return block;
}

void __RPC_USER MIDL_user_free(void *block)
{
	if (!block)
		return;
	for (int i = 0; i < live_count; i++) {
		if (live[i] == block) {
			live[i] = live[--live_count];
			SIZE_T size = HeapSize(GetProcessHeap(), 0, block);
			for (SIZE_T at = 0; size != (SIZE_T)-1 && at < size; at++)
				((BYTE *)block)[at] = 0x5a;
			return;
		}
	}
	bad_frees++;
}

void server_replace_name(handle_t binding, named *item)
{
	(void)binding;
	MIDL_user_free(item->name);
	item->name = MIDL_user_allocate(sizeof(L"replaced"));
	if (item->name)
		lstrcpyW(item->name, L"replaced");
}

static int names_seen;

void server_take_name(handle_t binding, named *item)
{
	(void)binding;
	if (item && item->name && !lstrcmpW(item->name, L"taken"))
		names_seen++;
}

void server_take_holder(handle_t binding, holder *item)
{
	(void)binding;
	if (item->inner && item->inner->name && !lstrcmpW(item->inner->name, L"held"))
		names_seen++;
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
	if (bad_frees) return 10 + bad_frees;
	for (int round = 0; round < 8; round++) {
		named item = {(wchar_t *)L"taken"};
		take_name(binding, &item);
	}
	if (names_seen != 8) return 8;
	for (int round = 0; round < 8; round++) {
		named inner = {(wchar_t *)L"held"};
		holder item = {&inner};
		take_holder(binding, &item);
	}
	if (names_seen != 16) return 9;
	RpcBindingFree(&binding);
	return bad_frees ? 40 + bad_frees : 0;
}
