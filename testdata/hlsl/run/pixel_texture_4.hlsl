// run: main ps_4_0 16
// expect: 4.0 40.0 400.0 5.0 8.0 80.0 800.0 6.0 12.0 120.0 1200.0 7.0 16.0 160.0 1600.0 8.0

Texture2D<float4> row : register(t1);
SamplerState nearest : register(s3);

float4 main(float4 position : SV_Position, float4 t : TEXCOORD0) : SV_Target
{
	float2 uv = float2(t.x * 0.5 + 0.5, 0.5);
	float4 r = row.Sample(nearest, uv) * 2 + row.SampleLevel(nearest, uv, 0) + row.Load(int3(position.x, 0, 0));
	r.w += uv.x * 4 + 0.5;
	return r;
}
