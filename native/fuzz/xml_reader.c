/* The XmlLite reader of xmllite.dll (native/guest/raven-xmllite/reader.c), which reads the manifests, configuration files and package
   descriptions installers and programs hand it, fed changed documents on the host under the address and undefined behaviour sanitizers.
   The seeds are small documents of each construct the reader tells apart: declarations with encodings, namespaces and prefixes,
   attributes, character and entity references, CDATA, comments, processing instructions, a document type, deep nesting and UTF-16 with
   its byte order mark. Each round changes one: flipped bytes, inserted markup, cut or repeated runs. It is read to its end or its first
   error with the document type allowed or not, as a document or a fragment, through an input that names an encoding or none, and every
   name, value and attribute of every node is read whole. Any document may be refused; nothing may be read or written outside what was
   allocated.

   cc -fshort-wchar -fsanitize=address,undefined -fno-sanitize-recover=all -Inative/fuzz/xml -Inative/guest/raven-xmllite
      native/fuzz/xml_reader.c native/guest/raven-xmllite/reader.c
   ./a.out [rounds] [seed] */

#include <stdio.h>
#include <stdlib.h>

#include "shared.h"

const IID IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
const IID IID_ISequentialStream = {0x0c733a30, 0x2a1c, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};
const IID IID_IXmlReader = {0x7279fc81, 0x709d, 0x4095, {0xb6, 0x3d, 0x69, 0xfe, 0x4b, 0x0d, 0x90, 0x30}};

static uint64_t state = 0x9e3779b97f4a7c15ull;

static uint64_t next(void)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

HANDLE GetProcessHeap(void)
{
	return (HANDLE)1;
}

void *HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
	(void)heap;
	return flags & HEAP_ZERO_MEMORY ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
}

void *HeapReAlloc(HANDLE heap, DWORD flags, void *block, SIZE_T size)
{
	(void)heap;
	(void)flags;
	return realloc(block, size ? size : 1);
}

BOOL HeapFree(HANDLE heap, DWORD flags, void *block)
{
	(void)heap;
	(void)flags;
	free(block);
	return TRUE;
}

LONG InterlockedIncrement(LONG volatile *value)
{
	return ++*value;
}

LONG InterlockedDecrement(LONG volatile *value)
{
	return --*value;
}

int lstrlenW(const WCHAR *text)
{
	int length = 0;

	while (text[length])
		++length;
	return length;
}

static WCHAR lower(WCHAR unit)
{
	return unit >= 'A' && unit <= 'Z' ? (WCHAR)(unit + 32) : unit;
}

int lstrcmpW(const WCHAR *left, const WCHAR *right)
{
	for (;; ++left, ++right) {
		const int difference = (int)*left - (int)*right;
		if (difference || !*left)
			return difference;
	}
}

int lstrcmpiW(const WCHAR *left, const WCHAR *right)
{
	for (;; ++left, ++right) {
		const int difference = (int)lower(*left) - (int)lower(*right);
		if (difference || !*left)
			return difference;
	}
}

/* UTF-8 as Windows decodes it, a malformed sequence giving U+FFFD; any other page byte for byte, which is enough for a reader that only
   looks at the units it gets. */
int MultiByteToWideChar(UINT code_page, DWORD flags, const char *bytes, int count, WCHAR *wide, int capacity)
{
	const unsigned char *at = (const unsigned char *)bytes, *end = at + count;
	int length = 0;

	(void)flags;
	while (at < end) {
		uint32_t point = *at++;
		if (code_page == CP_UTF8 && point >= 0x80) {
			int more = point >= 0xf0 && point < 0xf8 ? 3 : point >= 0xe0 ? 2 : point >= 0xc2 && point < 0xe0 ? 1 : -1;
			point = more == 3 ? point & 7 : more == 2 ? point & 15 : point & 31;
			for (int index = 0; index < more; ++index) {
				if (at >= end || (*at & 0xc0) != 0x80) {
					more = -1;
					break;
				}
				point = point << 6 | (*at++ & 0x3f);
			}
			if (more < 0 || point > 0x10ffff || (point >= 0xd800 && point < 0xe000))
				point = 0xfffd;
		}
		const int units = point >= 0x10000 ? 2 : 1;
		if (wide) {
			if (length + units > capacity)
				return 0;
			if (units == 2) {
				wide[length] = (WCHAR)(0xd800 + ((point - 0x10000) >> 10));
				wide[length + 1] = (WCHAR)(0xdc00 + ((point - 0x10000) & 0x3ff));
			} else
				wide[length] = (WCHAR)point;
		}
		length += units;
	}
	return length;
}

