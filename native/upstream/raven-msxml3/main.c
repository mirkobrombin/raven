/* SPDX-License-Identifier: MIT */
#include <initguid.h>
#include "mx.h"

HINSTANCE mx_instance;

/* What a class hands out, and for which release of the library. */
typedef HRESULT (*versioned_create)(mx_version version, REFIID riid, void **out);
typedef HRESULT (*plain_create)(REFIID riid, void **out);

typedef struct
{
	const GUID *clsid;
	versioned_create versioned;
	plain_create plain;
	mx_version version;
	const WCHAR *prog_id, *independent, *title;
} class_entry;

#define DOM(id, version, prog, title) {&CLSID_##id, mx_document_create, NULL, version, prog, NULL, title}

/* The GUIDs headers leave out for older releases. */
DEFINE_GUID(mx_clsid_old_document, 0x2933bf90, 0x7b36, 0x11d2, 0xb2, 0x0e, 0x00, 0xc0, 0x4f, 0x98, 0x3e, 0x60);
DEFINE_GUID(CLSID_XMLSchemaCache60, 0x88d96a07, 0xf192, 0x11d4, 0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5);
DEFINE_GUID(mx_clsid_xmlhttp_old, 0xed8c108e, 0x4349, 0x11d2, 0x91, 0xa4, 0x00, 0xc0, 0x4f, 0x79, 0x69, 0xe8);
DEFINE_GUID(mx_clsid_xmlparser, 0xd2423620, 0x51a0, 0x11d2, 0x9c, 0xaf, 0x00, 0x60, 0xb0, 0xec, 0x3d, 0x39);
DEFINE_GUID(mx_clsid_xmlparser26, 0xd2423622, 0x51a0, 0x11d2, 0x9c, 0xaf, 0x00, 0x60, 0xb0, 0xec, 0x3d, 0x39);
DEFINE_GUID(mx_clsid_xmlparser30, 0xf5078f31, 0xc551, 0x11d3, 0x89, 0xb9, 0x00, 0x00, 0xf8, 0x1f, 0xe2, 0x21);

