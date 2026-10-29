// compile: - fx_4_0
// compile: - fx_4_1
// compile: - fx_5_0
// error: - ps_4_0 entry point '' not found
// contains: float4x4 world_view : WORLDVIEW
// contains: string UIName = "World view";
// contains: float Step = 5.00000000e-01;
// contains: float4 tint = { 1.00000000e+00, 5.00000000e-01, 2.50000000e-01, 1.00000000e+00 };    // Offset: 64, size 16.
// contains: bool fogged = { true };    // Offset: 84, size 4.
// contains: int counts[3] = { 1, 2, 3 };    // Offset: 96, size 36.
// contains: cbuffer Lights : register(b2)
// contains: float intensity;    // Offset: 12, size 4.
// contains: Texture2D diffuse;
// contains: Filter = uint(MIN_MAG_MIP_LINEAR /* 21 */);
// contains: AddressV = uint(WRAP /* 1 */);
// contains: = uint(ONE /* 2 */);
// contains: RenderTargetWriteMask[0] = byte(0x0f);
// contains: DepthWriteMask = uint(ZERO /* 0 */);
// contains: CullMode = uint(NONE /* 1 */);
// contains: "Effect test"
// contains: VertexShader shared_vs = {
// contains: AB_BlendFactor = float4(0, 0, 0, 0);
// contains: AB_SampleMask = uint(4294967295);
// contains: DS_StencilRef = uint(0);
// contains: RasterizerState = wire;
// contains: string Purpose = "test";

float4x4 world_view : WORLDVIEW <string UIName = "World view"; float Step = 0.5;>;
float4 tint = float4(1, 0.5, 0.25, 1);
float scale = 2;
bool fogged = true;
int counts[3] = {1, 2, 3};
cbuffer Lights : register(b2)
{
	float3 light_direction = float3(0, -1, 0);
	float intensity;
};
Texture2D diffuse;
SamplerState linear_wrap
{
	Filter = MIN_MAG_MIP_LINEAR;
	AddressU = Wrap;
	AddressV = WRAP;
};
BlendState additive
{
	BlendEnable[0] = TRUE;
	SrcBlend = ONE;
	DestBlend = ONE;
	RenderTargetWriteMask[0] = 0x0F;
};
DepthStencilState no_depth { DepthEnable = FALSE; DepthWriteMask = ZERO; };
RasterizerState wire { FillMode = WIREFRAME; CullMode = NONE; };
string label = "Effect test";

struct VSOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS(float4 position : POSITION, float2 uv : TEXCOORD0)
{
	VSOut o;
	o.position = mul(position, world_view);
	o.uv = uv;
	return o;
}

float4 PS(VSOut i, uniform bool tinted) : SV_Target
{
	float4 c = diffuse.Sample(linear_wrap, i.uv) * scale;
	return tinted ? c * tint : c;
}

VertexShader shared_vs = CompileShader(vs_4_0, VS());

technique10 Plain <string Purpose = "test";>
{
	pass P0
	{
		SetVertexShader(shared_vs);
		SetGeometryShader(NULL);
		SetPixelShader(CompileShader(ps_4_0, PS(false)));
		SetBlendState(additive, float4(0, 0, 0, 0), 0xFFFFFFFF);
		SetDepthStencilState(no_depth, 0);
		SetRasterizerState(wire);
	}
	pass P1
	{
		VertexShader = shared_vs;
		PixelShader = CompileShader(ps_4_0, PS(true));
	}
}
