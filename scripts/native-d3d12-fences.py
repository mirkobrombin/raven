#!/usr/bin/env python3
"""Wait on Direct3D 12 fences from several threads through vkd3d-proton on Raven.

testdata/d3d12/fences.exe gives four threads an event each for successive values of one fence and blocks a fifth in
SetEventOnCompletion without an event. The queue signals those values behind a wait on a second fence, and the test checks that nobody
wakes until the CPU signals that second fence, then that every waiter wakes. When the device offers ID3D12Device1, one event also waits
on both fences together, for any of them and for all. The program writes "OK" or the step that failed to C:\\fences12.txt.

usage: native-d3d12-fences.py RAVEN FENCES_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
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
    shutil.copy(workload, run / "fences.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "fences.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    report = prefix / "drive_c" / "fences12.txt"
    text = report.read_text() if report.exists() else "missing"
    if result.returncode or not text.startswith("OK"):
        raise SystemExit(f"D3D12 fence waiters failed: exit {result.returncode}, report {text!r}, {result.stderr[-300:]!r}")
    print(f"every D3D12 fence waiter woke at its value and none before: {text}")


if __name__ == "__main__":
    main()
