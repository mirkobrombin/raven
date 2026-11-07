# Windows Installer components

`scripts/provision-installer.py` builds the Windows Installer engine and the libraries
installers use, for 64-bit programs and, with `--arch x86`, for 32-bit ones. Each Raven
library has its own folder and README under `native/guest/raven-*`.

These libraries come from their own releases, pinned by SHA-256 in the script:

| Library | Version | Used by |
| --- | --- | --- |
| zlib | 1.3.1 | `cabinet.dll`, `msxml3.dll`, `libtiff.dll`, `raven-appx.dll` |
| IJG JPEG | 10 | `libjpeg.dll`, imported by `windowscodecs.dll` |
| LibTIFF | 4.7.1 | `libtiff.dll`, imported by `windowscodecs.dll` |
| libxml2 | 2.12.8 | `msxml3.dll` |
| libxslt | 1.1.42 | `msxml3.dll` |

The build keeps each source archive as `NAME-VERSION-source.tar.*` and its notices under
`licenses/NAME-VERSION`, applies Raven's patches from `patches/<library>/`, and records
the commands, patches and component hashes in `provenance.json`.

This folder holds the pieces that belong to no single library: `support.c`, what the
installer engine needs from libraries Raven does not ship; `dllmain.c`, the entry point
of the libraries above that have none; `msftedit.c`, the rich edit 4.1 control on top of
`riched20.dll`.
