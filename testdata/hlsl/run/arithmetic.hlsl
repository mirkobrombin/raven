// run: main cs_5_0 24
// expect: 7 3 10 2 1 -2 -1 2147483645 1 40 1 4 7 6 3 1 0 1
// expect: 0.75 2.5 -1.5 9.0 0.5 -0.5

RWStructuredBuffer<uint> results : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	int a = int(id.x) + 5;
	int b = int(id.x) + 2;
	uint u = id.x + 4294967290;
	results[0] = a + b;
	results[1] = a - b;
	results[2] = a * b;
	results[3] = a / b;
	results[4] = a % b;
	results[5] = -a / b;
	results[6] = -a % b;
	results[7] = u / 2;
	results[8] = u % 3;
	results[9] = a << 3;
	results[10] = a >> 2;
	results[11] = a & 6;
	results[12] = a | b;
	results[13] = (a ^ 3) + 0;
	results[14] = (a > b) + (a >= 5) + (b < 1) + (b <= 2);
	results[15] = (a == 5) && (b != 5);
	results[16] = !(a == 5);
	results[17] = u > 7;
	float x = float(id.x) + 1.5;
	float y = float(id.x) + 2;
	results[18] = asuint(x / y);
	results[19] = asuint(x + 1);
	results[20] = asuint(x - 3);
	results[21] = asuint(y * y * 2 + 1);
	results[22] = asuint(fmod(x, 1.0) + 0);
	results[23] = asuint(fmod(-x, 1.0));
}
