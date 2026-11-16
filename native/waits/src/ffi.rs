//! The interface the host's C++ calls (waits.h). Objects and threads are numbers; results are the values Windows gives: a wait returns
//! WAIT_OBJECT_0 + index, WAIT_ABANDONED_0 + index, WAIT_IO_COMPLETION, WAIT_TIMEOUT, or RAVEN_WAIT_ENDED for a thread terminated while
//! it waited; the others return 0 or a Windows error code, with the value they give back through a pointer.

use crate::{Error, Kind, Outcome, Waits};
use std::time::Duration;

const WAIT_ABANDONED_0: u32 = 0x80;
const WAIT_IO_COMPLETION: u32 = 0xc0;
const WAIT_TIMEOUT: u32 = 0x102;
const WAIT_ENDED: u32 = 0xffff_fffe;
const WAIT_FAILED: u32 = 0xffff_ffff;
/// No time-out.
const INFINITE: u64 = u64::MAX;

const ERROR_INVALID_HANDLE: u32 = 6;
const ERROR_NOT_OWNER: u32 = 288;
const ERROR_TOO_MANY_POSTS: u32 = 298;

fn code(error: Error) -> u32 {
    match error {
        Error::Invalid => ERROR_INVALID_HANDLE,
        Error::NotOwner => ERROR_NOT_OWNER,
        Error::TooManyPosts => ERROR_TOO_MANY_POSTS,
    }
}

/// Stores the value through `out` when there is one, and gives back 0 or the error.
unsafe fn give(result: Result<u32, Error>, out: *mut u32) -> u32 {
    match result {
        Ok(value) => {
            if !out.is_null() {
                *out = value;
            }
            0
        }
        Err(error) => code(error),
    }
}

#[no_mangle]
pub extern "C" fn raven_waits_new() -> *mut Waits {
    Box::into_raw(Box::new(Waits::new()))
}

/// # Safety
/// `waits` came from raven_waits_new and no other call uses it any more.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_free(waits: *mut Waits) {
    if !waits.is_null() {
        drop(Box::from_raw(waits));
    }
}

/// Kinds: 0 an event reset on its own, 1 one reset by hand, 2 a mutex, 3 a semaphore counting to `max`. Gives back the object, or 0.
///
/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_create(waits: *const Waits, kind: u32, initial: u32, max: u32) -> u32 {
    let kind = match kind {
        0 => Kind::AutoEvent,
        1 => Kind::ManualEvent,
        2 => Kind::Mutex,
        3 => Kind::Semaphore { max },
        _ => return 0,
    };
    (*waits).create(kind, initial).unwrap_or(0)
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_close(waits: *const Waits, object: u32) {
    (*waits).close(object);
}

/// # Safety
/// `waits` came from raven_waits_new; `previous` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_set(waits: *const Waits, object: u32, previous: *mut u32) -> u32 {
    give((*waits).set(object), previous)
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_reset(waits: *const Waits, object: u32, previous: *mut u32) -> u32 {
    give((*waits).reset(object), previous)
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_pulse(waits: *const Waits, object: u32, previous: *mut u32) -> u32 {
    give((*waits).pulse(object), previous)
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_query(waits: *const Waits, object: u32, value: *mut u32) -> u32 {
    give((*waits).query(object), value)
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_release_mutex(waits: *const Waits, object: u32, thread: u32, previous: *mut u32) -> u32 {
    give((*waits).release_mutex(object, thread), previous)
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_release_semaphore(waits: *const Waits, object: u32, count: u32, previous: *mut u32) -> u32 {
    give((*waits).release_semaphore(object, count), previous)
}

/// Waits for any (`all` 0) or all of `count` objects, for `timeout_ns` nanoseconds or, with u64::MAX, without end.
///
/// # Safety
/// `waits` came from raven_waits_new; `objects` holds `count` objects.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_wait(waits: *const Waits, thread: u32, objects: *const u32, count: u32, all: u32, alertable: u32,
    timeout_ns: u64) -> u32 {
    if objects.is_null() || count == 0 {
        return WAIT_FAILED;
    }
    let objects = std::slice::from_raw_parts(objects, count as usize);
    let timeout = (timeout_ns != INFINITE).then(|| Duration::from_nanos(timeout_ns));
    match (*waits).wait(thread, objects, all != 0, alertable != 0, timeout) {
        Ok(Outcome::Object(index)) => index,
        Ok(Outcome::Abandoned(index)) => WAIT_ABANDONED_0 + index,
        Ok(Outcome::IoCompletion) => WAIT_IO_COMPLETION,
        Ok(Outcome::Timeout) => WAIT_TIMEOUT,
        Ok(Outcome::Ended) => WAIT_ENDED,
        Err(_) => WAIT_FAILED,
    }
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_queue_apc(waits: *const Waits, thread: u32) {
    (*waits).queue_apc(thread);
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_take_apcs(waits: *const Waits, thread: u32) -> u32 {
    (*waits).take_apcs(thread)
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_suspend(waits: *const Waits, thread: u32) {
    (*waits).suspend(thread);
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_resume(waits: *const Waits, thread: u32) {
    (*waits).resume(thread);
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_thread_ended(waits: *const Waits, thread: u32) {
    (*waits).thread_ended(thread);
}
