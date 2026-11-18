---------------------------- MODULE Completion ----------------------------
(* An I/O completion port as the host keeps it (native/nt/completion_port.h, winsock_api.cpp): one queue of packets, posted at its
   end and taken from its head by whichever waiting thread looks first, waits that may run out of time, and handles to the port that
   can be duplicated and closed. Closing the last handle abandons the port: the threads that wait on it give up with
   ERROR_ABANDONED_WAIT_0 and the packets still queued are gone.

   What is checked: each packet posted is taken once, still queued or dropped by the close, and never two of these; packets are taken in
   the order they were posted; no thread waits for good on a port whose handles are all closed, nor while the port holds a packet. *)
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
          state        \* each thread: "idle", or "wait" on the port

vars == <<queue, posted, taken, dropped, handles, abandoned, state>>

Init ==
    /\ queue = <<>>
    /\ posted = 0
    /\ taken = <<>>
    /\ dropped = {}
    /\ handles = 1
    /\ abandoned = FALSE
    /\ state = [t \in Threads |-> "idle"]

Open == handles > 0

\* PostQueuedCompletionStatus through an open handle.
Post(t) ==
    /\ state[t] = "idle" /\ Open /\ posted < Packets
    /\ posted' = posted + 1
    /\ queue' = Append(queue, posted + 1)
    /\ UNCHANGED <<taken, dropped, handles, abandoned, state>>

\* GetQueuedCompletionStatus: a packet at the head is taken at once; otherwise the thread waits (wait_packet).
Get(t) ==
    /\ state[t] = "idle" /\ Open
    /\ IF queue # <<>>
          THEN /\ taken' = Append(taken, Head(queue))
               /\ queue' = Tail(queue)
               /\ UNCHANGED state
          ELSE /\ state' = [state EXCEPT ![t] = "wait"]
               /\ UNCHANGED <<queue, taken>>
    /\ UNCHANGED <<posted, dropped, handles, abandoned>>

\* A waiting thread looks again and finds a packet: the port was not abandoned, since the close empties the queue.
Deliver(t) ==
    /\ state[t] = "wait" /\ queue # <<>> /\ ~abandoned
    /\ taken' = Append(taken, Head(queue))
    /\ queue' = Tail(queue)
    /\ state' = [state EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<posted, dropped, handles, abandoned>>

\* A wait with a time-out runs out with no packet.
TimeOut(t) ==
    /\ state[t] = "wait" /\ queue = <<>>
    /\ state' = [state EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<queue, posted, taken, dropped, handles, abandoned>>

\* A waiting thread looks again and finds the port abandoned: ERROR_ABANDONED_WAIT_0.
GiveUp(t) ==
    /\ state[t] = "wait" /\ abandoned
    /\ state' = [state EXCEPT ![t] = "idle"]
    /\ UNCHANGED <<queue, posted, taken, dropped, handles, abandoned>>

\* DuplicateHandle of an open handle.
Duplicate(t) ==
    /\ state[t] = "idle" /\ Open /\ handles < MaxHandles
    /\ handles' = handles + 1
    /\ UNCHANGED <<queue, posted, taken, dropped, abandoned, state>>

\* CloseHandle; the last one abandons the port and drops what is queued (CompletionPort::last_handle_closed).
Close(t) ==
    /\ state[t] = "idle" /\ Open
    /\ handles' = handles - 1
    /\ IF handles = 1
          THEN /\ abandoned' = TRUE
               /\ dropped' = dropped \cup {queue[i] : i \in 1..Len(queue)}
               /\ queue' = <<>>
          ELSE UNCHANGED <<abandoned, dropped, queue>>
    /\ UNCHANGED <<posted, taken, state>>

Next == \E t \in Threads :
    Post(t) \/ Get(t) \/ Deliver(t) \/ TimeOut(t) \/ GiveUp(t) \/ Duplicate(t) \/ Close(t)

\* A thread that waits looks again whenever the port changes: one with a packet or an abandoned port to find does so in the end.
Spec == Init /\ [][Next]_vars /\ \A t \in Threads : WF_vars(Deliver(t)) /\ WF_vars(GiveUp(t))

Range(s) == {s[i] : i \in 1..Len(s)}

TypeOK ==
    /\ posted \in 0..Packets
    /\ handles \in 0..MaxHandles
    /\ abandoned \in BOOLEAN
    /\ state \in [Threads -> {"idle", "wait"}]
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

\* No thread waits for good on a port whose handles are all closed, nor stays waiting for good while the port holds a packet.
NoStuckWait ==
    /\ \A t \in Threads : (state[t] = "wait" /\ handles = 0) ~> (state[t] = "idle")
    /\ \A t \in Threads : []<>(state[t] = "idle" \/ queue = <<>>)
=============================================================================
