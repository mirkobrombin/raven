#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

/* A type library whose file was changed after it was written, as a damaged or hostile one a program brings may be: a pointer type
   points back at itself. The library still loads, and the method using the type hands out a description whose pointers end. */

#ifdef _WIN64
#define SYSTEM SYS_WIN64
#else
#define SYSTEM SYS_WIN32
#endif

void *memset(void *target, int value, size_t count)
{
	unsigned char *bytes = target;
	for (size_t i = 0; i < count; i++)
		bytes[i] = (unsigned char)value;
	return target;
}

static BYTE file[65536];

static INT read_int(DWORD at)
{
	return file[at] | file[at + 1] << 8 | file[at + 2] << 16 | (INT)file[at + 3] << 24;
}

/* A library with one interface whose one method takes a long**. */
static int describe(const WCHAR *path)
{
	static const GUID library_id = {0x6a2e0d20, 0x1c3b, 0x4f6e, {0x8d, 0x21, 0x5b, 0x3c, 0x7a, 0x90, 0x11, 0x20}};
	static const GUID interface_id = {0x6a2e0d21, 0x1c3b, 0x4f6e, {0x8d, 0x21, 0x5b, 0x3c, 0x7a, 0x90, 0x11, 0x21}};
	static TYPEDESC inner, middle;
	static ELEMDESC param;
	static FUNCDESC desc;
	static const WCHAR *names[] = {L"Take", L"value"};
	ICreateTypeLib2 *library;
	ICreateTypeInfo *info;
	if (FAILED(CreateTypeLib2(SYSTEM, path, &library)))
		return 1;
	ICreateTypeLib2_SetGuid(library, &library_id);
	ICreateTypeLib2_SetName(library, L"RavenTypelibRead");
	if (FAILED(ICreateTypeLib2_CreateTypeInfo(library, L"IRavenRead", TKIND_INTERFACE, &info)))
		return 2;
	ICreateTypeInfo_SetGuid(info, &interface_id);
	inner.vt = VT_I4;
	middle.vt = VT_PTR;
	middle.lptdesc = &inner;
	param.tdesc.vt = VT_PTR;
	param.tdesc.lptdesc = &middle;
	param.paramdesc.wParamFlags = PARAMFLAG_FIN;
	desc.memid = 1;
	desc.funckind = FUNC_PUREVIRTUAL;
	desc.invkind = INVOKE_FUNC;
	desc.callconv = CC_STDCALL;
	desc.cParams = 1;
	desc.lprgelemdescParam = &param;
	desc.elemdescFunc.tdesc.vt = VT_HRESULT;
	if (FAILED(ICreateTypeInfo_AddFuncDesc(info, 0, &desc)) || FAILED(ICreateTypeInfo_SetFuncAndParamNames(info, 0, (LPOLESTR *)names, 2)))
		return 3;
	ICreateTypeInfo_Release(info);
	if (FAILED(ICreateTypeLib2_SaveAllChanges(library)))
		return 4;
	ICreateTypeLib2_Release(library);
	return 0;
}

/* The type description segment of the file: the long* entry, whose target is a plain type kept inline, is made to point at itself. */
static int loop_pointer(const WCHAR *path)
{
	DWORD size = 0, written = 0;
	HANDLE handle = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	BOOL read = handle != INVALID_HANDLE_VALUE && ReadFile(handle, file, sizeof file, &size, NULL);
	CloseHandle(handle);
	if (!read || size < 0x54 || size == sizeof file)
		return 5;
	DWORD directory = 0x54 + (read_int(0x14) & 0x100 ? 4 : 0) + read_int(0x20) * 4;
	if (directory + 16 * 15 > size)
		return 5;
	DWORD segment = (DWORD)read_int(directory + 9 * 16), length = (DWORD)read_int(directory + 9 * 16 + 4);
	BOOL found = FALSE;
	for (DWORD entry = 0; entry < length / 8 && segment + entry * 8 + 8 <= size && !found; entry++) {
		BYTE *at = file + segment + entry * 8;
		if (at[0] == VT_PTR && at[1] == 0 && at[4] == VT_I4 && at[5] == 0 && at[6] == 0 && at[7] == 0x80) {
			at[4] = (BYTE)(entry * 8);
			at[5] = (BYTE)(entry * 8 >> 8);
			at[7] = 0;
			found = TRUE;
		}
	}
	if (!found)
		return 6;
	handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	BOOL saved = handle != INVALID_HANDLE_VALUE && WriteFile(handle, file, size, &written, NULL) && written == size;
	CloseHandle(handle);
	return saved ? 0 : 7;
}

int raven_entry(void)
{
	WCHAR path[MAX_PATH];
	ITypeLib *library;
	ITypeInfo *info;
	FUNCDESC *desc;
	int result;
	CoInitialize(NULL);
	GetTempPathW(MAX_PATH, path);
	lstrcatW(path, sizeof(void *) == 8 ? L"raven-typelib-read64.tlb" : L"raven-typelib-read32.tlb");
	if ((result = describe(path)) || (result = loop_pointer(path)))
		return result;
	if (FAILED(LoadTypeLibEx(path, REGKIND_NONE, &library)) || FAILED(ITypeLib_GetTypeInfo(library, 0, &info)))
		return 8;
	if (FAILED(ITypeInfo_GetFuncDesc(info, 0, &desc)) || desc->cParams != 1)
		return 9;
	const TYPEDESC *type = &desc->lprgelemdescParam[0].tdesc;
	int steps = 0;
	while (type && type->vt == VT_PTR && steps < 1000) {
		type = type->lptdesc;
		steps++;
	}
	if (!type || steps == 1000)
		return 10;
	ITypeInfo_ReleaseFuncDesc(info, desc);
	ITypeInfo_Release(info);
	ITypeLib_Release(library);
	DeleteFileW(path);
	return 0;
}
