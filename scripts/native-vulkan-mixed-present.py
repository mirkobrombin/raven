#!/usr/bin/env python3
"""Present to a hidden window and to a shown one in one call, on Wayland.

testdata/native_cpu/hidden_present_client.c with "mixed" makes a swap chain over a hidden window, which Raven's Vulkan bridge keeps
itself, and one over a shown window, which the driver keeps, and gives both to each vkQueuePresentKHR. The program must return 0.

usage: native-vulkan-mixed-present.py RAVEN CLIENT WORKDIR
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

from lavapipe import icd as lavapipe_icd


def main():
    raven, client, work = (Path(argument).resolve() for argument in sys.argv[1:4])
    work.mkdir(parents=True, exist_ok=True)
    runtime = Path(tempfile.mkdtemp(prefix="wl-", dir=os.environ.get("TMPDIR", str(work))))
    env = os.environ | {"XDG_RUNTIME_DIR": str(runtime), "WAYLAND_DISPLAY": "wayland-0",
                        "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1",
                        "RAVEN_PREFIX": str(work / "prefix")}
    # The compositor draws in software, so it shows only what a software driver gives it.
    if "VK_DRIVER_FILES" not in env and lavapipe_icd().exists():
        env["VK_DRIVER_FILES"] = str(lavapipe_icd())
    compositor = None
    try:
        with (work / "compositor.log").open("wb") as log:
            compositor = subprocess.Popen(["labwc"], env=env, stdout=log, stderr=log)
            deadline = time.monotonic() + 10
            while not (runtime / "wayland-0").exists():
                if compositor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("isolated compositor did not start")
                time.sleep(0.05)
            result = subprocess.run([str(raven), "--run-process", "--execution=native", "--timeout-ms=20000", "--desktop=wayland",
                                     str(client), "--", "mixed"], env=env, capture_output=True, text=True, timeout=40)
        output = result.stdout + result.stderr
        if result.returncode or "Native entry returned 0 " not in output or "Validation Error:" in output or "VUID-" in output:
            raise RuntimeError(f"mixed present failed with status {result.returncode}: {output}")
        print("A hidden window and a shown window took their frames from one present")
    finally:
        if compositor is not None and compositor.poll() is None:
            compositor.kill()
            compositor.wait(timeout=10)
        shutil.rmtree(runtime)


if __name__ == "__main__":
    main()
