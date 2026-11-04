#!/usr/bin/env python3
"""Compare Raven with Wine on real Windows programs: how long each takes to show its window, and the CPU time and memory its processes use.

Every run starts the program under one runtime on a headless labwc of its own (Wine through its Wayland driver) and polls the compositor's
window list until the window the workload waits for appears; the wall time is that delay. Each process of a run carries a marker in its
environment, so CPU time and resident memory are summed from /proc for marked processes still alive when the window appears.
Those processes alone are stopped afterwards. Each workload runs once unmeasured to warm the prefix and
caches, then REPEATS times; the table shows medians.

Chrome also runs two pages that report through the page title: a JavaScript workload (milliseconds, lower is better) and a canvas animation
(frames drawn in five seconds, higher is better).

usage: benchmark-apps.py RAVEN TESTDATA CHROME_APPLICATION_DIR CLASSICUBE_EXE DXVK_PE64_DIRECTORY WORKDIR [REPEATS] [WORKLOAD...]
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

TICKS = os.sysconf("SC_CLK_TCK")
# Wine's server sits in the library directory on Debian style installations.
WINE = os.environ.get("RAVEN_BENCH_WINE", "wine")
WINESERVER = os.environ.get("RAVEN_BENCH_WINESERVER") or next((str(path) for path in (Path(WINE).with_name("wineserver"), Path("/usr/lib/wine/wineserver"), Path("/usr/lib/x86_64-linux-gnu/wine/wineserver"))
                   if path.exists()), "wineserver")

JS_PAGE = """<!doctype html><title>JS waiting</title><script>
function work() {
  const start = performance.now();
  let primes = 0;
  for (let n = 2; n < 3000000; n++) { let prime = true; for (let d = 2; d * d <= n; d++) if (n % d === 0) { prime = false; break; } if (prime) primes++; }
  const items = []; for (let i = 0; i < 400000; i++) items.push({key: (i * 7919) % 100003, text: "item" + i});
  items.sort((a, b) => a.key - b.key);
  let text = ""; for (let i = 0; i < 200000; i++) text += String.fromCharCode(65 + i % 26);
  const json = JSON.stringify(items.slice(0, 50000)); JSON.parse(json);
  document.title = "JS " + Math.round(performance.now() - start) + " " + primes + " " + text.length;
}
setTimeout(work, 500);
</script>"""

CANVAS_PAGE = """<!doctype html><title>Canvas waiting</title><canvas id=c width=800 height=600></canvas><script>
const context = document.getElementById("c").getContext("2d");
let frames = 0, start = 0;
function frame(now) {
  if (!start) start = now;
  for (let i = 0; i < 200; i++) { context.fillStyle = "hsl(" + ((i * 37 + frames) % 360) + ",70%,50%)"; context.fillRect((i * 53 + frames * 3) % 760, (i * 29 + frames * 2) % 560, 40, 40); }
  frames++;
  if (now - start < 5000) requestAnimationFrame(frame); else document.title = "Canvas " + frames;
}
setTimeout(() => requestAnimationFrame(frame), 500);
</script>"""

LOAD_PAGE = """<!doctype html><title>Raven Bench Page</title><body><h1>Raven Bench</h1><p>""" + "Lorem ipsum dolor sit amet. " * 400 + "</p></body>"


def titles(env):
    probe = subprocess.run(["wlrctl", "toplevel", "list"], env=env, capture_output=True, text=True, timeout=10)
    return [line.split(": ", 1)[1] if ": " in line else line for line in probe.stdout.splitlines()]


def marked(marker):
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if marker.encode() in (entry / "environ").read_bytes():
                found.append(int(entry.name))
        except OSError:
            continue
    return found


def usage(pids):
    """CPU seconds (with reaped children) and resident peak MiB of the given processes."""
    cpu, memory = 0.0, 0.0
    for pid in pids:
        try:
            fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            cpu += sum(int(value) for value in fields[11:15]) / TICKS
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith("VmHWM:"):
                    memory += int(line.split()[1]) / 1024.0
        except OSError:
            continue
    return cpu, memory


def stop(marker):
    for _ in range(20):
        pids = marked(marker)
        if not pids:
            return
        for pid in pids:
            try:
                os.kill(pid, 9)
            except OSError:
                pass
        time.sleep(0.2)


def main():
    raven, testdata, chrome, classicube, dxvk, work = (Path(value).resolve() for value in sys.argv[1:7])
    repeats = int(sys.argv[7]) if len(sys.argv) > 7 else 3
    selected = set(sys.argv[8:])
    work.mkdir(parents=True, exist_ok=True)
    runtime_dir = Path(f"/run/user/{os.getuid()}/raven-labwc-bench")
    runtime_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    for name in ("wayland-0", "wayland-0.lock"):
        (runtime_dir / name).unlink(missing_ok=True)
    base = {"PATH": str(Path(WINE).parent) + ":/usr/bin:/bin", "HOME": os.environ["HOME"], "TMPDIR": os.environ.get("TMPDIR", str(work)), "XDG_RUNTIME_DIR": str(runtime_dir),
            "WAYLAND_DISPLAY": "wayland-0", "VK_DRIVER_FILES": str(lavapipe_icd()), "LP_NUM_THREADS": "2"}
    compositor = subprocess.Popen(["labwc"], env=base | {"WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1"},
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 20
        while not (runtime_dir / "wayland-0").exists() and time.time() < deadline:
            time.sleep(0.1)
        # The programs' files, copied once so that neither runtime changes the originals.
        programs = work / "programs"
        if not programs.exists():
            programs.mkdir()
            if (testdata / "notepad.exe").exists():
                shutil.copy(testdata / "notepad.exe", programs / "notepad.exe")
            shutil.copytree(testdata / "npp", programs / "npp")
            if (testdata / "winmine").exists():
                shutil.copytree(testdata / "winmine", programs / "winmine")
            if chrome.is_dir():
                shutil.copytree(chrome, programs / "chrome")
            (programs / "classicube").mkdir()
            if classicube.is_file():
                shutil.copy(classicube, programs / "classicube" / "ClassiCube.exe")
            for name, page in (("load.html", LOAD_PAGE), ("js.html", JS_PAGE), ("canvas.html", CANVAS_PAGE)):
                (programs / name).write_text(page)
        # Wine: a prefix that is initialised and draws through its Wayland driver; DXVK serves Direct3D as it does for Raven.
        wine_prefix = work / "wine-prefix"
        wine_env = base | {"WINEPREFIX": str(wine_prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "mscoree=;mshtml=;d3d11,dxgi,d3d10core=n"}
        if not wine_prefix.exists():
            subprocess.run([WINE, "wineboot", "-u"], env=wine_env, capture_output=True, timeout=300)
            subprocess.run([WINE, "reg", "add", "HKCU\\Software\\Wine\\Drivers", "/v", "Graphics", "/d", "wayland", "/f"], env=wine_env,
                           capture_output=True, timeout=120)
            subprocess.run([WINESERVER, "-w"], env=wine_env, capture_output=True, timeout=120)
        for name in ("d3d11.dll", "dxgi.dll", "d3d10core.dll"):
            shutil.copy(dxvk / name, programs / "classicube" / name)
        raven_prefix = work / "raven-prefix"
        raven_env = base | {"RAVEN_PREFIX": str(raven_prefix), "RAVEN_D3D_DIRECTORY": str(dxvk), "RAVEN_COMMIT_LIMIT_MB": "4096"}

        # Both runtimes map the host's root to drive Z.
        def page(name):
            return "file:///Z:" + str(programs / name)

        chrome_flags = ["--no-sandbox", "--disable-gpu", "--no-first-run", "--no-default-browser-check", "--disable-background-networking"]
        # name, program, arguments, the title that ends the wait (a pattern), and how to read a result from it.
        workloads = [
            ("notepad", programs / "notepad.exe", [], r"Notepad$", None),
            # Wine runs programs as an administrator, which Notepad++ puts in its title.
            ("notepad++", programs / "npp" / "notepad++.exe", [], r"Notepad\+\+( \[Administrator\])?$", None),
            ("minesweeper", programs / "winmine" / "WINMINE.EXE", [], r"^Minesweeper$", None),
            ("classicube", programs / "classicube" / "ClassiCube.exe", [], r"^ClassiCube", None),
            ("chrome-load", programs / "chrome" / "chrome.exe", chrome_flags + [page("load.html")], r"^Raven Bench Page", None),
            ("chrome-js", programs / "chrome" / "chrome.exe", chrome_flags + [page("js.html")], r"^JS \d+", r"^JS (\d+)"),
            ("chrome-canvas", programs / "chrome" / "chrome.exe", chrome_flags + [page("canvas.html")], r"^Canvas \d+", r"^Canvas (\d+)"),
        ]
        rows, measurements = [], []
        for name, program, arguments, ready, result in workloads:
            if selected and name not in selected:
                continue
            if not program.is_file():
                rows.extend((name, runtime, None, None, None, None, "did not run: input absent") for runtime in ("raven", "wine"))
                continue
            for runtime in ("raven", "wine"):
                samples = []
                for index in range(repeats + 1):
                    marker = f"RAVEN_BENCH_RUN={name}-{runtime}-{index}-{os.getpid()}"
                    key, value = marker.split("=", 1)
                    if runtime == "raven":
                        command = [str(raven), "--run-process", "--execution=native", "--timeout-ms=600000", "--desktop=wayland", str(program)]
                        if arguments:
                            command += ["--"] + arguments
                        env = raven_env | {key: value}
                    else:
                        command = [WINE, str(program)] + arguments
                        env = wine_env | {key: value}
                    start = time.perf_counter()
                    process = subprocess.Popen(command, env=env, cwd=str(program.parent), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                               start_new_session=True)
                    shown, measured = None, None
                    while time.perf_counter() - start < float(os.environ.get("RAVEN_BENCH_TIMEOUT", "300")):
                        matches = [title for title in titles(base) if re.search(ready, title)]
                        if matches:
                            shown = time.perf_counter() - start
                            if result:
                                measured = float(re.search(result, matches[0]).group(1))
                            break
                        if process.poll() is not None and not marked(marker):
                            break
                        time.sleep(0.05)
                    cpu, memory = usage(marked(marker))
                    measurements.append(dict(workload=name, runtime=runtime, trial=index, warmup=index == 0,
                                             window_seconds=shown, cpu=cpu, rss_mib=memory, result=measured))
                    stop(marker)
                    process.wait(timeout=30)
                    if runtime == "wine":
                        subprocess.run([WINESERVER, "-k"], env=env, capture_output=True, timeout=60)
                    if index and shown is not None:
                        samples.append((shown, cpu, memory, measured))
                    time.sleep(1)
                if not samples:
                    rows.append((name, runtime, None, None, None, None, "no window"))
                    continue
                result_value = statistics.median(s[3] for s in samples) if result else None
                rows.append((name, runtime, statistics.median(s[0] for s in samples), statistics.median(s[1] for s in samples),
                             statistics.median(s[2] for s in samples), result_value, f"{len(samples)}/{repeats} runs"))
                print(rows[-1], flush=True)
        (work / "samples.json").write_text(json.dumps(measurements, indent=2) + "\n")
        print(f"{'workload':14} {'runtime':6} {'window s':>9} {'cpu s':>8} {'RSS MiB':>9} {'result':>9}  note")
        for name, runtime, wall, cpu, memory, value, note in rows:
            if wall is None:
                print(f"{name:14} {runtime:6} {'-':>9} {'-':>8} {'-':>9} {'-':>9}  {note}")
            else:
                shown_value = f"{value:9.0f}" if value is not None else f"{'':>9}"
                print(f"{name:14} {runtime:6} {wall:9.2f} {cpu:8.2f} {memory:9.1f} {shown_value}  {note}")
    finally:
        compositor.terminate()
        try:
            compositor.wait(timeout=10)
        except subprocess.TimeoutExpired:
            compositor.kill()


if __name__ == "__main__":
    main()