static const class_entry classes[] = {
	{&mx_clsid_old_document, mx_document_create, NULL, MX_1, L"Microsoft.XMLDOM", NULL, L"XML DOM Document"},
	DOM(DOMDocument, MX_DEFAULT, L"Msxml2.DOMDocument", L"XML DOM Document"),
	DOM(DOMDocument26, MX_26, L"Msxml2.DOMDocument.2.6", L"XML DOM Document 2.6"),
	DOM(DOMDocument30, MX_3, L"Msxml2.DOMDocument.3.0", L"XML DOM Document 3.0"),
	DOM(DOMDocument40, MX_4, L"Msxml2.DOMDocument.4.0", L"XML DOM Document 4.0"),
	DOM(DOMDocument60, MX_6, L"Msxml2.DOMDocument.6.0", L"XML DOM Document 6.0"),
	DOM(DOMFreeThreadedDocument, MX_DEFAULT, L"Microsoft.FreeThreadedXMLDOM", L"Free threaded XML DOM Document"),
	DOM(FreeThreadedDOMDocument, MX_DEFAULT, L"Msxml2.FreeThreadedDOMDocument", L"Free threaded XML DOM Document"),
	DOM(FreeThreadedDOMDocument26, MX_26, L"Msxml2.FreeThreadedDOMDocument.2.6", L"Free threaded XML DOM Document 2.6"),
	DOM(FreeThreadedDOMDocument30, MX_3, L"Msxml2.FreeThreadedDOMDocument.3.0", L"Free threaded XML DOM Document 3.0"),
	DOM(FreeThreadedDOMDocument40, MX_4, L"Msxml2.FreeThreadedDOMDocument.4.0", L"Free threaded XML DOM Document 4.0"),
	DOM(FreeThreadedDOMDocument60, MX_6, L"Msxml2.FreeThreadedDOMDocument.6.0", L"Free threaded XML DOM Document 6.0"),
	{&CLSID_XMLSchemaCache, mx_schema_cache_create, NULL, MX_DEFAULT, L"Msxml2.XMLSchemaCache", NULL, L"XML Schema Cache"},
	{&CLSID_XMLSchemaCache26, mx_schema_cache_create, NULL, MX_26, L"Msxml2.XMLSchemaCache.2.6", NULL, L"XML Schema Cache 2.6"},
	{&CLSID_XMLSchemaCache30, mx_schema_cache_create, NULL, MX_3, L"Msxml2.XMLSchemaCache.3.0", NULL, L"XML Schema Cache 3.0"},
	{&CLSID_XMLSchemaCache40, mx_schema_cache_create, NULL, MX_4, L"Msxml2.XMLSchemaCache.4.0", NULL, L"XML Schema Cache 4.0"},
	{&CLSID_XMLSchemaCache60, mx_schema_cache_create, NULL, MX_6, L"Msxml2.XMLSchemaCache.6.0", NULL, L"XML Schema Cache 6.0"},
	{&CLSID_XSLTemplate, mx_template_create, NULL, MX_DEFAULT, L"Msxml2.XSLTemplate", NULL, L"XSL Template"},
	{&CLSID_XSLTemplate26, mx_template_create, NULL, MX_26, L"Msxml2.XSLTemplate.2.6", NULL, L"XSL Template 2.6"},
	{&CLSID_XSLTemplate30, mx_template_create, NULL, MX_3, L"Msxml2.XSLTemplate.3.0", NULL, L"XSL Template 3.0"},
	{&CLSID_XSLTemplate40, mx_template_create, NULL, MX_4, L"Msxml2.XSLTemplate.4.0", NULL, L"XSL Template 4.0"},
	{&CLSID_XSLTemplate60, mx_template_create, NULL, MX_6, L"Msxml2.XSLTemplate.6.0", NULL, L"XSL Template 6.0"},
	{&CLSID_SAXXMLReader, mx_sax_reader_create, NULL, MX_DEFAULT, L"Msxml2.SAXXMLReader", NULL, L"SAX XML Reader"},
	{&CLSID_SAXXMLReader30, mx_sax_reader_create, NULL, MX_3, L"Msxml2.SAXXMLReader.3.0", NULL, L"SAX XML Reader 3.0"},
	{&CLSID_SAXXMLReader40, mx_sax_reader_create, NULL, MX_4, L"Msxml2.SAXXMLReader.4.0", NULL, L"SAX XML Reader 4.0"},
	{&CLSID_SAXXMLReader60, mx_sax_reader_create, NULL, MX_6, L"Msxml2.SAXXMLReader.6.0", NULL, L"SAX XML Reader 6.0"},
	{&CLSID_SAXAttributes, mx_sax_attributes_create, NULL, MX_DEFAULT, L"Msxml2.SAXAttributes", NULL, L"SAX Attributes"},
	{&CLSID_SAXAttributes30, mx_sax_attributes_create, NULL, MX_3, L"Msxml2.SAXAttributes.3.0", NULL, L"SAX Attributes 3.0"},
	{&CLSID_SAXAttributes40, mx_sax_attributes_create, NULL, MX_4, L"Msxml2.SAXAttributes.4.0", NULL, L"SAX Attributes 4.0"},
	{&CLSID_SAXAttributes60, mx_sax_attributes_create, NULL, MX_6, L"Msxml2.SAXAttributes.6.0", NULL, L"SAX Attributes 6.0"},
	{&CLSID_MXXMLWriter, mx_writer_create, NULL, MX_DEFAULT, L"Msxml2.MXXMLWriter", NULL, L"MX XML Writer"},
	{&CLSID_MXXMLWriter30, mx_writer_create, NULL, MX_3, L"Msxml2.MXXMLWriter.3.0", NULL, L"MX XML Writer 3.0"},
	{&CLSID_MXXMLWriter40, mx_writer_create, NULL, MX_4, L"Msxml2.MXXMLWriter.4.0", NULL, L"MX XML Writer 4.0"},
	{&CLSID_MXXMLWriter60, mx_writer_create, NULL, MX_6, L"Msxml2.MXXMLWriter.6.0", NULL, L"MX XML Writer 6.0"},
	{&CLSID_MXNamespaceManager, mx_namespace_manager_create, NULL, MX_DEFAULT, L"Msxml2.MXNamespaceManager", NULL, L"MX Namespace Manager"},
	{&CLSID_MXNamespaceManager40, mx_namespace_manager_create, NULL, MX_4, L"Msxml2.MXNamespaceManager.4.0", NULL, L"MX Namespace Manager 4.0"},
	{&CLSID_MXNamespaceManager60, mx_namespace_manager_create, NULL, MX_6, L"Msxml2.MXNamespaceManager.6.0", NULL, L"MX Namespace Manager 6.0"},
	{&CLSID_XMLHTTP, NULL, mx_http_create, MX_DEFAULT, L"Msxml2.XMLHTTP", NULL, L"XML HTTP"},
	{&mx_clsid_xmlhttp_old, NULL, mx_http_create, MX_DEFAULT, L"Microsoft.XMLHTTP", NULL, L"XML HTTP"},
	{&CLSID_XMLHTTP26, NULL, mx_http_create, MX_26, L"Msxml2.XMLHTTP.2.6", NULL, L"XML HTTP 2.6"},
	{&CLSID_XMLHTTP30, NULL, mx_http_create, MX_3, L"Msxml2.XMLHTTP.3.0", NULL, L"XML HTTP 3.0"},
	{&CLSID_XMLHTTP40, NULL, mx_http_create, MX_4, L"Msxml2.XMLHTTP.4.0", NULL, L"XML HTTP 4.0"},
	{&CLSID_XMLHTTP60, NULL, mx_http_create, MX_6, L"Msxml2.XMLHTTP.6.0", NULL, L"XML HTTP 6.0"},
	{&CLSID_ServerXMLHTTP, NULL, mx_server_http_create, MX_DEFAULT, L"Msxml2.ServerXMLHTTP", NULL, L"Server XML HTTP"},
	{&CLSID_ServerXMLHTTP30, NULL, mx_server_http_create, MX_3, L"Msxml2.ServerXMLHTTP.3.0", NULL, L"Server XML HTTP 3.0"},
	{&CLSID_ServerXMLHTTP40, NULL, mx_server_http_create, MX_4, L"Msxml2.ServerXMLHTTP.4.0", NULL, L"Server XML HTTP 4.0"},
	{&CLSID_ServerXMLHTTP60, NULL, mx_server_http_create, MX_6, L"Msxml2.ServerXMLHTTP.6.0", NULL, L"Server XML HTTP 6.0"},
	{&CLSID_XMLDocument, NULL, mx_legacy_document_create, MX_DEFAULT, L"Msxml", NULL, L"XML Document"},
	{&mx_clsid_xmlparser, NULL, mx_parser_create, MX_DEFAULT, L"Msxml2.XMLParser", NULL, L"XML Parser"},
	{&mx_clsid_xmlparser26, NULL, mx_parser_create, MX_26, L"Msxml2.XMLParser.2.6", NULL, L"XML Parser 2.6"},
	{&mx_clsid_xmlparser30, NULL, mx_parser_create, MX_3, L"Msxml2.XMLParser.3.0", NULL, L"XML Parser 3.0"},
};

