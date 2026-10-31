/* SPDX-License-Identifier: MIT */
#include "image.h"

// Metafiles are the one part this library only names: the runtime has no enhanced metafile APIs to record into or play
// from, so these calls say so instead of pretending. A real implementation starts when those APIs exist.

GpStatus GDIPAPI GdipCreateMetafileFromEmf(HENHMETAFILE handle, BOOL own, GpImage **out)
{
	(void)own;
	return handle && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipCreateMetafileFromWmf(HMETAFILE handle, BOOL own, const void *header, GpImage **out)
{
	(void)own;
	(void)header;
	return handle && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipCreateMetafileFromFile(const WCHAR *name, GpImage **out)
{
	return name && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipCreateMetafileFromWmfFile(const WCHAR *name, const void *header, GpImage **out)
{
	(void)header;
	return name && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipCreateMetafileFromStream(IStream *stream, GpImage **out)
{
	return stream && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipRecordMetafile(HDC dc, INT type, const GpRectF *frame, INT unit, const WCHAR *description, GpImage **out)
{
	(void)type;
	(void)frame;
	(void)unit;
	(void)description;
	return dc && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipRecordMetafileI(HDC dc, INT type, const GpRect *frame, INT unit, const WCHAR *description, GpImage **out)
{
	(void)type;
	(void)frame;
	(void)unit;
	(void)description;
	return dc && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipRecordMetafileFileName(const WCHAR *name, HDC dc, INT type, const GpRectF *frame, INT unit, const WCHAR *description, GpImage **out)
{
	(void)type;
	(void)frame;
	(void)unit;
	(void)description;
	return name && dc && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipRecordMetafileFileNameI(const WCHAR *name, HDC dc, INT type, const GpRect *frame, INT unit, const WCHAR *description, GpImage **out)
{
	(void)type;
	(void)frame;
	(void)unit;
	(void)description;
	return name && dc && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipRecordMetafileStream(IStream *stream, HDC dc, INT type, const GpRectF *frame, INT unit, const WCHAR *description, GpImage **out)
{
	(void)type;
	(void)frame;
	(void)unit;
	(void)description;
	return stream && dc && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipRecordMetafileStreamI(IStream *stream, HDC dc, INT type, const GpRect *frame, INT unit, const WCHAR *description, GpImage **out)
{
	(void)type;
	(void)frame;
	(void)unit;
	(void)description;
	return stream && dc && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetHemfFromMetafile(GpImage *image, HENHMETAFILE *out)
{
	return image && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetMetafileHeaderFromEmf(HENHMETAFILE handle, void *out)
{
	return handle && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetMetafileHeaderFromFile(const WCHAR *name, void *out)
{
	return name && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetMetafileHeaderFromMetafile(GpImage *image, void *out)
{
	return image && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetMetafileHeaderFromStream(IStream *stream, void *out)
{
	return stream && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetMetafileHeaderFromWmf(HMETAFILE handle, const void *header, void *out)
{
	(void)header;
	return handle && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipGetMetafileDownLevelRasterizationLimit(const GpImage *image, UINT *out)
{
	return image && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipSetMetafileDownLevelRasterizationLimit(GpImage *image, UINT limit)
{
	(void)limit;
	return image ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipPlayMetafileRecord(const GpImage *image, INT type, UINT flags, UINT size, const BYTE *data)
{
	(void)type;
	(void)flags;
	(void)size;
	(void)data;
	return image ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipConvertToEmfPlus(const void *graphics, GpImage *image, INT *success, INT type, const WCHAR *description, GpImage **out)
{
	(void)success;
	(void)type;
	(void)description;
	return graphics && image && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipConvertToEmfPlusToFile(const void *graphics, GpImage *image, INT *success, const WCHAR *name, INT type, const WCHAR *description, GpImage **out)
{
	(void)success;
	(void)name;
	(void)type;
	(void)description;
	return graphics && image && out ? NotImplemented : InvalidParameter;
}

GpStatus GDIPAPI GdipConvertToEmfPlusToStream(const void *graphics, GpImage *image, INT *success, IStream *stream, INT type, const WCHAR *description, GpImage **out)
{
	(void)success;
	(void)stream;
	(void)type;
	(void)description;
	return graphics && image && out ? NotImplemented : InvalidParameter;
}

UINT GDIPAPI GdipEmfToWmfBits(HENHMETAFILE handle, UINT size, BYTE *out, INT map_mode, INT flags)
{
	(void)handle;
	(void)size;
	(void)out;
	(void)map_mode;
	(void)flags;
	return 0;
}

#define ENUMERATE(name, ...) \
	GpStatus GDIPAPI name(const void *graphics, const GpImage *image, __VA_ARGS__) \
	{ \
		return graphics && image ? NotImplemented : InvalidParameter; \
	}

ENUMERATE(GdipEnumerateMetafileDestPoint, const GpPointF *a, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileDestPointI, const GpPoint *a, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileDestRect, const GpRectF *a, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileDestRectI, const GpRect *a, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileDestPoints, const GpPointF *a, INT n, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileDestPointsI, const GpPoint *a, INT n, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileSrcRectDestPoint, const GpPointF *a, const GpRectF *r, INT u, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileSrcRectDestPointI, const GpPoint *a, const GpRect *r, INT u, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileSrcRectDestRect, const GpRectF *a, const GpRectF *r, INT u, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileSrcRectDestRectI, const GpRect *a, const GpRect *r, INT u, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileSrcRectDestPoints, const GpPointF *a, INT n, const GpRectF *r, INT u, void *b, void *c, const void *d)
ENUMERATE(GdipEnumerateMetafileSrcRectDestPointsI, const GpPoint *a, INT n, const GpRect *r, INT u, void *b, void *c, const void *d)
