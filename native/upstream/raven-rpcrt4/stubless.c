/* SPDX-License-Identifier: MIT */
#include "rpc_own.h"

// The interpreter of Oif procedure strings, as the NDR format string documentation lays them out: a header, a handle
// description, sizes, flags, and one descriptor per parameter. Both ends walk the same descriptors in different order.

#define PARAM_MUST_FREE 0x0002
#define PARAM_IS_PIPE 0x0004
#define PARAM_IS_IN 0x0008
#define PARAM_IS_OUT 0x0010
#define PARAM_IS_RETURN 0x0020
#define PARAM_IS_BASE 0x0040
#define PARAM_BY_VALUE 0x0080
#define PARAM_SIMPLE_REF 0x0100
#define PARAM_ALLOC_MASK 0xe000

#define OI_FULL_POINTERS 0x01
#define OI_OBJECT 0x04
#define OI_HAS_RPC_FLAGS 0x08

#define OI2_HAS_EXTENSIONS 0x40
#define OI2_HAS_ASYNC_HANDLE 0x80

typedef struct proc_info
{
	BYTE handle_type, oi_flags, oi2_flags, param_count;
	USHORT proc_num, stack_size;
	ULONG rpc_flags;
	const BYTE *handle_desc;
	const BYTE *params;
	BOOL has_correlation;
} proc_info;

static USHORT read_u16(const BYTE *at)
{
	return (USHORT)(at[0] | at[1] << 8);
}

static void parse_proc(const BYTE *format, proc_info *proc)
{
	const BYTE *at = format;
	memset(proc, 0, sizeof(*proc));
	proc->handle_type = *at++;
	proc->oi_flags = *at++;
	if (proc->oi_flags & OI_HAS_RPC_FLAGS) {
		proc->rpc_flags = at[0] | at[1] << 8 | at[2] << 16 | (ULONG)at[3] << 24;
		at += 4;
	}
	proc->proc_num = read_u16(at);
	proc->stack_size = read_u16(at + 2);
	at += 4;
	if (!proc->handle_type) {
		proc->handle_desc = at;
		switch (*at) {
		case FC_BIND_PRIMITIVE:
			at += 4;
			break;
		default:
			at += 6;
			break;
		}
	}
	at += 4;	// the constant buffer sizes, which sizing everything makes unnecessary
	proc->oi2_flags = *at++;
	proc->param_count = *at++;
	if (proc->oi2_flags & OI2_HAS_EXTENSIONS) {
		BYTE extension = *at;
		// flags2 tells whether the conformance descriptors grew.
		proc->has_correlation = (at[1] & 0x07) != 0;
		at += extension;
	}
	proc->params = at;
}

typedef struct param_view
{
	USHORT attributes, offset;
	BYTE base;
	const BYTE *type;
} param_view;

static const BYTE *next_param(const BYTE *at, const MIDL_STUB_DESC *desc, param_view *param)
{
	param->attributes = read_u16(at);
	param->offset = read_u16(at + 2);
	if (param->attributes & PARAM_IS_BASE) {
		param->base = at[4];
		param->type = NULL;
	} else {
		param->base = 0;
		param->type = desc->pFormatTypes + read_u16(at + 4);
	}
	return at + 6;
}

static const BYTE base_formats[0x100][2] = {
#define B(x) [x] = {x, FC_PAD}
	B(FC_BYTE), B(FC_CHAR), B(FC_SMALL), B(FC_USMALL), B(FC_WCHAR), B(FC_SHORT), B(FC_USHORT), B(FC_LONG), B(FC_ULONG),
	B(FC_FLOAT), B(FC_HYPER), B(FC_DOUBLE), B(FC_ENUM16), B(FC_ENUM32), B(FC_IGNORE), B(FC_ERROR_STATUS_T),
	B(FC_INT3264), B(FC_UINT3264),
#undef B
};

static BOOL type_uses_slot(const BYTE *type)
{
	switch (*type) {
	case FC_RP: case FC_UP: case FC_OP: case FC_FP: case FC_IP:
		return TRUE;
	}
	return FALSE;
}

// Larger structures travel by reference in the x64 convention, even when the source says by value.
static BOOL by_value_indirect(const MIDL_STUB_MESSAGE *m, const BYTE *type)
{
#ifdef _WIN64
	SIZE_T size = ndr_fixed_size((MIDL_STUB_MESSAGE *)m, type);
	return size != 1 && size != 2 && size != 4 && size != 8;
#else
	(void)m; (void)type;
	return FALSE;
#endif
}

// The memory a walk should get for this parameter, whose slot on the argument stack is given.
static BYTE *param_memory(MIDL_STUB_MESSAGE *m, const param_view *param, void **slot)
{
	if (param->attributes & PARAM_IS_BASE)
		return param->attributes & PARAM_SIMPLE_REF ? *(BYTE **)slot : (BYTE *)slot;
	if (param->attributes & PARAM_BY_VALUE)
		return by_value_indirect(m, param->type) ? *(BYTE **)slot : (BYTE *)slot;
	if (param->attributes & PARAM_SIMPLE_REF)
		return *(BYTE **)slot;
	return type_uses_slot(param->type) ? (BYTE *)slot : *(BYTE **)slot;
}

static const BYTE *param_format(const param_view *param)
{
	return param->attributes & PARAM_IS_BASE ? base_formats[param->base] : param->type;
}

// ---- context handles among the parameters ----

static BOOL is_context(const param_view *param)
{
	return param->type && *param->type == FC_BIND_CONTEXT;
}

