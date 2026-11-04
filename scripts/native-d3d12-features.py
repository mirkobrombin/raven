#!/usr/bin/env python3
"""Run a Direct3D 12 workload through vkd3d-proton on Raven that draws with features the other workloads leave out, and check the pixels.

testdata/d3d12/features.exe draws two quads with one ExecuteIndirect into two render targets, with the quad colours in root constants. The
first target is blended over its black clear colour with an alpha of one half (the target's alpha becomes one half too); the second marks
which instance drew each pixel and keeps its grey clear colour elsewhere. An occlusion query is resolved into a buffer.

usage: native-d3d12-features.py RAVEN FEATURES_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
"""

from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
from lavapipe import icd as lavapipe_icd

SIZE = 256


def pixel(data, x, y):
    at = (y * SIZE + x) * 4
    return tuple(data[at:at + 4])


def near(value, expected, tolerance=3):
    return all(abs(a - b) <= tolerance for a, b in zip(value, expected))


def main():
    raven, workload, vkd3d, dxvk, work = map(Path, sys.argv[1:6])
    gpu = len(sys.argv) > 6 and sys.argv[6] == "gpu"
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d12.dll", "d3d12core.dll"):
        shutil.copy(vkd3d / name, run / name)
    shutil.copy(dxvk / "dxgi.dll", run / "dxgi.dll")
    shutil.copy(workload, run / "features.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "features.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    drive = prefix / "drive_c"
    report = (drive / "features12.txt").read_text() if (drive / "features12.txt").exists() else "missing"
    if result.returncode or report != "OK":
        raise SystemExit(f"D3D12 workload did not finish: exit {result.returncode}, report {report!r}, {result.stderr[-300:]!r}")
    first, second = (drive / "features12_0.raw").read_bytes(), (drive / "features12_1.raw").read_bytes()
    samples = struct.unpack("<Q", (drive / "features12_samples.raw").read_bytes())[0]
    if len(first) != SIZE * SIZE * 4 or len(second) != SIZE * SIZE * 4:
        raise SystemExit("a render target was not read back in full")
    # The left quad spans x 12.8 to 115.2 and the right one 140.8 to 243.2; both span y 25.6 to 230.4.
    checks = [
        ("blended left quad", pixel(first, 64, 128), (128, 0, 0, 128)),
        ("blended right quad", pixel(first, 192, 128), (0, 128, 0, 128)),
        ("gap between the quads", pixel(first, 128, 128), (0, 0, 0, 255)),
        ("above the quads", pixel(first, 64, 10), (0, 0, 0, 255)),
        ("instance mark in the left quad", pixel(second, 64, 128), (255, 128, 0, 255)),
        ("instance mark in the right quad", pixel(second, 192, 128), (0, 128, 255, 255)),
        ("second target outside the quads", pixel(second, 128, 128), (51, 51, 51, 255)),
    ]
    problems = [f"{label}: got {got}, expected {want}" for label, got, want in checks if not near(got, want)]
    allowed = (41000, 85000) if gpu else (41000, 43000)
    if not allowed[0] <= samples <= allowed[1]:
        problems.append(f"occlusion query counted {samples} samples, expected about 41600")
    if problems:
        raise SystemExit("; ".join(problems))
    print(f"an ExecuteIndirect draw into two targets, with blending, root constants and an occlusion query ({samples} samples), "
          "through vkd3d-proton matches every expected pixel")


if __name__ == "__main__":
    main()
