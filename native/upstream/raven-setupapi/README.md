# Guest setup API component

Raven's own setup API for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It reads INF files, runs file queues with their
source media and cabinets, installs sections (registry, INI files, copies, self registration,
start menu entries, services), keeps disk space lists and string tables and expands SZDD
files. The runtime's setupapi.dll hands those exports to it.

An open INF is a list of parsed files, and [Strings] substitution runs once a whole file is
read. A queue groups its copies by source media, so a callback hears about one media once.
There are no dialogs: prompts answer for themselves and the default queue callback skips a
file it cannot copy.

Raven has no plug and play manager or device tree. A device set opens empty and enumerates
nothing, device classes answer from the registry, and the configuration manager and driver
exports listed in `stubs.inc` report failure with the arguments their callers push.

`exports.txt` lists what the component exports; `name=other` exports one function under a
second name. See [LICENSES.md](../../../LICENSES.md).
