#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <webservices.h>

#define NAME(text) {sizeof(text) - 1, (BYTE *)(text), NULL, 0}

static int same(const WCHAR *left, const WCHAR *right)
{
	while (*left && *left == *right) ++left, ++right;
	return *left == *right;
}

/* A configuration answer: a version attribute, a channel enum, a flag, a nested server and a list of products, in a default namespace. */
static const char document[] =
	"\xef\xbb\xbf<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
	"<!-- served by a test -->\r\n"
	"<c:Config xmlns:c=\"urn:raven:test\" xmlns=\"urn:raven:test\" Version=\"16.0\">\r\n"
	"  <Channel>Current</Channel>\r\n"
	"  <Enabled>true</Enabled>\r\n"
	"  <Empty/>\r\n"
	"  <Server Port=\"443\"><Host>cdn &amp; more</Host></Server>\r\n"
	"  <Products><Product>Word</Product><Product>Excel&#x2122;</Product><Product><![CDATA[<Outlook>]]></Product></Products>\r\n"
	"  <Size>-42</Size>\r\n"
	"</c:Config>";

typedef struct { int port; WCHAR *host; } server;
typedef struct { WCHAR *version; int channel; BOOL enabled; server *server; WCHAR **products; ULONG product_count; int size; WS_STRING missing; } config;

static WS_XML_STRING ns = NAME("urn:raven:test"), config_name = NAME("Config"), version_name = NAME("Version"), channel_name = NAME("Channel"),
	enabled_name = NAME("Enabled"), server_name = NAME("Server"), port_name = NAME("Port"), host_name = NAME("Host"), products_name = NAME("Products"),
	product_name = NAME("Product"), size_name = NAME("Size"), missing_name = NAME("Missing"), beta_name = NAME("Beta"), current_name = NAME("Current"),
	empty_ns = NAME("");

