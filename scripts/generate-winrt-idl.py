#!/usr/bin/env python3
"""Writes native/upstream/raven-web/windows.web.http.idl, the Windows.Web.Http namespaces for widl, from Microsoft's Windows metadata as
the windows-rs project distributes it (MIT OR Apache-2.0, used under MIT): its generated bindings give each interface's identifier,
the order of its methods and their signatures, and the raw vtable layout each method is checked against. The hand-written part (the
imports and the few types of other namespaces Wine's IDL files lack) is windows.web.http.head.idl. With --check, the committed IDL is
compared with what the pinned sources give instead of being written."""
import argparse
import hashlib
from pathlib import Path
import re
import sys
import urllib.request

REVISION = "58217a690d6256ee229044a174de67d72d28bc35"
SOURCES = (("Windows.Web.Http", "Web/Http", "2d16c9b0e362b66443d58837588385e62c46c2dc548375992d692c2db5051626"),
           ("Windows.Web.Http.Headers", "Web/Http/Headers", "08e2a3ac344c76759bb08d9fc56a1585570578777244231054ba5e86b0b5d637"),
           ("Windows.Web.Http.Filters", "Web/Http/Filters", "270aa3244d5dd26fc57330cf046a79cbf8389168d6a54960d7c574656ecbb638"))
TARGET = Path(__file__).resolve().parents[1] / "native/upstream/raven-web/windows.web.http.idl"


def split_args(text):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def abi_args(abi):
    m = re.match(r"unsafe extern \"system\" fn\((.*)\) -> windows_core::HRESULT$", abi)
    return split_args(m.group(1))[1:] if m else []


def boxed(mod, ty, generics):
    """IReference<T> for a value type T that the ABI passes as an interface pointer."""
    rendered = mod.idl_type(ty, generics)
    if rendered.endswith("*") or rendered == "HSTRING":
        return rendered
    name = f"Windows.Foundation.IReference<{rendered}>"
    USED_GENERICS.add(name)
    return name + "*"


PRIM = {"bool": "boolean", "u8": "BYTE", "i8": "INT8", "u16": "UINT16", "i16": "INT16", "u32": "UINT32", "i32": "INT32", "u64": "UINT64",
        "i64": "INT64", "f32": "FLOAT", "f64": "DOUBLE", "windows_core::GUID": "GUID", "windows_core::HSTRING": "HSTRING", "u16char": "WCHAR", "windows_core::HRESULT": "HRESULT"}
FOUNDATION_VALUES = {"DateTime", "TimeSpan", "EventRegistrationToken", "Point", "Size", "Rect", "AsyncStatus"}



