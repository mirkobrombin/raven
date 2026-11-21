/* The code the runtime puts into a guest for its calls: the stub a served call goes through, and the page of gates a host thread has its
 * guest run short routines from. The runtime copies these bytes into the guest; it never runs them itself. The labels give the offsets
 * the runtime and the guest's own code agree on (see served_stub.h).
 *
 * A thread's block, which host and guest share, in words: 0 the stub of the call in progress (cleared with the answer, odd while a
 * routine asked of the guest has returned), 1 to 4 RCX, RDX, R8 and R9, 5 RSP, 6 to 9 the low halves of XMM0 to XMM3, 10 the answer,
 * 11 what the stub does with it (0 return, 1 stop for the runtime, 2 take the breakpoint the call falls back to, 3 run what the runtime
 * asks), 12 the time stamp the stub took when the call came in, 13 the stub of the last call answered, 14 the time stamp the runtime took
 * when it answered, 15 the time stamp the stub took when the guest went on with the answer. Words 300 on carry a request (see
 * native_cpu.cpp). */

	.intel_syntax noprefix
	.section .rodata, "a"

/* A served stub. It takes the block from the provider's own pointer, which stays zero until the runtime maps the block (the runtime
 * replaces the first twelve bytes to take the thread's block through GS instead), leaves its arguments there with the time it came in,
 * publishes its own address and spins until the runtime clears it. After a thousand rounds it stops at a breakpoint instead, which gives
 * the processor to the runtime when the two compete for it. RDX goes back to what the caller left before anything can stop: a declined
 * call stops with the argument registers as they were. */
	.global raven_served_stub, raven_served_stub_end, raven_served_stub_store, raven_served_stub_spin, raven_served_stub_wait
	.global raven_served_stub_tail, raven_served_stub_yield, raven_served_stub_trap
raven_served_stub:
.Lstub:
	mov r10, qword ptr [rip + 0]
	test r10, r10
	jz short .Ltrap
	/* The first instruction boundary after the fourteen bytes a program hooking the function copies out and jumps back past. */
	xchg ax, ax
raven_served_stub_store:
	mov qword ptr [r10 + 8], rcx
	mov qword ptr [r10 + 16], rdx
	mov qword ptr [r10 + 24], r8
	mov qword ptr [r10 + 32], r9
	mov qword ptr [r10 + 40], rsp
	movq qword ptr [r10 + 48], xmm0
	movq qword ptr [r10 + 56], xmm1
	movq qword ptr [r10 + 64], xmm2
	movq qword ptr [r10 + 72], xmm3
	rdtsc
	mov dword ptr [r10 + 96], eax
	mov dword ptr [r10 + 100], edx
	mov rdx, qword ptr [r10 + 16]
	lea r11, [rip + .Lstub]
	mov qword ptr [r10], r11
raven_served_stub_spin:
	mov r11d, 1000
1:	pause
2:	cmp qword ptr [r10], 0
	je short 3f
	dec r11d
	jnz short 1b
raven_served_stub_wait:
	int3
	jmp short 2b
3:	rdtsc
	mov dword ptr [r10 + 120], eax
	mov dword ptr [r10 + 124], edx
	mov rdx, qword ptr [r10 + 16]
	mov rax, qword ptr [r10 + 80]
	mov r11, qword ptr [r10 + 88]
	test r11, r11
	jz short 4f
/* What the stub does with an answer that is not plain: the served ones stop for the runtime, or take their breakpoint when the call was
 * declined; for those a host thread answers the runtime puts a jump to the routine that runs what it asks of the guest here instead
 * (raven_asked_tail). */
raven_served_stub_tail:
	cmp r11d, 2
	je short .Ltrap
raven_served_stub_yield:
	int3
4:	ret
raven_served_stub_trap:
.Ltrap:
	int3
	ret
raven_served_stub_end:

/* The first twelve bytes of a stub that takes the block of the thread it runs in, from its TEB. */
	.global raven_local_block, raven_local_block_end
raven_local_block:
	mov r10, qword ptr gs:[0x1ff0]
	nop
	nop
	nop
raven_local_block_end:

/* The wrapper of a call the guest may answer itself while other threads run (see answer_wrapper in providers.cpp). It reads the version
 * of the runtime's tables from the served block, runs the guest's own answer with the argument registers it saved, and keeps that answer
 * only if the version was even and did not move meanwhile. Another thread's call beside the turn publishes the one record it changed in
 * a moment, so the wrapper looks again a number of times before it gives up to the stub, which serves the call from the start and takes
 * the desktop lock whole for it. The runtime fills in the three rel32 fields that end at the labels: the provider's pointer to the served
 * block, the guest's answer, and the stub, twice; the second, at the label where the answer gave up, drops the answer's frame first. */
	.global raven_answer_wrapper, raven_answer_wrapper_end, raven_answer_wrapper_pointer, raven_answer_wrapper_answer
	.global raven_answer_wrapper_stub, raven_answer_wrapper_gives_up
raven_answer_wrapper:
	mov r11, qword ptr [rip + 0]
raven_answer_wrapper_pointer:
	test r11, r11
	jz short .Lanswer_stub
	mov r10, r11
	mov eax, 128
