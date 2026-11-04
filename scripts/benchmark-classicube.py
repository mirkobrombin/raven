#!/usr/bin/env python3
"""Compare the frame rate of ClassiCube's game under Raven and under Wine on the same compositor and Vulkan driver.

GAME_EXE is a Direct3D 11 build of ClassiCube compiled with -DCC_BENCH_FPS: it draws without a frame limit, counts the frames between
five and fifteen seconds after its first loaded-world frame and writes the frames per second to bench_fps.txt beside itself, then exits. The program
starts straight into singleplayer (--singleplayer) with the texture and sound packs of GAME_DIR, the frame limit off and DXVK for
Direct3D. Each runtime runs REPEATS times on its own isolated headless compositor; the table shows the median.

usage: benchmark-classicube.py RAVEN WINE GAME_EXE GAME_DIR DXVK_PE64_DIRECTORY WORKDIR [REPEATS]

The measured ClassiCube source revision is 33891a4e7e586941240261bc911312f228be57d3 from
https://github.com/ClassiCube/ClassiCube. In that checkout, normalize src/Game.c and src/Server.c to LF before applying
testdata/classicube/benchmark.patch from this repository, then build with:
  make windows CC=x86_64-w64-mingw32-gcc 'CFLAGS+=-DCC_GFX_BACKEND=CC_GFX_BACKEND_D3D11 -DCC_BENCH_FPS' -j2
Supply the game's texture and sound packs separately in GAME_DIR. The patch's upstream context keeps the
ClassiCube license in testdata/classicube/LICENSE; the game executable and packs are not distributed here.
The benchmark build fixes the generated world's seed to 1; runs use a 640 by 480 window and view distance 128.
"""

from pathlib import Path
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from lavapipe import icd as lavapipe_icd

def wineserver(wine):
    return next((str(path) for path in (wine.with_name("wineserver"), Path("/usr/lib/wine/wineserver"),
                 Path("/usr/lib/x86_64-linux-gnu/wine/wineserver")) if path.exists()), "wineserver")


def run_once(runtime, raven, wine, game, prefix, work):
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
            # The game reads its resources from its working directory; drive Z is the host's root.
            directory = "--directory=Z:" + str(game.parent).replace("/", "\\")
            command = [str(raven), "--run-process", "--execution=native", "--timeout-ms=120000", "--desktop=wayland", directory, str(game), "--",
                       "--singleplayer"]
        else:
            env |= {"WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "mscoree=;mshtml=;d3d11,dxgi,d3d10core=n"}
            command = [str(wine), str(game), "--singleplayer"]
        result = game.parent / "bench_fps.txt"
        result.unlink(missing_ok=True)
        process = subprocess.Popen(command, env=env, cwd=game.parent, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            process.wait(timeout=float(os.environ.get("RAVEN_BENCH_TIMEOUT", "120")))
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        if runtime == "wine":
            subprocess.run([wineserver(wine), "-k"], env=env, capture_output=True, timeout=60)
        text = result.read_text().strip() if result.exists() else ""
        config = game.parent / "bench_config.txt"
        configured = config.is_file() and config.read_text().strip() == "1 128 64 128 640 480"
        return int(text) if configured and process.returncode == 0 and re.fullmatch(r"[1-9]\d*", text) else None
    finally:
        compositor.terminate()
        compositor.wait(10)


def main():
    raven, wine, game_exe, game_dir, dxvk, work = (Path(value).resolve() for value in sys.argv[1:7])
    repeats = int(sys.argv[7]) if len(sys.argv) > 7 else 3
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    warm = work / "wine-warm"
    warm_env = {"PATH": "/usr/bin:/bin", "HOME": os.environ["HOME"], "WINEPREFIX": str(warm), "WINEDEBUG": "-all"}
    subprocess.run([str(wine), "wineboot", "-u"], env=warm_env, capture_output=True, timeout=300)
    subprocess.run([str(wine), "reg", "add", "HKCU\\Software\\Wine\\Drivers", "/v", "Graphics", "/d", "wayland", "/f"], env=warm_env,
                   capture_output=True, timeout=120)
    subprocess.run([wineserver(wine), "-w"], env=warm_env, capture_output=True, timeout=120)
    subprocess.run([wineserver(wine), "-k"], env=warm_env, capture_output=True, timeout=60)
    rates = {"raven": [], "wine": []}
    measurements = []
    for runtime in ("raven", "wine"):
        for index in range(repeats):
            # A fresh copy of the game for every run: the program writes its options and the result beside itself.
            game = work / f"game-{runtime}-{index}"
            game.mkdir()
            for name in ("texpacks", "audio"):
                if (game_dir / name).exists():
                    shutil.copytree(game_dir / name, game / name)
            (game / "options.txt").write_text("fpslimit=LimitNone\nwindow-width=640\nwindow-height=480\nviewdist=128\n")
            shutil.copy(game_exe, game / "ClassiCube.exe")
            for name in ("d3d11.dll", "dxgi.dll", "d3d10core.dll"):
                shutil.copy(dxvk / name, game / name)
            prefix = work / f"prefix-{runtime}-{index}"
            if runtime == "wine":
                shutil.copytree(warm, prefix, symlinks=True)
            rate = run_once(runtime, raven, wine, game / "ClassiCube.exe", prefix, work)
            measurements.append(dict(runtime=runtime, trial=index + 1, frames_per_second=rate))
            print(runtime, index, rate, flush=True)
            if rate:
                rates[runtime].append(rate)
    (work / "samples.json").write_text(json.dumps(measurements, indent=2) + "\n")
    print(f"{'runtime':8} {'frames per second':>18}  runs")
    for runtime, values in rates.items():
        print(f"{runtime:8} {statistics.median(values):18.1f}  {len(values)}/{repeats}" if values else f"{runtime:8} {'-':>18}  0/{repeats}")


if __name__ == "__main__":
    main()