class Module:
    def __init__(self, namespace, text):
        self.ns = namespace
        self.text = text
        self.interfaces = {}  # name -> (iid, [(field, abi)])
        self.enums = {}  # name -> (kind, [(value, number)])
        self.structs = {}  # name -> [(field, type)]
        self.classes = {}  # name -> dict
        self.methods = {}  # (iface, field) -> (params, ret)
        self.parse()

    def qualify(self, path):
        """windows-rs path -> dotted metadata name."""
        path = path.strip()
        for prefix, ns in (("windows_future::", "Windows.Foundation."), ("windows_collections::", "Windows.Foundation.Collections."),
                           ("windows_reference::", "Windows.Foundation."), ("windows_time::", "Windows.Foundation."), ("windows_core::", "")):
            if path.startswith(prefix):
                rest = path[len(prefix):]
                return (ns + rest) if ns else {"IInspectable": "IInspectable", "IUnknown": "IUnknown"}.get(rest, rest)
        parts = self.ns.split(".")
        while path.startswith("super::"):
            parts = parts[:-1]
            path = path[len("super::"):]
        return ".".join(parts + path.split("::"))

    def parse(self):
        t = self.text
        for m in re.finditer(r"define_interface!\((\w+), \w+_Vtbl, 0x([0-9a-f_]+)\);", t):
            raw = m.group(2).replace("_", "")
            iid = f"{raw[0:8]}-{raw[8:12]}-{raw[12:16]}-{raw[16:20]}-{raw[20:32]}"
            vt = re.search(r"pub struct " + m.group(1) + r"_Vtbl \{\n    pub base__: windows_core::IInspectable_Vtbl,\n(.*?)\n\}", t, re.S)
            fields = []
            if vt:
                for line in vt.group(1).splitlines():
                    f = re.match(r"\s+(?:pub )?(\w+): (.*),$", line)
                    if f:
                        fields.append((f.group(1), f.group(2)))
            # A cfg'd field appears twice (pub fn / usize): keep the first.
            seen, unique = set(), []
            for f in fields:
                if f[0] not in seen:
                    seen.add(f[0])
                    unique.append(f)
            self.interfaces[m.group(1)] = (iid, unique)
        for m in re.finditer(r"pub struct (\w+)\(pub (i32|u32)\);\nimpl \1 \{\n(.*?)\n\}", t, re.S):
            values = re.findall(r"pub const (\w+): Self = Self\((-?\d+)\);", m.group(3))
            self.enums[m.group(1)] = (m.group(2), values)
        for m in re.finditer(r"pub struct (\w+) \{\n(.*?)\n\}\nimpl windows_core::imp::TypeKind for \1", t, re.S):
            fields = re.findall(r"pub (\w+): (.*),", m.group(2))
            self.structs[m.group(1)] = fields
        for m in re.finditer(r"impl windows_core::RuntimeType for (\w+) \{\n    const SIGNATURE: windows_core::imp::ConstBuffer = "
                             r"windows_core::imp::ConstBuffer::for_class::<Self, (.*?)>\(\);", t):
            self.classes[m.group(1)] = dict(default=m.group(2), interfaces=[], statics=[], factories=[], activatable=False)
        for name, info in self.classes.items():
            h = re.search(r"interface_hierarchy!\(" + name + r", (.*?)\);", t)
            r = re.search(r"required_hierarchy!\(" + name + r", (.*?)\);", t)
            for group in (h, r):
                if group:
                    for item in split_args(group.group(1)):
                        if item not in ("windows_core::IUnknown", "windows_core::IInspectable"):
                            info["interfaces"].append(item)
            block = re.search(r"\nimpl " + name + r" \{\n(.*?)\n\}\n", t, re.S)
            if block:
                body = block.group(1)
                if "fn IActivationFactory<" in body:
                    info["activatable"] = True
                for f in re.findall(r"fn (\w+)<R, F: FnOnce\(&(\w+)\)", body):
                    if f[1].endswith("Factory") and f[1] != "IGenericFactory":
                        info["factories"].append(f[1])
                    elif f[1] != "IGenericFactory":
                        info["statics"].append(f[1])
        # High-level methods: which interface and vtable field each calls, with its Rust signature.
        for impl in re.finditer(r"\nimpl (\w+) \{\n(.*?)\n\}\n", t, re.S):
            owner, body = impl.group(1), impl.group(2)
            for fn in re.finditer(r"    pub fn (\w+)(?:<([^>]*(?:<[^>]*>[^>]*)*)>)?\((?:&self)?(?:, )?(.*?)\) -> windows_core::Result<(.*?)>(?: |\n    where\n(.*?)\n    )\{\n(.*?)\n    \}\n",
                                  body + "\n", re.S):
                params, ret, where, code = fn.group(3), fn.group(4), fn.group(5) or "", fn.group(6)
                field = re.search(r"vtable\((?:this|self)\)\.(\w+)\)", code)
                if not field:
                    continue
                cast = re.search(r"cast::<([\w:]+)>", code)
                static = re.search(r"Self::(\w+)\(\|this\|", code)
                if cast:
                    iface = cast.group(1).split("::")[-1]
                    if cast.group(1).split("::")[0] in ("super",) or "windows_" in cast.group(1):
                        continue  # an interface of another namespace: defined there
                elif static:
                    iface = static.group(1)
                elif owner in self.interfaces:
                    iface = owner
                elif owner in self.classes:
                    iface = self.classes[owner]["default"]
                else:
                    continue
                generics = {}
                for w in re.findall(r"(P\d+): windows_core::Param<(.*?)>,", where):
                    generics[w[0]] = w[1]
                if ret == "Self":
                    ret = owner
                generics["Self"] = owner
                self.methods.setdefault((iface, field.group(1)), (params, ret, generics))

    def is_value(self, q):
        local = q.rsplit(".", 1)[-1]
        if q in PRIM.values():
            return True
        if q.startswith(self.ns + ".") and (local in self.enums or local in self.structs) and q.count(".") == self.ns.count(".") + 1:
            return True
        if q.startswith("Windows.Foundation.") and local in FOUNDATION_VALUES:
            return True
        return VALUE_NAMES.__contains__(q)

    def idl_type(self, rust, generics, element=False):
        rust = re.sub(r"\s*([<>])\s*", r"\1", rust.strip())
        if rust.startswith("&"):
            rust = rust[1:].strip()
        if rust in generics:
            rust = generics[rust]
        opt = re.match(r"Option<(.*)>$", rust)
        if opt:
            rust = opt.group(1)
        if rust in generics:
            rust = generics[rust]
        if rust in PRIM:
            return PRIM[rust]
        g = re.match(r"([\w:]+)<(.*)>$", rust)
        if g:
            base = self.qualify(g.group(1))
            args = [self.idl_type(a, generics, element=True) for a in split_args(g.group(2))]
            name = f"{base}<{','.join(args)}>"
            USED_GENERICS.add(name)
            return name + "*"
        q = self.qualify(rust)
        return q if self.is_value(q) else q + "*"


