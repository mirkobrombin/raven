#!/usr/bin/env python3
"""Show a maximized window whose sizing border lies past the edges of the work area, on Wayland with a panel.

testdata/native_loader/maximized_border_client.c starts maximized and places its border past the edges of the work area it is told of,
as Windows and Chromium do. The compositor keeps a strip at the bottom of the screen free, as for a panel, and must show the window's
client area when it starts. A click on the centre of the screen must reach the window at that point,
which the program checks, and open its menu there; once the menu closes, the window restores itself, and must show again once the
compositor maximizes it.

usage: native-maximized-border.py RAVEN CLIENT WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

WINDOW = "title:maximized border"
PANEL = 48
COLOR = bytes((32, 128, 224))
FAILURES = {
    3: "the window is not where it placed itself",
    20: "the click on the screen did not reach the window",
    21: "the click reached the window at another point of the screen",
    22: "the work area the program is told of does not leave out the panel",
}


def run(env, *command):
    return subprocess.run(list(command), env=env, capture_output=True, timeout=10)


def screen(env, offsets=((0, 0),)):
    """The colours at these offsets from the centre of the screen and near the bottom left corner of the work area, which only a
    maximized window covers, and the screen's size."""
    image = run(env, "grim", "-t", "ppm", "-").stdout
    kind, size, _, pixels = (image.split(b"\n", 3) + [b""] * 4)[:4]
    if kind != b"P6":
        raise SystemExit("grim gave no screen image")
    width, height = map(int, size.split())
    points = [(width // 2 + x, height // 2 + y) for x, y in offsets] + [(20, height - PANEL - 20)]
    return [pixels[(y * width + x) * 3:(y * width + x) * 3 + 3] for x, y in points], width, height


def shows(env, step):
    deadline = time.monotonic() + 20
    while run(env, "wlrctl", "toplevel", "find", WINDOW).returncode or screen(env)[0] != [COLOR, COLOR]:
        if time.monotonic() > deadline:
            raise SystemExit(f"the maximized window does not show on the compositor {step}")
        time.sleep(0.2)


def main():
    raven, client, work = (Path(argument).resolve() for argument in sys.argv[1:4])
    work.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="wl-", dir=os.environ.get("TMPDIR", str(work))))
    env = os.environ | {"XDG_RUNTIME_DIR": str(runtime), "WAYLAND_DISPLAY": "wayland-0",
                        "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1",
                        "RAVEN_PREFIX": str(work / "prefix")}
    for key in ("DISPLAY", "MANGOHUD", "LD_PRELOAD"):
        env.pop(key, None)
    # The compositor reads its configuration from here, not the user's: a margin stands for a panel.
    (work / "labwc").mkdir(exist_ok=True)
    (work / "labwc" / "rc.xml").write_text(f'<?xml version="1.0"?>\n<labwc_config><margin bottom="{PANEL}"/></labwc_config>\n')
    compositor = program = None
    try:
        with (work / "compositor.log").open("wb") as log, (work / "program.log").open("w") as program_log:
            compositor = subprocess.Popen(["labwc", "-C", str(work / "labwc")], env=env, stdout=log, stderr=log)
            deadline = time.monotonic() + 10
            while not (runtime / "wayland-0").exists():
                if compositor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("isolated compositor did not start")
                time.sleep(0.05)
            program = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=60000", "--desktop=wayland", str(client)],
                                       env=env, stdout=program_log, stderr=subprocess.STDOUT)
            shows(env, "when it starts")
            _, width, height = screen(env)
            run(env, "wlrctl", "pointer", "move", "-10000", "-10000")
            run(env, "wlrctl", "pointer", "move", str(width // 2), str(height // 2))
            # The headless compositor has a pointer only since wlrctl made one: Raven takes it with its next poll.
            time.sleep(0.5)
            run(env, "wlrctl", "pointer", "click", "left")
            # The menu's top left corner lies at the click: the window shows just above and left of it, the menu just below and right.
            deadline = time.monotonic() + 20
            while (colours := screen(env, ((-4, -4), (4, 4)))[0])[0] != COLOR or colours[1] == COLOR:
                if program.poll() is not None or time.monotonic() > deadline:
                    raise SystemExit("the menu did not open where the click was")
                time.sleep(0.2)
            run(env, "wlrctl", "pointer", "move", "-200", "-200")
            run(env, "wlrctl", "pointer", "click", "left")
            deadline = time.monotonic() + 20
            while screen(env)[0][1] == COLOR:
                if program.poll() is not None or time.monotonic() > deadline:
                    raise SystemExit("the window did not restore itself after the click")
                time.sleep(0.2)
            run(env, "wlrctl", "toplevel", "maximize", WINDOW)
            shows(env, "after the compositor maximized it")
            run(env, "wlrctl", "toplevel", "close", WINDOW)
            program.wait(timeout=30)
        output = (work / "program.log").read_text(errors="replace")
        if "Native entry returned 0 " not in output:
            code = next((int(word) for line in output.splitlines() if line.startswith("Native entry returned ") for word in line.split()[3:4]), None)
            raise SystemExit(FAILURES.get(code, f"the program ended with {code}: {output[-500:]!r}"))
        print("the maximized window showed with its border past the edges of the work area, and a click and its menu reached it where it was")
    finally:
        for process in (program, compositor):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        shutil.rmtree(runtime)


if __name__ == "__main__":
    main()
