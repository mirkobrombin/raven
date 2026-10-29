// compile: - fx_5_0
// error: - fx_4_0 groups need fx_5_0
// contains: fxgroup Shaded
// contains: technique11 Outlined
// contains: int Order = 2;
// contains: GeometryShader streamed = {
// contains: ComputeShader = 
// contains: cs_5_0
// contains: BlendEnable[1] = bool(TRUE /* 1 */);
// contains: SrcBlendAlpha[1] = uint(INV_SRC_ALPHA /* 6 */);

struct Vertex { float4 position : SV_Position; };

Vertex VS(float4 position : POSITION) { Vertex v; v.position = position; return v; }

[maxvertexcount(3)]
void GS(triangle Vertex input[3], inout TriangleStream<Vertex> stream)
{
	for (int i = 0; i < 3; ++i)
		stream.Append(input[i]);
}

float4 PS(Vertex v) : SV_Target { return v.position; }

RWStructuredBuffer<uint> counters;

[numthreads(8, 1, 1)]
void CS(uint3 id : SV_DispatchThreadID) { counters[id.x] = id.x; }

GeometryShader streamed = ConstructGSWithSO(CompileShader(gs_5_0, GS()), "SV_Position.xyzw", NULL, NULL, NULL, 0);

BlendState second_target
{
	BlendEnable[1] = TRUE;
	SrcBlendAlpha[1] = INV_SRC_ALPHA;
};

fxgroup Shaded <int Order = 2;>
{
	technique11 Outlined
	{
		pass Draw
		{
			SetVertexShader(CompileShader(vs_5_0, VS()));
			SetGeometryShader(streamed);
			SetPixelShader(CompileShader(ps_5_0, PS()));
			SetBlendState(second_target, float4(1, 1, 1, 1), 0xffffffff);
		}
		pass Count
		{
			SetComputeShader(CompileShader(cs_5_0, CS()));
		}
	}
}
