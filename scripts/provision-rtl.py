#!/usr/bin/env python3
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import shutil
import subprocess


SOURCES = ("heap", "kernel", "memory", "tables")


def main():
    parser = argparse.ArgumentParser(description="Build the guest RTL component")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    objects = output / "objects"
    objects.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(output), TMP=str(output), TEMP=str(output))
    repo = Path(__file__).resolve().parents[1]
    root = repo / "native/upstream/raven-rtl"
    if not shutil.which("x86_64-w64-mingw32-gcc"):
        raise RuntimeError("required RTL build tool is missing: x86_64-w64-mingw32-gcc")

    def run(command):
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr or "RTL build command failed")

    def compile(name):
        run(["x86_64-w64-mingw32-gcc", "-c", "-O2", "-ffreestanding", "-ffunction-sections", "-fdata-sections", "-mcx16", "-D_NTSYSTEM_",
             f"-ffile-prefix-map={repo}=raven", "-I" + str(root), str(root / (name + ".c")), "-o", str(objects / (name + ".o"))])

    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile, SOURCES))
    run(["x86_64-w64-mingw32-gcc", "-shared", "-nostdlib", "-Wl,--exclude-all-symbols,--gc-sections,--entry,0,--image-base,0x1a1000000",
         "-Wl,--out-implib," + str(output / "raven-rtl.lib"), "-o", str(output / "raven-rtl.dll"),
         *[str(objects / (name + ".o")) for name in SOURCES], str(root / "raven-rtl.def"), "-lntdll", "-lgcc"])
    print(output / "raven-rtl.dll")


if __name__ == "__main__":
    main()
