------------------------------ MODULE Address ------------------------------
(* WaitOnAddress as the host keeps it, and the locks the guest RTL builds on it, step for step as native/guest/raven-rtl/kernel.c
   writes them: critical sections (LockCount -1 free, 0 held, 1 held with a waiter), SRW locks (1 held exclusively, shared holders in
   steps of 2, a bit for a waiter), and condition variables, whose sleepers the host keeps in a list while the variable's word counts
   them so a wake with nobody to wake stays in the guest (native/nt/sync_api.cpp).

   A wait compares the word and joins the address's list in one step under the list's lock; a waker changes the word first and then
   marks waiters under the same lock, from its turn or beside it. What is checked: one thread in a critical section, an exclusive SRW
   holder alone, and every thread ending its rounds, so no wake is lost and no thread sleeps for good. *)
EXTENDS Integers, FiniteSets, Sequences

CONSTANTS Sections,   \* threads that take a critical section and leave it
          Exclusive,  \* threads that take the SRW lock exclusively
          Shared,     \* threads that take it shared
          Producers,  \* threads that add an item under the section and wake a sleeper on the condition variable
          Consumers,  \* threads that sleep on the condition variable until there is an item
          Rounds      \* how many times each thread does its work

Threads == Sections \cup Exclusive \cup Shared \cup Producers \cup Consumers
\* The SRW waiter bit; holders stay below it.
W == 64

(* --algorithm Address
variables
    mem = [a \in {"cs", "srw"} |-> IF a = "cs" THEN -1 ELSE 0],
    \* The threads in each list: an address's waiters, and the condition variable's sleepers.
    waiting = [a \in {"cs", "srw", "cv"} |-> {}],
    woken = [t \in Threads |-> FALSE],
    inSection = {}, exclusive = {}, shared = {},
    items = 0,
    \* The condition variable's word: how many sleep on it.
    sleepers = 0;

macro WakeOne(address) begin
    if \E t \in waiting[address] : ~woken[t] then
        with t \in {u \in waiting[address] : ~woken[u]} do
            woken[t] := TRUE;
        end with;
    end if;
end macro;

macro WakeAll(address) begin
    woken := [t \in Threads |-> woken[t] \/ t \in waiting[address]];
end macro;

procedure WaitOn(address, expected)
begin
    compare:
        if mem[address] # expected then
            return;
        else
            waiting[address] := waiting[address] \cup {self};
            woken[self] := FALSE;
        end if;
    sleep:
        await woken[self];
    unlist:
        waiting[address] := waiting[address] \ {self};
        return;
end procedure;

procedure Enter()
variable state = 0;
begin
    try:
        if mem["cs"] = -1 then
            mem["cs"] := 0;
            goto entered;
        else
            state := mem["cs"];
        end if;
    mark:
        if state # 1 then
            state := mem["cs"] || mem["cs"] := 1;
        end if;
    contend:
        while state # -1 do
            call WaitOn("cs", 1);
    retake:
            state := mem["cs"] || mem["cs"] := 1;
        end while;
    entered:
        inSection := inSection \cup {self};
        return;
end procedure;

procedure Leave()
variable left = 0;
begin
    release:
        inSection := inSection \ {self};
        left := mem["cs"] || mem["cs"] := -1;
    wake_section:
        if left = 1 then
            WakeOne("cs");
        end if;
        return;
end procedure;

procedure SrwWait(value)
begin
    flag:
        if value < W then
            if mem["srw"] = value then
                mem["srw"] := value + W;
                value := value + W;
            else
                return;
            end if;
        end if;
    srw_sleep:
        call WaitOn("srw", value);
        return;
end procedure;

procedure AcquireExclusive()
variable word = 0;
begin
    look_exclusive:
        word := mem["srw"];
        if word % W # 0 then
            call SrwWait(word);
            goto look_exclusive;
        end if;
    take_exclusive:
        if mem["srw"] = word then
            mem["srw"] := word + 1;
            exclusive := exclusive \cup {self};
            return;
        else
            goto look_exclusive;
        end if;
end procedure;

procedure ReleaseExclusive()
variable held = 0;
begin
    clear_exclusive:
        exclusive := exclusive \ {self};
        held := mem["srw"] || mem["srw"] := 0;
    wake_exclusive:
        if held >= W then
            WakeAll("srw");
        end if;
        return;
end procedure;

procedure AcquireShared()
variable read = 0;
begin
    look_shared:
        read := mem["srw"];
        if read % 2 # 0 then
            call SrwWait(read);
            goto look_shared;
        end if;
    take_shared:
        if mem["srw"] = read then
            mem["srw"] := read + 2;
            shared := shared \cup {self};
            return;
        else
            goto look_shared;
        end if;
end procedure;

procedure ReleaseShared()
variable last = 0;
begin
    look_release:
        last := mem["srw"];
    drop_shared:
        if mem["srw"] = last then
            mem["srw"] := IF last % W = 2 THEN 0 ELSE last - 2;
            shared := shared \ {self};
            if last % W = 2 /\ last >= W then
                WakeAll("srw");
            end if;
            return;
        else
            goto look_release;
        end if;
end procedure;

\* SleepConditionVariableCS through the guest RTL: the host lists the sleeper and counts it in the word, leaves the section through the
\* guest, sleeps, takes itself off the list and enters the section again.
procedure Sleep()
begin
    list:
        waiting["cv"] := waiting["cv"] \cup {self};
        woken[self] := FALSE;
        sleepers := Cardinality(waiting["cv"] \cup {self});
        call Leave();
    doze:
        await woken[self];
    delist:
        waiting["cv"] := waiting["cv"] \ {self};
        sleepers := Cardinality(waiting["cv"] \ {self});
        call Enter();
        return;
end procedure;

fair process section \in Sections
variable sections_done = 0;
begin
    section_round:
        while sections_done < Rounds do
            call Enter();
    section_leave:
            call Leave();
    section_next:
            sections_done := sections_done + 1;
        end while;
end process;

fair process writer \in Exclusive
variable writes = 0;
begin
    writer_round:
        while writes < Rounds do
            call AcquireExclusive();
    writer_release:
            call ReleaseExclusive();
    writer_next:
            writes := writes + 1;
        end while;
end process;

fair process reader \in Shared
variable reads = 0;
begin
    reader_round:
        while reads < Rounds do
            call AcquireShared();
    reader_release:
            call ReleaseShared();
    reader_next:
            reads := reads + 1;
        end while;
end process;

fair process producer \in Producers
variable made = 0;
begin
    produce:
        while made < Rounds do
            call Enter();
    add:
            items := items + 1;
            made := made + 1;
            call Leave();
    look_sleepers:
            \* The guest asks the host only when the word counts a sleeper.
            if sleepers # 0 then
    wake_sleeper:
                WakeOne("cv");
            end if;
        end while;
end process;

fair process consumer \in Consumers
variable eaten = 0;
begin
    consume:
        while eaten < Rounds do
            call Enter();
    check:
            while items = 0 do
                call Sleep();
            end while;
    take:
            items := items - 1;
            eaten := eaten + 1;
            call Leave();
        end while;
end process;

end algorithm; *)
\* BEGIN TRANSLATION
CONSTANT defaultInitValue
VARIABLES pc, mem, waiting, woken, inSection, exclusive, shared, items, 
          sleepers, stack, address, expected, state, left, value, word, held, 
          read, last, sections_done, writes, reads, made, eaten

