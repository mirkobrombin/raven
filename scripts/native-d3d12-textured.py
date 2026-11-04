#!/usr/bin/env python3
"""Draw the textured, tinted, depth tested scene with Direct3D 12 through vkd3d-proton on Raven and compare the read-back pixels with a software rendering.

testdata/d3d12/textured.exe uploads a texture through an upload buffer, binds it and a sampler with descriptor tables, passes the tint as root
constants and uses a depth buffer; check-d3d11-textured.py renders the same scene independently (the Direct3D 11 workload draws the same one).
With the extra argument "gpu" the host's default Vulkan driver is used instead of llvmpipe.

usage: native-d3d12-textured.py RAVEN TEXTURED_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
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
    shutil.copy(workload, run / "textured.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "textured.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "textured12.txt").read_text() if (drive / "textured12.txt").exists() else "missing"
    if result.returncode or report != "OK":
        raise SystemExit(f"D3D12 workload did not finish: exit {result.returncode}, report {report!r}, {result.stderr[-300:]!r}")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-textured.py")), str(drive / "textured12.raw")],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "the Direct3D 12 textured scene does not match the reference")
    print("a Direct3D 12 textured scene (descriptor tables, sampler, root constants, depth) executed through vkd3d-proton matches the reference")


if __name__ == "__main__":
    main()
