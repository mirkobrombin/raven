//! The same waits on the kernel's NT synchronization objects (/dev/ntsync, Linux 6.14). Each object of the table is an object of the
//! kernel, and a host thread waits for its guest thread in the kernel with NTSYNC_IOC_WAIT_ANY or NTSYNC_IOC_WAIT_ALL: the kernel
//! hands objects to the waits they satisfy, with the semantics of WaitForMultipleObjects (ownership and abandonment of mutexes, waits
//! for all taken at once), and no lock of the host is held while a thread waits.
//!
//! What the kernel does not know is kept here, under a lock taken only around a wait and by those who end one: APCs, suspension,
//! termination and interruption. Each thread has an alert event of its own, which the kernel looks at with the objects of every wait;
//! whoever must end a wait sets it, and the thread finds out why from its state here.

use crate::{Error, Kind, ObjectId, Outcome, ThreadId};
use std::collections::HashMap;
use std::fs::File;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::sync::{Condvar, Mutex, MutexGuard};
use std::time::Duration;

const fn ioc(direction: u64, number: u64, size: u64) -> u64 {
    (direction << 30) | (size << 16) | ((b'N' as u64) << 8) | number
}
const WRITE: u64 = 1;
const READ: u64 = 2;
const CREATE_SEM: u64 = ioc(WRITE, 0x80, 8);
const SEM_RELEASE: u64 = ioc(READ | WRITE, 0x81, 4);
const WAIT_ANY: u64 = ioc(READ | WRITE, 0x82, 40);
const WAIT_ALL: u64 = ioc(READ | WRITE, 0x83, 40);
const CREATE_MUTEX: u64 = ioc(WRITE, 0x84, 8);
const MUTEX_UNLOCK: u64 = ioc(READ | WRITE, 0x85, 8);
const MUTEX_KILL: u64 = ioc(WRITE, 0x86, 4);
const CREATE_EVENT: u64 = ioc(WRITE, 0x87, 8);
const EVENT_SET: u64 = ioc(READ, 0x88, 4);
const EVENT_RESET: u64 = ioc(READ, 0x89, 4);
const EVENT_PULSE: u64 = ioc(READ, 0x8a, 4);
const SEM_READ: u64 = ioc(READ, 0x8b, 8);
const MUTEX_READ: u64 = ioc(READ, 0x8c, 8);
const EVENT_READ: u64 = ioc(READ, 0x8d, 8);

const EPERM: i32 = 1;
const EINTR: i32 = 4;
const EOVERFLOW: i32 = 75;
const ETIMEDOUT: i32 = 110;
const EOWNERDEAD: i32 = 130;
const CLOCK_MONOTONIC: i32 = 1;

#[repr(C)]
#[derive(Default)]
struct Pair {
    first: u32,
    second: u32,
}

#[repr(C)]
#[derive(Default)]
struct WaitArgs {
    timeout: u64,
    objects: u64,
    count: u32,
    index: u32,
    flags: u32,
    owner: u32,
    alert: u32,
    pad: u32,
}

#[repr(C)]
struct Timespec {
    seconds: i64,
    nanoseconds: i64,
}

extern "C" {
    fn ioctl(descriptor: i32, request: u64, ...) -> i32;
    fn clock_gettime(clock: i32, time: *mut Timespec) -> i32;
}

/// An ioctl on `descriptor` with `argument`: its result, or the error number.
fn control<T>(descriptor: RawFd, request: u64, argument: &mut T) -> Result<i32, i32> {
    // SAFETY: each request is given the structure of the size the request number encodes.
    let result = unsafe { ioctl(descriptor, request, argument as *mut T) };
    if result < 0 {
        Err(std::io::Error::last_os_error().raw_os_error().unwrap_or(0))
    } else {
        Ok(result)
    }
}

fn monotonic_now() -> u64 {
    let mut now = Timespec { seconds: 0, nanoseconds: 0 };
    // SAFETY: `now` is a writable timespec.
    unsafe { clock_gettime(CLOCK_MONOTONIC, &mut now) };
    now.seconds as u64 * 1_000_000_000 + now.nanoseconds as u64
}

struct Object {
    kind: Kind,
    descriptor: OwnedFd,
}

