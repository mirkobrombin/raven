#!/usr/bin/env python3
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import time
import xml.etree.ElementTree as ET


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def git(source, *arguments):
    return subprocess.check_output(["git", "-C", str(source), *arguments])


def snapshot(source, build):
    checkout = (source / ".git").exists()
    if checkout:
        paths = git(source, "ls-files", "--cached", "--others", "--exclude-standard", "-z").decode().split("\0")
    else:
        paths = [path.relative_to(source).as_posix() for path in source.rglob("*")
                 if (path.is_file() or path.is_symlink()) and not path.is_relative_to(build)]
    files = {}
    for name in sorted(set(paths) - {""}):
        path = source / name
        if path.is_symlink():
            files[name] = hashlib.sha256(os.readlink(path).encode()).hexdigest()
        elif path.is_file():
            files[name] = digest(path)
        else:
            raise RuntimeError(f"source input is missing: {name}")
    return dict(revision=git(source, "rev-parse", "HEAD").decode().strip() if checkout else None,
                dirty=bool(git(source, "status", "--porcelain", "--untracked-files=all")) if checkout else False,
                files=files, cache=digest(build / "CMakeCache.txt"))


def write_record(path, record):
    text = json.dumps(record, sort_keys=True, indent=2) + "\n"
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def runtime_paths(build):
    native = build / "native"
    files = [native / "raven", native / "raven-bootstrap"]
    names = """comctl32.dll raven-user32.dll raven-shell.dll raven-dwrite.dll raven-d2d1.dll
        raven-uianimation.dll ucrtbase.dll raven-rtl.dll raven-text.dll raven-storage.dll
        msi.dll cabinet.dll sxs.dll rpcrt4.dll libxslt.dll msxml3.dll atl100.dll oleaut32.dll
        libpng.dll libjpeg.dll libtiff.dll propsys.dll mlang.dll windowscodecs.dll gdiplus.dll
        urlmon.dll riched20.dll msftedit.dll msvfw32.dll advpack.dll raven-setupapi.dll
        control.exe regedit.exe dism.exe fondue.exe msiexec.exe regsvr32.exe
        mscoree.dll ucrtbase32.dll l_intl.nls""".split()
    for root in (native, native / "x86"):
        files += [root / name for name in names if (root / name).exists()]
    files += sorted((native / "roots").glob("*.crt"))
    paths = {path.relative_to(native).as_posix(): path for path in files}
    font_directory = cache_values(build).get("RAVEN_FONT_DIRECTORY")
    if font_directory and not font_directory.endswith("-NOTFOUND"):
        fonts = Path(font_directory)
        for face in ("Sans", "Mono", "Serif"):
            name = f"Liberation{face}-Regular.ttf"
            paths["fonts/" + name] = fonts / name
        paths.update({"fonts/" + path.name: path for path in sorted(fonts.glob("Liberation*.ttf"))})
    for path in paths.values():
        if path.is_symlink() or not path.is_file():
            raise RuntimeError(f"runtime input must be a regular file: {path}")
    return paths


def runtime_files(build):
    return {name: digest(path) for name, path in runtime_paths(build).items()}


