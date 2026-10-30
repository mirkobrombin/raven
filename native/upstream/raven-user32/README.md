# Guest user32 component

Raven's own window controls and text and dialog services for guest programs, built by
`scripts/provision-controls.py` with MinGW-w64 on the public Windows headers. It holds
the Button, Static, ListBox, ComboBox, Edit and ScrollBar classes, the dialog manager,
message boxes, `DrawText` and its relatives, the drawing helpers and the character
functions.

`exports.txt` lists what the component exports. `make_resources.py` draws the system
icons and OEM bitmaps at build time. See [LICENSES.md](../../../LICENSES.md).
