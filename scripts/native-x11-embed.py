#!/usr/bin/env python3
"""Show a Windows program's window inside a window of another program, on an isolated Xvfb server: x11_embed_test makes the host window
and checks that a child of it shows what embed.exe paints, as a plugin's editor shows in its host's.

usage: native-x11-embed.py X11_EMBED_TEST RAVEN EMBED_EXE WORKDIR
"""

from pathlib import Path
import os
import subprocess
import sys

from x11_session import X11Session


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    test, raven, program, work = sys.argv[1:]
    with X11Session(Path(work), manager=False) as session:
        result = subprocess.run([test, raven, program], env=os.environ | session.environment(), timeout=60)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
