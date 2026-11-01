#!/usr/bin/env python3
"""Generate native/upstream/unicode/normalization.inc: the data of Unicode normalization.

The data comes from the Unicode Character Database as Python's unicodedata module carries it.

usage: generate-normalization.py OUTPUT.inc
"""
import sys
import unicodedata


def is_hangul(character):
    return 0xac00 <= character <= 0xd7a3


def main():
    pool = []
    canonical = []
    compatibility = []
    pairs = []
    classes = []
    characters = [c for c in range(0x110000) if not 0xd800 <= c <= 0xdfff]
    for character in characters:
        text = chr(character)
        ccc = unicodedata.combining(text)
        if classes and classes[-1][1] + 1 == character and classes[-1][2] == ccc and ccc:
            classes[-1][1] = character
        elif ccc:
            classes.append([character, character, ccc])
        if is_hangul(character):
            continue
        for form, table in (("NFD", canonical), ("NFKD", compatibility)):
            mapped = unicodedata.normalize(form, text)
            if mapped == text or (form == "NFKD" and mapped == unicodedata.normalize("NFD", text)):
                continue
            table.append((character, len(pool), len(mapped)))
            pool.extend(ord(c) for c in mapped)
        fields = unicodedata.decomposition(text).split()
        # A primary composite: two canonical parts, and NFC keeps the character.
        if len(fields) == 2 and not fields[0].startswith("<") and unicodedata.normalize("NFC", text) == text:
            pairs.append((int(fields[0], 16), int(fields[1], 16), character))
    pairs.sort()

    def lines(items, per_line=6):
        return ["\t" + "".join(items[at:at + per_line]) for at in range(0, len(items), per_line)]

    with open(sys.argv[1], "w") as out:
        out.write(f"// Unicode {unicodedata.unidata_version} normalization data, made by scripts/generate-normalization.py.\n")
        out.write("constexpr uint32_t expansion_pool[] = {\n")
        out.write("\n".join(lines([f"0x{c:x}," for c in pool], 12)) + "\n};\n")
        for name, table in (("canonical_expansions", canonical), ("compatibility_expansions", compatibility)):
            out.write(f"constexpr Expansion {name}[] = {{\n")
            out.write("\n".join(lines([f"{{0x{c:x},{o},{n}}}," for c, o, n in table])) + "\n};\n")
        out.write("constexpr ClassRange class_ranges[] = {\n")
        out.write("\n".join(lines([f"{{0x{a:x},0x{b:x},{c}}}," for a, b, c in classes])) + "\n};\n")
        out.write("constexpr Composition compositions[] = {\n")
        out.write("\n".join(lines([f"{{0x{a:x},0x{b:x},0x{c:x}}}," for a, b, c in pairs])) + "\n};\n")


if __name__ == "__main__":
    main()
