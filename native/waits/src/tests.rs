use super::*;
use std::sync::Arc;
use std::time::Duration;

const SHORT: Option<Duration> = Some(Duration::from_millis(20));
const LONG: Option<Duration> = Some(Duration::from_secs(5));

/// The table the cases run on: the kernel's objects with RAVEN_TEST_NTSYNC=1, which needs /dev/ntsync, and the locked table otherwise.
fn waits() -> Arc<Table> {
    if std::env::var_os("RAVEN_TEST_NTSYNC").is_some() {
        return Arc::new(Table::Kernel(ntsync::Waits::open().expect("RAVEN_TEST_NTSYNC=1 needs /dev/ntsync")));
    }
    Arc::new(Table::Locked(Waits::new()))
}

/// Waits until `thread` waits, so that what follows happens to a wait in progress. A thread on the kernel's objects is marked waiting
/// just before it enters the kernel, and is given a moment to get there.
fn waiting(waits: &Table, thread: ThreadId) {
    while !waits.waiting(thread) {
        std::thread::yield_now();
    }
    if waits.kernel() {
        std::thread::sleep(Duration::from_millis(20));
    }
}

#[test]
fn auto_event_wakes_one_waiter_and_resets() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let mut threads = Vec::new();
    for thread in 1..=2 {
        let shared = waits.clone();
        threads.push(std::thread::spawn(move || shared.wait(thread, &[event], false, false, Some(Duration::from_millis(300))).unwrap()));
        waiting(&waits, thread);
    }
    waits.set(event).unwrap();
    let outcomes: Vec<_> = threads.into_iter().map(|thread| thread.join().unwrap()).collect();
    assert_eq!(outcomes.iter().filter(|&&outcome| outcome == Outcome::Object(0)).count(), 1);
    assert_eq!(outcomes.iter().filter(|&&outcome| outcome == Outcome::Timeout).count(), 1);
    assert_eq!(outcomes[0], Outcome::Object(0), "the oldest wait takes the event");
    assert_eq!(waits.query(event), Ok(0));
}

#[test]
fn manual_event_wakes_every_waiter_and_stays_set() {
    let waits = waits();
    let event = waits.create(Kind::ManualEvent, 0).unwrap();
    let threads: Vec<_> = (1..=3)
        .map(|thread| {
            let waits = waits.clone();
            let handle = std::thread::spawn(move || waits.wait(thread, &[event], false, false, LONG).unwrap());
            handle
        })
        .collect();
    for thread in 1..=3 {
        waiting(&waits, thread);
    }
    waits.set(event).unwrap();
    for thread in threads {
        assert_eq!(thread.join().unwrap(), Outcome::Object(0));
    }
    assert_eq!(waits.query(event), Ok(1));
}

#[test]
fn pulse_releases_only_the_waits_there_are() {
    let waits = waits();
    let event = waits.create(Kind::ManualEvent, 0).unwrap();
    let first = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(1, &[event], false, false, LONG).unwrap())
    };
    waiting(&waits, 1);
    assert_eq!(waits.pulse(event), Ok((0, 1)));
    assert_eq!(first.join().unwrap(), Outcome::Object(0));
    assert_eq!(waits.query(event), Ok(0));
    assert_eq!(waits.wait(2, &[event], false, false, SHORT), Ok(Outcome::Timeout), "a later wait does not see the pulse");
}

#[test]
fn a_wait_that_only_looks_does_not_block() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let mutex = waits.create(Kind::Mutex, 2).unwrap();
    for _ in 0..20 {
        let started = std::time::Instant::now();
        assert_eq!(waits.wait(1, &[event, mutex], false, false, Some(Duration::ZERO)), Ok(Outcome::Timeout));
        assert_eq!(waits.wait(1, &[event, mutex], true, false, Some(Duration::ZERO)), Ok(Outcome::Timeout));
        assert!(started.elapsed() < Duration::from_millis(1), "a look took {:?}", started.elapsed());
    }
    waits.interrupt(1);
    let started = std::time::Instant::now();
    assert_eq!(waits.wait(1, &[event], false, false, None), Ok(Outcome::Interrupted));
    assert!(started.elapsed() < Duration::from_millis(1), "an interrupted wait took {:?}", started.elapsed());
}

