#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <msxml2.h>

/* The XML document object as installers and their bootstrappers use it: found by its program identifier through the registry, made by
   the library registered for its class, parsing a document and answering an XPath query; then loading the same document from a file in a
   folder named like a bootstrapper's, through its URL. Last, a schema whose patterns use the code unit escapes of MSXML 6 inside character
   classes, as the deployment schema of Click-to-Run Office does, validating one document that matches and one that does not. */

static int validate(const WCHAR *xml, HRESULT *result)
{
	CLSID clsid;
	IXMLDOMDocument2 *schema = NULL, *document = NULL;
	IXMLDOMSchemaCollection *cache = NULL;
	IXMLDOMParseError *error = NULL;
	VARIANT_BOOL loaded = VARIANT_FALSE;
	BSTR text, name;
	VARIANT value;
	if (CLSIDFromProgID(L"Msxml2.DOMDocument.6.0", &clsid) != S_OK ||
		CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument2, (void **)&schema) != S_OK ||
		CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument2, (void **)&document) != S_OK)
		return 11;
	text = SysAllocString(L"<xs:schema xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" targetNamespace=\"urn:raven\" elementFormDefault=\"qualified\">"
		L"<xs:element name=\"v\"><xs:simpleType><xs:restriction base=\"xs:string\">"
		L"<xs:pattern value=\"[\\u0041-\\u0043]+[^&quot;\\u0000-\\u0020\\u007F]*\"/>"
		L"</xs:restriction></xs:simpleType></xs:element></xs:schema>");
	if (IXMLDOMDocument2_loadXML(schema, text, &loaded) != S_OK || loaded != VARIANT_TRUE)
		return 12;
	SysFreeString(text);
	if (CLSIDFromProgID(L"Msxml2.XMLSchemaCache.6.0", &clsid) != S_OK ||
		CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMSchemaCollection, (void **)&cache) != S_OK)
		return 13;
	name = SysAllocString(L"urn:raven");
	V_VT(&value) = VT_DISPATCH;
	V_DISPATCH(&value) = (IDispatch *)schema;
	if (IXMLDOMSchemaCollection_add(cache, name, value) != S_OK)
		return 14;
	V_VT(&value) = VT_DISPATCH;
	V_DISPATCH(&value) = (IDispatch *)cache;
	if (IXMLDOMDocument2_putref_schemas(document, value) != S_OK)
		return 15;
	text = SysAllocString(xml);
	loaded = VARIANT_FALSE;
	if (IXMLDOMDocument2_put_validateOnParse(document, VARIANT_FALSE) != S_OK || IXMLDOMDocument2_loadXML(document, text, &loaded) != S_OK ||
		loaded != VARIANT_TRUE)
		return 16;
	if (IXMLDOMDocument2_validate(document, &error) == E_NOTIMPL)
		return 17;
	*result = S_OK;
	if (error)
		IXMLDOMParseError_get_errorCode(error, (LONG *)result);
	if (error)
		IXMLDOMParseError_Release(error);
	SysFreeString(text);
	SysFreeString(name);
	IXMLDOMSchemaCollection_Release(cache);
	IXMLDOMDocument2_Release(document);
	IXMLDOMDocument2_Release(schema);
	return 0;
}

