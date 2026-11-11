#!/usr/bin/env python3
"""Install 7-Zip through Raven from both installers Igor Pavlov publishes, and use its file manager as a person does.

The EXE installer runs silent in one fresh prefix and the MSI package, through the Windows Installer, in another; each installation is
recorded under the package's name. In the first prefix, on an isolated headless compositor, the file manager opens a folder of three
files and shows them with their icons, sizes and dates. From it an encrypted 7z and a zip with a password are made through 7-Zip's own
Add dialog, then the 7z is opened, extracted through the Extract dialog with its password and compared with the originals. The zip is
read here with the password; the 7z is tested with 7-Zip's console program, with the right password and a wrong one. Last, 7-Zip is made
the program .7z files open with from its Options, and opening the archive through the shell starts it on that archive.

usage: native-7zip.py RAVEN EXE_INSTALLER MSI_PACKAGE EVIDENCE_DIR
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
_spec = importlib.util.spec_from_file_location("native_redist", Path(__file__).with_name("native-redist.py"))
native_redist = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(native_redist)

EXE_SHA256 = "bdd1a33de78618d16ee4ce148b849932c05d0015491c34887846d431d29f308e"
MSI_SHA256 = "ec6af1ea0367d16dde6639a89a080a524cebc4d4bedfe00ed0cac4b865a918d8"
PASSWORD = "Raven-7z!"
FOLDER = "C:\\Users\\Public\\data\\"
ARCHIVE = "C:\\Users\\Public\\archives\\data.7z"
NAMES = ("alpha.txt", "beta.bin", "gamma.dat")


def check_installer(path, digest):
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f"{path.name} is not the 7-Zip 24.09 package 7-zip.org publishes")


def installed(prefix, where):
    folder = prefix / "drive_c" / "Program Files" / "7-Zip"
    for name in ("7zFM.exe", "7zG.exe", "7z.exe", "7z.dll"):
        if not (folder / name).exists():
            raise RuntimeError(f"{where} left no {name}")
    records = sorted(path.name for path in Path(f"{prefix}.apps").iterdir())
    if "7z2409-x64" not in records:
        raise RuntimeError(f"{where} was recorded as {records}, not under the package's name")
    return folder


def wlrctl(*arguments):
    return subprocess.run(["wlrctl", *arguments], env=native_redist.environment(), capture_output=True, text=True, timeout=10)


def titles():
    return wlrctl("toplevel", "list").stdout


def wait_title(title, process=None, timeout=60):
    deadline = time.monotonic() + timeout
    while wlrctl("toplevel", "focus", f"title:{title}").returncode != 0:
        if process and process.poll() is not None:
            raise RuntimeError(f"7-Zip ended with status {process.returncode} before {title!r} appeared")
        if time.monotonic() > deadline:
            raise RuntimeError(f"{title!r} did not appear; the windows are:\n{titles()}")
        time.sleep(0.3)
    time.sleep(1)


def wait_gone(title, timeout=60):
    deadline = time.monotonic() + timeout
    while f": {title}" in titles() or f"{title}\n" in titles():
        if time.monotonic() > deadline:
            raise RuntimeError(f"{title!r} stayed open")
        time.sleep(0.3)


def keys(*arguments):
    subprocess.run(["wtype", *arguments], env=native_redist.environment(), check=True, timeout=10)


def write(text):
    """Text typed into the control that has the focus, after a key that changes nothing: the first keys of a new keyboard can get lost."""
    keys("-k", "Shift_L", "-s", "300", text)


def press(title, *arguments):
    """Keys go to the window named by its title; a key that changes nothing goes first, since the first after wtype loads its keymap can
    get lost."""
    wlrctl("toplevel", "focus", f"title:{title}")
    time.sleep(0.3)
    keys("-k", "Shift_L")
    keys(*arguments)


def click(x, y):
    """wlrctl moves relatively: the pointer is parked in the corner, then travels to the absolute position."""
    subprocess.run(["wlrctl", "pointer", "move", "-4000", "-4000"], env=native_redist.environment(), check=True, timeout=10)
    subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)], env=native_redist.environment(), check=True, timeout=10)
    time.sleep(0.4)
    subprocess.run(["wlrctl", "pointer", "click"], env=native_redist.environment(), check=True, timeout=10)
    time.sleep(0.6)


def shot(evidence, name):
    path = evidence / f"{name}.png"
    subprocess.run(["grim", str(path)], env=native_redist.environment(), check=True, timeout=20)
    return Image.open(path).convert("RGB")


def window_origin(image):
    """The top left corner of the one window on the black desktop."""
    pixels = image.load()
    width, height = image.size
    for y in range(height):
        for x in range(width):
            if sum(pixels[x, y]) > 60:
                return x, y
    raise RuntimeError("no window is on the screen")


def start(raven, prefix, program, arguments, log):
    env = native_redist.environment() | {"RAVEN_PREFIX": str(prefix)}
    return subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=600000", "--desktop=wayland", str(program),
                             "--", *arguments], env=env, stdout=log, stderr=log)


def wait_file(path, timeout=60):
    deadline = time.monotonic() + timeout
    while not path.exists() or path.with_name(path.name + ".tmp").exists():
        if time.monotonic() > deadline:
            raise RuntimeError(f"{path.name} was not made")
        time.sleep(0.5)
    time.sleep(1)


def listed(image, origin):
    """The three rows of the list: each has an icon at its start and digits in the size column."""
    pixels = image.load()

    def dark(left, right, top, bottom, limit):
        return any(60 < sum(pixels[origin[0] + x, origin[1] + y]) < limit for x in range(left, right) for y in range(top, bottom))

    return all(dark(8, 24, 140 + row * 17, 153 + row * 17, 600) and dark(225, 265, 140 + row * 17, 153 + row * 17, 400) for row in range(3))


def add(evidence, origin, name, archive, kind):
    """Everything in the folder is selected and given to the Add dialog, which makes `archive` with the password; it is then moved to
    the folder of archives, so the next selection holds the three files alone."""
    press(FOLDER, "-M", "ctrl", "-k", "a", "-m", "ctrl")
    click(origin[0] + 24, origin[1] + 70)
    wait_title("Add to Archive")
    shot(evidence, f"{name}-dialog")
    # The dialog opens centred over the file manager, so its controls are found from the file manager's corner.
    dialog = origin
    if kind == "zip":
        click(dialog[0] + 401, dialog[1] + 99)
        keys("-k", "Home")
        keys("z")
        keys("-k", "Return")
        time.sleep(0.5)
    click(dialog[0] + 640, dialog[1] + 308)
    write(PASSWORD)
    click(dialog[0] + 640, dialog[1] + 356)
    write(PASSWORD)
    shot(evidence, f"{name}-filled")
    click(dialog[0] + 515, dialog[1] + 510)
    wait_file(archive)
    wait_gone("Add to Archive")
    archive.rename(archive.parent.parent / "archives" / archive.name)
    time.sleep(1)


def console(raven, prefix, program, arguments, evidence, name):
    """A console program, with this script's output for its console."""
    env = native_redist.environment() | {"RAVEN_PREFIX": str(prefix)}
    done = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=120000", "--stdio=inherit",
                           str(program), "--", *arguments], env=env, capture_output=True, timeout=180)
    text = (done.stdout + done.stderr).decode(errors="replace")
    (evidence / f"{name}.log").write_text(text)
    return done.returncode, text


