#!/usr/bin/env python3
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

from component_licenses import mark_patched_sources, preserve_notices


# The ReactOS revision the common controls component is built from: one archive serves both.
REVISION = "dacf2a6c47fa562799f9c8a41ab6d5c558556558"
ARCHIVE_SHA256 = "5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69"
PREFIXES = ("sdk/include/", "sdk/lib/pseh/", "sdk/tools/hpp/", "sdk/tools/spec2def/", "sdk/lib/3rdparty/zlib/", "sdk/lib/3rdparty/libwine/",
            "dll/win32/cabinet/", "dll/win32/msi/", "dll/win32/sxs/", "dll/win32/ole32/", "dll/win32/coml2/",
            "base/system/msiexec/", "dll/win32/rpcrt4/", "sdk/lib/3rdparty/libxml2/", "sdk/lib/3rdparty/libwin-iconv/", "dll/3rdparty/libxslt/",
            "dll/win32/msxml3/", "dll/win32/stdole2.tlb/", "base/system/regsvr32/",
            "dll/win32/atl/", "dll/win32/atl100/", "dll/win32/oleaut32/", "dll/3rdparty/libpng/", "dll/3rdparty/libjpeg/", "dll/3rdparty/libtiff/",
            "dll/win32/propsys/", "dll/win32/mlang/", "dll/win32/windowscodecs/", "dll/win32/gdiplus/", "sdk/lib/getopt/", "dll/win32/urlmon/", "dll/win32/combase/", "dll/win32/riched20/", "dll/win32/msvfw32/", "dll/win32/advpack/", "dll/win32/setupapi/", "dll/win32/kernel32/wine/lzexpand.c")
# Interface headers the installer sources include, generated from the public IDL files with what they import.
IDLS = ("objidlbase", "commoncontrols", "fusion", "msxml2", "oaidl", "ocidl", "objidl", "unknwn", "wtypes", "propidl", "urlmon", "shobjidl", "servprov", "docobj", "oleidl",
        "exdisp", "activscp", "shldisp", "mshtml", "xmldom", "imnact", "msctf", "shtypes", "oleacc", "comcat", "mscoree", "wincodec", "dispex",
        "atliface", "winsxs", "msxml6", "msxml", "xmldso", "objsafe", "mshtmhst", "mshtmdid", "httprequest", "xmlparser", "asptlb", "perhist", "hlink", "mimeinfo", "mlang", "wincodec", "wincodecsdk", "propsys", "structuredquery", "dxgiformat", "dxgicommon", "dxgitype")
ZLIB = ("adler32.c", "crc32.c", "deflate.c", "inflate.c", "inffast.c", "inftrees.c", "trees.c", "zutil.c")
# The structured storage of ole32 and its base layer, coml2: compound files, their streams and property sets; and the bind contexts monikers
# are bound with and the streams over global memory.
STORAGE = ("ole32/bindctx.c", "combase/hglobalstream.c", "ole32/storage32.c", "ole32/stg_prop.c", "ole32/stg_stream.c", "ole32/filelockbytes.c", "ole32/dictionary.c",
           "coml2/memlockbytes.c", "coml2/stg_prop.c", "coml2/storage32.c")
COMPONENTS = ("cabinet.dll", "sxs.dll", "rpcrt4.dll", "msi.dll", "raven-storage.dll", "msiexec.exe", "libxslt.dll", "msxml3.dll", "regsvr32.exe", "atl100.dll", "oleaut32.dll", "libpng.dll", "libjpeg.dll",
              "libtiff.dll", "propsys.dll", "mlang.dll", "windowscodecs.dll", "gdiplus.dll", "urlmon.dll", "riched20.dll", "msftedit.dll", "msvfw32.dll", "advpack.dll", "raven-setupapi.dll")


