#!/usr/bin/env python3
"""A failed prefix registration is retried by the next independent process, once per process tree."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    raven, client, directory = (Path(value).resolve() for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="prefix-setup-", dir=os.environ.get("TMPDIR", directory)) as temporary:
        work = Path(temporary)
        runtime = work / "runtime"
        runtime.mkdir()
        shutil.copy2(raven, runtime / "raven")
        (runtime / "raven-bootstrap").symlink_to(raven.parent / "raven-bootstrap")
        (runtime / "regsvr32.exe").symlink_to(client)
        for name in ("msxml3.dll", "windowscodecs.dll", "mlang.dll", "propsys.dll"):
            (runtime / name).write_bytes(b"registration probe")
        prefix = work / "prefix"
        env = os.environ | {"RAVEN_PREFIX": str(prefix), "TMPDIR": str(work), "TMP": str(work), "TEMP": str(work)}
        env.pop("RAVEN_PREFIX_SETUP", None)
        for attempt in range(2):
            result = subprocess.run([str(runtime / "raven"), "--run-process", "--execution=native", str(client)],
                                    env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=25)
            if result.returncode:
                raise RuntimeError(f"prefix setup client failed: {result.stdout}")
            count = (prefix / "drive_c" / "registration-count.bin").read_bytes()
            if count != b"x" * (5 * (attempt + 1)):
                raise RuntimeError(f"registration ran {len(count)} times after {attempt + 1} independent launches; expected {5 * (attempt + 1)}")
        print("Failed registrations retry once per independent launch; suspended children accept tokens without repeating setup")


if __name__ == "__main__":
    main()
