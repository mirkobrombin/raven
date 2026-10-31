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
import sys


def main():
    parser = argparse.ArgumentParser(description="Build the common controls and the other guest components")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arch", choices=("x64", "x86"), default="x64", help="build the 64-bit components, or the 32-bit ones for PE32 programs")
    parser.add_argument("--components", default="all", help="comma separated: comctl32, user32, shell, crt (default: all that exist for the architecture)")
    args = parser.parse_args()
    x86 = args.arch == "x86"
    triple = "i686-w64-mingw32" if x86 else "x86_64-w64-mingw32"
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
    for tool in (triple + "-gcc", triple + "-windres"):
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

    objects = output / "objects"
    shutil.rmtree(objects, ignore_errors=True)
    objects.mkdir()
    own_comctl32 = repo / "native/upstream/raven-comctl32"
    own_user32 = repo / "native/upstream/raven-user32"
    sources = sorted(own_comctl32.glob("*.c"))

    def compile(path):
        name = "comctl32_" + path.stem
        run([triple + "-gcc", "-c", "-O2", "-w", "-fno-builtin", "-D_COMCTL32_", "-D_WIN32_WINNT=0x600", "-D_WIN32_IE=0x0600", "-DUNICODE", "-D_UNICODE",
             f"-ffile-prefix-map={repo}=raven", path, "-o", objects / (name + ".o")])
        return objects / (name + ".o")

    with ThreadPoolExecutor(max_workers=2) as pool:
        compiled = list(pool.map(compile, sources))
    # The exports with their public numbers, including the unnamed ones and those that forward to the system.
    functions = decorated(compiled)

    def decorate_export(line):
        match = re.fullmatch(r"(\w+)(?:=([\w.]+))?(.*)", line)
        name, target, rest = match.groups()
        if target and "." in target:
            return " " + name + "=" + target + rest + "\n"
        bytes_ = functions.get(target or name)
        suffix = "@" + str(bytes_) if x86 and bytes_ is not None else ""
        return " " + name + ("=" + target + suffix if target else suffix) + rest + "\n"

    (output / "comctl32.def").write_text("NAME comctl32.dll\nEXPORTS\n" + "".join(decorate_export(line) for line in (own_comctl32 / "exports.txt").read_text().splitlines()))
    resources = output / "comctl32-resources"
    run([sys.executable, own_comctl32 / "make_resources.py", resources])
    run([triple + "-windres", "-O", "coff", "-I", resources, "-i", resources / "comctl32.rc", "-o", objects / "comctl32.res.o"])
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{bases['comctl32']:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "comctl32.dll", *compiled, objects / "comctl32.res.o", output / "comctl32.def",
         "-luser32", "-lgdi32", "-ladvapi32", "-lkernel32", "-lole32", "-loleaut32", "-luxtheme", "-lshlwapi", "-luuid", "-lgcc"])
    if not wanted & {"user32", "shell", "crt", "dwrite", "d2d1", "uianimation"}:
        print(output / "comctl32.dll")
        return
    # The user32 component: Raven code on the public headers, with resources it generates itself.
    user_flags = ["-O2", "-w", "-fno-builtin", "-D_USER32_", "-D_WIN32_WINNT=0x600", "-DWINVER=0x600", "-DOEMRESOURCE", "-DUNICODE", "-D_UNICODE",
                  f"-ffile-prefix-map={repo}=raven", "-I" + str(own_user32)]
    user_sources = [own_user32 / name for name in ("button.c", "chars.c", "classes.c", "combo.c", "crt.c", "dialog.c", "draw.c", "edit.c", "listbox.c",
                                                   "messagebox.c", "scrollbar.c", "static.c", "text.c")]

    def compile_user(path):
        run([triple + "-gcc", "-c", *user_flags, path, "-o", objects / ("user32_" + path.stem + ".o")])
        return objects / ("user32_" + path.stem + ".o")

    with ThreadPoolExecutor(max_workers=2) as pool:
        user_objects = list(pool.map(compile_user, user_sources))
    defined = decorated(user_objects)
    exported = (own_user32 / "exports.txt").read_text().split()
    (output / "raven-user32.def").write_text("NAME raven-user32.dll\nEXPORTS\n" + "".join(definition_line(name, defined[name]) for name in exported))
    # The built-in bitmaps and icons that user32 serves to LoadBitmap and LoadImage without a module.
    resources = output / "user32-resources"
    run([sys.executable, own_user32 / "make_resources.py", resources])
    run([triple + "-windres", "-O", "coff", "-I", resources, "-i", resources / "user32.rc", "-o", objects / "user32.res.o"])
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{bases['user32']:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "raven-user32.dll", *user_objects, objects / "user32.res.o", output / "raven-user32.def",
         "-luser32", "-lgdi32", "-luxtheme", "-lkernel32", "-lgcc"])
    if not wanted & {"shell", "crt", "dwrite", "d2d1", "uianimation"}:
        print(output / "raven-user32.dll")
        return
    # The shell component is Raven code on the public headers: the file dialogs and the class registries of COM and the Windows Runtime.
    shell_support = repo / "native/upstream/raven-shell"
    shell_objects = [objects / "shell.o", objects / "shell_link.o", objects / "shell_com.o", objects / "shell_crt.o", objects / "shell_stream.o", objects / "shell_dialog.o",
                     objects / "shell_netaddress.o", objects / "shell_winrt.o", objects / "shell_knownfolder.o"]
    shell_flags = ["-O2", "-w", "-ffreestanding", f"-ffile-prefix-map={repo}=raven"]
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "shell.c", "-o", shell_objects[0]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "link.c", "-o", shell_objects[1]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "com.c", "-o", shell_objects[2]])
    run([triple + "-gcc", "-c", *shell_flags, "-fno-builtin", own_comctl32 / "crt.c", "-o", shell_objects[3]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "stream.c", "-o", shell_objects[4]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "dialog.c", "-o", shell_objects[5]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "netaddress.c", "-o", shell_objects[6]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "winrt.c", "-o", shell_objects[7]])
    run([triple + "-gcc", "-c", *shell_flags, shell_support / "knownfolder.c", "-o", shell_objects[8]])
    shell_defined = decorated(shell_objects)
    shell_names = ("CoCreateInstance", "CoCreateInstanceEx", "CoGetClassObject", "CoGetTreatAsClass", "CoGetMalloc", "SHCreateItemFromParsingName", "SHGetPropertyStoreForWindow", "PropVariantClear", "SHAppBarMessage", "GetOpenFileNameW", "GetSaveFileNameW", "CommDlgExtendedError", "GetFileTitleW", "IUnknown_QueryService", "QISearch", "CLSIDFromProgID",
                   "SHCreateStreamOnFileA", "SHCreateStreamOnFileW", "SHCreateStreamOnFileEx", "SHCreateMemStream", "PropVariantCopy", "CoGetContextToken", "CoGetObjectContext", "ExtractIconExW", "ExtractIconW", "UiaGetReservedNotSupportedValue", "UiaGetReservedMixedAttributeValue", "SHGetMalloc",
                   "OpenRegStream", "SHOpenRegStreamW", "SHOpenRegStreamA", "SHOpenRegStream2W", "SHOpenRegStream2A", "InitNetworkAddressControl",
                   "RoGetActivationFactory", "RoActivateInstance", "CoCreateFreeThreadedMarshaler")
    (output / "raven-shell.def").write_text("NAME raven-shell.dll\nEXPORTS\n" + "".join(definition_line(name, shell_defined.get(name)) for name in shell_names))
    run([triple + "-gcc", "-shared", "-nostdlib", f"-Wl,--entry,{entry_point},--image-base,{bases['shell']:#x},--no-insert-timestamp", *link_extra,
         "-o", output / "raven-shell.dll", *shell_objects, output / "raven-shell.def", "-luser32", "-lgdi32", "-ladvapi32", "-lkernel32", "-lole32", "-luuid", "-lruntimeobject", "-lgcc"])
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
        # Office binds D2D1CreateFactory by number, as ordinal 1 of the system library.
        (output / "raven-d2d1.def").write_text("NAME raven-d2d1.dll\nEXPORTS\nD2D1CreateFactory @1\n" + "".join(name + "\n" for name in (
            "D2D1ComputeMaximumScaleFactor", "D2D1ConvertColorSpace", "D2D1CreateDevice", "D2D1CreateDeviceContext",
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
    result = dict(commands=records,
                  component_sha256={name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                                    for name in ("comctl32.dll", "raven-user32.dll", "raven-shell.dll", "raven-dwrite.dll", "raven-d2d1.dll", "raven-uianimation.dll",
                                                 "ucrtbase.dll")
                                    if (output / name).exists()},
                  user32_exports=exported)
    (output / "provenance.json").write_text(json.dumps(result, indent=2) + "\n")
    print(output / "comctl32.dll")


if __name__ == "__main__":
    main()
