// Position-independent arm64 kernels. Called natively as C functions and
// copied verbatim into a Hypervisor.framework guest (EL1). No stack use.
    .text
    .balign 16
    .globl _gk_start, _gk_end, _mp_writer, _mp_reader, _sb_thread, _gk_hvc, _mmu_touch

_gk_start:

// Wait until *go == 2 after incrementing it. x3 = go. Clobbers x9,x10.
.macro START_SYNC
    mov     x9, #1
    ldaddal x9, x10, [x3]
1:  ldar    x10, [x3]
    cmp     x10, #2
    b.lo    1b
.endm

// uint64_t mp_writer(u64 *x, u64 *y, u64 n, u64 *go)
// for i in 1..n: *x = i; *y = i
_mp_writer:
    START_SYNC
    mov     x4, #1
2:  str     x4, [x0]
    str     x4, [x1]
    add     x4, x4, #1
    cmp     x4, x2
    b.ls    2b
    mov     x0, #0
    ret

// uint64_t mp_reader(u64 *x, u64 *y, u64 n, u64 *go)
// loop { a = *y; b = *x; if (b < a) violations++; } until a == n
// Under TSO b >= a always (x is written before y, loads are ordered).
_mp_reader:
    START_SYNC
    mov     x5, #0
    mov     x12, #0
2:  ldr     x6, [x1]
    ldr     x7, [x0]
    cmp     x7, x6
    cinc    x5, x5, lo
    add     x12, x12, #1
    cmp     x6, x2
    b.lo    2b
    str     x12, [x3, #8]       // loop count -> go[1] (proves overlap with writer)
    mov     x0, x5
    ret

// uint64_t sb_thread(u64 *mine, u64 *other, u8 *res, u64 n, u64 *bar)
// per iteration: barrier; *mine = 1; res[i] = *other; barrier; *mine = 0
_sb_thread:
    mov     x8, #0
    mov     x5, #0
3:  add     x8, x8, #2
    mov     x9, #1
    ldaddal x9, x10, [x4]
4:  ldar    x10, [x4]
    cmp     x10, x8
    b.lo    4b
    str     x9, [x0]
    ldr     x11, [x1]
    strb    w11, [x2, x5]
    add     x8, x8, #2
    ldaddal x9, x10, [x4]
5:  ldar    x10, [x4]
    cmp     x10, x8
    b.lo    5b
    str     xzr, [x0]
    add     x5, x5, #1
    cmp     x5, x3
    b.lo    3b
    mov     x0, #0
    ret

// u64 mmu_touch(u32 *va1, u32 *va2): writes 0x1111/0x2222, returns sum of read-backs
_mmu_touch:
    movz    w2, #0x1111
    str     w2, [x0]
    movz    w3, #0x2222
    str     w3, [x1]
    ldr     w4, [x0]
    ldr     w5, [x1]
    add     x0, x4, x5
    ret

// u64 spin_client(u32 *req, u32 *resp, u64 n): for i in 1..n: *req = i (release); spin until *resp == i (acquire)
    .globl _spin_client
_spin_client:
    mov     x4, #1
1:  stlr    w4, [x0]
2:  ldar    w5, [x1]
    cmp     w5, w4
    b.ne    2b
    add     x4, x4, #1
    cmp     x4, x2
    b.ls    1b
    mov     x0, #0
    ret

// Guest return trampoline: LR points here. Also used as the HVC round-trip loop.
_gk_hvc:
    hvc     #0
    b       _gk_hvc

_gk_end:

// EL1 vector table: every entry reports ESR/FAR/ELR to the host via hvc #0x7ff.
    .balign 2048
    .globl _gk_vectors, _gk_vectors_end
_gk_vectors:
    .rept 16
    .balign 128
    mrs     x0, esr_el1
    mrs     x1, far_el1
    mrs     x2, elr_el1
    hvc     #0x7ff
    .endr
    .balign 128
_gk_vectors_end:
