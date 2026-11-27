#!/usr/bin/env python3
"""Install real Windows programs with their own installers, then start them, under Raven and under Wine, and compare.

Each case runs its installer silently in a fresh prefix of each runtime, on a headless labwc of its own (Wine through its Wayland driver),
checks that the program's file is where the installer puts it on Windows, starts the program and waits for its main window. What each
runtime achieved, how long it took and a screenshot of the window go into a table, results.md, which the script writes again on every run.
results.json also records installation, process start, main window and the Notepad++ editing workflow. Each observed successful stage
counts for 25 percentage points; an untested workflow earns no points. The input directory supplies installers outside the source tree.

usage: benchmark-app-matrix.py RAVEN APPS_DIR EVIDENCE_DIR [CASE...]
"""

from pathlib import Path
import os
import json
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import importlib.util

import labwc_session
_spec = importlib.util.spec_from_file_location("native_redist", Path(__file__).with_name("native-redist.py"))
native_redist = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(native_redist)

WINE = os.environ.get("RAVEN_BENCH_WINE", "wine")
WINESERVER = os.environ.get("RAVEN_BENCH_WINESERVER") or next((str(path) for path in (Path(WINE).with_name("wineserver"), Path("/usr/lib/wine/wineserver"), Path("/usr/lib/x86_64-linux-gnu/wine/wineserver"))
                   if path.exists()), "wineserver")

# The case, the installer and how it runs silently (an MSI goes through msiexec), the program it installs, and the words the title of
# its main window holds; None for a program whose title is the folder it shows.
CASES = (
    ("7zip", "7z2409-x64.exe", ["/S"], r"C:\Program Files\7-Zip\7zFM.exe", None),
    ("7zip-msi", "7z2409-x64.msi", ["/qn"], r"C:\Program Files\7-Zip\7zFM.exe", None),
    ("notepadpp", "npp.8.7.1.Installer.x64.exe", ["/S"], r"C:\Program Files\Notepad++\notepad++.exe", "Notepad++"),
    ("vlc", "vlc-3.0.21-win64.exe", ["/S", "/L=1033"], r"C:\Program Files\VideoLAN\VLC\vlc.exe", "VLC media player"),
    ("audacity", "audacity-win-3.6.4-64bit.exe", ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], r"C:\Program Files\Audacity\Audacity.exe",
     "Audacity"),
    ("keepass", "KeePass-2.57-Setup.exe", ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"],
     r"C:\Program Files\KeePass Password Safe 2\KeePass.exe", "KeePass"),
    ("irfanview", "iview467_x64_setup.exe", ["/silent", r"/folder=C:\Program Files\IrfanView"], r"C:\Program Files\IrfanView\i_view64.exe",
     "IrfanView"),
)


def host_path(prefix, windows):
    """Where a C: path of a prefix lies on the host."""
    relative = windows[3:].replace("\\", "/")
    return prefix / "drive_c" / relative


def processes_with(marker):
    found = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                if marker.encode() in (entry / "environ").read_bytes():
                    found.append(int(entry.name))
            except OSError:
                pass
    return found


def stop(marker):
    for pid in processes_with(marker):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def window_titles():
    probe = subprocess.run(["wlrctl", "toplevel", "list"], env=native_redist.environment(), capture_output=True, text=True, timeout=10)
    return probe.stdout if probe.returncode == 0 else ""


