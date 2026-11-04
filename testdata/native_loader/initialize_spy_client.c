/* CoRegisterInitializeSpy and CoRevokeInitializeSpy: a registered spy hears of each CoInitializeEx and CoUninitialize on its thread,
   before and after, with the apartment's reference counts; PostInitialize decides what CoInitializeEx returns, chained through every
   spy; a cookie only revokes its own registration on its own thread. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <objbase.h>
#include <objidl.h>

/* What a spy saw, one entry per notification: the method (1 PreInitialize, 2 PostInitialize, 3 PreUninitialize, 4 PostUninitialize),
   which spy, and the arguments. */
struct seen {
	int method, spy;
	DWORD first, second, third;
};

static struct seen events[32];
static int logged;

struct spy {
	IInitializeSpy face;
	LONG references;
	int number;
	HRESULT answer;
};

static void note(IInitializeSpy *face, int method, DWORD first, DWORD second, DWORD third)
{
	if (logged < 32) {
		events[logged].method = method;
		events[logged].spy = ((struct spy *)face)->number;
		events[logged].first = first;
		events[logged].second = second;
		events[logged].third = third;
	}
	++logged;
}

static int same_guid(REFIID left, REFIID right)
{
	const BYTE *a = (const BYTE *)left, *b = (const BYTE *)right;
	for (int at = 0; at < 16; ++at)
		if (a[at] != b[at])
			return 0;
	return 1;
}