#[derive(Default)]
struct ThreadState {
    apcs: u32,
    suspended: bool,
    ended: bool,
    /// An interruption that came while the thread did not wait: its next wait that would block ends at once.
    interrupted: bool,
    /// The wait in the kernel, with the objects it is for and whether it is alertable.
    waiting: Option<(Vec<ObjectId>, bool)>,
    /// Why the thread's alert was set, when it was for an interruption or a detached object.
    called_away: bool,
    alert: Option<OwnedFd>,
    /// Whether the alert may be set, to be reset after the wait.
    alerted: bool,
}

#[derive(Default)]
struct State {
    objects: HashMap<ObjectId, Object>,
    threads: HashMap<ThreadId, ThreadState>,
    next: ObjectId,
}

pub struct Waits {
    device: File,
    /// An event that stays set: the alert of a wait that only looks, which the kernel then ends at once when it takes nothing.
    set: OwnedFd,
    state: Mutex<State>,
    /// Signalled when a suspended thread runs again.
    resumed: Condvar,
}

impl Waits {
    /// The waits on /dev/ntsync, or None where the kernel has none.
    pub fn open() -> Option<Self> {
        let device = File::options().read(true).write(true).open("/dev/ntsync").ok()?;
        let mut set = Pair { first: 1, second: 1 };
        let set = control(device.as_raw_fd(), CREATE_EVENT, &mut set).ok()?;
        // SAFETY: the kernel gave back a new descriptor of its own.
        let set = unsafe { OwnedFd::from_raw_fd(set) };
        Some(Self { device, set, state: Mutex::default(), resumed: Condvar::new() })
    }

    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn make(&self, request: u64, mut argument: Pair) -> Result<OwnedFd, Error> {
        let descriptor = control(self.device.as_raw_fd(), request, &mut argument).map_err(|_| Error::Invalid)?;
        // SAFETY: the kernel gave back a new descriptor of its own.
        Ok(unsafe { OwnedFd::from_raw_fd(descriptor) })
    }

    fn descriptor(state: &State, object: ObjectId) -> Result<RawFd, Error> {
        state.objects.get(&object).map(|object| object.descriptor.as_raw_fd()).ok_or(Error::Invalid)
    }

    fn event(state: &State, object: ObjectId) -> Result<RawFd, Error> {
        match state.objects.get(&object) {
            Some(Object { kind: Kind::AutoEvent | Kind::ManualEvent, descriptor }) => Ok(descriptor.as_raw_fd()),
            _ => Err(Error::Invalid),
        }
    }

    /// Sets the alert of a thread that waits in the kernel, which ends its wait there.
    fn alert(thread: &mut ThreadState) {
        if let (Some(_), Some(alert)) = (&thread.waiting, &thread.alert) {
            let _ = control(alert.as_raw_fd(), EVENT_SET, &mut 0u32);
            thread.alerted = true;
        }
    }

    /// A wait with a time-out of zero: it takes what is free now, with the event that stays set as its alert, in one call.
    fn look(&self, thread: ThreadId, objects: &[ObjectId], all: bool, alertable: bool) -> Result<Outcome, Error> {
        let mut descriptors = [0u32; 64];
        {
            let state = self.lock();
            if objects.is_empty() || objects.len() > descriptors.len() {
                return Err(Error::Invalid);
            }
            if alertable && state.threads.get(&thread).is_some_and(|me| me.apcs > 0) {
                return Ok(Outcome::IoCompletion);
            }
            for (slot, &object) in descriptors.iter_mut().zip(objects) {
                *slot = Self::descriptor(&state, object)? as u32;
            }
        }
        let mut args = WaitArgs {
            timeout: u64::MAX,
            objects: descriptors.as_ptr() as u64,
            count: objects.len() as u32,
            owner: thread,
            alert: self.set.as_raw_fd() as u32,
            ..WaitArgs::default()
        };
        let result = loop {
            match control(self.device.as_raw_fd(), if all { WAIT_ALL } else { WAIT_ANY }, &mut args) {
                Err(EINTR) => continue,
                result => break result,
            }
        };
        let first = if all { 0 } else { args.index };
        match result {
            Ok(_) if args.index < args.count => Ok(Outcome::Object(first)),
            Err(EOWNERDEAD) => Ok(Outcome::Abandoned(first)),
            Ok(_) | Err(ETIMEDOUT) => Ok(Outcome::Timeout),
            Err(_) => Err(Error::Invalid),
        }
    }

