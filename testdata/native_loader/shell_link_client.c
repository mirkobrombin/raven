// A shell link keeps properties as well as a path: a jump list gives each task its title through the link's IPropertyStore, as
// MusicBee does when its taskbar button is made. The exit code names the first check that failed.
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <propsys.h>

static const CLSID link_class = {0x00021401, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const PROPERTYKEY title_key = {{0xf29f85e0, 0x4ff9, 0x1068, {0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9}}, 2};

int raven_entry(void)
{
	if (FAILED(CoInitialize(NULL)))
		return 1;
	IShellLinkW *link = NULL;
	if (FAILED(CoCreateInstance(&link_class, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)) || !link)
		return 2;
	IShellLinkW_SetPath(link, L"C:\\windows\\notepad.exe");
	IPropertyStore *store = NULL;
	if (FAILED(IShellLinkW_QueryInterface(link, &IID_IPropertyStore, (void **)&store)) || !store)
		return 3;
	PROPVARIANT title = {0};
	title.vt = VT_LPWSTR;
	title.pwszVal = L"Play next";
	if (FAILED(IPropertyStore_SetValue(store, &title_key, &title)) || FAILED(IPropertyStore_Commit(store)))
		return 4;
	DWORD count = 0;
	PROPERTYKEY key;
	if (FAILED(IPropertyStore_GetCount(store, &count)) || count != 1 || FAILED(IPropertyStore_GetAt(store, 0, &key)) || key.pid != 2)
		return 5;
	PROPVARIANT read = {0};
	if (FAILED(IPropertyStore_GetValue(store, &title_key, &read)) || read.vt != VT_LPWSTR || lstrcmpW(read.pwszVal, L"Play next"))
		return 6;
	CoTaskMemFree(read.pwszVal);
	/* The store is a face of the link: it leads back to the link's other interfaces. */
	IShellLinkW *again = NULL;
	if (FAILED(IPropertyStore_QueryInterface(store, &IID_IShellLinkW, (void **)&again)) || again != link)
		return 7;
	IShellLinkW_Release(again);
	IPropertyStore_Release(store);
	IShellLinkW_Release(link);
	CoUninitialize();
	return 0;
}
