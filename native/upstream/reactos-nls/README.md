# CP1252 data

The unchanged `c_1252.txt` comes from ReactOS revision
`dacf2a6c47fa562799f9c8a41ab6d5c558556558`, at `media/nls/src/c_1252.txt`. The source archive SHA-256 is
`5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69`.
ReactOS identifies
Microsoft's Windows Supported Code Page Data Files as the text data's origin.
The same table is published as Unicode's `WindowsBestFit/bestfit1252.txt`.
Every byte matches after CRLF-to-LF conversion. `data-provenance.json` records
the URLs and hashes; [Unicode-3.0.txt](../../../LICENSES/Unicode-3.0.txt) preserves
the applicable data license. Unicode's Terms of Use cover data files under
`https://www.unicode.org/Public/` unless a specific exception is indicated.

Raven's `native/nt/nls_tables.cpp` runs only during the build. Its generated NLS file must match
`e2634ee32da7977d762d2d45be2053309ee718572a7d8bb0034180dc1b0b6b76` before
the qualified 65536-byte Unicode-to-CP1252 and 256-entry byte-to-Unicode tables
are embedded. Raven's conversion
uses that immutable data; no upstream loader or process state is adopted.
The SBCS mapping passed every UTF-16 code unit against pinned Wine NT and Win32
conversion.

## Other Windows single-byte code pages

`c_1250.txt`, `c_1251.txt` and `c_1253.txt` to `c_1258.txt` come from the same
ReactOS revision and archive, at `media/nls/src`. Each matches Unicode's
`WindowsBestFit/bestfit<page>.txt` byte for byte after CRLF-to-LF conversion;
`single-byte-provenance.json` records the URLs and hashes under the same
Unicode-3.0 terms. CP874 is left out because its ReactOS copy differs from the
Unicode file.

## OEM code pages

`CP437.TXT` and `CP850.TXT`, the OEM pages a console uses, are Unicode's
`MAPPINGS/VENDORS/MICSFT/PC` tables, unchanged. Unicode publishes no best-fit
table for them, so they carry only the byte-to-character table; every byte maps
to the same character as in ReactOS's `c_437.txt` and `c_850.txt` at the
revision above. A character converts back only to its own byte: there is no best
fit for these two pages, and a character they lack becomes the default `?`.

`sbcs.cmake` reads each text at build time into its 256-entry byte table and its
best-fit table, sorted by character; the build checks every input hash first.
MultiByteToWideChar, WideCharToMultiByte, IsValidCodePage and GetCPInfo use them,
and WC_NO_BEST_FIT_CHARS keeps only characters that read back unchanged. MB_COMPOSITE
decomposes only the CP1252 characters. Other legacy code pages, DBCS and UTF-8 ACP
remain unsupported.

MultiByteToWideChar uses this byte table for the owned ACP profile and explicit
CP1252. MB_COMPOSITE uses the selected Unicode canonical decompositions. Explicit
UTF-8 conversion uses a separate selected ReactOS decoder; its native adapter
and tests preserve the measured status/buffer behavior. This does not extend
the process ACP configuration or the existing NT encode_ansi helper to UTF-8.