vars == << pc, mem, waiting, woken, inSection, exclusive, shared, items, 
           sleepers, stack, address, expected, state, left, value, word, held, 
           read, last, sections_done, writes, reads, made, eaten >>

ProcSet == (Sections) \cup (Exclusive) \cup (Shared) \cup (Producers) \cup (Consumers)

Init == (* Global variables *)
        /\ mem = [a \in {"cs", "srw"} |-> IF a = "cs" THEN -1 ELSE 0]
        /\ waiting = [a \in {"cs", "srw", "cv"} |-> {}]
        /\ woken = [t \in Threads |-> FALSE]
        /\ inSection = {}
        /\ exclusive = {}
        /\ shared = {}
        /\ items = 0
        /\ sleepers = 0
        (* Procedure WaitOn *)
        /\ address = [ self \in ProcSet |-> defaultInitValue]
        /\ expected = [ self \in ProcSet |-> defaultInitValue]
        (* Procedure Enter *)
        /\ state = [ self \in ProcSet |-> 0]
        (* Procedure Leave *)
        /\ left = [ self \in ProcSet |-> 0]
        (* Procedure SrwWait *)
        /\ value = [ self \in ProcSet |-> defaultInitValue]
        (* Procedure AcquireExclusive *)
        /\ word = [ self \in ProcSet |-> 0]
        (* Procedure ReleaseExclusive *)
        /\ held = [ self \in ProcSet |-> 0]
        (* Procedure AcquireShared *)
        /\ read = [ self \in ProcSet |-> 0]
        (* Procedure ReleaseShared *)
        /\ last = [ self \in ProcSet |-> 0]
        (* Process section *)
        /\ sections_done = [self \in Sections |-> 0]
        (* Process writer *)
        /\ writes = [self \in Exclusive |-> 0]
        (* Process reader *)
        /\ reads = [self \in Shared |-> 0]
        (* Process producer *)
        /\ made = [self \in Producers |-> 0]
        (* Process consumer *)
        /\ eaten = [self \in Consumers |-> 0]
        /\ stack = [self \in ProcSet |-> << >>]
        /\ pc = [self \in ProcSet |-> CASE self \in Sections -> "section_round"
                                        [] self \in Exclusive -> "writer_round"
                                        [] self \in Shared -> "reader_round"
                                        [] self \in Producers -> "produce"
                                        [] self \in Consumers -> "consume"]

