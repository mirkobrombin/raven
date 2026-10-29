// compile: hull_quad hs_5_0
// compile: domain_quad ds_5_0
// compile: hull_triangle hs_5_0
// compile: domain_triangle ds_5_0
// error: hull_quad hs_4_0 hull and domain shaders need shader model 5

cbuffer Tessellation : register(b0)
{
	float4x4 view_projection;
	float edge_factor;
	float inside_factor;
};

struct ControlPoint
{
	float3 position : POSITION;
	float2 uv : TEXCOORD0;
};

struct QuadConstants
{
	float edges[4] : SV_TessFactor;
	float inside[2] : SV_InsideTessFactor;
	float3 centre : CENTRE;
};

QuadConstants quad_constants(InputPatch<ControlPoint, 4> patch, uint id : SV_PrimitiveID)
{
	QuadConstants constants;
	float3 centre = 0;
	for (int i = 0; i < 4; ++i) {
		constants.edges[i] = edge_factor + (id & 1);
		centre += patch[i].position;
	}
	constants.inside[0] = inside_factor;
	constants.inside[1] = inside_factor * 0.5;
	constants.centre = centre / 4;
	return constants;
}

[domain("quad")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("quad_constants")]
[maxtessfactor(32.0)]
ControlPoint hull_quad(InputPatch<ControlPoint, 4> patch, uint point : SV_OutputControlPointID)
{
	ControlPoint result = patch[point];
	result.uv = result.uv * 2;
	return result;
}

struct Vertex
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

[domain("quad")]
Vertex domain_quad(QuadConstants constants, float2 at : SV_DomainLocation, const OutputPatch<ControlPoint, 4> patch)
{
	float3 top = lerp(patch[0].position, patch[1].position, at.x);
	float3 bottom = lerp(patch[3].position, patch[2].position, at.x);
	float3 position = lerp(top, bottom, at.y) + constants.centre * constants.edges[1];
	Vertex vertex;
	vertex.position = mul(float4(position, 1), view_projection);
	vertex.uv = lerp(lerp(patch[0].uv, patch[1].uv, at.x), lerp(patch[3].uv, patch[2].uv, at.x), at.y);
	return vertex;
}

struct TriangleConstants
{
	float edges[3] : SV_TessFactor;
	float inside : SV_InsideTessFactor;
};

TriangleConstants triangle_constants(InputPatch<ControlPoint, 3> patch, const OutputPatch<ControlPoint, 3> outputs)
{
	TriangleConstants constants;
	constants.edges[0] = length(outputs[1].position - outputs[2].position) * edge_factor;
	constants.edges[1] = length(outputs[2].position - outputs[0].position) * edge_factor;
	constants.edges[2] = length(patch[0].position - patch[1].position) * edge_factor;
	constants.inside = inside_factor;
	return constants;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(3)]
[patchconstantfunc("triangle_constants")]
ControlPoint hull_triangle(InputPatch<ControlPoint, 3> patch, uint point : SV_OutputControlPointID, uint id : SV_PrimitiveID)
{
	ControlPoint result;
	result.position = patch[point].position + float(id) * 0.001;
	result.uv = patch[point].uv;
	return result;
}

[domain("tri")]
Vertex domain_triangle(TriangleConstants constants, float3 at : SV_DomainLocation, const OutputPatch<ControlPoint, 3> patch)
{
	Vertex vertex;
	float3 position = patch[0].position * at.x + patch[1].position * at.y + patch[2].position * at.z;
	vertex.position = mul(float4(position * constants.inside, 1), view_projection);
	vertex.uv = patch[0].uv * at.x + patch[1].uv * at.y + patch[2].uv * at.z;
	return vertex;
}
