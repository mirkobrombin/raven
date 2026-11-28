// Faulting stubs for item 4 and x18 probes for item 9.
    .text
    .balign 16
    .globl _do_brk, _do_brk_insn, _do_badsvc, _do_badsvc_insn, _do_load, _do_load_insn
_do_brk:
_do_brk_insn:
    brk     #0
    ret

// Invalid BSD syscall number via the Unix trap. Returns x0 as left by the kernel/handler.
_do_badsvc:
    mov     x16, #0x1ff0
_do_badsvc_insn:
    svc     #0x80
    ret

// x0 = address (PROT_NONE). Handler skips the load and puts 42 in x0.
_do_load:
_do_load_insn:
    ldr     x0, [x0]
    ret

// ---- x18 probes ----
// u64 x18_set_read(u64 v): x18 = v; return x18   (sanity)
    .globl _x18_set, _x18_get, _x18_spin, _x18_brk, _x18_load
_x18_set:
    mov     x18, x0
    ret
_x18_get:
    mov     x0, x18
    ret
// u64 x18_spin(u64 sentinel, u64 iters, u64 *changes): spin with x18=sentinel, count
// iterations where x18 != sentinel (and restore it). Returns final x18.
_x18_spin:
    mov     x18, x0
    mov     x9, #0
1:  cmp     x18, x0
    b.eq    2f
    add     x9, x9, #1
    mov     x18, x0
2:  subs    x1, x1, #1
    b.ne    1b
    str     x9, [x2]
    mov     x0, x18
    ret
// u64 x18_brk(u64 sentinel): x18 = sentinel; brk #0 (handler skips); return x18
_x18_brk:
    mov     x18, x0
    brk     #0
    mov     x0, x18
    ret
// u64 x18_load(u64 sentinel, u64 *protnone): x18 = sentinel; fault on load (handler skips); return x18
_x18_load:
    mov     x18, x0
    ldr     x9, [x1]
    mov     x0, x18
    ret

// u64 x18_call(u64 sentinel, void (*fn)(u64), u64 arg): x18 = sentinel; fn(arg); return x18
    .globl _x18_call
_x18_call:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x18, x0
    mov     x9, x1
    mov     x0, x2
    blr     x9
    mov     x0, x18
    ldp     x29, x30, [sp], #16
    ret

// u64 x18_rawsvc(u64 sentinel): x18 = sentinel; getpid via raw svc (no libc); return x18
    .globl _x18_rawsvc, _x18_noop
_x18_rawsvc:
    mov     x18, x0
    mov     x16, #20
    svc     #0x80
    mov     x0, x18
    ret
_x18_noop:
    ret
