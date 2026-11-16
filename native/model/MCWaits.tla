------------------------------ MODULE MCWaits ------------------------------
(* The models TLC checks (MCWaits.cfg, MCWaitsThree.cfg): two threads and an object of each kind, and three threads with an event
   that resets on its own, a mutex and a semaphore. The threads are interchangeable, so TLC looks at one of each set of states that
   differ only by which thread is which. *)
EXTENDS Waits, TLC

Kinds == <<"auto", "manual", "mutex", "semaphore">>
KindsThree == <<"auto", "mutex", "semaphore">>
Interchangeable == Permutations(Threads)
=============================================================================
