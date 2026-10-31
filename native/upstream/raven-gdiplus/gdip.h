/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_GDIP_H
#define RAVEN_GDIP_H

#include <windows.h>
#include <objidl.h>

// Public ABI of the flat API: status codes, enumerations and the structures callers hold. The objects behind the
// handles are private to this library.

typedef float REAL;
typedef DWORD ARGB;

typedef enum
{
	Ok, GenericError, InvalidParameter, OutOfMemory, ObjectBusy, InsufficientBuffer, NotImplemented, Win32Error, WrongState,
	Aborted, FileNotFound, ValueOverflow, AccessDenied, UnknownImageFormat, FontFamilyNotFound, FontStyleNotFound,
	NotTrueTypeFont, UnsupportedGdiplusVersion, GdiplusNotInitialized, PropertyNotFound, PropertyNotSupported, ProfileNotFound
} GpStatus;

typedef enum { FillModeAlternate, FillModeWinding } GpFillMode;
typedef enum { MatrixOrderPrepend, MatrixOrderAppend } GpMatrixOrder;
typedef enum { UnitWorld, UnitDisplay, UnitPixel, UnitPoint, UnitInch, UnitDocument, UnitMillimeter } GpUnit;
typedef enum { WarpModePerspective, WarpModeBilinear } WarpMode;

typedef enum
{
	PathPointTypeStart = 0, PathPointTypeLine = 1, PathPointTypeBezier = 3, PathPointTypePathTypeMask = 7,
	PathPointTypeDashMode = 0x10, PathPointTypePathMarker = 0x20, PathPointTypeCloseSubpath = 0x80
} PathPointType;

typedef enum
{
	LineCapFlat = 0, LineCapSquare = 1, LineCapRound = 2, LineCapTriangle = 3, LineCapNoAnchor = 0x10,
	LineCapSquareAnchor = 0x11, LineCapRoundAnchor = 0x12, LineCapDiamondAnchor = 0x13, LineCapArrowAnchor = 0x14,
	LineCapCustom = 0xff
} GpLineCap;

typedef enum { LineJoinMiter, LineJoinBevel, LineJoinRound, LineJoinMiterClipped } GpLineJoin;
typedef enum { DashCapFlat = 0, DashCapRound = 2, DashCapTriangle = 3 } GpDashCap;
typedef enum { DashStyleSolid, DashStyleDash, DashStyleDot, DashStyleDashDot, DashStyleDashDotDot, DashStyleCustom } GpDashStyle;
typedef enum { PenAlignmentCenter, PenAlignmentInset } GpPenAlignment;
typedef enum { CustomLineCapTypeDefault, CustomLineCapTypeAdjustableArrow } CustomLineCapType;
typedef enum { BrushTypeSolidColor, BrushTypeHatchFill, BrushTypeTextureFill, BrushTypePathGradient, BrushTypeLinearGradient } GpBrushType;
typedef enum { PenTypeSolidColor = 0, PenTypeHatchFill, PenTypeTextureFill, PenTypePathGradient, PenTypeLinearGradient, PenTypeUnknown = -1 } GpPenType;
typedef enum { WrapModeTile, WrapModeTileFlipX, WrapModeTileFlipY, WrapModeTileFlipXY, WrapModeClamp } GpWrapMode;
typedef enum { CombineModeReplace, CombineModeIntersect, CombineModeUnion, CombineModeXor, CombineModeExclude, CombineModeComplement } CombineMode;
typedef enum { LinearGradientModeHorizontal, LinearGradientModeVertical, LinearGradientModeForwardDiagonal, LinearGradientModeBackwardDiagonal } LinearGradientMode;

typedef enum
{
	StringAlignmentNear, StringAlignmentCenter, StringAlignmentFar
} StringAlignment;

typedef enum
{
	StringTrimmingNone, StringTrimmingCharacter, StringTrimmingWord, StringTrimmingEllipsisCharacter,
	StringTrimmingEllipsisWord, StringTrimmingEllipsisPath
} StringTrimming;

typedef enum { HotkeyPrefixNone, HotkeyPrefixShow, HotkeyPrefixHide } HotkeyPrefix;
typedef enum { StringDigitSubstituteUser, StringDigitSubstituteNone, StringDigitSubstituteNational, StringDigitSubstituteTraditional } StringDigitSubstitute;

