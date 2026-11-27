"""An isolated headless labwc that tests show their windows on, one for software rendering and one for the GPU.

The compositor of a kind is shared: a test that finds one answering reuses it and leaves it running, as the next test of the same kind
would start the same one. Tests that use it hold the raven_compositor resource lock.
"""

from pathlib import Path
import os
import subprocess
import time


def runtime(gpu):
    """The compositor's runtime directory, under RAVEN_TEST_RUNTIME or the user's own."""
    base = Path(os.environ.get("RAVEN_TEST_RUNTIME") or f"/run/user/{os.getuid()}")
    return base / f"raven-labwc-{'gpu' if gpu else 'runtime'}"


def environment(gpu):
    return {"PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": str(runtime(gpu)), "WAYLAND_DISPLAY": "wayland-0"}


def start(log, gpu):
    """The compositor started here, or None when one already answers. With the GPU it renders with GLES so that it offers buffer
    sharing; without, with pixman."""
    directory = runtime(gpu)
    if (directory / "wayland-0").exists():
        probe = subprocess.run(["wlrctl", "toplevel", "list"], env=environment(gpu), capture_output=True, timeout=10)
        if probe.returncode == 0:
            return None
        (directory / "wayland-0").unlink(missing_ok=True)
        (directory / "wayland-0.lock").unlink(missing_ok=True)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    env = environment(gpu) | {"WLR_BACKENDS": "headless", "WLR_RENDERER": "gles2" if gpu else "pixman", "WLR_HEADLESS_OUTPUTS": "1"}
    process = subprocess.Popen(["labwc"], env=env, stdout=log, stderr=log)
    for _ in range(50):
        if (directory / "wayland-0").exists():
            time.sleep(0.5)
            return process
        time.sleep(0.1)
    raise RuntimeError("isolated compositor did not start")