compare(self) == /\ pc[self] = "compare"
                 /\ IF mem[address[self]] # expected[self]
                       THEN /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                            /\ address' = [address EXCEPT ![self] = Head(stack[self]).address]
                            /\ expected' = [expected EXCEPT ![self] = Head(stack[self]).expected]
                            /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                            /\ UNCHANGED << waiting, woken >>
                       ELSE /\ waiting' = [waiting EXCEPT ![address[self]] = waiting[address[self]] \cup {self}]
                            /\ woken' = [woken EXCEPT ![self] = FALSE]
                            /\ pc' = [pc EXCEPT ![self] = "sleep"]
                            /\ UNCHANGED << stack, address, expected >>
                 /\ UNCHANGED << mem, inSection, exclusive, shared, items, 
                                 sleepers, state, left, value, word, held, 
                                 read, last, sections_done, writes, reads, 
                                 made, eaten >>

sleep(self) == /\ pc[self] = "sleep"
               /\ woken[self]
               /\ pc' = [pc EXCEPT ![self] = "unlist"]
               /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                               shared, items, sleepers, stack, address, 
                               expected, state, left, value, word, held, read, 
                               last, sections_done, writes, reads, made, eaten >>

unlist(self) == /\ pc[self] = "unlist"
                /\ waiting' = [waiting EXCEPT ![address[self]] = waiting[address[self]] \ {self}]
                /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                /\ address' = [address EXCEPT ![self] = Head(stack[self]).address]
                /\ expected' = [expected EXCEPT ![self] = Head(stack[self]).expected]
                /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                /\ UNCHANGED << mem, woken, inSection, exclusive, shared, 
                                items, sleepers, state, left, value, word, 
                                held, read, last, sections_done, writes, reads, 
                                made, eaten >>

WaitOn(self) == compare(self) \/ sleep(self) \/ unlist(self)

try(self) == /\ pc[self] = "try"
             /\ IF mem["cs"] = -1
                   THEN /\ mem' = [mem EXCEPT !["cs"] = 0]
                        /\ pc' = [pc EXCEPT ![self] = "entered"]
                        /\ state' = state
                   ELSE /\ state' = [state EXCEPT ![self] = mem["cs"]]
                        /\ pc' = [pc EXCEPT ![self] = "mark"]
                        /\ mem' = mem
             /\ UNCHANGED << waiting, woken, inSection, exclusive, shared, 
                             items, sleepers, stack, address, expected, left, 
                             value, word, held, read, last, sections_done, 
                             writes, reads, made, eaten >>

mark(self) == /\ pc[self] = "mark"
              /\ IF state[self] # 1
                    THEN /\ /\ mem' = [mem EXCEPT !["cs"] = 1]
                            /\ state' = [state EXCEPT ![self] = mem["cs"]]
                    ELSE /\ TRUE
                         /\ UNCHANGED << mem, state >>
              /\ pc' = [pc EXCEPT ![self] = "contend"]
              /\ UNCHANGED << waiting, woken, inSection, exclusive, shared, 
                              items, sleepers, stack, address, expected, left, 
                              value, word, held, read, last, sections_done, 
                              writes, reads, made, eaten >>

contend(self) == /\ pc[self] = "contend"
                 /\ IF state[self] # -1
                       THEN /\ /\ address' = [address EXCEPT ![self] = "cs"]
                               /\ expected' = [expected EXCEPT ![self] = 1]
                               /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "WaitOn",
                                                                        pc        |->  "retake",
                                                                        address   |->  address[self],
                                                                        expected  |->  expected[self] ] >>
                                                                    \o stack[self]]
                            /\ pc' = [pc EXCEPT ![self] = "compare"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "entered"]
                            /\ UNCHANGED << stack, address, expected >>
                 /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                                 shared, items, sleepers, state, left, value, 
                                 word, held, read, last, sections_done, writes, 
                                 reads, made, eaten >>

retake(self) == /\ pc[self] = "retake"
                /\ /\ mem' = [mem EXCEPT !["cs"] = 1]
                   /\ state' = [state EXCEPT ![self] = mem["cs"]]
                /\ pc' = [pc EXCEPT ![self] = "contend"]
                /\ UNCHANGED << waiting, woken, inSection, exclusive, shared, 
                                items, sleepers, stack, address, expected, 
                                left, value, word, held, read, last, 
                                sections_done, writes, reads, made, eaten >>

