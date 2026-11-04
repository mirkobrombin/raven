#!/usr/bin/env python3
"""Run Direct3D 12 commands through vkd3d-proton on Raven and check the pixels the GPU produced.

testdata/d3d12/clear.exe creates a command queue, a render target and a read-back buffer, clears the target to (0.2, 0.4, 0.6, 1.0) with
ClearRenderTargetView, copies it into the buffer, waits on a fence and writes the buffer to a file. Every pixel must be (51, 102, 153, 255).

usage: native-d3d12-clear.py RAVEN CLEAR_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
from lavapipe import icd as lavapipe_icd

EXPECTED = bytes((51, 102, 153, 255))


def main():
    raven, workload, vkd3d, dxvk, work = map(Path, sys.argv[1:6])
    gpu = len(sys.argv) > 6 and sys.argv[6] == "gpu"
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d12.dll", "d3d12core.dll"):
        shutil.copy(vkd3d / name, run / name)
    shutil.copy(dxvk / "dxgi.dll", run / "dxgi.dll")
    shutil.copy(workload, run / "clear.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "clear.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "clear.txt").read_text() if (drive / "clear.txt").exists() else "missing"
    if result.returncode or report != "OK":
        raise SystemExit(f"D3D12 workload did not finish: exit {result.returncode}, report {report!r}, {result.stderr[-300:]!r}")
    pixels = (drive / "clear.raw").read_bytes()
    if len(pixels) != 64 * 64 * 4:
        raise SystemExit(f"read-back holds {len(pixels)} bytes")
    wrong = sum(1 for index in range(0, len(pixels), 4) if pixels[index:index + 4] != EXPECTED)
    if wrong:
        raise SystemExit(f"{wrong} of 4096 read-back pixels differ from {tuple(EXPECTED)}")
    print("a Direct3D 12 clear and copy executed through vkd3d-proton: all 4096 read-back pixels equal", tuple(EXPECTED))


if __name__ == "__main__":
    main()
