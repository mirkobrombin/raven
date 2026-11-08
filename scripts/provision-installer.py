#!/usr/bin/env python3
import argparse
import atexit
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

from component_licenses import mark_patched_sources


# Libraries taken from their own releases: zlib, which cabinet.dll, msxml3.dll, the image libraries and raven-appx.dll compress and
# inflate with, and the Independent JPEG Group's library the codecs read and write JPEG through. Each is kept beside the components
# as NAME-VERSION-source.tar.gz with the notices listed here, and unpacked to NAME-source.
UPSTREAM = {
    "zlib": dict(version="1.3.1", sha256="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
                 url="https://zlib.net/fossils/zlib-{version}.tar.gz", notices=("LICENSE", "README")),
    "libjpeg": dict(version="10", sha256="8b9eaa13242690ebd03e1728ab1edf97a81a78ed6e83624d493655f31ac95ab5",
                    url="https://www.ijg.org/files/jpegsrc.v{version}.tar.gz", notices=("README",)),
    "libtiff": dict(version="4.7.1", sha256="f698d94f3103da8ca7438d84e0344e453fe0ba3b7486e04c5bf7a9a3fabe9b69",
                    url="https://download.osgeo.org/libtiff/tiff-{version}.tar.gz", notices=("LICENSE.md", "README.md")),
    "libxml2": dict(version="2.12.8", sha256="43ad877b018bc63deb2468d71f95219c2fac196876ef36d1bee51d226173ec93",
                    url="https://download.gnome.org/sources/libxml2/2.12/libxml2-{version}.tar.xz", notices=("Copyright", "README.md")),
    "libxslt": dict(version="1.1.42", sha256="85ca62cac0d41fc77d3f6033da9df6fd73d20ea2fc18b0a3609ffb4110e1baeb",
                    url="https://download.gnome.org/sources/libxslt/1.1/libxslt-{version}.tar.xz", notices=("Copyright", "README.md")),
}
# The parts of libxml2 msxml3.dll is built with, and the features its configuration turns on for them.
XML_SOURCES = ("buf.c", "catalog.c", "chvalid.c", "debugXML.c", "dict.c", "encoding.c", "entities.c", "error.c", "globals.c", "hash.c",
               "HTMLparser.c", "HTMLtree.c", "list.c", "parser.c", "parserInternals.c", "pattern.c", "relaxng.c", "SAX.c", "SAX2.c", "threads.c",
               "tree.c", "uri.c", "valid.c", "xinclude.c", "xmlIO.c", "xmlmemory.c", "xmlreader.c", "xmlregexp.c", "xmlsave.c", "xmlschemas.c",
               "xmlschemastypes.c", "xmlstring.c", "xmlunicode.c", "xpath.c", "xpointer.c")
XML_FEATURES = dict(WITH_TRIO=0, WITH_THREADS=1, WITH_THREAD_ALLOC=0, WITH_TREE=1, WITH_OUTPUT=1, WITH_PUSH=1, WITH_READER=1, WITH_PATTERN=1,
                    WITH_WRITER=1, WITH_SAX1=1, WITH_FTP=0, WITH_HTTP=0, WITH_VALID=1, WITH_HTML=1, WITH_LEGACY=0, WITH_C14N=1, WITH_CATALOG=1,
                    WITH_XPATH=1, WITH_XPTR=1, WITH_XPTR_LOCS=0, WITH_XINCLUDE=1, WITH_ICONV=0, WITH_ICU=0, WITH_ISO8859X=1, WITH_DEBUG=1,
                    WITH_MEM_DEBUG=0, WITH_REGEXPS=1, WITH_SCHEMAS=1, WITH_SCHEMATRON=1, WITH_MODULES=0, MODULE_EXTENSION=".dll", WITH_ZLIB=0,
                    WITH_LZMA=0)
# libtiff's sources; the codecs whose support is not configured below compile to nothing.
TIFF_SOURCES = ("tif_aux.c", "tif_close.c", "tif_codec.c", "tif_color.c", "tif_compress.c", "tif_dir.c", "tif_dirinfo.c", "tif_dirread.c",
                "tif_dirwrite.c", "tif_dumpmode.c", "tif_error.c", "tif_extension.c", "tif_fax3.c", "tif_fax3sm.c", "tif_flush.c", "tif_getimage.c",
                "tif_hash_set.c", "tif_jbig.c", "tif_jpeg.c", "tif_jpeg_12.c", "tif_lerc.c", "tif_luv.c", "tif_lzma.c", "tif_lzw.c", "tif_next.c",
                "tif_ojpeg.c", "tif_open.c", "tif_packbits.c", "tif_pixarlog.c", "tif_predict.c", "tif_print.c", "tif_read.c", "tif_strip.c",
                "tif_swab.c", "tif_thunder.c", "tif_tile.c", "tif_version.c", "tif_warning.c", "tif_webp.c", "tif_write.c", "tif_zip.c", "tif_zstd.c",
                "tif_win32.c")
ZLIB = ("adler32.c", "crc32.c", "deflate.c", "inflate.c", "inffast.c", "inftrees.c", "trees.c", "zutil.c")
COMPONENTS = ("cabinet.dll", "sxs.dll", "rpcrt4.dll", "msi.dll", "raven-storage.dll", "msiexec.exe", "msxml3.dll", "atl100.dll", "oleaut32.dll", "libjpeg.dll",
              "libtiff.dll", "propsys.dll", "mlang.dll", "windowscodecs.dll", "gdiplus.dll", "urlmon.dll", "riched20.dll", "msftedit.dll", "msvfw32.dll", "advpack.dll", "raven-setupapi.dll", "xmllite.dll",
              "raven-appx.dll", "mspatcha.dll", "vbscript.dll", "scrrun.dll", "wshom.ocx", "jscript.dll")


# The export list of a library from its specification file: the subset of the format the specifications here use, one export per line,
# an ordinal or "@" for the next one, stdcall or cdecl, -private, and a stdcall name decorated with its argument bytes on x86.
def spec_exports(spec, name, x86):
    lines = ["; File generated automatically, do not edit!", "", f"NAME {name}", "", "EXPORTS"]
    ordinal = 0
    for raw in spec.read_text().splitlines():
        text = raw.split("#", 1)[0].strip()
        if not text:
            continue
        match = re.fullmatch(r"(@|\d+)\s+(stdcall|cdecl)((?:\s+-\w+)*)\s+([A-Za-z_]\w*)\(([^)]*)\)", text)
        if not match:
            raise RuntimeError(f"{spec}: unsupported specification line: {raw}")
        number, convention, options, symbol, arguments = match.groups()
        ordinal = int(number) if number != "@" else ordinal + 1
        entry = symbol
        if x86 and convention == "stdcall":
            size = sum({"int64": 8, "double": 8, "int128": 16}.get(argument, 4) for argument in arguments.split())
            entry = f"{symbol}={symbol}@{size}"
        lines.append(f" {entry} @{ordinal}" + (" PRIVATE" if "-private" in options.split() else ""))
    return "\n".join(lines) + "\n"


# CMake's configure_file for the templates of a library: #cmakedefine lines become definitions for the names set, and @NAME@ the values,
# empty for a name not set as CMake leaves it.
def configure_file(template, target, values):
    def line(text):
        match = re.match(r"#(\s*)cmakedefine (\w+)(.*)", text)
        if match:
            indent, name, rest = match.groups()
            return f"#{indent}define {name}{rest}" if values.get(name) else f"/* #{indent}undef {name} */"
        return text
    text = "\n".join(line(text) for text in template.read_text().splitlines()) + "\n"
    text = re.sub(r"@(\w+)@", lambda match: str(values.get(match.group(1), "")), text)
    target.write_text(text)


