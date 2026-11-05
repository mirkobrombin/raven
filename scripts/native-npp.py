#!/usr/bin/env python3
"""Drive the unmodified Notepad++ through Raven on an isolated headless compositor across three fresh processes.

Session 1 creates a document and saves it through the Save As dialog, session 2 reopens the saved file on its own, appends a line and
saves with the keyboard, session 3 reopens it again and reads the editor contents back through the host clipboard. Every check is
something the compositor displays (window titles, screenshots), a file in the prefix or text another client receives.

The desktop is an isolated labwc by default; x11 runs the same sessions on the experimental X11 backend, on Xvfb with Openbox.

usage: native-npp.py RAVEN NOTEPAD_DIRECTORY EVIDENCE_DIR [wayland|x11]
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

from PIL import Image, ImageChops

from x11_session import X11Session

RUNTIME = Path(f"/run/user/{os.getuid()}/raven-labwc-runtime")
FIRST = b"Session one line\r\nsecond line, saved by session 1"
SECOND = FIRST + b"\r\nappended by session 2"
# The X server of an x11 run; None on Wayland.
x11 = None


def environment():
    if x11:
        return {"PATH": "/usr/bin:/bin"} | x11.environment()
    return {"PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": str(RUNTIME), "WAYLAND_DISPLAY": "wayland-0"}


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


def titles():
    if x11:
        probe = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "."], env=environment(), capture_output=True, text=True, timeout=10)
        names = (subprocess.run(["xdotool", "getwindowname", window], env=environment(), capture_output=True, text=True, timeout=10)
                 for window in probe.stdout.split())
        return [name.stdout.strip() for name in names if name.returncode == 0]
    probe = subprocess.run(["wlrctl", "toplevel", "list"], env=environment(), capture_output=True, text=True, timeout=10)
    return [line.split(": ", 1)[1] for line in probe.stdout.splitlines() if ": " in line]


def wait_for(description, condition, timeout=240):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(0.5)
    raise RuntimeError(f"timed out waiting for {description}; windows: {titles()}")


def screenshot(path):
    if x11:
        return x11.capture(path)
    subprocess.run(["grim", str(path)], env=environment(), check=True, timeout=20)
    return Image.open(path).convert("RGB")


def window_box(image):
    box = image.point(lambda value: 255 if value > 8 else 0).convert("L").getbbox()
    if not box:
        raise RuntimeError("no window is visible on the isolated compositor")
    return box


def click(x, y):
    if x11:
        subprocess.run(["xdotool", "mousemove", str(x), str(y)], env=environment(), check=True, timeout=10)
        time.sleep(0.3)
        subprocess.run(["xdotool", "click", "1"], env=environment(), check=True, timeout=10)
        time.sleep(0.8)
        return
    # wlrctl moves relatively: park the pointer in the corner, then travel to the absolute position.
    subprocess.run(["wlrctl", "pointer", "move", "-4000", "-4000"], env=environment(), check=True, timeout=10)
    subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=environment(), check=True, timeout=10)
    time.sleep(0.3)
    subprocess.run(["wlrctl", "pointer", "click", "left"], env=environment(), check=True, timeout=10)
    time.sleep(0.8)


def keys(text):
    if x11:
        subprocess.run(["xdotool", "type", "--delay", "50", text], env=environment(), check=True, timeout=30)
        time.sleep(0.5)
        return
    wtype(text)


def press(combination):
    """Presses a key with its modifiers, written as xdotool names them: ctrl+End."""
    if x11:
        subprocess.run(["xdotool", "key", combination], env=environment(), check=True, timeout=30)
        time.sleep(0.5)
        return
    *modifiers, key = combination.split("+")
    wtype(*(argument for modifier in modifiers for argument in ("-M", modifier)), "-k", key,
          *(argument for modifier in modifiers for argument in ("-m", modifier)))


def wtype(*arguments):
    # wtype creates a virtual keyboard per call; the delay lets the compositor finish its focus handover so no key is lost.
    subprocess.run(["wtype", "-s", "500", *arguments], env=environment(), check=True, timeout=30)
    time.sleep(0.5)


def clipboard():
    if x11:
        probe = subprocess.run(["xclip", "-o", "-selection", "clipboard"], env=environment(), capture_output=True, timeout=10)
        return probe.stdout if probe.returncode == 0 else None
    probe = subprocess.run(["wl-paste", "-n"], env=environment(), capture_output=True, timeout=10)
    return probe.stdout if probe.returncode == 0 else None


def main_window(expected):
    return next((title for title in titles() if title.endswith("Notepad++") and expected(title)), None)


def start(raven, program, prefix, evidence, name):
    output = (evidence / f"{name}.log").open("wb")
    env = environment() | {"RAVEN_PREFIX": str(prefix)}
    # The visual style the run asks for reaches the program, so the classic look can be checked as well as the default one.
    if "RAVEN_THEME" in os.environ:
        env["RAVEN_THEME"] = os.environ["RAVEN_THEME"]
    desktop = "--desktop=x11" if x11 else "--desktop=wayland"
    return subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=600000", desktop, str(program)],
                            env=env, stdout=output, stderr=output)


def same_window(left, right):
    """Equal apart from something as small as a blinking caret."""
    changed = ImageChops.difference(left, right).convert("L").point(lambda value: 255 if value > 16 else 0).getbbox()
    return changed is None or (changed[2] - changed[0]) * (changed[3] - changed[1]) <= 40 * 40


def settle(evidence, name):
    """Wait until the window stops changing, then return its screenshot and bounding box."""
    previous = None
    stable = 0
    deadline = time.time() + 180
    while time.time() < deadline:
        image = screenshot(evidence / f"{name}.png")
        # A window that is mapped but has not drawn yet leaves the screen black; that is not a settled frame.
        drawn = image.point(lambda value: 255 if value > 8 else 0).convert("L").getbbox() is not None
        stable = stable + 1 if drawn and previous is not None and same_window(image, previous) else 0
        previous = image
        if stable >= 3:
            return image, window_box(image)
        time.sleep(1)
    raise RuntimeError("the window never settled")


def close(process, box):
    # The caption button of the compositor frame ends the process cleanly.
    click(box[2] - 14, box[1] + 14)
    process.wait(timeout=180)
    if process.returncode != 0:
        raise RuntimeError(f"Notepad++ ended with status {process.returncode}")


def main():
    global x11
    raven, source, evidence = map(Path, sys.argv[1:4])
    shutil.rmtree(evidence, ignore_errors=True)
    evidence.mkdir(parents=True)
    if sys.argv[4:] == ["x11"]:
        with X11Session(evidence / "x11") as x11:
            workflow(raven, source, evidence)
    else:
        workflow(raven, source, evidence)


def workflow(raven, source, evidence):
    program_directory = evidence / "notepad"
    shutil.copytree(source, program_directory)
    program = program_directory / "notepad++.exe"
    prefix = evidence / "prefix"
    saved = prefix / "drive_c" / "persist.txt"
    log = (evidence / "compositor.log").open("wb")
    compositor = None if x11 else start_compositor(log)
    process = None
    try:
        # Session 1: a new document typed through the keyboard, saved with the Save As dialog.
        process = start(raven, program, prefix, evidence, "session1")
        wait_for("the editor window", lambda: main_window(lambda title: "new 1" in title))
        image, box = settle(evidence, "01-start")
        click(box[0] + 560, box[1] + 260)
        keys("Session one line")
        press("Return")
        keys("second line, saved by session 1")
        press("ctrl+s")
        wait_for("the Save As dialog", lambda: "Save As" in titles())
        time.sleep(1.5)
        keys("persist")
        press("Return")
        wait_for("the saved file", lambda: saved.exists() and saved.read_bytes() == FIRST)
        wait_for("the saved title", lambda: main_window(lambda title: "persist.txt" in title and not title.startswith("*")))
        after, box = settle(evidence, "02-saved")
        if same_window(after, image):
            raise RuntimeError("typing did not change the editor")
        close(process, box)

        # Session 2: the file comes back by itself; append through the keyboard and save without a dialog.
        process = start(raven, program, prefix, evidence, "session2")
        wait_for("the reopened document", lambda: main_window(lambda title: "persist.txt" in title))
        reopened, box = settle(evidence, "03-reopened")
        click(box[0] + 560, box[1] + 260)
        press("ctrl+End")
        press("Return")
        keys("appended by session 2")
        press("ctrl+s")
        wait_for("the second save", lambda: saved.read_bytes() == SECOND)
        wait_for("the clean title", lambda: main_window(lambda title: "persist.txt" in title and not title.startswith("*")))
        _, box = settle(evidence, "04-appended")
        close(process, box)

        # Session 3: select everything and read it back through the host clipboard, which carries LF line ends.
        process = start(raven, program, prefix, evidence, "session3")
        wait_for("the document again", lambda: main_window(lambda title: "persist.txt" in title))
        _, box = settle(evidence, "05-third")
        click(box[0] + 560, box[1] + 260)
        press("ctrl+a")
        press("ctrl+c")
        expected = SECOND.replace(b"\r\n", b"\n")
        wait_for("the editor text on the clipboard", lambda: clipboard() == expected, timeout=30)
        close(process, box)
        if saved.read_bytes() != SECOND:
            raise RuntimeError("the file changed during the last session")

        # Session 4: switching to dark mode inside the open Preferences dialog re-themes that dialog as well as the main window.
        process = start(raven, program, prefix, evidence, "session4")
        wait_for("the document for the theme switch", lambda: main_window(lambda title: "persist.txt" in title))
        _, box = settle(evidence, "06-before-theme")
        click(box[0] + 313, box[1] + 39)
        click(box[0] + 343, box[1] + 59)
        wait_for("the Preferences dialog", lambda: "Preferences" in titles())
        time.sleep(2)
        click(box[0] + 160, box[1] + 235)
        time.sleep(1)
        probe = (box[0] + 810, box[1] + 500)
        light = screenshot(evidence / "07-preferences-light.png")
        if min(light.getpixel(probe)) < 200:
            raise RuntimeError(f"the Preferences dialog is not light before the switch: {light.getpixel(probe)}")
        click(box[0] + 348, box[1] + 225)
        deadline = time.time() + 90
        while True:
            time.sleep(2)
            dark = screenshot(evidence / "08-preferences-dark.png")
            if max(dark.getpixel(probe)) <= 90:
                break
            if time.time() > deadline:
                raise RuntimeError(f"the open Preferences dialog kept its light background: {dark.getpixel(probe)}")
        close(process, box)
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        if compositor:
            compositor.kill()
            compositor.wait()
    print("Notepad++ created, saved, reopened, edited and read back the same file across three fresh processes")


if __name__ == "__main__":
    main()
