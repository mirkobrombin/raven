#!/usr/bin/env python3
"""Writes native/guest/raven-winrt/foundation.h and native/guest/raven-web/windows.web.http.h, the Windows Runtime types Raven's
components use as C declarations, from Microsoft's Windows metadata as the windows-rs project distributes it (MIT OR Apache-2.0, used
under MIT). Its generated bindings give each interface's identifier, the order of its methods and their signatures, the raw vtable layout
each method is checked against, and the type signatures the identifiers of generic instances are computed from, as the Windows Runtime
type system specifies; its windows-collections, windows-future and windows-reference crates give the generic interfaces.

windows.web.http.h declares the Windows.Web.Http, Windows.Web.Http.Headers and Windows.Web.Http.Filters namespaces, and what they need of
others that are not Windows.Foundation or Windows.Storage.Streams at their minimum. foundation.h, which raven-winrt and so each component
includes, declares what they need of Windows.Foundation, its collections and Windows.Storage.Streams, with the generic instances of those
types alone; the declarations of MinGW's headers for those namespaces differ between its releases, and only its classic COM headers are
included. Each interface sits under the guard widl's headers give it. With --check, the committed headers are compared with what the
pinned sources give instead of being written."""
import argparse
import hashlib
from pathlib import Path
import re
import sys
import urllib.request

REVISION = "58217a690d6256ee229044a174de67d72d28bc35"
SOURCE = f"https://raw.githubusercontent.com/microsoft/windows-rs/{REVISION}/crates/libs"
# The windows crate's modules: the namespaces declared here, then those whose types they name.
MODULES = (("Windows.Web.Http", "2d16c9b0e362b66443d58837588385e62c46c2dc548375992d692c2db5051626"),
           ("Windows.Web.Http.Headers", "08e2a3ac344c76759bb08d9fc56a1585570578777244231054ba5e86b0b5d637"),
           ("Windows.Web.Http.Filters", "270aa3244d5dd26fc57330cf046a79cbf8389168d6a54960d7c574656ecbb638"),
           ("Windows.Foundation", "1e9ed32ba69839fab0eb9ade6acc399480cc6a09c096ab5fdda74392f6195cc5"),
           ("Windows.Storage.Streams", "e5f4de19dc1a5ae60541221eaba5bab262c5ff8bb85ac8ebaad7cb426102ca6b"),
           ("Windows.Networking", "59f69e79980463a2c164580c2d51ab07c1fae4cb41053da33cdb7ce6346b9edc"),
           ("Windows.Networking.Sockets", "f8f73eeb72966b7549e604d7f9cc63b10e37323939855b4bac89739bb257e2d1"),
           ("Windows.Security.Cryptography.Certificates", "0c55a5435898dad5483fb1d0fb92ececcadce8bdb1de718901e73827d8fc9575"),
           ("Windows.Security.Credentials", "c21c1d45d201258bae67fe23a6ca94a83093a2e595aa8fe91735317d7bc13efc"),
           ("Windows.Globalization", "d2d392f8c526241975bff30e2eef19e65d9387632d1b369429c99b361f602b25"),
           ("Windows.System", "c19d46414f859385bd9adc9685e566dddce051c36e25003609a061f7e535a3c2"))
DECLARED = ("Windows.Web.Http", "Windows.Web.Http.Headers", "Windows.Web.Http.Filters")
# The crates of the generic interfaces, the asynchronous ones' base and the time structures, with the namespace each serves.
CRATES = (("collections", "Windows.Foundation.Collections", "45989f164dc1154d2afd15a507d088eda1d5eb0addfb82a600912dd839be1419"),
          ("future", "Windows.Foundation", "1997b5af103d19e3ddde11970d61ef82dd006b836c9495e20aaeb0aa13fdf9be"),
          ("reference", "Windows.Foundation", "6353cc1760afeb9d3120e1ec764d267369d0171d3891611eacbb20f8df54d1ca"),
          ("time", "Windows.Foundation", "25a8e2bf9bb05364b189f32f1d2323ecb51c47c5f5a43cffd52d5b8c7a6a2401"))
