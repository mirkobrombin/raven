#!/usr/bin/env python3
"""Install KeePass 2.57 through Raven on the .NET Framework 4.8 its setup asks for, and keep passwords in it as a person does.

In a fresh prefix the .NET Framework 4.8 offline installer runs with /q /norestart, then the KeePass setup runs silent. On an isolated
headless compositor KeePass makes a new database with a master password, saved through its Save dialog, adds an entry and edits it,
copies the entry's password to the clipboard, saves and closes the database. It is then opened again through the Open dialog: a wrong
master password is refused, the right one shows the entry, and its user name and password come back through the clipboard as edited.

usage: native-keepass.py RAVEN NDP48_INSTALLER KEEPASS_SETUP EVIDENCE_DIR
"""

from pathlib import Path
import hashlib
import importlib.util
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


native_redist = load("native_redist", "native-redist.py")
# The compositor helpers the 7-Zip workflow drives its windows with.
desktop = load("native_7zip", "native-7zip.py")

NETFX_SHA256 = "68c9986a8dcc0214d909aa1f31bee9fb5461bb839edca996a75b08ddffc1483f"
KEEPASS_SHA256 = "ea53f7f944fada950cd7bb154deb078123a357b7bc5e2484851762b3552eb48b"
MASTER = "Raven-Kp57!"
SECRET = "Pw-Raven-4821"
USER = "raven-user"
EDITED_USER = "raven-edited"
MAIN = "KeePass"
SAVED = "Database.kdbx - KeePass"


def check_installer(path, digest, what):
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f"{path.name} is not {what}")


def clipboard():
    done = subprocess.run(["wl-paste", "-n"], env=native_redist.environment(), capture_output=True, text=True, timeout=10)
    return done.stdout


def wait_clipboard(text, timeout=10):
    deadline = time.monotonic() + timeout
    while clipboard() != text:
        if time.monotonic() > deadline:
            raise RuntimeError(f"the clipboard holds {clipboard()!r}, not {text!r}")
        time.sleep(0.3)


def install(raven, prefix, netfx, keepass, evidence):
    code = native_redist.run(raven, prefix, netfx, ["/q", "/norestart"], evidence / "netfx.log", timeout=2400)
    if code not in (0, 3010):
        raise RuntimeError(f"the .NET Framework installer ended with status {code}")
    if not (prefix / "drive_c" / "windows" / "Microsoft.NET" / "Framework64" / "v4.0.30319" / "clr.dll").is_file():
        raise RuntimeError("the .NET Framework 4.8 runtime is missing")
    code = native_redist.run(raven, prefix, keepass, ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/ALLUSERS"],
                             evidence / "install.log", timeout=900)
    if code != 0:
        raise RuntimeError(f"the KeePass setup ended with status {code}")
    program = prefix / "drive_c" / "Program Files" / "KeePass Password Safe 2" / "KeePass.exe"
    if not program.is_file():
        raise RuntimeError("the setup left no KeePass.exe")
    records = sorted(path.name for path in Path(f"{prefix}.apps").iterdir())
    if "KeePass-2.57-Setup" not in records:
        raise RuntimeError(f"the setup was recorded as {records}, not under its name")
    # The setup compiles KeePass ahead of time, and mscorlib with it; NGen drops the image again if compiling fails.
    images = prefix / "drive_c" / "windows" / "assembly" / "NativeImages_v4.0.30319_64"
    for image in ("mscorlib/*/mscorlib.ni.dll", "KeePass/*/KeePass.ni.exe"):
        if not any(images.glob(image)):
            raise RuntimeError(f"the setup left no native image {image}")
    return program


def create(evidence, origin):
    """A new database with a master password, saved where the Save dialog offers, with one entry added and then edited."""
    desktop.press(MAIN, "-M", "ctrl", "-k", "n", "-m", "ctrl")
    desktop.wait_title("KeePass")
    time.sleep(2)
    desktop.keys("-k", "Shift_L", "-k", "Return")
    desktop.wait_title("Create New Database")
    desktop.shot(evidence, "save-dialog")
    desktop.press("Create New Database", "-k", "Return")
    desktop.wait_title("Create Master Key")
    desktop.click(origin[0] + 379, origin[1] + 131)
    desktop.write(MASTER)
    desktop.click(origin[0] + 379, origin[1] + 154)
    desktop.write(MASTER)
    desktop.shot(evidence, "master-key")
    desktop.click(origin[0] + 457, origin[1] + 497)
    # The password is short, so KeePass asks before taking it.
    time.sleep(3)
    desktop.click(origin[0] + 288, origin[1] + 278)
    desktop.wait_title("Configure New Database")
    desktop.click(origin[0] + 373, origin[1] + 142)
    desktop.write("RavenVault")
    desktop.click(origin[0] + 442, origin[1] + 443)
    # The emergency sheet is offered last; it is skipped.
    time.sleep(4)
    desktop.shot(evidence, "emergency-sheet")
    desktop.click(origin[0] + 249, origin[1] + 329)
    desktop.wait_title("Database.kdbx* - KeePass")
    desktop.shot(evidence, "created")

    desktop.click(origin[0] + 310, origin[1] + 281)
    desktop.press("Database.kdbx* - KeePass", "-M", "ctrl", "-k", "i", "-m", "ctrl")
    desktop.wait_title("Add Entry")
    desktop.click(origin[0] + 324, origin[1] + 130)
    desktop.write("Raven Mail")
    desktop.click(origin[0] + 364, origin[1] + 155)
    desktop.write(USER)
    for field in (180, 205):
        desktop.click(origin[0] + 346, origin[1] + field)
        desktop.keys("-k", "Shift_L", "-M", "ctrl", "-k", "a", "-m", "ctrl", "-s", "200", SECRET)
    desktop.shot(evidence, "add-entry")
    desktop.click(origin[0] + 437, origin[1] + 457)
    time.sleep(3)

    desktop.press("Database.kdbx* - KeePass", "-k", "Return")
    desktop.wait_title("Edit Entry")
    desktop.click(origin[0] + 364, origin[1] + 155)
    desktop.keys("-k", "Shift_L", "-M", "ctrl", "-k", "a", "-m", "ctrl", "-s", "200", EDITED_USER)
    desktop.shot(evidence, "edit-entry")
    desktop.click(origin[0] + 437, origin[1] + 457)
    time.sleep(3)
    desktop.shot(evidence, "edited")


