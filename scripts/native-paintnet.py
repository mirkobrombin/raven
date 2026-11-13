#!/usr/bin/env python3
"""Install paint.net 5.2 through Raven from its own installer, and draw and save a picture in it as a person does.

In a fresh prefix the installer from the archive dotPDN publishes runs with /auto. On an isolated headless compositor paint.net starts
on the Direct3D libraries of the directory given, a stroke is drawn across the canvas with the brush, the mouse held down from its first
point to its last, and the picture is saved as PNG through the Save As dialog and the save options that follow it. The file on the host is
then read: its size, the white the canvas began with and the stroke where it was drawn.

usage: native-paintnet.py RAVEN INSTALLER_ZIP VIRTUAL_POINTER DXVK_DIRECTORY EVIDENCE_DIR
"""

from pathlib import Path
import hashlib
import importlib.util
import shutil
import subprocess
import sys
import time
import zipfile

from PIL import Image

sys.dont_write_bytecode = True


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


native_redist = load("native_redist", "native-redist.py")
# The compositor helpers the 7-Zip workflow drives its windows with.
desktop = load("native_7zip", "native-7zip.py")

INSTALLER_SHA256 = "fe79df875fab758f06dbe2b0cf1947cc135251f4d8549401bd5b8411ef5773b4"
MAIN = "Untitled - Paint.NET 5.2"
DRAWN = "*Untitled - Paint.NET 5.2"
SAVED = "stroke.png - Paint.NET 5.2"
# The stroke, from the window's corner: down to the right across the canvas and back to the left, clear of the tool windows.
STROKE = ((260, 180), (520, 300), (380, 400))


def install(raven, prefix, archive, directx, evidence):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != INSTALLER_SHA256:
        raise RuntimeError(f"{archive.name} is not the paint.net 5.2 installer dotPDN publishes")
    with zipfile.ZipFile(archive) as packed:
        setup = evidence / next(name for name in packed.namelist() if name.endswith(".exe"))
        packed.extract(setup.name, evidence)
    # The installer ends by starting paint.net itself for its last steps, which loads Direct3D as paint.net always does.
    code = native_redist.run(raven, prefix, setup, ["/auto"], evidence / "install.log", watch=evidence / "install.png", timeout=1800,
                             more={"RAVEN_D3D_DIRECTORY": str(directx)})
    if code != 0:
        raise RuntimeError(f"the paint.net installer ended with status {code:#x}")
    program = prefix / "drive_c" / "Program Files" / "Paint.NET" / "paintdotnet.exe"
    if not program.is_file():
        raise RuntimeError("the installer left no paintdotnet.exe")
    records = sorted(path.name for path in Path(f"{prefix}.apps").iterdir())
    if setup.stem not in records:
        raise RuntimeError(f"the installer was recorded as {records}, not under its name")
    # What the installer unpacked itself into is its own to clear away.
    temp = next((prefix / "drive_c" / "Users").glob("*/AppData/Local/Temp"))
    left = sorted(path.name for path in temp.iterdir() if path.name.startswith("7zS"))
    if left:
        raise RuntimeError(f"the installer left its unpacked files behind: {left}")
    return program


def start(raven, prefix, program, directx, log):
    env = native_redist.environment() | {"RAVEN_PREFIX": str(prefix), "RAVEN_D3D_DIRECTORY": str(directx)}
    return subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=900000", "--desktop=wayland", str(program)],
                            env=env, stdout=log, stderr=log)


def draw(pointer, screen, origin):
    """The brush pressed at the stroke's first point, dragged through the others and let go at the last."""
    points = [str(value) for x, y in STROKE for value in (origin[0] + x, origin[1] + y)]
    subprocess.run([str(pointer), str(screen[0]), str(screen[1]), *points], env=native_redist.environment(), check=True, timeout=60)