    pub fn create(&self, kind: Kind, initial: u32) -> Result<ObjectId, Error> {
        let descriptor = match kind {
            Kind::AutoEvent | Kind::ManualEvent => {
                self.make(CREATE_EVENT, Pair { first: u32::from(kind == Kind::ManualEvent), second: u32::from(initial != 0) })?
            }
            Kind::Mutex => self.make(CREATE_MUTEX, Pair { first: initial, second: u32::from(initial != 0) })?,
            Kind::Semaphore { max } if max == 0 || initial > max => return Err(Error::Invalid),
            Kind::Semaphore { max } => self.make(CREATE_SEM, Pair { first: initial, second: max })?,
        };
        let mut state = self.lock();
        state.next += 1;
        let id = state.next;
        state.objects.insert(id, Object { kind, descriptor });
        Ok(id)
    }

    pub fn close(&self, object: ObjectId) {
        self.lock().objects.remove(&object);
    }

    fn event_call(&self, object: ObjectId, request: u64) -> Result<u32, Error> {
        let descriptor = Self::event(&self.lock(), object)?;
        let mut previous = 0u32;
        control(descriptor, request, &mut previous).map_err(|_| Error::Invalid)?;
        Ok(previous)
    }

    pub fn set(&self, object: ObjectId) -> Result<u32, Error> {
        self.event_call(object, EVENT_SET)
    }

    pub fn reset(&self, object: ObjectId) -> Result<u32, Error> {
        self.event_call(object, EVENT_RESET)
    }

    /// PulseEvent. The kernel does not tell how many waits a pulse ended: for an event reset on its own, it is set and reset again,
    /// and the reset finds it taken when a wait took it in between; one reset by hand ends every wait there is, which is told as one.
    pub fn pulse(&self, object: ObjectId) -> Result<(u32, u32), Error> {
        let (descriptor, kind) = {
            let state = self.lock();
            (Self::event(&state, object)?, state.objects[&object].kind)
        };
        let mut previous = 0u32;
        if kind == Kind::ManualEvent {
            control(descriptor, EVENT_PULSE, &mut previous).map_err(|_| Error::Invalid)?;
            return Ok((previous, 1));
        }
        control(descriptor, EVENT_SET, &mut previous).map_err(|_| Error::Invalid)?;
        let mut left = 0u32;
        control(descriptor, EVENT_RESET, &mut left).map_err(|_| Error::Invalid)?;
        Ok((previous, u32::from(left == 0)))
    }

    /// What the object holds, as Waits::detach gives it, and its descriptor closed; the waits for it end interrupted.
    pub fn detach(&self, object: ObjectId) -> Result<(u32, ThreadId, u32), Error> {
        let mut state = self.lock();
        let removed = state.objects.remove(&object).ok_or(Error::Invalid)?;
        let held = Self::read(&removed)?;
        for thread in state.threads.values_mut() {
            if thread.waiting.as_ref().is_some_and(|(objects, _)| objects.contains(&object)) {
                thread.called_away = true;
                Self::alert(thread);
            }
        }
        Ok(held)
    }

    /// An event's state or a semaphore's count, with a mutex's owner and depth.
    fn read(object: &Object) -> Result<(u32, ThreadId, u32), Error> {
        let mut pair = Pair::default();
        let request = match object.kind {
            Kind::AutoEvent | Kind::ManualEvent => EVENT_READ,
            Kind::Mutex => MUTEX_READ,
            Kind::Semaphore { .. } => SEM_READ,
        };
        match control(object.descriptor.as_raw_fd(), request, &mut pair) {
            Ok(_) => {}
            // An abandoned mutex is read all the same: nobody owns it.
            Err(EOWNERDEAD) => return Ok((0, 0, 0)),
            Err(_) => return Err(Error::Invalid),
        }
        Ok(match object.kind {
            Kind::AutoEvent | Kind::ManualEvent => (pair.second, 0, 0),
            Kind::Mutex => (0, pair.first, pair.second),
            Kind::Semaphore { .. } => (pair.first, 0, 0),
        })
    }