static void client_context_marshal(MIDL_STUB_MESSAGE *m, const param_view *param, void **slot)
{
	NDR_CCONTEXT context = param->type[1] & HANDLE_PARAM_IS_VIA_PTR ? *(NDR_CCONTEXT *)*slot : *(NDR_CCONTEXT *)slot;
	if (param->type[1] & NDR_CONTEXT_HANDLE_CANNOT_BE_NULL && !context)
		RpcRaiseException(RPC_X_SS_IN_NULL_CONTEXT);
	NDRCContextMarshall(context, m->Buffer);
	m->Buffer += 20;
}

static void client_context_unmarshal(MIDL_STUB_MESSAGE *m, const param_view *param, void **slot, void *retval)
{
	if (m->Buffer + 20 > m->BufferEnd)
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	NDR_CCONTEXT *target = param->attributes & PARAM_IS_RETURN ? (NDR_CCONTEXT *)retval : (NDR_CCONTEXT *)*slot;
	if (param->attributes & PARAM_IS_RETURN)
		*target = NULL;
	NDRCContextUnmarshall(target, m->SavedHandle, m->Buffer, m->RpcMsg->DataRepresentation);
	m->Buffer += 20;
}

// ---- finding the handle of a call ----

typedef struct bound_handle
{
	RPC_BINDING_HANDLE handle;
	GENERIC_BINDING_ROUTINE_PAIR *pair;
	void *object;
} bound_handle;

static void bind_call(const MIDL_STUB_DESC *desc, const proc_info *proc, BYTE *args, bound_handle *bound)
{
	memset(bound, 0, sizeof(*bound));
	switch (proc->handle_type) {
	case FC_AUTO_HANDLE:
		bound->handle = *desc->IMPLICIT_HANDLE_INFO.pAutoHandle;
		return;
	case FC_BIND_PRIMITIVE:
	case FC_CALLBACK_HANDLE:
		bound->handle = *desc->IMPLICIT_HANDLE_INFO.pPrimitiveHandle;
		return;
	case FC_BIND_GENERIC: {
		GENERIC_BINDING_INFO *info = desc->IMPLICIT_HANDLE_INFO.pGenericBindingInfo;
		bound->object = info->pObj;
		bound->handle = ((GENERIC_BINDING_ROUTINE)info->pfnBind)(info->pObj);
		return;
	}
	case 0:
		break;
	default:
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	}
	const BYTE *handle = proc->handle_desc;
	BYTE *slot = args + read_u16(handle + 2);
	switch (*handle) {
	case FC_BIND_PRIMITIVE:
		bound->handle = handle[1] & HANDLE_PARAM_IS_VIA_PTR ? **(RPC_BINDING_HANDLE **)slot : *(RPC_BINDING_HANDLE *)slot;
		return;
	case FC_BIND_GENERIC: {
		BYTE flag_size = handle[1], size = flag_size & 0x0f;
		bound->pair = &desc->aGenericBindingRoutinePairs[handle[4]];
		if (flag_size & HANDLE_PARAM_IS_VIA_PTR) {
			bound->object = **(void ***)slot;
		} else {
			bound->object = NULL;
			memcpy(&bound->object, slot, size > sizeof(void *) ? sizeof(void *) : size);
		}
		bound->handle = ((GENERIC_BINDING_ROUTINE)bound->pair->pfnBind)(bound->object);
		return;
	}
	case FC_BIND_CONTEXT: {
		NDR_CCONTEXT context = handle[1] & HANDLE_PARAM_IS_VIA_PTR ? **(NDR_CCONTEXT **)slot : *(NDR_CCONTEXT *)slot;
		if (!context && handle[1] & NDR_CONTEXT_HANDLE_CANNOT_BE_NULL)
			RpcRaiseException(RPC_X_SS_IN_NULL_CONTEXT);
		bound->handle = context ? NDRCContextBinding(context) : NULL;
		if (!bound->handle)
			RpcRaiseException(RPC_X_SS_IN_NULL_CONTEXT);
		return;
	}
	}
	RpcRaiseException(RPC_X_BAD_STUB_DATA);
}

static void unbind_call(const MIDL_STUB_DESC *desc, const proc_info *proc, bound_handle *bound)
{
	if (bound->pair)
		((GENERIC_UNBIND_ROUTINE)bound->pair->pfnUnbind)(bound->object, bound->handle);
	else if (!proc->handle_type || proc->handle_type != FC_BIND_GENERIC)
		return;
	else if (desc->IMPLICIT_HANDLE_INFO.pGenericBindingInfo)
		((GENERIC_UNBIND_ROUTINE)desc->IMPLICIT_HANDLE_INFO.pGenericBindingInfo->pfnUnbind)(bound->object, bound->handle);
}

// ---- the client's side ----

static void marshal_in(MIDL_STUB_MESSAGE *m, const MIDL_STUB_DESC *desc, const proc_info *proc, BYTE *args, ndr_op op)
{
	const BYTE *at = proc->params;
	for (BYTE i = 0; i < proc->param_count; i++) {
		param_view param;
		at = next_param(at, desc, &param);
		if (!(param.attributes & PARAM_IS_IN) || param.attributes & PARAM_IS_PIPE)
			continue;
		void **slot = (void **)(args + param.offset);
		if (is_context(&param)) {
			if (op == NDR_SIZE) {
				m->BufferLength = (m->BufferLength + 3) & ~3u;
				m->BufferLength += 20;
			} else {
				m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
				client_context_marshal(m, &param, slot);
			}
			continue;
		}
		m->MaxCount = 0;
		ndr_run(m, op, param_memory(m, &param, slot), param_format(&param));
	}
}

