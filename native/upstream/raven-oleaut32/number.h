/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_OLEAUT32_NUMBER_H
#define RAVEN_OLEAUT32_NUMBER_H

double oa_round_even(double value);
HRESULT oa_r8_to_i8(double value, LONGLONG *out);
HRESULT oa_r8_to_u8(double value, ULONGLONG *out);
LONGLONG oa_cy_to_i8(LONGLONG scaled);
HRESULT oa_dec_to_i8(const DECIMAL *value, LONGLONG *out);
HRESULT oa_dec_to_u8(const DECIMAL *value, ULONGLONG *out);
double oa_dec_to_r8(const DECIMAL *value);
void oa_dec_from_i8(LONGLONG value, DECIMAL *out);
void oa_dec_from_u8(ULONGLONG value, DECIMAL *out);
HRESULT oa_dec_from_parts(const BYTE *digits, ULONG count, int exponent, BOOL negative, DECIMAL *out);
HRESULT oa_cy_from_r8(double value, CY *out);
HRESULT oa_cy_from_i8(LONGLONG value, CY *out);
HRESULT oa_pow(double base, double power, double *out);
ULONG oa_digits_of_r8(double value, int precision, BYTE *digits, int *exponent);

#endif
