#!/usr/bin/env python3
"""Show a bridged VST 3 plugin's editor in a Wayland host's window, on an isolated labwc.

native/vst/wayland_editor_test.cpp is the host: a blue window of 400 by 300 pixels whose surface the editor is attached to. Once it says
the editor is attached, the screen must show the test plugin's green, 300 by 200 pixels, at the top left corner of the blue window, which
shows on to the right and below: inside the host's window, not a window of its own. The green is then clicked in its middle; the host must
hear of the edit the click makes, and the editor must come off its window again.

usage: native-wayland-vst-editor.py WORKDIR TEST MODULE
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from PIL import Image

WIDTH, HEIGHT = 300, 200
HOST_WIDTH, HOST_HEIGHT = 400, 300
GREEN, BLUE = (0, 255, 0), (0, 0, 255)


def bounds(image, colour):
    """The box of the pixels of one colour, on every second row and column, and how many there are."""
    found = [(x, y) for y in range(0, image.height, 2) for x in range(0, image.width, 2) if image.getpixel((x, y)) == colour]
    if not found:
        return None, 0
    return (min(x for x, _ in found), min(y for _, y in found), max(x for x, _ in found) + 2, max(y for _, y in found) + 2), len(found) * 4


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    work, test, module = (Path(argument).resolve() for argument in sys.argv[1:4])
    work.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="wl-", dir=os.environ.get("TMPDIR", str(work))))
    env = os.environ | {"XDG_RUNTIME_DIR": str(runtime), "WAYLAND_DISPLAY": "wayland-0",
                        "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1"}
    # A Wayland host has no X server: the plugin shows its editor on the host's surface.
    for key in ("DISPLAY", "MANGOHUD", "LD_PRELOAD"):
        env.pop(key, None)
    host = compositor = None
    lines = []
    try:
        with (work / "compositor.log").open("wb") as compositor_log:
            compositor = subprocess.Popen(["labwc"], env=env, stdout=compositor_log, stderr=compositor_log)
            deadline = time.monotonic() + 10
            while not (runtime / "wayland-0").exists():
                if compositor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("isolated compositor did not start")
                time.sleep(0.05)
            subprocess.run(["wlr-randr", "--output", "HEADLESS-1", "--custom-mode", "1280x720@60Hz"], env=env, check=True, timeout=10)
            host = subprocess.Popen([str(test), str(module)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            attached = threading.Event()

            def follow():
                for line in host.stdout:
                    lines.append(line)
                    print(line, end="", flush=True)
                    if line.startswith("editor: attached"):
                        attached.set()
            reader = threading.Thread(target=follow, daemon=True)
            reader.start()
            if not attached.wait(60):
                raise RuntimeError("the host did not attach the editor")
            # The editor's green covers the host's window once the runtime drew it.
            capture = work / "screen.png"
            deadline = time.monotonic() + 30
            while True:
                subprocess.run(["grim", str(capture)], env=env, check=True, timeout=10)
                image = Image.open(capture).convert("RGB")
                editor, shown = bounds(image, GREEN)
                if shown >= WIDTH * HEIGHT * 0.95:
                    break
                if host.poll() is not None:
                    raise RuntimeError(f"the host ended with status {host.returncode} before the editor showed")
                if time.monotonic() > deadline:
                    raise RuntimeError(f"the editor's green does not show ({shown} pixels)")
                time.sleep(0.2)
            window, _ = bounds(image, BLUE)
            left, top, right, bottom = editor
            if (right - left, bottom - top) != (WIDTH, HEIGHT):
                raise RuntimeError(f"the editor is {right - left} by {bottom - top}, not {WIDTH} by {HEIGHT}")
            if not window or window[:2] != (left, top) or (window[2] - window[0], window[3] - window[1]) != (HOST_WIDTH, HOST_HEIGHT):
                raise RuntimeError(f"the editor at {editor} is not at the corner of the host's {HOST_WIDTH} by {HOST_HEIGHT} window ({window})")
            print(f"editor: shown at the corner of the host's window, {right - left} by {bottom - top}", flush=True)
            subprocess.run(["wlrctl", "pointer", "move", "-4000", "-4000"], env=env, check=True, timeout=10)
            subprocess.run(["wlrctl", "pointer", "move", str((left + right) // 2), str((top + bottom) // 2)], env=env, check=True, timeout=10)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=env, check=True, timeout=10)
            status = host.wait(timeout=60)
            reader.join(timeout=10)
            if status:
                raise RuntimeError(f"the host ended with status {status}")
    finally:
        for process in (host, compositor):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        shutil.rmtree(runtime)


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as error:
        sys.exit(f"native-wayland-vst-editor: {error}")
