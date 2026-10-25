/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven mscoree.dll: the system's entry to the .NET Framework, which managed programs and the libraries that host the runtime import.
 *
 * As on Windows, the system library itself holds no runtime: it finds the shim implementation the installed .NET Framework brings,
 * mscoreei.dll in the version folder under the registry's InstallRoot, and every export goes on to the export of that name there. When
 * no .NET Framework is installed, a managed program hears so in the dialog Windows shows, and ends; other processes do not load it.
 *
 * This is Raven code on the public Windows headers. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The exports, in the order of the table the thunks index. */
#define EXPORTS(X) \
	X(0, CLRCreateInstance) X(1, CallFunctionShim) X(2, CloseCtrs) X(3, ClrCreateManagedInstance) X(4, CoEEShutDownCOM) \
	X(5, CoInitializeCor) X(6, CoInitializeEE) X(7, CoUninitializeCor) X(8, CoUninitializeEE) X(9, CollectCtrs) \
	X(10, CorBindToCurrentRuntime) X(11, CorBindToRuntime) X(12, CorBindToRuntimeByCfg) X(13, CorBindToRuntimeByPath) \
	X(14, CorBindToRuntimeByPathEx) X(15, CorBindToRuntimeEx) X(16, CorBindToRuntimeHost) X(17, CorExitProcess) X(18, CorGetSvc) \
	X(19, CorIsLatestSvc) X(20, CorMarkThreadInThreadPool) X(21, CorTickleSvc) X(22, CreateConfigStream) \
	X(23, CreateDebuggingInterfaceFromVersion) X(24, CreateInterface) X(25, DllCanUnloadNow) X(26, DllGetClassObject) \
	X(27, DllRegisterServer) X(28, DllUnregisterServer) X(29, EEDllGetClassObjectFromClass) X(30, EEDllRegisterServer) \
	X(31, EEDllUnregisterServer) X(32, GetAssemblyMDImport) X(33, GetCORRequiredVersion) X(34, GetCORRootDirectory) \
	X(35, GetCORSystemDirectory_RetAddr) X(36, GetCORVersion) X(37, GetCompileInfo) X(38, GetFileVersion) X(39, GetHashFromAssemblyFile) \
	X(40, GetHashFromAssemblyFileW) X(41, GetHashFromBlob) X(42, GetHashFromFile) X(43, GetHashFromFileW) X(44, GetHashFromHandle) \
	X(45, GetHostConfigurationFile) X(46, GetMetaDataInternalInterface) X(47, GetMetaDataInternalInterfaceFromPublic) \
	X(48, GetMetaDataPublicInterfaceFromInternal) X(49, GetPermissionRequests) X(50, GetPrivateContextsPerfCounters) \
	X(51, GetProcessExecutableHeap) X(52, GetRealProcAddress) X(53, GetRequestedRuntimeInfo) X(54, GetRequestedRuntimeVersion) \
	X(55, GetRequestedRuntimeVersionForCLSID) X(56, GetStartupFlags) X(57, GetTargetForVTableEntry) X(58, GetTokenForVTableEntry) \
	X(59, GetVersionFromProcess) X(60, GetXMLElement) X(61, GetXMLElementAttribute) X(62, GetXMLObject) X(63, IEE) X(64, InitErrors) \
	X(65, InitSSAutoEnterThread) X(66, LoadLibraryShim) X(67, LoadLibraryWithPolicyShim) X(68, LoadStringRC) X(69, LoadStringRCEx) \
	X(70, LockClrVersion) X(71, LogHelp_LogAssert) X(72, LogHelp_NoGuiOnAssert) X(73, LogHelp_TerminateOnAssert) \
	X(74, MetaDataGetDispenser) X(75, ND_CopyObjDst) X(76, ND_CopyObjSrc) X(77, ND_RI2) X(78, ND_RI4) X(79, ND_RI8) X(80, ND_RU1) \
	X(81, ND_WI2) X(82, ND_WI4) X(83, ND_WI8) X(84, ND_WU1) X(85, OpenCtrs) X(86, PostError) X(87, PostErrorVA) \
	X(88, ReOpenMetaDataWithMemory) X(89, ReOpenMetaDataWithMemoryEx) X(90, RunDll32ShimW) X(91, RuntimeOSHandle) \
	X(92, RuntimeOpenImage) X(93, RuntimeReleaseHandle) X(94, SetTargetForVTableEntry) X(95, StrongNameCompareAssemblies) \
	X(96, StrongNameErrorInfo) X(97, StrongNameFreeBuffer) X(98, StrongNameGetBlob) X(99, StrongNameGetBlobFromImage) \
	X(100, StrongNameGetPublicKey) X(101, StrongNameHashSize) X(102, StrongNameKeyDelete) X(103, StrongNameKeyGen) \
	X(104, StrongNameKeyGenEx) X(105, StrongNameKeyInstall) X(106, StrongNameSignatureGeneration) \
	X(107, StrongNameSignatureGenerationEx) X(108, StrongNameSignatureSize) X(109, StrongNameSignatureVerification) \
	X(110, StrongNameSignatureVerificationEx) X(111, StrongNameSignatureVerificationFromImage) X(112, StrongNameTokenFromAssembly) \
	X(113, StrongNameTokenFromAssemblyEx) X(114, StrongNameTokenFromPublicKey) X(115, TranslateSecurityAttributes) X(116, UpdateError) \
	X(117, _CorDllMain) X(118, _CorExeMain) X(119, _CorExeMain2)

