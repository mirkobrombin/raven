# Guest msxml3 component

Raven's own msxml3.dll for guest programs, built by `scripts/provision-installer.py` with MinGW-w64
on the public Windows headers, over the public interfaces of libxml2 and libxslt, which it links
statically. It holds the DOM document and its nodes for every class version, XPath and the older XSL
pattern language, schema caches over XSD, XSL templates and processors, the SAX reader and writer
with their automation faces, the namespace manager, `XMLHTTP` and `ServerXMLHTTP` over urlmon, and
the old `XMLDocument` object model.

The type library is generated at build time: `make_idl.py` writes its IDL from the declarations in
the public headers and `widl` compiles it into the DLL's resources.

XDR schemas are accepted by a cache but neither enforced nor used for data types. Text in a double
byte code page needs the runtime's conversion tables, which do not cover them yet. The SAX reader
does not resolve external entities. `XMLParser` keeps its settings but does not drive a node factory:
`Run` returns `E_NOTIMPL`. See [LICENSES.md](../../../LICENSES.md).