static void unmarshal_out(MIDL_STUB_MESSAGE *m, const MIDL_STUB_DESC *desc, const proc_info *proc, BYTE *args, void *retval)
{
	const BYTE *at = proc->params;
	for (BYTE i = 0; i < proc->param_count; i++) {
		param_view param;
		at = next_param(at, desc, &param);
		if (!(param.attributes & (PARAM_IS_OUT | PARAM_IS_RETURN)) || param.attributes & PARAM_IS_PIPE)
			continue;
		void **slot = (void **)(args + param.offset);
		if (is_context(&param)) {
			m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
			client_context_unmarshal(m, &param, slot, retval);
			continue;
		}
		BYTE *memory;
		if (param.attributes & PARAM_IS_RETURN)
			memory = retval;
		else
			memory = param_memory(m, &param, slot);
		if (param.attributes & PARAM_IS_RETURN && param.type && !type_uses_slot(param.type) && !(param.attributes & PARAM_BY_VALUE)) {
			*(void **)retval = NULL;
			NdrPointerUnmarshall(m, (BYTE **)retval, param.type, 0);
			continue;
		}
		ndr_run(m, NDR_UNMARSHAL, memory, param_format(&param));
	}
}

// Through the ellipsis of the generated stub C has turned every float into a double.
static void narrow_floats(const MIDL_STUB_DESC *desc, const proc_info *proc, BYTE *args)
{
#ifdef _WIN64
	const BYTE *at = proc->params;
	for (BYTE i = 0; i < proc->param_count; i++) {
		param_view param;
		at = next_param(at, desc, &param);
		if (param.attributes & PARAM_IS_BASE && param.base == FC_FLOAT && !(param.attributes & (PARAM_SIMPLE_REF | PARAM_IS_RETURN))) {
			double value;
			memcpy(&value, args + param.offset, sizeof(value));
			float narrow = (float)value;
			memcpy(args + param.offset, &narrow, sizeof(narrow));
		}
	}
#else
	(void)desc; (void)proc; (void)args;
#endif
}

static LONG_PTR run_client_call(const MIDL_STUB_DESC *desc, const BYTE *format, BYTE *args)
{
	proc_info proc;
	parse_proc(format, &proc);
	narrow_floats(desc, &proc, args);
	MIDL_STUB_MESSAGE m;
	RPC_MESSAGE rpc;
	NdrClientInitializeNew(&rpc, &m, (PMIDL_STUB_DESC)desc, proc.proc_num);
	m.StackTop = args;
	if (proc.rpc_flags)
		rpc.RpcFlags = proc.rpc_flags;
	if (proc.has_correlation)
		NdrCorrelationInitialize(&m, NULL, 0, 0);
	m.FullPtrXlatTables = proc.oi_flags & OI_FULL_POINTERS ? NdrFullPointerXlatInit(0, XLAT_CLIENT) : NULL;
	ULONGLONG retval = 0;
	bound_handle bound;
	bind_call(desc, &proc, args, &bound);
	m.BufferLength = 0;
	marshal_in(&m, desc, &proc, args, NDR_SIZE);
	NdrGetBuffer(&m, m.BufferLength, bound.handle);
	m.SavedHandle = bound.handle;
	marshal_in(&m, desc, &proc, args, NDR_MARSHAL);
	NdrSendReceive(&m, m.Buffer);
	m.fBufferValid = TRUE;
	if (m.FullPtrXlatTables) {
		NdrFullPointerXlatFree(m.FullPtrXlatTables);
		m.FullPtrXlatTables = NdrFullPointerXlatInit(0, XLAT_CLIENT);
	}
	unmarshal_out(&m, desc, &proc, args, &retval);
	NdrFreeBuffer(&m);
	if (m.FullPtrXlatTables)
		NdrFullPointerXlatFree(m.FullPtrXlatTables);
	unbind_call(desc, &proc, &bound);
	return (LONG_PTR)retval;
}

// The generated stub passes the original arguments after these two (x64), or a pointer to them (x86).
__attribute__((noinline)) CLIENT_CALL_RETURN RPC_VAR_ENTRY NdrClientCall2(PMIDL_STUB_DESC desc, PFORMAT_STRING format, ...)
{
	__builtin_va_list list;
	__builtin_va_start(list, format);
	CLIENT_CALL_RETURN result;
#ifdef _WIN64
	BYTE *args = (BYTE *)list;
#else
	BYTE *args = __builtin_va_arg(list, BYTE *);
#endif
	__builtin_va_end(list);
	result.Pointer = (void *)run_client_call(desc, format, args);
	return result;
}

CLIENT_CALL_RETURN RPC_VAR_ENTRY NdrClientCall(PMIDL_STUB_DESC desc, PFORMAT_STRING format, ...)
{
	__builtin_va_list list;
	__builtin_va_start(list, format);
	CLIENT_CALL_RETURN result;
#ifdef _WIN64
	BYTE *args = (BYTE *)list;
#else
	BYTE *args = __builtin_va_arg(list, BYTE *);
#endif
	__builtin_va_end(list);
	result.Pointer = (void *)run_client_call(desc, format, args);
	return result;
}

CLIENT_CALL_RETURN RPC_VAR_ENTRY NdrClientCall3(MIDL_STUBLESS_PROXY_INFO *info, ULONG proc, void *ret, ...)
{
	__builtin_va_list list;
	__builtin_va_start(list, ret);
	CLIENT_CALL_RETURN result;
	const BYTE *format = info->ProcFormatString + info->FormatStringOffset[proc];
#ifdef _WIN64
	BYTE *args = (BYTE *)list;
#else
	BYTE *args = __builtin_va_arg(list, BYTE *);
#endif
	__builtin_va_end(list);
	result.Pointer = (void *)run_client_call(info->pStubDesc, format, args);
	if (ret)
		*(void **)ret = result.Pointer;
	return result;
}

