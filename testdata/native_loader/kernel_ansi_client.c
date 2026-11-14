#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The ANSI entry points of the file and environment APIs agree with their wide twins.
static int same(const char *left,const char *right)
{
	while(*left && *left==*right) {++left;++right;}
	return *left==*right;
}

static unsigned length(const char *text)
{
	unsigned count=0;
	while(text[count]) ++count;
	return count;
}

int raven_entry(void)
{
	static const char data[]="ansi payload";
	char directory[MAX_PATH],path[MAX_PATH],copy[MAX_PATH],full[MAX_PATH],*part=NULL,buffer[64];
	WIN32_FIND_DATAA found;
	HANDLE file,search;
	DWORD written=0,read=0,count;
	unsigned long long value;
	count=GetCurrentDirectoryA(MAX_PATH,directory);
	if (!count || count>=MAX_PATH || length(directory)!=count || directory[1]!=':') return 1;
	if (!SetEnvironmentVariableA("RAVEN_ANSI_PROBE","probe value") || GetEnvironmentVariableA("RAVEN_ANSI_PROBE",buffer,sizeof buffer)!=11 || !same(buffer,"probe value")) return 2;
	if (GetEnvironmentVariableA("RAVEN_ANSI_ABSENT",buffer,sizeof buffer) || GetLastError()!=ERROR_ENVVAR_NOT_FOUND) return 3;
	if (!CreateDirectoryA("ansi-dir",NULL) || (GetFileAttributesA("ansi-dir")&FILE_ATTRIBUTE_DIRECTORY)==0) return 4;
	file=CreateFileA("ansi-dir\\one.txt",GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
	if (file==INVALID_HANDLE_VALUE || !WriteFile(file,data,sizeof data-1,&written,NULL) || written!=sizeof data-1) return 5;
	CloseHandle(file);
	if (!CopyFileA("ansi-dir\\one.txt","ansi-dir\\two.txt",FALSE) || !MoveFileA("ansi-dir\\two.txt","ansi-dir\\three.txt")) return 6;
	if (GetFileAttributesA("ansi-dir\\two.txt")!=INVALID_FILE_ATTRIBUTES || GetFileAttributesA("ansi-dir\\three.txt")==INVALID_FILE_ATTRIBUTES) return 7;
	if (!MoveFileExA("ansi-dir\\three.txt","ansi-dir\\four.txt",0) || !MoveFileWithProgressA("ansi-dir\\four.txt","ansi-dir\\three.txt",NULL,NULL,0) ||
		GetFileAttributesA("ansi-dir\\four.txt")!=INVALID_FILE_ATTRIBUTES) return 90;
	/* A console program such as 7-Zip's switches the file functions to the OEM code page: 0x82 is e acute there, and in ANSI 0xe9 is. */
	SetFileApisToOEM();
	if (AreFileApisANSI()) return 91;
	file=CreateFileA("ansi-dir\\\x82.txt",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
	if (file==INVALID_HANDLE_VALUE) return 92;
	CloseHandle(file);
	if (GetFileAttributesW(L"ansi-dir\\\x00e9.txt")==INVALID_FILE_ATTRIBUTES) return 93;
	SetFileApisToANSI();
	if (!AreFileApisANSI() || !DeleteFileA("ansi-dir\\\xe9.txt")) return 94;
	file=CreateFileA("ansi-dir\\three.txt",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
	if (file==INVALID_HANDLE_VALUE || !ReadFile(file,buffer,sizeof buffer,&read,NULL) || read!=sizeof data-1) return 8;
	CloseHandle(file);
	for (value=0;value<read;++value) if (buffer[value]!=data[value]) return 9;
	count=GetFullPathNameA("ansi-dir\\one.txt",MAX_PATH,full,&part);
	if (!count || count>=MAX_PATH || !part || !same(part,"one.txt") || full[1]!=':') return 10;
	search=FindFirstFileA("ansi-dir\\t*.txt",&found);
	if (search==INVALID_HANDLE_VALUE || !same(found.cFileName,"three.txt") || found.nFileSizeLow!=sizeof data-1 || FindNextFileA(search,&found)) return 11;
	FindClose(search);
	if (!DeleteFileA("ansi-dir\\one.txt") || !DeleteFileA("ansi-dir\\three.txt") || !RemoveDirectoryA("ansi-dir")) return 12;
	if (GetFileAttributesA("ansi-dir")!=INVALID_FILE_ATTRIBUTES) return 13;
	count=GetModuleFileNameA(NULL,path,MAX_PATH);
	if (!count || count>=MAX_PATH || length(path)!=count || path[length(path)-1]!='e') return 14;
	/* A name looked up in the activation context, as the .NET runtime does before it loads a library: the ANSI form answers as the
	   wide one does. */
	ACTCTX_SECTION_KEYED_DATA wide_data={sizeof wide_data},narrow_data={sizeof narrow_data};
	SetLastError(0);
	const BOOL wide_found=FindActCtxSectionStringW(0,NULL,ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION,L"comctl32.dll",&wide_data);
	const DWORD wide_error=GetLastError();
	SetLastError(0);
	if (FindActCtxSectionStringA(0,NULL,ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION,"comctl32.dll",&narrow_data)!=wide_found ||
		GetLastError()!=wide_error || narrow_data.ulDataFormatVersion!=wide_data.ulDataFormatVersion) return 95;
	(void)copy;
	return 0;
}
