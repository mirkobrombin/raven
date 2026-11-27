#!/usr/bin/env python3
"""Install the DirectX End-User Runtimes of June 2010 through Raven, as a game's setup does, and use what they install.

The package Microsoft publishes unpacks itself into a folder, then DXSETUP.exe runs silent from there. Before it runs, a program of each
width that needs the libraries finds none; after it, the shader compiler, D3DX, X3DAudio, XAudio2 and XInput load from the system
directories and work, every library is where Windows puts it, and DirectX's own log says the installation succeeded. A second run over
the installed runtimes succeeds as well, and the programs still work.

usage: native-directx-redist.py RAVEN PACKAGE CHECK64 CHECK32 EVIDENCE_DIR
"""

from pathlib import Path
import importlib.util
import shutil
import sys

import labwc_session

# The compositor and the runs are the ones of the Visual C++ workflow, loaded without leaving compiled files in the tree.
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("native_redist", Path(__file__).with_name("native-redist.py"))
native_redist = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(native_redist)

# The libraries the runtimes install for Windows 10 in both system directories, by family and version.
LIBRARIES = ([f"d3dx9_{version}.dll" for version in range(24, 44)] + [f"d3dx10_{version}.dll" for version in range(33, 44)] +
             ["d3dx11_42.dll", "d3dx11_43.dll", "d3dcsx_42.dll", "d3dcsx_43.dll"] + [f"d3dcompiler_{version}.dll" for version in range(33, 44)] +
             [f"xinput1_{version}.dll" for version in range(1, 4)] + [f"xaudio2_{version}.dll" for version in range(0, 8)] +
             [f"x3daudio1_{version}.dll" for version in range(0, 8)] + [f"xapofx1_{version}.dll" for version in range(0, 6)] +
             [f"xactengine2_{version}.dll" for version in range(0, 11)] + [f"xactengine3_{version}.dll" for version in range(0, 8)])


def check(raven, prefix, program, evidence, step, expected):
    """Runs a check program from the prefix's drive, as an installed game runs; it returns the number of the first check that failed."""
    target = prefix / "drive_c" / program.name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(program, target)
    log = evidence / f"{step}-{program.stem}.log"
    native_redist.run(raven, prefix, target, [], log, timeout=120)
    if f"Native entry returned {expected} ".encode() not in log.read_bytes():
        raise RuntimeError(f"{step}: {program.name} did not return {expected}, see {log.name}")


def setup(raven, prefix, evidence, step):
    code = native_redist.run(raven, prefix, prefix / "drive_c" / "dx" / "DXSETUP.exe", ["/silent"], evidence / f"{step}.log", timeout=900)
    logs = prefix / "drive_c" / "windows" / "Logs"
    text = (logs / "DirectX.log").read_text(encoding="utf-8", errors="replace")
    shutil.copy(logs / "DirectX.log", evidence / f"{step}-DirectX.log")
    if (logs / "DXError.log").exists():
        shutil.copy(logs / "DXError.log", evidence / f"{step}-DXError.log")
    last = [line for line in text.splitlines() if "Installation ended with value" in line]
    if code != 0 or not last or "value 0 " not in last[-1]:
        raise RuntimeError(f"{step}: DXSETUP ended with status {code}: {last[-1] if last else 'no result in DirectX.log'}")


def main():
    raven, package, check64, check32, evidence = map(Path, sys.argv[1:6])
    evidence.mkdir(parents=True, exist_ok=True)
    prefix = evidence / "prefix"
    shutil.rmtree(prefix, ignore_errors=True)
    log = (evidence / "compositor.log").open("wb")
    compositor = labwc_session.start(log, False)
    try:
        # Without the runtimes the programs find no shader compiler.
        check(raven, prefix, check64, evidence, "before", 1)
        check(raven, prefix, check32, evidence, "before", 1)
        code = native_redist.run(raven, prefix, package, ["/Q", "/T:C:\\dx"], evidence / "unpack.log", timeout=300)
        if code != 0 or not (prefix / "drive_c" / "dx" / "DXSETUP.exe").exists():
            raise RuntimeError(f"the package did not unpack (status {code})")
        setup(raven, prefix, evidence, "install")
        for folder in ("system32", "syswow64"):
            present = {path.name.lower() for path in (prefix / "drive_c" / "windows" / folder).iterdir()}
            missing = [name for name in LIBRARIES if name not in present]
            if missing:
                raise RuntimeError(f"{folder} lacks {', '.join(missing)}")
        check(raven, prefix, check64, evidence, "installed", 0)
        check(raven, prefix, check32, evidence, "installed", 0)
        setup(raven, prefix, evidence, "reinstall")
        check(raven, prefix, check64, evidence, "reinstalled", 0)
        check(raven, prefix, check32, evidence, "reinstalled", 0)
        print(f"PASS: DirectX June 2010 installed {len(LIBRARIES)} libraries in each system directory, both widths use them, installed again")
    finally:
        if compositor:
            compositor.terminate()
            compositor.wait(timeout=10)


if __name__ == "__main__":
    main()
