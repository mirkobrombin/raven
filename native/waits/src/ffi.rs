//! The interface the host's C++ calls (waits.h). Objects and threads are numbers; results are the values Windows gives: a wait returns
//! WAIT_OBJECT_0 + index, WAIT_ABANDONED_0 + index, WAIT_IO_COMPLETION, WAIT_TIMEOUT, raven_wait_ended for a thread terminated while
//! it waited or raven_wait_interrupted for one the host called away; the others return 0 or a Windows error code, with the value they give back through a pointer.

use crate::{Error, Kind, Outcome, Signal, Table};
use std::sync::atomic::AtomicU32;
use std::time::Duration;

const WAIT_ABANDONED_0: u32 = 0x80;
const WAIT_IO_COMPLETION: u32 = 0xc0;
const WAIT_TIMEOUT: u32 = 0x102;
const WAIT_ENDED: u32 = 0xffff_fffe;
const WAIT_INTERRUPTED: u32 = 0xffff_fffd;
/// raven_waits_line_up: the thread stands in the line, or the table has none.
const WAIT_LINED: u32 = 0xffff_fffc;
const WAIT_UNLINED: u32 = 0xffff_fffb;
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
pub extern "C" fn raven_waits_new() -> *mut Table {
    Box::into_raw(Box::new(Table::new()))
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_kernel(waits: *const Table) -> u32 {
    u32::from((*waits).kernel())
}

/// A new descriptor of an event's ntsync object, which the caller closes, or -1: no such event, or no /dev/ntsync.
///
/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_event_descriptor(waits: *const Table, object: u32) -> i32 {
    (*waits).event_descriptor(object).map_or(-1, std::os::fd::IntoRawFd::into_raw_fd)
}

/// # Safety
/// `waits` came from raven_waits_new and no other call uses it any more.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_free(waits: *mut Table) {
    if !waits.is_null() {
        drop(Box::from_raw(waits));
    }
}

/// Kinds: 0 an event reset on its own, 1 one reset by hand, 2 a mutex, 3 a semaphore counting to `max`. Gives back the object, or 0.
///
/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_create(waits: *const Table, kind: u32, initial: u32, max: u32) -> u32 {
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
pub unsafe extern "C" fn raven_waits_close(waits: *const Table, object: u32) {
    (*waits).close(object);
}

/// # Safety
/// `waits` came from raven_waits_new; `previous` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_set(waits: *const Table, object: u32, previous: *mut u32) -> u32 {
    give((*waits).set(object), previous)
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_reset(waits: *const Table, object: u32, previous: *mut u32) -> u32 {
    give((*waits).reset(object), previous)
}

/// Gives back the state before through `previous`, and through `granted` how many waits the pulse ended.
///
/// # Safety
/// As raven_waits_set; `granted` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_pulse(waits: *const Table, object: u32, previous: *mut u32, granted: *mut u32) -> u32 {
    let result = (*waits).pulse(object);
    if let (Ok((_, ended)), false) = (result, granted.is_null()) {
        *granted = ended;
    }
    give(result.map(|(before, _)| before), previous)
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_query(waits: *const Table, object: u32, value: *mut u32) -> u32 {
    give((*waits).query(object), value)
}

/// Takes `object` out of the table (see Waits::detach), giving back its state or count, and a mutex's owner and depth.
///
/// # Safety
/// `waits` came from raven_waits_new; the pointers are null or writable.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_detach(waits: *const Table, object: u32, value: *mut u32, owner: *mut u32, depth: *mut u32) -> u32 {
    match (*waits).detach(object) {
        Ok((count, holder, held)) => {
            for (out, item) in [(value, count), (owner, holder), (depth, held)] {
                if !out.is_null() {
                    *out = item;
                }
            }
            0
        }
        Err(error) => code(error),
    }
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_release_mutex(waits: *const Table, object: u32, thread: u32, previous: *mut u32) -> u32 {
    give((*waits).release_mutex(object, thread), previous)
}

/// # Safety
/// As raven_waits_set.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_release_semaphore(waits: *const Table, object: u32, count: u32, previous: *mut u32) -> u32 {
    give((*waits).release_semaphore(object, count), previous)
}

/// Waits for any (`all` 0) or all of `count` objects, for `timeout_ns` nanoseconds or, with u64::MAX, without end.
///
/// # Safety
/// `waits` came from raven_waits_new; `objects` holds `count` objects.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_wait(waits: *const Table, thread: u32, objects: *const u32, count: u32, all: u32, alertable: u32,
    timeout_ns: u64) -> u32 {
    if objects.is_null() || count == 0 {
        return WAIT_FAILED;
    }
    let objects = std::slice::from_raw_parts(objects, count as usize);
    let timeout = (timeout_ns != INFINITE).then(|| Duration::from_nanos(timeout_ns));
    match (*waits).wait(thread, objects, all != 0, alertable != 0, timeout) {
        Ok(outcome) => result(outcome),
        Err(_) => WAIT_FAILED,
    }
}

fn result(outcome: Outcome) -> u32 {
    match outcome {
        Outcome::Object(index) => index,
        Outcome::Abandoned(index) => WAIT_ABANDONED_0 + index,
        Outcome::IoCompletion => WAIT_IO_COMPLETION,
        Outcome::Timeout => WAIT_TIMEOUT,
        Outcome::Ended => WAIT_ENDED,
        Outcome::Interrupted => WAIT_INTERRUPTED,
    }
}

/// Lines `thread` up for any or all of `count` objects without blocking (Waits::line_up): gives raven_wait_lined when it stands in the
/// line, after which whoever ends its wait stores 1 at `signal` and raven_waits_leave gives the result; raven_wait_unlined on the
/// kernel's objects, which have no such line; or the result of a wait that ended at once.
///
/// # Safety
/// `waits` came from raven_waits_new; `objects` holds `count` objects; `signal` stays valid until raven_waits_leave for the thread.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_line_up(waits: *const Table, thread: u32, objects: *const u32, count: u32, all: u32, alertable: u32,
    signal: *const AtomicU32) -> u32 {
    if objects.is_null() || count == 0 || signal.is_null() {
        return WAIT_FAILED;
    }
    let objects = std::slice::from_raw_parts(objects, count as usize);
    match (*waits).line_up(thread, objects, all != 0, alertable != 0, Signal::new(signal)) {
        None => WAIT_UNLINED,
        Some(Ok(None)) => WAIT_LINED,
        Some(Ok(Some(outcome))) => result(outcome),
        Some(Err(_)) => WAIT_FAILED,
    }
}

/// Takes `thread` out of the line: the result of its wait, or WAIT_TIMEOUT when it had not ended.
///
/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_leave(waits: *const Table, thread: u32) -> u32 {
    (*waits).leave(thread).map_or(WAIT_TIMEOUT, result)
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_queue_apc(waits: *const Table, thread: u32) {
    (*waits).queue_apc(thread);
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_take_apcs(waits: *const Table, thread: u32) -> u32 {
    (*waits).take_apcs(thread)
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_interrupt(waits: *const Table, thread: u32) {
    (*waits).interrupt(thread);
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_waiting(waits: *const Table, thread: u32) -> u32 {
    u32::from((*waits).waiting(thread))
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_suspend(waits: *const Table, thread: u32) {
    (*waits).suspend(thread);
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_resume(waits: *const Table, thread: u32) {
    (*waits).resume(thread);
}

/// # Safety
/// `waits` came from raven_waits_new.
#[no_mangle]
pub unsafe extern "C" fn raven_waits_thread_ended(waits: *const Table, thread: u32) {
    (*waits).thread_ended(thread);
}
