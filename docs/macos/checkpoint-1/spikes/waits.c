// Item 10: wait/wake ping-pong latency, in-process (2 threads) and cross-process (fork, shared mapping).
// usage: waits [iters]
#include "bench.h"
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <os/os_sync_wait_on_address.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/mach_error.h>
#include <stdatomic.h>

// Private API (not in the SDK): declared by hand, from xnu bsd/sys/ulock.h.
extern int __ulock_wait(uint32_t op, void *addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t op, void *addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT        1
#define UL_COMPARE_AND_WAIT_SHARED 3

enum { RUNS = 30 };
static uint64_t ITERS = 100000;

typedef struct { _Atomic uint32_t req, pad1[31], resp, pad2[31]; _Atomic int child_state; pthread_mutex_t m; pthread_cond_t c; } shared_t;
static shared_t *S;
static int kind;   // which mechanism
enum { K_SPIN, K_OSSYNC, K_ULOCK, K_COND, K_DSEM, K_MACHSEM, K_OSSYNC_SH, K_ULOCK_SH, K_MACHSEM_X, K_POSIXSEM_X, K_SPIN_X };
static const char *KN[] = { "spin", "os_sync_wait_on_address", "ulock_wait_private", "pthread_cond", "dispatch_semaphore", "mach_semaphore",
                            "xproc_os_sync_shared", "xproc_ulock_shared_private", "xproc_mach_semaphore", "xproc_posix_named_sem", "xproc_spin" };
static dispatch_semaphore_t dA, dB;
static semaphore_t mA, mB;
static sem_t *pA, *pB;


// Pass two semaphore send rights to a forked child: the child inherits TASK_BOOTSTRAP_PORT,
// which the parent temporarily points at a rendezvous receive port.
typedef struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t p[2]; mach_msg_trailer_t t; } portmsg_t;
static mach_port_t rv_port, saved_bootstrap;
static void rv_before_fork(void) {
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rv_port);
    mach_port_insert_right(mach_task_self(), rv_port, rv_port, MACH_MSG_TYPE_MAKE_SEND);
    task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &saved_bootstrap);
    CHECK(task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, rv_port) == KERN_SUCCESS, "set bootstrap");
}
static void rv_parent_after_fork(void) {
    task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, saved_bootstrap);
    struct { mach_msg_header_t h; mach_msg_trailer_t t; } req;
    kern_return_t kr = mach_msg(&req.h, MACH_RCV_MSG, 0, sizeof req, rv_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    CHECK(kr == KERN_SUCCESS, "rv receive %d", kr);
    portmsg_t m = {0};
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    m.h.msgh_size = sizeof m - sizeof m.t; m.h.msgh_remote_port = req.h.msgh_remote_port;
    m.b.msgh_descriptor_count = 2;
    m.p[0].name = mA; m.p[0].disposition = MACH_MSG_TYPE_COPY_SEND; m.p[0].type = MACH_MSG_PORT_DESCRIPTOR;
    m.p[1].name = mB; m.p[1].disposition = MACH_MSG_TYPE_COPY_SEND; m.p[1].type = MACH_MSG_PORT_DESCRIPTOR;
    kr = mach_msg(&m.h, MACH_SEND_MSG, m.h.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    CHECK(kr == KERN_SUCCESS, "rv send %d", kr);
}
static int rv_child(void) {
    mach_port_t bs, reply;
    task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bs);
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply);
    mach_msg_header_t h = {0};
    h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    h.msgh_size = sizeof h; h.msgh_remote_port = bs; h.msgh_local_port = reply;
    if (mach_msg(&h, MACH_SEND_MSG, sizeof h, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL)) return 0;
    portmsg_t m;
    if (mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, reply, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL)) return 0;
    mA = m.p[0].name; mB = m.p[1].name;
    return mA && mB;
}

static void wake_word(_Atomic uint32_t *w) {
    switch (kind) {
    case K_OSSYNC: os_sync_wake_by_address_any((void *)w, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE); break;
    case K_OSSYNC_SH: os_sync_wake_by_address_any((void *)w, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED); break;
    case K_ULOCK: __ulock_wake(UL_COMPARE_AND_WAIT, (void *)w, 0); break;
    case K_ULOCK_SH: __ulock_wake(UL_COMPARE_AND_WAIT_SHARED, (void *)w, 0); break;
    }
}
static void wait_word(_Atomic uint32_t *w, uint32_t target) {
    uint32_t v;
    while ((v = atomic_load_explicit(w, memory_order_acquire)) != target) {
        switch (kind) {
        case K_OSSYNC: os_sync_wait_on_address((void *)w, v, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE); break;
        case K_OSSYNC_SH: os_sync_wait_on_address((void *)w, v, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED); break;
        case K_ULOCK: __ulock_wait(UL_COMPARE_AND_WAIT, (void *)w, v, 0); break;
        case K_ULOCK_SH: __ulock_wait(UL_COMPARE_AND_WAIT_SHARED, (void *)w, v, 0); break;
        default: break;   // spin
        }
    }
}

