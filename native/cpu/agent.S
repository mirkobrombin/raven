/* The agent: the code a guest thread runs when the kernel hands it a signal, so that what stopped it (a breakpoint, a fault, a system
 * call Syscall User Dispatch turned away) reaches the runtime over the thread's block instead of through ptrace. The runtime copies
 * these bytes into the guest; it never runs them itself. The labels give the offsets the runtime and the guest agree on (see agent.h).
 *
 * The handler runs on the thread's alternate signal stack, which lies on a boundary of its own size and keeps the address of the
 * thread's block in its first word: the handler finds the block from the stack pointer alone, whatever GS or FS hold, so 32-bit
 * threads take the same path. It leaves the signal, its information and the interrupted context in the block, tells the runtime and
 * waits for the answer, spinning a while and then sleeping on the state word. The runtime reads and changes the interrupted context
 * where the kernel left it; returning through rt_sigreturn then resumes the thread as the runtime left that context. The system calls
 * the agent makes lie inside its own code, the range Syscall User Dispatch lets through.
 *
 * Words of the block: 400 the state (0 nothing, 1 a signal waits for the runtime, 2 the runtime has answered; the runtime sleeps and
 * wakes on its low half), 401 the signal, 402 the address of its siginfo, 403 the address of the interrupted ucontext, 404 the
 * segment registers the runtime asks for with its answer, which a signal frame does not carry (bit 0 the FS base, bit 1 the GS base,
 * bits 2 to 5 the FS, GS, DS and ES selectors), 405 the FS base and 406 the GS base asked for, 408 to 411 the FS, GS, DS and ES
 * selectors asked for. Selectors are loaded before bases: loading one sets its base from the descriptor. */

	.intel_syntax noprefix
	.section .rodata, "a"

	.set AGENT_STACK, 65536
	.set AGENT_STATE, 400 * 8
	.set SYS_FUTEX, 202
	.set SYS_RT_SIGRETURN, 15
	.set FUTEX_WAIT, 0
	.set FUTEX_WAKE, 1
	.set SYS_ARCH_PRCTL, 158
	.set ARCH_SET_GS, 0x1001
	.set ARCH_SET_FS, 0x1002
	.set SYS_CLONE, 56
	.set SYS_SIGALTSTACK, 131
	.set SYS_PRCTL, 157
	.set SYS_RT_SIGPROCMASK, 14
	.set SIG_SETMASK, 2
	.set PR_SET_SYSCALL_USER_DISPATCH, 59
	.set PR_SYS_DISPATCH_ON, 1
	.set AGENT_THREAD, 1000
	.set AGENT_WARDEN, 1001
	.set SYS_POLL, 7
	.set SYS_EXIT, 60
	.set SYS_KILL, 62
	.set SYS_PIDFD_OPEN, 434
	.set SIGKILL, 9
	.set POLLIN, 1
	.set WARDEN_CLONE, 0x00850f00	/* the service thread's own: a thread of the guest, untraced */
	.set THREAD_CLONE, 0x00048700	/* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_PARENT | CLONE_SYSVSEM, no exit signal */

	.global raven_agent, raven_agent_end, raven_agent_handler, raven_agent_restorer
	.balign 64
raven_agent:
raven_agent_handler:
	mov rax, rsp
	and rax, -AGENT_STACK
	mov rbx, qword ptr [rax]
	mov qword ptr [rbx + AGENT_STATE + 8], rdi
	mov qword ptr [rbx + AGENT_STATE + 16], rsi
	mov qword ptr [rbx + AGENT_STATE + 24], rdx
	mov dword ptr [rbx + AGENT_STATE], 1
	/* Every register is the agent's to use: rt_sigreturn puts back the interrupted ones. */
	lea rdi, [rbx + AGENT_STATE]
	mov esi, FUTEX_WAKE
	mov edx, 1
	xor r10d, r10d
	mov eax, SYS_FUTEX
	syscall
