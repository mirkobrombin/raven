#!/usr/bin/env python3
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import time
import urllib.request


# The ICU release Windows' own ICU is built from in spirit: ICU4C under the Unicode licence, its C interface exported unsuffixed (Windows
# builds with renaming off) from icu.dll, with icuuc.dll and icuin.dll forwarding to it as they do on Windows.
VERSION = "79.1"
URL = "https://github.com/unicode-org/icu/releases/download/release-79.1/icu4c-79.1-sources.tgz"
# From the release's SHASUM512.txt.
SHA512 = ("9098adf9ba7fbe11d92963415cc80bf09de39ce1372414dbc2561e5f8ef57479a9fe4b6f7abc61911e4659e5953be987c459297d29e61b064d1931a16195a973")
DATA = "icudt79l"
# The data the library carries, cut from the release's prebuilt package: Unicode normalization (NFKC, case folding, IDNA), the property
# tables that are not compiled in, converter aliases, the root locale bundles with what locale resolution reads, every break rule
# without the dictionaries of the scripts written without spaces, and collation for every locale but the large Chinese, Japanese and
# Korean tailorings, which fall back to the root order.
KEEP_TOP = {"nfkc.nrm", "nfkc_cf.nrm", "nfkc_scf.nrm", "uts46.nrm", "uemoji.icu", "ulayout.icu", "cnvalias.icu", "root.res",
            "res_index.res", "pool.res", "langInfo.res", "keyTypeData.res", "metadata.res", "icuver.res", "icustd.res"}
DROP_COLLATION = {"coll/zh.res", "coll/ja.res", "coll/ko.res"}
# The time zone comes from Windows (uprv_detectWindowsTimeZone) and the offset from localtime, not from the C runtime's _tzset, _tzname
# and _timezone, as in ICU's own build for the Universal Windows Platform.
FLAGS = ("-O2", "-DNDEBUG", "-DU_STATIC_IMPLEMENTATION", "-DU_DISABLE_RENAMING=1", "-DU_USING_ICU_NAMESPACE=0", "-DU_HIDE_OBSOLETE_UTF_OLD_H=1",
         "-DU_HAVE_TZSET=0", "-DU_HAVE_TZNAME=0", "-DU_HAVE_TIMEZONE=0",
         "-DWINVER=0x0601", "-D_WIN32_WINNT=0x0601", "-ffunction-sections", "-fdata-sections", "-ffile-prefix-map={source}=icu")


def keep(name):
    if "/" not in name:
        return name in KEEP_TOP
    if name.startswith("brkitr/"):
        return not name.endswith(".dict")
    if name.startswith("coll/"):
        return name not in DROP_COLLATION
    return False


def subset_data(package, output):
    """Writes the items keep() selects into a new common data package of the same format: the header, a table of name and data offsets
    from the table's start, the names, then the items, each on a 16-byte boundary (ucmndata.h)."""
    data = package.read_bytes()
    header = struct.unpack_from("<H", data, 0)[0]
    count = struct.unpack_from("<I", data, header)[0]
    entries = [struct.unpack_from("<II", data, header + 4 + 8 * index) for index in range(count)]
    items = []
    for index, (name_offset, data_offset) in enumerate(entries):
        name = data[header + name_offset:data.index(b"\0", header + name_offset)].decode()
        end = entries[index + 1][1] if index + 1 < count else len(data) - header
        if not name.startswith(DATA + "/"):
            raise RuntimeError(f"unexpected ICU data item: {name}")
        if keep(name[len(DATA) + 1:]):
            items.append((name, data[header + data_offset:header + end]))
    names = b"".join(name.encode() + b"\0" for name, _ in items)
    table = 4 + 8 * len(items)
    position = (header + table + len(names) + 15) // 16 * 16 - header
    offsets, blob = [], bytearray()
    names_at = table
    for name, item in items:
        offsets.append((names_at, position + len(blob)))
        names_at += len(name) + 1
        blob += item
        blob += b"\0" * (-len(blob) % 16)
    result = bytearray(data[:header]) + struct.pack("<I", len(items))
    for name_offset, data_offset in offsets:
        result += struct.pack("<II", name_offset, data_offset)
    result += names
    result += b"\0" * (header + position - len(result))
    result += blob
    output.write_bytes(result)
    return [name for name, _ in items]


def public_functions(source):
    """The C functions the public headers declare (U_CAPI ... name(...))."""
    names = set()
    for header in sorted([*(source / "common/unicode").glob("*.h"), *(source / "i18n/unicode").glob("*.h")]):
        text = re.sub(r"/\*.*?\*/|//[^\n]*", " ", header.read_text(errors="replace"), flags=re.S)
        for match in re.finditer(r"\bU_CAPI\b([^;{}()]*?)\b([A-Za-z_]\w*)\s*\(", text):
            names.add(match.group(2))
    return names


def defined_functions(nm, archive):
    lines = subprocess.run([nm, "--defined-only", "-g", str(archive)], check=True, capture_output=True, text=True).stdout.splitlines()
    return {line.split()[-1] for line in lines if len(line.split()) == 3 and line.split()[1] == "T"}