def main():
    parser = argparse.ArgumentParser(description="Build the pinned ReactOS Windows Installer components: msi, cabinet, sxs and structured storage")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--arch", choices=("x64", "x86"), default="x64", help="build the 64-bit components, or the 32-bit ones for PE32 programs")
    args = parser.parse_args()
    x86 = args.arch == "x86"
    triple = "i686-w64-mingw32" if x86 else "x86_64-w64-mingw32"
    spec_arch = "-a=i386" if x86 else "-a=x86_64"
    entry_point = "_DllMain@12" if x86 else "DllMain"
    link_extra = ["-Wl,--kill-at"] if x86 else []
    bases = dict(cabinet=0x1e000000, sxs=0x1e200000, rpcrt4=0x1d800000, msi=0x1e400000, storage=0x1ec00000, xslt=0x1d400000, msxml=0x1d000000, atl=0x1cc00000, oleaut=0x1c800000) if x86 else dict(
        cabinet=0x1e0000000, sxs=0x1e2000000, rpcrt4=0x1d8000000, msi=0x1e4000000, storage=0x1ec000000, xslt=0x1d4000000, msxml=0x1d0000000, atl=0x1cc000000, oleaut=0x1c8000000)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(output), TMP=str(output), TEMP=str(output))
    repo = Path(__file__).resolve().parents[1]
    archive = args.archive.resolve() if args.archive else output / "reactos-source.tar.gz"
    if not archive.exists():
        urllib.request.urlretrieve(f"https://codeload.github.com/reactos/reactos/tar.gz/{REVISION}", archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise RuntimeError("ReactOS source archive checksum differs")
    source = output / "source"
    shutil.rmtree(source, ignore_errors=True)
    with tarfile.open(archive) as upstream:
        for member in upstream:
            parts = Path(member.name).parts[1:]
            name = "/".join(parts)
            if not member.isfile() or not (name.startswith(PREFIXES) or ("/" not in name and name.startswith("COPYING"))):
                continue
            if not parts or ".." in parts or Path(name).is_absolute():
                raise RuntimeError("invalid upstream archive path")
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(upstream.extractfile(member).read())
    for tool in ("clang", triple + "-gcc", "widl", "bison"):
        if not shutil.which(tool):
            raise RuntimeError(f"required installer build tool is missing: {tool}")
    records = []

    def run(command, cwd=None):
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
        records.append(dict(command=[str(part) for part in command], returncode=result.returncode))
        if result.returncode:
            raise RuntimeError(result.stderr or "installer build command failed: " + " ".join(map(str, command)))
        return result

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

    # Raven's corrections to upstream behavior are kept as reviewable patches and recorded in the provenance.
    support = repo / "native/upstream/reactos-installer"
    patches = sorted((support / "patches").glob("*.patch"))
    for patch in patches:
        run(["patch", "-p1", "--forward", "--silent", "-i", patch], source)
    mark_patched_sources(source, patches)
    generated = output / "generated"
    shutil.rmtree(generated, ignore_errors=True)
    (generated / "psdk").mkdir(parents=True)
    (generated / "reactos").mkdir()
    run(["clang", "-O2", "-w", source / "sdk/tools/hpp/hpp.c", "-o", output / "hpp"])
    run(["clang", "-O1", "-w", source / "sdk/tools/spec2def/spec2def.c", "-o", output / "spec2def"])
    for name in ("winnt", "ntdef", "devioctl"):
        run([output / "hpp", name + ".template.h", generated / "psdk" / (name + ".h")], source / "sdk/include/xdk")
    widl = ["widl", "-m32" if x86 else "-m64", "-D__REACTOS__", "-I", source / "sdk/include/psdk", "-I", source / "sdk/include", "-I", generated / "psdk"]
    idls = set()

    def idl(name):
        path = source / "sdk/include/psdk" / (name + ".idl")
        if name in idls or not path.exists():
            return
        idls.add(name)
        for dependency in re.findall(r'import\s+"([^"]+)\.idl"', path.read_text(errors="replace")):
            idl(dependency)
        run([*widl, "-h", "-H", generated / "psdk" / (name + ".h"), path])

    for name in IDLS:
        idl(name)
    # Every other public interface header too, as the components include many of them; one widl cannot read is left out.
    for path in sorted((source / "sdk/include/psdk").glob("*.idl")):
        try:
            idl(path.stem)
        except RuntimeError:
            pass
    # The version headers are configure-time templates upstream; the component pins their values.
    version = (source / "sdk/include/reactos/version.h.cmake").read_text()
    for key, value in (("KERNEL_VERSION_MAJOR", "0"), ("KERNEL_VERSION_MINOR", "4"), ("KERNEL_VERSION_PATCH_LEVEL", "17"), ("COPYRIGHT_YEAR", "2026")):
        version = version.replace("@" + key + "@", value)
    (generated / "version.h").write_text(version)
    (generated / "reactos/version.h").write_text(version)
    buildno = (source / "sdk/include/reactos/buildno.h.cmake").read_text()
    for key, value in (("KERNEL_VERSION_BUILD", "custom"), ("REVISION", REVISION[:7]), ("KERNEL_VERSION", "0.4.17-dev"), ("COMMIT_HASH", REVISION),
                       ("REACTOS_DLL_VERSION_MAJOR", "42"), ("DLL_VERSION_STR", "42.4.17-dev"), ("CMAKE_C_COMPILER_ID", "GNU"), ("CMAKE_C_COMPILER_VERSION", "13")):
        buildno = buildno.replace("@" + key + "@", value)
    (generated / "reactos/buildno.h").write_text(buildno)
    # The Wine-compat header overlay sits one directory below its psdk siblings upstream; Wine modules see it first.
    overlay = source / "sdk/include/reactos/wine"
    shutil.copytree(source / "sdk/include/wine", overlay, dirs_exist_ok=True)
    gcc_include = run([triple + "-gcc", "-print-file-name=include"]).stdout.strip()
    includes = [generated, generated / "psdk", source / "sdk/lib/pseh/include", source / "sdk/include/vcruntime", overlay, source / "sdk/include/psdk",
                source / "sdk/include/crt", source / "sdk/include/ndk", source / "sdk/include", source / "sdk/include/reactos", source / "sdk/include/ddk"]
    # A function the headers do not declare would return a truncated int: none is allowed. The sources use the old CRT names, which the
    # oldnames library supplies upstream.
    flags = ["-O2", "-nostdinc", "-isystem", gcc_include, "-fgnu89-inline", "-Werror=implicit-function-declaration", "-Werror=int-conversion",
             "-Dwcsdup=_wcsdup", "-Dwcsicmp=_wcsicmp", "-Dwcsnicmp=_wcsnicmp", "-Dwcsupr=_wcsupr", "-Dwcslwr=_wcslwr", "-Dstrdup=_strdup", "-Dstricmp=_stricmp", "-Dstrnicmp=_strnicmp",
             "-D_WIN32_WINNT=0x600", "-DWINVER=0x600", "-D_WIN32_IE=0x0A00", "-D__REACTOS__", "-DUNICODE", "-D_UNICODE",
             "-D_M_IX86" if x86 else "-D_M_AMD64", "-D__WINESRC__", "-D_WINE", *([] if x86 else ["-D__ROS_LONG64__"]),
             # 32-bit handlers read the exception through the frame based PSEH of ReactOS, as the guarded blocks themselves do.
             *(["-D_exception_code()=_SEH2_GetExceptionCode()", "-D_exception_info()=_SEH2_GetExceptionInformation()"] if x86 else []),
             f"-ffile-prefix-map={source}=reactos", f"-ffile-prefix-map={repo}=raven", f"-ffile-prefix-map={output}=component"]
    objects = output / "objects"
    shutil.rmtree(objects, ignore_errors=True)
    objects.mkdir()

    def compile(path, name, extra=(), wine_module=True):
        target = objects / (name + ".o")
        paths = includes if wine_module else [path for path in includes if path != overlay]
        run([triple + "-gcc", "-c", *flags, *extra, *["-I" + str(path) for path in paths], path, "-o", target])
        return target

    def link(name, base, object_files, definition, libraries, implib=None):
        run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{base:#x},--no-insert-timestamp", *link_extra,
             *([f"-Wl,--out-implib,{implib}"] if implib and not x86 else []), "-o", output / name, *object_files, definition, *libraries])
        # A 32-bit import library keeps the stdcall decorations its callers link against, for the plain names the library exports.
        if implib and x86:
            stdcall = output / (Path(name).stem + "-import.def")
            stdcall.write_text(re.sub(r"^(\s*)(\w+)=\w+(@\d+)", r"\1\2\3", Path(definition).read_text(), flags=re.M))
            run([triple + "-dlltool", "-d", stdcall, "-l", implib, "-k"])

    debug = compile(repo / "native/upstream/reactos-controls/debug.c", "debug")
    # 32-bit code registers its exception frames on the thread's chain: the frame based PSEH support of ReactOS is built into each component.
    pseh = []
    if x86:
        pseh_root = source / "sdk/lib/pseh"
        for name in ("framebased-gcchack.c", "framebased-gcchack-asm.S", "framebased.S", "../framebased.c"):
            target = objects / ("pseh_" + Path(name).name.replace(".", "_") + ".o")
            language = ["-x", "assembler-with-cpp"] if name.endswith(".S") else [*flags, *["-I" + str(path) for path in includes]]
            run([triple + "-gcc", "-c", *language, "-I" + str(pseh_root / "include"), "-I" + str(pseh_root / "include/pseh"), pseh_root / "i386" / name,
                 "-o", target])
            pseh.append(target)
        debug = [debug, *pseh]
    else:
        debug = [debug]
    # cabinet.dll: cabinet files, their compression and extraction, on zlib.
    zlib_include = source / "sdk/include/reactos/libs/zlib"
    zlib = [compile(source / "sdk/lib/3rdparty/zlib" / name, "zlib_" + Path(name).stem, ["-I" + str(zlib_include)]) for name in ZLIB]
    cabinet_root = source / "dll/win32/cabinet"
    with ThreadPoolExecutor(max_workers=2) as pool:
        cabinet = list(pool.map(lambda name: compile(cabinet_root / name, "cabinet_" + Path(name).stem,
                                                     ["-DHAVE_ZLIB", "-I" + str(cabinet_root), "-I" + str(zlib_include)]),
                                ("cabinet_main.c", "fci.c", "fdi.c", "stubs.c")))
    run([output / "spec2def", spec_arch, "-n=cabinet.dll", "-d=" + str(output / "cabinet.def"), cabinet_root / "cabinet.spec"])
    link("cabinet.dll", bases["cabinet"], [*zlib, *cabinet, *debug], output / "cabinet.def", ["-lmsvcrt", "-lkernel32", "-lntdll", "-lgcc"],
         output / "libcabinet.a")
    # sxs.dll: the side-by-side assembly cache.
    sxs_root = source / "dll/win32/sxs"
    run([output / "spec2def", spec_arch, "-n=sxs.dll", "-d=" + str(output / "sxs.def"), "-s=" + str(generated / "sxs_stubs.c"), sxs_root / "sxs.spec"])
    sxs = [compile(sxs_root / name, "sxs_" + Path(name).stem, ["-I" + str(sxs_root)]) for name in ("cache.c", "name.c", "sxs.c", "guid.c")]
    sxs.append(compile(generated / "sxs_stubs.c", "sxs_stubs"))
    link("sxs.dll", bases["sxs"], [*sxs, *debug], output / "sxs.def", ["-loleaut32", "-lole32", "-lmsvcrt", "-lkernel32", "-lntdll", "-luuid", "-lgcc"],
         output / "libsxs.a")
    compile(support / "support.c", "installer_support")
    # rpcrt4.dll: remote procedure calls, which the installer's custom action server speaks over local pipes, with the NDR engine.
    rpc_root = source / "dll/win32/rpcrt4"
    run([*widl, "-I", rpc_root, "-c", "-C", generated / "epm_c.c", "-h", "-H", generated / "epm_c.h", rpc_root / "epm.idl"])
    run([*widl, "-I", rpc_root, "-Oicf", "-p", "-P", generated / "ndr_types_p.c", "-h", "-H", generated / "ndr_types.h", rpc_root / "ndr_types.idl"])
    run([*widl, "--dlldata-only", "--dlldata=" + str(generated / "proxy.dlldata.c"), "ndr_types"])
    run([output / "spec2def", spec_arch, "-n=rpcrt4.dll", "-d=" + str(output / "rpcrt4.def"), "-s=" + str(generated / "rpcrt4_stubs.c"), rpc_root / "rpcrt4.spec"])
    rpc_flags = ["-D_RPCRT4_", "-DCOM_NO_WINDOWS_H", "-D_CRT_NON_CONFORMING_SWPRINTFS", "-DMSWMSG", "-I" + str(rpc_root)]
    rpc_sources = [rpc_root / name for name in ("cproxy.c", "cpsf.c", "cstub.c", "ndr_clientserver.c", "ndr_contexthandle.c", "ndr_es.c",
                   "ndr_fullpointer.c", "ndr_marshall.c", "ndr_ole.c", "ndr_stubless.c", "ros_extra.c", "rpc_assoc.c", "rpc_async.c", "rpc_binding.c",
                   "rpc_epmap.c", "rpc_message.c", "rpcrt4_main.c", "rpc_server.c", "rpc_transport.c", "thunks.c", "unix_func.c", "ndr_typelib.c")] + [
                   generated / name for name in ("epm_c.c", "ndr_types_p.c", "proxy.dlldata.c", "rpcrt4_stubs.c")]
    with ThreadPoolExecutor(max_workers=2) as pool:
        rpc = list(pool.map(lambda path: compile(path, "rpcrt4_" + path.stem.replace(".", "_"), rpc_flags), rpc_sources))
    rpc.append(compile(source / "sdk/lib/3rdparty/libwine/register.c", "rpcrt4_register", ["-I" + str(rpc_root)]))
    link("rpcrt4.dll", bases["rpcrt4"], [*rpc, objects / "installer_support.o", *debug], output / "rpcrt4.def",
         [*[f"-l{library}" for library in ("advapi32", "ws2_32", "iphlpapi", "wininet", "secur32", "user32", "ole32", "oleaut32", "msvcrt",
          "kernel32", "ntdll", "uuid", "gcc", "kernel32", "msvcrt")]], output / "librpcrt4.a")
    # msi.dll: the Windows Installer engine, its SQL and condition grammars, and the RPC stubs of its custom action server.
    msi_root = source / "dll/win32/msi"
    run(["bison", "-o", generated / "cond.tab.c", "-d", msi_root / "cond.y"])
    run(["bison", "-o", generated / "sql.tab.c", "-d", msi_root / "sql.y"])
    msi_widl = [*widl, "-I", msi_root, "--prefix-server=s_"]
    for name in ("msiserver", "winemsi"):
        run([*msi_widl, "-h", "-H", generated / (name + ".h"), msi_root / (name + ".idl")])
        run([*msi_widl, "-u", "-U", generated / (name + "_i.c"), msi_root / (name + ".idl")])
    run([*msi_widl, "-c", "-C", generated / "winemsi_c.c", msi_root / "winemsi.idl"])
    run([*msi_widl, "-s", "-S", generated / "winemsi_s.c", msi_root / "winemsi.idl"])
    run([output / "spec2def", spec_arch, "-n=msi.dll", "-d=" + str(output / "msi.def"), "-s=" + str(generated / "msi_stubs.c"), msi_root / "msi.spec"])
    msi_sources = sorted(msi_root.glob("*.c")) + [generated / name for name in ("cond.tab.c", "sql.tab.c", "msiserver_i.c", "winemsi_c.c",
                                                                                 "winemsi_s.c", "msi_stubs.c")]
    msi_flags = ["-DMSIRUNMODE=MSIRUNMODE_T", "-I" + str(msi_root)]
    with ThreadPoolExecutor(max_workers=2) as pool:
        msi = list(pool.map(lambda path: compile(path, "msi_" + path.stem, msi_flags, wine_module=False), msi_sources))
    msi.append(compile(source / "sdk/lib/3rdparty/libwine/register.c", "wine_register", ["-I" + str(msi_root)]))
    # Its strings, bitmaps, registration script and version: installers check that version before they run.
    wrapper = output / "resource-preprocessor.sh"
    wrapper.write_text("#!/bin/sh\nexec " + triple + "-gcc -E -xc -DRC_INVOKED -nostdinc -isystem " + gcc_include + " " +
                       " ".join("-I" + str(path) for path in [msi_root, *includes, generated / "reactos"] if path != overlay) + ' -w "$@"\n')
    wrapper.chmod(0o755)
    run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-DLANGUAGE_EN_US", "-D__REACTOS__", "-D__WINESRC__",
         "-D_M_IX86" if x86 else "-D_M_AMD64", "-I" + str(msi_root), "-i", msi_root / "msi.rc", "-o", objects / "msi.res.o"])
    msi.append(objects / "msi.res.o")
    msi.append(objects / "installer_support.o")
    msi.append(compile(source / "sdk/lib/3rdparty/libwine/dll_register.c", "msi_dll_register", ["-I" + str(msi_root)]))
    link("msi.dll", bases["msi"], [*msi, *debug], output / "msi.def",
         ["-L" + str(output), "-lsxs", "-lcabinet", "-lrpcrt4", *[f"-l{library}" for library in ("advapi32", "comctl32", "gdi32", "ole32", "oleaut32", "shell32",
          "shlwapi", "urlmon", "user32", "version", "wininet", "imagehlp", "msvcrt", "kernel32", "ntdll", "odbccp32", "crypt32",
          "wintrust", "uuid", "gcc")]])
    # raven-storage.dll: what ole32 exports of structured storage, which the runtime's ole32 hands to it.
    ole_root = source / "dll/win32/ole32"
    for name in ("dcom", "irot"):
        run([*widl, "-I", ole_root, "-h", "-H", generated / (name + ".h"), ole_root / (name + ".idl")])
    storage_flags = ["-D_OLE32_", "-DCOM_NO_WINDOWS_H", "-DENTRY_PREFIX=OLE32_", "-DWINE_REGISTER_DLL", "-I" + str(ole_root)]
    storage = [compile(source / "dll/win32" / name, "storage_" + name.replace("/", "_")[:-2], [*storage_flags, "-I" + str((source / "dll/win32" / name).parent)])
               for name in STORAGE]
    storage.append(compile(support / "storage.c", "storage_support"))
    run([output / "spec2def", spec_arch, "-n=ole32.dll", "-d=" + str(output / "ole32.def"), ole_root / "ole32.spec"])
    public = {match.group(1) for match in re.finditer(r"^\s*([A-Za-z_][A-Za-z0-9_]*)(?:\s|=|@|$)", (output / "ole32.def").read_text(), re.M)}
    defined = decorated(storage)
    exported = sorted((public & set(defined)) - {"EXPORTS", "NAME", "LIBRARY"})
    (output / "raven-storage.def").write_text("NAME raven-storage.dll\nEXPORTS\n" + "".join(
        " " + name + ("@" + str(defined[name]) if x86 and defined[name] is not None else "") + "\n" for name in exported))
    link("raven-storage.dll", bases["storage"], [*storage, *debug], output / "raven-storage.def",
         ["-lole32", "-loleaut32", "-ladvapi32", "-luser32", "-lmsvcrt", "-lkernel32", "-lntdll", "-luuid", "-lgcc"])
    # msiexec.exe: the command line of the installer, which installers and the shell start for packages.
    msiexec_root = source / "base/system/msiexec"
    msiexec = [compile(msiexec_root / name, "msiexec_" + Path(name).stem, ["-I" + str(msiexec_root)], wine_module=False)
               for name in ("msiexec.c", "service.c")]
    msiexec.append(compile(support / "entry.c", "msiexec_entry"))
    run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-DLANGUAGE_EN_US", "-D__REACTOS__",
         "-D_M_IX86" if x86 else "-D_M_AMD64", "-I" + str(msiexec_root), "-i", msiexec_root / "rsrc.rc", "-o", objects / "msiexec.res.o"])
    run([triple + "-gcc", "-nostdlib", "-mwindows", f"-Wl,--entry,{'_raven_program_entry@0' if x86 else 'raven_program_entry'},--no-insert-timestamp",
         "-o", output / "msiexec.exe", *msiexec, objects / "msiexec.res.o", objects / "installer_support.o", *debug, "-L" + str(output),
         *[f"-l{library}" for library in ("msi", "user32", "advapi32", "ole32", "comctl32", "msvcrt", "kernel32", "ntdll", "uuid", "gcc")]])
    # msxml3.dll: the XML document object model installers and their bootstrappers parse their manifests with, on libxml2 and libxslt.
    xml_root = source / "sdk/lib/3rdparty/libxml2"
    iconv_root = source / "sdk/lib/3rdparty/libwin-iconv"
    xml_flags = ["-U__WINESRC__", "-U_WINE", "-D_CRT_DECLARE_NONSTDC_NAMES=1", "-D__MINGW32__", "-D_DLL", "-D__USE_CRTIMP", "-DLIBXML_STATIC", "-D_REENTRANT", "-D_MBCS", "-D_WINDOWS", "-DWIN32", "-DHAVE_CONFIG_H", "-DICONV_CONST=const",
                 "-I" + str(xml_root / "include"), "-I" + str(xml_root), "-I" + str(iconv_root), "-I" + str(zlib_include)]
    # The C runtime's mathematics, which mingw's msvcrt import library would bring in as static code of its own.
    (output / "crtmath.def").write_text("LIBRARY msvcrt.dll\nEXPORTS\n" + "".join(" " + name + "\n" for name in
                                        ("floor", "ceil", "fabs", "fmod", "log", "log10", "exp", "pow", "sqrt", "atan", "atan2", "tan",
                                         "floorf", "ceilf", "_hypot")))
    run([triple + "-dlltool", "-d", output / "crtmath.def", "-l", output / "libcrtmath.a"])
    xml = [compile(iconv_root / "win_iconv.c", "iconv_win_iconv", xml_flags, wine_module=False)]
    xml_sources = sorted(path for path in xml_root.glob("*.c"))
    with ThreadPoolExecutor(max_workers=2) as pool:
        xml += list(pool.map(lambda path: compile(path, "libxml2_" + path.stem, xml_flags, wine_module=False), xml_sources))
    xslt_root = source / "dll/3rdparty/libxslt"
    xslt_flags = [*xml_flags, "-DHAVE_WIN32_THREADS", "-I" + str(xslt_root), "-I" + str(source / "sdk/include/reactos/libs/libxslt"), "-I" + str(source / "sdk/include/reactos/libs")]
    with ThreadPoolExecutor(max_workers=2) as pool:
        xslt = list(pool.map(lambda path: compile(path, "libxslt_" + path.stem, xslt_flags, wine_module=False), sorted(xslt_root.glob("*.c"))))
    run([output / "spec2def", spec_arch, "-n=libxslt.dll", "-d=" + str(output / "libxslt.def"), xslt_root / "libxslt.spec"])
    link("libxslt.dll", bases["xslt"], [*xslt, *xml, *zlib, *debug], output / "libxslt.def", ["-L" + str(output), "-lcrtmath", "-lws2_32", "-lmsvcrt", "-lkernel32", "-lgcc", "-lkernel32", "-lmsvcrt"],
         output / "liblibxslt.a")
    msxml_root = source / "dll/win32/msxml3"
    run(["bison", "-p", "xslpattern_", "-o", generated / "xslpattern.tab.c", "-d", msxml_root / "xslpattern.y"])
    # The scanner reads the buffer it is given: no standard stream, whose data msvcrt exports and the runtime's C library does not.
    run(["flex", "--nostdinit", "-o", generated / "xslpattern.yy.c", msxml_root / "xslpattern.l"])
    for name in ("xmlparser", "msxml3_v1"):
        run([*widl, "-I", msxml_root, "-h", "-H", generated / (name + ".h"), msxml_root / (name + ".idl")])
    run([output / "spec2def", spec_arch, "-n=msxml3.dll", "-d=" + str(output / "msxml3.def"), "-s=" + str(generated / "msxml3_stubs.c"), msxml_root / "msxml3.spec"])
    msxml_flags = ["-D_DLL", "-D_WINE", "-DLIBXML_STATIC", "-DCOM_NO_WINDOWS_H", "-DHAVE_LIBXML2", "-DHAVE_LIBXSLT", "-I" + str(msxml_root), "-I" + str(xml_root / "include"),
                   "-I" + str(source / "sdk/include/reactos/libs/libxslt"), "-I" + str(source / "sdk/include/reactos/libs"), "-I" + str(xslt_root)]
    msxml_sources = sorted(msxml_root.glob("*.c")) + [generated / name for name in ("xslpattern.tab.c", "xslpattern.yy.c", "msxml3_stubs.c")]
    with ThreadPoolExecutor(max_workers=2) as pool:
        msxml = list(pool.map(lambda path: compile(path, "msxml3_" + path.stem.replace(".", "_"), msxml_flags), msxml_sources))
    msxml.append(compile(source / "sdk/lib/3rdparty/libwine/register.c", "msxml3_register", ["-I" + str(msxml_root)]))
    msxml.append(compile(source / "sdk/lib/3rdparty/libwine/dll_register.c", "msxml3_dll_register", ["-I" + str(msxml_root)]))
    # Its registration scripts and type library, which DllRegisterServer writes into the registry; the library imports stdole2's.
    run([*widl, "-t", "-T", generated / "stdole2.tlb", source / "dll/win32/stdole2.tlb/std_ole_v2.idl"])
    run([*widl, "-I", msxml_root, "-L", generated, "-t", "-T", generated / "msxml3_v1.tlb", msxml_root / "msxml3_v1.idl"])
    run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-DLANGUAGE_EN_US", "-D__REACTOS__", "-D__WINESRC__",
         "-D_M_IX86" if x86 else "-D_M_AMD64", "-I" + str(msxml_root), "-I" + str(generated), "-i", msxml_root / "version.rc", "-o", objects / "msxml3.res.o"])
    msxml.append(objects / "msxml3.res.o")
    link("msxml3.dll", bases["msxml"], [*msxml, *xml, *zlib, objects / "installer_support.o", *debug], output / "msxml3.def",
         ["-Wl,--disable-auto-import", "-L" + str(output), "-llibxslt", "-lcrtmath", *[f"-l{library}" for library in ("urlmon", "ws2_32", "shlwapi", "oleaut32", "ole32", "user32", "msvcrt",
          "kernel32", "ntdll", "uuid", "gcc", "kernel32", "msvcrt")]])
    # atl100.dll: the ATL registrar, which reads the registration scripts libraries register their classes with.
    atl_root, atl100_root = source / "dll/win32/atl", source / "dll/win32/atl100"
    run([*widl, "-I", atl_root, "-h", "-H", generated / "atl_classes.h", atl_root / "atl_classes.idl"])
    run([output / "spec2def", spec_arch, "-n=atl100.dll", "-d=" + str(output / "atl100.def"), "-s=" + str(generated / "atl100_stubs.c"), atl100_root / "atl100.spec"])
    atl_flags = ["-D_ATL_VER=_ATL_VER_100", "-I" + str(atl100_root), "-I" + str(atl_root)]
    atl = [compile(atl_root / name, "atl100_" + Path(name).stem, atl_flags) for name in ("atl.c", "atl_ax.c", "registrar.c")]
    atl.append(compile(generated / "atl100_stubs.c", "atl100_stubs"))
    run([*widl, "-u", "-U", generated / "atliface_i.c", source / "sdk/include/psdk/atliface.idl"])
    atl.append(compile(generated / "atliface_i.c", "atl100_atliface_i"))
    link("atl100.dll", bases["atl"], [*atl, objects / "installer_support.o", *debug], output / "atl100.def",
         [*[f"-l{library}" for library in ("ole32", "oleaut32", "user32", "gdi32", "advapi32", "shlwapi", "msvcrt", "kernel32", "ntdll", "uuid",
          "gcc", "kernel32", "msvcrt")]])
    # oleaut32.dll: automation, variants, safe arrays and type libraries, with the proxies of its interfaces.
    oleaut_root = source / "dll/win32/oleaut32"
    oleaut_widl = [*widl, "-I", oleaut_root]
    for name in ("oleaut32_oaidl", "oleaut32_ocidl"):
        run([*oleaut_widl, "-h", "-H", generated / (name + ".h"), oleaut_root / (name + ".idl")])
        run([*oleaut_widl, "-p", "-P", generated / (name + "_p.c"), oleaut_root / (name + ".idl")])
    run([*widl, "--dlldata-only", "--dlldata=" + str(generated / "oleaut32_dlldata.c"), "oleaut32_oaidl", "oleaut32_ocidl"])
    run([output / "spec2def", spec_arch, "-n=oleaut32.dll", "-d=" + str(output / "oleaut32.def"), "-s=" + str(generated / "oleaut32_stubs.c"),
         oleaut_root / "oleaut32.spec"])
    oleaut_flags = ["-D_OLEAUT32_", "-DPROXY_DELEGATION", "-DWINE_REGISTER_DLL", "-DENTRY_PREFIX=OLEAUTPS_", "-DPROXY_CLSID=CLSID_PSFactoryBuffer",
                    "-I" + str(oleaut_root)]
    oleaut_sources = [oleaut_root / name for name in ("connpt.c", "dispatch.c", "hash.c", "oleaut.c", "olefont.c", "olepicture.c", "olepropframe.c",
                      "recinfo.c", "safearray.c", "thunks.c", "typelib.c", "usrmarshal.c", "varformat.c", "variant.c", "vartype.c")] + [
                      generated / name for name in ("oleaut32_stubs.c", "oleaut32_dlldata.c", "oleaut32_oaidl_p.c", "oleaut32_ocidl_p.c")]
    with ThreadPoolExecutor(max_workers=2) as pool:
        oleaut = list(pool.map(lambda path: compile(path, "oleaut32_" + path.stem, oleaut_flags), oleaut_sources))
    oleaut.append(compile(source / "sdk/lib/3rdparty/libwine/register.c", "oleaut32_register", ["-I" + str(oleaut_root)]))
    oleaut.append(compile(source / "sdk/lib/3rdparty/libwine/dll_canunload.c", "oleaut32_canunload", ["-I" + str(oleaut_root)]))
    run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-DLANGUAGE_EN_US", "-D__REACTOS__", "-D__WINESRC__",
         "-D_M_IX86" if x86 else "-D_M_AMD64", "-I" + str(oleaut_root), "-I" + str(generated), "-i", oleaut_root / "oleaut32.rc", "-o", objects / "oleaut32.res.o"])
    oleaut.append(objects / "oleaut32.res.o")
    # The imaging component is loaded when a picture first needs it, as the ReactOS build delay-loads it: programs that never decode a
    # picture do not load it and the libraries behind it.
    oleaut.append(compile(support / "codecs.c", "oleaut32_codecs"))
    link("oleaut32.dll", bases["oleaut"], [*oleaut, objects / "installer_support.o", *debug], output / "oleaut32.def",
         ["-L" + str(output), "-lrpcrt4", "-lcrtmath", *[f"-l{library}" for library in ("ole32", "user32", "gdi32", "advapi32", "comctl32", "urlmon",
          "msvcrt", "kernel32", "ntdll", "uuid", "gcc", "kernel32", "msvcrt")]])
    # The libraries below follow one pattern: the sources their ReactOS build lists, compiled with its definitions, a definition file from
    # their specification and the libraries they import. Their base addresses count down from below the ones above.
    next_base = [0x1c4000000 if not x86 else 0x1c400000]

    def listed_sources(root):
        text = (root / "CMakeLists.txt").read_text()
        names = []
        for block in re.findall(r"list\(APPEND SOURCE(.*?)\)", text, re.S)[:1]:
            names += [token for token in block.split() if token.endswith(".c") and not token.startswith(("#", "$"))]
        return [root / name for name in names]

    def library(name, root, extra_flags=(), libraries=(), wine_module=True, sources=None, objects_more=(), helpers=(), rc=None, interfaces=()):
        stem = Path(name).stem
        spec = root / (stem + ".spec") if (root / (stem + ".spec")).exists() else next(root.glob("*.spec"))
        run([output / "spec2def", spec_arch, "-n=" + name, "-d=" + str(output / (stem + ".def")), "-s=" + str(generated / (stem + "_stubs.c")), spec])
        flags_here = [*extra_flags, "-I" + str(root)]
        parts = sources if sources is not None else listed_sources(root)
        with ThreadPoolExecutor(max_workers=2) as pool:
            built = list(pool.map(lambda path: compile(path, stem + "_" + path.stem.replace(".", "_"), flags_here, wine_module), parts))
        built.append(compile(generated / (stem + "_stubs.c"), stem + "_spec_stubs", flags_here, wine_module))
        # The identifiers of the public interfaces it names, which mingw-w64's uuid library does not all have.
        for interface in interfaces:
            run([*widl, "-u", "-U", generated / (interface + "_i.c"), source / "sdk/include/psdk" / (interface + ".idl")])
            built.append(compile(generated / (interface + "_i.c"), stem + "_" + interface + "_i", flags_here, wine_module))
        for helper in helpers:
            built.append(compile(source / "sdk/lib/3rdparty/libwine" / helper, stem + "_" + Path(helper).stem, flags_here))
        if rc:
            run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-DLANGUAGE_EN_US", "-D__REACTOS__", "-D__WINESRC__",
                 "-D_M_IX86" if x86 else "-D_M_AMD64", "-I" + str(root), "-I" + str(generated), "-i", root / rc, "-o", objects / (stem + ".res.o")])
            built.append(objects / (stem + ".res.o"))
        next_base[0] -= 0x400000 if x86 else 0x4000000
        link(name, next_base[0], [*built, *objects_more, objects / "installer_support.o", *debug], output / (stem + ".def"),
             ["-L" + str(output), "-lcrtmath", *[f"-l{library}" for library in (*libraries, "msvcrt", "kernel32", "ntdll", "uuid", "gcc", "kernel32", "msvcrt")]],
             output / ("lib" + stem + ".a"))

    plain = ["-U__WINESRC__", "-U_WINE", "-D_CRT_DECLARE_NONSTDC_NAMES=1", "-D_DLL", "-DWIN32", "-D_WINDOWS"]
    entry_object = compile(support / "dllmain.c", "plain_dllmain")
    libs = source / "sdk/include/reactos/libs"
    library("libpng.dll", source / "dll/3rdparty/libpng", [*plain, "-DNDEBUG=", "-DPNG_BUILD_DLL", "-I" + str(libs / "libpng"), "-I" + str(zlib_include)],
            wine_module=False, objects_more=[*zlib, entry_object])
    library("libjpeg.dll", source / "dll/3rdparty/libjpeg", [*plain, "-D_MBCS", "-DJPEG_DLL", "-Dmain=mainptr", "-I" + str(libs / "libjpeg")], wine_module=False,
            objects_more=[entry_object])
    library("libtiff.dll", source / "dll/3rdparty/libtiff", [*plain, "-DNDEBUG=", "-DUSE_WIN32_FILEIO", "-DTIF_PLATFORM_CONSOLE", "-I" + str(libs / "libtiff"),
            "-I" + str(libs / "libjpeg"), "-I" + str(zlib_include)], ["libjpeg", "user32"], wine_module=False, objects_more=[*zlib, entry_object])
    registration = ("register.c", "dll_register.c", "dll_canunload.c")
    library("propsys.dll", source / "dll/win32/propsys", ["-D_PROPSYS_"], ["ole32", "oleaut32"], helpers=registration[:2], rc="propsys.rc")
    library("mlang.dll", source / "dll/win32/mlang", ["-DCOM_NO_WINDOWS_H", "-D_CRT_NON_CONFORMING_SWPRINTFS"], ["gdi32", "advapi32", "oleaut32"],
            helpers=registration[:2], rc="mlang.rc", interfaces=("mlang",))
    # windowscodecs.dll: the imaging component; its interfaces' proxies register through a script widl writes from the IDL.
    codecs_root = source / "dll/win32/windowscodecs"
    codecs_widl = [*widl, "-I", codecs_root]
    run([*codecs_widl, "-h", "-H", generated / "windowscodecs_wincodec.h", codecs_root / "windowscodecs_wincodec.idl"])
    run([*codecs_widl, "-p", "-P", generated / "windowscodecs_wincodec_p.c", codecs_root / "windowscodecs_wincodec.idl"])
    run([*codecs_widl, "-r", "-o", generated / "windowscodecs_wincodec_r.rgs", codecs_root / "windowscodecs_wincodec.idl"])
    run([*widl, "--dlldata-only", "--dlldata=" + str(generated / "windowscodecs_dlldata.c"), "windowscodecs_wincodec"])
    (generated / "windowscodecs_r.rc").write_text("1 WINE_REGISTRY windowscodecs_wincodec_r.rgs\n")
    run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-D__REACTOS__", "-D__WINESRC__", "-D_M_IX86" if x86 else "-D_M_AMD64",
         "-I" + str(generated), "-i", generated / "windowscodecs_r.rc", "-o", objects / "windowscodecs_r.res.o"])
    codecs_flags = ["-DENTRY_PREFIX=WIC_", "-DPROXY_DELEGATION", "-DWINE_REGISTER_DLL", "-I" + str(libs / "libpng"), "-I" + str(libs / "libjpeg"),
                    "-I" + str(libs / "libtiff"), "-I" + str(zlib_include)]
    library("windowscodecs.dll", codecs_root, codecs_flags, ["libjpeg", "libpng", "libtiff", "ole32", "oleaut32", "rpcrt4", "shlwapi", "user32", "gdi32",
            "advapi32", "propsys"], sources=[*listed_sources(codecs_root), codecs_root / "bitmap.c", codecs_root / "guid.c",
            generated / "windowscodecs_wincodec_p.c", generated / "windowscodecs_dlldata.c"], objects_more=[objects / "windowscodecs_r.res.o"],
            helpers=("register.c",), rc="version.rc")
    gdiplus_root = source / "dll/win32/gdiplus"
    library("gdiplus.dll", gdiplus_root, ["-D_USE_MATH_DEFINES", "-U_WIN32_WINNT", "-D_WIN32_WINNT=0x601", "-UWINVER", "-DWINVER=0x601"], ["mlang", "shlwapi", "oleaut32", "ole32", "user32", "gdi32", "windowscodecs"],
            sources=[*listed_sources(gdiplus_root), gdiplus_root / "guid.c"], rc="gdiplus.rc")
    # urlmon.dll: URLs and monikers, the protocols behind them and the interfaces' proxies, registered like the imaging component's.
    urlmon_root = source / "dll/win32/urlmon"
    urlmon_widl = [*widl, "-I", urlmon_root]
    run([*urlmon_widl, "-h", "-H", generated / "urlmon_urlmon.h", urlmon_root / "urlmon_urlmon.idl"])
    run([*urlmon_widl, "-p", "-P", generated / "urlmon_urlmon_p.c", urlmon_root / "urlmon_urlmon.idl"])
    run([*urlmon_widl, "-r", "-o", generated / "urlmon_urlmon_r.rgs", urlmon_root / "urlmon_urlmon.idl"])
    run([*widl, "--dlldata-only", "--dlldata=" + str(generated / "urlmon_dlldata.c"), "urlmon_urlmon"])
    # The setup information its resources carry, in the UTF-16 the ReactOS build converts it to.
    (generated / "urlmon_utf16.inf").write_bytes("\ufeff".encode("utf-16-le") + (urlmon_root / "urlmon.inf").read_text().encode("utf-16-le"))
    library("urlmon.dll", urlmon_root, ["-D_URLMON_", "-DENTRY_PREFIX=URLMON_", "-DPROXY_DELEGATION", "-DWINE_REGISTER_DLL", "-include",
            str(urlmon_root / "clsid.h")], ["rpcrt4", "propsys", "ole32", "oleaut32", "shlwapi", "shell32", "wininet", "user32", "advapi32"],
            sources=[*listed_sources(urlmon_root), generated / "urlmon_urlmon_p.c", generated / "urlmon_dlldata.c"], helpers=("register.c",), rc="urlmon.rc",
            objects_more=[compile(support / "setup.c", "urlmon_setup")])
    # riched20.dll: the rich edit control, with the type library of its text object model that its resources carry.
    riched_root = source / "dll/win32/riched20"
    run([*widl, "-I", riched_root, "-L", generated, "-t", "-T", generated / "riched_tom.tlb", riched_root / "riched_tom.idl"])
    library("riched20.dll", riched_root, [], ["ole32", "oleaut32", "usp10", "imm32", "user32", "gdi32"], rc="version.rc")
    # msftedit.dll: the newer rich edit library, which imports riched20.dll and passes every export on to it.
    (output / "msftedit.def").write_text("LIBRARY msftedit.dll\nEXPORTS\n" + "".join(
        f"\t{name} = riched20.{name}{' DATA' if name.startswith('IID_') else ''}\n" for name in ("CreateTextServices", "IID_IRichEditOle",
        "IID_IRichEditOleCallback", "IID_ITextHost", "IID_ITextHost2", "IID_ITextServices", "REExtendedRegisterClass", "RichEdit10ANSIWndProc",
        "RichEditANSIWndProc")))
    next_base[0] -= 0x400000 if x86 else 0x4000000
    link("msftedit.dll", next_base[0], [compile(support / "msftedit.c", "msftedit", plain, False)], output / "msftedit.def",
         ["-L" + str(output), *[f"-l{library}" for library in ("riched20", "kernel32", "gcc")]])
    # msvfw32.dll: video for Windows, which the animation control opens its clips through. Its English strings only: the shared ReactOS
    # resource script also names a manifest the ReactOS build generates.
    (generated / "msvfw32.rc").write_text('#include "msvideo_private.h"\n#pragma code_page(65001)\n#include "lang/msvfw32_En.rc"\n')
    library("msvfw32.dll", source / "dll/win32/msvfw32", [], ["winmm", "comctl32", "user32", "gdi32", "advapi32"], rc=generated / "msvfw32.rc")
    # advpack.dll: the setup helpers of Internet Explorer's time, which IExpress packages and DirectX setup ask whether the user is an
    # administrator and run INF files through.
    library("advpack.dll", source / "dll/win32/advpack", [], ["ole32", "setupapi", "version", "advapi32"], rc="advpack.rc")
    # raven-setupapi.dll: the setup API, which the runtime's setupapi.dll hands its setup information files, file queues and cabinets to;
    # device management reaches the plug and play service through the RPC client widl writes.
    setupapi_root = source / "dll/win32/setupapi"
    run([*widl, "-I", source / "sdk/include/reactos/idl", "-h", "-H", generated / "pnp_c.h", "-c", "-C", generated / "pnp_c.c",
         source / "sdk/include/reactos/idl/pnp.idl"])
    # Its icons and English strings: the shared resource script also names a manifest the ReactOS build generates.
    (generated / "setupapi.rc").write_text("#include <windef.h>\n#include <winuser.h>\n" + "".join(f'#include "{setupapi_root / name}"\n' for name in
                                           ("resource.h", "res/icons_res.rc")) + f'#pragma code_page(65001)\n#include "{setupapi_root / "lang/en-US.rc"}"\n')
    # The LZ expansion functions it copies files with are kernel32's on Windows; they are built into its image, with the public headers in
    # place of kernel32's private one.
    (generated / "lzexpand").mkdir(exist_ok=True)
    (generated / "lzexpand/k32.h").write_text("#include <stdarg.h>\n#include <string.h>\n#include <ctype.h>\n#include <windef.h>\n#include <winbase.h>\n"
                                              "#include <winnls.h>\n#include <stdio.h>\n#include <wine/debug.h>\n#include <wine/unicode.h>\nWINE_DEFAULT_DEBUG_CHANNEL(lzexpand);\n")
    # It is a ReactOS module, which reaches the native headers rather than Wine's.
    library("raven-setupapi.dll", setupapi_root, ["-U__WINESRC__", "-U_WINE", "-D_SETUPAPI_", "-D_CFGMGR32_", "-D_SETUPAPI_VER=0x0502", "-I" + str(source / "sdk/include/reactos/idl"),
            "-I" + str(generated)], ["rpcrt4", "gdi32", "comctl32", "advapi32", "user32", "version", "comdlg32", "shell32", "wintrust"],
            wine_module=False, sources=[*listed_sources(setupapi_root), generated / "pnp_c.c"], rc=generated / "setupapi.rc",
            objects_more=[compile(support / "printers.c", "setupapi_printers"),
                          compile(source / "dll/win32/kernel32/wine/lzexpand.c", "setupapi_lzexpand", ["-I" + str(generated / "lzexpand")],
                                  wine_module=False)])
    # regsvr32.exe: registers the classes a library serves, as installers and the runtime's own prefix setup run it.
    regsvr32_root = source / "base/system/regsvr32"
    regsvr32 = [compile(regsvr32_root / "regsvr32.c", "regsvr32_regsvr32", ["-DUNICODE", "-D_UNICODE", "-I" + str(regsvr32_root)], wine_module=False),
                compile(support / "entry_wide.c", "regsvr32_entry")]
    run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-DLANGUAGE_EN_US", "-D__REACTOS__",
         "-D_M_IX86" if x86 else "-D_M_AMD64", "-I" + str(regsvr32_root), "-i", regsvr32_root / "regsvr32.rc", "-o", objects / "regsvr32.res.o"])
    run([triple + "-gcc", "-nostdlib", "-mwindows", f"-Wl,--entry,{'_raven_program_entry@0' if x86 else 'raven_program_entry'},--no-insert-timestamp",
         "-o", output / "regsvr32.exe", *regsvr32, objects / "regsvr32.res.o", *debug,
         *[f"-l{library}" for library in ("user32", "shell32", "ole32", "msvcrt", "kernel32", "ntdll", "gcc", "kernel32")]])
    result = dict(revision=REVISION, archive_sha256=ARCHIVE_SHA256, arch=args.arch, commands=records,
                  license_notices=preserve_notices(source, output),
                  component_sha256={name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in COMPONENTS},
                  storage_exports=exported,
                  patches={patch.name: hashlib.sha256(patch.read_bytes()).hexdigest() for patch in patches},
                  source_hashes={path.relative_to(source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted(source.rglob("*")) if path.is_file() and not path.is_relative_to(overlay)})
    (output / "provenance.json").write_text(json.dumps(result, indent=2) + "\n")
    for name in COMPONENTS:
        print(output / name)


if __name__ == "__main__":
    main()
