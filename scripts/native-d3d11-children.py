#!/usr/bin/env python3
"""Present two Direct3D 11 swap chains through DXVK on Raven into two child windows of one Wayland window.

testdata/d3d11/children.exe makes the swap chain of one child while that child is hidden and shows it afterwards, and gives a second
child, visible from the start, a swap chain of its own. This script runs it on an isolated headless compositor, captures the output
with grim and requires a red and a green square of the children's size, side by side at the distance the program placed them: each
window shows its own swap chain, and the one made while hidden appears once the window shows.

usage: native-d3d11-children.py RAVEN CHILDREN_EXE DXVK_PE64_DIRECTORY WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

from PIL import Image

SIDE = 160
RUNTIME = Path(f"/run/user/{os.getuid()}/raven-labwc-runtime")


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
    env = environment() | {"WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1"}
    process = subprocess.Popen(["labwc"], env=env, stdout=log, stderr=log)
    for _ in range(50):
        if (RUNTIME / "wayland-0").exists():
            time.sleep(0.5)
            return process
        time.sleep(0.1)
    raise RuntimeError("isolated compositor did not start")


def square(screen, colour):
    """The top-left corner of a SIDE by SIDE square filled with the colour, give or take a unit per channel, or None."""
    def near(pixel):
        return all(abs(a - b) <= 1 for a, b in zip(pixel, colour))
    pixels = screen.load()
    for y in range(screen.height - SIDE + 1):
        for x in range(screen.width - SIDE + 1):
            if not near(pixels[x, y]):
                continue
            if all(near(pixels[x + dx, y + dy]) for dy in range(SIDE) for dx in range(SIDE)):
                return x, y
            return None
    return None


def main():
    raven, workload, dxvk, work = map(Path, sys.argv[1:5])
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d11.dll", "dxgi.dll", "d3d10core.dll"):
        shutil.copy(dxvk / name, run / name)
    shutil.copy(workload, run / "children.exe")
    with open(work / "compositor.log", "wb") as log:
        compositor = start_compositor(log)
        env = os.environ | environment() | {"RAVEN_PREFIX": str(prefix)}
        env["PATH"] = os.environ["PATH"]
        lavapipe = Path("/usr/share/vulkan/icd.d/lvp_icd.json")
        if "VK_DRIVER_FILES" not in env and lavapipe.exists():
            env["VK_DRIVER_FILES"] = str(lavapipe)
        process = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=100000", "--desktop=wayland", str(run / "children.exe")],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        status = prefix / "drive_c" / "children.txt"
        try:
            deadline = time.time() + 90
            while time.time() < deadline and process.poll() is None and not status.exists():
                time.sleep(0.5)
            if not status.exists() or status.read_text() != "OK":
                raise SystemExit(f"workload did not present to both children: {status.read_text() if status.exists() else 'missing'}")
            time.sleep(1)
            capture = work / "capture.png"
            subprocess.run(["grim", str(capture)], env=environment(), check=True, timeout=20)
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
            if compositor:
                compositor.terminate()
    screen = Image.open(capture).convert("RGB")
    red, green = square(screen, (204, 26, 26)), square(screen, (26, 178, 51))
    print(f"red square at {red}, green square at {green}")
    if not red:
        raise SystemExit("the child whose swap chain was made while hidden shows nothing")
    if not green:
        raise SystemExit("the second child of the window shows nothing")
    if (green[0] - red[0], green[1] - red[1]) != (SIDE + 20, 0):
        raise SystemExit("the children's swap chains are not where the children are")
    print("two child windows of one window each show their own swap chain, including the one made while it was hidden")


if __name__ == "__main__":
    main()