1:	mov ecx, 2000
2:	cmp dword ptr [rbx + AGENT_STATE], 1
	jne short 3f
	pause
	dec ecx
	jnz short 2b
	lea rdi, [rbx + AGENT_STATE]
	mov esi, FUTEX_WAIT
	mov edx, 1
	xor r10d, r10d
	mov eax, SYS_FUTEX
	syscall
	jmp short 1b
3:	test dword ptr [rbx + AGENT_STATE + 32], 4
	jz short 6f
	mov eax, dword ptr [rbx + AGENT_STATE + 64]
	mov fs, eax
6:	test dword ptr [rbx + AGENT_STATE + 32], 8
	jz short 7f
	mov eax, dword ptr [rbx + AGENT_STATE + 72]
	mov gs, eax
7:	test dword ptr [rbx + AGENT_STATE + 32], 16
	jz short 8f
	mov eax, dword ptr [rbx + AGENT_STATE + 80]
	mov ds, eax
8:	test dword ptr [rbx + AGENT_STATE + 32], 32
	jz short 9f
	mov eax, dword ptr [rbx + AGENT_STATE + 88]
	mov es, eax
9:	test dword ptr [rbx + AGENT_STATE + 32], 1
	jz short 4f
	mov edi, ARCH_SET_FS
	mov rsi, qword ptr [rbx + AGENT_STATE + 40]
	mov eax, SYS_ARCH_PRCTL
	syscall
4:	test dword ptr [rbx + AGENT_STATE + 32], 2
	jz short 5f
	mov edi, ARCH_SET_GS
	mov rsi, qword ptr [rbx + AGENT_STATE + 48]
	mov eax, SYS_ARCH_PRCTL
	syscall
5:	mov dword ptr [rbx + AGENT_STATE + 32], 0
	mov dword ptr [rbx + AGENT_STATE], 0
	ret
/* The return the kernel takes from the handler (SA_RESTORER). */
raven_agent_restorer:
	mov eax, SYS_RT_SIGRETURN
	syscall
	ud2

/* The service thread: the guest's own thread that makes system calls for the runtime, so the runtime never needs a guest thread
 * stopped to map, protect or release memory, start threads and the like. RDI is its block, in words: 0 the state (0 nothing, 1 a call
 * waits, 2 it is done; the runtime sleeps and wakes on its low half), 1 the call's number, 2 to 7 its arguments, 8 its result. It
 * never returns.
 *
 * Number 1000 is the agent's own: it starts a guest thread already in the agent's hands, with argument 2 the top of the thread's
 * stack and argument 3 its block. The thread is a child of the runtime's (CLONE_PARENT from this thread, whose process the runtime
 * started), so the runtime reaps it, and has the signal handlers of this thread's process. The alternate stack and Syscall User
 * Dispatch are not inherited: the thread sets them up itself from its block (the stack_t at word 504, the range the dispatch lets
 * through at words 508 and 509, the selector in word 407), leaves their results in words 510 and 511 and stops on its breakpoint,
 * held in the handler until the runtime gives it a context. The result is the thread's id.
 *
 * Number 1001 starts the guest's warden, with argument 2 the runtime's process id: a thread of the guest that waits for the runtime to
 * end and then ends the guest's whole process group, which no tracer does for tasks in the agent's hands. It waits on a pidfd, which
 * tells when the runtime's process ends rather than the thread that started the guest. It uses no stack and takes no signal. The
 * result is the warden's id, or the error of opening the pidfd. Words 10 to 11 of the block are its own. */
	.global raven_agent_service
raven_agent_service:
	mov rbx, rdi
1:	mov ecx, 2000
2:	cmp dword ptr [rbx], 1
	je short 3f
	pause
	dec ecx
	jnz short 2b
	mov rdi, rbx
	mov esi, FUTEX_WAIT
	xor edx, edx
	xor r10d, r10d
	mov eax, SYS_FUTEX
	syscall
	jmp short 1b
3:	mov rax, qword ptr [rbx + 8]
	mov rdi, qword ptr [rbx + 16]
	mov rsi, qword ptr [rbx + 24]
	mov rdx, qword ptr [rbx + 32]
	mov r10, qword ptr [rbx + 40]
	mov r8, qword ptr [rbx + 48]
	mov r9, qword ptr [rbx + 56]
	cmp rax, AGENT_THREAD
	je short 10f
	cmp rax, AGENT_WARDEN
	je 12f
	syscall
