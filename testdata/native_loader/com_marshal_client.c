#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

/* A class factory handed to another process in marshaled form, as a program gives one to a helper it started:
   the helper unmarshals it, asks for an instance through it and calls the object it gets back. */

int memcmp(const void *left, const void *right, size_t count)
{
	const unsigned char *a = left, *b = right;
	for (size_t i = 0; i < count; i++)
		if (a[i] != b[i])
			return a[i] - b[i];
	return 0;
}

static LONG created;

static HRESULT WINAPI unknown_query(IUnknown *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI unknown_addref(IUnknown *iface)
{
	(void)iface;
	return 2;
}

static ULONG WINAPI unknown_release(IUnknown *iface)
{
	(void)iface;
	return 1;
}

static const IUnknownVtbl unknown_table = { unknown_query, unknown_addref, unknown_release };
static IUnknown instance = { (IUnknownVtbl *)&unknown_table };

static HRESULT WINAPI factory_query(IClassFactory *iface, REFIID iid, void **out)
{
	if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
		*out = iface;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI factory_addref(IClassFactory *iface)
{
	(void)iface;
	return 2;
}

static ULONG WINAPI factory_release(IClassFactory *iface)
{
	(void)iface;
	return 1;
}

static HRESULT WINAPI factory_create(IClassFactory *iface, IUnknown *outer, REFIID iid, void **out)
{
	(void)iface, (void)outer;
	InterlockedIncrement(&created);
	return IUnknown_QueryInterface(&instance, iid, out);
}

static HRESULT WINAPI factory_lock(IClassFactory *iface, BOOL locking)
{
	(void)iface, (void)locking;
	return S_OK;
}

static const IClassFactoryVtbl factory_table = { factory_query, factory_addref, factory_release, factory_create, factory_lock };
static IClassFactory factory = { (IClassFactoryVtbl *)&factory_table };

static int helper(const WCHAR *mapping_name)
{
	HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name);
	if (!mapping)
		return 30;
	const BYTE *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	if (!view)
		return 31;
	const DWORD size = *(const DWORD *)view;
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
		return 32;
	IStream *stream;
	if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)))
		return 33;
	LARGE_INTEGER start = {0};
	IStream_Write(stream, view + 4, size, NULL);
	IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
	IClassFactory *remote = NULL;
	if (FAILED(CoUnmarshalInterface(stream, &IID_IClassFactory, (void **)&remote)) || !remote)
		return 34;
	IUnknown *object = NULL;
	if (FAILED(IClassFactory_CreateInstance(remote, NULL, &IID_IUnknown, (void **)&object)) || !object)
		return 35;
	IClassFactory *again = NULL;
	if (IClassFactory_QueryInterface(remote, &IID_IClassFactory, (void **)&again) != S_OK || again != remote)
		return 36;
	IClassFactory_Release(again);
	void *absent = NULL;
	if (IUnknown_QueryInterface(object, &IID_IClassFactory, &absent) != E_NOINTERFACE)
		return 37;
	IUnknown_Release(object);
	IClassFactory_Release(remote);
	/* Table data is read again after its first reader let go, and the object must still answer. */
	IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
	if (FAILED(CoUnmarshalInterface(stream, &IID_IClassFactory, (void **)&remote)) || !remote)
		return 38;
	if (FAILED(IClassFactory_CreateInstance(remote, NULL, &IID_IUnknown, (void **)&object)) || !object)
		return 39;
	IUnknown_Release(object);
	IClassFactory_Release(remote);
	return 0;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	for (const WCHAR *at = line; *at; at++)
		if (at[0] == L'/' && at[1] == L'h' && at[2] == L' ')
			return helper(at + 3);
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
		return 1;
	/* A marshaled reference comes back as the object itself in its own apartment. */
	DWORD size = 0;
	if (FAILED(CoGetMarshalSizeMax(&size, &IID_IClassFactory, (IUnknown *)&factory, MSHCTX_LOCAL, NULL, MSHLFLAGS_NORMAL)) || !size)
		return 2;
	IStream *stream;
	if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)))
		return 3;
	if (FAILED(CoMarshalInterface(stream, &IID_IClassFactory, (IUnknown *)&factory, MSHCTX_LOCAL, NULL, MSHLFLAGS_TABLESTRONG)))
		return 4;
	LARGE_INTEGER start = {0};
	ULARGE_INTEGER end;
	IStream_Seek(stream, start, STREAM_SEEK_CUR, &end);
	IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
	IClassFactory *direct = NULL;
	if (FAILED(CoUnmarshalInterface(stream, &IID_IClassFactory, (void **)&direct)) || direct != &factory)
		return 5;

	static const WCHAR name[] = L"raven-com-marshal-test";
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, name);
	BYTE *view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0) : NULL;
	if (!view)
		return 6;
	IStream_Seek(stream, start, STREAM_SEEK_SET, NULL);
	IStream_Read(stream, view + 4, end.LowPart, NULL);
	*(DWORD *)view = end.LowPart;

	WCHAR self[MAX_PATH], command[MAX_PATH + 64];
	GetModuleFileNameW(NULL, self, MAX_PATH);
	wsprintfW(command, L"\"%s\" /h %s", self, name);
	STARTUPINFOW startup = { sizeof startup };
	PROCESS_INFORMATION process;
	if (!CreateProcessW(self, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
		return 7;
	/* The serving apartment keeps its messages moving, or the helper's calls would never run. */
	for (;;) {
		const DWORD waited = MsgWaitForMultipleObjects(1, &process.hProcess, FALSE, 30000, QS_ALLINPUT);
		if (waited == WAIT_OBJECT_0)
			break;
		if (waited != WAIT_OBJECT_0 + 1)
			return 8;
		MSG message;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&message);
	}
	DWORD status = 99;
	GetExitCodeProcess(process.hProcess, &status);
	if (status)
		return (int)status;
	if (created != 2)
		return 9;
	return 0;
}