# The namespaces foundation.h declares in full, as far as the declared ones reach into them.
FOUNDATION = ("Windows.Foundation", "Windows.Foundation.Collections", "Windows.Storage.Streams")
HEADERS = ("inspectable.h", "asyncinfo.h", "eventtoken.h")
# Types of the classic COM headers rather than of a namespace.
GLOBAL = {"Windows.Foundation.AsyncStatus": "AsyncStatus", "Windows.Foundation.IAsyncInfo": "IAsyncInfo",
          "Windows.Foundation.EventRegistrationToken": "EventRegistrationToken"}
ROOT = Path(__file__).resolve().parents[1]
FOUNDATION_TARGET = ROOT / "native/guest/raven-winrt/foundation.h"
TARGET = ROOT / "native/guest/raven-web/windows.web.http.h"

PRIM = {"bool": ("boolean", "b1"), "u8": ("BYTE", "u1"), "i8": ("INT8", "i1"), "u16": ("UINT16", "u2"), "i16": ("INT16", "i2"),
        "u32": ("UINT32", "u4"), "i32": ("INT32", "i4"), "u64": ("UINT64", "u8"), "i64": ("INT64", "i8"), "f32": ("FLOAT", "f4"),
        "f64": ("DOUBLE", "f8"), "windows_core::GUID": ("GUID", "g16"), "windows_core::HSTRING": ("HSTRING", "string"),
        "windows_core::HRESULT": ("HRESULT", "struct(Windows.Foundation.HResult;i4)")}
TOKEN = ("struct", "Windows.Foundation.EventRegistrationToken")
OBJECT = ("object",)
# Methods without arguments that return a value but are not properties.
METHODS = {"EnsureSuccessStatusCode", "ToString", "First", "MoveNext", "GetDeferral", "GetView", "GetResults"}
# The namespace identifier of the generic instances' identifiers.
PINTERFACE = bytes.fromhex("11f47ad57b7342c0abae878b1e16adee")


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


def guid(raw):
    raw = raw.replace("_", "").replace("-", "").lower()
    return f"{raw[0:8]}-{raw[8:12]}-{raw[12:16]}-{raw[16:20]}-{raw[20:32]}"


def from_signature(signature):
    """The identifier of a generic instance: a name based (SHA-1) identifier of its signature, as the type system specifies."""
    digest = bytearray(hashlib.sha1(PINTERFACE + signature.encode()).digest()[:16])
    digest[6] = (digest[6] & 0x0f) | 0x50
    digest[8] = (digest[8] & 0x3f) | 0x80
    return guid(digest.hex())


