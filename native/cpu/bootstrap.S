.section .text
.global _start
_start:
	int3
	syscall
	int3
	ud2
/* The batch gate: runs the system calls of a table in guest memory, each seven quadwords (the number and six arguments), writing each
   result over its number, and stops at the INT3 after the last, or after the first that fails. rdi is the table, rsi how many calls it
   holds. */
	mov %rdi, %rbx
	mov %rsi, %rbp
1:	test %rbp, %rbp
	jz 2f
	mov (%rbx), %rax
	mov 8(%rbx), %rdi
	mov 16(%rbx), %rsi
	mov 24(%rbx), %rdx
	mov 32(%rbx), %r10
	mov 40(%rbx), %r8
	mov 48(%rbx), %r9
	syscall
	mov %rax, (%rbx)
	cmp $-4095, %rax
	jae 2f
	add $56, %rbx
	dec %rbp
	jmp 1b
2:	int3
	ud2

.section .note.GNU-stack,"",@progbits
