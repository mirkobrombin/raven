#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define CHECK(code,condition) do { if(!(condition)) return code; } while(0)

static CRITICAL_SECTION section;
static volatile LONG counter;
static HANDLE gate_event;
static DWORD tls_slot;
static SRWLOCK lock;
static CONDITION_VARIABLE ready_variable;
static volatile LONG ready_flag;
static volatile LONG apc_value;
static volatile DWORD apc_thread;
static HANDLE apc_ready;
static HANDLE mutex;
static volatile LONG order[8];
static volatile LONG order_count;

static DWORD WINAPI counting(LPVOID parameter)
{
	// Sleep(0) forces the scheduler to interleave the threads inside the critical section's neighbourhood.
	for(int i=0;i<100;++i) {
		EnterCriticalSection(&section);
		LONG seen=counter;
		Sleep(0);
		counter=seen+1;
		LeaveCriticalSection(&section);
		Sleep(0);
	}
	return (DWORD)(ULONG_PTR)parameter;
}

static DWORD WINAPI waiting(LPVOID parameter)
{
	(void)parameter;
	if(WaitForSingleObject(gate_event,5000)!=WAIT_OBJECT_0) return 1;
	return 42;
}

static DWORD WINAPI tls_reader(LPVOID parameter)
{
	(void)parameter;
	if(TlsGetValue(tls_slot)!=0) return 1;
	TlsSetValue(tls_slot,(LPVOID)(ULONG_PTR)77);
	Sleep(10);
	return TlsGetValue(tls_slot)==(LPVOID)(ULONG_PTR)77 ? 0 : 2;
}

static SLIST_HEADER list;
static SLIST_ENTRY list_entries[16];

static DWORD WINAPI list_worker(LPVOID parameter)
{
	// Each thread only pushes entries it owns, the ones it pushed or popped last, so no entry is ever in the list twice.
	PSLIST_ENTRY owned[4];
	for(int i=0;i<4;++i) owned[i]=&list_entries[(ULONG_PTR)parameter*4+i];
	for(int round=0;round<500;++round) {
		for(int i=0;i<4;++i) InterlockedPushEntrySList(&list,owned[i]);
		if(!(round&15)) Sleep(0);
		for(int i=0;i<4;++i) if(!(owned[i]=InterlockedPopEntrySList(&list))) return 1;
	}
	return 0;
}

static DWORD WINAPI producer(LPVOID parameter)
{
	(void)parameter;
	Sleep(20);
	AcquireSRWLockExclusive(&lock);
	ready_flag=1;
	ReleaseSRWLockExclusive(&lock);
	WakeAllConditionVariable(&ready_variable);
	return 0;
}

static DWORD WINAPI ordered(LPVOID parameter)
{
	Sleep((DWORD)(ULONG_PTR)parameter);
	order[order_count++]=(LONG)(ULONG_PTR)parameter;
	return 0;
}

static VOID CALLBACK apc(ULONG_PTR parameter)
{
	apc_value=(LONG)parameter;
	apc_thread=GetCurrentThreadId();
}

static DWORD WINAPI alertable_worker(LPVOID parameter)
{
	const DWORD mode=(DWORD)(ULONG_PTR)parameter;
	SetEvent(apc_ready);
	const DWORD result=mode ? SleepEx(2000,TRUE) : WaitForSingleObjectEx(gate_event,2000,TRUE);
	return result==WAIT_IO_COMPLETION && apc_value==(LONG)(400+mode) && apc_thread==GetCurrentThreadId() ? 0 : 1;
}

static DWORD WINAPI mutex_owner(LPVOID parameter)
{
	(void)parameter;
	if(WaitForSingleObject(mutex,0)!=WAIT_TIMEOUT) return 1;
	if(ReleaseMutex(mutex)) return 2;
	return GetLastError()==ERROR_NOT_OWNER ? 0 : 3;
}

