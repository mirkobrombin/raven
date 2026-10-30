# Cabinet

`cabinet.dll`, built for both widths by `scripts/provision-installer.py`: FDI to read cabinets, FCI to write them, and the older `Extract`.

It is Raven code written from Microsoft's public Cabinet Format and LZX Data Compression specifications; deflate is zlib.

- `extract.c`: FDI. Reads stored, MSZIP, LZX and Quantum folders, checks block sums, and follows a folder across volumes through the
  `fdintNEXT_CABINET` notification.
- `lzx.c`, `quantum.c`: the two decoders, keeping their history across the blocks of a folder.
- `compose.c`: FCI. Writes stored and MSZIP folders and splits them over volumes on the size the caller sets, ending a volume on part of a
  block. A request for LZX or Quantum writes a stored folder.
- `session.c`: `Extract` and the version queries. `DeleteExtractedFiles` does nothing.

`FDITruncateCabinet` fails with `ERROR_CALL_NOT_IMPLEMENTED`.
