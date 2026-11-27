#!/usr/bin/env python3
"""Install OpenAL through Raven with the installer Creative publishes, silent as games run it, and use what it installs.

Before the installer runs, a program of each width that needs OpenAL finds no router; after it, the router and its DirectSound wrapper
are in both system directories and the router answers in both widths. A second run over the installed copy succeeds as well.

usage: native-openal-redist.py RAVEN INSTALLER CHECK64 CHECK32 EVIDENCE_DIR
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

LIBRARIES = ("openal32.dll", "wrap_oal.dll")


def check(raven, prefix, program, evidence, step, expected):
    """Runs a check program from the prefix's drive, as an installed game runs; it returns the number of the first check that failed."""
    target = prefix / "drive_c" / program.name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(program, target)
    log = evidence / f"{step}-{program.stem}.log"
    native_redist.run(raven, prefix, target, [], log, timeout=120)
    if f"Native entry returned {expected} ".encode() not in log.read_bytes():
        raise RuntimeError(f"{step}: {program.name} did not return {expected}, see {log.name}")


def install(raven, prefix, installer, evidence, step):
    code = native_redist.run(raven, prefix, installer, ["/s"], evidence / f"{step}.log", timeout=300)
    if code != 0:
        raise RuntimeError(f"{step}: the installer ended with status {code}")
    for folder in ("system32", "syswow64"):
        present = {path.name.lower() for path in (prefix / "drive_c" / "windows" / folder).iterdir()}
        missing = [name for name in LIBRARIES if name not in present]
        if missing:
            raise RuntimeError(f"{step}: {folder} lacks {', '.join(missing)}")


def main():
    raven, installer, check64, check32, evidence = map(Path, sys.argv[1:6])
    evidence.mkdir(parents=True, exist_ok=True)
    prefix = evidence / "prefix"
    shutil.rmtree(prefix, ignore_errors=True)
    log = (evidence / "compositor.log").open("wb")
    compositor = labwc_session.start(log, False)
    try:
        check(raven, prefix, check64, evidence, "before", 1)
        check(raven, prefix, check32, evidence, "before", 1)
        install(raven, prefix, installer, evidence, "install")
        check(raven, prefix, check64, evidence, "installed", 0)
        check(raven, prefix, check32, evidence, "installed", 0)
        install(raven, prefix, installer, evidence, "reinstall")
        check(raven, prefix, check64, evidence, "reinstalled", 0)
        check(raven, prefix, check32, evidence, "reinstalled", 0)
        print("PASS: OpenAL installed in both system directories, both widths use it, installed again")
    finally:
        if compositor:
            compositor.terminate()
            compositor.wait(timeout=10)


if __name__ == "__main__":
    main()
