// Item 5: in-process "gate" trampolines.
    .text
    .balign 16
    .globl _empty_fn, _gate_gpr, _gate_full
_empty_fn:
    ret

// void gate_gpr(ctx *c): save x19-x30 + sp in c, switch to c->host_sp, call c->fn(c), restore.
// ctx layout: [0..96) x19-x30, [96] sp, [104] host_sp, [112] fn, [128..192) d8-d15
_gate_gpr:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x9, sp
    str     x9, [x0, #96]
    mov     x19, x0
    ldr     x9, [x0, #104]
    mov     sp, x9
    ldr     x10, [x0, #112]
    blr     x10
    mov     x0, x19
    ldr     x9, [x0, #96]
    mov     sp, x9
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ret

// Same plus d8-d15 and NZCV/FPCR/FPSR save/restore.
_gate_full:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    stp     d8, d9, [x0, #128]
    stp     d10, d11, [x0, #144]
    stp     d12, d13, [x0, #160]
    stp     d14, d15, [x0, #176]
    mrs     x9, nzcv
    mrs     x10, fpcr
    mrs     x11, fpsr
    stp     x9, x10, [x0, #192]
    str     x11, [x0, #208]
    mov     x9, sp
    str     x9, [x0, #96]
    mov     x19, x0
    ldr     x9, [x0, #104]
    mov     sp, x9
    ldr     x10, [x0, #112]
    blr     x10
    mov     x0, x19
    ldr     x9, [x0, #96]
    mov     sp, x9
    ldp     x9, x10, [x0, #192]
    ldr     x11, [x0, #208]
    msr     nzcv, x9
    msr     fpcr, x10
    msr     fpsr, x11
    ldp     d8, d9, [x0, #128]
    ldp     d10, d11, [x0, #144]
    ldp     d12, d13, [x0, #160]
    ldp     d14, d15, [x0, #176]
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ret
