// compile: main vs_4_0
// compile: main vs_5_0
// compile: skinned vs_5_0

cbuffer Camera : register(b0)
{
	float4x4 world;
	float4x4 view_projection;
	float3 light_direction;
	float time;
};

cbuffer Bones
{
	float4x3 bones[32];
};

struct Input
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	uint4 indices : BLENDINDICES;
	float4 weights : BLENDWEIGHT;
};

struct Output
{
	float4 position : SV_Position;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	float light : TEXCOORD1;
};

Output main(Input input)
{
	Output output;
	float4 world_position = mul(float4(input.position, 1.0), world);
	output.position = mul(world_position, view_projection);
	output.normal = normalize(mul(input.normal, (float3x3)world));
	output.uv = input.uv + float2(sin(time), cos(time)) * 0.01;
	output.light = saturate(dot(output.normal, -light_direction));
	return output;
}

Output skinned(Input input)
{
	float3 position = 0;
	float3 normal = 0;
	for (int i = 0; i < 4; ++i) {
		const float4x3 bone = bones[input.indices[i]];
		position += mul(float4(input.position, 1), bone) * input.weights[i];
		normal += mul(input.normal, (float3x3)bone) * input.weights[i];
	}
	Output output;
	output.position = mul(mul(float4(position, 1), world), view_projection);
	output.normal = normalize(normal);
	output.uv = input.uv;
	output.light = max(0, dot(output.normal, -light_direction));
	return output;
}
