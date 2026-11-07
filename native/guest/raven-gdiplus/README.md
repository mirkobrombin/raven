# Guest gdiplus component

Raven's own gdiplus.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64
on the public Windows headers. It holds graphics, paths, pens and caps, brushes, regions, matrices,
string formats, fonts and text, image attributes and bitmaps. Pictures are read and written through
`windowscodecs.dll`.

Metafiles are absent: creating, playing or recording one answers `NotImplemented`.
`DirectionVertical` text is laid out as horizontal text, `GdipAddPathString` adds the boxes of the
glyphs rather than their outlines and `GdipWindingModeOutline` is not implemented.

See [LICENSES.md](../../../LICENSES.md).
