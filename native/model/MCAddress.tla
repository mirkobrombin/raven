------------------------------ MODULE MCAddress ------------------------------
(* The models TLC checks: MCAddressSections.cfg, a thread on a critical section beside a producer and a consumer on a condition
   variable over the same section; MCAddressSrw.cfg, an exclusive and two shared holders of an SRW lock. *)
EXTENDS Address, TLC

=============================================================================
