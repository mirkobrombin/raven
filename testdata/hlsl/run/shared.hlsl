// run: main cs_5_0 4
// expect: 2016 64 63 64

RWStructuredBuffer<uint> results : register(u0);

groupshared uint values[64];
groupshared uint largest;

[numthreads(64, 1, 1)]
void main(uint index : SV_GroupIndex)
{
	values[index] = index;
	if (index == 0)
		largest = 0;
	GroupMemoryBarrierWithGroupSync();
	InterlockedMax(largest, index);
	for (uint stride = 32; stride > 0; stride >>= 1) {
		if (index < stride)
			values[index] += values[index + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0) {
		results[0] = values[0];
		results[2] = largest;
	}
	if (index == 1)
		results[1] = 0;
	AllMemoryBarrierWithGroupSync();
	InterlockedAdd(results[1], 1);
	if (index == 63) {
		uint original;
		InterlockedExchange(results[3], 64, original);
	}
}
