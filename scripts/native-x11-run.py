#!/usr/bin/env python3
"""Run a program on an isolated Xvfb server, with no window manager, as a host program that makes its own windows runs: the tests of
windows shown inside another program's window use it (x11_embed_test, the VST 3 editor test).

usage: native-x11-run.py WORKDIR PROGRAM [ARGUMENT...]
"""

from pathlib import Path
import os
import subprocess
import sys

from x11_session import X11Session


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    with X11Session(Path(sys.argv[1]), manager=False) as session:
        result = subprocess.run(sys.argv[2:], env=os.environ | session.environment(), timeout=120)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
