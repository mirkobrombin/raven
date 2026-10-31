# Guest atl100 component

Raven's own atl100.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64 on
the public Windows headers. It holds the registrar, which reads the scripts libraries register their
classes with, the module helpers ATL programs call, and the host that places a control in a window.

The registrar reads the whole script language: keys with `ForceRemove`, `NoRemove` and `Delete`,
values of the `s`, `d` and `b` types, and `%NAME%` replacements. Numbers are read in decimal.

The control host gives a control its site and keeps it the size of the window. A name that is a URL or
a path opens in the system's web browser class, which a prefix may not have. Dialog templates that
name a control by identifier do not build, property maps are not persisted, and the exports of the
public list that are not implemented stop the caller with an exception. See
[LICENSES.md](../../../LICENSES.md).
