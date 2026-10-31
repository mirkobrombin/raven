# Guest msvfw32 component

Raven's own msvfw32.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64 on
the public Windows headers. It holds the codec manager, DrawDib and the MCI window class.

The manager opens the codecs a program installs as functions and the DLLs named under the `Drivers32`
registry key or in `system.ini`. None is built in, so what a prefix can decode depends on what it holds.
DrawDib stretches DIBs onto a device and passes compressed sources through a decompressor.

The MCI window keeps its creation rules and messages but plays nothing, as Raven has no MCI devices
behind it. `ICCompressorChoose` answers as if the user cancelled, the preview file dialogs show no
preview, and `ICMThunk` and `StretchDIB` stop the caller with an exception. See
[LICENSES.md](../../../LICENSES.md).
