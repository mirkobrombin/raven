#!/usr/bin/env python3
"""Writes native/upstream/raven-appx/appxpackaging.h, the packaging API of Windows (the AppxFactory and AppxBundleFactory classes, their
readers and writers) as C declarations, from Microsoft's Windows metadata as the windows-rs project distributes it (MIT OR Apache-2.0, used
under MIT). Its generated bindings give each interface's identifier and base, the order of its methods and their raw ABI, the parameter names
(the vtable shims) and the interfaces behind the ABI's untyped pointers (the wrapper methods); the three are checked against one another.
MinGW has no appxpackaging.h. With --check, the committed header is compared with what the pinned sources give instead of being written."""
import argparse
import hashlib
from pathlib import Path
import re
import sys
import urllib.request

REVISION = "58217a690d6256ee229044a174de67d72d28bc35"
SOURCES = (("appxpackaging", "3f6aa927339038fbd8d7e247e225206aaad3e98ec4679c006b020058a554c64b"),
           ("winerror", "68711e86d0c408f24c35ee39752fc719be2ad54b639d8d4de90d5c9b6cf579ff"))
TARGET = Path(__file__).resolve().parents[1] / "native/upstream/raven-appx/appxpackaging.h"

SCALARS = {"windows_core::PCWSTR": "LPCWSTR", "windows_core::PWSTR": "LPWSTR", "windows_core::PCSTR": "LPCSTR", "windows_core::BOOL": "BOOL",
           "windows_core::HRESULT": "HRESULT", "windows_core::GUID": "GUID", "bool": "BOOL", "i32": "INT32", "u32": "UINT32"}
# Interfaces of other headers the packaging API passes; the rest are its own.
FOREIGN = {"IStream", "IUri", "IXMLDOMDocument", "IUnknown"}


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


def c_type(rust, interfaces):
    """A Rust type of the bindings as C; None for the ABI's untyped interface pointers."""
    rust = rust.strip().replace("super::", "").replace("windows_core::IUnknown", "IUnknown")
    if rust in ("*mut core::ffi::c_void", "*mut *mut core::ffi::c_void"):
        return None
    m = re.fullmatch(r"core::mem::ManuallyDrop<Option<(\w+)>>", rust)
    if m:
        return m.group(1) + " *"
    m = re.fullmatch(r"Option<(.+)>", rust)
    if m:
        return c_type(m.group(1), interfaces)
    m = re.fullmatch(r"\*(mut|const) (.+)", rust)
    if m:
        inner = c_type(m.group(2), interfaces)
        if inner is None:
            return None
        return ("const " if m.group(1) == "const" else "") + inner + (" *" if not inner.endswith("*") else "*")
    if rust in SCALARS:
        return SCALARS[rust]
    if rust in interfaces or rust in FOREIGN:
        return rust + " *"
    if re.fullmatch(r"\w+", rust):
        return rust
    raise RuntimeError(f"no C type for {rust}")


