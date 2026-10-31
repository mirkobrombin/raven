# Guest mlang component

Raven's own mlang.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64 on
the public Windows headers. It lists code pages and language tags, converts text between encodings,
detects encodings and answers font linking questions.

Conversion goes through the system's `MultiByteToWideChar` and `WideCharToMultiByte`. EUC-JP and
ISO-2022-JP are spoken here by rearranging Shift-JIS, so they work as far as the system handles page
932. `pages.inc` and the tag table in `tags.c` hold the facts of the encodings, in the published names
and numbers; the fonts named for each page and script are the system defaults those pages expect.

Encoding detection looks at byte order marks and UTF-8 validity and otherwise falls back on the
caller's page, then on 1252. The conversion's `dwProperty` and fallback characters are ignored, as is
the language asked for in descriptions. See [LICENSES.md](../../../LICENSES.md).
