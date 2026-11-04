# Windows double-byte code page data

`bestfit932.txt` (Japanese), `bestfit936.txt` (Simplified Chinese), `bestfit949.txt` (Korean) and `bestfit950.txt`
(Traditional Chinese) are Unicode's `MAPPINGS/VENDORS/MICSFT/WindowsBestFit` tables, Microsoft's description of each
code page as Windows converts it. The only change is CRLF to LF; the comments keep their original code page bytes.
`provenance.json` records the URLs and hashes, and [Unicode-3.0.txt](../../../LICENSES/Unicode-3.0.txt) the data
license, under Unicode's Terms of Use for the files in `https://www.unicode.org/Public/`.

`native/nt/dbcs_tables.cpp` runs only during the build. It reads each text, checks that every section holds as many
entries as it announces, and writes the lead byte ranges, the byte and double-byte tables and the best-fit table that
Raven's conversions use. Nothing from the files is loaded at run time.
