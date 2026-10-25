#!/usr/bin/env python3
"""Describe, for 32-bit programs, how every Vulkan structure and command crosses into the bridge's 64-bit layouts.

The bridge reads structures laid out the way the host compiler lays them out. A 32-bit program lays them out with 4-byte pointers,
size_t and dispatchable handles, so every offset after one of those differs. The 32-bit layout is asked of an i686 MinGW-w64 compiler
(generate-vulkan-layout32.py); the 64-bit one is taken from the host headers by offsetof in the generated tables. A runtime engine
walks the tables to rebuild a program's arguments in the layout the bridge expects, and to copy what the driver filled back.

usage: generate-vulkan-thunk32.py vk.xml vulkan_core.h OUTPUT.inc I686_COMPILER INCLUDE_DIRECTORY
"""

import importlib.util
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), HERE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


marshal = load("generate-vulkan-marshal")
layout32 = load("generate-vulkan-layout32")

SIZE_T_TYPES = {"size_t"}
OPAQUE_TYPES = {"HINSTANCE", "HWND", "HMONITOR", "HANDLE", "LPCWSTR", "HMODULE", "RROutput", "Display", "LPVOID", "void"}
NO_LENGTH = "no_length"


class Generator:
    def __init__(self, registry, header_text, layouts, sizes):
        self.registry = registry
        self.declared = set(re.findall(r"\bVk\w+\b", header_text))
        # Values the headers define only for beta extensions are not available to a normal build.
        released = re.sub(r"#ifdef VK_ENABLE_BETA_EXTENSIONS.*?#endif", "", header_text, flags=re.S)
        self.stypes = set(re.findall(r"\b(VK_STRUCTURE_TYPE_\w+)\s*=", released))
        self.layouts = layouts
        self.sizes = sizes
        self.struct_names = [name for name in sorted(registry.structs) if name in layouts]
        self.index = {name: position for position, name in enumerate(self.struct_names)}
        self.supported = {}
        self.lens = [("end", 0, 0)]
        self.lens_at = {}
        self.fields = []
        self.struct_rows = []
        self.params = []
        self.commands = []

    def base(self, member):
        return self.registry.resolve_struct(member.type)

    def kind_of(self, member):
        registry = self.registry
        base = self.base(member)
        if member.name == "pNext" and member.pointers == 1:
            return "pnext"
        if member.type.startswith("PFN_"):
            return "widen_pointer"
        if member.pointers == 0:
            if base in registry.handles:
                if registry.handles[base]["dispatchable"]:
                    return "dhandles" if member.array else "dhandle"
                return "scalar"
            if base in registry.structs:
                return "nested"
            if base in registry.unions:
                if base == "VkDescriptorDataEXT":
                    return "descriptor_data"
                return "raw_union"
            if base in SIZE_T_TYPES:
                return "widen_value"
            if base in OPAQUE_TYPES:
                return "widen_pointer"
            return "scalar"
        if member.pointers == 1:
            if base == "char":
                return "widen_pointer"
            if base in registry.structs:
                return "pointer_struct"
            if base in registry.handles and registry.handles[base]["dispatchable"]:
                return "pointer_dhandles"
            return "widen_pointer"
        if member.pointers == 2 and base == "char":
            return "pointer_strings"
        if member.pointers == 2 and base in registry.structs:
            return "pointer_struct_pointers"
        return None

    def plain(self, name, seen=()):
        """True when a structure or union holds no pointers, handles or pointer-sized members, so both ABIs lay it out alike."""
        name = self.registry.resolve_struct(name)
        if name in seen:
            return False
        info = self.registry.structs.get(name) or self.registry.unions.get(name)
        if info is None:
            return False
        for member in info["members"]:
            base = self.base(member)
            if member.pointers or member.bitfield or base in self.registry.handles or base in SIZE_T_TYPES or base in OPAQUE_TYPES:
                return False
            if base in self.registry.structs or base in self.registry.unions:
                if not self.plain(base, seen + (name,)):
                    return False
        return True

    def union_plain(self, name):
        return self.plain(name)

    def struct_supported(self, name):
        name = self.registry.resolve_struct(name)
        if name in self.supported:
            return self.supported[name] is not False
        if name not in self.index:
            return False
        self.supported[name] = None
        ok = True
        info = self.registry.structs[name]
        for member in info["members"]:
            if member.bitfield:
                ok = False
                break
            kind = self.kind_of(member)
            if kind is None:
                ok = False
                break
            if kind in ("nested", "pointer_struct", "pointer_struct_pointers") and not self.struct_supported(self.base(member)):
                ok = False
                break
            if kind == "raw_union" and not self.union_plain(self.base(member)):
                ok = False
                break
        self.supported[name] = ok
        return ok

    # ---- length programs ----
    def tokens(self, expression):
        return re.findall(r"->|[A-Za-z_][A-Za-z_0-9]*|\d+|[()+/*]", expression)

    def compile_length(self, expression, resolve):
        """Postfix program for a registry length expression; resolve maps a name to operations."""
        if not expression or expression == "null-terminated" or "latexmath" in expression:
            return None
        tokens = self.tokens(expression)
        position = 0
        output = []

        def primary():
            nonlocal position
            token = tokens[position]
            position += 1
            if token == "(":
                expression_()
                position += 1
                return
            if token.isdigit():
                output.append(("constant", int(token), 0))
                return
            if position < len(tokens) and tokens[position] == "->":
                member = tokens[position + 1]
                position += 2
                output.extend(resolve(token, member))
                return
            output.extend(resolve(token, None))

        def term():
            nonlocal position
            primary()
            while position < len(tokens) and tokens[position] == "/":
                position += 1
                primary()
                output.append(("divide", 0, 0))

        def expression_():
            nonlocal position
            term()
            while position < len(tokens) and tokens[position] == "+":
                position += 1
                term()
                output.append(("add", 0, 0))

        expression_()
        if position != len(tokens):
            raise ValueError(f"cannot compile length {expression}")
        return output

    def add_length(self, program):
        if program is None:
            return NO_LENGTH
        key = tuple(program)
        if key not in self.lens_at:
            self.lens_at[key] = len(self.lens)
            self.lens.extend(program)
            self.lens.append(("end", 0, 0))
        return self.lens_at[key]

    # ---- structures ----
    def build_structs(self):
        registry = self.registry
        for name in self.struct_names:
            info = registry.structs[name]
            layout = self.layouts[name]
            first = len(self.fields)
            ok = self.struct_supported(name)
            stype = "0"
            if ok:
                members = info["members"]
                positions = {member.name: first + at for at, member in enumerate(members)}
                if members and members[0].name == "sType" and members[0].values:
                    stype = members[0].values.split(",")[0]
                    if stype not in self.stypes:
                        stype = "0"
                for member in members:
                    kind = self.kind_of(member)
                    off32, size32 = layout["members"][member.name]
                    target = self.base(member)
                    target_index = self.index.get(target, "no_target") if kind in ("nested", "pointer_struct", "pointer_struct_pointers") else "no_target"
                    length = NO_LENGTH
                    if kind == "descriptor_data":
                        target_index = self.index["VkDescriptorAddressInfoEXT"]
                        length = self.add_length([("field", positions["type"], 0)])
                    if kind in ("pointer_struct", "pointer_struct_pointers", "pointer_strings", "pointer_dhandles") and member.len:
                        def resolve(identifier, arrow, positions=positions):
                            if arrow is not None or identifier not in positions:
                                raise ValueError(f"{name}.{member.name}: {identifier}")
                            return [("field", positions[identifier], 0)]
                        try:
                            length = self.add_length(self.compile_length(member.len[0], resolve))
                        except (ValueError, IndexError):
                            ok = False
                            break
                    self.fields.append((kind, off32, size32, f"offsetof({name}, {member.name})", f"sizeof((({name} *)0)->{member.name})", target_index, length))
            if not ok:
                del self.fields[first:]
                self.supported[name] = False
            self.struct_rows.append((name, layout["size"], first, len(self.fields) - first, stype if ok else "0", ok))

    # ---- commands ----
    def build_commands(self, bridge_names):
        registry = self.registry
        for name in sorted(bridge_names):
            target = registry.resolve_command(name)
            if target not in registry.commands:
                continue
            info = registry.commands[target]
            first = len(self.params)
            words = 0
            rows = []
            ok = True
            positions = {param.name: at for at, param in enumerate(info["params"])}
            for at, param in enumerate(info["params"]):
                base = self.base(param)
                floating = param.type in ("float", "double") and param.pointers == 0
                length = NO_LENGTH
                target_index = "no_target"
                if param.pointers == 0:
                    if base in registry.handles:
                        if registry.handles[base]["dispatchable"]:
                            kind, width = "dhandle", 1
                        else:
                            kind, width = "words", 2
                    elif base in registry.structs or base in registry.unions:
                        ok = False
                        break
                    else:
                        width = max(1, (self.sizes[param.type] + 3) // 4)
                        kind = "word" if width == 1 else "words"
                else:
                    width = 1
                    if param.pointers == 2 and base == "void":
                        kind = "out_pointer"
                    elif param.pointers != 1:
                        ok = False
                        break
                    elif base in registry.structs:
                        if not self.struct_supported(base):
                            ok = False
                            break
                        kind = "in_struct" if param.const else "inout_struct"
                        target_index = self.index[base]
                    elif base in registry.handles and registry.handles[base]["dispatchable"]:
                        kind = "in_dhandles" if param.const else "out_dhandles"
                    else:
                        kind = "pass"
                    if param.len and kind != "pass":
                        def resolve(identifier, arrow, positions=positions, info=info):
                            if identifier not in positions:
                                raise ValueError(identifier)
                            index = positions[identifier]
                            other = info["params"][index]
                            if arrow is not None:
                                struct = self.base(other)
                                members = registry.structs[struct]["members"]
                                names = [member.name for member in members]
                                if arrow not in names:
                                    raise ValueError(arrow)
                                return [("arrow", index, names.index(arrow), struct)]
                            if other.pointers:
                                return [("deref", index, 0)]
                            return [("param", index, 0)]
                        try:
                            program = self.compile_length(param.len[0], resolve)
                        except (ValueError, IndexError):
                            ok = False
                            break
                        length = self.add_length([item[:3] for item in program] if program else None)
                        if program:
                            for item in program:
                                if item[0] == "arrow":
                                    self.arrows.append(item)
                rows.append((kind, "true" if floating else "false", target_index, length))
                words += width
            if not ok:
                continue
            ret = info["return"]
            if ret == "void":
                ret_kind = "none"
            elif ret == "PFN_vkVoidFunction":
                ret_kind = "pointer"
            else:
                ret_kind = "words" if self.sizes.get(ret, 4) == 8 else "word"
            self.params.extend(rows)
            self.commands.append((name, ret_kind, first, len(rows), words))

    def emit(self, output):
        Path(output).with_name("vulkan_thunk32_exports.def").write_text("LIBRARY vulkan-1.dll\nEXPORTS\n" +
            "".join(f"{name}@{words * 4}\n" for name, ret, first, count, words in self.commands))
        lines = ["// Generated by scripts/generate-vulkan-thunk32.py from the Vulkan registry and an i686 compiler; do not edit.\n"]
        lines.append("static const FieldInfo field_rows[] = {\n")
        for kind, off32, size32, off64, size64, target, length in self.fields:
            lines.append(f"\t{{Field::{kind}, {off32}, {size32}, {off64}, {size64}, {target}, {length}}},\n")
        lines.append("\t{Field::scalar, 0, 0, 0, 0, no_target, no_length},\n};\n")
        lines.append("static const StructInfo struct_rows[] = {\n")
        for name, size32, first, count, stype, ok in self.struct_rows:
            lines.append(f'\t{{"{name}", {size32}, sizeof({name}), {first}, {count}, {stype}, {"true" if ok else "false"}}},\n')
        lines.append("};\n")
        lines.append("static const LenOp length_rows[] = {\n")
        for op, a, b, *_ in self.lens:
            lines.append(f"\t{{Op::{op}, {a}, {b}}},\n")
        lines.append("};\n")
        lines.append("static const ParamInfo param_rows[] = {\n")
        for kind, floating, target, length in self.params:
            lines.append(f"\t{{Param::{kind}, {floating}, {target}, {length}}},\n")
        lines.append("\t{Param::word, false, no_target, no_length},\n};\n")
        lines.append("static const CommandInfo command_rows[] = {\n")
        for name, ret, first, count, words in self.commands:
            lines.append(f'\t{{"{name}", Ret::{ret}, {first}, {count}, {words}}},\n')
        lines.append("};\n")
        Path(output).write_text("".join(lines))


def type_sizes(registry, header_text, compiler, include):
    """sizeof, for the 32-bit compiler, of every type a command takes by value."""
    names = set()
    returns = set()
    for info in registry.commands.values():
        for param in info["params"]:
            if param.pointers == 0 and registry.resolve_struct(param.type) not in registry.handles:
                names.add(param.type)
        returns.add(info["return"])
    names |= {name for name in returns if name not in ("void", "PFN_vkVoidFunction")}
    declared = set(re.findall(r"\b(?:Vk|PFN_vk)\w+\b", header_text))
    wanted = sorted(name for name in names if name in declared or name in ("float", "double", "uint32_t", "uint64_t", "int32_t", "int64_t", "size_t", "int", "uint8_t", "uint16_t"))
    lines = ["#include <stddef.h>", "#include <stdint.h>", "#define VK_USE_PLATFORM_WIN32_KHR", "#include <windows.h>", "#include <vulkan/vulkan.h>",
             "const unsigned sizes[] = {0xC0FFEE01u,"]
    lines += [f"sizeof({name})," for name in wanted]
    lines.append("};")
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / "sizes.c"
        source.write_text("\n".join(lines) + "\n")
        objfile = Path(scratch) / "sizes.o"
        raw = Path(scratch) / "sizes.bin"
        result = subprocess.run([compiler, "-O0", "-c", "-w", f"-I{include}", str(source), "-o", str(objfile)], capture_output=True, text=True)
        if result.returncode:
            sys.stderr.write(result.stderr[-3000:])
            sys.exit(1)
        subprocess.run([Path(compiler).with_name(Path(compiler).name.replace("gcc", "objcopy")), "-O", "binary", "-j", ".rdata", str(objfile), str(raw)], check=True)
        data = raw.read_bytes()
    values = [int.from_bytes(data[at:at + 4], "little") for at in range(0, len(data) - len(data) % 4, 4)]
    values = values[values.index(0xC0FFEE01) + 1:]
    return dict(zip(wanted, values))


def main():
    registry = marshal.Registry(sys.argv[1])
    header_text = Path(sys.argv[2]).read_text()
    compiler, include = sys.argv[4], sys.argv[5]
    layouts = layout32.compute_layouts(registry, header_text, compiler, include)
    sizes = type_sizes(registry, header_text, compiler, include)
    generator = Generator(registry, header_text, layouts, sizes)
    generator.arrows = []
    generator.build_structs()
    commands = set(registry.required_commands) | set(registry.aliases_command)
    commands = {name for name in commands if ("PFN_" + registry.resolve_command(name)) in set(re.findall(r"\bPFN_vk\w+\b", header_text))}
    generator.build_commands(commands)
    generator.emit(sys.argv[3])
    supported = sum(1 for row in generator.struct_rows if row[5])
    print(supported, "of", len(generator.struct_rows), "structures,", len(generator.commands), "commands", file=sys.stderr)


if __name__ == "__main__":
    main()
