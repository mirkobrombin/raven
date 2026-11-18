/* The part of the public XmlLite interface the reader implements (native/guest/raven-xmllite/reader.c), declared for building it on the
   host for native/fuzz/xml_reader.c: the node types, states and properties with their documented values, the errors it reports, and
   IXmlReader with its methods in their documented order. */
#ifndef RAVEN_FUZZ_XMLLITE_H
#define RAVEN_FUZZ_XMLLITE_H

#include "windows.h"

typedef enum
{
	XmlNodeType_None = 0,
	XmlNodeType_Element = 1,
	XmlNodeType_Attribute = 2,
	XmlNodeType_Text = 3,
	XmlNodeType_CDATA = 4,
	XmlNodeType_ProcessingInstruction = 7,
	XmlNodeType_Comment = 8,
	XmlNodeType_DocumentType = 10,
	XmlNodeType_Whitespace = 13,
	XmlNodeType_EndElement = 15,
	XmlNodeType_XmlDeclaration = 17,
} XmlNodeType;

typedef enum
{
	XmlReadState_Initial = 0,
	XmlReadState_Interactive = 1,
	XmlReadState_Error = 2,
	XmlReadState_EndOfFile = 3,
	XmlReadState_Closed = 4,
} XmlReadState;

typedef enum
{
	XmlConformanceLevel_Auto = 0,
	XmlConformanceLevel_Fragment = 1,
	XmlConformanceLevel_Document = 2,
} XmlConformanceLevel;

typedef enum
{
	DtdProcessing_Prohibit = 0,
	DtdProcessing_Parse = 1,
} DtdProcessing;

typedef enum
{
	XmlReaderProperty_MultiLanguage = 0,
	XmlReaderProperty_ConformanceLevel = 1,
	XmlReaderProperty_RandomAccess = 2,
	XmlReaderProperty_XmlResolver = 3,
	XmlReaderProperty_DtdProcessing = 4,
	XmlReaderProperty_ReadState = 5,
	XmlReaderProperty_MaxElementDepth = 6,
	XmlReaderProperty_MaxEntityExpansion = 7,
} XmlReaderProperty;

#define MX_E_INPUTEND ((HRESULT)0xC00CEE01u)
#define MX_E_ENCODING ((HRESULT)0xC00CEE02u)
#define WC_E_WHITESPACE ((HRESULT)0xC00CEE21u)
#define WC_E_SEMICOLON ((HRESULT)0xC00CEE22u)
#define WC_E_GREATERTHAN ((HRESULT)0xC00CEE23u)
#define WC_E_QUOTE ((HRESULT)0xC00CEE24u)
#define WC_E_EQUAL ((HRESULT)0xC00CEE25u)
#define WC_E_LESSTHAN ((HRESULT)0xC00CEE26u)
#define WC_E_HEXDIGIT ((HRESULT)0xC00CEE27u)
#define WC_E_DIGIT ((HRESULT)0xC00CEE28u)
#define WC_E_XMLCHARACTER ((HRESULT)0xC00CEE2Bu)
#define WC_E_NAMECHARACTER ((HRESULT)0xC00CEE2Cu)
#define WC_E_SYNTAX ((HRESULT)0xC00CEE2Du)
#define WC_E_COMMENT ((HRESULT)0xC00CEE2Fu)
#define WC_E_ELEMENTMATCH ((HRESULT)0xC00CEE3Bu)
#define WC_E_UNIQUEATTRIBUTE ((HRESULT)0xC00CEE3Cu)
#define WC_E_LEADINGXML ((HRESULT)0xC00CEE3Eu)
#define WC_E_ENCNAME ((HRESULT)0xC00CEE41u)
#define WC_E_UNDECLAREDENTITY ((HRESULT)0xC00CEE47u)
#define WC_E_PI ((HRESULT)0xC00CEE4Au)
#define WC_E_CDSECTEND ((HRESULT)0xC00CEE4Du)
#define WC_E_DTDPROHIBITED ((HRESULT)0xC00CEE4Fu)
#define NC_E_QNAMECOLON ((HRESULT)0xC00CEE62u)
#define NC_E_UNDECLAREDPREFIX ((HRESULT)0xC00CEE65u)
#define NC_E_EMPTYURI ((HRESULT)0xC00CEE66u)