static int critical_options(void)
{
	ULONG_PTR peb;
#ifdef __x86_64__
	__asm__ volatile("movq %%gs:0x60,%0" : "=r"(peb));
	const BYTE *version = (const BYTE *)(peb + 0x118);
#else
	__asm__ volatile("movl %%fs:0x30,%0" : "=r"(peb));
	const BYTE *version = (const BYTE *)(peb + 0xa4);
#endif
	if (*(const DWORD *)version != 10 || *(const DWORD *)(version + 4) ||
		*(const WORD *)(version + 8) != 19045 || *(const WORD *)(version + 10) ||
		*(const DWORD *)(version + 12) != VER_PLATFORM_WIN32_NT) return 85;
	CRITICAL_SECTION option;
	const DWORD flags[] = {1, 0x00ffffff, 0x01000001, 0x10000000};
	for (unsigned index = 0; index < sizeof(flags) / sizeof(flags[0]); ++index) {
		for (unsigned at = 0; at < sizeof(option); ++at) ((BYTE *)&option)[at] = 0xa5;
		if (!InitializeCriticalSectionEx(&option, 0, flags[index])) return 80;
		if ((flags[index] & 0x01000000) && option.DebugInfo && (ULONG_PTR)option.DebugInfo != (ULONG_PTR)-1) return 81;
		EnterCriticalSection(&option);
		if (!TryEnterCriticalSection(&option)) return 82;
		LeaveCriticalSection(&option);
		LeaveCriticalSection(&option);
		DeleteCriticalSection(&option);
	}
	for (unsigned at = 0; at < sizeof(option); ++at) ((BYTE *)&option)[at] = 0xa5;
	if (InitializeCriticalSectionEx(&option, 0, 0x20000001) || GetLastError() != ERROR_INVALID_PARAMETER) return 83;
	for (unsigned at = 0; at < sizeof(option); ++at) if (((BYTE *)&option)[at] != 0xa5) return 84;
	return 0;
}

