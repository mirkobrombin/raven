# Guest urlmon component

Raven's own urlmon.dll for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It holds the URI objects and builder, URL monikers,
the asynchronous bindings with their status callback holder, the `file`, `mk`, `http`, `https`
and `ftp` protocols (the network ones over wininet), the internet session with its namespaces
and MIME filters, MIME sniffing, the security and zone managers, and the blocking download calls.

The saved form of a URI and the results of the parsing calls follow what programs can observe.
Interfaces are not marshalled across processes through proxies, the `about` protocol is left to
whoever registers it, code download (`AsyncInstallDistributionUnit`) returns `E_NOTIMPL`, and no
Mark of the Web is written next to downloaded files.

`exports.txt` lists the exports: `name` by name, `name=other` under a second name, an `@n` entry
at that ordinal without a name. Entries that point at `urlmon_missing` return `E_NOTIMPL`. See
[LICENSES.md](../../../LICENSES.md).