class Source:
    """One file of the bindings, with its whitespace made single spaces, and what it declares for the namespace it serves."""

    def __init__(self, namespace, text, crate):
        self.ns = namespace
        self.crate = crate
        # One space for any whitespace, none inside the parentheses rustfmt breaks across lines, and no trailing commas there.
        self.text = re.sub(r",? \)", ")", re.sub(r"\( ", "(", re.sub(r"\s+", " ", text)))
        t = self.text
        self.interfaces = {}  # name -> (iid, base, [(field, [ABI argument])])
        for name, iid in re.findall(r"define_interface!\((\w+), \w+_Vtbl, 0x([0-9a-f_]+)\);", t):
            self.interfaces[name] = (guid(iid),) + self.vtable(name)
        self.templates = {}  # name -> (piid, [parameter], base, [(field, [ABI argument])])
        for name, params in re.findall(r"pub struct (\w+)<([\w, ]+)>\( ?windows_core::IUnknown,", t):
            piid = re.search(r"RuntimeType for " + name + r"<[\w, ]+> \{ const SIGNATURE: windows_core::imp::ConstBuffer = "
                             r"windows_core::imp::ConstBuffer::new\(\) ?\.push_slice\(b\"pinterface\(\{([0-9a-f-]{36})\}\"\)", t)
            if piid:
                self.templates[name] = (piid.group(1), [p.strip() for p in params.split(",")]) + self.vtable(name)
        self.enums = {}  # name -> [(value, number)]
        for name, body in re.findall(r"pub struct (\w+)\(pub (?:i32|u32)\); impl \1 \{ (.*?) \}", t):
            self.enums[name] = re.findall(r"pub const (\w+): Self = Self\((-?\d+)\);", body)
        self.structs = {}  # name -> [(field, Rust type)]
        for name, body in re.findall(r"pub struct (\w+) \{ ((?:pub \w+: [^{}]*?, )+)\}", t):
            if not name.endswith("_Vtbl"):
                fields = re.findall(r"pub (\w+): ([^{}]*?), ", body + " ")
                # The crates name the fields in Rust's snake case; the metadata's names are their Pascal case.
                self.structs[name] = [("".join(part.title() for part in field.split("_")) if crate else field, ty) for field, ty in fields]
        self.signatures = dict(re.findall(r"RuntimeType for (\w+) \{ const SIGNATURE: windows_core::imp::ConstBuffer = "
                                          r"windows_core::imp::ConstBuffer::from_slice\(b\"((?:enum|struct)\(.*?\))\"\);", t))
        self.classes = dict(re.findall(r"RuntimeType for (\w+) \{ const SIGNATURE: windows_core::imp::ConstBuffer = "
                                       r"windows_core::imp::ConstBuffer::for_class::<Self, (.*?)>\(\);", t))
        self.requires = {}  # interface, class or template -> [Rust type]
        for name, required in re.findall(r"required_hierarchy!\((\w+), (.*?)\);", t):
            self.requires.setdefault(name, []).extend(split_args(required))
        for required, name in re.findall(r"CanInto<([^{]*?)> for (\w+)<[\w, ]+> \{ const QUERY: bool = true;", t):
            self.requires.setdefault(name, []).append(required)
        self.methods = {}  # (interface, field) -> (owner, parameters, result, where, handler)
        self.parse_methods()

    def vtable(self, name):
        body = re.search(r"pub struct " + name + r"_Vtbl(?:<[\w, ]+>)?(?: where [^{]*?)? \{ (?:pub )?base__: windows_core::(\w+)_Vtbl, (.*?)\}", self.text)
        fields, seen = [], set()
        for field, abi in re.findall(r"(?:pub )?(\w+): unsafe extern \"system\" fn\((.*?)\) -> windows_core::HRESULT,", body.group(2)):
            if field not in seen:  # a field for a feature left out appears twice, the second time as usize
                seen.add(field)
                fields.append((field, [a.split(": ", 1)[-1] if re.match(r"\w+: ", a) else a for a in split_args(abi)[1:]]))
        return body.group(1), fields

    def parse_methods(self):
        t = self.text
        heads = [(m.start(), m.end(), m.group(1)) for m in re.finditer(r"\bimpl(?:<[^{]*?>)? (\w+)(?:<[\w, ]+>)? \{", t)]
        bounds = [m.start() for m in re.finditer(r"\bimpl\b", t)] + [len(t)]
        for start, end, owner in heads:
            stop = next(b for b in bounds if b > start)
            for fn in re.finditer(r"pub fn (\w+)(?:<(?:[^(){}]|\([^(){}]*\))*?>)?\((?:&self(?:, )?)?([^{}]*?)\) -> windows_core::Result<([^{}]*?)> ?(?:where (.*?))?\{(.*?)(?= pub fn |$)",
                                  t[end:stop]):
                params, result, where, body = fn.group(2), fn.group(3), fn.group(4) or "", fn.group(5)
                field = re.search(r"vtable\((?:this|self)\)\.(\w+)\)", body)
                if not field:
                    continue
                cast = re.search(r"cast::<([\w:]+)>", body)
                static = re.search(r"Self::(\w+)\(\|this\|", body)
                if cast:
                    if "::" in cast.group(1):
                        continue  # an interface of another namespace: declared there
                    interface = cast.group(1)
                elif static:
                    interface = static.group(1)
                elif owner in self.interfaces or owner in self.templates:
                    interface = owner
                elif owner in self.classes:
                    interface = self.classes[owner]
                else:
                    continue
                handler = re.search(r"<([\w:]+<.*?>)>::new\(move \|", body)
                self.methods.setdefault((interface, field.group(1)), (owner, params, result, where, handler.group(1) if handler else None))
        # An interface programs implement has a trait of its methods, typed as the wrappers are; it serves those without one.
        for trait in re.finditer(r"pub trait (\w+)_Impl(?:<[^{]*?>)?(?:: [^{]*?)? \{ (.*?) \}", t):
            for fn in re.finditer(r"fn (\w+)\(&self(?:, )?(.*?)\) -> windows_core::Result<(.*?)>;", trait.group(2)):
                self.methods.setdefault((trait.group(1), fn.group(1)), (trait.group(1), fn.group(2), fn.group(3), "", None))

    def qualify(self, path):
        """A path of the bindings as a dotted metadata name."""
        path = path.strip()
        for prefix, ns in (("windows_future::", "Windows.Foundation."), ("windows_collections::", "Windows.Foundation.Collections."),
                           ("windows_reference::", "Windows.Foundation."), ("windows_time::", "Windows.Foundation.")):
            if path.startswith(prefix):
                return ns + path[len(prefix):]
        parts = self.ns.split(".")
        if self.crate and "::" not in path:
            return self.ns + "." + path
        while path.startswith("super::"):
            parts = parts[:-1]
            path = path[len("super::"):]
        return ".".join(parts + path.split("::"))