/* --- Class objects. --- */

typedef struct
{
	const IClassFactoryVtbl *vtbl;
	const class_entry *entry;
	LONG refs;
} class_factory;

static LONG module_locks;

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!IsEqualGUID(riid, &IID_IUnknown) && !IsEqualGUID(riid, &IID_IClassFactory))
		return E_NOINTERFACE;
	*out = iface;
	IClassFactory_AddRef(iface);
	return S_OK;
}

static ULONG WINAPI factory_AddRef(IClassFactory *iface)
{
	return InterlockedIncrement(&((class_factory *)iface)->refs);
}

static ULONG WINAPI factory_Release(IClassFactory *iface)
{
	class_factory *self = (class_factory *)iface;
	ULONG left = InterlockedDecrement(&self->refs);
	if (!left)
		mx_free(self);
	return left;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
	const class_entry *entry = ((class_factory *)iface)->entry;
	if (!out)
		return E_POINTER;
	*out = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return entry->versioned ? entry->versioned(entry->version, riid, out) : entry->plain(riid, out);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock)
{
	if (lock)
		InterlockedIncrement(&module_locks);
	else
		InterlockedDecrement(&module_locks);
	return S_OK;
}

static const IClassFactoryVtbl factory_table = {
	factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer,
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
	unsigned index;
	if (!out)
		return E_POINTER;
	*out = NULL;
	for (index = 0; index < ARRAYSIZE(classes); index++)
		if (IsEqualGUID(clsid, classes[index].clsid))
		{
			class_factory *factory = mx_alloc(sizeof(*factory));
			HRESULT status;
			if (!factory)
				return E_OUTOFMEMORY;
			factory->vtbl = &factory_table;
			factory->entry = &classes[index];
			factory->refs = 1;
			status = IClassFactory_QueryInterface((IClassFactory *)factory, riid, out);
			IClassFactory_Release((IClassFactory *)factory);
			return status;
		}
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return S_FALSE;
}

