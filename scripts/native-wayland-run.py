#!/usr/bin/env python3
"""Run a program on Raven's Wayland desktop on an isolated headless compositor, with DXVK's Direct3D on lavapipe, as the tests of programs
that present to their windows need: the app of the Windows Runtime that presents to its core window uses it.

usage: native-wayland-run.py RAVEN PROGRAM DXVK_DIRECTORY WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys

from lavapipe import icd as lavapipe_icd
import labwc_session


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    raven, program, dxvk, work = map(Path, sys.argv[1:5])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    with open(work / "compositor.log", "wb") as log:
        compositor = labwc_session.start(log, False)
        env = os.environ | labwc_session.environment(False) | {"RAVEN_PREFIX": str(work / "prefix"), "RAVEN_D3D_DIRECTORY": str(dxvk)}
        env["PATH"] = os.environ["PATH"]
        if "VK_DRIVER_FILES" not in env and lavapipe_icd().exists():
            env["VK_DRIVER_FILES"] = str(lavapipe_icd())
        try:
            result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=100000", "--desktop=wayland", str(program)],
                                    env=env, timeout=120)
        finally:
            if compositor:
                compositor.terminate()
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
