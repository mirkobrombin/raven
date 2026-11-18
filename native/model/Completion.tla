---------------------------- MODULE Completion ----------------------------
(* An I/O completion port as the host keeps it (native/nt/completion_port.h, winsock_api.cpp): one queue of packets, posted at its
   end and taken from its head by whichever waiting thread looks first, waits that may run out of time, and handles to the port that
   can be duplicated and closed. Closing the last handle abandons the port: the threads that wait on it give up with
   ERROR_ABANDONED_WAIT_0 and the packets still queued are gone. A thread that waits alertably (GetQueuedCompletionStatusEx) runs the
   APCs queued to it when there is no packet to take, and its wait ends with WAIT_IO_COMPLETION.

   What is checked: each packet posted is taken once, still queued or dropped by the close, and never two of these; packets are taken in
   the order they were posted; no thread waits for good on a port whose handles are all closed, nor while the port holds a packet, nor
   alertably with an APC queued to it. *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Threads,     \* threads that post to the port and wait on it
          Packets,     \* how many packets may be posted in all
          MaxHandles   \* how many handles to the port may be open at once

VARIABLES queue,       \* the packets posted and not taken, oldest first; a packet is the number of its post
          posted,      \* how many packets were posted
          taken,       \* the packets taken, in the order they were taken
          dropped,     \* the packets the close of the last handle threw away
          handles,     \* the handles to the port that are open
          abandoned,   \* whether the last handle was closed
          state,       \* each thread: "idle", or "wait" on the port
          alertable,   \* whether each thread's wait is alertable
          apcs         \* whether each thread has an APC queued to it

vars == <<queue, posted, taken, dropped, handles, abandoned, state, alertable, apcs>>

Init ==
    /\ queue = <<>>
    /\ posted = 0
    /\ taken = <<>>
    /\ dropped = {}
    /\ handles = 1
    /\ abandoned = FALSE
    /\ state = [t \in Threads |-> "idle"]
    /\ alertable = [t \in Threads |-> FALSE]
    /\ apcs = [t \in Threads |-> FALSE]

Open == handles > 0

\* PostQueuedCompletionStatus through an open handle.
Post(t) ==
    /\ state[t] = "idle" /\ Open /\ posted < Packets
    /\ posted' = posted + 1
    /\ queue' = Append(queue, posted + 1)
    /\ UNCHANGED <<taken, dropped, handles, abandoned, state, alertable, apcs>>

\* GetQueuedCompletionStatus(Ex): a packet at the head is taken at once; an alertable wait with nothing queued runs the thread's APCs
\* and ends; otherwise the thread waits (wait_packet).
Get(t, alert) ==
    /\ state[t] = "idle" /\ Open
    /\ alertable' = [alertable EXCEPT ![t] = alert]
    /\ IF queue # <<>>
          THEN /\ taken' = Append(taken, Head(queue))
               /\ queue' = Tail(queue)
               /\ UNCHANGED <<state, apcs>>
          ELSE IF alert /\ apcs[t]
          THEN /\ apcs' = [apcs EXCEPT ![t] = FALSE]
               /\ UNCHANGED <<state, queue, taken>>
          ELSE /\ state' = [state EXCEPT ![t] = "wait"]
               /\ UNCHANGED <<queue, taken, apcs>>
    /\ UNCHANGED <<posted, dropped, handles, abandoned>>

\* QueueUserAPC to another thread.
QueueApc(t, u) ==
    /\ state[t] = "idle" /\ t # u /\ ~apcs[u]
    /\ apcs' = [apcs EXCEPT ![u] = TRUE]
    /\ UNCHANGED <<queue, posted, taken, dropped, handles, abandoned, state, alertable>>

\* An alertable waiter looks again and finds an APC and no packet: it runs the APC and its wait ends with WAIT_IO_COMPLETION.
Alerted(t) ==
    /\ state[t] = "wait" /\ alertable[t] /\ apcs[t] /\ queue = <<>> /\ ~abandoned
    /\ apcs' = [apcs EXCEPT ![t] = FALSE]
    /\ state' = [state EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<queue, posted, taken, dropped, handles, abandoned, alertable>>

\* A waiting thread looks again and finds a packet: the port was not abandoned, since the close empties the queue.
Deliver(t) ==
    /\ state[t] = "wait" /\ queue # <<>> /\ ~abandoned
    /\ taken' = Append(taken, Head(queue))
    /\ queue' = Tail(queue)
    /\ state' = [state EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<posted, dropped, handles, abandoned, alertable, apcs>>

\* A wait with a time-out runs out with no packet.
TimeOut(t) ==
    /\ state[t] = "wait" /\ queue = <<>>
    /\ state' = [state EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<queue, posted, taken, dropped, handles, abandoned, alertable, apcs>>

\* A waiting thread looks again and finds the port abandoned: ERROR_ABANDONED_WAIT_0.
GiveUp(t) ==
    /\ state[t] = "wait" /\ abandoned
    /\ state' = [state EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<queue, posted, taken, dropped, handles, abandoned, alertable, apcs>>

\* DuplicateHandle of an open handle.
Duplicate(t) ==
    /\ state[t] = "idle" /\ Open /\ handles < MaxHandles
    /\ handles' = handles + 1
    /\ UNCHANGED <<queue, posted, taken, dropped, abandoned, state, alertable, apcs>>

\* CloseHandle; the last one abandons the port and drops what is queued (CompletionPort::last_handle_closed).
Close(t) ==
    /\ state[t] = "idle" /\ Open
    /\ handles' = handles - 1
    /\ IF handles = 1
          THEN /\ abandoned' = TRUE
               /\ dropped' = dropped \cup {queue[i] : i \in 1..Len(queue)}
               /\ queue' = <<>>
          ELSE UNCHANGED <<abandoned, dropped, queue>>
    /\ UNCHANGED <<posted, taken, state, alertable, apcs>>

Next == \E t \in Threads :
    \/ Post(t) \/ Deliver(t) \/ TimeOut(t) \/ GiveUp(t) \/ Alerted(t) \/ Duplicate(t) \/ Close(t)
    \/ \E alert \in BOOLEAN : Get(t, alert)
    \/ \E u \in Threads : QueueApc(t, u)

\* A thread that waits looks again whenever the port changes: one with a packet or an abandoned port to find does so in the end.
Spec == Init /\ [][Next]_vars /\ \A t \in Threads : WF_vars(Deliver(t)) /\ WF_vars(GiveUp(t)) /\ WF_vars(Alerted(t))

Range(s) == {s[i] : i \in 1..Len(s)}

TypeOK ==
    /\ posted \in 0..Packets
    /\ handles \in 0..MaxHandles
    /\ abandoned \in BOOLEAN
    /\ state \in [Threads -> {"idle", "wait"}]
    /\ alertable \in [Threads -> BOOLEAN]
    /\ apcs \in [Threads -> BOOLEAN]
    /\ Range(queue) \cup Range(taken) \cup dropped \subseteq 1..posted

\* Each packet posted is in exactly one place: queued, taken once, or dropped.
Accounted ==
    /\ Range(queue) \cup Range(taken) \cup dropped = 1..posted
    /\ Range(queue) \cap Range(taken) = {}
    /\ Range(queue) \cap dropped = {}
    /\ Range(taken) \cap dropped = {}
    /\ Len(taken) = Cardinality(Range(taken))
    /\ Len(queue) = Cardinality(Range(queue))

\* Packets are taken, and stay queued, in the order they were posted.
InOrder ==
    /\ \A i \in 1..Len(taken) - 1 : taken[i] < taken[i + 1]
    /\ \A i \in 1..Len(queue) - 1 : queue[i] < queue[i + 1]
    /\ \A i \in 1..Len(taken), j \in 1..Len(queue) : taken[i] < queue[j]

\* An abandoned port holds nothing and has no handle.
AbandonedEmpty == abandoned => queue = <<>> /\ handles = 0

\* No thread waits for good on a port whose handles are all closed, nor stays waiting for good while the port holds a packet, nor waits
\* alertably for good with an APC queued to it.
NoStuckWait ==
    /\ \A t \in Threads : (state[t] = "wait" /\ handles = 0) ~> (state[t] = "idle")
    /\ \A t \in Threads : []<>(state[t] = "idle" \/ queue = <<>>)
    /\ \A t \in Threads : (state[t] = "wait" /\ alertable[t] /\ apcs[t]) ~> (state[t] = "idle")
=============================================================================
