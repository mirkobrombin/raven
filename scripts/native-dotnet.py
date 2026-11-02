#!/usr/bin/env python3
"""Install the .NET Framework 4.8 through Raven from Microsoft's offline installer and run a console, a WinForms and a WPF program on it.

In a fresh prefix a .NET program first meets no runtime and must show the message Windows shows then. The offline installer unpacks
its packages as it does on Windows (/x), our msiexec installs the full framework and the patch that brings it to 4.8, and the same
program then runs, followed by a WinForms window and a WPF window whose screenshots are kept. The WPF program is compiled here, against
the WPF assemblies the framework just installed, as Mono has none of its own.

With the XNA Framework 4.0 redistributable given, it is installed next through our msiexec, and a program checks what a game checks
first: the framework's key in the 32-bit registry and its assemblies in the global assembly cache.

With the .NET Framework 3.5 SP1 full package given (dotnetfx35.exe), a program built for the 2.0 runtime first meets Windows Features
on Demand, as on Windows 10, then dism.exe turns the feature on from the package, reports it enabled, and the same program runs on the
2.0 runtime.

usage: native-dotnet.py RAVEN NDP48_INSTALLER PROGRAMS_DIR EVIDENCE_DIR [XNA_REDIST_MSI [NETFX35_PACKAGE]]
"""

from pathlib import Path
import os
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import importlib.util
_spec = importlib.util.spec_from_file_location("native_redist", Path(__file__).with_name("native-redist.py"))
native_redist = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(native_redist)

MISSING_TITLE = ".NET Framework Initialization Error"


def window_titles():
    probe = subprocess.run(["wlrctl", "toplevel", "list"], env=native_redist.environment(), capture_output=True, text=True, timeout=10)
    return probe.stdout if probe.returncode == 0 else ""


