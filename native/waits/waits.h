#pragma once

// The waits on kernel objects of native/waits (see src/ffi.rs): objects and threads are numbers; a wait returns WAIT_OBJECT_0 + index,
// WAIT_ABANDONED_0 + index, WAIT_IO_COMPLETION, WAIT_TIMEOUT, raven_wait_ended for a thread terminated while it waited, or
// raven_wait_interrupted for one the host called away; the others return 0 or a Windows error code, with their value through a pointer.

#include <cstdint>

extern "C" {

struct RavenWaits;

constexpr uint32_t raven_wait_ended = 0xfffffffe, raven_wait_interrupted = 0xfffffffd, raven_wait_failed = 0xffffffff;
constexpr uint64_t raven_wait_infinite = UINT64_MAX;
enum RavenWaitKind : uint32_t { raven_auto_event, raven_manual_event, raven_mutex, raven_semaphore };

RavenWaits *raven_waits_new();
void raven_waits_free(RavenWaits *waits);
// 1 when the table keeps its objects in the kernel's /dev/ntsync, 0 when it keeps them itself.
uint32_t raven_waits_kernel(const RavenWaits *waits);
uint32_t raven_waits_create(const RavenWaits *waits, uint32_t kind, uint32_t initial, uint32_t max);
void raven_waits_close(const RavenWaits *waits, uint32_t object);
uint32_t raven_waits_set(const RavenWaits *waits, uint32_t object, uint32_t *previous);
uint32_t raven_waits_reset(const RavenWaits *waits, uint32_t object, uint32_t *previous);
uint32_t raven_waits_pulse(const RavenWaits *waits, uint32_t object, uint32_t *previous, uint32_t *granted);
uint32_t raven_waits_detach(const RavenWaits *waits, uint32_t object, uint32_t *value, uint32_t *owner, uint32_t *depth);
uint32_t raven_waits_query(const RavenWaits *waits, uint32_t object, uint32_t *value);
uint32_t raven_waits_release_mutex(const RavenWaits *waits, uint32_t object, uint32_t thread, uint32_t *previous);
uint32_t raven_waits_release_semaphore(const RavenWaits *waits, uint32_t object, uint32_t count, uint32_t *previous);
uint32_t raven_waits_wait(const RavenWaits *waits, uint32_t thread, const uint32_t *objects, uint32_t count, uint32_t all,
	uint32_t alertable, uint64_t timeout_ns);
void raven_waits_queue_apc(const RavenWaits *waits, uint32_t thread);
uint32_t raven_waits_take_apcs(const RavenWaits *waits, uint32_t thread);
void raven_waits_interrupt(const RavenWaits *waits, uint32_t thread);
uint32_t raven_waits_waiting(const RavenWaits *waits, uint32_t thread);
void raven_waits_suspend(const RavenWaits *waits, uint32_t thread);
void raven_waits_resume(const RavenWaits *waits, uint32_t thread);
void raven_waits_thread_ended(const RavenWaits *waits, uint32_t thread);

}
