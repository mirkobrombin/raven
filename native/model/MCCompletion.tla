---------------------------- MODULE MCCompletion ----------------------------
(* The model TLC checks (MCCompletion.cfg): three threads, four packets and up to two handles to the port. The threads stay distinct:
   a check of what must happen in the end is not sound under symmetry. *)
EXTENDS Completion, TLC
=============================================================================