def missing_runtime(raven, prefix, program, evidence, title=MISSING_TITLE, name="missing"):
    """Starts the program without the runtime it needs and waits for the window Windows shows then; the screenshot goes into the evidence.
    The program and what it started (Features on Demand runs as a process of its own) end afterwards."""
    env = native_redist.environment() | {"RAVEN_PREFIX": str(prefix)}
    with (evidence / f"{name}.log").open("wb") as log:
        process = subprocess.Popen([str(raven), "--run-process", "--execution=native", "--timeout-ms=600000", "--desktop=wayland", str(program)],
                                   env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            # Features on Demand is a process of its own, and the program it was started for has ended by the time it shows.
            deadline = time.time() + 600
            while time.time() < deadline:
                if title in window_titles():
                    time.sleep(2)
                    subprocess.run(["grim", str(evidence / f"{name}.png")], env=native_redist.environment(), timeout=30)
                    return
                time.sleep(1)
            raise RuntimeError(f"no '{title}' window without the runtime (status {process.poll()})")
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except OSError:
                pass
            process.wait()


def step(raven, prefix, program, arguments, evidence, name, watch=None, timeout=3000):
    code = native_redist.run(raven, prefix, program, arguments, evidence / f"{name}.log", watch=watch, timeout=timeout)
    if code not in (0, 3010):
        raise RuntimeError(f"{name}: {program.name} ended with status {code}")


def main():
    raven, installer, programs, evidence = map(Path, sys.argv[1:5])
    xna = Path(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[5] else None
    netfx35 = Path(sys.argv[6]) if len(sys.argv) > 6 and sys.argv[6] else None
    msiexec = raven.parent / "msiexec.exe"
    evidence.mkdir(parents=True, exist_ok=True)
    prefix = evidence / "prefix"
    shutil.rmtree(prefix, ignore_errors=True)
    folder = prefix / "drive_c" / "dotnet"
    folder.mkdir(parents=True)
    for name in ("hello64.exe", "forms64.exe", "xna64.exe", "hello20.exe"):
        shutil.copy(programs / name, folder / name)
    log = (evidence / "compositor.log").open("wb")
    compositor = native_redist.start_compositor(log)
    try:
        missing_runtime(raven, prefix, folder / "hello64.exe", evidence)
        step(raven, prefix, installer, ["/x:C:\\ndp", "/q"], evidence, "unpack")
        step(raven, prefix, msiexec, ["/i", "C:\\ndp\\netfx_Full_x64.msi", "/qn"], evidence, "full")
        runtime = prefix / "drive_c/windows/Microsoft.NET/Framework64/v4.0.30319/clr.dll"
        before = runtime.read_bytes()
        step(raven, prefix, msiexec, ["/p", "C:\\ndp\\netfx_Patch_x64.msp", "/qn", "EXTUI=1"], evidence, "patch")
        if runtime.read_bytes() == before:
            raise RuntimeError("the reliability update left the runtime's files as they were")
        code = native_redist.run(raven, prefix, folder / "hello64.exe", [], evidence / "hello.log", timeout=300)
        said = (folder / "hello-out.txt").read_text() if (folder / "hello-out.txt").exists() else ""
        if code != 42 or "Hello from .NET 4.0.30319" not in said:
            raise RuntimeError(f"the console program did not run on the installed runtime (status {code}, '{said.strip()}')")
        step(raven, prefix, folder / "forms64.exe", [], evidence, "forms", watch=evidence / "forms.png", timeout=300)
        shown = (folder / "forms-out.txt").read_text() if (folder / "forms-out.txt").exists() else ""
        if not shown.startswith("Raven .NET|WinForms on .NET 4.0.30319"):
            raise RuntimeError(f"the WinForms window did not show (wrote '{shown.strip()}')")
        assemblies = prefix / "drive_c" / "windows" / "Microsoft.NET" / "assembly"
        references = [assemblies / "GAC_MSIL" / "PresentationFramework" / "v4.0_4.0.0.0__31bf3856ad364e35" / "PresentationFramework.dll",
            assemblies / "GAC_64" / "PresentationCore" / "v4.0_4.0.0.0__31bf3856ad364e35" / "PresentationCore.dll",
            assemblies / "GAC_MSIL" / "WindowsBase" / "v4.0_4.0.0.0__31bf3856ad364e35" / "WindowsBase.dll",
            assemblies / "GAC_MSIL" / "System.Xaml" / "v4.0_4.0.0.0__b77a5c561934e089" / "System.Xaml.dll"]
        source = Path(__file__).resolve().parent.parent / "testdata" / "dotnet" / "wpf.cs"
        subprocess.run([shutil.which("mcs") or "mcs", "-sdk:4.5", "-platform:x64", "-target:winexe", *[f"-r:{reference}" for reference in references],
            f"-out:{folder / 'wpf64.exe'}", str(source)], check=True, timeout=300)
        step(raven, prefix, folder / "wpf64.exe", [], evidence, "wpf", watch=evidence / "wpf.png", timeout=300)
        drawn = (folder / "wpf-out.txt").read_text() if (folder / "wpf-out.txt").exists() else ""
        if not drawn.startswith("Raven WPF|WPF on .NET 4.0.30319"):
            raise RuntimeError(f"the WPF window did not render (wrote '{drawn.strip()}')")
        installed = ""
        if xna:
            shutil.copy(xna, prefix / "drive_c" / "xnafx40_redist.msi")
            step(raven, prefix, msiexec, ["/i", "C:\\xnafx40_redist.msi", "/qn"], evidence, "xna")
            code = native_redist.run(raven, prefix, folder / "xna64.exe", [], evidence / "xna-check.log", timeout=300)
            found = (folder / "xna-out.txt").read_text() if (folder / "xna-out.txt").exists() else ""
            if code != 0:
                raise RuntimeError(f"the XNA Framework 4.0 is not where a game looks for it (status {code}, '{found.strip()}')")
            installed = ", XNA Framework 4.0 installed"
        if netfx35:
            dism = prefix / "drive_c" / "windows" / "system32" / "dism.exe"
            (folder / "hello-out.txt").unlink(missing_ok=True)
            missing_runtime(raven, prefix, folder / "hello20.exe", evidence, title="Windows Features", name="feature-on-demand")
            shutil.copy(netfx35, prefix / "drive_c" / "dotnetfx35.exe")
            step(raven, prefix, dism, ["/Online", "/Enable-Feature", "/FeatureName:NetFx3", "/All", "/LimitAccess", "/Source:C:\\dotnetfx35.exe"],
                 evidence, "netfx35", timeout=3600)
            step(raven, prefix, dism, ["/Online", "/Get-FeatureInfo", "/FeatureName:NetFx3"], evidence, "netfx35-info")
            report = (prefix / "drive_c" / "windows" / "Logs" / "DISM" / "dism.log").read_text(errors="replace")
            if "State : Enabled" not in report:
                raise RuntimeError("dism does not report the .NET Framework 3.5 feature enabled")
            code = native_redist.run(raven, prefix, folder / "hello20.exe", [], evidence / "hello20.log", timeout=300)
            said20 = (folder / "hello-out.txt").read_text() if (folder / "hello-out.txt").exists() else ""
            if code != 42 or "Hello from .NET 2.0.50727" not in said20:
                raise RuntimeError(f"the 2.0 program did not run on the enabled feature (status {code}, '{said20.strip()}')")
            installed += f", .NET Framework 3.5 enabled from its package ({said20.strip()})"
        print(f"PASS: missing runtime reported, .NET Framework 4.8 installed, {said.strip()}, WinForms and WPF windows shown{installed}")
    finally:
        if compositor:
            compositor.terminate()
            compositor.wait(timeout=10)


if __name__ == "__main__":
    main()
