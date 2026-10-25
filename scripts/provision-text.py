#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from component_licenses import mark_modified


def main():
    parser = argparse.ArgumentParser(description="Build the selected ReactOS guest text-layout component")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[1] / "native/upstream/reactos-text"
    provenance = json.loads((source / "provenance.json").read_text())
    for name, digest in provenance["local_files"].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"selected text source checksum differs: {name}")
    env = dict(os.environ, TMPDIR=str(output), TMP=str(output), TEMP=str(output))
    commands = []

    def run(command):
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        commands.append(dict(command=command, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr))
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        return result.stdout

    patched = output / "text.c"
    run(["patch", "--batch", "--output", str(patched), str(source / "reactos-rtl-text.c"), str(source / "failure-propagation.patch")])
    worker = output / "layout.c"
    run(["patch", "--batch", "--output", str(worker), str(patched), str(source / "layout-contracts.patch")])
    mark_modified(worker)
    patched.unlink()
    component = output / "raven-text.dll"
    run(["x86_64-w64-mingw32-gcc", "-O2", "-DNDEBUG", "-D__REACTOS__", "-D_CRTBLD",
         "-ffunction-sections", "-fdata-sections", "-fno-builtin", "-shared", "-nostdlib",
         "-Wl,--entry,0,--image-base,0x1db000000,--gc-sections,--no-insert-timestamp,--out-implib," + str(output / "raven-text.a"),
         "-I" + str(source), str(worker), *[str(source / n) for n in
         ("wrapper.c", "memcpy.c", "memmove.c", "wcslen.c", "wcschr.c", "wcsrchr.c")],
         "-o", str(component), "-luser32", "-lgdi32", "-lkernel32", "-lgcc"])
    imports = run(["x86_64-w64-mingw32-objdump", "-p", str(component)])
    (output / "imports.txt").write_text(imports)
    provenance.update(component_sha256=hashlib.sha256(component.read_bytes()).hexdigest(),
                      patched_sha256=hashlib.sha256(worker.read_bytes()).hexdigest())
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(component)


if __name__ == "__main__":
    main()
