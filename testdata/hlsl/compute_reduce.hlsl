// compile: main cs_5_0
// compile: histogram cs_5_0
// compile: copy cs_4_0

struct Particle
{
	float3 position;
	float3 velocity;
	float life;
};

RWStructuredBuffer<Particle> particles : register(u0);
RWByteAddressBuffer counters : register(u1);
RWTexture2D<float4> image : register(u2);
StructuredBuffer<float> inputs : register(t0);
RWStructuredBuffer<uint> bins : register(u3);

cbuffer Step
{
	float delta;
	uint total;
};

groupshared float partial[64];

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex, uint3 group : SV_GroupID)
{
	Particle particle = particles[id.x];
	particle.velocity.y -= 9.81 * delta;
	particle.position += particle.velocity * delta;
	particle.life = max(0, particle.life - delta);
	particles[id.x] = particle;
	partial[index] = id.x < total ? inputs[id.x] : 0;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 32; stride > 0; stride >>= 1) {
		if (index < stride)
			partial[index] += partial[index + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0) {
		uint previous;
		counters.InterlockedAdd(0, 1, previous);
		counters.Store(4 + group.x * 4, asuint(partial[0]));
		image[uint2(group.x, 0)] = float4(partial[0], previous, 0, 1);
	}
}

[numthreads(8, 8, 1)]
void histogram(uint3 id : SV_DispatchThreadID)
{
	float4 color = image[id.xy];
	uint bucket = uint(saturate(dot(color.rgb, float3(0.299, 0.587, 0.114))) * 15);
	InterlockedAdd(bins[bucket], 1);
}

RWStructuredBuffer<float> output_values : register(u4);

[numthreads(32, 1, 1)]
void copy(uint3 id : SV_DispatchThreadID)
{
	output_values[id.x] = inputs[id.x] * 2;
}
