#!/usr/bin/env python3
"""Draw a triangle through a Direct3D 12 pipeline with a geometry stage or with tessellation stages on vkd3d-proton and Raven.

testdata/d3d12/geometry12.exe adds a geometry shader to the pipeline and testdata/d3d12/tessellation12.exe adds a hull and a domain shader
and draws a three control point patch; both stages are the hand assembled ones of the Direct3D 11 workloads (generate-d3d11-gs.py,
generate-d3d11-tess.py) and halve the triangle. check-d3d11-triangle.py compares the read-back pixels with the reference rasterizer for the
half size triangle, so a run that drops the stage, or a patch the tessellator culls, fails the comparison.

usage: native-d3d12-stages.py RAVEN STAGES_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
from lavapipe import icd as lavapipe_icd


def main():
    raven, workload, vkd3d, dxvk, work = map(Path, sys.argv[1:6])
    gpu = len(sys.argv) > 6 and sys.argv[6] == "gpu"
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d12.dll", "d3d12core.dll"):
        shutil.copy(vkd3d / name, run / name)
    shutil.copy(dxvk / "dxgi.dll", run / "dxgi.dll")
    shutil.copy(workload, run / "stages.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "stages.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "stages.txt").read_text() if (drive / "stages.txt").exists() else "missing"
    if result.returncode or report != "OK":
        raise SystemExit(f"D3D12 workload did not finish: exit {result.returncode}, report {report!r}, {result.stderr[-300:]!r}")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-triangle.py")), str(drive / "stages.raw"), "256", "256", "26", "26", "76", "0.5"],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "the Direct3D 12 triangle does not match the reference")
    print("a Direct3D 12 pipeline with the extra stage executed through vkd3d-proton and Raven's Vulkan bridge and matches the half size reference")


if __name__ == "__main__":
    main()
