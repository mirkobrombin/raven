# Cross-architecture entitlement probes (2026-11-18 11:10-11:20, AC power, M5, macOS 27.2.1)
Signing: team Apple Development certificate, team provisioning profile with the cross-architecture capability (identifiers withheld).
Programs: probe.c (os_cross_arch_is_supported, mmap MAP_FIXED low addresses), mp.c (message-passing litmus, 300000x64 reads).

| Binary | pagezero | launch | os_cross_arch | 0x10000 | 0x400000 | 0x7ffe0000 | 0x140000000 |
|---|---|---|---|---|---|---|---|
| ad-hoc | default 4GB | ok | 1 | ENOMEM | ENOMEM | ENOMEM | ok |
| entitled .app | default 4GB | ok | 1 | ok | ok | ok | ok |
| ad-hoc | 0x4000 | killed (137) | - | - | - | - | - |
| entitled .app | 0x4000 | ok | 1 | ok | ok | ok | ok |
| entitled .app | 0x1000 (ld rounds to 0x4000) | ok | 1 | ok | ok | ok | ok |
| entitled naked Mach-O (no bundle, no profile) | default | killed (137) | - | - | - | - | - |

MP litmus violations (3 runs): arm64 plain 9/0/12; arm64 entitled 19/0/6; x86_64 under Rosetta 0/0/0.
=> entitlement does not enable TSO for the process by itself. SB litmus is invalid for TSO (TSO allows SB) - discarded.
