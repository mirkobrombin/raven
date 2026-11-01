#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#include <xmllite.h>

static int contains(const char *text, ULONG size, const char *part)
{
	for (ULONG at = 0; at < size; ++at) {
		ULONG index = 0;
		while (part[index] && at + index < size && text[at + index] == part[index])
			++index;
		if (!part[index])
			return 1;
	}
	return 0;
}

/* Programs compare what the reader answers by address: the empty string, a namespace and a prefix are one string each, from the declaration on. */
static int string_identity(void)
{
	static const char document[] = "<elem xmlns=\"myns\" xmlns:p=\"pns\"><p:inner p:attr=\"\"/>text</elem>";
	static const LARGE_INTEGER start;
	IStream *stream;
	IXmlReader *reader;
	XmlNodeType type;
	const WCHAR *name, *uri, *empty, *value, *text, *xmlns_name, *pns, *p;
	if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK || IStream_Write(stream, document, sizeof document - 1, NULL) != S_OK ||
		IStream_Seek(stream, start, STREAM_SEEK_SET, NULL) != S_OK || CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL) != S_OK ||
		IXmlReader_SetInput(reader, (IUnknown *)stream) != S_OK)
		return 40;
	if (IXmlReader_Read(reader, &type) != S_OK || type != XmlNodeType_Element)
		return 41;
	IXmlReader_GetValue(reader, &empty, NULL);
	IXmlReader_GetPrefix(reader, &text, NULL);
	if (text != empty)
		return 42;
	IXmlReader_GetLocalName(reader, &name, NULL);
	IXmlReader_GetQualifiedName(reader, &text, NULL);
	IXmlReader_GetNamespaceUri(reader, &uri, NULL);
	if (text != name)
		return 43;
	if (IXmlReader_MoveToNextAttribute(reader) != S_OK)
		return 44;
	IXmlReader_GetValue(reader, &value, NULL);
	IXmlReader_GetPrefix(reader, &text, NULL);
	IXmlReader_GetLocalName(reader, &xmlns_name, NULL);
	if (value != uri || text != empty)
		return 45;
	IXmlReader_GetQualifiedName(reader, &text, NULL);
	if (text != xmlns_name)
		return 46;
	if (IXmlReader_MoveToNextAttribute(reader) != S_OK)
		return 47;
	IXmlReader_GetValue(reader, &pns, NULL);
	IXmlReader_GetPrefix(reader, &text, NULL);
	IXmlReader_GetLocalName(reader, &p, NULL);
	if (text != xmlns_name)
		return 48;
	if (IXmlReader_Read(reader, &type) != S_OK || type != XmlNodeType_Element)
		return 49;
	IXmlReader_GetPrefix(reader, &text, NULL);
	IXmlReader_GetNamespaceUri(reader, &value, NULL);
	if (text != p || value != pns)
		return 50;
	if (IXmlReader_MoveToNextAttribute(reader) != S_OK)
		return 51;
	IXmlReader_GetPrefix(reader, &text, NULL);
	IXmlReader_GetNamespaceUri(reader, &value, NULL);
	if (text != p || value != pns)
		return 52;
	IXmlReader_GetValue(reader, &value, NULL);
	if (value == empty)
		return 53;
	if (IXmlReader_Read(reader, &type) != S_OK || type != XmlNodeType_Text)
		return 54;
	IXmlReader_GetNamespaceUri(reader, &value, NULL);
	IXmlReader_GetLocalName(reader, &text, NULL);
	if (value != empty || text != empty)
		return 55;
	if (IXmlReader_Read(reader, &type) != S_OK || type != XmlNodeType_EndElement)
		return 56;
	IXmlReader_GetLocalName(reader, &text, NULL);
	IXmlReader_GetNamespaceUri(reader, &value, NULL);
	if (text != name || value != uri)
		return 57;
	IXmlReader_Release(reader);
	IStream_Release(stream);
	return 0;
}

/* The streaming XML reader and writer: a configuration read node by node from a stream, with an element's attribute and text, and a
 * document written to a stream. */
