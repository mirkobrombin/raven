// Libraries start after the libraries they import. order-root.dll imports order-a.dll, order-b.dll and order-f.dll in that order.
// order-b.dll imports order-a.dll, and order-f.dll forwards an export to order-a.dll without importing it: order-a.dll is found
// first, but has to start before both, which record whether it had.
#include <windows.h>

#if defined(RAVEN_ORDER_A)
static LONG started;
__declspec(dllexport) LONG RavenOrderStarted(void) { return started; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		started = 1;
	return TRUE;
}
#elif defined(RAVEN_ORDER_B)
__declspec(dllimport) LONG RavenOrderStarted(void);
static LONG saw;
__declspec(dllexport) LONG RavenOrderSaw(void) { return saw; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		saw = RavenOrderStarted();
	return TRUE;
}
#elif defined(RAVEN_ORDER_F)
// Exported through attach_order_forward.def, which also forwards RavenOrderForwarded to order-a.dll.
typedef LONG (*started_function)(void);
static LONG saw;
LONG RavenOrderForwarderSaw(void) { return saw; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		const HMODULE target = GetModuleHandleW(L"order-a.dll");
		const started_function started = target ? (started_function)GetProcAddress(target, "RavenOrderStarted") : 0;
		saw = started ? started() : 0;
	}
	return TRUE;
}
#elif defined(RAVEN_ORDER_ROOT)
__declspec(dllimport) LONG RavenOrderStarted(void);
__declspec(dllimport) LONG RavenOrderSaw(void);
__declspec(dllimport) LONG RavenOrderForwarded(void);
__declspec(dllimport) LONG RavenOrderForwarderSaw(void);
__declspec(dllexport) LONG RavenOrderCheck(void)
{
	return RavenOrderStarted() * 4 + RavenOrderSaw() * 2 + RavenOrderForwarderSaw() * RavenOrderForwarded();
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
#else
typedef LONG (*check_function)(void);

int raven_entry(void)
{
	HMODULE root = LoadLibraryW(L"order-root.dll");
	if (!root)
		return 1;
	const check_function check = (check_function)GetProcAddress(root, "RavenOrderCheck");
	if (!check)
		return 2;
	// order-a.dll started (4), had started when order-b.dll did (2), and had started when order-f.dll did (1).
	const LONG result = check();
	if (result != 7)
		return 10 + result;
	return 0;
}
#endif
