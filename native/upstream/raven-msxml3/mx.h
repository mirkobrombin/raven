/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_MSXML_H
#define RAVEN_MSXML_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <oaidl.h>
#include <objsafe.h>
#include <olectl.h>
#include <ocidl.h>
#include <dispex.h>
#include <urlmon.h>
#include <msxml2.h>
#include <msxml2did.h>
#ifdef MX_LEGACY_MODEL
#include <msxml.h>
#endif

#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlsave.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlschemas.h>
#include <libxml/SAX2.h>
#include <libxml/valid.h>
#include <libxml/uri.h>

#define CP_UNICODE_ONLY 0
#ifndef E_XML_BUFFERTOOSMALL
#define E_XML_BUFFERTOOSMALL ((HRESULT)0xC00CE226)
#endif

/* --- Which release of the library a class speaks for: a few rules change between them. --- */

typedef enum
{
	MX_DEFAULT = 0,
	MX_1 = 10,
	MX_2 = 20,
	MX_26 = 26,
	MX_3 = 30,
	MX_4 = 40,
	MX_6 = 60,
} mx_version;

/* --- Text. --- */

xmlChar *mx_to_utf8(const WCHAR *text);
xmlChar *mx_to_utf8_counted(const WCHAR *text, int length);
WCHAR *mx_to_wide(const xmlChar *text);
BSTR mx_bstr(const xmlChar *text);
BSTR mx_bstr_counted(const xmlChar *text, int bytes);
HRESULT mx_return_string(const xmlChar *text, BSTR *out);
HRESULT mx_return_null_variant(VARIANT *out);
void *mx_alloc(SIZE_T size);
void *mx_zalloc(SIZE_T size);
void *mx_realloc(void *block, SIZE_T size);
void mx_free(void *block);
BOOL mx_is_blank(const xmlChar *text);
HRESULT mx_variant_text(const VARIANT *from, VARIANT *text);

/* --- A dispatch face over the library's type information. --- */

typedef enum
{
	MX_TYPE_NODE,
	MX_TYPE_DOCUMENT,
	MX_TYPE_DOCUMENT2,
	MX_TYPE_DOCUMENT3,
	MX_TYPE_ELEMENT,
	MX_TYPE_ATTRIBUTE,
	MX_TYPE_TEXT,
	MX_TYPE_CDATA,
	MX_TYPE_COMMENT,
	MX_TYPE_PI,
	MX_TYPE_FRAGMENT,
	MX_TYPE_DOCTYPE,
	MX_TYPE_ENTITY_REF,
	MX_TYPE_ENTITY,
	MX_TYPE_NOTATION,
	MX_TYPE_NODE_LIST,
	MX_TYPE_NODE_MAP,
	MX_TYPE_SELECTION,
	MX_TYPE_PARSE_ERROR,
	MX_TYPE_IMPLEMENTATION,
	MX_TYPE_SCHEMA_CACHE,
	MX_TYPE_TEMPLATE,
	MX_TYPE_PROCESSOR,
	MX_TYPE_HTTP,
	MX_TYPE_SERVER_HTTP,
	MX_TYPE_WRITER,
	MX_TYPE_ATTRIBUTES,
	MX_TYPE_VB_READER,
	MX_TYPE_NS_MANAGER,
	MX_TYPE_NS_PREFIXES,
	MX_TYPE_VB_ATTRIBUTES,
	MX_TYPE_VB_LOCATOR,
	MX_TYPE_XML_DOCUMENT,
	MX_TYPE_XML_ELEMENT,
	MX_TYPE_XML_COLLECTION,
	MX_TYPE_XML_ATTRIBUTE,
	MX_TYPE_COUNT,
} mx_type;

