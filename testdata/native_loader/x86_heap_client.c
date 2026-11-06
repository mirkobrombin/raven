#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Two million pairs fit in the test's limit only when they do not stop in the runtime. */
static DWORD WINAPI churn(void *failures)
{
	HANDLE heap=GetProcessHeap();
	for (int round=0; round<1000000; ++round) {
		DWORD *block=(DWORD *)HeapAlloc(heap,0,16+round%200);
		if (!block) {
			InterlockedIncrement((LONG *)failures);
			return 1;
		}
		block[1]=(DWORD)round;
		if (block[1]!=(DWORD)round) InterlockedIncrement((LONG *)failures);
		HeapFree(heap,0,block);
	}
	return 0;
}

/* Each worker keeps blocks alive and checks their contents before it frees, grows or hands them to another worker, which frees them
 * later: blocks move between threads and between the guest-side lists and the runtime's. */
#define POOL 64
static void *volatile handed[256];

static DWORD next_random(DWORD *state)
{
	*state=*state*1103515245u+12345u;
	return *state>>8;
}

static void fill(DWORD *block,DWORD words,DWORD tag)
{
	for (DWORD index=0; index<words; ++index) block[index]=tag^index;
}

static int intact(const DWORD *block,DWORD words,DWORD tag)
{
	for (DWORD index=0; index<words; ++index)
		if (block[index]!=(tag^index)) return 0;
	return 1;
}

static DWORD WINAPI exchange(void *parameter)
{
	HANDLE heap=GetProcessHeap();
	DWORD state=(DWORD)(INT_PTR)parameter*7919+1;
	DWORD *blocks[POOL]={0},words[POOL],tags[POOL];
	for (int round=0; round<300000; ++round) {
		const int slot=next_random(&state)%POOL;
		if (!blocks[slot]) {
			const DWORD count=1+next_random(&state)%300,zero=next_random(&state)&1;
			DWORD *block=(DWORD *)HeapAlloc(heap,zero?HEAP_ZERO_MEMORY:0,count*4);
			if (!block) return 1;
			for (DWORD index=0; zero && index<count; ++index)
				if (block[index]) return 2;
			blocks[slot]=block;
			words[slot]=count;
			tags[slot]=next_random(&state);
			fill(block,count,tags[slot]);
			continue;
		}
		if (!intact(blocks[slot],words[slot],tags[slot])) return 3;
		const DWORD action=next_random(&state)%8;
		if (action==0) {
			const DWORD count=1+next_random(&state)%600;
			DWORD *block=(DWORD *)HeapReAlloc(heap,0,blocks[slot],count*4);
			if (!block || !intact(block,count<words[slot]?count:words[slot],tags[slot])) return 4;
			blocks[slot]=block;
			words[slot]=count;
			tags[slot]=next_random(&state);
			fill(block,count,tags[slot]);
			continue;
		}
		if (action==1) {
			void *previous=InterlockedExchangePointer(&handed[next_random(&state)%256],blocks[slot]);
			if (previous) HeapFree(heap,0,previous);
		} else
			HeapFree(heap,0,blocks[slot]);
		blocks[slot]=0;
	}
	for (int slot=0; slot<POOL; ++slot)
		if (blocks[slot]) HeapFree(heap,0,blocks[slot]);
	return 0;
}

int raven_entry(void)
{
	HANDLE heap=GetProcessHeap();
	if (!heap || heap!=GetProcessHeap()) return 1;
	BYTE *first=(BYTE *)HeapAlloc(heap,HEAP_ZERO_MEMORY,100);
	BYTE *second=(BYTE *)HeapAlloc(heap,0,100);
	if (!first || !second || first==second) return 2;
	for (int index=0; index<100; ++index)
		if (first[index]) return 3;
	for (int index=0; index<100; ++index) first[index]=(BYTE)index;
	if (HeapSize(heap,0,first)<100) return 4;
	first=(BYTE *)HeapReAlloc(heap,0,first,5000);
	if (!first || HeapSize(heap,0,first)<5000) return 5;
	for (int index=0; index<100; ++index)
		if (first[index]!=index) return 6;
	if (HeapReAlloc(heap,HEAP_REALLOC_IN_PLACE_ONLY,first,4000)!=first) return 30;
	if (HeapReAlloc(heap,HEAP_REALLOC_IN_PLACE_ONLY,first,1<<20) || GetLastError()!=ERROR_NOT_ENOUGH_MEMORY) return 31;
	if (!HeapFree(heap,0,second)) return 7;
	BYTE *again=(BYTE *)HeapAlloc(heap,HEAP_ZERO_MEMORY,100);
	if (again!=second) return 8;
	for (int index=0; index<100; ++index)
		if (again[index]) return 9;
	BYTE *large=(BYTE *)HeapAlloc(heap,0,3<<20);
	if (!large) return 10;
	large[0]=1;
	large[(3<<20)-1]=2;
	if (!HeapFree(heap,0,large) || !HeapFree(heap,0,first) || !HeapFree(heap,0,again)) return 11;
	HeapFree(heap,0,NULL);
	HGLOBAL global=GlobalAlloc(GMEM_FIXED,64);
	if (!global || !HeapFree(heap,0,global)) return 12;
	HANDLE runnable=HeapCreate(HEAP_CREATE_ENABLE_EXECUTE,0,0);
	BYTE *code=(BYTE *)HeapAlloc(runnable,0,32);
	if (!code) return 13;
	code[0]=0xc3;
	((void (*)(void))code)();
	if (!HeapFree(runnable,0,code)) return 14;
	static BYTE *blocks[64];
	for (int index=0; index<64; ++index) {
		int size=16*(index+1);
		if (!(blocks[index]=(BYTE *)HeapAlloc(heap,HEAP_ZERO_MEMORY,size))) return 15;
		for (int at=0; at<size; ++at) {
			if (blocks[index][at]) return 16;
			blocks[index][at]=(BYTE)(index+1);
		}
	}
	for (int index=0; index<64; ++index) {
		if (HeapSize(heap,0,blocks[index])<(SIZE_T)(16*(index+1))) return 17;
		for (int at=0; at<16*(index+1); ++at)
			if (blocks[index][at]!=(BYTE)(index+1)) return 18;
	}
	for (int index=0; index<64; ++index)
		if (!HeapFree(heap,0,blocks[index])) return 19;
	HANDLE threads[2];
	LONG failures=0;
	for (int index=0; index<2; ++index)
		threads[index]=CreateThread(NULL,0,churn,&failures,0,NULL);
	if (!threads[0] || !threads[1]) return 20;
	WaitForMultipleObjects(2,threads,TRUE,INFINITE);
	if (failures) return 21;
	HANDLE workers[4];
	for (int index=0; index<4; ++index)
		if (!(workers[index]=CreateThread(NULL,0,exchange,(void *)(INT_PTR)index,0,NULL))) return 22;
	WaitForMultipleObjects(4,workers,TRUE,INFINITE);
	for (int index=0; index<4; ++index) {
		DWORD code=0;
		GetExitCodeThread(workers[index],&code);
		if (code) return 22+(int)code;
	}
	return 0;
}
