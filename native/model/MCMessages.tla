---------------------------- MODULE MCMessages ----------------------------
(* The model TLC checks (MCMessages.cfg): three threads, two messages posted and three sent in all, so two threads can send to each
   other at once while a third ends. The threads stay distinct: a check of what must happen in the end is not sound under symmetry. *)
EXTENDS Messages, TLC
=============================================================================