    pub fn query(&self, object: ObjectId) -> Result<u32, Error> {
        let state = self.lock();
        let object = state.objects.get(&object).ok_or(Error::Invalid)?;
        let (count, owner, _) = Self::read(object)?;
        Ok(if object.kind == Kind::Mutex { owner } else { count })
    }

    pub fn release_mutex(&self, object: ObjectId, thread: ThreadId) -> Result<u32, Error> {
        let descriptor = {
            let state = self.lock();
            match state.objects.get(&object) {
                Some(Object { kind: Kind::Mutex, descriptor }) => descriptor.as_raw_fd(),
                _ => return Err(Error::Invalid),
            }
        };
        if thread == 0 {
            return Err(Error::NotOwner);
        }
        let mut pair = Pair { first: thread, second: 0 };
        match control(descriptor, MUTEX_UNLOCK, &mut pair) {
            Ok(_) => Ok(pair.second),
            Err(EPERM) => Err(Error::NotOwner),
            Err(_) => Err(Error::Invalid),
        }
    }

    pub fn release_semaphore(&self, object: ObjectId, count: u32) -> Result<u32, Error> {
        let descriptor = {
            let state = self.lock();
            match state.objects.get(&object) {
                Some(Object { kind: Kind::Semaphore { .. }, descriptor }) => descriptor.as_raw_fd(),
                _ => return Err(Error::Invalid),
            }
        };
        if count == 0 {
            return Err(Error::TooManyPosts);
        }
        let mut posted = count;
        match control(descriptor, SEM_RELEASE, &mut posted) {
            Ok(_) => Ok(posted),
            Err(EOVERFLOW) => Err(Error::TooManyPosts),
            Err(_) => Err(Error::Invalid),
        }
    }

    pub fn wait(&self, thread: ThreadId, objects: &[ObjectId], all: bool, alertable: bool, timeout: Option<Duration>) -> Result<Outcome, Error> {
        if timeout == Some(Duration::ZERO) {
            return self.look(thread, objects, all, alertable);
        }
        let deadline = timeout.map(|timeout| monotonic_now().saturating_add(timeout.as_nanos().min(u64::MAX as u128) as u64));
        loop {
            let (descriptors, alert) = {
                let mut state = self.lock();
                if objects.is_empty() || objects.len() > 64 {
                    return Err(Error::Invalid);
                }
                let descriptors =
                    objects.iter().map(|&object| Self::descriptor(&state, object).map(|descriptor| descriptor as u32)).collect::<Result<Vec<_>, _>>()?;
                let alert = match state.threads.get(&thread).and_then(|me| me.alert.as_ref()) {
                    Some(alert) => alert.as_raw_fd(),
                    None => {
                        let alert = self.make(CREATE_EVENT, Pair { first: 1, second: 0 })?;
                        let raw = alert.as_raw_fd();
                        state.threads.entry(thread).or_default().alert = Some(alert);
                        raw
                    }
                };
                let me = state.threads.get_mut(&thread).expect("the thread was added above");
                if alertable && me.apcs > 0 {
                    return Ok(Outcome::IoCompletion);
                }
                // A suspended thread takes nothing until it runs again.
                while state.threads.get(&thread).is_some_and(|me| me.suspended && !me.ended) {
                    state = self.resumed.wait(state).unwrap_or_else(|poisoned| poisoned.into_inner());
                }
                let me = state.threads.get_mut(&thread).expect("only a wait forgets its thread");
                if me.ended {
                    state.threads.remove(&thread);
                    return Ok(Outcome::Ended);
                }
                me.waiting = Some((objects.to_vec(), alertable));
                (descriptors, alert)
            };
            let mut args = WaitArgs {
                timeout: deadline.unwrap_or(u64::MAX),
                objects: descriptors.as_ptr() as u64,
                count: descriptors.len() as u32,
                owner: thread,
                alert: alert as u32,
                ..WaitArgs::default()
            };
            // A wait whose time is already up, or that was interrupted before it began, only looks (see look): the kernel sleeps until its
            // next tick on a time already past, but takes a free object before it looks at the alert, so the thread's alert is set first
            // and the wait comes back at once, with an object or with the alert.
            let interrupted = std::mem::take(&mut self.lock().threads.get_mut(&thread).expect("the thread waits").interrupted);
            let look = interrupted || deadline.is_some_and(|deadline| monotonic_now() >= deadline);
            if look {
                args.timeout = u64::MAX;
                let _ = control(alert, EVENT_SET, &mut 0u32);
                self.lock().threads.get_mut(&thread).expect("the thread waits").alerted = true;
            }
            let result = loop {
                match control(self.device.as_raw_fd(), if all { WAIT_ALL } else { WAIT_ANY }, &mut args) {
                    Err(EINTR) => continue,
                    result => break result,
                }
            };
            let mut state = self.lock();
            let me = state.threads.get_mut(&thread).expect("only a wait forgets its thread");
            me.waiting = None;
            let called_away = std::mem::take(&mut me.called_away);
            if std::mem::take(&mut me.alerted) {
                let _ = control(alert, EVENT_RESET, &mut 0u32);
            }
            let first = if all { 0 } else { args.index };
            match result {
                Ok(_) if args.index < args.count => return Ok(Outcome::Object(first)),
                Err(EOWNERDEAD) => return Ok(Outcome::Abandoned(first)),
                Err(ETIMEDOUT) => return Ok(Outcome::Timeout),
                Ok(_) => {}
                Err(_) => return Err(Error::Invalid),
            }
            if look && !me.ended {
                return Ok(if interrupted { Outcome::Interrupted } else { Outcome::Timeout });
            }
            // The alert ended the wait: the thread was terminated, called away, given an APC, or suspended.
            if me.ended {
                state.threads.remove(&thread);
                return Ok(Outcome::Ended);
            }
            if called_away {
                return Ok(Outcome::Interrupted);
            }
            if alertable && me.apcs > 0 && !me.suspended {
                return Ok(Outcome::IoCompletion);
            }
            if deadline.is_some_and(|deadline| monotonic_now() >= deadline) {
                return Ok(Outcome::Timeout);
            }
        }
    }