def reopen(evidence, origin):
    """The database closed and opened again: a wrong master password is refused, the right one opens it."""
    desktop.press(SAVED, "-M", "ctrl", "-k", "w", "-m", "ctrl")
    time.sleep(3)
    desktop.press(MAIN, "-M", "ctrl", "-k", "o", "-m", "ctrl")
    desktop.wait_title("Open Database File")
    desktop.shot(evidence, "open-dialog")
    desktop.click(origin[0] + 295, origin[1] + 162)
    desktop.click(origin[0] + 778, origin[1] + 566)
    desktop.wait_title("Open Database - Database.kdbx")
    desktop.click(origin[0] + 367, origin[1] + 237)
    desktop.write("not-the-password")
    desktop.click(origin[0] + 406, origin[1] + 328)
    time.sleep(4)
    refused = desktop.shot(evidence, "refused")
    if f": {SAVED}" in desktop.titles():
        raise RuntimeError("a wrong master password opened the database")
    desktop.click(origin[0] + 329, origin[1] + 302)
    desktop.wait_title("Open Database - Database.kdbx")
    desktop.click(origin[0] + 367, origin[1] + 237)
    desktop.keys("-k", "Shift_L", "-M", "ctrl", "-k", "a", "-m", "ctrl", "-s", "200", MASTER)
    desktop.click(origin[0] + 406, origin[1] + 328)
    desktop.wait_title(SAVED)
    desktop.shot(evidence, "reopened")
    return refused


def main():
    raven, netfx, keepass, evidence = map(Path, sys.argv[1:5])
    check_installer(netfx, NETFX_SHA256, "the .NET Framework 4.8 offline installer Microsoft publishes")
    check_installer(keepass, KEEPASS_SHA256, "the KeePass 2.57 setup keepass.info publishes")
    shutil.rmtree(evidence, ignore_errors=True)
    evidence.mkdir(parents=True)
    with (evidence / "compositor.log").open("wb") as log:
        compositor = native_redist.start_compositor(log)
        try:
            prefix = evidence / "prefix"
            program = install(raven, prefix, netfx, keepass, evidence)
            output = (evidence / "keepass.log").open("wb")
            # A virtual keyboard kept for the whole session: each new one wtype makes can lose its first keys before its keymap lands.
            keyboard = subprocess.Popen(["wtype", "-s", "100000000"], env=native_redist.environment())
            process = desktop.start(raven, prefix, program, [], output)
            try:
                desktop.wait_title(MAIN, process, timeout=120)
                time.sleep(5)
                origin = desktop.window_origin(desktop.shot(evidence, "started"))
                # The first start asks about update checks; they are turned off.
                desktop.click(origin[0] + 256, origin[1] + 228)
                time.sleep(2)
                create(evidence, origin)

                desktop.click(origin[0] + 216, origin[1] + 140)
                desktop.press("Database.kdbx* - KeePass", "-M", "ctrl", "-k", "c", "-m", "ctrl")
                wait_clipboard(SECRET)
                desktop.press("Database.kdbx* - KeePass", "-M", "ctrl", "-k", "s", "-m", "ctrl")
                desktop.wait_title(SAVED)
                saved = next((prefix / "drive_c" / "Users").glob("*/Documents/Database.kdbx"), None)
                if not saved or saved.stat().st_size < 1024:
                    raise RuntimeError("the database was not saved")
                if b"raven-edited" in saved.read_bytes() or SECRET.encode() in saved.read_bytes():
                    raise RuntimeError("the database holds the entry unencrypted")

                reopen(evidence, origin)
                desktop.click(origin[0] + 216, origin[1] + 140)
                desktop.press(SAVED, "-M", "ctrl", "-k", "b", "-m", "ctrl")
                wait_clipboard(EDITED_USER)
                desktop.press(SAVED, "-M", "ctrl", "-k", "c", "-m", "ctrl")
                wait_clipboard(SECRET)
                desktop.press(SAVED, "-M", "ctrl", "-k", "q", "-m", "ctrl")
                process.wait(timeout=60)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                output.close()
                keyboard.kill()
                keyboard.wait()
            if process.returncode != 0:
                raise RuntimeError(f"KeePass ended with status {process.returncode}")
        finally:
            if compositor:
                compositor.terminate()
    print("PASS: KeePass 2.57 installed on .NET 4.8, made, saved and reopened a database with an edited entry and its password")


if __name__ == "__main__":
    main()
