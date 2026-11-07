# Windows single-byte code page data

| File | Source |
| --- | --- |
| `c_1250.txt` to `c_1258.txt` | Unicode's `MAPPINGS/VENDORS/MICSFT/WindowsBestFit/bestfit<page>.txt`, CRLF changed to LF |
| `CP437.TXT`, `CP850.TXT` | Unicode's `MAPPINGS/VENDORS/MICSFT/PC` tables, unchanged |

`data-provenance.json` (CP1252) and `single-byte-provenance.json` (the others) record the
URLs and hashes. The data is under [Unicode-3.0](../../../LICENSES/Unicode-3.0.txt).

The build checks every hash, then `native/nt/nls_tables.cpp` and `sbcs.cmake` turn the
texts into the byte and best-fit tables that MultiByteToWideChar, WideCharToMultiByte,
IsValidCodePage and GetCPInfo use. CP437 and CP850 have no best-fit table, so a character
they lack becomes `?`. The double-byte pages are in [`windows-bestfit`](../windows-bestfit/README.md).
