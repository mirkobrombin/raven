# Guest rich edit component

Raven's own riched20.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64
on the public Windows headers. `msftedit.dll` forwards to it. It holds the RichEdit 1.0, 2.0 and 5.0
window classes, the document model with undo, layout and painting, RTF reading and writing, text
services for programs with their own host, and `IRichEditOle` with the text object model.

The type library of the text object model comes from `tom_library.idl`, which the build compiles and
`resources.rc` carries, so automation clients can reach every interface.

Embedded objects are measured and drawn through `IViewObject`, but RTF neither reads nor writes them.
Tables, right-to-left layout and complex script shaping are absent. `GetClipboardData` hands out plain
text only. In the object model finding text, `MoveWhile` and `MoveUntil`, cases, screen points and units
larger than a character other than the story answer `E_NOTIMPL`, and an open or saved document has no
file behind it. See [LICENSES.md](../../../LICENSES.md).
