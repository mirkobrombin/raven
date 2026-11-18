----------------------------- MODULE Messages -----------------------------
(* The message queues of a process's threads as the host keeps them (native/desktop/windows.cpp): each thread has a queue of posted
   messages, and a message sent to a window of another thread waits in the list of sent messages until that thread takes it. The sender
   waits for the answer and meanwhile answers the messages sent to its own windows (SendMessage); a thread takes the messages sent to it
   whenever it looks at its queue (GetMessage) or waits for an answer of its own. A thread that ends answers the messages sent to it that
   it never took with 0 (NativeWindows::thread_ended), and its posted messages are gone. The threads are message loops: one that runs
   comes back to its queue, or ends, in the end.

   What is checked: no sender waits for good, two threads that send to each other at once included; a sent message is answered once;
   no message stays sent to a thread that ended; and a thread that waits for a posted message gets one that is there. *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS Threads,    \* the threads, each with its queue and a window
          MaxPosts,   \* how many messages may be posted in all
          MaxSends    \* how many messages may be sent in all

VARIABLES state,      \* each thread: "run", "get" (waits for a posted message), "send" (waits for an answer) or "dead"
          posted,     \* each thread's queue: the numbers of the messages posted to it, oldest first
          sent,       \* the sent messages: [from, to, answered, taken]
          waiting,    \* each thread that sends: the number of the message it waits on
          posts,      \* how many messages were posted
          sends       \* how many messages were sent

vars == <<state, posted, sent, waiting, posts, sends>>

None == 0

Init ==
    /\ state = [t \in Threads |-> "run"]
    /\ posted = [t \in Threads |-> <<>>]
    /\ sent = <<>>
    /\ waiting = [t \in Threads |-> None]
    /\ posts = 0
    /\ sends = 0

\* PostMessage to a window of a thread that is alive.
Post(t, u) ==
    /\ state[t] = "run" /\ state[u] # "dead" /\ posts < MaxPosts
    /\ posted' = [posted EXCEPT ![u] = Append(@, posts + 1)]
    /\ posts' = posts + 1
    /\ UNCHANGED <<state, sent, waiting, sends>>

\* SendMessage to a window of another thread that is alive: the message joins the sent list and the sender waits for its answer.
Send(t, u) ==
    /\ state[t] = "run" /\ t # u /\ state[u] # "dead" /\ sends < MaxSends
    /\ sent' = Append(sent, [from |-> t, to |-> u, answered |-> FALSE, taken |-> FALSE])
    /\ waiting' = [waiting EXCEPT ![t] = Len(sent) + 1]
    /\ state' = [state EXCEPT ![t] = "send"]
    /\ sends' = sends + 1
    /\ UNCHANGED <<posted, posts>>

\* GetMessage: a posted message is taken at once, otherwise the thread waits.
Get(t) ==
    /\ state[t] = "run"
    /\ IF posted[t] # <<>>
          THEN /\ posted' = [posted EXCEPT ![t] = Tail(@)]
               /\ UNCHANGED state
          ELSE /\ state' = [state EXCEPT ![t] = "get"]
               /\ UNCHANGED posted
    /\ UNCHANGED <<sent, waiting, posts, sends>>

\* A thread that looks at its queue, or waits for an answer, runs a message sent to it (serve_sent) and answers it.
Serve(t) ==
    /\ state[t] \in {"get", "send"}
    /\ \E i \in 1..Len(sent) :
          /\ sent[i].to = t /\ ~sent[i].taken
          /\ sent' = [sent EXCEPT ![i].taken = TRUE, ![i].answered = TRUE]
    /\ UNCHANGED <<state, posted, waiting, posts, sends>>

\* A waiting sender finds its answer and goes on.
Answered(t) ==
    /\ state[t] = "send" /\ sent[waiting[t]].answered
    /\ state' = [state EXCEPT ![t] = "run"]
    /\ waiting' = [waiting EXCEPT ![t] = None]
    /\ UNCHANGED <<posted, sent, posts, sends>>

\* A thread waiting in GetMessage finds a posted message.
Woken(t) ==
    /\ state[t] = "get" /\ posted[t] # <<>>
    /\ posted' = [posted EXCEPT ![t] = Tail(@)]
    /\ state' = [state EXCEPT ![t] = "run"]
    /\ UNCHANGED <<sent, waiting, posts, sends>>

\* A thread ends: what was sent to it and not taken is answered with 0, and its queue is gone.
End(t) ==
    /\ state[t] = "run"
    /\ state' = [state EXCEPT ![t] = "dead"]
    /\ posted' = [posted EXCEPT ![t] = <<>>]
    /\ sent' = [i \in 1..Len(sent) |-> IF sent[i].to = t /\ ~sent[i].taken THEN [sent[i] EXCEPT !.answered = TRUE] ELSE sent[i]]
    /\ UNCHANGED <<waiting, posts, sends>>

Next == \E t \in Threads :
    \/ Get(t) \/ Serve(t) \/ Answered(t) \/ Woken(t) \/ End(t)
    \/ \E u \in Threads : Post(t, u) \/ Send(t, u)

\* Each thread is a message loop: running, it comes back to its queue or ends; looking at its queue or waiting for an answer, it takes
\* what was sent to it; and a waiter goes on once it has what it waits for.
Spec == Init /\ [][Next]_vars /\ \A t \in Threads :
    WF_vars(Get(t) \/ End(t)) /\ WF_vars(Serve(t)) /\ WF_vars(Answered(t)) /\ WF_vars(Woken(t))

TypeOK ==
    /\ state \in [Threads -> {"run", "get", "send", "dead"}]
    /\ posts \in 0..MaxPosts
    /\ sends \in 0..MaxSends
    /\ Len(sent) = sends
    /\ \A t \in Threads : waiting[t] \in 0..sends

\* A sender waits on exactly the message it sent, and only while it waits.
WaitsOnOwn == \A t \in Threads :
    /\ (state[t] = "send") <=> (waiting[t] # None)
    /\ waiting[t] # None => sent[waiting[t]].from = t

\* A message is taken only once it is answered, and nothing is left sent to a thread that ended.
NoneForDead == \A i \in 1..Len(sent) :
    /\ (sent[i].taken => sent[i].answered)
    /\ (state[sent[i].to] = "dead" => sent[i].answered)

\* No sender waits for good, and no thread waits for good in GetMessage on a message posted to it.
NoStuckWait == \A t \in Threads :
    /\ (state[t] = "send") ~> (state[t] # "send")
    /\ []<>(state[t] # "get" \/ posted[t] = <<>>)
=============================================================================