def main():
    parser = argparse.ArgumentParser(description="Build the pinned ICU release as Windows' icu.dll, icuuc.dll and icuin.dll")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path, help="a downloaded copy of the ICU4C source archive")
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    args = parser.parse_args()
    started = time.monotonic()
    triple = "x86_64-w64-mingw32"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(output), TMP=str(output), TEMP=str(output))
    repo = Path(__file__).resolve().parents[1]
    for tool in ("gcc", "g++", "ar", "nm", "strip", "objdump"):
        if not shutil.which(f"{triple}-{tool}"):
            raise RuntimeError(f"required ICU build tool is missing: {triple}-{tool}")
    archive = args.archive.resolve() if args.archive else output / "icu4c-79.1-sources.tgz"
    if not archive.exists():
        urllib.request.urlretrieve(URL, archive)
    if hashlib.sha512(archive.read_bytes()).hexdigest() != SHA512:
        raise RuntimeError("ICU source archive checksum differs")
    root = output / "source"
    shutil.rmtree(root, ignore_errors=True)
    wanted = ("icu/LICENSE", "icu/source/common/", "icu/source/i18n/", f"icu/source/data/in/{DATA}.dat")
    with tarfile.open(archive) as upstream:
        members = [member for member in upstream if member.isfile() and member.name.startswith(wanted)]
        upstream.extractall(root, members=members, filter="data")
    source = root / "icu/source"
    build = output / "build"
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir()
    records = []

    def run(command):
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        records.append(dict(command=command, returncode=result.returncode))
        if result.returncode:
            raise RuntimeError(" ".join(command) + "\n" + result.stderr[-4000:])
        return result.stdout

    data_items = subset_data(source / f"data/in/{DATA}.dat", build / f"{DATA}.dat")
    flags = [flag.format(source=source) for flag in FLAGS]
    libraries = {}
    for library, define in (("common", "-DU_COMMON_IMPLEMENTATION"), ("i18n", "-DU_I18N_IMPLEMENTATION")):
        names = [line.strip() for line in (source / library / "sources.txt").read_text().splitlines() if line.strip()]
        objects = []
        commands = []
        for name in names:
            target = build / library / (Path(name).stem + ".o")
            target.parent.mkdir(exist_ok=True)
            compiler = f"{triple}-g++" if name.endswith(".cpp") else f"{triple}-gcc"
            standard = ["-std=c++17"] if name.endswith(".cpp") else []
            commands.append([compiler, *standard, *flags, define, f"-I{source / 'common'}", f"-I{source / 'i18n'}",
                             "-c", str(source / library / name), "-o", str(target)])
            objects.append(target)
        with ThreadPoolExecutor(args.jobs) as pool:
            list(pool.map(run, commands))
        libraries[library] = build / f"libicu{library}.a"
        run([f"{triple}-ar", "rcs", str(libraries[library]), *map(str, objects)])
    # Windows' ICU exports the C interface only; icuuc.dll answers for the common library's half of it and icuin.dll for the rest.
    public = public_functions(source)
    common_exports = sorted(public & defined_functions(f"{triple}-nm", libraries["common"]))
    i18n_exports = sorted(public & defined_functions(f"{triple}-nm", libraries["i18n"]))
    (build / "icu.def").write_text("LIBRARY icu.dll\nEXPORTS\n" + "".join(f"    {name}\n" for name in
                                   [*common_exports, *i18n_exports, "NormalizeString", "IsNormalizedString"]))
    for library, exports in (("icuuc", common_exports), ("icuin", i18n_exports)):
        (build / f"{library}.def").write_text(f"LIBRARY {library}.dll\nEXPORTS\n" + "".join(f"    {name} = icu.{name}\n" for name in exports))
    (build / "data.S").write_text(f'\t.section .rdata,"dr"\n\t.globl {DATA[:-1]}_dat\n\t.balign 16\n{DATA[:-1]}_dat:\n'
                                  f'\t.incbin "{build / (DATA + ".dat")}"\n')
    run([f"{triple}-gcc", "-c", str(build / "data.S"), "-o", str(build / "data.o")])
    run([f"{triple}-gcc", "-O2", "-Wall", "-DU_DISABLE_RENAMING=1", f"-I{source / 'common'}", "-c",
         str(repo / "native/upstream/raven-icu/normalize.c"), "-o", str(build / "normalize.o")])
    component = output / "icu.dll"
    run([f"{triple}-g++", "-shared", "-static", "-static-libgcc", "-static-libstdc++", "-Wl,--gc-sections,--no-insert-timestamp",
         "-Wl,--image-base,0x1b0000000", "-o", str(component), str(build / "icu.def"), str(build / "normalize.o"), str(build / "data.o"),
         "-Wl,--start-group", str(libraries["i18n"]), str(libraries["common"]), "-Wl,--end-group", "-ladvapi32"])
    run([f"{triple}-strip", "--strip-unneeded", str(component)])
    (build / "empty.c").write_text("")
    for library in ("icuuc", "icuin"):
        run([f"{triple}-gcc", "-shared", "-nostdlib", "-Wl,--entry,0,--no-insert-timestamp", "-o", str(output / f"{library}.dll"),
             str(build / f"{library}.def"), str(build / "empty.c")])
    dump = run([f"{triple}-objdump", "-p", str(component)])
    (output / "imports.txt").write_text(dump[dump.find("The Import Tables"):dump.find("The Export Tables")])
    licenses = output / "licenses"
    licenses.mkdir(exist_ok=True)
    shutil.copyfile(root / "icu/LICENSE", licenses / "ICU-LICENSE")
    sizes = {name: (output / name).stat().st_size for name in ("icu.dll", "icuuc.dll", "icuin.dll")}
    (output / "provenance.json").write_text(json.dumps(dict(
        project="ICU4C", version=VERSION, url=URL, archive_sha512=SHA512, license="Unicode-3.0 (licenses/ICU-LICENSE)",
        data=dict(package=f"{DATA}.dat", bytes=(build / f"{DATA}.dat").stat().st_size, items=data_items),
        exports=dict(icuuc=len(common_exports), icuin=len(i18n_exports)), sizes=sizes,
        seconds=round(time.monotonic() - started),
        components={name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in sizes},
        commands=len(records)), indent=2) + "\n")
    print(component)


if __name__ == "__main__":
    main()