/* The bytes of one document, handed out in pieces of random size. */
struct stream {
	ISequentialStream iface;
	LONG references;
	const BYTE *data;
	size_t size, at;
};

static struct stream *stream_of(ISequentialStream *iface)
{
	return CONTAINING_RECORD(iface, struct stream, iface);
}

static HRESULT WINAPI stream_QueryInterface(ISequentialStream *iface, REFIID riid, void **object)
{
	if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ISequentialStream)) {
		*object = iface;
		++stream_of(iface)->references;
		return S_OK;
	}
	*object = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI stream_AddRef(ISequentialStream *iface)
{
	return (ULONG)++stream_of(iface)->references;
}

static ULONG WINAPI stream_Release(ISequentialStream *iface)
{
	return (ULONG)--stream_of(iface)->references;
}

static HRESULT WINAPI stream_Read(ISequentialStream *iface, void *buffer, ULONG count, ULONG *done)
{
	struct stream *stream = stream_of(iface);
	size_t left = stream->size - stream->at, piece = 1 + (size_t)(next() % 5000);

	if (piece > count)
		piece = count;
	if (piece > left)
		piece = left;
	memcpy(buffer, stream->data + stream->at, piece);
	stream->at += piece;
	if (done)
		*done = (ULONG)piece;
	return piece ? S_OK : S_FALSE;
}

static HRESULT WINAPI stream_Write(ISequentialStream *iface, const void *buffer, ULONG count, ULONG *done)
{
	(void)iface;
	(void)buffer;
	(void)count;
	if (done)
		*done = 0;
	return E_FAIL;
}

static ISequentialStreamVtbl stream_vtable = {stream_QueryInterface, stream_AddRef, stream_Release, stream_Read, stream_Write};

static const char *const seeds[] = {
	"<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n"
	"<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
	"  <assemblyIdentity type=\"win32\" name=\"Raven.Sample\" version=\"1.2.3.4\" processorArchitecture=\"amd64\"/>\n"
	"  <dependency><dependentAssembly><assemblyIdentity name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\"/>"
	"</dependentAssembly></dependency>\n"
	"  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges>"
	"<requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/></requestedPrivileges></security></trustInfo>\n"
	"</assembly>\n",
	"<?xml version='1.0' encoding='windows-1252'?><a:root xmlns:a='urn:a' xmlns:b=\"urn:b\" b:attr='x &amp; y &#65;&#x42;'>"
	"<b:child a:one=\"1\" two='2'>text &lt;here&gt; &quot;q&quot; &apos;</b:child><![CDATA[ <not> &markup; ]]>"
	"<!-- a comment - with dashes --><?target some data?><empty/></a:root>",
	"<!DOCTYPE note [<!ELEMENT note (#PCDATA)><!ENTITY writer \"someone\">]><note>&lt; &#x1F600; &#9;</note>",
	"<configuration><appSettings><add key=\"a\" value=\"1\"/><add key=\"b\" value=\"2\"/></appSettings>"
	"<startup><supportedRuntime version=\"v4.0\" sku=\".NETFramework,Version=v4.8\"/></startup></configuration>",
	"<r><a><b><c><d><e><f><g><h><i><j><k><l><m><n><o><p>deep</p></o></n></m></l></k></j></i></h></g></f></e></d></c></b></a></r>",
	"<?xml version=\"1.0\"?>\r\n<x  y = \"1\"\r\n z='2' >\r\n mixed <![CDATA[x]]> &amp; <y/>\r\n</x>\r\n  ",
	"<p:a xmlns:p=\"urn:p\"><p:b xmlns:p=\"urn:q\" xmlns=\"urn:default\"><c xmlns=\"\"/></p:b><p:d xml:lang=\"en\" xml:space=\"preserve\"/></p:a>",
};