    pub fn queue_apc(&self, thread: ThreadId) {
        let mut state = self.lock();
        let me = state.threads.entry(thread).or_default();
        me.apcs += 1;
        if me.waiting.as_ref().is_some_and(|(_, alertable)| *alertable) && !me.suspended {
            Self::alert(me);
        }
    }

    pub fn take_apcs(&self, thread: ThreadId) -> u32 {
        self.lock().threads.get_mut(&thread).map_or(0, |me| std::mem::take(&mut me.apcs))
    }

    pub fn interrupt(&self, thread: ThreadId) {
        let mut state = self.lock();
        let me = state.threads.entry(thread).or_default();
        if me.waiting.is_some() {
            me.called_away = true;
            Self::alert(me);
        } else {
            me.interrupted = true;
        }
    }

    pub fn waiting(&self, thread: ThreadId) -> bool {
        self.lock().threads.get(&thread).is_some_and(|me| me.waiting.is_some())
    }

    /// The thread's wait in the kernel ends and begins again once it runs, so that it takes nothing meanwhile.
    pub fn suspend(&self, thread: ThreadId) {
        let mut state = self.lock();
        let me = state.threads.entry(thread).or_default();
        me.suspended = true;
        Self::alert(me);
    }

    pub fn resume(&self, thread: ThreadId) {
        let mut state = self.lock();
        state.threads.entry(thread).or_default().suspended = false;
        self.resumed.notify_all();
    }

    /// The thread's wait ends, and the kernel abandons the mutexes it holds to the waits for them.
    pub fn thread_ended(&self, thread: ThreadId) {
        let mut state = self.lock();
        for object in state.objects.values().filter(|object| object.kind == Kind::Mutex) {
            let mut owner = thread;
            let _ = control(object.descriptor.as_raw_fd(), MUTEX_KILL, &mut owner);
        }
        let me = state.threads.entry(thread).or_default();
        if me.waiting.is_some() || me.suspended {
            me.ended = true;
            Self::alert(me);
            self.resumed.notify_all();
        } else {
            state.threads.remove(&thread);
        }
    }
}
