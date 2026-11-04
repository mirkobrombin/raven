#!/usr/bin/env python3
"""Draw a triangle with Direct3D 12 through vkd3d-proton on Raven and compare the read-back pixels with a reference rasterizer.

testdata/d3d12/draw.exe builds a root signature, a graphics pipeline from vs_5_0 and ps_5_0 shaders and a vertex buffer, records a clear, a
draw, a transition and a copy into a read-back buffer, runs them on a queue and waits on a fence. check-d3d11-triangle.py compares the
pixels with an independent triangle rasterizer (the same geometry as the Direct3D 11 workload).

usage: native-d3d12-draw.py RAVEN DRAW_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
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
    shutil.copy(workload, run / "draw.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "draw.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "draw.txt").read_text() if (drive / "draw.txt").exists() else "missing"
    if result.returncode or report != "OK":
        raise SystemExit(f"D3D12 workload did not finish: exit {result.returncode}, report {report!r}, {result.stderr[-300:]!r}")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-triangle.py")), str(drive / "draw.raw")],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "the Direct3D 12 triangle does not match the reference")
    print("a Direct3D 12 pipeline and draw executed through vkd3d-proton and Raven's Vulkan bridge match the reference")


if __name__ == "__main__":
    main()
