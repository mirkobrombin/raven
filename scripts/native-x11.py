#!/usr/bin/env python3
"""Run desktop workloads on the experimental X11 backend, on an isolated Xvfb server with Openbox, and check what the server shows.

Modes:

- metrics: screen-metrics.exe maximizes a window painted green with red corners; the window must cover the screen and keep its corners.
- opengl, opengl-maximized: opengl-host.exe draws a red frame through the native OpenGL bridge on an EGL XCB surface; the frame must
  reach the screen, and when maximized cover it with its green corners in place.
- present: window.exe presents a Direct3D 11 swap chain through DXVK and Raven's Vulkan bridge on an XCB surface; the captured screen
  must hold the last frame the GPU read back, pixel for pixel, before and after the program resizes its window.

Xvfb renders in software and Vulkan uses lavapipe when available: these checks cover correctness, not speed.

usage: native-x11.py RAVEN CLIENT_EXE metrics|opengl|opengl-maximized|present WORKDIR [DXVK_PE64_DIRECTORY]
"""

from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import sys
import time

from x11_session import X11Session
from lavapipe import icd as lavapipe_icd

SIZE = (1280, 720)


def wait_for(predicate, process, timeout, failure):
    deadline = time.monotonic() + timeout
    while not predicate():
        if process.poll() is not None:
            raise SystemExit(f"{failure}: the workload exited with status {process.returncode}")
        if time.monotonic() > deadline:
            raise SystemExit(f"{failure} within {timeout} seconds")
        time.sleep(0.05)


def count(image, color):
    return sum(number for number, pixel in image.getcolors(image.width * image.height) if pixel == color)


def corners(image, color, label):
    for x in (0, 1):
        for y in (0, 1):
            quadrant = image.crop((x * image.width // 2, y * image.height // 2, (x + 1) * image.width // 2, (y + 1) * image.height // 2))
            if count(quadrant, color) < 32:
                raise SystemExit(f"a maximized {label} corner is clipped")


def main():
    raven, client, mode, work = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    shutil.copy(client, run / client.name)
    if mode == "present":
        for name in ("d3d11.dll", "dxgi.dll", "d3d10core.dll"):
            shutil.copy(Path(sys.argv[5]) / name, run / name)
    env = os.environ | {"RAVEN_PREFIX": str(prefix), "RAVEN_TRACE_DEBUG": "1"}
    for key in ("WAYLAND_DISPLAY", "MANGOHUD", "LD_PRELOAD"):
        env.pop(key, None)
    lavapipe = lavapipe_icd()
    if "VK_DRIVER_FILES" not in env and lavapipe.exists():
        env["VK_DRIVER_FILES"] = str(lavapipe)
    if mode.startswith("opengl"):
        env |= {"RAVEN_NATIVE_OPENGL": "1", "LIBGL_ALWAYS_SOFTWARE": "1"}
    arguments = {"metrics": ["--", *map(str, SIZE), "0", "0"], "opengl": [], "opengl-maximized": ["--", "--maximize"], "present": []}[mode]
    guest = None
    with X11Session(work / "x11", SIZE) as session, (work / "guest.log").open("wb") as log:
        env |= session.environment()
        guest = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=100000", "--desktop=x11", "--stdio=inherit",
                                  str(run / client.name), *arguments], env=env, stdout=log, stderr=subprocess.STDOUT)
        text = lambda: (work / "guest.log").read_text(errors="replace")
        try:
            if mode == "metrics":
                wait_for(lambda: "Screen metrics frame presented" in text(), guest, 30, "the maximized window was not presented")
                time.sleep(0.5)
                image = session.capture(work / "screen.png")
                if count(image, (0, 255, 0)) < image.width * image.height * 0.75:
                    raise SystemExit("the maximized window does not cover the screen")
                corners(image, (255, 0, 0), "window")
                if guest.wait(timeout=20):
                    raise SystemExit(f"screen workload failed: {text()[-1500:]}")
                print("X11: display metrics and maximized corners verified")
            elif mode.startswith("opengl"):
                wait_for(lambda: "OpenGL frame presented" in text(), guest, 30, "the OpenGL frame was not presented")
                time.sleep(0.5)
                image = session.capture(work / "screen.png")
                if count(image, (255, 0, 0)) < 32 * 32:
                    raise SystemExit("the server does not show the red OpenGL frame")
                if mode == "opengl-maximized":
                    if count(image, (255, 0, 0)) < image.width * image.height * 0.75:
                        raise SystemExit("the maximized OpenGL surface does not cover the screen")
                    corners(image, (0, 255, 0), "OpenGL")
                if guest.wait(timeout=20):
                    raise SystemExit(f"OpenGL workload failed: {text()[-1500:]}")
                print("X11: OpenGL frame on an EGL XCB surface verified")
            else:
                drive = prefix / "drive_c"
                done = lambda name: (drive / name).exists() and (drive / name).read_text() == "OK"
                wait_for(lambda: done("window.txt"), guest, 90, "the workload did not present all frames")
                time.sleep(1)
                session.capture(work / "capture.png")
                wait_for(lambda: done("window_resized.txt"), guest, 90, "the workload did not render after resizing")
                time.sleep(1)
                session.capture(work / "capture-resized.png")
        finally:
            if guest.poll() is None:
                guest.terminate()
                try:
                    guest.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    guest.kill()
    if mode == "present":
        specification = importlib.util.spec_from_file_location("present", Path(__file__).with_name("native-d3d11-present.py"))
        present = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(present)
        drive = prefix / "drive_c"
        present.compare(work / "capture.png", (drive / "window_last.raw").read_bytes(), present.SIZE, present.SIZE, "first window")
        present.compare(work / "capture-resized.png", (drive / "window_resized.raw").read_bytes(), 320, 200, "resized window")
        print("X11: a Direct3D 11 swap chain on an XCB surface matches the GPU read-back before and after a resize")


if __name__ == "__main__":
    main()