/// A thread the program ends while it waits is called away right after: its wait ends however the two reach it.
#[test]
fn a_wait_ended_with_its_thread_and_called_away_returns() {
    for _ in 0..20 {
        let waits = waits();
        let event = waits.create(Kind::AutoEvent, 0).unwrap();
        let (sender, receiver) = std::sync::mpsc::channel();
        let waiter = {
            let waits = waits.clone();
            std::thread::spawn(move || sender.send(waits.wait(1, &[event], false, false, None)).unwrap())
        };
        waiting(&waits, 1);
        waits.thread_ended(1);
        waits.interrupt(1);
        let outcome = receiver.recv_timeout(Duration::from_secs(2)).expect("the wait of an ended thread went on");
        assert!(matches!(outcome, Ok(Outcome::Ended | Outcome::Interrupted)), "{outcome:?}");
        waiter.join().unwrap();
    }
}

#[test]
fn wait_for_any_reports_the_lowest_free_index() {
    let waits = waits();
    let objects = [waits.create(Kind::AutoEvent, 0).unwrap(), waits.create(Kind::ManualEvent, 1).unwrap(),
        waits.create(Kind::Semaphore { max: 2 }, 1).unwrap()];
    assert_eq!(waits.wait(1, &objects, false, false, Some(Duration::ZERO)), Ok(Outcome::Object(1)));
    assert_eq!(waits.query(objects[2]), Ok(1), "only the object reported is taken");
}

#[test]
fn wait_for_all_takes_everything_at_once_or_nothing() {
    let waits = waits();
    let mutex = waits.create(Kind::Mutex, 0).unwrap();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    assert_eq!(waits.wait(1, &[mutex, event], true, false, Some(Duration::ZERO)), Ok(Outcome::Timeout));
    assert_eq!(waits.query(mutex), Ok(0), "a wait for all that is not satisfied takes nothing");
    let all = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(1, &[mutex, event], true, false, LONG).unwrap())
    };
    waiting(&waits, 1);
    waits.set(event).unwrap();
    assert_eq!(all.join().unwrap(), Outcome::Object(0));
    assert_eq!(waits.query(mutex), Ok(1));
    assert_eq!(waits.query(event), Ok(0));
}

#[test]
fn a_wait_for_all_passed_over_does_not_hold_back_a_wait_for_any() {
    let waits = waits();
    let mutex = waits.create(Kind::Mutex, 3).unwrap();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let all = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(1, &[mutex, event], true, false, Some(Duration::from_millis(300))).unwrap())
    };
    waiting(&waits, 1);
    let any = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(2, &[event], false, false, LONG).unwrap())
    };
    waiting(&waits, 2);
    waits.set(event).unwrap();
    assert_eq!(any.join().unwrap(), Outcome::Object(0), "the event goes to the wait it satisfies");
    assert_eq!(all.join().unwrap(), Outcome::Timeout);
}

#[test]
fn mutex_is_taken_again_by_its_owner_and_released_as_often() {
    let waits = waits();
    let mutex = waits.create(Kind::Mutex, 0).unwrap();
    assert_eq!(waits.wait(1, &[mutex], false, false, None), Ok(Outcome::Object(0)));
    assert_eq!(waits.wait(1, &[mutex], false, false, None), Ok(Outcome::Object(0)));
    assert_eq!(waits.wait(2, &[mutex], false, false, Some(Duration::ZERO)), Ok(Outcome::Timeout));
    assert_eq!(waits.release_mutex(mutex, 2), Err(Error::NotOwner));
    assert_eq!(waits.release_mutex(mutex, 1), Ok(2));
    assert_eq!(waits.query(mutex), Ok(1));
    assert_eq!(waits.release_mutex(mutex, 1), Ok(1));
    assert_eq!(waits.query(mutex), Ok(0));
    assert_eq!(waits.release_mutex(mutex, 1), Err(Error::NotOwner));
}

