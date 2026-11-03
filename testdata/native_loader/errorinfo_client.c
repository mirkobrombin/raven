#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

typedef HRESULT (WINAPI *take_function)(ULONG, IErrorInfo **);

static BOOL same_text(BSTR text, const WCHAR *expected)
{
	BOOL same = text && !lstrcmpW(text, expected);
	SysFreeString(text);
	return same;
}

static DWORD WINAPI other_thread(void *parameter)
{
	IErrorInfo *error = (IErrorInfo *)1;
	return GetErrorInfo(0, &error) == S_FALSE && !error;
}

/* The error object automation passes along with a failure: made and filled through oleaut32, handed to the thread, read back through
 * ole32 once and then gone. Another thread never sees it, and a later object replaces the first. */
int raven_entry(void)
{
	static const GUID guid = {0x1b2c3d4e, 0x5f60, 0x4718, {0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0}};
	ICreateErrorInfo *create = NULL;
	if (CreateErrorInfo(&create) != S_OK || !create) return 1;
	ICreateErrorInfo_SetGUID(create, &guid);
	ICreateErrorInfo_SetSource(create, L"Raven.Probe");
	ICreateErrorInfo_SetDescription(create, L"Something failed");
	ICreateErrorInfo_SetHelpContext(create, 42);
	IErrorInfo *error = NULL;
	if (ICreateErrorInfo_QueryInterface(create, &IID_IErrorInfo, (void **)&error) != S_OK) return 2;
	ICreateErrorInfo_Release(create);
	if (SetErrorInfo(0, error) != S_OK) return 3;
	IErrorInfo_Release(error);

	HANDLE thread = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
	DWORD alone = 0;
	WaitForSingleObject(thread, INFINITE);
	GetExitCodeThread(thread, &alone);
	CloseHandle(thread);
	if (!alone) return 4;

	HMODULE ole = LoadLibraryW(L"ole32.dll");
	take_function take = ole ? (take_function)GetProcAddress(ole, "GetErrorInfo") : NULL;
	if (!take) return 5;
	IErrorInfo *got = NULL;
	if (take(0, &got) != S_OK || !got) return 6;
	GUID read;
	DWORD context = 0;
	BSTR text = NULL;
	if (IErrorInfo_GetGUID(got, &read) != S_OK || read.Data1 != guid.Data1 || read.Data4[7] != guid.Data4[7]) return 7;
	if (IErrorInfo_GetSource(got, &text) != S_OK || !same_text(text, L"Raven.Probe")) return 8;
	if (IErrorInfo_GetDescription(got, &text) != S_OK || !same_text(text, L"Something failed")) return 9;
	if (IErrorInfo_GetHelpFile(got, &text) != S_OK || text) return 10;
	if (IErrorInfo_GetHelpContext(got, &context) != S_OK || context != 42) return 11;
	IErrorInfo_Release(got);
	if (take(0, &got) != S_FALSE || got) return 12;

	if (CreateErrorInfo(&create) != S_OK) return 13;
	ICreateErrorInfo_SetDescription(create, L"first");
	ICreateErrorInfo_QueryInterface(create, &IID_IErrorInfo, (void **)&error);
	SetErrorInfo(0, error);
	IErrorInfo_Release(error);
	ICreateErrorInfo_SetDescription(create, L"second");
	ICreateErrorInfo_QueryInterface(create, &IID_IErrorInfo, (void **)&error);
	ICreateErrorInfo_Release(create);
	IErrorInfo *later = NULL;
	CreateErrorInfo(&create);
	ICreateErrorInfo_SetDescription(create, L"later");
	ICreateErrorInfo_QueryInterface(create, &IID_IErrorInfo, (void **)&later);
	ICreateErrorInfo_Release(create);
	SetErrorInfo(0, later);
	IErrorInfo_Release(later);
	IErrorInfo_Release(error);
	if (GetErrorInfo(0, &got) != S_OK || IErrorInfo_GetDescription(got, &text) != S_OK || !same_text(text, L"later")) return 14;
	IErrorInfo_Release(got);
	if (SetErrorInfo(0, NULL) != S_OK || GetErrorInfo(0, &got) != S_FALSE) return 15;
	return 0;
}
