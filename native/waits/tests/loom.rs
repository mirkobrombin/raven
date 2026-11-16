//! Every interleaving loom finds of a few threads around the table: a wake that is lost leaves a thread parked for good, which loom
//! reports as a deadlock. Run with RUSTFLAGS="--cfg loom" cargo test --release --test loom.
#![cfg(loom)]

use loom::sync::Arc;
use loom::thread;
use raven_waits::{Kind, Outcome, Waits};

#[test]
fn set_does_not_miss_a_wait_that_is_starting() {
    loom::model(|| {
        let waits = Arc::new(Waits::new());
        let event = waits.create(Kind::AutoEvent, 0).unwrap();
        let waiter = {
            let waits = waits.clone();
            thread::spawn(move || waits.wait(1, &[event], false, false, None).unwrap())
        };
        waits.set(event).unwrap();
        assert_eq!(waiter.join().unwrap(), Outcome::Object(0));
        assert_eq!(waits.query(event), Ok(0));
    });
}

#[test]
fn two_releases_wake_two_waiters_of_a_semaphore() {
    loom::model(|| {
        let waits = Arc::new(Waits::new());
        let semaphore = waits.create(Kind::Semaphore { max: 2 }, 0).unwrap();
        let waiters: Vec<_> = (1..=2)
            .map(|thread| {
                let waits = waits.clone();
                thread::spawn(move || waits.wait(thread, &[semaphore], false, false, None).unwrap())
            })
            .collect();
        let releaser = {
            let waits = waits.clone();
            thread::spawn(move || {
                waits.release_semaphore(semaphore, 1).unwrap();
            })
        };
        waits.release_semaphore(semaphore, 1).unwrap();
        releaser.join().unwrap();
        for waiter in waiters {
            assert_eq!(waiter.join().unwrap(), Outcome::Object(0));
        }
        assert_eq!(waits.query(semaphore), Ok(0));
    });
}

#[test]
fn released_mutex_goes_to_its_waiter() {
    loom::model(|| {
        let waits = Arc::new(Waits::new());
        let mutex = waits.create(Kind::Mutex, 1).unwrap();
        let waiter = {
            let waits = waits.clone();
            thread::spawn(move || {
                let outcome = waits.wait(2, &[mutex], false, false, None).unwrap();
                waits.release_mutex(mutex, 2).unwrap();
                outcome
            })
        };
        waits.release_mutex(mutex, 1).unwrap();
        assert_eq!(waiter.join().unwrap(), Outcome::Object(0));
        assert_eq!(waits.query(mutex), Ok(0));
    });
}

#[test]
fn ended_owner_and_waiter_race_to_an_abandoned_mutex() {
    loom::model(|| {
        let waits = Arc::new(Waits::new());
        let mutex = waits.create(Kind::Mutex, 1).unwrap();
        let waiter = {
            let waits = waits.clone();
            thread::spawn(move || waits.wait(2, &[mutex], false, false, None).unwrap())
        };
        waits.thread_ended(1);
        assert_eq!(waiter.join().unwrap(), Outcome::Abandoned(0));
        assert_eq!(waits.query(mutex), Ok(2));
    });
}

#[test]
fn apc_and_set_race_for_an_alertable_wait() {
    loom::model(|| {
        let waits = Arc::new(Waits::new());
        let event = waits.create(Kind::AutoEvent, 0).unwrap();
        let waiter = {
            let waits = waits.clone();
            thread::spawn(move || waits.wait(1, &[event], false, true, None).unwrap())
        };
        let alerter = {
            let waits = waits.clone();
            thread::spawn(move || waits.queue_apc(1))
        };
        waits.set(event).unwrap();
        alerter.join().unwrap();
        // The wait ends one way or the other; the event is taken only by a wait it ended.
        match waiter.join().unwrap() {
            Outcome::Object(0) => assert_eq!(waits.query(event), Ok(0)),
            Outcome::IoCompletion => assert_eq!(waits.query(event), Ok(1)),
            other => panic!("the wait ended with {other:?}"),
        }
        assert_eq!(waits.take_apcs(1), 1);
    });
}

#[test]
fn wait_for_all_and_wait_for_any_share_a_semaphore() {
    loom::model(|| {
        let waits = Arc::new(Waits::new());
        let mutex = waits.create(Kind::Mutex, 0).unwrap();
        let semaphore = waits.create(Kind::Semaphore { max: 2 }, 0).unwrap();
        let all = {
            let waits = waits.clone();
            thread::spawn(move || {
                let outcome = waits.wait(1, &[mutex, semaphore], true, false, None).unwrap();
                waits.release_mutex(mutex, 1).unwrap();
                outcome
            })
        };
        let any = {
            let waits = waits.clone();
            thread::spawn(move || waits.wait(2, &[semaphore], false, false, None).unwrap())
        };
        waits.release_semaphore(semaphore, 1).unwrap();
        waits.release_semaphore(semaphore, 1).unwrap();
        assert_eq!(all.join().unwrap(), Outcome::Object(0));
        assert_eq!(any.join().unwrap(), Outcome::Object(0));
        assert_eq!(waits.query(mutex), Ok(0));
    });
}

#[test]
fn resume_hands_over_what_was_set_while_suspended() {
    loom::model(|| {
        let waits = Arc::new(Waits::new());
        let event = waits.create(Kind::ManualEvent, 0).unwrap();
        let waiter = {
            let waits = waits.clone();
            thread::spawn(move || waits.wait(1, &[event], false, false, None).unwrap())
        };
        waits.suspend(1);
        let setter = {
            let waits = waits.clone();
            thread::spawn(move || {
                waits.set(event).unwrap();
            })
        };
        waits.resume(1);
        setter.join().unwrap();
        assert_eq!(waiter.join().unwrap(), Outcome::Object(0));
    });
}
