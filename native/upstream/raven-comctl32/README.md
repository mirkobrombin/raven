# Guest common controls component

Raven's own common controls for guest programs, built by
`scripts/provision-controls.py` with MinGW-w64 on the public Windows headers. It holds
the toolbar, rebar, status bar, tooltip, tab, trackbar, progress, up-down, header, list
view, tree view, animation, date and time picker, month calendar, IP address, hot key,
extended combo box and SysLink classes, image lists, property sheets, task dialogs, the
DPA, DSA, MRU and string helpers and window subclassing.

The classes in `raven-user32` are not duplicated here. `exports.txt` lists what the
component exports, with its ordinals. `make_resources.py` draws the toolbar strips,
cursors and icons at build time. `crt.c` is the small C runtime it links instead of a
full one, and the shell component uses it too. See [LICENSES.md](../../../LICENSES.md).