class Runtime:
    def __init__(self, name, raven, work):
        self.name, self.raven, self.prefix = name, raven, work / f"{name}-prefix"
        self.marker = f"RAVEN_MATRIX_RUN={name}-{os.getpid()}"

    def environment(self):
        env = native_redist.environment() | {self.marker.split("=")[0]: self.marker.split("=")[1]}
        if self.name == "wine":
            env |= {"WINEPREFIX": str(self.prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "mscoree=;mshtml=", "HOME": str(self.prefix.parent)}
        else:
            env |= {"RAVEN_PREFIX": str(self.prefix)}
        return env

    def command(self, program, arguments, wait):
        if self.name == "wine":
            return [WINE, str(program), *arguments]
        command = [str(self.raven), "--run-process", "--execution=native", "--timeout-ms=900000", "--desktop=wayland", str(program)]
        return command + (["--", *arguments] if arguments else [])

    def fresh(self):
        shutil.rmtree(self.prefix, ignore_errors=True)
        if self.name == "wine":
            env = self.environment()
            subprocess.run([WINE, "wineboot", "-i"], env=env, capture_output=True, timeout=300)
            subprocess.run([WINE, "reg", "add", "HKCU\\Software\\Wine\\Drivers", "/v", "Graphics", "/d", "wayland", "/f"], env=env,
                           capture_output=True, timeout=120)
            subprocess.run([WINESERVER, "-w"], env=env, timeout=120)

    def finish(self):
        stop(self.marker)
        if self.name == "wine":
            subprocess.run([WINESERVER, "-k"], env=self.environment(), capture_output=True, timeout=60)


def install(runtime, installer, arguments, log):
    """Runs the installer to its end; the seconds it took, or None when it failed or never ended."""
    if installer.suffix == ".msi":
        program = "msiexec" if runtime.name == "wine" else runtime.raven.parent / "msiexec.exe"
        arguments = ["/i", "Z:" + str(installer).replace("/", "\\"), *arguments]
    else:
        program = installer
    start = time.time()
    with log.open("wb") as output:
        process = subprocess.Popen(runtime.command(program, arguments, True), env=runtime.environment(), stdout=output, stderr=output)
        try:
            process.wait(timeout=float(os.environ.get("RAVEN_MATRIX_TIMEOUT", "900")))
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            return None
    if runtime.name == "wine":
        subprocess.run([WINESERVER, "-w"], env=runtime.environment(), timeout=300)
    return time.time() - start if process.returncode in (0, 194) else None


def start(runtime, program, title, picture, log):
    """Starts the program and waits for a window whose title holds `title`; the seconds that took, or None."""
    began = time.time()
    with log.open("wb") as output:
        process = subprocess.Popen(runtime.command(program, [], False), env=runtime.environment(), stdout=output, stderr=output)
        time.sleep(0.25)
        launched = process.poll() is None or process.returncode == 0
        deadline = began + float(os.environ.get("RAVEN_MATRIX_WINDOW_TIMEOUT", "120"))
        while time.time() < deadline:
            listed = window_titles()
            if (title.lower() in listed.lower()) if title else listed.strip():
                waited = time.time() - began
                time.sleep(2)
                subprocess.run(["grim", str(picture)], env=native_redist.environment(), timeout=30)
                return waited, launched
            time.sleep(0.5)
    return None, launched


def seconds(value):
    return "failed" if value is None else f"{value:.1f} s"


def main():
    raven, apps, evidence = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    wanted = set(sys.argv[4:])
    evidence.mkdir(parents=True, exist_ok=True)
    log = (evidence / "compositor.log").open("wb")
    compositor = labwc_session.start(log, False)
    rows, failures, measurements = [], [], []
    try:
        for name, installer, arguments, program, title in CASES:
            if wanted and name not in wanted:
                continue
            missing = not (apps / installer).is_file()
            results = {}
            for runtime in (Runtime("raven", raven, evidence / name), Runtime("wine", raven, evidence / name)):
                (evidence / name).mkdir(parents=True, exist_ok=True)
                if not (apps / installer).is_file():
                    measurements.append(dict(case=name, runtime=runtime.name, status="input absent", score=None))
                    results[runtime.name] = (None, None)
                    (evidence / "results.json").write_text(json.dumps(measurements, indent=2) + "\n")
                    continue
                runtime.fresh()
                took = install(runtime, apps / installer, arguments, evidence / name / f"{runtime.name}-install.log")
                installed = took is not None and host_path(runtime.prefix, program).exists()
                shown, launched = None, False
                workflow = None
                if installed:
                    target = program if runtime.name == "wine" else host_path(runtime.prefix, program)
                    shown, launched = start(runtime, target, title, evidence / name / f"{runtime.name}.png", evidence / name / f"{runtime.name}-start.log")
                runtime.finish()
                if name == "notepadpp" and runtime.name == "raven" and shown is not None:
                    workflow_log = evidence / name / "raven-workflow.log"
                    with workflow_log.open("wb") as output:
                        probe = subprocess.run([sys.executable, str(Path(__file__).with_name("native-npp.py")), str(raven),
                                                str(host_path(runtime.prefix, program).parent), str(evidence / name / "workflow")],
                                               stdout=output, stderr=output, timeout=1200)
                    workflow = probe.returncode == 0
                measurements.append(dict(case=name, runtime=runtime.name, installer=installer, install=installed, start=launched,
                                         window=shown is not None, workflow=workflow, score=25 * (int(installed) + int(launched) + int(shown is not None) + int(workflow is True)),
                                         install_seconds=took if installed else None, window_seconds=shown))
                (evidence / "results.json").write_text(json.dumps(measurements, indent=2) + "\n")
                results[runtime.name] = (took if installed else None, shown)
            raven_install, raven_start = results["raven"]
            wine_install, wine_start = results["wine"]
            if missing:
                rows.append(f"| {name} | did not run | did not run | did not run | did not run | not tested |")
                continue
            behind = (raven_install is None or raven_start is None or
                      (wine_install is not None and raven_install > 2 * wine_install + 5) or
                      (wine_start is not None and raven_start > 2 * wine_start + 5))
            if behind:
                failures.append(name)
            rows.append(f"| {name} | {seconds(raven_install)} | {seconds(wine_install)} | {seconds(raven_start)} | {seconds(wine_start)} | "
                        f"{'no' if behind else 'yes'} |")
    finally:
        if compositor:
            compositor.terminate()
            compositor.wait(timeout=10)
    table = ["| Case | Raven install | Wine install | Raven start | Wine start | Install/start comparison |",
             "|---|---|---|---|---|---|", *rows]
    (evidence / "results.md").write_text("\n".join(table) + "\n")
    print("\n".join(table))
    if failures:
        print("FAIL: " + ", ".join(failures))
        sys.exit(1)
    print("PASS: no attempted install/start comparison failed")


if __name__ == "__main__":
    main()