def cache_values(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        match = re.match(r"([^/#][^:]*):[^=]+=(.*)$", line)
        if match:
            values[match[1]] = match[2]
    return values


def required_materials(cache):
    keys = [key for key in cache if key.startswith("RAVEN_") and
            ("COMPONENT_DIR" in key or key.endswith(("_COMPONENT_PATH", "_PE32_DIR", "_PE64_DIR", "_FONT_DIRECTORY", "_SKIA_LIBRARY")))]
    keys += [key for key, value in cache.items() if key.endswith("_LIBRARY_RELEASE") and value.endswith(".a")]
    keys += [key for key in ("RAVEN_ROOT_CERTIFICATE_DIRECTORY",) if key in cache]
    return {key for key in keys if cache[key] and not cache[key].endswith("-NOTFOUND")}


def add_bytes(archive, name, data, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size, info.mode, info.mtime = len(data), mode, 0
    archive.addfile(info, io.BytesIO(data))


def bundle(source, build, output, source_output, test_report, materials_path):
    record = json.loads((build / "native-distribution-build.json").read_text())
    current = snapshot(source, build)
    if current["revision"] is None:
        raise RuntimeError("release packaging requires a Git checkout; extracted source archives can still be built")
    if current["dirty"]:
        raise RuntimeError("distribution requires a clean source tree; local runtime work must not be packaged")
    if record["inputs"] != current or record["binaries"] != runtime_files(build):
        raise RuntimeError("source, configuration or runtime files changed after the recorded build; rebuild first")
    report = ET.parse(test_report).getroot()
    cases = report.findall(".//testcase")
    if not cases or report.findall(".//failure") or report.findall(".//error"):
        raise RuntimeError("a nonempty passing CTest JUnit report is required")
    if test_report.stat().st_mtime < record["finished"]:
        raise RuntimeError("test report predates the recorded build")
    expected = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "--show-only=json-v1"]))
    names = [case.get("name") for case in cases]
    if len(names) != len(set(names)) or set(names) != {case["name"] for case in expected["tests"]}:
        raise RuntimeError("test report does not cover the complete configured CTest suite")
    cache = cache_values(build)
    review = json.loads((source / "LICENSES.json").read_text())
    blockers = [item["name"] for item in review["components"]
                if item["review"] != "reviewed" and
                any(cache.get(key) and not cache[key].endswith("-NOTFOUND") for key in item.get("cmake_keys", []))]
    if blockers:
        raise RuntimeError("unresolved license review for: " + ", ".join(blockers) + "; see LICENSES.json")
    materials = json.loads(materials_path.read_text()) if materials_path else []
    covered, checked = set(), []
    for item in materials:
        name = item["name"]
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name) or name in {value["name"] for value in checked}:
            raise RuntimeError(f"invalid or duplicate source material name: {name}")
        paths = [Path(item["source"]), Path(item["recipe"]), *map(Path, item["notices"])]
        notice_root = paths[1].parent / "licenses"
        paths += [path for path in sorted(notice_root.rglob("*")) if path.is_file() and path not in paths]
        if not item["notices"] or any(path.is_symlink() or not path.is_file() for path in paths):
            raise RuntimeError(f"source material needs a source archive, recipe and notices: {name}")
        if digest(paths[0]) != item["sha256"]:
            raise RuntimeError(f"source material checksum differs: {name}")
        if not tarfile.is_tarfile(paths[0]):
            raise RuntimeError(f"source material must be a source tar archive, not a DLL or binary: {name}")
        covered.update(item["cmake_keys"])
        checked.append(dict(name=name, paths=paths, cmake_keys=item["cmake_keys"], sha256=item["sha256"]))
    missing = required_materials(cache) - covered
    if missing:
        raise RuntimeError("missing corresponding source materials for: " + ", ".join(sorted(missing)))
    for key in ("CMAKE_EXE_LINKER_FLAGS", "CMAKE_CXX_FLAGS"):
        if re.search(r"(?:^|\s)-static(?:\s|$)|\.a(?:\s|$)", cache.get(key, "")):
            raise RuntimeError("custom static linking needs a dependency review before packaging")
    for destination in (output, source_output):
        if destination.exists():
            raise RuntimeError(f"refusing to overwrite package: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + ".partial")
    source_partial = source_output.with_name(source_output.name + ".partial")
    files, source_files, created, published = {}, {}, [], []
    try:
        stream = partial.open("xb")
        created.append(partial)
        source_stream = source_partial.open("xb")
        created.append(source_partial)
        with stream, source_stream, tarfile.open(fileobj=stream, mode="w:gz") as archive, \
                tarfile.open(fileobj=source_stream, mode="w:gz") as sources:
            inputs = runtime_paths(build)
            for name in record["binaries"]:
                path = inputs[name]
                data = path.read_bytes()
                if hashlib.sha256(data).hexdigest() != record["binaries"][name]:
                    raise RuntimeError(f"runtime file changed while packaging: {name}")
                destination = "runtime/" + name
                add_bytes(archive, destination, data, path.stat().st_mode & 0o777)
                files[destination] = hashlib.sha256(data).hexdigest()
            names = ["LICENSE", "LICENSES.md", "LICENSES.json", "docs/packaging.md"]
            names += sorted(path.relative_to(source).as_posix() for path in (source / "LICENSES").iterdir() if path.is_file())
            for name in names:
                data = (source / name).read_bytes()
                add_bytes(archive, name, data)
                add_bytes(sources, name, data)
                files[name] = hashlib.sha256(data).hexdigest()
                source_files[name] = files[name]
            data = (f"Corresponding source: {source_output.name}\n"
                    f"Revision: {current['revision']}\n"
                    "Download the source archive from the same release as this binary archive.\n"
                    "Runtime: GPL-3.0-or-later. Guest components retain their recorded terms.\n").encode()
            add_bytes(archive, "SOURCE.txt", data)
            files["SOURCE.txt"] = hashlib.sha256(data).hexdigest()
            source_tar = git(source, "archive", "--format=tar", "--prefix=raven-source/", current["revision"])
            add_bytes(sources, "raven.tar", source_tar)
            source_files["raven.tar"] = hashlib.sha256(source_tar).hexdigest()
            for item in checked:
                for index, path in enumerate(item["paths"]):
                    destination = f"sources/{item['name']}/{index}-{path.name}"
                    checksum = digest(path)
                    if index == 0 and checksum != item["sha256"]:
                        raise RuntimeError(f"source material changed while packaging: {item['name']}")
                    with path.open("rb") as material:
                        info = tarfile.TarInfo(destination)
                        info.size, info.mode = path.stat().st_size, 0o644
                        sources.addfile(info, material)
                    if digest(path) != checksum:
                        raise RuntimeError(f"source material changed while packaging: {path}")
                    source_files[destination] = checksum
                    if index > 0:
                        data = path.read_bytes()
                        add_bytes(archive, "notices/" + destination, data)
                        files["notices/" + destination] = checksum
            info = dict(revision=current["revision"], runtime_license="GPL-3.0-or-later",
                        original_code_license="GPL-3.0-or-later", binaries=record["binaries"],
                        guest_adapter_license="MIT", guest_components=review["components"],
                        source_files=current["files"], configured_materials=sorted(required_materials(cache)),
                        test_cases=len(cases), skipped=sum(case.find("skipped") is not None for case in cases),
                        features={key: bool(value and not value.endswith("-NOTFOUND")) for key, value in cache.items()
                                  if key.startswith("RAVEN_") and ("COMPONENT" in key or "SKIA" in key or "PE64_DIR" in key)})
            data = (json.dumps(info, indent=2) + "\n").encode()
            add_bytes(archive, "BUILD-INFO.json", data)
            add_bytes(sources, "BUILD-INFO.json", data)
            files["BUILD-INFO.json"] = hashlib.sha256(data).hexdigest()
            source_files["BUILD-INFO.json"] = files["BUILD-INFO.json"]
            checksums = "".join(f"{value}  {name}\n" for name, value in sorted(files.items()))
            add_bytes(archive, "SHA256SUMS", checksums.encode())
            checksums = "".join(f"{value}  {name}\n" for name, value in sorted(source_files.items()))
            add_bytes(sources, "SHA256SUMS", checksums.encode())
        source_hash = digest(source_partial)
        if snapshot(source, build) != current or runtime_files(build) != record["binaries"]:
            raise RuntimeError("build inputs changed while packaging")
        os.link(source_partial, source_output)
        published.append(source_output)
        os.link(partial, output)
        published.append(output)
    except BaseException:
        for path in published:
            path.unlink()
        raise
    finally:
        for path in created:
            path.unlink(missing_ok=True)
    print(output)
    print(source_output)
    print(f"Corresponding source SHA-256: {source_hash}")


def main():
    parser = argparse.ArgumentParser(description="Record a native build and package its corresponding sources and licenses")
    parser.add_argument("mode", choices=("prepare", "seal", "bundle"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source-output", type=Path)
    parser.add_argument("--test-report", type=Path)
    parser.add_argument("--materials", type=Path)
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    try:
        if build.is_relative_to(source):
            raise RuntimeError("build directory must be outside the source tree")
        prepared = build / "native-distribution-inputs.json"
        if args.mode == "prepare":
            write_record(prepared, snapshot(source, build))
        elif args.mode == "seal":
            inputs = json.loads(prepared.read_text())
            if inputs != snapshot(source, build):
                raise RuntimeError("source or configuration changed during the build")
            if (build / "native/raven").stat().st_mtime < prepared.stat().st_mtime:
                raise RuntimeError("runtime predates its source record; rebuild first")
            record = dict(inputs=inputs, binaries=runtime_files(build), finished=time.time())
            write_record(build / "native-distribution-build.json", record)
        else:
            if not args.output or not args.source_output or not args.test_report:
                raise RuntimeError("bundle requires --output, --source-output and --test-report")
            if args.output.resolve() == args.source_output.resolve():
                raise RuntimeError("binary and source outputs must be different files")
            if any(path.resolve().is_relative_to(source) for path in (args.output, args.source_output)):
                raise RuntimeError("package output must be outside the source tree")
            bundle(source, build, args.output.resolve(), args.source_output.resolve(), args.test_report, args.materials)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError, ET.ParseError) as error:
        parser.exit(1, f"native packaging failed: {error}\n")


if __name__ == "__main__":
    main()
