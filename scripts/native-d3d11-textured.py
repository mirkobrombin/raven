#!/usr/bin/env python3
"""Draw a textured, tinted, depth tested Direct3D 11 scene through DXVK on Raven and compare the read-back pixels with a software rendering.

testdata/d3d11/textured.exe uses an immutable checkerboard texture with a point sampler, a constant buffer updated between two draws and a depth
buffer; check-d3d11-textured.py renders the same scene independently. With the extra argument "gpu" the host's default Vulkan driver is used
instead of llvmpipe.

usage: native-d3d11-textured.py RAVEN TEXTURED_EXE DXVK_PE64_DIRECTORY WORKDIR [gpu]
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
from lavapipe import icd as lavapipe_icd


def main():
    raven, workload, dxvk, work = map(Path, sys.argv[1:5])
    gpu = len(sys.argv) > 5 and sys.argv[5] == "gpu"
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d11.dll", "dxgi.dll", "d3d10core.dll"):
        shutil.copy(dxvk / name, run / name)
    shutil.copy(workload, run / "textured.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "textured.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "textured.txt").read_text() if (drive / "textured.txt").exists() else "missing"
    if result.returncode or report != "OK":
        raise SystemExit(f"workload did not finish: exit {result.returncode}, status {report!r}, {result.stderr[-300:]!r}")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-textured.py")), str(drive / "textured.raw")],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "the textured scene does not match the reference")


if __name__ == "__main__":
    main()