// side 0 = initiator (timed), side 1 = responder
static void ping(int side, uint32_t i) {
    switch (kind) {
    case K_COND:
        pthread_mutex_lock(&S->m);
        if (side == 0) { S->req = i; pthread_cond_broadcast(&S->c); while (S->resp != i) pthread_cond_wait(&S->c, &S->m); }
        else { while (S->req != i) pthread_cond_wait(&S->c, &S->m); S->resp = i; pthread_cond_broadcast(&S->c); }
        pthread_mutex_unlock(&S->m);
        return;
    case K_DSEM: if (side == 0) { dispatch_semaphore_signal(dB); dispatch_semaphore_wait(dA, DISPATCH_TIME_FOREVER); }
                 else { dispatch_semaphore_wait(dB, DISPATCH_TIME_FOREVER); dispatch_semaphore_signal(dA); } return;
    case K_MACHSEM: case K_MACHSEM_X:
        if (side == 0) { semaphore_signal(mB); semaphore_wait(mA); } else { semaphore_wait(mB); semaphore_signal(mA); } return;
    case K_POSIXSEM_X: if (side == 0) { sem_post(pB); sem_wait(pA); } else { sem_wait(pB); sem_post(pA); } return;
    default:
        if (side == 0) { atomic_store_explicit(&S->req, i, memory_order_release); wake_word(&S->req); wait_word(&S->resp, i); }
        else { wait_word(&S->req, i); atomic_store_explicit(&S->resp, i, memory_order_release); wake_word(&S->resp); }
    }
}

static void responder(void) {
    set_qos();
    uint32_t n = 0;
    for (int r = -2; r < RUNS; r++) for (uint64_t i = 0; i < ITERS; i++) ping(1, ++n);
}
static void *responder_thread(void *p) { responder(); return NULL; }

static void initiator(double *s) {
    uint32_t n = 0;
    for (int r = -2; r < RUNS; r++) {
        uint64_t a = now_ns();
        for (uint64_t i = 0; i < ITERS; i++) ping(0, ++n);
        if (r >= 0) s[r] = (double)(now_ns() - a) / ITERS;
    }
}

static void reset_shared(void) {
    S->req = 0; S->resp = 0; S->child_state = 0;
}

int main(int argc, char **argv) {
    set_qos(); setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) ITERS = strtoull(argv[1], NULL, 0);
    reset_jsonl("waits");
    S = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    pthread_mutex_init(&S->m, NULL); pthread_cond_init(&S->c, NULL);
    dA = dispatch_semaphore_create(0); dB = dispatch_semaphore_create(0);
    double s[RUNS]; char note[128];
    for (kind = K_SPIN; kind <= K_MACHSEM; kind++) {
        reset_shared();
        if (kind == K_MACHSEM) { semaphore_create(mach_task_self(), &mA, SYNC_POLICY_FIFO, 0); semaphore_create(mach_task_self(), &mB, SYNC_POLICY_FIFO, 0); }
        pthread_t t; pthread_create(&t, NULL, responder_thread, NULL);
        initiator(s);
        pthread_join(t, NULL);
        snprintf(note, sizeof note, "2 threads, %llu round-trips per run, wake always issued", (unsigned long long)ITERS);
        report("waits", KN[kind], "ns_per_roundtrip", s, RUNS, note);
    }
    // cross-process variants
    for (kind = K_OSSYNC_SH; kind <= K_SPIN_X; kind++) {
        reset_shared();
        if (kind == K_MACHSEM_X) {
            semaphore_create(mach_task_self(), &mA, SYNC_POLICY_FIFO, 0); semaphore_create(mach_task_self(), &mB, SYNC_POLICY_FIFO, 0);
            mach_port_t ports[2] = { mA, mB };
            kern_return_t kr = mach_ports_register(mach_task_self(), ports, 2);
            printf("mach_ports_register -> %d (%s)\n", kr, mach_error_string(kr));
            rv_before_fork();
        }
        if (kind == K_POSIXSEM_X) {
            char na[64], nb[64]; snprintf(na, sizeof na, "/rvA%d", getpid()); snprintf(nb, sizeof nb, "/rvB%d", getpid());
            pA = sem_open(na, O_CREAT | O_EXCL, 0600, 0); pB = sem_open(nb, O_CREAT | O_EXCL, 0600, 0);
            CHECK(pA != SEM_FAILED && pB != SEM_FAILED, "sem_open");
            sem_unlink(na); sem_unlink(nb);
        }
        pid_t pid = fork();
        if (pid == 0) {
            if (kind == K_MACHSEM_X) {
                mach_port_array_t arr; mach_msg_type_number_t cnt = 0;
                kern_return_t kr = mach_ports_lookup(mach_task_self(), &arr, &cnt);
                printf("child: mach_ports_lookup kr=%d cnt=%u ports=[0x%x,0x%x] (0 = not inherited)\n", kr, cnt, cnt > 0 ? arr[0] : 0, cnt > 1 ? arr[1] : 0);
                int ok = rv_child();
                printf("child: semaphores received via inherited TASK_BOOTSTRAP_PORT rendezvous: %s\n", ok ? "yes" : "no");
                fflush(stdout);
                if (!ok) { S->child_state = 2; _exit(3); }
            }
            S->child_state = 1;
            responder(); _exit(0);
        }
        if (kind == K_MACHSEM_X) rv_parent_after_fork();
        while (S->child_state == 0) ;
        if (S->child_state != 1) { int st; waitpid(pid, &st, 0); printf("RESULT %s: NOT verified (child could not get the ports)\n", KN[kind]); continue; }
        S->child_state = 0;
        initiator(s);
        int st; waitpid(pid, &st, 0);
        snprintf(note, sizeof note, "2 processes (fork), MAP_SHARED|MAP_ANON, %llu round-trips per run; child status %d", (unsigned long long)ITERS, st);
        report("waits", KN[kind], "ns_per_roundtrip", s, RUNS, note);
    }
    return 0;
}
