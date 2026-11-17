#!/usr/bin/env python3
"""Check that a layered window drawn through UpdateLayeredWindow reaches the screen with its alpha on Wayland.

testdata/native_loader/layered_window_client.c shows a 240x160 popup whose left half is opaque magenta and whose right half is fully
transparent, over a larger window painted green. The capture must hold the magenta half whole, and the green in the right half. With
"hidden" the program hands the pixels over before it shows the window.

usage: native-layered-window.py RAVEN CLIENT WORKDIR [hidden]
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image

WIDTH, HEIGHT = 240, 160
MAGENTA, GREEN = (255, 0, 255), (0, 255, 0)


def main():
    raven, client, work = (Path(argument).resolve() for argument in sys.argv[1:4])
    work.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="wl-", dir=os.environ.get("TMPDIR", str(work))))
    env = os.environ | {"XDG_RUNTIME_DIR": str(runtime), "WAYLAND_DISPLAY": "wayland-0",
                        "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1",
                        "RAVEN_PREFIX": str(work / "prefix"), "RAVEN_TRACE_DEBUG": "1"}
    for key in ("MANGOHUD", "LD_PRELOAD"):
        env.pop(key, None)
    guest = compositor = None
    try:
        with (work / "compositor.log").open("wb") as compositor_log, (work / "guest.log").open("wb") as log:
            compositor = subprocess.Popen(["labwc"], env=env, stdout=compositor_log, stderr=compositor_log)
            deadline = time.monotonic() + 10
            while not (runtime / "wayland-0").exists():
                if compositor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("isolated compositor did not start")
                time.sleep(0.05)
            subprocess.run(["wlr-randr", "--output", "HEADLESS-1", "--custom-mode", "1280x720@60Hz"], env=env, check=True, timeout=10)
            guest = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=20000", "--desktop=wayland",
                                      str(client), "--", *sys.argv[4:5]], env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 20
            while "Layered window updated" not in (work / "guest.log").read_text(errors="replace"):
                if guest.poll() is not None:
                    raise RuntimeError(f"layered workload exited with status {guest.returncode}: " + (work / "guest.log").read_text(errors="replace"))
                if time.monotonic() > deadline:
                    raise RuntimeError("layered workload did not update its window")
                time.sleep(0.05)
            time.sleep(0.5)
            capture = work / "screen.png"
            subprocess.run(["grim", str(capture)], env=env, check=True, timeout=10)
            image = Image.open(capture).convert("RGB")
            # The window goes where the compositor puts it, so it is found by its magenta.
            found = [(x, y) for y in range(0, image.height, 2) for x in range(0, image.width, 2) if image.getpixel((x, y)) == MAGENTA]
            if not found:
                raise RuntimeError("no magenta on the screen: the layered window was not drawn")
            left, top = min(x for x, _ in found), min(y for _, y in found)
            half = image.crop((left, top, left + WIDTH // 2, top + HEIGHT))
            opaque = sum(count for count, pixel in half.getcolors(half.width * half.height) if pixel == MAGENTA)
            if opaque < half.width * half.height * 0.95:
                raise RuntimeError(f"only {opaque} of {half.width * half.height} pixels of the opaque half are magenta")
            clear = image.crop((left + WIDTH // 2 + 4, top + 4, left + WIDTH - 4, top + HEIGHT - 4))
            covered = sum(count for count, pixel in clear.getcolors(clear.width * clear.height) if pixel != GREEN)
            if covered:
                raise RuntimeError(f"{covered} pixels of the transparent half hide the green window under it")
            if guest.wait(timeout=15):
                raise RuntimeError("layered workload cleanup failed")
            print(f"Layered window verified at {left},{top}: opaque half magenta, the window under it seen through the other")
    finally:
        for process in (guest, compositor):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        shutil.rmtree(runtime)


if __name__ == "__main__":
    main()
