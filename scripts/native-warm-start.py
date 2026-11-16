#!/usr/bin/env python3
"""Starts the same program three times: the first leaves a warm host behind, the second prepares it, and the third must be a process
forked from it, not the raven command itself. Each start must still read the standard input it was given, write to its own standard
output and end with the program's exit status."""

import os
import subprocess
import sys
import tempfile

raven, program = sys.argv[1], sys.argv[2]
command = [raven, "--run-process", "--execution=native", "--stdio=inherit", program]


def start(name, check_host):
	launcher = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
	pipe = os.readlink(f"/proc/{launcher.pid}/fd/0")
	if launcher.stdout.readline() != b"READY\n":
		sys.exit(f"{name}: the program did not start")
	if check_host:
		# The processes that run a start hold its standard input. Run by the raven command they all descend from it; on a warm host the
		# start is a fork of the host instead.
		def parent(process):
			with open(f"/proc/{process}/stat") as stat:
				return int(stat.read().rsplit(")", 1)[1].split()[1])

		def descends(process):
			while process > 1:
				if process == launcher.pid:
					return True
				process = parent(process)
			return False

		elsewhere = []
		for entry in os.listdir("/proc"):
			if entry.isdigit():
				try:
					if os.readlink(f"/proc/{entry}/fd/0") == pipe and not descends(int(entry)):
						elsewhere.append(int(entry))
				except OSError:
					pass
		if not elsewhere:
			sys.exit(f"{name}: the start ran in the raven command, not on a warm host")
	launcher.stdin.write(name.encode() + b"\n")
	launcher.stdin.close()
	output = launcher.stdout.read()
	status = launcher.wait(timeout=60)
	if output != b"GOT " + name.encode() + b"\n" or status != 23:
		sys.exit(f"{name}: output {output!r}, status {status}")


with tempfile.TemporaryDirectory() as prefix:
	os.environ["RAVEN_PREFIX"] = prefix
	os.environ["RAVEN_WARM_SECONDS"] = "5"
	os.environ.pop("RAVEN_WARM", None)
	start("first", False)
	start("second", False)
	start("third", True)
print("PASS")