class Model:
    def __init__(self, sources):
        self.sources = sources
        self.kinds, self.iids, self.home, self.templates = {}, {}, {}, {}
        for source in sources:
            for name, (iid, base, fields) in source.interfaces.items():
                dotted = f"{source.ns}.{name}"
                self.kinds[dotted] = "delegate" if base == "IUnknown" else "iface"
                self.iids[dotted] = iid
                self.home[dotted] = source
            for name in source.templates:
                self.templates[f"{source.ns}.{name}"] = source
            for name in source.enums:
                self.kinds[f"{source.ns}.{name}"] = "enum"
                self.home[f"{source.ns}.{name}"] = source
            for name in source.structs:
                if name in source.signatures:
                    self.kinds[f"{source.ns}.{name}"] = "struct"
                    self.home[f"{source.ns}.{name}"] = source
            for name in source.classes:
                self.kinds[f"{source.ns}.{name}"] = "class"
                self.home[f"{source.ns}.{name}"] = source
        self.kinds["Windows.Foundation.EventRegistrationToken"] = "struct"
        self.used = []

    def resolve(self, rust, source, binding, where=None):
        """A Rust type of the bindings as a type of the model: a tuple of its kind and name."""
        rust = re.sub(r"\s*([<>])\s*", r"\1", rust.strip()).removeprefix("&").strip()
        for wrapper in (r"Option<(.*)>", r"windows_core::Ref<(.*)>", r"<(\w+) as windows_core::imp::Type<\1>>::Default"):
            m = re.fullmatch(wrapper, rust)
            if m:
                return self.resolve(m.group(1), source, binding, where)
        if rust in binding:
            return binding[rust]
        if where and rust in where:
            return self.resolve(where[rust], source, binding)
        if rust in PRIM:
            return ("prim",) + PRIM[rust]
        if rust == "windows_core::IInspectable":
            return OBJECT
        g = re.fullmatch(r"([\w:]+)<(.*)>", rust)
        if g:
            return ("generic", source.qualify(g.group(1)), tuple(self.resolve(a, source, binding, where) for a in split_args(g.group(2))))
        dotted = source.qualify(rust)
        if dotted not in self.kinds:
            raise RuntimeError(f"{source.ns}: no type for {rust}")
        return (self.kinds[dotted], dotted)

    def default(self, dotted):
        source = self.home[dotted]
        return self.resolve(source.classes[dotted.rsplit(".", 1)[1]], source, {})

    def signature(self, ty):
        kind = ty[0]
        if kind == "prim":
            return ty[2]
        if kind == "object":
            return "cinterface(IInspectable)"
        if ty == TOKEN:
            return "struct(Windows.Foundation.EventRegistrationToken;i8)"
        if kind in ("enum", "struct"):
            return self.home[ty[1]].signatures[ty[1].rsplit(".", 1)[1]]
        if kind == "iface":
            return "{" + self.iids[ty[1]] + "}"
        if kind == "delegate":
            return "delegate({" + self.iids[ty[1]] + "})"
        if kind == "class":
            return f"rc({ty[1]};{self.signature(self.default(ty[1]))})"
        source = self.templates[ty[1]]
        piid = source.templates[ty[1].rsplit(".", 1)[1]][0]
        return "pinterface({" + piid + "};" + ";".join(self.signature(a) for a in ty[2]) + ")"

    def name(self, ty):
        """The C name of an interface, a generic instance, a structure or an enumeration."""
        if ty[0] == "generic":
            local = ty[1].rsplit(".", 1)[1]
            if self.templates[ty[1]].templates[local][2] == "IUnknown" and not local.startswith("I"):
                local = "I" + local
            return f"__F{local}_{len(ty[2])}_" + "_".join(self.mangle(a) for a in ty[2])
        if ty[1] in GLOBAL:
            return GLOBAL[ty[1]]
        namespace, local = ty[1].rsplit(".", 1)
        return "__x_ABI_C" + "_C".join(namespace.split(".")) + "_C" + ("I" + local if ty[0] == "delegate" and not local.startswith("I") else local)

    def mangle(self, ty):
        """A type as part of a generic instance's name."""
        kind = ty[0]
        if kind == "prim":
            return ty[1]
        if kind == "object":
            return "IInspectable"
        if kind in ("enum", "struct"):
            return ty[1].rsplit(".", 1)[1]
        if kind == "generic":
            return self.name(ty)
        return "__C".join(ty[1].split("."))

    def c(self, ty):
        """A type as C passes it; the type is noted among those the header declares."""
        self.note(ty)
        kind = ty[0]
        if kind == "prim":
            return ty[1]
        if kind == "object":
            return "IInspectable *"
        if kind in ("enum", "struct"):
            return self.name(ty)
        if kind == "class":
            return self.c(self.default(ty[1]))
        return self.name(ty) + " *"

    def note(self, ty):
        if ty not in self.used:
            self.used.append(ty)
        return ty

    def is_value(self, ty):
        return ty[0] in ("prim", "enum", "struct") and ty[1] != "HSTRING"

    def members(self, interface, source, fields, binding, itself=None):
        """An interface's methods after those of IUnknown and IInspectable: (name, [(C type, parameter)])."""
        names = {field for field, _ in fields}
        out = []
        for field, abi in fields:
            if field.startswith("Remove") and field[6:] in names:
                out.append((f"remove_{field[6:]}", [("EventRegistrationToken", "token")]))
                continue
            sig = source.methods.get((interface, field))
            if not sig:
                raise RuntimeError(f"{interface}.{field}: signature not found")
            owner, params, result, where_text, handler = sig
            where = {}
            for clause in split_args(where_text):
                m = re.fullmatch(r"(P\d+): windows_core::Param<(.*)>", clause.strip())
                if m:
                    where[m.group(1)] = m.group(2)
            local = dict(binding)
            if owner in source.classes:
                local["Self"] = self.resolve(owner, source, binding)
            elif itself:
                local["Self"] = itself
            if "Remove" + field in names:
                ty = self.resolve(handler, source, local) if handler else self.resolve(split_args(params)[0].split(":", 1)[1], source, local, where)
                out.append((f"add_{field}", [(self.c(ty), "handler"), ("EventRegistrationToken *", "token")]))
                continue
            args, position = [], 0
            for p in split_args(params):
                if ":" not in p:
                    raise RuntimeError(f"{interface}.{field}: parameter {p!r} of {params!r}")
                pname, ptype = (part.strip() for part in p.split(":", 1))
                arr = re.fullmatch(r"&(mut )?\[(.*)\]", ptype)
                if arr:
                    args += [("UINT32", f"{pname}_size"), (pointer(self.c(self.resolve(arr.group(2), source, local, where))), pname)]
                    position += 2
                    continue
                if ptype.startswith("&mut "):
                    args.append((pointer(self.c(self.resolve(ptype[5:], source, local, where))), pname))
                else:
                    ty = self.resolve(ptype, source, local, where)
                    if self.is_value(ty) and position < len(abi) and abi[position] == "*mut core::ffi::c_void":
                        ty = ("generic", "Windows.Foundation.IReference", (ty,))
                    args.append((self.c(ty), pname))
                position += 1
            if result != "()":
                arr = re.fullmatch(r"windows_core::Array<(.*)>", result)
                if arr:
                    args += [("UINT32 *", "value_size"), (pointer(pointer(self.c(self.resolve(arr.group(1), source, local, where)))), "value")]
                else:
                    ty = self.resolve("Self" if result == "Self" else result, source, local, where)
                    if self.is_value(ty) and abi and abi[-1] == "*mut *mut core::ffi::c_void":
                        ty = ("generic", "Windows.Foundation.IReference", (ty,))
                    taken = any(p.split(":", 1)[0].strip() == "value" for p in split_args(params))
                    args.append((pointer(self.c(ty)), "result" if taken else "value"))
            name = field
            if field.startswith("Set") and field[3:] in names and len(split_args(params)) == 1 and result == "()":
                name = "put_" + field[3:]
            elif not params and result != "()" and not field.endswith("Async") and field not in METHODS:
                name = "get_" + field
            if len(args) != len(abi):
                raise RuntimeError(f"{interface}.{field}: {len(args)} arguments for {len(abi)}")
            for (ctype, pname), entry in zip(args, abi):
                # A string handle and an interface pointer are as indirect as each other in the ABI.
                depth = ctype.count("*") + ctype.startswith("HSTRING")
                if depth != entry.count("*mut") + entry.count("*const") and "AbiType" not in entry:
                    raise RuntimeError(f"{interface}.{field}: {ctype} {pname} against {entry}")
            out.append((name, args))
        return out

    def generic_members(self, ty):
        source = self.templates[ty[1]]
        name = ty[1].rsplit(".", 1)[1]
        piid, params, base, fields = source.templates[name]
        return base, self.members(name, source, fields, dict(zip(params, ty[2])), ty)

    def requirements(self, ty):
        """The generic instances an interface, a class or a generic instance requires."""
        name = ty[1].rsplit(".", 1)[1]
        if ty[0] == "generic":
            source = self.templates[ty[1]]
            binding = dict(zip(source.templates[name][1], ty[2]))
        else:
            source, binding = self.home[ty[1]], {}
        return [self.resolve(r, source, binding) for r in source.requires.get(name, [])
                if r not in ("windows_core::IUnknown", "windows_core::IInspectable")]

    def class_interfaces(self, dotted):
        """A class's interfaces: its default, those it requires, and the factories and statics its methods go through."""
        source, local = self.home[dotted], dotted.rsplit(".", 1)[1]
        found = [self.default(dotted)] + self.requirements(("class", dotted))
        for (interface, _), (owner, *_) in source.methods.items():
            ty = ("iface", f"{source.ns}.{interface}")
            if owner == local and f"{source.ns}.{interface}" in self.kinds and ty not in found:
                found.append(ty)
        return found

    def is_foundation(self, ty):
        """Whether foundation.h declares a type: one of its namespaces', or a generic instance of such types alone."""
        if ty[0] in ("prim", "object"):
            return True
        if ty[0] == "generic":
            return all(self.is_foundation(a) for a in ty[2])
        return ty[1].rsplit(".", 1)[0] in FOUNDATION


