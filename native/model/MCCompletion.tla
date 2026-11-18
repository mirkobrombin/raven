---------------------------- MODULE MCCompletion ----------------------------
(* The model TLC checks (MCCompletion.cfg): three threads, four packets and up to two handles to the port. The threads are
   interchangeable, so TLC looks at one of each set of states that differ only by which thread is which. *)
EXTENDS Completion, TLC

Interchangeable == Permutations(Threads)
=============================================================================