entered(self) == /\ pc[self] = "entered"
                 /\ inSection' = (inSection \cup {self})
                 /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                 /\ state' = [state EXCEPT ![self] = Head(stack[self]).state]
                 /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                 /\ UNCHANGED << mem, waiting, woken, exclusive, shared, items, 
                                 sleepers, address, expected, left, value, 
                                 word, held, read, last, sections_done, writes, 
                                 reads, made, eaten >>

Enter(self) == try(self) \/ mark(self) \/ contend(self) \/ retake(self)
                  \/ entered(self)

release(self) == /\ pc[self] = "release"
                 /\ inSection' = inSection \ {self}
                 /\ /\ left' = [left EXCEPT ![self] = mem["cs"]]
                    /\ mem' = [mem EXCEPT !["cs"] = -1]
                 /\ pc' = [pc EXCEPT ![self] = "wake_section"]
                 /\ UNCHANGED << waiting, woken, exclusive, shared, items, 
                                 sleepers, stack, address, expected, state, 
                                 value, word, held, read, last, sections_done, 
                                 writes, reads, made, eaten >>

wake_section(self) == /\ pc[self] = "wake_section"
                      /\ IF left[self] = 1
                            THEN /\ IF \E t \in waiting["cs"] : ~woken[t]
                                       THEN /\ \E t \in {u \in waiting["cs"] : ~woken[u]}:
                                                 woken' = [woken EXCEPT ![t] = TRUE]
                                       ELSE /\ TRUE
                                            /\ woken' = woken
                            ELSE /\ TRUE
                                 /\ woken' = woken
                      /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                      /\ left' = [left EXCEPT ![self] = Head(stack[self]).left]
                      /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                      /\ UNCHANGED << mem, waiting, inSection, exclusive, 
                                      shared, items, sleepers, address, 
                                      expected, state, value, word, held, read, 
                                      last, sections_done, writes, reads, made, 
                                      eaten >>

Leave(self) == release(self) \/ wake_section(self)

flag(self) == /\ pc[self] = "flag"
              /\ IF value[self] < W
                    THEN /\ IF mem["srw"] = value[self]
                               THEN /\ mem' = [mem EXCEPT !["srw"] = value[self] + W]
                                    /\ value' = [value EXCEPT ![self] = value[self] + W]
                                    /\ pc' = [pc EXCEPT ![self] = "srw_sleep"]
                                    /\ stack' = stack
                               ELSE /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                    /\ value' = [value EXCEPT ![self] = Head(stack[self]).value]
                                    /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                                    /\ mem' = mem
                    ELSE /\ pc' = [pc EXCEPT ![self] = "srw_sleep"]
                         /\ UNCHANGED << mem, stack, value >>
              /\ UNCHANGED << waiting, woken, inSection, exclusive, shared, 
                              items, sleepers, address, expected, state, left, 
                              word, held, read, last, sections_done, writes, 
                              reads, made, eaten >>

srw_sleep(self) == /\ pc[self] = "srw_sleep"
                   /\ /\ address' = [address EXCEPT ![self] = "srw"]
                      /\ expected' = [expected EXCEPT ![self] = value[self]]
                      /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "WaitOn",
                                                               pc        |->  Head(stack[self]).pc,
                                                               address   |->  address[self],
                                                               expected  |->  expected[self] ] >>
                                                           \o Tail(stack[self])]
                   /\ pc' = [pc EXCEPT ![self] = "compare"]
                   /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                                   shared, items, sleepers, state, left, value, 
                                   word, held, read, last, sections_done, 
                                   writes, reads, made, eaten >>

SrwWait(self) == flag(self) \/ srw_sleep(self)

look_exclusive(self) == /\ pc[self] = "look_exclusive"
                        /\ word' = [word EXCEPT ![self] = mem["srw"]]
                        /\ IF word'[self] % W # 0
                              THEN /\ /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "SrwWait",
                                                                               pc        |->  "look_exclusive",
                                                                               value     |->  value[self] ] >>
                                                                           \o stack[self]]
                                      /\ value' = [value EXCEPT ![self] = word'[self]]
                                   /\ pc' = [pc EXCEPT ![self] = "flag"]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "take_exclusive"]
                                   /\ UNCHANGED << stack, value >>
                        /\ UNCHANGED << mem, waiting, woken, inSection, 
                                        exclusive, shared, items, sleepers, 
                                        address, expected, state, left, held, 
                                        read, last, sections_done, writes, 
                                        reads, made, eaten >>

