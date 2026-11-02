#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#ifdef CONVERSION_DUMP
#include <stdio.h>
#else
#include "multibyte_reference.h"
#endif

static uint64_t digest=0xcbf29ce484222325ULL;
static unsigned calls;
__declspec(dllexport) uint64_t conversion_rows[900][3];

static void add(uint32_t value)
{
	for (unsigned i=0;i<4;++i) { digest^=(BYTE)(value>>(i*8));digest*=0x100000001b3ULL; }
}

static void convert(UINT code_page,DWORD flags,const char *input,int length,int capacity)
{
	WCHAR output[520];for(unsigned i=0;i<520;++i)output[i]=0x7777;
	SetLastError(71);int result=MultiByteToWideChar(code_page,flags,input,length,output,capacity);DWORD error=GetLastError();
	add(result);add(error);uint64_t row=0xcbf29ce484222325ULL;
	for(unsigned i=0;i<520;++i) { add(output[i]);for(unsigned j=0;j<4;++j) { row^=(BYTE)((uint32_t)output[i]>>(j*8));row*=0x100000001b3ULL; } }
	conversion_rows[calls][0]=(uint32_t)result;conversion_rows[calls][1]=error;conversion_rows[calls][2]=row;
#ifdef CONVERSION_DUMP
	printf("row=%u result=%d error=%lu digest=%016llx\n",calls,result,error,(unsigned long long)row);
#endif
	++calls;
}

static void scenarios(void)
{
	char bytes[256];for(unsigned i=0;i<256;++i)bytes[i]=(char)i;
	const UINT pages[]={CP_ACP,CP_THREAD_ACP,1252};
	for(unsigned page=0;page<3;++page)
		for(DWORD flags=0;flags<16;++flags) {
			convert(pages[page],flags,bytes,256,0);
			convert(pages[page],flags,bytes,256,520);
			convert(pages[page],flags,bytes,256,1);
			convert(pages[page],flags,bytes,256,255);
		}
	const char *vectors[]={"A","\xc2\xa2","\xe4\xb8\xad","\xf0\x9f\x99\x82","a\xf0\x9f\x99\x82","\xf0\x9f\x99\x82" "a",
		"\xc0\xaf","a\xc0\xaf","\xe1\x80","\xe1\x80" "a","\xed\xa0\x80","\xe0\x80\x80","\xf4\x90\x80\x80","\xf5\x80\x80\x80","\x80",""};
	for(unsigned i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i)
		for(DWORD flags=0;flags<=8;flags+=8)
			for(int capacity=0;capacity<=8;++capacity) {
				int length=0;while(vectors[i][length])++length;
				convert(CP_UTF8,flags,vectors[i],-1,capacity);
				convert(CP_UTF8,flags,vectors[i],length,capacity);
			}
	const int lengths[]={0,-1,-2,-99,1,2,3};
	for(DWORD flags=0;flags<32;++flags) {
		convert(CP_UTF8,flags,"\xc3\xa9",2,8);
		convert(CP_UTF8,flags,"\xc3\xa9",2,0);
	}
	for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i)convert(1252,0,"ab",lengths[i],8);
	convert(1252,0,NULL,1,8);convert(1252,0,"a",1,-1);convert(CP_UTF8,1,"a",1,8);convert(1252,16,"a",1,8);
	SetLastError(71);add(MultiByteToWideChar(1252,0,"a",1,NULL,1));add(GetLastError());++calls;
	SetLastError(71);add(MultiByteToWideChar(1252,0,"a",1,(WCHAR *)(UINT_PTR)0x12345678,0));add(GetLastError());++calls;
	WCHAR same[8]={0x6261,0};SetLastError(71);add(MultiByteToWideChar(1252,0,(const char *)same,-1,same,0));add(GetLastError());++calls;
}

#ifdef CONVERSION_DUMP
int main(void)
{
	scenarios();printf("calls=%u digest=%016llx\n",calls,(unsigned long long)digest);return 0;
}
#else
int raven_entry(void)
{
	scenarios();if(calls!=conversion_calls || digest!=conversion_digest)return 1;
#ifdef CONVERSION_BOUNDARY
	WCHAR same[8]={0x6261,0};
	SetLastError(71);if(MultiByteToWideChar(1252,0,(const char *)same,-1,same,8) || GetLastError()!=ERROR_INVALID_PARAMETER || same[0]!=0x6261)return 4;
	WCHAR output=0x7777;
	if(MultiByteToWideChar(1,0,"a",1,&output,1)!=1 || output!=L'a')return 2;
	output=0x7777;
	SetLastError(71);if(MultiByteToWideChar(1252,0,"a",0x40000001,&output,1) || GetLastError()!=ERROR_CALL_NOT_IMPLEMENTED || output!=0x7777)return 3;
#endif
	return 0;
}
#endif