def pointer(ctype):
    return ctype + ("*" if ctype.endswith("*") else " *")


def vtable(lines, name, base, members):
    lines.append(f"typedef struct {name}Vtbl {{")
    lines.append(f"    HRESULT (STDMETHODCALLTYPE *QueryInterface)({name} *This, REFIID riid, void **object);")
    lines.append(f"    ULONG (STDMETHODCALLTYPE *AddRef)({name} *This);")
    lines.append(f"    ULONG (STDMETHODCALLTYPE *Release)({name} *This);")
    if base == "IInspectable":
        lines.append(f"    HRESULT (STDMETHODCALLTYPE *GetIids)({name} *This, ULONG *count, IID **iids);")
        lines.append(f"    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)({name} *This, HSTRING *name);")
        lines.append(f"    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)({name} *This, TrustLevel *level);")
    for method, args in members:
        rendered = "".join(f", {ctype}{'' if ctype.endswith('*') else ' '}{pname}" for ctype, pname in args)
        lines.append(f"    HRESULT (STDMETHODCALLTYPE *{method})({name} *This{rendered});")
    lines.append(f"}} {name}Vtbl;")
    lines.append(f"struct {name} {{ const {name}Vtbl *lpVtbl; }};")


def define_guid(name, iid):
    raw = iid.replace("-", "")
    return f"DEFINE_GUID({name}, 0x{raw[0:8]}, 0x{raw[8:12]}, 0x{raw[12:16]}, " + ", ".join(f"0x{raw[i:i + 2]}" for i in range(16, 32, 2)) + ");"


