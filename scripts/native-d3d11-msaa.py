#!/usr/bin/env python3
"""Render a multisampled Direct3D 11 triangle through DXVK on Raven, resolve it, and check the read-back pixels against a software rendering.

testdata/d3d11/msaa.exe draws into a four sample render target and resolves it with ResolveSubresource; check-d3d11-msaa.py requires the interior and
exterior to match an independent rendering and the edges to be partly covered. With the extra argument "gpu" the host's default Vulkan driver is used
instead of llvmpipe.

usage: native-d3d11-msaa.py RAVEN MSAA_EXE DXVK_PE64_DIRECTORY WORKDIR [gpu]
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
    shutil.copy(workload, run / "msaa.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "msaa.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "msaa.txt").read_text() if (drive / "msaa.txt").exists() else "missing"
    if result.returncode or report != "OK":
        raise SystemExit(f"workload did not finish: exit {result.returncode}, status {report!r}, {result.stderr[-300:]!r}")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-msaa.py")), str(drive / "msaa.raw")],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "the multisampled triangle does not match the reference")


if __name__ == "__main__":
    main()
