# CP1252 data

The unchanged `txt2nls.cpp` and `c_1252.txt` come from ReactOS revision
`dacf2a6c47fa562799f9c8a41ab6d5c558556558`, at `sdk/tools/txt2nls/main.cpp`
and `media/nls/src/c_1252.txt`. The source archive SHA-256 is
`5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69`.
The tool retains its GPL-2.0-or-later notice and COPYING. ReactOS identifies
Microsoft's Windows Supported Code Page Data Files as the text data's origin.
The same table is published as Unicode's `WindowsBestFit/bestfit1252.txt`.
Every byte matches after CRLF-to-LF conversion. `data-provenance.json` records
the URLs and hashes; [Unicode-3.0.txt](../../../LICENSES/Unicode-3.0.txt) preserves
the applicable data license. Unicode's Terms of Use cover data files under
`https://www.unicode.org/Public/` unless a specific exception is indicated.
This records the table's terms separately from the GPL converter's license.

The tool runs only during the build. Its generated NLS file must match
`e2634ee32da7977d762d2d45be2053309ee718572a7d8bb0034180dc1b0b6b76` before
the qualified 65536-byte Unicode-to-CP1252 and 256-entry byte-to-Unicode tables
are embedded. Raven's conversion
uses that immutable data; no upstream loader or process state is adopted.
The SBCS mapping passed every UTF-16 code unit against pinned Wine NT and Win32
conversion. Other legacy code pages, DBCS and UTF-8 ACP remain unsupported.

MultiByteToWideChar uses this byte table for the owned ACP profile and explicit
CP1252. MB_COMPOSITE uses the selected Unicode canonical decompositions. Explicit
UTF-8 conversion uses a separate selected ReactOS decoder; its native adapter
and tests preserve the measured status/buffer behavior. This does not extend
the process ACP configuration or the existing NT encode_ansi helper to UTF-8.