class Module:
    def __init__(self, text, errors):
        self.text = text
        self.interfaces = {}  # name -> (iid, base)
        for name, iid in re.findall(r"define_interface!\((\w+), \w+_Vtbl, 0x([0-9a-f_]+)\);", text):
            raw = iid.replace("_", "")
            self.interfaces[name] = f"{raw[0:8]}-{raw[8:12]}-{raw[12:16]}-{raw[16:20]}-{raw[20:32]}"
        self.bases = {name: re.search(r"pub struct " + name + r"_Vtbl \{\n    pub base__: (?:windows_core::)?(\w+)_Vtbl,", text).group(1)
                      for name in self.interfaces}
        self.enums = {}  # type -> [(name, value)]
        for name, ty, value in re.findall(r"^pub const (\w+): (\w+) = (-?\d+);$", text, re.M):
            self.enums.setdefault(ty, []).append((name, int(value)))
        self.enum_types = re.findall(r"^pub type (\w+) = i32;$", text, re.M)
        self.classes = re.findall(r"^pub const (\w+): windows_core::GUID = windows_core::GUID::from_u128\(0x([0-9a-f_]+)\);$", text, re.M)
        self.structs = []
        for name, body in re.findall(r"^pub struct (\w+) \{\n((?:    pub \w+: [^\n]+,\n)+)\}", text, re.M):
            if not name.endswith("_Vtbl"):
                self.structs.append((name, re.findall(r"    pub (\w+): ([^\n]+),\n", body)))
        self.errors = re.findall(r"^pub const (APPX_E_\w+): windows_core::HRESULT = windows_core::HRESULT\((0x[0-9A-Fa-f]+)_u32 as _\);$", errors, re.M)

    def vtable(self, name):
        """The methods an interface adds, each as (method, [(parameter, C type)])."""
        body = re.search(r"pub struct " + name + r"_Vtbl \{\n(.*?)\n\}", self.text, re.S).group(1)
        fields = re.findall(r"^    pub (\w+): unsafe extern \"system\" fn\((.*)\) -> windows_core::HRESULT,$", body, re.M)
        if re.search(r"^    pub (?!base__)\w+: (?!unsafe extern)", body, re.M):
            raise RuntimeError(f"{name}: a vtable field that is not a method")
        shims = dict(re.findall(r"unsafe extern \"system\" fn (\w+)<Identity: " + name + r"_Impl, const OFFSET: isize>\((.*?)\) -> windows_core::HRESULT \{",
                                self.text))
        impl = re.search(r"^impl " + name + r" \{\n(.*?)^\}", self.text, re.S | re.M)
        wrappers = {}
        for m in re.finditer(r"pub unsafe fn (\w+)(?:<([^>]*)>)?\(&self(?:, )?(.*?)\)(?: -> ([^{\n]+?))?\s*(?:where\n(.*?))?\{", impl.group(1) if impl else "", re.S):
            wrappers[m.group(1)] = m
        methods = []
        for field, abi in fields:
            raw = split_args(abi)[1:]
            names = [argument.split(":")[0].strip().removeprefix("r#") for argument in split_args(shims[field])[1:]]
            if len(names) != len(raw):
                raise RuntimeError(f"{name}.{field}: the shim has {len(names)} parameters for {len(raw)}")
            types = [c_type(argument, self.interfaces) for argument in raw]
            if None in types:
                given = self.wrapper_types(name, field, wrappers.get(field))
                if len(given) != len(raw):
                    raise RuntimeError(f"{name}.{field}: the wrapper gives {len(given)} parameters for {len(raw)}")
                for index, (known, wrapped) in enumerate(zip(types, given)):
                    if known is None:
                        if wrapped is None or not wrapped.endswith("*"):
                            raise RuntimeError(f"{name}.{field}: no interface for parameter {index}")
                        if raw[index].count("*mut") != wrapped.count("*"):
                            raise RuntimeError(f"{name}.{field}: {wrapped} against {raw[index]}")
                        types[index] = wrapped
            methods.append((field, list(zip(names, types))))
        return methods

    def wrapper_types(self, name, field, m):
        """The C types a wrapper method passes, in ABI order (a slice is its length and its elements, a Result its last parameter)."""
        if m is None:
            raise RuntimeError(f"{name}.{field}: untyped pointers and no wrapper")
        bounds = dict(re.findall(r"(P\d): windows_core::Param<([^>]+)>", (m.group(2) or "") + (m.group(5) or "")))
        out = []
        for argument in split_args(m.group(3) or ""):
            ty = argument.split(":", 1)[1].strip()
            if ty in bounds:
                out.append(c_type(bounds[ty], self.interfaces))
            elif ty.startswith("&["):
                out += ["UINT32", "const " + c_type(ty[2:-1], self.interfaces) + " *"]
            else:
                out.append(c_type(ty, self.interfaces))
        result = (m.group(4) or "").strip()
        r = re.fullmatch(r"windows_core::Result<(.+)>", result)
        if r and r.group(1) != "()":
            inner = c_type(r.group(1), self.interfaces)
            out.append(inner + ("*" if inner.endswith("*") else " *"))
        return out

    def chain(self, name):
        """Every method of an interface, those of its bases first."""
        if name == "IUnknown":
            return [("QueryInterface", [("riid", "REFIID"), ("object", "void **")]), ("AddRef", None), ("Release", None)]
        return self.chain(self.bases[name]) + self.vtable(name)


