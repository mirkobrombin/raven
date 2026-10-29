// compile: main ps_4_0
// compile: main ps_5_0
// compile: alpha_test ps_4_0
// compile: depth ps_5_0
// compile: shadowed ps_5_0
// compile: scaled ps_5_0
// compile: fogged ps_4_0

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

static const int light_count = 3;
static const int shadow_count = 1;
Texture2D shadows[shadow_count] : register(t4);

float shadow_of(int index, float2 uv)
{
	Texture2D map = shadows[index];
	return map.Sample(linear_sampler, uv).r;
}

// The loop runs unrolled, so the counter reaches the resource index as a constant through the call, and the
// lights without a shadow map leave the call out.
float4 shadowed(Input input) : SV_Target
{
	float4 color = 0;
	for (int i = 0; i < light_count; ++i) {
		float4 lit = tint * (input.light + i);
		if (i < shadow_count && any(lit.rgb))
			lit *= shadow_of(i, input.uv);
		color += lit;
	}
	return color;
}

float4 scaled(Input input, uniform float scale, uniform bool invert) : SV_Target
{
	float4 color = diffuse.Sample(linear_sampler, input.uv) * scale;
	return invert ? 1 - color : color;
}

struct Fog
{
	float density;
	float3 colour;
	float3 apply(float3 lit, float depth) { return lerp(colour, lit, exp(-density * depth)); }
};

cbuffer Atmosphere
{
	Fog fog;
};

float4 fogged(Input input) : SV_Target
{
	float4 color = diffuse.Sample(linear_sampler, input.uv);
	return float4(fog.apply(color.rgb, input.position.z), color.a);
}
