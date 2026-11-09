#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#define NTDDI_VERSION 0x0a000006
#include <windows.h>

static DWORD WINAPI idle(LPVOID parameter)
{
	(void)parameter;
	return 0;
}

static __attribute__((aligned(16))) unsigned char buffer[0x800];

// A context laid out in a buffer starts where the processor's alignment allows, inside the length the size query reported.
static int laid_out(BOOL second)
{
	DWORD length=0;
	CONTEXT *context=NULL;
	BOOL done=second?InitializeContext2(NULL,CONTEXT_FULL,&context,&length,0):InitializeContext(NULL,CONTEXT_FULL,&context,&length);
	if (done || GetLastError()!=ERROR_INSUFFICIENT_BUFFER || length<sizeof(CONTEXT)) return 1;
	DWORD small=sizeof(CONTEXT)-1;
	if (InitializeContext(buffer,CONTEXT_FULL,&context,&small) || GetLastError()!=ERROR_INSUFFICIENT_BUFFER || small!=length) return 2;
	if (length>sizeof(buffer)-1) return 3;
	done=second?InitializeContext2(buffer+1,CONTEXT_FULL,&context,&length,0):InitializeContext(buffer+1,CONTEXT_FULL,&context,&length);
	if (!done || !context) return 4;
	if ((ULONG_PTR)context%__alignof__(CONTEXT) || (unsigned char *)context<buffer+1) return 5;
	if ((unsigned char *)(context+1)>buffer+1+length) return 6;
	if (context->ContextFlags!=CONTEXT_FULL) return 7;
	return 0;
}

// Background processing mode is entered and left by the thread itself and is never reported as a priority.
int raven_entry(void)
{
	HANDLE self=GetCurrentThread();
	if (GetThreadPriority(self)!=THREAD_PRIORITY_NORMAL) return 1;
	if (!SetThreadPriority(self,THREAD_MODE_BACKGROUND_BEGIN)) return 2;
	if (GetThreadPriority(self)!=THREAD_PRIORITY_IDLE) return 3;
	if (SetThreadPriority(self,THREAD_MODE_BACKGROUND_BEGIN) || GetLastError()!=ERROR_THREAD_MODE_ALREADY_BACKGROUND) return 4;
	if (!SetThreadPriority(self,THREAD_MODE_BACKGROUND_END)) return 5;
	if (GetThreadPriority(self)!=THREAD_PRIORITY_NORMAL) return 6;
	if (SetThreadPriority(self,THREAD_MODE_BACKGROUND_END) || GetLastError()!=ERROR_THREAD_MODE_NOT_BACKGROUND) return 7;
	if (!SetThreadPriority(self,THREAD_PRIORITY_ABOVE_NORMAL) || !SetThreadPriority(self,THREAD_MODE_BACKGROUND_BEGIN)) return 8;
	if (!SetThreadPriority(self,THREAD_MODE_BACKGROUND_END) || GetThreadPriority(self)!=THREAD_PRIORITY_ABOVE_NORMAL) return 9;
	HANDLE other=CreateThread(NULL,0,idle,NULL,CREATE_SUSPENDED,NULL);
	if (!other) return 10;
	BOOL refused=!SetThreadPriority(other,THREAD_MODE_BACKGROUND_BEGIN) && GetLastError()==ERROR_INVALID_PARAMETER;
	int reported=GetThreadPriority(other);
	ResumeThread(other);
	WaitForSingleObject(other,INFINITE);
	CloseHandle(other);
	if (!refused) return 11;
	if (reported!=THREAD_PRIORITY_NORMAL) return 12;
	int first=laid_out(FALSE);
	if (first) return 20+first;
	int second=laid_out(TRUE);
	if (second) return 30+second;
	return 0;
}
