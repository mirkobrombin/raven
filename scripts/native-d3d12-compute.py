#!/usr/bin/env python3
"""Run a Direct3D 12 compute dispatch through vkd3d-proton on Raven and compare the buffer it wrote with the values computed here.

testdata/d3d12/compute.exe dispatches 4 groups of 64 threads; thread i writes i * i + 7 plus the red channel of texel (i mod 8, (i / 8) mod 8)
of an 8 by 8 checkerboard (read with Load through a descriptor table) into a typed unordered access buffer, which is copied into a read-back
buffer and written to a file.

usage: native-d3d12-compute.py RAVEN COMPUTE_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
"""

from pathlib import Path
import os
import shutil
import struct
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
    shutil.copy(workload, run / "compute.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "compute.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "compute12.txt").read_text() if (drive / "compute12.txt").exists() else "missing"
    if result.returncode or report != "OK":
        raise SystemExit(f"workload did not finish: exit {result.returncode}, status {report!r}, {result.stderr[-300:]!r}")
    data = (drive / "compute12.raw").read_bytes()
    if len(data) != 1024:
        raise SystemExit(f"expected 1024 bytes, got {len(data)}")
    values = struct.unpack("<256I", data)
    expected = [i * i + 7 + (255 if ((i // 8) % 8 + i % 8) % 2 == 0 else 240) for i in range(256)]
    wrong = [index for index in range(256) if values[index] != expected[index]]
    if wrong:
        raise SystemExit(f"{len(wrong)} of 256 values differ; first at {wrong[0]}: got {values[wrong[0]]}, expected {expected[wrong[0]]}")
    print("a Direct3D 12 compute dispatch through vkd3d-proton wrote all 256 expected values")


if __name__ == "__main__":
    main()