def download(url, path):
    # Download servers answer with a gateway error or stall now and then; a few spaced attempts ride it out. A missing file does not come
    # back.
    for attempt in range(4):
        try:
            urllib.request.urlretrieve(url, path)
            return
        except OSError as error:
            if attempt == 3 or (isinstance(error, urllib.error.HTTPError) and error.code < 500):
                raise
            time.sleep(15 * (attempt + 1))


# Downloads an archive beside where it belongs and moves it into place once its checksum matches, so an interrupted download is not kept;
# one already there is checked the same way.
def fetch(url, archive, sha256, what):
    if not archive.exists():
        temporary = archive.with_suffix(archive.suffix + ".part")
        try:
            download(url, temporary)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != sha256:
                raise RuntimeError(f"downloaded {what} checksum differs")
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != sha256:
        raise RuntimeError(f"{what} checksum differs: {archive} is incomplete or another version; delete it to download it again")


def main():
    parser = argparse.ArgumentParser(description="Build the Windows Installer components and the libraries they install with")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arch", choices=("x64", "x86"), default="x64", help="build the 64-bit components, or the 32-bit ones for PE32 programs")
    args = parser.parse_args()
    x86 = args.arch == "x86"
    triple = "i686-w64-mingw32" if x86 else "x86_64-w64-mingw32"
    entry_point = "_DllMain@12" if x86 else "DllMain"
    link_extra = ["-Wl,--kill-at"] if x86 else []
    bases = dict(cabinet=0x1e000000, sxs=0x1e200000, rpcrt4=0x1d800000, msi=0x1e400000, storage=0x1ec00000, xslt=0x1d400000, msxml=0x1d000000, atl=0x1cc00000, oleaut=0x1c800000) if x86 else dict(
        cabinet=0x1e0000000, sxs=0x1e2000000, rpcrt4=0x1d8000000, msi=0x1e4000000, storage=0x1ec000000, xslt=0x1d4000000, msxml=0x1d0000000, atl=0x1cc000000, oleaut=0x1c8000000)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # The tools' temporary files go to a directory of their own, removed on exit, so none of them stays in the output.
    output.mkdir(parents=True, exist_ok=True)
    scratch = tempfile.mkdtemp(prefix=".tmp-", dir=output)
    atexit.register(shutil.rmtree, scratch, True)
    env = dict(os.environ, TMPDIR=scratch, TMP=scratch, TEMP=scratch)
    repo = Path(__file__).resolve().parents[1]
    upstream_sources = {}
    for name, release in UPSTREAM.items():
        version = release["version"]
        packed = output / f"{name}-{version}-source.tar.{release['url'].rsplit('.', 1)[1]}"
        fetch(release["url"].format(version=version), packed, release["sha256"], f"{name} {version} source archive")
        unpacked = upstream_sources[name] = output / f"{name}-source"
        shutil.rmtree(unpacked, ignore_errors=True)
        with tarfile.open(packed) as upstream:
            for member in upstream:
                parts = Path(member.name).parts[1:]
                if not member.isfile() or not parts:
                    continue
                if ".." in parts or Path(member.name).is_absolute():
                    raise RuntimeError(f"invalid {name} archive path")
                target = unpacked.joinpath(*parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(upstream.extractfile(member).read())
    zlib_source, jpeg_source, tiff_source = upstream_sources["zlib"], upstream_sources["libjpeg"], upstream_sources["libtiff"] / "libtiff"
    xml_root, xslt_root = upstream_sources["libxml2"], upstream_sources["libxslt"]
    # The library's own configuration for Windows compilers, which keeps boolean an unsigned char as the platform headers have it.
    shutil.copyfile(jpeg_source / "jconfig.vc", jpeg_source / "jconfig.h")
    for tool in (triple + "-gcc", triple + "-dlltool", "widl", "bison"):
        if not shutil.which(tool):
            raise RuntimeError(f"required installer build tool is missing: {tool}")
    # The headers of MinGW-w64 12 lack declarations the components use, such as MIDL_STUB_MESSAGE.CorrDespIncrement.
    macros = subprocess.run([triple + "-gcc", "-E", "-dM", "-include", "_mingw.h", "-x", "c", os.devnull], capture_output=True, text=True).stdout
    version = re.search(r"#define __MINGW64_VERSION_MAJOR (\d+)", macros)
    if version and int(version.group(1)) < 13:
        raise RuntimeError(f"{triple}-gcc uses the headers of MinGW-w64 {version.group(1)}; the installer components need MinGW-w64 13 or newer")
    records = []

    def run(command, cwd=None):
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
        records.append(dict(command=[str(part) for part in command], returncode=result.returncode))
        if result.returncode:
            raise RuntimeError(result.stderr or "installer build command failed: " + " ".join(map(str, command)))
        return result

    def mingw_include(header):
        """The directory of the cross compiler's own search path that holds a header: /usr/<triple>/include on Debian and Ubuntu,
        /usr/<triple>/sys-root/mingw/include on Fedora."""
        listing = run([triple + "-gcc", "-xc", "-E", "-v", os.devnull]).stderr.splitlines()
        start = listing.index("#include <...> search starts here:") + 1
        for line in listing[start:listing.index("End of search list.")]:
            directory = Path(line.strip()).resolve()
            if (directory / header).exists():
                return directory
        raise RuntimeError(f"the {triple} compiler has no {header}")

    def decorated(object_files):
        """Defined global functions of the objects by plain name, with their stdcall argument bytes on 32 bits (None for the others)."""
        names = {}
        for line in run([triple + "-nm", "-g", "--defined-only", *object_files]).stdout.splitlines():
            parts = line.split()
            if len(parts) != 3 or parts[1] not in "Tt":
                continue
            symbol = parts[2]
            if x86:
                match = re.fullmatch(r"_(\w+)@(\d+)", symbol)
                if match:
                    names[match.group(1)] = int(match.group(2))
                elif symbol.startswith("_"):
                    names.setdefault(symbol[1:], None)
            else:
                names[symbol] = None
        return names

    # Raven's corrections to a library taken from its own release are kept as reviewable patches in a folder named after it, and recorded
    # in the provenance.
    support = repo / "native/guest/raven-installer"
    patches = []
    for name, unpacked in upstream_sources.items():
        own = sorted((support / "patches" / name).glob("*.patch"))
        for patch in own:
            run(["patch", "-p1", "--forward", "--silent", "-i", patch], unpacked)
        mark_patched_sources(unpacked, own)
        patches += own
    generated = output / "generated"
    shutil.rmtree(generated, ignore_errors=True)
    generated.mkdir(parents=True)
    # Type libraries are compiled against the interface definitions installed with the IDL compiler.
    widl = ["widl", "-m32" if x86 else "-m64"]
    # A toolchain whose default C runtime is the UCRT gives that library the name msvcrt and keeps the one of msvcrt.dll as msvcrt-os.
    msvcrt = "-lmsvcrt-os" if Path(run([triple + "-gcc", "-print-file-name=libmsvcrt-os.a"]).stdout.strip()).is_absolute() else "-lmsvcrt"
    # Raven code and the upstream libraries on the MinGW-w64 headers. A function the headers do not declare would return a truncated int: none
    # is allowed. GCC 14 and later reject the pointer mismatches the upstream libraries still carry; they stay warnings, as with the compilers
    # those libraries are built with upstream. The formatting functions are the C runtime's own, as the components link no other.
    flags = ["-O2", "-Werror=implicit-function-declaration", "-Werror=int-conversion", "-Wno-error=incompatible-pointer-types",
             "-D__USE_MINGW_ANSI_STDIO=0", "-D_WIN32_WINNT=0x600", "-DWINVER=0x600", "-D_WIN32_IE=0x0A00", "-DUNICODE", "-D_UNICODE",
             f"-ffile-prefix-map={repo}=raven", f"-ffile-prefix-map={output}=component"]
    objects = output / "objects"
    shutil.rmtree(objects, ignore_errors=True)
    objects.mkdir()

    def compile(path, name, extra=()):
        target = objects / (name + ".o")
        run([triple + "-gcc", "-c", *flags, *extra, "-I" + str(generated), path, "-o", target])
        return target

    def runtime(libraries):
        return [msvcrt if library == "-lmsvcrt" else library for library in libraries]

    def link(name, base, object_files, definition, libraries, implib=None):
        run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{base:#x},--no-insert-timestamp", *link_extra,
             *([f"-Wl,--out-implib,{implib}"] if implib and not x86 else []), "-o", output / name, *object_files, definition,
             output / "libcrtimports.a", *runtime(libraries)])
        # A 32-bit import library keeps the stdcall decorations its callers link against, for the plain names the library exports.
        if implib and x86:
            stdcall = output / (Path(name).stem + "-import.def")
            stdcall.write_text(re.sub(r"^(\s*)(\w+)=\w+(@\d+)", r"\1\2\3", Path(definition).read_text(), flags=re.M))
            run([triple + "-dlltool", "-d", stdcall, "-l", implib, "-k"])

    # Import these CRT functions directly; newer MinGW libraries replace them with static wrappers.
    imports = output / "crtimports.def"
    imports.write_text("LIBRARY msvcrt.dll\nEXPORTS\n _assert\n iswctype\n isprint\n _gmtime64\n")
    run([triple + "-dlltool", "-d", imports, "-l", output / "libcrtimports.a"])
    # Newer 32-bit MinGW libraries import the LZ expansion functions from lz32, a forwarder; kernel32 exports them.
    expansion = output / "lzimports.def"
    expansion.write_text("LIBRARY kernel32.dll\nEXPORTS\n" + "".join(" " + name + ("@" + size if x86 else "") + "\n" for name, size in
                         (("LZOpenFileW", "12"), ("LZRead", "12"), ("LZSeek", "12"), ("LZClose", "4"))))
    run([triple + "-dlltool", "-d", expansion, "-l", output / "liblzimports.a", *(["-k"] if x86 else [])])
    # cabinet.dll: cabinet files, their compression and extraction, on zlib.
    zlib_include = zlib_source
    zlib = [compile(zlib_source / name, "zlib_" + Path(name).stem, ["-I" + str(zlib_include)]) for name in ZLIB]
    cabinet_root = repo / "native/guest/raven-cabinet"
    with ThreadPoolExecutor(max_workers=2) as pool:
        cabinet = list(pool.map(lambda name: compile(cabinet_root / name, "cabinet_" + Path(name).stem,
                                                     ["-I" + str(cabinet_root), "-I" + str(zlib_include)]),
                                ("compose.c", "extract.c", "lzx.c", "quantum.c", "session.c")))
    (output / "cabinet.def").write_text(spec_exports(cabinet_root / "cabinet.spec", "cabinet.dll", x86))
    link("cabinet.dll", bases["cabinet"], [*zlib, *cabinet], output / "cabinet.def", ["-lmsvcrt", "-lkernel32", "-lntdll", "-lgcc"],
         output / "libcabinet.a")
    # sxs.dll: the side-by-side assembly cache.
    own_sxs = repo / "native/guest/raven-sxs"
    (output / "sxs.def").write_text("LIBRARY sxs.dll\nEXPORTS\n" + (own_sxs / "exports.txt").read_text())
    run([triple + "-gcc", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-DCONST_VTABLE", "-shared", "-nostdlib",
         f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{bases['sxs']:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "sxs.dll", own_sxs / "sxs.c", repo / "native/guest/raven-comctl32/crt.c", output / "sxs.def", "-luuid", "-lole32", "-loleaut32", "-luser32", "-lkernel32", "-lgcc"])
    # The installer engine links against the callers' view of these: on x86 their names carry their argument sizes.
    (output / "sxs_import.def").write_text("LIBRARY sxs.dll\nEXPORTS\n" + "".join(f" {name}{'@' + str(size) if x86 else ''}\n" for name, size in
                                           (("CreateAssemblyCache", 8), ("CreateAssemblyNameObject", 16), ("SxsLookupClrGuid", 24), ("SxspGenerateManifestPathOnAssemblyIdentity", 16))))
    run([triple + "-dlltool", *(["-k"] if x86 else []), "-d", output / "sxs_import.def", "-l", output / "libsxs.a"])
    compile(support / "support.c", "installer_support")
    # rpcrt4.dll: Raven's own remote procedure calls, which the installer's custom action server speaks over local pipes.
    own_rpc = repo / "native/guest/raven-rpcrt4"
    rpc_objects = []
    for path in [*sorted(own_rpc.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("rpcrt4_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-D_UNICODE", "-DCOBJMACROS", "-D_RPCRT4_", "-I" + str(own_rpc), "-o", target, path])
        rpc_objects.append(target)
    rpc_exports = (own_rpc / "exports.txt").read_text()
    (output / "rpcrt4.def").write_text("LIBRARY rpcrt4.dll\nEXPORTS\n" + rpc_exports)
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{bases['rpcrt4']:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "rpcrt4.dll", *rpc_objects, output / "rpcrt4.def", "-lole32", "-loleaut32", "-luuid", "-luser32", "-ladvapi32", "-lkernel32", "-lntdll", "-lgcc"])
    rpc_defined = decorated(rpc_objects)
    (output / "rpcrt4_import.def").write_text("LIBRARY rpcrt4.dll\nEXPORTS\n" + "".join(
        f" {name}{'' if rpc_defined.get(name) is None else '@' + str(rpc_defined[name])}\n" for name in (line.split()[0] for line in rpc_exports.splitlines() if line.strip()) if "PRIVATE" not in name))
    run([triple + "-dlltool", *(["-k"] if x86 else []), "-d", output / "rpcrt4_import.def", "-l", output / "librpcrt4.a"])
    # msi.dll: the Windows Installer engine, Raven's own.
    own_msi = repo / "native/guest/raven-msi"
    msi_objects = []
    for path in [*sorted(p for p in own_msi.glob("*.c") if p.name != "msiexec.c"), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("msi_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-D_UNICODE", "-DCONST_VTABLE", "-D_CRTIMP=",
             "-I" + str(own_msi), path, "-o", target])
        msi_objects.append(target)
    msi_exports = (own_msi / "exports.txt").read_text()
    msi_defined = decorated(msi_objects)
    def msi_line(line):
        name = line.split()[0]
        return " " + name + ("@" + str(msi_defined[name]) if x86 and msi_defined.get(name) is not None else "") + " " + " ".join(line.split()[1:]) + "\n"
    (output / "msi.def").write_text("LIBRARY msi.dll\nEXPORTS\n" + "".join(msi_line(line) for line in [*msi_exports.splitlines(), "RavenActionHost"] if line.strip()))
    link("msi.dll", bases["msi"], msi_objects, output / "msi.def",
         # The toolchain's oleaut32 import library by its file name: beside the output, -loleaut32 would find the runtime's own oleaut32.dll.
         ["-L" + str(output), "-lcabinet", "-lrpcrt4", "-l:liboleaut32.a",
          *[f"-l{library}" for library in ("advapi32", "ole32", "shell32", "user32", "gdi32", "version", "kernel32", "uuid", "gcc")]], implib=output / "libmsi.a")
    # raven-storage.dll: Raven's own structured storage, which the runtime's ole32 hands those exports to.
    own_storage = repo / "native/guest/raven-storage"
    own_objects = []
    for path in [*sorted(own_storage.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("storage_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=c11", "-fno-builtin", "-D_WIN32_WINNT=0x600", "-DUNICODE", "-D_OLE32_", "-DCONST_VTABLE",
             "-I" + str(own_storage), path, "-o", target])
        own_objects.append(target)
    exported = (own_storage / "exports.txt").read_text().split()
    defined = decorated(own_objects)
    (output / "raven-storage.def").write_text("NAME raven-storage.dll\nEXPORTS\n" + "".join(
        " " + name + ("@" + str(defined[name]) if x86 and defined[name] is not None else "") + "\n" for name in exported))
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{bases['storage']:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "raven-storage.dll", *own_objects, output / "raven-storage.def", "-lole32", "-loleaut32", "-luser32", "-ladvapi32", "-lkernel32", "-luuid", "-lgcc"])
    # msiexec.exe: the command line of the installer, which installers and the shell start for packages.
    run([triple + "-gcc", "-O2", "-std=gnu11", "-fno-builtin", "-nostdlib", "-mwindows", "-D_WIN32_WINNT=0x601", "-DUNICODE",
         f"-Wl,--entry,{'_raven_msiexec@0' if x86 else 'raven_msiexec'},--no-insert-timestamp", "-I" + str(own_msi),
         own_msi / "msiexec.c", repo / "native/guest/raven-comctl32/crt.c", "-o", output / "msiexec.exe", "-L" + str(output), "-lmsi", "-luser32", "-lkernel32", "-lgcc"])
    # msxml3.dll: the XML document object model installers and their bootstrappers parse their manifests with, on libxml2 and libxslt.
    # libxml2's configuration: its own one for Windows compilers, and the version header with the features listed above. Files are
    # examined with the C runtime's plain stat, for libxslt as well.
    shutil.copyfile(xml_root / "include/win32config.h", xml_root / "config.h")
    major, minor, micro = (int(part) for part in UPSTREAM["libxml2"]["version"].split("."))
    configure_file(xml_root / "include/libxml/xmlversion.h.in", xml_root / "include/libxml/xmlversion.h", dict(
        XML_FEATURES, VERSION=UPSTREAM["libxml2"]["version"], LIBXML_VERSION_NUMBER=major * 10000 + minor * 100 + micro, LIBXML_VERSION_EXTRA=""))
    xml_flags = ["-DNO_LARGEFILE_SOURCE", "-D_CRT_DECLARE_NONSTDC_NAMES=1", "-D__MINGW32__", "-D_DLL", "-D__USE_CRTIMP", "-DLIBXML_STATIC", "-D_REENTRANT", "-D_MBCS", "-D_WINDOWS", "-DWIN32", "-DHAVE_CONFIG_H",
                 "-I" + str(xml_root / "include"), "-I" + str(xml_root), "-I" + str(zlib_include)]
    # The C runtime's mathematics, which mingw's msvcrt import library would bring in as static code of its own.
    (output / "crtmath.def").write_text("LIBRARY msvcrt.dll\nEXPORTS\n" + "".join(" " + name + "\n" for name in
                                        ("floor", "ceil", "fabs", "fmod", "log", "log10", "exp", "pow", "sqrt", "atan", "atan2", "tan",
                                         "floorf", "ceilf", "_hypot")))
    run([triple + "-dlltool", "-d", output / "crtmath.def", "-l", output / "libcrtmath.a"])
    xml_sources = [xml_root / name for name in XML_SOURCES]
    with ThreadPoolExecutor(max_workers=2) as pool:
        xml = list(pool.map(lambda path: compile(path, "libxml2_" + path.stem, xml_flags), xml_sources))
    # libxslt's configuration: its own one for Windows, which its internal header reads as config.h under mingw, and a version header
    # with the debugger, profiler, plugins and trio left out.
    shutil.copyfile(xslt_root / "libxslt/win32config.h", xslt_root / "libxslt/config.h")
    major, minor, micro = (int(part) for part in UPSTREAM["libxslt"]["version"].split("."))
    configure_file(xslt_root / "libxslt/xsltconfig.h.in", xslt_root / "libxslt/xsltconfig.h", dict(
        VERSION=UPSTREAM["libxslt"]["version"], LIBXSLT_VERSION_NUMBER=major * 10000 + minor * 100 + micro, LIBXSLT_VERSION_EXTRA="",
        WITH_XSLT_DEBUG=0, WITH_TRIO=0, WITH_DEBUGGER=0, WITH_PROFILER=0, WITH_MODULES=0, LIBXSLT_DEFAULT_PLUGINS_PATH=""))
    xslt_flags = [*xml_flags, "-DHAVE_WIN32_THREADS", "-I" + str(xslt_root), "-I" + str(xslt_root / "libxslt")]
    with ThreadPoolExecutor(max_workers=2) as pool:
        xslt = list(pool.map(lambda path: compile(path, "libxslt_" + path.stem, xslt_flags), sorted((xslt_root / "libxslt").glob("*.c"))))
    # Its registration scripts' type library comes from the public headers, and stays with stdole2's for what else needs it.
    run([*widl, "-t", "-T", generated / "stdole2.tlb", repo / "native/guest/raven-oleaut32/stdole2.idl"])
    own_msxml = repo / "native/guest/raven-msxml3"
    headers = mingw_include("msxml2.h")
    run([sys.executable, own_msxml / "make_idl.py", headers / "msxml2.h", headers / "msxml2did.h", headers / "msxml.h", generated / "msxml3.idl"])
    run([*widl, "-I", headers, "-L", generated, "-t", "-o", generated / "msxml3.tlb", generated / "msxml3.idl"])
    run([triple + "-windres", "-O", "coff", "-I", generated, "-i", own_msxml / "resources.rc", "-o", objects / "msxml3_resources.o"])
    msxml_flags = ["-O2", "-std=gnu11", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-DLIBXML_STATIC", "-DLIBXSLT_STATIC", "-DCONST_VTABLE", "-I" + str(own_msxml),
                   "-I" + str(xml_root / "include"), "-I" + str(xml_root), "-I" + str(xslt_root)]
    msxml_objects = [objects / "msxml3_resources.o"]
    for path in sorted(own_msxml.glob("*.c")):
        target = objects / ("msxml3_" + path.stem + ".o")
        run([triple + "-gcc", "-c", *msxml_flags, path, "-o", target])
        msxml_objects.append(target)
    (output / "msxml3.def").write_text("LIBRARY msxml3.dll\nEXPORTS\n" + (own_msxml / "exports.txt").read_text())
    # MinGW-w64's own support library brings the C99 formatting and the stat path handling its headers route those calls through.
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{bases['msxml']:#x},--no-insert-timestamp,--allow-multiple-definition", *link_extra,
         "-o", output / "msxml3.dll", *msxml_objects, *xslt, *xml, *zlib, objects / "installer_support.o", output / "msxml3.def", output / "libcrtmath.a",
         *runtime(f"-l{library}" for library in ("urlmon", "ws2_32", "shlwapi", "oleaut32", "ole32", "user32", "advapi32", "msvcrt", "mingwex", "kernel32", "ntdll", "uuid", "gcc", "kernel32", "msvcrt", "kernel32"))])
    # atl100.dll: the ATL registrar, which reads the registration scripts libraries register their classes with, and the control host.
    own_atl = repo / "native/guest/raven-atl100"
    atl_objects = []
    for part in ("module", "registrar", "ax"):
        atl_objects.append(objects / ("atl100_" + part + ".o"))
        run([triple + "-gcc", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-DCONST_VTABLE", "-D_CRTIMP=", "-w", "-c", "-o", atl_objects[-1], own_atl / (part + ".c")])
    (output / "atl100.def").write_text("LIBRARY atl100.dll\nEXPORTS\n" + (own_atl / "exports.txt").read_text())
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{bases['atl']:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "atl100.dll", *atl_objects, repo / "native/guest/raven-comctl32/crt.c", output / "atl100.def", "-luuid", "-lole32", "-loleaut32", "-ladvapi32", "-lgdi32", "-luser32", "-lkernel32", "-lgcc"])
    # oleaut32.dll: Raven's own automation, variants, safe arrays and type libraries, carrying the standard type library.
    own_oleaut = repo / "native/guest/raven-oleaut32"
    oleaut_objects = []
    for path in [*sorted(own_oleaut.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("oleaut32_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-D_OLEAUT32_", "-DCONST_VTABLE", "-D_CRTIMP=", "-I" + str(own_oleaut), path, "-o", target])
        oleaut_objects.append(target)
    run([triple + "-windres", "-I", generated, "-I", own_oleaut, own_oleaut / "resources.rc", "-o", objects / "oleaut32.res.o"])
    oleaut_objects.append(objects / "oleaut32.res.o")
    oleaut_defined = decorated(oleaut_objects)
    def oleaut_export(line):
        entry, _, fixed = line.partition(" ")
        name, _, implementation = entry.partition("=")
        target = implementation or name
        words = "@" + str(oleaut_defined[target]) if x86 and oleaut_defined.get(target) is not None else ""
        return f" {name}={implementation}{words} {fixed}\n" if implementation else f" {name}{words} {fixed}\n"
    (output / "oleaut32.def").write_text("LIBRARY oleaut32.dll\nEXPORTS\n" + "".join(oleaut_export(line) for line in (own_oleaut / "exports.txt").read_text().splitlines()))
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{bases['oleaut']:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "oleaut32.dll", *oleaut_objects, output / "oleaut32.def", "-luuid", "-lole32", "-lgdi32", "-luser32", "-ladvapi32", "-lkernel32", "-lgcc"])
    # The libraries below follow one pattern: their sources compiled with the flags given, a definition file from their specification
    # and the libraries they import. Their base addresses count down from below the ones above.
    next_base = [0x1c4000000 if not x86 else 0x1c400000]

    def library(name, root, extra_flags=(), libraries=(), sources=(), objects_more=()):
        stem = Path(name).stem
        spec = root / (stem + ".spec") if (root / (stem + ".spec")).exists() else next(root.glob("*.spec"))
        (output / (stem + ".def")).write_text(spec_exports(spec, name, x86))
        flags_here = [*extra_flags, "-I" + str(root)]
        with ThreadPoolExecutor(max_workers=2) as pool:
            built = list(pool.map(lambda path: compile(path, stem + "_" + path.stem.replace(".", "_"), flags_here), sources))
        next_base[0] -= 0x400000 if x86 else 0x4000000
        link(name, next_base[0], [*built, *objects_more, objects / "installer_support.o"], output / (stem + ".def"),
             ["-L" + str(output), "-lcrtmath", *[f"-l{library}" for library in (*libraries, "msvcrt", "mingwex", "kernel32", "ntdll", "uuid", "gcc", "kernel32", "msvcrt", "kernel32")]],
             output / ("lib" + stem + ".a"))

    plain = ["-D_CRT_DECLARE_NONSTDC_NAMES=1", "-D_DLL", "-DWIN32", "-D_WINDOWS"]
    entry_object = compile(support / "dllmain.c", "plain_dllmain")
    # libjpeg.dll: the library's sources as its makefile lists them, with the memory manager that needs no temporary files, exporting the
    # whole interface jpeglib.h declares.
    jpeg_sources = re.search(r"^LIBSOURCES *=(.*?)\n\n", (jpeg_source / "Makefile.am").read_text(), re.S | re.M).group(1)
    jpeg_sources = [jpeg_source / name.replace("@MEMORYMGR@", "jmemnobs") for name in jpeg_sources.replace("\\", " ").split()]
    jpeg_interface = sorted(set(re.findall(r"EXTERN\([^)]*\) +(jpeg_\w+)", (jpeg_source / "jpeglib.h").read_text())))
    (jpeg_source / "libjpeg.spec").write_text("".join(f"@ cdecl {name}()\n" for name in jpeg_interface))
    library("libjpeg.dll", jpeg_source, [*plain, "-I" + str(jpeg_source)], sources=jpeg_sources, objects_more=[entry_object])
    # libtiff.dll: configured as its CMake build would be for Windows, with the codecs on JPEG and zlib and the ones needing other
    # libraries left out, exporting the interface its own definition file lists.
    size = 4 if x86 else 8
    configure_file(tiff_source / "tif_config.h.cmake.in", tiff_source / "tif_config.h", dict(
        CCITT_SUPPORT=1, CHECK_JPEG_YCBCR_SUBSAMPLING=1, HAVE_ASSERT_H=1, HAVE_FCNTL_H=1, HAVE_FSEEKO=1, HAVE_IO_H=1, HAVE_SETMODE=1,
        HAVE_STRINGS_H=1, HAVE_SYS_TYPES_H=1, HAVE_UNISTD_H=1, USE_WIN32_FILEIO=1, STRIP_SIZE_DEFAULT=8192, TIFF_MAX_DIR_COUNT=1048576,
        SIZEOF_SIZE_T=size, LIBJPEG_12_PATH="", PACKAGE_NAME="LibTIFF Software", PACKAGE_BUGREPORT="tiff@lists.osgeo.org",
        PACKAGE_TARNAME="tiff", PACKAGE_URL=""))
    configure_file(tiff_source / "tiffconf.h.cmake.in", tiff_source / "tiffconf.h", dict(
        TIFF_INT8_T="int8_t", TIFF_INT16_T="int16_t", TIFF_INT32_T="int32_t", TIFF_INT64_T="int64_t", TIFF_UINT8_T="uint8_t",
        TIFF_UINT16_T="uint16_t", TIFF_UINT32_T="uint32_t", TIFF_UINT64_T="uint64_t", TIFF_SSIZE_T=f"int{size * 8}_t", HAVE_IEEEFP=1,
        HOST_BIG_ENDIAN=0, CCITT_SUPPORT=1, JPEG_SUPPORT=1, LOGLUV_SUPPORT=1, LZW_SUPPORT=1, NEXT_SUPPORT=1, PACKBITS_SUPPORT=1,
        PIXARLOG_SUPPORT=1, THUNDER_SUPPORT=1, ZIP_SUPPORT=1, STRIPCHOP_DEFAULT=1, SUBIFD_SUPPORT=1, DEFAULT_EXTRASAMPLE_AS_ALPHA=1,
        CHECK_JPEG_YCBCR_SUBSAMPLING=1, MDI_SUPPORT=1))
    tiff_interface = (tiff_source / "libtiff.def").read_text().split("EXPORTS", 1)[1].split()
    (tiff_source / "libtiff.spec").write_text("".join(f"@ cdecl {name}()\n" for name in tiff_interface))
    library("libtiff.dll", tiff_source, [*plain, "-DNDEBUG=", "-DALLOW_TIFF_NON_EXT_ALLOC_FUNCTIONS", "-I" + str(tiff_source),
            "-I" + str(jpeg_source), "-I" + str(zlib_include)], ["libjpeg", "user32"],
            sources=[tiff_source / name for name in TIFF_SOURCES], objects_more=[*zlib, entry_object])
    registration = ("register.c", "dll_register.c", "dll_canunload.c")
    # propsys.dll: property values, the in-memory store and the system's property table, generated from the public key list.
    own_propsys = repo / "native/guest/raven-propsys"
    propsys_flags = ["-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-DCONST_VTABLE", "-DINITGUID", "-D_PROPSYS_", "-I" + str(generated)]
    run([sys.executable, own_propsys / "make_schema.py", mingw_include("propkey.h") / "propkey.h", generated / "schema.inc"])
    propsys_objects = []
    for part in ("convert", "store", "system", "main"):
        propsys_objects.append(objects / ("propsys_" + part + ".o"))
        run([triple + "-gcc", *propsys_flags, "-w", "-c", "-o", propsys_objects[-1], own_propsys / (part + ".c")])
    (output / "propsys.def").write_text("LIBRARY propsys.dll\nEXPORTS\n" + (own_propsys / "exports.txt").read_text())
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "propsys.dll", *propsys_objects, repo / "native/guest/raven-comctl32/crt.c", output / "propsys.def", "-luuid", "-lole32", "-loleaut32", "-ladvapi32", "-luser32", "-lkernel32", "-lgcc"])
    # Other components link against the callers' view: on x86 the names carry their argument sizes, which only the objects know.
    wanted = {line.split("=")[0].strip() for line in (own_propsys / "exports.txt").read_text().splitlines() if "=" not in line}
    propsys_names = sorted({word.lstrip("_") for line in subprocess.run([triple + "-nm", "-g", "--defined-only", *map(str, propsys_objects)], env=env, check=True, capture_output=True, text=True).stdout.splitlines()
                        for word in line.split()[-1:] if word.lstrip("_").split("@")[0] in wanted})
    (output / "propsys_import.def").write_text("LIBRARY propsys.dll\nEXPORTS\n" + "".join(f" {name}\n" for name in propsys_names))
    run([triple + "-dlltool", *(["-k"] if x86 else []), "-d", output / "propsys_import.def", "-l", output / "libpropsys.a"])
    # mlang.dll: code pages, language tags and font linking.
    own_mlang = repo / "native/guest/raven-mlang"
    mlang_objects = []
    for part in ("pages", "tags", "fontlink", "language", "main"):
        mlang_objects.append(objects / ("mlang_" + part + ".o"))
        run([triple + "-gcc", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-DCONST_VTABLE", "-w", "-c", "-o", mlang_objects[-1], own_mlang / (part + ".c")])
    (output / "mlang.def").write_text("LIBRARY mlang.dll\nEXPORTS\n" + (own_mlang / "exports.txt").read_text())
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "mlang.dll", *mlang_objects, repo / "native/guest/raven-comctl32/crt.c", output / "mlang.def", "-luuid", "-lole32", "-loleaut32", "-ladvapi32", "-lgdi32", "-luser32", "-lkernel32", "-lgcc"])
    wanted = {line.split()[0] for line in (own_mlang / "exports.txt").read_text().splitlines() if "PRIVATE" not in line}
    mlang_names = sorted({word.lstrip("_") for line in subprocess.run([triple + "-nm", "-g", "--defined-only", *map(str, mlang_objects)], env=env, check=True, capture_output=True, text=True).stdout.splitlines()
                        for word in line.split()[-1:] if word.lstrip("_").split("@")[0] in wanted})
    (output / "mlang_import.def").write_text("LIBRARY mlang.dll\nEXPORTS\n" + "".join(f" {name}\n" for name in mlang_names))
    run([triple + "-dlltool", *(["-k"] if x86 else []), "-d", output / "mlang_import.def", "-l", output / "libmlang.a"])
    # windowscodecs.dll: Raven's own imaging component; JPEG and TIFF go through the libraries built above, named by path so
    # no export of the component can resolve to one of the installer's own import libraries.
    own_codecs = repo / "native/guest/raven-windowscodecs"
    codecs_objects = []
    for path in [*sorted(own_codecs.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("codecs_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-D_CRTIMP=", "-DCOBJMACROS", "-I" + str(own_codecs),
             "-I" + str(jpeg_source), "-I" + str(tiff_source), "-w", path, "-o", target])
        codecs_objects.append(target)
    codecs_exports = (own_codecs / "exports.txt").read_text()
    (output / "windowscodecs.def").write_text("LIBRARY windowscodecs.dll\nEXPORTS\n" + codecs_exports)
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "windowscodecs.dll", *codecs_objects, output / "windowscodecs.def", output / "liblibjpeg.a", output / "liblibtiff.a", "-lole32", "-loleaut32", "-luuid", "-ladvapi32",
         "-lgdi32", "-luser32", "-lkernel32", "-lgcc"])
    codecs_defined = decorated(codecs_objects)
    codecs_names = [line.split()[0].partition("=")[0] for line in codecs_exports.splitlines() if line.strip()]
    (output / "windowscodecs_import.def").write_text("LIBRARY windowscodecs.dll\nEXPORTS\n" + "".join(
        f" {name}{'@' + str(codecs_defined[name]) if x86 and codecs_defined.get(name) is not None else ''}\n" for name in codecs_names))
    run([triple + "-dlltool", *(["-k"] if x86 else []), "-d", output / "windowscodecs_import.def", "-l", output / "libwindowscodecs.a"])
    # gdiplus.dll: Raven's own drawing library, which reads and writes pictures through the imaging component.
    own_gdiplus = repo / "native/guest/raven-gdiplus"
    gdiplus_objects = []
    for path in [*sorted(own_gdiplus.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("gdiplus_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-D_UNICODE", "-DCOBJMACROS", "-D_CRTIMP=", "-I" + str(own_gdiplus), path, "-o", target])
        gdiplus_objects.append(target)
    gdiplus_exports = (own_gdiplus / "exports.txt").read_text()
    (output / "gdiplus.def").write_text("LIBRARY gdiplus.dll\nEXPORTS\n" + gdiplus_exports)
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "gdiplus.dll", *gdiplus_objects, output / "gdiplus.def", "-lole32", "-loleaut32", "-lshlwapi", "-luuid", "-luser32", "-lgdi32", "-lkernel32", "-ladvapi32", "-lgcc"])
    gdiplus_defined = decorated(gdiplus_objects)
    gdiplus_names = [line.strip() for line in gdiplus_exports.splitlines() if line.strip()]
    (output / "gdiplus_import.def").write_text("LIBRARY gdiplus.dll\nEXPORTS\n" + "".join(
        f" {name}{'@' + str(gdiplus_defined[name]) if x86 and gdiplus_defined.get(name) is not None else ''}\n" for name in gdiplus_names))
    run([triple + "-dlltool", *(["-k"] if x86 else []), "-d", output / "gdiplus_import.def", "-l", output / "libgdiplus.a"])
    # urlmon.dll: Raven's own. Ordinals without a name are the ones old callers import by.
    own_urlmon = repo / "native/guest/raven-urlmon"
    urlmon_objects = []
    for path in [*sorted(own_urlmon.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("urlmon_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-DCONST_VTABLE", "-D_CRTIMP=", "-I" + str(own_urlmon), path, "-o", target])
        urlmon_objects.append(target)
    urlmon_defined = decorated(urlmon_objects)
    def urlmon_export(line):
        entry, _, fixed = line.partition(" ")
        name, _, implementation = entry.partition("=")
        target = implementation or name
        words = "@" + str(urlmon_defined[target]) if x86 and urlmon_defined.get(target) is not None else ""
        if name.startswith("@"):
            return f" ordinal{name[1:]}={target}{words} {name} NONAME\n"
        return f" {name}={implementation}{words} {fixed}\n" if implementation else f" {name}{words} {fixed}\n"
    (output / "urlmon.def").write_text("LIBRARY urlmon.dll\nEXPORTS\n" + "".join(urlmon_export(line) for line in (own_urlmon / "exports.txt").read_text().splitlines()))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "urlmon.dll", *urlmon_objects, output / "urlmon.def", "-luuid", "-lole32", "-loleaut32", "-lshlwapi", "-ladvapi32", "-lwininet", "-luser32", "-lshell32", "-lkernel32", "-lgcc"])
    # riched20.dll: Raven's own rich edit control, with the type library of its text object model that its resources carry.
    own_riched = repo / "native/guest/raven-riched20"
    run([*widl, "-L", generated, "-t", "-T", generated / "tom_library.tlb", own_riched / "tom_library.idl"])
    run([triple + "-windres", "-I", generated, own_riched / "resources.rc", "-o", objects / "riched20.res.o"])
    riched_objects = [objects / "riched20.res.o"]
    for path in [*sorted(own_riched.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("riched20_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-D_UNICODE", "-DCOBJMACROS", "-D_CRTIMP=", "-I" + str(own_riched), path, "-o", target])
        riched_objects.append(target)
    riched_exports = (own_riched / "exports.txt").read_text()
    (output / "riched20.def").write_text("LIBRARY riched20.dll\nEXPORTS\n" + riched_exports)
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "riched20.dll", *riched_objects, output / "riched20.def", "-lole32", "-loleaut32", "-luuid", "-luser32", "-lgdi32", "-lkernel32", "-lgcc"])
    (output / "riched20_import.def").write_text("LIBRARY riched20.dll\nEXPORTS\n" + (" CreateTextServices@12\n" if x86 else " CreateTextServices\n"))
    run([triple + "-dlltool", *(["-k"] if x86 else []), "-d", output / "riched20_import.def", "-l", output / "libriched20.a"])
    # msftedit.dll: the newer rich edit library, which imports riched20.dll and passes every export on to it.
    (output / "msftedit.def").write_text("LIBRARY msftedit.dll\nEXPORTS\n" + "".join(
        f"\t{name} = riched20.{name}{' DATA' if name.startswith('IID_') else ''}\n" for name in ("CreateTextServices", "IID_IRichEditOle",
        "IID_IRichEditOleCallback", "IID_ITextHost", "IID_ITextHost2", "IID_ITextServices", "REExtendedRegisterClass", "RichEdit10ANSIWndProc",
        "RichEditANSIWndProc")))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    link("msftedit.dll", next_base[0], [compile(support / "msftedit.c", "msftedit", plain)], output / "msftedit.def",
         ["-L" + str(output), *[f"-l{library}" for library in ("riched20", "kernel32", "gcc")]])
    # msvfw32.dll: Raven's own video for Windows: the codec manager, DrawDib and the MCI window.
    own_vfw = repo / "native/guest/raven-msvfw32"
    vfw_objects = []
    for part in ("codecs", "draw", "mciwnd", "main"):
        vfw_objects.append(objects / ("msvfw32_" + part + ".o"))
        run([triple + "-gcc", "-O2", "-std=gnu11", "-fno-builtin", "-D_WIN32_WINNT=0x601", "-DUNICODE", "-D_CRTIMP=", "-w", "-c", "-o", vfw_objects[-1], own_vfw / (part + ".c")])
    (output / "msvfw32.def").write_text("LIBRARY msvfw32.dll\nEXPORTS\n" + (own_vfw / "exports.txt").read_text())
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp,--enable-stdcall-fixup", *link_extra,
         "-o", output / "msvfw32.dll", *vfw_objects, repo / "native/guest/raven-comctl32/crt.c", output / "msvfw32.def", "-lcomdlg32", "-ladvapi32", "-lgdi32", "-luser32", "-lkernel32", "-lgcc"])
    # xmllite.dll: the streaming XML reader and writer, which the Office Click-to-Run client reads its configuration with.
    xmllite_root = repo / "native/guest/raven-xmllite"
    library("xmllite.dll", xmllite_root, sources=[xmllite_root / name for name in ("guid.c", "reader.c", "writer.c")])
    # raven-appx.dll: the packaging API (the AppxFactory and AppxBundleFactory classes), which package managers read .appx and .msix
    # packages with. Raven code on the MinGW headers and the interface declarations generated from Microsoft's metadata; it reads the
    # manifests with xmllite and inflates with the zlib cabinet.dll is built on.
    appx_root = repo / "native/guest/raven-appx"
    appx = []
    for name in ("package.c", "archive.c", "blockmap.c", "manifest.c", "support.c"):
        target = objects / ("appx_" + Path(name).stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-Wall", "-Wno-unused-parameter", f"-ffile-prefix-map={repo}=raven",
             "-I" + str(appx_root), "-I" + str(zlib_include), appx_root / name, "-o", target])
        appx.append(target)
    (output / "raven-appx.def").write_text("NAME raven-appx.dll\nEXPORTS\n")
    next_base[0] -= 0x400000 if x86 else 0x4000000
    link("raven-appx.dll", next_base[0], [*appx, *zlib], output / "raven-appx.def",
         ["-L" + str(output), *[f"-l{library}" for library in ("xmllite", "urlmon", "ole32", "bcrypt", "msvcrt", "kernel32", "uuid", "gcc")]])
    # raven-setupapi.dll: Raven's own setup API, which the runtime's setupapi.dll hands those exports to.
    own_setup = repo / "native/guest/raven-setupapi"
    setup_objects = []
    for path in [*sorted(own_setup.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("setup_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=c11", "-fno-builtin", "-D_WIN32_WINNT=0x600", "-DUNICODE", "-D_SETUPAPI_", "-D_SETUPAPI_VER=0x0600", "-DCONST_VTABLE",
             "-I" + str(own_setup), path, "-o", target])
        setup_objects.append(target)
    defined = decorated(setup_objects)
    def setup_export(entry):
        name, _, implementation = entry.partition("=")
        words = "@" + str(defined[implementation or name]) if x86 and defined[implementation or name] is not None else ""
        return f" {name}={implementation}{words}\n" if implementation else f" {name}{words}\n"
    (output / "raven-setupapi.def").write_text("LIBRARY raven-setupapi.dll\nEXPORTS\n" + "".join(setup_export(entry) for entry in (own_setup / "exports.txt").read_text().split()))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "raven-setupapi.dll", *setup_objects, output / "raven-setupapi.def", "-lole32", "-lshell32", "-lversion", "-ladvapi32", "-luser32", "-lkernel32", "-luuid", "-lgcc"])
    # advpack.dll: Raven's own, over that setup API. Its export order gives the ordinals old callers import by.
    own_advpack = repo / "native/guest/raven-advpack"
    advpack_objects = []
    for path in [*sorted(own_advpack.glob("*.c")), repo / "native/guest/raven-comctl32/crt.c"]:
        target = objects / ("advpack_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=c11", "-fno-builtin", "-D_WIN32_WINNT=0x600", "-DUNICODE", "-I" + str(own_advpack), path, "-o", target])
        advpack_objects.append(target)
    defined = decorated(advpack_objects)
    def advpack_export(ordinal, entry):
        name, _, implementation = entry.partition("=")
        words = "@" + str(defined[implementation or name]) if x86 and defined[implementation or name] is not None else ""
        return f" {name}={implementation}{words} @{ordinal}\n" if implementation else f" {name}{words} @{ordinal}\n"
    (output / "advpack.def").write_text("LIBRARY advpack.dll\nEXPORTS\n" + "".join(advpack_export(ordinal, entry) for ordinal, entry in enumerate((own_advpack / "exports.txt").read_text().split(), 1)))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "advpack.dll", *advpack_objects, output / "advpack.def", "-lsetupapi", "-lole32", "-lshell32", "-lversion", "-ladvapi32", "-luser32", "-lkernel32", "-luuid", "-lgcc"])
    # mspatcha.dll: Raven's own, applying the delta patches of Windows Installer patches.
    own_mspatcha = repo / "native/guest/raven-mspatcha"
    mspatcha_objects = []
    for path in sorted(own_mspatcha.glob("*.c")):
        target = objects / ("mspatcha_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=c11", "-fno-builtin", "-D_WIN32_WINNT=0x600", "-DUNICODE", "-I" + str(own_mspatcha), path, "-o", target])
        mspatcha_objects.append(target)
    defined = decorated(mspatcha_objects)
    (output / "mspatcha.def").write_text("LIBRARY mspatcha.dll\nEXPORTS\n" + "".join(
        f" {name}@{defined[name]}\n" if x86 and defined[name] is not None else f" {name}\n" for name in (own_mspatcha / "exports.txt").read_text().split()))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "mspatcha.dll", *mspatcha_objects, output / "mspatcha.def", "-lmsvcrt", "-lkernel32", "-lgcc"])
    # vbscript.dll: Raven's own VBScript engine, an Active Scripting server.
    own_vbscript = repo / "native/guest/raven-vbscript"
    vbscript_objects = []
    for path in sorted(own_vbscript.glob("*.c")):
        target = objects / ("vbscript_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=c11", "-fno-builtin", "-D_WIN32_WINNT=0x600", "-DUNICODE", "-I" + str(own_vbscript), path, "-o", target])
        vbscript_objects.append(target)
    defined = decorated(vbscript_objects)
    (output / "vbscript.def").write_text("LIBRARY vbscript.dll\nEXPORTS\n" + "".join(
        f" {name}@{defined[name]} PRIVATE\n" if x86 and defined[name] is not None else f" {name} PRIVATE\n" for name in (own_vbscript / "exports.txt").read_text().split()))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "vbscript.dll", *vbscript_objects, output / "vbscript.def", "-lole32", "-loleaut32", "-luser32", "-luuid", "-lmsvcrt",
         "-lkernel32", "-lgcc"])
    # scrrun.dll: Raven's own scripting runtime, the Dictionary and FileSystemObject classes.
    own_scrrun = repo / "native/guest/raven-scrrun"
    scrrun_objects = []
    for path in sorted(own_scrrun.glob("*.c")):
        target = objects / ("scrrun_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=c11", "-fno-builtin", "-D_WIN32_WINNT=0x600", "-DUNICODE", "-I" + str(own_scrrun), path, "-o", target])
        scrrun_objects.append(target)
    defined = decorated(scrrun_objects)
    (output / "scrrun.def").write_text("LIBRARY scrrun.dll\nEXPORTS\n" + "".join(
        f" {name}@{defined[name]} PRIVATE\n" if x86 and defined[name] is not None else f" {name} PRIVATE\n" for name in (own_scrrun / "exports.txt").read_text().split()))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "scrrun.dll", *scrrun_objects, output / "scrrun.def", "-lole32", "-loleaut32", "-luser32", "-lversion", "-luuid", "-lmsvcrt",
         "-lkernel32", "-lgcc"])
    # wshom.ocx: Raven's own script host object model, WScript.Shell and WScript.Network, on the scripting runtime's objects and streams.
    own_wshom = repo / "native/guest/raven-wshom"
    wshom_objects = []
    for path in [*sorted(own_wshom.glob("*.c")), own_scrrun / "object.c", own_scrrun / "textstream.c"]:
        target = objects / ("wshom_" + path.parent.name + "_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=c11", "-fno-builtin", "-D_WIN32_WINNT=0x600", "-DUNICODE", "-I" + str(own_wshom), path, "-o", target])
        wshom_objects.append(target)
    defined = decorated(wshom_objects)
    (output / "wshom.def").write_text("LIBRARY wshom.ocx\nEXPORTS\n" + "".join(
        f" {name}@{defined[name]} PRIVATE\n" if x86 and defined[name] is not None else f" {name} PRIVATE\n" for name in (own_wshom / "exports.txt").read_text().split()))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "wshom.ocx", *wshom_objects, output / "wshom.def", "-lole32", "-loleaut32", "-luser32", "-lshell32", "-ladvapi32", "-luuid",
         "-lmsvcrt", "-lkernel32", "-lgcc"])
    # jscript.dll: Raven's own JScript engine, an Active Scripting server; its regular expressions are the VBScript engine's matcher.
    own_jscript = repo / "native/guest/raven-jscript"
    jscript_objects = []
    for path in [*sorted(own_jscript.glob("*.c")), own_vbscript / "regex.c"]:
        target = objects / ("jscript_" + path.parent.name + "_" + path.stem + ".o")
        run([triple + "-gcc", "-c", "-O2", "-std=c11", "-fno-builtin", "-D_WIN32_WINNT=0x600", "-DUNICODE", "-I" + str(own_jscript), path, "-o", target])
        jscript_objects.append(target)
    defined = decorated(jscript_objects)
    (output / "jscript.def").write_text("LIBRARY jscript.dll\nEXPORTS\n" + "".join(
        f" {name}@{defined[name]} PRIVATE\n" if x86 and defined[name] is not None else f" {name} PRIVATE\n" for name in (own_jscript / "exports.txt").read_text().split()))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{'_DllMain@12' if x86 else 'DllMain'},--image-base,{next_base[0]:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "jscript.dll", *jscript_objects, output / "jscript.def", "-lole32", "-loleaut32", "-luser32", "-luuid", "-lmsvcrt",
         "-lkernel32", "-lgcc"])
    notices = {}
    for name, release in UPSTREAM.items():
        for notice in release["notices"]:
            relative = f"{name}-{release['version']}/{notice}"
            target = output / "licenses" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(upstream_sources[name] / notice, target)
            notices[relative] = hashlib.sha256(target.read_bytes()).hexdigest()
    result = dict(upstream={name: dict(version=release["version"], sha256=release["sha256"]) for name, release in UPSTREAM.items()}, arch=args.arch,
                  commands=records, license_notices=notices,
                  component_sha256={name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in COMPONENTS},
                  storage_exports=exported,
                  patches={patch.name: hashlib.sha256(patch.read_bytes()).hexdigest() for patch in patches})
    (output / "provenance.json").write_text(json.dumps(result, indent=2) + "\n")
    for name in COMPONENTS:
        print(output / name)


if __name__ == "__main__":
    main()
