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
| UTF-8 conversion linked into the host | GPL-2.0-or-later; `native/upstream/reactos-utf8/provenance.json`, ReactOS revision `32bb061415f813b6ced25ff91f4d258f33c5191f` |
| Window-border functions linked into the host | LGPL-2.1-or-later; `native/upstream/reactos-nonclient/provenance.json` |
| Original Raven PE adapters | MIT; exact file list in LICENSES.json and terms in LICENSES/MIT.txt |
| ReactOS-derived PE controls, RTL, installer and optional text components | Distributed under GPL version 2 with the original GPL, LGPL, BSD and other source notices retained; pinned recipes and modifications in `scripts/provision-*.py` and `native/upstream` |
| CP1252 build converter | GPL-2.0-or-later; build tool, not linked into the host |
| CP1252 mapping data | Unicode-3.0; `native/upstream/reactos-nls/data-provenance.json` |
| Unicode decomposition data | Unicode terms; `native/upstream/unicode/LICENSE` and provenance.json |
| libpng codec | PNG Reference Library License version 2; `native/upstream/libpng` |
| Skia CPU backend | BSD-style terms and the notices of any compiled dependencies; `native/upstream/skia` |
| Liberation fonts, if bundled | SIL Open Font License 1.1; preserve the font package's copyright and license |
| DXVK, if bundled | zlib license and upstream notices; pinned version in `native/graphics/backends.json` |
| vkd3d-proton, if bundled | LGPL-2.1-or-later and upstream notices; pinned version in the same record |
| ReactOS delay-import test helper | MIT; `native/upstream/reactos-delayimp` |

The native link contains the selected UTF-8 and window-border functions. Its
GPLv3 source distribution includes the material needed to modify and rebuild the
LGPL portion, as LGPL-2.1 section 6 permits. Source notices remain intact.

The PE guest components are loaded and executed in the supervised Windows
process. They are not ELF libraries linked or loaded into the Linux host. Their
NT/Win32 calls cross the runtime's system-service boundary. Raven-owned code in
those PE components stays MIT so it can be combined with their GPLv2 inputs.
For sources referring to ReactOS's root COPYING, distribution uses GPL version 2;
it does not infer an "or later" permission. Explicit GPL-2.0-or-later inputs also
permit that version. LGPL portions keep their notices and corresponding source.

The controls build includes GPL user32 code as well as LGPL common controls; it
must not be described as LGPL-only. The RTL includes GPL, LGPL and BSD inputs.
Installer components also contain zlib, XML/XSLT/iconv, PNG, IJG JPEG and TIFF
code with their individual notices. Bison-generated parsers retain their skeleton
exception; the generator's GPL license alone does not determine the output's license.

IJG JPEG distributions include this acknowledgement: "This software is based in
part on the work of the Independent JPEG Group." Preserve its README's copyright
and warranty text too. System dependencies used from the recipient's distribution
are separate from copies bundled in an archive; any added static or bundled
library needs its applicable source material and notices.

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
- [ReactOS intellectual property guideline](https://reactos.org/intellectual-property-guideline/)
