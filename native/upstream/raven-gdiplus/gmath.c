/* SPDX-License-Identifier: MIT */
#include "gdip.h"
#include "gmath.h"

// The floating point functions the drawing code uses, so the library needs no C runtime. Series are cut where a
// float result stops changing, which is all the API returns.

double gp_m_sqrt(double value)
{
#if defined(__x86_64__)
	__asm__("sqrtsd %1, %0" : "=x"(value) : "x"(value));
#else
	__asm__("fsqrt" : "+t"(value));
#endif
	return value;
}

double gp_m_floor(double value)
{
	if (value >= 4.5e15 || value <= -4.5e15 || value != value)
		return value;
	double whole = (double)(long long)value;
	return whole > value ? whole - 1.0 : whole;
}

double gp_m_ceil(double value)
{
	return -gp_m_floor(-value);
}

double gp_m_fmod(double value, double divisor)
{
	double quotient = value / divisor;
	double whole = quotient < 0 ? gp_m_ceil(quotient) : gp_m_floor(quotient);
	return value - whole * divisor;
}

static double sin_series(double x)
{
	double s = x * x;
	return x * (1.0 + s * (-1.0 / 6 + s * (1.0 / 120 + s * (-1.0 / 5040 + s * (1.0 / 362880 + s * (-1.0 / 39916800 + s / 6227020800.0))))));
}

static double cos_series(double x)
{
	double s = x * x;
	return 1.0 + s * (-1.0 / 2 + s * (1.0 / 24 + s * (-1.0 / 720 + s * (1.0 / 40320 + s * (-1.0 / 3628800 + s / 479001600.0)))));
}

double gp_m_sin(double value)
{
	double turn = gp_m_floor(value / (GP_PI / 2.0) + 0.5);
	double rest = value - turn * (GP_PI / 2.0);
	switch ((long long)turn & 3) {
	case 0: return sin_series(rest);
	case 1: return cos_series(rest);
	case 2: return -sin_series(rest);
	default: return -cos_series(rest);
	}
}

double gp_m_cos(double value)
{
	double turn = gp_m_floor(value / (GP_PI / 2.0) + 0.5);
	double rest = value - turn * (GP_PI / 2.0);
	switch ((long long)turn & 3) {
	case 0: return cos_series(rest);
	case 1: return -sin_series(rest);
	case 2: return -cos_series(rest);
	default: return sin_series(rest);
	}
}

double gp_m_tan(double value)
{
	return gp_m_sin(value) / gp_m_cos(value);
}

double gp_m_atan(double value)
{
	if (value < 0.0)
		return -gp_m_atan(-value);
	if (value > 1.0)
		return GP_PI / 2.0 - gp_m_atan(1.0 / value);
	// Two half-angle steps bring the argument under tan(pi/16), where the series converges fast.
	int halvings = 0;
	while (value > 0.2) {
		value = value / (1.0 + gp_m_sqrt(1.0 + value * value));
		halvings++;
	}
	double s = value * value;
	double result = value * (1.0 + s * (-1.0 / 3 + s * (1.0 / 5 + s * (-1.0 / 7 + s * (1.0 / 9 + s * (-1.0 / 11 + s * (1.0 / 13 - s / 15)))))));
	return result * (double)(1 << halvings);
}

double gp_m_atan2(double y, double x)
{
	if (x > 0.0)
		return gp_m_atan(y / x);
	if (x < 0.0)
		return y >= 0.0 ? gp_m_atan(y / x) + GP_PI : gp_m_atan(y / x) - GP_PI;
	if (y == 0.0)
		return 0.0;
	return y > 0.0 ? GP_PI / 2.0 : -GP_PI / 2.0;
}

static double log_natural(double value)
{
	int exponent = 0;
	while (value > 1.5) {
		value /= 2.0;
		exponent++;
	}
	while (value < 0.75) {
		value *= 2.0;
		exponent--;
	}
	double t = (value - 1.0) / (value + 1.0), s = t * t;
	double series = 2.0 * t * (1.0 + s * (1.0 / 3 + s * (1.0 / 5 + s * (1.0 / 7 + s * (1.0 / 9 + s * (1.0 / 11 + s / 13))))));
	return series + exponent * 0.69314718055994530942;
}

static double exp_natural(double value)
{
	double whole = gp_m_floor(value / 0.69314718055994530942 + 0.5);
	double rest = value - whole * 0.69314718055994530942;
	double series = 1.0 + rest * (1.0 + rest / 2 * (1.0 + rest / 3 * (1.0 + rest / 4 * (1.0 + rest / 5 * (1.0 + rest / 6 * (1.0 + rest / 7 * (1.0 + rest / 8)))))));
	long long n = (long long)whole;
	for (; n > 0; n--)
		series *= 2.0;
	for (; n < 0; n++)
		series /= 2.0;
	return series;
}

double gp_m_pow(double base, double exponent)
{
	if (exponent == 0.0)
		return 1.0;
	if (base <= 0.0)
		return 0.0;
	return exp_natural(exponent * log_natural(base));
}
