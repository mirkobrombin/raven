#!/usr/bin/env python3
"""Draw the same window of built-in controls with a 64-bit and a 32-bit program on Raven and require identical pixels.

testdata/native_loader/controls_probe.c opens a window with a static label, a push button, a check box, an edit field and a group box. The
32-bit build runs on the 32-bit runtime and the 64-bit build on the 64-bit one, each beside its own copies of the common controls and user32
components. This script runs both on an isolated headless compositor, captures each window with grim and requires that the two pictures are
equal pixel for pixel and that they show text (a window of blank controls would also be equal).

usage: native-controls-probe.py RAVEN PROBE64_EXE PROBE32_EXE WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

from PIL import Image, ImageChops

RUNTIME = Path(os.environ.get("RAVEN_TEST_RUNTIME") or f"/run/user/{os.getuid()}") / "raven-labwc-runtime"


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


def titles():
    probe = subprocess.run(["wlrctl", "toplevel", "list"], env=environment(), capture_output=True, text=True, timeout=10)
    return [line.split(": ", 1)[1] for line in probe.stdout.splitlines() if ": " in line]


def capture(raven, program, prefix, picture):
    env = os.environ | environment() | {"RAVEN_PREFIX": str(prefix)}
    env["PATH"] = os.environ["PATH"]
    process = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=120000", "--desktop=wayland", str(program)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.time() + 120
        while time.time() < deadline and "Controls" not in titles():
            if process.poll() is not None:
                raise SystemExit(f"{program.name} ended before its window appeared: {process.stderr.read().decode()[-300:]!r}")
            time.sleep(0.3)
        else:
            if "Controls" not in titles():
                raise SystemExit(f"{program.name} showed no window")
        time.sleep(4)
        subprocess.run(["grim", str(picture)], env=environment(), check=True, timeout=20)
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
    image = Image.open(picture).convert("RGB")
    box = image.point(lambda value: 255 if value > 8 else 0).convert("L").getbbox()
    if not box:
        raise SystemExit(f"{program.name}: the compositor output is empty")
    return image.crop(box)


def main():
    raven, probe64, probe32, work = map(Path, sys.argv[1:5])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    with open(work / "compositor.log", "wb") as log:
        compositor = start_compositor(log)
        try:
            wide = capture(raven, probe64, work / "prefix64", work / "probe64.png")
            narrow = capture(raven, probe32, work / "prefix32", work / "probe32.png")
        finally:
            if compositor:
                compositor.terminate()
    if wide.size != narrow.size:
        raise SystemExit(f"the windows differ in size: {wide.size} against {narrow.size}")
    different = sum(1 for value in ImageChops.difference(wide, narrow).convert("L").point(lambda v: 255 if v > 8 else 0).getdata() if value)
    dark = sum(1 for pixel in wide.getdata() if max(pixel) < 90)
    print(f"64-bit and 32-bit windows of {wide.size[0]}x{wide.size[1]}: {different} differing pixels, {dark} dark pixels (text and lines)")
    if different:
        raise SystemExit("the 32-bit controls are not drawn like the 64-bit ones")
    if dark < 400:
        raise SystemExit("the windows show no text")
    print("the built-in controls draw identically for 32-bit and 64-bit programs")


if __name__ == "__main__":
    main()
