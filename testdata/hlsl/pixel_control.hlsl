// compile: main ps_4_0
// compile: main ps_5_0
// compile: blur ps_4_0

static const float weights[5] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};

Texture2D source;
SamplerState point_sampler;
float2 texel;
int mode;
uint count;

float shade(float value, int kind)
{
	if (kind == 0)
		return value;
	if (kind == 1)
		return value * value;
	for (int i = 0; i < 3; ++i) {
		if (value > 0.5)
			return 1 - value;
		value *= 2;
	}
	return value;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float total = 0;
	uint n = 0;
	for (uint i = 0; i < count; i++) {
		if (i % 2 == 1)
			continue;
		if (i > 16)
			break;
		total += float(i) / 3.0;
		n++;
	}
	int k = 0;
	while (k < mode) {
		total += 0.5;
		k += 1;
	}
	do {
		total *= 0.5;
	} while (total > 4);
	switch (mode) {
	case 0:
		total = 1;
		break;
	case 1:
	case 2:
		total += 2;
		break;
	default:
		total = -total;
		break;
	}
	float4 color = float4(shade(total, mode), uv, n);
	color.x = mode > 2 ? color.y : color.z;
	return color;
}

float4 blur(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float4 sum = source.Sample(point_sampler, uv) * weights[0];
	for (int i = 1; i < 5; ++i) {
		sum += source.Sample(point_sampler, uv + float2(texel.x * i, 0)) * weights[i];
		sum += source.Sample(point_sampler, uv - float2(texel.x * i, 0)) * weights[i];
	}
	return sum;
}
