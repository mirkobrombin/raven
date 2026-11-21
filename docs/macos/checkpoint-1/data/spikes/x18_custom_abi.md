# os_set_custom_x18_abi_enabled on macOS 27.2.1 (M5), 2026-11-18 13:20-13:40, AC power

Source: spikes/build/x18diag.c (enable mode, x18=0x5eed, N raw traps, read TPIDR_EL0, x18, os_custom_x18_abi_enabled()).
Disassembly (lldb): os_set_custom_x18_abi_enabled = user-space only. It flips bit 48 of TPIDR_EL0 and tail-calls a commpage
routine (0xfffec0008 -> 0xfffec02a4) that writes TPIDR_EL0 under restart protection. No syscall. Same-value toggle -> brk with
"attempted to switch to already enabled / out of already disabled custom x18 ABI mode" (crash report, PROVEN).
Toggle pair cost: median 2.93 ns, p95 3.13, sd 0.13 (30 runs x 1e5 pairs; results/x18c__toggle_pair.csv).

| Case | Runs | Mode + x18 preserved |
|---|---|---|
| 1000 x getpid svc, ad-hoc | 1 | yes |
| 1..100 x swtch_pri / thread_switch / swtch trap | 4 | yes |
| 10000 x swtch_pri, ad-hoc | 5 | 1 of 5 (others: TPIDR low bits = new CPU, bit 48 gone, x18=0) |
| 1e6 x getpid, ad-hoc | 3 | 0 of 3 (CPU number changed) |
| 10000 x swtch_pri, cross-arch entitled bundle | 5 | 2 of 5 |
| 1e6 x getpid, cross-arch entitled bundle | 3 | 0 of 3 |

Conclusion (PROVEN on this build): the mode bit lives in TPIDR_EL0 next to the CPU number; when the kernel reschedules the
thread on another CPU it rewrites TPIDR_EL0 without bit 48, the mode is dropped and x18 is zeroed. The entitlement does not
change this. The header's promise (x18 saved/restored across context switches) does not hold on 27.2.1.
Not tested: whether pinning (no public API on macOS) or a newer build fixes it. Report to Apple via Feedback Assistant.