static HRESULT STDMETHODCALLTYPE spy_query(IInitializeSpy *face, REFIID iid, void **out)
{
	if (same_guid(iid, &IID_IUnknown) || same_guid(iid, &IID_IInitializeSpy)) {
		*out = face;
		IInitializeSpy_AddRef(face);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE spy_add(IInitializeSpy *face) { return (ULONG)InterlockedIncrement(&((struct spy *)face)->references); }
static ULONG STDMETHODCALLTYPE spy_release(IInitializeSpy *face) { return (ULONG)InterlockedDecrement(&((struct spy *)face)->references); }

static HRESULT STDMETHODCALLTYPE spy_pre_initialize(IInitializeSpy *face, DWORD co_init, DWORD references)
{
	note(face, 1, co_init, references, 0);
	return S_OK;
}

/* Spy 2 answers with its own HRESULT when it has one; the others pass on what they were given. */
static HRESULT STDMETHODCALLTYPE spy_post_initialize(IInitializeSpy *face, HRESULT result, DWORD co_init, DWORD references)
{
	note(face, 2, (DWORD)result, co_init, references);
	return ((struct spy *)face)->answer ? ((struct spy *)face)->answer : result;
}

static HRESULT STDMETHODCALLTYPE spy_pre_uninitialize(IInitializeSpy *face, DWORD references)
{
	note(face, 3, references, 0, 0);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE spy_post_uninitialize(IInitializeSpy *face, DWORD references)
{
	note(face, 4, references, 0, 0);
	return S_OK;
}

static IInitializeSpyVtbl spy_methods = {spy_query, spy_add, spy_release, spy_pre_initialize, spy_post_initialize, spy_pre_uninitialize,
	spy_post_uninitialize};

/* An object that is no spy: it answers only for IUnknown. */
static HRESULT STDMETHODCALLTYPE plain_query(IUnknown *face, REFIID iid, void **out)
{
	if (same_guid(iid, &IID_IUnknown)) {
		*out = face;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE plain_count(IUnknown *face) { (void)face; return 1; }

static IUnknownVtbl plain_methods = {plain_query, plain_count, plain_count};

static int expect(int at, int method, int spy, DWORD first, DWORD second, DWORD third)
{
	return at < logged && events[at].method == method && events[at].spy == spy && events[at].first == first && events[at].second == second &&
		events[at].third == third;
}

static DWORD WINAPI other_thread(void *parameter)
{
	/* Another thread's cookie is not this thread's to revoke. */
	return CoRevokeInitializeSpy(*(ULARGE_INTEGER *)parameter) == E_INVALIDARG ? 0 : 1;
}

int raven_entry(void)
{
	struct spy first = {{&spy_methods}, 1, 1, 0}, second = {{&spy_methods}, 1, 2, 0};
	IUnknown plain = {&plain_methods};
	ULARGE_INTEGER cookie_one, cookie_two, wrong;
	HANDLE thread;
	DWORD code = 1;

	if (CoRegisterInitializeSpy(&first.face, NULL) != E_INVALIDARG)
		return 1;
	if (CoRegisterInitializeSpy((IInitializeSpy *)&plain, &cookie_one) != E_NOINTERFACE)
		return 2;
	/* Registration needs no initialized apartment and holds a reference of its own. */
	if (CoRegisterInitializeSpy(&first.face, &cookie_one) != S_OK || first.references != 2)
		return 3;

	if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK)
		return 4;
	if (logged != 2 || !expect(0, 1, 1, COINIT_APARTMENTTHREADED, 0, 0) || !expect(1, 2, 1, S_OK, COINIT_APARTMENTTHREADED, 1))
		return 5;
	/* A second initialization of the same kind is S_FALSE, and the other kind fails; the spy sees each count and result. */
	logged = 0;
	if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_FALSE || CoInitializeEx(NULL, COINIT_MULTITHREADED) != RPC_E_CHANGED_MODE)
		return 6;
	if (logged != 4 || !expect(0, 1, 1, COINIT_APARTMENTTHREADED, 1, 0) || !expect(1, 2, 1, S_FALSE, COINIT_APARTMENTTHREADED, 2) ||
		!expect(2, 1, 1, COINIT_MULTITHREADED, 2, 0) || !expect(3, 2, 1, (DWORD)RPC_E_CHANGED_MODE, COINIT_MULTITHREADED, 2))
		return 7;
	logged = 0;
	CoUninitialize();
	CoUninitialize();
	if (logged != 4 || !expect(0, 3, 1, 2, 0, 0) || !expect(1, 4, 1, 1, 0, 0) || !expect(2, 3, 1, 1, 0, 0) || !expect(3, 4, 1, 0, 0, 0))
		return 8;

	/* Two spies are told in the order they registered, and each PostInitialize gets the HRESULT the one before it returned. */
	second.answer = 0x00000123;
	if (CoRegisterInitializeSpy(&second.face, &cookie_two) != S_OK || second.references != 2)
		return 9;
	if (cookie_one.QuadPart == cookie_two.QuadPart)
		return 10;
	logged = 0;
	if (CoInitializeEx(NULL, COINIT_MULTITHREADED) != 0x00000123)
		return 11;
	if (logged != 4 || !expect(0, 1, 1, COINIT_MULTITHREADED, 0, 0) || !expect(1, 1, 2, COINIT_MULTITHREADED, 0, 0) ||
		!expect(2, 2, 1, S_OK, COINIT_MULTITHREADED, 1) || !expect(3, 2, 2, S_OK, COINIT_MULTITHREADED, 1))
		return 12;
	CoUninitialize();

	thread = CreateThread(NULL, 0, other_thread, &cookie_one, 0, NULL);
	if (!thread || WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &code) || code)
		return 13;
	CloseHandle(thread);

	/* Revoking releases the registration's reference; the cookie is spent after that, and a made-up one was never good. */
	if (CoRevokeInitializeSpy(cookie_one) != S_OK || first.references != 1)
		return 14;
	if (CoRevokeInitializeSpy(cookie_one) != E_INVALIDARG)
		return 15;
	wrong.QuadPart = cookie_two.QuadPart ^ ((ULONGLONG)1 << 40);
	if (CoRevokeInitializeSpy(wrong) != E_INVALIDARG)
		return 16;
	logged = 0;
	second.answer = 0;
	if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK)
		return 17;
	CoUninitialize();
	if (logged != 4 || !expect(0, 1, 2, COINIT_APARTMENTTHREADED, 0, 0) || !expect(3, 4, 2, 0, 0, 0))
		return 18;
	if (CoRevokeInitializeSpy(cookie_two) != S_OK || second.references != 1)
		return 19;
	logged = 0;
	if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK)
		return 20;
	CoUninitialize();
	return logged ? 21 : 0;
}