USED_GENERICS = set()
METHODS_NOT_PROPERTIES = {"EnsureSuccessStatusCode", "GetDeferral", "ToString"}
VALUE_NAMES = {"Windows.Security.Cryptography.Certificates.ChainValidationResult", "Windows.Networking.Sockets.SocketSslErrorSeverity", "Windows.Storage.Streams.UnicodeEncoding", "Windows.Storage.Streams.InputStreamOptions"}


def emit(modules):
    for mod in modules:
        for e in mod.enums:
            VALUE_NAMES.add(f"{mod.ns}.{e}")
        for st in mod.structs:
            VALUE_NAMES.add(f"{mod.ns}.{st}")
    sections = dict(forward=[], values=[], interfaces=[], classes=[])
    for mod in modules:
        fw = [f"namespace {mod.ns} {{"]
        for e in mod.enums:
            fw.append(f"    typedef enum {e} {e};")
        for st in mod.structs:
            fw.append(f"    typedef struct {st} {st};")
        for iface in mod.interfaces:
            fw.append(f"    interface {iface};")
        for cls in mod.classes:
            fw.append(f"    runtimeclass {cls};")
        fw.append("}\n")
        sections["forward"].append("\n".join(fw))
        vals = [f"namespace {mod.ns} {{"]
        for e, (kind, values) in mod.enums.items():
            flags = "flags, " if kind == "u32" else ""
            vals.append(f"    [{flags}contract(Windows.Foundation.UniversalApiContract, 1.0)]\n    enum {e}\n    {{")
            vals.append(",\n".join(f"        {n} = {v}" for n, v in values))
            vals.append("    };\n")
        for st, fields in mod.structs.items():
            vals.append(f"    [contract(Windows.Foundation.UniversalApiContract, 1.0)]\n    struct {st}\n    {{")
            for f, ty in fields:
                vals.append(f"        {mod.idl_type(ty, {})} {f};")
            vals.append("    };\n")
        vals.append("}\n")
        sections["values"].append("\n".join(vals))
        ifs = [f"namespace {mod.ns} {{"]
        for iface, (iid, fields) in mod.interfaces.items():
            ifs.append(f"    [\n        contract(Windows.Foundation.UniversalApiContract, 1.0),\n        uuid({iid})\n    ]\n    interface {iface} : IInspectable\n    {{")
            names = {f for f, _ in fields}
            for field, abi in fields:
                sig = mod.methods.get((iface, field))
                if not sig:
                    ifs.append(f"        /* {field}: signature not found */ HRESULT {field}();")
                    continue
                params, ret, generics = sig
                abi = abi_args(dict(fields)[field])
                args = []
                for position, p in enumerate(split_args(params)):
                    pname, ptype = p.split(":", 1)
                    pname, ptype = pname.strip(), ptype.strip()
                    arr = re.match(r"&(mut )?\[(.*)\]$", ptype)
                    if arr:
                        el = mod.idl_type(arr.group(2), generics, element=True)
                        args.append(f"[in] UINT32 {pname}_size")
                        args.append(f"[{'out' if arr.group(1) else 'in'}, size_is({pname}_size)] {el} *{pname}")
                    elif ptype.startswith("&mut "):
                        args.append(f"[out] {mod.idl_type(ptype[5:], generics)} *{pname}")
                    elif position < len(abi) and abi[position] == "*mut core::ffi::c_void":
                        args.append(f"[in] {boxed(mod, ptype, generics)} {pname}")
                    else:
                        args.append(f"[in] {mod.idl_type(ptype, generics)} {pname}")
                attrs, name = "", field
                if ret != "()":
                    rarr = re.match(r"windows_core::Array<(.*)>$", ret)
                    if rarr:
                        el = mod.idl_type(rarr.group(1), generics, element=True)
                        args.append("[out] UINT32 *value_size")
                        args.append(f"[out, retval, size_is(, *value_size)] {el} **value")
                    else:
                        taken = any(p.split(":", 1)[0].strip() == "value" for p in split_args(params))
                        rendered = boxed(mod, ret, generics) if abi and abi[-1] == "*mut *mut core::ffi::c_void" else mod.idl_type(ret, generics)
                        args.append(f"[out, retval] {rendered} *{'result' if taken else 'value'}")
                if field.startswith("Set") and field[3:] in names and len(split_args(params)) == 1 and ret == "()":
                    attrs, name = "[propput] ", field[3:]
                elif not params and ret != "()" and not field.endswith("Async") and field not in METHODS_NOT_PROPERTIES:
                    attrs = "[propget] "
                ifs.append(f"        {attrs}HRESULT {name}({', '.join(args)});")
            ifs.append("    }\n")
        ifs.append("}\n")
        sections["interfaces"].append("\n".join(ifs))
        cl = [f"namespace {mod.ns} {{"]
        for cls, info in mod.classes.items():
            attrs = []
            if info["activatable"]:
                attrs.append("activatable(Windows.Foundation.UniversalApiContract, 1.0)")
            for f in info["factories"]:
                attrs.append(f"activatable({mod.ns}.{f}, Windows.Foundation.UniversalApiContract, 1.0)")
            for st in info["statics"]:
                attrs.append(f"static({mod.ns}.{st}, Windows.Foundation.UniversalApiContract, 1.0)")
            attrs.append("contract(Windows.Foundation.UniversalApiContract, 1.0)")
            attrs.append("marshaling_behavior(agile)")
            cl.append("    [\n" + ",\n".join("        " + a for a in attrs) + "\n    ]")
            cl.append(f"    runtimeclass {cls}\n    {{")
            if "<" in info["default"]:
                default = mod.idl_type(info["default"], {})[:-1]
            else:
                default = f"{mod.ns}.{info['default']}"
            cl.append(f"        [default] interface {default};")
            for i in info["interfaces"]:
                rendered = mod.idl_type(i, {})[:-1]
                if rendered != default:
                    cl.append(f"        interface {rendered};")
            cl.append("    }\n")
        cl.append("}\n")
        sections["classes"].append("\n".join(cl))
    return sections


