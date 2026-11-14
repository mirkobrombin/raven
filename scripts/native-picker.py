#!/usr/bin/env python3
"""Pick a host file in Raven's file dialog, as a person does, and check what the program may then do with it.

testdata/native_loader/picker_client.c asks for a file to open, once through GetOpenFileName and once through the dialog object whose
event handler learns the choice, and then for a place to save. The dialog shows the host's files; the person types the path of one and
confirms. The program may read the file it opened but not change it, and may write the one it saves.
The exit code of the program says which part went wrong.

usage: native-picker.py RAVEN PICKER_EXE WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

RUNTIME = Path(os.environ.get("RAVEN_TEST_RUNTIME") or f"/run/user/{os.getuid()}") / "raven-labwc-runtime"
FAILURES = {
    10: "the dialog returned no file",
    11: "the dialog returned a file in the prefix instead of the host file",
    12: "the file open dialog object could not be created",
    13: "the dialog did not tell the program's event handler what was chosen",
    20: "the program could not write the file it chose to save",
    30: "the program could not open the file it chose",
    31: "the program read something else than the chosen file",
    32: "the program could change a file it only opened",
    33: "changing an opened file failed with another error than access denied",
}


def environment():
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


def pick(raven, fixture, prefix, title, path, arguments):
    """Run the program, wait for its dialog, type the host path in Windows form and confirm."""
    env = os.environ | environment() | {"RAVEN_PREFIX": str(prefix)}
    env["PATH"] = os.environ["PATH"]
    program = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=60000", "--desktop=wayland", str(fixture),
                                "--", *arguments], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    try:
        deadline = time.monotonic() + 30
        while subprocess.run(["wlrctl", "toplevel", "focus", f"title:{title}"], env=environment(), capture_output=True,
                             timeout=10).returncode != 0:
            if program.poll() is not None or time.monotonic() > deadline:
                raise SystemExit(f"the {title} dialog did not appear")
            time.sleep(0.2)
        time.sleep(1.5)
        # The first keys after wtype loads its keymap can get lost: a key that changes nothing goes first.
        subprocess.run(["wtype", "-k", "End"], env=environment(), check=True, timeout=10)
        subprocess.run(["wtype", "Z:" + str(path).replace("/", "\\")], env=environment(), check=True, timeout=10)
        subprocess.run(["wtype", "-k", "Return"], env=environment(), check=True, timeout=10)
        output, errors = program.communicate(timeout=30)
    finally:
        if program.poll() is None:
            program.kill()
    if "Native entry returned 0 " not in output:
        code = next((int(word) for line in output.splitlines() if line.startswith("Native entry returned ") for word in line.split()[3:4]), None)
        raise SystemExit(FAILURES.get(code, f"the program ended with {code}: {output[-300:]!r} {errors[-300:]!r}"))


def main():
    raven, fixture, work = map(Path, sys.argv[1:4])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    # The program runs from a folder of its own, which it reaches anyway; the host file lies outside it.
    program = work / "program"
    program.mkdir()
    fixture = Path(shutil.copy(fixture, program / fixture.name))
    host = work / "host"
    host.mkdir()
    (host / "picked.txt").write_bytes(b"picked")
    with open(work / "compositor.log", "wb") as log:
        compositor = start_compositor(log)
        try:
            pick(raven, fixture, work / "prefix", "Open", host / "picked.txt", [])
            if (host / "picked.txt").read_bytes() != b"picked":
                raise SystemExit("the file the program only opened changed")
            pick(raven, fixture, work / "prefix", "Open", host / "picked.txt", ["--events"])
            pick(raven, fixture, work / "prefix", "Save As", host / "saved.txt", ["--save"])
            if not (host / "saved.txt").exists() or (host / "saved.txt").read_bytes() != b"saved":
                raise SystemExit("the file the program saved is not on the host")
        finally:
            if compositor:
                compositor.terminate()
    print("a host file picked in the dialog was readable but not writable after Open, also through the event handler, and written after Save As")


if __name__ == "__main__":
    main()
