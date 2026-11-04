#!/usr/bin/env python3
"""Verify normal GDI/WGL calls, pixel readback and the compositor's displayed frame.

usage: native-opengl.py RAVEN CLIENT_EXE MESA_PE64_DIRECTORY|native WORKDIR [gpu]
"""

from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image
from lavapipe import icd as lavapipe_icd


def main():
    raven, client, work = (Path(sys.argv[index]).resolve() for index in (1, 2, 4))
    native = sys.argv[3] == "native"
    gpu = "gpu" in sys.argv[5:]
    maximize = "maximize" in sys.argv[5:]
    invalid_array = next((option for option in sys.argv[5:] if option.startswith(("array-", "bad-element-")) or option == "forged-elements"), None)
    options = (["--maximize"] if maximize else []) + (["--" + invalid_array] if invalid_array else [])
    if "queued-elements" in sys.argv[5:]:
        options.append("--queued-elements")
    work.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="wl-", dir=os.environ.get("TMPDIR", str(work))))
    env = os.environ | {"XDG_RUNTIME_DIR": str(runtime), "WAYLAND_DISPLAY": "wayland-0",
                        "WLR_BACKENDS": "headless", "WLR_RENDERER": "vulkan" if gpu else "pixman",
                        "WLR_HEADLESS_OUTPUTS": "1", "RAVEN_PREFIX": str(work / "prefix"),
                        "RAVEN_TRACE_DEBUG": "1"}
    if native:
        env["RAVEN_NATIVE_OPENGL"] = "1"
        env.pop("RAVEN_OPENGL_DIRECTORY", None)
    else:
        env["RAVEN_OPENGL_DIRECTORY"] = str(Path(sys.argv[3]).resolve())
        env["RAVEN_GUEST_GALLIUM_DRIVER"] = "zink"
        env.pop("RAVEN_NATIVE_OPENGL", None)
    for key in ("MANGOHUD", "LIBGL_ALWAYS_SOFTWARE", "GALLIUM_DRIVER", "RAVEN_GUEST_LIBGL_ALWAYS_SOFTWARE"):
        env.pop(key, None)
    if not gpu:
        if native:
            env["LIBGL_ALWAYS_SOFTWARE"] = "1"
        env["RAVEN_GUEST_LIBGL_ALWAYS_SOFTWARE"] = "1"
        lavapipe = lavapipe_icd()
        if lavapipe.exists():
            env["VK_DRIVER_FILES"] = str(lavapipe)
    guest = compositor = None
    try:
        with (work / "compositor.log").open("wb") as compositor_log, (work / "guest.log").open("wb") as log:
            compositor = subprocess.Popen(["labwc"], env=env, stdout=compositor_log, stderr=compositor_log)
            deadline = time.monotonic() + 10
            while not (runtime / "wayland-0").exists():
                if compositor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("isolated compositor did not start")
                time.sleep(0.05)
            guest = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=30000",
                                      "--desktop=wayland", "--stdio=inherit", str(client),
                                      *(["--", *options] if options else [])], env=env,
                                     stdout=log, stderr=subprocess.STDOUT)
            if invalid_array:
                status = guest.wait(timeout=30)
                text = (work / "guest.log").read_text(errors="replace")
                if invalid_array == "forged-elements":
                    expected = "queued OpenGL indexed draw has no element buffer"
                elif invalid_array.startswith("bad-element-"):
                    expected = "OpenGL element " + invalid_array.removeprefix("bad-element-") + " exceeds"
                else:
                    expected = r"OpenGL draw uses an uncopied (client array|texture array|vertex attribute)"
                if status != 1 or not re.search(expected, text):
                    raise RuntimeError(f"unsafe OpenGL draw was not rejected: status {status}, {text[-1000:]}")
                print("Unsafe OpenGL draw rejected before host drawing")
                return
            deadline = time.monotonic() + 30
            while "OpenGL frame presented" not in (work / "guest.log").read_text(errors="replace"):
                status = guest.poll()
                if status is not None:
                    raise RuntimeError(f"OpenGL workload exited with status {status} before presentation")
                if time.monotonic() > deadline:
                    raise RuntimeError("OpenGL workload did not present a frame within 30 seconds")
                time.sleep(0.05)
            time.sleep(0.2)
            capture = work / "screen.png"
            subprocess.run(["grim", str(capture)], env=env, check=True, timeout=10)
            image = Image.open(capture).convert("RGB")
            colors = image.getcolors(image.width * image.height)
            if sum(count for count, pixel in colors if pixel == (255, 0, 0)) < 32 * 32:
                raise RuntimeError("the compositor did not display the red OpenGL frame")
            if maximize:
                if sum(count for count, pixel in colors if pixel == (255, 0, 0)) < image.width * image.height * 0.75:
                    raise RuntimeError("maximized OpenGL surface does not cover the display")
                for x in (0, 1):
                    for y in (0, 1):
                        quadrant = image.crop((x * image.width // 2, y * image.height // 2,
                                               (x + 1) * image.width // 2, (y + 1) * image.height // 2))
                        if sum(count for count, pixel in quadrant.getcolors(quadrant.width * quadrant.height) if pixel == (0, 255, 0)) < 32:
                            raise RuntimeError("a maximized OpenGL corner is clipped")
            if guest.wait(timeout=10):
                raise RuntimeError("OpenGL cleanup failed")
            text = (work / "guest.log").read_text(errors="replace")
            if native and env.get("RAVEN_QUEUED_GRAPHICS") and env.get("RAVEN_PROFILE"):
                queued = re.findall(r"profile: queued calls=(\d+)", text)
                if not queued:
                    raise RuntimeError("OpenGL queue counters were not reported")
                active = any(int(count) for count in queued)
                if env.get("RAVEN_QUEUED_OPENGL") and not active:
                    raise RuntimeError("OpenGL workload did not execute queued calls")
                if not env.get("RAVEN_QUEUED_OPENGL") and active:
                    raise RuntimeError("OpenGL workload queued calls without its experimental option")
            if gpu and any(name in text.lower() for name in ("llvmpipe", "lavapipe", "softpipe")):
                raise RuntimeError("GPU test used a software renderer")
            print("OpenGL readback and compositor pixels verified")
    finally:
        for process in (guest, compositor):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        shutil.rmtree(runtime)


if __name__ == "__main__":
    main()
