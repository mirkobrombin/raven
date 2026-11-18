// The guest kernel at EL1 and the dispatcher translated code runs under at EL0. Vm copies both into guest RAM; nothing here runs on
// the host. Neither touches x18.

	.text

// VBAR_EL1. An svc from EL0 is the dispatcher asking the host for the next block: hvc #0 exits to the host, which resumes the vCPU
// at the next instruction once the mailbox names the code to enter. Before returning to EL0, the instruction cache is invalidated if
// the host wrote code meanwhile. Any other exception reports itself with hvc #1 (EL0) or hvc #(0x10 + vector) and never returns;
// the host reads ESR_EL1, FAR_EL1 and ELR_EL1.
	.p2align 11
	.globl _raven_hvf_vectors, _raven_hvf_vectors_end
_raven_hvf_vectors:
	.irp vector, 0, 1, 2, 3, 4, 5, 6, 7
	.p2align 7
	hvc #(0x10 + \vector)
	.endr

	.p2align 7
	// Lower EL, AArch64, synchronous. x9 is free here: the dispatcher holds nothing in it, and a fault never returns.
	mrs x9, esr_el1
	lsr x9, x9, #26
	cmp x9, #0x15
	b.ne 2f
	hvc #0
	// Where a vCPU starts too, with ELR_EL1 at the dispatcher: x19 is the mailbox, whose word at 24 asks for the invalidation.
	.globl _raven_hvf_resume
_raven_hvf_resume:
	ldr w9, [x19, #24]
	cbz w9, 1f
	str wzr, [x19, #24]
	ic ialluis
	dsb ish
	isb
1:	eret
2:	hvc #1
	b 2b

	.irp vector, 9, 10, 11, 12, 13, 14, 15
	.p2align 7
	hvc #(0x10 + \vector)
	.endr
	.p2align 7
_raven_hvf_vectors_end:

// The EL0 dispatcher, entered with x19 at this vCPU's mailbox: the CpuState, the code to enter and the enter trampoline. The
// trampoline keeps x19 as a callee-saved register and returns the exit reason in w0, which the host reads at the svc.
	.p2align 12
	.globl _raven_hvf_dispatcher, _raven_hvf_dispatcher_end
_raven_hvf_dispatcher:
1:	ldp x0, x1, [x19]
	ldr x16, [x19, #16]
	blr x16
	svc #0
	b 1b
_raven_hvf_dispatcher_end:
