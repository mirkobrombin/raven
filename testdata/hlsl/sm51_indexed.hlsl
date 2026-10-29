// compile: main ps_5_1
// error: unsized ps_5_0 an array of resources without a size needs shader model 5.1
// error: fixed ps_5_0 an array of resources needs a constant index before shader model 5.1
// error: twice ps_5_1 an array of resources takes one dynamic index
// listed only
// contains: dcl_resource_texture2d (float,float,float,float) t1[0:*], space=2
// contains: dcl_resource_texture2d (float,float,float,float) t0[8:11], space=0
// contains: dcl_sampler s0[1:2], space=0
// contains: dcl_uav_typed_buffer (uint,uint,uint,uint) u0[0:2], space=3
// contains: {nonuniform}

struct Material { float4 albedo; uint texture; };
ConstantBuffer<Material> material : register(b0, space1);
Texture2D textures[] : register(t0, space2);
Texture2D<float> masks[4] : register(t8);
SamplerState samplers[2] : register(s1);
RWBuffer<uint> outputs[3] : register(u0, space3);
Texture2D grid[2][2] : register(t12);

float4 main(float2 uv : TEXCOORD, nointerpolation uint index : INDEX) : SV_Target
{
	outputs[index % 3][0] = index;
	float m = masks[index & 3].Sample(samplers[index & 1], uv);
	return textures[NonUniformResourceIndex(material.texture + index)].Sample(samplers[0], uv) * material.albedo * m;
}

float4 fixed(float2 uv : TEXCOORD, nointerpolation uint index : INDEX) : SV_Target
{
	return masks[index & 3].Sample(samplers[0], uv);
}

float4 twice(float2 uv : TEXCOORD, nointerpolation uint index : INDEX) : SV_Target
{
	return grid[index][index].Sample(samplers[0], uv);
}

float4 unsized(float2 uv : TEXCOORD) : SV_Target
{
	return textures[1].Sample(samplers[0], uv);
}