// Asynchronous calls keep to the protocol of the documentation: the call returns at once and the outcome is collected
// through RpcAsyncCompleteCall. The exchange runs on a thread of its own.

typedef struct async_call
{
	MIDL_STUB_MESSAGE m;
	RPC_MESSAGE rpc;
	proc_info proc;
	const MIDL_STUB_DESC *desc;
	const BYTE *format;
	BYTE *args;
	RPC_ASYNC_STATE *state;
	RPC_STATUS status;
	HANDLE thread;
	bound_handle bound;
} async_call;

static void async_notify(RPC_ASYNC_STATE *state)
{
	switch (state->NotificationType) {
	case RpcNotificationTypeEvent:
		SetEvent(state->u.hEvent);
		break;
	case RpcNotificationTypeApc:
		QueueUserAPC((PAPCFUNC)state->u.APC.NotificationRoutine, state->u.APC.hThread, (ULONG_PTR)state);
		break;
	case RpcNotificationTypeIoc:
		PostQueuedCompletionStatus(state->u.IOC.hIOPort, state->u.IOC.dwNumberOfBytesTransferred, state->u.IOC.dwCompletionKey, state->u.IOC.lpOverlapped);
		break;
	case RpcNotificationTypeHwnd:
		PostMessageW(state->u.HWND.hWnd, state->u.HWND.Msg, 0, 0);
		break;
	case RpcNotificationTypeCallback:
		state->u.NotificationRoutine(state, NULL, RpcCallComplete);
		break;
	default:
		break;
	}
}

static DWORD WINAPI async_thread(void *arg)
{
	async_call *call = arg;
	call->status = I_RpcSendReceive(&call->rpc);
	async_notify(call->state);
	return 0;
}

CLIENT_CALL_RETURN RPC_VAR_ENTRY NdrAsyncClientCall(PMIDL_STUB_DESC desc, PFORMAT_STRING format, ...)
{
	__builtin_va_list list;
	__builtin_va_start(list, format);
#ifdef _WIN64
	BYTE *stack = (BYTE *)list;
#else
	BYTE *stack = __builtin_va_arg(list, BYTE *);
#endif
	__builtin_va_end(list);
	CLIENT_CALL_RETURN result;
	result.Pointer = NULL;
	async_call *call = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*call));
	if (!call)
		RpcRaiseException(RPC_S_OUT_OF_MEMORY);
	parse_proc(format, &call->proc);
	call->desc = desc;
	call->format = format;
	call->args = HeapAlloc(GetProcessHeap(), 0, call->proc.stack_size ? call->proc.stack_size : sizeof(void *));
	memcpy(call->args, stack, call->proc.stack_size);
	// By the Oif rules the first parameter of an asynchronous procedure is its state.
	call->state = *(RPC_ASYNC_STATE **)call->args;
	call->state->StubInfo = call;
	call->m.RpcMsg = &call->rpc;
	NdrClientInitializeNew(&call->rpc, &call->m, desc, call->proc.proc_num);
	call->m.StackTop = call->args;
	if (call->proc.has_correlation)
		NdrCorrelationInitialize(&call->m, NULL, 0, 0);
	bind_call(desc, &call->proc, call->args, &call->bound);
	marshal_in(&call->m, desc, &call->proc, call->args, NDR_SIZE);
	NdrGetBuffer(&call->m, call->m.BufferLength, call->bound.handle);
	marshal_in(&call->m, desc, &call->proc, call->args, NDR_MARSHAL);
	call->rpc.BufferLength = (ULONG)(call->m.Buffer - (BYTE *)call->rpc.Buffer);
	call->thread = CreateThread(NULL, 0, async_thread, call, 0, NULL);
	if (!call->thread)
		RpcRaiseException(RPC_S_OUT_OF_RESOURCES);
	return result;
}

RPC_STATUS rpc_async_status(RPC_ASYNC_STATE *state)
{
	async_call *call = state->StubInfo;
	if (!call)
		return RPC_S_INVALID_BINDING;
	return WaitForSingleObject(call->thread, 0) == WAIT_OBJECT_0 ? call->status : RPC_S_ASYNC_CALL_PENDING;
}

RPC_STATUS rpc_async_complete(RPC_ASYNC_STATE *state, void *reply)
{
	async_call *call = state->StubInfo;
	if (!call)
		return RPC_S_INVALID_ASYNC_HANDLE;
	WaitForSingleObject(call->thread, INFINITE);
	CloseHandle(call->thread);
	RPC_STATUS status = call->status;
	if (status == RPC_S_OK) {
		call->m.Buffer = call->m.BufferStart = call->rpc.Buffer;
		call->m.BufferEnd = call->m.BufferStart + call->rpc.BufferLength;
		ULONGLONG retval = 0;
		unmarshal_out(&call->m, call->desc, &call->proc, call->args, &retval);
		if (reply)
			memcpy(reply, &retval, sizeof(void *));
		I_RpcFreeBuffer(&call->rpc);
	}
	unbind_call(call->desc, &call->proc, &call->bound);
	state->StubInfo = NULL;
	rpc_free(call->args);
	rpc_free(call);
	return status;
}

// ---- the server's side ----

LONG_PTR rpc_call_manager(void *function, void *args, SIZE_T stack_size);

