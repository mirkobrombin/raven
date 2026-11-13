// A cursor file read into an empty picture through its stream becomes an icon handle that keeps the cursor's hot spot, as Windows Forms
// loads its hand cursor (MusicBee shows it over its album grid). The exit code names the first check that failed.
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <ole2.h>
#include <olectl.h>

/* A 32x32 monochrome cursor: the directory, one entry with the hot spot at (5, 7), then the bitmap with its XOR and AND masks. */
static BYTE file[6 + 16 + 40 + 8 + 128 + 128];

static void put16(BYTE *at, WORD value)
{
	at[0] = (BYTE)value;
	at[1] = (BYTE)(value >> 8);
}

static void put32(BYTE *at, DWORD value)
{
	put16(at, (WORD)value);
	put16(at + 2, (WORD)(value >> 16));
}

static void build(void)
{
	BYTE *entry = file + 6, *image = file + 22;
	put16(file + 2, 2);
	put16(file + 4, 1);
	entry[0] = 32;
	entry[1] = 32;
	put16(entry + 4, 5);
	put16(entry + 6, 7);
	put32(entry + 8, sizeof file - 22);
	put32(entry + 12, 22);
	put32(image, 40);
	put32(image + 4, 32);
	put32(image + 8, 64);
	put16(image + 12, 1);
	put16(image + 14, 1);
	put32(image + 44, 0xffffff);
	for (int row = 0; row < 32; row++)
		image[48 + row * 4] = 0xf0;
}

int raven_entry(void)
{
	build();
	if (FAILED(CoInitialize(NULL)))
		return 1;
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof file);
	BYTE *bytes = memory ? GlobalLock(memory) : NULL;
	if (!bytes)
		return 2;
	for (unsigned index = 0; index < sizeof file; index++)
		bytes[index] = file[index];
	GlobalUnlock(memory);
	IStream *stream = NULL;
	if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)))
		return 3;
	IPicture *picture = NULL;
	if (FAILED(OleCreatePictureIndirect(NULL, &IID_IPicture, TRUE, (void **)&picture)) || !picture)
		return 4;
	IPersistStream *persist = NULL;
	if (FAILED(IPicture_QueryInterface(picture, &IID_IPersistStream, (void **)&persist)) || !persist)
		return 5;
	if (FAILED(IPersistStream_Load(persist, stream)))
		return 6;
	SHORT type = 0;
	OLE_HANDLE handle = 0;
	if (FAILED(IPicture_get_Type(picture, &type)) || type != PICTYPE_ICON || FAILED(IPicture_get_Handle(picture, &handle)) || !handle)
		return 7;
	ICONINFO info;
	if (!GetIconInfo((HICON)(ULONG_PTR)handle, &info) || info.fIcon || info.xHotspot != 5 || info.yHotspot != 7)
		return 8;
	BITMAP mask;
	if (!GetObjectW(info.hbmMask, sizeof mask, &mask) || mask.bmWidth != 32)
		return 9;
	DeleteObject(info.hbmMask);
	if (info.hbmColor)
		DeleteObject(info.hbmColor);
	IPersistStream_Release(persist);
	IPicture_Release(picture);
	IStream_Release(stream);
	CoUninitialize();
	return 0;
}