int raven_entry(void)
{
	CLSID clsid;
	IXMLDOMDocument *document = NULL;
	IXMLDOMNode *node = NULL;
	VARIANT_BOOL loaded = VARIANT_FALSE;
	BSTR text = NULL, query, value = NULL;
	if (FAILED(CoInitialize(NULL)))
		return 1;
	if (CLSIDFromProgID(L"Msxml2.DOMDocument", &clsid) != S_OK)
		return 2;
	if (CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument, (void **)&document) != S_OK || !document)
		return 3;
	text = SysAllocString(L"<bundle><package id=\"vcRuntime\" size=\"4242\">runtime</package></bundle>");
	if (IXMLDOMDocument_loadXML(document, text, &loaded) != S_OK || loaded != VARIANT_TRUE)
		return 4;
	query = SysAllocString(L"/bundle/package/@size");
	if (IXMLDOMDocument_selectSingleNode(document, query, &node) != S_OK || !node)
		return 5;
	if (IXMLDOMNode_get_text(node, &value) != S_OK || !value || lstrcmpW(value, L"4242"))
		return 6;
	SysFreeString(value);
	IXMLDOMNode_Release(node);
	/* A document far longer than a path: its text is converted whole, in both directions. */
	enum { long_text = 2 * 1024 * 1024 };
	WCHAR *wide_text = GlobalAlloc(GMEM_FIXED, (long_text + 1) * sizeof(WCHAR));
	char *narrow_text = GlobalAlloc(GMEM_FIXED, long_text + 1);
	if (!wide_text || !narrow_text)
		return 90;
	for (int at = 0; at < long_text; at++)
		narrow_text[at] = 'x';
	narrow_text[0] = '<', narrow_text[1] = 'a', narrow_text[2] = '>';
	narrow_text[long_text - 4] = '<', narrow_text[long_text - 3] = '/', narrow_text[long_text - 2] = 'a', narrow_text[long_text - 1] = '>';
	narrow_text[long_text] = 0;
	if (MultiByteToWideChar(CP_UTF8, 0, narrow_text, -1, wide_text, long_text + 1) != long_text + 1)
		return 91;
	BSTR long_document = SysAllocString(wide_text);
	if (IXMLDOMDocument_loadXML(document, long_document, &loaded) != S_OK || loaded != VARIANT_TRUE)
		return 92;
	SysFreeString(long_document);
	GlobalFree(wide_text);
	GlobalFree(narrow_text);
	WCHAR folder[MAX_PATH], path[MAX_PATH];
	GetTempPathW(MAX_PATH, folder);
	lstrcatW(folder, L"{F2DA498E-66A3-4183-B794-FD6A62ED92B0}");
	CreateDirectoryW(folder, NULL);
	lstrcatW(folder, L"\\.ba");
	CreateDirectoryW(folder, NULL);
	lstrcpyW(path, folder);
	lstrcatW(path, L"\\BootstrapperApplicationData.xml");
	static const char data[] = "<bundle><package id=\"vcRuntime\" size=\"4343\">runtime</package></bundle>";
	DWORD written;
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, data, sizeof data - 1, &written, NULL) || !CloseHandle(file))
		return 7;
	VARIANT source;
	VariantInit(&source);
	V_VT(&source) = VT_BSTR;
	V_BSTR(&source) = SysAllocString(path);
	loaded = VARIANT_FALSE;
	if (IXMLDOMDocument_load(document, source, &loaded) != S_OK || loaded != VARIANT_TRUE)
		return 8;
	if (IXMLDOMDocument_selectSingleNode(document, query, &node) != S_OK || !node)
		return 9;
	if (IXMLDOMNode_get_text(node, &value) != S_OK || !value || lstrcmpW(value, L"4343"))
		return 10;
	DeleteFileW(path);
	SysFreeString(value);
	VariantClear(&source);
	IXMLDOMNode_Release(node);
	node = NULL;
	/* A query that matches nothing answers S_FALSE with no node, and one that does not parse fails. */
	BSTR missing = SysAllocString(L"/bundle/absent"), broken = SysAllocString(L"/bundle/[");
	if (IXMLDOMDocument_selectSingleNode(document, missing, &node) != S_FALSE || node)
		return 20;
	if (SUCCEEDED(IXMLDOMDocument_selectSingleNode(document, broken, &node)) || node)
		return 21;
	SysFreeString(missing);
	SysFreeString(broken);
	/* A malformed document does not load: S_FALSE, no document element, and a parse error with a code, a reason and a line. */
	BSTR malformed = SysAllocString(L"<bundle>\n<package></bundle>");
	IXMLDOMElement *root = NULL;
	IXMLDOMParseError *error = NULL;
	BSTR reason = NULL;
	long code = 0, line = 0;
	loaded = VARIANT_TRUE;
	if (IXMLDOMDocument_loadXML(document, malformed, &loaded) != S_FALSE || loaded != VARIANT_FALSE)
		return 22;
	if (IXMLDOMDocument_get_documentElement(document, &root) != S_FALSE || root)
		return 23;
	if (IXMLDOMDocument_get_parseError(document, &error) != S_OK || !error || IXMLDOMParseError_get_errorCode(error, &code) != S_OK || code >= 0 ||
		IXMLDOMParseError_get_reason(error, &reason) != S_OK || !reason || !reason[0] || IXMLDOMParseError_get_line(error, &line) != S_OK || line < 1)
		return 24;
	SysFreeString(reason);
	IXMLDOMParseError_Release(error);
	SysFreeString(malformed);
	SysFreeString(query);
	SysFreeString(text);
	IXMLDOMDocument_Release(document);
	HRESULT matched = E_FAIL, refused = S_OK;
	int failed = validate(L"<v xmlns=\"urn:raven\">CAB-x</v>", &matched);
	if (failed)
		return failed;
	if (matched != S_OK)
		return 18;
	if ((failed = validate(L"<v xmlns=\"urn:raven\">CA B</v>", &refused)))
		return failed;
	if (refused == S_OK)
		return 19;
	CoUninitialize();
	return 0;
}
