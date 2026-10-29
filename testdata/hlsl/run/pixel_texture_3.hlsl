// run: main ps_3_0 16
// expect: 6.0 60.0 600.0 7.0 12.0 120.0 1200.0 8.0 18.0 180.0 1800.0 9.0 24.0 240.0 2400.0 10.0

sampler2D row;

float4 main(float4 t : TEXCOORD0) : COLOR
{
	float2 uv = float2(t.x * 0.5 + 0.5, 0.5);
	float4 r = tex2D(row, uv) * 2 + tex2Dproj(row, float4(uv * 2, 0, 2)) + tex2Dbias(row, float4(uv, 0, 0));
	r += tex2Dlod(row, float4(uv, 0, 0)) + tex2Dgrad(row, uv, float2(0, 0), float2(0, 0));
	r.w += uv.x * 4 + 0.5;
	return r;
}
