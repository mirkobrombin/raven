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
 * wakes on its low half), 401 the signal, 402 the address of its siginfo, 403 the address of the interrupted ucontext. */

	.intel_syntax noprefix
	.section .rodata, "a"

	.set AGENT_STACK, 65536
	.set AGENT_STATE, 400 * 8
	.set SYS_FUTEX, 202
	.set SYS_RT_SIGRETURN, 15
	.set FUTEX_WAIT, 0
	.set FUTEX_WAKE, 1

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
3:	mov dword ptr [rbx + AGENT_STATE], 0
	ret
/* The return the kernel takes from the handler (SA_RESTORER). */
raven_agent_restorer:
	mov eax, SYS_RT_SIGRETURN
	syscall
	ud2
raven_agent_end:

	.section .note.GNU-stack, "", @progbits
