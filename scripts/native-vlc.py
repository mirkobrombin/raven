#!/usr/bin/env python3
"""Install VLC through Raven with the installer VideoLAN publishes, and play a video in it as a person does.

The video is made here: an uncompressed AVI whose frames each take a colour of their own and carry a white bar that sweeps across, with a
440 Hz tone. On an isolated headless compositor VLC is installed silent in a fresh prefix, started, its first-run privacy question answered,
and the video opened from the Open dialog. The frame on screen tells the playback time: its colour names the frame up to a cycle of 256
and the bar's place settles which cycle. The video must play, stop moving on pause, jump ahead on a seek, lower its volume, fill the
screen and come back, with its sound going to the audio endpoint, and VLC must quit cleanly.

usage: native-vlc.py RAVEN VLC_INSTALLER EVIDENCE_DIR
"""

from pathlib import Path
import hashlib
import importlib.util
import math
import shutil
import struct
import subprocess
import sys
import time

from PIL import Image

# The compositor and the runs are the ones of the Visual C++ workflow, loaded without leaving compiled files in the tree.
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("native_redist", Path(__file__).with_name("native-redist.py"))
native_redist = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(native_redist)

INSTALLER_SHA256 = "9742689a50e96ddc04d80ceff046b28da2beefd617be18166f8c5e715ec60c59"
FPS = 25
SECONDS = 60
FRAMES = FPS * SECONDS
MAIN = "VLC media player"
PRIVACY = "Privacy and Network Access Policy"
OPEN = "Select one or more files to open"


def chunk(tag, data):
    return tag + struct.pack("<I", len(data)) + data + (b"\0" if len(data) % 2 else b"")


def riff_list(kind, tag, data):
    return chunk(kind, tag + data)


def frame_colour(number):
    return (number * 37) % 256, (number * 91) % 256, (255 - number * 53) % 256


def make_avi(path, width=160, height=120, rate=44100):
    """An AVI of raw RGB frames and 16-bit stereo PCM, interleaved one frame of each per video frame."""
    row = (width * 3 + 3) & ~3
    frame_size = row * height
    samples_per_frame = rate // FPS
    audio_size = samples_per_frame * 4
    avih = struct.pack("<IIIIIIIIIIIIII", 1000000 // FPS, (frame_size + audio_size) * FPS, 0, 0x10, FRAMES, 0, 2, frame_size,
                       width, height, 0, 0, 0, 0)
    video_header = struct.pack("<4s4sIHHIIIIIIIIhhhh", b"vids", b"DIB ", 0, 0, 0, 0, 1, FPS, 0, FRAMES, frame_size, 0xffffffff, 0, 0, 0,
                               width, height)
    bitmap = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, frame_size, 0, 0, 0, 0)
    audio_header = struct.pack("<4s4sIHHIIIIIIIIhhhh", b"auds", b"\0\0\0\0", 0, 0, 0, 0, 1, rate, 0, FRAMES * samples_per_frame, audio_size,
                               0xffffffff, 4, 0, 0, 0, 0)
    wave = struct.pack("<HHIIHHH", 1, 2, rate, rate * 4, 4, 16, 0)
    header = riff_list(b"LIST", b"hdrl", chunk(b"avih", avih) + riff_list(b"LIST", b"strl", chunk(b"strh", video_header) + chunk(b"strf", bitmap))
                       + riff_list(b"LIST", b"strl", chunk(b"strh", audio_header) + chunk(b"strf", wave)))
    movie = bytearray()
    index = bytearray()
    phase = 0
    for number in range(FRAMES):
        red, green, blue = frame_colour(number)
        bar = (number * width) // FRAMES
        left, right = max(bar - 5, 0), min(bar + 6, width)
        background = bytes((blue, green, red))
        line = background * left + b"\xff\xff\xff" * (right - left) + background * (width - right) + b"\0" * (row - width * 3)
        sound = bytearray()
        for _ in range(samples_per_frame):
            value = int(12000 * math.sin(phase))
            phase += 2 * math.pi * 440 / rate
            sound += struct.pack("<hh", value, value)
        for tag, data in ((b"00db", line * height), (b"01wb", bytes(sound))):
            index += struct.pack("<4sIII", tag, 0x10, len(movie) + 4, len(data))
            movie += chunk(tag, data)
    body = b"AVI " + header + riff_list(b"LIST", b"movi", bytes(movie)) + chunk(b"idx1", bytes(index))
    Path(path).write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


class Screen:
    def __init__(self, evidence):
        self.evidence = evidence

    def shot(self, name):
        path = self.evidence / f"{name}.png"
        subprocess.run(["grim", str(path)], env=native_redist.environment(), check=True, timeout=20)
        return Image.open(path).convert("RGB")