#ifdef _WIN64
// The stack is copied below the home area, and the first four slots load both register files, as a callee reads the one
// its prototype asks for. A float is in the low half of its slot either way.
__asm__(
	".text\n"
	".globl rpc_call_manager\n"
	".def rpc_call_manager; .scl 2; .type 32; .endef\n"
	".seh_proc rpc_call_manager\n"
	"rpc_call_manager:\n"
	"	pushq %rbp\n"
	"	.seh_pushreg %rbp\n"
	"	movq %rsp, %rbp\n"
	"	.seh_setframe %rbp, 0\n"
	"	.seh_endprologue\n"
	"	movq %rcx, %rax\n"
	"	movq %rdx, %r10\n"
	"	cmpq $32, %r8\n"
	"	jae 1f\n"
	"	movq $32, %r8\n"
	"1:\n"
	"	addq $15, %r8\n"
	"	andq $-16, %r8\n"
	"	subq %r8, %rsp\n"
	"	movq %rsi, %r11\n"
	"	movq %rdi, %r9\n"
	"	movq %r10, %rsi\n"
	"	movq %rsp, %rdi\n"
	"	movq %r8, %rcx\n"
	"	shrq $3, %rcx\n"
	"	rep movsq\n"
	"	movq %r11, %rsi\n"
	"	movq %r9, %rdi\n"
	"	movq 0(%rsp), %rcx\n"
	"	movq 8(%rsp), %rdx\n"
	"	movq 16(%rsp), %r8\n"
	"	movq 24(%rsp), %r9\n"
	"	movsd 0(%rsp), %xmm0\n"
	"	movsd 8(%rsp), %xmm1\n"
	"	movsd 16(%rsp), %xmm2\n"
	"	movsd 24(%rsp), %xmm3\n"
	"	callq *%rax\n"
	"	movq %rbp, %rsp\n"
	"	popq %rbp\n"
	"	retq\n"
	"	.seh_endproc\n");
#else
// The callee cleans up after itself, or not, in either convention: esp comes back from ebp all the same.
__asm__(
	".text\n"
	".globl _rpc_call_manager\n"
	"_rpc_call_manager:\n"
	"	pushl %ebp\n"
	"	movl %esp, %ebp\n"
	"	pushl %esi\n"
	"	pushl %edi\n"
	"	movl 16(%ebp), %ecx\n"
	"	subl %ecx, %esp\n"
	"	andl $-16, %esp\n"
	"	movl 12(%ebp), %esi\n"
	"	movl %esp, %edi\n"
	"	cld\n"
	"	rep movsb\n"
	"	calll *8(%ebp)\n"
	"	leal -8(%ebp), %esp\n"
	"	popl %edi\n"
	"	popl %esi\n"
	"	popl %ebp\n"
	"	retl\n");
#endif

typedef struct param_extra
{
	ULONGLONG inline_value;		// where a by-reference base type lives during the call
	NDR_SCONTEXT context;
} param_extra;

static void server_unmarshal(MIDL_STUB_MESSAGE *m, const MIDL_STUB_DESC *desc, const proc_info *proc, BYTE *args, param_extra *extra)
{
	// In parameters first: an out parameter's size may come from them.
	const BYTE *at = proc->params;
	for (BYTE i = 0; i < proc->param_count; i++) {
		param_view param;
		at = next_param(at, desc, &param);
		void **slot = (void **)(args + param.offset);
		if (param.attributes & PARAM_IS_PIPE)
			RpcRaiseException(RPC_S_CANNOT_SUPPORT);
		if (!(param.attributes & PARAM_IS_IN))
			continue;
		if (is_context(&param)) {
			m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
			if (m->Buffer + 20 > m->BufferEnd)
				RpcRaiseException(RPC_X_BAD_STUB_DATA);
			extra[i].context = NDRSContextUnmarshall2(m->RpcMsg->Handle, m->Buffer, m->RpcMsg->DataRepresentation, (void *)m->StubDesc->RpcInterfaceInformation, param.type[1] & (RPC_CONTEXT_HANDLE_SERIALIZE | RPC_CONTEXT_HANDLE_DONT_SERIALIZE));
			m->Buffer += 20;
			if (param.type[1] & NDR_CONTEXT_HANDLE_CANNOT_BE_NULL && !NDRSContextValue(extra[i].context))
				RpcRaiseException(RPC_X_SS_CONTEXT_MISMATCH);
			*slot = param.type[1] & HANDLE_PARAM_IS_VIA_PTR ? (void *)NDRSContextValue(extra[i].context) : *NDRSContextValue(extra[i].context);
			continue;
		}
		if (param.attributes & PARAM_IS_BASE) {
			if (param.attributes & PARAM_SIMPLE_REF)
				*slot = &extra[i].inline_value;
			ndr_run(m, NDR_UNMARSHAL, param_memory(m, &param, slot), param_format(&param));
			continue;
		}
		if (param.attributes & PARAM_BY_VALUE) {
			if (by_value_indirect(m, param.type))
				*slot = NdrAllocate(m, ndr_memory_size(m, param.type));
			ndr_run(m, NDR_UNMARSHAL, param_memory(m, &param, slot), param.type);
			continue;
		}
		if (type_uses_slot(param.type)) {
			*slot = NULL;
			ndr_run(m, NDR_UNMARSHAL, (BYTE *)slot, param.type);
			continue;
		}
		// Flat data stays where it is in the buffer; anything else gets a block.
		BYTE *in_place = ndr_flat_start(m, param.type);
		*slot = in_place ? in_place : NdrAllocate(m, ndr_memory_size(m, param.type));
		if (!in_place)
			memset(*slot, 0, ndr_memory_size(m, param.type));
		ndr_run(m, NDR_UNMARSHAL, *(BYTE **)slot, param.type);
	}
	at = proc->params;
	for (BYTE i = 0; i < proc->param_count; i++) {
		param_view param;
		at = next_param(at, desc, &param);
		void **slot = (void **)(args + param.offset);
		if (param.attributes & PARAM_IS_IN || !(param.attributes & PARAM_IS_OUT))
			continue;
		if (is_context(&param)) {
			extra[i].context = NDRSContextUnmarshall2(m->RpcMsg->Handle, NULL, m->RpcMsg->DataRepresentation, (void *)m->StubDesc->RpcInterfaceInformation, 0);
			*slot = (void *)NDRSContextValue(extra[i].context);
			continue;
		}
		if (param.attributes & PARAM_IS_BASE) {
			*slot = &extra[i].inline_value;
			continue;
		}
		if (type_uses_slot(param.type) && *param.type != FC_IP) {
			const BYTE *target = param.type[1] & FC_SIMPLE_POINTER ? param.type + 2 : param.type + 2 + (SHORT)read_u16(param.type + 2);
			SIZE_T size = param.attributes & PARAM_ALLOC_MASK ? (SIZE_T)((param.attributes & PARAM_ALLOC_MASK) >> 13) * 8 : ndr_out_size(m, args, target);
			*slot = NdrAllocate(m, size);
			memset(*slot, 0, size);
			continue;
		}
		SIZE_T size = param.attributes & PARAM_ALLOC_MASK ? (SIZE_T)((param.attributes & PARAM_ALLOC_MASK) >> 13) * 8 : ndr_out_size(m, args, param.type);
		*slot = NdrAllocate(m, size);
		memset(*slot, 0, size);
	}
}

