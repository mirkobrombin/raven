#!/usr/bin/env python3
"""Install the Visual C++ redistributables from 2005 to 2022, both widths, through Raven, alone and all together in one prefix.

Each package runs unmodified and silent, as Microsoft publishes it. A program built against the runtime it brings must then load that
runtime from where the package put it and run. Together, the packages go into one prefix oldest first or newest first; after each one
every program installed so far must still run, and at the end the older packages go in again over the newer ones.

usage: native-redist-matrix.py RAVEN REDIST_DIR FIXTURES_DIR EVIDENCE_DIR alone PACKAGE
       native-redist-matrix.py RAVEN REDIST_DIR FIXTURES_DIR EVIDENCE_DIR together forward|backward
"""

from pathlib import Path
import importlib.util
import shutil
import sys

import labwc_session

# The compositor and the runs are the ones the 2015-2022 workflow uses, loaded without leaving compiled files in the tree.
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("native_redist", Path(__file__).with_name("native-redist.py"))
native_redist = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(native_redist)

# Oldest first: the package, its silent command line, and the program built against its runtime (in the fixtures, by width).
BURN = ["/install", "/quiet", "/norestart"]
PACKAGES = (
    ("vc2005_x86.exe", ["/q"], "x86/crt-vc80.exe"),
    ("vc2005_x64.exe", ["/q"], "x64/crt-vc80.exe"),
    ("vc2008_x86.exe", ["/q"], "x86/crt-vc90.exe"),
    ("vc2008_x64.exe", ["/q"], "x64/crt-vc90.exe"),
    ("vc2010_x86.exe", ["/q", "/norestart"], "x86/crt-vc100.exe"),
    ("vc2010_x64.exe", ["/q", "/norestart"], "x64/crt-vc100.exe"),
    ("vc2012_x86.exe", BURN, "x86/crt-vc110.exe"),
    ("vc2012_x64.exe", BURN, "x64/crt-vc110.exe"),
    ("vc2013_x86.exe", BURN, "x86/crt-vc120.exe"),
    ("vc2013_x64.exe", BURN, "x64/crt-vc120.exe"),
    ("vc_redist.x86.exe", BURN, "x86/redist-check.exe"),
    ("vc_redist.x64.exe", BURN, "x64/redist-check.exe"),
)


def install(raven, redist, prefix, package, arguments, evidence, step):
    code = native_redist.run(raven, prefix, redist / package, arguments, evidence / f"{step}-{Path(package).stem}.log")
    if code not in (0, 3010):
        raise RuntimeError(f"{step}: {package} ended with status {code}")


def check(raven, fixtures, prefix, checker, evidence, step):
    """Runs a program built against one runtime from the prefix's drive, as an installed program runs."""
    target = prefix / "drive_c" / checker.replace("/", "-")
    shutil.copy(fixtures / checker, target)
    log = evidence / f"{step}-{target.stem}.log"
    code = native_redist.run(raven, prefix, target, [], log, timeout=60)
    if code != 0 or b"Native entry returned 0 " not in log.read_bytes():
        raise RuntimeError(f"{step}: {checker} did not run with its runtime (status {code})")


def main():
    raven, redist, fixtures, evidence = map(Path, sys.argv[1:5])
    mode, which = sys.argv[5], sys.argv[6]
    evidence.mkdir(parents=True, exist_ok=True)
    log = (evidence / "compositor.log").open("wb")
    compositor = labwc_session.start(log, False)
    try:
        if mode == "alone":
            package, arguments, checker = next(item for item in PACKAGES if item[0] == which)
            prefix = evidence / "prefix"
            shutil.rmtree(prefix, ignore_errors=True)
            install(raven, redist, prefix, package, arguments, evidence, "install")
            check(raven, fixtures, prefix, checker, evidence, "installed")
            install(raven, redist, prefix, package, arguments, evidence, "reinstall")
            check(raven, fixtures, prefix, checker, evidence, "reinstalled")
            print(f"PASS: {package} installed alone, its program ran, installed again")
        else:
            order = list(PACKAGES) if which == "forward" else list(reversed(PACKAGES))
            prefix = evidence / "prefix"
            shutil.rmtree(prefix, ignore_errors=True)
            done = []
            for step, (package, arguments, checker) in enumerate(order):
                install(raven, redist, prefix, package, arguments, evidence, f"{step:02d}")
                done.append(checker)
                for program in done:
                    check(raven, fixtures, prefix, program, evidence, f"{step:02d}")
            # The older packages again, over the newer ones.
            for step, (package, arguments, _) in enumerate(PACKAGES[:6]):
                install(raven, redist, prefix, package, arguments, evidence, f"again-{step:02d}")
            for program in done:
                check(raven, fixtures, prefix, program, evidence, "again")
            print(f"PASS: all redistributables together {which}, every program ran after each, older ones installed again")
    finally:
        if compositor:
            compositor.terminate()
            compositor.wait(timeout=10)


if __name__ == "__main__":
    main()
