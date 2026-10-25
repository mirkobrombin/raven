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


REVISION = "dacf2a6c47fa562799f9c8a41ab6d5c558556558"
ARCHIVE_SHA256 = "5373bbbb60541cb2a49e0acb53360e1218d4f5d56c31f9187b341b9fc16daa69"
PREFIXES = ("sdk/include/", "sdk/lib/pseh/", "sdk/tools/hpp/", "sdk/tools/spec2def/", "dll/win32/comctl32/",
            "win32ss/include/", "win32ss/user/user32/", "win32ss/user/rtl/")
# Upstream user-mode sources of the user32 component, and the exports they define that user32 programs import.
USER32_SOURCES = ("controls/button.c", "controls/combo.c", "controls/edit.c", "controls/listbox.c", "controls/static.c",
                  "windows/dialog.c", "windows/draw.c", "windows/font.c", "windows/messagebox.c", "windows/text.c")
IDLS = ("shtypes", "commoncontrols")


def main():
    parser = argparse.ArgumentParser(description="Build the pinned selective ReactOS common controls component")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--arch", choices=("x64", "x86"), default="x64", help="build the 64-bit components, or the 32-bit ones for PE32 programs")
    parser.add_argument("--components", default="all", help="comma separated: comctl32, user32, shell, crt (default: all that exist for the architecture)")
    args = parser.parse_args()
    x86 = args.arch == "x86"
    triple = "i686-w64-mingw32" if x86 else "x86_64-w64-mingw32"
    cpu_define = "-D_M_IX86" if x86 else "-D_M_AMD64"
    long_defines = [] if x86 else ["-D__ROS_LONG64__"]
    spec_arch = "-a=i386" if x86 else "-a=x86_64"
    # PE32 libraries are stdcall decorated at link time and export plain names; the entry point is _DllMain@12.
    entry_point = "_DllMain@12" if x86 else "DllMain"
    link_extra = ["-Wl,--kill-at"] if x86 else []
    bases = dict(comctl32=0x1f000000, user32=0x1f400000, shell=0x1f800000, crt=0x1fc00000) if x86 else dict(
        comctl32=0x1f0000000, user32=0x1f4000000, shell=0x1f8000000, crt=0x1fc000000)
    wanted = {"comctl32", "user32", "shell", "crt", "dwrite", "d2d1", "uianimation"} if args.components == "all" else set(args.components.split(","))
    if x86:
        # The 32-bit C runtime component is built by the main build; the others are described here.
        wanted.discard("crt")
        wanted.discard("dwrite")
        wanted.discard("d2d1")
        wanted.discard("uianimation")
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
    for tool in ("clang", triple + "-gcc", triple + "-windres", "widl"):
        if not shutil.which(tool):
            raise RuntimeError(f"required controls build tool is missing: {tool}")
    records = []

    def run(command, cwd=None):
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
        records.append(dict(command=[str(part) for part in command], returncode=result.returncode))
        if result.returncode:
            raise RuntimeError(result.stderr or "controls build command failed: " + " ".join(map(str, command)))
        return result

    def decorated(object_files):
        """Defined global functions of the objects by plain name, with their stdcall argument bytes on 32 bits (None for the others)."""
        names = {}
        for line in run([triple + "-nm", "-g", "--defined-only", *object_files]).stdout.splitlines():
            parts = line.split()
            if len(parts) != 3 or parts[1] not in "TtDdBbRr":
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

    def definition_line(name, bytes_):
        return " " + name + ("@" + str(bytes_) if x86 and bytes_ is not None else "") + "\n"

    # Raven's own corrections to upstream behavior are kept as reviewable patches and recorded in the provenance.
    patches = sorted((repo / "native/upstream/reactos-controls/patches").glob("*.patch"))
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
    idls = set()

    def idl(name):
        if name in idls:
            return
        idls.add(name)
        path = source / "sdk/include/psdk" / (name + ".idl")
        for dependency in re.findall(r'import\s+"([^"]+)\.idl"', path.read_text()):
            idl(dependency)
        run(["widl", "-m32" if x86 else "-m64", "-h", "-H", generated / "psdk" / (name + ".h"), "-I", path.parent, path])

    for name in ("objidl", "oleidl", "urlmon", "propidl", "wtypes", "objidlbase") + IDLS:
        idl(name)
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
    # The Wine-compat header overlay sits one directory below its psdk siblings upstream.
    overlay = source / "sdk/include/reactos/wine"
    shutil.copytree(source / "sdk/include/wine", overlay, dirs_exist_ok=True)
    gcc_include = run([triple + "-gcc", "-print-file-name=include"]).stdout.strip()
    includes = [source / "dll/win32/comctl32", generated, generated / "psdk", source / "sdk/lib/pseh/include", source / "sdk/include/vcruntime",
                overlay, source / "sdk/include/psdk", source / "sdk/include/crt", source / "sdk/include/ndk", source / "sdk/include",
                source / "sdk/include/reactos", source / "sdk/include/ddk"]
    flags = ["-O2", "-nostdinc", "-isystem", gcc_include, "-fgnu89-inline", "-w", "-D_WIN32_WINNT=0x600", "-DWINVER=0x600", "-D_WIN32_IE=0x0A00",
             "-D__REACTOS__", "-DUNICODE", "-D_UNICODE", cpu_define, "-D__WINESRC__", "-D_WINE", *long_defines, "-D_COMCTL32_",
             f"-ffile-prefix-map={source}=reactos", f"-ffile-prefix-map={repo}=raven", f"-ffile-prefix-map={output}=component",
             *["-I" + str(path) for path in includes]]
    support = repo / "native/upstream/reactos-controls"
    objects = output / "objects"
    shutil.rmtree(objects, ignore_errors=True)
    objects.mkdir()
    sources = sorted((source / "dll/win32/comctl32").glob("*.c")) + [support / "crt.c", support / "debug.c", support / "guids.c"]

    def compile(path):
        name = path.stem if path.parent == support else "comctl32_" + path.stem
        freestanding = ["-ffreestanding", "-fno-builtin"] if path.name == "crt.c" else []
        run([triple + "-gcc", "-c", *flags, *freestanding, path, "-o", objects / (name + ".o")])
        return objects / (name + ".o")

    with ThreadPoolExecutor(max_workers=2) as pool:
        compiled = list(pool.map(compile, sources))
    # 32-bit code registers its exception frames on the thread's chain: the frame based PSEH support of ReactOS is built into each component.
    pseh_objects = []
    if x86:
        pseh = source / "sdk/lib/pseh"
        for name in ("framebased-gcchack.c", "framebased-gcchack-asm.S", "framebased.S", "../framebased.c"):
            target = objects / ("pseh_" + Path(name).name.replace(".", "_") + ".o")
            language = ["-x", "assembler-with-cpp"] if name.endswith(".S") else flags
            run([triple + "-gcc", "-c", *language, "-I" + str(pseh / "include"), "-I" + str(pseh / "include/pseh"), pseh / "i386" / name, "-o", target])
            pseh_objects.append(target)
    run([output / "spec2def", spec_arch, "-n=comctl32.dll", "-d=" + str(output / "comctl32.def"), source / "dll/win32/comctl32/comctl32.spec"])
    # Programs import this version 6 function by ordinal; the pinned specification leaves the implemented function unexported.
    with (output / "comctl32.def").open("a") as definition:
        definition.write(" LoadIconWithScaleDown @381\n")
    wrapper = output / "resource-preprocessor.sh"
    wrapper.write_text("#!/bin/sh\nexec " + triple + "-gcc -E -xc -DRC_INVOKED -nostdinc -isystem " + gcc_include + " " +
                       " ".join("-I" + str(path) for path in includes[1:] + [generated / "reactos"]) + ' -w "$@"\n')
    wrapper.chmod(0o755)
    run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-DLANGUAGE_EN_US", "-D__REACTOS__", "-D_WIN32_IE=0x0A00",
         "-D__WINESRC__", cpu_define, "-I" + str(source / "dll/win32/comctl32"), "-I" + str(generated / "reactos"),
         "-i", source / "dll/win32/comctl32/rsrc.rc", "-o", objects / "comctl32.res.o"])
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{bases['comctl32']:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "comctl32.dll", *compiled, *pseh_objects, objects / "comctl32.res.o", output / "comctl32.def",
         "-luser32", "-lgdi32", "-ladvapi32", "-lkernel32", "-lntdll", "-limm32", "-lusp10", "-luxtheme", "-lwinmm",
         "-lole32", "-loleaut32", "-lshlwapi", "-luuid", "-lgcc"])
    if not wanted & {"user32", "shell", "crt", "dwrite", "d2d1", "uianimation"}:
        print(output / "comctl32.dll")
        return
    # The user32 component: upstream user-mode sources on a bridge that stands in for the kernel services.
    user_support = repo / "native/upstream/reactos-user32"
    user_root = source / "win32ss/user/user32"
    user_includes = [user_support / "shim", source / "win32ss/include", user_root / "include", *includes[1:]]
    user_flags = ["-O2", "-nostdinc", "-isystem", gcc_include, "-fgnu89-inline", "-w", "-D_WIN32_WINNT=0x600", "-DWINVER=0x600",
                  "-D__REACTOS__", "-DUNICODE", "-D_UNICODE", cpu_define, *long_defines, "-DUSE_PSEH",
                  f"-ffile-prefix-map={source}=reactos", f"-ffile-prefix-map={repo}=raven", f"-ffile-prefix-map={output}=component",
                  *["-I" + str(path) for path in user_includes]]
    user_sources = [user_root / name for name in USER32_SOURCES] + [source / "win32ss/user/rtl/text.c",
                    user_support / "bridge.c", user_support / "register.c", user_support / "scrollbar_control.c", support / "crt.c", support / "debug.c"]

    def compile_user(path):
        name = "user32_" + (path.parent.name + "_" if path.parent.name in ("windows", "controls", "rtl") else "") + path.stem
        freestanding = ["-ffreestanding", "-fno-builtin"] if path.name == "crt.c" else []
        run([triple + "-gcc", "-c", *user_flags, *freestanding, path, "-o", objects / (name + ".o")])
        return objects / (name + ".o")

    with ThreadPoolExecutor(max_workers=2) as pool:
        user_objects = list(pool.map(compile_user, user_sources))
    run([output / "spec2def", spec_arch, "-n=user32.dll", "-d=" + str(output / "user32.def"), user_root / "user32.spec"])
    public = {match.group(1) for match in re.finditer(r"^\s*([A-Za-z_][A-Za-z0-9_]*)(?:\s|=|@|$)", (output / "user32.def").read_text(), re.M)}
    defined = decorated(user_objects)
    # FrameRect and FillRect keep the measured host behavior (error codes for invalid brushes and device contexts).
    exported = sorted((public & set(defined)) - {"EXPORTS", "NAME", "FrameRect", "FillRect"})
    (output / "raven-user32.def").write_text("NAME raven-user32.dll\nEXPORTS\n" + "".join(definition_line(name, defined[name]) for name in exported) +
                                             definition_line("RavenInitControls", defined.get("RavenInitControls", 0)))
    # The built-in bitmaps, icons, cursors and strings that user32 serves to LoadBitmap and LoadImage without a module.
    run([triple + "-windres", "--preprocessor=" + str(wrapper), "-O", "coff", "-DLANGUAGE_EN_US", "-D__REACTOS__", "-D_WIN32_IE=0x0A00",
         cpu_define, "-I" + str(user_root), "-I" + str(user_root / "include"), "-I" + str(generated / "reactos"),
         "-i", user_root / "user32.rc", "-o", objects / "user32.res.o"])
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{bases['user32']:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "raven-user32.dll", *user_objects, *pseh_objects, objects / "user32.res.o", output / "raven-user32.def",
         "-luser32", "-lgdi32", "-ladvapi32", "-lkernel32", "-lntdll", "-limm32", "-lusp10", "-lgcc"])
    if not wanted & {"shell", "crt", "dwrite", "d2d1", "uianimation"}:
        print(output / "raven-user32.dll")
        return
    # The shell component is Raven code on the public headers: the file dialogs and the class registry.
    shell_support = repo / "native/upstream/raven-shell"
    shell_objects = [objects / "shell.o", objects / "shell_link.o", objects / "shell_com.o", objects / "shell_crt.o", objects / "shell_stream.o", objects / "shell_dialog.o"]
    shell_flags = ["-O2", "-w", "-ffreestanding", f"-ffile-prefix-map={repo}=raven"]
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "shell.c", "-o", shell_objects[0]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "link.c", "-o", shell_objects[1]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "com.c", "-o", shell_objects[2]])
    run([triple + "-gcc", "-c", *shell_flags, "-fno-builtin", support / "crt.c", "-o", shell_objects[3]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "stream.c", "-o", shell_objects[4]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "dialog.c", "-o", shell_objects[5]])
    shell_defined = decorated(shell_objects)
    shell_names = ("CoCreateInstance", "CoGetClassObject", "CoGetMalloc", "SHCreateItemFromParsingName", "SHGetPropertyStoreForWindow", "PropVariantClear", "SHAppBarMessage", "GetOpenFileNameW", "GetSaveFileNameW", "CommDlgExtendedError", "GetFileTitleW", "IUnknown_QueryService", "CLSIDFromProgID",
                   "SHCreateStreamOnFileA", "SHCreateStreamOnFileW", "SHCreateStreamOnFileEx", "PropVariantCopy", "CoGetContextToken", "CoGetObjectContext", "ExtractIconExW", "ExtractIconW", "UiaGetReservedNotSupportedValue", "UiaGetReservedMixedAttributeValue", "SHGetMalloc",
                   "OpenRegStream", "SHOpenRegStreamW", "SHOpenRegStreamA", "SHOpenRegStream2W", "SHOpenRegStream2A")
    (output / "raven-shell.def").write_text("NAME raven-shell.dll\nEXPORTS\n" + "".join(definition_line(name, shell_defined.get(name)) for name in shell_names))
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{bases['shell']:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "raven-shell.dll", *shell_objects, output / "raven-shell.def", "-luser32", "-lgdi32", "-ladvapi32", "-lkernel32", "-lole32", "-luuid", "-lgcc"])
    # control.exe: the control panel, a program on the public headers that asks for the common controls 6 in its manifest.
    control_support = repo / "native/upstream/raven-control"
    control_objects = [objects / "control.o", objects / "control_res.o"]
    run([triple + "-gcc", "-c", "-O2", "-Wall", "-fno-builtin", f"-ffile-prefix-map={repo}=raven", control_support / "control.c", "-o", control_objects[0]])
    run([triple + "-windres", "-O", "coff", "-I" + str(control_support), "-i", control_support / "control.rc", "-o", control_objects[1]])
    run([triple + "-gcc", "-nostdlib", "-mwindows", f"-Wl,--entry,{'_raven_program_entry@0' if x86 else 'raven_program_entry'},--no-insert-timestamp", *link_extra,
         "-o", output / "control.exe", *control_objects, shell_objects[3], "-luser32", "-lgdi32", "-ladvapi32", "-lcomctl32", "-lkernel32", "-lgcc"])
    # dism.exe: the feature servicing tool, as far as turning on the .NET Framework 3.5 from a local package; a console program.
    dism_support = repo / "native/upstream/raven-dism"
    run([triple + "-gcc", "-c", "-O2", "-Wall", "-fno-builtin", f"-ffile-prefix-map={repo}=raven", dism_support / "dism.c", "-o", objects / "dism.o"])
    # As on Windows, the tool asks to run as an administrator, and so do the installers it starts.
    run([triple + "-windres", "-O", "coff", "-I" + str(dism_support), "-i", dism_support / "dism.rc", "-o", objects / "dism_res.o"])
    run([triple + "-gcc", "-nostdlib", "-mconsole", f"-Wl,--entry,{'_raven_program_entry@0' if x86 else 'raven_program_entry'},--no-insert-timestamp", *link_extra,
         "-o", output / "dism.exe", objects / "dism.o", objects / "dism_res.o", shell_objects[3], "-ladvapi32", "-lkernel32", "-lgcc"])
    # fondue.exe: Windows Features on Demand, which offers the .NET Framework 3.5 to a program that needs it and has dism.exe install it.
    run([triple + "-gcc", "-c", "-O2", "-Wall", "-fno-builtin", f"-ffile-prefix-map={repo}=raven", dism_support / "fondue.c", "-o", objects / "fondue.o"])
    run([triple + "-gcc", "-nostdlib", "-mwindows", f"-Wl,--entry,{'_raven_program_entry@0' if x86 else 'raven_program_entry'},--no-insert-timestamp", *link_extra,
         "-o", output / "fondue.exe", objects / "fondue.o", shell_objects[3], "-lcomdlg32", "-luser32", "-ladvapi32", "-lkernel32", "-lgcc"])
    # regedit.exe: the Registry Editor as setup programs run it, importing .reg files with /s.
    run([triple + "-gcc", "-c", "-O2", "-Wall", "-fno-builtin", f"-ffile-prefix-map={repo}=raven", dism_support / "regedit.c", "-o", objects / "regedit.o"])
    run([triple + "-gcc", "-nostdlib", "-mwindows", f"-Wl,--entry,{'_raven_program_entry@0' if x86 else 'raven_program_entry'},--no-insert-timestamp", *link_extra,
         "-o", output / "regedit.exe", objects / "regedit.o", shell_objects[3], "-ladvapi32", "-lkernel32", "-lgcc"])
    if wanted & {"dwrite", "d2d1", "uianimation"}:
        # The host services the DirectWrite and Direct2D components call: fonts measured and rasterized with FreeType, surfaces drawn with Skia.
        host_names = ["RavenFontClose", "RavenFontGlyphs", "RavenFontInfo", "RavenFontMetrics", "RavenFontName", "RavenFontOpen", "RavenFontOutline",
                      "RavenFontRender", "RavenFontScan", "RavenFontTable", "RavenTrace", "RavenCanvasCreate", "RavenCanvasDestroy", "RavenCanvasDraw",
                      "RavenCanvasPixels", "RavenCanvasSize", "RavenPathCombine", "RavenPathWiden"]
        (output / "ravenhost.def").write_text("LIBRARY ravenhost.dll\nEXPORTS\n" + "".join(name + "\n" for name in host_names))
        run([triple + "-dlltool", "-d", output / "ravenhost.def", "-l", output / "libravenhost.a"])
    if "dwrite" in wanted:
        # The DirectWrite component: Raven code on the public headers, whose fonts come from the host through ravenhost.dll.
        dwrite_support = repo / "native/upstream/raven-dwrite"
        dwrite_objects = [objects / "dwrite.o", objects / "dwrite_crt.o"]
        dwrite_flags = ["-O2", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-ffreestanding", "-fno-builtin", f"-ffile-prefix-map={repo}=raven"]
        run([triple + "-gcc", "-c", *dwrite_flags, "-I" + str(dwrite_support), dwrite_support / "dwrite.c", "-o", dwrite_objects[0]])
        run([triple + "-gcc", "-c", *dwrite_flags, dwrite_support / "crt.c", "-o", dwrite_objects[1]])
        (output / "raven-dwrite.def").write_text("NAME raven-dwrite.dll\nEXPORTS\nDWriteCreateFactory\n")
        run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,0x1fb000000,--no-insert-timestamp", *link_extra,
             "-o", output / "raven-dwrite.dll", *dwrite_objects, output / "raven-dwrite.def", "-L" + str(output), "-lravenhost", "-lgdi32", "-lkernel32", "-lgcc"])
    if "d2d1" in wanted:
        # The Direct2D component: Raven code on the public headers; surfaces live with the host and are drawn with Skia through ravenhost.dll,
        # text comes from the DirectWrite component and pictures from the imaging component.
        d2d1_support = repo / "native/upstream/raven-d2d1"
        d2d1_objects = [objects / "d2d1.o", objects / "d2d1_crt.o"]
        d2d1_flags = ["-O2", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-ffreestanding", "-fno-builtin", "-fno-math-errno",
                      f"-ffile-prefix-map={repo}=raven"]
        run([triple + "-gcc", "-c", *d2d1_flags, "-I" + str(d2d1_support), d2d1_support / "d2d1.c", "-o", d2d1_objects[0]])
        run([triple + "-gcc", "-c", *d2d1_flags, repo / "native/upstream/raven-dwrite/crt.c", "-o", d2d1_objects[1]])
        (output / "raven-d2d1.def").write_text("NAME raven-d2d1.dll\nEXPORTS\n" + "".join(name + "\n" for name in (
            "D2D1ComputeMaximumScaleFactor", "D2D1ConvertColorSpace", "D2D1CreateDevice", "D2D1CreateDeviceContext", "D2D1CreateFactory",
            "D2D1InvertMatrix", "D2D1IsMatrixInvertible", "D2D1MakeRotateMatrix", "D2D1MakeSkewMatrix", "D2D1SinCos", "D2D1Tan", "D2D1Vec3Length")))
        run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,0x1fa000000,--no-insert-timestamp", *link_extra,
             "-o", output / "raven-d2d1.dll", *d2d1_objects, output / "raven-d2d1.def", "-L" + str(output), "-lravenhost", "-ldwrite",
             "-lwindowscodecs", "-luser32", "-lgdi32", "-lkernel32", "-lgcc"])
    if "uianimation" in wanted:
        # The Windows Animation component: Raven code on the public headers, the classes UIAnimation.dll registers.
        animation_support = repo / "native/upstream/raven-uianimation"
        animation_objects = [objects / "uianimation.o", objects / "uianimation_crt.o"]
        animation_flags = ["-O2", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-ffreestanding", "-fno-builtin", "-fno-math-errno",
                           f"-ffile-prefix-map={repo}=raven"]
        run([triple + "-gcc", "-c", *animation_flags, "-I" + str(animation_support), animation_support / "uianimation.c", "-o", animation_objects[0]])
        run([triple + "-gcc", "-c", *animation_flags, repo / "native/upstream/raven-dwrite/crt.c", "-o", animation_objects[1]])
        (output / "raven-uianimation.def").write_text("NAME raven-uianimation.dll\nEXPORTS\nDllCanUnloadNow PRIVATE\nDllGetClassObject PRIVATE\n")
        run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,0x1f9000000,--no-insert-timestamp", *link_extra,
             "-o", output / "raven-uianimation.dll", *animation_objects, output / "raven-uianimation.def", "-L" + str(output), "-lravenhost",
             "-luuid", "-lkernel32", "-lgcc"])
    if "crt" in wanted:
        # The C runtime component: the Universal CRT entry points programs import from ucrtbase.dll.
        crt_object = objects / "ucrtbase.o"
        run([triple + "-gcc", "-c", "-O2", "-w", "-ffreestanding", "-fno-builtin", f"-ffile-prefix-map={repo}=raven",
             repo / "native/upstream/raven-crt/ucrtbase.c", "-o", crt_object])
        run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{bases['crt']:#x},--no-insert-timestamp", *link_extra,
             "-o", output / "ucrtbase.dll", crt_object, "-lkernel32", "-lgcc"])
    result = dict(revision=REVISION, archive_sha256=ARCHIVE_SHA256, commands=records,
                  license_notices=preserve_notices(source, output),
                  component_sha256={name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                                    for name in ("comctl32.dll", "raven-user32.dll", "raven-shell.dll", "raven-dwrite.dll", "raven-d2d1.dll", "raven-uianimation.dll",
                                                 "ucrtbase.dll")
                                    if (output / name).exists()},
                  user32_exports=exported,
                  patches={patch.name: hashlib.sha256(patch.read_bytes()).hexdigest() for patch in patches},
                  source_hashes={path.relative_to(source).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted(source.rglob("*")) if path.is_file() and not path.is_relative_to(overlay)})
    (output / "provenance.json").write_text(json.dumps(result, indent=2) + "\n")
    print(output / "comctl32.dll")


if __name__ == "__main__":
    main()