int raven_entry(void)
{
	WS_ENUM_VALUE channels[] = {{1, &beta_name}, {2, &current_name}};
	WS_ENUM_DESCRIPTION channel_type = {channels, 2, 7, NULL};
	WS_FIELD_DESCRIPTION port = {WS_ATTRIBUTE_FIELD_MAPPING, &port_name, &empty_ns, WS_INT32_TYPE, NULL, FIELD_OFFSET(server, port)};
	WS_FIELD_DESCRIPTION host = {WS_ELEMENT_FIELD_MAPPING, &host_name, &ns, WS_WSZ_TYPE, NULL, FIELD_OFFSET(server, host)};
	WS_FIELD_DESCRIPTION *server_fields[] = {&port, &host};
	WS_STRUCT_DESCRIPTION server_type = {sizeof(server), __alignof__(server), server_fields, 2, NULL, NULL};
	WS_FIELD_DESCRIPTION version = {WS_ATTRIBUTE_FIELD_MAPPING, &version_name, &empty_ns, WS_WSZ_TYPE, NULL, FIELD_OFFSET(config, version)};
	WS_FIELD_DESCRIPTION channel = {WS_ELEMENT_FIELD_MAPPING, &channel_name, &ns, WS_ENUM_TYPE, &channel_type, FIELD_OFFSET(config, channel)};
	WS_FIELD_DESCRIPTION enabled = {WS_ELEMENT_FIELD_MAPPING, &enabled_name, &ns, WS_BOOL_TYPE, NULL, FIELD_OFFSET(config, enabled)};
	WS_FIELD_DESCRIPTION server_field = {WS_ELEMENT_FIELD_MAPPING, &server_name, &ns, WS_STRUCT_TYPE, &server_type, FIELD_OFFSET(config, server), WS_FIELD_POINTER};
	WS_FIELD_DESCRIPTION products = {WS_REPEATING_ELEMENT_FIELD_MAPPING, &products_name, &ns, WS_WSZ_TYPE, NULL, FIELD_OFFSET(config, products), 0, NULL,
		FIELD_OFFSET(config, product_count), &product_name, &ns};
	WS_FIELD_DESCRIPTION size = {WS_ELEMENT_FIELD_MAPPING, &size_name, &ns, WS_INT32_TYPE, NULL, FIELD_OFFSET(config, size)};
	WS_FIELD_DESCRIPTION missing = {WS_ELEMENT_FIELD_MAPPING, &missing_name, &ns, WS_STRING_TYPE, NULL, FIELD_OFFSET(config, missing), WS_FIELD_OPTIONAL};
	WS_FIELD_DESCRIPTION *config_fields[] = {&version, &channel, &enabled, &server_field, &products, &size, &missing};
	WS_STRUCT_DESCRIPTION config_type = {sizeof(config), __alignof__(config), config_fields, 7, NULL, NULL};

	WS_ERROR *error = NULL;
	WS_HEAP *heap = NULL;
	WS_XML_READER *reader = NULL;
	if (WsCreateError(NULL, 0, &error) || WsCreateHeap(1 << 16, 0, NULL, 0, &heap, error) || WsCreateReader(NULL, 0, &reader, error)) return 1;
	WS_XML_READER_TEXT_ENCODING encoding = {{WS_XML_READER_ENCODING_TYPE_TEXT}, WS_CHARSET_AUTO};
	WS_XML_READER_BUFFER_INPUT input = {{WS_XML_READER_INPUT_TYPE_BUFFER}, (void *)document, sizeof(document) - 1};
	if (WsSetInput(reader, &encoding.encoding, &input.input, NULL, 0, error) || WsFillReader(reader, sizeof(document), NULL, error)) return 2;
	BOOL found = TRUE;
	if (WsReadToStartElement(reader, &products_name, &ns, &found, error) || found) return 3;
	if (WsReadToStartElement(reader, &config_name, &ns, &found, error) || !found) return 4;
	config value;
	if (WsReadType(reader, WS_ELEMENT_TYPE_MAPPING, WS_STRUCT_TYPE, &config_type, WS_READ_REQUIRED_VALUE, heap, &value, sizeof(value), error)) return 5;
	if (!same(value.version, L"16.0") || value.channel != 2 || value.enabled != TRUE || value.size != -42 || value.missing.length || value.missing.chars) return 6;
	if (!value.server || value.server->port != 443 || !same(value.server->host, L"cdn & more")) return 7;
	if (value.product_count != 3 || !same(value.products[0], L"Word") || !same(value.products[1], L"Excel\x2122") || !same(value.products[2], L"<Outlook>")) return 8;
	/* The same document read element by element: the content of Config after its start, then the end. */
	if (WsSetInput(reader, &encoding.encoding, &input.input, NULL, 0, error) || WsReadToStartElement(reader, NULL, NULL, NULL, error) ||
		WsReadStartElement(reader, error)) return 9;
	if (WsReadToStartElement(reader, &channel_name, &ns, NULL, error) || WsReadStartElement(reader, error)) return 10;
	WCHAR *text = NULL;
	if (WsReadType(reader, WS_ELEMENT_CONTENT_TYPE_MAPPING, WS_WSZ_TYPE, NULL, WS_READ_REQUIRED_POINTER, heap, &text, sizeof(text), error) ||
		!same(text, L"Current") || WsReadEndElement(reader, error)) return 11;
	/* Skipping Enabled whole leaves the reader before Empty. */
	if (WsReadToStartElement(reader, &enabled_name, &ns, &found, error) || !found || WsSkipNode(reader, error) ||
		WsReadToStartElement(reader, &enabled_name, &ns, &found, error) || found ||
		WsReadToStartElement(reader, NULL, NULL, &found, error) || !found) return 12;
	/* A required value the document does not have, and malformed input, fail with WS_E_INVALID_FORMAT and say why. */
	config_fields[6] = &(WS_FIELD_DESCRIPTION){WS_ELEMENT_FIELD_MAPPING, &missing_name, &ns, WS_STRING_TYPE, NULL, FIELD_OFFSET(config, missing)};
	if (WsSetInput(reader, &encoding.encoding, &input.input, NULL, 0, error) ||
		WsReadType(reader, WS_ELEMENT_TYPE_MAPPING, WS_STRUCT_TYPE, &config_type, WS_READ_REQUIRED_VALUE, heap, &value, sizeof(value), error) !=
		(HRESULT)0x803d0000) return 13;
	ULONG strings = 0;
	WS_STRING message;
	if (WsGetErrorProperty(error, WS_ERROR_PROPERTY_STRING_COUNT, &strings, sizeof(strings)) || !strings || WsGetErrorString(error, 0, &message) ||
		!message.length) return 14;
	static const char broken[] = "<a><b></a>";
	WS_XML_READER_BUFFER_INPUT bad = {{WS_XML_READER_INPUT_TYPE_BUFFER}, (void *)broken, sizeof(broken) - 1};
	if (WsSetInput(reader, &encoding.encoding, &bad.input, NULL, 0, NULL) != (HRESULT)0x803d0000) return 15;
	/* Many empty elements with text between them: three nodes for each one. */
	static const char flat[] = "<r> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> <i/> </r>";
	WS_XML_READER_BUFFER_INPUT flat_input = {{WS_XML_READER_INPUT_TYPE_BUFFER}, (void *)flat, sizeof(flat) - 1};
	static WS_XML_STRING item_name = NAME("i");
	struct { WCHAR **items; ULONG count; } list;
	WS_FIELD_DESCRIPTION items = {WS_REPEATING_ELEMENT_FIELD_MAPPING, NULL, NULL, WS_WSZ_TYPE, NULL, 0, 0, NULL, sizeof(WCHAR **), &item_name, &empty_ns};
	WS_FIELD_DESCRIPTION *list_fields[] = {&items};
	WS_STRUCT_DESCRIPTION list_type = {sizeof(list), __alignof__(list), list_fields, 1, NULL, NULL};
	if (WsSetInput(reader, &encoding.encoding, &flat_input.input, NULL, 0, error) ||
		WsReadType(reader, WS_ELEMENT_TYPE_MAPPING, WS_STRUCT_TYPE, &list_type, WS_READ_REQUIRED_VALUE, heap, &list, sizeof(list), error) ||
		list.count != 20 || !same(list.items[19], L"")) return 16;
	{
		/* Times convert between the two counts, and XML strings compare by their bytes. */
		FILETIME file = {0x2a3b4c5d, 0x01d9e8f7}, back;
		WS_DATETIME time;
		WS_XML_STRING left = NAME("item"), right = NAME("item"), other = NAME("items");
		if (WsFileTimeToDateTime(&file, &time, error) || time.format != WS_DATETIME_FORMAT_UTC ||
			time.ticks != ((ULONGLONG)file.dwHighDateTime << 32 | file.dwLowDateTime) + 504911232000000000ULL ||
			WsDateTimeToFileTime(&time, &back, error) || back.dwLowDateTime != file.dwLowDateTime || back.dwHighDateTime != file.dwHighDateTime) return 17;
		time.ticks = 5;
		if (WsDateTimeToFileTime(&time, &back, error) == S_OK) return 18;
		if (WsXmlStringEquals(&left, &right, error) != S_OK || WsXmlStringEquals(&left, &other, error) != S_FALSE) return 19;
	}
	{
		/* A service a program cannot reach: no proxy is made, and the call says why. */
		WS_SERVICE_PROXY *proxy = (WS_SERVICE_PROXY *)1;
		if (WsCreateServiceProxy(WS_CHANNEL_TYPE_REQUEST, WS_HTTP_CHANNEL_BINDING, NULL, NULL, 0, NULL, 0, &proxy, error) != E_NOTIMPL || proxy) return 20;
		WsFreeServiceProxy(NULL);
	}
	WsFreeReader(reader);
	WsFreeHeap(heap);
	WsFreeError(error);
	return 0;
}