def render(module):
    lines = ["/* Generated by scripts/generate-appx-header.py from windows-rs " + REVISION + " (Microsoft's Windows metadata, MIT): do not edit. */",
             "#ifndef RAVEN_APPXPACKAGING_H", "#define RAVEN_APPXPACKAGING_H", "", "#include <objidl.h>", "#include <urlmon.h>", "#include <msxml.h>", ""]
    for code, value in module.errors:
        lines.append(f"#ifndef {code}\n#define {code} _HRESULT_TYPEDEF_({value}L)\n#endif")
    lines.append("")
    for ty in module.enum_types:
        lines.append(f"typedef INT32 {ty};")
        for name, value in sorted(module.enums.get(ty, []), key=lambda entry: (entry[1], entry[0])):
            lines.append(f"#define {name} (({ty}){value})")
    lines.append("")
    for name in module.interfaces:
        lines.append(f"typedef struct {name} {name};")
    lines.append("")
    for name, fields in module.structs:
        lines.append(f"typedef struct {name} {{")
        for field, ty in fields:
            lines.append(f"    {c_type(ty, module.interfaces)} {field};".replace("* ", "*"))
        lines.append(f"}} {name};")
    lines.append("")
    for name, guid in module.classes:
        raw = guid.replace("_", "")
        lines.append(f"DEFINE_GUID(CLSID_{name}, 0x{raw[0:8]}, 0x{raw[8:12]}, 0x{raw[12:16]}, " + ", ".join(
            f"0x{raw[i:i + 2]}" for i in range(16, 32, 2)) + ");")
    for name, iid in module.interfaces.items():
        raw = iid.replace("-", "")
        lines.append("")
        lines.append(f"DEFINE_GUID(IID_{name}, 0x{raw[0:8]}, 0x{raw[8:12]}, 0x{raw[12:16]}, " + ", ".join(
            f"0x{raw[i:i + 2]}" for i in range(16, 32, 2)) + ");")
        lines.append(f"typedef struct {name}Vtbl {{")
        for method, parameters in module.chain(name):
            if parameters is None:
                lines.append(f"    ULONG (STDMETHODCALLTYPE *{method})({name} *This);")
                continue
            rendered = "".join(f", {ty}{'' if ty.endswith('*') else ' '}{parameter}" for parameter, ty in parameters)
            lines.append(f"    HRESULT (STDMETHODCALLTYPE *{method})({name} *This{rendered});")
        lines.append(f"}} {name}Vtbl;")
        lines.append(f"struct {name} {{ const {name}Vtbl *lpVtbl; }};")
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", type=Path, help="directory holding (or to keep) the pinned windows-rs files")
    parser.add_argument("--check", action="store_true", help="compare with the committed header instead of writing it")
    args = parser.parse_args()
    texts = []
    for path, digest in SOURCES:
        cached = args.cache / (path + ".rs") if args.cache else None
        if cached and cached.exists():
            data = cached.read_bytes()
        else:
            url = f"https://raw.githubusercontent.com/microsoft/windows-rs/{REVISION}/crates/libs/windows/src/Windows/Win32/{path}/mod.rs"
            data = urllib.request.urlopen(url).read()
            if cached:
                cached.parent.mkdir(parents=True, exist_ok=True)
                cached.write_bytes(data)
        if hashlib.sha256(data).hexdigest() != digest:
            raise RuntimeError(f"windows-rs source checksum differs: {path}")
        texts.append(data.decode())
    header = render(Module(*texts))
    if args.check:
        if TARGET.read_text() != header:
            sys.exit(f"{TARGET} differs from what windows-rs {REVISION} gives")
        return
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(header)


if __name__ == "__main__":
    main()
