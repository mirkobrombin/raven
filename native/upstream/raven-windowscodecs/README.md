# Guest windowscodecs component

Raven's own windowscodecs.dll for guest programs, built by `scripts/provision-installer.py` with
MinGW-w64 on the public Windows headers. It holds the imaging factory, bitmaps, palettes, streams,
the scaler, flipper, clipper and format converter, and the BMP, PNG, GIF, ICO, JPEG and TIFF codecs.
JPEG and TIFF go through the `libjpeg.dll` and `libtiff.dll` built beside it.

Metadata is read from PNG chunks, GIF blocks, IFD, Exif and GPS directories and JPEG APP0 and APP1,
with query readers and writers over those blocks.

The encoders do not write metadata: `GetMetadataQueryWriter` on an encoder or frame answers that the
operation is unsupported, and the metadata objects cannot save themselves. DDS and WMP images, XMP and
IPTC blocks and the fast metadata encoder are absent. A path that names a block by `[*]` finds nothing
more than the first. Icons cannot be turned into bitmaps while the runtime lacks `CreateIconIndirect`.
See [LICENSES.md](../../../LICENSES.md).
