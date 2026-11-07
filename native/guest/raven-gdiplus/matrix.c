/* SPDX-License-Identifier: MIT */
#include "gdip.h"
#include "gmath.h"

// Six numbers of a 3x2 matrix in row-vector form: a point (x, y, 1) times the matrix gives the transformed point.

REAL gp_deg2rad(REAL degrees)
{
	return degrees * (REAL)(GP_PI / 180.0);
}

void gp_matrix_identity(GpMatrix *out)
{
	static const GpMatrix identity = {{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}};
	*out = identity;
}

void gp_matrix_mul(GpMatrix *out, const GpMatrix *first, const GpMatrix *second)
{
	const REAL *a = first->m, *b = second->m;
	GpMatrix r;
	r.m[0] = a[0] * b[0] + a[1] * b[2];
	r.m[1] = a[0] * b[1] + a[1] * b[3];
	r.m[2] = a[2] * b[0] + a[3] * b[2];
	r.m[3] = a[2] * b[1] + a[3] * b[3];
	r.m[4] = a[4] * b[0] + a[5] * b[2] + b[4];
	r.m[5] = a[4] * b[1] + a[5] * b[3] + b[5];
	*out = r;
}

void gp_matrix_apply(const GpMatrix *matrix, GpPointF *points, INT count)
{
	const REAL *m = matrix->m;
	for (INT i = 0; i < count; i++) {
		REAL x = points[i].X, y = points[i].Y;
		points[i].X = x * m[0] + y * m[2] + m[4];
		points[i].Y = x * m[1] + y * m[3] + m[5];
	}
}

static double determinant(const GpMatrix *matrix)
{
	return (double)matrix->m[0] * matrix->m[3] - (double)matrix->m[1] * matrix->m[2];
}

BOOL gp_matrix_invert(const GpMatrix *matrix, GpMatrix *out)
{
	const REAL *m = matrix->m;
	double det = determinant(matrix);
	if (det == 0.0)
		return FALSE;
	GpMatrix r;
	r.m[0] = (REAL)(m[3] / det);
	r.m[1] = (REAL)(-m[1] / det);
	r.m[2] = (REAL)(-m[2] / det);
	r.m[3] = (REAL)(m[0] / det);
	r.m[4] = (REAL)((m[2] * (double)m[5] - m[3] * (double)m[4]) / det);
	r.m[5] = (REAL)((m[1] * (double)m[4] - m[0] * (double)m[5]) / det);
	*out = r;
	return TRUE;
}

static void compose(GpMatrix *matrix, const GpMatrix *step, GpMatrixOrder order)
{
	if (order == MatrixOrderAppend)
		gp_matrix_mul(matrix, matrix, step);
	else
		gp_matrix_mul(matrix, step, matrix);
}

GpStatus GDIPAPI GdipCreateMatrix(GpMatrix **out)
{
	if (!out)
		return InvalidParameter;
	if (!(*out = gp_alloc(sizeof(GpMatrix))))
		return OutOfMemory;
	gp_matrix_identity(*out);
	return Ok;
}

GpStatus GDIPAPI GdipCreateMatrix2(REAL m11, REAL m12, REAL m21, REAL m22, REAL dx, REAL dy, GpMatrix **out)
{
	if (!out)
		return InvalidParameter;
	if (!(*out = gp_alloc(sizeof(GpMatrix))))
		return OutOfMemory;
	(*out)->m[0] = m11;
	(*out)->m[1] = m12;
	(*out)->m[2] = m21;
	(*out)->m[3] = m22;
	(*out)->m[4] = dx;
	(*out)->m[5] = dy;
	return Ok;
}