static void server_marshal(MIDL_STUB_MESSAGE *m, const MIDL_STUB_DESC *desc, const proc_info *proc, BYTE *args, param_extra *extra, void *retval, ndr_op op)
{
	const BYTE *at = proc->params;
	for (BYTE i = 0; i < proc->param_count; i++) {
		param_view param;
		at = next_param(at, desc, &param);
		if (!(param.attributes & (PARAM_IS_OUT | PARAM_IS_RETURN)))
			continue;
		void **slot = (void **)(args + param.offset);
		if (is_context(&param)) {
			if (op == NDR_SIZE) {
				m->BufferLength = (m->BufferLength + 3) & ~3u;
				m->BufferLength += 20;
				continue;
			}
			m->Buffer = (BYTE *)(((ULONG_PTR)m->Buffer + 3) & ~(ULONG_PTR)3);
			NDR_SCONTEXT context = extra[i].context;
			if (param.attributes & PARAM_IS_RETURN) {
				context = NDRSContextUnmarshall2(m->RpcMsg->Handle, NULL, m->RpcMsg->DataRepresentation, (void *)m->StubDesc->RpcInterfaceInformation, 0);
				*NDRSContextValue(context) = *(void **)retval;
			}
			NDR_RUNDOWN rundown = m->StubDesc->apfnNdrRundownRoutines ? m->StubDesc->apfnNdrRundownRoutines[param.type[2]] : NULL;
			NDRSContextMarshall2(m->RpcMsg->Handle, context, m->Buffer, rundown, (void *)m->StubDesc->RpcInterfaceInformation, 0);
			m->Buffer += 20;
			continue;
		}
		BYTE *memory = param.attributes & PARAM_IS_RETURN ? (BYTE *)retval : param_memory(m, &param, slot);
		if (param.attributes & PARAM_IS_RETURN && param.type && !type_uses_slot(param.type) && !(param.attributes & PARAM_BY_VALUE))
			memory = *(BYTE **)retval;
		ndr_run(m, op, memory, param_format(&param));
	}
}

static void server_free(MIDL_STUB_MESSAGE *m, const MIDL_STUB_DESC *desc, const proc_info *proc, BYTE *args, void *retval)
{
	const BYTE *at = proc->params;
	for (BYTE i = 0; i < proc->param_count; i++) {
		param_view param;
		at = next_param(at, desc, &param);
		if (!(param.attributes & PARAM_MUST_FREE) || param.attributes & PARAM_IS_BASE || is_context(&param))
			continue;
		void **slot = (void **)(args + param.offset);
		if (param.attributes & PARAM_IS_RETURN) {
			ndr_run(m, NDR_FREE, retval, param.type);
			continue;
		}
		ndr_run(m, NDR_FREE, param_memory(m, &param, slot), param.type);
		// What the walk itself does not own: the block a non-pointer parameter sat in.
		if (!type_uses_slot(param.type) && !(param.attributes & PARAM_BY_VALUE) && *slot && !((BYTE *)*slot >= m->BufferStart && (BYTE *)*slot < m->BufferEnd))
			ndr_release_block(m, *slot);
	}
}

