#!/usr/bin/env python3
"""Run the Direct3D 11 tessellation workload through DXVK on Raven and verify the pixels it read back.

The workload (testdata/d3d11/tessellation.exe) is an independent Windows program: it draws a vertex-coloured triangle as a three control
point patch through a vertex shader, a hull shader, the fixed function tessellator, a domain shader that halves the x and y of the
tessellated points, and a pixel shader, and copies the target to a staging texture. DXVK translates every stage to Vulkan and Raven's
Vulkan boundary forwards the tessellation pipeline to the host driver. check-d3d11-triangle.py compares the returned pixels with a
reference rasterizer for the half size triangle; a patch the tessellator culls draws nothing, and a run that skips the domain shader
draws the full size triangle, so both fail the comparison.

usage: native-d3d11-tessellation.py RAVEN TESSELLATION_EXE DXVK_PE64_DIRECTORY WORKDIR [gpu]
With the extra argument "gpu" the host's default Vulkan drivers are used instead of the software rasterizer.
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
    shutil.copy(workload, run / "tessellation.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    gpu = len(sys.argv) > 5 and sys.argv[5] == "gpu"
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "tessellation.exe")],
                            capture_output=True, text=True, timeout=280, env=env)
    status = (prefix / "drive_c" / "tessellation.txt").read_text() if (prefix / "drive_c" / "tessellation.txt").exists() else "missing"
    if result.returncode or status != "OK":
        raise SystemExit(f"workload did not finish: exit {result.returncode}, status {status!r}, {result.stderr[-300:]!r}")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-triangle.py")), str(prefix / "drive_c" / "tessellation.raw"), "256", "256", "26", "26", "76", "0.5"],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "pixel comparison failed")
    print("a Direct3D 11 patch was tessellated by DXVK and Raven's Vulkan bridge and halved by the domain shader")


if __name__ == "__main__":
    main()