#[test]
fn ended_owner_abandons_its_mutex_to_the_next_waiter() {
    let waits = waits();
    let mutex = waits.create(Kind::Mutex, 1).unwrap();
    let next = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(2, &[mutex], false, false, LONG).unwrap())
    };
    waiting(&waits, 2);
    waits.thread_ended(1);
    assert_eq!(next.join().unwrap(), Outcome::Abandoned(0));
    assert_eq!(waits.query(mutex), Ok(2));
    waits.release_mutex(mutex, 2).unwrap();
    assert_eq!(waits.wait(3, &[mutex], false, false, None), Ok(Outcome::Object(0)), "the abandonment is reported once");
}

#[test]
fn semaphore_counts_within_its_bounds() {
    let waits = waits();
    assert_eq!(waits.create(Kind::Semaphore { max: 2 }, 3), Err(Error::Invalid));
    let semaphore = waits.create(Kind::Semaphore { max: 2 }, 0).unwrap();
    assert_eq!(waits.release_semaphore(semaphore, 3), Err(Error::TooManyPosts));
    assert_eq!(waits.release_semaphore(semaphore, 2), Ok(0));
    assert_eq!(waits.release_semaphore(semaphore, 1), Err(Error::TooManyPosts));
    assert_eq!(waits.wait(1, &[semaphore], false, false, None), Ok(Outcome::Object(0)));
    assert_eq!(waits.query(semaphore), Ok(1));
}

#[test]
fn apc_ends_an_alertable_wait_and_waits_for_one_that_is_not() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let alertable = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(1, &[event], false, true, LONG).unwrap())
    };
    waiting(&waits, 1);
    waits.queue_apc(1);
    assert_eq!(alertable.join().unwrap(), Outcome::IoCompletion);
    assert_eq!(waits.take_apcs(1), 1);
    waits.queue_apc(2);
    assert_eq!(waits.wait(2, &[event], false, false, SHORT), Ok(Outcome::Timeout), "a wait that is not alertable ignores the APC");
    assert_eq!(waits.wait(2, &[event], false, true, None), Ok(Outcome::IoCompletion), "the APC is still queued for the next one");
}

#[test]
fn suspended_waiter_takes_nothing_until_it_runs_again() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let suspended = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(1, &[event], false, false, LONG).unwrap())
    };
    waiting(&waits, 1);
    waits.suspend(1);
    waits.set(event).unwrap();
    assert_eq!(waits.query(event), Ok(1), "the event waits for the suspended thread");
    waits.resume(1);
    assert_eq!(suspended.join().unwrap(), Outcome::Object(0));
    assert_eq!(waits.query(event), Ok(0));
}

#[test]
fn terminated_waiter_ends_its_wait() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let waiter = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(1, &[event], false, false, None).unwrap())
    };
    waiting(&waits, 1);
    waits.thread_ended(1);
    assert_eq!(waiter.join().unwrap(), Outcome::Ended);
    waits.set(event).unwrap();
    assert_eq!(waits.query(event), Ok(1), "an ended wait takes nothing");
}

#[test]
fn interrupted_wait_takes_nothing_and_leaves_the_line() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let waiter = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(1, &[event], false, false, None).unwrap())
    };
    waiting(&waits, 1);
    waits.interrupt(1);
    assert_eq!(waiter.join().unwrap(), Outcome::Interrupted);
    waits.set(event).unwrap();
    assert_eq!(waits.query(event), Ok(1));
}

#[test]
fn interruption_before_the_wait_ends_the_next_wait_that_blocks() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    waits.interrupt(1);
    assert_eq!(waits.wait(1, &[event], false, false, Some(Duration::ZERO)), Ok(Outcome::Timeout), "a wait that only looks keeps it");
    assert!(!waits.waiting(1));
    assert_eq!(waits.wait(1, &[event], false, false, None), Ok(Outcome::Interrupted));
    assert_eq!(waits.wait(1, &[event], false, false, SHORT), Ok(Outcome::Timeout), "it is taken once");
}

#[test]
fn detached_object_ends_the_waits_for_it() {
    let waits = waits();
    let event = waits.create(Kind::ManualEvent, 0).unwrap();
    let other = waits.create(Kind::ManualEvent, 0).unwrap();
    let waiter = {
        let waits = waits.clone();
        std::thread::spawn(move || waits.wait(1, &[other, event], false, false, None).unwrap())
    };
    waiting(&waits, 1);
    assert_eq!(waits.detach(event), Ok((0, 0, 0)));
    assert_eq!(waiter.join().unwrap(), Outcome::Interrupted);
    assert_eq!(waits.set(event), Err(Error::Invalid));
    let mutex = waits.create(Kind::Mutex, 4).unwrap();
    assert_eq!(waits.detach(mutex), Ok((0, 4, 1)));
}