def draft(modules):
    sections = emit(modules)
    return ("/* @@FORWARD */\n" + "\n".join(sections["forward"]) + "\n" + "/* @@VALUES */\n" + "\n".join(sections["values"]) + "\n" +
            "/* @@GENERICS */\n" + "".join("        interface " + g + ";\n" for g in sorted(USED_GENERICS)) +
            "/* @@INTERFACES */\n" + "\n".join(sections["interfaces"]) + "\n" + "/* @@CLASSES */\n" + "\n".join(sections["classes"]) + "\n")


def assemble(d, head):
    parts = re.split(r'/\* @@(\w+) \*/\n', d)
    sec = {parts[i]: parts[i + 1] for i in range(1, len(parts), 2)}
    handler = ('Windows.Foundation.TypedEventHandler<Windows.Web.Http.Filters.HttpBaseProtocolFilter *, '
               'Windows.Web.Http.Filters.HttpServerCustomValidationRequestedEventArgs *>')
    ifs = sec['INTERFACES']
    ifs = ifs.replace('        HRESULT ServerCustomValidationRequested([in] Windows.Web.Http.Filters.F* handler, [out, retval] EventRevoker* *value);',
                      f'        [eventadd] HRESULT ServerCustomValidationRequested([in] {handler} *handler, [out, retval] EventRegistrationToken *token);')
    ifs = ifs.replace('        /* RemoveServerCustomValidationRequested: signature not found */ HRESULT RemoveServerCustomValidationRequested();',
                      '        [eventremove] HRESULT ServerCustomValidationRequested([in] EventRegistrationToken token);')
    assert 'not found' not in ifs
    gen = sec['GENERICS'].rstrip().splitlines() + [
        f'        interface {handler};',
        '        interface Windows.Foundation.IReference<Windows.Foundation.DateTime>;',
        '        interface Windows.Foundation.IReference<Windows.Foundation.TimeSpan>;']


    def tidy(text):
        text = re.sub(r'(\w|>)\*', r'\1 *', text)
        return text.replace('* *', '**').replace(',', ', ').replace(',  ', ', ')


    C, F = 'Windows.Foundation.Collections.', 'Windows.Foundation.'
    RANK = [F + 'IReference', C + 'IKeyValuePair', C + 'IIterator', C + 'IIterable', C + 'IVectorView', C + 'IVector', C + 'IMapView', C + 'IMap',
            F + 'AsyncOperationProgressHandler', F + 'AsyncOperationWithProgressCompletedHandler', F + 'IAsyncOperationWithProgress',
            F + 'TypedEventHandler']


    def implied(base, args):
        out = [(base, args)]
        one = {C + 'IIterable': [C + 'IIterator'], C + 'IVector': [C + 'IVectorView', C + 'IIterable', C + 'IIterator'],
               C + 'IVectorView': [C + 'IIterable', C + 'IIterator']}
        for extra in one.get(base, []):
            out.append((extra, args))
        if base in (C + 'IMap', C + 'IMapView'):
            pair = f'{C}IKeyValuePair<{args}>'
            out += [(C + 'IMapView', args), (C + 'IKeyValuePair', args), (C + 'IIterable', pair + ' *'), (C + 'IIterator', pair + ' *')]
        if base == F + 'IAsyncOperationWithProgress':
            out += [(F + 'AsyncOperationProgressHandler', args), (F + 'AsyncOperationWithProgressCompletedHandler', args)]
        if base == C + 'IIterable' and args.startswith(C + 'IKeyValuePair<'):
            out.append((C + 'IKeyValuePair', args[len(C + 'IKeyValuePair<'):args.rindex('>')]))
        return out


    entries = set()
    for line in gen:
        m = re.match(r'\s*interface (.*?)<(.*)>;$', tidy(line))
        for base, args in implied(m.group(1), m.group(2).strip()):
            entries.add((base, tidy(args).replace('  ', ' ')))
    ordered = sorted(entries, key=lambda e: (RANK.index(e[0]), e[1]))
    references = [f'        interface {base}<{args}>;' for base, args in ordered if base == F + 'IReference']
    gen = [f'        interface {base}<{args}>;' for base, args in ordered if base != F + 'IReference']
    return (head + tidy(sec['FORWARD']) + '\nnamespace Windows.Web.Http {\n    declare {\n' + '\n'.join(references) + '\n    }\n}\n\n' +
            tidy(sec['VALUES']) + '\nnamespace Windows.Web.Http {\n    declare {\n' + '\n'.join(gen) + '\n    }\n}\n\n' + tidy(ifs) + '\n' +
            tidy(sec['CLASSES']))


