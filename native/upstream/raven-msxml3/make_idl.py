#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the IDL of the library's type information from the public C++ declarations in msxml2.h and msxml.h.

The headers carry no directions or property kinds, so they follow from the names: get_ reads, put_ and
putref_ write, and a method's last pointer that is not an interface given to it is what it returns."""
import re
import sys

LIBRARY_ID = "f5078f18-c551-11d3-89b9-0000f81fe221"
INTERFACE = re.compile(r'MIDL_INTERFACE\("([0-9a-fA-F-]+)"\)\s*(\w+)\s*:\s*public\s+(\w+)\s*\{(.*?)\n\};', re.S)
METHOD = re.compile(r'virtual\s+HRESULT\s+STDMETHODCALLTYPE\s+(\w+)\(\s*(.*?)\)\s*=\s*0;', re.S)
ENUM = re.compile(r'typedef\s+enum\s+(\w+)\s*\{(.*?)\}\s*(\w+)\s*;', re.S)
# Each of these answers the default member of its object.
# The old object model comes from the other header, which also repeats everything else.
LEGACY = {"IXMLElementCollection", "IXMLDocument", "IXMLDocument2", "IXMLElement", "IXMLElement2", "IXMLAttribute"}
DEFAULT_MEMBERS = {("IXMLDOMNodeList", "item"), ("IXMLDOMNamedNodeMap", "item"), ("IXMLDOMParseError", "errorCode"), ("IXMLDOMSelection", "item"), ("IMXNamespacePrefixes", "item")}
# A few interfaces are named differently in the identifier list.
PREFIXES = {"IXMLDOMCharacterData": ["DOM_DATA"], "IXMLDOMParseError": ["DOM_ERROR"], "IXMLDOMParseError2": ["DOM_ERROR2"],
            "IXMLDOMParseErrorCollection": ["DOM_ERRORCOLLECTION"], "IXMLDOMSchemaCollection": ["XMLDOM_SCHEMACOLLECTION"],
            "IXMLDOMSchemaCollection2": ["XMLDOM_SCHEMACOLLECTION"], "IXSLTemplate": ["XMLDOM_TEMPLATE"], "IXSLProcessor": ["XMLDOM_PROCESSOR"],
            "IMXWriter": ["MX_WRITER"], "IMXAttributes": ["MX_ATTRIBUTES"], "IMXReaderControl": ["MX_READER_CONTROL"],
            "IMXNamespaceManager": ["MX_NSMGR"], "IVBMXNamespaceManager": ["MX_NSMGR"], "IVBSAXXMLReader": ["SAX_XMLREADER"],
            "IVBSAXXMLFilter": ["SAX_XMLFILTER"], "IVBSAXAttributes": ["SAX_ATTRIBUTES"], "IVBSAXContentHandler": ["SAX_CONTENTHANDLER"],
            "IVBSAXLexicalHandler": ["SAX_LEXICALHANDLER"], "IVBSAXDeclHandler": ["SAX_DECLHANDLER"], "IVBSAXDTDHandler": ["SAX_DTDHANDLER"],
            "IXMLElement2": ["XMLELEMENT"], "IXMLDocument2": ["XMLDOCUMENT"],
            "IVBSAXEntityResolver": ["SAX_ENTITYRESOLVER"], "IVBSAXErrorHandler": ["SAX_ERRORHANDLER"], "IVBSAXLocator": ["SAX_LOCATOR"]}


def identifier_table(text):
    return {name: int(value, 0) for name, value in re.findall(r'#define\s+DISPID_(\w+)\s+(0x[0-9a-fA-F]+|\d+)', text)}


def prefixes_of(name):
    if name in PREFIXES:
        return PREFIXES[name]
    tail = re.sub(r'^IXMLDOM|^I', '', name).upper()
    return ["DOM_" + tail, "XMLDOM_" + tail, tail]