def manage(raven, prefix, folder, evidence):
    data = prefix / "drive_c" / "Users" / "Public" / "data"
    data.mkdir(parents=True)
    archives = data.parent / "archives"
    archives.mkdir()
    contents = {"alpha.txt": b"alpha " * 900, "beta.bin": bytes(range(256)) * 80, "gamma.dat": b"gamma\r\n" * 3000}
    for name, body in contents.items():
        (data / name).write_bytes(body)
    log = (evidence / "7zfm.log").open("wb")
    process = start(raven, prefix, folder / "7zFM.exe", [FOLDER], log)
    try:
        wait_title(FOLDER, process)
        image = shot(evidence, "file-manager")
        origin = window_origin(image)
        if not listed(image, origin):
            raise RuntimeError("the list does not show the three files with their icons and sizes")

        add(evidence, origin, "add-7z", data / "data.7z", "7z")
        add(evidence, origin, "add-zip", data / "data.zip", "zip")

        with zipfile.ZipFile(archives / "data.zip") as archive:
            if sorted(archive.namelist()) != sorted(NAMES):
                raise RuntimeError(f"the zip holds {archive.namelist()}")
            for item in archive.infolist():
                if not item.flag_bits & 1:
                    raise RuntimeError(f"{item.filename} is not encrypted in the zip")
                if archive.read(item, pwd=PASSWORD.encode()) != contents[item.filename]:
                    raise RuntimeError(f"{item.filename} reads back different from the zip")

        press(FOLDER, "-M", "alt", "-k", "F4", "-m", "alt")
        process.wait(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        log.close()
    if process.returncode != 0:
        raise RuntimeError(f"the file manager ended with status {process.returncode}")

    code, text = console(raven, prefix, folder / "7z.exe", ["t", f"-p{PASSWORD}", ARCHIVE], evidence, "test-right")
    if code != 0 or "Everything is Ok" not in text:
        raise RuntimeError("the 7z does not test good with its password")
    code, text = console(raven, prefix, folder / "7z.exe", ["l", "-slt", f"-p{PASSWORD}", ARCHIVE], evidence, "list")
    if text.count("Encrypted = +") != 3:
        raise RuntimeError("the 7z does not hold three encrypted files")
    code, text = console(raven, prefix, folder / "7z.exe", ["t", "-pwrong", ARCHIVE], evidence, "test-wrong")
    if code == 0 or "Wrong password" not in text:
        raise RuntimeError("the 7z tests good with a wrong password")

    # The archive opened in the file manager, extracted to the folder its Extract dialog offers.
    log = (evidence / "extract.log").open("wb")
    process = start(raven, prefix, folder / "7zFM.exe", [ARCHIVE], log)
    archive_title = ARCHIVE + "\\"
    try:
        wait_title(archive_title, process)
        origin = window_origin(shot(evidence, "archive"))
        press(archive_title, "-M", "ctrl", "-k", "a", "-m", "ctrl")
        click(origin[0] + 66, origin[1] + 70)
        # From inside an archive the file manager extracts through its Copy dialog, which offers the archive's folder.
        wait_title("Copy")
        shot(evidence, "extract-dialog")
        press("Copy", "-k", "Return")
        wait_title("Enter password")
        press("Enter password", PASSWORD)
        keys("-k", "Return")
        deadline = time.monotonic() + 60
        target = None
        while not target:
            target = next((place for place in (archives / "data", archives) if all((place / name).exists() for name in NAMES)), None)
            if time.monotonic() > deadline:
                shot(evidence, "not-extracted")
                raise RuntimeError(f"the archive was not extracted; the windows are:\n{titles()}")
            time.sleep(0.5)
        time.sleep(2)
        shot(evidence, "extracted")
        press(archive_title, "-M", "alt", "-k", "F4", "-m", "alt")
        process.wait(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        log.close()
    for name in NAMES:
        if (target / name).read_bytes() != contents[name]:
            raise RuntimeError(f"{name} was extracted different from the original")

    associate(raven, prefix, folder, evidence)


def associate(raven, prefix, folder, evidence):
    """7-Zip's Options make it the program .7z files open with; the shell then opens the archive in it."""
    log = (evidence / "options.log").open("wb")
    process = start(raven, prefix, folder / "7zFM.exe", [FOLDER], log)
    try:
        wait_title(FOLDER, process)
        origin = window_origin(shot(evidence, "before-options"))
        click(origin[0] + 193, origin[1] + 39)
        write("o")
        wait_title("Options")
        shot(evidence, "options")
        click(origin[0] + 411, origin[1] + 55)
        shot(evidence, "associated")
        click(origin[0] + 521, origin[1] + 562)
        time.sleep(1)
        press(FOLDER, "-M", "alt", "-k", "F4", "-m", "alt")
        process.wait(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        log.close()
    code, text = console(raven, prefix, prefix / "drive_c" / "windows" / "system32" / "reg.exe", ["query", "HKCU\\Software\\Classes\\.7z"],
                         evidence, "reg")
    if code != 0 or "7-Zip.7z" not in text:
        raise RuntimeError(".7z files were not given to 7-Zip")

    log = (evidence / "shell-open.log").open("wb")
    process = start(raven, prefix, prefix / "drive_c" / "windows" / "system32" / "cmd.exe",
                    ["/c", "start", "", ARCHIVE], log)
    try:
        wait_title(ARCHIVE + "\\", timeout=60)
        shot(evidence, "shell-open")
        press(ARCHIVE + "\\", "-M", "alt", "-k", "F4", "-m", "alt")
        time.sleep(2)
    finally:
        if process.poll() is None:
            process.wait(timeout=30)
        log.close()


def main():
    raven, exe, msi, evidence = map(Path, sys.argv[1:5])
    check_installer(exe, EXE_SHA256)
    check_installer(msi, MSI_SHA256)
    shutil.rmtree(evidence, ignore_errors=True)
    evidence.mkdir(parents=True)
    with (evidence / "compositor.log").open("wb") as log:
        compositor = native_redist.start_compositor(log)
        try:
            # The Windows Installer package, installed by the msiexec the runtime builds.
            from_msi = evidence / "msi-prefix"
            setup = from_msi / "drive_c" / "setup"
            setup.mkdir(parents=True)
            shutil.copy(msi, setup / msi.name)
            code = native_redist.run(raven, from_msi, raven.parent / "msiexec.exe", ["/i", f"C:\\setup\\{msi.name}", "/qn"],
                                     evidence / "msi-install.log", timeout=600)
            if code != 0:
                raise RuntimeError(f"msiexec ended with status {code}")
            installed(from_msi, "the MSI package")

            prefix = evidence / "prefix"
            code = native_redist.run(raven, prefix, exe, ["/S"], evidence / "install.log", timeout=600)
            if code != 0:
                raise RuntimeError(f"the installer ended with status {code}")
            manage(raven, prefix, installed(prefix, "the installer"), evidence)
        finally:
            if compositor:
                compositor.terminate()
    print("PASS: 7-Zip installed from its EXE and MSI, made and extracted archives with a password, opened a 7z through the shell")


if __name__ == "__main__":
    main()
