#!/usr/bin/env python3
"""Generate the table of Vulkan feature, property and format query structures that can travel in a pNext chain.

Every structure named VkPhysicalDevice...Features or ...Properties, or Vk...Format...Properties (any vendor suffix), whose members hold no pointers except
pNext is a plain block of bytes: it can be copied between guest and host without translating handles. The table pairs each
structure type value with the size of the host structure.

The structure type value of each structure is read from the registry (the values attribute of its sType member) when its path is given
as a third argument, and guessed from the structure name otherwise; the guess fails for names with digit suffixes such as 3D.

usage: generate-vulkan-chains.py /usr/include/vulkan/vulkan_core.h build/native/vulkan-generated/vulkan_chain_table.inc [/usr/share/vulkan/registry/vk.xml]
"""

import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree


def snake(name):
    text = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name)
    text = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", text)
    return text.upper()


def registry_types(path):
    """Structure name to the structure type enumerator named by its sType member."""
    found = {}
    for element in ElementTree.parse(path).getroot().iter("type"):
        if element.get("category") != "struct":
            continue
        for member in element.findall("member"):
            name = member.find("name")
            if name is not None and name.text == "sType" and member.get("values"):
                found[element.get("name")] = member.get("values").split(",")[0]
    return found


def provided_names(include_root):
    """Every structure name and structure type enumerator the compiler really provides.

    Structures and enumerators behind preprocessor switches are absent, which is what the generated table must reflect.
    Reading the preprocessed text instead of the diagnostics keeps the result independent of the compiler's locale.
    """
    result = subprocess.run(
        ["cc", "-E", "-P", "-I" + include_root, "-x", "c", "-"],
        input="#include <vulkan/vulkan.h>\n", capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(result.stderr)
    return set(re.findall(r"\b(?:VK_STRUCTURE_TYPE_\w+|Vk\w+)\b", result.stdout))


def probe_sizes(include_root, entries):
    """Compile the table so a structure the headers only declare incompletely fails here, not in the bridge."""
    with tempfile.TemporaryDirectory() as directory:
        probe = os.path.join(directory, "probe.c")
        with open(probe, "w") as handle:
            handle.write("#include <vulkan/vulkan.h>\nstatic const struct { int type; unsigned long size; } probe[] = {\n")
            for enumerator, name in entries:
                handle.write(f"{{{enumerator}, sizeof({name})}},\n")
            handle.write("};\n")
        result = subprocess.run(["cc", "-fsyntax-only", "-I" + include_root, probe], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(result.stderr)


def main():
    header = open(sys.argv[1]).read()
    registry = registry_types(sys.argv[3]) if len(sys.argv) > 3 else {}
    enumerators = set(re.findall(r"\b(VK_STRUCTURE_TYPE_[A-Z0-9_]+)\b", header))
    entries = []
    for match in re.finditer(r"typedef struct (Vk\w+) \{(.*?)\} \1;", header, re.S):
        name, body = match.groups()
        device_structure = re.match(r"VkPhysicalDevice\w*(Features|Properties)\w*$", name)
        # Format and image format query results (VkFormatProperties3, external and Ycbcr image format answers) also travel in chains.
        format_answer = re.match(r"Vk(?!PhysicalDevice|Video)\w*Format\w*Properties\w*$", name) and name not in ("VkFormatProperties2", "VkImageFormatProperties2")
        if not (device_structure or format_answer) or "sType" not in body:
            continue
        members = [line.strip() for line in body.split(";") if line.strip()]
        if any("*" in member and "pNext" not in member for member in members):
            continue
        suffix = re.search(r"(KHR|EXT|NV|NVX|AMD|INTEL|QCOM|ARM|HUAWEI|VALVE|MESA|IMG|ANDROID|FUCHSIA|GOOGLE|SEC|QNX|MSFT|EXT_PRIVATE)$", name)
        stem = name[:-len(suffix.group(1))] if suffix else name
        base = snake(stem[2:])
        tail = ("_" + suffix.group(1)) if suffix else ""
        # The enumerator spelling of digits varies: Vulkan11 is VULKAN_1_1, Robustness2 is ROBUSTNESS_2, Int64 stays INT64.
        candidates = [
            base,
            re.sub(r"(?<=[A-Z])(\d)", r"_\1", base),
            re.sub(r"VULKAN(\d)(\d)", r"VULKAN_\1_\2", base),
            re.sub(r"VULKAN(\d)(\d)", r"VULKAN_\1_\2", re.sub(r"(?<=[A-Z])(\d)", r"_\1", base)),
        ]
        candidate = registry.get(name) or next((("VK_STRUCTURE_TYPE_" + item + tail) for item in candidates if ("VK_STRUCTURE_TYPE_" + item + tail) in enumerators), None)
        if not candidate:
            continue
        entries.append((candidate, name))
    entries.sort()
    # Beta-only structures are declared behind preprocessor switches; keep what the plain headers really provide.
    include_root = os.path.dirname(os.path.dirname(os.path.abspath(sys.argv[1])))
    provided = provided_names(include_root)
    entries = [(e, n) for e, n in entries if e in provided and n in provided]
    probe_sizes(include_root, entries)
    print("// Generated by scripts/generate-vulkan-chains.py from the Vulkan headers; do not edit.", file=open(sys.argv[2], "w"))
    with open(sys.argv[2], "a") as output:
        for enumerator, name in entries:
            print(f"{{{enumerator}, sizeof({name})}},", file=output)
    print(len(entries), "structures")


if __name__ == "__main__":
    main()