take_exclusive(self) == /\ pc[self] = "take_exclusive"
                        /\ IF mem["srw"] = word[self]
                              THEN /\ mem' = [mem EXCEPT !["srw"] = word[self] + 1]
                                   /\ exclusive' = (exclusive \cup {self})
                                   /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                   /\ word' = [word EXCEPT ![self] = Head(stack[self]).word]
                                   /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                              ELSE /\ pc' = [pc EXCEPT ![self] = "look_exclusive"]
                                   /\ UNCHANGED << mem, exclusive, stack, word >>
                        /\ UNCHANGED << waiting, woken, inSection, shared, 
                                        items, sleepers, address, expected, 
                                        state, left, value, held, read, last, 
                                        sections_done, writes, reads, made, 
                                        eaten >>

AcquireExclusive(self) == look_exclusive(self) \/ take_exclusive(self)

clear_exclusive(self) == /\ pc[self] = "clear_exclusive"
                         /\ exclusive' = exclusive \ {self}
                         /\ /\ held' = [held EXCEPT ![self] = mem["srw"]]
                            /\ mem' = [mem EXCEPT !["srw"] = 0]
                         /\ pc' = [pc EXCEPT ![self] = "wake_exclusive"]
                         /\ UNCHANGED << waiting, woken, inSection, shared, 
                                         items, sleepers, stack, address, 
                                         expected, state, left, value, word, 
                                         read, last, sections_done, writes, 
                                         reads, made, eaten >>

wake_exclusive(self) == /\ pc[self] = "wake_exclusive"
                        /\ IF held[self] >= W
                              THEN /\ woken' = [t \in Threads |-> woken[t] \/ t \in waiting["srw"]]
                              ELSE /\ TRUE
                                   /\ woken' = woken
                        /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                        /\ held' = [held EXCEPT ![self] = Head(stack[self]).held]
                        /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                        /\ UNCHANGED << mem, waiting, inSection, exclusive, 
                                        shared, items, sleepers, address, 
                                        expected, state, left, value, word, 
                                        read, last, sections_done, writes, 
                                        reads, made, eaten >>

ReleaseExclusive(self) == clear_exclusive(self) \/ wake_exclusive(self)

look_shared(self) == /\ pc[self] = "look_shared"
                     /\ read' = [read EXCEPT ![self] = mem["srw"]]
                     /\ IF read'[self] % 2 # 0
                           THEN /\ /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "SrwWait",
                                                                            pc        |->  "look_shared",
                                                                            value     |->  value[self] ] >>
                                                                        \o stack[self]]
                                   /\ value' = [value EXCEPT ![self] = read'[self]]
                                /\ pc' = [pc EXCEPT ![self] = "flag"]
                           ELSE /\ pc' = [pc EXCEPT ![self] = "take_shared"]
                                /\ UNCHANGED << stack, value >>
                     /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                                     shared, items, sleepers, address, 
                                     expected, state, left, word, held, last, 
                                     sections_done, writes, reads, made, eaten >>

take_shared(self) == /\ pc[self] = "take_shared"
                     /\ IF mem["srw"] = read[self]
                           THEN /\ mem' = [mem EXCEPT !["srw"] = read[self] + 2]
                                /\ shared' = (shared \cup {self})
                                /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                /\ read' = [read EXCEPT ![self] = Head(stack[self]).read]
                                /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                           ELSE /\ pc' = [pc EXCEPT ![self] = "look_shared"]
                                /\ UNCHANGED << mem, shared, stack, read >>
                     /\ UNCHANGED << waiting, woken, inSection, exclusive, 
                                     items, sleepers, address, expected, state, 
                                     left, value, word, held, last, 
                                     sections_done, writes, reads, made, eaten >>

AcquireShared(self) == look_shared(self) \/ take_shared(self)

look_release(self) == /\ pc[self] = "look_release"
                      /\ last' = [last EXCEPT ![self] = mem["srw"]]
                      /\ pc' = [pc EXCEPT ![self] = "drop_shared"]
                      /\ UNCHANGED << mem, waiting, woken, inSection, 
                                      exclusive, shared, items, sleepers, 
                                      stack, address, expected, state, left, 
                                      value, word, held, read, sections_done, 
                                      writes, reads, made, eaten >>

drop_shared(self) == /\ pc[self] = "drop_shared"
                     /\ IF mem["srw"] = last[self]
                           THEN /\ mem' = [mem EXCEPT !["srw"] = IF last[self] % W = 2 THEN 0 ELSE last[self] - 2]
                                /\ shared' = shared \ {self}
                                /\ IF last[self] % W = 2 /\ last[self] >= W
                                      THEN /\ woken' = [t \in Threads |-> woken[t] \/ t \in waiting["srw"]]
                                      ELSE /\ TRUE
                                           /\ woken' = woken
                                /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                /\ last' = [last EXCEPT ![self] = Head(stack[self]).last]
                                /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                           ELSE /\ pc' = [pc EXCEPT ![self] = "look_release"]
                                /\ UNCHANGED << mem, woken, shared, stack, 
                                                last >>
                     /\ UNCHANGED << waiting, inSection, exclusive, items, 
                                     sleepers, address, expected, state, left, 
                                     value, word, held, read, sections_done, 
                                     writes, reads, made, eaten >>

