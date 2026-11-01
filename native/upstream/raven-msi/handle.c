/* SPDX-License-Identifier: MIT */
#include "msi_own.h"

void package_startup(void);

static CRITICAL_SECTION handle_lock;
static msi_object **handles;
static UINT handle_room;

void object_init(msi_object *object, object_kind kind, void (*destroy)(msi_object *))
{
	object->kind = kind;
	object->refs = 1;
	object->destroy = destroy;
}

void object_ref(msi_object *object)
{
	InterlockedIncrement(&object->refs);
}

void object_unref(msi_object *object)
{
	if (object && !InterlockedDecrement(&object->refs)) {
		if (object->destroy)
			object->destroy(object);
		msi_free(object);
	}
}

MSIHANDLE handle_open(msi_object *object)
{
	MSIHANDLE handle = 0;
	EnterCriticalSection(&handle_lock);
	for (UINT i = 0; i < handle_room; i++)
		if (!handles[i]) {
			handle = i + 1;
			break;
		}
	if (!handle) {
		UINT room = handle_room ? handle_room * 2 : 256;
		msi_object **grown = msi_grow(handles, room * sizeof(*handles));
		if (grown) {
			handle = handle_room + 1;
			handles = grown;
			handle_room = room;
		}
	}
	if (handle) {
		handles[handle - 1] = object;
		object_ref(object);
	}
	LeaveCriticalSection(&handle_lock);
	return handle;
}

msi_object *handle_lookup(MSIHANDLE handle, object_kind kind)
{
	msi_object *object = NULL;
	EnterCriticalSection(&handle_lock);
	if (handle && handle <= handle_room && handles[handle - 1] && (!kind || handles[handle - 1]->kind == kind)) {
		object = handles[handle - 1];
		object_ref(object);
	}
	LeaveCriticalSection(&handle_lock);
	return object;
}

UINT WINAPI MsiCloseHandle(MSIHANDLE handle)
{
	msi_object *object = NULL;
	if (!handle)
		return ERROR_SUCCESS;
	EnterCriticalSection(&handle_lock);
	if (handle <= handle_room) {
		object = handles[handle - 1];
		handles[handle - 1] = NULL;
	}
	LeaveCriticalSection(&handle_lock);
	if (!object)
		return ERROR_INVALID_HANDLE;
	object_unref(object);
	return ERROR_SUCCESS;
}

UINT WINAPI MsiCloseAllHandles(void)
{
	UINT closed = 0;
	for (UINT i = 0; i < handle_room; i++)
		if (handles[i]) {
			MsiCloseHandle(i + 1);
			closed++;
		}
	return closed;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
		InitializeCriticalSection(&handle_lock);
		package_startup();
	}
	return TRUE;
}
