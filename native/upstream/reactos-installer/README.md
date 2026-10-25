# Windows Installer components

`scripts/provision-installer.py` builds the Windows Installer engine and the libraries it
installs with from the pinned ReactOS archive the common controls component uses
(`dacf2a6c47fa562799f9c8a41ab6d5c558556558`, SHA-256
`5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69`), for 64-bit
programs and, with `--arch x86`, for 32-bit ones:

- `msi.dll` (`dll/win32/msi`): the installer engine, its SQL and condition grammars
  (bison) and the RPC stubs of its custom action server (widl);
- `msiexec.exe` (`base/system/msiexec`) and `regsvr32.exe` (`base/system/regsvr32`),
  which each prefix also keeps in its system directories;
- `cabinet.dll` (`dll/win32/cabinet`, on `sdk/lib/3rdparty/zlib`);
- `sxs.dll` (`dll/win32/sxs`): the side-by-side assembly cache;
- `rpcrt4.dll` (`dll/win32/rpcrt4`): remote procedure calls over local named pipes;
- `raven-storage.dll`: the structured storage of `dll/win32/ole32` and
  `dll/win32/coml2`, the compound files packages are made of, with the bind contexts of
  ole32 and the global memory streams of `dll/win32/combase`. The runtime's ole32 hands
  those exports to it;
- `oleaut32.dll` (`dll/win32/oleaut32`): automation, variants and type libraries;
- `msxml3.dll` (`dll/win32/msxml3`, on `sdk/lib/3rdparty/libxml2` and
  `dll/3rdparty/libxslt`) and `atl100.dll` (`dll/win32/atl100`);
- `urlmon.dll` (`dll/win32/urlmon`): URLs, monikers and the protocols behind them;
- the imaging stack: `windowscodecs.dll`, `gdiplus.dll`, `propsys.dll`, `mlang.dll`
  and `libpng.dll`, `libjpeg.dll`, `libtiff.dll` (`dll/3rdparty`);
- `advpack.dll` (`dll/win32/advpack`): the setup helpers IExpress packages and the
  DirectX setup run INF files through;
- `raven-setupapi.dll` (`dll/win32/setupapi`, with the LZ expansion functions of
  `dll/win32/kernel32/wine/lzexpand.c`): setup information files, file queues and
  cabinets, which the runtime's `setupapi.dll` hands to it; device management reaches
  the plug and play service through the RPC client widl writes from `pnp.idl`.

The runtime takes these libraries from beside itself whatever copies a program ships or
an installer leaves in the system directory. Each new prefix registers the classes of
`msxml3.dll`, `windowscodecs.dll`, `mlang.dll` and `propsys.dll` through `regsvr32`.

Raven's corrections to upstream behavior live in `patches/` and are applied before the
build. `support.c`, `storage.c`, `codecs.c`, `setup.c`, `printers.c`, `dllmain.c`, `entry.c`
and `entry_wide.c` are Raven code: they answer the compatibility database, binary file
patches, the class key helper and the delay-loaded imports the sources expect from
libraries Raven does not ship, and give the programs and plain libraries their entry
points. Binary patches of installed files are not applied yet.
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