HRESULT mx_type_info(mx_type type, ITypeInfo **out);
void mx_release_type_infos(void);
HRESULT mx_get_ids(mx_type type, REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *ids);
HRESULT mx_invoke(mx_type type, void *object, DISPID id, REFIID riid, LCID lcid, WORD flags,
	DISPPARAMS *params, VARIANT *result, EXCEPINFO *excep, UINT *arg_err);

/* The extended dispatch face of an object: its own Invoke behind it, and for a collection, members named by number. */
typedef struct
{
	const IDispatchExVtbl *vtbl;
	IDispatch *outer;
	BOOL collection;
} mx_dispex;

void mx_dispex_init(mx_dispex *self, void *outer, BOOL collection);

/* --- The state every wrapper of one parsed tree shares. It lives in the tree's _private. --- */

typedef struct mx_orphan
{
	struct mx_orphan *next;
	xmlNodePtr node;
} mx_orphan;

typedef struct mx_select_ns
{
	struct mx_select_ns *next;
	xmlChar *prefix, *uri;
} mx_select_ns;

/* What a program set on a document outlives any one load of it, and every object over any of its trees sees the same. */
typedef struct
{
	LONG refs;
	BOOL xpath;
	BOOL prohibit_dtd, new_parser, multiple_errors, validate_parse, resolve_externals, preserve_space;
	BOOL allow_document_function, allow_xslt_script, normalize_attributes, parse_error_chain;
	LONG max_entity_chars;
	xmlChar *selection_namespaces;
	mx_select_ns *namespaces;
	IUnknown *schemas;
	LONG max_element_depth;
} mx_properties;

typedef struct
{
	LONG refs;
	mx_orphan *orphans;
	mx_version version;
	mx_properties *props;
	BSTR url;
	xmlDocPtr *pins;
	LONG pin_count;
} mx_tree;

xmlDocPtr mx_tree_new_document(mx_version version);
void mx_tree_attach(xmlDocPtr tree, mx_version version);
void mx_tree_detach(xmlDocPtr tree);
void mx_tree_share_properties(xmlDocPtr tree, xmlDocPtr from);
mx_tree *mx_tree_of(xmlDocPtr tree);
void mx_tree_ref(xmlDocPtr tree);
void mx_tree_unref(xmlDocPtr tree);
void mx_tree_orphan(xmlNodePtr node);
void mx_tree_adopted(xmlNodePtr node);
void mx_tree_set_namespaces(mx_tree *data, const xmlChar *text);

/* --- Nodes. --- */

#define MX_BLANK_BEFORE 1
#define MX_BLANK_AT_END 2
#define MX_EXPLICIT_END 8
#define MX_ATTRIBUTES_FIRST 16
extern const char mx_declaration_mark[];
#define MX_IMPLIED_DECLARATION 4

HRESULT mx_node_create(xmlNodePtr node, IXMLDOMNode **out);
HRESULT mx_node_return(xmlNodePtr node, IXMLDOMNode **out);
xmlNodePtr mx_node_get(IUnknown *object);
HRESULT mx_serialize(xmlNodePtr node, BSTR *out);
xmlChar *mx_node_text(xmlNodePtr node);
const xmlChar *mx_local_name(const xmlChar *qualified, const xmlChar **prefix_end);
HRESULT mx_node_transform(xmlNodePtr node, IXMLDOMNode *style, BSTR *out, IStream *stream, VARIANT *object);
HRESULT mx_node_transform_to_object(xmlNodePtr node, IXMLDOMNode *style, VARIANT target);

/* The document object over a tree, new or sharing one that already exists. */
HRESULT mx_document_create(mx_version version, REFIID riid, void **out);
HRESULT mx_document_wrap(xmlDocPtr tree, REFIID riid, void **out);
HRESULT mx_document_load_stream(IStream *stream, xmlDocPtr *out);

/* --- Collections. --- */