static const char *const tokens[] = {
	"<", ">", "/>", "</", "&", ";", "&#", "&#x", "&amp;", "&lt;", "]]>", "<![CDATA[", "<!--", "-->", "<?", "?>", "<!DOCTYPE",
	"xmlns:", "xmlns=\"\"", ":", "=", "\"", "'", " ", "\r", "\n", "\xef\xbb\xbf", "\xc3\xa9", "\xf0\x9f\x98\x80", "\xff", "\x80", "\0",
	"encoding=\"utf-16\"", "encoding=\"x\"", "<?xml version=\"1.0\"?>", "&#1114112;", "&#xD800;", "&#0;", "xml:", "&undefined;",
};

/* The UTF-16 form of a seed, with its byte order mark, little- or big-endian. */
static size_t utf16(const char *text, BYTE *out, BOOL big)
{
	size_t length = 0;

	out[length++] = big ? 0xfe : 0xff;
	out[length++] = big ? 0xff : 0xfe;
	for (; *text; ++text) {
		out[length++] = big ? 0 : (BYTE)*text;
		out[length++] = big ? (BYTE)*text : 0;
	}
	return length;
}

static size_t change(BYTE *data, size_t size, size_t capacity)
{
	const unsigned changes = 1 + (unsigned)(next() % 6);

	for (unsigned count = 0; count < changes; ++count) {
		const size_t at = size ? (size_t)(next() % (size + 1)) : 0;
		switch (next() % 5) {
		case 0:
			if (at < size)
				data[at] ^= (BYTE)(1u << (next() % 8));
			break;
		case 1:
			if (at < size)
				data[at] = (BYTE)next();
			break;
		case 2: {
			const char *token = tokens[next() % ARRAYSIZE(tokens)];
			const size_t length = token[0] ? strlen(token) : 1;
			if (size + length <= capacity) {
				memmove(data + at + length, data + at, size - at);
				memcpy(data + at, token, length);
				size += length;
			}
			break;
		}
		case 3:
			if (at < size) {
				const size_t cut = (size_t)(next() % (size - at + 1));
				memmove(data + at, data + at + cut, size - at - cut);
				size -= cut;
			}
			break;
		default:
			if (at < size) {
				const size_t length = (size_t)(next() % (size - at < 64 ? size - at : 64)) + 1;
				for (unsigned copy = 0; copy < 1 + next() % 8 && size + length <= capacity; ++copy) {
					memmove(data + at + length, data + at, size - at);
					size += length;
				}
			}
		}
	}
	return size;
}

static uint64_t touch(const WCHAR *text, UINT length)
{
	uint64_t sum = 0;

	if (!text)
		return 0;
	for (UINT index = 0; index <= length; ++index)
		sum += text[index];
	return sum;
}

static uint64_t node(IXmlReader *reader)
{
	const WCHAR *text;
	UINT length = 0, number = 0;
	uint64_t sum = 0;

	if (SUCCEEDED(reader->lpVtbl->GetQualifiedName(reader, &text, &length)))
		sum += touch(text, length);
	if (SUCCEEDED(reader->lpVtbl->GetLocalName(reader, &text, &length)))
		sum += touch(text, length);
	if (SUCCEEDED(reader->lpVtbl->GetPrefix(reader, &text, &length)))
		sum += touch(text, length);
	if (SUCCEEDED(reader->lpVtbl->GetNamespaceUri(reader, &text, &length)))
		sum += touch(text, length);
	if (next() % 3) {
		if (SUCCEEDED(reader->lpVtbl->GetValue(reader, &text, &length)))
			sum += touch(text, length);
	} else {
		WCHAR chunk[7];
		while (reader->lpVtbl->ReadValueChunk(reader, chunk, 1 + (UINT)(next() % 7), &number) == S_OK && number)
			sum += touch(chunk, number - 1);
	}
	reader->lpVtbl->GetLineNumber(reader, &number);
	reader->lpVtbl->GetLinePosition(reader, &number);
	reader->lpVtbl->GetDepth(reader, &number);
	reader->lpVtbl->IsEmptyElement(reader);
	reader->lpVtbl->IsDefault(reader);
	return sum + number;
}

