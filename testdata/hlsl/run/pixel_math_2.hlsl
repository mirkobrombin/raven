// run: main ps_2_0 16
// expect: -1.750128 2.0 -1193.5 -875.317763 0.630179 6.928203 -92.5 28.138889 0.752376 13.416408 1008.5 83.288489 0.047529 21.16601 2109.5 1187.002611

float4 main(float4 t : TEXCOORD0) : COLOR
{
	// x is 0, 2, 4 and 6 across the four pixels, a is -1.25, -0.25, 0.75 and 1.75.
	float x = t.x * 4 + 3;
	float a = x * 0.5 - 1.25;
	float3 v = float3(a, 1, 2);
	float4 r;
	r.x = sin(a) + cos(a * 2);
	r.y = sqrt(x + 1) + pow(x + 1, 1.5);
	r.z = floor(a) * 100 + frac(a) * 10 + ceil(a) + round(a) * 1000;
	r.w = saturate(a) + lerp(1, 5, 0.25) + normalize(v).x * 10 + length(v) + abs(a) * 100 + int(a) * 1000;
	return r;
}