enum
{
	StringFormatFlagsDirectionRightToLeft = 0x1, StringFormatFlagsDirectionVertical = 0x2, StringFormatFlagsNoFitBlackBox = 0x4,
	StringFormatFlagsDisplayFormatControl = 0x20, StringFormatFlagsNoFontFallback = 0x400, StringFormatFlagsMeasureTrailingSpaces = 0x800,
	StringFormatFlagsNoWrap = 0x1000, StringFormatFlagsLineLimit = 0x2000, StringFormatFlagsNoClip = 0x4000
};

typedef struct { REAL X, Y; } GpPointF;
typedef struct { INT X, Y; } GpPoint;
typedef struct { REAL X, Y, Width, Height; } GpRectF;
typedef struct { INT X, Y, Width, Height; } GpRect;
typedef struct { INT Count; GpPointF *Points; BYTE *Types; } GpPathData;
typedef struct { INT First, Length; } CharacterRange;

typedef struct gp_matrix { REAL m[6]; } GpMatrix;

typedef struct gp_path
{
	GpFillMode fill;
	INT count, capacity;
	GpPointF *points;
	BYTE *types;
	BOOL new_figure;
} GpPath;

typedef struct gp_iterator
{
	GpPointF *points;
	BYTE *types;
	INT count, subpath, kind, kind_end, marker;
} GpPathIterator;

struct gp_brush;
typedef struct gp_brush GpBrush;
typedef struct gp_cap GpCustomLineCap;
typedef struct gp_cap GpAdjustableArrowCap;

typedef struct gp_pen
{
	REAL width;
	GpUnit unit;
	GpBrush *brush;
	GpLineCap start_cap, end_cap;
	GpDashCap dash_cap;
	GpCustomLineCap *custom_start, *custom_end;
	GpLineJoin join;
	REAL miter_limit;
	GpPenAlignment alignment;
	GpDashStyle dash_style;
	REAL *dashes;
	INT dash_count;
	REAL dash_offset;
	REAL *compound;
	INT compound_count;
	GpMatrix transform;
} GpPen;

struct gp_cap
{
	CustomLineCapType type;
	GpPath *fill, *stroke;
	GpLineCap base_cap, start_cap, end_cap;
	GpLineJoin join;
	REAL inset, scale;
	REAL arrow_width, arrow_height, arrow_inset;
	BOOL arrow_filled;
};

typedef struct gp_format
{
	INT flags;
	LANGID language;
	StringDigitSubstitute digit_substitute;
	LANGID digit_language;
	StringAlignment alignment, line_alignment;
	StringTrimming trimming;
	HotkeyPrefix hotkey;
	REAL first_tab, *tabs;
	INT tab_count;
	CharacterRange *ranges;
	INT range_count;
	BOOL typographic;
} GpStringFormat;

#define GDIPAPI __stdcall

void *gp_alloc(SIZE_T size);
void *gp_realloc(void *memory, SIZE_T size);
void gp_free(void *memory);
void *gp_dup(const void *data, SIZE_T size);

REAL gp_deg2rad(REAL degrees);
void gp_matrix_identity(GpMatrix *out);
void gp_matrix_mul(GpMatrix *out, const GpMatrix *first, const GpMatrix *second);
void gp_matrix_apply(const GpMatrix *matrix, GpPointF *points, INT count);
BOOL gp_matrix_invert(const GpMatrix *matrix, GpMatrix *out);

GpStatus gp_path_new(GpFillMode fill, GpPath **out);
void gp_path_delete(GpPath *path);
GpPath *gp_path_clone(const GpPath *path);
BOOL gp_path_reserve(GpPath *path, INT extra);
void gp_path_push(GpPath *path, REAL x, REAL y, BYTE type);
GpStatus gp_path_flatten(GpPath *path, const GpMatrix *matrix, REAL flatness);
GpStatus gp_path_widen(GpPath *path, const GpPen *pen, const GpMatrix *matrix, REAL flatness);
void gp_path_bounds(const GpPath *path, GpRectF *out);
INT gp_arc_points(GpPointF *out, REAL x, REAL y, REAL width, REAL height, REAL start, REAL sweep);
BOOL gp_flat_contains(const GpPointF *points, const BYTE *types, INT count, GpFillMode fill, REAL x, REAL y);

GpBrush *gp_brush_clone(const GpBrush *brush);
void gp_brush_delete(GpBrush *brush);
GpBrush *gp_solid_new(ARGB color);

GpStatus GDIPAPI GdipClonePen(GpPen *pen, GpPen **out);
GpStatus GDIPAPI GdipCloneCustomLineCap(GpCustomLineCap *cap, GpCustomLineCap **out);
GpStatus GDIPAPI GdipDeleteCustomLineCap(GpCustomLineCap *cap);

#endif
