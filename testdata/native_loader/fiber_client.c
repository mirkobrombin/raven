#include <windows.h>
#include <string.h>

// Fibers of a 64-bit program, served by the guest RTL component (the C runtime is loaded here through strlen too): a thread becomes a fiber, a new
// fiber gets its parameter through GetFiberData, the two switch back and forth with their stacks and saved registers kept, and a
// deleted fiber's thread is a fiber still until it converts back. Returns 0, or the number of the check that failed.
static void *main_fiber, *worker;
static volatile int rounds;

static void WINAPI work(void *parameter)
{
	if (GetFiberData() != parameter || GetCurrentFiber() != worker)
		ExitProcess(20);
	for (;;) {
		volatile double kept = 1.5 * (rounds + 1);
		++rounds;
		SwitchToFiber(main_fiber);
		if (kept != 1.5 * rounds)
			ExitProcess(21);
	}
}

int raven_entry(void)
{
	static char parameter[] = "fiber parameter";
	if (strlen(parameter) != 15)
		return 1;
	if (IsThreadAFiber())
		return 2;
	main_fiber = ConvertThreadToFiber(NULL);
	if (!main_fiber || !IsThreadAFiber() || GetCurrentFiber() != main_fiber)
		return 3;
	worker = CreateFiberEx(0, 256 * 1024, 0, work, parameter);
	if (!worker)
		return 4;
	for (int index = 0; index < 3; ++index) {
		const int before = rounds;
		SwitchToFiber(worker);
		if (rounds != before + 1 || GetCurrentFiber() != main_fiber)
			return 5 + index;
	}
	DeleteFiber(worker);
	if (!ConvertFiberToThread() || IsThreadAFiber())
		return 9;
	return 0;
}