def verify(modules, idl):
    """Each method's arguments against its vtable entry: as many, and as indirect."""
    problems = []
    for mod in modules:
        for iface, (iid, fields) in mod.interfaces.items():
            body = re.search(r"interface " + iface + r" : IInspectable\n    \{\n(.*?)\n    \}", idl, re.S).group(1)
            methods = [line.strip() for line in body.splitlines() if "HRESULT" in line]
            if len(methods) != len(fields):
                problems.append(f"{iface}: {len(methods)} methods for {len(fields)} vtable entries")
                continue
            for (field, abi), method in zip(fields, methods):
                expected, args = abi_args(abi), re.search(r"\((.*)\);$", method).group(1)
                given = split_args(args) if args.strip() else []
                if len(given) != len(expected):
                    problems.append(f"{iface}.{field}: {len(given)} arguments for {len(expected)}")
                    continue
                for argument, entry in zip(given, expected):
                    flat = re.sub(r"<[^<>]*(<[^<>]*>[^<>]*)*>", "", argument)
                    out = "[out" in argument
                    if "HSTRING" not in argument and flat.count("*") - out != entry.count("*mut") - out:
                        problems.append(f"{iface}.{field}: {argument} against {entry}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", type=Path, help="directory holding (or to keep) the pinned windows-rs files")
    parser.add_argument("--check", action="store_true", help="compare with the committed IDL instead of writing it")
    args = parser.parse_args()
    modules = []
    for namespace, path, digest in SOURCES:
        cached = args.cache / (path.replace("/", "_") + ".rs") if args.cache else None
        if cached and cached.exists():
            data = cached.read_bytes()
        else:
            url = f"https://raw.githubusercontent.com/microsoft/windows-rs/{REVISION}/crates/libs/windows/src/Windows/{path}/mod.rs"
            data = urllib.request.urlopen(url).read()
            if cached:
                cached.parent.mkdir(parents=True, exist_ok=True)
                cached.write_bytes(data)
        if hashlib.sha256(data).hexdigest() != digest:
            raise RuntimeError(f"windows-rs source checksum differs: {path}")
        modules.append(Module(namespace, data.decode()))
    idl = assemble(draft(modules), (TARGET.parent / "windows.web.http.head.idl").read_text())
    problems = verify(modules, idl)
    if problems:
        raise RuntimeError("generated IDL disagrees with the vtables:\n" + "\n".join(problems))
    if args.check:
        if TARGET.read_text() != idl:
            sys.exit(f"{TARGET} differs from what windows-rs {REVISION} gives")
        return
    TARGET.write_text(idl)


if __name__ == "__main__":
    main()
