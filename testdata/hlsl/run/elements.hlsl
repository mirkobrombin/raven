// run: main cs_5_0 8
// expect: 30 31 32 33 7 6 5 4

struct Row
{
	uint values[4];
};

RWStructuredBuffer<Row> results : register(u0);

groupshared uint blocks[2][4];

[numthreads(4, 1, 1)]
void main(uint index : SV_GroupIndex)
{
	// Both indices are known only at run time.
	blocks[index & 1][index] = index + 30;
	blocks[(index + 1) & 1][index] = 7 - index;
	GroupMemoryBarrierWithGroupSync();
	results[0].values[index] = blocks[index & 1][index];
	results[1].values[index] = blocks[(index + 1) & 1][index];
}
