// run: main cs_5_0 12
// expect: 30 4 9 55 2 7 26 1.0 6.0 3.0 -3.0 11

RWStructuredBuffer<uint> results : register(u0);

static const uint table[6] = {10, 20, 30, 40, 50, 60};

struct Item
{
	uint key;
	float weight;
};

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint zero = id.x;
	results[0] = table[zero + 2];
	uint local[8];
	for (uint i = zero; i < 8; ++i)
		local[i] = i * i;
	results[1] = local[zero + 2];
	results[2] = local[3];
	uint sum = 0;
	for (uint j = zero; j <= 5; ++j)
		sum += local[j];
	results[3] = sum;
	Item items[3];
	items[0].key = 2;
	items[1].key = 7;
	items[2].key = 9;
	results[4] = items[zero].key;
	results[5] = items[zero + 1].key;
	float3x3 m = {1, 2, 3, 4, 5, 6, 7, 8, 10};
	float3 v = float3(zero + 1, 2, 3);
	results[6] = uint(dot(mul(m, v), float3(1, 0, 0)) + 12);
	results[7] = asuint(mul(v, m).x / 30);
	results[8] = asuint(transpose(m)[2].x * 2 + 0);
	results[9] = asuint(m[zero][2]);
	results[10] = asuint(determinant(m));
	float2 pair = float2(zero + 4, 7);
	results[11] = uint(pair[zero + 1]) + uint(pair[zero]);
}
