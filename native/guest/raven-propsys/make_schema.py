#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn the property keys of the public propkey.h into the table of known system properties."""
import re
import sys


LISTS = re.compile(r"(Authors|Keywords|Ids|Addresses|Names|Categories|Artists|Composers|Contributors|Genre|Tags|Participants|Recipients|Conductors|Publishers)$")


def property_type(name):
    """The header carries no types, so they follow the naming the schema uses throughout."""
    if name.endswith(("Guid", "ContainerId", "ClassId")):
        return "VT_CLSID"
    if LISTS.search(name):
        return "VT_VECTOR | VT_LPWSTR"
    if re.search(r"(^|\.)Date[A-Z]|Date$|Time$", name):
        return "VT_FILETIME"
    if name.endswith(("Size", "Count", "Bytes")):
        return "VT_UI8"
    if re.search(r"\.Is[A-Z]|^System\.Is[A-Z]", name):
        return "VT_BOOL"
    return "VT_LPWSTR"


def main():
    pattern = re.compile(r"DEFINE_PROPERTYKEY\(PKEY_(\w+),\s*0x([0-9a-f]+),\s*0x([0-9a-f]+),\s*0x([0-9a-f]+),\s*((?:0x[0-9a-f]+,\s*){8})(\d+)\)")
    rows = []
    seen = set()
    with open(sys.argv[1], encoding="latin-1") as header:
        for match in pattern.finditer(header.read()):
            name = "System." + match[1].replace("_", ".")
            if name in seen:
                continue
            seen.add(name)
            data4 = match[5].rstrip(", \n")
            rows.append(f'\t{{{{{{0x{match[2]}, 0x{match[3]}, 0x{match[4]}, {{{data4}}}}}, {match[6]}}}, L"{name}", {property_type(name)}}},\n')
    with open(sys.argv[2], "w") as output:
        output.writelines(rows)


if __name__ == "__main__":
    main()
