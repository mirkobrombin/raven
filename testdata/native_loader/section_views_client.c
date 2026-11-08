#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Every writable view of a file shares the file's pages: a byte written through one view, or through a view of another section of the
// same file, is there at once in the others and in the file, without unmapping first.
int raven_entry(void)
{
	char path[MAX_PATH];
	DWORD count=0;
	char byte=0;
	if (!GetTempPathA(MAX_PATH,path)) return 1;
	lstrcatA(path,"raven-section-views.bin");
	HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_FLAG_DELETE_ON_CLOSE,NULL);
	if (file==INVALID_HANDLE_VALUE) return 2;
	HANDLE section=CreateFileMappingA(file,NULL,PAGE_READWRITE,0,0x20000,NULL);
	HANDLE other=CreateFileMappingA(file,NULL,PAGE_READWRITE,0,0x20000,NULL);
	if (!section || !other) return 3;
	if (GetFileSize(file,NULL)!=0x20000) return 4;
	char *first=MapViewOfFile(section,FILE_MAP_WRITE,0,0,0x20000);
	char *second=MapViewOfFile(section,FILE_MAP_WRITE,0,0x10000,0x10000);
	char *third=MapViewOfFile(other,FILE_MAP_WRITE,0,0,0x20000);
	const char *reader=MapViewOfFile(other,FILE_MAP_READ,0,0,0x20000);
	if (!first || !second || !third || !reader) return 5;
	first[0x10005]=42;
	if (second[5]!=42 || third[0x10005]!=42 || reader[0x10005]!=42) return 6;
	second[7]=9;
	if (first[0x10007]!=9 || third[0x10007]!=9) return 7;
	third[100]=77;
	if (first[100]!=77) return 8;
	if (SetFilePointer(file,100,NULL,FILE_BEGIN)!=100 || !ReadFile(file,&byte,1,&count,NULL) || count!=1 || byte!=77) return 9;
	return 0;
}