ReleaseShared(self) == look_release(self) \/ drop_shared(self)

list(self) == /\ pc[self] = "list"
              /\ waiting' = [waiting EXCEPT !["cv"] = waiting["cv"] \cup {self}]
              /\ woken' = [woken EXCEPT ![self] = FALSE]
              /\ sleepers' = Cardinality(waiting'["cv"] \cup {self})
              /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Leave",
                                                       pc        |->  "doze",
                                                       left      |->  left[self] ] >>
                                                   \o stack[self]]
              /\ left' = [left EXCEPT ![self] = 0]
              /\ pc' = [pc EXCEPT ![self] = "release"]
              /\ UNCHANGED << mem, inSection, exclusive, shared, items, 
                              address, expected, state, value, word, held, 
                              read, last, sections_done, writes, reads, made, 
                              eaten >>

doze(self) == /\ pc[self] = "doze"
              /\ woken[self]
              /\ pc' = [pc EXCEPT ![self] = "delist"]
              /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                              shared, items, sleepers, stack, address, 
                              expected, state, left, value, word, held, read, 
                              last, sections_done, writes, reads, made, eaten >>

delist(self) == /\ pc[self] = "delist"
                /\ waiting' = [waiting EXCEPT !["cv"] = waiting["cv"] \ {self}]
                /\ sleepers' = Cardinality(waiting'["cv"] \ {self})
                /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Enter",
                                                         pc        |->  Head(stack[self]).pc,
                                                         state     |->  state[self] ] >>
                                                     \o Tail(stack[self])]
                /\ state' = [state EXCEPT ![self] = 0]
                /\ pc' = [pc EXCEPT ![self] = "try"]
                /\ UNCHANGED << mem, woken, inSection, exclusive, shared, 
                                items, address, expected, left, value, word, 
                                held, read, last, sections_done, writes, reads, 
                                made, eaten >>

Sleep(self) == list(self) \/ doze(self) \/ delist(self)

section_round(self) == /\ pc[self] = "section_round"
                       /\ IF sections_done[self] < Rounds
                             THEN /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Enter",
                                                                           pc        |->  "section_leave",
                                                                           state     |->  state[self] ] >>
                                                                       \o stack[self]]
                                  /\ state' = [state EXCEPT ![self] = 0]
                                  /\ pc' = [pc EXCEPT ![self] = "try"]
                             ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                                  /\ UNCHANGED << stack, state >>
                       /\ UNCHANGED << mem, waiting, woken, inSection, 
                                       exclusive, shared, items, sleepers, 
                                       address, expected, left, value, word, 
                                       held, read, last, sections_done, writes, 
                                       reads, made, eaten >>

section_leave(self) == /\ pc[self] = "section_leave"
                       /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Leave",
                                                                pc        |->  "section_next",
                                                                left      |->  left[self] ] >>
                                                            \o stack[self]]
                       /\ left' = [left EXCEPT ![self] = 0]
                       /\ pc' = [pc EXCEPT ![self] = "release"]
                       /\ UNCHANGED << mem, waiting, woken, inSection, 
                                       exclusive, shared, items, sleepers, 
                                       address, expected, state, value, word, 
                                       held, read, last, sections_done, writes, 
                                       reads, made, eaten >>

section_next(self) == /\ pc[self] = "section_next"
                      /\ sections_done' = [sections_done EXCEPT ![self] = sections_done[self] + 1]
                      /\ pc' = [pc EXCEPT ![self] = "section_round"]
                      /\ UNCHANGED << mem, waiting, woken, inSection, 
                                      exclusive, shared, items, sleepers, 
                                      stack, address, expected, state, left, 
                                      value, word, held, read, last, writes, 
                                      reads, made, eaten >>

section(self) == section_round(self) \/ section_leave(self)
                    \/ section_next(self)

writer_round(self) == /\ pc[self] = "writer_round"
                      /\ IF writes[self] < Rounds
                            THEN /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "AcquireExclusive",
                                                                          pc        |->  "writer_release",
                                                                          word      |->  word[self] ] >>
                                                                      \o stack[self]]
                                 /\ word' = [word EXCEPT ![self] = 0]
                                 /\ pc' = [pc EXCEPT ![self] = "look_exclusive"]
                            ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                                 /\ UNCHANGED << stack, word >>
                      /\ UNCHANGED << mem, waiting, woken, inSection, 
                                      exclusive, shared, items, sleepers, 
                                      address, expected, state, left, value, 
                                      held, read, last, sections_done, writes, 
                                      reads, made, eaten >>