.Lanswer_try:
	mov r11, qword ptr [r10 + 0x7b0]
	test r11b, 1
	jnz short .Lanswer_busy
	/* Seven words keep the stack aligned for the call; the four over its home space are the version and the arguments. */
	push rax
	push r10
	push r9
	push r8
	push rdx
	push rcx
	push r11
	.byte 0xe8
	.long 0
raven_answer_wrapper_answer:
	mov r11, qword ptr [rsp + 0x28]
	mov r11, qword ptr [r11 + 0x7b0]
	cmp r11, qword ptr [rsp]
	jne short .Lanswer_changed
	add rsp, 0x38
	ret
.Lanswer_changed:
	add rsp, 8
	pop rcx
	pop rdx
	pop r8
	pop r9
	pop r10
	pop rax
.Lanswer_busy:
	pause
	dec eax
	jnz short .Lanswer_try
.Lanswer_stub:
	.byte 0xe9
	.long 0
raven_answer_wrapper_stub:
raven_answer_wrapper_gives_up:
	add rsp, 0x10
	pop rcx
	pop rdx
	pop r8
	pop r9
	add rsp, 0x10
	.byte 0xe9
	.long 0
raven_answer_wrapper_end:

/* The tail of a stub a host thread answers: what is not a plain answer goes to the routine at word 320 of the block. */
	.global raven_asked_tail, raven_asked_tail_end
raven_asked_tail:
	jmp qword ptr [r10 + 2560]
raven_asked_tail_end:

/* The page of gates. At its start the locked exchanges the memory task runs, each followed by the breakpoint it completes at; from
 * 0x40 the routine a stub goes to for a request (word 11 is 3 there), which runs the function at word 301 with the twelve arguments
 * from word 302 on and goes back to waiting in its stub, or takes the stub's breakpoint for a declined call; then the exchanges a guest
 * waiting in its stub runs for the runtime, each on a 16-byte boundary. The argument registers come back from a routine as the caller of
 * the stub left them, as they do from a call that stops: code of the guest's own components counts on it. */
	.global raven_gate_page, raven_gate_page_end, raven_gate_exchange32, raven_gate_exchange64, raven_gate_exchange32_done
	.global raven_gate_exchange64_done, raven_gate_request, raven_gate_request_return, raven_gate_request_exchange64
	.global raven_gate_request_exchange32, raven_gate_request_syscall, raven_gate_request_syscall_done
	.balign 64
raven_gate_page:
raven_gate_exchange32:
	lock cmpxchg dword ptr [rcx], edx
raven_gate_exchange32_done:
	int3
	ret
raven_gate_exchange64:
	lock cmpxchg qword ptr [rcx], rdx
raven_gate_exchange64_done:
	int3
	ret
	.org raven_gate_page + 0x40, 0xcc
raven_gate_request:
	cmp r11d, 2
	jne short 1f
	mov r11, qword ptr [r10 + 2400]
	.byte 0x49, 0x83, 0xc3, raven_served_stub_trap - raven_served_stub	/* add r11, the offset of the breakpoint */
	jmp r11
1:	mov r11, qword ptr [r10 + 2400]
	push r11
	push rcx
	push rdx
	push r8
	push r9
	sub rsp, 0x60
	.irp slot, 0, 1, 2, 3, 4, 5, 6, 7
	mov rax, qword ptr [r10 + 2448 + \slot * 8]
	mov qword ptr [rsp + 32 + \slot * 8], rax
	.endr
	mov rcx, qword ptr [r10 + 2416]
	mov rdx, qword ptr [r10 + 2424]
	mov r8, qword ptr [r10 + 2432]
	mov r9, qword ptr [r10 + 2440]
	call qword ptr [r10 + 2408]
raven_gate_request_return:
	add rsp, 0x60
	pop r9
	pop r8
	pop rdx
	pop rcx
	pop r11
	mov r10, qword ptr gs:[0x1ff0]
	mov qword ptr [r10 + 2408], rax
	lea rax, [r11 + 1]
	mov qword ptr [r10], rax
	.byte 0x49, 0x83, 0xc3, raven_served_stub_spin - raven_served_stub	/* add r11, the offset of the spin */
	jmp r11
	.balign 16, 0xcc
raven_gate_request_exchange64:
	mov rax, rdx
	lock cmpxchg qword ptr [rcx], r8
	ret
	.balign 16, 0xcc
raven_gate_request_exchange32:
	mov eax, edx
	lock cmpxchg dword ptr [rcx], r8d
	ret
/* A Linux system call a guest waiting in its stub makes for the runtime (see NativeProcess::memory_syscall): the number in RCX, the
 * arguments in RDX, R8, R9 and then on the stack, as the request routine passes them. RDI and RSI are the caller's to keep. */
	.balign 16, 0xcc
raven_gate_request_syscall:
	push rdi
	push rsi
	mov rax, rcx
	mov rdi, rdx
	mov rsi, r8
	mov rdx, r9
	mov r10, qword ptr [rsp + 56]
	mov r8, qword ptr [rsp + 64]
	mov r9, qword ptr [rsp + 72]
	syscall
raven_gate_request_syscall_done:
	pop rsi
	pop rdi
	ret
raven_gate_page_end:

	.section .note.GNU-stack, "", @progbits
