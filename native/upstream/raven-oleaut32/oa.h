/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
#ifndef RAVEN_OLEAUT32_OA_H
#define RAVEN_OLEAUT32_OA_H

#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <olectl.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* The header turns these into assignments; the DLL exports them as functions. */
#undef VarUI4FromUI4
#undef VarI4FromI4
#undef VarUI8FromUI8
#undef VarI8FromI8

#ifndef DEC_SIGN
#define DEC_SIGN(d) ((d)->sign)
#define DEC_SCALE(d) ((d)->scale)
#define DEC_HI32(d) ((d)->Hi32)
#define DEC_LO64(d) ((d)->Lo64)
#endif

/* Same type conversions exist too: a general caller need not tell them apart. */
#define OA_SELF(Name, C) HRESULT WINAPI Var##Name##From##Name(C in, C *out);
OA_SELF(I1, CHAR)
OA_SELF(UI1, BYTE)
OA_SELF(I2, SHORT)
OA_SELF(UI2, USHORT)
OA_SELF(I4, LONG)
OA_SELF(UI4, ULONG)
OA_SELF(I8, LONG64)
OA_SELF(UI8, ULONG64)
OA_SELF(R4, FLOAT)
OA_SELF(R8, DOUBLE)
OA_SELF(Cy, CY)
OA_SELF(Bool, VARIANT_BOOL)
OA_SELF(Date, DATE)
HRESULT WINAPI VarDecFromDec(DECIMAL *in, DECIMAL *out);

/* The element size a variant type has in an array or a variant; 0 when it has none. */
ULONG oa_vartype_size(VARTYPE type);

void oa_font_startup(void);
void oa_typelib_startup(void);

#endif
