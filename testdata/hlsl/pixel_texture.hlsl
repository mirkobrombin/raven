// compile: main ps_4_0
// compile: main ps_5_0
// compile: alpha_test ps_4_0
// compile: depth ps_5_0

Texture2D diffuse : register(t0);
Texture2D detail;
SamplerState linear_sampler : register(s0);

float4 tint;
float alpha_reference;

struct Input
{
	float4 position : SV_Position;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	float light : TEXCOORD1;
};

float4 main(Input input) : SV_Target
{
	float4 color = diffuse.Sample(linear_sampler, input.uv) * tint;
	float3 extra = detail.SampleLevel(linear_sampler, input.uv * 4, 0, int2(1, -1)).rgb;
	color.rgb = lerp(color.rgb, color.rgb * extra, 0.5) * input.light;
	return color;
}

float4 alpha_test(Input input) : SV_Target
{
	float4 color = diffuse.Sample(linear_sampler, input.uv);
	clip(color.a - alpha_reference);
	if (color.r > 0.9)
		discard;
	return color;
}

struct Targets
{
	float4 color : SV_Target0;
	float4 normal : SV_Target1;
	float depth : SV_Depth;
};

Targets depth(Input input)
{
	Targets output;
	uint width, height, levels;
	diffuse.GetDimensions(0, width, height, levels);
	output.color = diffuse.Load(int3(input.position.xy, 0)) + float(width + height + levels) * 0.0001;
	output.normal = float4(normalize(input.normal) * 0.5 + 0.5, 1);
	output.depth = input.position.z * 0.5;
	return output;
}
