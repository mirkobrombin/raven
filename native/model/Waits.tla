------------------------------- MODULE Waits -------------------------------
(* The waits on kernel objects as Windows defines them, and the protocol Raven's host follows to keep them: events (reset by hand or on
   their own, pulsed too), mutexes (owned, taken again by their owner, abandoned when it ends), semaphores; WaitForMultipleObjects for any
   and for all of a set, alertable or not, with APCs, time-outs, suspension and termination.

   The protocol: whoever changes an object hands it, under the objects' lock and in one step, to the waits it now satisfies, oldest
   first; a waiting thread does not look for itself. ntsync does the same in the kernel, the host does it without ntsync. What is checked:
   no thread waits while its wait could be satisfied (no lost wake), a mutex has one owner and only a live one, a semaphore stays within
   its bounds. *)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Threads,      \* the threads
          None,         \* no thread: the owner of a mutex nobody holds
          Kind,         \* the kind of each object: an object is a number in 1..Len(Kind)
          SemaphoreMax, \* the most a semaphore counts to
          MaxRecursion  \* how deep a mutex is taken again, to keep the model finite

Objects == 1..Len(Kind)
Events == {o \in Objects : Kind[o] \in {"auto", "manual"}}
Mutexes == {o \in Objects : Kind[o] = "mutex"}
Semaphores == {o \in Objects : Kind[o] = "semaphore"}

VARIABLES
    count,      \* an event's state (0 or 1) or a semaphore's count; unused for a mutex
    owner,      \* a mutex's owner, or None
    depth,      \* how many times its owner holds a mutex
    abandoned,  \* a mutex whose owner ended holding it, until someone takes it
    state,      \* "run", "wait", or "dead" for each thread
    wants,      \* the objects a waiting thread waits for
    all,        \* whether it waits for all of them
    alertable,  \* whether an APC ends its wait
    apcs,       \* the APCs queued to each thread
    suspended,  \* whether each thread is suspended
    line        \* the waiting threads, oldest first

vars == <<count, owner, depth, abandoned, state, wants, all, alertable, apcs, suspended, line>>

(* Whether object o can be taken by thread t, in the given counts and owners. *)
Free(c, w, t, o) == IF o \in Mutexes THEN w[o] = None \/ w[o] = t ELSE c[o] > 0

Satisfied(c, w, t, objects, every) ==
    IF every THEN \A o \in objects : Free(c, w, t, o) ELSE \E o \in objects : Free(c, w, t, o)

Lowest(S) == CHOOSE o \in S : \A p \in S : o <= p

(* What a wait for any takes: the free object of the lowest index, as WaitForMultipleObjects reports. *)
Chosen(c, w, t, objects, every) == IF every THEN objects ELSE {Lowest({o \in objects : Free(c, w, t, o)})}