#[test]
fn ffi_gives_windows_values() {
    unsafe {
        let waits = ffi::raven_waits_new();
        let event = ffi::raven_waits_create(waits, 1, 0, 0);
        let mutex = ffi::raven_waits_create(waits, 2, 0, 0);
        let objects = [event, mutex];
        assert_eq!(ffi::raven_waits_wait(waits, 1, objects.as_ptr(), 2, 1, 0, 0), 0x102);
        assert_eq!(ffi::raven_waits_wait(waits, 1, objects.as_ptr(), 2, 0, 0, 0), 1);
        let mut previous = 7;
        assert_eq!(ffi::raven_waits_release_mutex(waits, mutex, 2, &mut previous), 288);
        assert_eq!(previous, 7);
        ffi::raven_waits_thread_ended(waits, 1);
        assert_eq!(ffi::raven_waits_wait(waits, 2, objects.as_ptr(), 2, 0, 0, u64::MAX), 0x81);
        assert_eq!(ffi::raven_waits_set(waits, 99, std::ptr::null_mut()), 6);
        ffi::raven_waits_free(waits);
    }
}

/// A wait lined up without parking, as a host thread that runs several guest threads makes it: whoever grants it tells its word, and
/// leave gives what it took. The kernel's objects have no such line.
fn lined(waits: &Table, thread: ThreadId, objects: &[ObjectId], signal: &std::sync::atomic::AtomicU32) -> Option<Result<Option<Outcome>, Error>> {
    // Safety: every case keeps its word until the wait has left the line.
    waits.line_up(thread, objects, false, false, unsafe { Signal::new(signal) })
}

#[test]
fn a_lined_up_wait_is_told_when_its_object_is_set() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let signal = std::sync::atomic::AtomicU32::new(0);
    let Some(lined_up) = lined(&waits, 1, &[event], &signal) else {
        assert!(waits.kernel());
        return;
    };
    assert_eq!(lined_up, Ok(None));
    assert!(waits.waiting(1));
    let setter = waits.clone();
    std::thread::spawn(move || setter.set(event).unwrap()).join().unwrap();
    assert_eq!(signal.load(std::sync::atomic::Ordering::Acquire), 1);
    assert_eq!(waits.leave(1), Some(Outcome::Object(0)));
    assert_eq!(waits.query(event), Ok(0), "the wait took the event");
    assert!(!waits.waiting(1));
}

#[test]
fn a_satisfied_wait_is_not_lined_up() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 1).unwrap();
    let signal = std::sync::atomic::AtomicU32::new(0);
    let Some(lined_up) = lined(&waits, 1, &[event], &signal) else { return };
    assert_eq!(lined_up, Ok(Some(Outcome::Object(0))));
    assert_eq!(signal.load(std::sync::atomic::Ordering::Acquire), 0);
    assert!(!waits.waiting(1));
}

#[test]
fn a_wait_that_leaves_the_line_takes_nothing() {
    let waits = waits();
    let event = waits.create(Kind::AutoEvent, 0).unwrap();
    let signal = std::sync::atomic::AtomicU32::new(0);
    if lined(&waits, 1, &[event], &signal).is_none() {
        return;
    }
    assert_eq!(waits.leave(1), None);
    assert!(!waits.waiting(1));
    waits.set(event).unwrap();
    assert_eq!(signal.load(std::sync::atomic::Ordering::Acquire), 0);
    assert_eq!(waits.query(event), Ok(1), "nobody took the event");
}

#[test]
fn a_lined_up_wait_ends_with_its_thread() {
    let waits = waits();
    let event = waits.create(Kind::ManualEvent, 0).unwrap();
    let signal = std::sync::atomic::AtomicU32::new(0);
    if lined(&waits, 1, &[event], &signal).is_none() {
        return;
    }
    waits.thread_ended(1);
    assert_eq!(signal.load(std::sync::atomic::Ordering::Acquire), 1);
    assert_eq!(waits.leave(1), Some(Outcome::Ended));
}
