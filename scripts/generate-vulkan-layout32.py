#!/usr/bin/env python3
"""Lay out every Vulkan structure the way a 32-bit Windows compiler does, by asking one.

A 32-bit program's structures differ from the host's in the width of pointers, size_t and dispatchable handles, and so in every offset
after one. The compiler that built the program is the authority, so this script writes a C file that records sizeof and offsetof of each
member, compiles it for i686 with MinGW-w64, and reads the numbers back from the assembly.

usage: generate-vulkan-layout32.py vk.xml vulkan_core.h OUTPUT.json [COMPILER] [INCLUDE_DIRECTORY]
"""

import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location("marshal", Path(__file__).with_name("generate-vulkan-marshal.py"))
marshal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(marshal)


def compute_layouts(registry, header_text, compiler, include):
    """sizeof and offsetof/sizeof of every member of every declared structure, as the 32-bit compiler sees them."""
    declared = set(re.findall(r"\bVk\w+\b", header_text))
    entries = []
    for name, info in sorted(registry.structs.items()):
        if name not in declared:
            continue
        if any(member.bitfield for member in info["members"]):
            continue
        entries.append((name, info["members"]))
    lines = ["#include <stddef.h>", "#include <stdint.h>", "#define VK_USE_PLATFORM_WIN32_KHR", "#include <windows.h>", "#include <vulkan/vulkan.h>",
             "#include <vulkan/vulkan_win32.h>", "const unsigned layout[] = {0xC0FFEE01u,"]
    index = []
    for name, members in entries:
        lines.append(f"sizeof({name}),")
        index.append((name, "size", None))
        for member in members:
            lines.append(f"offsetof({name}, {member.name}), sizeof((({name} *)0)->{member.name}),")
            index.append((name, "member", member.name))
    lines.append("};")
    with tempfile.TemporaryDirectory(dir=None) as scratch:
        source = Path(scratch) / "layout.c"
        source.write_text("\n".join(lines) + "\n")
        objfile = Path(scratch) / "layout.o"
        raw = Path(scratch) / "layout.bin"
        result = subprocess.run([compiler, "-O0", "-c", "-w", f"-I{include}", str(source), "-o", str(objfile)], capture_output=True, text=True)
        if result.returncode:
            sys.stderr.write(result.stderr[-4000:])
            sys.exit(1)
        # The table is the only initialised data of the object that starts with the marker; its bytes are the values in order.
        subprocess.run([Path(compiler).with_name(Path(compiler).name.replace("gcc", "objcopy")), "-O", "binary", "-j", ".rdata", str(objfile), str(raw)], check=True)
        data = raw.read_bytes()
    values = [int.from_bytes(data[at:at + 4], "little") for at in range(0, len(data) - len(data) % 4, 4)]
    values = values[values.index(0xC0FFEE01) + 1:]
    cursor = 0
    layouts = {}
    for name, kind, member in index:
        if kind == "size":
            layouts[name] = {"size": values[cursor], "members": {}}
            cursor += 1
        else:
            layouts[name]["members"][member] = [values[cursor], values[cursor + 1]]
            cursor += 2
    if cursor > len(values):
        sys.exit(f"needed {cursor} values, found {len(values)}")
    return layouts


def main():
    registry = marshal.Registry(sys.argv[1])
    header_text = Path(sys.argv[2]).read_text()
    compiler = sys.argv[4] if len(sys.argv) > 4 else "i686-w64-mingw32-gcc"
    include = sys.argv[5] if len(sys.argv) > 5 else str(Path(sys.argv[2]).parent.parent)
    layouts = compute_layouts(registry, header_text, compiler, include)
    Path(sys.argv[3]).write_text(json.dumps(layouts, sort_keys=True, indent=0) + "\n")
    print(len(layouts), "structures laid out")


if __name__ == "__main__":
    main()