def render(model):
    """The two headers' lines: foundation.h's, then windows.web.http.h's."""
    full = DECLARED + FOUNDATION
    interfaces, enums, structs, generics, foreign = [], [], [], [], []
    for source in model.sources:
        if not source.crate and source.ns in DECLARED:
            for name in [*source.interfaces, *source.enums, *[s for s in source.structs if s in source.signatures], *source.classes]:
                model.note((model.kinds[f"{source.ns}.{name}"], f"{source.ns}.{name}"))
    # Every type a declared one names, through its methods, fields, requirements and, for a class, its interfaces.
    done = set()
    while len(done) < len(model.used):
        for ty in list(model.used):
            if ty in done:
                continue
            done.add(ty)
            if ty[0] in ("prim", "object") or ty == TOKEN or ty[0] != "generic" and ty[1] in GLOBAL:
                continue
            if ty[0] == "generic":
                for a in ty[2]:
                    model.note(a)
                generics.append((ty, *model.generic_members(ty)))
                for required in model.requirements(ty):
                    model.note(required)
                continue
            namespace, local = ty[1].rsplit(".", 1)
            source = model.home[ty[1]]
            if ty[0] == "class":
                for interface in model.class_interfaces(ty[1]) if namespace in full else [model.default(ty[1])]:
                    model.note(interface)
            elif ty[0] in ("iface", "delegate"):
                if namespace in full:
                    iid, base, fields = source.interfaces[local]
                    interfaces.append((ty, iid, base, model.members(local, source, fields, {})))
                    for required in model.requirements(ty):
                        model.note(required)
                else:
                    foreign.append(ty)
            elif ty[0] == "enum":
                enums.append((ty, source.enums[local]))
            else:
                structs.append((ty, [(field, model.note(model.resolve(rust, source, {}))) for field, rust in source.structs[local]]))
    order = lambda entries: sorted(entries, key=lambda entry: model.name(entry[0]))
    parts = []
    for foundation in (True, False):
        mine = lambda entries: order([e for e in entries if model.is_foundation(e[0]) == foundation])
        guard = "RAVEN_WINRT_FOUNDATION_H" if foundation else "RAVEN_WINDOWS_WEB_HTTP_H"
        lines = ["/* Generated by scripts/generate-winrt-header.py from windows-rs " + REVISION + " (Microsoft's Windows metadata, MIT): do not edit. */",
                 f"#ifndef {guard}", f"#define {guard}", ""]
        lines += [f"#include <{h}>" for h in HEADERS] if foundation else ['#include "../raven-winrt/foundation.h"']
        lines.append("")
        for ty in [e[0] for e in mine(generics)] + sorted(t for t in foreign if not foundation) + [e[0] for e in mine(interfaces)]:
            name = model.name(ty)
            lines += [f"#ifndef __{name}_FWD_DEFINED__", f"#define __{name}_FWD_DEFINED__", f"typedef struct {name} {name};", "#endif"]
        lines.append("")
        for ty, values in mine(enums):
            name, local = model.name(ty), ty[1].rsplit(".", 1)[1]
            lines.append(f"typedef enum {name} {{")
            lines.append(",\n".join(f"    {local}_{value} = {number}" for value, number in values))
            lines.append(f"}} {name};")
        lines.append("")
        for ty, fields in mine(structs):
            name = model.name(ty)
            lines.append(f"typedef struct {name} {{")
            for field, fty in fields:
                ctype = model.c(fty)
                lines.append(f"    {ctype}{'' if ctype.endswith('*') else ' '}{field};")
            lines.append(f"}} {name};")
        for ty, base, members in mine(generics):
            name = model.name(ty)
            lines += ["", f"#ifndef __{name}_INTERFACE_DEFINED__", f"#define __{name}_INTERFACE_DEFINED__",
                      define_guid("IID_" + name, from_signature(model.signature(ty)))]
            vtable(lines, name, base, members)
            lines.append("#endif")
        for ty, iid, base, members in mine(interfaces):
            name = model.name(ty)
            lines += ["", f"#ifndef __{name}_INTERFACE_DEFINED__", f"#define __{name}_INTERFACE_DEFINED__", define_guid("IID_" + name, iid)]
            vtable(lines, name, base, members)
            lines.append("#endif")
        lines += ["", "#endif", ""]
        parts.append("\n".join(lines))
    return parts