typedef struct IXmlReader IXmlReader;
typedef struct IXmlReaderVtbl
{
	HRESULT (WINAPI *QueryInterface)(IXmlReader *self, REFIID riid, void **object);
	ULONG (WINAPI *AddRef)(IXmlReader *self);
	ULONG (WINAPI *Release)(IXmlReader *self);
	HRESULT (WINAPI *SetInput)(IXmlReader *self, IUnknown *input);
	HRESULT (WINAPI *GetProperty)(IXmlReader *self, UINT property, LONG_PTR *value);
	HRESULT (WINAPI *SetProperty)(IXmlReader *self, UINT property, LONG_PTR value);
	HRESULT (WINAPI *Read)(IXmlReader *self, XmlNodeType *type);
	HRESULT (WINAPI *GetNodeType)(IXmlReader *self, XmlNodeType *type);
	HRESULT (WINAPI *MoveToFirstAttribute)(IXmlReader *self);
	HRESULT (WINAPI *MoveToNextAttribute)(IXmlReader *self);
	HRESULT (WINAPI *MoveToAttributeByName)(IXmlReader *self, const WCHAR *local, const WCHAR *uri);
	HRESULT (WINAPI *MoveToElement)(IXmlReader *self);
	HRESULT (WINAPI *GetQualifiedName)(IXmlReader *self, const WCHAR **name, UINT *length);
	HRESULT (WINAPI *GetNamespaceUri)(IXmlReader *self, const WCHAR **uri, UINT *length);
	HRESULT (WINAPI *GetLocalName)(IXmlReader *self, const WCHAR **name, UINT *length);
	HRESULT (WINAPI *GetPrefix)(IXmlReader *self, const WCHAR **prefix, UINT *length);
	HRESULT (WINAPI *GetValue)(IXmlReader *self, const WCHAR **value, UINT *length);
	HRESULT (WINAPI *ReadValueChunk)(IXmlReader *self, WCHAR *buffer, UINT wanted, UINT *read);
	HRESULT (WINAPI *GetBaseUri)(IXmlReader *self, const WCHAR **uri, UINT *length);
	BOOL (WINAPI *IsDefault)(IXmlReader *self);
	BOOL (WINAPI *IsEmptyElement)(IXmlReader *self);
	HRESULT (WINAPI *GetLineNumber)(IXmlReader *self, UINT *line);
	HRESULT (WINAPI *GetLinePosition)(IXmlReader *self, UINT *position);
	HRESULT (WINAPI *GetAttributeCount)(IXmlReader *self, UINT *count);
	HRESULT (WINAPI *GetDepth)(IXmlReader *self, UINT *depth);
	BOOL (WINAPI *IsEOF)(IXmlReader *self);
} IXmlReaderVtbl;
struct IXmlReader
{
	const IXmlReaderVtbl *lpVtbl;
};
#define IXmlReader_AddRef(self) (self)->lpVtbl->AddRef(self)

HRESULT WINAPI CreateXmlReader(REFIID riid, void **object, IMalloc *allocator);
HRESULT WINAPI CreateXmlReaderInputWithEncodingCodePage(IUnknown *stream, IMalloc *allocator, UINT code_page, BOOL hint, const WCHAR *base_uri,
	IUnknown **input);
HRESULT WINAPI CreateXmlReaderInputWithEncodingName(IUnknown *stream, IMalloc *allocator, const WCHAR *encoding, BOOL hint, const WCHAR *base_uri,
	IUnknown **input);

#endif