HRESULT mx_children_list(xmlNodePtr parent, IXMLDOMNodeList **out);
HRESULT mx_found_list(xmlNodePtr context, xmlNodePtr *items, LONG count, IXMLDOMNodeList **out);
HRESULT mx_attribute_map(xmlNodePtr owner, IXMLDOMNamedNodeMap **out);
xmlNodePtr mx_declaration_attribute(xmlNodePtr owner, xmlNsPtr space);
HRESULT mx_dtd_map(xmlDocPtr tree, xmlHashTablePtr table, IXMLDOMNamedNodeMap **out);
HRESULT mx_enum_create(IUnknown *collection, IUnknown **out);
HRESULT mx_implementation_create(IXMLDOMImplementation **out);

/* --- Selection. --- */

HRESULT mx_select_nodes(xmlNodePtr context, const WCHAR *query, IXMLDOMNodeList **out);
HRESULT mx_select_single(xmlNodePtr context, const WCHAR *query, IXMLDOMNode **out);
HRESULT mx_select_by_name(xmlNodePtr context, const WCHAR *name, IXMLDOMNodeList **out);
xmlChar *mx_pattern_to_xpath(const xmlChar *pattern, const mx_tree *data);

/* --- Parse errors. --- */

typedef struct
{
	LONG code, line, column, position;
	BSTR url, reason, source;
} mx_error_info;

void mx_error_clear(mx_error_info *info);
void mx_error_from_libxml(mx_error_info *info, const xmlError *error, const WCHAR *url);
HRESULT mx_parse_error_create(const mx_error_info *info, IXMLDOMParseError **out);

/* --- The parser, shared by the document and the schema loader. --- */

HRESULT mx_parse_memory(const char *data, int size, const char *encoding, mx_version version,
	BOOL preserve, BOOL resolve, BOOL prohibit, const WCHAR *url, LONG max_depth, xmlDocPtr *out, mx_error_info *error);
char *mx_decode_input(const char *data, int size, int *out_size, BOOL *unknown);
xmlChar *mx_declared_encoding(const char *text, int size);
BOOL mx_page_of_name(const xmlChar *name, UINT *page);
HRESULT mx_read_stream(IStream *stream, char **data, ULONG *size);
HRESULT mx_read_url(const WCHAR *url, char **data, ULONG *size);

/* --- Classes. --- */

HRESULT mx_schema_cache_create(mx_version version, REFIID riid, void **out);
HRESULT mx_schema_validate(IUnknown *cache, xmlDocPtr tree, xmlNodePtr node, mx_error_info *error);
xmlSchemaPtr mx_schema_of_uri(IUnknown *cache, const xmlChar *uri);
void mx_tree_hold(xmlDocPtr tree, xmlNodePtr node);
void mx_tree_free_doc(xmlDocPtr tree);
xmlNodePtr mx_blank_before(xmlNodePtr node);
xmlNodePtr mx_blank_at_end(xmlNodePtr node);
HRESULT mx_error_support_create(const GUID *first, const GUID *const *more, ISupportErrorInfo **out);
BOOL mx_is_document(IUnknown *object);
void mx_normalize_lines(xmlChar *text);
BOOL mx_schema_covers(IUnknown *cache, xmlNodePtr element);
HRESULT mx_namespace_view_create(xmlDocPtr tree, IXMLDOMSchemaCollection **out);
HRESULT mx_template_create(mx_version version, REFIID riid, void **out);
HRESULT mx_sax_reader_create(mx_version version, REFIID riid, void **out);
HRESULT mx_sax_attributes_create(mx_version version, REFIID riid, void **out);
HRESULT mx_writer_create(mx_version version, REFIID riid, void **out);
HRESULT mx_namespace_manager_create(mx_version version, REFIID riid, void **out);
HRESULT mx_http_create(REFIID riid, void **out);
HRESULT mx_server_http_create(REFIID riid, void **out);
HRESULT mx_legacy_document_create(REFIID riid, void **out);
HRESULT mx_parser_create(REFIID riid, void **out);

extern HINSTANCE mx_instance;

#endif
