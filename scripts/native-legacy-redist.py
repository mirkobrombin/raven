#!/usr/bin/env python3
"""Install the Visual C++ 2005 and 2008 redistributables through Raven into one prefix and run a program built against each runtime.

Each package first starts as a user would start it, and its first window is captured before it is stopped. Then both run unmodified
and silent in a fresh prefix, the older first. Each installs its runtime as a side-by-side assembly; a program that names that assembly
in its manifest must load the library from the assembly's folder and run, the two programs side by side in the same prefix. The older
package is then installed again over the newer one, and both programs must still run.

usage: native-legacy-redist.py RAVEN EVIDENCE_DIR INSTALLER CHECKER [INSTALLER CHECKER ...]
"""

from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import sys
import time

import labwc_session

# The compositor and the runs are the ones the 2015-2022 workflow uses, loaded without leaving compiled files in the tree.
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("native_redist", Path(__file__).with_name("native-redist.py"))
native_redist = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(native_redist)


def check(raven, prefix, checker, evidence, step):
    """Runs a program built against one runtime from the prefix's drive, as an installed program runs."""
    target = prefix / "drive_c" / checker.name
    shutil.copy(checker, target)
    log = evidence / f"{step}-{checker.stem}.log"
    code = native_redist.run(raven, prefix, target, [], log, timeout=60)
    if code != 0 or b"Native entry returned 0 " not in log.read_bytes():
        raise RuntimeError(f"{step}: {checker.name} did not run with its side-by-side runtime (status {code})")


def first_window(raven, prefix, installer, picture, log):
    """Starts the installer without arguments and keeps its first window once it is on screen; the installer and the processes it
    started are then stopped."""
    env = native_redist.environment() | {"RAVEN_PREFIX": str(prefix)}
    with log.open("wb") as output:
        process = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=300000", "--desktop=wayland",
                                    str(installer)], env=env, stdout=output, stderr=output, start_new_session=True)
        try:
            deadline = time.time() + 240
            while time.time() < deadline and process.poll() is None:
                time.sleep(1)
                capture = picture.with_suffix(".capture.png")
                if subprocess.run(["grim", str(capture)], env=native_redist.environment(), timeout=20).returncode != 0:
                    continue
                if native_redist.visible_area(capture) >= 20000:
                    # The window may still be painting: one more second, then the picture that counts.
                    time.sleep(1)
                    subprocess.run(["grim", str(picture)], env=native_redist.environment(), timeout=20, check=True)
                    capture.unlink(missing_ok=True)
                    return
                capture.unlink(missing_ok=True)
            raise RuntimeError(f"{installer.name} showed no window")
        finally:
            try:
                os.killpg(process.pid, 9)
            except ProcessLookupError:
                pass
            process.wait(timeout=30)


def main():
    raven, evidence = Path(sys.argv[1]), Path(sys.argv[2])
    pairs = [(Path(sys.argv[index]), Path(sys.argv[index + 1])) for index in range(3, len(sys.argv) - 1, 2)]
    evidence.mkdir(parents=True, exist_ok=True)
    prefix = evidence / "prefix"
    shutil.rmtree(prefix, ignore_errors=True)
    log = (evidence / "compositor.log").open("wb")
    compositor = labwc_session.start(log, False)
    try:
        for installer, _ in pairs:
            first_window(raven, prefix, installer, evidence / f"{installer.stem}.png", evidence / f"window-{installer.stem}.log")
        for installer, checker in pairs:
            code = native_redist.run(raven, prefix, installer, ["/q"], evidence / f"install-{installer.stem}.log")
            if code not in (0, 3010):
                raise RuntimeError(f"{installer.name} ended with status {code}")
            stores = list((prefix / "drive_c" / "windows" / "WinSxS").glob("x86_microsoft.vc*0.crt_*"))
            if not any(folder.name.startswith(f"x86_microsoft.vc{checker.stem[-2:]}.crt_") for folder in stores):
                raise RuntimeError(f"{installer.name} left no runtime assembly in WinSxS")
            check(raven, prefix, checker, evidence, "installed")
        # Every program runs side by side with the other runtime installed.
        for _, checker in pairs:
            check(raven, prefix, checker, evidence, "together")
        # The older package again, over the newer one.
        installer = pairs[0][0]
        code = native_redist.run(raven, prefix, installer, ["/q"], evidence / f"reinstall-{installer.stem}.log")
        if code not in (0, 3010):
            raise RuntimeError(f"reinstalling {installer.name} ended with status {code}")
        for _, checker in pairs:
            check(raven, prefix, checker, evidence, "reinstalled")
    finally:
        if compositor:
            compositor.terminate()
            compositor.wait(timeout=10)
    print("PASS: installed side by side, each program ran with its own runtime, reinstalled")


if __name__ == "__main__":
    main()
