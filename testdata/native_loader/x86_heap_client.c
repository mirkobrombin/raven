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
	return 0;
}
