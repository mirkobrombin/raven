# Windows Installer components

`scripts/provision-installer.py` builds the Windows Installer engine and the libraries it
installs with, for 64-bit programs and, with `--arch x86`, for 32-bit ones, on the MinGW-w64
headers. zlib, the Independent JPEG Group's library, LibTIFF, libxml2 and libxslt come from
their own releases, pinned by SHA-256 in the script (zlib 1.3.1, IJG JPEG 10, LibTIFF 4.7.1,
libxml2 2.12.8 and libxslt 1.1.42, configured by the script for Windows), kept beside the
components as `NAME-VERSION-source.tar.*` with their notices under `licenses/NAME-VERSION`.
The components are:

- `msi.dll` and `msiexec.exe`: Raven's own from [`native/upstream/raven-msi`](../raven-msi/README.md),
  which each prefix also keeps in its system directories;
- `sxs.dll`: Raven's own side-by-side assembly cache from [`native/upstream/raven-sxs`](../raven-sxs/README.md);
- `rpcrt4.dll`: Raven's own from [`native/upstream/raven-rpcrt4`](../raven-rpcrt4/README.md): remote
  procedure calls and the NDR engine;
- `raven-storage.dll`: Raven's own structured storage from
  [`native/upstream/raven-storage`](../raven-storage/README.md);
- `oleaut32.dll`: Raven's own from [`native/upstream/raven-oleaut32`](../raven-oleaut32/README.md): automation, variants and type libraries;
- `msxml3.dll`: Raven's own from [`native/upstream/raven-msxml3`](../raven-msxml3/README.md), on
  libxml2 and libxslt, and `atl100.dll`, Raven's own from [`native/upstream/raven-atl100`](../raven-atl100/README.md);
- `urlmon.dll`: Raven's own from [`native/upstream/raven-urlmon`](../raven-urlmon/README.md): URLs, monikers
  and the protocols behind them;
- `riched20.dll`: Raven's own from [`native/upstream/raven-riched20`](../raven-riched20/README.md): the rich
  edit control and its object model;
- the imaging stack: `windowscodecs.dll` (Raven's own, from
  [`native/upstream/raven-windowscodecs`](../raven-windowscodecs/README.md)), `gdiplus.dll` (Raven's own, from
  [`native/upstream/raven-gdiplus`](../raven-gdiplus/README.md)), `mlang.dll` (Raven's own, from
  [`native/upstream/raven-mlang`](../raven-mlang/README.md)) and `libjpeg.dll` (IJG JPEG 10) and `libtiff.dll` (LibTIFF 4.7.1), which the codecs import (PNG is decoded by `windowscodecs.dll` itself);
- `advpack.dll`: Raven's own from [`native/upstream/raven-advpack`](../raven-advpack/README.md): the setup helpers IExpress packages and the
  DirectX setup run INF files through;
- `raven-appx.dll` (`native/upstream/raven-appx`, Raven code on the same zlib): the
  packaging API, whose AppxFactory and AppxBundleFactory classes package managers read
  `.appx` and `.msix` packages with.

The runtime takes these libraries from beside itself whatever copies a program ships or
an installer leaves in the system directory. Each new prefix registers the classes of
`msxml3.dll`, `windowscodecs.dll`, `mlang.dll` and `propsys.dll` through `regsvr32`.

Raven's corrections to those libraries live in `patches/<library>/` and are applied before
the build. `support.c`, `storage.c`, `codecs.c`, `setup.c`, `dllmain.c`, `entry.c`,
`entry_wide.c` and `clicktorun.c` are Raven code: they answer the compatibility database,
binary file patches, the class key helper and the delay-loaded imports the sources expect
from libraries Raven does not ship, give the programs and plain libraries their entry
points, and let the installer's queries find the components of Click-to-Run products, and
the files those products publish by qualifier, in the manifests their packages keep. Binary patches of installed files are not applied yet.
The build records every command, the patches and the hash of each component in
`provenance.json`.

Raven-owned adapters remain MIT. The ReactOS-derived PE components are
distributed under GPL version 2 with the source notices retained.
The Wine-derived installer libraries contain LGPL-2.1-or-later code. This is not
the license of every input: zlib, libxml2, libxslt, IJG JPEG
and TIFF retain their own notices. In particular, IJG's acknowledgement must
accompany a distribution that includes its JPEG library. Generated Bison parsers
retain their GPL skeleton notice and parser exception; the generator's license
alone does not make every generated parser GPL-only.

The provisioner retains root license texts and module license, copyright and
README files. Keep the exact source archive, patches, generated inputs and
`provenance.json` with the binaries as described in
[packaging](../../../docs/packaging.md). [LICENSES.md](../../../LICENSES.md)
records the distribution boundaries; the source files remain authoritative.