// Shared by RPC calls and COM stubs, which differ in who the function belongs to and how the reply buffer is obtained.
static LONG interpret_stub(MIDL_SERVER_INFO *info, const BYTE *format, RPC_MESSAGE *rpc, IRpcStubBuffer *stub, IRpcChannelBuffer *channel, ULONG proc_index)
{
	const MIDL_STUB_DESC *desc = info->pStubDesc;
	proc_info proc;
	parse_proc(format, &proc);
	MIDL_STUB_MESSAGE m;
	if (proc.oi_flags & OI_OBJECT) {
		NdrStubInitialize(rpc, &m, (PMIDL_STUB_DESC)desc, channel);
	} else {
		NdrServerInitializeNew(rpc, &m, (PMIDL_STUB_DESC)desc);
	}
	m.Unused = NDR_TRACKS_MEMORY;
	if (rpc->DataRepresentation && (rpc->DataRepresentation & 0xffff) != NDR_LOCAL_DATA_REPRESENTATION)
		RpcRaiseException(RPC_X_BAD_STUB_DATA);
	if (proc.has_correlation)
		NdrCorrelationInitialize(&m, NULL, 0, 0);
	// The manager's register arguments always load from four slots, used or not.
	SIZE_T stack = proc.stack_size < 4 * sizeof(void *) ? 4 * sizeof(void *) : proc.stack_size;
	BYTE *args = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, stack + proc.param_count * sizeof(param_extra) + sizeof(ULONGLONG));
	if (!args)
		RpcRaiseException(RPC_S_OUT_OF_MEMORY);
	param_extra *extra = (param_extra *)(args + ((stack + 15) & ~(SIZE_T)15));
	m.StackTop = args;
	m.FullPtrXlatTables = proc.oi_flags & OI_FULL_POINTERS ? NdrFullPointerXlatInit(0, XLAT_SERVER) : NULL;
	if (!proc.handle_type && proc.handle_desc && *proc.handle_desc == FC_BIND_PRIMITIVE)
		*(RPC_BINDING_HANDLE *)(args + read_u16(proc.handle_desc + 2)) = rpc->Handle;
	server_unmarshal(&m, desc, &proc, args, extra);
	void *function;
	if (proc.oi_flags & OI_OBJECT) {
		void *object = ((CStdStubBuffer *)stub)->pvServerObject;
		*(void **)args = object;
		function = (*(void ***)object)[proc_index];
	} else if (rpc->ManagerEpv) {
		function = ((void **)rpc->ManagerEpv)[proc_index];
	} else {
		function = (void *)info->DispatchTable[proc_index];
	}
	ULONGLONG retval = (ULONGLONG)rpc_call_manager(function, args, stack);
	m.BufferLength = 0;
	server_marshal(&m, desc, &proc, args, extra, &retval, NDR_SIZE);
	if (proc.oi_flags & OI_OBJECT) {
		NdrStubGetBuffer(stub, channel, &m);
	} else {
		rpc->BufferLength = m.BufferLength;
		RPC_STATUS status = I_RpcGetBuffer(rpc);
		if (status != RPC_S_OK)
			RpcRaiseException(status);
		m.Buffer = rpc->Buffer;
	}
	server_marshal(&m, desc, &proc, args, extra, &retval, NDR_MARSHAL);
	rpc->BufferLength = (ULONG)(m.Buffer - (BYTE *)rpc->Buffer);
	server_free(&m, desc, &proc, args, &retval);
	if (m.FullPtrXlatTables)
		NdrFullPointerXlatFree(m.FullPtrXlatTables);
	ndr_release_all(&m);
	rpc_free(args);
	return S_OK;
}

void RPC_ENTRY NdrServerCall2(PRPC_MESSAGE rpc)
{
	const RPC_SERVER_INTERFACE *spec = rpc->RpcInterfaceInformation;
	MIDL_SERVER_INFO *info = (MIDL_SERVER_INFO *)spec->InterpreterInfo;
	ULONG proc = rpc->ProcNum & 0x7fff;
	interpret_stub(info, info->ProcString + info->FmtStringOffset[proc], rpc, NULL, NULL, proc);
}

void RPC_ENTRY NdrServerCall(PRPC_MESSAGE rpc)
{
	NdrServerCall2(rpc);
}

void RPC_ENTRY NdrServerCallAll(PRPC_MESSAGE rpc)
{
	NdrServerCall2(rpc);
}

void RPC_ENTRY NdrAsyncServerCall(PRPC_MESSAGE rpc)
{
	(void)rpc;
	RpcRaiseException(RPC_S_CANNOT_SUPPORT);
}

LONG RPC_ENTRY NdrStubCall2(struct IRpcStubBuffer *stub, struct IRpcChannelBuffer *channel, PRPC_MESSAGE rpc, DWORD *phase)
{
	const CInterfaceStubHeader *header = (const CInterfaceStubHeader *)((const BYTE *)((CStdStubBuffer *)stub)->lpVtbl - offsetof(CInterfaceStubVtbl, Vtbl));
	if (phase)
		*phase = STUB_UNMARSHAL;
	ULONG proc = rpc->ProcNum & 0x7fff;
	MIDL_SERVER_INFO *info = (MIDL_SERVER_INFO *)header->pServerInfo;
	return interpret_stub(info, info->ProcString + info->FmtStringOffset[proc], rpc, stub, channel, proc);
}

LONG RPC_ENTRY NdrStubCall(struct IRpcStubBuffer *stub, struct IRpcChannelBuffer *channel, PRPC_MESSAGE rpc, DWORD *phase)
{
	return NdrStubCall2(stub, channel, rpc, phase);
}

// ---- COM proxies without generated code ----

typedef struct object_call
{
	void *object;
	const MIDL_STUBLESS_PROXY_INFO *info;
	const BYTE *format;
	BYTE *args;
	ULONG index;
	LONG_PTR result;
} object_call;

static void marshal_object_call(MIDL_STUB_MESSAGE *m, const proc_info *proc, object_call *call, ULONGLONG *retval)
{
	const MIDL_STUB_DESC *desc = call->info->pStubDesc;
	marshal_in(m, desc, proc, call->args, NDR_SIZE);
	NdrProxyGetBuffer(call->object, m);
	marshal_in(m, desc, proc, call->args, NDR_MARSHAL);
	m->RpcMsg->BufferLength = (ULONG)(m->Buffer - (BYTE *)m->RpcMsg->Buffer);
	NdrProxySendReceive(call->object, m);
	unmarshal_out(m, desc, proc, call->args, retval);
}

typedef struct guarded_object
{
	MIDL_STUB_MESSAGE *m;
	const proc_info *proc;
	object_call *call;
	ULONGLONG *retval;
} guarded_object;

static void run_guarded_object(void *arg)
{
	guarded_object *guarded = arg;
	marshal_object_call(guarded->m, guarded->proc, guarded->call, guarded->retval);
}

