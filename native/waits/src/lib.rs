//! The waits on kernel objects, as native/model/Waits.tla specifies them: events (reset by hand or on their own, pulsed too), mutexes
//! (owned, taken again by their owner, abandoned when it ends), semaphores; waits for any or all of a set, alertable or not, with
//! time-outs, suspension and termination.
//!
//! Whoever changes an object hands it, under the table's lock and in the same step, to the waits it now satisfies, oldest first, and
//! wakes their threads; a waiting thread never looks for itself. A thread here is a host thread waiting for its guest thread: it parks,
//! which on Linux is a futex wait on a word of its own, and is woken by the one that granted its wait.

#[cfg(loom)]
use loom::{sync::Mutex, thread};
#[cfg(not(loom))]
use std::{sync::Mutex, thread};

use std::collections::{HashMap, VecDeque};
use std::time::{Duration, Instant};

pub mod ffi;

pub type ObjectId = u32;
pub type ThreadId = u32;

/// No thread: the owner of a mutex nobody holds.
const NOBODY: ThreadId = 0;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    AutoEvent,
    ManualEvent,
    Mutex,
    Semaphore { max: u32 },
}

/// How a wait ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// It took the object at this index of the set (for a wait for all, the first).
    Object(u32),
    /// It took an abandoned mutex at this index.
    Abandoned(u32),
    /// An APC was queued to the thread, which waited alertably.
    IoCompletion,
    Timeout,
    /// The thread was terminated while it waited.
    Ended,
    /// The host called the thread away to do something for it (see Waits::interrupt); the wait took nothing and begins again after.
    Interrupted,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// No such object, or an operation its kind does not have.
    Invalid,
    /// ReleaseMutex by a thread that does not own the mutex.
    NotOwner,
    /// ReleaseSemaphore past the semaphore's most.
    TooManyPosts,
}

struct Object {
    kind: Kind,
    /// An event's state (0 or 1) or a semaphore's count.
    count: u32,
    owner: ThreadId,
    depth: u32,
    abandoned: bool,
}

struct Pending {
    objects: Vec<ObjectId>,
    all: bool,
    alertable: bool,
}

#[derive(Default)]
struct ThreadState {
    apcs: u32,
    suspended: bool,
    wait: Option<Pending>,
    outcome: Option<Outcome>,
    parked: Option<thread::Thread>,
    /// An interruption that came while the thread did not wait: its next wait that would block ends at once.
    interrupted: bool,
}

#[derive(Default)]
struct State {
    objects: HashMap<ObjectId, Object>,
    threads: HashMap<ThreadId, ThreadState>,
    /// The waiting threads, oldest first.
    line: VecDeque<ThreadId>,
    next: ObjectId,
}

impl State {
    fn free(&self, thread: ThreadId, object: ObjectId) -> bool {
        let object = &self.objects[&object];
        match object.kind {
            Kind::Mutex => object.owner == NOBODY || object.owner == thread,
            _ => object.count > 0,
        }
    }

    fn satisfied(&self, thread: ThreadId, objects: &[ObjectId], all: bool) -> bool {
        if all {
            objects.iter().all(|&object| self.free(thread, object))
        } else {
            objects.iter().any(|&object| self.free(thread, object))
        }
    }

    /// Takes what the wait satisfied: the free object of the lowest index for a wait for any, every object for one for all.
    fn take(&mut self, thread: ThreadId, objects: &[ObjectId], all: bool) -> Outcome {
        let taken: Vec<usize> = if all {
            (0..objects.len()).collect()
        } else {
            vec![(0..objects.len()).find(|&index| self.free(thread, objects[index])).expect("a satisfied wait has a free object")]
        };
        let mut abandoned = None;
        for &index in &taken {
            let object = self.objects.get_mut(&objects[index]).expect("waits are for objects that exist");
            match object.kind {
                Kind::AutoEvent | Kind::Semaphore { .. } => object.count -= 1,
                Kind::ManualEvent => {}
                Kind::Mutex => {
                    object.owner = thread;
                    object.depth += 1;
                    if std::mem::take(&mut object.abandoned) && abandoned.is_none() {
                        abandoned = Some(index as u32);
                    }
                }
            }
        }
        let first = if all { 0 } else { taken[0] as u32 };
        match abandoned {
            Some(_) => Outcome::Abandoned(first),
            None => Outcome::Object(first),
        }
    }

    /// Ends the wait of a thread in the line with `outcome` and wakes it.
    fn end(&mut self, thread: ThreadId, outcome: Outcome) {
        self.line.retain(|&other| other != thread);
        let state = self.threads.get_mut(&thread).expect("a waiting thread is known");
        state.wait = None;
        state.outcome = Some(outcome);
        if let Some(parked) = &state.parked {
            parked.unpark();
        }
    }

