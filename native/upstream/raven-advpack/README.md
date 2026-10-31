# Guest advpack component

Raven's own advpack.dll for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers and on the setup API of `raven-setupapi`. It runs
advanced INF sections (custom destinations, pre and post commands, self registration, per
user components), handles the `REGINST` resources of `RegInstall`, deletes trees, extracts
cabinets and answers version, administrator and reboot questions.

There are no dialogs. The rollback calls that save and restore registry values or files
(`RegSaveRestore`, `RegRestoreAll`, `FileSaveRestore` and their variants) keep nothing and
return `E_NOTIMPL`.

`exports.txt` lists the exports in ordinal order; `name=other` exports one function under a
second name. See [LICENSES.md](../../../LICENSES.md).
