// compile: - fx_2_0
// contains: float4x4 world_view_projection
// contains: string UIName = <object id
// contains: float4 tint = {1.00000000e+00, 5.00000000e-01, 2.50000000e-01, 1.00000000e+00};
// contains: int counts[3] = { {1, 2, 3} };
// contains: bool fogged = true;
// contains: textureCUBE environment
// contains: sampler2D diffuse =
// contains: MinFilter = LINEAR /* 2 */;
// contains: AddressU = CLAMP /* 3 */;
// contains: BorderColor = -8388353;
// contains: parameter "diffuse_map"
// contains: SrcBlend = SRCALPHA /* 5 */;
// contains: DestBlend = INVSRCALPHA /* 6 */;
// contains: CullMode = NONE /* 1 */;
// contains: ColorWriteEnable = 7;
// contains: AlphaBlendEnable = true;
// contains: FogStart = 2.50000000e-01;
// contains: parameter "fog_color"
// contains: parameter "shared_vs"
// contains: vs_2_0
// contains: ps_2_0
// contains: texld
// contains: string description = <object id
// contains: VertexShader shared_vs = <object id
// contains: pass 
// contains: ZEnable = 0;

float4x4 world_view_projection : WORLDVIEWPROJECTION <string UIName = "World view projection";>;
float4 tint = {1, 0.5, 0.25, 1};
int counts[3] = {1, 2, 3};
bool fogged = true;
shared float4 fog_color = {0.5, 0.5, 0.5, 1};
string description = "Effect9 test";
texture diffuse_map;
textureCUBE environment;
sampler2D diffuse = sampler_state
{
	Texture = <diffuse_map>;
	MinFilter = Linear;
	MagFilter = Linear;
	AddressU = Clamp;
	BorderColor = 0xff8000ff;
};

float4 VS(float4 p : POSITION, float2 uv : TEXCOORD0, out float2 uv_out : TEXCOORD0) : POSITION
{
	uv_out = uv;
	return mul(p, world_view_projection);
}

float4 PS(float2 uv : TEXCOORD0) : COLOR
{
	return tex2D(diffuse, uv) * tint;
}

VertexShader shared_vs = compile vs_2_0 VS();

technique Blend <string Purpose = "test";>
{
	pass P0
	{
		AlphaBlendEnable = true;
		SrcBlend = SrcAlpha;
		DestBlend = InvSrcAlpha;
		CullMode = None;
		ColorWriteEnable = RED | GREEN | BLUE;
		FogStart = 0.25;
		FogColor = <fog_color>;
		VertexShader = <shared_vs>;
		PixelShader = compile ps_2_0 PS();
	}
}

technique Old
{
	pass
	{
		ZEnable = false;
		VertexShader = compile vs_1_1 VS();
		PixelShader = NULL;
	}
}
