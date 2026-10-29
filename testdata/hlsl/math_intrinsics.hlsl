// compile: main ps_4_0
// compile: main ps_5_0
// compile: integers ps_5_0

float4 a;
float4 b;
int4 c;
uint4 d;
float3x3 rotation;

float4 main(float4 position : SV_Position, float4 v : TEXCOORD0) : SV_Target
{
	float4 r = 0;
	r += abs(v) + sqrt(abs(a)) + rsqrt(abs(b) + 1);
	r += pow(abs(v), 2.2) + exp(v) + exp2(v) + log(abs(v) + 1) + log2(abs(v) + 1) + log10(abs(v) + 1);
	r += sin(v) + cos(v) + tan(v) + asin(saturate(v)) + acos(saturate(v)) + atan(v) + atan2(v, a);
	r += sinh(v) + cosh(v) + tanh(v);
	r += floor(v) + ceil(v) + round(v) + trunc(v) + frac(v) + fmod(v, 3) + sign(v);
	r += step(0.5, v) + smoothstep(0, 1, v) + lerp(a, b, v) + clamp(v, 0, 1) + min(a, b) + max(a, b);
	r += degrees(v) + radians(v) + ddx(v) + ddy(v) + fwidth(v);
	r.xyz += cross(a.xyz, b.xyz) + reflect(a.xyz, normalize(b.xyz)) + refract(a.xyz, normalize(b.xyz), 0.9);
	r.xyz += mul(rotation, v.xyz) + mul(v.xyz, transpose(rotation));
	r.x += length(v) + distance(a, b) + dot(a, b) + determinant(rotation);
	r.y += any(v > 1) ? 1 : 0;
	r.z += all(v < 1) ? 1 : 0;
	r.w += isnan(v.x) || isinf(v.y) || !isfinite(v.z) ? 1 : 0;
	float s, co;
	sincos(v.x, s, co);
	float whole;
	r.x += s + co + modf(v.y, whole) + whole;
	r += asfloat(asuint(v) ^ 1) + float4(c) + float4(d) + ldexp(v, 2) + mad(a, b, v);
	return r;
}

uint4 integers(float4 position : SV_Position, nointerpolation int4 v : TEXCOORD0) : SV_Target
{
	int4 x = v * c + c / (v | 1) - c % 3;
	uint4 y = d / 7 + d % 5 + (d << 2) + (d >> 1) + (uint4(v) & d) + (uint4(v) | d) + (uint4(v) ^ d);
	y += countbits(d) + reversebits(d) + firstbitlow(d) + firstbithigh(d) + uint4(firstbithigh(x));
	y += uint4(abs(x)) + uint4(min(x, 3)) + max(y, 2) + uint4(clamp(x, -2, 2));
	y += f32tof16(float4(v)) + asuint(f16tof32(d));
	return y + uint4(x) + uint4(~x) + uint4(-x);
}