/* Reads a document with settings drawn at random, or, for a seed, as it is meant to be read: to its end, or 0 when it fails. */
static uint64_t read_document(const BYTE *data, size_t size, BOOL seed)
{
	static const WCHAR *const encodings[] = {L"utf-8", L"UTF-16", L"windows-1252", L"iso-8859-1", L"us-ascii", L"bogus"};
	struct stream stream = {{&stream_vtable}, 1, data, size, 0};
	IXmlReader *reader = NULL;
	IUnknown *input = NULL;
	XmlNodeType type;
	uint64_t sum = 0;
	HRESULT status;

	if (FAILED(CreateXmlReader(&IID_IXmlReader, (void **)&reader, NULL)))
		abort();
	reader->lpVtbl->SetProperty(reader, XmlReaderProperty_DtdProcessing, seed ? DtdProcessing_Parse : (LONG_PTR)(next() % 2));
	reader->lpVtbl->SetProperty(reader, XmlReaderProperty_ConformanceLevel, seed ? XmlConformanceLevel_Document : (LONG_PTR)(next() % 3));
	if (!seed && next() % 4 == 0)
		reader->lpVtbl->SetProperty(reader, XmlReaderProperty_MaxElementDepth, (LONG_PTR)(next() % 20));
	switch (seed ? 0 : next() % 3) {
	case 0:
		status = reader->lpVtbl->SetInput(reader, (IUnknown *)&stream.iface);
		break;
	case 1:
		status = CreateXmlReaderInputWithEncodingName((IUnknown *)&stream.iface, NULL, encodings[next() % ARRAYSIZE(encodings)], next() % 2,
			next() % 2 ? L"file:///base.xml" : NULL, &input);
		if (SUCCEEDED(status))
			status = reader->lpVtbl->SetInput(reader, input);
		break;
	default:
		status = CreateXmlReaderInputWithEncodingCodePage((IUnknown *)&stream.iface, NULL, next() % 2 ? CP_UTF8 : 1200, next() % 2, NULL, &input);
		if (SUCCEEDED(status))
			status = reader->lpVtbl->SetInput(reader, input);
	}
	for (unsigned count = 0; SUCCEEDED(status) && count < 100000; ++count) {
		status = reader->lpVtbl->Read(reader, &type);
		if (status != S_OK)
			break;
		sum += node(reader);
		UINT attributes = 0;
		reader->lpVtbl->GetAttributeCount(reader, &attributes);
		if (reader->lpVtbl->MoveToFirstAttribute(reader) == S_OK) {
			do
				sum += node(reader);
			while (reader->lpVtbl->MoveToNextAttribute(reader) == S_OK);
			if (next() % 2)
				reader->lpVtbl->MoveToAttributeByName(reader, L"version", NULL);
			reader->lpVtbl->MoveToElement(reader);
		}
	}
	if (seed && (status != S_FALSE || !reader->lpVtbl->IsEOF(reader)))
		sum = 0;
	if (input)
		IUnknown_Release(input);
	reader->lpVtbl->Release(reader);
	return sum;
}

int main(int argc, char **argv)
{
	const unsigned long rounds = argc > 1 ? strtoul(argv[1], NULL, 10) : 40000;
	static BYTE data[1 << 16];
	unsigned long read = 0;

	if (argc > 2)
		state ^= strtoull(argv[2], NULL, 10) * 0x2545f4914f6cdd1dull;
	/* The seeds themselves read through. */
	for (size_t which = 0; which < ARRAYSIZE(seeds); ++which)
		if (!read_document((const BYTE *)seeds[which], strlen(seeds[which]), TRUE)) {
			printf("seed %zu does not read through\n", which);
			return 1;
		}
	for (unsigned long round = 0; round < rounds; ++round) {
		const char *seed = seeds[next() % ARRAYSIZE(seeds)];
		size_t size;
		if (next() % 8 == 0)
			size = utf16(seed, data, next() % 2);
		else {
			size = strlen(seed);
			memcpy(data, seed, size);
		}
		size = change(data, size, sizeof(data));
		if (read_document(data, size, FALSE))
			++read;
	}
	printf("%lu rounds, %lu documents read in part or whole\n", rounds, read);
	return 0;
}
