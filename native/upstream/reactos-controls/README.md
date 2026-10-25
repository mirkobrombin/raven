# Common controls component

`scripts/provision-controls.py` builds the ReactOS common controls library
(`dll/win32/comctl32`) from the pinned archive at
`dacf2a6c47fa562799f9c8a41ab6d5c558556558` (SHA-256
`5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69`). The upstream
sources, resources and export specification are compiled as pinned, apart from the patches below; the build
records every command and the SHA-256 of each extracted source in
`provenance.json`.

Raven's corrections to upstream behavior live in `patches/` as unified diffs applied
before the build; their hashes are recorded in `provenance.json`.

The build exports `LoadIconWithScaleDown` at ordinal 381 as version 6 programs expect. `debug.c` replaces the upstream trace formatter with a version that never
traces, and `guids.c` instantiates the image list interface identifiers. Both are
Raven code. The component imports only the system DLLs that Raven provides.

`comctl32.dll` uses LGPL-2.1-or-later sources. The separate `raven-user32.dll`
also compiles GPL-2.0-or-later sources: `windows/dialog.c`, `draw.c`, `font.c`,
`messagebox.c`, `text.c` and `win32ss/user/rtl/text.c`. The two libraries must not
be described as one LGPL-only component. Raven's own adapters, shell, CRT, DirectWrite, Direct2D and animation code
remain MIT. The ReactOS-derived PE libraries are distributed under GPL version 2
with their original source notices retained, including LGPL and other terms.

The provisioner retains the upstream root license texts and module notices.
`provenance.json` identifies the sources, patches and binaries of each build.
See [LICENSES.md](../../../LICENSES.md) and the source-bundle requirements in
[packaging](../../../docs/packaging.md) before distributing these binaries.
