// run: main cs_5_0 10
// expect: 20 5 21 7 12 100 3 4 15 30

RWStructuredBuffer<uint> results : register(u0);

uint first_over(uint limit, uint start)
{
	for (uint i = start; i < 100; ++i) {
		if (i * i > limit)
			return i;
	}
	return 0;
}

void split(uint value, out uint high, inout uint low)
{
	high = value / 10;
	low += value % 10;
}

uint classify(int value)
{
	switch (value) {
	case 0:
		return 10;
	case 1:
	case 2:
		return 12;
	default:
		break;
	}
	return 3;
}

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint zero = id.x;
	uint sum = 0;
	for (uint i = zero; i < 10; ++i) {
		if (i % 2)
			continue;
		sum += i;
	}
	results[0] = sum;
	uint n = 0;
	while (true) {
		if (n >= 5 + zero)
			break;
		++n;
	}
	results[1] = n;
	results[2] = first_over(400 + zero, 1);
	uint high = 0, low = 2;
	split(75 + zero, high, low);
	results[3] = high;
	results[4] = classify(int(zero) + 2);
	uint k = zero;
	do {
		k += 10;
		if (k < 50)
			continue;
		k *= 2;
	} while (k < 90);
	results[5] = k;
	results[6] = classify(int(zero) + 7);
	results[7] = low - 3 + (low == 7 ? 0 : 100);
	uint total = 0;
	for (uint row = zero; row < 3; ++row)
		for (uint column = zero; column < 3; ++column) {
			if (column > row)
				break;
			total += row + column;
		}
	results[8] = total + 3;
	results[9] = classify(int(zero)) * 3;
}