    /// Hands the objects to the waits in the line that they satisfy, oldest first. A grant only takes, so one pass is enough. A
    /// suspended thread is passed over: it takes nothing until it runs again.
    fn grant(&mut self) -> u32 {
        let mut granted = 0;
        let line: Vec<ThreadId> = self.line.iter().copied().collect();
        for thread in line {
            let state = &self.threads[&thread];
            if state.suspended {
                continue;
            }
            let pending = state.wait.as_ref().expect("a thread in the line waits");
            if !self.satisfied(thread, &pending.objects, pending.all) {
                continue;
            }
            let (objects, all) = (pending.objects.clone(), pending.all);
            let outcome = self.take(thread, &objects, all);
            self.end(thread, outcome);
            granted += 1;
        }
        granted
    }

    fn object(&mut self, object: ObjectId) -> Result<&mut Object, Error> {
        self.objects.get_mut(&object).ok_or(Error::Invalid)
    }

    fn event(&mut self, object: ObjectId) -> Result<&mut Object, Error> {
        let object = self.object(object)?;
        match object.kind {
            Kind::AutoEvent | Kind::ManualEvent => Ok(object),
            _ => Err(Error::Invalid),
        }
    }
}

#[derive(Default)]
pub struct Waits {
    state: Mutex<State>,
}

impl Waits {
    pub fn new() -> Self {
        Self::default()
    }

    fn lock(&self) -> impl std::ops::DerefMut<Target = State> + '_ {
        self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// A new object: `initial` is an event's state (0 or 1), a semaphore's count, or the thread that owns a new mutex (0 for none).
    pub fn create(&self, kind: Kind, initial: u32) -> Result<ObjectId, Error> {
        let object = match kind {
            Kind::AutoEvent | Kind::ManualEvent => Object { kind, count: u32::from(initial != 0), owner: NOBODY, depth: 0, abandoned: false },
            Kind::Mutex => Object { kind, count: 0, owner: initial, depth: u32::from(initial != NOBODY), abandoned: false },
            Kind::Semaphore { max } if max == 0 || initial > max => return Err(Error::Invalid),
            Kind::Semaphore { .. } => Object { kind, count: initial, owner: NOBODY, depth: 0, abandoned: false },
        };
        let mut state = self.lock();
        state.next += 1;
        let id = state.next;
        state.objects.insert(id, object);
        Ok(id)
    }

    /// Forgets an object. The host calls it when the last handle to the object closes, when no wait can still be for it.
    pub fn close(&self, object: ObjectId) {
        self.lock().objects.remove(&object);
    }

    pub fn set(&self, object: ObjectId) -> Result<u32, Error> {
        let mut state = self.lock();
        let event = state.event(object)?;
        let previous = std::mem::replace(&mut event.count, 1);
        state.grant();
        Ok(previous)
    }

    pub fn reset(&self, object: ObjectId) -> Result<u32, Error> {
        let mut state = self.lock();
        Ok(std::mem::replace(&mut state.event(object)?.count, 0))
    }

    /// PulseEvent: the event is set for the waits there are now, and reset. Gives back the state before and how many waits it ended.
    pub fn pulse(&self, object: ObjectId) -> Result<(u32, u32), Error> {
        let mut state = self.lock();
        let previous = std::mem::replace(&mut state.event(object)?.count, 1);
        let granted = state.grant();
        state.event(object)?.count = 0;
        Ok((previous, granted))
    }

    /// Takes an object out of the table, for an object that goes to memory other processes share: it gives back what the object holds
    /// (an event's state or a semaphore's count, a mutex's owner and depth), and the waits for it end interrupted, to wait again where
    /// the object is now.
    pub fn detach(&self, object: ObjectId) -> Result<(u32, ThreadId, u32), Error> {
        let mut state = self.lock();
        let removed = state.objects.remove(&object).ok_or(Error::Invalid)?;
        let waiting: Vec<ThreadId> = state
            .line
            .iter()
            .copied()
            .filter(|thread| state.threads[thread].wait.as_ref().is_some_and(|pending| pending.objects.contains(&object)))
            .collect();
        for thread in waiting {
            state.end(thread, Outcome::Interrupted);
        }
        Ok((removed.count, removed.owner, removed.depth))
    }

    /// Whether an event is set, a semaphore's count, or a mutex's owner.
    pub fn query(&self, object: ObjectId) -> Result<u32, Error> {
        let mut state = self.lock();
        let object = state.object(object)?;
        Ok(if object.kind == Kind::Mutex { object.owner } else { object.count })
    }

    pub fn release_mutex(&self, object: ObjectId, thread: ThreadId) -> Result<u32, Error> {
        let mut state = self.lock();
        let mutex = state.object(object)?;
        if mutex.kind != Kind::Mutex {
            return Err(Error::Invalid);
        }
        if mutex.owner != thread || thread == NOBODY {
            return Err(Error::NotOwner);
        }
        let previous = mutex.depth;
        mutex.depth -= 1;
        if mutex.depth == 0 {
            mutex.owner = NOBODY;
            state.grant();
        }
        Ok(previous)
    }

    /// Adds `count` to a semaphore and gives back the count before.
    pub fn release_semaphore(&self, object: ObjectId, count: u32) -> Result<u32, Error> {
        let mut state = self.lock();
        let semaphore = state.object(object)?;
        let Kind::Semaphore { max } = semaphore.kind else { return Err(Error::Invalid) };
        let previous = semaphore.count;
        if count == 0 || count > max - previous {
            return Err(Error::TooManyPosts);
        }
        semaphore.count += count;
        state.grant();
        Ok(previous)
    }