// Failures of the channel come back as a result code, and out parameters are left in a defined state.
static LONG_PTR object_client_call(object_call *call)
{
	proc_info proc;
	parse_proc(call->format, &proc);
	const MIDL_STUB_DESC *desc = call->info->pStubDesc;
	MIDL_STUB_MESSAGE m;
	RPC_MESSAGE rpc;
	ULONGLONG retval = 0;
	NdrProxyInitialize(call->object, &rpc, &m, (PMIDL_STUB_DESC)desc, call->index);
	m.StackTop = call->args;
	if (proc.has_correlation)
		NdrCorrelationInitialize(&m, NULL, 0, 0);
	m.FullPtrXlatTables = proc.oi_flags & OI_FULL_POINTERS ? NdrFullPointerXlatInit(0, XLAT_CLIENT) : NULL;
	guarded_object guarded = {&m, &proc, call, &retval};
	LONG code = rpc_guard(run_guarded_object, &guarded);
	if (code) {
		// Nothing valid came out, so what the caller was promised gets cleared rather than left dangling.
		const BYTE *at = proc.params;
		for (BYTE i = 0; i < proc.param_count; i++) {
			param_view param;
			at = next_param(at, desc, &param);
			if (param.attributes & PARAM_IS_OUT && !(param.attributes & PARAM_IS_RETURN) && param.type)
				NdrClearOutParameters(&m, param.type, *(void **)(call->args + param.offset));
		}
		retval = NdrProxyErrorHandler(code);
	}
	NdrProxyFreeBuffer(call->object, &m);
	if (m.FullPtrXlatTables)
		NdrFullPointerXlatFree(m.FullPtrXlatTables);
	return (LONG_PTR)retval;
}

// Reached through the method thunks with the slot number, the arguments as a stack, and the saved float registers.
LONG_PTR rpc_object_method(ULONG index, BYTE *args, double *float_regs, ULONG *stack_size)
{
	void *object = *(void **)args;
	const BYTE *vtable = *(const BYTE **)object;
	const CInterfaceProxyHeader *header = (const CInterfaceProxyHeader *)(vtable - offsetof(CInterfaceProxyVtbl, Vtbl));
	const MIDL_STUBLESS_PROXY_INFO *info = header->pStublessProxyInfo;
	object_call call = {object, info, info->ProcFormatString + info->FormatStringOffset[index], args, index, 0};
	proc_info proc;
	parse_proc(call.format, &proc);
	if (stack_size)
		*stack_size = proc.stack_size;
#ifdef _WIN64
	// Floats reach the method in registers, while the walk reads them off the stack.
	const BYTE *at = proc.params;
	for (BYTE i = 0; i < proc.param_count; i++) {
		param_view param;
		at = next_param(at, info->pStubDesc, &param);
		if (param.attributes & PARAM_IS_BASE && (param.base == FC_FLOAT || param.base == FC_DOUBLE) && !(param.attributes & PARAM_SIMPLE_REF) && param.offset < 32)
			*(double *)(args + param.offset) = float_regs[param.offset / 8];
	}
#else
	(void)float_regs;
#endif
	return object_client_call(&call);
}

#define STUBLESS_COUNT 512

#ifdef _WIN64
__asm__(
	".text\n"
	"rpc_object_common:\n"
	"	subq $72, %rsp\n"
	"	movq %rcx, 80(%rsp)\n"
	"	movq %rdx, 88(%rsp)\n"
	"	movq %r8, 96(%rsp)\n"
	"	movq %r9, 104(%rsp)\n"
	"	movsd %xmm0, 32(%rsp)\n"
	"	movsd %xmm1, 40(%rsp)\n"
	"	movsd %xmm2, 48(%rsp)\n"
	"	movsd %xmm3, 56(%rsp)\n"
	"	movl %eax, %ecx\n"
	"	leaq 80(%rsp), %rdx\n"
	"	leaq 32(%rsp), %r8\n"
	"	xorq %r9, %r9\n"
	"	callq rpc_object_method\n"
	"	movq %rax, %xmm0\n"
	"	addq $72, %rsp\n"
	"	retq\n"
	".p2align 4\n"
	".globl rpc_stubless_thunks\n"
	"rpc_stubless_thunks:\n"
	".set rpc_thunk_index, 0\n"
	".rept 512\n"
	"	movl $rpc_thunk_index, %eax\n"
	"	jmp rpc_object_common\n"
	"	.p2align 4\n"
	"	.set rpc_thunk_index, rpc_thunk_index + 1\n"
	".endr\n");
#else
__asm__(
	".text\n"
	"rpc_object_common:\n"
	"	subl $4, %esp\n"
	"	movl %esp, %ecx\n"
	"	pushl %ecx\n"
	"	pushl $0\n"
	"	leal 16(%esp), %ecx\n"
	"	pushl %ecx\n"
	"	pushl %eax\n"
	"	calll _rpc_object_method\n"
	"	addl $16, %esp\n"
	"	popl %ecx\n"
	"	popl %edx\n"
	"	addl %ecx, %esp\n"
	"	jmpl *%edx\n"
	".p2align 4\n"
	".globl _rpc_stubless_thunks\n"
	"_rpc_stubless_thunks:\n"
	".set rpc_thunk_index, 0\n"
	".rept 512\n"
	"	movl $rpc_thunk_index, %eax\n"
	"	jmp rpc_object_common\n"
	"	.p2align 4\n"
	"	.set rpc_thunk_index, rpc_thunk_index + 1\n"
	".endr\n");
#endif

extern const BYTE rpc_stubless_thunks[];

void *rpc_stubless_thunk(ULONG index)
{
	return index < STUBLESS_COUNT ? (void *)(rpc_stubless_thunks + index * 16) : NULL;
}
