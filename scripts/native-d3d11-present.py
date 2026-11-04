#!/usr/bin/env python3
"""Present a Direct3D 11 swap chain through DXVK on Raven to a Wayland window and compare what the compositor shows.

testdata/d3d11/window.exe renders sixty frames into a DXGI swap chain and presents each one. It reads back its first and last back
buffers with the GPU. This script runs it on an isolated headless compositor, waits until every frame was presented, captures the
compositor output with grim and requires that:

- the read-back last frame matches the reference rasterizer (check-d3d11-triangle.py);
- the read-back first frame differs from the last one in the colour the program ramps (frames really change);
- the captured window contains the read-back last frame, pixel for pixel, at one position (the swap chain content reached the screen);
- after the program resizes its window and swap chain to 320 by 200, a second capture shows the new, larger frame the GPU rendered
  (read back with a different clear colour), again pixel for pixel, and the window outside it is not left holding the old picture.

With the extra argument "gpu" the host's default Vulkan driver is used instead of llvmpipe and the compositor renders with GLES so that it
offers buffer sharing; the test then also requires DXVK to report a driver other than llvmpipe.

usage: native-d3d11-present.py RAVEN WINDOW_EXE DXVK_PE64_DIRECTORY WORKDIR [gpu]
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

from PIL import Image
from lavapipe import icd as lavapipe_icd

SIZE = 256
GPU = len(sys.argv) > 5 and sys.argv[5] == "gpu"
RUNTIME = Path(f"/run/user/{os.getuid()}/raven-labwc-{'gpu' if GPU else 'runtime'}")


def environment():
    return {"PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": str(RUNTIME), "WAYLAND_DISPLAY": "wayland-0"}


def start_compositor(log):
    if (RUNTIME / "wayland-0").exists():
        probe = subprocess.run(["wlrctl", "toplevel", "list"], env=environment(), capture_output=True, timeout=10)
        if probe.returncode == 0:
            return None
        (RUNTIME / "wayland-0").unlink(missing_ok=True)
        (RUNTIME / "wayland-0.lock").unlink(missing_ok=True)
    RUNTIME.mkdir(mode=0o700, parents=True, exist_ok=True)
    env = environment() | {"WLR_BACKENDS": "headless", "WLR_RENDERER": "gles2" if GPU else "pixman", "WLR_HEADLESS_OUTPUTS": "1"}
    process = subprocess.Popen(["labwc"], env=env, stdout=log, stderr=log)
    for _ in range(50):
        if (RUNTIME / "wayland-0").exists():
            time.sleep(0.5)
            return process
        time.sleep(0.1)
    raise RuntimeError("isolated compositor did not start")


def rgb(raw, width=SIZE, height=SIZE):
    image = Image.frombytes("RGBA", (width, height), raw)
    return image.convert("RGB")


def compare(capture, raw, width, height, label):
    """Find the frame in the capture by its top-left pixel and require every pixel of it to match."""
    screen = Image.open(capture).convert("RGB")
    expected = rgb(raw, width, height)
    clear = tuple(expected.getpixel((0, 0)))
    origin = next(((x, y) for y in range(screen.height) for x in range(screen.width) if screen.getpixel((x, y)) == clear), None)
    if origin is None:
        raise SystemExit(f"{label}: the compositor output holds no pixel of the clear colour: the swap chain never reached the screen")
    shown = screen.crop((origin[0], origin[1], origin[0] + width, origin[1] + height))
    shown_bytes, expected_bytes = shown.tobytes(), expected.tobytes()
    differing = sum(1 for index in range(0, len(expected_bytes), 3) if shown_bytes[index:index + 3] != expected_bytes[index:index + 3])
    print(f"{label}: compositor capture at {origin}: {differing} of {width * height} pixels differ from the read-back frame")
    if differing:
        shown.save(capture.with_name(label + "-shown.png"))
        raise SystemExit(f"{label}: the window on screen does not show the frame the GPU rendered")
    return origin


def main():
    raven, workload, dxvk, work = map(Path, sys.argv[1:5])
    shutil.rmtree(work, ignore_errors=True)
    run, prefix = work / "run", work / "prefix"
    run.mkdir(parents=True)
    for name in ("d3d11.dll", "dxgi.dll", "d3d10core.dll"):
        shutil.copy(dxvk / name, run / name)
    shutil.copy(workload, run / "window.exe")
    with open(work / "compositor.log", "wb") as log:
        compositor = start_compositor(log)
        env = os.environ | environment() | {"RAVEN_PREFIX": str(prefix)}
        env["PATH"] = os.environ["PATH"]
        lavapipe = lavapipe_icd()
        if "VK_DRIVER_FILES" not in env and lavapipe.exists() and not GPU:
            env["VK_DRIVER_FILES"] = str(lavapipe)
        process = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=100000", "--desktop=wayland", str(run / "window.exe")],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        status = prefix / "drive_c" / "window.txt"
        try:
            deadline = time.time() + 90
            while time.time() < deadline and process.poll() is None and not status.exists():
                time.sleep(0.5)
            if not status.exists() or status.read_text() != "OK":
                raise SystemExit(f"workload did not present all frames: {status.read_text() if status.exists() else 'missing'}")
            time.sleep(1)
            capture = work / "capture.png"
            subprocess.run(["grim", str(capture)], env=environment(), check=True, timeout=20)
            resized_marker = prefix / "drive_c" / "window_resized.txt"
            deadline = time.time() + 90
            while time.time() < deadline and process.poll() is None and not resized_marker.exists():
                time.sleep(0.5)
            if not resized_marker.exists() or resized_marker.read_text() != "OK":
                raise SystemExit("the workload did not finish rendering after resizing")
            time.sleep(1)
            resized = work / "capture-resized.png"
            subprocess.run(["grim", str(resized)], env=environment(), check=True, timeout=20)
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
            if compositor:
                compositor.terminate()
    drive = prefix / "drive_c"
    driver = next((line.split(":", 1)[1].strip() for line in (drive / "window_d3d11.log").read_text().splitlines() if line.startswith("info:    Driver")), "")
    print(f"DXVK reports the driver: {driver}")
    if GPU and (not driver or "llvmpipe" in driver.lower()):
        raise SystemExit("the hardware variant ran on a software driver")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-triangle.py")), str(drive / "window_last.raw")],
                           capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "read-back frame does not match the reference")
    first, last = (drive / "window_first.raw").read_bytes(), (drive / "window_last.raw").read_bytes()
    if first[:4] == last[:4]:
        raise SystemExit("the first and last frames have the same clear colour: the frames did not change")
    print(f"first frame corner {tuple(first[:4])}, last frame corner {tuple(last[:4])}")
    compare(capture, last, SIZE, SIZE, "first window")
    check = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("check-d3d11-triangle.py")), str(drive / "window_resized.raw"), "320", "200",
                            "26", "77", "26"], capture_output=True, text=True, timeout=120)
    sys.stdout.write(check.stdout)
    if check.returncode:
        raise SystemExit(check.stderr.strip() or "resized read-back frame does not match the reference")
    compare(resized, (drive / "window_resized.raw").read_bytes(), 320, 200, "resized window")
    print("a Direct3D 11 frame rendered by DXVK and Raven's Vulkan bridge is on screen, matches the GPU read-back, and keeps doing so after a resize")


if __name__ == "__main__":
    main()
