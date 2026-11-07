# Licenses

Raven's Linux host runtime is GPL-3.0-or-later. Original Raven guest adapters
listed in [LICENSES.json](LICENSES.json) remain MIT-licensed. Imported source keeps
its copyright, license notices and any exceptions. Earlier MIT grants remain valid.
The release is a collection of components with these terms, not a single MIT or
GPLv3 license applied to every file.

## Host and guest components

| Component | Terms and source |
| --- | --- |
| Linux `raven` and `raven-bootstrap` | GPL-3.0-or-later; root LICENSE |
| Original Raven PE adapters | MIT; exact file list in LICENSES.json and terms in LICENSES/MIT.txt |
| PE installer components | Raven code under MIT with zlib, libxml2, libxslt, IJG JPEG and LibTIFF under their own notices; pinned releases, recipe and patches in `scripts/provision-installer.py` and `native/guest/raven-installer` |
| CP1252 mapping data | Unicode-3.0; `native/upstream/windows-codepages/data-provenance.json` |
| Code pages 1250, 1251 and 1253-1258 mapping data | Unicode-3.0; `native/upstream/windows-codepages/single-byte-provenance.json` |
| Unicode decomposition data | Unicode terms; `native/upstream/unicode/LICENSE` and provenance.json |
| libpng codec | PNG Reference Library License version 2; `native/upstream/libpng` |
| Skia CPU backend | BSD-style terms and the notices of any compiled dependencies; `native/upstream/skia` |
| Liberation fonts, if bundled | SIL Open Font License 1.1; preserve the font package's copyright and license |
| Raven Sans fonts, if bundled | Bitstream Vera and Arev font terms, DejaVu changes in the public domain; derived from DejaVu Sans Condensed 2.37 by `scripts/provision-text.py` under a name the terms permit; LICENSES/DejaVu-fonts.txt |
| DXVK, if bundled | zlib license and upstream notices; pinned version in `native/graphics/backends.json` |
| vkd3d-proton, if bundled | LGPL-2.1-or-later and upstream notices; pinned version in the same record |
| SQLite (`winsqlite3.dll`), if bundled | Public domain (SPDX `blessing`); unmodified amalgamation pinned in `scripts/provision-sqlite.py` |
| Windows Runtime interface declarations | MIT (Microsoft's metadata from windows-rs); `native/guest/raven-web/windows.web.http.h`, `native/guest/raven-winrt/foundation.h` |
| Windows packaging interface declarations | MIT (Microsoft's metadata from windows-rs); `native/guest/raven-appx/appxpackaging.h` |

The PE guest components run in the supervised Windows process; they are not
linked or loaded into the Linux host.

This software is based in part on the work of the Independent JPEG Group.

## Release contents

Users download the binary archive. The same release also provides the exact
corresponding source, patches and build/install scripts, including source material
for the third-party inputs actually shipped. Users do not have to download the
source to run Raven. Both archives include notices; the binary archive identifies
its matching source archive. See [packaging](docs/packaging.md).

License texts are in [LICENSES](LICENSES). File notices remain authoritative;
a manifest does not replace them or grant permission over third-party work.

- [GNU license compatibility](https://www.gnu.org/licenses/license-compatibility.html)
- [GPLv3, corresponding source and section 6](https://www.gnu.org/licenses/gpl-3.0.html)
- [LGPL 2.1, section 6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