(* The record of the objects after thread t takes `taken`: an event that resets on its own resets, a semaphore counts down, a mutex
   becomes t's. *)
Take(s, t, taken) ==
    [s EXCEPT
        !.count = [o \in Objects |-> IF o \in taken /\ (Kind[o] = "auto" \/ o \in Semaphores) THEN s.count[o] - 1 ELSE s.count[o]],
        !.owner = [o \in Objects |-> IF o \in taken /\ o \in Mutexes THEN t ELSE s.owner[o]],
        !.depth = [o \in Objects |-> IF o \in taken /\ o \in Mutexes THEN s.depth[o] + 1 ELSE s.depth[o]],
        !.abandoned = s.abandoned \ taken]

(* The state s after thread u's wait ends: it runs again, out of the line, and keeps nothing of the wait. How the wait ended (the
   object it took, an APC, the time-out, the thread's end) follows from the step that ends it; the tests of the implementation
   check the values Windows gives for each. *)
Ended(s, u) ==
    [s EXCEPT !.state[u] = "run", !.wants[u] = {}, !.all[u] = FALSE, !.alertable[u] = FALSE,
        !.line = SelectSeq(s.line, LAMBDA v : v # u)]

(* Hands the objects to the waits in the line that they satisfy, oldest first. A grant only takes, so one pass is enough. A suspended
   thread is passed over: it takes nothing until it runs again. *)
RECURSIVE Grant(_, _)
Grant(s, waiting) ==
    IF waiting = <<>> THEN s
    ELSE LET t == Head(waiting) IN
        IF ~s.suspended[t] /\ Satisfied(s.count, s.owner, t, s.wants[t], s.all[t])
        THEN LET taken == Chosen(s.count, s.owner, t, s.wants[t], s.all[t]) IN
            Grant(Ended(Take(s, t, taken), t), Tail(waiting))
        ELSE Grant(s, Tail(waiting))

Current == [count |-> count, owner |-> owner, depth |-> depth, abandoned |-> abandoned, state |-> state, wants |-> wants, all |-> all,
    alertable |-> alertable, apcs |-> apcs, suspended |-> suspended, line |-> line]

(* The next state is s. *)
Become(s) ==
    /\ count' = s.count /\ owner' = s.owner /\ depth' = s.depth /\ abandoned' = s.abandoned /\ state' = s.state
    /\ wants' = s.wants /\ all' = s.all /\ alertable' = s.alertable /\ apcs' = s.apcs /\ suspended' = s.suspended
    /\ line' = s.line

(* The next state is s after granting what it allows. *)
Settle(s) == Become(Grant(s, s.line))

Init ==
    /\ count = [o \in Objects |-> 0]
    /\ owner = [o \in Objects |-> None]
    /\ depth = [o \in Objects |-> 0]
    /\ abandoned = {}
    /\ state = [t \in Threads |-> "run"]
    /\ wants = [t \in Threads |-> {}]
    /\ all = [t \in Threads |-> FALSE]
    /\ alertable = [t \in Threads |-> FALSE]
    /\ apcs = [t \in Threads |-> 0]
    /\ suspended = [t \in Threads |-> FALSE]
    /\ line = <<>>

Running(t) == state[t] = "run" /\ ~suspended[t]

(* WaitForMultipleObjects (or for one object). An alertable wait with an APC queued runs it and ends at once; a wait the objects satisfy
   now takes them and ends; any other waits, last in line. *)
Wait(t, objects, every, alert) ==
    /\ Running(t)
    /\ IF alert /\ apcs[t] > 0
       THEN Become([Current EXCEPT !.apcs[t] = @ - 1])
       ELSE IF Satisfied(count, owner, t, objects, every)
       THEN LET taken == Chosen(count, owner, t, objects, every) IN
            Become(Take(Current, t, taken))
       ELSE Become([Current EXCEPT !.state[t] = "wait", !.wants[t] = objects, !.all[t] = every, !.alertable[t] = alert,
            !.line = Append(line, t)])

SetEvent(t, o) == Running(t) /\ o \in Events /\ Settle([Current EXCEPT !.count[o] = 1])
ResetEvent(t, o) == Running(t) /\ o \in Events /\ Become([Current EXCEPT !.count[o] = 0])

(* PulseEvent: the event is set for the waits there are now, and reset. *)
PulseEvent(t, o) ==
    /\ Running(t) /\ o \in Events
    /\ LET g == Grant([Current EXCEPT !.count[o] = 1], line) IN Become([g EXCEPT !.count[o] = 0])

ReleaseMutex(t, o) ==
    /\ Running(t) /\ o \in Mutexes /\ owner[o] = t
    /\ Settle([Current EXCEPT !.depth[o] = @ - 1, !.owner[o] = IF depth[o] = 1 THEN None ELSE t])

ReleaseSemaphore(t, o) == Running(t) /\ o \in Semaphores /\ count[o] < SemaphoreMax /\ Settle([Current EXCEPT !.count[o] = @ + 1])

(* An APC queued to a thread in an alertable wait ends the wait, unless the wait was satisfied first. *)
QueueApc(t, u) ==
    /\ Running(t) /\ state[u] # "dead" /\ apcs[u] = 0
    /\ IF state[u] = "wait" /\ alertable[u] /\ ~suspended[u]
       THEN Become(Ended(Current, u))
       ELSE Become([Current EXCEPT !.apcs[u] = 1])

TimeOut(u) == state[u] = "wait" /\ Become(Ended(Current, u))

Suspend(t, u) == Running(t) /\ t # u /\ state[u] # "dead" /\ ~suspended[u] /\ Become([Current EXCEPT !.suspended[u] = TRUE])

(* A thread that runs again takes what its wait may take now, and an APC queued meanwhile ends an alertable wait. *)
Resume(t, u) ==
    /\ Running(t) /\ suspended[u]
    /\ LET s == [Current EXCEPT !.suspended[u] = FALSE] IN
        IF state[u] = "wait" /\ alertable[u] /\ apcs[u] > 0
        THEN Become([Ended(s, u) EXCEPT !.apcs[u] = @ - 1])
        ELSE Settle(s)

(* A thread ends, by itself or terminated: it leaves the line, and the mutexes it holds are abandoned and go to the waits for them. *)
Terminate(t, u) ==
    /\ Running(t) /\ state[u] # "dead"
    /\ LET held == {o \in Mutexes : owner[o] = u}
           ended == Ended(Current, u) IN
        Settle([ended EXCEPT
            !.state[u] = "dead",
            !.owner = [o \in Objects |-> IF o \in held THEN None ELSE owner[o]],
            !.depth = [o \in Objects |-> IF o \in held THEN 0 ELSE depth[o]],
            !.abandoned = abandoned \cup held,
            !.suspended[u] = FALSE,
            !.apcs[u] = 0])

Next ==
    \/ \E t \in Threads, objects \in SUBSET Objects \ {{}}, every, alert \in BOOLEAN :
        (every => Cardinality(objects) > 1) /\ Wait(t, objects, every, alert)
    \/ \E t \in Threads, o \in Objects :
        SetEvent(t, o) \/ ResetEvent(t, o) \/ PulseEvent(t, o) \/ ReleaseMutex(t, o) \/ ReleaseSemaphore(t, o)
    \/ \E t, u \in Threads : QueueApc(t, u) \/ Suspend(t, u) \/ Resume(t, u) \/ Terminate(t, u)
    \/ \E u \in Threads : TimeOut(u)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ count \in [Objects -> 0..SemaphoreMax]
    /\ owner \in [Objects -> Threads \cup {None}]
    /\ state \in [Threads -> {"run", "wait", "dead"}]
    /\ apcs \in [Threads -> 0..1]

(* No lost wake: a thread that waits, and is not suspended, has a wait the objects do not satisfy and no APC that would end it. *)
NoLostWake ==
    \A t \in Threads : state[t] = "wait" /\ ~suspended[t] =>
        /\ ~Satisfied(count, owner, t, wants[t], all[t])
        /\ ~(alertable[t] /\ apcs[t] > 0)

(* The line holds exactly the waiting threads, each once. *)
LineExact ==
    /\ {line[i] : i \in 1..Len(line)} = {t \in Threads : state[t] = "wait"}
    /\ Len(line) = Cardinality({t \in Threads : state[t] = "wait"})

(* A mutex is held by one live thread, as many times as it was taken, or by nobody. *)
MutexSafe ==
    \A o \in Mutexes :
        /\ (owner[o] = None) <=> (depth[o] = 0)
        /\ owner[o] # None => state[owner[o]] # "dead"
        /\ o \in abandoned => owner[o] = None

EventsBinary == \A o \in Events : count[o] \in 0..1

(* Keeps the model finite: a mutex taken again more than MaxRecursion times is not explored further. *)
Bounded == \A o \in Mutexes : depth[o] <= MaxRecursion

=============================================================================