def load(path, digest, cache, url):
    cached = cache / path if cache else None
    if cached and cached.exists():
        data = cached.read_bytes()
    else:
        data = urllib.request.urlopen(url).read()
        if cached:
            cached.parent.mkdir(parents=True, exist_ok=True)
            cached.write_bytes(data)
    if hashlib.sha256(data).hexdigest() != digest:
        raise RuntimeError(f"windows-rs source checksum differs: {path}")
    return data.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", type=Path, help="directory holding (or to keep) the pinned windows-rs files")
    parser.add_argument("--check", action="store_true", help="compare with the committed header instead of writing it")
    args = parser.parse_args()
    sources = []
    for namespace, digest in MODULES:
        path = namespace.removeprefix("Windows.").replace(".", "/")
        text = load(path.replace("/", "_") + ".rs", digest, args.cache, f"{SOURCE}/windows/src/Windows/{path}/mod.rs")
        sources.append(Source(namespace, text, False))
    for crate, namespace, digest in CRATES:
        sources.append(Source(namespace, load(f"crate_{crate}.rs", digest, args.cache, f"{SOURCE}/{crate}/src/bindings.rs"), True))
    for target, header in zip((FOUNDATION_TARGET, TARGET), render(Model(sources))):
        if args.check:
            if target.read_text() != header:
                sys.exit(f"{target} differs from what windows-rs {REVISION} gives")
        else:
            target.write_text(header)


if __name__ == "__main__":
    main()