def parameters(text):
    result = []
    for part in text.split(","):
        part = " ".join(part.split())
        part = re.sub(r'\s*/\*.*?\*/\s*', ' ', part).strip()
        part = re.sub(r'\s*=.*$', '', part)
        if not part or part == "void":
            continue
        match = re.match(r'(.*?)(\w+)$', part)
        if not match:
            raise SystemExit("cannot read parameter: " + part)
        result.append((match.group(1).replace("const ", "").replace("WINBOOL", "long").strip(), match.group(2)))
    return result


def is_output(kind, names):
    stars = kind.count("*")
    base = kind.replace("*", "").strip()
    return stars >= 2 or (stars == 1 and base not in names and not base.endswith("Vtbl"))


def method_line(owner, name, args, ids, used, names, counter):
    kind = ""
    plain = name
    for prefix, attribute in (("get_", "propget"), ("putref_", "propputref"), ("put_", "propput")):
        if name.startswith(prefix):
            kind, plain = attribute, name[len(prefix):]
    identity = None
    if (owner, plain) in DEFAULT_MEMBERS:
        identity = 0
    elif plain == "_newEnum":
        identity = -4
    elif (owner, plain) == ("IXMLDOMNamedNodeMap", "length"):
        identity = ids["DOM_NODELIST_LENGTH"]
    else:
        for prefix in prefixes_of(owner):
            key = prefix + "_" + plain.upper()
            if key in ids:
                identity = ids[key]
                break
    if identity is None or (identity, kind) in used:
        identity = counter[0]
        counter[0] += 1
    used.add((identity, kind))
    written = []
    for index, (type_name, arg) in enumerate(args):
        last = index == len(args) - 1
        if last and kind == "propget" or last and not kind and is_output(type_name, names):
            direction = "out, retval"
        elif is_output(type_name, names):
            direction = "out"
        else:
            direction = "in"
        written.append("[%s] %s %s_" % (direction, type_name, arg))
    attributes = "id(%#x)%s" % (identity & 0xffffffff, ", " + kind if kind else "")
    return "\t\t[%s] HRESULT %s(%s);" % (attributes, plain, ", ".join(written))


def main():
    header = open(sys.argv[1]).read()
    ids = identifier_table(open(sys.argv[2]).read())
    legacy = open(sys.argv[3]).read()
    found = {}
    for guid, name, parent, body in INTERFACE.findall(header):
        found[name] = (guid, parent, METHOD.findall(body))
    for guid, name, parent, body in INTERFACE.findall(legacy):
        if name in LEGACY:
            found[name] = (guid, parent, METHOD.findall(body))

    def automation(name):
        while name in found:
            name = found[name][1]
        return name == "IDispatch"

    chosen = [name for name in found if automation(name)]
    out = ['import "oaidl.idl";', '', '[uuid(%s), version(3.0), helpstring("Microsoft XML, v3.0")]' % LIBRARY_ID, 'library MSXML2', '{',
           '\timportlib("stdole2.tlb");', '']
    known = set(found)
    for _, body, name in ENUM.findall(header):
        out.append("\ttypedef enum { %s } %s;" % (" ".join(body.split()), name))
    for name in chosen:
        out.append("\tinterface %s;" % name)
    out.append("")
    done = set()

    def emit(name):
        if name in done or name not in chosen:
            return
        done.add(name)
        guid, parent, methods = found[name]
        emit(parent)
        out.append("\t[object, uuid(%s), dual, oleautomation]\n\tinterface %s : %s\n\t{" % (guid, name, parent))
        counter = [0x4000]
        used = set()
        for method, arguments in methods:
            # The interfaces outside automation show up here as plain unknown ones.
            given = [(re.sub(r'^(\w+)', lambda m: "IUnknown" if m.group(1) in known and m.group(1) not in chosen else m.group(1), kind), arg)
                     for kind, arg in parameters(arguments)]
            out.append(method_line(name, method, given, ids, used, known | {"IUnknown", "IDispatch"}, counter))
        out.append("\t}\n")

    for name in chosen:
        emit(name)
    out.append("}")
    open(sys.argv[4], "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