11:	mov qword ptr [rbx + 64], rax
	mov dword ptr [rbx], 2
	mov rdi, rbx
	mov esi, FUTEX_WAKE
	mov edx, 1
	xor r10d, r10d
	mov eax, SYS_FUTEX
	syscall
	jmp short 1b
10:	mov r12, rsi
	mov rsi, rdi
	mov edi, THREAD_CLONE
	xor edx, edx
	xor r10d, r10d
	xor r8d, r8d
	mov eax, SYS_CLONE
	syscall
	test rax, rax
	jnz short 11b
	lea rdi, [r12 + 504 * 8]
	xor esi, esi
	mov eax, SYS_SIGALTSTACK
	syscall
	mov qword ptr [r12 + 510 * 8], rax
	mov edi, PR_SET_SYSCALL_USER_DISPATCH
	mov esi, PR_SYS_DISPATCH_ON
	mov rdx, qword ptr [r12 + 508 * 8]
	mov r10, qword ptr [r12 + 509 * 8]
	lea r8, [r12 + 407 * 8]
	mov eax, SYS_PRCTL
	syscall
	mov qword ptr [r12 + 511 * 8], rax
	/* The service thread's mask came along; a breakpoint whose signal waits would end the thread instead of stopping it. */
	push 0
	mov edi, SIG_SETMASK
	mov rsi, rsp
	xor edx, edx
	mov r10d, 8
	mov eax, SYS_RT_SIGPROCMASK
	syscall
	pop rcx
	.global raven_agent_born
raven_agent_born:
	int3
	ud2
12:	xor esi, esi
	mov eax, SYS_PIDFD_OPEN
	syscall
	test rax, rax
	js 11b
	mov dword ptr [rbx + 88], eax
	mov dword ptr [rbx + 92], POLLIN
	mov qword ptr [rbx + 80], -1
	mov edi, WARDEN_CLONE
	xor esi, esi
	xor edx, edx
	xor r10d, r10d
	xor r8d, r8d
	mov eax, SYS_CLONE
	syscall
	test rax, rax
	jnz 11b
	/* The warden shares the service thread's stack pointer: no signal may run a handler on it. */
	mov edi, SIG_SETMASK
	lea rsi, [rbx + 80]
	xor edx, edx
	mov r10d, 8
	mov eax, SYS_RT_SIGPROCMASK
	syscall
13:	lea rdi, [rbx + 88]
	mov esi, 1
	mov edx, -1
	mov eax, SYS_POLL
	syscall
	test rax, rax
	jz short 13b
	/* A failed wait leaves the guest alone: only the runtime's end may end it. */
	js short 14f
	xor edi, edi
	mov esi, SIGKILL
	mov eax, SYS_KILL
	syscall
14:	xor edi, edi
	mov eax, SYS_EXIT
	syscall
	ud2

/* Starts the service thread: the runtime runs this from a stopped task with RDI the service block and RSI the top of the thread's
 * stack. The thread joins the task's thread group, so it ends with the process, and is untraced even when the task traces its clones.
 * The task stops on the breakpoint after the call with RAX the new thread's id, or the error. */
	.set SERVICE_CLONE, 0x00850f00	/* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_UNTRACED */
	.global raven_agent_spawn, raven_agent_spawned
raven_agent_spawn:
	mov r12, rdi
	mov edi, SERVICE_CLONE
	xor edx, edx
	xor r10d, r10d
	xor r8d, r8d
	mov eax, SYS_CLONE
	syscall
	test rax, rax
	jz short 1f
raven_agent_spawned:
	int3
1:	mov rdi, r12
	jmp raven_agent_service

/* Where a traced task goes to be handed to the agent: the runtime points it here and lets it go, and its breakpoint brings it into
 * the handler, held there as it was held stopped. */
	.global raven_agent_park
raven_agent_park:
	int3
	ud2
raven_agent_end:

	.section .note.GNU-stack, "", @progbits
