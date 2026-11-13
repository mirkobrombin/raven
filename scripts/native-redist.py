#!/usr/bin/env python3
"""Install, check, reinstall and remove a Visual C++ redistributable through Raven on an isolated headless compositor.

The bundle runs unmodified from a fresh prefix: first with its progress window (/passive), whose screenshot is kept, then again
silently over the installed copy, then removed. After each step the bundle's own log, the files in the system directory and a program
running in the prefix say what Windows would show.

usage: native-redist.py RAVEN VC_REDIST.EXE CHECKER.EXE SYSTEM_FOLDER EVIDENCE_DIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

from PIL import Image

RUNTIME = Path(f"/run/user/{os.getuid()}/raven-labwc-runtime")


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


def visible_area(path):
    """How much of the screen is not the compositor's black background."""
    image = Image.open(path).convert("L").point(lambda value: 255 if value > 8 else 0)
    return sum(image.histogram()[255:])


def run(raven, prefix, program, arguments, output, watch=None, timeout=600, more=None):
    """Runs a program in the prefix; with watch set, the screen is captured every second and the fullest capture kept there. What `more`
    holds is added to the program's environment."""
    env = environment() | {"RAVEN_PREFIX": str(prefix)} | (more or {})
    if "RAVEN_THEME" in os.environ:
        env["RAVEN_THEME"] = os.environ["RAVEN_THEME"]
    with output.open("wb") as log:
        process = subprocess.Popen([str(raven), "--run-process", "--execution=native", f"--timeout-ms={timeout * 1000}", "--desktop=wayland",
                                    str(program), "--", *arguments], env=env, stdout=log, stderr=log)
        best = 0
        deadline = time.time() + timeout + 30
        while process.poll() is None:
            if time.time() > deadline:
                process.kill()
                raise RuntimeError(f"{program.name} {' '.join(arguments)} did not finish")
            time.sleep(1)
            if watch:
                capture = watch.with_suffix(".capture.png")
                if subprocess.run(["grim", str(capture)], env=environment(), timeout=20).returncode == 0:
                    area = visible_area(capture)
                    if area > best:
                        best = area
                        shutil.copy(capture, watch)
                    capture.unlink(missing_ok=True)
    return process.returncode


def bundle_log(prefix, evidence, step):
    """The bundle's main log of the last run, copied into the evidence; its packages' logs go along."""
    temp = next((prefix / "drive_c" / "Users").glob("*/AppData/Local/Temp"))
    logs = sorted(temp.glob("dd_vcredist_*.log"), key=lambda path: path.stat().st_mtime)
    main = [log for log in logs if "_0" not in log.stem.split("_", 3)[-1]]
    if not main:
        raise RuntimeError(f"{step}: the bundle wrote no log")
    for log in logs:
        shutil.copy(log, evidence / f"{step}-{log.name}")
    return main[-1].read_text(encoding="utf-8", errors="replace")


def main():
    raven, installer, checker, folder, evidence = map(Path, sys.argv[1:6])
    evidence.mkdir(parents=True, exist_ok=True)
    prefix = evidence / "prefix"
    shutil.rmtree(prefix, ignore_errors=True)
    system = prefix / "drive_c" / "windows" / str(folder)
    log = (evidence / "compositor.log").open("wb")
    compositor = start_compositor(log)
    try:
        # Install with the progress window, from a prefix that has never seen the runtime.
        code = run(raven, prefix, installer, ["/install", "/passive", "/norestart"], evidence / "install.log", evidence / "progress.png")
        if code != 0:
            raise RuntimeError(f"install ended with status {code}")
        text = bundle_log(prefix, evidence, "install")
        if "Apply complete, result: 0x0" not in text or "Exit code: 0x0" not in text:
            raise RuntimeError("the bundle log does not report a complete install")
        if not (evidence / "progress.png").exists() or visible_area(evidence / "progress.png") < 20000:
            raise RuntimeError("the progress window never showed")
        for name in ("vcruntime140.dll", "msvcp140.dll"):
            if not (system / name).exists():
                raise RuntimeError(f"{name} is not in {folder}")
        code = run(raven, prefix, checker, [], evidence / "check-installed.log", timeout=60)
        if code != 0 or b"Native entry returned 0 " not in (evidence / "check-installed.log").read_bytes():
            raise RuntimeError(f"the installed runtime is not what programs see (status {code})")

        # Installing again over the installed copy succeeds and leaves it in place.
        code = run(raven, prefix, installer, ["/install", "/quiet", "/norestart"], evidence / "reinstall.log")
        if code not in (0, 3010):
            raise RuntimeError(f"reinstall ended with status {code}")
        bundle_log(prefix, evidence, "reinstall")
        if not (system / "vcruntime140.dll").exists():
            raise RuntimeError("reinstalling removed the runtime")

        # Removal takes the files and the runtime key away.
        code = run(raven, prefix, installer, ["/uninstall", "/quiet", "/norestart"], evidence / "uninstall.log")
        if code not in (0, 3010):
            raise RuntimeError(f"uninstall ended with status {code}")
        bundle_log(prefix, evidence, "uninstall")
        code = run(raven, prefix, checker, ["--removed"], evidence / "check-removed.log", timeout=60)
        if code != 0 or b"Native entry returned 0 " not in (evidence / "check-removed.log").read_bytes():
            raise RuntimeError(f"the runtime is still installed after removal (status {code})")
    finally:
        if compositor:
            compositor.terminate()
            compositor.wait(timeout=10)
    print("PASS: installed with its progress window, checked, reinstalled and removed")


if __name__ == "__main__":
    main()