def save(prefix, evidence):
    """The picture saved as PNG where the Save As dialog is told, with the options the dialog after it offers."""
    desktop.press(DRAWN, "-M", "ctrl", "-k", "s", "-m", "ctrl")
    desktop.wait_title("Save As")
    time.sleep(2)
    desktop.write("C:\\stroke")
    desktop.shot(evidence, "save-as")
    desktop.keys("-k", "Return")
    desktop.wait_title("Save Options", timeout=120)
    # The options show what the file will be once they have worked it out; the file is written when they are accepted.
    time.sleep(15)
    desktop.shot(evidence, "save-options")
    desktop.press("Save Options", "-k", "Return")
    saved = prefix / "drive_c" / "stroke.png"
    desktop.wait_file(saved, timeout=180)
    desktop.wait_title(SAVED, timeout=120)
    return saved


def check(saved):
    """The file holds the white picture paint.net began with, 800 by 600, and the black stroke across it where it was drawn."""
    picture = Image.open(saved).convert("RGB")
    if picture.size != (800, 600):
        raise RuntimeError(f"the saved picture is {picture.size[0]} by {picture.size[1]}, not 800 by 600")
    pixels = picture.load()
    dark = [(x, y) for y in range(600) for x in range(800) if sum(pixels[x, y]) < 200]
    white = sum(1 for y in range(600) for x in range(800) if pixels[x, y] == (255, 255, 255))
    if len(dark) < 400:
        raise RuntimeError(f"the saved picture holds {len(dark)} pixels of the stroke, too few for the stroke drawn")
    if white < 800 * 600 - 6 * len(dark):
        raise RuntimeError(f"only {white} pixels of the saved picture are the white of the canvas")
    left, right = min(x for x, _ in dark), max(x for x, _ in dark)
    top, bottom = min(y for _, y in dark), max(y for _, y in dark)
    # The canvas is shown smaller than it is, so the stroke is wider and taller in the file than on the screen, by a known little.
    wide, high = STROKE[1][0] - STROKE[0][0], STROKE[2][1] - STROKE[0][1]
    if not (wide * 0.9 <= right - left <= wide * 1.4 and high * 0.9 <= bottom - top <= high * 1.4):
        raise RuntimeError(f"the stroke covers {left},{top} to {right},{bottom}, not the {wide} by {high} it was drawn over")
    # It runs down to the right and comes back: its rightmost point lies between its top and its bottom, not at either.
    turn = sum(y for x, y in dark if x >= right - 3) / sum(1 for x, _ in dark if x >= right - 3)
    if not top + (bottom - top) * 0.3 <= turn <= top + (bottom - top) * 0.8:
        raise RuntimeError(f"the stroke turns at row {turn:.0f}, not between its ends")


def main():
    raven, archive, pointer, directx, evidence = map(Path, sys.argv[1:6])
    shutil.rmtree(evidence, ignore_errors=True)
    shutil.rmtree(f"{evidence / 'prefix'}.apps", ignore_errors=True)
    evidence.mkdir(parents=True)
    with (evidence / "compositor.log").open("wb") as log:
        compositor = native_redist.start_compositor(log)
        try:
            prefix = evidence / "prefix"
            program = install(raven, prefix, archive, directx, evidence)
            output = (evidence / "paintnet.log").open("wb")
            # A virtual keyboard kept for the whole session: each new one wtype makes can lose its first keys before its keymap lands.
            keyboard = subprocess.Popen(["wtype", "-s", "100000000"], env=native_redist.environment())
            process = start(raven, prefix, program, directx, output)
            try:
                desktop.wait_title(MAIN, process, timeout=300)
                # Its tool windows and its first picture come after the window itself.
                time.sleep(30)
                started = desktop.shot(evidence, "started")
                origin = desktop.window_origin(started)
                draw(pointer, started.size, origin)
                desktop.wait_title(DRAWN, process, timeout=60)
                desktop.shot(evidence, "drawn")
                saved = save(prefix, evidence)
                desktop.shot(evidence, "saved")
                check(saved)
                desktop.press(SAVED, "-M", "alt", "-k", "F4", "-m", "alt")
                process.wait(timeout=120)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                output.close()
                keyboard.kill()
                keyboard.wait()
            if process.returncode != 0:
                raise RuntimeError(f"paint.net ended with status {process.returncode}")
        finally:
            if compositor:
                compositor.terminate()
    print("PASS: paint.net 5.2 installed, drew a stroke with the brush and saved it as a PNG that holds it")


if __name__ == "__main__":
    main()
