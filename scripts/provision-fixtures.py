#!/usr/bin/env python3
"""Build test programs and fetch checksum-qualified third-party fixtures outside the source tree."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile


DOWNLOADS = (
    ("npp.zip", "https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.7.6/npp.8.7.6.portable.x64.zip",
     "8947bbdf3b87f466dea39ccab2f992177a132404b3907a669a4483484fc691cc", "npp"),
    ("sqlite.zip", "https://www.sqlite.org/2025/sqlite-tools-win-x64-3500400.zip",
     "8ce18347ea86a1ce65f33b533d2f144d8d1237140529fbf818574ca11fa13ad5", "."),
    ("busybox", "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox",
     "6e123e7f3202a8c1e9b1f94d8941580a25135382b99e8d3e34fb858bba311348", None),
)


def fetch(output):
    cache = output / "downloads"
    cache.mkdir(parents=True, exist_ok=True)
    for name, url, checksum, destination in DOWNLOADS:
        archive = cache / name
        if not archive.exists():
            temporary = archive.with_suffix(archive.suffix + ".part")
            try:
                urllib.request.urlretrieve(url, temporary)
                if hashlib.sha256(temporary.read_bytes()).hexdigest() != checksum:
                    raise RuntimeError("fixture checksum differs: " + name)
                temporary.replace(archive)
            finally:
                temporary.unlink(missing_ok=True)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != checksum:
            raise RuntimeError("fixture checksum differs: " + name)
        if destination is None:
            shutil.copyfile(archive, output / name)
            (output / name).chmod(0o755)
            continue
        target = output / destination
        target.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as upstream:
            for member in upstream.infolist():
                path = Path(member.filename)
                if path.is_absolute() or ".." in path.parts or "\\" in member.filename:
                    raise RuntimeError("invalid fixture archive path")
                if member.is_dir():
                    continue
                extracted = target / path
                extracted.parent.mkdir(parents=True, exist_ok=True)
                extracted.write_bytes(upstream.read(member))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="x86_64-w64-mingw32-gcc")
    parser.add_argument("--skip-downloads", action="store_true")
    parser.add_argument("--data-only", action="store_true")
    parser.add_argument("--legacy", action="store_true", help="also build the earlier DLL, latency and Wayland fixtures")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "testdata"
    output = args.output.resolve()
    if output == source or source in output.parents:
        raise RuntimeError("fixture output must be outside testdata")
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(output), TMP=str(output), TEMP=str(output))
    for path in source.rglob("*.hex"):
        target = output / path.relative_to(source).with_suffix("")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(bytes.fromhex(path.read_text()))
    if not args.skip_downloads:
        fetch(output)
    if args.data_only:
        return
    startup = output / "benchmark/startup.exe"
    startup.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([args.cc, "-O2", "-nostdlib", "-fno-stack-protector", "-Wl,--entry,entry,--no-insert-timestamp",
                    "-o", str(startup), str(source / "benchmark/startup.c"), "-lkernel32"], env=env, check=True)
    workloads = [("d3d11", path.stem, path.name, []) for path in sorted((source / "d3d11").glob("*.c"))]
    workloads += [("d3d12", path.stem, path.name, []) for path in sorted((source / "d3d12").glob("*.c")) if path.stem != "stages"]
    workloads += [("d3d12", name, "stages.c", ["-D" + define]) for name, define in (
        ("geometry12", "GEOMETRY"), ("tessellation12", "TESSELLATION"))]
    workloads += [("d3d12", name, "raytrace.c", ["-D" + define]) for name, define in (
        ("rtscene", "SCENE"), ("rtanyhit", "ANYHIT"), ("rtprocedural", "PROCEDURAL"), ("rayquery", "INLINE_QUERY"))]
    workloads.append(("d3d12", "window12_vsync", "window12.c", ["-DSYNC_INTERVAL=1"]))
    for folder, name, filename, defines in workloads:
        target = output / folder / (name + ".exe")
        target.parent.mkdir(parents=True, exist_ok=True)
        inputs = [source / folder / filename, *(source / folder).glob("*.h")]
        if folder == "d3d12":
            inputs += list((source / "d3d11").glob("*.h"))
        if target.exists() and all(path.stat().st_mtime <= target.stat().st_mtime for path in inputs):
            continue
        subprocess.run([args.cc, "-O2", "-nostdlib", "-fno-stack-protector", "-fno-builtin", "-mno-stack-arg-probe",
                        "-Wl,--entry,entry,--no-insert-timestamp", *defines, "-o", str(target), str(source / folder / filename),
                        "-l" + folder, "-ldxgi", "-ldxguid", "-luuid", "-luser32", "-lkernel32"], env=env, check=True)
    if source.joinpath("d3d9test/d3d9test.c").exists():
        target = output / "d3d9test/d3d9test.exe"
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([args.cc, "-O2", "-o", str(target), str(source / "d3d9test/d3d9test.c"),
                        "-ld3d9", "-luser32", "-lgdi32"], env=env, check=True)
    if args.legacy:
        dlls = output / "dll_fixture"
        dlls.mkdir(exist_ok=True)
        for name in ("raven_min", "raven_win32k"):
            subprocess.run([args.cc, "-O0", "-shared", "-nostdlib", "-fno-stack-protector",
                            "-Wl,--entry,_DllMainCRTStartup,--no-insert-timestamp", "-o", str(dlls / (name + ".dll")),
                            str(source / "dll_fixture" / (name + ".c"))], env=env, check=True)
        subprocess.run(["i686-w64-mingw32-gcc", "-nostdlib", "-fno-stack-protector", "-Wl,--entry,_raven_entry,--no-insert-timestamp",
                        "-o", str(dlls / "raven_gui_min.exe"), str(source / "dll_fixture/raven_gui_min.c"),
                        "-luser32", "-lgdi32", "-lkernel32"], env=env, check=True)
        (output / "latency").mkdir(exist_ok=True)
        subprocess.run([args.cc, "-O2", "-nostdlib", "-fno-stack-protector", "-Wl,--entry,entry,--no-insert-timestamp",
                        "-o", str(output / "latency/latency.exe"), str(source / "latency/latency.c"),
                        "-luser32", "-lkernel32"], env=env, check=True)
        subprocess.run(["make", "-C", str(source.parent), "-j1", "BUILD_DIR=" + str(output / "wayland")], env=env, check=True)


if __name__ == "__main__":
    main()