writer_release(self) == /\ pc[self] = "writer_release"
                        /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "ReleaseExclusive",
                                                                 pc        |->  "writer_next",
                                                                 held      |->  held[self] ] >>
                                                             \o stack[self]]
                        /\ held' = [held EXCEPT ![self] = 0]
                        /\ pc' = [pc EXCEPT ![self] = "clear_exclusive"]
                        /\ UNCHANGED << mem, waiting, woken, inSection, 
                                        exclusive, shared, items, sleepers, 
                                        address, expected, state, left, value, 
                                        word, read, last, sections_done, 
                                        writes, reads, made, eaten >>

writer_next(self) == /\ pc[self] = "writer_next"
                     /\ writes' = [writes EXCEPT ![self] = writes[self] + 1]
                     /\ pc' = [pc EXCEPT ![self] = "writer_round"]
                     /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                                     shared, items, sleepers, stack, address, 
                                     expected, state, left, value, word, held, 
                                     read, last, sections_done, reads, made, 
                                     eaten >>

writer(self) == writer_round(self) \/ writer_release(self)
                   \/ writer_next(self)

reader_round(self) == /\ pc[self] = "reader_round"
                      /\ IF reads[self] < Rounds
                            THEN /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "AcquireShared",
                                                                          pc        |->  "reader_release",
                                                                          read      |->  read[self] ] >>
                                                                      \o stack[self]]
                                 /\ read' = [read EXCEPT ![self] = 0]
                                 /\ pc' = [pc EXCEPT ![self] = "look_shared"]
                            ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                                 /\ UNCHANGED << stack, read >>
                      /\ UNCHANGED << mem, waiting, woken, inSection, 
                                      exclusive, shared, items, sleepers, 
                                      address, expected, state, left, value, 
                                      word, held, last, sections_done, writes, 
                                      reads, made, eaten >>

reader_release(self) == /\ pc[self] = "reader_release"
                        /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "ReleaseShared",
                                                                 pc        |->  "reader_next",
                                                                 last      |->  last[self] ] >>
                                                             \o stack[self]]
                        /\ last' = [last EXCEPT ![self] = 0]
                        /\ pc' = [pc EXCEPT ![self] = "look_release"]
                        /\ UNCHANGED << mem, waiting, woken, inSection, 
                                        exclusive, shared, items, sleepers, 
                                        address, expected, state, left, value, 
                                        word, held, read, sections_done, 
                                        writes, reads, made, eaten >>

reader_next(self) == /\ pc[self] = "reader_next"
                     /\ reads' = [reads EXCEPT ![self] = reads[self] + 1]
                     /\ pc' = [pc EXCEPT ![self] = "reader_round"]
                     /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                                     shared, items, sleepers, stack, address, 
                                     expected, state, left, value, word, held, 
                                     read, last, sections_done, writes, made, 
                                     eaten >>

reader(self) == reader_round(self) \/ reader_release(self)
                   \/ reader_next(self)

produce(self) == /\ pc[self] = "produce"
                 /\ IF made[self] < Rounds
                       THEN /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Enter",
                                                                     pc        |->  "add",
                                                                     state     |->  state[self] ] >>
                                                                 \o stack[self]]
                            /\ state' = [state EXCEPT ![self] = 0]
                            /\ pc' = [pc EXCEPT ![self] = "try"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                            /\ UNCHANGED << stack, state >>
                 /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                                 shared, items, sleepers, address, expected, 
                                 left, value, word, held, read, last, 
                                 sections_done, writes, reads, made, eaten >>

add(self) == /\ pc[self] = "add"
             /\ items' = items + 1
             /\ made' = [made EXCEPT ![self] = made[self] + 1]
             /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Leave",
                                                      pc        |->  "look_sleepers",
                                                      left      |->  left[self] ] >>
                                                  \o stack[self]]
             /\ left' = [left EXCEPT ![self] = 0]
             /\ pc' = [pc EXCEPT ![self] = "release"]
             /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, shared, 
                             sleepers, address, expected, state, value, word, 
                             held, read, last, sections_done, writes, reads, 
                             eaten >>

look_sleepers(self) == /\ pc[self] = "look_sleepers"
                       /\ IF sleepers # 0
                             THEN /\ pc' = [pc EXCEPT ![self] = "wake_sleeper"]
                             ELSE /\ pc' = [pc EXCEPT ![self] = "produce"]
                       /\ UNCHANGED << mem, waiting, woken, inSection, 
                                       exclusive, shared, items, sleepers, 
                                       stack, address, expected, state, left, 
                                       value, word, held, read, last, 
                                       sections_done, writes, reads, made, 
                                       eaten >>

