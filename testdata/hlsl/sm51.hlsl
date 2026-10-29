// compile: main ps_5_1
// error: main ps_5_0 register spaces need shader model 5.1
// contains: dcl_constantBuffer cb1[1:1][2], immediateIndexed, space=2
// contains: dcl_constantBuffer cb0[0:0][1], immediateIndexed, space=0
// contains: dcl_resource_texture2d (float,float,float,float) t0[3:3], space=1
// contains: dcl_uav_structured u0[2:2], 4, space=0
// contains: mul r1.xyzw, r0.xyzw, cb1[1][0].xyzw

struct Light { float4 color; float3 direction; float power; };
ConstantBuffer<Light> light : register(b1, space2);
cbuffer Frame : register(b0) { float4 tint; };
Texture2D image : register(t3, space1);
SamplerState linear_sampler : register(s0, space1);
RWStructuredBuffer<uint> counts : register(u2);

float4 main(float2 uv : TEXCOORD) : SV_Target
{
	counts[0] = 1;
	return image.Sample(linear_sampler, uv) * light.color * light.power + tint;
}
