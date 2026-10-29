// run: main ps_2_0 16
// expect: 4.0 40.0 400.0 5.0 8.0 80.0 800.0 6.0 12.0 120.0 1200.0 7.0 16.0 160.0 1600.0 8.0

sampler2D row : register(s2);

float4 main(float4 t : TEXCOORD0) : COLOR
{
	// Each pixel reads its own texel: u is 0.125, 0.375, 0.625 and 0.875.
	float2 uv = float2(t.x * 0.5 + 0.5, 0.5);
	float4 r = tex2D(row, uv) * 2 + tex2Dproj(row, float4(uv * 2, 0, 2)) + tex2Dbias(row, float4(uv, 0, 0));
	r.w += uv.x * 4 + 0.5;
	return r;
}