wake_sleeper(self) == /\ pc[self] = "wake_sleeper"
                      /\ IF \E t \in waiting["cv"] : ~woken[t]
                            THEN /\ \E t \in {u \in waiting["cv"] : ~woken[u]}:
                                      woken' = [woken EXCEPT ![t] = TRUE]
                            ELSE /\ TRUE
                                 /\ woken' = woken
                      /\ pc' = [pc EXCEPT ![self] = "produce"]
                      /\ UNCHANGED << mem, waiting, inSection, exclusive, 
                                      shared, items, sleepers, stack, address, 
                                      expected, state, left, value, word, held, 
                                      read, last, sections_done, writes, reads, 
                                      made, eaten >>

producer(self) == produce(self) \/ add(self) \/ look_sleepers(self)
                     \/ wake_sleeper(self)

consume(self) == /\ pc[self] = "consume"
                 /\ IF eaten[self] < Rounds
                       THEN /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Enter",
                                                                     pc        |->  "check",
                                                                     state     |->  state[self] ] >>
                                                                 \o stack[self]]
                            /\ state' = [state EXCEPT ![self] = 0]
                            /\ pc' = [pc EXCEPT ![self] = "try"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
                            /\ UNCHANGED << stack, state >>
                 /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                                 shared, items, sleepers, address, expected, 
                                 left, value, word, held, read, last, 
                                 sections_done, writes, reads, made, eaten >>

check(self) == /\ pc[self] = "check"
               /\ IF items = 0
                     THEN /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Sleep",
                                                                   pc        |->  "check" ] >>
                                                               \o stack[self]]
                          /\ pc' = [pc EXCEPT ![self] = "list"]
                     ELSE /\ pc' = [pc EXCEPT ![self] = "take"]
                          /\ stack' = stack
               /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                               shared, items, sleepers, address, expected, 
                               state, left, value, word, held, read, last, 
                               sections_done, writes, reads, made, eaten >>

take(self) == /\ pc[self] = "take"
              /\ items' = items - 1
              /\ eaten' = [eaten EXCEPT ![self] = eaten[self] + 1]
              /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Leave",
                                                       pc        |->  "consume",
                                                       left      |->  left[self] ] >>
                                                   \o stack[self]]
              /\ left' = [left EXCEPT ![self] = 0]
              /\ pc' = [pc EXCEPT ![self] = "release"]
              /\ UNCHANGED << mem, waiting, woken, inSection, exclusive, 
                              shared, sleepers, address, expected, state, 
                              value, word, held, read, last, sections_done, 
                              writes, reads, made >>

consumer(self) == consume(self) \/ check(self) \/ take(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in ProcSet:  \/ WaitOn(self) \/ Enter(self) \/ Leave(self)
                               \/ SrwWait(self) \/ AcquireExclusive(self)
                               \/ ReleaseExclusive(self)
                               \/ AcquireShared(self) \/ ReleaseShared(self)
                               \/ Sleep(self))
           \/ (\E self \in Sections: section(self))
           \/ (\E self \in Exclusive: writer(self))
           \/ (\E self \in Shared: reader(self))
           \/ (\E self \in Producers: producer(self))
           \/ (\E self \in Consumers: consumer(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Sections : /\ WF_vars(section(self))
                                  /\ WF_vars(Enter(self))
                                  /\ WF_vars(Leave(self))
                                  /\ WF_vars(WaitOn(self))
        /\ \A self \in Exclusive : /\ WF_vars(writer(self))
                                   /\ WF_vars(AcquireExclusive(self))
                                   /\ WF_vars(ReleaseExclusive(self))
                                   /\ WF_vars(WaitOn(self))
                                   /\ WF_vars(SrwWait(self))
        /\ \A self \in Shared : /\ WF_vars(reader(self))
                                /\ WF_vars(AcquireShared(self))
                                /\ WF_vars(ReleaseShared(self))
                                /\ WF_vars(WaitOn(self))
                                /\ WF_vars(SrwWait(self))
        /\ \A self \in Producers : /\ WF_vars(producer(self))
                                   /\ WF_vars(Enter(self))
                                   /\ WF_vars(Leave(self))
                                   /\ WF_vars(WaitOn(self))
        /\ \A self \in Consumers : /\ WF_vars(consumer(self))
                                   /\ WF_vars(Enter(self))
                                   /\ WF_vars(Sleep(self))
                                   /\ WF_vars(Leave(self))
                                   /\ WF_vars(WaitOn(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

SectionSafe == Cardinality(inSection) <= 1
SrwSafe == exclusive # {} => Cardinality(exclusive) = 1 /\ shared = {}
ItemsSafe == items >= 0
\* Every thread does all its rounds: a lost wake leaves one asleep for good.
AllDone == <>(\A t \in Threads : pc[t] = "Done")
=============================================================================
