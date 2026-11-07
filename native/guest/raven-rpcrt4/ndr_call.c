/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

// ---- setting up and running a call ----

static void init_common(MIDL_STUB_MESSAGE *m, RPC_MESSAGE *rpc, PMIDL_STUB_DESC desc)
{
	m->RpcMsg = rpc;
	m->StubDesc = desc;
	m->pfnAllocate = desc->pfnAllocate;
	m->pfnFree = desc->pfnFree;
	m->pAllocAllNodesContext = NULL;
	m->pPointerQueueState = NULL;
	m->IgnoreEmbeddedPointers = 0;
	m->PointerBufferMark = NULL;
	m->uFlags = 0;
	m->StackTop = NULL;
	m->FullPtrRefId = 0;
	m->UniquePtrCount = 0;
	m->PointerLength = 0;
	m->fInDontFree = 0;
	m->fDontCallFreeInst = 0;
	m->fHasReturn = 0;
	m->fHasExtensions = 0;
	m->fHasNewCorrDesc = 0;
	m->fIsIn = 0;
	m->fIsOut = 0;
	m->fIsOicf = 0;
	m->fBufferValid = 0;
	m->fNeedMCCP = 0;
	m->fHasMemoryValidateCallback = 0;
	m->fUnused2 = 0;
	m->dwDestContext = MSHCTX_DIFFERENTMACHINE;
	m->pvDestContext = NULL;
	m->pRpcChannelBuffer = NULL;
	m->pArrayInfo = NULL;
	m->dwStubPhase = 0;
	m->pAsyncMsg = NULL;
	m->pCorrInfo = NULL;
	m->pCorrMemory = NULL;
	m->pMemoryList = NULL;
	m->CorrDespIncrement = 0;
	m->ReuseBuffer = 0;
}

void RPC_ENTRY NdrClientInitializeNew(PRPC_MESSAGE rpc, PMIDL_STUB_MESSAGE m, PMIDL_STUB_DESC desc, unsigned int proc)
{
	rpc->Handle = NULL;
	rpc->ProcNum = proc | RPC_FLAGS_VALID_BIT;
	rpc->RpcInterfaceInformation = desc->RpcInterfaceInformation;
	rpc->RpcFlags = 0;
	m->BufferStart = NULL;
	m->BufferEnd = NULL;
	m->BufferLength = 0;
	m->IsClient = TRUE;
	init_common(m, rpc, desc);
}

unsigned char *RPC_ENTRY NdrServerInitializeNew(PRPC_MESSAGE rpc, PMIDL_STUB_MESSAGE m, PMIDL_STUB_DESC desc)
{
	init_common(m, rpc, desc);
	m->Buffer = m->BufferStart = rpc->Buffer;
	m->BufferEnd = (BYTE *)rpc->Buffer + rpc->BufferLength;
	m->BufferLength = 0;
	m->IsClient = FALSE;
	m->FullPtrXlatTables = NULL;
	return NULL;
}

unsigned char *RPC_ENTRY NdrGetBuffer(PMIDL_STUB_MESSAGE m, ULONG length, RPC_BINDING_HANDLE handle)
{
	m->RpcMsg->Handle = handle;
	m->RpcMsg->BufferLength = length;
	RPC_STATUS status = I_RpcGetBuffer(m->RpcMsg);
	if (status != RPC_S_OK)
		RpcRaiseException(status);
	m->Buffer = m->RpcMsg->Buffer;
	m->fBufferValid = TRUE;
	m->SavedHandle = handle;
	return m->Buffer;
}

void RPC_ENTRY NdrFreeBuffer(PMIDL_STUB_MESSAGE m)
{
	if (!m->fBufferValid)
		return;
	I_RpcFreeBuffer(m->RpcMsg);
	m->fBufferValid = FALSE;
}

unsigned char *RPC_ENTRY NdrSendReceive(PMIDL_STUB_MESSAGE m, unsigned char *buffer_end)
{
	if (buffer_end && buffer_end >= (BYTE *)m->RpcMsg->Buffer)
		m->RpcMsg->BufferLength = (ULONG)(buffer_end - (BYTE *)m->RpcMsg->Buffer);
	RPC_STATUS status = I_RpcSendReceive(m->RpcMsg);
	if (status != RPC_S_OK) {
		m->fBufferValid = FALSE;
		RpcRaiseException(status);
	}
	m->Buffer = m->BufferStart = m->RpcMsg->Buffer;
	m->BufferEnd = m->BufferStart + m->RpcMsg->BufferLength;
	m->BufferLength = m->RpcMsg->BufferLength;
	return NULL;
}

void RPC_ENTRY NdrRpcSmSetClientToOsf(PMIDL_STUB_MESSAGE m)
{
	(void)m;
}

void *RPC_ENTRY NdrOleAllocate(size_t size)
{
	return CoTaskMemAlloc(size);
}

void RPC_ENTRY NdrOleFree(void *block)
{
	CoTaskMemFree(block);
}
