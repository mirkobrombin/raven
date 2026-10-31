# Windows Installer components

`scripts/provision-installer.py` builds the Windows Installer engine and the libraries it
installs with from the pinned ReactOS archive the common controls component uses
(`dacf2a6c47fa562799f9c8a41ab6d5c558556558`, SHA-256
`5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69`), for 64-bit
programs and, with `--arch x86`, for 32-bit ones:

- `msi.dll` (`dll/win32/msi`): the installer engine, its SQL and condition grammars
  (bison) and the RPC stubs of its custom action server (widl);
- `msiexec.exe` (`base/system/msiexec`),
  which each prefix also keeps in its system directories;
- `sxs.dll`: Raven's own side-by-side assembly cache from [`native/upstream/raven-sxs`](../raven-sxs/README.md);
- `rpcrt4.dll` (`dll/win32/rpcrt4`): remote procedure calls over local named pipes;
- `raven-storage.dll`: Raven's own structured storage from
  [`native/upstream/raven-storage`](../raven-storage/README.md);
- `oleaut32.dll` (`dll/win32/oleaut32`): automation, variants and type libraries;
- `msxml3.dll`: Raven's own from [`native/upstream/raven-msxml3`](../raven-msxml3/README.md), on
  `sdk/lib/3rdparty/libxml2` and `dll/3rdparty/libxslt`, and `atl100.dll`, Raven's own from [`native/upstream/raven-atl100`](../raven-atl100/README.md);
- `urlmon.dll`: Raven's own from [`native/upstream/raven-urlmon`](../raven-urlmon/README.md): URLs, monikers
  and the protocols behind them;
- the imaging stack: `windowscodecs.dll` (Raven's own, from
  [`native/upstream/raven-windowscodecs`](../raven-windowscodecs/README.md)), `gdiplus.dll`, `mlang.dll` (Raven's own, from
  [`native/upstream/raven-mlang`](../raven-mlang/README.md)) and `libpng.dll`, `libjpeg.dll`, `libtiff.dll` (`dll/3rdparty`);
- `advpack.dll`: Raven's own from [`native/upstream/raven-advpack`](../raven-advpack/README.md): the setup helpers IExpress packages and the
  DirectX setup run INF files through.

The runtime takes these libraries from beside itself whatever copies a program ships or
an installer leaves in the system directory. Each new prefix registers the classes of
`msxml3.dll`, `windowscodecs.dll`, `mlang.dll` and `propsys.dll` through `regsvr32`.

Raven's corrections to upstream behavior live in `patches/` and are applied before the
build. `support.c`, `storage.c`, `codecs.c`, `setup.c`, `dllmain.c`, `entry.c`,
`entry_wide.c` and `clicktorun.c` are Raven code: they answer the compatibility database,
binary file patches, the class key helper and the delay-loaded imports the sources expect
from libraries Raven does not ship, give the programs and plain libraries their entry
points, and let the installer's queries find the components of Click-to-Run products, and
the files those products publish by qualifier, in the manifests their packages keep. Binary patches of installed files are not applied yet.
The build records every command, the hash of each extracted source and of each
component in `provenance.json`.

Raven-owned adapters remain MIT. The ReactOS-derived PE components are
distributed under GPL version 2 with the source notices retained.
The Wine-derived installer libraries contain LGPL-2.1-or-later code. This is not
the license of every input: zlib, libxml2, libxslt, libwin-iconv, libpng, IJG JPEG
and TIFF retain their own notices. In particular, IJG's acknowledgement must
accompany a distribution that includes its JPEG library. Generated Bison parsers
retain their GPL skeleton notice and parser exception; the generator's license
alone does not make every generated parser GPL-only.

The provisioner retains root license texts and module license, copyright and
README files. Keep the exact source archive, patches, generated inputs and
`provenance.json` with the binaries as described in
[packaging](../../../docs/packaging.md). [LICENSES.md](../../../LICENSES.md)
records the distribution boundaries; the source files remain authoritative.
