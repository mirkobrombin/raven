# Guest text layout

The original text worker and five CRT bodies are selected at ReactOS revision
32bb061415f813b6ced25ff91f4d258f33c5191f. Source paths, hashes and notices remain
in provenance.json and the source files. The text worker carries GPL-2.0-or-later
and explicit Wine-derived attribution. COPYING and COPYING.LIB are retained.
This is selected ReactOS reuse, not a direct Wine source port or a clean-room
implementation. The PE component is distributed under GPL version 2, retaining the worker's
GPL-2.0-or-later notice and the original CRT source. Raven-owned wrapper and
shim code remains MIT. It is not linked into the Linux host.

scripts/provision-text.py applies failure-propagation.patch and layout-contracts.patch
and builds a PE64
component with no CRT startup or external CRT DLL. The patch carries errors
through measurements, checks font metrics and restores clipping on failed output.
The adapter builds the user-mode source branch only. It owns no Windows state:
all GDI resources, font metrics, pixels and process memory belong to Raven.

RavenDrawTextExW is a private component export. The optional native tests compare
72 qualified rendering/result cases, 588 alignment/margin/ellipsis cases and 28
unsupported-path cases, including bounded horizontal arithmetic. They do
not publish USER32 DrawText/DrawTextEx, register controls or claim complete layout
compatibility. The 3780-case audit now has 186 differences against Wine after the margin and
vertical-centering corrections, compared with 1114 before them. Remaining path
ellipsis, wrapping/calculation and tab cases still need correction. Microsoft
documents margin units as average-character widths; the pinned Wine probe uses
pixel increments. Direct Windows comparison remains open.
