#!/usr/bin/env python3
"""Maximize and restore a Raven window from the compositor, and check that the program follows.

testdata/native_loader/frame_restore_client.c opens an overlapped window. labwc maximizes it and restores it again with its own shortcut
(Super+A, ToggleMaximize), as a desktop does with a shortcut or a move holding its modifier key, and closes it with Alt+F4. The program
must hear SC_MAXIMIZE and then SC_RESTORE, as Windows sends them when the shell changes a window's state, and end restored at its own
rectangle; its exit code says which part went wrong.

usage: native-host-maximize.py RAVEN FRAME_RESTORE_EXE WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

RUNTIME = Path(f"/run/user/{os.getuid()}/raven-labwc-runtime")
WINDOW = "title:frame restore"
FAILURES = {
    10: "the compositor maximized the window and the program did not hear SC_MAXIMIZE",
    11: "the compositor restored the window and the program did not hear SC_RESTORE",
    12: "the window stayed maximized after the compositor restored it",
    13: "the restored window is not at its own rectangle",
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


def toplevel(action):
    return subprocess.run(["wlrctl", "toplevel", action, WINDOW], env=environment(), capture_output=True, timeout=10).returncode == 0


def shortcut(modifier, key):
    """Press one of labwc's default keybinds on the focused window."""
    subprocess.run(["wtype", "-M", modifier, *key, "-m", modifier], env=environment(), check=True, timeout=10)


def main():
    raven, fixture, work = map(Path, sys.argv[1:4])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    with open(work / "compositor.log", "wb") as log:
        compositor = start_compositor(log)
        env = os.environ | environment() | {"RAVEN_PREFIX": str(work / "prefix")}
        env["PATH"] = os.environ["PATH"]
        program = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=60000", "--desktop=wayland", str(fixture)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        try:
            deadline = time.monotonic() + 20
            while not toplevel("find"):
                if program.poll() is not None or time.monotonic() > deadline:
                    raise SystemExit("the window did not appear on the compositor")
                time.sleep(0.2)
            if not toplevel("focus"):
                raise SystemExit("wlrctl could not focus the window")
            for modifier, key in (("logo", ["a"]), ("logo", ["a"]), ("alt", ["-k", "F4"])):
                time.sleep(1.5)
                shortcut(modifier, key)
            output, errors = program.communicate(timeout=30)
        finally:
            if program.poll() is None:
                program.kill()
            if compositor:
                compositor.terminate()
    if "Native entry returned 0 " not in output:
        code = next((int(word) for line in output.splitlines() if line.startswith("Native entry returned ") for word in line.split()[3:4]), None)
        raise SystemExit(FAILURES.get(code, f"the program ended with {code}: {output[-300:]!r} {errors[-300:]!r}"))
    print("the compositor maximized and restored the window, and the program heard SC_MAXIMIZE and SC_RESTORE and ended restored")


if __name__ == "__main__":
    main()
