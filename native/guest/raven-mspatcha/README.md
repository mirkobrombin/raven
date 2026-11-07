# Guest mspatcha component

Raven's own mspatcha.dll for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It applies the PA19 delta patches that Windows
Installer patches and other updaters carry: it checks the patch and the old file against their
CRC-32, decodes the LZX DELTA stream of [MS-PATCH] over a window that starts with the old file,
carries the retained ranges across and checks the new file's CRC-32.

- `pa19.c`: the patch file header, the choice of old file and the checks around the decoding.
- `lzxd.c`: the LZX DELTA decoder, with its uncompressed, verbatim and aligned blocks, long matches
  and the x86 call translation.
- `api.c`: the exports, over file names, handles and buffers.

Patches for 32-bit executables whose old file was rewritten before it was compared, and old files
larger than the decoding window, fail with `ERROR_PATCH_PACKAGE_UNSUPPORTED`. The signature
functions (`GetFilePatchSignature*`, `NormalizeFileForPatchSignature`) fail with
`ERROR_CALL_NOT_IMPLEMENTED`.

`exports.txt` lists the exports. See [LICENSES.md](../../../LICENSES.md).
