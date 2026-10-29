// C++ exceptions as GCC emits them for Windows: the throw raises one exception, and every frame with locals to destroy on the way to the
// catch raises another to resume the unwind. A program throws often, on the main thread and on others, without the nesting the
// handlers run at piling up. Returns 0 when every exception was caught and every local destroyed.

#include <windows.h>

#include <stdexcept>
#include <string>

static int destroyed;

struct Guard {
	~Guard() { ++destroyed; }
};

__attribute__((noinline)) static void inner(int value)
{
	Guard guard;
	std::string text(40, 'x');
	if (value >= 0)
		throw std::runtime_error(text);
}

__attribute__((noinline)) static void middle(int value)
{
	Guard guard;
	inner(value);
}

__attribute__((noinline)) static int outer(int value)
{
	Guard guard;
	try {
		middle(value);
	} catch (const std::exception &) {
		return 1;
	}
	return 0;
}

static DWORD WINAPI worker(void *)
{
	int caught = 0;
	for (int index = 0; index < 200; ++index)
		caught += outer(index);
	return DWORD(caught);
}

int main()
{
	int caught = 0;
	for (int index = 0; index < 200; ++index)
		caught += outer(index);
	HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
	DWORD other = 0;
	if (!thread || WaitForSingleObject(thread, INFINITE) || !GetExitCodeThread(thread, &other))
		return 2;
	CloseHandle(thread);
	return caught == 200 && other == 200 && destroyed == 1200 ? 0 : 1;
}