// The matrix mapping the rectangle onto the parallelogram given by its top-left, top-right and bottom-left corners.
GpStatus GDIPAPI GdipCreateMatrix3(const GpRectF *rect, const GpPointF *points, GpMatrix **out)
{
	if (!rect || !points || !out)
		return InvalidParameter;
	if (!(*out = gp_alloc(sizeof(GpMatrix))))
		return OutOfMemory;
	GpMatrix to_unit = {{1.0f, 0.0f, 0.0f, 1.0f, -rect->X, -rect->Y}};
	GpMatrix shape;
	REAL width = rect->Width, height = rect->Height;
	shape.m[0] = (points[1].X - points[0].X) / width;
	shape.m[1] = (points[1].Y - points[0].Y) / width;
	shape.m[2] = (points[2].X - points[0].X) / height;
	shape.m[3] = (points[2].Y - points[0].Y) / height;
	shape.m[4] = points[0].X;
	shape.m[5] = points[0].Y;
	gp_matrix_mul(*out, &to_unit, &shape);
	return Ok;
}

GpStatus GDIPAPI GdipCreateMatrix3I(const GpRect *rect, const GpPoint *points, GpMatrix **out)
{
	if (!rect || !points)
		return InvalidParameter;
	GpRectF box = {(REAL)rect->X, (REAL)rect->Y, (REAL)rect->Width, (REAL)rect->Height};
	GpPointF corners[3];
	for (int i = 0; i < 3; i++) {
		corners[i].X = (REAL)points[i].X;
		corners[i].Y = (REAL)points[i].Y;
	}
	return GdipCreateMatrix3(&box, corners, out);
}

GpStatus GDIPAPI GdipCloneMatrix(GpMatrix *matrix, GpMatrix **out)
{
	if (!matrix || !out)
		return InvalidParameter;
	if (!(*out = gp_dup(matrix, sizeof(GpMatrix))))
		return OutOfMemory;
	return Ok;
}

GpStatus GDIPAPI GdipDeleteMatrix(GpMatrix *matrix)
{
	if (!matrix)
		return InvalidParameter;
	gp_free(matrix);
	return Ok;
}

GpStatus GDIPAPI GdipSetMatrixElements(GpMatrix *matrix, REAL m11, REAL m12, REAL m21, REAL m22, REAL dx, REAL dy)
{
	if (!matrix)
		return InvalidParameter;
	matrix->m[0] = m11;
	matrix->m[1] = m12;
	matrix->m[2] = m21;
	matrix->m[3] = m22;
	matrix->m[4] = dx;
	matrix->m[5] = dy;
	return Ok;
}

GpStatus GDIPAPI GdipGetMatrixElements(const GpMatrix *matrix, REAL *out)
{
	if (!matrix || !out)
		return InvalidParameter;
	memcpy(out, matrix->m, sizeof(matrix->m));
	return Ok;
}

GpStatus GDIPAPI GdipInvertMatrix(GpMatrix *matrix)
{
	if (!matrix)
		return InvalidParameter;
	if (matrix->m[0] == 1.0f && matrix->m[1] == 0.0f && matrix->m[2] == 0.0f && matrix->m[3] == 1.0f && matrix->m[4] == 0.0f && matrix->m[5] == 0.0f)
		return Ok;
	return gp_matrix_invert(matrix, matrix) ? Ok : InvalidParameter;
}

GpStatus GDIPAPI GdipMultiplyMatrix(GpMatrix *matrix, const GpMatrix *other, GpMatrixOrder order)
{
	if (!matrix || !other)
		return InvalidParameter;
	if (order != MatrixOrderPrepend && order != MatrixOrderAppend)
		return InvalidParameter;
	compose(matrix, other, order);
	return Ok;
}

GpStatus GDIPAPI GdipRotateMatrix(GpMatrix *matrix, REAL angle, GpMatrixOrder order)
{
	if (!matrix || (order != MatrixOrderPrepend && order != MatrixOrderAppend))
		return InvalidParameter;
	REAL radians = gp_deg2rad(angle), c = gp_cosf(radians), s = gp_sinf(radians);
	GpMatrix step = {{c, s, -s, c, 0.0f, 0.0f}};
	compose(matrix, &step, order);
	return Ok;
}

