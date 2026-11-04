#!/usr/bin/env python3
"""Run the Direct3D 11 triangle workload through DXVK on Raven and verify the pixels it read back.

The workload (testdata/d3d11/triangle.exe) is an independent Windows program: it clears an offscreen render target, draws a
vertex-coloured triangle with its own shaders and copies the target to a staging texture. DXVK translates every call to Vulkan,
Raven's Vulkan boundary forwards it to the host driver, and check-d3d11-triangle.py compares the returned pixels with a reference
rasterizer. A run that only creates the device, or whose draw is culled or dropped, fails the comparison.

usage: native-d3d11.py RAVEN TRIANGLE_EXE DXVK_PE64_DIRECTORY WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
from lavapipe import icd as lavapipe_icd


def main():
    raven, workload, dxvk, work = map(Path, sys.argv[1:5])
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d11.dll", "dxgi.dll", "d3d10core.dll"):
        shutil.copy(dxvk / name, run / name)
    shutil.copy(workload, run / "triangle.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "triangle.exe")],
                            capture_output=True, text=True, timeout=280, env=env)
    status = (prefix / "drive_c" / "triangle.txt").read_text() if (prefix / "drive_c" / "triangle.txt").exists() else "missing"
    if result.returncode or status != "OK":
        raise SystemExit(f"workload did not finish: exit {result.returncode}, status {status!r}, {result.stderr[-300:]!r}")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-triangle.py")), str(prefix / "drive_c" / "triangle.raw")],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "pixel comparison failed")


if __name__ == "__main__":
    main()