def video(image):
    """The video on screen: the frame it shows and the rectangle it fills, or None when no frame of the video is there."""
    pixels = image.load()
    width, height = image.size
    counts = {}
    for y in range(0, height, 8):
        for x in range(0, width, 8):
            counts[pixels[x, y]] = counts.get(pixels[x, y], 0) + 1
    known = {}
    for number in range(256):
        known.setdefault(frame_colour(number), number)
    found = [(count, colour) for colour, count in counts.items() if colour in known and colour not in ((0, 0, 0), (255, 255, 255))]
    if not found:
        return None
    colour = max(found)[1]
    xs, ys = [], []
    for y in range(0, height, 2):
        for x in range(0, width, 2):
            if pixels[x, y] == colour:
                xs.append(x)
                ys.append(y)
    left, right, top, bottom = min(xs), max(xs), min(ys), max(ys)
    # The white bar on the middle row of the video settles which cycle of 256 colours the frame is in.
    row = (top + bottom) // 2
    white = [x for x in range(left, right + 1) if pixels[x, row] == (255, 255, 255)]
    estimate = (sum(white) / len(white) - left) / max(right - left, 1) * FRAMES if white else 0
    candidates = [known[colour] + cycle * 256 for cycle in range((FRAMES + 255) // 256) if known[colour] + cycle * 256 < FRAMES]
    frame = min(candidates, key=lambda number: abs(number - estimate))
    return frame, (left, top, right, bottom)


def wlrctl(*arguments):
    return subprocess.run(["wlrctl", "toplevel", *arguments], env=native_redist.environment(), capture_output=True, text=True, timeout=10)


def wait_title(title, process, timeout=60):
    deadline = time.monotonic() + timeout
    while wlrctl("focus", f"title:{title}").returncode != 0:
        if process.poll() is not None:
            raise RuntimeError(f"VLC ended with status {process.returncode} before {title!r} appeared")
        if time.monotonic() > deadline:
            raise RuntimeError(f"{title!r} did not appear")
        time.sleep(0.3)
    time.sleep(1)


def keys(*arguments):
    subprocess.run(["wtype", *arguments], env=native_redist.environment(), check=True, timeout=10)


def press(title, *arguments):
    """Keys go to the window named by its title; a key that changes nothing goes first, since the first after wtype loads its keymap can
    get lost."""
    wlrctl("focus", f"title:{title}")
    time.sleep(0.3)
    keys("-k", "Shift_L")
    keys(*arguments)


def wait_video(screen, name, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        seen = video(screen.shot(name))
        if seen:
            return seen
        time.sleep(0.5)
    raise RuntimeError(f"{name}: no frame of the video is on screen")


def point(x, y):
    """wlrctl moves relatively: the pointer is parked in the corner, then travels to the absolute position."""
    subprocess.run(["wlrctl", "pointer", "move", "-4000", "-4000"], env=native_redist.environment(), check=True, timeout=10)
    subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=native_redist.environment(), check=True, timeout=10)


def progress(image, rectangle):
    """Where the played part of the seek bar ends, under the video: the blue of its fill."""
    left, top, right, bottom = rectangle
    pixels = image.load()
    for y in range(bottom + 2, min(bottom + 30, image.size[1])):
        blue = [x for x in range(image.size[0]) if pixels[x, y][2] > 180 and pixels[x, y][0] < 90 and 100 < pixels[x, y][1] < 160]
        if len(blue) > 10:
            return max(blue), y
    return None


def green(image, rectangle):
    """The pixels of the volume slider's fill, in the controls under the video: nothing else there is green."""
    left, top, right, bottom = rectangle
    pixels = image.load()
    count = 0
    for y in range(bottom + 4, min(bottom + 80, image.size[1])):
        for x in range(image.size[0]):
            red, green_value, blue = pixels[x, y]
            count += green_value > 150 and red < 140 and blue < 90
    return count


def install(raven, prefix, installer, evidence):
    if hashlib.sha256(installer.read_bytes()).hexdigest() != INSTALLER_SHA256:
        raise RuntimeError(f"{installer.name} is not the VLC 3.0.21 installer VideoLAN publishes")
    code = native_redist.run(raven, prefix, installer, ["/S"], evidence / "install.log", timeout=600)
    program = prefix / "drive_c" / "Program Files" / "VideoLAN" / "VLC" / "vlc.exe"
    if code != 0 or not program.exists():
        raise RuntimeError(f"the installer ended with status {code} and left {'a' if program.exists() else 'no'} vlc.exe")
    return program


def play(raven, prefix, program, evidence):
    screen = Screen(evidence)
    env = native_redist.environment() | {"RAVEN_PREFIX": str(prefix)}
    log = (evidence / "vlc.log").open("wb")
    vlc_log = prefix / "drive_c" / "Users" / "Public" / "vlc-log.txt"
    process = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=600000", "--desktop=wayland", str(program),
                                "--", "--file-logging", "--logfile=C:\\Users\\Public\\vlc-log.txt", "--verbose=2"],
                               env=env, stdout=log, stderr=log)
    try:
        # The first run asks about network access before the main window shows; the question must be in front, and is accepted.
        wait_title(PRIVACY, process)
        screen.shot("privacy")
        press(PRIVACY, "-k", "Return")
        wait_title(MAIN, process)
        press(MAIN, "-M", "ctrl", "-k", "o", "-m", "ctrl")
        wait_title(OPEN, process)
        screen.shot("open-dialog")
        keys("-k", "End")
        keys("C:\\Users\\Public\\test.avi")
        keys("-k", "Return")
        time.sleep(2)
        first, rectangle = wait_video(screen, "playing-1")
        time.sleep(1.5)
        second, _ = wait_video(screen, "playing-2")
        if not 10 <= second - first <= 75:
            raise RuntimeError(f"playback went from frame {first} to {second} in 1.5 seconds")
        if not any(f"test.avi - {MAIN}" in line for line in wlrctl("list").stdout.splitlines()):
            raise RuntimeError("the window title does not name the video")

        press(MAIN, "-k", "space")
        time.sleep(1)
        paused, _ = wait_video(screen, "paused-1")
        time.sleep(1.5)
        still, _ = wait_video(screen, "paused-2")
        if still != paused:
            raise RuntimeError(f"the video moved from frame {paused} to {still} while paused")

        # Over the seek bar VLC shows the time under the pointer in a translucent layered window.
        bar = progress(screen.shot("paused-2"), rectangle)
        if not bar:
            raise RuntimeError("the seek bar is not under the video")
        point(bar[0] + 40, bar[1])
        time.sleep(1.5)
        screen.shot("seek-tooltip")
        point(0, 0)
        if process.poll() is not None:
            raise RuntimeError(f"VLC ended with status {process.returncode} when the pointer went over the seek bar")

        # Alt+Right jumps ten seconds ahead.
        press(MAIN, "-M", "alt", "-k", "Right", "-m", "alt")
        time.sleep(1.5)
        jumped, _ = wait_video(screen, "seeked")
        if not 7 * FPS <= jumped - still <= 13 * FPS:
            raise RuntimeError(f"a ten second seek went from frame {still} to {jumped}")
        press(MAIN, "-k", "space")

        before = green(screen.shot("volume-1"), rectangle)
        for _ in range(4):
            press(MAIN, "-M", "ctrl", "-k", "Down", "-m", "ctrl")
            time.sleep(0.3)
        time.sleep(0.7)
        after = green(screen.shot("volume-2"), rectangle)
        if not before or after >= before * 0.9:
            raise RuntimeError(f"the volume slider did not go down: {before} then {after} pixels of its fill")

        press(MAIN, "-k", "f")
        time.sleep(2)
        _, full = wait_video(screen, "fullscreen")
        width, height = screen.shot("fullscreen").size
        if full[3] - full[1] < height - 8:
            raise RuntimeError(f"fullscreen shows the video in {full}, not the whole height of the screen")
        press(MAIN, "-k", "Escape")
        time.sleep(2)
        _, back = wait_video(screen, "windowed")
        if back[3] - back[1] > height * 0.8 or abs((back[2] - back[0]) - (rectangle[2] - rectangle[0])) > 8:
            raise RuntimeError(f"after fullscreen the video is in {back}, not back in the window ({rectangle})")

        press(MAIN, "-M", "ctrl", "-k", "q", "-m", "ctrl")
        process.wait(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        log.close()
    if process.returncode != 0:
        raise RuntimeError(f"VLC ended with status {process.returncode}")
    text = vlc_log.read_text(errors="replace")
    shutil.copy(vlc_log, evidence / "vlc-log.txt")
    if 'using audio output module "mmdevice"' not in text:
        raise RuntimeError("the sound did not go to the audio endpoint")
    for failure in ("cannot load module", "Unable to load extensions module", "audio output failed"):
        if failure in text:
            raise RuntimeError(f"VLC's log says {failure!r}")


def main():
    raven, installer, evidence = map(Path, sys.argv[1:4])
    shutil.rmtree(evidence, ignore_errors=True)
    evidence.mkdir(parents=True)
    prefix = evidence / "prefix"
    with (evidence / "compositor.log").open("wb") as log:
        compositor = native_redist.start_compositor(log)
        try:
            program = install(raven, prefix, installer, evidence)
            public = prefix / "drive_c" / "Users" / "Public"
            public.mkdir(parents=True, exist_ok=True)
            make_avi(public / "test.avi")
            play(raven, prefix, program, evidence)
        finally:
            if compositor:
                compositor.terminate()
    print("PASS: VLC installed, opened the video from its dialog, played, paused, seeked, lowered the volume, went fullscreen and back")


if __name__ == "__main__":
    main()
