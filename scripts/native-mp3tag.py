#!/usr/bin/env python3
"""Install Mp3tag 3.36.1 for x64 through Raven from its own setup and tag a folder of MP3 files as a person does.

In a fresh prefix the setup runs silent. Three MP3 files with ID3v2.3 tags and a PNG picture are written to a folder on the host, and
Mp3tag opens that folder on an isolated headless compositor. The title of one file is changed in the tag panel and saved; then all three
are selected and their artist and album changed at once and saved; then the first gets the picture as its cover through the Open dialog
and is saved. The files on the host are read back: each holds the tags it was given and only the first holds the cover.

usage: native-mp3tag.py RAVEN MP3TAG_SETUP EVIDENCE_DIR
"""

from pathlib import Path
import hashlib
import importlib.util
import shutil
import struct
import subprocess
import sys
import time
import zlib

sys.dont_write_bytecode = True


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


native_redist = load("native_redist", "native-redist.py")
# The compositor helpers the 7-Zip workflow drives its windows with.
desktop = load("native_7zip", "native-7zip.py")

SETUP_SHA256 = "a977861003a87b1f00f2e0c91263c61bf12d457c6d457b367ce5f69de28fbea0"
MAIN = "Mp3tag v3.36.1"
TITLES = ("Morning Light", "Second Wind", "Last Call")
EDITED_TITLE = "Sunrise Edit"
ARTIST = "New Artist"
ALBUM = "Bulk Album"
# MPEG-1 Layer III at 128 kbit/s and 44.1 kHz without padding: 417 bytes a frame, and side information of zeros decodes to silence.
SILENCE = b"\xff\xfb\x90\x64" + bytes(413)


def syncsafe(value):
    return bytes([(value >> 21) & 127, (value >> 14) & 127, (value >> 7) & 127, value & 127])


def text_frame(name, text):
    payload = b"\0" + text.encode("latin-1")
    return name.encode() + struct.pack(">I", len(payload)) + b"\0\0" + payload


def write_music(folder):
    folder.mkdir(parents=True)
    files = []
    for number, title in enumerate(TITLES, 1):
        frames = b"".join(text_frame(name, text) for name, text in
                          (("TIT2", title), ("TPE1", "Old Artist"), ("TALB", "Raw Album"), ("TRCK", str(number))))
        path = folder / f"{number:02} {title}.mp3"
        path.write_bytes(b"ID3\x03\0\0" + syncsafe(len(frames)) + frames + SILENCE * 400)
        files.append(path)
    # A 64 by 64 picture of colour bands, as a PNG.
    rows = b"".join(b"\0" + bytes([200, 40 + y * 2, 30]) * 64 for y in range(64))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    cover = folder / "cover.png"
    cover.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 2, 0, 0, 0)) +
                      chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    return files, cover


def decode_text(payload):
    encoding, data = payload[0], payload[1:]
    if encoding == 1:
        text = data.decode("utf-16")
    elif encoding == 2:
        text = data.decode("utf-16-be")
    elif encoding == 3:
        text = data.decode("utf-8")
    else:
        text = data.decode("latin-1")
    return text.rstrip("\0")


def read_tag(path):
    """The text frames of the file's ID3v2 tag, and the picture frames' data."""
    data = path.read_bytes()
    if data[:3] != b"ID3":
        raise RuntimeError(f"{path.name} has no ID3v2 tag")
    version = data[3]
    size = (data[6] << 21) | (data[7] << 14) | (data[8] << 7) | data[9]
    body = data[10:10 + size]
    texts, pictures = {}, []
    at = 0
    while at + 10 <= len(body) and body[at] != 0:
        name = body[at:at + 4].decode("latin-1")
        raw = body[at + 4:at + 8]
        length = (raw[0] << 21) | (raw[1] << 14) | (raw[2] << 7) | raw[3] if version == 4 else struct.unpack(">I", raw)[0]
        payload = body[at + 10:at + 10 + length]
        if name.startswith("T") and name != "TXXX" and payload:
            texts[name] = decode_text(payload)
        elif name == "APIC":
            pictures.append(payload)
        at += 10 + length
    return texts, pictures


def install(raven, prefix, setup, evidence):
    code = native_redist.run(raven, prefix, setup, ["/S"], evidence / "install.log", timeout=600)
    if code != 0:
        raise RuntimeError(f"the Mp3tag setup ended with status {code}")
    program = prefix / "drive_c" / "Program Files" / "Mp3tag" / "Mp3tag.exe"
    if not program.is_file():
        raise RuntimeError("the setup left no Mp3tag.exe")
    return program


