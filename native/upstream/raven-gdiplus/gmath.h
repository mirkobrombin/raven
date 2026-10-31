/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_GMATH_H
#define RAVEN_GMATH_H

#define GP_PI 3.14159265358979323846

double gp_m_sin(double);
double gp_m_cos(double);
double gp_m_tan(double);
double gp_m_atan(double);
double gp_m_atan2(double, double);
double gp_m_sqrt(double);
double gp_m_pow(double, double);
double gp_m_fmod(double, double);
double gp_m_floor(double);
double gp_m_ceil(double);

#define sin gp_m_sin
#define cos gp_m_cos
#define tan gp_m_tan
#define atan gp_m_atan
#define atan2 gp_m_atan2
#define sqrt gp_m_sqrt
#define pow gp_m_pow
#define fmod gp_m_fmod
#define floor gp_m_floor
#define ceil gp_m_ceil

#define gp_sinf(x) ((REAL)gp_m_sin(x))
#define gp_cosf(x) ((REAL)gp_m_cos(x))
#define gp_sqrtf(x) ((REAL)gp_m_sqrt(x))
#define gp_floor(x) ((REAL)gp_m_floor(x))
#define gp_ceil(x) ((REAL)gp_m_ceil(x))
#define gp_fabs(x) ((x) < 0 ? -(x) : (x))

typedef const void *GpGraphicsArg;

#endif
