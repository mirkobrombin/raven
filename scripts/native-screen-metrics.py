#!/usr/bin/env python3
"""Check Win32 display metrics, all four corners of a maximized window and the shade of its caption on Wayland."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image


def main():
    raven, client, work = (Path(sys.argv[index]).resolve() for index in (1, 2, 4))
    mode = sys.argv[3]
    physical, scale, transform, logical = {
        "normal": ((1280, 720), "1", "normal", (1280, 720)),
        "scaled": ((1920, 1080), "1.5", "normal", (1280, 720)),
        "rotated": ((1024, 768), "1", "90", (768, 1024)),
        "resize": ((1280, 720), "1", "normal", (1280, 720)),
        "dark": ((1280, 720), "1", "normal", (1280, 720)),
        "frames": ((1280, 720), "1", "normal", (1280, 720)),
        "colored": ((1280, 720), "1", "normal", (1280, 720)),
    }[mode]
    work.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="wl-", dir=os.environ.get("TMPDIR", str(work))))
    env = os.environ | {"XDG_RUNTIME_DIR": str(runtime), "WAYLAND_DISPLAY": "wayland-0",
                        "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1",
                        "RAVEN_PREFIX": str(work / "prefix"), "RAVEN_TRACE_DEBUG": "1"}
    for key in ("MANGOHUD", "LD_PRELOAD"):
        env.pop(key, None)
    next_size = (960, 540) if mode == "resize" else (0, 0)
    guest = compositor = None
    try:
        with (work / "compositor.log").open("wb") as compositor_log, (work / "guest.log").open("wb") as log:
            compositor = subprocess.Popen(["labwc"], env=env, stdout=compositor_log, stderr=compositor_log)
            deadline = time.monotonic() + 10
            while not (runtime / "wayland-0").exists():
                if compositor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("isolated compositor did not start")
                time.sleep(0.05)
            subprocess.run(["wlr-randr", "--output", "HEADLESS-1", "--custom-mode", f"{physical[0]}x{physical[1]}@60Hz",
                            "--scale", scale, "--transform", transform], env=env, check=True, timeout=10)
            guest = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=20000",
                                      "--desktop=wayland", "--stdio=inherit", str(client), "--",
                                      *map(str, logical), *map(str, next_size), {"dark": "1", "frames": "2", "colored": "3"}.get(mode, "0")], env=env,
                                     stdout=log, stderr=subprocess.STDOUT)
            changed = False
            deadline = time.monotonic() + 20
            while "Screen metrics frame presented" not in (work / "guest.log").read_text(errors="replace"):
                if mode == "resize" and not changed and "Screen change ready" in (work / "guest.log").read_text(errors="replace"):
                    subprocess.run(["wlr-randr", "--output", "HEADLESS-1", "--custom-mode", "960x540@60Hz"],
                                   env=env, check=True, timeout=10)
                    changed = True
                if guest.poll() is not None:
                    raise RuntimeError(f"screen workload exited with status {guest.returncode}: " + (work / "guest.log").read_text(errors="replace"))
                if time.monotonic() > deadline:
                    raise RuntimeError("screen workload did not present")
                time.sleep(0.05)
            time.sleep(0.3)
            capture = work / "screen.png"
            subprocess.run(["grim", str(capture)], env=env, check=True, timeout=10)
            image = Image.open(capture).convert("RGB")
            colors = dict((pixel, count) for count, pixel in image.getcolors(image.width * image.height))
            if colors.get((0, 255, 0), 0) < image.width * image.height * 0.75:
                raise RuntimeError("maximized client does not cover its display")
            for x in (0, 1):
                for y in (0, 1):
                    quadrant = image.crop((x * image.width // 2, y * image.height // 2,
                                           (x + 1) * image.width // 2, (y + 1) * image.height // 2))
                    if sum(count for count, pixel in quadrant.getcolors(quadrant.width * quadrant.height) if pixel == (255, 0, 0)) < 32:
                        raise RuntimeError("a maximized client corner is clipped")
            # The caption above the client area: light, or dark once the program asked for the dark frame.
            caption = image.getpixel((image.width // 3, 4))
            if mode in ("normal", "dark") and (max(caption) > 0x40 if mode == "dark" else min(caption) < 0xc0):
                raise RuntimeError(f"caption shade {caption} does not match the {mode} frame")
            # DWMWA_CAPTION_COLOR gives the caption the program's own color.
            if mode == "colored" and max(abs(a - b) for a, b in zip(caption, (0x10, 0x60, 0xa0))) > 2:
                raise RuntimeError(f"caption {caption} is not the color the program gave it")
            # One child's border is its own, drawn blue; the other's blue lies under the border drawn for it.
            blue = colors.get((0, 0, 255), 0)
            if mode == "frames" and not 2 * (100 + 40) - 4 <= blue <= 2 * (100 + 40):
                raise RuntimeError(f"{blue} blue border pixels instead of one child's own border")
            if guest.wait(timeout=10):
                raise RuntimeError("screen workload cleanup failed")
            print(f"Display metrics and maximized corners verified: {mode}")
    finally:
        for process in (guest, compositor):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        shutil.rmtree(runtime)


if __name__ == "__main__":
    main()
