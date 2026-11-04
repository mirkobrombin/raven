// Management instrumentation as System.Management uses it: the path parser takes paths apart and puts them together again, and the
// locator connects to root\cimv2, whose queries answer with the processor, the operating system's memory and this very process.
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <wbemcli.h>
#include <wmiutils.h>

static const CLSID path_class = {0xcf4cc405, 0xe2c5, 0x4ddd, {0xb3, 0xce, 0x5e, 0x75, 0x82, 0xd8, 0xc9, 0xfa}};
static const CLSID locator_class = {0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
static const IID path_iid = {0x3bc15af2, 0x736c, 0x477e, {0x9e, 0x51, 0x23, 0x8a, 0xf8, 0x66, 0x7d, 0xcc}};
static const IID locator_iid = {0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
static const IID security_iid = {0x0000013d, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static int path_says(IWbemPath *path, LONG flags, const wchar_t *wanted)
{
	wchar_t text[128];
	ULONG length = 0;
	if (IWbemPath_GetText(path, flags, &length, NULL) || length != (ULONG)lstrlenW(wanted) + 1)
		return 0;
	length = 128;
	return !IWbemPath_GetText(path, flags, &length, text) && !lstrcmpW(text, wanted);
}

static int paths(void)
{
	IWbemPath *path = NULL;
	if (CoCreateInstance(&path_class, NULL, CLSCTX_INPROC_SERVER, &path_iid, (void **)&path) || !path)
		return 10;
	ULONG count = 0;
	ULONGLONG info = 0;
	wchar_t text[64];
	ULONG length = 64;
	if (IWbemPath_SetText(path, WBEMPATH_CREATE_ACCEPT_ALL, L"\\\\.\\root\\cimv2") || IWbemPath_GetNamespaceCount(path, &count) || count != 2)
		return 11;
	if (IWbemPath_GetServer(path, &length, text) || lstrcmpW(text, L".") || !path_says(path, WBEMPATH_GET_SERVER_TOO, L"\\\\.\\root\\cimv2") ||
		!path_says(path, WBEMPATH_GET_NAMESPACE_ONLY, L"root\\cimv2"))
		return 12;
	if (IWbemPath_GetInfo(path, 0, &info) || !(info & WBEMPATH_INFO_SERVER_NAMESPACE_ONLY) || (info & (WBEMPATH_INFO_IS_CLASS_REF | WBEMPATH_INFO_IS_INST_REF)))
		return 13;
	// A relative instance path: no server, the class and its key.
	if (IWbemPath_SetText(path, WBEMPATH_CREATE_ACCEPT_ALL, L"Win32_Process.Handle=\"4\"") || IWbemPath_GetInfo(path, 0, &info) ||
		!(info & WBEMPATH_INFO_IS_INST_REF) || !path_says(path, WBEMPATH_GET_RELATIVE_ONLY, L"Win32_Process.Handle=\"4\""))
		return 14;
	length = 64;
	if (IWbemPath_GetServer(path, &length, text) != WBEM_E_NOT_AVAILABLE || IWbemPath_GetClassName(path, &length, text) || lstrcmpW(text, L"Win32_Process"))
		return 15;
	// "root" alone is a namespace only when the caller says so.
	if (IWbemPath_SetText(path, WBEMPATH_CREATE_ACCEPT_ALL | WBEMPATH_TREAT_SINGLE_IDENT_AS_NS, L"root") || IWbemPath_GetNamespaceCount(path, &count) ||
		count != 1 || IWbemPath_GetInfo(path, 0, &info) || (info & WBEMPATH_INFO_IS_CLASS_REF))
		return 16;
	if (IWbemPath_SetText(path, WBEMPATH_CREATE_ACCEPT_ALL, L"Win32_Process") || IWbemPath_GetInfo(path, 0, &info) || !(info & WBEMPATH_INFO_IS_CLASS_REF))
		return 17;
	IWbemPath_Release(path);
	return 0;
}

static BSTR text(const wchar_t *value)
{
	return SysAllocString(value);
}

static HRESULT query(IWbemServices *services, const wchar_t *wql, IEnumWbemClassObject **found)
{
	BSTR language = text(L"WQL"), statement = text(wql);
	const HRESULT status = IWbemServices_ExecQuery(services, language, statement, WBEM_FLAG_RETURN_IMMEDIATELY, NULL, found);
	SysFreeString(language);
	SysFreeString(statement);
	return status;
}

// The first object a query gives, and how many it gives in all.
static IWbemClassObject *first(IWbemServices *services, const wchar_t *wql, ULONG *total)
{
	IEnumWbemClassObject *found = NULL;
	*total = 0;
	if (query(services, wql, &found) || !found)
		return NULL;
	IWbemClassObject *object = NULL, *other = NULL;
	ULONG returned = 0;
	if (!IEnumWbemClassObject_Next(found, WBEM_INFINITE, 1, &object, &returned) && returned == 1)
		*total = 1;
	while (IEnumWbemClassObject_Next(found, WBEM_INFINITE, 1, &other, &returned) == S_OK && returned == 1) {
		IWbemClassObject_Release(other);
		++*total;
	}
	IEnumWbemClassObject_Release(found);
	return object;
}

static int queries(void)
{
	IWbemLocator *locator = NULL;
	IWbemServices *services = NULL;
	if (CoCreateInstance(&locator_class, NULL, CLSCTX_INPROC_SERVER, &locator_iid, (void **)&locator) || !locator)
		return 20;
	BSTR other = text(L"root\\default"), cimv2 = text(L"\\\\.\\root\\cimv2");
	if (IWbemLocator_ConnectServer(locator, other, NULL, NULL, NULL, 0, NULL, NULL, &services) != WBEM_E_INVALID_NAMESPACE)
		return 21;
	if (IWbemLocator_ConnectServer(locator, cimv2, NULL, NULL, NULL, 0, NULL, NULL, &services) || !services)
		return 22;
	SysFreeString(other);
	SysFreeString(cimv2);
	// The security set on the services, as on a proxy, is the security they report.
	IClientSecurity *security = NULL;
	DWORD level = 0;
	if (IWbemServices_QueryInterface(services, &security_iid, (void **)&security) || !security ||
		IClientSecurity_SetBlanket(security, (IUnknown *)services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
			RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE) ||
		IClientSecurity_QueryBlanket(security, (IUnknown *)services, NULL, NULL, NULL, &level, NULL, NULL, NULL) || level != RPC_C_AUTHN_LEVEL_PKT_PRIVACY)
		return 29;
	IClientSecurity_Release(security);

	// Unsigned 16 and 32 bit values come as VT_I4, 64 bit ones as decimal text.
	ULONG total = 0;
	VARIANT value;
	CIMTYPE type = 0;
	IWbemClassObject *processor = first(services, L"select Architecture, NumberOfCores, CurrentClockSpeed, MaxClockSpeed, Family, ProcessorId from Win32_Processor", &total);
	if (!processor || total != 1)
		return 23;
	SYSTEM_INFO system;
	GetNativeSystemInfo(&system);
	if (IWbemClassObject_Get(processor, L"Architecture", 0, &value, &type, NULL) || V_VT(&value) != VT_I4 || type != CIM_UINT16 ||
		V_I4(&value) != system.wProcessorArchitecture)
		return 24;
	if (IWbemClassObject_Get(processor, L"NumberOfCores", 0, &value, &type, NULL) || V_VT(&value) != VT_I4 || V_I4(&value) < 1)
		return 25;
	if (IWbemClassObject_Get(processor, L"ProcessorId", 0, &value, &type, NULL) || V_VT(&value) != VT_BSTR || lstrlenW(V_BSTR(&value)) != 16)
		return 26;
	VariantClear(&value);
	if (IWbemClassObject_Get(processor, L"__CLASS", 0, &value, NULL, NULL) || V_VT(&value) != VT_BSTR || lstrcmpW(V_BSTR(&value), L"Win32_Processor"))
		return 27;
	VariantClear(&value);
	if (IWbemClassObject_Get(processor, L"Missing", 0, &value, NULL, NULL) != WBEM_E_NOT_FOUND)
		return 28;
	IWbemClassObject_Release(processor);

	IWbemClassObject *os = first(services, L"select TotalVisibleMemorySize, FreePhysicalMemory, TotalVirtualMemorySize, FreeVirtualMemory from Win32_OperatingSystem", &total);
	if (!os || total != 1)
		return 30;
	MEMORYSTATUSEX memory = {sizeof(memory)};
	GlobalMemoryStatusEx(&memory);
	if (IWbemClassObject_Get(os, L"TotalVisibleMemorySize", 0, &value, &type, NULL) || V_VT(&value) != VT_BSTR || type != CIM_UINT64)
		return 31;
	ULONGLONG kilobytes = 0;
	for (const wchar_t *digit = V_BSTR(&value); *digit; digit++)
		kilobytes = kilobytes * 10 + (*digit - L'0');
	VariantClear(&value);
	if (kilobytes != memory.ullTotalPhys / 1024)
		return 32;
	IWbemClassObject_Release(os);

	// This process, found by its identifier; its children, none, by theirs.
	wchar_t statement[96];
	wsprintfW(statement, L"Select * From Win32_Process Where ProcessID=%u", GetCurrentProcessId());
	IWbemClassObject *self = first(services, statement, &total);
	if (!self || total != 1)
		return 33;
	if (IWbemClassObject_Get(self, L"ProcessId", 0, &value, NULL, NULL) || V_VT(&value) != VT_I4 || (DWORD)V_I4(&value) != GetCurrentProcessId())
		return 34;
	IWbemClassObject_Release(self);
	wsprintfW(statement, L"Select * From Win32_Process Where ParentProcessID=%u", GetCurrentProcessId());
	self = first(services, statement, &total);
	if (self || total)
		return 35;
	self = first(services, L"select Name from Win32_Process where Name like 'wmi-client%' and not ProcessId = 0", &total);
	if (!self || !total)
		return 36;
	IWbemClassObject_Release(self);

	IEnumWbemClassObject *found = NULL;
	if (query(services, L"select * from Win32_NoSuchClass", &found) != WBEM_E_INVALID_CLASS || query(services, L"select from", &found) != WBEM_E_INVALID_QUERY ||
		query(services, L"select Missing from Win32_Processor", &found) != WBEM_E_INVALID_QUERY)
		return 37;
	// A clone starts where its original is; a reset goes back to the start.
	if (query(services, L"select * from Win32_OperatingSystem", &found) || !found)
		return 38;
	IEnumWbemClassObject *clone = NULL;
	IWbemClassObject *object = NULL;
	ULONG returned = 0;
	if (IEnumWbemClassObject_Clone(found, &clone) || IEnumWbemClassObject_Next(found, WBEM_INFINITE, 1, &object, &returned) || returned != 1)
		return 39;
	IWbemClassObject_Release(object);
	if (IEnumWbemClassObject_Next(found, WBEM_INFINITE, 1, &object, &returned) != WBEM_S_FALSE || returned || IEnumWbemClassObject_Reset(found) ||
		IEnumWbemClassObject_Next(found, WBEM_INFINITE, 1, &object, &returned) || returned != 1)
		return 40;
	IWbemClassObject_Release(object);
	if (IEnumWbemClassObject_Next(clone, WBEM_INFINITE, 1, &object, &returned) || returned != 1)
		return 41;
	IWbemClassObject_Release(object);
	IEnumWbemClassObject_Release(clone);
	IEnumWbemClassObject_Release(found);

	BSTR instance = text(L"Win32_OperatingSystem=@");
	object = NULL;
	if (IWbemServices_GetObject(services, instance, 0, NULL, &object, NULL) || !object)
		return 42;
	SysFreeString(instance);
	IWbemClassObject_Release(object);
	IWbemServices_Release(services);
	IWbemLocator_Release(locator);
	return 0;
}

int raven_entry(void)
{
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
		return 1;
	int result = paths();
	if (!result)
		result = queries();
	CoUninitialize();
	return result;
}
