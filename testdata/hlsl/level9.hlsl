// compile: main ps_4_0_level_9_1
// compile: main ps_4_0_level_9_3
// compile: transform vs_4_0_level_9_1
// error: main ps_4_0_level_9_9 invalid target
// error: main ps_5_0x invalid target
// error: main gs_4_0_level_9_3 invalid target

float4x4 world_view_projection;
float4 tint;
Texture2D image;
SamplerState linear_sampler;

float4 transform(float4 position : POSITION) : SV_Position
{
	return mul(position, world_view_projection);
}

float4 main(float2 uv : TEXCOORD) : SV_Target
{
	return image.Sample(linear_sampler, uv) * tint;
}
