// run: main cs_5_0 55
// expect: 0.4794255495071411 0.8775825500488281 0.5463024973869324 0.5235987901687622 1.8234765529632568 1.1071487665176392 -0.244978666305542 -3.017237663269043 -1.6951513290405273 1.4142135381698608 0.7071067690849304 11.313708305358887
// expect: 1.6487212181091309 4.0 0.6931471824645996 3.0 2.0 0.5 -2.0 -1.0 2.0 -1.0 1.0 0.875
// expect: 0.15625 0.0 -1 0.25 2.076655864715576 0.24253562092781067 6.0 2.0 1.0 -0.25 0.5 0.5210952758789062
// expect: 1.1276259422302246 0.46211716532707214 28.647890090942383 2.5 0.5 1.0 0.75 0.5 8 252641280 15 4
// expect: 4294967295 2 7 4294967293 2 16384 1065353216

RWStructuredBuffer<uint> results : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint zero = id.x;
	float x = float(zero) + 0.5;
	float y = float(zero) - 0.25;
	float z = float(zero) + 2.0;
	uint u = zero + 0xf0f0;
	uint n = zero;
	results[0] = asuint(sin(x));
	results[1] = asuint(cos(x));
	results[2] = asuint(tan(x));
	results[3] = asuint(asin(x));
	results[4] = asuint(acos(y));
	results[5] = asuint(atan(z));
	results[6] = asuint(atan(y));
	results[7] = asuint(atan2(y, -z));
	results[8] = asuint(atan2(-z, y));
	results[9] = asuint(sqrt(z));
	results[10] = asuint(rsqrt(z));
	results[11] = asuint(pow(z, 3.5));
	results[12] = asuint(exp(x));
	results[13] = asuint(exp2(z));
	results[14] = asuint(log(z));
	results[15] = asuint(log2(z * 4));
	results[16] = asuint(log10(z * 50));
	results[17] = asuint(frac(-x * 3));
	results[18] = asuint(floor(-x * 3));
	results[19] = asuint(ceil(-x * 3));
	results[20] = asuint(round(z + 0.5));
	results[21] = asuint(trunc(-x * 3));
	results[22] = asuint(clamp(z, 0, 1));
	results[23] = asuint(lerp(x, z, 0.25));
	results[24] = asuint(smoothstep(0, z, x));
	results[25] = asuint(step(x, y));
	results[26] = asuint(sign(y));
	results[27] = asuint(abs(y));
	results[28] = asuint(length(float3(x, y, z)));
	results[29] = asuint(normalize(float2(z, x)).y);
	results[30] = asuint(dot(float3(x, y, z), float3(1, 2, 3)));
	results[31] = asuint(cross(float3(1, 0, 0), float3(0, z, 0)).z);
	results[32] = asuint(reflect(float2(x, -1), float2(0, 1)).y);
	results[33] = asuint(min(x, y));
	results[34] = asuint(max(x, y));
	results[35] = asuint(sinh(x));
	results[36] = asuint(cosh(x));
	results[37] = asuint(tanh(x));
	results[38] = asuint(degrees(x));
	results[39] = asuint(distance(float2(0, 0), float2(3 * x, 4 * x)));
	results[40] = asuint(fmod(z + x, 1));
	results[41] = asuint(saturate(z));
	results[42] = asuint(mad(x, z, y));
	results[43] = asuint(rcp(z));
	results[44] = countbits(u);
	results[45] = reversebits(u);
	results[46] = firstbithigh(u);
	results[47] = firstbitlow(u);
	results[48] = firstbithigh(n);
	results[49] = uint(firstbithigh(-8 - int(zero)));
	results[50] = uint(abs(-7 - int(zero)));
	results[51] = uint(max(-3 - int(zero), -9));
	results[52] = uint(sign(-5 - int(zero)) + 3);
	results[53] = f32tof16(z);
	results[54] = asuint(f16tof32(0x3c00 + zero));
}
