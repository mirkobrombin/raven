// compile: - fx_5_0
// contains: RasterizerState fill[2] = {
// contains: FillMode = uint(WIREFRAME /* 2 */);
// contains: RasterizerState = fill[mode];
// contains: uint mode = { 1 };
// contains: PixelShader = 

interface ILight
{
	float3 illuminate(float3 normal);
};

class Ambient : ILight
{
	float3 color;
	float3 illuminate(float3 normal);
};

class Hemisphere : Ambient
{
	float3 ground;
	float3 illuminate(float3 normal);
};

float3 Ambient::illuminate(float3 normal) { return color; }
float3 Hemisphere::illuminate(float3 normal) { return lerp(ground, color, normal.y * 0.5 + 0.5); }

cbuffer Lights
{
	Ambient flat_light;
	Hemisphere sky_light;
};

uint mode = 1;

RasterizerState fill[2]
{
	{
		FillMode = SOLID;
	},
	{
		FillMode = WIREFRAME;
	}
};

float4 shade(uniform ILight light, float3 normal : NORMAL) : SV_Target
{
	return float4(light.illuminate(normal), 1);
}

technique11 Lit
{
	pass Flat
	{
		SetRasterizerState(fill[mode]);
		SetPixelShader(CompileShader(ps_5_0, shade(flat_light)));
	}
	pass Sky
	{
		SetPixelShader(CompileShader(ps_4_0, shade(sky_light)));
	}
}
