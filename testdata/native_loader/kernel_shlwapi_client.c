#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>

#define CHECK(code,condition) do { if(!(condition)) return code; } while(0)

static int same(const WCHAR *left,const WCHAR *right)
{
	while(*left && *left==*right) {++left;++right;}
	return *left==*right;
}

static void set(WCHAR *target,const WCHAR *text)
{
	while((*target++=*text++)) {}
}

int raven_entry(void)
{
	WCHAR path[MAX_PATH];
	set(path,L"C:\\dir\\file.txt");
	CHECK(1,PathRemoveFileSpecW(path) && same(path,L"C:\\dir"));
	CHECK(2,PathRemoveFileSpecW(path) && same(path,L"C:\\"));
	CHECK(3,!PathRemoveFileSpecW(path) && same(path,L"C:\\"));
	set(path,L"file.txt");
	CHECK(4,PathRemoveFileSpecW(path) && same(path,L""));
	set(path,L"\\\\server\\share\\dir\\x.txt");
	CHECK(5,PathRemoveFileSpecW(path) && same(path,L"\\\\server\\share\\dir"));
	set(path,L"C:\\dir\\sub\\");
	CHECK(6,PathRemoveFileSpecW(path) && same(path,L"C:\\dir\\sub"));

	CHECK(7,same(PathFindFileNameW(L"C:\\a\\b.txt"),L"b.txt"));
	CHECK(8,same(PathFindExtensionW(L"C:\\a.b\\file.tar.gz"),L".gz"));
	CHECK(9,*PathFindExtensionW(L"C:\\a.b\\file")==0);
	set(path,L"C:\\a\\b.txt");
	PathStripPathW(path);
	CHECK(10,same(path,L"b.txt"));
	set(path,L"C:\\a\\b.txt");
	PathRemoveExtensionW(path);
	CHECK(11,same(path,L"C:\\a\\b"));
	set(path,L"C:\\a\\b");
	CHECK(12,PathAddExtensionW(path,L".log") && same(path,L"C:\\a\\b.log"));
	CHECK(13,!PathAddExtensionW(path,L".txt"));

	set(path,L"C:\\a");
	CHECK(14,PathAppendW(path,L"b\\c.txt") && same(path,L"C:\\a\\b\\c.txt"));
	set(path,L"C:\\a\\");
	CHECK(15,PathAppendW(path,L"\\b") && same(path,L"C:\\a\\b"));
	WCHAR combined[MAX_PATH];
	CHECK(16,PathCombineW(combined,L"C:\\a\\b",L"..\\c") && same(combined,L"C:\\a\\c"));
	CHECK(17,PathCombineW(combined,L"C:\\a",L"D:\\x") && same(combined,L"D:\\x"));
	CHECK(18,PathCanonicalizeW(combined,L"C:\\a\\.\\b\\..\\c\\") && same(combined,L"C:\\a\\c\\"));

	CHECK(19,PathIsRelativeW(L"a\\b") && !PathIsRelativeW(L"C:\\a") && !PathIsRelativeW(L"\\\\srv\\share"));
	CHECK(20,PathGetDriveNumberW(L"d:\\x")==3 && PathGetDriveNumberW(L"x")==-1);
	CHECK(21,PathIsNetworkPathW(L"\\\\srv\\share\\x") && !PathIsNetworkPathW(L"C:\\x"));
	CHECK(22,PathMatchSpecW(L"notes.TXT",L"*.txt") && PathMatchSpecW(L"a.cpp",L"*.h;*.cpp") && !PathMatchSpecW(L"a.cpp",L"*.h;*.c"));
	CHECK(23,PathMatchSpecW(L"abc",L"a?c") && !PathMatchSpecW(L"abc",L"a?"));

	WCHAR compact[40];
	CHECK(24,PathCompactPathExW(compact,L"C:\\one\\two\\three\\four\\file.txt",20,0));
	int length=0;
	while(compact[length]) ++length;
	CHECK(25,length<20 && compact[length-1]=='t' && compact[length-8]=='f');

	CHECK(26,PathFileExistsW(L"C:\\Windows") && !PathFileExistsW(L"C:\\no\\such\\path"));
	CHECK(27,PathIsDirectoryW(L"C:\\Windows") && !PathIsDirectoryW(L"C:\\no"));

	// HLS uses shlwapi's 0..240 scale; a primary colour has full saturation at half luminance.
	WORD hue,luminance,saturation;
	ColorRGBToHLS(RGB(255,0,0),&hue,&luminance,&saturation);
	CHECK(28,hue==0 && luminance==120 && saturation==240);
	CHECK(29,ColorHLSToRGB(0,120,240)==RGB(255,0,0));
	CHECK(30,ColorHLSToRGB(80,120,240)==RGB(0,255,0));
	ColorRGBToHLS(RGB(128,128,128),&hue,&luminance,&saturation);
	CHECK(31,saturation==0 && luminance==120);

	WCHAR size[32];
	StrFormatByteSizeW(1536,size,32);
	CHECK(32,same(size,L"1.50 KB"));
	StrFormatByteSizeW(512,size,32);
	CHECK(33,same(size,L"512 bytes"));
	CHECK(34,StrCmpIW(L"Hello",L"hELLO")==0 && StrCmpW(L"a",L"b")<0);
	CHECK(35,StrToIntW(L"-42x")==-42);
	/* The arguments of a command line start after the first space outside quotes. */
	const WCHAR *line=L"\"C:\\Program Files\\Office\\run.exe\" /quiet now";
	CHECK(36,PathGetArgsW(line)==line+34 && same(PathGetArgsW(L"alone"),L""));
	WCHAR command[]=L"\"C:\\Program Files\\run.exe\" /quiet now";
	PathRemoveArgsW(command);
	CHECK(37,same(command,L"\"C:\\Program Files\\run.exe\""));
	WCHAR lone[]=L"alone";
	PathRemoveArgsW(lone);
	CHECK(38,same(lone,L"alone"));
	WCHAR way[MAX_PATH];
	CHECK(39,PathRelativePathToW(way,L"C:\\a\\b\\file.txt",FILE_ATTRIBUTE_NORMAL,L"C:\\a\\c\\d.txt",FILE_ATTRIBUTE_NORMAL) && same(way,L"..\\c\\d.txt"));
	CHECK(40,PathRelativePathToW(way,L"C:\\A\\b",FILE_ATTRIBUTE_DIRECTORY,L"c:\\a\\b\\e",FILE_ATTRIBUTE_NORMAL) && same(way,L".\\e"));
	CHECK(41,!PathRelativePathToW(way,L"C:\\a",FILE_ATTRIBUTE_DIRECTORY,L"D:\\a",FILE_ATTRIBUTE_NORMAL));
	return 0;
}
