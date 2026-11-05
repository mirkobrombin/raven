/* Several waiters on Direct3D 12 fences: threads wait for different values of one fence through events and through a blocking
 * SetEventOnCompletion, the queue holds its signals behind a wait on a second fence that the CPU releases, and one event waits on
 * both fences together, for all of them and for any. Writes "OK" or the step that failed to C:\fences12.txt. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>

#define WAITERS 4

static void write_text(const char *text)
{
	HANDLE f = CreateFileW(L"C:\\fences12.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD n;
	WriteFile(f, text, lstrlenA(text), &n, NULL);
	CloseHandle(f);
}

static void fail(const char *step)
{
	char text[96] = "failed ";
	lstrcatA(text, step);
	write_text(text);
	ExitProcess(1);
}

#define CHECK(step, call) do { if (FAILED(call)) fail(step); } while (0)

static ID3D12Fence *fence;
static HANDLE events[WAITERS];
static volatile LONG woken[WAITERS], blocked_done;

/* Waiter i sleeps on its own event, which the fence sets when it reaches i + 1. */
static DWORD WINAPI wait_event(void *parameter)
{
	const int i = (int)(INT_PTR)parameter;
	if (WaitForSingleObject(events[i], 30000) == WAIT_OBJECT_0)
		InterlockedExchange(&woken[i], 1);
	return 0;
}

/* Without an event, SetEventOnCompletion blocks the calling thread until the fence reaches the value. */
static DWORD WINAPI wait_blocking(void *parameter)
{
	(void)parameter;
	if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, WAITERS, NULL)))
		InterlockedExchange(&blocked_done, 1);
	return 0;
}

void __stdcall entry(void)
{
	ID3D12Device *device = NULL;
	CHECK("device", D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device));
	D3D12_COMMAND_QUEUE_DESC desc = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
	ID3D12CommandQueue *queue = NULL;
	CHECK("queue", ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue, (void **)&queue));
	ID3D12Fence *gate = NULL;
	CHECK("fence", ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
	CHECK("gate", ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gate));

	HANDLE threads[WAITERS + 1];
	for (int i = 0; i < WAITERS; ++i) {
		events[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
		CHECK("completion", ID3D12Fence_SetEventOnCompletion(fence, (UINT64)i + 1, events[i]));
		threads[i] = CreateThread(NULL, 0, wait_event, (void *)(INT_PTR)i, 0, NULL);
	}
	threads[WAITERS] = CreateThread(NULL, 0, wait_blocking, NULL, 0, NULL);

	/* The queue signals every value only after the gate opens; until then nobody may wake. */
	CHECK("queue wait", ID3D12CommandQueue_Wait(queue, gate, 1));
	for (UINT64 value = 1; value <= WAITERS; ++value)
		CHECK("queue signal", ID3D12CommandQueue_Signal(queue, fence, value));
	Sleep(200);
	if (ID3D12Fence_GetCompletedValue(fence) != 0 || blocked_done)
		fail("signalled before the gate opened");
	for (int i = 0; i < WAITERS; ++i)
		if (woken[i])
			fail("woken before the gate opened");

	/* One event for both fences: "any" is met by the gate alone, "all" only when the fence has reached its last value too. */
	ID3D12Device1 *device1 = NULL;
	HANDLE any = CreateEventW(NULL, FALSE, FALSE, NULL), all = CreateEventW(NULL, FALSE, FALSE, NULL);
	ID3D12Fence *pair[2] = {fence, gate};
	const UINT64 any_values[2] = {1000, 1}, all_values[2] = {WAITERS, 1};
	const int multiple = SUCCEEDED(ID3D12Device_QueryInterface(device, &IID_ID3D12Device1, (void **)&device1));
	if (multiple) {
		CHECK("any", ID3D12Device1_SetEventOnMultipleFenceCompletion(device1, pair, any_values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, any));
		CHECK("all", ID3D12Device1_SetEventOnMultipleFenceCompletion(device1, pair, all_values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, all));
	}

	CHECK("open gate", ID3D12Fence_Signal(gate, 1));
	if (WaitForMultipleObjects(WAITERS + 1, threads, TRUE, 30000) != WAIT_OBJECT_0)
		fail("waiters did not finish");
	for (int i = 0; i < WAITERS; ++i)
		if (!woken[i])
			fail("an event waiter was not woken");
	if (!blocked_done)
		fail("the blocking waiter did not return");
	if (ID3D12Fence_GetCompletedValue(fence) != WAITERS || ID3D12Fence_GetCompletedValue(gate) != 1)
		fail("completed values");
	if (multiple && (WaitForSingleObject(any, 10000) != WAIT_OBJECT_0 || WaitForSingleObject(all, 10000) != WAIT_OBJECT_0))
		fail("multiple fence wait");

	/* A value already reached sets its event at once. */
	HANDLE done = CreateEventW(NULL, FALSE, FALSE, NULL);
	CHECK("reached", ID3D12Fence_SetEventOnCompletion(fence, 1, done));
	if (WaitForSingleObject(done, 5000) != WAIT_OBJECT_0)
		fail("reached value");
	write_text(multiple ? "OK multiple=yes" : "OK multiple=no");
	ExitProcess(0);
}