GpStatus GDIPAPI GdipScaleMatrix(GpMatrix *matrix, REAL x, REAL y, GpMatrixOrder order)
{
	if (!matrix || (order != MatrixOrderPrepend && order != MatrixOrderAppend))
		return InvalidParameter;
	GpMatrix step = {{x, 0.0f, 0.0f, y, 0.0f, 0.0f}};
	compose(matrix, &step, order);
	return Ok;
}

GpStatus GDIPAPI GdipShearMatrix(GpMatrix *matrix, REAL x, REAL y, GpMatrixOrder order)
{
	if (!matrix || (order != MatrixOrderPrepend && order != MatrixOrderAppend))
		return InvalidParameter;
	GpMatrix step = {{1.0f, y, x, 1.0f, 0.0f, 0.0f}};
	compose(matrix, &step, order);
	return Ok;
}

GpStatus GDIPAPI GdipTranslateMatrix(GpMatrix *matrix, REAL x, REAL y, GpMatrixOrder order)
{
	if (!matrix || (order != MatrixOrderPrepend && order != MatrixOrderAppend))
		return InvalidParameter;
	GpMatrix step = {{1.0f, 0.0f, 0.0f, 1.0f, x, y}};
	compose(matrix, &step, order);
	return Ok;
}

GpStatus GDIPAPI GdipTransformMatrixPoints(GpMatrix *matrix, GpPointF *points, INT count)
{
	if (!matrix || !points || count <= 0)
		return InvalidParameter;
	gp_matrix_apply(matrix, points, count);
	return Ok;
}

static LONG round_to_long(REAL value)
{
	return (LONG)gp_floor(value + 0.5f);
}

GpStatus GDIPAPI GdipTransformMatrixPointsI(GpMatrix *matrix, GpPoint *points, INT count)
{
	if (!matrix || !points || count <= 0)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		GpPointF point = {(REAL)points[i].X, (REAL)points[i].Y};
		gp_matrix_apply(matrix, &point, 1);
		points[i].X = round_to_long(point.X);
		points[i].Y = round_to_long(point.Y);
	}
	return Ok;
}

GpStatus GDIPAPI GdipVectorTransformMatrixPoints(GpMatrix *matrix, GpPointF *points, INT count)
{
	if (!matrix || !points || count <= 0)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		REAL x = points[i].X, y = points[i].Y;
		points[i].X = x * matrix->m[0] + y * matrix->m[2];
		points[i].Y = x * matrix->m[1] + y * matrix->m[3];
	}
	return Ok;
}

GpStatus GDIPAPI GdipVectorTransformMatrixPointsI(GpMatrix *matrix, GpPoint *points, INT count)
{
	if (!matrix || !points || count <= 0)
		return InvalidParameter;
	for (INT i = 0; i < count; i++) {
		REAL x = (REAL)points[i].X, y = (REAL)points[i].Y;
		points[i].X = round_to_long(x * matrix->m[0] + y * matrix->m[2]);
		points[i].Y = round_to_long(x * matrix->m[1] + y * matrix->m[3]);
	}
	return Ok;
}

GpStatus GDIPAPI GdipIsMatrixInvertible(const GpMatrix *matrix, BOOL *out)
{
	if (!matrix || !out)
		return InvalidParameter;
	*out = determinant(matrix) != 0.0;
	return Ok;
}

GpStatus GDIPAPI GdipIsMatrixEqual(const GpMatrix *matrix, const GpMatrix *other, BOOL *out)
{
	if (!matrix || !other || !out)
		return InvalidParameter;
	*out = TRUE;
	for (int i = 0; i < 6; i++) {
		REAL difference = matrix->m[i] - other->m[i];
		if (gp_fabs(difference) > 1e-5f)
			*out = FALSE;
	}
	return Ok;
}

GpStatus GDIPAPI GdipIsMatrixIdentity(const GpMatrix *matrix, BOOL *out)
{
	if (!matrix || !out)
		return InvalidParameter;
	GpMatrix identity;
	gp_matrix_identity(&identity);
	return GdipIsMatrixEqual(matrix, &identity, out);
}
