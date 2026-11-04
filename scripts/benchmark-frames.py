#!/usr/bin/env python3
"""Compare how fast Raven and Wine present Direct3D 11 frames from the same program on the same compositor and driver.

testdata/d3d11/present_rate.exe draws a triangle into a swap chain and presents it without waiting for the display, 2000 times after a
warm-up, timing those presents with the performance counter; it writes the frame count and the microseconds they took. That gives the
presentation rate of the whole path (DXVK, the Vulkan driver and the compositor) for a 256 by 256 window. Each runtime runs REPEATS times
on its own isolated headless compositor and the table shows the median.

usage: benchmark-frames.py RAVEN WINE TESTDATA DXVK_PE64_DIRECTORY WORKDIR [REPEATS]
"""

from pathlib import Path
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from lavapipe import icd as lavapipe_icd


def run_once(runtime, raven, wine, program, prefix, work, index):
    runtime_dir = Path(f"/run/user/{os.getuid()}/raven-labwc-bench")
    runtime_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    for name in ("wayland-0", "wayland-0.lock"):
        (runtime_dir / name).unlink(missing_ok=True)
    env = {"PATH": "/usr/bin:/bin", "HOME": os.environ["HOME"], "TMPDIR": os.environ.get("TMPDIR", str(work)), "XDG_RUNTIME_DIR": str(runtime_dir),
           "WAYLAND_DISPLAY": "wayland-0", "VK_DRIVER_FILES": str(lavapipe_icd()), "LP_NUM_THREADS": "2"}
    compositor = subprocess.Popen(["labwc"], env=env | {"WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1"},
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if (runtime_dir / "wayland-0").exists():
                break
            time.sleep(0.1)
        time.sleep(0.5)
        if runtime == "raven":
            env["RAVEN_PREFIX"] = str(prefix)
            command = [str(raven), "--run-process", "--execution=native", "--timeout-ms=100000", "--desktop=wayland", str(program)]
        else:
            env |= {"WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "mscoree=;mshtml=;d3d11,dxgi,d3d10core=n"}
            command = [str(wine), str(program)]
        marker = prefix / "drive_c" / "present_rate.txt"
        marker.unlink(missing_ok=True)
        process = subprocess.Popen(command, env=env, cwd=program.parent, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.time() + float(os.environ.get("RAVEN_BENCH_TIMEOUT", "120"))
            while time.time() < deadline and process.poll() is None and not marker.exists():
                time.sleep(0.05)
            time.sleep(0.2)
            fields = marker.read_text().split() if marker.exists() else []
            if len(fields) != 2 or not fields[0].isdigit():
                return None
            return int(fields[0]) / (int(fields[1]) / 1e6)
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
            if runtime == "wine":
                subprocess.run([str(wine.with_name("wineserver") if wine.with_name("wineserver").exists() else "/usr/lib/x86_64-linux-gnu/wine/wineserver"), "-k"],
                               env=env, capture_output=True, timeout=60)
    finally:
        compositor.terminate()
        compositor.wait(10)


def main():
    raven, wine, testdata, dxvk, work = (Path(value).resolve() for value in sys.argv[1:6])
    repeats = int(sys.argv[6]) if len(sys.argv) > 6 else 3
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    program_dir = work / "program"
    program_dir.mkdir()
    for name in ("d3d11.dll", "dxgi.dll", "d3d10core.dll"):
        shutil.copy(dxvk / name, program_dir / name)
    shutil.copy(testdata / "d3d11" / "present_rate.exe", program_dir / "present_rate.exe")
    warm = work / "wine-warm"
    warm_env = {"PATH": "/usr/bin:/bin", "HOME": os.environ["HOME"], "WINEPREFIX": str(warm), "WINEDEBUG": "-all"}
    subprocess.run([str(wine), "wineboot", "-u"], env=warm_env, capture_output=True, timeout=300)
    server = wine.with_name("wineserver")
    if not server.exists():
        server = Path("/usr/lib/x86_64-linux-gnu/wine/wineserver")
    subprocess.run([str(server), "-k"], env=warm_env, capture_output=True, timeout=60)
    rates = {"raven": [], "wine": []}
    measurements = []
    for runtime in ("raven", "wine"):
        for index in range(repeats):
            prefix = work / f"prefix-{runtime}-{index}"
            if runtime == "wine":
                shutil.copytree(warm, prefix, symlinks=True)
            rate = run_once(runtime, raven, wine, program_dir / "present_rate.exe", prefix, work, index)
            measurements.append(dict(runtime=runtime, trial=index + 1, frames_per_second=rate))
            if rate:
                rates[runtime].append(rate)
    (work / "samples.json").write_text(json.dumps(measurements, indent=2) + "\n")
    print(f"{'runtime':8} {'frames per second':>18}  runs")
    for runtime, values in rates.items():
        print(f"{runtime:8} {statistics.median(values):18.1f}  {len(values)}/{repeats}" if values else f"{runtime:8} {'-':>18}  0/{repeats}")


if __name__ == "__main__":
    main()
