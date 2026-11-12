// C++ exceptions as the Microsoft compiler emits them, against the system msvcrt.dll: a catch by reference, a catch by value through the
// copy constructor, a rethrow caught in the same function and in the caller, catch(...), a new exception thrown from a catch block, and
// the destructors of the locals the exceptions leave, and many rethrows in a row. Built for both widths; the entry returns the number of the first check that failed.

// The table every type descriptor points to, which a program links statically as the Microsoft libraries do.
extern "C" const void *const type_info_table[1] __asm__("??_7type_info@@6B@") = {nullptr};

static int guards, destroyed;

struct Failure {
	int value;
	explicit Failure(int value) : value(value) {}
	Failure(const Failure &other) : value(other.value + 100) {}
	~Failure() { ++destroyed; }
};

struct Guard {
	~Guard() { ++guards; }
};

__declspec(noinline) void thrower(int value)
{
	Guard guard;
	if (value)
		throw Failure(value);
}

__declspec(noinline) int by_reference()
{
	try {
		thrower(7);
	} catch (Failure &failure) {
		return failure.value;
	}
	return -1;
}

__declspec(noinline) int by_value()
{
	try {
		thrower(3);
	} catch (int) {
		return -1;
	} catch (Failure failure) {
		return failure.value;
	}
	return -2;
}

__declspec(noinline) int rethrown()
{
	try {
		try {
			thrower(1);
		} catch (Failure &) {
			throw;
		}
	} catch (Failure &failure) {
		return failure.value;
	}
	return -1;
}

__declspec(noinline) int anything()
{
	try {
		throw 42;
	} catch (...) {
		return 5;
	}
	return -1;
}

__declspec(noinline) void passes_on()
{
	Guard guard;
	try {
		thrower(9);
	} catch (Failure &) {
		throw;
	}
}

__declspec(noinline) int caller_catches()
{
	try {
		passes_on();
	} catch (Failure &failure) {
		return failure.value;
	}
	return -1;
}

__declspec(noinline) int replaced()
{
	try {
		try {
			thrower(2);
		} catch (Failure &) {
			throw 17;
		}
	} catch (int value) {
		return value;
	}
	return -1;
}

extern "C" int raven_entry()
{
	if (by_reference() != 7)
		return 1;
	if (by_value() != 103)
		return 2;
	if (rethrown() != 1)
		return 3;
	if (anything() != 5)
		return 4;
	if (caller_catches() != 9)
		return 5;
	if (replaced() != 17)
		return 6;
	// One guard in each call that threw, and the one of passes_on, which the rethrow left.
	if (guards != 6)
		return 7;
	if (destroyed < 3)
		return 8;
	// A rethrow caught outside the catch it left gives back what the exception took, as the .NET runtime rethrows by the thousand.
	for (int round = 0; round < 1000; ++round)
		if (rethrown() != 1 || caller_catches() != 9)
			return 9;
	return 0;
}
