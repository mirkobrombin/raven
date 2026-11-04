#!/usr/bin/env python3
"""Create a Direct3D 12 device through vkd3d-proton on Raven and check what the program reports.

testdata/d3d12/device.exe calls D3D12CreateDevice and asks the device for its highest feature level. vkd3d-proton loads d3d12core.dll
at run time, queries the Vulkan driver for the instance and device capabilities it requires, and builds its descriptor and memory model
on top of the driver. The test requires a successful device and a feature level of at least 11.0 (0xb000). This proves the dependency
path only: no D3D12 command is recorded or presented by this test.

usage: native-d3d12.py RAVEN DEVICE_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
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
    shutil.copy(workload, run / "device.exe")
    env = os.environ | {"RAVEN_PREFIX": str(prefix)}
    lavapipe = lavapipe_icd()
    if not gpu and "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=250000", str(run / "device.exe")],
                            capture_output=True, text=True, timeout=280, env=env, cwd=run)
    report = prefix / "drive_c" / "d3d12.txt"
    text = report.read_text() if report.exists() else "missing"
    fields = dict(item.split("=", 1) for item in text.split() if "=" in item)
    if result.returncode or fields.get("hr") != "00000000" or fields.get("query") != "ok" or int(fields.get("level", "0"), 16) < 0xB000:
        raise SystemExit(f"D3D12 device was not created: exit {result.returncode}, report {text!r}, {result.stderr[-300:]!r}")
    print(f"D3D12CreateDevice succeeded through vkd3d-proton: {text}")


if __name__ == "__main__":
    main()
