/* The Windows types and calls the XmlLite reader (native/guest/raven-xmllite/reader.c) uses, declared for building it on the host for
   native/fuzz/xml_reader.c, which defines what is declared here. Built with -fshort-wchar, so that wide literals are the 16-bit units of
   Windows. objbase.h beside it only includes this file; xmllite.h declares the reader's interface. */
#ifndef RAVEN_FUZZ_XML_WINDOWS_H
#define RAVEN_FUZZ_XML_WINDOWS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD, ULONG, UINT;
typedef int32_t LONG, HRESULT;
typedef intptr_t LONG_PTR;
typedef size_t SIZE_T;
typedef wchar_t WCHAR;
typedef int BOOL;
typedef void *HANDLE;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define CP_UTF8 65001u

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005u)
#define E_NOINTERFACE ((HRESULT)0x80004002u)
#define E_INVALIDARG ((HRESULT)0x80070057u)
#define E_OUTOFMEMORY ((HRESULT)0x8007000Eu)
#define E_PENDING ((HRESULT)0x8000000Au)
#define SUCCEEDED(status) ((HRESULT)(status) >= 0)
#define FAILED(status) ((HRESULT)(status) < 0)

#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))
#define CopyMemory(to, from, size) memcpy((to), (from), (size))
#define ZeroMemory(to, size) memset((to), 0, (size))
#define CONTAINING_RECORD(address, type, field) ((type *)((char *)(address) - offsetof(type, field)))

typedef struct
{
	uint32_t Data1;
	uint16_t Data2, Data3;
	uint8_t Data4[8];
} GUID, IID;
typedef const GUID *REFIID;
#define IsEqualGUID(a, b) (!memcmp((a), (b), sizeof(GUID)))

#define HEAP_ZERO_MEMORY 8u
HANDLE GetProcessHeap(void);
void *HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
void *HeapReAlloc(HANDLE heap, DWORD flags, void *block, SIZE_T size);
BOOL HeapFree(HANDLE heap, DWORD flags, void *block);
LONG InterlockedIncrement(LONG volatile *value);
LONG InterlockedDecrement(LONG volatile *value);
int lstrlenW(const WCHAR *text);
int lstrcmpW(const WCHAR *left, const WCHAR *right);
int lstrcmpiW(const WCHAR *left, const WCHAR *right);
int MultiByteToWideChar(UINT code_page, DWORD flags, const char *bytes, int count, WCHAR *wide, int capacity);

/* COM objects as C sees them: a table of functions first. */
typedef struct IUnknown IUnknown;
typedef struct IUnknownVtbl
{
	HRESULT (WINAPI *QueryInterface)(IUnknown *self, REFIID riid, void **object);
	ULONG (WINAPI *AddRef)(IUnknown *self);
	ULONG (WINAPI *Release)(IUnknown *self);
} IUnknownVtbl;
struct IUnknown
{
	IUnknownVtbl *lpVtbl;
};
#define IUnknown_QueryInterface(self, riid, object) (self)->lpVtbl->QueryInterface((self), (riid), (object))
#define IUnknown_AddRef(self) (self)->lpVtbl->AddRef(self)
#define IUnknown_Release(self) (self)->lpVtbl->Release(self)

typedef struct ISequentialStream ISequentialStream;
typedef struct ISequentialStreamVtbl
{
	HRESULT (WINAPI *QueryInterface)(ISequentialStream *self, REFIID riid, void **object);
	ULONG (WINAPI *AddRef)(ISequentialStream *self);
	ULONG (WINAPI *Release)(ISequentialStream *self);
	HRESULT (WINAPI *Read)(ISequentialStream *self, void *buffer, ULONG count, ULONG *done);
	HRESULT (WINAPI *Write)(ISequentialStream *self, const void *buffer, ULONG count, ULONG *done);
} ISequentialStreamVtbl;
struct ISequentialStream
{
	ISequentialStreamVtbl *lpVtbl;
};
#define ISequentialStream_Read(self, buffer, count, done) (self)->lpVtbl->Read((self), (buffer), (count), (done))
#define ISequentialStream_Release(self) (self)->lpVtbl->Release(self)

typedef struct IMalloc IMalloc;
typedef struct IMallocVtbl
{
	HRESULT (WINAPI *QueryInterface)(IMalloc *self, REFIID riid, void **object);
	ULONG (WINAPI *AddRef)(IMalloc *self);
	ULONG (WINAPI *Release)(IMalloc *self);
	void *(WINAPI *Alloc)(IMalloc *self, SIZE_T size);
	void *(WINAPI *Realloc)(IMalloc *self, void *block, SIZE_T size);
	void (WINAPI *Free)(IMalloc *self, void *block);
} IMallocVtbl;
struct IMalloc
{
	IMallocVtbl *lpVtbl;
};
#define IMalloc_Alloc(self, size) (self)->lpVtbl->Alloc((self), (size))
#define IMalloc_Free(self, block) (self)->lpVtbl->Free((self), (block))
#define IMalloc_AddRef(self) (self)->lpVtbl->AddRef(self)
#define IMalloc_Release(self) (self)->lpVtbl->Release(self)

extern const IID IID_IUnknown, IID_ISequentialStream, IID_IXmlReader;

#endif
