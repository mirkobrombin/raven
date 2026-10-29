// compile: transform vs_2_0
// compile: transform vs_3_0
// compile: skinned vs_2_0
// compile: skinned vs_3_0
// compile: displaced vs_3_0
// compile: lit ps_2_0
// compile: lit ps_2_b
// compile: lit ps_3_0
// compile: screen ps_3_0
// compile: blur ps_2_0
// compile: blur ps_3_0
// compile: early ps_2_0
// compile: early ps_3_0
// compile: chosen ps_2_0
// compile: chosen ps_3_0
// compile: counted ps_3_0
// error: bitwise ps_2_0 bitwise operations need shader model 4
// error: displaced vs_2_0 vertex shaders sample textures from vs_3_0
// error: screen ps_2_0 needs ps_3_0
// error: endless ps_2_0 loops that cannot be unrolled need shader model 3
// error: lit gs_4_0 entry point
// error: lit ps_1_1 is not a shader model

float4x4 world_view_projection;
float4x3 world;
float3 light_direction;
float4 material = float4(1, 0.5, 0.25, 1);
float4x3 bones[24];
int bone_count;
bool fogged;
float fog_start, fog_end;
float4 offsets[4];
float steps;

sampler2D diffuse : register(s0);
sampler2D normals;
sampler2D height;

struct Plain {
	float4 position : POSITION;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
	float fog : FOG;
};

Plain transform(float4 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0)
{
	Plain out_vertex;
	out_vertex.position = mul(position, world_view_projection);
	const float3 n = normalize(mul(normal, (float3x3)world));
	out_vertex.color = material * saturate(dot(n, -light_direction));
	out_vertex.uv = uv;
	out_vertex.fog = fogged ? saturate((fog_end - out_vertex.position.z) / (fog_end - fog_start)) : 1;
	return out_vertex;
}

Plain skinned(float4 position : POSITION, float4 weights : BLENDWEIGHT, float4 indices : BLENDINDICES, float2 uv : TEXCOORD0)
{
	float3 place = 0;
	for (int i = 0; i < 4; ++i)
		place += mul(position, bones[int(indices[i])]) * weights[i];
	Plain out_vertex;
	out_vertex.position = mul(float4(place, 1), world_view_projection);
	out_vertex.uv = uv;
	out_vertex.color = float4(weights.xyz, 1);
	out_vertex.fog = 1;
	return out_vertex;
}

float4 displaced(float4 position : POSITION, float2 uv : TEXCOORD0) : POSITION
{
	position.y += tex2Dlod(height, float4(uv, 0, 0)).x;
	return mul(position, world_view_projection);
}

float4 lit(float2 uv : TEXCOORD0, float4 color : COLOR0) : COLOR
{
	const float3 n = tex2D(normals, uv).xyz * 2 - 1;
	const float shade = saturate(dot(n, -light_direction));
	return tex2D(diffuse, uv) * color * shade;
}

float4 screen(float2 position : VPOS, float face : VFACE) : COLOR
{
	return float4(frac(position / 8), face > 0 ? 1 : 0, 1);
}

float4 blur(float2 uv : TEXCOORD0) : COLOR
{
	float4 sum = 0;
	for (int i = 0; i < 4; ++i)
		sum += tex2D(diffuse, uv + offsets[i].xy) * offsets[i].z;
	return sum;
}

float4 early(float2 uv : TEXCOORD0) : COLOR
{
	const float4 color = tex2D(diffuse, uv);
	if (color.a < 0.5)
		return float4(1, 0, 0, 1);
	if (color.r > 0.9)
		return color * 0.5;
	return color;
}

float4 chosen(float2 uv : TEXCOORD0) : COLOR
{
	float4 result = 0;
	switch (int(uv.x * 4)) {
	case 0:
		result = float4(1, 0, 0, 1);
		break;
	case 1:
	case 2:
		result = float4(0, 1, 0, 1);
		break;
	default:
		result = tex2D(diffuse, uv);
		break;
	}
	return result;
}

float4 counted(float2 uv : TEXCOORD0) : COLOR
{
	float4 sum = 0;
	for (int i = 0; i < 16; ++i) {
		if (i >= int(steps))
			break;
		if (i == 2)
			continue;
		sum += tex2D(diffuse, uv + i * 0.01);
	}
	return sum / steps;
}

float4 bitwise(float2 uv : TEXCOORD0) : COLOR
{
	const int bits = int(uv.x * 255);
	return float4((bits & 15) / 15.0, (bits >> 4) / 15.0, 0, 1);
}

float4 endless(float2 uv : TEXCOORD0) : COLOR
{
	float4 sum = 0;
	for (int i = 0; i < int(steps); ++i)
		sum += uv.xyxy;
	return sum;
}
