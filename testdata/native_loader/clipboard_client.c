/* Round trips several clipboard formats inside the process and returns 0, or the number of the first failing check. */
#define COBJMACROS
#include <windows.h>
#include <ole2.h>

static int same_bytes(const void *left, const void *right, unsigned size)
{
	const unsigned char *a = left, *b = right;
	for (unsigned index = 0; index < size; ++index)
		if (a[index] != b[index])
			return 0;
	return 1;
}

unsigned int raven_entry(void)
{
	static const unsigned char payload[] = {1, 2, 3, 250, 251, 0, 7};
	static const WCHAR text[] = {'r', 'a', 'v', 'e', 'n', 0};
	UINT custom = RegisterClipboardFormatW(L"RavenTestFormat");
	if (custom < 0xC000 || RegisterClipboardFormatW(L"RavenTestFormat") != custom)
		return 1;
	WCHAR name[32];
	if (GetClipboardFormatNameW(custom, name, 32) != 15 || name[0] != 'R' || name[14] != 't')
		return 2;
	HGLOBAL blob = GlobalAlloc(GMEM_MOVEABLE, sizeof payload), words = GlobalAlloc(GMEM_MOVEABLE, sizeof text);
	if (!blob || !words)
		return 3;
	void *bytes = GlobalLock(blob);
	for (unsigned index = 0; index < sizeof payload; ++index)
		((unsigned char *)bytes)[index] = payload[index];
	GlobalUnlock(blob);
	WCHAR *wide = GlobalLock(words);
	for (unsigned index = 0; index < 6; ++index)
		wide[index] = text[index];
	GlobalUnlock(words);
	if (!OpenClipboard(NULL) || !EmptyClipboard())
		return 4;
	if (SetClipboardData(custom, blob) != blob || SetClipboardData(CF_UNICODETEXT, words) != words)
		return 5;
	if (!CloseClipboard())
		return 6;
	if (!IsClipboardFormatAvailable(custom) || !IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_HDROP))
		return 7;
	if (!OpenClipboard(NULL))
		return 8;
	HANDLE got = GetClipboardData(custom);
	if (got != blob || !same_bytes(GlobalLock(got), payload, sizeof payload) || GlobalSize(got) < sizeof payload)
		return 9;
	GlobalUnlock(got);
	if (GetClipboardData(CF_HDROP) != NULL)
		return 10;
	int seen_custom = 0, seen_text = 0;
	for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format)) {
		seen_custom |= format == custom;
		seen_text |= format == CF_UNICODETEXT;
	}
	if (!seen_custom || !seen_text)
		return 11;
	/* The OLE clipboard shows the same data: the formats it lists, the ones it has, and a copy of each that the caller frees. */
	CloseClipboard();
	IDataObject *data = NULL;
	FORMATETC wanted = {(CLIPFORMAT)custom, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	STGMEDIUM medium;
	if (OleGetClipboard(&data) || !data)
		return 13;
	if (IDataObject_QueryGetData(data, &wanted) != S_OK)
		return 14;
	if (IDataObject_GetData(data, &wanted, &medium) || medium.tymed != TYMED_HGLOBAL || medium.hGlobal == blob ||
		!same_bytes(GlobalLock(medium.hGlobal), payload, sizeof payload))
		return 15;
	GlobalUnlock(medium.hGlobal);
	ReleaseStgMedium(&medium);
	wanted.cfFormat = CF_HDROP;
	if (IDataObject_QueryGetData(data, &wanted) != DV_E_FORMATETC)
		return 16;
	IEnumFORMATETC *formats = NULL;
	FORMATETC listed;
	int listed_custom = 0;
	if (IDataObject_EnumFormatEtc(data, DATADIR_GET, &formats) || !formats)
		return 17;
	while (IEnumFORMATETC_Next(formats, 1, &listed, NULL) == S_OK)
		listed_custom |= listed.cfFormat == custom;
	IEnumFORMATETC_Release(formats);
	IDataObject_Release(data);
	if (!listed_custom)
		return 18;
	if (!OpenClipboard(NULL))
		return 19;
	if (!EmptyClipboard() || IsClipboardFormatAvailable(custom) || CountClipboardFormats() != 0)
		return 12;
	CloseClipboard();
	return 0;
}
