#!/usr/bin/env python3
"""Drive the unmodified Windows Minesweeper through Raven on an isolated headless compositor.

Every check reads what the compositor actually displays (grim screenshots) or what the process
wrote to the prefix; nothing is taken from Raven's own state.

usage: native-minesweeper.py RAVEN WINMINE.EXE EVIDENCE_DIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

from PIL import Image

RUNTIME = Path(os.environ.get("RAVEN_TEST_RUNTIME") or f"/run/user/{os.getuid()}") / "raven-labwc-runtime"


def environment():
    env = {"PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": str(RUNTIME), "WAYLAND_DISPLAY": "wayland-0"}
    return env


def start_compositor(log):
    """Start a headless labwc that never touches the user's own session."""
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


def screenshot(path):
    subprocess.run(["grim", str(path)], env=environment(), check=True, timeout=20)
    return Image.open(path).convert("RGB")


def window_box(image):
    """Bounding box of everything that is not the compositor's black background."""
    box = image.point(lambda value: 255 if value > 8 else 0).convert("L").getbbox()
    if not box:
        raise RuntimeError("no window is visible on the isolated compositor")
    return box


def pointer(x, y):
    # wlrctl moves relatively: park the pointer in the corner, then travel to the absolute position.
    subprocess.run(["wlrctl", "pointer", "move", "-4000", "-4000"], env=environment(), check=True, timeout=10)
    subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=environment(), check=True, timeout=10)
    time.sleep(0.3)


def click(x, y):
    pointer(x, y)
    subprocess.run(["wlrctl", "pointer", "click", "left"], env=environment(), check=True, timeout=10)
    time.sleep(0.8)


def wait_for_window(evidence, name, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            image = screenshot(evidence / name)
            return image, window_box(image)
        except RuntimeError:
            time.sleep(0.5)
    raise RuntimeError("window never appeared")


def crop_equal(left, right, box):
    return left.crop(box).tobytes() == right.crop(box).tobytes()


def main():
    raven, program, evidence = map(Path, sys.argv[1:])
    evidence.mkdir(parents=True, exist_ok=True)
    prefix = evidence / "prefix"
    shutil.rmtree(prefix, ignore_errors=True)
    log = (evidence / "compositor.log").open("wb")
    compositor = start_compositor(log)
    env = environment() | {"RAVEN_PREFIX": str(prefix)}
    # The visual style the run asks for reaches the program, so the classic look can be checked as well as the default one.
    if "RAVEN_THEME" in os.environ:
        env["RAVEN_THEME"] = os.environ["RAVEN_THEME"]
    output = (evidence / "raven.log").open("wb")
    process = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=180000",
                                "--desktop=wayland", str(program)], env=env, stdout=output, stderr=output)
    try:
        start, box = wait_for_window(evidence, "01-start.png")
        time.sleep(2)
        start = screenshot(evidence / "01-start.png")
        box = window_box(start)
        left, top = box[0], box[1]
        width, height = box[2] - box[0], box[3] - box[1]
        # Beginner window: 170x258 outer, a 9x9 field starting 24 px from the left and 112 px from the top.
        if (width, height) != (170, 258):
            raise RuntimeError(f"unexpected window size {width}x{height}")
        # Pixel facts independent of Raven: the smiley's yellow face and the red LED digits.
        face = start.getpixel((left + 86, top + 74))
        if not (face[0] > 200 and face[1] > 200 and face[2] < 80):
            raise RuntimeError(f"smiley face is not yellow: {face}")
        led = [start.getpixel((left + 14 + dx, top + 74)) for dx in range(0, 48)]
        if not any(pixel[0] > 200 and pixel[1] < 40 and pixel[2] < 40 for pixel in led):
            raise RuntimeError("no red LED segments are lit")

        click(left + 32, top + 120)
        time.sleep(2)
        after_click = screenshot(evidence / "02-first-click.png")
        if crop_equal(start, after_click, (left + 12, top + 100, left + width - 12, top + height - 12)):
            raise RuntimeError("clicking a cell did not change the minefield")
        if crop_equal(start, after_click, (left + 120, top + 62, left + 160, top + 84)):
            raise RuntimeError("the elapsed-time counter did not start")

        # F2 is an accelerator for New game: the revealed cell must be covered again and the timer reset.
        # wtype creates a virtual keyboard per call; waiting lets the compositor finish its focus handover so the press is not reported as already held.
        subprocess.run(["wtype", "-s", "400", "-k", "F2"], env=environment(), check=True, timeout=10)
        time.sleep(1.5)
        after_f2 = screenshot(evidence / "02b-after-f2.png")
        if not crop_equal(start, after_f2, (left + 12, top + 100, left + width - 12, top + height - 12)):
            raise RuntimeError("F2 did not start a fresh game")

        click(left + 24, top + 36)
        menu_image = screenshot(evidence / "03-game-menu.png")
        if crop_equal(after_f2, menu_image, (left, top + 56, left + 160, top + 250)):
            raise RuntimeError("the Game menu did not open")
        # Intermediate is the third entry of the Game menu's difficulty group; Windows 11 menus space their items wider than classic ones.
        click(left + 30, top + (100 if os.environ.get("RAVEN_THEME") == "classic" else 120))
        time.sleep(2)
        resized, box2 = wait_for_window(evidence, "04-intermediate.png")
        width2, height2 = box2[2] - box2[0], box2[3] - box2[1]
        if width2 <= width or height2 <= height:
            raise RuntimeError(f"Intermediate did not enlarge the window ({width2}x{height2})")
        # Close with the caption button, which must end the process cleanly.
        click(box2[2] - 14, box2[1] + 14)
        process.wait(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if compositor:
            compositor.kill()
            compositor.wait()
    hive = (prefix / "registry.dat").read_text()
    key = "HKEY_CURRENT_USER\\\\Software\\\\Microsoft\\\\winmine"
    if f"V\t{key}\tDifficulty\t4\t01000000" not in hive.splitlines():
        raise RuntimeError("the chosen difficulty was not persisted in the registry")
    if process.returncode not in (0, None):
        raise RuntimeError(f"process ended with status {process.returncode}")
    print("Minesweeper started, drew its UI, accepted pointer and menu input, resized, closed and persisted its setting")


if __name__ == "__main__":
    main()
