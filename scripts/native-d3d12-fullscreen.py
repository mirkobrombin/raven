#!/usr/bin/env python3
"""Take a Direct3D 12 swap chain through vkd3d-proton and DXVK's DXGI on Raven into full screen mode and out again, and check what the GPU rendered in each state.

testdata/d3d12/fullscreen12.exe renders a few frames in a window, calls SetFullscreenState(TRUE), resizes the buffers to what the swap chain now
is, renders and presents again, then leaves full screen and does the same. Each state's last frame is read back with the GPU and written to a
file with the size the swap chain reported. This script runs it on an isolated headless compositor and requires that the full screen state is
the size of the display and the window state is the size it had, that both frames hold the clear colour of their round with the triangle
inside, and that the program ended by itself.

usage: native-d3d12-fullscreen.py RAVEN FULLSCREEN12_EXE VKD3D_PE64_DIRECTORY DXVK_PE64_DIRECTORY WORKDIR [gpu]
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

from PIL import Image
from lavapipe import icd as lavapipe_icd

SIZE = 256
GPU = len(sys.argv) > 6 and sys.argv[6] == "gpu"
RUNTIME = Path(f"/run/user/{os.getuid()}/raven-labwc-{'gpu' if GPU else 'runtime'}")


def environment():
    return {"PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": str(RUNTIME), "WAYLAND_DISPLAY": "wayland-0"}


def start_compositor(log):
    if (RUNTIME / "wayland-0").exists():
        probe = subprocess.run(["wlrctl", "toplevel", "list"], env=environment(), capture_output=True, timeout=10)
        if probe.returncode == 0:
            return None
        (RUNTIME / "wayland-0").unlink(missing_ok=True)
        (RUNTIME / "wayland-0.lock").unlink(missing_ok=True)
    RUNTIME.mkdir(mode=0o700, parents=True, exist_ok=True)
    env = environment() | {"WLR_BACKENDS": "headless", "WLR_RENDERER": "gles2" if GPU else "pixman", "WLR_HEADLESS_OUTPUTS": "1"}
    process = subprocess.Popen(["labwc"], env=env, stdout=log, stderr=log)
    for _ in range(50):
        if (RUNTIME / "wayland-0").exists():
            time.sleep(0.5)
            return process
        time.sleep(0.1)
    raise RuntimeError("isolated compositor did not start")


def pixel(data, width, x, y):
    at = (y * width + x) * 4
    return tuple(data[at:at + 3])


def main():
    raven, workload, vkd3d, dxvk, work = map(Path, sys.argv[1:6])
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d12.dll", "d3d12core.dll"):
        shutil.copy(vkd3d / name, run / name)
    shutil.copy(dxvk / "dxgi.dll", run / "dxgi.dll")
    shutil.copy(workload, run / "fullscreen12.exe")
    with open(work / "compositor.log", "wb") as log:
        compositor = start_compositor(log)
        env = os.environ | environment() | {"RAVEN_PREFIX": str(prefix)}
        env["PATH"] = os.environ["PATH"]
        lavapipe = lavapipe_icd()
        if "VK_DRIVER_FILES" not in env and lavapipe.exists() and not GPU:
            env["VK_DRIVER_FILES"] = str(lavapipe)
        try:
            result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=800000", "--desktop=wayland", str(run / "fullscreen12.exe")],
                                    capture_output=True, text=True, timeout=850, env=env)
        finally:
            if compositor:
                compositor.terminate()
    drive = prefix / "drive_c"
    status = (drive / "fullscreen12.txt").read_text() if (drive / "fullscreen12.txt").exists() else "missing"
    if result.returncode or status != "OK":
        raise SystemExit(f"workload did not finish: exit {result.returncode}, status {status!r}, {result.stderr[-300:]!r}")
    full_size, back_size = (drive / "fullscreen12_full.txt").read_text(), (drive / "fullscreen12_back.txt").read_text()
    print(f"full screen state {full_size}, window state {back_size}")
    if back_size != "256x256" or "x" not in full_size:
        raise SystemExit("the swap chain did not return to the size of its window")
    full_width, full_height = map(int, full_size.split("x"))
    if (full_width, full_height) == (256, 256) or full_width < 640:
        raise SystemExit(f"the full screen state is not the size of a display: {full_size}")
    for label, name, width, height, clear in (("full screen", "fullscreen12_full.raw", full_width, full_height, (26, 102, 77)),
                                              ("window", "fullscreen12_back.raw", 256, 256, (26, 51, 77))):
        data = (drive / name).read_bytes()
        if len(data) != width * height * 4:
            raise SystemExit(f"{label}: read back {len(data)} bytes, expected {width * height * 4}")
        corner, center = pixel(data, width, 0, 0), pixel(data, width, width // 2, height // 2)
        if any(abs(a - b) > 1 for a, b in zip(corner, clear)):
            raise SystemExit(f"{label}: the corner is {corner}, expected the clear colour {clear}")
        if all(abs(a - b) <= 1 for a, b in zip(center, clear)):
            raise SystemExit(f"{label}: the triangle is missing from the frame")
        print(f"{label}: clear colour {corner}, triangle colour at the centre {center}")
    print("a Direct3D 12 swap chain went into full screen mode and out again through vkd3d-proton and DXVK, and rendered correctly in both")


if __name__ == "__main__":
    main()
