#!/usr/bin/env python3
"""Run a shader model 6 compute shader with wave intrinsics through vkd3d-proton on Raven and check what it wrote.

testdata/d3d12/wave.exe dispatches four groups of 64 threads of a cs_6_0 shader (wave.hlsl, compiled to DXIL) that checks six identities of
the wave intrinsics, each true for any wave size (a sum of ones equals the count of active lanes, a prefix sum of ones is the lane index,
the maximum and minimum lane, a read of lane zero, all and any, the first lane), and writes a mask of the identities that held and the wave
size for every thread. Every thread must report all six, and the wave size must be one power of two the same for every thread. When the
device has no wave operations the test is skipped (exit code 77). With the extra argument "gpu" the host's default Vulkan drivers are used
and not the software rasterizer.

usage: native-d3d12-wave.py RAVEN WAVE_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
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
    shutil.copy(workload, run / "wave.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "wave.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "wave.txt").read_text() if (drive / "wave.txt").exists() else "missing"
    if not result.returncode and report.startswith("SKIP"):
        print("skipped:", report)
        raise SystemExit(77)
    if result.returncode or report != "OK":
        raise SystemExit(f"wave workload did not finish: exit {result.returncode}, report {report!r}, {result.stderr[-300:]!r}")
    words = struct.unpack("<512I", (drive / "wave.raw").read_bytes())
    masks, sizes = words[0::2], words[1::2]
    wrong = [(index, mask) for index, mask in enumerate(masks) if mask != 63]
    if wrong:
        raise SystemExit(f"{len(wrong)} of {len(masks)} threads missed an identity, the first thread {wrong[0][0]} with mask {wrong[0][1]:06b}")
    size = sizes[0]
    if set(sizes) != {size} or size & (size - 1) or not 4 <= size <= 128:
        raise SystemExit(f"the wave sizes are not one power of two between 4 and 128: {sorted(set(sizes))}")
    print(f"all {len(masks)} threads satisfied the six wave identities with a wave size of {size}")
    print("shader model 6 wave intrinsics ran through vkd3d-proton and Raven's Vulkan bridge")


if __name__ == "__main__":
    main()