#define COUNT 121
#define CORDLLMAIN 117

#define NAME(index, name) #name,
/* The last entry is number 24, which has no name: the runtime asks the system shim through it for an interface of the shim
 * implementation, which exports it at the same number. mscoree64.def and mscoree32.def give it its number. */
static const char *const names[COUNT] = {EXPORTS(NAME) MAKEINTRESOURCEA(24)};
static FARPROC targets[COUNT];
static HMODULE implementation;

/* The shim implementation of the installed Framework: InstallRoot names the folder of the versions, which a program of each width sees in
 * its own registry view; without the value, the folder Windows puts the Framework in. */
static BOOL implementation_path(WCHAR *path)
{
	DWORD size = (MAX_PATH + 40) * sizeof(WCHAR);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\.NETFramework", L"InstallRoot", RRF_RT_REG_SZ, NULL, path, &size) != ERROR_SUCCESS) {
		const UINT length = GetWindowsDirectoryW(path, MAX_PATH);
		if (!length || length >= MAX_PATH)
			return FALSE;
		lstrcatW(path, sizeof(void *) == 8 ? L"\\Microsoft.NET\\Framework64\\" : L"\\Microsoft.NET\\Framework\\");
	}
	const int length = lstrlenW(path);
	if (length && path[length - 1] != L'\\')
		lstrcatW(path, L"\\");
	lstrcatW(path, L"v4.0.30319\\mscoreei.dll");
	return TRUE;
}

static HMODULE load_implementation(void)
{
	WCHAR path[MAX_PATH + 40];
	return implementation_path(path) ? LoadLibraryW(path) : NULL;
}

/* Whether the program of the process is managed code, which carries the runtime's header in its image. */
static BOOL managed_program(void)
{
	const BYTE *base = (const BYTE *)GetModuleHandleW(NULL);
	const IMAGE_NT_HEADERS *headers = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
	const IMAGE_DATA_DIRECTORY *directory = headers->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
		? &((const IMAGE_NT_HEADERS64 *)headers)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR]
		: &((const IMAGE_NT_HEADERS32 *)headers)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
	return directory->VirtualAddress && directory->Size;
}

/* A library loaded as managed code where no runtime exists does not load. */
static BOOL WINAPI no_runtime_library(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	return FALSE;
}

