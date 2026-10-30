/* The helper a linker's delay-load thunks call: finds a delayed import on first use, as Microsoft's documentation of the delay-load helper
   describes, with its notification and failure hooks. */
#include <windows.h>
#include <delayimp.h>

extern PfnDliHook __pfnDliNotifyHook2;
extern PfnDliHook __pfnDliFailureHook2;
extern const IMAGE_DOS_HEADER __ImageBase;

static void *from_rva(RVA rva) { return (char *)&__ImageBase + rva; }

static FARPROC notify(unsigned event, DelayLoadInfo *info) { return __pfnDliNotifyHook2 ? __pfnDliNotifyHook2(event, info) : NULL; }
static FARPROC fail(unsigned event, DelayLoadInfo *info) { return __pfnDliFailureHook2 ? __pfnDliFailureHook2(event, info) : NULL; }

static void raise_failure(DWORD error, DelayLoadInfo *info)
{
	ULONG_PTR argument = (ULONG_PTR)info;
	RaiseException(VcppException(ERROR_SEVERITY_ERROR, error), 0, 1, &argument);
}

static HMODULE find_library(DelayLoadInfo *info, HMODULE *cached)
{
	if (*cached)
		return *cached;
	HMODULE module = (HMODULE)notify(dliNotePreLoadLibrary, info);
	if (!module)
		module = LoadLibraryA(info->szDll);
	if (!module) {
		info->dwLastError = GetLastError();
		module = (HMODULE)fail(dliFailLoadLib, info);
	}
	if (module)
		*cached = module;
	return module;
}

FARPROC WINAPI __delayLoadHelper2(PCImgDelayDescr descriptor, PImgThunkData slot)
{
	PImgThunkData addresses = from_rva(descriptor->rvaIAT), names = from_rva(descriptor->rvaINT);
	const ptrdiff_t index = slot - addresses;
	DelayLoadInfo info = {sizeof(info), descriptor, (FARPROC *)&slot->u1.Function, from_rva(descriptor->rvaDLLName)};
	info.dlp.fImportByName = !IMAGE_SNAP_BY_ORDINAL(names[index].u1.Ordinal);
	if (info.dlp.fImportByName)
		info.dlp.szProcName = (const char *)((PIMAGE_IMPORT_BY_NAME)from_rva((RVA)names[index].u1.AddressOfData))->Name;
	else
		info.dlp.dwOrdinal = IMAGE_ORDINAL(names[index].u1.Ordinal);
	info.pfnCur = notify(dliStartProcessing, &info);
	if (!info.pfnCur) {
		info.hmodCur = find_library(&info, (HMODULE *)from_rva(descriptor->rvaHmod));
		if (!info.hmodCur) {
			raise_failure(ERROR_MOD_NOT_FOUND, &info);
			return info.pfnCur;
		}
		info.dwLastError = ERROR_SUCCESS;
		info.pfnCur = notify(dliNotePreGetProcAddress, &info);
		if (!info.pfnCur)
			info.pfnCur = GetProcAddress(info.hmodCur, info.dlp.szProcName);
		if (!info.pfnCur) {
			info.dwLastError = GetLastError();
			info.pfnCur = fail(dliFailGetProc, &info);
		}
		if (!info.pfnCur)
			raise_failure(ERROR_PROC_NOT_FOUND, &info);
		else
			info.dwLastError = ERROR_SUCCESS;
	}
	slot->u1.Function = (DWORD_PTR)info.pfnCur;
	notify(dliNoteEndProcessing, &info);
	return info.pfnCur;
}
