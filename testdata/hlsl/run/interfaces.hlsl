// run: main cs_5_0 4
// expect: 8 9 22 19

RWStructuredBuffer<uint> results : register(u0);

interface IOperation
{
	uint run(uint value);
};

interface INamed
{
	uint id();
};

class Doubler : IOperation
{
	uint run(uint value) { return value * 2; }
};

class Adder : IOperation
{
	uint amount;
	uint run(uint value) { return value + amount; }
};

class Squarer : IOperation
{
	uint run(uint value) { return value * value; }
};

// A class deriving from another takes its interfaces and may replace its methods.
class Tripler : Doubler
{
	uint run(uint value) { return value * 3; }
};

class Labelled : INamed, IOperation
{
	uint id() { return 7; }
	uint run(uint value) { return value + 1; }
};

uint apply(IOperation operation, uint value) { return operation.run(value); }

[numthreads(1, 1, 1)]
void main()
{
	Doubler doubler;
	Adder adder;
	adder.amount = 5;
	Squarer squarer;
	Tripler tripler;
	Labelled labelled;
	IOperation chosen = adder;
	results[0] = apply(doubler, 4);
	results[1] = apply(adder, 4);
	results[2] = apply(squarer, 4) + chosen.run(1);
	results[3] = apply(tripler, 4) + labelled.id();
}
