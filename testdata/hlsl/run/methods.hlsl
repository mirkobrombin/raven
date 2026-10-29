// run: main cs_5_0 4
// expect: 5 15 30 1

RWStructuredBuffer<uint> results : register(u0);

struct Counter
{
	uint total;
	void add(uint value) { total += value; }
	uint twice() { return total * 2; }
	// Calls a sibling method and names the object as this.
	uint report() { return twice() + this.total; }
};

class Weighted : Counter
{
	uint weight;
	uint scaled() { return report() * weight; }
};

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	Weighted counter;
	counter.total = id.x;
	counter.weight = 2;
	counter.add(4);
	counter.add(1);
	results[0] = counter.total;
	results[1] = counter.report();
	results[2] = counter.scaled();
	Counter plain;
	plain.total = 1;
	results[3] = plain.total;
}
