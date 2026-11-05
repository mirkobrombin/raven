#!/usr/bin/env python3
"""Install the .NET Framework 4.8 through Raven the way application bootstrappers do: Microsoft's offline installer run with /q /norestart,
which unpacks itself and hands over to its own Setup.exe, which installs the packages. Then a console program runs on the framework.

usage: native-dotnet-setup.py RAVEN NDP48_INSTALLER PROGRAMS_DIR EVIDENCE_DIR
"""

from pathlib import Path
import html
import os
import re
import shutil
import subprocess
import sys
import time


def run(raven, prefix, program, arguments, output, timeout):
    env = dict(os.environ, RAVEN_PREFIX=str(prefix))
    started = time.time()
    with output.open("wb") as log:
        process = subprocess.run([str(raven), "--run-process", "--execution=native", f"--timeout-ms={timeout * 1000}", str(program), "--",
                                  *arguments], env=env, stdout=log, stderr=log, timeout=timeout + 60)
    return process.returncode, time.time() - started


def setup_result(prefix):
    """The last line of the newest Setup log, the one that gives the final result."""
    logs = sorted((prefix / "drive_c" / "Users").glob("*/AppData/Local/Temp/*Setup_*.html"), key=lambda path: path.stat().st_mtime)
    if not logs:
        return ""
    data = logs[-1].read_bytes()
    text = data.decode("utf-16" if data[:2] == b"\xff\xfe" else "utf-8", "replace")
    text = html.unescape(re.sub(r"<[^>]+>", " ", re.sub(r"<(br|/tr|/div|/p|/li)[^>]*>", "\n", text, flags=re.I)))
    lines = [re.sub(r"\s+", " ", line).strip() for line in text.split("\n")]
    return next((line for line in reversed(lines) if "Final Result" in line), "")


def main():
    raven, installer, programs, evidence = map(Path, sys.argv[1:5])
    evidence.mkdir(parents=True, exist_ok=True)
    prefix = evidence / "prefix"
    shutil.rmtree(prefix, ignore_errors=True)
    prefix.mkdir()

    code, seconds = run(raven, prefix, installer, ["/q", "/norestart"], evidence / "setup.log", 2400)
    result = setup_result(prefix)
    print(f"setup ended with status {code} after {seconds:.0f} s: {result}")
    if code not in (0, 3010):
        raise RuntimeError(f"the installer ended with status {code}")
    if "successfully" not in result:
        raise RuntimeError("the setup log does not report a successful installation")
    for folder in ("Framework", "Framework64"):
        runtime = prefix / "drive_c" / "windows" / "Microsoft.NET" / folder / "v4.0.30319" / "clr.dll"
        if not runtime.is_file():
            raise RuntimeError(f"the runtime is missing: {runtime}")

    folder = prefix / "drive_c" / "dotnet"
    folder.mkdir()
    shutil.copy(programs / "hello64.exe", folder / "hello64.exe")
    code, _ = run(raven, prefix, folder / "hello64.exe", [], evidence / "hello.log", 300)
    said = (folder / "hello-out.txt").read_text() if (folder / "hello-out.txt").exists() else ""
    if code != 42 or "Hello from .NET 4.0.30319" not in said:
        raise RuntimeError(f"the console program did not run on the installed runtime (status {code}, '{said.strip()}')")
    print(f"PASS: {said.strip()}")


if __name__ == "__main__":
    main()
