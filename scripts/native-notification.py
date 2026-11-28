#!/usr/bin/env python3
"""Check that a balloon of a tray icon reaches the Linux desktop as a notification.

testdata/native_loader/notification_client.c shows two balloons. A gdbus put first on PATH records how it is called; each balloon
must call the notification service's Notify with the program's name, the balloon's title and its text as GVariant strings.

usage: native-notification.py RAVEN CLIENT WORKDIR
"""

from pathlib import Path
import os
import subprocess
import sys
import time

EXPECTED = [
    ["'notification'", "0", "''", "'Backup'", "'It\\'s done\\u000aall files'", "@as []", "@a{sv} {}", "-1"],
    ["'notification'", "0", "''", "'Second'", "'Plain text'", "@as []", "@a{sv} {}", "-1"],
]
HEAD = ["call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path", "/org/freedesktop/Notifications",
        "--method", "org.freedesktop.Notifications.Notify"]


def main():
    raven, client, work = (Path(argument).resolve() for argument in sys.argv[1:4])
    work.mkdir(parents=True, exist_ok=True)
    folder = work / "bin"
    folder.mkdir(exist_ok=True)
    record = work / "calls"
    record.unlink(missing_ok=True)
    fake = folder / "gdbus"
    fake.write_text('#!/bin/sh\nfor argument do printf "%s\\0" "$argument"; done >> "$RAVEN_GDBUS_RECORD"\nprintf "\\n" >> "$RAVEN_GDBUS_RECORD"\n')
    fake.chmod(0o755)
    env = os.environ | {"PATH": f"{folder}:{os.environ.get('PATH', '/usr/bin:/bin')}", "RAVEN_GDBUS_RECORD": str(record),
                        "RAVEN_PREFIX": str(work / "prefix")}
    result = subprocess.run([str(raven), "--run-process", "--execution=native", str(client)], env=env, capture_output=True, text=True,
                            timeout=60)
    print(result.stdout, result.stderr, sep="")
    if "Native entry returned 0 " not in result.stdout + result.stderr:
        raise SystemExit("the notification client failed")
    # The notifications are shown by processes of their own, which may finish after the program.
    deadline = time.monotonic() + 10
    calls = []
    while time.monotonic() < deadline:
        calls = [line.split("\0")[:-1] for line in record.read_text().split("\n")[:-1]] if record.exists() else []
        if len(calls) >= len(EXPECTED):
            break
        time.sleep(0.1)
    calls.sort(key=lambda call: call[len(HEAD) + 3:len(HEAD) + 4])
    if calls != [HEAD + expected for expected in EXPECTED]:
        raise SystemExit(f"notification calls differ: {calls!r}")
    print("Both balloons reached the notification service")


if __name__ == "__main__":
    main()