/* --- Registration. --- */

static LSTATUS write_string(HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *value)
{
	HKEY key;
	LSTATUS status = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
	if (status)
		return status;
	status = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
	RegCloseKey(key);
	return status;
}

static void guid_text(const GUID *guid, WCHAR *text)
{
	StringFromGUID2(guid, text, 39);
}

static const struct { const WCHAR *prog_id; const GUID *clsid; } extra_names[] = {
	{L"Microsoft.XMLDOM", &CLSID_DOMDocument},
	{L"Microsoft.XMLDOM.1.0", &CLSID_DOMDocument},
	{L"MSXML.DOMDocument", &CLSID_DOMDocument},
	{L"Msxml2.DOMDocument.3.0", &CLSID_DOMDocument30},
	{L"MSXML2.DOMDocument", &CLSID_DOMDocument},
	{L"Microsoft.FreeThreadedXMLDOM.1.0", &CLSID_DOMFreeThreadedDocument},
	{L"MSXML.FreeThreadedDOMDocument", &CLSID_DOMFreeThreadedDocument},
	{L"Microsoft.XMLHTTP.1.0", &mx_clsid_xmlhttp_old},
	{L"MSXML2.XSLTemplate", &CLSID_XSLTemplate},
	{L"MSXML.XMLSchemaCache", &CLSID_XMLSchemaCache},
	{L"Microsoft.XMLParser", &mx_clsid_xmlparser},
};

HRESULT WINAPI DllRegisterServer(void)
{
	WCHAR module[MAX_PATH], path[128], guid[39];
	unsigned index;
	ITypeLib *library;
	HRESULT status;
	GetModuleFileNameW(mx_instance, module, MAX_PATH);
	for (index = 0; index < ARRAYSIZE(classes); index++)
	{
		const class_entry *entry = &classes[index];
		guid_text(entry->clsid, guid);
		wsprintfW(path, L"CLSID\\%s", guid);
		write_string(HKEY_CLASSES_ROOT, path, NULL, entry->title);
		wsprintfW(path, L"CLSID\\%s\\InprocServer32", guid);
		write_string(HKEY_CLASSES_ROOT, path, NULL, module);
		write_string(HKEY_CLASSES_ROOT, path, L"ThreadingModel", L"Both");
		wsprintfW(path, L"CLSID\\%s\\ProgID", guid);
		write_string(HKEY_CLASSES_ROOT, path, NULL, entry->prog_id);
		write_string(HKEY_CLASSES_ROOT, entry->prog_id, NULL, entry->title);
		wsprintfW(path, L"%s\\CLSID", entry->prog_id);
		write_string(HKEY_CLASSES_ROOT, path, NULL, guid);
	}
	for (index = 0; index < ARRAYSIZE(extra_names); index++)
	{
		guid_text(extra_names[index].clsid, guid);
		wsprintfW(path, L"%s\\CLSID", extra_names[index].prog_id);
		write_string(HKEY_CLASSES_ROOT, path, NULL, guid);
	}
	status = LoadTypeLibEx(module, REGKIND_REGISTER, &library);
	if (SUCCEEDED(status))
		ITypeLib_Release(library);
	return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	WCHAR path[128], guid[39];
	unsigned index;
	for (index = 0; index < ARRAYSIZE(classes); index++)
	{
		guid_text(classes[index].clsid, guid);
		wsprintfW(path, L"CLSID\\%s", guid);
		RegDeleteTreeW(HKEY_CLASSES_ROOT, path);
		RegDeleteTreeW(HKEY_CLASSES_ROOT, classes[index].prog_id);
	}
	for (index = 0; index < ARRAYSIZE(extra_names); index++)
		RegDeleteTreeW(HKEY_CLASSES_ROOT, extra_names[index].prog_id);
	return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		mx_instance = instance;
		xmlInitParser();
		break;
	case DLL_PROCESS_DETACH:
		if (!reserved)
		{
			mx_release_type_infos();
			xmlCleanupParser();
		}
		break;
	}
	return TRUE;
}
