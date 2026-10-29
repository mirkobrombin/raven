// compile: main gs_4_0
// compile: main gs_5_0

cbuffer Camera
{
	float4x4 view_projection;
	float size;
};

struct Point
{
	float3 position : POSITION;
	float4 color : COLOR;
};

struct Vertex
{
	float4 position : SV_Position;
	float4 color : COLOR;
	float2 uv : TEXCOORD0;
};

[maxvertexcount(4)]
void main(point Point input[1], inout TriangleStream<Vertex> stream)
{
	const float2 corners[4] = {float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1)};
	for (int i = 0; i < 4; ++i) {
		Vertex output;
		output.position = mul(float4(input[0].position + float3(corners[i] * size, 0), 1), view_projection);
		output.color = input[0].color;
		output.uv = corners[i] * 0.5 + 0.5;
		stream.Append(output);
	}
	stream.RestartStrip();
}