int raven_entry(void)
{
	static const char document[] = "<?xml version=\"1.0\"?><Configuration><Add Channel=\"Current\">ready</Add></Configuration>";
	IStream *input = NULL, *output = NULL;
	IXmlReader *reader = NULL;
	IXmlWriter *writer = NULL;
	if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 1;
	if (CreateStreamOnHGlobal(NULL, TRUE, &input) != S_OK || IStream_Write(input, document, sizeof document - 1, NULL) != S_OK) return 2;
	LARGE_INTEGER start = {0};
	IStream_Seek(input, start, STREAM_SEEK_SET, NULL);
	if (CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL) != S_OK || !reader) return 3;
	if (IXmlReader_SetInput(reader, (IUnknown *)input) != S_OK) return 4;
	XmlNodeType type;
	const WCHAR *name = NULL, *value = NULL;
	int elements = 0, attribute = 0, text = 0;
	while (IXmlReader_Read(reader, &type) == S_OK) {
		if (type == XmlNodeType_Element) {
			if (IXmlReader_GetLocalName(reader, &name, NULL) != S_OK) return 5;
			if (!lstrcmpW(name, L"Configuration") && elements == 0)
				++elements;
			else if (!lstrcmpW(name, L"Add") && elements == 1) {
				++elements;
				if (IXmlReader_MoveToAttributeByName(reader, L"Channel", NULL) != S_OK || IXmlReader_GetValue(reader, &value, NULL) != S_OK) return 6;
				attribute = !lstrcmpW(value, L"Current");
			}
		} else if (type == XmlNodeType_Text) {
			if (IXmlReader_GetValue(reader, &value, NULL) != S_OK) return 7;
			text = !lstrcmpW(value, L"ready");
		}
	}
	if (elements != 2 || !attribute || !text) return 8;
	if (CreateStreamOnHGlobal(NULL, TRUE, &output) != S_OK) return 9;
	if (CreateXmlWriter(&IID_IXmlWriter, (void **)&writer, NULL) != S_OK || !writer) return 10;
	if (IXmlWriter_SetOutput(writer, (IUnknown *)output) != S_OK || IXmlWriter_WriteStartElement(writer, NULL, L"Product", NULL) != S_OK ||
		IXmlWriter_WriteAttributeString(writer, NULL, L"ID", NULL, L"O365ProPlusRetail") != S_OK || IXmlWriter_WriteEndElement(writer) != S_OK ||
		IXmlWriter_Flush(writer) != S_OK) return 11;
	HGLOBAL memory = NULL;
	STATSTG stat;
	if (GetHGlobalFromStream(output, &memory) != S_OK || IStream_Stat(output, &stat, STATFLAG_NONAME) != S_OK) return 12;
	const char *written = GlobalLock(memory);
	if (!written || !contains(written, stat.cbSize.LowPart, "<Product ID=\"O365ProPlusRetail\"")) return 13;
	GlobalUnlock(memory);
	IXmlWriter_Release(writer);
	IXmlReader_Release(reader);
	IStream_Release(output);
	IStream_Release(input);
	{
		/* A document copied whole from a reader into a writer, declaration and an empty default namespace included, as programs load
		 * a file into a writer to keep it. */
		static const char copied[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<R Id=\"7\" xmlns=\"\">\r\n  <S>\r\n    <U T=\"1\" />\r\n  </S>\r\n</R>";
		HRESULT result;
		if (CreateStreamOnHGlobal(NULL, TRUE, &input) != S_OK || IStream_Write(input, copied, sizeof copied - 1, NULL) != S_OK) return 14;
		IStream_Seek(input, start, STREAM_SEEK_SET, NULL);
		if (CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL) != S_OK || IXmlReader_SetInput(reader, (IUnknown *)input) != S_OK) return 15;
		if (CreateStreamOnHGlobal(NULL, TRUE, &output) != S_OK || CreateXmlWriter(&IID_IXmlWriter, (void **)&writer, NULL) != S_OK ||
			IXmlWriter_SetOutput(writer, (IUnknown *)output) != S_OK) return 16;
		while (!IXmlReader_IsEOF(reader))
			if ((result = IXmlWriter_WriteNode(writer, reader, TRUE)) != S_OK && result != S_FALSE)
				return 100 + (int)(result & 0xff);
		if (IXmlWriter_Flush(writer) != S_OK) return 17;
		if (GetHGlobalFromStream(output, &memory) != S_OK || IStream_Stat(output, &stat, STATFLAG_NONAME) != S_OK) return 18;
		written = GlobalLock(memory);
		if (!written || !contains(written, stat.cbSize.LowPart, "<U T=\"1\"") || !contains(written, stat.cbSize.LowPart, "</R>")) return 19;
		GlobalUnlock(memory);
		IXmlWriter_Release(writer);
		IXmlReader_Release(reader);
		IStream_Release(output);
		IStream_Release(input);
	}
	return string_identity();
}