    /// WaitForMultipleObjects (or for one object), from the host thread that stands for `thread`. A time-out of zero only looks; none
    /// waits for as long as it takes.
    pub fn wait(&self, thread: ThreadId, objects: &[ObjectId], all: bool, alertable: bool, timeout: Option<Duration>) -> Result<Outcome, Error> {
        let deadline = timeout.map(|timeout| Instant::now() + timeout);
        {
            let mut state = self.lock();
            if objects.is_empty() || objects.iter().any(|object| !state.objects.contains_key(object)) {
                return Err(Error::Invalid);
            }
            let me = state.threads.entry(thread).or_default();
            if alertable && me.apcs > 0 {
                return Ok(Outcome::IoCompletion);
            }
            if state.satisfied(thread, objects, all) {
                return Ok(state.take(thread, objects, all));
            }
            if timeout == Some(Duration::ZERO) {
                return Ok(Outcome::Timeout);
            }
            let me = state.threads.get_mut(&thread).expect("the thread was added above");
            if std::mem::take(&mut me.interrupted) {
                return Ok(Outcome::Interrupted);
            }
            me.wait = Some(Pending { objects: objects.to_vec(), all, alertable });
            me.outcome = None;
            me.parked = Some(thread::current());
            state.line.push_back(thread);
        }
        loop {
            park(deadline);
            let mut state = self.lock();
            // A thread that ended is forgotten once its wait has ended too.
            let Some(me) = state.threads.get_mut(&thread) else { return Ok(Outcome::Ended) };
            if let Some(outcome) = me.outcome.take() {
                return Ok(outcome);
            }
            if deadline.is_some_and(|deadline| Instant::now() >= deadline) {
                state.end(thread, Outcome::Timeout);
                state.threads.get_mut(&thread).expect("the thread waited").outcome = None;
                return Ok(Outcome::Timeout);
            }
        }
    }

    /// An APC was queued to `thread`: an alertable wait ends for it; otherwise it counts until the thread waits alertably.
    pub fn queue_apc(&self, thread: ThreadId) {
        let mut state = self.lock();
        let me = state.threads.entry(thread).or_default();
        me.apcs += 1;
        if me.wait.as_ref().is_some_and(|pending| pending.alertable) && !me.suspended {
            state.end(thread, Outcome::IoCompletion);
        }
    }

    /// The host ran the APCs queued to `thread`; gives back how many there were.
    pub fn take_apcs(&self, thread: ThreadId) -> u32 {
        self.lock().threads.get_mut(&thread).map_or(0, |me| std::mem::take(&mut me.apcs))
    }

    /// Ends the wait of `thread` without its taking anything, so that its host thread can do work only it can do (ptrace answers
    /// only the thread that traces); the host waits again after. As for a time-out, the model has the wait leave the line.
    pub fn interrupt(&self, thread: ThreadId) {
        let mut state = self.lock();
        let me = state.threads.entry(thread).or_default();
        if me.wait.is_some() {
            state.end(thread, Outcome::Interrupted);
        } else {
            me.interrupted = true;
        }
    }

    /// Whether `thread` stands in the line, its wait not yet ended.
    pub fn waiting(&self, thread: ThreadId) -> bool {
        self.lock().line.contains(&thread)
    }

    pub fn suspend(&self, thread: ThreadId) {
        self.lock().threads.entry(thread).or_default().suspended = true;
    }

    /// A thread that runs again takes what its wait may take now, and an APC queued meanwhile ends an alertable wait.
    pub fn resume(&self, thread: ThreadId) {
        let mut state = self.lock();
        let me = state.threads.entry(thread).or_default();
        me.suspended = false;
        if me.apcs > 0 && me.wait.as_ref().is_some_and(|pending| pending.alertable) {
            state.end(thread, Outcome::IoCompletion);
        } else {
            state.grant();
        }
    }

    /// A thread ended, by itself or terminated: its wait ends, and the mutexes it holds are abandoned and go to the waits for them.
    pub fn thread_ended(&self, thread: ThreadId) {
        let mut state = self.lock();
        if state.threads.get(&thread).is_some_and(|me| me.wait.is_some()) {
            state.end(thread, Outcome::Ended);
        }
        for object in state.objects.values_mut() {
            if object.kind == Kind::Mutex && object.owner == thread {
                object.owner = NOBODY;
                object.depth = 0;
                object.abandoned = true;
            }
        }
        state.grant();
        state.threads.remove(&thread);
    }
}

#[cfg(not(loom))]
fn park(deadline: Option<Instant>) {
    match deadline {
        None => thread::park(),
        Some(deadline) => thread::park_timeout(deadline.saturating_duration_since(Instant::now())),
    }
}

/// loom has no time: the checked cases wait without a time-out.
#[cfg(loom)]
fn park(deadline: Option<Instant>) {
    assert!(deadline.is_none(), "the loom cases wait without a time-out");
    thread::park();
}

#[cfg(all(test, not(loom)))]
mod tests;
