// compile: shade ps_5_0
// compile: shade ps_4_0
// error: dynamic ps_5_0 'light' is an interface instance chosen at run time, which needs dynamic shader linkage
// error: unset ps_5_0 an interface variable needs the class instance it stands for
// error: picked ps_5_0 interface parameter 'source' is chosen at run time, which needs dynamic shader linkage
// error: wrong ps_5_0 no overload of 'lit' takes these arguments

interface ILight
{
	float3 illuminate(float3 normal);
};

class Ambient : ILight
{
	float3 color;
	float3 illuminate(float3 normal) { return color; }
};

class Directional : ILight
{
	float3 color;
	float3 direction;
	float3 illuminate(float3 normal) { return color * saturate(dot(normal, -direction)); }
};

struct Plain { float3 color; };

cbuffer Lights { Ambient ambient; Directional sun; Plain plain; };
ILight light;

float3 lit(ILight source, float3 normal) { return source.illuminate(normal); }

float4 shade(float3 normal : NORMAL) : SV_Target
{
	ILight chosen = sun;
	return float4(lit(ambient, normal) + chosen.illuminate(normal), 1);
}

float4 dynamic(float3 normal : NORMAL) : SV_Target { return float4(light.illuminate(normal), 1); }

float4 unset(float3 normal : NORMAL) : SV_Target
{
	ILight chosen;
	return float4(chosen.illuminate(normal), 1);
}

float4 wrong(float3 normal : NORMAL) : SV_Target { return float4(lit(plain, normal), 1); }

float4 picked(ILight source, float3 normal : NORMAL) : SV_Target { return float4(source.illuminate(normal), 1); }