def wait_main(process, timeout=120):
    deadline = time.monotonic() + timeout
    while MAIN not in desktop.titles():
        if process.poll() is not None:
            raise RuntimeError(f"Mp3tag ended with status {process.returncode} before its window appeared")
        if time.monotonic() > deadline:
            raise RuntimeError(f"Mp3tag did not open; the windows are:\n{desktop.titles()}")
        time.sleep(0.5)


def replace_field(origin, y, text):
    """The tag panel's field at height y takes the text in place of what it holds."""
    desktop.click(origin[0] + 140, origin[1] + y)
    desktop.keys("-k", "Shift_L", "-M", "ctrl", "-k", "a", "-m", "ctrl", "-s", "200", text)


def save():
    desktop.keys("-k", "Shift_L", "-M", "ctrl", "-k", "s", "-m", "ctrl")
    time.sleep(4)


def check(files, cover):
    picture = cover.read_bytes()
    for number, (path, title) in enumerate(zip(files, TITLES)):
        texts, pictures = read_tag(path)
        wanted = {"TIT2": EDITED_TITLE if number == 0 else title, "TPE1": ARTIST, "TALB": ALBUM}
        for name, value in wanted.items():
            if texts.get(name) != value:
                raise RuntimeError(f"{path.name} holds {name} {texts.get(name)!r}, not {value!r}")
        if number == 0 and not any(picture in frame and b"image/png" in frame for frame in pictures):
            raise RuntimeError(f"{path.name} holds no cover with the PNG picture")
        if number and pictures:
            raise RuntimeError(f"{path.name} got a cover it was not given")


def main():
    raven, setup, evidence = map(Path, sys.argv[1:4])
    if hashlib.sha256(setup.read_bytes()).hexdigest() != SETUP_SHA256:
        raise RuntimeError(f"{setup.name} is not the Mp3tag 3.36.1 x64 setup mp3tag.de publishes")
    shutil.rmtree(evidence, ignore_errors=True)
    evidence.mkdir(parents=True)
    files, cover = write_music(evidence / "music")
    with (evidence / "compositor.log").open("wb") as log:
        compositor = native_redist.start_compositor(log)
        try:
            prefix = evidence / "prefix"
            program = install(raven, prefix, setup, evidence)
            output = (evidence / "mp3tag.log").open("wb")
            # A virtual keyboard kept for the whole session: each new one wtype makes can lose its first keys before its keymap lands.
            keyboard = subprocess.Popen(["wtype", "-s", "100000000"], env=native_redist.environment())
            process = desktop.start(raven, prefix, program, [f"Z:{files[0].parent}"], output)
            try:
                wait_main(process)
                time.sleep(8)
                origin = desktop.window_origin(desktop.shot(evidence, "started"))
                # The first start offers to look for updates; that is declined.
                desktop.click(origin[0] + 549, origin[1] + 403)
                time.sleep(3)
                desktop.shot(evidence, "folder")

                # One file: its title.
                desktop.click(origin[0] + 380, origin[1] + 99)
                time.sleep(1)
                replace_field(origin, 118, EDITED_TITLE)
                save()
                desktop.shot(evidence, "title-saved")

                # All three at once: artist and album.
                desktop.click(origin[0] + 380, origin[1] + 111)
                desktop.keys("-k", "Shift_L", "-M", "ctrl", "-k", "a", "-m", "ctrl")
                time.sleep(1)
                replace_field(origin, 157, ARTIST)
                replace_field(origin, 196, ALBUM)
                save()
                desktop.shot(evidence, "bulk-saved")

                # The first file again, given the picture as its cover through the Open dialog.
                desktop.click(origin[0] + 380, origin[1] + 99)
                time.sleep(1)
                desktop.click(origin[0] + 97, origin[1] + 537)
                desktop.keys("-k", "Shift_L", "-M", "ctrl", "-M", "alt", "-k", "a", "-m", "alt", "-m", "ctrl")
                time.sleep(3)
                desktop.shot(evidence, "open-dialog")
                desktop.keys("-k", "Shift_L", "-s", "200", "Z:" + str(cover).replace("/", "\\"))
                time.sleep(1)
                desktop.keys("-k", "Return")
                time.sleep(3)
                desktop.shot(evidence, "cover")
                save()
                desktop.shot(evidence, "cover-saved")
                check(files, cover)

                desktop.keys("-k", "Shift_L", "-M", "alt", "-k", "F4", "-m", "alt")
                process.wait(timeout=60)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                output.close()
                keyboard.kill()
                keyboard.wait()
            if process.returncode != 0:
                raise RuntimeError(f"Mp3tag ended with status {process.returncode}")
        finally:
            if compositor:
                compositor.terminate()
    print("PASS: Mp3tag 3.36.1 installed, changed one title, the artist and album of three files and a cover, saved to the host files")


if __name__ == "__main__":
    main()