int raven_entry(void)
{
	const int options = critical_options();
	if (options) return options;
	ULONG_PTR stack_low=0,stack_high=0;
	GetCurrentThreadStackLimits(&stack_low,&stack_high);
	CHECK(78,stack_low && stack_low<(ULONG_PTR)&stack_low && (ULONG_PTR)&stack_low<stack_high);
	CHECK(79,InitializeCriticalSectionEx(&section,0,1));
	HANDLE workers[4];
	for(int i=0;i<4;++i) {
		workers[i]=CreateThread(0,0,counting,(LPVOID)(ULONG_PTR)(i+1),0,0);
		CHECK(1,workers[i]!=0);
	}
	CHECK(2,WaitForMultipleObjects(4,workers,TRUE,5000)==WAIT_OBJECT_0);
	CHECK(3,counter==400);
	for(int i=0;i<4;++i) {
		DWORD code=0;
		CHECK(4,GetExitCodeThread(workers[i],&code) && code==(DWORD)(i+1));
		CloseHandle(workers[i]);
	}

	gate_event=CreateEventW(0,TRUE,FALSE,0);
	HANDLE waiter=CreateThread(0,0,waiting,0,0,0);
	CHECK(5,waiter!=0);
	DWORD code=0;
	CHECK(6,WaitForSingleObject(waiter,30)==WAIT_TIMEOUT);
	CHECK(7,GetExitCodeThread(waiter,&code) && code==STILL_ACTIVE);
	SetEvent(gate_event);
	CHECK(8,WaitForSingleObject(waiter,5000)==WAIT_OBJECT_0);
	CHECK(9,GetExitCodeThread(waiter,&code) && code==42);
	CloseHandle(waiter);

	tls_slot=TlsAlloc();
	CHECK(10,tls_slot!=TLS_OUT_OF_INDEXES);
	CHECK(11,TlsSetValue(tls_slot,(LPVOID)(ULONG_PTR)5));
	HANDLE reader=CreateThread(0,0,tls_reader,0,0,0);
	CHECK(12,WaitForSingleObject(reader,5000)==WAIT_OBJECT_0);
	CHECK(13,GetExitCodeThread(reader,&code) && code==0);
	CHECK(14,TlsGetValue(tls_slot)==(LPVOID)(ULONG_PTR)5);
	CloseHandle(reader);
	TlsFree(tls_slot);

	InitializeSListHead(&list);
	CHECK(90,!InterlockedPopEntrySList(&list) && !InterlockedFlushSList(&list) && QueryDepthSList(&list)==0);
	CHECK(91,!InterlockedPushEntrySList(&list,&list_entries[0]) && InterlockedPushEntrySList(&list,&list_entries[1])==&list_entries[0]);
	CHECK(92,QueryDepthSList(&list)==2 && InterlockedPopEntrySList(&list)==&list_entries[1] && QueryDepthSList(&list)==1);
	CHECK(93,InterlockedPushEntrySList(&list,&list_entries[2])==&list_entries[0] && InterlockedFlushSList(&list)==&list_entries[2] &&
		list_entries[2].Next==&list_entries[0] && !list_entries[0].Next && QueryDepthSList(&list)==0 && !InterlockedPopEntrySList(&list));
	for(int i=0;i<4;++i) {
		workers[i]=CreateThread(0,0,list_worker,(LPVOID)(ULONG_PTR)i,0,0);
		CHECK(94,workers[i]!=0);
	}
	CHECK(95,WaitForMultipleObjects(4,workers,TRUE,10000)==WAIT_OBJECT_0);
	for(int i=0;i<4;++i) {
		CHECK(96,GetExitCodeThread(workers[i],&code) && code==0);
		CloseHandle(workers[i]);
	}
	CHECK(97,QueryDepthSList(&list)==0 && !InterlockedPopEntrySList(&list));
	// Waits already decided: a signaled manual event stays signaled, a semaphore gives one count per wait, an empty one times out.
	HANDLE ready=CreateEventW(0,TRUE,TRUE,0), counted=CreateSemaphoreW(0,3,3,0);
	for(int i=0;i<1000;++i) CHECK(98,WaitForSingleObject(ready,INFINITE)==WAIT_OBJECT_0);
	for(int i=0;i<3;++i) CHECK(99,WaitForSingleObjectEx(counted,0,FALSE)==WAIT_OBJECT_0);
	CHECK(100,WaitForSingleObject(counted,0)==WAIT_TIMEOUT && ResetEvent(ready) && WaitForSingleObject(ready,10)==WAIT_TIMEOUT);
	CloseHandle(ready);
	CloseHandle(counted);

	InitializeSRWLock(&lock);
	InitializeConditionVariable(&ready_variable);
	HANDLE make=CreateThread(0,0,producer,0,0,0);
	AcquireSRWLockExclusive(&lock);
	while(!ready_flag)
		CHECK(15,SleepConditionVariableSRW(&ready_variable,&lock,5000,0));
	ReleaseSRWLockExclusive(&lock);
	CHECK(16,ready_flag==1);
	CHECK(17,WaitForSingleObject(make,5000)==WAIT_OBJECT_0);
	CloseHandle(make);
	AcquireSRWLockExclusive(&lock);
	CHECK(18,!SleepConditionVariableSRW(&ready_variable,&lock,15,0) && GetLastError()==ERROR_TIMEOUT);
	ReleaseSRWLockExclusive(&lock);

	// Sleeping threads wake in time order, not creation order.
	HANDLE sleepers[3];
	DWORD delays[3]={600,100,300};
	for(int i=0;i<3;++i) sleepers[i]=CreateThread(0,0,ordered,(LPVOID)(ULONG_PTR)delays[i],0,0);
	CHECK(19,WaitForMultipleObjects(3,sleepers,TRUE,5000)==WAIT_OBJECT_0);
	CHECK(20,order[0]==100 && order[1]==300 && order[2]==600);
	for(int i=0;i<3;++i) CloseHandle(sleepers[i]);

	mutex=CreateMutexW(0,TRUE,0);
	CHECK(21,mutex!=0);
	HANDLE other=CreateThread(0,0,mutex_owner,0,0,0);
	CHECK(22,WaitForSingleObject(other,5000)==WAIT_OBJECT_0);
	CHECK(23,GetExitCodeThread(other,&code) && code==0);
	CHECK(24,WaitForSingleObject(mutex,0)==WAIT_OBJECT_0);
	CHECK(25,ReleaseMutex(mutex) && ReleaseMutex(mutex));
	CHECK(26,!ReleaseMutex(mutex));
	CloseHandle(other);

	HANDLE semaphore=CreateSemaphoreW(0,2,3,0);
	CHECK(27,WaitForSingleObject(semaphore,0)==WAIT_OBJECT_0 && WaitForSingleObject(semaphore,0)==WAIT_OBJECT_0);
	CHECK(28,WaitForSingleObject(semaphore,10)==WAIT_TIMEOUT);
	LONG previous=-1;
	CHECK(29,ReleaseSemaphore(semaphore,3,&previous) && previous==0);
	CHECK(30,!ReleaseSemaphore(semaphore,1,0) && GetLastError()==ERROR_TOO_MANY_POSTS);

	CHECK(31,QueueUserAPC(apc,GetCurrentThread(),123));
	CHECK(32,SleepEx(1000,TRUE)==WAIT_IO_COMPLETION && apc_value==123);

	// A named mutex is the same object for every opener.
	HANDLE first=CreateMutexW(0,FALSE,L"raven-named-mutex");
	HANDLE second=CreateMutexW(0,FALSE,L"raven-named-mutex");
	CHECK(33,first && second && GetLastError()==ERROR_ALREADY_EXISTS);
	CHECK(34,GetCurrentThreadId()!=0);
	for(DWORD mode=0;mode<2;++mode) {
		apc_ready=CreateEventW(0,FALSE,FALSE,0);
		CHECK(35,apc_ready && ResetEvent(gate_event));
		HANDLE worker=CreateThread(0,0,alertable_worker,(LPVOID)(ULONG_PTR)mode,0,0);
		CHECK(36,worker && WaitForSingleObject(apc_ready,2000)==WAIT_OBJECT_0);
		Sleep(20);
		CHECK(37,QueueUserAPC(apc,worker,400+mode));
		CHECK(38,WaitForSingleObject(worker,500)==WAIT_OBJECT_0);
		CHECK(39,GetExitCodeThread(worker,&code) && code==0);
		CloseHandle(worker);
		CloseHandle(apc_ready);
	}
	return 0;
}