/* The address an export goes on to: the implementation's function of that name. Without a runtime, a program that needs it hears so in
 * the words Windows uses, and ends. */
void *raven_mscoree_resolve(unsigned index)
{
	if (targets[index])
		return (void *)targets[index];
	if (!implementation)
		implementation = load_implementation();
	FARPROC target = implementation ? GetProcAddress(implementation, names[index]) : NULL;
	if (!target) {
		if (index == CORDLLMAIN)
			return (void *)no_runtime_library;
		MessageBoxW(NULL, L"To run this application, you first must install one of the following versions of the .NET Framework:\n"
			L"  v4.0.30319\n\nContact your application publisher for instructions about obtaining the appropriate version of the .NET "
			L"Framework.", L".NET Framework Initialization Error", MB_OK | MB_ICONERROR);
		ExitProcess(0x80131700);
	}
	targets[index] = target;
	return (void *)target;
}

/* Each export saves the argument registers, finds its target and jumps to it with the caller's arguments untouched. */
#ifdef __x86_64__
#define THUNK(index, name) \
	".globl " #name "\n" #name ":\n" \
	"	push %rcx\n	push %rdx\n	push %r8\n	push %r9\n	sub $0x68, %rsp\n" \
	"	movdqu %xmm0, 0x20(%rsp)\n	movdqu %xmm1, 0x30(%rsp)\n	movdqu %xmm2, 0x40(%rsp)\n	movdqu %xmm3, 0x50(%rsp)\n" \
	"	mov $" #index ", %ecx\n	call raven_mscoree_resolve\n" \
	"	movdqu 0x20(%rsp), %xmm0\n	movdqu 0x30(%rsp), %xmm1\n	movdqu 0x40(%rsp), %xmm2\n	movdqu 0x50(%rsp), %xmm3\n" \
	"	add $0x68, %rsp\n	pop %r9\n	pop %r8\n	pop %rdx\n	pop %rcx\n	jmp *%rax\n"
#else
#define THUNK(index, name) \
	".globl _" #name "\n_" #name ":\n	push $" #index "\n	call _raven_mscoree_resolve\n	add $4, %esp\n	jmp *%eax\n"
#endif
#define EXPORT(index, name) " -export:" #name

__asm__(".text\n" EXPORTS(THUNK) THUNK(120, raven_mscoree_shim_interface));
__asm__(".section .drectve\n.ascii \"" EXPORTS(EXPORT) "\"\n.text");

/* The runtime's folder: the implementation is told which code asks, by its return address, as the system shim tells it, so that a
 * library bound to an older runtime gets that runtime's folder. */
typedef HRESULT (WINAPI *directory_with_caller)(WCHAR *, DWORD, DWORD *, void *);

HRESULT WINAPI GetCORSystemDirectory(WCHAR *buffer, DWORD size, DWORD *length)
{
	return ((directory_with_caller)raven_mscoree_resolve(35))(buffer, size, length, __builtin_return_address(0));
}
#ifdef __x86_64__
__asm__(".section .drectve\n.ascii \" -export:GetCORSystemDirectory\"\n.text");
#else
__asm__(".section .drectve\n.ascii \" -export:GetCORSystemDirectory=GetCORSystemDirectory@12\"\n.text");
#endif

/* Number 142, which has no name: the version of Windows, which the shim implementation asks the system shim for, as GetVersionEx answers.
 * mscoree64.def and mscoree32.def give it its number. */
BOOL WINAPI raven_mscoree_version(OSVERSIONINFOW *info)
{
	return GetVersionExW(info);
}

/* Without a Framework installed, the library is there only for managed programs, which hear that they need one. Any other process finds
 * no library, as on a Windows without the Framework: an installer looking for the runtime goes on without it. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		WCHAR path[MAX_PATH + 40];
		if (!managed_program() && (!implementation_path(path) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES))
			return FALSE;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
