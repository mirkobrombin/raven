// Guest code for hvf2.c. Copied into guest RAM: vectors at offset 0, code at OFF_CODE, EL0 code at OFF_EL0.
// The same functions also run natively on the host for comparison (mem_plain, mem_rcpc).
	.text
	.p2align 11
	.globl _v2_vectors, _v2_vectors_end
_v2_vectors:
	.p2align 7
	hvc #9                       // 0x000 current EL, SP0, sync
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7                   // 0x200 current EL, SPx, sync
	mrs x9, esr_el1
	lsr x10, x9, #26
	cmp x10, #0x25               // data abort taken at EL1: count it and skip the instruction
	b.ne 1f
	mrs x11, elr_el1
	add x11, x11, #4
	msr elr_el1, x11
	add x20, x20, #1
	eret
1:	mov x0, x9
	mrs x1, far_el1
	mrs x2, elr_el1
	hvc #8
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7                   // 0x400 lower EL, AArch64, sync
	mrs x9, esr_el1
	lsr x10, x9, #26
	cmp x10, #0x15               // svc from EL0: count and return
	b.ne 2f
	add x21, x21, #1
	eret
2:	mov x0, x18                  // brk or anything else from EL0: report x18 as EL0 left it
	mov x1, x21
	hvc #2
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
	.p2align 7
	hvc #9
_v2_vectors_end:

	.p2align 12
	.globl _v2_start, _v2_end, _v2_hvc, _mem_plain, _mem_rcpc, _fault_loop, _low_va, _x18_el1, _x18_el0_enter, _stage2_loop
_v2_start:
_v2_hvc:
	hvc #0
	b _v2_hvc

// mem_plain(buf, iters): 5 dependent load/store pairs per iteration over one cache line set.
_mem_plain:
	mov x9, #0
1:	ldr x2, [x0]
	add x2, x2, #1
	str x2, [x0, #64]
	ldr x3, [x0, #128]
	add x3, x3, x2
	str x3, [x0, #192]
	ldr x4, [x0, #256]
	add x4, x4, x3
	str x4, [x0, #320]
	ldr x5, [x0, #384]
	add x5, x5, x4
	str x5, [x0, #448]
	ldr x6, [x0, #64]
	add x9, x9, x6
	str x9, [x0]
	subs x1, x1, #1
	b.ne 1b
	mov x0, x9
	ret

// mem_rcpc: the same with LDAPR/STLR, as a translator emits for x86 ordering without hardware TSO.
_mem_rcpc:
	mov x9, #0
	add x10, x0, #64
	add x11, x0, #128
	add x12, x0, #192
	add x13, x0, #256
	add x14, x0, #320
	add x15, x0, #384
	add x16, x0, #448
1:	ldapr x2, [x0]
	add x2, x2, #1
	stlr x2, [x10]
	ldapr x3, [x11]
	add x3, x3, x2
	stlr x3, [x12]
	ldapr x4, [x13]
	add x4, x4, x3
	stlr x4, [x14]
	ldapr x5, [x15]
	add x5, x5, x4
	stlr x5, [x16]
	ldapr x6, [x10]
	add x9, x9, x6
	stlr x9, [x0]
	subs x1, x1, #1
	b.ne 1b
	mov x0, x9
	ret

// fault_loop(bad_va, n): each load takes a translation fault handled by the EL1 vector; returns the count.
_fault_loop:
	mov x20, #0
1:	ldr x2, [x0]
	subs x1, x1, #1
	b.ne 1b
	mov x0, x20
	ret

// low_va(a, b): write and read back through two low virtual addresses.
_low_va:
	movz x2, #0x1234
	str x2, [x0]
	movz x3, #0x5678
	str x3, [x1]
	ldr x4, [x0]
	ldr x5, [x1]
	add x0, x4, x5
	ret

// x18_el1(sentinel, n): keeps x18 across n host exits (hvc #3), returns x18.
_x18_el1:
	mov x18, x0
1:	hvc #3
	subs x1, x1, #1
	b.ne 1b
	mov x0, x18
	ret

// x18_el0_enter(sentinel, n, el0_pc, el0_sp): drop to EL0 with x18 set; EL0 makes n svc calls then brk.
_x18_el0_enter:
	mov x18, x0
	mov x22, x1
	mov x21, #0
	msr elr_el1, x2
	msr sp_el0, x3
	mov x9, #0x3c0               // EL0t, DAIF masked
	msr spsr_el1, x9
	isb
	eret

// stage2_loop(va, n): loads from a VA whose IPA has no stage-2 mapping; each load exits to the host, which skips it.
_stage2_loop:
1:	ldr x2, [x0]
	subs x1, x1, #1
	b.ne 1b
	ret
_v2_end:

// EL0 code, copied to its own EL0-accessible page.
	.p2align 12
	.globl _v2_el0, _v2_el0_end
_v2_el0:
1:	svc #0
	subs x22, x22, #1
	b.ne 1b
	brk #0
_v2_el0_end:
